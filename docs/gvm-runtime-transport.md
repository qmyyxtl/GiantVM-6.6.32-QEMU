# 巨虚在启动时选择 TCP / RDMA

本分支在同一份 QEMU 和 KVM 构建中支持两种传输。每个节点启动前指定
`method=tcp` 或 `method=rdma`；未指定时保持 RDMA。集群所有节点必须选择相同方法。
这是启动时选择，不支持运行中热切换。

## 选择覆盖哪些路径

- KVM DSM：`KVM_DSM_SET_TRANSPORT` 在 `KVM_DSM_ENABLE` 前设置 VM 的方法。
  `dsm_network_ops` 属于 VM，不再修改全局函数表。TCP 和 RDMA 使用各自的连接类型，
  通过类型明确的适配函数进入同一套 DSM 协议。已有 ENABLE ioctl 的结构和编号保持不变。
  新 QEMU 要求新版 KVM 支持该选择接口；不静默退回其他传输。
- QEMU 控制面：设备目录、PCI/MMIO/PIO、IPI 和中断等现有消息均使用选中的传输。
  TCP 保留独立的请求、响应连接及控制消息与普通设备处理线程的分离。
- 私有磁盘 I/O：TCP 使用独立 socket 和内存服务线程，复用 READV/WRITEV 请求、
  NUMA home 路由和 KVM_DSM_MEMCPY 一致性语义。
  RDMA 保留注册缓冲池及接收端 DSM lease + RDMA WRITE 的直接写入实现。
- TCP 没有 RDMA 单边写语义，`GVM_PRIVATE_IO_ZCOPY=send|direct` 在 TCP 下明确记录为 inactive，
  实际使用 mode=0。切回 RDMA 后同一设置重新生效。TCP 不创建 QP、MR 或打开 uverbs。
- QMP `qom-get /machine gvm-method` 显示选择的方法；私有 I/O stats 同时显示 method 和实际 mode。

TCP 帧处理限制长度、处理短读/短写、超时和 EOF。数据交付失败不发布成功的 guest I/O。
KVM TCP 仍沿用固定页大小的消息缓冲和有界响应缓存，尚未针对吞吐调优。
两种传输目前均面向可信集群，没有加密认证、故障恢复或热迁移协议。
固定端口仍限制同一主机/IP 上并行运行多个同类集群，per-VM 选择不等于端口隔离。

## 一次构建

两台主机使用同分支源码。以下示例路径为当前部署路径。

```bash
cd /home/xtl/GVM-kernel
scripts/config --enable KVM_DSM_TCP --enable KVM_DSM_RDMA
make olddefconfig
make -j192 modules_prepare
make -j192 M=arch/x86/kvm modules

cd /home/xtl/GVM-qemu
ninja -C build -j192 qemu-system-x86_64
```

QEMU 构建需包含 RDMA 支持和 libibverbs/librdmacm，以生成同时支持两种传输的二进制；
TCP 运行不要求 RDMA 网卡。KVM 双后端模块仍链接 RDMA core 模块。
若现有构建产物归 root 所有，构建命令需要 sudo。保持内核 release 为 `6.6.32-krdma+`。

先正常关闭所有使用 KVM 的 VM，再执行：

```bash
sudo /home/xtl/GVM-kernel/scripts/gvm-load-kvm.sh
```

脚本加载源码树中的 kvm.ko / kvm-intel.ko，不安装主机内核、不重启主机。
它检查构建配置、kernel release、/dev/kvm 占用，并设置 `enable_apicv=0`。
当前巨虚的 LAPIC 转发依赖这个设置；否则 BIOS 会等待收不到 INIT/SIPI 的远端 CPU。
这不是 TCP 专属要求。之后切换 method 不需要重新编译或重新加载模块。

## 两节点启动

`gvm-device-node.sh 0|1 method=tcp|rdma ...` 是基础脚本；
`GVM_METHOD=tcp|rdma` 也可指定，显式 method 参数优先。

`gvm-device-disk-node.sh` 封装当前双盘配置：4 vCPU、4 GiB、两个 NUMA 节点、
每节点两个本地 CPU，一个 owner 本地 IOThread、一块既有 raw 数据盘，
`cache=none,aio=threads`，MSI-X，HPET，`-nic none`。
它不会创建、格式化或挂载数据盘。

TCP 使用普通以太网地址。在两个终端分别执行：

Garen（master）：

```bash
sudo env GVM_IPLIST="10.0.16.104 10.0.16.91" \
  GVM_GUEST_KERNEL=/home/xtl/GVM-guest-kernel/arch/x86/boot/bzImage \
  GVM_QUIET=1 LOOM_RDMA_RECV_CQ_EVENTS=1 \
  numactl --cpunodebind=0 --membind=0 \
  /home/xtl/GVM-qemu/scripts/gvm-device-disk-node.sh 0 method=tcp
```

Ezreal（slave）：

```bash
sudo env GVM_IPLIST="10.0.16.104 10.0.16.91" \
  GVM_QUIET=1 LOOM_RDMA_RECV_CQ_EVENTS=1 \
  numactl --cpunodebind=0 --membind=0 \
  /home/xtl/GVM-qemu/scripts/gvm-device-disk-node.sh 1 method=tcp
```

切回 RDMA 时两边只把 method 改成 rdma；本部署还需将 GVM_IPLIST
换成 IB 地址 `172.0.1.4 172.0.1.2`。地址选取与传输类型是两个概念；
也可通过可达的 IPoIB 地址运行 TCP。

主串口：Garen localhost:5555；QMP：每台主机 localhost:4444。
根盘为既有 `/home/xtl/start/debian-12-40G.raw`，
数据盘为 `/home/xtl/start/gvm-device-data-node0.raw` / `node1.raw`。
可用 GVM_ROOT_IMAGE / GVM_DATA_IMAGE 覆盖路径。
两个节点需同时启动；TCP 建连与消息超时会停止失败的启动，不会自动换成 RDMA。

此前使用的 `loom-bar-miss-index` 是启动后 QMP 设置：

```json
{"execute":"qmp_capabilities"}
{"execute":"qom-set","arguments":{"path":"/machine","property":"loom-bar-miss-index","value":true}}
{"execute":"qom-get","arguments":{"path":"/machine","property":"gvm-method"}}
{"execute":"qom-get","arguments":{"path":"/machine","property":"gvm-private-io-stats"}}
```

## 验证与限制

本轮只验证功能，不做性能测试。普通以太网 TCP 的 DSM 延迟会拖慢 guest 启动；
不要仅因启动慢而重启主机或修改 HPET。首次验证出现 systemd 设备等待超时进入
emergency mode；继续等待后设备全部就绪，root 维护登录成功，随后恢复 default target。
具体完成状态见后续验证记录。

TCP 分帧单元测试不依赖 RDMA 硬件：

```bash
ninja -C build tests/unit/test-gvm-transport
build/tests/unit/test-gvm-transport
```

覆盖拆包、连续帧、超长/空帧、半帧断连和超时，6 项通过。
端到端使用已有 `scripts/gvm-device-guest-rdma-io-test.py --run-io`：
名字保留 RDMA，但数据校验也适用于 TCP。它会覆盖写入两块专用数据盘，
只应对序列号 GVM-DATA-OWNER0/1 的既有 1 GiB 实验盘执行。
不创建额外实验数据文件。
