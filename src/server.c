/* server.c — sequential RFC 1350 server: RRQ (read) and WRQ (write)
 * lockstep transfers over one UDP socket, with retransmit and path checks. */
#include "tftp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RECV_TIMEOUT_SEC 1

static void send_error(int sock, const struct sockaddr *addr, socklen_t alen,
                       uint16_t code, const char *msg) {
    uint8_t pkt[TFTP_MAX_PACKET];
    int len = tftp_build_error(pkt, sizeof pkt, code, msg);
    if (len > 0)
        sendto(sock, pkt, (size_t)len, 0, addr, alen);
}

/* Receive one datagram from the transfer peer; strangers get error 5. */
static ssize_t recv_from_peer(int sock, uint8_t *rx, size_t cap,
                              const struct sockaddr_in *peer) {
    struct sockaddr_in from;
    socklen_t flen;
    ssize_t r;
    for (;;) {
        flen = sizeof from;
        r = recvfrom(sock, rx, cap, 0, (struct sockaddr *)&from, &flen);
        if (r < 0)
            return -1;
        if (from.sin_addr.s_addr == peer->sin_addr.s_addr &&
            from.sin_port == peer->sin_port)
            return r;
        send_error(sock, (struct sockaddr *)&from, flen,
                   TFTP_ERR_UNKNOWN_ID, NULL);
    }
}

/* RRQ: push the file block by block, resending on lost ACKs. */
static int serve_read(int sock, const struct sockaddr_in *peer, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        send_error(sock, (struct sockaddr *)peer, sizeof *peer,
                   errno == ENOENT ? TFTP_ERR_NOT_FOUND : TFTP_ERR_ACCESS,
                   NULL);
        return -1;
    }
    uint8_t pkt[TFTP_MAX_PACKET], rx[TFTP_MAX_PACKET];
    tftp_packet parsed;
    uint16_t block = 1;
    int retries = 0;

    for (;;) {
        if (block == 0) { /* 65536th block: > 32 MiB */
            fclose(fp);
            send_error(sock, (struct sockaddr *)peer, sizeof *peer,
                       TFTP_ERR_FULL, NULL);
            return -1;
        }
        uint8_t payload[TFTP_BLOCK_SIZE];
        size_t n = fread(payload, 1, TFTP_BLOCK_SIZE, fp);
        int eof_done = n < TFTP_BLOCK_SIZE;
        int plen = tftp_build_data(pkt, sizeof pkt, block, payload, n);
        if (plen < 0) {
            fclose(fp);
            return -1;
        }
        sendto(sock, pkt, (size_t)plen, 0, (struct sockaddr *)peer, sizeof *peer);
        ssize_t r = recv_from_peer(sock, rx, sizeof rx, peer);
        if (r < 0) {
            if (++retries > TFTP_MAX_RETRIES) {
                fprintf(stderr, "tftp-lite: peer lost during read\n");
                fclose(fp);
                return -1;
            }
            continue; /* retransmit the same DATA */
        }
        retries = 0;
        if (tftp_parse(rx, (size_t)r, &parsed) != 0 ||
            parsed.opcode != TFTP_OP_ACK)
            continue; /* junk — retransmit */
        if (parsed.block != block)
            continue; /* stale ACK — retransmit */
        if (eof_done) {
            fclose(fp);
            return 0;
        }
        block = tftp_next_block(block);
    }
}

/* WRQ: ACK 0, then consume DATA in lockstep, tolerating duplicates. */
static int serve_write(int sock, const struct sockaddr_in *peer, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        send_error(sock, (struct sockaddr *)peer, sizeof *peer,
                   TFTP_ERR_ACCESS, NULL);
        return -1;
    }
    uint8_t rx[TFTP_MAX_PACKET], ackpkt[4];
    tftp_packet parsed;
    uint16_t expected = 1;
    uint16_t acked = 0; /* last ACK sent */
    int retries = 0;

    tftp_build_ack(ackpkt, sizeof ackpkt, acked); /* ACK0 opens the transfer */
    sendto(sock, ackpkt, 4, 0, (struct sockaddr *)peer, sizeof *peer);

    for (;;) {
        ssize_t r = recv_from_peer(sock, rx, sizeof rx, peer);
        if (r < 0) {
            if (++retries > TFTP_MAX_RETRIES) {
                fprintf(stderr, "tftp-lite: peer lost during write\n");
                fclose(fp);
                return -1;
            }
            tftp_build_ack(ackpkt, sizeof ackpkt, acked); /* resend */
            sendto(sock, ackpkt, 4, 0, (struct sockaddr *)peer, sizeof *peer);
            continue;
        }
        retries = 0;
        if (tftp_parse(rx, (size_t)r, &parsed) != 0) {
            send_error(sock, (struct sockaddr *)peer, sizeof *peer,
                       TFTP_ERR_ILLEGAL, NULL);
            fclose(fp);
            unlink(path); /* half-written files are worse than no file */
            return -1;
        }
        if (parsed.opcode == TFTP_OP_ERROR) {
            fprintf(stderr, "tftp-lite: client error: %s\n",
                    parsed.error_msg ? parsed.error_msg : "?");
            fclose(fp);
            return -1;
        }
        if (parsed.opcode != TFTP_OP_DATA)
            continue;
        if (parsed.block == expected) {
            if (fwrite(parsed.data, 1, parsed.data_len, fp) != parsed.data_len) {
                send_error(sock, (struct sockaddr *)peer, sizeof *peer,
                           TFTP_ERR_FULL, NULL);
                fclose(fp);
                return -1;
            }
            acked = expected;
            expected = tftp_next_block(expected);
        }
        /* fresh block, duplicate or resync: (re)ACK what we hold */
        tftp_build_ack(ackpkt, sizeof ackpkt, acked);
        sendto(sock, ackpkt, 4, 0, (struct sockaddr *)peer, sizeof *peer);
        if (parsed.block == acked && parsed.data_len < TFTP_BLOCK_SIZE &&
            parsed.opcode == TFTP_OP_DATA && acked == expected - 1) {
            fclose(fp);
            return 0; /* final block acknowledged: transfer complete */
        }
    }
}

int tftp_serve(const char *dir, int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct timeval tv = {RECV_TIMEOUT_SEC, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }
    printf("tftp-lite: serving %s on udp/%d (octet mode, sequential)\n",
           dir, port);

    uint8_t rx[TFTP_MAX_PACKET];
    tftp_packet req;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        ssize_t r = recvfrom(sock, rx, sizeof rx, 0,
                             (struct sockaddr *)&peer, &plen);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            perror("recvfrom");
            break;
        }
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);

        if (tftp_parse(rx, (size_t)r, &req) != 0) {
            send_error(sock, (struct sockaddr *)&peer, plen,
                       TFTP_ERR_ILLEGAL, NULL);
            continue;
        }
        if (req.opcode != TFTP_OP_RRQ && req.opcode != TFTP_OP_WRQ) {
            send_error(sock, (struct sockaddr *)&peer, plen,
                       TFTP_ERR_ILLEGAL, NULL);
            continue;
        }
        if (strcmp(req.mode, "octet") != 0) {
            fprintf(stderr, "tftp-lite: %s wants mode '%s' (only octet)\n",
                    ip, req.mode);
            send_error(sock, (struct sockaddr *)&peer, plen,
                       TFTP_ERR_ILLEGAL, "only octet mode is supported");
            continue;
        }
        if (!tftp_filename_safe(req.filename)) {
            fprintf(stderr, "tftp-lite: refusing unsafe name '%s' from %s\n",
                    req.filename, ip);
            send_error(sock, (struct sockaddr *)&peer, plen,
                       TFTP_ERR_ACCESS, "illegal file name");
            continue;
        }
        char path[TFTP_MAX_NAME + 1024];
        snprintf(path, sizeof path, "%s/%s", dir, req.filename);

        fprintf(stderr, "tftp-lite: %s %s from %s\n",
                req.opcode == TFTP_OP_RRQ ? "GET" : "PUT",
                req.filename, ip);
        if (req.opcode == TFTP_OP_RRQ)
            serve_read(sock, &peer, path);
        else
            serve_write(sock, &peer, path);
    }
    close(sock);
    return 1;
}
