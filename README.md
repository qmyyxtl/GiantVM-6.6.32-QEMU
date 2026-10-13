# GiantVM QEMU

GiantVM runs one guest operating system across two physical hosts. Each host runs
a QEMU process that supplies its local CPUs and owns its local devices.
The guest sees one set of CPUs and independent devices from both hosts; two
disks remain two disks rather than becoming a single logical volume.

This repository is based on **QEMU 9.0.90** and works with the matching
[GiantVM KVM](https://github.com/qmyyxtl/GiantVM-6.6.32-KVM/tree/device-aggregation-runtime-transport-20261013).
KVM provides distributed shared memory (DSM). QEMU manages the guest and
forwards device operations to the appropriate owner.

One build supports TCP and RDMA for KVM DSM, QEMU control messages, and private
disk I/O transfers. Choose `method=tcp` or `method=rdma` at startup on both hosts.

## Requirements

- Two Intel x86-64 hosts running the matching GiantVM KVM modules.
- Reachable IPv4 addresses on both hosts; an operational RDMA fabric for RDMA mode.
- A guest kernel that can boot the root image without an initramfs, with serial
  console, root disk/filesystem, and virtio-blk support.
- An identical initial raw root image on both hosts and a separate raw data image
  for each device owner.

The example below uses an existing Debian root image with its root filesystem
on `/dev/sda1`. The launch script passes this root device and does not attach
an initramfs. Adapt `scripts/gvm-device-node.sh` if your image needs a different
root device or an initramfs.

## Build

Install build dependencies on both hosts. For Debian/Ubuntu:

```bash
sudo apt-get install build-essential git python3 python3-venv ninja-build \
  pkg-config libglib2.0-dev libpixman-1-dev libibverbs-dev librdmacm-dev \
  numactl iproute2 psmisc
```

Build the same branch on each host:

```bash
git clone --branch device-aggregation-runtime-transport-20261013 \
  https://github.com/qmyyxtl/GiantVM-6.6.32-QEMU.git
cd GiantVM-6.6.32-QEMU
mkdir -p build
cd build
../configure --target-list=x86_64-softmmu --enable-kvm --enable-rdma \
  --disable-werror
ninja -j"$(nproc)" qemu-system-x86_64
cd ..
```

Enable RDMA at build time to include both transports, even if the first run
uses TCP. The runtime method selects which transport is used.

Build and load the host modules following the
[KVM README](https://github.com/qmyyxtl/GiantVM-6.6.32-KVM/tree/device-aggregation-runtime-transport-20261013#build).
The launch scripts require `kvm_intel enable_apicv=0`.

## Run a two-node guest

Run the following commands from this repository's root directory on each host.
Replace the example addresses and image paths with your own.

The disk launcher starts a 4-vCPU, 4-GiB guest with two NUMA nodes, two local
vCPUs per host, and one independent virtio-blk data disk per host.
It uses MSI-X, an owner-local IOThread, `cache=none,aio=threads`, HPET,
and `-nic none`. Host transport connections do not require a guest network card.

Prepare these files before starting:

| File | Location |
| --- | --- |
| Guest `bzImage` | Master only |
| Raw root image | Both hosts, identical initial contents |
| Owner 0 raw data image | Master |
| Owner 1 raw data image | Slave |

The launcher opens existing files; it does not download, create, or format images.

### TCP

Use the same ordered `GVM_IPLIST` on both hosts: master first, slave second.
Start both commands promptly in separate terminals.

Master / node 0 (example: Garen, `10.0.16.104`):

```bash
sudo env \
  GVM_IPLIST="10.0.16.104 10.0.16.91" \
  GVM_ROOT_IMAGE=/home/xtl/start/debian-12-40G.raw \
  GVM_DATA_IMAGE=/home/xtl/start/gvm-device-data-node0.raw \
  GVM_GUEST_KERNEL=/home/xtl/GVM-guest-kernel/arch/x86/boot/bzImage \
  GVM_GUEST_EXTRA_ARGS="clocksource=hpet isolcpus=domain,1-3" \
  GVM_QUIET=1 LOOM_RDMA_RECV_CQ_EVENTS=1 \
  numactl --cpunodebind=0 --membind=0 \
  ./scripts/gvm-device-disk-node.sh 0 method=tcp
```

Slave / node 1 (example: Ezreal, `10.0.16.91`):

```bash
sudo env \
  GVM_IPLIST="10.0.16.104 10.0.16.91" \
  GVM_ROOT_IMAGE=/home/xtl/start/debian-12-40G.raw \
  GVM_DATA_IMAGE=/home/xtl/start/gvm-device-data-node1.raw \
  GVM_QUIET=1 LOOM_RDMA_RECV_CQ_EVENTS=1 \
  numactl --cpunodebind=0 --membind=0 \
  ./scripts/gvm-device-disk-node.sh 1 method=tcp
```

Only the master supplies the guest kernel and command line.
`isolcpus=domain,1-3` keeps all four guest CPUs online while excluding CPUs 1–3
from automatic scheduler-domain load balancing. Keep this setting for the
current TCP startup configuration. TCP boot can take several minutes.

### RDMA

Use the same commands with `method=rdma` on both hosts and replace
`GVM_IPLIST` with their RDMA-network addresses, for example:

```text
GVM_IPLIST="172.0.1.4 172.0.1.2"
```

For InfiniBand, ensure both ports are Active, a subnet manager is running, and
IPoIB addresses are configured before starting. Both nodes must use the same
transport; changing it requires restarting the guest, not rebuilding the code.

### Console and transport check

Connect to the master serial console from the master host, for example:

```bash
socat -,raw,echo=0 TCP:127.0.0.1:5555
```

Install `socat` if needed. Log in using the credentials belonging to your image.
Keep only one serial client connected.

QMP listens on `127.0.0.1:4444` on each host. Connect with
`socat - TCP:127.0.0.1:4444`, then send these commands one at a time:

```json
{"execute":"qmp_capabilities"}
{"execute":"qom-set","arguments":{"path":"/machine","property":"loom-bar-miss-index","value":true}}
{"execute":"qom-get","arguments":{"path":"/machine","property":"gvm-method"}}
```

The final response should report the selected `tcp` or `rdma` method.
In the guest, `lsblk` shows the independent data disks. Shut the guest down
with `poweroff` and wait for both QEMU processes to exit before reloading KVM.

The current two-node scripts use these host transport ports:

| Path | Master | Slave |
| --- | --- | --- |
| KVM DSM | 37710 | 37711 |
| QEMU control | 40004, 40005 | 40002, 40003 |
| Private disk I/O | 41001 | 41004 |

Allow the selected transport between the hosts. Keep these ports available;
the scripts also reserve local QMP port 4444 and master serial port 5555.
