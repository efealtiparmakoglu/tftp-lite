/* tftp.c — RFC 1350 wire format: requests, data, acks, errors. */
#include "tftp.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int build_request(uint8_t *out, size_t cap, int opcode,
                         const char *filename, const char *mode) {
    if (!filename || !mode || !*filename)
        return -1;
    size_t nlen = strlen(filename);
    size_t mlen = strlen(mode);
    if (nlen > TFTP_MAX_NAME || mlen > 15)
        return -1;
    size_t total = 2 + nlen + 1 + mlen + 1;
    if (cap < total)
        return -1;
    out[0] = (uint8_t)(opcode >> 8);
    out[1] = (uint8_t)(opcode & 0xFF);
    memcpy(out + 2, filename, nlen);
    out[2 + nlen] = 0;
    memcpy(out + 3 + nlen, mode, mlen);
    out[2 + nlen + 1 + mlen] = 0;
    for (size_t i = 0; i < mlen; i++) /* modes are case-insensitive */
        out[3 + nlen + i] = (uint8_t)tolower(out[3 + nlen + i]);
    return (int)total;
}

int tftp_build_rrq(uint8_t *out, size_t cap, const char *filename, const char *mode) {
    return build_request(out, cap, TFTP_OP_RRQ, filename, mode);
}

int tftp_build_wrq(uint8_t *out, size_t cap, const char *filename, const char *mode) {
    return build_request(out, cap, TFTP_OP_WRQ, filename, mode);
}

int tftp_build_data(uint8_t *out, size_t cap, uint16_t block,
                    const uint8_t *data, size_t len) {
    if (len > TFTP_BLOCK_SIZE)
        return -1;
    if (cap < 4 + len)
        return -1;
    out[0] = 0;
    out[1] = TFTP_OP_DATA;
    out[2] = (uint8_t)(block >> 8);
    out[3] = (uint8_t)(block & 0xFF);
    if (len)
        memcpy(out + 4, data, len);
    return 4 + (int)len;
}

int tftp_build_ack(uint8_t *out, size_t cap, uint16_t block) {
    if (cap < 4)
        return -1;
    out[0] = 0;
    out[1] = TFTP_OP_ACK;
    out[2] = (uint8_t)(block >> 8);
    out[3] = (uint8_t)(block & 0xFF);
    return 4;
}

int tftp_build_error(uint8_t *out, size_t cap, uint16_t code, const char *msg) {
    size_t mlen = msg ? strlen(msg) : 0;
    if (cap < 4 + mlen + 1)
        return -1;
    out[0] = 0;
    out[1] = TFTP_OP_ERROR;
    out[2] = (uint8_t)(code >> 8);
    out[3] = (uint8_t)(code & 0xFF);
    if (mlen)
        memcpy(out + 4, msg, mlen);
    out[4 + mlen] = 0;
    return 4 + (int)mlen + 1;
}

int tftp_parse(const uint8_t *buf, size_t len, tftp_packet *pkt) {
    if (!buf || !pkt || len < 4)
        return -1;
    memset(pkt, 0, sizeof(*pkt));
    pkt->opcode = (buf[0] << 8) | buf[1];

    switch (pkt->opcode) {
    case TFTP_OP_DATA: {
        pkt->block = (uint16_t)((buf[2] << 8) | buf[3]);
        pkt->data = buf + 4;
        pkt->data_len = len - 4;
        if (pkt->data_len > TFTP_BLOCK_SIZE)
            return -1;
        return 0;
    }
    case TFTP_OP_ACK: {
        if (len != 4) /* RFC 1350: ACK is exactly 4 bytes */
            return -1;
        pkt->block = (uint16_t)((buf[2] << 8) | buf[3]);
        return 0;
    }
    case TFTP_OP_ERROR: {
        pkt->error_code = (uint16_t)((buf[2] << 8) | buf[3]);
        pkt->error_msg = (const char *)buf + 4;
        if (!memchr(buf + 4, 0, len - 4))
            return -1; /* message must be NUL-terminated inside the packet */
        return 0;
    }
    case TFTP_OP_RRQ:
    case TFTP_OP_WRQ: {
        const uint8_t *nul1 = memchr(buf + 2, 0, len - 2);
        if (!nul1)
            return -1;
        size_t nlen = (size_t)(nul1 - (buf + 2));
        if (nlen == 0 || nlen > TFTP_MAX_NAME)
            return -1;
        size_t mode_start = 2 + nlen + 1;
        const uint8_t *nul2 = memchr(buf + mode_start, 0, len - mode_start);
        if (!nul2)
            return -1;
        size_t mlen = (size_t)(nul2 - (buf + mode_start));
        if (mlen == 0 || mlen > 15)
            return -1;
        memcpy(pkt->filename, buf + 2, nlen);
        for (size_t i = 0; i < mlen && i < 15; i++)
            pkt->mode[i] = (char)tolower(buf[mode_start + i]);
        return 0;
    }
    default:
        return -2;
    }
}

int tftp_filename_safe(const char *name) {
    if (!name || !*name)
        return 0;
    size_t len = strlen(name);
    if (len > TFTP_MAX_NAME)
        return 0;
    if (strcmp(name, "..") == 0 || strcmp(name, ".") == 0)
        return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\')
            return 0;
        if ((unsigned char)*p < 0x20)
            return 0;
    }
    /* reject any ".." path component, e.g. "a/../b" */
    const char *p = name;
    while ((p = strstr(p, "..")) != NULL) {
        int left_ok = (p == name) || *(p - 1) == '/';
        int right_ok = 1;
        if (p[2] && p[2] != '/')
            right_ok = 0;
        if (left_ok && right_ok)
            return 0; /* stands alone as a component */
        p += 2;
    }
    return 1;
}

const char *tftp_error_string(uint16_t code) {
    switch (code) {
    case TFTP_ERR_NOT_FOUND:
        return "file not found";
    case TFTP_ERR_ACCESS:
        return "access violation";
    case TFTP_ERR_FULL:
        return "disk full or allocation exceeded";
    case TFTP_ERR_ILLEGAL:
        return "illegal TFTP operation";
    case TFTP_ERR_UNKNOWN_ID:
        return "unknown transfer id";
    default:
        return "unknown error";
    }
}

uint16_t tftp_next_block(uint16_t block) {
    return (uint16_t)(block + 1); /* wraps 65535 -> 0 per RFC 1350 */
}
