/* Copyright (C) 2007-2014 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Pablo Rincon <pablo.rincon.crespo@gmail.com>
 * \author Victor Julien <victor@inliniac.net>
 *
 * App-layer parser for SSH protocol
 *
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "threads.h"

#include "util-print.h"
#include "util-pool.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"

#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-ssh.h"

#include "conf.h"

#include "util-spm.h"
#include "util-unittest.h"
#include "util-debug.h"
#include "flow-private.h"

#include "util-byte.h"
#include "util-memcmp.h"

/** \internal
 *  \brief Function to parse the SSH version string of the client
 *
 *  The input to this function is a byte buffer starting with SSH-
 *
 *  \param  ssh_state   Pointer the state in which the value to be stored
 *  \param  input       Pointer the received input data
 *  \param  input_len   Length in bytes of the received data
 */
static int SSHParseVersion(SshState *state, SshHeader *header, const uint8_t *input, uint32_t input_len)
{
    const uint8_t *line_ptr = input;
    uint32_t line_len = input_len;

    /* is it the version line? */
    if (SCMemcmp("SSH-", line_ptr, 4) != 0) {
        SCReturnInt(-1);
    }
    if (line_len > 255) {
        SCLogInfo("Invalid version string, it should be less than 255 characters including <CR><NL>");
        SCReturnInt(-1);
    }

    /* ok, we have found the version line/string, skip it and parse proto version */
    line_ptr += 4;
    line_len -= 4;

    uint8_t *proto_end = BasicSearch(line_ptr, line_len, (uint8_t*)"-", 1);
    if (proto_end == NULL) {
        /* Strings starting with SSH- are not allowed
         * if they are not the real version string */
        SCLogDebug("Info Version String for SSH (invalid usage of SSH- prefix)");
        SCReturnInt(-1);
    }
    uint64_t proto_ver_len = (uint64_t)(proto_end - line_ptr);
    header->proto_version = SCMalloc(proto_ver_len + 1);
    if (header->proto_version == NULL) {
        SCReturnInt(-1);
    }
    memcpy(header->proto_version, line_ptr, proto_ver_len);
    header->proto_version[proto_ver_len] = '\0';

    /* Now lets parse the software & version */
    line_ptr += proto_ver_len + 1;
    line_len -= proto_ver_len + 1;
    if (line_len < 1) {
        SCLogInfo("No software version specified (weird)");
        header->flags |= SSH_FLAG_VERSION_PARSED;
        /* Return the remaining length */
        SCReturnInt(0);
    }

    const uint8_t *sw_end = BasicSearch(line_ptr, line_len, (uint8_t*)" ", 1);
    if (sw_end == NULL) {
        sw_end = BasicSearch(line_ptr, line_len, (uint8_t*)"\r", 1);
        if (sw_end == NULL) {
            sw_end = line_ptr + line_len;
        }
    }

    uint64_t sw_ver_len = (uint64_t)(sw_end - line_ptr);
    header->software_version = SCMalloc(sw_ver_len + 1);
    if (header->software_version == NULL) {
        SCReturnInt(-1);
    }
    memcpy(header->software_version, line_ptr, sw_ver_len);
    header->software_version[sw_ver_len] = '\0';
    if (header->software_version[sw_ver_len - 1] == 0x0d)
        header->software_version[sw_ver_len - 1] = '\0';

    header->flags |= SSH_FLAG_VERSION_PARSED;

    /* Return the remaining length */
    int len = input_len - (sw_end - input);
    SCReturnInt(len);
}

static int SSHParseClientRecordHeader(SshState *state, SshHeader *header, uint8_t *input, uint32_t input_len)
{
    BUG_ON(input_len != 6);

    /* input and input_len now point past initial line */
    uint32_t pkt_len = 0;
    int r = ByteExtractUint32(&pkt_len, BYTE_BIG_ENDIAN,
            4, input);
    if (r != 4) {
        SCLogInfo("xtract 4 bytes failed %d", r);
        SCReturnInt(-1);
    }
    if (pkt_len < 2) {
        SCReturnInt(-1);
    }

    header->pkt_len = pkt_len;
    SCLogInfo("pkt len: %"PRIu32, pkt_len);

    input += 4;
    input_len -= 4;

    header->padding_len = *input;

    input += 1;
    input_len -= 1;

    SCLogInfo("padding: %u", header->padding_len);

    header->msg_code = *input;

    input += 1;
    input_len -= 1;

    SCLogInfo("msg code: %u", header->msg_code);

    if (header->msg_code == SSH_MSG_NEWKEYS) {
        /* done */
        SCLogInfo("done");
        state->flags |= SSH_FLAG_PARSER_DONE;
    } else {
        /* not yet done */
        SCLogInfo("not done");
    }
    SCReturnInt(0);
}

/** \internal
 *  \brief Function to parse the SSH field in packet received from the client
 *
 *  Input to this function is a byte buffer starting with SSH- up to at least
 *  a \r or \n character.
 *
 *  \param  ssh_state   Pointer the state in which the value to be stored
 *  \param  input       Pointer the received input data
 *  \param  input_len   Length in bytes of the received data
 */
static int SSHParseRecord(SshState *state, SshHeader *header, uint8_t *input, uint32_t input_len)
{
    SCEnter();
    int ret = 0;

    if (state->flags & SSH_FLAG_PARSER_DONE) {
        SCReturnInt(0);
    }

    SCLogDebug("state %p, input %p,input_len %" PRIu32,
               state, input, input_len);
    PrintRawDataFp(stdout, input, input_len);

    if (!(header->flags & SSH_FLAG_VERSION_PARSED)) {
        ret = SSHParseVersion(state, header, input, input_len);
        if (ret < 0) {
            SCLogDebug("Invalid version string");
            SCReturnInt(-1);
        } else if (header->flags & SSH_FLAG_VERSION_PARSED) {
            SCLogInfo("Version string parsed, remaining length %d", ret);
            input += input_len - ret;
            input_len -= (input_len - ret);
            ret = 0;

            uint32_t u = 0;
            while (u < input_len && (input[u] == '\r' || input[u] == '\n')) {
                u++;
            }
            SCLogInfo("skipping %u EOL bytes", u);
            input += u;
            input_len -= u;

            if (input_len == 0)
                SCReturnInt(0);

        } else {
            BUG_ON(1);// we only call this when we have enough data
            SCLogDebug("Version string not parsed yet");
            //pstate->parse_field = 0;
            SCReturnInt(0);
        }
    } else {
        SCLogDebug("Version string already parsed");
    }

    /* skip bytes from the current record if we have to */
    if (state->cli_hdr.record_left > 0) {
        SCLogInfo("skipping bytes part of the current record");
        if (state->cli_hdr.record_left > input_len) {
            state->cli_hdr.record_left -= input_len;
            SCLogInfo("all input skipped, %u left in record", state->cli_hdr.record_left);
            SCReturnInt(0);
        } else {
            input_len -= state->cli_hdr.record_left;
            input += state->cli_hdr.record_left;
            state->cli_hdr.record_left = 0;

            if (input_len == 0) {
                SCLogInfo("all input skipped");
                SCReturnInt(0);
            }
        }
    }

again:
    /* input is too small, even when combined with stored bytes */
    if (state->cli_hdr.buf_offset + input_len < 6) {
        memcpy(state->cli_hdr.buf + state->cli_hdr.buf_offset, input, input_len);
        state->cli_hdr.buf_offset += input_len;

        //PrintRawDataFp(stdout, state->cli_hdr.buf, state->cli_hdr.buf_offset);
        SCReturnInt(0);

    /* we have enough bytes to parse 6 bytes, lets see if we have
     * previously stored some */
    } else if (state->cli_hdr.buf_offset > 0) {
        uint8_t needed = 6 - state->cli_hdr.buf_offset;

        SCLogInfo("parse stored");
        memcpy(state->cli_hdr.buf + state->cli_hdr.buf_offset, input, needed);
        state->cli_hdr.buf_offset = 6;

        // parse the 6
        if (SSHParseClientRecordHeader(state, header, state->cli_hdr.buf, 6) < 0)
            SCReturnInt(-1);
        state->cli_hdr.buf_offset = 0;

        uint32_t record_left = state->cli_hdr.pkt_len - 2;
        input_len -= needed;
        input += needed;

        if (record_left > input_len) {
            state->cli_hdr.record_left = record_left - input_len;
        } else {
            input_len -= record_left;
            if (input_len == 0)
                SCReturnInt(0);

            input += record_left;

            SCLogDebug("we have %u left to parse", input_len);
            goto again;

        }

    /* nothing stored, lets parse this directly */
    } else {
        // parse the 6
        SCLogInfo("parse direct");
        PrintRawDataFp(stdout, input, input_len);
        if (SSHParseClientRecordHeader(state, header, input, 6) < 0)
            SCReturnInt(-1);

        uint32_t record_left = state->cli_hdr.pkt_len - 2;
        SCLogInfo("record left %u", record_left);
        input_len -= 6;
        input += 6;

        if (record_left > input_len) {
            state->cli_hdr.record_left = record_left - input_len;
        } else {
            input_len -= record_left;
            if (input_len == 0)
                SCReturnInt(0);
            input += record_left;
            //PrintRawDataFp(stdout, input, input_len);

            SCLogDebug("we have %u left to parse", input_len);
            goto again;
        }
    }

#if 0
    if (state->cli_hdr.msg_code == SSH_MSG_NEWKEYS) {
        /* We are not going to inspect any packet more
         * as the data is now encrypted */
        SCLogDebug("SSH parser done (the rest of the communication is encrypted)");
        pstate->flags |= APP_LAYER_PARSER_DONE;
        pstate->flags |= APP_LAYER_PARSER_NO_INSPECTION;
        pstate->flags |= APP_LAYER_PARSER_NO_REASSEMBLY;
        pstate->parse_field = 1;
        SCReturnInt(1);
    }
#endif
    SCReturnInt(0);
}

static int EnoughData(uint8_t *input, uint32_t input_len)
{
    uint32_t u;
    for (u = 0; u < input_len; u++) {
        if (input[u] == '\r' || input[u] == '\n')
            return TRUE;
    }
    return FALSE;
}

static int SSHParseRequest(Flow *f, void *state, AppLayerParserState *pstate,
                           uint8_t *input, uint32_t input_len,
                           void *local_data)
{
    SshState *ssh_state = (SshState *)state;

    if (ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED || EnoughData(input, input_len) == TRUE) {
        SCLogInfo("enough data, parse now");
        // parse now
        int r = SSHParseRecord(ssh_state, &ssh_state->cli_hdr, input, input_len);
        SCReturnInt(r);
    } else {
        // buffer
    }

    PrintRawDataFp(stdout, input, input_len);
    return 0;
}

static int SSHParseResponse(Flow *f, void *state, AppLayerParserState *pstate,
                            uint8_t *input, uint32_t input_len,
                            void *local_data)
{
//    PrintRawDataFp(stdout, input, input_len);
    return 0;
}

/** \brief Function to allocates the SSH state memory
 */
static void *SSHStateAlloc(void)
{
    void *s = SCMalloc(sizeof(SshState));
    if (unlikely(s == NULL))
        return NULL;

    memset(s, 0, sizeof(SshState));
    return s;
}

/** \brief Function to free the SSH state memory
 */
static void SSHStateFree(void *state)
{
    SshState *s = (SshState *)state;
    if (s->cli_hdr.proto_version != NULL)
        SCFree(s->cli_hdr.proto_version);
    if (s->cli_hdr.software_version != NULL)
        SCFree(s->cli_hdr.software_version);
    if (s->srv_hdr.proto_version != NULL)
        SCFree(s->srv_hdr.proto_version);
    if (s->srv_hdr.software_version != NULL)
        SCFree(s->srv_hdr.software_version);

    SCFree(s);
}

static int SSHRegisterPatternsForProtocolDetection(void)
{
    if (AppLayerProtoDetectPMRegisterPatternCI(IPPROTO_TCP, ALPROTO_SSH,
                                               "SSH-", 4, 0, STREAM_TOSERVER) < 0)
    {
        return -1;
    }
    if (AppLayerProtoDetectPMRegisterPatternCI(IPPROTO_TCP, ALPROTO_SSH,
                                               "SSH-", 4, 0, STREAM_TOCLIENT) < 0)
    {
        return -1;
    }
    return 0;
}

/** \brief Function to register the SSH protocol parsers and other functions
 */
void RegisterSSHParsers(void)
{
    char *proto_name = "ssh";

    if (AppLayerProtoDetectConfProtoDetectionEnabled("tcp", proto_name)) {
        AppLayerProtoDetectRegisterProtocol(ALPROTO_SSH, proto_name);
        if (SSHRegisterPatternsForProtocolDetection() < 0)
            return;
    }

    if (AppLayerParserConfParserEnabled("tcp", proto_name)) {
        AppLayerParserRegisterParser(IPPROTO_TCP, ALPROTO_SSH, STREAM_TOSERVER,
                                     SSHParseRequest);
        AppLayerParserRegisterParser(IPPROTO_TCP, ALPROTO_SSH, STREAM_TOCLIENT,
                                     SSHParseResponse);
        AppLayerParserRegisterStateFuncs(IPPROTO_TCP, ALPROTO_SSH, SSHStateAlloc, SSHStateFree);
        AppLayerParserRegisterParserAcceptableDataDirection(IPPROTO_TCP,
                ALPROTO_SSH, STREAM_TOSERVER|STREAM_TOCLIENT);
    } else {
//        SCLogInfo("Parsed disabled for %s protocol. Protocol detection"
//                  "still on.", proto_name);
    }

#ifdef UNITTESTS
    AppLayerParserRegisterProtocolUnittests(IPPROTO_TCP, ALPROTO_SSH, SSHParserRegisterTests);
#endif
}

/* UNITTESTS */
#ifdef UNITTESTS

/** \test Send a version string in one chunk (client version str). */
static int SSHParserTest01(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf[] = "SSH-2.0-MySSHClient-0.5.1\n";
    uint32_t sshlen = sizeof(sshbuf) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER|STREAM_EOF, sshbuf, sshlen);
    if (r != 0) {
        printf("toclient chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        result = 0;
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        result = 0;
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        result = 0;
        goto end;
    }

    result = 1;

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a version string in one chunk but multiple lines and comments.
 *        (client version str)
 */
static int SSHParserTest02(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf[] = "lalala\n lal al al\nSSH-2.0-MySSHClient-0.5.1 some comments...\n";
    uint32_t sshlen = sizeof(sshbuf) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER|STREAM_EOF, sshbuf, sshlen);
    if (r != 0) {
        printf("toclient chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        result = 0;
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        result = 0;
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        result = 0;
        goto end;
    }

    result = 1;

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a invalid version string in one chunk but multiple lines and comments.
 *        (client version str)
 */
static int SSHParserTest03(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf[] = "lalala\n lal al al\nSSH-2.0 some comments...\n";
    uint32_t sshlen = sizeof(sshbuf) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER|STREAM_EOF, sshbuf, sshlen);
    if (r == 0) {
        printf("toclient chunk 1 returned %" PRId32 ", expected != 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* Ok, it returned an error. Let's make sure we didn't parse the string at all */

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        result = 0;
        goto end;
    }

    if (ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED) {
        printf("Client version string parsed? It's not a valid string: ");
        result = 0;
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version != NULL) {
        result = 0;
        goto end;
    }

    if (ssh_state->cli_hdr.software_version != NULL) {
        result = 0;
        goto end;
    }

    result = 1;

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a version string in one chunk (server version str). */
static int SSHParserTest04(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf[] = "SSH-2.0-MySSHClient-0.5.1\n";
    uint32_t sshlen = sizeof(sshbuf) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT|STREAM_EOF, sshbuf, sshlen);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        result = 0;
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_SERVER_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (ssh_state->srv_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (ssh_state->srv_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        result = 0;
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        result = 0;
        goto end;
    }

    result = 1;

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a version string in one chunk but multiple lines and comments.
 *        (server version str)
 */
static int SSHParserTest05(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf[] = "lalala\n lal al al\nSSH-2.0-MySSHClient-0.5.1 some comments...\n";
    uint32_t sshlen = sizeof(sshbuf) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT|STREAM_EOF, sshbuf, sshlen);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        result = 0;
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_SERVER_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (ssh_state->srv_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (ssh_state->srv_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        result = 0;
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        result = 0;
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        result = 0;
        goto end;
    }

    result = 1;

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a invalid version string in one chunk but multiple lines and comments.
 *        (server version str)
 */
static int SSHParserTest06(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf[] = "lalala\n lal al al\nSSH-2.0 some comments...\n";
    uint32_t sshlen = sizeof(sshbuf) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT|STREAM_EOF, sshbuf, sshlen);
    if (r == 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected != 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* Ok, it returned an error. Let's make sure we didn't parse the string at all */

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        result = 0;
        goto end;
    }

    if (ssh_state->flags & SSH_FLAG_SERVER_VERSION_PARSED) {
        printf("Client version string parsed? It's not a valid string: ");
        result = 0;
        goto end;
    }

    if (ssh_state->srv_hdr.proto_version != NULL) {
        result = 0;
        goto end;
    }

    if (ssh_state->srv_hdr.software_version != NULL) {
        result = 0;
        goto end;
    }

    result = 1;

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

static int SSHParserTest07(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "SSH-2.";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = { "0-MySSHClient-0.5.1\r\n"};
    uint32_t sshlen2 = sizeof(sshbuf2) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    result = 1;

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a version banner in three chunks. */
static int SSHParserTest08(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "Welcome to this ssh server\nSSH-";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = "2.";
    uint32_t sshlen2 = sizeof(sshbuf2) - 1;
    uint8_t sshbuf3[] = { "0-MySSHClient-0.5.1\r\n"};
    uint32_t sshlen3 = sizeof(sshbuf3) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf3, sshlen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

static int SSHParserTest09(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "SSH-2.";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = { "0-MySSHClient-0.5.1\r\n"};
    uint32_t sshlen2 = sizeof(sshbuf2) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_SERVER_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->srv_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->srv_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    result = 1;

end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a version banner in three chunks. */
static int SSHParserTest10(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "Welcome to this ssh server\nSSH-";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = "2.";
    uint32_t sshlen2 = sizeof(sshbuf2) - 1;
    uint8_t sshbuf3[] = { "0-MySSHClient-0.5.1\r\n"};
    uint32_t sshlen3 = sizeof(sshbuf3) - 1;
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf3, sshlen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_SERVER_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->srv_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->srv_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a banner and record in three chunks. */
static int SSHParserTest11(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "SSH-2.0-MySSHClient-0.5.1\r\n";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = { 0x00, 0x00, 0x00, 0x03, 0x01, 21, 0x00};
    uint32_t sshlen2 = sizeof(sshbuf2);
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_PARSER_DONE)) {
        printf("Didn't detect the msg code of new keys (ciphered data starts): ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a banner and 2 records record in four chunks. */
static int SSHParserTest12(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "SSH-2.0-MySSHClient-0.5.1\r\n";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = { 0x00, 0x00, 0x00, 0x03,0x01, 17, 0x00};
    uint32_t sshlen2 = sizeof(sshbuf2);
    uint8_t sshbuf3[] = { 0x00, 0x00, 0x00, 0x03,0x01, 21, 0x00};
    uint32_t sshlen3 = sizeof(sshbuf3);
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf3, sshlen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_PARSER_DONE)) {
        printf("Didn't detect the msg code of new keys (ciphered data starts): ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a banner and 2 records record in four chunks. */
static int SSHParserTest13(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "SSH-2.0-MySSHClient-0.5.1\r\n";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = { 0x00, 0x00, 0x00, 0x02, 0x01, 17};
    uint32_t sshlen2 = sizeof(sshbuf2);
    uint8_t sshbuf3[] = { 0x00, 0x00, 0x00, 0x02, 0x01, 21};
    uint32_t sshlen3 = sizeof(sshbuf3);
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();
    uint32_t u;

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    for (u = 0; u < sshlen2; u++) {
        SCMutexLock(&f.m);
        r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, &sshbuf2[u], 1);
        if (r != 0) {
            printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
            SCMutexUnlock(&f.m);
            goto end;
        }
        SCMutexUnlock(&f.m);
    }
    for (u = 0; u < sshlen3; u++) {
        SCMutexLock(&f.m);
        r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, &sshbuf3[u], 1);
        if (r != 0) {
            printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
            SCMutexUnlock(&f.m);
            goto end;
        }
        SCMutexUnlock(&f.m);
    }
    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_PARSER_DONE)) {
        printf("Didn't detect the msg code of new keys (ciphered data starts): ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a banner and 2 records record in four chunks. */
static int SSHParserTest14(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "SSH-2.0-MySSHClient-0.5.1\r\n";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = { 0x00, 0x00, 0x00, 0x10, 0x01, 17, 0x00};
    uint32_t sshlen2 = sizeof(sshbuf2);

    uint8_t sshbuf3[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint32_t sshlen3 = sizeof(sshbuf3);
    uint8_t sshbuf4[] = { 0x09, 0x10, 0x11, 0x12, 0x13, 0x00};
    uint32_t sshlen4 = sizeof(sshbuf4);

    /* first byte of this record in sshbuf4 */
    uint8_t sshbuf5[] = { 0x00, 0x00, 0x02, 0x01, 21};
    uint32_t sshlen5 = sizeof(sshbuf5);
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf3, sshlen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf4, sshlen4);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf5, sshlen5);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_PARSER_DONE)) {
        printf("Didn't detect the msg code of new keys (ciphered data starts): ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send a banner and 2 records record in four chunks. */
static int SSHParserTest15(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "SSH-2.0-MySSHClient-0.5.1\r\n";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = { 0x00, 0x00, 0x00, 0x10, 0x01, 17, 0x00};
    uint32_t sshlen2 = sizeof(sshbuf2);

    uint8_t sshbuf3[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint32_t sshlen3 = sizeof(sshbuf3);
    uint8_t sshbuf4[] = { 0x09, 0x10, 0x11, 0x12, 0x13, 0x00};
    uint32_t sshlen4 = sizeof(sshbuf4);

    /* first byte of this record in sshbuf4 */
    uint8_t sshbuf5[] = { 0x00, 0x00, 0x02, 0x01, 20, 0x00, 0x00, 0x00, 0x02, 0x01, 21};
    uint32_t sshlen5 = sizeof(sshbuf5);
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf3, sshlen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf4, sshlen4);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOSERVER, sshbuf5, sshlen5);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->cli_hdr.flags & SSH_FLAG_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->cli_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->cli_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_PARSER_DONE)) {
        printf("Didn't detect the msg code of new keys (ciphered data starts): ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send toserver a banner and record in three chunks. */
static int SSHParserTest16(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "Welcome to this ssh server\nSSH-";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = "2.0-MySSHClient-0.5.1\r\n";
    uint32_t sshlen2 = sizeof(sshbuf2) - 1;
    uint8_t sshbuf3[] = { 0x00, 0x00, 0x00, 0x03,0x01, 21, 0x00};
    uint32_t sshlen3 = sizeof(sshbuf3);
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf3, sshlen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_SERVER_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->srv_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->srv_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_PARSER_DONE)) {
        printf("Didn't detect the msg code of new keys (ciphered data starts): ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/** \test Send toserver a banner and 2 records record in four chunks. */
static int SSHParserTest17(void) {
    int result = 0;
    Flow f;
    uint8_t sshbuf1[] = "Welcome to this ssh server\nSSH-";
    uint32_t sshlen1 = sizeof(sshbuf1) - 1;
    uint8_t sshbuf2[] = "2.0-MySSHClient-0.5.1\r\n";
    uint32_t sshlen2 = sizeof(sshbuf2) - 1;
    uint8_t sshbuf3[] = { 0x00, 0x00, 0x00, 0x03,0x01, 17, 0x00};
    uint32_t sshlen3 = sizeof(sshbuf3);
    uint8_t sshbuf4[] = { 0x00, 0x00, 0x00, 0x03,0x01, 21, 0x00};
    uint32_t sshlen4 = sizeof(sshbuf4);
    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf1, sshlen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf2, sshlen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf3, sshlen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_SSH, STREAM_TOCLIENT, sshbuf4, sshlen4);
    if (r != 0) {
        printf("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    SshState *ssh_state = f.alstate;
    if (ssh_state == NULL) {
        printf("no ssh state: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_SERVER_VERSION_PARSED)) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->srv_hdr.software_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (ssh_state->srv_hdr.proto_version == NULL) {
        printf("Client version string not parsed: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.software_version, "MySSHClient-0.5.1", strlen("MySSHClient-0.5.1")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if (strncmp((char*)ssh_state->srv_hdr.proto_version, "2.0", strlen("2.0")) != 0) {
        printf("Client version string not parsed correctly: ");
        goto end;
    }

    if ( !(ssh_state->flags & SSH_FLAG_PARSER_DONE)) {
        printf("Didn't detect the msg code of new keys (ciphered data starts): ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    return result;
}

#endif /* UNITTESTS */

void SSHParserRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("SSHParserTest01 - ToServer", SSHParserTest01, 1);
    UtRegisterTest("SSHParserTest02 - ToServer", SSHParserTest02, 1);
    UtRegisterTest("SSHParserTest03 - ToServer", SSHParserTest03, 1);
    UtRegisterTest("SSHParserTest04 - ToClient", SSHParserTest04, 1);
    UtRegisterTest("SSHParserTest05 - ToClient", SSHParserTest05, 1);
    UtRegisterTest("SSHParserTest06 - ToClient", SSHParserTest06, 1);
    UtRegisterTest("SSHParserTest07 - ToServer 2 chunks", SSHParserTest07, 1);
    UtRegisterTest("SSHParserTest08 - ToServer 3 chunks", SSHParserTest08, 1);
    UtRegisterTest("SSHParserTest09 - ToClient 2 chunks", SSHParserTest09, 1);
    UtRegisterTest("SSHParserTest10 - ToClient 3 chunks", SSHParserTest10, 1);
    UtRegisterTest("SSHParserTest11 - ToClient 4 chunks", SSHParserTest11, 1);
    UtRegisterTest("SSHParserTest12 - ToClient 4 chunks", SSHParserTest12, 1);
    UtRegisterTest("SSHParserTest13 - ToClient 4 chunks", SSHParserTest13, 1);
    UtRegisterTest("SSHParserTest14 - ToClient 4 chunks", SSHParserTest14, 1);
    UtRegisterTest("SSHParserTest15", SSHParserTest15, 1);
    UtRegisterTest("SSHParserTest16", SSHParserTest16, 1);
    UtRegisterTest("SSHParserTest17", SSHParserTest17, 1);
#endif /* UNITTESTS */
}

