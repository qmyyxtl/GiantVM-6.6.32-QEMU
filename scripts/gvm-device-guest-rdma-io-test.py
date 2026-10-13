#!/usr/bin/env python3
"""Destructive end-to-end RDMA/DSM I/O regression on dedicated 1 GiB GVM disks.
No test files are created. Requires both owner serials and native virtio_blk.
"""
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--run-io',action='store_true',help='authorize overwriting the two dedicated GVM data disks')
if not parser.parse_args().run_io:
 parser.error('--run-io is required; the dedicated data disks will be overwritten')
import os, mmap, ctypes as C, fcntl, struct, json, traceback, hashlib, time
from pathlib import Path
CAPACITY=1024**3
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
    fd = os.open("/dev/" + block.name, os.O_RDWR | os.O_DIRECT)
    size = struct.unpack("Q", fcntl.ioctl(fd, 0x80081272, bytes(8)))[0]
    if size != CAPACITY:
        os.close(fd)
        raise RuntimeError("dedicated 1 GiB disk required")
    return fd

children=[]
started=time.monotonic()
for owner in [0,1]:
 for node in [0,1]:
  r,w=os.pipe()
  pid=os.fork()
  if pid:
   os.close(w); children.append((pid,r)); continue
  os.close(r)
  try:
   os.sched_setaffinity(0,{node*2})
   fd=disk(owner)
   sizes=[512,4096,8192,65536,1048576,2097152]
   transferred=0
   for round_no in range(3):
    for index,size in enumerate(sizes):
     offset=node*(CAPACITY//2)+(round_no*len(sizes)+index)*4*1024**2
     with mmap.mmap(-1,size) as send, mmap.mmap(-1,size) as receive:
      pattern=os.urandom(size)
      send[:]=pattern
      receive[:]=bytes(size)
      assert os.pwritev(fd,[send],offset)==size
      send[:]=bytes([0x5a])*size
      os.fdatasync(fd)
      assert os.preadv(fd,[receive],offset)==size
      assert receive[:]==pattern,('same CPU mismatch',owner,node,offset,size)
      os.sched_setaffinity(0,{(1-node)*2})
      receive[:]=bytes(size)
      assert os.preadv(fd,[receive],offset)==size
      assert receive[:]==pattern,('migrated CPU mismatch',owner,node,offset,size)
      os.sched_setaffinity(0,{node*2})
      transferred+=size*3
   os.close(fd)
   result=dict(owner=owner,node=node,passed=True,bytes=transferred,cases=18)
   os.write(w,(json.dumps(result)+'\n').encode())
   os._exit(0)
  except BaseException:
   os.write(w,(json.dumps(dict(owner=owner,node=node,error=traceback.format_exc()))+'\n').encode())
   os._exit(1)
passed=True
for pid,r in children:
 with os.fdopen(r) as pipe: result=json.loads(pipe.readline())
 status=os.waitpid(pid,0)[1]
 passed &= not status and result.get('passed',False)
 print('POOL_CORRECTNESS',json.dumps(result),flush=True)
print('POOL_CORRECTNESS_SUMMARY',json.dumps(dict(passed=passed,seconds=time.monotonic()-started)),flush=True)
assert passed, 'concurrent I/O correctness failed'

import os,mmap,json,traceback,time
from pathlib import Path
try:
 cases=0
 for owner in [0,1]:
  slot=("09","0a")[owner]
  b=list(Path("/sys/bus/pci/devices/0000:00:"+slot+".0").glob("virtio*/block/*"))[0]
  assert (b/"serial").read_text().strip()=="GVM-DATA-OWNER%d"%owner
  fd=os.open("/dev/"+b.name,os.O_RDWR|os.O_DIRECT)
  for node in [0,1]:
   os.sched_setaffinity(0,{node*2})
   for size in [4096,65536,1048576]:
    with mmap.mmap(-1,size+8192) as src,mmap.mmap(-1,size+8192) as dst:
     pattern=os.urandom(size)
     src[:]=b"\xa5"*(size+8192)
     dst[:]=b"\x5a"*(size+8192)
     src[512:512+size]=pattern
     sm=memoryview(src)[512:512+size];dm=memoryview(dst)[512:512+size]
     offset=128*1024**2+node*512*1024**2
     assert os.pwritev(fd,[sm],offset)==size
     os.fdatasync(fd)
     assert os.preadv(fd,[dm],offset)==size
     assert bytes(dm)==pattern
     assert dst[:512]==b"\x5a"*512 and dst[512+size:]==b"\x5a"*7680
     os.sched_setaffinity(0,{(1-node)*2})
     assert bytes(dm)==pattern
     assert dst[:512]==b"\x5a"*512 and dst[512+size:]==b"\x5a"*7680
     os.sched_setaffinity(0,{node*2})
     sm.release();dm.release()
     cases+=1
  os.close(fd)
 print("DIRECT_GUARDS",json.dumps(dict(passed=True,cases=cases)),flush=True)
except BaseException:
 traceback.print_exc()
 raise

import os, mmap, ctypes as C, fcntl, struct, json, traceback, hashlib, time
from pathlib import Path
CAPACITY=1024**3
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
    fd = os.open("/dev/" + block.name, os.O_RDWR | os.O_DIRECT)
    size = struct.unpack("Q", fcntl.ioctl(fd, 0x80081272, bytes(8)))[0]
    if size != CAPACITY:
        os.close(fd)
        raise RuntimeError("dedicated 1 GiB disk required")
    return fd


try:
 cases=0
 for owner in [0,1]:
  home=1-owner
  os.sched_setaffinity(0,{home*2})
  fd=disk(owner)
  size=8192
  with mmap.mmap(-1,size+8192) as buf, mmap.mmap(-1,size) as src, mmap.mmap(-1,4096) as ctl:
   buf[:]=b"\x5a"*(size+8192)
   src[:]=os.urandom(size);pattern=src[:]
   offset=256*1024**2
   assert os.pwritev(fd,[src],offset)==size
   os.fdatasync(fd)
   ctl[:]=bytes(4096)
   pid=os.fork()
   if pid==0:
    os.sched_setaffinity(0,{owner*2})
    ctl[1]=1
    counter=0
    while not ctl[0]:
     counter+=1
     struct.pack_into("Q",buf,0,counter)
     time.sleep(.0005)
    os._exit(0)
   view=memoryview(buf)[512:512+size]
   try:
    while not ctl[1]: time.sleep(.001)
    for i in range(32):
     assert os.preadv(fd,[view],offset)==size
     assert bytes(view)==pattern,("race data mismatch",owner,i)
     assert buf[8:512]==b"\x5a"*504
     assert buf[512+size:]==b"\x5a"*7680
     cases+=1
   finally:
    ctl[0]=1
    status=os.waitpid(pid,0)[1]
    view.release()
   assert status==0 and struct.unpack_from("Q",buf,0)[0]>0
  os.close(fd)
 print("DIRECT_DSM_RACE",json.dumps(dict(passed=True,cases=cases)),flush=True)
except BaseException:
 traceback.print_exc()
 raise

print('RDMA_IO_TEST_PASS',flush=True)
