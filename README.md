# GiantVM 6.6.32 — RDMA QEMU

本仓库保存与 [RDMA KVM](https://github.com/xue-123-song/GVM-kernel/tree/device-aggregation)
配套的 GiantVM QEMU，基于 **QEMU 9.0.90**。
两台物理机上的 QEMU 协同运行一个 guest OS，guest 看到跨端 CPU，
以及各有 owner 的独立 PCI 设备。两块盘仍是 disk1、disk2，不合并为逻辑卷。

## 当前版本

当前开发在原 origin `git@github.com:xue-123-song/GVM-qemu.git` 的
`device-aggregation` 分支。`0deb973ad9` 实现控制面目录推送，
配套 `86812dc787` 修复远端 CPU 占位线程暂停状态，验证记录见末尾。
配套 KVM 源码基线为 `c05e13ce3`，两端 host 内核名仍为 `6.6.32-krdma+`。

供同事使用的 GiantVM 新仓库保留 2026-10-09 整理时的快照；
该快照保留此前历史，`main` 与 `device-aggregation` 当时指向同一版本，
本轮没有更新该副本。

## 功能与边界

- `-local-cpu` 描述本节点 CPU 范围和 IPoIB 节点列表。
- `loom-aggregate=on,loom-numa-node=N` 为设备标记 owner；
  `LOOM_DEVICE_AGGREGATION=1` 启用现有设备聚合实现。
  这些是代码保留的参数名，未在仓库迁移时重命名。
- 跨端 PCI 配置和设备访问由现有设备转发逻辑处理；
  QEMU 对 guest RAM 的相关访问经过 DSM，DMA pin 遵守内核所有权协议。
- 可选 owner IOMMU 模式通过标准 ACPI DMAR 按设备归属提供控制单元；
  已接入 VFIO 标准映射生命周期，并验证两端独立 X710 VF 的真实收发；
  网卡吞吐和非 DSM 开销优化仍未完成。
- 已验证跨端独立 raw 数据盘读写、flush、跨端回读；
  VFIO 网卡有独立启动/测试工具，当前带宽回归未挂网卡，
  不能将本轮结果解读为真实网卡聚合吞吐测试。
- 最新双盘读测试已观察到带宽增益：2935 MiB/s，约为两端单盘吞吐之和的 83%；
  尚未达到线性扩展，剩余开销仍待进一步区分。

## 构建

构建依赖包括编译器、Python 3、Ninja、pkg-config、GLib/Pixman 开发包；
其余依赖依 configure 检查与启用功能补齐。

```bash
git clone -b device-aggregation git@github.com:xue-123-song/GVM-qemu.git
cd GVM-qemu
mkdir -p build
cd build
../configure --target-list=x86_64-softmmu --enable-kvm --enable-vnc --disable-werror
ninja -j8 qemu-system-x86_64
./qemu-system-x86_64 --version
```

历史整理记录与后续原 origin 上的开发记录分别保留；当前构建和验证见末尾。

## 两端启动

先准备匹配 host 内核、正常 RDMA/IPoIB 链路、既有 root 镜像，
以及 master 使用的 guest bzImage。当前验证 guest 为
`6.12.0-vanilla-vfio #20`，4 vCPU / 4 GiB，`clocksource=hpet`。
下面是使用既有专用 1 GiB raw 的基础 INTx 启动示例；
最新 MSI-X/IOThread 参数见末尾。新机器需先另行准备数据盘。

在两端各自的本仓库目录执行；需要 host 上的 KVM/DSM 权限。
以下保留当前部署使用的镜像路径，按实际环境替换。

Garen/master：

```bash
sudo env GVM_USER_NET=0 GVM_IPLIST='172.0.1.4 172.0.1.2' \
  GVM_GUEST_KERNEL=/home/xtl/start/bzImage-6.12-vfio-bss2m \
  GVM_GUEST_EXTRA_ARGS=clocksource=hpet \
  ./scripts/gvm-device-node.sh 0 \
  -drive file=/home/xtl/start/gvm-device-data-node0.raw,format=raw,if=none,id=data-owner-0,cache=none,aio=threads \
  -device virtio-blk-pci,drive=data-owner-0,id=data-owner-0,addr=09.0,serial=GVM-DATA-OWNER0,ioeventfd=off,vectors=0,loom-aggregate=on,loom-numa-node=0
```

Ezreal/slave（不指定 guest bzImage）：

```bash
sudo env GVM_USER_NET=0 GVM_IPLIST='172.0.1.4 172.0.1.2' \
  ./scripts/gvm-device-node.sh 1 \
  -drive file=/home/xtl/start/gvm-device-data-node1.raw,format=raw,if=none,id=data-owner-1,cache=none,aio=threads \
  -device virtio-blk-pci,drive=data-owner-1,id=data-owner-1,addr=0a.0,serial=GVM-DATA-OWNER1,ioeventfd=off,vectors=0,loom-aggregate=on,loom-numa-node=1
```

QMP 在每端 `127.0.0.1:4444`，master 串口在 `127.0.0.1:5555`。
使用现有串口客户端登录 guest root 后再运行串口执行工具。

## guest 中断配置与带宽测试

不修改 guest 内核代码；在 guest root shell 配置运行时 IRQ affinity。
先根据 `/proc/interrupts` 和 PCI/virtio 对应关系确认 IRQ；
下列 10/11 仅适用于当前 `vectors=0` 的测试拓扑，其他设备布局会改变编号。

```bash
echo 0 > /proc/irq/10/smp_affinity_list  # Garen 的 vda -> guest CPU0
echo 2 > /proc/irq/11/smp_affinity_list  # Ezreal 的 vdb -> guest CPU2
cat /proc/irq/10/effective_affinity_list
cat /proc/irq/11/effective_affinity_list
cat /proc/interrupts
```

中断迁移可能到下一次中断才生效；应在 I/O 后确认 effective affinity 和计数。
核对 irqbalance 是否会改写；当前 guest 中该服务 inactive。
本配置重启后失效；若持久化，应按设备身份发现 IRQ 再设置，不能普遍硬编码 10/11。

在 master host 运行既有测试工具（专用测试数据盘会被覆盖）：

```bash
./scripts/gvm-device-serial-exec.py \
  ./scripts/gvm-device-guest-disk-bandwidth.py \
  --guest-args='--seconds 15 --rounds 3' \
  --timeout 240 --preserve-on-timeout
```

默认先完整预写两盘，再轮换测试单盘0、单盘1、双盘并行。
仅对已经完整预写的既有测试盘，使用 `--skip-prefill` 跳过准备。
worker 在 CPU0/CPU2 固定后才创建并首触缓冲区；每端写自己的 owner 盘。
工具通过串口内存传入代码，不创建 guest 脚本、日志或结果文件。

2026-10-09，修改 IRQ 后三轮 15 秒、1 MiB、QD=8 中位数：

| 工作负载 | MiB/s | 两端 DSM 慢路径 fault / 写入 MiB 范围 |
| --- | ---: | ---: |
| Garen 单盘 | 968.5 | 6.9–11.2 |
| Ezreal 单盘 | 940.3 | 8.6–9.9 |
| 双盘并行合计 | 555.8 | 149–174 |

并行带宽只有最快单盘的 0.574 倍、两端单盘中位数之和的 0.291 倍。
计数包含后台活动且非原子；fault 的热点页面尚未逐页定位。
1 GiB 数据集循环覆盖，不能据此宣称介质稳态性能。
完整因果对照与已知启动 WARN、历史 deadlock 见
[实验与设计记录](docs/gvm-device-aggregation.md)。

## 工具入口

| 工具 | 用途 |
| --- | --- |
| `scripts/gvm-device-node.sh` | 启动单个分布式节点 |
| `scripts/gvm-rdma-link.py` | RDMA/IPoIB 链路相关工具 |
| `scripts/gvm-device-vfio-node.py` | VFIO 设备测试入口 |
| `scripts/gvm-device-hardware-node.py` | 真实网络/存储后端；模型设备，不是 VFIO |
| `scripts/gvm-device-guest-disk-test.py` | 独立磁盘正确性验证 |
| `scripts/gvm-device-guest-disk-bandwidth.py` | owner-aware 单盘/双盘写带宽 |
| `scripts/gvm-device-serial-exec.py` | 通过已登录串口在内存中执行 guest 工具 |

保留上游 `README.rst`、版权、许可证和历史署名。许可见 `COPYING` 等原文件。
guest 镜像、部署 raw、编译产物不纳入本次整理。

## 2026-10-10：不修改 guest 的 owner 本地 I/O 路径

后续开发继续在原 origin `git@github.com:xue-123-song/GVM-qemu.git` 的
`device-aggregation` 分支进行；上面的 GiantVM 新仓库是供同事使用的快照，
本轮未向该副本推送。

`500b5fa0e8` 将 BAR 路由改为先检查 owner 本地目录。本地命中仍校验设备
generation，但不再刷新无关远端目录。两端 CPU 对两端 EDU 寄存器读写及
PCI memory-enable 关闭/恢复通过；探针观察到约 1.24 万次本地磁盘 doorbell，
对应的远端目录 RPC 为零。远端访问与未知地址仍保留原有刷新行为，
因此 HPET/串口等非 PCI 访问的目录轮询尚未消除，不能宣称控制面失效机制已完成。

聚合模式禁止本地 irqfd，以免完成中断绕过 QEMU 的跨节点 MSI 路由。
virtio PCI 应在选择 guest notifier 时直接使用用户态 notifier，而不是先尝试
被禁止的 irqfd 再导致 virtio-blk 数据路径回退。MSI-X、owner 本地 ioeventfd
和 IOThread 可以组合使用；普通非聚合模式的 irqfd 选择保持原样。

在两端各自增加一个 IOThread，并将本地数据盘连接到该线程：
```
-object iothread,id=owner-io,poll-max-ns=0
-device virtio-blk-pci,...,ioeventfd=on,vectors=5,iothread=owner-io,loom-aggregate=on,loom-numa-node=N
```
这里的省略号需替换为前面例子的 drive/id/addr/serial；不改变 raw 后端
`cache=none,aio=threads`，不启用 IOThread 忙轮询。QMP `query-iothreads`
与线程级探针均确认两端队列在各自 IOThread 上处理，数据校验读测试通过。
当前四队列 MSI-X 中断为 managed IRQ，直接写 affinity 可能返回 EPERM；
应核对队列与 CPU 的实际对应关系，不能照搬上面的 INTx 配置。

4 vCPU / 4 GiB / 双 NUMA，1 MiB O_DIRECT Linux AIO 顺序读；
单盘两 worker、每 worker QD4；双盘四 worker、总 QD16。
worker 在对应 owner CPU 上固定后首触缓冲区；使用既有 1 GiB raw 数据，
计时读取降低频率，每阶段 8 秒、三轮，无性能探针：

| 配置 | Garen 单盘 | Ezreal 单盘 | 双盘合计 |
| --- | ---: | ---: | ---: |
| 本地 BAR 优化，ioeventfd=off | 1895.9 | 1617.0 | 1662.5 |
| 本地 BAR 优化，ioeventfd=on + IOThread | 1562.0 | 1712.8 | 2303.0 |

单位 MiB/s，数值为三轮中位数。双盘提高约 39%，但 Garen 单盘下降约 18%；
双盘只达到两端单盘吞吐之和的约 70%，尚未达到线性扩展。
两组之间重启了 guest，DSM 页状态会变化，不能把全部差值归因于单项代码。
小容量循环读取和硬件 RAID 缓存也限制了结果的外推，不代表持续介质带宽。
本轮没有挂网卡，没有测量 VFIO 网卡聚合吞吐。

跨节点完成路径也已验证：在 CPU2/3 上读取 Garen 盘、在 CPU0/1 上读取
Ezreal 盘，数据校验通过。Garen 盘 req.0/1 分别在远端 CPU2/3 增加
902/857 次中断；Ezreal 盘 req.2/3 分别在远端 CPU0/1 增加 754/768 次中断。
该短测用于验证路由正确性，缓冲区处于远端，不能作为本地吞吐对照。

## 2026-10-10：将目录更新移到控制面

运行时不再为每次 BAR/非 PCI 访问拉取远端设备目录。owner 在 PCI 配置写入、
BAR 重配置和 reset 中发布 448 字节前缀，并在控制操作返回前等待所有 peer 确认。
启动时仍用目录 RPC 拉取完整描述符；协议版本升为 3，所有节点必须一起更新。
扩展配置读取仍使用 owner RPC，与原来的转发契约一致。

- 目录更新和确认直接由 RDMA 接收线程处理，只获取目录缓存锁，不获取 BQL。
  普通请求由每个 peer 的 FIFO 工作线程处理并注册 RCU；队列最多 1024 项。
  控制通知绕开普通 response-CQ 锁，避免配置 RPC 中的反向确认互等。
- reset 开始发布 odd sequence，结束发布新的 generation/even sequence；
  快照等待稳定前缀。确认或 reset 发布超过 5 秒未完成时停止聚合，
  不重试结果未知的 I/O；这不是节点故障后的持续可用机制。
- 收到更新后增加本地 epoch，代理映射只在 epoch 改变时刷新。
  普通跨端 MMIO 转发也先走聚合路由；代理回调拒绝失效的旧 BAR 地址，
  INTx 使用已发布的 generation，避免接收新 generation 后仍检查旧描述符。
- `86812dc787` 单独修正远端 vCPU 占位线程的 pause/resume 状态。
  这类线程不执行 guest；本节点无需等待它消费 vCPU halt_cond。
  普通 CPU 的处理保持原样。

验证使用同一个 `6.12.0-vanilla-vfio #20` guest，没有修改 host/guest 内核、
guest 驱动或 DSM 协议。已通过两端 CPU 对两端未绑定驱动的 EDU 做 BAR
重定位、新地址寄存器读写、旧地址失效及恢复；两端并发各五次重配置通过。
两端 stop/cont、整机 reset 后重新登录通过；普通四 vCPU TCG 配置的
stop/cont/reset/退出也通过，后者仅验证通用控制逻辑，不是普通 KVM 性能对照。

包含重配置和双盘读的 15 秒探针窗口中，两端各发布 51 次控制更新，
处理 3029/2959 次磁盘队列通知；HPET 访问合计 135089 次，另有串口访问。
两端目录 RPC 均为 0。PCI 设备的实际远端访问和 slave 的 HPET owner 转发
仍按原契约执行，不能将目录 RPC 归零解释为所有跨端 I/O RPC 归零。

沿用前节 1 MiB 顺序读、单盘两 worker QD8/双盘四 worker QD16，
首触缓冲区在设备 owner 的 CPU 上，每阶段 8 秒，三轮，无性能探针。
代码通过串口在内存中执行，不创建脚本、日志或结果文件；本轮不做预写，
读结果校验既有两盘的数据模式。这里是两 worker 的内存测试程序，
不应将上面默认单 worker 的旧脚本直接视为相同测试。

| 配置 | Garen 单盘 | Ezreal 单盘 | 双盘合计 |
| --- | ---: | ---: | ---: |
| 本地 BAR 快路径 + IOThread，仍轮询目录 | 1562.0 | 1712.8 | 2303.0 |
| 控制面目录推送 + IOThread | 1823.8 | 1732.4 | 2935.0 |

单位 MiB/s，三轮中位数。双盘增加约 27%，达到两端单盘吞吐之和的约 83%；
Garen 单盘增加约 17%，Ezreal 约 1%。不同阶段间重启过 guest，DSM 页状态
以及小容量循环读/RAID 缓存会影响结果，不能把全部差值归因于目录优化。

使用远端 CPU 读 owner 盘的功能检查也通过：Garen 盘 req.0/1 在 CPU2/3
增加 785/695 次中断；Ezreal 盘 req.2/3 在 CPU0/1 增加 752/826 次中断，
数据校验成功。该检查的缓冲区在远端，不能拿其吞吐替代本地性测试。

host 线程固定到 RDMA 卡所在 NUMA node 1 的不同物理核，只使双盘中位数
约提高 4%，同时使 Ezreal 单盘下降约 15%；已恢复原亲和性，没有保留为推荐配置。
完成路径分段探针显示映射/解除映射及 ring 更新包含大量 DSM pin/unpin；
高频探针显著影响吞吐，不能据此把剩余性能损失全部归为 DSM。

当前仍需处理或测量：非 PCI 实际设备转发、本地目录扫描/队列成本、
无需 guest DMA bridge 的真实网卡 DMA 路径及双网卡并行吞吐。
本轮仍未挂网卡，尚未完成“除 DSM 外尽可能降低开销”的整体目标。

### BAR 路由描述符复制优化（2026-10-10）

BAR 地址查找和远端 BAR 回调只读取 generation、BAR 地址/大小等前 192 字节，
不再每次复制包含 PCI 配置的 4288 字节完整描述符。控制路径仍使用完整
snapshot；接收缓存锁、reset 奇数 sequence 等待、generation 校验和失效机制保留。
未改 guest 内核或驱动，HPET 配置保持不变。

两端编译和 checkpatch 通过；CPU0/2 对两端 EDU 的 BAR 移动、memory enable
关闭/恢复、旧地址失效、并发重配置通过。原磁盘读测试三轮中位数：
Garen 单盘双 worker 1783.9、Ezreal 单盘双 worker 1722.9、
双盘四 worker 2933.9 MiB/s。双盘与优化前 2935.0 基本持平，
没有证据证明吞吐提高；不能把复制字节数下降当成实测性能收益。
运行实例仍为四 vCPU、两节点各两 CPU、4 GiB、owner IOThread、MSI-X。

正常关闭上一版实例时，master 返回 0，slave 返回 SIGSEGV；
两端已重新启动并通过上述检查。退出异常尚未定位，不能宣称正常退出已验证。
当前 VFIO staging 路径仍依赖 guest DMA bridge；透明真实网卡支持和
双网卡吞吐目标尚未完成。

### 分布式 QEMU 退出顺序修正（2026-10-10）

旧版在收到 shutdown 请求时就唤醒远端 CPU 占位线程，使其退出并执行
kvm_arch_destroy_vcpu/munmap。此时 VM 停止流程尚未完成，状态通知可能仍遍历
这些 vCPU 注册的回调，合并 MMIO ring 也可能仍引用已释放的 kvm_run 映射。
现场确认旧版 slave 在 kvm_flush_coalesced_mmio_buffer 的 ring 读取处 SIGSEGV；
调整映射生命周期时还暴露 master 的 vm_state_notify 崩溃。

最终只修改退出顺序：请求阶段仅通知主循环；qemu_cleanup 先执行 vm_shutdown，
完成停止通知和 block drain，再唤醒远端 CPU 退出。不保留额外的 ring alias
迁移改动，也不改 guest 或 KVM 内核。仅含该顺序修正的版本在两端重新启动、
root 登录、双盘四 worker 读取校验、guest sync/poweroff 后，两端 QEMU 均返回 0。
普通双 CPU/64 MiB TCG 实例的 QMP quit 也返回 0；这不是普通 KVM 性能基线。
短时读取属于功能回归，不作为新的吞吐优化结论。

透明 VFIO 下一步需要标准 vIOMMU 的设备级映射通知：分别保留 IOVA 与 GPA，
将远端设备映射/失效送至 owner，并在解除映射前发布已完成的 DMA 写入。
当前 master 的 vIOMMU 控制状态不会自动配置 slave；仅添加 intel-iommu 参数
不能解决问题。现有 staging 仍依赖 DMA bridge，尚未实现上述透明路径。

### Q35 / 标准 vIOMMU 基础验证（2026-10-10）

现有 6.12.0-vanilla-vfio guest 内核已内置 SATA_AHCI、INTEL_IOMMU、I40E 和 IAVF，
本次不改 guest 内核/驱动。两端 machine 改为 q35,kernel-irqchip=split，
添加 intel-iommu,caching-mode=on,intremap=off；master guest 命令行增加
intel_iommu=on iommu.strict=1，保留 clocksource=hpet、四 CPU 和两节点 NUMA。

Q35 巨虚已启动并 root 登录。guest 显示 DMAR enabled、Translated domain、
strict invalidation、Queued invalidation 和独立 IOMMU groups；两个 owner 盘
仍可见，双盘四 worker 的短时只读数据校验通过。该检查不作为新的吞吐基线。

两端运行状态直接检查：master dmar_enabled=true、qi_enabled=true、
root=0x4b78000；slave dmar_enabled=false、qi_enabled=false、root=0。
这证明单纯添加 vIOMMU 参数不会同步远端设备 DMA 映射。

启动器 scripts/gvm-device-node.sh 新增 GVM_MACHINE=pc|q35（默认 pc）和
GVM_IOMMU=0|1（默认 0，开启时要求 q35），便于继续验证；本轮运行参数仍使用
既有 owner IOThread/双 NUMA 配置。仅主节点开启 guest DMA remapping，
不是已完成的跨节点 vIOMMU/VFIO 方案，暂不应直接据此挂远端真实网卡。

下一步应将 master 上远端 PCI proxy 的标准 IOMMU 映射/失效通知发送到 owner，
保留 IOVA、GPA、权限和映射生命周期；失效确认前必须完成 owner 的 DMA 回写。
不能让 slave 独立消费同一失效队列并提前发布完成，也不能假设现有 staging
中 IOVA=GPA 的约定适用于翻译后的 DMA 地址。HPET 不作调整。

### 非恒等 DMA 地址与 AHCI 映射标记（2026-10-10）

staging buffer 现在分别保存 IOVA 和 GPA；pool_copy 使用 IOVA，
guest RAM 读写使用 GPA。新增 loom_vfio_dma_register(bc, iova, gpa, size, direction)，
为标准 IOMMU 翻译通知提供入口。旧 bridge REGISTER 仍以 GPA=IOVA 调用，
不会因此自动接入标准 IOMMU。重复注册到不同 GPA、冲突的重叠翻译以及地址溢出被拒绝。

在未修改的 guest 中，通过 Python stdin 分配并锁定一页内存，在 QEMU 中用
临时空 container 调用实际 staging 函数，IOVA=0x4000000000、GPA=0x089a8000。
验证初始拷贝、device-to-CPU 修改 0xbc、CPU-to-pool 修改 0xcd、未修改字节保持 0xa5，
以及注销前将 0xde 写回 guest；guest 直接检查最终内容通过。
旧 identity REGISTER 的重复引用、两次 UNREGISTER、最终 buffer count=0 也通过。
这是 staging 同步函数验证，未通过 VFIO ioctl 映射真实网卡，也未验证跨节点 IOMMU 通知。

Q35 重启过程中另发现根盘 AHCI 超时并最终 VFS panic。源码/运行现场显示：
CLB 的 map_page 输出错误地写到 res_fis_is_dsm，lst_is_dsm 始终为 false，
命令头路径因此跳过已有的 DSM pin/unpin。修正为写入 lst_is_dsm 后，新实例
正常启动、root 登录，并通过双盘四 worker 短时只读校验。不能凭一次启动
断言 Q35/AHCI 的所有问题均已排除。

两端已编译，guest 内核/驱动和 host KVM 未修改，HPET 保留。
下一步评估按 owner 报告独立 DMAR 控制窗口和设备 scope，避免本地 DMA 映射
必须经过 master；该 owner IOMMU 路径及真实双网卡吞吐尚未实现。

### Owner 本地 IOMMU 控制路径（2026-10-10）

LOOM_OWNER_IOMMU=1 为每个 owner 提供独立的标准 VT-d 寄存器窗口，
地址为 0xfed90000 + owner * 4096。master 的 ACPI DMAR 表按设备 BDF 归属
生成多个 DRHD；非聚合设备和 IOAPIC 属于节点 0。每个窗口只由对应 owner
的 Intel IOMMU 模型处理，拥有独立 root table 和 queued-invalidation 状态。
本地访问直接 dispatch；远端访问使用现有释放 BQL 后等待的 owner RPC。
不是将同一队列复制给多端消费。启动目录同时校验各节点的 owner-IOMMU 模式。

在 Garen/Ezreal 两节点实测，未修改的 guest Linux 识别 dmar0/dmar1：
05.0、09.0、根盘 AHCI 属于 dmar0；06.0、0a.0 属于 dmar1。
两端 dmar_enabled/qi_enabled 均为 true，root 分别为 0x4bee000/0x4bec000。
CPU0/2 分别读取两个单元各 64 次，版本均为 0x10。
探针显示 Garen 本地 CPU0 dispatch 64 次、Ezreal 本地 CPU2 dispatch 64 次；
仅跨 owner 读取各产生 64 次 RPC。完成一次完整 system_reset 并重新登录后，
两个单元的 GSTS 均为 0xc4000000，TE/QIES 保持启用，四组读取检查再次通过。
双盘四 worker 短时只读校验通过；该轮不作为新的吞吐基线。

启动器增加 GVM_OWNER_IOMMU=1（默认 0），要求 GVM_MACHINE=q35、
GVM_IOMMU=1；两端使用相同设置，并保留现有 NUMA、owner IOThread 和
clocksource=hpet 配置。该模式目前要求 intremap=off、pasid=off、
device-iotlb=off，未验证三/四节点运行。

本轮已实现 owner 本地的 IOMMU 控制基础，但 VFIO 标准 MAP/UNMAP 通知还没有
自动接入 staging buffer 注册/回写生命周期；真实双网卡验证和性能目标仍未完成。

### VFIO staging 范围失效生命周期（2026-10-10）

loom_vfio_dma_unmap 现在处理覆盖多个映射和部分映射的失效范围，
保留左右存活的 pool 与 buffer，分别保留原 GPA、权限和引用信息。
backend 成功解除硬件 DMA 后，先将失效区间的设备修改发布到 guest，
再释放该区间的 staging；存活区间不会在这一步被整块回写或释放。
发布失败时保留 staging 并向调用者返回错误。container 销毁直接清理资源，
不在销毁阶段重新访问 guest RAM。

两端编译；以实际 loom-dma.c 源码和模拟 RAM 后端运行内存内测试，
覆盖中间拆分、首尾删除、带空洞的多映射失效、写回失败后重试、
地址空间末尾失效、未对齐拆分拒绝、剩余区间双向同步及最终清理。
该验证没有创建磁盘上的实验文件，不是物理 VFIO/DMA 收发验证。
自动接入标准 MAP 通知、真实网卡以及吞吐测量仍需继续推进。
guest 内核、驱动和 HPET 未修改。

### 标准 VFIO MAP 通知接入（2026-10-10）

vfio_iommu_map_notify 使用标准通知中的 IOVA 和 translated_addr/GPA，
调用新的 vfio_container_dma_map_iommu。staging 模式先初始化 pool 和同步
buffer，再交给 VFIO backend，避免硬件观察到尚未填充的 pool。
设备只读映射使用 to-device 同步，允许设备写入的映射使用双向同步。
大叶映射按最多 64 MiB 分段注册；初始化或 backend MAP 失败时回滚。
普通非 staging 模式仍直接使用原 RAM 地址。

gvm-device-vfio-node.py 默认使用 q35、owner IOMMU 和双 NUMA，
不挂 DMA bridge，guest 使用已有标准 IAVF/I40E 驱动。
--legacy-dma-bridge 保留旧测试入口；数据盘改为现有 MSI-X/owner IOThread 参数。
HPET 保留。该入口仍只接受独立 IOMMU group 的 SR-IOV VF，不解绑 host PF。

实际源码配合模拟 RAM/backend 的内存测试通过：初始化先于 backend MAP、
自动映射/失效生命周期、只读权限、大叶分段、初始化失败和 backend 失败回滚、
普通非 staging 路径。此记录尚不证明真实网卡成功初始化或真实 DMA 收发，
需继续进行硬件验证，不能据此宣称吞吐已改善。

### 双 X710 VF 的标准驱动硬件验证（2026-10-10）

两端 0000:0c:00.0 / ens101f0np0 的 host PF 仍使用 i40e、链路 UP，
各创建一个 VF 0000:0c:02.0，独立 IOMMU group，由启动器绑定 vfio-pci。
Garen VF MAC=02:47:56:4d:00:01，Ezreal VF MAC=02:47:56:4d:01:01。
host 的管理端口和 ibs4f0 DSM 链路未解绑。当前 VF 保留供后续测试。

两端用 scripts/gvm-device-vfio-node.py 启动，分别传入 node 0/1、
--host 0000:0c:02.0、--data-disk /home/xtl/start/gvm-device-data-node0.raw
或 node1.raw。env 使用 GVM_IPLIST='172.0.1.4 172.0.1.2'、
GVM_GUEST_EXTRA_ARGS=clocksource=hpet。前提是已创建 VF 并配置上述 MAC，
且两端旧实验 QEMU 已退出。启动器退出时恢复 VF 原 driver；
当前实验仍在运行。

guest 未修改的 6.12.0-vanilla-vfio #20 正常启动并登录，
原生 iavf 驱动识别 00:07.0/enp0s7/dmar0 与 00:08.0/enp0s8/dmar1，
两张独立网卡均报告 10 Gbps 链路 UP。guest clocksource 确认 hpet。

收发使用两个临时子网，避免 guest 内部本地路由绕过硬件：
- enp0s7=198.18.10.2/24，目标 Ezreal host PF 上的 198.18.10.1/24。
- enp0s8=198.18.11.2/24，目标 Garen host PF 上的 198.18.11.1/24。

两组初始 5 次 ping 均无丢包；对端 host PF 的 tcpdump 直接看到上述 VF
MAC 发出的 ICMP request 及 reply，未写 pcap 文件。驱动计数随后刷新，
两张卡 RX/TX 都增长、错误数为 0，不能以首次立即读取的 0 判定无收发。

IRQ affinity 按设备 owner 设置为 07.0 -> CPU0-1、08.0 -> CPU2-3，
活跃队列后续 effective affinity 转到 owner CPU；尚未再次触发的 IRQ
可能仍显示旧 effective affinity，需要下一次实际中断确认。
配置后各 10 次 ping 无丢包；期间使用过 GDB 暂停检查，因此这不是性能对照。
初始 RTT 已有约 20-60 ms，不能宣称网卡开销低或达到线性吞吐。

关闭两张接口后，两端实际 staging map/buffer 均降到 66 个 / 270336 字节，
重新开启接口后再次各 5 次 ping 无丢包，检查未发现 DMA fault 或 TX timeout。
Garen 重新开启后的首批回复出现最高约 1 秒延迟，此处只作为生命周期功能检查。
接口开启时曾观察 Garen 1178 个 / 4825088 字节、Ezreal 1114 个 / 4562944 字节。
现有同步遍历 buffer，pool_copy 再遍历 map 链表，重复查找会随映射数放大，
下一步应去除这部分非 DSM 成本，并测量同步、复制与队列处理时间。

两端测试 IP alias 当前保留；结束此硬件测试时分别删除：
Ezreal: sudo ip addr del 198.18.10.1/24 dev ens101f0np0
Garen: sudo ip addr del 198.18.11.1/24 dev ens101f0np0
停止本实验 QEMU、VF 恢复 iavf 后，确认 VF 未被其他进程使用，再将各 PF
sriov_numvfs 恢复 0。不得在 VF 仍直通时直接删除。Garen 的其他 libvirt VM 未操作。

### DMA pool 地址缓存（2026-10-10）

标准 IOMMU 的单映射 buffer 在注册时缓存 pool HVA，sync_buffer 直接复制，
不再为每个 buffer 或改变的字节段扫描 map 链表。部分失效的临时视图和存活
片段同时调整缓存地址；缓存不指向 LoomDMAMap 对象，map 结构拆分不使存活
pool 地址失效。跨多个映射的旧 bridge buffer 保留原查找路径。

真实源码配合模拟 RAM/backend 的生命周期测试通过，包括中间拆分后的
双向同步、失效范围最终写回、跨 map buffer fallback、映射失败回滚。
固定 Ezreal host CPU0，1178 个 4 KiB buffer，连续 200 次无变化 from-device
同步、三轮：修改前每次约 5210-5225 us，修改后约 551-557 us。
此测试隔离 DSM 和硬件，不是网卡吞吐结果；实际网络效果仍需实测。
guest 和 HPET 均未修改。
