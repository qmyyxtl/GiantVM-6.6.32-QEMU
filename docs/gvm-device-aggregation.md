# RDMA 版设备聚合

本分支在 GVM 的 RDMA 巨虚上移植 Loom PCI owner/remote 结构。一个 OS 分别看到各节点持有的 PCI 设备，设备不合并为逻辑卷或网络接口。

## 当前运行配置（Garen / Ezreal，2026-10-08 VFIO 枚举验证）

Garen 为 master（CPU0/1，172.0.1.4），Ezreal 为 slave（CPU2/3，
172.0.1.2）。两端 host 均为 6.6.32-krdma+，QEMU 9.0.90，一个
4 CPU / 4 GiB guest 使用 6.12.0-vanilla、clocksource=hpet，可登录 root。

两端各将空闲的 X710 功能 0000:0c:00.1 从 i40e 改绑 vfio-pci。
该功能各自独占 IOMMU group 25。guest 00:07.0 属于 Garen，00:08.0
属于 Ezreal，均识别为真实 Intel X710 [8086:1572]，不是 e1000 模型。
CPU0 和 CPU2 分别读取两块设备的 PCI 配置，四组 vendor/device 均为
86 80 72 15。两端 QMP query-status 均为 running。

启动沿用 scripts/gvm-device-node.sh，设置 GVM_USER_NET=0、
GVM_IPLIST="172.0.1.4 172.0.1.2"、GVM_GUEST_EXTRA_ARGS=clocksource=hpet；
master 使用 /home/xtl/start/bzImage-6.12-bss2m。每端追加：

```text
-device vfio-pci,host=0000:0c:00.1,id=vfio-owner-N,addr=BDF,rombar=0,x-no-mmap=on,x-no-kvm-msi=on,x-no-kvm-msix=on,loom-aggregate=on,loom-numa-node=N
```

N=0 时 BDF=07.0，N=1 时 BDF=08.0；须先在对应 host 改绑该 PCI 功能。
本次启动与改绑管理器通过内存执行，未产生实验脚本、日志、镜像或 overlay。
退出后管理器恢复原 i40e 驱动。不要改绑承载 SSH 的 I350、RDMA 传输卡，
或仍承载 host/macvtap 网络的 X710 0000:0c:00.0。

**当前只通过 PCI 纳入及配置空间访问，未通过 VFIO 收发或跨机硬件 DMA。**
guest 没有为 X710 绑定驱动，modprobe i40e 返回缺少
/lib/modules/6.12.0-vanilla 对应模块。Garen 空闲端口在单机对照退出并
恢复 i40e 后启用检查，ethtool 报 Link detected: no；Ezreal 空闲端口的
物理链路在此次验证中未测。后续收发需要匹配的 guest 驱动及有效链路，
还需要解决 VFIO IOMMU 映射与 RDMA DSM 页面所有权的配合：硬件 DMA
直接访问 owner host 的 RAM，不经过现有 KVM_DSM_MEMCPY/MEMPIN 软件路径。

此前 macvtap + e1000 配置通过 EDU BAR/DMA/MSI/双向并发及四组各
10 个 1400-byte ICMP 包，详见后文；这些结果不作为本次 VFIO 的收发结果。
已修复远端 CPU BQL 错配和退出忙等，并验证退出、接口清理及重新启动。

磁盘后端可以是每个 owner 各自的 raw 或 qcow2 文件，guest 保持两块
独立磁盘；也可使用独立物理块设备。镜像格式不解决跨机 DMA/virtqueue
缓存一致性问题。当前 Garen/Ezreal 没有独立空闲物理数据盘。经用户授权，已各创建一个
1 GiB raw 稀疏数据盘：Garen /home/xtl/start/gvm-device-data-node0.raw，
Ezreal /home/xtl/start/gvm-device-data-node1.raw，初始各实际分配 4 KiB。
它们尚未挂入 guest，未验收跨机磁盘读写。

两端已连通的 X710 0000:0c:00.0 均为 10 Gb/s，sriov_totalvfs=64、
sriov_numvfs=0。可以保留 host PF，并从这个物理端口创建 VF 供 guest
直通；这不需要启用未连通的第二物理端口。PF 直通需要 i40e；VF
直通需要 iavf。原 guest 源码配置的 CONFIG_I40E、CONFIG_I40EVF
均关闭，已有镜像的驱动模块版本也不匹配运行中的 guest 内核。

每次 host 重启后恢复 IB 地址（仅运行时配置）：

```bash
# Garen
sudo /home/xtl/GVM-qemu/scripts/gvm-rdma-link.py 172.0.1.4/24
# Ezreal
sudo /home/xtl/GVM-qemu/scripts/gvm-rdma-link.py 172.0.1.2/24
```

Garen 标准 QEMU 的 worker-g4 有自定义 KVM exit 42 的 ABI 兼容问题，
目前 paused，见末节。物理数据盘聚合、真实 VFIO DMA、长期稳定性未验收。

## 早期启动配置（Ezreal / Jinx）

Host 使用现有 6.6.32-krdma+，QEMU 基于 9.0.90。默认 4 CPU、4 GiB，Ezreal 执行 CPU0/1，Jinx 执行 CPU2/3。两端必须紧接着启动，沿用已有 IB 地址 172.0.1.2、172.0.1.3。

在 Ezreal/Jinx 的 /home/xtl/GVM-qemu 分别运行：

```bash
GVM_QUIET=1 ./scripts/gvm-device-node.sh 0  # Ezreal
GVM_QUIET=1 ./scripts/gvm-device-node.sh 1  # Jinx
```

节点0使用已有 /home/xtl/start/bzImage 和原始 Debian 根盘；节点1不指定 kernel。脚本不创建日志、PID 文件、磁盘副本或 overlay。默认各 owner 一个 EDU，guest BDF 为 00:05.0、00:06.0。LOOM_DEVICE_AGGREGATION=1 启用聚合；设备以 loom-aggregate=on 标记，addr 必须全局唯一。额外 QEMU 参数可以直接追加到脚本后。

串口与 QMP 分别监听各主机的 loopback 5555（master）和 4444：

```bash
ssh -t Ezreal 'socat STDIO,raw,echo=0,escape=0x1d TCP:127.0.0.1:5555'
```

## 机制

- owner 持有唯一设备模型，其他节点创建 loom-remote-pci。启动前通过 RDMA 获取根总线静态目录；不映射共享 DAX，也不依赖跨主机共享指针或原子操作。
- 配置与 BAR 操作定向 owner，读写均得到处理确认。BAR 映射前缀按需从 owner 刷新；扩展配置读取由 owner 执行。
- RDMA 调用有长度检查、每 peer 的响应串行化及超时；等待期间释放 BQL。超时意味着操作结果未知，不自动重试设备 I/O。
- 模拟 DMA 沿用 GVM 的 KVM_DSM_MEMCPY/MEMPIN。真实 VFIO 硬件 DMA 尚未接入这一协议。
- 第一版跨节点 MSI 支持 fixed、physical、xAPIC 目标，按全局 CPU 归属路由，在设备回调结束后发送。聚合模式停用本地 irqfd 快捷路径。
- INTx 经 master 的统一 PCI/IOAPIC 视图投递。配置复位具有设备代次；访问失败停止当前 QEMU，不提供透明恢复。

## 验证

scripts/gvm-device-guest-test.py 在 guest 中验证 CPU0/CPU2 × owner0/owner1 的枚举、BAR、有序访问、DMA 往返和双向并发请求。DMA 缓冲区使用启动参数预留的 64 KiB guest RAM。

MSI 目标测试使用 Linux 现有 IRQ-work 向量 0xf6，观察 /proc/interrupts 的 IWI 计数。它证明投递到指定 CPU，但不能代替 PCI 驱动的 IRQ 分配、亲和性、MSI-X 或 irqfd 集成测试。测试代码可经串口以内存 exec 运行，无需在 guest 保存脚本。

## 当前边界

只支持单根总线上的静态 PCI endpoint；动态热插拔、逻辑/广播 MSI、内核 irqfd 加速、真实 VFIO DMA、跨节点故障 supervisor 尚未实现。现有根盘仍使用 master 的旧设备路径。设备聚合逻辑位于 QEMU；Garen 的 host 内核使用同步后的 RDMA 基线编译。


## 早期 EDU 真实双机验证（Ezreal / Jinx）

Ezreal 与 Jinx 均保持 6.6.32-krdma+，各运行 QEMU 9.0.90；同一 guest 为 6.12.0-rc5-g8f585d63b94a-dirty，4 CPU 在线。root/root 可通过串口登录。

- Guest 同时枚举 00:05.0（owner0/Ezreal）和 00:06.0（owner1/Jinx）。
- CPU0/CPU2 × owner0/owner1 的四组 BAR 身份、有序写读、阶乘计算均通过。
- 四组 4096-byte DMA 往返均通过；切换到另一节点 CPU 后，数据仍一致。
- owner × 提交 CPU × 目标 CPU 的八组 MSI 各完成 8 次 IRQ-work 投递。
- 双向并发访问通过：两个进程分别固定到 CPU0/CPU2，各访问远端 owner 128 轮。
- 未创建实验日志、镜像、overlay 或 guest 测试文件；测试代码由串口直接送入 Python 内存执行。

上述结果只验收模拟 EDU 与 fixed physical MSI 路由，不代表真实 VFIO DMA、网卡收发、磁盘读写或 Linux PCI 驱动 IRQ 亲和性已经验收。


## 真实硬件后端检查（2026-10-08）

这项检查将 QEMU e1000/virtio-blk 连接到物理以太网与物理块设备，不是 VFIO PCI 直通。RDMA 巨虚仍使用 ibs4f0；SSH 管理网卡不改绑。

- Ezreal 的 X710 ens101f0np0 已连接，Jinx 的 X710 ens101f0 属于现有 br0。其他空闲以太网口及第二个 IB 口未检测到链路。
- Jinx /dev/sdb 为 116.4 TiB 物理 RAID 逻辑盘，sdb1 挂载 /data 并提供 NFS；它与系统盘共用 smartpqi 控制器 32:00.0，因此不能整控制器 VFIO 直通。
- 数据盘以 O_RDONLY | O_DIRECT 打开，通过 fdset 和 readonly=on 提供给 guest。没有卸载 /data，没有对物理盘执行写入，没有创建测试镜像、overlay 或日志文件。

双节点启动尝试中，00:07.0、00:08.0 网络设备及 owner1 的 00:09.0 块设备进入两端聚合目录，但启动未完成：Jinx 设备模型线程停在 KVM_DSM_MEMCPY → kvm_dsm_page_fault → kvm_dsm_fetch → krdma_receive。master 的 PCI owner 请求随后超时。退出/kill 后还留有 DSM 内核等待，需要先清理宿主机状态才能继续。当前不能宣称跨节点网卡收发或物理盘读取通过。

另行在 Jinx 执行单节点对照（同一 QEMU，2 CPU、2 GiB，无 DSM，已有 bzImage-6.12-bss2m；根盘也只读，init=/bin/bash）：

- guest 枚举 e1000 网络设备及 116.4 TiB 的只读 /dev/vdb 数据盘。
- CPU0/CPU1 各从偏移 0、16 MiB、1 GiB、1 TiB 读取 1 MiB，8 次 SHA-256 均与 host /dev/sdb 一致。
- guest 192.168.100.242 经 Jinx br0 → ens101f0 → 物理网络访问 Ezreal 192.168.100.17；CPU0/CPU1 各发 10 个 ICMP 包，均收到 10 个回复，丢包为 0。
- 修复接入脚本中的 TAP MAC 问题：桥端 TAP MAC 必须与 guest 网卡 MAC 不同，否则 local FDB 会截留外部单播回包。macvtap 的 MAC 则需匹配 guest。

可复用的只读单节点检查启动方式：

```bash
# Jinx；复用已有 br0、guest kernel 和原始根盘
sudo ./scripts/gvm-device-hardware-node.py 1 --parent br0 \
    --data-disk /dev/sdb --single-node \
    --guest-kernel /home/xtl/start/bzImage-6.12-bss2m
# 串口 loopback 5556、QMP 4445；shell 中挂载 proc/sysfs 即可检查。
```

启动器退出后删除自己创建的网络接口，不生成文件。这次早期多节点硬件后端尝试有上述启动阻塞；后文记录 Garen / Ezreal 配置的验证结果。真实 VFIO DMA 仍未实现。

代码阅读还发现 MemoryRegionCache/virtio ring 的直接 RAM 访问未经过 DSM；krdma_receive 的 EAGAIN 等待循环缺少 signal/stop 检查。它们是后续需要修补和验证的路径，本次没有改 host 内核，也没有把源码判断当作已确认的完整根因。


## Garen master / Jinx slave

两端先运行 `sudo ./scripts/gvm-rdma-link.py ADDRESS/24`：Garen 为
172.0.1.4，Jinx 为 172.0.1.3。运行内核须支持 KVM DSM RDMA。两端设置
`GVM_IPLIST='172.0.1.4 172.0.1.3'`，Garen 执行 node 0，Jinx 执行 node 1。
Garen master 可复用 `/home/xtl/start/bzImage-6.12-bss2m`。

聚合网络设备关闭 option ROM (`romfile=`)：当前远端设备描述只发布普通
BAR，并未发布 expansion ROM；网络设备不承担 guest 启动。此设置关闭
一个不受支持的路径，并不证明它是先前 DSM 阻塞的根因。物理数据盘
使用各 owner 唯一的 BDF：node0 为 00:09.0，node1 为 00:0a.0。


## Garen master / Ezreal slave

Garen 使用 172.0.1.4，Ezreal 使用 172.0.1.2，两端设置
`GVM_IPLIST='172.0.1.4 172.0.1.2'`。Garen 的 QEMU 构建未包含
libslirp，两端显式设置 `GVM_USER_NET=0`，串口登录和聚合设备测试
不依赖默认 user 网络。Garen 执行 node 0，Ezreal 执行 node 1；
master 设置 `GVM_GUEST_KERNEL=/home/xtl/start/bzImage-6.12-bss2m`。
物理网络后端可在两端选择现有 `ens101f0np0`，无需改绑 IB 或管理网卡。


### Garen / Ezreal EDU 验收（2026-10-08）

Garen master 与 Ezreal slave 均运行 6.6.32-krdma+，guest 使用已有
bzImage-6.12-bss2m，运行版本为 6.12.0-vanilla，4 CPU 在线，可进入
root shell。CPU0/CPU2 × owner0/owner1 的 BAR、有序访问、4096-byte
DMA 往返与跨 CPU 数据检查全部通过；八组 MSI 各 8 次投递通过，
两个 CPU 同时访问各自远端 owner 各 128 轮通过。两端 IPoIB ping
均 5/5，双向 rping 校验各 1000 × 4096 byte 通过。

GVM_USER_NET=0 同时指定 -nic none，禁止 QEMU 自动添加默认网卡。


退出测试发现旧 GVM 的远端 vCPU 分支在释放 BQL 后进入远端等待，
退出时未重新获得 BQL，就进入公共销毁路径并再次 bql_unlock，
每节点两个远端线程触发断言。现修复为释放远端锁后重新获得 BQL，
再执行公共销毁逻辑。启动器关闭 core dump，避免失败时产生文件。


### Garen / Ezreal 物理网络后端验收（2026-10-08）

两端各经 macvtap 连接 ens101f0np0，guest 分别枚举 00:07.0、00:08.0，
e1000 驱动均成功绑定。它们是各自独立的 QEMU PCI 网卡、物理网络
后端，不是 VFIO 直通，也没有将两块网卡合并成一个逻辑接口。

Guest enp0s7 为 192.168.100.241，enp0s8 为 192.168.100.242。CPU0、
CPU2 各经 enp0s7 访问 Ezreal 192.168.100.17、经 enp0s8 访问 Garen
192.168.100.14：四组各 10 个 1400-byte ICMP 包全部收到，丢包为零。
两块 guest 网卡的 rx_errors/tx_errors/rx_dropped/tx_dropped 均为零。
代码在串口 Python 内存执行，未保存 guest 脚本；可复用源码为
scripts/gvm-device-guest-network-test.py，测试只修改 guest 运行时
网络配置，不写持久化网络文件。这是功能测试，不是性能基准。

Garen 与 Ezreal 各只有系统用的 /dev/sda，未发现独立数据盘；
当前物理网络配置没有为 guest 附加这些系统盘，未验证真实数据盘
聚合。现有系统盘、Garen 的保留 DRAM pmem0 都不能冒充独立数据盘。


退出验证进一步确认旧 wakeup_remote_cpu 的非原子计数空循环被
优化成无限跳转：GDB 中计数已经为 0，反汇编仍为跳回自身的 jmp。
接收线程不能退出，主线程停在 rdma_router_cleanup 的 pthread_join。
现使用远端互斥锁、条件变量、通知代次及存活线程计数：先设待确认
数量再通知，每线程每代确认一次；shutdown 状态保留，重复通知
不会等待已退出的线程。公共清理还覆盖 SIGTERM 发起的退出。


### 当前时钟配置与启动命令

复测时曾出现一次远端网卡 ping 5/10。进一步测得 guest 使用
kvm-clock 时，CPU2 切回 CPU0 的 CLOCK_MONOTONIC 倒退约 65–75 ms；
使用 host 计时限定等待的 raw ICMP 四组各 10 包均收到，1400-byte
载荷逐字节校验通过。切换 guest 到 hpet 后，八次跨 CPU 采样均递增，
常规 ping 四组各 10 包再次全部收到。该配置是本轮时钟问题的规避
措施，并未修复跨 host 的 KVM pvclock 协议，也没有验收性能或长期
稳定性；首组 ping 仍可出现较大延迟。

两端紧接着启动，不生成日志、镜像、overlay 或 PID 文件：

```bash
# Garen master，CPU0/1
sudo GVM_USER_NET=0 GVM_IPLIST='172.0.1.4 172.0.1.2' \
  GVM_GUEST_KERNEL=/home/xtl/start/bzImage-6.12-bss2m \
  GVM_GUEST_EXTRA_ARGS='clocksource=hpet' \
  /home/xtl/GVM-qemu/scripts/gvm-device-hardware-node.py 0 \
  --parent ens101f0np0

# Ezreal slave，CPU2/3
sudo GVM_USER_NET=0 GVM_IPLIST='172.0.1.4 172.0.1.2' \
  /home/xtl/GVM-qemu/scripts/gvm-device-hardware-node.py 1 \
  --parent ens101f0np0
```

Master 串口监听 Garen loopback 5555，两端 QMP 都在 loopback 4444。
Guest 网络测试脚本配置运行时地址，见前节；本配置没有附加物理数据盘。

Garen 的旧 libvirt worker-g4 使用系统标准 QEMU，重启后报告
`KVM: unknown exit reason 42`（自定义 KVM_EXIT_DSM_APIC_BASE），
目前 paused；常规 QEMU 与该 RDMA 内核的 ABI 兼容需要另行处理。

## 2026-10-08 guest 内置 X710 驱动编译与单机验证

在 Garen /home/xtl/GVM-guest-kernel（d48b38507 基线，原有 fair.c 修改
保留）启用 CONFIG_I40E=y、CONFIG_I40EVF=y，后者选择 CONFIG_IAVF=y。
LOCALVERSION=-vanilla-vfio、LOCALVERSION_AUTO=n；使用 make -j16
LOCALVERSION= bzImage 编译成功，版本 6.12.0-vanilla-vfio。

链接脚本 BSS 起点与终点均使用 ALIGN(PMD_SIZE)，延续正在运行的
bzImage-6.12-bss2m 的 2 MiB 边界。新 ELF 的 __bss_start 为
ffffffff83800000，__bss_stop 为 ffffffff83a00000，i40e_init_module、
iavf_init_module 均已链接进内核。

产物在 Garen /home/xtl/start/bzImage-6.12-vfio-bss2m，配置归档为
同名 .config；SHA256 为
ad76802aa96aa68ca277e892f1619d5e0047e040d5e2cbf90f9df3426b6670e2。
未安装或切换 host 内核。

暂停原跨机配置后，用 Garen 空闲 0000:0c:00.1 做单机 VFIO 对照：
2 CPU、2 GiB，已有 Ubuntu raw 根盘只读，init=/bin/bash。guest 的
00:07.0 显示 Kernel driver in use: i40e，出现 eth0，读到固件
fw 9.140.77473 / nvm 9.40、MAC 9c:c2:c4:ba:9b:20。
iavf 的初始化日志存在，但此次没有 VF 设备，未验收 VF 驱动 probe。
物理第二端口没有已确认的有效链路，未验证收发。

单机对照已退出并恢复 host 驱动，随后恢复原双节点 VFIO 枚举配置。
原跨机配置继续使用旧 6.12.0-vanilla 内核；新内核尚未用于跨机
硬件 DMA 验证。单机驱动初始化通过不代表 RDMA DSM 的 DMA 路径通过。
