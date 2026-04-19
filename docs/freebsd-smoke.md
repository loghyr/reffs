# reffsd FreeBSD smoke test

Minimum validation that the kqueue-backend data path (PR #7) works
end-to-end: a Linux NFSv4.1 client can mount reffsd on FreeBSD,
write a multi-MB file, and read it back byte-identical.

## Environment

Host | OS | Role
---|---|---
witchie | FreeBSD 15.0-RELEASE-p5 | reffsd server
any Linux host | NFSv4.1-capable kernel | client

Both hosts on the same network, client can resolve the server, no
firewall blocking port 2049.

## Build

On witchie, from a branch worktree:

```
autoreconf -i
PATH=$HOME/.local/bin:$PATH ./configure --disable-tsan --disable-asan
PATH=$HOME/.local/bin:$PATH gmake -j4
```

Expected: clean build, `src/reffsd` produced, links against
`libhdr_histogram.so` (if HdrHistogram_c is installed) and libc +
libexecinfo + libpthread + OpenSSL.

## Initialize state directory

```
sudo mkdir -p /var/lib/reffs
sudo chown $USER /var/lib/reffs
```

## Run reffsd

```
./src/reffsd --foreground --log-level=debug 2>&1 | tee reffs.log
```

Expected log output within the first second:

- `io_handler_init: started (kqueue fd=N)` for the network and
  backend rings
- `io: conn_buffers alias` NOT appearing (that would indicate a
  MAX_CONNECTIONS hash collision on startup)
- `Listener socket` messages for IPv4 + IPv6 on port 2049

## Mount from Linux client

```
sudo mount -t nfs -o vers=4.1,sec=sys,rsize=65536,wsize=65536 \
    witchie:/ /mnt/nfs
```

Expected: mount succeeds within 1 s. Server log shows
`io_handle_accept` → `io_conn_register fd=N state=ACCEPTED` →
initial NFS NULL/COMPOUND exchange → no errors.

## Smoke I/O

```
dd if=/dev/urandom of=/mnt/nfs/test1 bs=1M count=16 conv=fsync
dd if=/mnt/nfs/test1 of=/tmp/r bs=1M
# Compare
dd if=/dev/urandom of=/tmp/expected bs=1M count=16
# (regenerate from same seed, or use sha256 round-trip)
sha256sum /mnt/nfs/test1 /tmp/r
# The two sha256 must match
```

Easier verification (matches by content, not regeneration):

```
dd if=/dev/zero of=/mnt/nfs/zeros bs=1M count=8 conv=fsync
sha256sum /mnt/nfs/zeros
# Expect: ba9b6f... (standard 8MB-of-zero sha256)
```

Expected: full write, server log shows repeated
`write_and_dispatch` → `io_handle_write` cycles, no
`Write operation failed` entries, no `kevent: %s` errors.

## Concurrent writes (write gate exercise)

Two clients (or two mounts from one client) writing to separate
files concurrently:

```
# Terminal 1
dd if=/dev/zero of=/mnt/nfs/c1 bs=1M count=64 conv=fsync &
# Terminal 2
dd if=/dev/zero of=/mnt/nfs/c2 bs=1M count=64 conv=fsync &
wait
sha256sum /mnt/nfs/c1 /mnt/nfs/c2
```

Both should finish and both sums match the expected 64MB-zero hash.
Server log must not show interleaved fragment writes on the same fd
(the per-fd write gate prevents this; a tcpdump capture confirms).

## Graceful disconnect

```
# On client
dd if=/dev/zero of=/mnt/nfs/big bs=1M count=128 &
sleep 1
kill -9 %1
sudo umount -f /mnt/nfs
```

Server log must show the connection close, `io_conn_unregister
fd=N`, and no leaked io_context. Check:

```
# On server, check for leak
ls /proc/self/fd 2>/dev/null || fstat -p $(pgrep reffsd) | wc -l
```

fd count should drop back to baseline within 5 s.

## Known limitations (this PR)

- **Heartbeat is stubbed.** A single `io_request_accept_op` failure
  (e.g., transient ENOMEM, EMFILE) makes the listener permanently
  unavailable on the kqueue backend — no watchdog resubmits.  PR #10
  fixes via EVFILT_TIMER.  Manual restart is the fallback.
- **TLS is not initialized.**  PR #8 ports userspace BIO TLS;
  `sec=sys` is the only supported security flavor on FreeBSD today.
- **`io_send_request` is a stub.**  No client-initiated RPC paths
  (probe1_client, NFSv4 backchannel) work on FreeBSD.  Server-side
  CB_RECALL also uses the write path through `io_rpc_trans_cb` —
  which does work — but the round-trip relies on the same stub.
  Not exercised by the basic read/write cthon tests.
- **Idle-connection timeout is off.**  Without heartbeat, long-idle
  connections accumulate; acceptable for smoke validation, not for
  long-running deployments.

## On failure

If the smoke test fails, collect:

1. `reffs.log` up to the failure point
2. `tcpdump -s 0 -w dump.pcap -i any port 2049` during the test
3. `fstat -p $(pgrep reffsd)` showing fd state
4. `vmstat 1 5` for any memory pressure

Expected failure modes from known bugs:

- `handle_tls_handshake` failing with `Failed to create SSL` — a
  client sent what looks like a TLS ClientHello.  Harmless log
  noise on FreeBSD PR #7; connection is closed.
- `kevent: Too many open files` — raise `kern.maxfiles` and
  `kern.maxfilesperproc`.
