# 巨虚磁盘私有 RDMA 缓冲区原型

本文记录默认复制路径及初次验证。后续共用注册缓冲池与直接写 guest 的实现、启动开关和结果见 [RDMA 直接传输](gvm-rdma-direct-io.md)。

此分支从 QEMU `53cf493234`（已恢复的 virtio-blk 基线）出发。Host KVM 保持
`98793b608`，运行版本仍为 `6.6.32-krdma+`；guest 保持
`6.12.0-vanilla-vfio #26`。本次仅修改 QEMU。

## 数据通路

设备 owner 仍负责 virtio-blk、原有 BlockBackend 和本地 raw 文件。根据描述符映射
到的 NUMA RAM memdev，选择内存节点：

- 本节点 RAM：保留原来的 `dma_memory_map / KVM_DSM_MEMPIN` 路径。
- 其他节点 RAM：为描述符分配不参与 DSM 的 host 缓冲区。同一请求的分散段按内存节点
  打包，使用独立 RDMA RC 连接、CQ 和 `gvm-io-memory` 线程传输。
- 写盘：内存节点通过 `KVM_DSM_MEMCPY` 取 guest 数据，RDMA 返回设备 owner，
  然后由原来的 QEMU 后端写盘。
- 读盘：原来的后端读入 owner 的私有缓冲区，RDMA 发给内存节点，内存节点通过
  `KVM_DSM_MEMCPY` 发布到 guest RAM，确认完成后才发布 virtqueue used 和通知 guest。
- 请求头、状态字也按相同规则访问。virtqueue ring 本身仍走已有 DSM 路径。

NUMA 归属只是路由提示，并不假定它等于当前 DSM owner。内存节点复制 guest RAM 时
必须经过 DSM 检查；页面已迁移时仍可能触发 DSM。本原型减少的是设备 owner 为
处理远端数据而逐页取得、长时间 pin 住 guest 页面的行为，不是取消一致性协议。

这是两端参与的 RDMA SEND/RECV 私有缓冲区协议，不是 NVMe-oF 协议，也没有直接把
guest RAM 注册为可远端写入的 MR。它包含本地复制，不宣称零复制。raw 的
`cache=none,aio=threads`、IOThread、flush 和完成语义保持原状。

## 代码入口

- `migration/gvm-private-io.c`：
  - `gvm_private_io_start`：独立连接、常驻注册缓冲区、2 MiB 内容回环检查。
  - `gvm_private_io_map`：识别 RAM memdev，远端分配私有缓冲区，本地返回原路径。
  - `gvm_private_io_prepare / complete`：按请求批量取出或发布数据。
  - `io_callv`：打包最多 240 段、2 MiB 数据。
  - `io_service / io_guest_copy`：在内存节点执行受 DSM 保护的复制。
  - `gvm_private_io_unmap`：检查发布状态，回收缓冲区。
- `hw/virtio/virtio.c`：virtio-blk 描述符 map、完整链解析完成、unmap 的接入点。
- `interrupt-router.c`：启动和停止独立数据通道。原控制消息通道未承载这些大块数据。

接收缓冲区必须在返回响应前重新提交，否则下一请求可能遭遇 RC RNR 重试。
发送完成和下一条接收完成可以交错，服务线程显式保存提前到达的接收完成。

## 启动

沿用原有 Garen node 0 / Ezreal node 1 启动参数，仅在两端 QEMU 环境中添加：

```sh
GVM_PRIVATE_IO=1
```

必须通过 `sudo -n env GVM_PRIVATE_IO=1 ...` 显式传递。不开此变量时使用原路径。
两端必须使用相同代码和协议版本。每个巨虚节点对应一个 NUMA RAM memdev，
当前验证配置为 4 vCPU、4 GiB，node 0 对应 CPU 0/1 与 n0，node 1 对应 CPU 2/3 与 n1。
HPET 不变，`-nic none`，两个 owner 各自挂原有 1 GiB raw 数据盘；
单盘使用 2 个本节点 worker，双盘同时使用 4 个 worker。

额外 RDMA CM 监听端口为 `41000 + local_node * 4 + peer_node`。
当前二节点对应 Garen 41001、Ezreal 41004。数据通道不复用 DSM 或原 QEMU 控制 RPC。

QMP 中可以读取 `/machine` 的 `gvm-private-io` 和 `gvm-private-io-stats` 属性。
stats 中 read/write 是相对于 guest RAM 的方向：
磁盘写对应 read_bytes，磁盘读对应 write_bytes。calls 包含启动回环。
开关为启动配置，不能在有请求时在线切换。

## 正确性与边界

- 实际四路并发检查：两块盘各由两个节点访问各自不重叠区域。
- 512 B、4 KiB、8 KiB、64 KiB、1 MiB、2 MiB，每路 18 个用例。
- 随机数据全内容校验；写完成后立即改写源缓冲区；fdatasync 后读回；
  再切换到另一节点 CPU、清空目的缓冲区并再次读回。
- 合并传输版本四路全部通过，合计 72 个用例；并发检查约 2.65 秒。
- 按请求独占私有缓冲区；RDMA 完成和 guest 数据发布均先于 guest I/O 完成。
- 长度、协议、请求序号、内存偏移检查失败或传输结果不确定时停止进程，
  不把错误伪装成成功，不盲目重发写操作。
- 私有映射总量上限 64 MiB/进程；每连接具有独立注册收发缓冲区。
- 原型未实现断线恢复、在线协议升级、迁移恢复；启用时阻止 QEMU migration。
  RAM memdev 拓扑须在运行期间保持不变。正常关闭后资源随进程退出回收。
- 已验证的是 x86 双节点 virtio-blk/raw 配置，不包含 VFIO、网络设备或跨节点缓存系统。

## 性能方法

所有结果单位 MiB/s。同一 guest、host 内核、raw 文件、队列与时钟配置：
guest O_DIRECT、host cache=none、1 MiB 请求、每 worker QD4。
主要矩阵每项 8 秒、3 轮取中位数，轮间轮换单盘/双盘顺序。
写性能包含末尾 fsync；不是每个请求执行一次 fsync。
读后抽样完整块校验在计时外，另有前述完整正确性检查。

本地聚合沿用历史口径：读完成后不在每轮额外遍历缓冲区。
跨机读每次完成后访问每个 4 KiB 页的一个字节，确保 guest 实际消费返回数据。
跨机写同时测重复使用固定内容，以及每次完成后重新写满缓冲区；
后者用于暴露 CPU 生产数据引起的 DSM 页面往返。

基准程序和日志在串口 stdin / 工具内存中传输，未创建新的测试盘、临时脚本或日志文件。
结果只代表此配置和这些顺序 I/O 工作负载，不是介质稳态或任意负载的性能保证。

## 结果

2026-10-13，代码数据路径为 `a57fa1e453`。原始 8 秒 × 3 轮矩阵：

| 本地访问 | 修改前 | 开启私有 RDMA 路径 | 变化 |
|---|---:|---:|---:|
| Garen 单盘读 | 1800.43 | 1824.54 | +1.34% |
| Ezreal 单盘读 | 1724.80 | 1714.72 | -0.58% |
| 双盘读 | 2950.67 | 2973.80 | +0.78% |
| Garen 单盘写 | 999.86 | 1002.27 | +0.24% |
| Ezreal 单盘写 | 981.88 | 925.70 | -5.72%，须进一步核验 |
| 双盘写 | 1937.86 | 1942.48 | +0.24% |

读聚合效率为 84.02%。按同期单盘中位数之和计算的写聚合效率为 100.75%，
略高于 100% 来自不同轮次的吞吐波动，不能解释成超线性收益。

Ezreal 单盘写首轮矩阵超过 5% 门槛，因此预先固定增加一组相同二进制的启动开关对照：
每项 15 秒、5 轮，比较全部样本中位数，不选择最好一轮：

- 开启：[965.36, 1019.25, 960.86, 959.64, 990.43]，中位数 **965.36**。
- 关闭：[994.71, 1023.18, 995.09, 993.17, 996.04]，中位数 **995.09**。
- 配对条件下中位数下降 **2.99%**；相对最初 981.88 的基线下降 **1.68%**。
- 开启时绝大多数本地数据未进入私有 RDMA 通道；观察窗口内该端私有传输仅约 5.1 MiB，
  而基准写入约 71 GiB。短轮次存在波动，不能声称每个单独样本都小于 5%。
- 不修改后端、NUMA、时钟、IOThread、QD 或缓存设置来取得此结果。

跨机访问（8 秒 × 3 轮中位数，单盘 2 worker；CPU 位于非设备 owner）：

| 工作负载 | Garen 磁盘：前 → 后 | Ezreal 磁盘：前 → 后 |
|---|---:|---:|
| 读后访问每个页面 | 150.48 → 1083.69 | 148.39 → 1080.64 |
| 写固定内容缓冲区 | 785.43 → 909.29 | 825.83 → 1011.06 |
| 每轮重新写满缓冲区后写盘 | 111.49 → 869.38 | 119.90 → 967.88 |

读提升约 7.2–7.3 倍，实际由 CPU 重新生产数据的写提升约 7.8–8.1 倍。
所有性能阶段均完成各自的块回读检查。完整并发随机数据校验见前述正确性部分。

本地写对照使用仓库原有 `scripts/gvm-device-guest-disk-bandwidth.py`：
SHA256 `f66b94b34f4ac12d07b59a97876c6ff775c3ae491db22e5e5ffea6e6a2e458aa`。
已登录串口后，可通过原有串口工具传入
`--owner 1 --seconds 15 --rounds 5 --skip-prefill`。
该脚本只允许序列号匹配的专用 1 GiB 测试盘；写测试会覆盖其内容。


## 数据路径计数核验

最终启用状态下，另外运行一次带短时 kprobe 的跨机读检查（性能数字不计入上面的无探针中位数）。
20 秒探针窗口覆盖一段约 8 秒的读取及准备/收尾：

- guest 实际完成 8,538,554,368 字节（约 7.95 GiB），并访问每个页面、完成块校验。
- Garen 设备 owner 的 DSM pin 总字节数为 871,568（不足 1 MiB），包含窗口内控制路径。
- Ezreal 的 `gvm-io-memory` 执行 DSM memcpy 共 8,547,081,668 字节；
  该线程在窗口内没有触发 `kvm_dsm_fetch`。
- 两端 vCPU、ring/控制路径仍有 DSM fetch，不能声称整个 I/O 过程完全没有 DSM。
- 这证明大块读数据没有通过设备 owner 的逐页 DSM pin 取得；
  guest RAM 的最终发布仍由内存节点的 DSM memcpy 保护。

探针已退出。最终重启巨虚后，四路随机读写/跨 CPU 读回检查再次全部通过（约 2.18 秒）。
两端运行相同 QEMU 源码；host KVM、guest 内核、HPET、raw 后端和 `-nic none` 保持原配置。
