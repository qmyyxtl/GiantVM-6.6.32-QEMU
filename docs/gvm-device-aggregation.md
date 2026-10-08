# RDMA 版设备聚合

本分支在 GVM 的 RDMA 巨虚上移植 Loom PCI owner/remote 结构。一个 OS 分别看到各节点持有的 PCI 设备，设备不合并为逻辑卷或网络接口。

## 启动

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

只支持单根总线上的静态 PCI endpoint；动态热插拔、逻辑/广播 MSI、内核 irqfd 加速、真实 VFIO DMA、跨节点故障 supervisor 尚未实现。现有根盘仍使用 master 的旧设备路径。本分支不修改 host 内核。


## 已完成的真实双机验证

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

启动器退出后删除自己创建的网络接口，不生成文件。多节点硬件后端模式尚有上述启动阻塞，不能作为已验证配置重复运行；真实 VFIO DMA 仍未实现。

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
