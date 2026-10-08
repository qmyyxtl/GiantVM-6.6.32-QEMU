#!/usr/bin/env python3
"""Run one RDMA device owner with a VF; preserve the host PF and restore binding."""
import argparse
import os
from pathlib import Path
import resource
import signal
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("node", type=int, choices=(0, 1))
parser.add_argument("--host", required=True, help="VF PCI address, e.g. 0000:0c:02.0")
args = parser.parse_args()
if os.geteuid() != 0:
    parser.error("run with sudo; VFIO group access is required")
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parent.parent
pci = Path("/sys/bus/pci/devices") / args.host
if not (pci / "physfn").exists():
    parser.error("only an SR-IOV VF is accepted; refusing to detach a host PF")
parent = (pci / "physfn").resolve()
if (parent / "driver").resolve().name != "i40e":
    parser.error("the host PF must remain bound to i40e")
members = list((pci / "iommu_group/devices").iterdir())
if len(members) != 1 or members[0].name != args.host:
    parser.error("VF must have an independent IOMMU group")
original = (pci / "driver").resolve().name if (pci / "driver").exists() else None
if original not in (None, "iavf"):
    parser.error("VF is already owned by another driver")
child = None
bound = False

def forward(signum, frame):
    if child is not None:
        child.send_signal(signum)

for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(sig, forward)
try:
    subprocess.run(["modprobe", "vfio-pci"], check=True)
    (pci / "driver_override").write_text("vfio-pci\n")
    if original:
        (pci / "driver/unbind").write_text(args.host)
    bound = True
    Path("/sys/bus/pci/drivers_probe").write_text(args.host)
    if (pci / "driver").resolve().name != "vfio-pci":
        raise RuntimeError("VFIO binding failed")
    env = dict(os.environ, GVM_USER_NET="0", GVM_QUIET="1")
    env.setdefault("GVM_IPLIST", "172.0.1.4 172.0.1.2")
    env.setdefault("GVM_GUEST_KERNEL", "/home/xtl/start/bzImage-6.12-vfio-bss2m")
    env.setdefault("GVM_GUEST_EXTRA_ARGS", "clocksource=hpet")
    if args.node == 0 and not Path(env["GVM_GUEST_KERNEL"]).is_file():
        raise RuntimeError("guest kernel does not exist")
    endpoint = (f"vfio-pci,host={args.host},id=vfio-vf-owner-{args.node},"
                f"addr=0{7 + args.node}.0,rombar=0,x-no-mmap=on,"
                "x-no-kvm-msi=on,x-no-kvm-msix=on,"
                f"loom-aggregate=on,loom-numa-node={args.node}")
    print(f"VF {args.host}; host PF {parent.name} remains on i40e", flush=True)
    child = subprocess.Popen([str(root / "scripts/gvm-device-node.sh"),
                              str(args.node), "-device", endpoint], env=env)
    print(f"QEMU PID {child.pid}", flush=True)
    raise SystemExit(child.wait())
finally:
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=20)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
    if bound:
        if (pci / "driver").exists():
            (pci / "driver/unbind").write_text(args.host)
        (pci / "driver_override").write_text((original or "") + "\n")
        if original:
            Path("/sys/bus/pci/drivers_probe").write_text(args.host)
        (pci / "driver_override").write_text("\n")
        print(f"Restored VF {args.host} to {original or 'unbound'}", flush=True)
