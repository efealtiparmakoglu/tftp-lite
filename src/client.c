/* client.c — RFC 1350 client: get (RRQ) and put (WRQ) with lockstep
 * retransmission and duplicate tolerance. */
#include "tftp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RECV_TIMEOUT_SEC 1

static int resolve_peer(const char *host, int port, struct sockaddr_in *out) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "tftp-lite: resolve %s: %s\n", host, gai_strerror(rc));
        return -1;
    }
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    return 0;
}

static int open_socket(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    struct timeval tv = {RECV_TIMEOUT_SEC, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return sock;
}

static int report_error(const tftp_packet *parsed) {
    fprintf(stderr, "tftp-lite: server error %u: %s\n", parsed->error_code,
            parsed->error_msg && *parsed->error_msg
                ? parsed->error_msg
                : tftp_error_string(parsed->error_code));
    return -1;
}

/* RRQ then DATA/ACK lockstep until a short block closes the transfer. */
int tftp_get(const char *host, int port, const char *remote, const char *local) {
    struct sockaddr_in peer;
    if (resolve_peer(host, port, &peer) < 0)
        return -1;
    int sock = open_socket();
    if (sock < 0)
        return -1;

    uint8_t tx[TFTP_MAX_PACKET], rx[TFTP_MAX_PACKET];
    int tlen = tftp_build_rrq(tx, sizeof tx, remote, "octet");
    sendto(sock, tx, (size_t)tlen, 0, (struct sockaddr *)&peer, sizeof peer);

    FILE *fp = NULL;
    uint16_t expected = 1;
    int retries = 0;
    tftp_packet parsed;

    for (;;) {
        ssize_t r = recv(sock, rx, sizeof rx, 0);
        if (r < 0) {
            if (++retries > TFTP_MAX_RETRIES) {
                fprintf(stderr, "tftp-lite: %s: no answer (timeout)\n", host);
                goto fail;
            }
            sendto(sock, tx, (size_t)tlen, 0, (struct sockaddr *)&peer,
                   sizeof peer); /* resend last request or last ACK */
            continue;
        }
        if (tftp_parse(rx, (size_t)r, &parsed) != 0) {
            uint8_t err[64];
            int elen = tftp_build_error(err, sizeof err, TFTP_ERR_ILLEGAL, NULL);
            sendto(sock, err, (size_t)elen, 0, (struct sockaddr *)&peer,
                   sizeof peer);
            goto fail;
        }
        retries = 0;
        if (parsed.opcode == TFTP_OP_ERROR) {
            report_error(&parsed);
            goto fail;
        }
        if (parsed.opcode != TFTP_OP_DATA || parsed.block != expected)
            continue; /* stray or duplicate — ACK already flying */

        if (!fp) {
            if (strcmp(local, "-") == 0) {
                fp = stdout;
            } else {
                fp = fopen(local, "wb");
                if (!fp) {
                    perror(local);
                    goto fail;
                }
            }
        }
        if (fwrite(parsed.data, 1, parsed.data_len, fp) != parsed.data_len) {
            uint8_t err[64];
            int elen = tftp_build_error(err, sizeof err, TFTP_ERR_FULL, NULL);
            sendto(sock, err, (size_t)elen, 0, (struct sockaddr *)&peer,
                   sizeof peer);
            goto fail;
        }
        tlen = tftp_build_ack(tx, sizeof tx, expected); /* timeout resend = ACK */
        sendto(sock, tx, (size_t)tlen, 0, (struct sockaddr *)&peer, sizeof peer);
        if (parsed.data_len < TFTP_BLOCK_SIZE) {
            if (fp != stdout)
                fclose(fp);
            close(sock);
            return 0;
        }
        expected = tftp_next_block(expected);
    }
fail:
    if (fp && fp != stdout)
        fclose(fp);
    if (fp == NULL && strcmp(local, "-") != 0)
        unlink(local); /* never leave a half-written file behind */
    close(sock);
    return -1;
}

/* WRQ then DATA/ACK lockstep: push the file block by block. An empty final
 * DATA block is sent when the file size is an exact multiple of 512. */
int tftp_put(const char *host, int port, const char *local, const char *remote) {
    struct sockaddr_in peer;
    if (resolve_peer(host, port, &peer) < 0)
        return -1;
    int sock = open_socket();
    if (sock < 0)
        return -1;
    int is_stdin = strcmp(local, "-") == 0;
    FILE *fp = is_stdin ? stdin : fopen(local, "rb");
    if (!fp) {
        perror(local);
        close(sock);
        return -1;
    }

    uint8_t tx[TFTP_MAX_PACKET], rx[TFTP_MAX_PACKET], chunk[TFTP_BLOCK_SIZE];
    int tlen = tftp_build_wrq(tx, sizeof tx, remote, "octet");
    sendto(sock, tx, (size_t)tlen, 0, (struct sockaddr *)&peer, sizeof peer);

    uint16_t block = 0;
    size_t last_len = TFTP_BLOCK_SIZE; /* pretend, so ACK0 opens the flow */
    int retries = 0;
    tftp_packet parsed;

    for (;;) {
        ssize_t r = recv(sock, rx, sizeof rx, 0);
        if (r < 0) {
            if (++retries > TFTP_MAX_RETRIES) {
                fprintf(stderr, "tftp-lite: %s: no answer (timeout)\n", host);
                goto fail;
            }
            sendto(sock, tx, (size_t)tlen, 0, (struct sockaddr *)&peer,
                   sizeof peer); /* resend WRQ or current DATA */
            continue;
        }
        if (tftp_parse(rx, (size_t)r, &parsed) != 0)
            continue;
        retries = 0;
        if (parsed.opcode == TFTP_OP_ERROR) {
            report_error(&parsed);
            goto fail;
        }
        if (parsed.opcode != TFTP_OP_ACK || parsed.block != block)
            continue; /* stale or stray */

        if (last_len < TFTP_BLOCK_SIZE) {
            fclose(fp);
            close(sock);
            return 0; /* final block acknowledged */
        }
        size_t n = fread(chunk, 1, TFTP_BLOCK_SIZE, fp);
        if (n == 0 && block > 0 && last_len == TFTP_BLOCK_SIZE) {
            /* exact multiple of 512: one empty block closes the transfer */
            n = 0;
        } else if (n == 0 && block == 0) {
            fprintf(stderr, "tftp-lite: %s: empty input\n", local);
            goto fail;
        }
        block = tftp_next_block(block);
        tlen = tftp_build_data(tx, sizeof tx, block, chunk, n);
        last_len = n;
        sendto(sock, tx, (size_t)tlen, 0, (struct sockaddr *)&peer, sizeof peer);
    }
fail:
    if (fp && !is_stdin)
        fclose(fp);
    close(sock);
    return -1;
}
