#!/usr/bin/env python3
"""Turn a sparse APFS container image into the seed list apfs_rw_test writes to the phone.

usage: seed-chunks.py IMAGE -o LIST

A container built by mkapfs.py is a sparse file: a 48 GiB one is 150 KB of real data, the rest
holes.  What the flash has to receive is those bytes and zeros where the file has holes, and
that description is much smaller than the image -- small enough to travel in the /redwood image
of a phone boot, which the loader caps at 64 MB with the kernel.

The format (userland/src/apfs_rw_test.c, seed_chunks) is little-endian:
    "HCK1", u32 record count,
    then per record: u32 kind (0 = data, 1 = zeros), u64 offset, u32 length, [data if kind 0]

Records are split at 1 MiB, which is what the test writes a buffer at a time.  The list covers
the image from its first byte to the end of its last data region: what lies beyond is free space
in the container, and the space manager says so -- nothing reads it before writing it.
"""
import argparse
import os
import struct
import sys

CHUNK = 1 << 20


def regions(path):
    """(data, offset, length) pieces of the file, holes included as zeros, up to the last data."""
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        fd = f.fileno()
        off = 0
        out = []
        while off < size:
            try:
                start = os.lseek(fd, off, os.SEEK_DATA)
            except OSError:
                break                       # no more data: the rest is a hole
            end = os.lseek(fd, start, os.SEEK_HOLE)
            out.append(('data', start, end - start))
            off = end
    if not out:
        return []
    last = out[-1][1] + out[-1][2]
    pieces = []
    at = 0
    for kind, start, length in out:
        if start > at:
            pieces.append(('zeros', at, start - at))
        pieces.append((kind, start, length))
        at = start + length
    assert at == last
    return pieces


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('image')
    ap.add_argument('-o', '--output', required=True)
    args = ap.parse_args()

    pieces = regions(args.image)
    with open(args.image, 'rb') as src, open(args.output, 'wb') as out:
        records = []
        for kind, start, length in pieces:
            at = start
            left = length
            while left > 0:
                n = min(left, CHUNK)
                if kind == 'data':
                    src.seek(at)
                    records.append((0, at, n, src.read(n)))
                else:
                    records.append((1, at, n, None))
                at += n
                left -= n
        out.write(b'HCK1')
        out.write(struct.pack('<I', len(records)))
        for kind, off, n, data in records:
            out.write(struct.pack('<IQI', kind, off, n))
            if data is not None:
                out.write(data)
    total = sum(r[2] for r in records)
    print('%s: %d records, %d bytes of image, %d bytes of list' %
          (args.output, len(records), total, os.path.getsize(args.output)))


if __name__ == '__main__':
    sys.exit(main())