/* main.c — tftp-lite CLI: serve | get | put */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tftp_serve(const char *dir, int port);
int tftp_get(const char *host, int port, const char *remote, const char *local);
int tftp_put(const char *host, int port, const char *local, const char *remote);

static const char *usage_text =
    "tftp-lite 1.0.0 — minimal RFC 1350 TFTP client/server (octet mode)\n"
    "\n"
    "usage:\n"
    "  tftp-lite serve <dir> [--port N]      share DIR over udp (default 6969)\n"
    "  tftp-lite get <host> <port> <remote> [local]   download (- = stdout)\n"
    "  tftp-lite put <host> <port> <local> [remote]   upload (- = stdin)\n"
    "\n"
    "exit codes: 0 ok, 1 runtime failure, 2 usage error\n";

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fputs(usage_text, stderr);
        return 2;
    }

    if (strcmp(argv[1], "serve") == 0) {
        if (argc < 3) {
            fputs(usage_text, stderr);
            return 2;
        }
        const char *dir = argv[2];
        int port = 6969;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = atoi(argv[++i]);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "tftp-lite: bad port\n");
                    return 2;
                }
            } else {
                fprintf(stderr, "tftp-lite: unknown option %s\n", argv[i]);
                return 2;
            }
        }
        return tftp_serve(dir, port);
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc < 5) {
            fputs(usage_text, stderr);
            return 2;
        }
        const char *host = argv[2];
        int port = atoi(argv[3]);
        const char *remote = argv[4];
        const char *local = argc > 5 ? argv[5] : remote;
        return tftp_get(host, port, remote, local) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "put") == 0) {
        if (argc < 5) {
            fputs(usage_text, stderr);
            return 2;
        }
        const char *host = argv[2];
        int port = atoi(argv[3]);
        const char *local = argv[4];
        const char *remote = argc > 5 ? argv[5] : local;
        return tftp_put(host, port, local, remote) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0) {
        puts("tftp-lite 1.0.0");
        return 0;
    }

    fprintf(stderr, "tftp-lite: unknown command '%s'\n", argv[1]);
    fputs(usage_text, stderr);
    return 2;
}
