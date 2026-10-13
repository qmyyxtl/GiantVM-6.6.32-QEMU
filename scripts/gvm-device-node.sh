#!/usr/bin/env bash
# One node of a four-vCPU distributed guest; no logs, pidfiles or overlays.
set -euo pipefail
# Validation failures must not leave core dump files.
ulimit -c 0
node=${1:?Usage: gvm-device-node.sh 0|1 [additional QEMU arguments]}
shift
method=${GVM_METHOD:-rdma}
if [[ ${1:-} == method=* ]]; then
    method=${1#method=}
    shift
fi
case "$method" in tcp|rdma) ;; *) echo 'method must be tcp or rdma' >&2; exit 2 ;; esac
case "$node" in 0|1) ;; *) echo 'node must be 0 or 1' >&2; exit 2 ;; esac

# Reject stale or sudo-filtered node addresses before creating a KVM VM.
read -r -a cluster_ips <<< "${GVM_IPLIST:-172.0.1.2 172.0.1.3}"
if [[ ${#cluster_ips[@]} != 2 ]]; then
    echo 'GVM_IPLIST must contain exactly two IPv4 addresses' >&2
    exit 2
fi
python3 - "$node" "${cluster_ips[@]}" <<'PY'
import ipaddress
import json
import subprocess
import sys

node = int(sys.argv[1])
addresses = sys.argv[2:]
try:
    if any(ipaddress.ip_address(a).version != 4 for a in addresses):
        raise ValueError("IPv4 addresses required")
    if addresses[0] == addresses[1]:
        raise ValueError("node addresses must be distinct")
    links = json.loads(subprocess.check_output(["ip", "-j", "-4", "addr", "show"]))
    local = {a["local"] for link in links for a in link.get("addr_info", [])
             if a.get("family") == "inet"}
    if addresses[node] not in local:
        raise ValueError(f"node {node} address {addresses[node]} is not assigned "
                         "on this host; pass GVM_IPLIST explicitly through sudo env")
except (ValueError, OSError, subprocess.CalledProcessError) as error:
    print(f"GVM startup address validation failed: {error}", file=sys.stderr)
    sys.exit(2)
PY
# The current DSM LAPIC forwarding path requires software APIC handling.
if [[ -r /sys/module/kvm_intel/parameters/enable_apicv ]] &&
   [[ $(cat /sys/module/kvm_intel/parameters/enable_apicv) != N ]]; then
    echo 'GVM requires kvm_intel enable_apicv=0; stop VMs before reloading the module' >&2
    exit 2
fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export LOOM_DEVICE_AGGREGATION=1
machine=${GVM_MACHINE:-pc}
case "$machine" in
    pc|q35) ;;
    *) echo 'GVM_MACHINE must be pc or q35' >&2; exit 2 ;;
esac
case ${GVM_IOMMU:-0} in
    0) ;;
    1)
        if [[ $machine != q35 ]]; then
            echo 'GVM_IOMMU=1 requires GVM_MACHINE=q35' >&2
            exit 2
        fi
        ;;
    *) echo 'GVM_IOMMU must be 0 or 1' >&2; exit 2 ;;
esac
case ${GVM_OWNER_IOMMU:-0} in
    0) ;;
    1)
        if [[ ${GVM_IOMMU:-0} != 1 ]]; then
            echo 'GVM_OWNER_IOMMU=1 requires GVM_IOMMU=1' >&2
            exit 2
        fi
        export LOOM_OWNER_IOMMU=1
        ;;
    *) echo 'GVM_OWNER_IOMMU must be 0 or 1' >&2; exit 2 ;;
esac
args=(
    -enable-kvm -machine "$machine,kernel-irqchip=split" -smp 4 -m 4G
    -display none -monitor none
    -qmp tcp:127.0.0.1:4444,server=on,wait=off
    -local-cpu "method=$method,cpus=2,start=$((node * 2)),iplist=${GVM_IPLIST:-172.0.1.2 172.0.1.3}"
    -drive "file=${GVM_ROOT_IMAGE:-/home/xtl/start/debian-12-40G.raw},format=raw,id=rootfs,if=none,file.locking=off"
    -device ide-hd,drive=rootfs,id=rootfs
    -device "edu,id=edu-owner-$node,addr=0$((5 + node)).0,loom-aggregate=on,loom-numa-node=$node"
)
# Owner mode gives each peer its own unit; ordinary mode uses the master unit.
if [[ ${GVM_IOMMU:-0} == 1 ]]; then
    args+=(-device intel-iommu,caching-mode=on,intremap=off)
fi
# Some host builds omit libslirp; the serial console needs no user network.
case ${GVM_USER_NET:-1} in
    1) args+=(-netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22
              -device virtio-net-pci,netdev=net0,addr=03.0) ;;
    0) args+=(-nic none) ;;
    *) echo 'GVM_USER_NET must be 0 or 1' >&2; exit 2 ;;
esac
if [[ $node == 0 ]]; then
    guest_cmdline='notsc nokaslr console=ttyS0 root=/dev/sda1 memmap=64K$0x08000000'
    if [[ ${GVM_IOMMU:-0} == 1 ]]; then
        guest_cmdline+=' intel_iommu=on iommu.strict=1'
    fi
    args+=(
        -serial tcp:127.0.0.1:5555,server=on,wait=off
        -kernel "${GVM_GUEST_KERNEL:-/home/xtl/start/bzImage}"
        -append "$guest_cmdline ${GVM_GUEST_EXTRA_ARGS:-}"
    )
else
    args+=(-serial null)
fi
if [[ ${GVM_QUIET:-0} == 1 ]]; then
    exec "$root/build/qemu-system-x86_64" "${args[@]}" "$@" >/dev/null
fi
exec "$root/build/qemu-system-x86_64" "${args[@]}" "$@"
