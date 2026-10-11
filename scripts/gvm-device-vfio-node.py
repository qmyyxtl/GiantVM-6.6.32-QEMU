#!/usr/bin/env python3
"""Run one RDMA device owner with a VF; preserve the host PF and restore binding."""
import argparse
import os
from pathlib import Path
import resource
import signal
import subprocess

def iommu_environment(physical, legacy_bridge):
    if physical not in ("0", "1"):
        raise ValueError("LOOM_VFIO_PHYSICAL_DMA must be 0 or 1")
    if physical == "1" and legacy_bridge:
        raise ValueError("physical DMA cannot use the guest-assisted DMA bridge")
    if legacy_bridge:
        return {}
    enabled = "0" if physical == "1" else "1"
    return dict(GVM_MACHINE="q35", GVM_IOMMU=enabled,
                GVM_OWNER_IOMMU=enabled, LOOM_OWNER_IOMMU=enabled)


parser = argparse.ArgumentParser()
parser.add_argument("node", type=int, choices=(0, 1))
parser.add_argument("--host", required=True, help="VF PCI address, e.g. 0000:0c:02.0")
parser.add_argument("--legacy-dma-bridge", action="store_true",
                    help="use the old guest-assisted identity DMA bridge")
parser.add_argument("--data-disk", type=Path, help="existing dedicated raw data image")
parser.add_argument("--device-numa-roots", action="store_true",
                    help="expose owner-specific PCI roots with ACPI NUMA proximity")
args = parser.parse_args()
physical = os.environ.get("LOOM_VFIO_PHYSICAL_DMA", "0")
try:
    startup_iommu = iommu_environment(physical, args.legacy_dma_bridge)
except ValueError as exc:
    parser.error(str(exc))

if args.device_numa_roots and (physical != "1" or args.legacy_dma_bridge):
    parser.error("--device-numa-roots requires physical DMA without the legacy bridge")

if args.data_disk is not None:
    args.data_disk = args.data_disk.resolve()
    if not args.data_disk.is_file() or args.data_disk.stat().st_size != 1024 ** 3:
        parser.error("--data-disk must be an existing dedicated 1 GiB raw image")
if os.geteuid() != 0:
    parser.error("run with sudo; VFIO group access is required")
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parent.parent
pci = Path("/sys/bus/pci/devices") / args.host
if not (pci / "physfn").exists():
    parser.error("only an SR-IOV VF is accepted; refusing to detach a host PF")
if physical == "1" and ((pci / "vendor").read_text().strip() != "0x8086" or
                        (pci / "device").read_text().strip() != "0x154c"):
    parser.error("physical DMA supports only Intel X710 VF (8086:154c)")
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
    env = dict(os.environ, GVM_USER_NET="0", GVM_QUIET="1",
               LOOM_VFIO_DSM_DMA="1")
    env.setdefault("GVM_IPLIST", "172.0.1.4 172.0.1.2")
    env.setdefault("GVM_GUEST_KERNEL", "/home/xtl/start/bzImage-6.12-vfio-bss2m")
    env.setdefault("GVM_GUEST_EXTRA_ARGS", "clocksource=hpet")
    if args.node == 0 and not Path(env["GVM_GUEST_KERNEL"]).is_file():
        raise RuntimeError("guest kernel does not exist")
    topology = []
    endpoint_bus = ""
    endpoint_addr = f"0{7 + args.node}.0"
    if args.device_numa_roots:
        for node in range(2):
            topology += ["-device",
                         f"pxb-pcie,id=device-root-{node},bus=pcie.0,"
                         f"addr={0x0b + node:02x}.0,bus_nr={32 + node * 32},"
                         f"numa_node={node}",
                         "-device", f"pcie-root-port,id=device-port-{node},"
                         f"bus=device-root-{node},addr=00.0,chassis={node + 1},"
                         f"slot={node + 1}"]
        endpoint_bus = f"bus=device-port-{args.node},"
        endpoint_addr = "00.0"
    endpoint = (f"vfio-pci,host={args.host},id=vfio-vf-owner-{args.node},"
                f"{endpoint_bus}addr={endpoint_addr},rombar=0,x-no-mmap=on,"
                "x-no-kvm-msi=on,x-no-kvm-msix=on,"
                f"loom-aggregate=on,loom-numa-node={args.node}")
    bridge = (f"loom-dma-bridge,id=dma-owner-{args.node},"
              f"addr=0{3 + args.node}.0,target-devfn={8 * (7 + args.node)},"
              f"loom-aggregate=on,loom-numa-node={args.node}")
    extra = []
    if args.legacy_dma_bridge:
        extra += ["-device", bridge]
    else:
        env.update(startup_iommu)
        extra += ["-object", "memory-backend-ram,id=n0,size=2G",
                  "-object", "memory-backend-ram,id=n1,size=2G",
                  "-numa", "node,nodeid=0,cpus=0-1,memdev=n0",
                  "-numa", "node,nodeid=1,cpus=2-3,memdev=n1",
                  "-numa", "dist,src=0,dst=1,val=20",
                  "-numa", "dist,src=1,dst=0,val=20"]
    if args.data_disk is not None:
        extra += ["-object", "iothread,id=owner-io,poll-max-ns=0",
                  "-drive", f"file={args.data_disk},format=raw,if=none,"
                 f"id=data-owner-{args.node},cache=none,aio=threads",
                 "-device", f"virtio-blk-pci,drive=data-owner-{args.node},"
                 f"id=data-owner-{args.node},"
                 + ("bus=pcie.0," if args.device_numa_roots else "") +
                 f"addr=0{9 + args.node:x}.0,"
                 f"serial=GVM-DATA-OWNER{args.node},ioeventfd=on,vectors=5,"
                 f"loom-aggregate=on,loom-numa-node={args.node},"
                 "iothread=owner-io"]
    print(f"VF {args.host}; host PF {parent.name} remains on i40e", flush=True)
    child = subprocess.Popen([str(root / "scripts/gvm-device-node.sh"),
                              str(args.node), *topology, "-device", endpoint,
                              *extra], env=env)
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
