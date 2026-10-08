#!/usr/bin/env python3
"""Run in the guest. No output files; DMA uses boot-reserved guest RAM.

EDU MSI destination testing uses Linux's existing IRQ-work vector (x86 0xf6).
This validates delivery to the requested CPU without a new guest module.
It does not substitute for a PCI driver's Linux IRQ allocation/affinity test.
"""
import ctypes
import math
import mmap
import os
from pathlib import Path
import struct
import time

BDFS = ("0000:00:05.0", "0000:00:06.0")
CPUS = (0, 2)
PAGE = 4096
DMA_GPA = 0x08000000

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def read32(bar, offset):
    return ctypes.c_uint32.from_buffer(bar, offset).value

def write32(bar, offset, value):
    ctypes.c_uint32.from_buffer(bar, offset).value = value

def read64(bar, offset):
    return ctypes.c_uint64.from_buffer(bar, offset).value

def write64(bar, offset, value):
    ctypes.c_uint64.from_buffer(bar, offset).value = value

def wait_for(predicate, message):
    deadline = time.monotonic() + 5
    while not predicate():
        require(time.monotonic() < deadline, message)
        time.sleep(0.002)

def irq_work():
    for line in Path("/proc/interrupts").read_text().splitlines():
        if line.lstrip().startswith("IWI:"):
            return [int(x) for x in line.split()[1:5]]
    raise RuntimeError("guest has no IRQ-work interrupt counters")

def main():
    affinity = os.sched_getaffinity(0)
    maps = []
    configs = []
    commands = []
    dma = None
    try:
        for bdf in BDFS:
            path = Path("/sys/bus/pci/devices") / bdf
            config = os.open(path / "config", os.O_RDWR)
            configs.append(config)
            require(os.pread(config, 4, 0) == struct.pack("<I", 0x11e81234),
                    f"{bdf}: EDU PCI identity")
            command = os.pread(config, 2, 4)
            commands.append(command)
            os.pwrite(config, struct.pack("<H", struct.unpack("<H", command)[0] | 6), 4)
            fd = os.open(path / "resource0", os.O_RDWR | os.O_SYNC)
            maps.append(mmap.mmap(fd, PAGE, mmap.MAP_SHARED,
                                  mmap.PROT_READ | mmap.PROT_WRITE))
            os.close(fd)
        previous = [None, None]
        for cpu in CPUS:
            os.sched_setaffinity(0, {cpu})
            for owner, bar in enumerate(maps):
                require(read32(bar, 0) == 0x010000ed, "EDU BAR identity")
                if previous[owner] is not None:
                    require(read32(bar, 4) == previous[owner], "unique owner state")
                value = 0x12340000 | (cpu << 8) | owner
                write32(bar, 4, value)
                previous[owner] = (~value) & 0xffffffff
                require(read32(bar, 4) == previous[owner], "ordered BAR write/read")
                n = 5 + cpu + owner
                write32(bar, 8, n)
                wait_for(lambda: not read32(bar, 0x20) & 1, "factorial timeout")
                require(read32(bar, 8) == math.factorial(n), "owner factorial")
                print(f"PASS BAR owner={owner} submit_cpu={cpu}", flush=True)

        require("08000000-0800ffff : Reserved" in Path("/proc/iomem").read_text(),
                "DMA RAM must be reserved by memmap=64K$0x08000000")
        fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        dma = mmap.mmap(fd, 2 * PAGE, mmap.MAP_SHARED,
                        mmap.PROT_READ | mmap.PROT_WRITE, offset=DMA_GPA)
        os.close(fd)
        for cpu in CPUS:
            os.sched_setaffinity(0, {cpu})
            for owner, bar in enumerate(maps):
                pattern = bytes((i + cpu * 17 + owner * 31) & 255 for i in range(PAGE))
                dma[:PAGE] = pattern
                dma[PAGE:] = bytes([0xa5]) * PAGE
                write64(bar, 0x80, DMA_GPA)
                write64(bar, 0x88, 0x40000)
                write64(bar, 0x90, PAGE)
                write64(bar, 0x98, 1)
                wait_for(lambda: not read64(bar, 0x98) & 1, "DMA read timeout")
                write64(bar, 0x80, 0x40000)
                write64(bar, 0x88, DMA_GPA + PAGE)
                write64(bar, 0x90, PAGE)
                write64(bar, 0x98, 3)
                wait_for(lambda: not read64(bar, 0x98) & 1, "DMA write timeout")
                require(dma[PAGE:] == pattern, "DMA data mismatch")
                os.sched_setaffinity(0, {2 if cpu == 0 else 0})
                require(dma[PAGE:] == pattern, "remote CPU sees stale DMA data")
                os.sched_setaffinity(0, {cpu})
                print(f"PASS DMA owner={owner} submit_cpu={cpu} bytes={PAGE}", flush=True)

        # EDU has one 64-bit MSI capability; use a safe, existing kernel handler.
        for owner, (config, bar) in enumerate(zip(configs, maps)):
            cap = os.pread(config, 1, 0x34)[0]
            seen = set()
            while cap and cap not in seen and os.pread(config, 1, cap)[0] != 5:
                seen.add(cap)
                cap = os.pread(config, 1, cap + 1)[0]
            require(cap and os.pread(config, 1, cap)[0] == 5, "MSI capability")
            original = os.pread(config, 16, cap)
            control = struct.unpack("<H", original[2:4])[0]
            data_offset = cap + (12 if control & 0x80 else 8)
            try:
                for target in CPUS:
                    os.pwrite(config, struct.pack("<I", 0xfee00000 | (target << 12)), cap + 4)
                    if control & 0x80:
                        os.pwrite(config, bytes(4), cap + 8)
                    os.pwrite(config, struct.pack("<H", 0xf6), data_offset)
                    os.pwrite(config, struct.pack("<H", control | 1), cap + 2)
                    for submit in CPUS:
                        os.sched_setaffinity(0, {submit})
                        for _ in range(8):
                            before = irq_work()[target]
                            write32(bar, 0x60, 1)
                            wait_for(lambda: irq_work()[target] > before, "MSI timeout")
                            write32(bar, 0x64, 1)
                        print(f"PASS MSI owner={owner} submit_cpu={submit} "
                              f"irq_cpu={target} count=8", flush=True)
            finally:
                os.pwrite(config, struct.pack("<H", control & ~1), cap + 2)
                os.pwrite(config, original[4:16], cap + 4)
                os.pwrite(config, original[2:4], cap + 2)

        children = []
        for index, cpu in enumerate(CPUS):
            pid = os.fork()
            if not pid:
                try:
                    os.sched_setaffinity(0, {cpu})
                    bar = maps[1-index]
                    for n in range(128):
                        value = (cpu << 24) | n
                        write32(bar, 4, value)
                        require(read32(bar, 4) == ((~value) & 0xffffffff),
                                "concurrent remote BAR")
                    os._exit(0)
                except BaseException:
                    os._exit(1)
            children.append(pid)
        for pid in children:
            _, status = os.waitpid(pid, 0)
            require(status == 0, "concurrent remote requests failed")
        print("PASS concurrent bidirectional owner access", flush=True)
        print("PASS distributed PCI/BAR/DMA/MSI matrices", flush=True)
    finally:
        os.sched_setaffinity(0, affinity)
        if dma is not None:
            dma.close()
        for bar in maps:
            bar.close()
        for config, command in zip(configs, commands):
            os.pwrite(config, command, 4)
            os.close(config)

if __name__ == "__main__":
    main()
