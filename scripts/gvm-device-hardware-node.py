#!/usr/bin/env python3
"""Attach real network/storage backends to a distributed guest, without files.

Run with sudo on each node. QEMU runs as the invoking user. The physical data
disk is opened O_RDONLY; the temporary network interface is removed on exit.
These are QEMU PCI models backed by physical hardware, not VFIO passthrough.
"""
import argparse
import fcntl
import os
from pathlib import Path
import pwd
import signal
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("node", type=int, choices=(0, 1))
parser.add_argument("--parent", required=True,
                    help="physical Ethernet interface or existing bridge")
parser.add_argument("--data-disk", help="physical block device, read-only")
parser.add_argument("--single-node", action="store_true",
                    help="control run: 2 local CPUs, read-only root, no DSM")
parser.add_argument("--guest-kernel",
                    help="existing guest bzImage for --single-node")
args = parser.parse_args()
if args.single_node and not args.guest_kernel:
    parser.error("--single-node requires --guest-kernel")

if os.geteuid() != 0 or not os.environ.get("SUDO_USER"):
    parser.error("run with sudo as the ordinary QEMU user")
user = pwd.getpwnam(os.environ["SUDO_USER"])
root = Path(__file__).resolve().parent.parent
iface = f"gvm-local{args.node}" if args.single_node else f"gvm-net{args.node}"
fds = []
created = False
child = None
bridge_parent = Path("/sys/class/net", args.parent, "bridge").is_dir()

def run(*command):
    subprocess.run(command, check=True)

def drop_privileges():
    os.initgroups(user.pw_name, user.pw_gid)
    os.setgid(user.pw_gid)
    os.setuid(user.pw_uid)

def stop_child(signum, frame):
    if child is not None:
        child.send_signal(signum)

try:
    if Path("/sys/class/net", iface).exists():
        parser.error(f"{iface} already exists; refusing to change it")
    if not bridge_parent:
        run("ip", "link", "add", "link", args.parent, "name", iface,
            "type", "macvtap", "mode", "bridge")
    else:
        run("ip", "tuntap", "add", "dev", iface, "mode", "tap",
            "user", user.pw_name)
    created = True
    mac = f"52:54:00:da:00:{16 + args.node:02x}"
    # A bridge TAP needs a different host MAC; a local bridge FDB entry
    # must not consume unicast replies destined for the guest.
    host_mac = mac if not bridge_parent else f"52:54:00:da:10:{16 + args.node:02x}"
    run("ip", "link", "set", "dev", iface, "address", host_mac)
    if bridge_parent:
        run("ip", "link", "set", "dev", iface, "master", args.parent)
    run("ip", "link", "set", "dev", iface, "up")
    if not bridge_parent:
        index = Path("/sys/class/net", iface, "ifindex").read_text().strip()
        netfd = os.open(f"/dev/tap{index}", os.O_RDWR)
    else:
        netfd = os.open("/dev/net/tun", os.O_RDWR)
    fds.append(netfd)
    # IFF_TAP | IFF_NO_PI; no vhost or kernel virtqueue bypass.
    fcntl.ioctl(netfd, 0x400454ca,
                struct.pack("16sH", iface.encode(), 0x1002) + bytes(22))
    extra = [
        "-netdev", f"tap,id=physical-net,fd={netfd},vhost=off",
        "-device", f"e1000,id=physical-net-owner-{args.node},"
                   f"netdev=physical-net,mac={mac},addr=0{7 + args.node}.0,"
                   f"loom-aggregate=on,loom-numa-node={args.node}",
    ]
    if args.data_disk:
        disk = Path(args.data_disk)
        if not disk.is_block_device():
            parser.error("--data-disk must be a physical block device")
        diskfd = os.open(disk, os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECT)
        fds.append(diskfd)
        extra += [
            "-add-fd", f"fd={diskfd},set=1,opaque=physical-data-readonly",
            "-drive", "file=/dev/fdset/1,format=raw,id=physical-data,if=none,"
                      "readonly=on,file.driver=host_device,file.locking=off,"
                      "cache=none",
            "-device", f"virtio-blk-pci,drive=physical-data,id=physical-data,"
                       f"addr=09.0,ioeventfd=off,vectors=0,"
                       f"loom-aggregate=on,loom-numa-node={args.node}",
        ]
    env = dict(os.environ, GVM_QUIET="1")
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop_child)
    command = [str(root / "scripts/gvm-device-node.sh"), str(args.node)]
    if args.single_node:
        # The control guest uses existing images read-only and an init shell.
        # Ports differ from the distributed guest; no router is started.
        env.pop("LOOM_DEVICE_AGGREGATION", None)
        image = env.get("GVM_ROOT_IMAGE", "/home/xtl/start/debian-12-40G.raw")
        command = [
            str(root / "build/qemu-system-x86_64"),
            "-enable-kvm", "-machine", "kernel-irqchip=split",
            "-smp", "2", "-m", "2G", "-display", "none", "-monitor", "none",
            "-qmp", "tcp:127.0.0.1:4445,server=on,wait=off",
            "-serial", "tcp:127.0.0.1:5556,server=on,wait=off",
            "-kernel", args.guest_kernel,
            "-append", "notsc nokaslr console=ttyS0 root=/dev/vda1 ro init=/bin/bash",
            "-drive", f"file={image},format=raw,id=rootfs,if=none,"
                      "readonly=on,file.locking=off",
            "-device", "virtio-blk-pci,drive=rootfs,id=rootfs,addr=04.0",
        ]
    child = subprocess.Popen(
        [*command, *extra], env=env, stdout=subprocess.DEVNULL,
        pass_fds=fds, preexec_fn=drop_privileges,
    )
    raise SystemExit(child.wait())
finally:
    for fd in fds:
        os.close(fd)
    if created:
        subprocess.run(["ip", "link", "delete", "dev", iface], check=False)
