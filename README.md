# tftp-lite

**Minimal RFC 1350 TFTP client and server in C — hand-packed packets, lockstep block verification, retransmission, path-traversal defense. Single binary, no dependencies beyond libc.**

```
$ tftp-lite serve /srv/firmware --port 6969
$ tftp-lite get 127.0.0.1 6969 firmware.bin
$ tftp-lite put 127.0.0.1 6969 backup.tar backup.tar
```

TFTP is UDP file transfer with a 512-byte lockstep: block 1 must be
acknowledged before block 2 flies. That makes it the cleanest protocol to
actually read — every packet is one of five tiny shapes — and a perfect
exercise in doing retries, duplicate tolerance and termination *correctly*,
which is where most hand-rolled UDP code quietly rots.

## Why tftp-lite

- **Wire format by hand.** RRQ/WRQ/DATA/ACK/ERROR are packed with explicit
  big-endian byte writes; the parser is strict where RFC 1359-style tools
  usually aren't: ACK is exactly 4 bytes, DATA payloads over 512 are
  rejected on both build and parse, ERROR strings must be NUL-terminated
  inside the packet, request filenames need the double-NUL grammar.
- **Block verification.** Stale ACKs and duplicate DATA are re-answered,
  not fatal; every packet is (re)sent up to `TFTP_MAX_RETRIES` times before
  the transfer aborts; the exact-512-multiple file gets the empty final
  DATA block, both directions.
- **Defensive server.** Filenames must be bare names — `/`, `\`, control
  characters, `.` / `..` components and over-long names are refused with
  an access-violation ERROR before any filesystem touch; half-written
  upload files are unlinked on protocol errors; strangers sending packets
  mid-transfer get "unknown transfer id". 32 MiB cap (65535 blocks) is
  enforced with a disk-full error.
- **octet only, honestly.** `mode` is parsed case-insensitively but only
  `octet` is accepted — silently pretending netascii is binary corrupts
  files, so that request gets an explicit illegal-operation error.
- **One binary, three verbs.** `serve`, `get`, `put`; `get`/`put` accept
  `-` for stdout/stdin piping.
- **Sequential by design.** One transfer at a time keeps the lockstep
  readable; this is a lab tool, not an internet-scale daemon.

## Usage

```sh
tftp-lite serve <dir> [--port N]               # default port 6969
tftp-lite get <host> <port> <remote> [local]   # local '-' = stdout
tftp-lite put <host> <port> <local> [remote]   # local '-' = stdin
```

Exit codes: 0 ok, 1 runtime failure (timeout, server error, missing file),
2 usage error.

## Protocol notes

Transfers run on the server's listening port (RFC 1350 allows allocating a
per-transfer TID; the sequential design makes it unnecessary). Retransmit
policy is symmetric: both sides resend their last packet on a 1-second
receive timeout and give up after 5 consecutive misses. Block numbers wrap
65535 → 0 per the RFC; the block counter guard stops runaways first.

## Development

```sh
make test    # 16 packet-layer test groups + localhost e2e
```

The e2e round-trips a 333 KB random payload, an exact-512-multiple file and
an upload, then asserts the error paths: missing files exit 1, and a
`../` traversal attempt is refused without writing anything.

## License

MIT — see [LICENSE](LICENSE).
