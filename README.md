# GiantVM 6.6.32 — RDMA QEMU

本仓库保存与 [RDMA KVM](https://github.com/xue-123-song/GVM-kernel/tree/device-aggregation)
配套的 GiantVM QEMU，基于 **QEMU 9.0.90**。
两台物理机上的 QEMU 协同运行一个 guest OS，guest 看到跨端 CPU，
以及各有 owner 的独立 PCI 设备。两块盘仍是 disk1、disk2，不合并为逻辑卷。

## 当前版本

本次整理从原 `device-aggregation` 分支 `0be1943958` 起保留提交历史；
新增仓库入口说明和 2026-10-09 IRQ locality 验证记录，功能代码保持不变。
`main` 与 `device-aggregation` 在整理时指向同一版本。
配套 KVM 功能基线为 `878db8c75`，host 内核名 `6.6.32-krdma+`。

## 功能与边界

- `-local-cpu` 描述本节点 CPU 范围和 IPoIB 节点列表。
- `loom-aggregate=on,loom-numa-node=N` 为设备标记 owner；
  `LOOM_DEVICE_AGGREGATION=1` 启用现有设备聚合实现。
  这些是代码保留的参数名，未在仓库迁移时重命名。
- 跨端 PCI 配置和设备访问由现有设备转发逻辑处理；
  QEMU 对 guest RAM 的相关访问经过 DSM，DMA pin 遵守内核所有权协议。
- 已验证跨端独立 raw 数据盘读写、flush、跨端回读；
  VFIO 网卡有独立启动/测试工具，当前带宽回归未挂网卡，
  不能将本轮结果解读为真实网卡聚合吞吐测试。
- **双盘并行目前没有带宽增益**；共享状态/页面争用仍待定位。

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
下面使用已部署的专用 1 GiB raw；新机器需先另行准备数据盘。

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
