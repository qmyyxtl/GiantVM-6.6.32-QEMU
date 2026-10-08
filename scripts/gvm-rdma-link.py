#!/usr/bin/env python3
"""Bring up a GVM IPoIB link without logs or persistent network files."""
import argparse
import ipaddress
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("address", help="node IP address with prefix, e.g. 172.0.1.4/24")
parser.add_argument("--pci", default="0000:ac:00.0")
args = parser.parse_args()
address = ipaddress.IPv4Interface(args.address)
if os.geteuid() != 0:
    parser.error("run with sudo")
for module in ("ib_ipoib", "ib_uverbs", "rdma_ucm"):
    subprocess.run(["modprobe", module], check=True)
links = []
for path in Path("/sys/class/net").iterdir():
    if (path / "type").read_text().strip() == "32" and \
            (path / "device").resolve().name == args.pci:
        links.append(path.name)
if len(links) != 1:
    parser.error(f"expected one IPoIB interface for {args.pci}, found {links}")
name = links[0]
import json
info = json.loads(subprocess.check_output(
    ["ip", "-j", "-4", "address", "show", "dev", name], text=True))
existing = [(a["local"], a["prefixlen"])
            for link in info for a in link.get("addr_info", [])]
desired = (str(address.ip), address.network.prefixlen)
if existing and existing != [desired]:
    parser.error(f"{name} already has different addresses: {existing}")
for command in (
    ["ip", "link", "set", "dev", name, "mtu", "2044"],
    ["ip", "link", "set", "dev", name, "up"],
):
    subprocess.run(command, check=True)
if not existing:
    subprocess.run(["ip", "address", "add", str(address), "dev", name], check=True)
print(subprocess.check_output(["ip", "-br", "address", "show", "dev", name],
                              text=True).strip())
