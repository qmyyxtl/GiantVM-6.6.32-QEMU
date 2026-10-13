#!/usr/bin/env python3
"""Owner-aware sequential read bandwidth using native Linux AIO, without files.
Requires dedicated 1 GiB GVM data disks. Read-only variant; preserves existing data.
This measures the current QEMU/raw/controller path, not media steady-state speed.
"""
import argparse
import ctypes as C
import fcntl
import json
import mmap
import os
from pathlib import Path
import select
import statistics
import struct
import time
import traceback

def parse_size(value):
    value = value.strip().lower()
    scale = {'k':1024, 'm':1024**2, 'g':1024**3}
    return int(value[:-1])*scale[value[-1]] if value[-1] in scale else int(value)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--owner', type=int, choices=(0,1))
parser.add_argument('--block-size', type=parse_size, default=1024**2)
parser.add_argument('--queue-depth', type=int, default=4)
parser.add_argument('--workers-per-owner', type=int, choices=(1,2), default=2)
parser.add_argument('--seconds', type=float, default=15)
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--prefill-bytes', type=parse_size, default=1024**3)
parser.add_argument('--prepare-only', action='store_true')
parser.add_argument('--skip-prefill', action='store_true')
parser.add_argument('--cpu-node', type=int, choices=(0,1), help='CPU/memory first-touch node; default is each disk owner')
parser.add_argument('--no-touch-pages', action='store_true', help='match historical local aggregation test without consuming every page')
args = parser.parse_args()
TOUCH_DATA = not args.no_touch_pages
BS, QD, SECONDS, ROUNDS = args.block_size, args.queue_depth, args.seconds, args.rounds
CAPACITY = 1024 ** 3
PREFILL_BYTES = args.prefill_bytes
WORKERS = args.workers_per_owner
REGION = CAPACITY // WORKERS
if REGION % BS or PREFILL_BYTES % (WORKERS * BS):
    parser.error('block/prefill must divide evenly across owner workers')
if BS < 4096 or BS % 4096 or CAPACITY % BS or not 1 <= QD <= 64 or SECONDS <= 0 or ROUNDS < 1:
    parser.error('aligned block size dividing 1 GiB, QD 1--64, positive seconds/rounds required')
if PREFILL_BYTES < BS or PREFILL_BYTES > CAPACITY or PREFILL_BYTES % BS:
    parser.error('prefill bytes must be a multiple of block size within 1 GiB')
LIBC = C.CDLL(None, use_errno=True)
LIBC.syscall.restype = C.c_long

class IOCB(C.Structure):
    _fields_ = [("data", C.c_uint64), ("key", C.c_uint32), ("rw_flags", C.c_uint32),
                ("opcode", C.c_uint16), ("priority", C.c_int16), ("fd", C.c_uint32),
                ("buffer", C.c_uint64), ("bytes", C.c_uint64), ("offset", C.c_int64),
                ("reserved", C.c_uint64), ("flags", C.c_uint32), ("resfd", C.c_uint32)]

class Event(C.Structure):
    _fields_ = [("data", C.c_uint64), ("obj", C.c_uint64),
                ("result", C.c_int64), ("result2", C.c_int64)]

class Timespec(C.Structure):
    _fields_ = [("sec", C.c_long), ("nsec", C.c_long)]

def call(number, *args):
    value = LIBC.syscall(C.c_long(number), *args)
    if value < 0:
        error = C.get_errno()
        raise OSError(error, os.strerror(error))
    return value

def emit(tag, obj):
    print(tag + " " + json.dumps(obj, sort_keys=True), flush=True)

def disk(owner):
    slot = ("09", "0a")[owner]
    blocks = list(Path("/sys/bus/pci/devices/0000:00:" + slot + ".0").glob("virtio*/block/*"))
    if len(blocks) != 1:
        raise RuntimeError("dedicated owner disk missing")
    block = blocks[0]
    if (block / "serial").read_text().strip() != "GVM-DATA-OWNER%d" % owner:
        raise RuntimeError("disk serial mismatch")
    if (block / "device/driver").resolve().name != "virtio_blk":
        raise RuntimeError("native virtio_blk required")
    fd = os.open("/dev/" + block.name, os.O_RDONLY | os.O_DIRECT)
    size = struct.unpack("Q", fcntl.ioctl(fd, 0x80081272, bytes(8)))[0]
    if size != CAPACITY:
        os.close(fd)
        raise RuntimeError("dedicated 1 GiB disk required")
    return fd

def worker(owner, index, ready, gate, output, full):
    # Bind before allocation so first-touch follows the selected CPU node.
    cpu = (owner if args.cpu_node is None else args.cpu_node) * 2 + index
    os.sched_setaffinity(0, {cpu})
    fd = disk(owner)
    region_start = index * REGION
    buffers = []
    exports = []
    ctx = C.c_uint64(0)
    try:
        call(206, C.c_uint(QD), C.byref(ctx))  # io_setup
        snapshots = {}
        pattern = bytes(BS)
        for unused in range(QD):
            b = mmap.mmap(-1, BS, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
            b[:] = pattern
            buffers.append(b)
            exports.append((C.c_char * BS).from_buffer(b))
        cbs = [IOCB(data=i, opcode=0, fd=fd, buffer=C.addressof(exports[i]), bytes=BS)
               for i in range(QD)]
        free = list(range(QD))
        events = (Event * QD)()
        timeout = Timespec(1, 0)
        for offset in (region_start, region_start + REGION - BS):
            if os.preadv(fd, [memoryview(buffers[0])], offset) != BS:
                raise RuntimeError("snapshot short read")
            snapshots[offset] = bytes(buffers[0])
        os.write(ready, b"R")
        os.read(gate, 1)
        start = time.monotonic()
        last_clock_check = 0
        running = True
        target_blocks = PREFILL_BYTES // (WORKERS * BS)
        region_start = index * REGION
        deadline = start + SECONDS
        sent = completed = inflight = 0
        checksum = 0
        peak = 0
        stalls = 0
        while True:
            if full:
                running = sent < target_blocks
            if running and free:
                wanted = min(len(free), target_blocks - sent) if full else len(free)
                chosen = free[:wanted]
                for n, i in enumerate(chosen):
                    cbs[i].offset = region_start + ((sent + n) % (REGION // BS)) * BS
                pointers = (C.POINTER(IOCB) * wanted)(*[C.pointer(cbs[i]) for i in chosen])
                accepted = call(209, ctx, C.c_long(wanted), pointers)  # io_submit
                if accepted:
                    del free[:accepted]
                    sent += accepted
                    inflight += accepted
                    peak = max(peak, inflight)
            if not inflight:
                if not running:
                    break
                raise RuntimeError("AIO submit made no progress")
            count = call(208, ctx, C.c_long(1), C.c_long(QD),
                         events, C.byref(timeout))  # io_getevents
            if not count:
                stalls += 1
                if stalls > 30:
                    raise RuntimeError("AIO completion stalled")
                continue
            stalls = 0
            for i in range(count):
                event = events[i]
                if event.result != BS or event.result2:
                    raise RuntimeError("AIO short/error completion: %d/%d" %
                                       (event.result, event.result2))
                if TOUCH_DATA:
                    checksum ^= sum(buffers[event.data][::4096])
                free.append(event.data)
                completed += 1
                inflight -= 1
            # Avoid a shared HPET read for every submit/completion batch.
            if not full and completed - last_clock_check >= 64:
                running = time.monotonic() < deadline
                last_clock_check = completed
        assert completed == sent and not inflight
        end = time.monotonic()
        flushed = end
        if not completed:
            raise RuntimeError("no completed reads")
        for offset, expected in snapshots.items():
            if os.preadv(fd, [memoryview(buffers[0])], offset) != BS or buffers[0][:] != expected:
                raise RuntimeError("read consistency verification failed")
        usage = os.times()
        result = dict(owner=owner, worker=index, cpu=cpu, bytes=completed * BS, elapsed=end-start,
                      flush_seconds=flushed-end, mib_s=completed*BS/(1024**2)/(end-start),
                      flushed_mib_s=completed*BS/(1024**2)/(flushed-start), start=start, end=end, flush_end=flushed, verified=True,
                      region_start=region_start, region_bytes=REGION,
                      queue_depth_peak=peak,
                      cpu_seconds=usage.user+usage.system, prefill=full, touch_pages=TOUCH_DATA, checksum=checksum)
        os.write(output, (json.dumps(result) + "\n").encode())
    finally:
        if ctx.value:
            call(207, ctx)  # io_destroy
        exports.clear()
        for b in buffers:
            b.close()
        os.close(fd)

def phase(name, owners, full=False):
    children = []
    emit("BENCH_PREPARE", dict(name=name, owners=owners, workers=len(owners)*WORKERS, bs=BS, per_worker_qd=QD))
    for owner, index in [(o, i) for o in owners for i in range(WORKERS)]:
        ready_r, ready_w = os.pipe()
        gate_r, gate_w = os.pipe()
        out_r, out_w = os.pipe()
        pid = os.fork()
        if pid == 0:
            try:
                for entry in children:
                    for fd in entry[1:]:
                        os.close(fd)
                os.close(ready_r); os.close(gate_w); os.close(out_r)
                worker(owner, index, ready_w, gate_r, out_w, full)
                os._exit(0)
            except BaseException:
                traceback.print_exc()
                try: os.write(out_w, b'{"error":"worker failed"}\n')
                except OSError: pass
                os._exit(1)
        os.close(ready_w); os.close(gate_r); os.close(out_w)
        children.append((pid, ready_r, gate_w, out_r))
    for pid, ready_r, gate_w, out_r in children:
        if os.read(ready_r, 1) != b"R":
            raise RuntimeError("worker did not initialize")
    emit("BENCH_BEGIN", dict(name=name, owners=owners))
    # Allow the host observer to take both endpoints' counter snapshots.
    time.sleep(1)
    for pid, ready_r, gate_w, out_r in children:
        os.write(gate_w, b"S")
    results = []
    for pid, ready_r, gate_w, out_r in children:
        with os.fdopen(out_r) as pipe:
            result = json.loads(pipe.readline())
        unused, status = os.waitpid(pid, 0)
        os.close(ready_r); os.close(gate_w)
        if status or "error" in result:
            raise RuntimeError("benchmark child failed")
        results.append(result)
    duration = max(r["end"] for r in results) - min(r["start"] for r in results)
    total = sum(r["bytes"] for r in results) / (1024**2) / duration
    flush_duration = max(r["flush_end"] for r in results) - min(r["start"] for r in results)
    flushed_total = sum(r["bytes"] for r in results) / (1024**2) / flush_duration
    obj = dict(name=name, owners=owners, results=results, total_mib_s=total,
               flushed_total_mib_s=flushed_total,
               common_window_seconds=duration, flushed_window_seconds=flush_duration)
    emit("BENCH_END", obj)
    return obj

if os.uname().machine != "x86_64" or C.sizeof(IOCB) != 64:
    raise RuntimeError("x86_64 Linux AIO ABI required")
owners = [args.owner] if args.owner is not None else [0,1]
emit("BENCH_CONFIG", dict(mode="read-only", block_size=BS, queue_depth=QD, seconds=SECONDS,
                         rounds=ROUNDS, owners=owners, workers_per_owner=WORKERS,
                         prefill_bytes=PREFILL_BYTES,
                         full_device_prefill=not args.skip_prefill and PREFILL_BYTES==CAPACITY))
if not args.skip_prefill:
    raise RuntimeError("read-only test requires --skip-prefill")
if args.prepare_only:
    raise SystemExit(0)
all_results = []
for round_no in range(1, ROUNDS + 1):
    cases = [("single%d"%args.owner, owners)] if args.owner is not None else [
        ("single0",[0]),("single1",[1]),("parallel",[0,1])]
    rotate = (round_no-1) % len(cases)
    cases = cases[rotate:] + cases[:rotate]
    for name, selected in cases:
        all_results.append(phase(name+"-r"+str(round_no), selected))
summary = {}
for name in sorted(set(r["name"].rsplit("-r",1)[0] for r in all_results)):
    values = [r["total_mib_s"] for r in all_results if r["name"].startswith(name+"-")]
    flushed_values = [r["flushed_total_mib_s"] for r in all_results if r["name"].startswith(name+"-")]
    summary[name] = dict(median_mib_s=statistics.median(values), samples_mib_s=values,
                         median_flushed_mib_s=statistics.median(flushed_values),
                         samples_flushed_mib_s=flushed_values)
if args.owner is None:
    summary["gain_vs_best_single"] = summary["parallel"]["median_mib_s"] / max(
        summary["single0"]["median_mib_s"],summary["single1"]["median_mib_s"])
    summary["ratio_to_separate_single_sum"] = summary["parallel"]["median_mib_s"] / (
        summary["single0"]["median_mib_s"] + summary["single1"]["median_mib_s"])
    summary["flushed_ratio_to_separate_single_sum"] = summary["parallel"]["median_flushed_mib_s"] / (
        summary["single0"]["median_flushed_mib_s"] + summary["single1"]["median_flushed_mib_s"])
emit("BENCH_SUMMARY",summary)


