#!/usr/bin/env python3
"""Run in this Garen/Ezreal guest; use runtime network settings and stdout only."""
import os, subprocess
from pathlib import Path

def require(ok, msg):
    if not ok:
        raise RuntimeError(msg)

def run(*args):
    p = subprocess.run(args, capture_output=True, text=True, timeout=12)
    require(p.returncode == 0, str(args) + "\n" + p.stdout + p.stderr)
    return p.stdout

devices = [
    ("0000:00:07.0", "192.168.100.241", "192.168.100.17"),
    ("0000:00:08.0", "192.168.100.242", "192.168.100.14"),
]
networks = []
for owner, (bdf, ip, peer) in enumerate(devices):
    entries = list((Path("/sys/bus/pci/devices") / bdf / "net").iterdir())
    require(len(entries) == 1, "one network interface per PCI device")
    iface = entries[0].name
    for prop, value in [("rp_filter", "0"), ("arp_ignore", "1"), ("arp_announce", "2")]:
        for conf in ("all", iface):
            p = Path("/proc/sys/net/ipv4/conf") / conf / prop
            p.write_text(value)
    run("ip", "link", "set", "dev", iface, "up")
    run("ip", "addr", "replace", ip + "/21", "dev", iface)
    networks.append((owner, iface, peer))
    print("NETWORK_READY owner=" + str(owner) + " bdf=" + bdf + " interface=" + iface, flush=True)

affinity = os.sched_getaffinity(0)
try:
    for cpu in (0, 2):
        os.sched_setaffinity(0, {cpu})
        for owner, iface, peer in networks:
            out = run("ping", "-n", "-I", iface, "-c", "10", "-i", "0.1", "-W", "1", "-M", "do", "-s", "1400", "-p", "deadbeef", peer)
            require("10 received" in out and "0% packet loss" in out, "packet loss\n" + out)
            print("PASS physical-network owner=" + str(owner) + " submit_cpu=" + str(cpu) + " peer=" + peer + " count=10 payload=1400\n" + "\n".join(out.splitlines()[-3:]), flush=True)
    for owner, iface, peer in networks:
        base = Path("/sys/class/net") / iface / "statistics"
        print("NETWORK_COUNTERS owner=" + str(owner) + " " + " ".join(k + "=" + (base / k).read_text().strip() for k in ("rx_packets", "tx_packets", "rx_errors", "tx_errors", "rx_dropped", "tx_dropped")), flush=True)
    print("PASS distributed physical network matrix", flush=True)
finally:
    os.sched_setaffinity(0, affinity)
