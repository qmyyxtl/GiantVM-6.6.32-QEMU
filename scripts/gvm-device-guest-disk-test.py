#!/usr/bin/env python3
"""Verify only the two dedicated 1 GiB aggregated data disks using direct I/O.
Writes 8 MiB of deterministic test data to each disk, at offsets 8--22 MiB.
No guest files or filesystems are created. Root system disks are never selected.
"""
import argparse
import sys
import fcntl
import hashlib
import mmap
import os
from pathlib import Path
import struct

SIZE = 128 * 1024
BLOCKS = 16
BASE = 8 * 1024 * 1024
STRIDE = 4 * 1024 * 1024

def require(ok, message):
    if not ok:
        raise RuntimeError(message)

def pattern(owner, cpu, block):
    seed = ("GVM-DISK owner=%d cpu=%d block=%d" % (owner, cpu, block)).encode()
    return hashlib.sha256(seed).digest() * (SIZE // 32)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--backend-owner", type=int, choices=(0, 1))
parser.add_argument("--backend-image", type=Path)
args = parser.parse_args()
if args.backend_owner is not None:
    require(args.backend_image is not None, "backend image required")
    require(args.backend_image.is_file() and args.backend_image.stat().st_size == 1024 ** 3,
            "dedicated regular 1 GiB image required")
    fd = os.open(args.backend_image, os.O_RDONLY)
    try:
        for cpu in (0, 1, 2, 3):
            for index in range(BLOCKS):
                offset = BASE + cpu * STRIDE + index * SIZE
                require(os.pread(fd, SIZE, offset) == pattern(args.backend_owner, cpu, index),
                        "host backend data mismatch cpu=%d block=%d" % (cpu, index))
        print("HOST_BACKEND_PASS owner=%d blocks=64 bytes=8388608" % args.backend_owner,
              flush=True)
    finally:
        os.close(fd)
    sys.exit(0)
require(args.backend_image is None, "specify backend owner with image")

affinity = os.sched_getaffinity(0)
fds = []
try:
    for owner, slot in ((0, "09"), (1, "0a")):
        pci = Path("/sys/bus/pci/devices/0000:00:" + slot + ".0")
        blocks = list(pci.glob("virtio*/block/*"))
        require(len(blocks) == 1, "expected one disk at owner BDF " + slot)
        block = blocks[0]
        require((block / "device/driver").resolve().name == "virtio_blk",
                "native virtio_blk driver required")
        require((block / "serial").read_text().strip() == "GVM-DATA-OWNER%d" % owner,
                "dedicated data disk serial required")
        dev = "/dev/" + block.name
        require(not list(block.glob(block.name + "[0-9]*")), "partitioned disk refused")
        fd = os.open(dev, os.O_RDWR | os.O_DIRECT)
        fds.append(fd)
        size = struct.unpack("Q", fcntl.ioctl(fd, 0x80081272, bytes(8)))[0]
        require(size == 1024 ** 3, "dedicated disk must be 1 GiB")
        print("DISK_READY owner=%d pci=00:%s.0 device=%s bytes=%d driver=virtio_blk" %
              (owner, slot, dev, size), flush=True)

    with mmap.mmap(-1, SIZE) as buf:
        view = memoryview(buf)
        try:
            for cpu in (0, 1, 2, 3):
                for owner, fd in enumerate(fds):
                    os.sched_setaffinity(0, {cpu})
                    for index in range(BLOCKS):
                        offset = BASE + cpu * STRIDE + index * SIZE
                        buf[:] = pattern(owner, cpu, index)
                        require(os.pwritev(fd, [view], offset) == SIZE, "short direct write")
                    os.fsync(fd)
                    reader = (cpu + 2) % 4
                    os.sched_setaffinity(0, {reader})
                    for index in reversed(range(BLOCKS)):
                        offset = BASE + cpu * STRIDE + index * SIZE
                        buf[:] = bytes(SIZE)
                        require(os.preadv(fd, [view], offset) == SIZE, "short direct read")
                        require(buf[:] == pattern(owner, cpu, index),
                                "disk mismatch owner=%d writer=%d reader=%d block=%d" %
                                (owner, cpu, reader, index))
                    print("DISK_PASS owner=%d writer_cpu=%d reader_cpu=%d bytes=%d direct_io=yes" %
                          (owner, cpu, reader, SIZE * BLOCKS), flush=True)

            # Every CPU independently verifies both complete persisted regions.
            for reader in (0, 1, 2, 3):
                os.sched_setaffinity(0, {reader})
                for owner, fd in enumerate(fds):
                    for writer in (0, 1, 2, 3):
                        for index in range(BLOCKS):
                            offset = BASE + writer * STRIDE + index * SIZE
                            require(os.preadv(fd, [view], offset) == SIZE, "short read")
                            require(buf[:] == pattern(owner, writer, index),
                                    "persisted data mismatch")
                    print("DISK_FULL_READ_PASS owner=%d reader_cpu=%d bytes=%d" %
                          (owner, reader, SIZE * BLOCKS * 4), flush=True)
        finally:
            view.release()
    print("PASS two independent aggregated disks: 16 MiB written, 80 MiB directly read and verified",
          flush=True)
finally:
    os.sched_setaffinity(0, affinity)
    for fd in fds:
        os.close(fd)
