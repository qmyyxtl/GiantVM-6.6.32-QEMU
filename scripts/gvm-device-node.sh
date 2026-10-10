#!/usr/bin/env bash
# One node of a four-vCPU distributed guest; no logs, pidfiles or overlays.
set -euo pipefail
# Validation failures must not leave core dump files.
ulimit -c 0
node=${1:?Usage: gvm-device-node.sh 0|1 [additional QEMU arguments]}
shift
case "$node" in 0|1) ;; *) echo 'node must be 0 or 1' >&2; exit 2 ;; esac
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
    -local-cpu "cpus=2,start=$((node * 2)),iplist=${GVM_IPLIST:-172.0.1.2 172.0.1.3}"
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
