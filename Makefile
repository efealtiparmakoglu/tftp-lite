CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wpedantic

SRC = src/tftp.c src/server.c src/client.c src/main.c

all: tftp-lite

tftp-lite: $(SRC) src/tftp.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

tftp_tests: tests/tftp_tests.c src/tftp.c src/tftp.h
	$(CC) $(CFLAGS) -o tftp_tests tests/tftp_tests.c src/tftp.c

test: tftp-lite tftp_tests
	./tftp_tests
	mkdir -p /tmp/tftp-e2e && chmod 700 /tmp/tftp-e2e
	rm -f /tmp/tftp-e2e/*
	dd if=/dev/urandom of=/tmp/tftp-e2e/payload.bin bs=1000 count=333 2>/dev/null
	dd if=/dev/urandom of=/tmp/tftp-e2e/upload.bin bs=513 count=301 2>/dev/null
	head -c 512 /dev/urandom > /tmp/tftp-e2e/exact.bin
	( \
	./tftp-lite serve /tmp/tftp-e2e --port 47696 >/dev/null 2>/tmp/tftp-e2e/server.log & \
	SRV=$$!; sleep 0.3; \
	./tftp-lite get 127.0.0.1 47696 payload.bin /tmp/tftp-e2e/got.bin || exit 1; \
	cmp /tmp/tftp-e2e/payload.bin /tmp/tftp-e2e/got.bin || exit 1; \
	./tftp-lite get 127.0.0.1 47696 exact.bin /tmp/tftp-e2e/exact.got || exit 1; \
	cmp /tmp/tftp-e2e/exact.bin /tmp/tftp-e2e/exact.got || exit 1; \
	./tftp-lite put 127.0.0.1 47696 /tmp/tftp-e2e/upload.bin upload2.bin || exit 1; \
	cmp /tmp/tftp-e2e/upload.bin /tmp/tftp-e2e/upload2.bin || exit 1; \
	./tftp-lite get 127.0.0.1 47696 missing.bin /tmp/tftp-e2e/never >/dev/null 2>&1; \
	test $$? -eq 1 || { echo "e2e: missing-file get should exit 1"; exit 1; }; \
	./tftp-lite get 127.0.0.1 47696 ../payload.bin /tmp/tftp-e2e/never2 >/dev/null 2>&1; \
	test $$? -eq 1 || { echo "e2e: traversal should exit 1"; exit 1; }; \
	test ! -f /tmp/tftp-e2e/never2 || { echo "e2e: traversal wrote a file!"; exit 1; }; \
	kill $$SRV; \
	) && echo "e2e: get/put/exact-multiple/error/traversal-defense OK"

clean:
	rm -f tftp-lite tftp_tests
	rm -rf /tmp/tftp-e2e

.PHONY: all test clean
