#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Patterned IO generator/verifier for pNFS flexfiles striping wire tests.

Every 64-byte record embeds its own absolute file offset, so the DS
backing files are self-describing:

  write      write the pattern to a file (run against the NFS mount)
  verify     read the file back and check every record (end-to-end)
  verify-ds  check one DS backing file directly: records must sit at
             the offset they claim (sparse packing), belong to this
             DS's stripe ((off // su) % width == stripe), and cover
             every block this stripe owns; foreign blocks must be holes
  find-ds    scan a DS backing directory (e.g. the reffs runway pool of
             pool_NNNNNN.dat files) for objects tagged by a write, infer
             each object's stripe index from its embedded offsets, verify
             each, and aggregate coverage: reffs backs one MDS file with
             MULTIPLE pool objects per DS (one per layout segment, ~1 MiB
             windows), so with --size find-ds checks that the union of
             all matched objects covers every block this stripe owns,
             with no block covered twice

Each record carries a tag (default: basename of the written file), so
find-ds can pick our object out of the pre-allocated pool -- the pool
file name says nothing about which MDS file it backs.  Stale objects
from earlier runs survive --fresh (only MDS state is wiped), so use a
distinct file name per run or pass an explicit --tag.

Example, width=2 su=4096, file written on the client:
    stripe_pattern.py write /mnt/point/f1 --size 8M
    stripe_pattern.py verify /mnt/point/f1
    stripe_pattern.py find-ds /exports/DS/1 --tag f1 --width 2 --size 8M
    stripe_pattern.py find-ds /exports/DS/2 --tag f1 --width 2 --size 8M
"""

import argparse
import errno
import os
import sys

RECLEN = 64
MAGIC = b"FFSTRIPE1"
TAGLEN = 20


def clean_tag(s):
    t = "".join(c for c in os.path.basename(s) if c.isalnum() or c in "_-")
    if not t:
        sys.exit("empty tag after sanitizing %r" % s)
    return t[:TAGLEN].encode()


def parse_size(s):
    mult = {"k": 1 << 10, "m": 1 << 20, "g": 1 << 30}
    s = s.lower()
    if s and s[-1] in mult:
        return int(s[:-1]) * mult[s[-1]]
    return int(s)


def record(off, tag):
    r = b"%s off=0x%016x tag=%s" % (MAGIC, off, tag.ljust(TAGLEN))
    return r.ljust(RECLEN - 1, b".") + b"\n"


def make_block(blk_off, su, tag):
    return b"".join(record(blk_off + i, tag) for i in range(0, su, RECLEN))


def parse_record(rec):
    """Return (off, tag) or None if rec is not a well-formed record."""
    if not rec.startswith(MAGIC):
        return None
    try:
        off = int(rec[len(MAGIC) + 5:len(MAGIC) + 23], 16)
    except ValueError:
        return None
    tag = rec[len(MAGIC) + 28:len(MAGIC) + 28 + TAGLEN].rstrip()
    return off, tag


def check_record(rec, expect_off, tag, path, errors):
    parsed = parse_record(rec)
    if parsed is None:
        errors.append("%s: offset 0x%x: malformed record %r" %
                      (path, expect_off, rec[:40]))
        return False
    got_off, got_tag = parsed
    if got_off != expect_off:
        errors.append("%s: offset 0x%x: record claims offset 0x%x" %
                      (path, expect_off, got_off))
        return False
    if tag is not None and got_tag != tag:
        errors.append("%s: offset 0x%x: record tagged %r, expected %r" %
                      (path, expect_off, got_tag, tag))
        return False
    return True


def cmd_write(args):
    size, su = parse_size(args.size), args.su
    tag = clean_tag(args.tag or args.file)
    if size % su:
        sys.exit("size %d not a multiple of stripe unit %d" % (size, su))
    with open(args.file, "wb") as f:
        for off in range(0, size, su):
            f.write(make_block(off, su, tag))
        f.flush()
        os.fsync(f.fileno())
    print("wrote %d bytes (%d blocks of %d) to %s, tag=%s" %
          (size, size // su, su, args.file, tag.decode()))


def cmd_verify(args):
    errors, nrec = [], 0
    tag = clean_tag(args.tag or args.file)
    with open(args.file, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        for off in range(0, size, RECLEN):
            rec = f.read(RECLEN)
            if len(rec) != RECLEN:
                errors.append("%s: short read at 0x%x" % (args.file, off))
                break
            check_record(rec, off, tag, args.file, errors)
            nrec += 1
    return report(args.file, nrec, errors)


def data_extents(fd, end):
    """Yield (start, stop) data extents in [0, end).  Falls back to one
    whole-file extent if the filesystem lacks SEEK_DATA."""
    pos = 0
    while pos < end:
        try:
            start = os.lseek(fd, pos, os.SEEK_DATA)
        except OSError as e:
            if e.errno == errno.ENXIO:      # no data at or past pos
                return
            yield pos, end                  # EINVAL etc: not seek-hole capable
            return
        if start >= end:
            return
        stop = os.lseek(fd, start, os.SEEK_HOLE)
        yield start, min(stop, end)
        pos = stop


def verify_ds_file(path, stripe, width, su, size, tag):
    """Verify one DS object's internal consistency: every non-zero data
    block must hold well-formed records at their claimed absolute
    offsets, tagged `tag`, in a block with this stripe's parity.
    A chunk object legitimately covers only its segment window, so
    coverage completeness is NOT checked here (find-ds aggregates it).
    Returns (exit_status, set of verified block indices)."""
    errors, nrec = [], 0
    covered = set()
    with open(path, "rb") as f:
        fd = f.fileno()
        ds_size = os.fstat(fd).st_size
        span = min(size, ds_size) if size is not None else ds_size
        blocks = set()
        for start, stop in data_extents(fd, span):
            blocks.update(range(start // su, (stop + su - 1) // su))
        for blk in sorted(blocks):
            f.seek(blk * su)
            data = f.read(su)
            if data.count(0) == len(data):
                continue                    # zero-filled: treat as hole
            if blk % width != stripe:
                errors.append("%s: foreign block at 0x%x (stripe %d) "
                              "contains data" %
                              (path, blk * su, blk % width))
                continue
            ok = True
            for i in range(0, len(data), RECLEN):
                if check_record(data[i:i + RECLEN], blk * su + i,
                                tag, path, errors):
                    nrec += 1
                else:
                    ok = False
            if ok:
                covered.add(blk)
    window = ("blocks %d..%d" % (min(covered), max(covered))) if covered \
        else "no data"
    print("%s: stripe %d/%d: %d records in %d blocks (%s)" %
          (path, stripe, width, nrec, len(covered), window))
    return report(path, nrec, errors, quiet=True), covered


def cmd_verify_ds(args):
    size = parse_size(args.size) if args.size else None
    tag = clean_tag(args.tag) if args.tag else None
    rc, _ = verify_ds_file(args.file, args.stripe, args.width, args.su,
                           size, tag)
    return rc


def probe_ds_file(path, width, su, tag):
    """If path's first data is a record tagged `tag`, return the stripe
    index it implies; else return None."""
    try:
        with open(path, "rb") as f:
            fd = f.fileno()
            fsize = os.fstat(fd).st_size
            if fsize == 0:
                return None
            for start, _stop in data_extents(fd, fsize):
                f.seek(start)
                parsed = parse_record(f.read(RECLEN))
                if parsed is None or parsed[1] != tag:
                    return None
                return (parsed[0] // su) % width
    except PermissionError:
        print("WARN: %s: permission denied (run as root?)" % path,
              file=sys.stderr)
    except OSError:
        pass
    return None


def cmd_find_ds(args):
    width, su = args.width, args.su
    size = parse_size(args.size) if args.size else None
    tag = clean_tag(args.tag)
    matches = []
    for name in sorted(os.listdir(args.dir)):
        path = os.path.join(args.dir, name)
        if not os.path.isfile(path):
            continue
        stripe = probe_ds_file(path, width, su, tag)
        if stripe is not None:
            matches.append((path, stripe))
    if not matches:
        print("%s: no object tagged %r found" % (args.dir, tag.decode()))
        return 1
    rc = 0
    stripes = sorted({s for _, s in matches})
    if len(stripes) > 1:
        # one DS dir should serve exactly one stripe role; mixed parity
        # means crossed writes or stale same-tag leftovers
        print("%s: FAIL: objects disagree on stripe parity: %s" %
              (args.dir, ", ".join("%s=stripe %d" % m for m in matches)))
        rc = 1
    covered = {}
    for path, stripe in matches:
        print("%s: tagged %r, inferred stripe %d" %
              (path, tag.decode(), stripe))
        st, blocks = verify_ds_file(path, stripe, width, su, size, tag)
        rc |= st
        for b in blocks:
            covered.setdefault(b, []).append(path)
    # each owned block must be covered exactly once per mirror copy
    bad = {b: v for b, v in covered.items() if len(v) != args.mirrors}
    if bad:
        b = min(bad)
        print("%s: FAIL: %d blocks covered by %s objects, expected %d "
              "(first: 0x%x in %s)" %
              (args.dir, len(bad),
               "/".join(str(n) for n in sorted({len(v)
                                                for v in bad.values()})),
               args.mirrors, b * su,
               ", ".join(bad[b])), file=sys.stderr)
        rc = 1
    if size is not None and len(stripes) == 1:
        expected = set(range(stripes[0], size // su, width))
        missing = sorted(expected - set(covered))
        if missing:
            print("%s: FAIL: %d of %d owned blocks missing (first at 0x%x)" %
                  (args.dir, len(missing), len(expected), missing[0] * su))
            rc = 1
        elif not rc:
            print("%s: COVERAGE OK: %d objects (%d mirror copies), %d/%d "
                  "owned blocks, stripe %d" %
                  (args.dir, len(matches), args.mirrors, len(covered),
                   len(expected), stripes[0]))
    elif size is None:
        print("%s: note: no --size given, coverage completeness not checked"
              % args.dir)
    return rc


def report(path, nrec, errors, quiet=False):
    for e in errors[:20]:
        print("FAIL: " + e, file=sys.stderr)
    if len(errors) > 20:
        print("... and %d more errors" % (len(errors) - 20), file=sys.stderr)
    if errors:
        print("%s: FAIL (%d errors)" % (path, len(errors)))
        return 1
    if not quiet:
        print("%s: OK (%d records)" % (path, nrec))
    else:
        print("%s: OK" % path)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--su", type=int, default=4096,
                   help="stripe unit in bytes (default 4096)")
    sub = p.add_subparsers(dest="cmd", required=True)

    w = sub.add_parser("write", help="write pattern file")
    w.add_argument("file")
    w.add_argument("--size", default="8M", help="file size (default 8M)")
    w.add_argument("--tag", default=None,
                   help="record tag (default: basename of FILE)")

    v = sub.add_parser("verify", help="verify via read-back")
    v.add_argument("file")
    v.add_argument("--tag", default=None,
                   help="expected tag (default: basename of FILE)")

    d = sub.add_parser("verify-ds", help="verify a DS backing file")
    d.add_argument("file")
    d.add_argument("--stripe", type=int, required=True,
                   help="0-based stripe index of this DS")
    d.add_argument("--width", type=int, required=True,
                   help="stripe width W")
    d.add_argument("--size", default=None,
                   help="original file size (default: DS file size)")
    d.add_argument("--tag", default=None,
                   help="expected tag (default: don't check)")

    fd = sub.add_parser("find-ds",
                        help="find and verify tagged objects in a DS dir")
    fd.add_argument("dir")
    fd.add_argument("--tag", required=True,
                    help="tag to search for (basename used at write)")
    fd.add_argument("--width", type=int, required=True,
                    help="stripe width W")
    fd.add_argument("--size", default=None,
                    help="original file size (default: DS file size)")
    fd.add_argument("--mirrors", type=int, default=1,
                    help="mirror copies per DS: each owned block must be "
                         "covered by exactly this many objects (default 1; "
                         "reffs M = layout_width / stripe_width)")

    args = p.parse_args()
    if args.cmd == "write":
        return cmd_write(args)
    if args.cmd == "verify":
        return cmd_verify(args)
    if args.cmd == "verify-ds":
        return cmd_verify_ds(args)
    return cmd_find_ds(args)


if __name__ == "__main__":
    sys.exit(main())
