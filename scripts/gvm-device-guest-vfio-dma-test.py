#!/usr/bin/env python3
"""Native VFIO DSM functional check. Run through the guest serial console in memory."""
import os, mmap, struct, subprocess
from pathlib import Path

def require(ok, message):
    if not ok:
        raise RuntimeError(message)

for slot, name in (("07", "enp0s7"), ("08", "enp0s8")):
    dev = Path("/sys/bus/pci/devices/0000:00:" + slot + ".0")
    require((dev / "vendor").read_text().strip() == "0x8086", "Intel VF required")
    require((dev / "device").read_text().strip() == "0x154c", "X710 VF required")
    require((dev / "driver").resolve().name == "iavf", "native iavf driver required")
    require((dev / "net" / name).is_dir(), "expected per-owner interface")
for slot in ("03", "04"):
    dev = Path("/sys/bus/pci/devices/0000:00:" + slot + ".0")
    require((dev / "driver").resolve().name == "loom-dsm-dma", "DMA bridge missing")
    fd = os.open(str(dev / "resource0"), os.O_RDONLY)
    bar = mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ)
    require(struct.unpack_from("<I", bar, 0)[0] == 0x4c444d41, "bridge protocol magic")
    require(struct.unpack_from("<i", bar, 8)[0] == 0, "DMA bridge reported an error")
    require(struct.unpack_from("<I", bar, 0x30)[0] > 0, "DMA buffers not registered")
    bar.close()
    os.close(fd)

# Runtime settings only; these are the same independent interfaces as the
# network matrix test. No persistent guest files or network configuration.
for iface, ip in (("enp0s7", "192.168.100.241"), ("enp0s8", "192.168.100.242")):
    for prop, value in (("rp_filter", "0"), ("arp_ignore", "1"), ("arp_announce", "2")):
        for conf in ("all", iface):
            (Path("/proc/sys/net/ipv4/conf") / conf / prop).write_text(value)
    subprocess.run(["ip", "link", "set", "dev", iface, "up"], check=True, timeout=12)
    subprocess.run(["ip", "addr", "replace", ip + "/21", "dev", iface], check=True, timeout=12)

import os,socket,struct,time,hashlib,subprocess
from pathlib import Path
def checksum(data):
 if len(data)%2:data+=b'\0'
 v=sum(struct.unpack("!%dH"%(len(data)//2),data))
 v=(v&65535)+(v>>16);v=(v&65535)+(v>>16)
 return (~v)&65535
aff=os.sched_getaffinity(0)
try:
 for cpu in (0,1,2,3):
  os.sched_setaffinity(0,{cpu})
  for owner,iface,peer in [(0,"enp0s7","192.168.100.17"),(1,"enp0s8","192.168.100.14")]:
   sock=socket.socket(socket.AF_INET,socket.SOCK_RAW,socket.IPPROTO_ICMP)
   sock.setsockopt(socket.SOL_SOCKET,socket.SO_BINDTODEVICE,iface.encode()+b"\0")
   ident=(os.getpid()+cpu*97+owner*701)&65535
   got=0
   for seq in range(1,11):
    payload=(hashlib.sha256(struct.pack("!HHH",ident,seq,cpu)+os.urandom(16)).digest()*44)[:1400]
    head=struct.pack("!BBHHH",8,0,0,ident,seq)
    pkt=struct.pack("!BBHHH",8,0,checksum(head+payload),ident,seq)+payload
    sock.sendto(pkt,(peer,0));end=time.monotonic()+2
    while time.monotonic()<end:
     sock.settimeout(max(.01,end-time.monotonic()))
     try:data,addr=sock.recvfrom(65535)
     except socket.timeout:break
     hlen=(data[0]&15)*4;icmp=data[hlen:]
     if len(icmp)<8:continue
     typ,code,cs,rid,rseq=struct.unpack("!BBHHH",icmp[:8])
     if typ==0 and addr[0]==peer and rid==ident and rseq==seq:
      if icmp[8:]!=payload or checksum(icmp)!=0:
       raise RuntimeError("CORRUPT CPU=%d owner=%d seq=%d"%(cpu,owner,seq))
      got+=1;break
    else:pass
    if got!=seq:raise RuntimeError("LOST CPU=%d owner=%d seq=%d"%(cpu,owner,seq))
   sock.close()
   print("PAYLOAD_PASS cpu=%d owner=%d interface=%s peer=%s packets=%d bytes=1400"%(cpu,owner,iface,peer,got),flush=True)
finally:os.sched_setaffinity(0,aff)
time.sleep(3)
for iface in ("enp0s7","enp0s8"):
 p=Path("/sys/class/net")/iface
 print("VF_STATE",iface,"carrier", (p/"carrier").read_text().strip(),"speed",(p/"speed").read_text().strip(),flush=True)
 print("VF_COUNTERS",iface," ".join(k+"="+(p/"statistics"/k).read_text().strip() for k in ("rx_packets","tx_packets","rx_errors","tx_errors","rx_dropped","tx_dropped")),flush=True)
print("PASS native VFIO payload matrix 80/80",flush=True)

import os,socket,struct,time,hashlib
def checksum(data):
 if len(data)%2:data+=b'\0'
 v=sum(struct.unpack("!%dH"%(len(data)//2),data))
 v=(v&65535)+(v>>16);v=(v&65535)+(v>>16)
 return (~v)&65535
aff=os.sched_getaffinity(0)
try:
 for owner,cpu,iface,peer in [(0,2,"enp0s7","192.168.100.17"),(1,0,"enp0s8","192.168.100.14")]:
  os.sched_setaffinity(0,{cpu});sock=socket.socket(socket.AF_INET,socket.SOCK_RAW,socket.IPPROTO_ICMP)
  sock.setsockopt(socket.SOL_SOCKET,socket.SO_BINDTODEVICE,iface.encode()+b"\0")
  ident=(os.getpid()+owner*179)&65535;received=0
  for start in range(1,641,8):
   pending={}
   for seq in range(start,start+8):
    payload=(hashlib.sha256(struct.pack("!HH",ident,seq)).digest()*44)[:1400]
    pending[seq]=payload
    head=struct.pack("!BBHHH",8,0,0,ident,seq)
    sock.sendto(struct.pack("!BBHHH",8,0,checksum(head+payload),ident,seq)+payload,(peer,0))
   end=time.monotonic()+3
   while pending and time.monotonic()<end:
    sock.settimeout(max(.01,end-time.monotonic()))
    try:data,addr=sock.recvfrom(65535)
    except socket.timeout:break
    icmp=data[(data[0]&15)*4:]
    if len(icmp)<8:continue
    typ,code,cs,rid,rseq=struct.unpack("!BBHHH",icmp[:8])
    if typ==0 and addr[0]==peer and rid==ident and rseq in pending:
     if checksum(icmp)!=0 or icmp[8:]!=pending[rseq]:raise RuntimeError("BURST CORRUPTION")
     del pending[rseq];received+=1
   if pending:raise RuntimeError("BURST LOST owner=%d seq=%s"%(owner,sorted(pending)))
  sock.close()
  print("DMA_RING_REUSE_PASS owner=%d remote_submit_cpu=%d received=%d payload=1400 burst=8"%(owner,cpu,received),flush=True)
finally:os.sched_setaffinity(0,aff)
print("PASS native VFIO ring reuse 1280/1280",flush=True)

print("PASS native VFIO DSM DMA functional validation", flush=True)
