/* tftp_tests.c — packet layer unit tests (no network). */
#include <stdio.h>
#include <string.h>

#include "../src/tftp.h"

static int tests_run = 0;

#define FAIL(...)                                                             \
    do {                                                                      \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);                           \
        printf(__VA_ARGS__);                                                  \
        printf("\n");                                                         \
        return 1;                                                             \
    } while (0)

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond))                                                          \
            FAIL("check failed: %s", #cond);                                  \
    } while (0)

static void test_reset(void) { tests_run++; }

static int test_rrq_roundtrip(void) {
    uint8_t buf[TFTP_MAX_PACKET];
    tftp_packet pkt;
    int n = tftp_build_rrq(buf, sizeof buf, "firmware.bin", "octet");
    CHECK(n == 2 + 12 + 1 + 5 + 1);
    CHECK(memcmp(buf, "\x00\x01", 2) == 0);
    CHECK(buf[n - 1] == 0 && buf[n - 2] == 't');
    CHECK(tftp_parse(buf, (size_t)n, &pkt) == 0);
    CHECK(pkt.opcode == TFTP_OP_RRQ);
    CHECK(strcmp(pkt.filename, "firmware.bin") == 0);
    CHECK(strcmp(pkt.mode, "octet") == 0);
    test_reset();
    return 0;
}

static int test_wrq_mode_lowered(void) {
    uint8_t buf[TFTP_MAX_PACKET];
    tftp_packet pkt;
    int n = tftp_build_wrq(buf, sizeof buf, "up.dat", "OcteT");
    CHECK(n > 0);
    CHECK(tftp_parse(buf, (size_t)n, &pkt) == 0);
    CHECK(pkt.opcode == TFTP_OP_WRQ);
    CHECK(strcmp(pkt.mode, "octet") == 0);
    test_reset();
    return 0;
}

static int test_rrq_missing_second_nul(void) {
    /* filename NUL then end of packet: no mode at all */
    uint8_t buf[] = {0x00, 0x01, 'a', '.', 'b', 0x00};
    tftp_packet pkt;
    CHECK(tftp_parse(buf, sizeof buf, &pkt) == -1);
    test_reset();
    return 0;
}

static int test_rrq_empty_filename_rejected(void) {
    uint8_t buf[] = {0x00, 0x01, 0x00, 'o', 'c', 't', 0x00};
    tftp_packet pkt;
    CHECK(tftp_parse(buf, sizeof buf, &pkt) == -1);
    test_reset();
    return 0;
}

static int test_data_roundtrip(void) {
    uint8_t buf[TFTP_MAX_PACKET];
    uint8_t payload[16];
    memset(payload, 0xAB, sizeof payload);
    int n = tftp_build_data(buf, sizeof buf, 0x0102, payload, sizeof payload);
    CHECK(n == 4 + 16);
    tftp_packet pkt;
    CHECK(tftp_parse(buf, (size_t)n, &pkt) == 0);
    CHECK(pkt.opcode == TFTP_OP_DATA);
    CHECK(pkt.block == 0x0102);
    CHECK(pkt.data_len == 16);
    CHECK(pkt.data[0] == 0xAB && pkt.data[15] == 0xAB);
    test_reset();
    return 0;
}

static int test_data_oversized_rejected_on_build(void) {
    uint8_t buf[TFTP_MAX_PACKET + 16];
    uint8_t big[TFTP_BLOCK_SIZE + 1];
    memset(big, 0, sizeof big);
    CHECK(tftp_build_data(buf, sizeof buf, 1, big, TFTP_BLOCK_SIZE + 1) == -1);
    test_reset();
    return 0;
}

static int test_data_oversized_rejected_on_parse(void) {
    uint8_t buf[TFTP_MAX_PACKET + 16] = {0x00, 0x03, 0x00, 0x01};
    tftp_packet pkt;
    CHECK(tftp_parse(buf, sizeof buf, &pkt) == -1); /* 513+ payload */
    test_reset();
    return 0;
}

static int test_data_empty_is_legal(void) {
    /* the final block of an exact multiple of 512 is an empty DATA */
    uint8_t buf[8];
    int n = tftp_build_data(buf, sizeof buf, 7, NULL, 0);
    CHECK(n == 4);
    tftp_packet pkt;
    CHECK(tftp_parse(buf, (size_t)n, &pkt) == 0);
    CHECK(pkt.block == 7 && pkt.data_len == 0);
    test_reset();
    return 0;
}

static int test_ack_strict_length(void) {
    uint8_t buf[4] = {0x00, 0x04, 0x00, 0x07};
    tftp_packet pkt;
    CHECK(tftp_parse(buf, sizeof buf, &pkt) == 0);
    CHECK(pkt.opcode == TFTP_OP_ACK && pkt.block == 7);
    uint8_t broken[5] = {0x00, 0x04, 0x00, 0x07, 0x00};
    CHECK(tftp_parse(broken, sizeof broken, &pkt) == -1); /* 5-byte ACK */
    test_reset();
    return 0;
}

static int test_error_roundtrip(void) {
    uint8_t buf[64];
    int n = tftp_build_error(buf, sizeof buf, TFTP_ERR_NOT_FOUND, "nope");
    CHECK(n == 4 + 4 + 1);
    tftp_packet pkt;
    CHECK(tftp_parse(buf, (size_t)n, &pkt) == 0);
    CHECK(pkt.opcode == TFTP_OP_ERROR);
    CHECK(pkt.error_code == TFTP_ERR_NOT_FOUND);
    CHECK(strcmp(pkt.error_msg, "nope") == 0);
    /* default message when msg == NULL */
    n = tftp_build_error(buf, sizeof buf, TFTP_ERR_ILLEGAL, NULL);
    CHECK(n == 5);
    CHECK(tftp_parse(buf, (size_t)n, &pkt) == 0);
    CHECK(pkt.error_msg[0] == 0);
    test_reset();
    return 0;
}

static int test_error_unterminated_rejected(void) {
    uint8_t buf[] = {0x00, 0x05, 0x00, 0x01, 'x', 'y', 'z'}; /* no NUL */
    tftp_packet pkt;
    CHECK(tftp_parse(buf, sizeof buf, &pkt) == -1);
    test_reset();
    return 0;
}

static int test_malformed_inputs(void) {
    uint8_t buf[16] = {0};
    tftp_packet pkt;
    CHECK(tftp_parse(buf, 3, &pkt) == -1);   /* shorter than a header */
    CHECK(tftp_parse(NULL, 10, &pkt) == -1); /* no buffer */
    buf[0] = 0x00;
    buf[1] = 0x09; /* unknown opcode */
    CHECK(tftp_parse(buf, 4, &pkt) == -2);
    test_reset();
    return 0;
}

static int test_capacity_enforced(void) {
    uint8_t tiny[4];
    uint8_t payload[8] = {0};
    CHECK(tftp_build_rrq(tiny, sizeof tiny, "longname.file", "octet") == -1);
    CHECK(tftp_build_data(tiny, sizeof tiny, 1, payload, sizeof payload) == -1);
    CHECK(tftp_build_ack(tiny, 3, 1) == -1);
    CHECK(tftp_build_error(tiny, sizeof tiny, 1, "quite a long message") == -1);
    test_reset();
    return 0;
}

static int test_block_wrap(void) {
    CHECK(tftp_next_block(1) == 2);
    CHECK(tftp_next_block(65534) == 65535);
    CHECK(tftp_next_block(65535) == 0); /* RFC 1350 wraps */
    test_reset();
    return 0;
}

static int test_error_strings(void) {
    CHECK(strcmp(tftp_error_string(TFTP_ERR_NOT_FOUND), "file not found") == 0);
    CHECK(strcmp(tftp_error_string(TFTP_ERR_ACCESS), "access violation") == 0);
    CHECK(strcmp(tftp_error_string(TFTP_ERR_FULL), "disk full or allocation exceeded") == 0);
    CHECK(strcmp(tftp_error_string(TFTP_ERR_ILLEGAL), "illegal TFTP operation") == 0);
    CHECK(strcmp(tftp_error_string(TFTP_ERR_UNKNOWN_ID), "unknown transfer id") == 0);
    CHECK(strcmp(tftp_error_string(99), "unknown error") == 0);
    test_reset();
    return 0;
}

static int test_filename_safety(void) {
    CHECK(tftp_filename_safe("ok.bin") == 1);
    CHECK(tftp_filename_safe("firmware-2026.tar.gz") == 1);
    CHECK(tftp_filename_safe("") == 0);
    CHECK(tftp_filename_safe(NULL) == 0);
    CHECK(tftp_filename_safe("/") == 0);
    CHECK(tftp_filename_safe("a/b") == 0);
    CHECK(tftp_filename_safe("a\\b") == 0);
    CHECK(tftp_filename_safe("..") == 0);
    CHECK(tftp_filename_safe(".") == 0);
    CHECK(tftp_filename_safe("../escape") == 0);
    CHECK(tftp_filename_safe("a/../b") == 0);
    CHECK(tftp_filename_safe("a..b") == 1);  /* dots inside a name are fine */
    CHECK(tftp_filename_safe("...") == 1);   /* a weird but harmless name */
    char longname[TFTP_MAX_NAME + 2];
    memset(longname, 'a', sizeof longname);
    longname[sizeof longname - 1] = 0;
    CHECK(tftp_filename_safe(longname) == 0); /* too long */
    test_reset();
    return 0;
}

int main(void) {
    int fails = 0;
    fails += test_rrq_roundtrip();
    fails += test_wrq_mode_lowered();
    fails += test_rrq_missing_second_nul();
    fails += test_rrq_empty_filename_rejected();
    fails += test_data_roundtrip();
    fails += test_data_oversized_rejected_on_build();
    fails += test_data_oversized_rejected_on_parse();
    fails += test_data_empty_is_legal();
    fails += test_ack_strict_length();
    fails += test_error_roundtrip();
    fails += test_error_unterminated_rejected();
    fails += test_malformed_inputs();
    fails += test_capacity_enforced();
    fails += test_block_wrap();
    fails += test_error_strings();
    fails += test_filename_safety();
    if (fails) {
        printf("tftp_tests: %d test function(s) FAILED\n", fails);
        return 1;
    }
    printf("tftp_tests: all %d test groups passed\n", tests_run);
    return 0;
}
