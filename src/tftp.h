/* tftp.h — RFC 1350 packet encoding/decoding for tftp-lite. */
#ifndef TFTP_H
#define TFTP_H

#include <stddef.h>
#include <stdint.h>

#define TFTP_OP_RRQ   1
#define TFTP_OP_WRQ   2
#define TFTP_OP_DATA  3
#define TFTP_OP_ACK   4
#define TFTP_OP_ERROR 5

#define TFTP_BLOCK_SIZE  512
#define TFTP_MAX_PACKET  (4 + TFTP_BLOCK_SIZE)
#define TFTP_MAX_RETRIES 5
#define TFTP_MAX_BLOCKS  65535

#define TFTP_ERR_NOT_FOUND 1
#define TFTP_ERR_ACCESS    2
#define TFTP_ERR_FULL      3
#define TFTP_ERR_ILLEGAL   4
#define TFTP_ERR_UNKNOWN_ID 5

#define TFTP_MAX_NAME 255

typedef struct {
    int opcode;
    uint16_t block;       /* DATA / ACK            */
    uint16_t error_code;  /* ERROR                 */
    const char *error_msg;/* ERROR, points into buffer */
    const uint8_t *data;  /* DATA payload          */
    size_t data_len;      /* DATA payload length   */
    char filename[TFTP_MAX_NAME + 1]; /* RRQ / WRQ */
    char mode[16];        /* RRQ / WRQ             */
} tftp_packet;

/* All builders return the wire length, or -1 on overflow/bad input. */
int tftp_build_rrq(uint8_t *out, size_t cap, const char *filename, const char *mode);
int tftp_build_wrq(uint8_t *out, size_t cap, const char *filename, const char *mode);
int tftp_build_data(uint8_t *out, size_t cap, uint16_t block,
                    const uint8_t *data, size_t len);
int tftp_build_ack(uint8_t *out, size_t cap, uint16_t block);
int tftp_build_error(uint8_t *out, size_t cap, uint16_t code, const char *msg);

/* Returns 0 on success, -1 malformed, -2 unknown opcode. */
int tftp_parse(const uint8_t *buf, size_t len, tftp_packet *pkt);

/* 1 when the name is a bare safe file name (no path, no "..", not empty). */
int tftp_filename_safe(const char *name);

const char *tftp_error_string(uint16_t code);
uint16_t tftp_next_block(uint16_t block);

#endif
