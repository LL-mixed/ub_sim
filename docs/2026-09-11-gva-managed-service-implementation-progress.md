# 受管理 GVA 服务实施与验证记录

日期：2026-09-11。对应总计划 M1 与地址管理补充计划 AM1。

## 当前验收边界

本轮接通常驻 worker、真实 backing 的发布/正常释放、客户端可见性操作及双 guest
组合入口。64-byte 与 64 KiB CPU 双向共享及正常回收用例均已通过；后者在修复
QEMU 发送队列计数错误后通过。r20 另通过双方只读/guard page CPU 故障探针、
同代重新映射及连续两轮共享回收。AM1 权限/边界等完整矩阵仍待补齐，M1 和 AM1
均未完成整阶段验收。
CPU/PTO 联合访问、安全重用、恢复、多 home 和 W5 新路径
尚未获得本轮通过证据。禁止以历史 W5 结果代替这些验收。
服务的 import_mappings 仍缺少实际客户端/provider 映射上报；当前为零不能证明
数据面不存在映射。allocator 高水位、可重用地址与对象 live reservation 也需独立
计量，禁止将单调分配策略下的对象计数清零解读为已具备地址空间循环重用。
object-session 的本地 holder 跟踪仍只覆盖当前 view；跨 key 操作可能丢失本地
泄漏提示，服务端实际引用仍保留。M1.4 需补齐多 key/session 跟踪及中途退出负例，
当前正例通过显式 release 和最终服务统计验证回收，不能推广为所有序列均无泄漏。

r20 实验使用提交前的隔离开发快照与预构建 initramfs，未放宽正式构建的源码
锁定检查，不能作为发布认证。总计划修订 4 已移除 SSD 交付，本记录的后续任务
按该范围执行，不改变历史实验结果。

用户随后授权本地提交并继续实现。已验证的平台与服务改动分别提交在
`codex/gva-managed-service-completion` 分支，尚未推送：

| 仓库 | 本地提交 |
| --- | --- |
| mem_service | `3fde02f135fa8f066089f5e6b1d7505f96dc64de` |
| kernel | `021a7dc9111aac81a9f1586ad093dd72840de8ec` |
| QEMU | `43d078eabc12cbb8dc8bdd8d5e433392e8d869b6` |

根仓库 gitlink 与 mem_service.lock 保持原值，待远端可获取后再成对更新。本次
根仓库提交是开发检查点，不能凭旧 pin 直接复现新组合；构建仍应保留源码锁门槛。

## 本轮实现

- gva_manager 共享库补充 unexport：核对真实 segment、export 身份及范围后调用
  内核；retire 保持独立步骤，调用者负责先排空 holder。
- OBMM worker 保存每项 reservation 的 key/generation、segment、export 与 opaque
  descriptor。正常释放依次记录意图、unexport、retire、reclaim 确认；失败保留状态。
  能力输出明确单 home、禁用地址重用、强制撤销未支持、恢复待核对。
- worker 显式接受部署的 allocation_granularity_bytes。当前 guest pool 要求
  2 MiB；逻辑对象大小不变，backing 向上对齐，SDK 数据操作仍受逻辑大小约束，
  物理占用统计使用发布的 backing 地址跨度。
- ALLOCATING 取消进入 RETIRING，迟到 publish 登记待清理 reservation，保持禁止
  acquire；空 reservation 也须由当前 home worker 明确确认取消。
- object-session 增加 publish_data/wait_visible，复用进程内中立 provider 接口，
  没有新增 payload 控制 RPC。OBMM session 的独立 endpoint 先验证 peer canary。
- 原双节点 apps runner 新增配置文件读取，并使用真实 worker 自动创建/释放对象。
  A 写 B 读、B 写回 A 校验通过 managed 对象状态排序；不人工填 descriptor。
- 修复 daemon 对客户端提前断开触发的 SIGPIPE 处理，避免单个失败回复终止服务。

## 已完成的定向验证

所有 Linux 执行、集成及 QEMU 工作位于 n4-910c1；本机只运行轻量交叉编译、静态
检查和不加载模型的定向测试。

| 检查 | 结果与边界 |
| --- | --- |
| gva_manager 共享库 | 远端 7/7；包含真实 ioctl 包装测试，不能替代 guest 数据面 |
| 双 guest 地址分配器 r16 | 双方均完成 11 项 allocator 检查与 segment 分配/查询/退役，runner PASS；使用保存的 kernel r9 / QEMU r12，未覆盖新 UNMAP V2 |
| provider-backed allocation 服务测试 | 最新取消/迟到发布及容量统计版 16/16，136.779 秒 |
| 逻辑大小与 padding、物理占用统计 | 远端 2/2，17.151 秒 |
| SIGPIPE 断连与 allocation 操作 | 远端 8/8，68.543 秒 |
| worker 构建、事务与启动保护 | 最新版本远端 4/4，89.883 秒；完整 worker 循环覆盖 refresh 字段回归 |
| 本机 manager/worker 与 GSVA wire/CPU/接收端 | 最新组合 14 项中 12 项通过、2 项 Linux-only 跳过，4.584 秒；无模型/QEMU 执行 |
| OBMM provider 最新定向套件 | 远端 7/7，7.108 秒；含双 token 导入与可见性边界包装执行 |
| 对齐占用、隔离和未发布取消统计 | r9 远端 5/5，67.052 秒；覆盖完整生命周期及被杀客户端 holder 保留 |
| 逻辑映射范围与三 session 编排 | r10 远端 3/3，86.323 秒；barrier 取消由 home 明确确认 |
| release verifier 原预算重测 | r10 远端 1/1，19.220 秒（含 class 编译）；保留子进程原有 10 秒预算 |
| mem_service 第一轮全量 | 204 项：198 通过、4 跳过、1 failure、1 error；不能计为通过 |
| mem_service 第二轮全量 | 207 项：195 通过、4 跳过、7 failures、1 error；3250.802 秒；不能计为通过 |
| mem_service 第三轮全量 | 208 项：204 通过、4 环境跳过，1479.438 秒；冻结的 r10 服务快照通过 |
| mem_service 最新全量 r17 | 209 项：205 通过、4 环境跳过，2773.130 秒，零失败；冻结的 r15 功能源码，覆盖新增 CPU 探针和逻辑映射保护 |
| ub_sim guest unittest 全量 r14 | 427 项：411 通过、5 跳过、4 failures、7 errors，93.434 秒；未通过。源码锁检查明确拒绝未提交的 mem_service，W5 bootstrap 随之中止 |
| ub_sim guest unittest 全量 r19 | 429 项：413 通过、5 跳过、4 failures、7 errors，348.591 秒；新增 view-release 定向测试通过，剩余失败仍为源码锁门槛及依赖该门槛的 W5 入口 |
| guest app layout pytest r15 | 74 项：70 通过、4 失败，1.36 秒；新增共享探针/runner 检查通过。基线已有 app 清单遗漏、package checksum 黄金值过期、docs/demo 命名冲突；不能计作全量通过 |
| guest app layout pytest r19 | 74/74，1.14 秒；补齐 obmm_test 与 PTO app 的现有矩阵入口，按 c042d66 锁定契约更新黄金值，将已有图改名为 validation 并同步引用；图内容 SHA256 不变 |
| 新 QEMU 配套单测 | 31/31：OBMM remote 6、remote model 7、void response 5、async load 11、bridge lock 2 |
| 新 kernel r16 与 helper | 完整 Image、modules_prepare 和匹配的 linqu_ub_drv.ko 构建通过 |
| 双 guest 最终组合 r20 | runner 1/1 PASS；两轮 64 KiB 双向共享、双方各两次 readonly/guard 探针、同代重映射、6 个对象真实回收；远端读/写错误及超时均为 0 |

第一轮全量失败包括源码包中的 macOS `._*` 附属文件影响 vendored aggregate，
以及断连触发 SIGPIPE。已重打无附属文件快照并修复 SIGPIPE。
第二轮失败包括：四项旧物理占用断言、两项把 backing 大小作为逻辑映射范围的
用例、三 session 编排遗漏未发布 barrier 的 home 取消确认；另有 release verifier
超过原有 10 秒预算。占用相关回归已由 r9 定向测试通过，
逻辑范围、编排及 release verifier 已由 r10 定向测试通过；第三轮全量通过。
保留原有超时预算、校验值和失败日志。

r17 使用容器内显式可执行的临时文件系统运行全部服务测试。此前 r15 在迁移慢速
构建输出时停止，r16 因临时挂载默认 noexec 而停止；这两次未完成运行不计作通过。
r17 运行期间功能源码保持冻结；之后仅补充 provider 验证记录及修正集成文档的
MEM_SERVICE_ROOT 默认路径，没有修改已测试的功能代码。

## Guest 失败记录及修复依据

| Run ID | 观察结果 | 处理 |
| --- | --- | --- |
| managed-share-20260911-r1/r2 | 两端基础 OBMM conformance 通过；64 KiB canary 被 2 MiB pool 粒度拒绝；nodeA 自连接超时 | 显式 backing 粒度与 canary 大小；启用 initramfs loopback |
| managed-share-20260911-r3/r4 | 两端独立 session canary 通过，主对象进入 ALLOCATING；worker 没有取任务，随后出现 timer softirq soft lockup | 发现 refresh 回复字段使用错误；修正 worker 以 provider_directory_ready 判断目录；soft lockup 继续独立排查 |
| managed-share-20260911-r6 | worker 实际分配/export 两个对象；A 同址映射、写入并发布 64 KiB；B 同址映射后同步返回 ENOENT | GSVA map ID 被错误传入 legacy SIM_DEC_SYNC；CPU 双向共享仍未通过 |
| managed-share-20260911-r8 | 新 GSVA V2 导入及 acquire/fence 完成；B 的严格远端读持续返回成功，但 A 的 timer softirq 出现持续 soft lockup；runner 超时，0/1 | 保留原失败；检查长时间接收负载下的 guest 与 QEMU 调度 |
| managed-share-20260911-r9-small | 64-byte 对象，双方同 VA 双向写读/校验成功，3 个对象真实 unexport/retire/reclaim，最终 live refs/in-flight 为 0；runner 1/1 | 使用与 r8 相同产物，缩小数据范围作为诊断；不能替代 64 KiB 和完整 AM1 验收 |
| managed-share-20260911-r10-profile / r11 | 64 KiB 继续超时；r11 的每轮 RX 64 帧上限没有消除风暴；A 普通数据帧累计 35,193,096，严格 I/O 帧 59,483 | 调用栈和计数定位发送队列无限重放，r12 修复等待 guest 重测 |
| managed-share-20260911-r12 | 64 KiB 双向 CPU 数据、可见性、3 个真实对象回收及内核健康检查通过；runner 1/1，reclaim_ok_count=3 | 验证累计 SQ CI 修复；仍使用 r8 initramfs，需最新版服务产物及 AM1 负例补测 |
| managed-share-20260911-r13 | 最新 r10 服务快照，两轮 64 KiB 共享，6 个对象实际回收；runner 1/1，远端读写/超时错误均为 0 | 同一基础设施连续接受新 session；不等于同代对象重新映射通过 |
| managed-share-20260911-r14 | 逻辑视图保护在首次 map 失败；guest 明确输出 OBMM mprotect not supported；runner 0/1 | native 匿名映射测试不能替代设备映射；改为初始建立独立 view/guard VMA，保留 OBMM 禁止拆分的约束 |
| managed-view-old-20260911-r18 | 新 provider/CLI 配旧 kernel r9/QEMU r12：A/B readonly CPU 探针均 SIGSEGV，A guard offset=65536 为 SIGSEGV；B 第二次 map 被 stale epoch tombstone 拒绝，整体失败 | 真实复现旧 unmap 退役语义问题；只计保护探针的局部证据，不计共享用例通过 |
| managed-share-20260911-r20 | 新 kernel/QEMU/provider/CLI 组合，两轮共享和页保护通过；B 的 4 次 GSVA V2 unimport 成功，重映射同代对象成功；reclaim_ok_count=6，runner 1/1 | 验证 view release 与对象 retire 分离；正常释放后服务 live refs/in-flight 为 0。import 实际计数和 aperture 高水位仍缺独立统计，不能据此关闭完整回收验收 |

## 视图保护与解除映射的实现和验证

- SDK 将逻辑 view 长度与完整 backing 分开传递。完整 backing 区间内，逻辑视图外的
  完整页使用独立 PROT_NONE VMA；末页 padding 只受 SDK 字节检查。r14 的 mprotect
  方案被真实 OBMM 拒绝，失败保留；修正后的 provider 定向测试 r15 为 7/7、6.252 秒，
  包含真实 CPU 页故障以及碰撞回滚保留既有 VMA。
- CLI 新增 probe_readonly/probe_guard，在原映射进程捕获目标地址的同步故障后恢复
  信号处理器。OBMM VMA 带 VM_DONTCOPY，不能用 fork 后的缺失映射证明权限。
  CLI 与 provider 首轮定向组合 9/9、23.384 秒；r20 又在双方真实 OBMM 映射上
  验证 readonly 与 guard（offset=65536）均产生 SIGSEGV。
- 发现旧 GSVA unmap 会退役 coherence 并留下 tombstone，阻止同代对象再次导入。
  新增内部 UNMAP version=2，专用于 managed view 解除；完成 fence、coherence/route
  删除、TLB 刷新后释放视图。Retire 事件及旧 version=1 保留 tombstone 语义。
  内核收到 GSVA 失败后不再调用 legacy unmap。独立生产函数包装测试 2/2、0.726 秒；
  r20 的新组合通过真实 guest 同代重映射，并继续完成双向 payload 校验。
- r14 打包检查另发现更新后的 run_app 被复制到 rootfs 根目录，boot 实际使用 bin/run_app。
  r14 尚未执行新增 CPU 探针。后续包显式覆盖 bin/run_app，并由 runner 强制检查双方
  cpu_fault_probe 证据，避免脚本遗漏被旧流程成功掩盖。
- 新 QEMU r16 已构建，配套单测 31/31。SHA256 为
  `75851f6a998944151f627559d239535dec398cdfa37a2b159f4951d7268d620e`。
  kernel r16 完整 Image 与匹配的 linqu_ub_drv.ko 已构建通过；r15/r16 慢速构建的
  中止与切换日志均保留。r20 使用独立版本产物，旧 Image/QEMU/initramfs 保留。

| r20 固定产物 | SHA-256 |
| --- | --- |
| Kernel Image r16 | `72b9d86751c35fb087fdd75b711aedbce1bb0b5c2aaabe697846e664cffa627a` |
| linqu_ub_drv.ko | `130e1f11a9588cba4a0db5ae4e0060688af6a395e29b138b6d5422009f5ee329` |
| managed-share-r20.cpio.gz | `5ee4fdf022498f6e8267be1569d27be90ea5c3787ebc56e13137878ebaddd815` |
| bin/run_app | `1a3029d20e9dab86292212374ea25bbfdc81813c3429532627286095782d6b8b` |

打包时逐字核对 bin/run_app 与源码。r14 遗留在 rootfs 根目录的无效 run_app 副本
移动到实验目录的 run_app-unused-root-copy-r14，保留可恢复副本；旧镜像未修改。

provider-refresh 不返回 data_plane_ready；此前 worker 检查该字段会一直等待。
修复没有更改服务整体 readiness 语义：控制进程的本地 data plane 与 provider 目录
分别报告。完整 worker 循环回归专门覆盖 directory_ready=true、data_plane_ready=false。

### r6 暴露的跨层接口缺口

opaque descriptor 已分别携带 GSVA lease token 与 OBMM export token，但现有
`obmm_do_import_gsva_desc_v1` 到 QEMU CPU window 的路径只保留一个 token ID。
真实分配中 GSVA token 为 2，export token 为 97，不能互换。后续补齐必须遵守：

- 使用版本化的 provider 私有导入描述和映射请求，保留旧版本长度与字段语义；旧端
  明确拒绝新版本。真实 GSVA lease 与物理 export 身份均须保留、分别校验。
- GSVA range 可见性进入 GSVA fence/acquire 路径，legacy SIM_DEC shadow-window
  同步只用于相应旧映射。任何 fence、token、range 或 checksum 错误必须失败。
- 新导入路径不得通过重写 segment token 来凑成 export token，不得在同步失败后
  自动改用旧映射、地址或传输。home backing 的发布与访问授权仍须按真实身份核对。

为排除历史增量产物问题，另在全新输出目录构建相同内核源码与外部 helper module。
缺失的 flex/bison 等构建依赖装在独立 Docker image 中，宿主环境未修改。

干净基线的 Image 与 helper module 已构建完成，保留在实验目录的
`kernel-clean-baseline/`。新增 import v4、显式导入标志和 GSVA_MAP_V2 的内核已
构建通过（r8）；r7 编译发现的两处私有指针类型错误已修正，失败日志保留。
QEMU 首次构建因缺少 glib 开发依赖中断，已在专用镜像补齐。r9 构建完成且配套
31 项单测通过；内核 r9 进一步显式清零新报文的保留 padding，构建通过。
v2 报文布局编译检查通过：旧 QEMU payload 为 140 bytes，旧内核
结构含尾部 padding 为 144 bytes；新报文双方均为 152 bytes，export token 位于
offset 144。旧 opcode 与旧布局保持原值。

QEMU 新 CPU 路径使用版本化的严格 I/O 消息，沿用既有 batch carrier，读写均等待
对端结果；DMA 失败不进入 legacy 的其他 route/raw-address 回退。接收端生产函数
的轻量包装执行测试通过，覆盖读写、DMA 错误、版本/长度/保留字段和 token 拒绝。
这项测试没有启动 QEMU，无法替代真实 UMMU 翻译、home GSVA 身份校验及 guest
访问异常的验收。provider 双 token/事件错误包装用例在远端通过（12.447 秒），
后续新增的操作前范围检查已由远端 r8 的 7/7 provider 定向套件覆盖。

### 新组合的 CPU 共享实验

`managed-share-20260911-r8` 使用 `managed-share-r8.env`，结果为超时失败。
`managed-share-20260911-r9-small` 使用相同产物，64-byte 诊断用例通过。
该用例主对象 VA 为 `0x700000000000`，初始与反向 checksum 分别为
`0xdb21843fe12f9283`、`0x4b5d778e14829283`，`reclaim_ok_count=3`。
本轮固定产物 SHA-256：

| 产物 | SHA-256 |
| --- | --- |
| Kernel Image（private import v4、GSVA_MAP_V2、显式 padding） | `089eb18296db2f0c8747f7983b07bde47666712781d4b2cb0b188f952c4c2b5f` |
| managed-share-r8.cpio.gz | `be9cff8c11bcffbffa5791774708ef5244df9c53dd4cf647706db8bad42e3483` |
| QEMU（fd046b39 基线上的双 token、严格 I/O 与 CPU fault 回传） | `70d530da1351f0da01106ce875ffba15506ce0a5f9b1cda4e6a402eab5c469b3` |

initramfs 固定于 r8；后续 service 隔离/取消占用统计修正和 QEMU 严格写操作的统计
分类修正未进入此轮产物，须由后续重建补测。第三轮 service 全量通过，使用独立的
`service-tests-r10/mem_service` 快照，执行期间源文件未变化。

### 长运行的发送队列计数错误

r10/r11 的 QEMU 调用栈显示主事件线程收包时持有 BQL，vCPU 等待锁；另有 LPI
扫描开销。r11 添加每轮最多 64 帧的接收预算，仍出现 soft lockup。随后采样确认
180 秒内有约 3,519 万普通数据帧，包含被重复使用的异常 TCP SYN。

根因已由实际驱动与 QEMU 源码对应确认：`udma_post_one_wr` 累计增加 `sq->pi`，
doorbell 携带累计 PI；`ubc_process_sq` 却对 CI 取模。PI 超过队列深度后，
`while (ci != pi)` 无法结束并反复执行旧 WQE。r12 保持 CI 累计，仅 DMA 槽位取模，
同时拒绝超过一圈的提交距离和无效深度。生产函数执行测试覆盖 32 圈、32 位计数
溢出、重复/陈旧 doorbell 与过量提交；与 GSVA wire 套件合计 5/5，1.934 秒。
这项修复已由 r12 的 64 KiB 双向共享与回收通过验证。多 WQEBB 请求跨度、跨环 SGE 和完整多 SGE 发送
仍需单独核对，不能把单槽计数修正描述为完整发送队列认证。

诊断仅附加本次实验的明确 QEMU PID，未修改宿主 ptrace 策略，未停止其他任务。
`qemu-profile-r10-*.log`、`qemu-packet-r11.log` 与 `perf-r11-nodeA.*` 保留原始证据。

## 实验位置与复现约束

远端工作目录为
`n4-910c1:/home/ll/gva-manager-integration/reclaim-20260911.BGbjiB`。
`share-r*.log` 为 runner 输出，guest/QEMU 日志位于该目录的
`guest-linux/aarch64/logs/managed-share-20260911-r*_apps_iter1/`。
`service-suite.log` 保留第一轮失败，`service-suite-r5.log` 保留第二轮全量失败日志。
`out/gva-manager-integration/managed-share.env` 记录实际配置，
`managed-share-r*.cpio.gz` 保留各轮 initramfs。

实际入口为既有 `run_ub_dual_node_apps.sh --config <配置> --app mem_service_object_share`。
实验通过预构建产物参数运行；正式源码消费仍要求 clean、tested、remotely fetchable
的 mem_service commit 和一致的 gitlink/lock。独立开发快照结果不得改写为已发布。

r20 两端 guest、runner、helper build、QEMU 单测及 r19 根仓库回归日志已复制为本地
生成证据包 `out/gva-manager-integration/verified-r20-evidence-complete.tar.gz`，SHA-256
为 `3d04fe556463e06411ddaa37d24f998ce2384dfe3ca7d2be3eaf0a89a7f61a97`。
该包不包含体积较大的 QEMU 原始 trace；原始 trace 保留在远端实验目录。
最新服务全量日志为 `out/gva-manager-integration/service-suite-r17.log`，SHA-256 为
`9625a792e7efe94d00723554835de2d3348b14ce20b9b375599ee41ba2c69032`。
4 个跳过项分别为 arm64 deb 工具链检查，以及独立服务仓库不含 ub_sim 文件的
3 项下游契约检查；不能将跳过项视为对应环境已经验证。

## 下一批的前置条件与未完成项

本地提交已获授权并完成。正式源码锁验收仍需子仓库提交可从远端获取，再同时
更新父仓库 gitlink/lock；当前尚无 push 授权。历史 r19 校验器明确拒绝 dirty
mem_service；提交后旧 lock 与新 checkout 的 revision 不一致同样必须拒绝。
本轮未增加绕过开关，未修改父仓库 pin，也未把已知失败计作通过。

后续实现仍包括 M1.4 的完整失败矩阵和实际映射计量，以及 AM2 的 CPU/PTO
联合访问、AM3 的恢复/重用、AM4 的多 home/W5/独立
发布。总计划 M2-M5 对应的动态身份、恢复、压力与 replay 联合失效同样未关闭。
正式源码锁恢复通过后，仍需逐项实现和运行验收，不能由 r20 的 CPU 正例推定完成。
