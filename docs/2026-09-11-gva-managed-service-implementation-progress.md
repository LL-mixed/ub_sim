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
受管理 SDK 与 object-session 已接通实际映射的事务上报；独立客户端存活期间计数
为 1、提前 release 被拒绝、清理后归零的远端进程回归已通过（见本记录末尾续作）。
raw mapping 入口和 kernel 实际资源枚举仍不在该计量内，零值无法独立证明整个
数据面不存在映射。allocator 高水位、可重用地址与对象 live reservation 也需独立
计量，禁止将单调分配策略下的对象计数清零解读为已具备地址空间循环重用。
r45 已增加 OBMM endpoint 资源快照与双 guest 存活期采样，核对本进程持有的
export/import、VMA、逻辑视图及清理残留；该诊断不提供 kernel 全局资源枚举。
r46/r48 续作增加已注册 OBMM region 的 sysfs 枚举命令与基线比较。r48 双 guest
失败于 nodeB QEMU 堆损坏退出；r52 挂接调试器后完整运行通过，双方最终枚举回到
基线。优化版内存检查构建 r51 已成功；r53 在真实双 guest 路径捕获 GSVA route
释放后被 RCU 清理线程访问的完整 ASan 调用栈。r55 另定位关机阶段访问已释放
MigrationState 的缺陷，两处修复已进入 r56 构建并通过 35 项相关 QEMU 单测。
r57 双 guest 未再报告这两处释放后访问，但在 60 秒回写屏障超时。r59 同源正常
优化构建完成后，r60 保持 64 KiB/两轮/原预算通过，双方实际资源检查回到基线；
r61 的 64-byte ASan/UBSan 生命周期回归也完成两轮和关机，没有新的错误报告。
这些结果支持检查开销影响等待预算的解释；不据此认定 r48 堆损坏的唯一根因。
详见本记录末尾，M1/AM1 仍未完成。
r71 补齐双方实际固定 VA 冲突拒绝、原映射保留和后续完整读写/回收；r72 用同一
镜像通过 36 次 SDK 预期拒绝与八次幂等重放。r78/r79 进一步通过 SDK 不可变绑定
核对、116 次绑定变异拒绝及原 SDK 回归。provider/kernel 独立身份保护、容量与
完整失败矩阵仍有未完成项，M1/AM1 保持未完成。
object-session 已补齐按 key/generation/session 跟踪本进程确认的 holder，跨 key
查询不再隐藏未释放引用；正常结束和操作失败退出均报告已知 holder。旧 acquire/
release 应答重放不重复改变本地持有状态。该记录仅用于 CLI 操作约束和泄漏诊断，
服务端仍为引用权威；跨进程恢复、kernel 资源核对和完整失败矩阵仍待实现与验收。

r20 实验使用提交前的隔离开发快照与预构建 initramfs，未放宽正式构建的源码
锁定检查，不能作为发布认证。总计划修订 4 已移除 SSD 交付，本记录的后续任务
按该范围执行，不改变历史实验结果。

用户随后授权本地提交并继续实现。已验证的平台与服务改动分别提交在
`codex/gva-managed-service-completion` 分支，尚未推送：

| 仓库 | 本地提交 |
| --- | --- |
| mem_service 首批 | `3fde02f135fa8f066089f5e6b1d7505f96dc64de` |
| mem_service holder 续作 | `aa539204a402395df31175eeaa95989b8ad90f20` |
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
| mem_service 全量 r17 | 209 项：205 通过、4 环境跳过，2773.130 秒，零失败；冻结的 r15 功能源码，覆盖新增 CPU 探针和逻辑映射保护 |
| mem_service 最新全量 r23/r24 | 215 项：211 通过、4 环境跳过，零失败；全部 13 个模块分 4 个隔离组运行，包含新增 6 项 holder 回归 |
| ub_sim guest unittest 全量 r14 | 427 项：411 通过、5 跳过、4 failures、7 errors，93.434 秒；未通过。源码锁检查明确拒绝未提交的 mem_service，W5 bootstrap 随之中止 |
| ub_sim guest unittest 全量 r19 | 429 项：413 通过、5 跳过、4 failures、7 errors，348.591 秒；新增 view-release 定向测试通过，剩余失败仍为源码锁门槛及依赖该门槛的 W5 入口 |
| guest app layout pytest r15 | 74 项：70 通过、4 失败，1.36 秒；新增共享探针/runner 检查通过。基线已有 app 清单遗漏、package checksum 黄金值过期、docs/demo 命名冲突；不能计作全量通过 |
| guest app layout pytest r19 | 74/74，1.14 秒；补齐 obmm_test 与 PTO app 的现有矩阵入口，按 c042d66 锁定契约更新黄金值，将已有图改名为 validation 并同步引用；图内容 SHA256 不变 |
| 新 QEMU 配套单测 | 31/31：OBMM remote 6、remote model 7、void response 5、async load 11、bridge lock 2 |
| 新 kernel r16 与 helper | 完整 Image、modules_prepare 和匹配的 linqu_ub_drv.ko 构建通过 |
| 双 guest 组合 r20 | runner 1/1 PASS；两轮 64 KiB 双向共享、双方各两次 readonly/guard 探针、同代重映射、6 个对象真实回收；远端读/写错误及超时均为 0 |
| 双 guest 最新组合 r24 | runner 1/1 PASS；新 holder 版本重复通过 r20 的两轮共享、保护探针、同代重映射和 6 次回收；远端读/写错误及超时均为 0 |

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

## M1.4 续作：CLI holder 跟踪与 r24 双 guest 验证

首批根仓库开发检查点为 `be4acc23cedbcd59007653416b2fd6825c902050`，包含本总计划
的内存 backing 范围修订、guest 集成和既有验证记录；不包含未获授权的无关修改。
随后继续修复 object-session 的本地持有状态：

- 按 key/generation/session 保存所有本进程成功确认的引用，查询其他对象不清除
  记录；为其他 session 执行 acquire 不授予当前 session 映射资格。
- 活跃映射独立保存 backing 长度，readonly/guard 探针按该映射及其 holder 核对，
  不依赖最后一次 inspect 选择的对象。当前仍只允许一个活跃映射。
- 对本进程已经处理的成功 acquire/release 幂等键记账。旧 acquire 重放不恢复
  已释放的本地资格；旧 release 重放不移除后来重新获得的引用。
- 正常结束及操作失败退出均列出全部已知未释放 holder。仍要求显式 unmap/release，
  不在异常退出时推测释放安全性，不代替服务端引用记录或跨进程恢复协议。

新增 6 项 CLI/daemon 真实子进程回归。旧提交在 r22 暴露 5 项行为失败；另 1 项
测试最初把映射参数写成 `read,write`，修正为现有 CLI 接受的 `readwrite` 后，
r24 单独回放旧提交仍失败于跨 key 查询后 probe 的 `readable_mapping_required`。
修复版完整 object-session 组 36/36 通过，包含上述全部 6 项；未放宽断言或预算。

服务套件在远端按模块分组执行，各组使用独立容器及可执行 tmpfs，入口均为
`python3 -m unittest -v <模块列表>`。覆盖 `tests/` 全部 13 个 test 模块：

- runtime：`tests.test_mem_service_daemon_runtime`。
- session：`tests.test_mem_service_object_session`。
- control：`tests.test_mem_service_allocation_ops`、
  `tests.test_mem_service_provider_backed_allocation`、
  `tests.test_mem_service_provider_directory`、`tests.test_mem_service_network_control`。
- providers：`tests.test_mem_service_record_recycling`、`tests.test_mem_service_obmm_provider`、
  `tests.test_mem_service_tcp_provider`、`tests.test_mem_service_roce_provider`、
  `tests.test_mem_service_terminal_token_visibility`、`tests.test_mem_service_kv_reserved`、
  `tests.test_repository_contract`。

功能源码在各组执行期间保持冻结；session 组使用修正测试参数后的 r24 快照，
其余三组使用相同功能源码的 r23 快照。r22 串行运行在切换隔离并行组时中止，
r23 session 组在发现上述测试参数错误后中止，均不计作通过。

| 隔离组 | 结果 | 用时 |
| --- | --- | --- |
| runtime r23 | 76 项：75 通过、1 工具链跳过 | 866.109 秒 |
| session r24 | 36/36 通过 | 319.142 秒 |
| control r23 | 40/40 通过 | 358.468 秒 |
| providers r23 | 63 项：60 通过、3 下游环境跳过 | 23.881 秒 |

合计 215 项、211 通过、4 跳过、零失败。各组并行且起始时间不同，上表耗时不相加
作为总体墙钟时间。本机 `cc -fsyntax-only -Wall -Wextra` 和 `git diff --check`
均通过。根仓库全量仍保留 r19 的源码锁失败记录，本轮未把服务套件通过写成根仓库
全量通过。原有兼容测试随完整服务套件执行，不扩展总计划的内存 backing 范围。

r24 通过项目 Makefile 重建启用 OBMM 的 CLI，使用独立 rootfs/initramfs 运行
`run_ub_dual_node_apps.sh --config <managed-share-r24.env> --app mem_service_object_share`。
复用 r20 已核验的 kernel r16、helper 和 QEMU r16，runner 1/1 PASS：两轮 64 KiB
双向 payload 校验、双方 readonly/guard CPU 探针、同代重映射和 6 次真实回收均通过。
最终 live_objects/backing_allocated_bytes/address_reserved_bytes/live_refs/in_flight/
quarantined_objects/quarantined_bytes 均为 0。节点 B 累计 remote_reads=1048794、
remote_writes=131092，read_timeouts/read_errors/write_errors 均为 0。
这些计数证明该用例通过，不能替代尚未实现的实际 import 映射统计和安全地址重用验收。

| r24 续作源码与产物 | SHA-256 |
| --- | --- |
| apps/mem_service/mem_service.c | `b79c9f09958f07767b97bf66f0343b469494c09c43388128f18961a2f75b29df` |
| tests/test_mem_service_object_session.py | `af29ccbf0f3fc999eaefab632375c5e5b44f9c4579161faf28ebd792c8ad96bc` |
| linqu_mem_service-r24 | `1f1ec7b5ae5ee98ed5412dbb8cb56901b63f436d347f62b2cb7997463893527a` |
| managed-share-r24.cpio.gz | `c3bc1dfee33c26b562b46d3e048b3e25b65d66cd4e93c1d5d9b97caf4fe6caba` |

## M1.4 续作：映射事务控制面与 r34 完整回归

在 holder 跟踪提交之后，新增 `mapping-transition` CLI、typed SDK 与 wire `0x7d`。
core 保存独立 mapping ID、对象 generation、holder session 和 pending/active/closing
状态，尚存事务阻止 holder release；统计区分已确认 import 与未完成事务。
每个 holder 可持有多个 mapping ID，ID 在当前服务生命周期内不复用。该控制接口
不执行 provider import/unmap，现有 object-session 自动接线与真实 guest 计量尚未完成，
不能将诊断命令上报的数量作为 kernel 资源快照或重启恢复证据。

远端专用目录为 `n4-910c1:/home/ll/gva-manager-integration/mapping-20260911.UkR8At`。
r25 的 4 项控制事务回归通过（34.557 秒）；r27 改为 CLI 经 typed SDK 调用，新增
readiness 丢失后清理测试，5/5 通过（43.561 秒）。r28 的 schema、release、package、
upgrade/rollback、compat、old/new compatibility、API/ABI 共 7 个契约入口通过。
新 schema 为 38 operations、216 fields、16730 bytes，校验值 `0x62fe2d5d`；
全部关联 manifest 由项目 print 目标生成，并核对传递依赖校验值。

r25 构建暴露 schema 超过原 16384-byte 渲染缓冲；扩大到 32768 后 r26 得到实际
长度与校验值。r27 保留关联契约旧校验值时仍失败，按依赖顺序更新后 r28 通过。
另补齐 release manifest 遗漏的 managed 操作，并新增与 schema 操作集合一致性测试。

r29 完整套件启动后，新增加的 home incarnation 替换回归在冻结的 r29 源码上失败：
旧对象获得 mapping_id=2 的 pending 事务（`mapping-red-r30.log`，1 failure，8.796 秒）。
修正为 begin/confirm 在幂等重放前与执行前均核对对象绑定的 home incarnation；
已有事务的 inspect/close/finish/cancel 保持可用。r29 providers 组 63 项中 60 通过、
3 环境跳过（24.123 秒）；其余三个组因上述明确的代码修正停止，不计作全量通过。
r30 定向回放上述 home 替换负例通过（8.699 秒），并验证旧 begin 幂等应答无法
绕过新准入检查。r30 完整套件共 222 项，216 通过、4 环境跳过、2 errors；
日志为 `service-suite-r30-<组名>.log`。session 43/43（402.322 秒）、control
40/40（376.019 秒）；providers 60 通过、3 跳过（23.789 秒）；runtime
73 通过、1 跳过、2 errors（886.620 秒）。本轮未通过，未据此提交。
冻结源码位于 `/tmp/gva-mapping-r30.4jcors`；daemon SHA-256 为
`f8a4cc91c947c71aa89f2b97d301a20083b4427400d150bd4fa579dfb85a2de5`，
新测试模块 SHA-256 为
`c810d39c49f6c5970fc4f3fd32fcb1899b5c223cabe59e2a35e33ee55339bb19`，
均已与当时本机待提交源码核对一致。

安装态的两项 errors 均来自 `install-smoke`；r31 独立回放同一错误（18.666 秒），
r32 直接执行 Make 确认在第 979 行比较旧 schema checksum 时失败。
已同步 Makefile 安装门禁中遗漏的四处 schema checksum/operation/field 断言，
并让测试包装输出失败命令的 stdout/stderr。所有 manifest 保持 CLI 生成结果。
r33 在 `/tmp/gva-mapping-r33.9dYyDg` 冻结该修正，按测试 ID 分成 17 个独立容器
重跑完整 222 项，每个容器有独立构建目录和 2 CPU/4 GiB 上限。两项安装回归
均通过；本轮为 217 通过、4 跳过、1 error。唯一 error 为发布认证 verifier
在 10 秒命令预算内触发冷构建超时；测试此前依赖其他用例留下的 host binary。
已将 host binary 构建改为显式前置步骤（独立 120 秒构建预算），认证命令仍维持
10 秒限制。r34 冻结在 `/tmp/gva-mapping-r34.II7FDb`，以相同 17 分片重跑全部
222 项，每容器上限 4 CPU/4 GiB。17 个分片全部退出 0：218 通过、4 环境跳过，
无 failure/error；按测试 ID 核对 222 项无重复、无遗漏。runtime 为 75 通过、
1 跳过，session 43/43、control 40/40、providers 为 60 通过、3 跳过。
最长分片 284.219 秒。未放宽认证判据或跳过失败用例。

22 个提交文件的 SHA-256 已逐一与 r34 冻结源码核对一致，独立仓库提交为
`5b2f959047da63fd9c7aa66ccf4b76bb301ac24f`
（`Track managed mapping transactions and enforce lifecycle gates`）。
服务仓库提交后工作树干净；未 push，父仓库 gitlink/lock 保持原 pin。
证据包为 `out/gva-manager-integration/verified-r34-evidence.tar.gz`，SHA-256 为
`32e74e36dc01fa8cc288a80a0bfba844eeaf2f63daaffccef094d159e036909b`。
其中包含完整分片日志、历史失败日志、容器退出状态、分片入口和源码校验清单。
4 个跳过项仍是 arm64 deb 工具链检查及 3 项独立服务仓库缺失的下游契约检查。
本批未重跑真实 guest，也未改变根仓库 r19 源码锁失败的结论；M1/AM1 仍未完成。

下一步自动接线前还需处理已定位的失败路径：中立 provider wrapper 在无效 map
返回后的 unmap 失败被忽略；OBMM map 失败时 unimport 结果被忽略，且旧
`obmm_unmap_region()` 为 void 并忽略 munmap 结果。必须先保留未确认资源与可重试
清理状态，才能允许 SDK 对 pending 事务发 cancel/finish；禁止把一般错误码视为
资源已清空的证明。

r31b 独立 provider 故障注入已复现 wrapper 缺口：`map_rc=-1`、
`teardown_calls=1`、`retained_handle=0`、`retained_binding=0`，退出码 1。
这是生产 wrapper 的受控回滚负例，未执行设备映射；初版 r31 probe 缺少 readiness
回调，未到达 map，保留为无效诊断，不计作该问题的证据。

### 后续 r35：失败映射的清理归属

在 `5b2f959` 后继续实现中立 wrapper、SDK 和 object-session 的清理状态保留。
无效 map 返回且回滚失败时，返回 `MEM_SERVICE_MAPPING_CLEANUP_REQUIRED`，
保留 owner/handle 和对象 key/generation，清零 base/len/flags；unmap 失败也禁止
继续访问，并保留重试所需的 handle。CLI 不再直接丢弃失败映射状态，报告
`map_cleanup_required`，在清理完成前阻止 release；退出清理仍失败时报告
`cleanup_pending` 并保留 endpoint 归属。未修改 public record/binding 结构布局。

新增可执行回归覆盖错误长度、错误固定地址、错误 memory kind、带 handle 的失败
返回、空地址、干净回滚、正常映射解除失败后的访问撤销、重复清理以及无 unmap
回调时提前拒绝。n4-910c1 定向回归通过（1 项，2.260 秒），日志为
`mapping-cleanup-focused-r35.log`。它使用受控 provider 回调，未访问真实设备。
本机 C 语法与 diff 空白检查通过；10 个改动文件与
`/tmp/gva-mapping-r35.rSDihR` 冻结快照逐一核对 SHA-256 一致。

r35 的 17 个隔离分片全部退出 0，完整 223 项中 219 通过、4 环境跳过；
测试 ID 无重复、无遗漏。最长分片 280.219 秒。日志为
`service-suite-r35-<组名>-<分片>.log`，提交前再次逐一核对 10 个文件的 SHA-256。
独立仓库提交为 `bec4a97957b17dc7d2cd200f95ae6ab2e5e72a34`
（`Retain failed mapping cleanup ownership across SDK and CLI`），提交后工作树干净。
证据包 `out/gva-manager-integration/verified-r35-evidence.tar.gz` 的 SHA-256 为
`ece97b168da8116487359b26313f8b68bd1bdf4f12fadd339d55e4a24bc34f91`。
未 push，父仓库 gitlink/lock 未改；未新增真实 guest 验收结果。
OBMM 内部部分 VMA/import 回滚、映射事务自动接线及实际 guest 计量仍待完成；
此次 SDK 修正不能证明 provider 内部已保留所有失败资源。

r36 独立 OBMM 负例对冻结 r35 源码执行：import 返回有效身份，模拟设备 open
失败后再令 unimport 失败；结果为 `map_rc=-1 imports=1 unimports=1 active_slot=0
handle=0`，退出码 1，确认 provider 丢失了未清理 import 的跟踪状态。该受控负例
及源码随 r35 证据包保留，未执行真实设备操作；后续修复与验收见下节。

### 后续 r36/r37：OBMM 部分回滚与 endpoint 清理

本批为严格视图逐项保存实际创建的 VMA，回滚仅解除归属明确的范围；解除失败
保留 slot、import ID、fd 与映射 handle。确认全部 VMA 已解除后才关闭 fd、执行
unimport；任何失败保留清理状态并拒绝范围访问。close 结果不确定时保留隔离状态，
不盲目重试可能已经复用的 fd 编号。新增 `endpoint_close_checked()`，关闭失败
保留上下文并拒绝新注册/映射，旧 void 接口保持兼容并报告未完成清理；CLI 把
关闭失败传播为非零退出码。

OBMM 的 7 项定向回归通过（6.723 秒），覆盖新增的 import/open/unimport 失败、
部分 VMA 回滚、预先存在的映射保护、范围访问拒绝、fd 编号不重试和 endpoint
unexport 失败保留。r36 完整 223 项为 219 通过、4 环境跳过，17 分片全部退出 0，
最长分片 286.184 秒。8 个改动文件与 `/tmp/gva-mapping-r36.tES9Re` 逐一核对
SHA-256 一致。

新 CLI 与 initramfs 构建通过，分别为
`2d29160fd1dccb9a7bcb84cfca87d394ec2d29969ed7a751a176061411e4c193` 和
`d7bfac873823283673d245d202e7288967752983efda00ea1abbf7aecc05face`。
双 guest r36 失败：实验构建目录实际 QEMU 为
`a2ad02dc1f807353d297b0b222b1b07b8b0e71566191cb93150ac0414e87fe32`，与 r24
已验收产物不同，nodeB QEMU 明确报告 `GSVA_UNMAP: bad version 2`。
客户端在 unmap 失败后保留 holder 并报告 `cleanup_pending`；nodeA 等待返回对象
超时。此轮不能作为正确版本组合的验收通过，日志与配置保留。

r37 显式只读绑定已验收的 QEMU 文件（`75851f6a…`），启动前用
`guest-artifacts-r37.sha256` 核对 QEMU、kernel、CLI 和 initramfs 四项均通过，
运行相同两轮 64 KiB 双向共享、同代重映射、页保护及回收用例，runner 退出 0。
两端 session 均两轮成功（provider 每轮 23 ops，consumer 每轮 15 ops）；
`reclaim_ok_count=6`，结束时 live_objects/live_refs/backing_allocated_bytes/
address_reserved_bytes/in_flight/quarantined_objects/quarantined_bytes 均为 0。
nodeB 记录 `remote_reads=1048794`、`remote_writes=131092`，read_timeouts、
read_errors、write_errors 均为 0；本轮未出现 cleanup_pending/unmap_failed/kernel
panic，已检查无遗留 QEMU 进程。未接入自动上报的 import_mappings 计量仍不覆盖
实际映射峰值，不能用结束时的零计数推定该项完成。

服务提交为 `cd94554b8cd5dd5c983c192c4d8218c048776187`
（`Preserve OBMM rollback ownership and report endpoint cleanup failures`），
提交前再次核对 8 个文件与测试快照一致，提交后服务工作树干净。证据包为
`out/gva-manager-integration/verified-r37-evidence.tar.gz`，SHA-256 为
`f76fd074ffd6885c7ed5499583ae002044d8375fbf10348db2b61d6f1799c3c0`。
包含完整服务日志、受控负例、r36 失败与 r37 成功的 guest 日志、构建日志、
QEMU 统计摘录、配置与启动前哈希清单；较大的完整 QEMU trace 保留在远端。
首次打包因 tar 选项位置错误未通过，文件改名为 `.incomplete.tar.gz` 保留；
重新打包退出 0、检查成员清单并核对本地/远端 SHA-256 一致后才登记上述证据包。
该隔离预构建组合不修改父仓库源码锁，也不代表正式发布认证已完成。

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
r17 服务全量日志为 `out/gva-manager-integration/service-suite-r17.log`，SHA-256 为
`9625a792e7efe94d00723554835de2d3348b14ce20b9b375599ee41ba2c69032`。
4 个跳过项分别为 arm64 deb 工具链检查，以及独立服务仓库不含 ub_sim 文件的
3 项下游契约检查；不能将跳过项视为对应环境已经验证。

r24 续作证据包为 `out/gva-manager-integration/verified-r24-evidence.tar.gz`，SHA-256
为 `fcb4f653f00aa56a1b3c5bfa072ae902a9bf1b2a814ad081d206735ca968d1ac`。
包含四组完整服务日志、r22/r24 旧版本失败日志、r24 CLI 构建与 runner 日志和双方
guest 日志；较大的 QEMU trace 与版本化产物继续保留在远端。本次测试容器已退出，
检查远端未遗留 QEMU 进程，既有其他容器未停止。

## M1.4 续作：SDK 自动映射事务与真实进程计数

新增 `mem_service_client_map_managed_allocation()` 和
`mem_service_client_unmap_managed_allocation()`，使用独立 lifecycle 结构，保留
旧 client record、mapping、provider binding 及 wire 布局。object-session 自动
调用新接口，map 的 operation ID 来自 128-bit 系统随机 nonce。业务调用方无需
手动发送 mapping-transition；控制面仅接收元数据，实际映射仍在客户端 provider。

正常顺序为 BEGIN、核对 pending、provider map、CONFIRM、核对 active；解除时
核对当前状态、CLOSE、provider unmap、FINISH。失败的 map 在确认无残留后 CANCEL。
任何应答或 provider 清理不确定均保留 lifecycle；即使本地 handle 已清理，也需
取得 FINISH/CANCEL 的确定应答，才允许释放 holder。BEGIN 使用单次 wire attempt，
防止底层自动重试以 readiness 拒绝覆盖先前未收到的成功应答。后续恢复使用相同
operation ID。历史 active/closing 应答无法授予当前进程实际 handle 的清理权。

新增测试 `tests.test_mem_service_managed_mapping` 包含两项 unittest：

- 真实 daemon 和独立 object-session 进程完成 map/write/read 后，由外部进程
  检查 `live_refs=1`、`import_mappings=1`，尝试提前 release 并确认拒绝；随后
  retire、unmap、release，检查两项计数归零。旧 cd94554 在 r38 明确失败于
  `import_mappings=0`；SDK 初版 r39 与已有 cleanup 回归共 2/2 通过，11.087 秒。
- 编译并执行真实 SDK/provider wrapper，注入控制应答丢失、wire timeout、
  provider unmap 失败和旧 BEGIN 重放，共 11 个 C 场景；检查不重复 map、
  不丢失 handle/事务、撤销 SDK 访问，以及无 handle 时不结束 active/closing。
  此项包装控制调用以注入故障，不能代替实际网络故障或 kernel 资源枚举。

r38 的首次实现版部署在文件传输全部结束前启动，实际消费旧源码，因此该次
focused 日志也保留失败，不计作新实现结果。r39 在核对三个功能文件的本地/远端
SHA-256 一致后重跑。后续 r40/r41/r42 使用完成传输并校验的冻结 archive 覆盖
cd94554 基线，测试期间不修改各自源码快照。

r40 完整服务套件 225 项：221 通过、4 环境跳过，17 个分片全部退出 0，最长
361.508 秒。它尚未包含 BEGIN timeout/自动重试和旧 active/closing 的最终保护，
不能替代最终版回归。r41 同样为 225 项、221 通过、4 跳过，17 分片全部退出 0，
最长 335.488 秒；r41 双 guest 两轮共享、双方保护探针和 6 次回收通过。
新增旧 active/closing 应答回归在 r41 SDK 上实测失败于预期的 cleanup-required
断言（10.694 秒），证明最终保护确实需要补齐；该负例日志保留。
r42 最终版对应 mem_service 提交
`8b8ee14d949bc1747649120bc19f5ab20d613e54`，独立仓库工作区干净，尚未推送。
完整 16 个测试模块分 17 个隔离分片执行，225 个 selected test ID 无重复：
221 通过、4 环境跳过、零失败，所有容器退出 0；最长分片 295.349 秒。
跳过仍为 1 项 arm64 deb 工具链与 3 项独立仓库缺少下游 ub_sim 文件的契约。
本机仅运行两份 C 文件的 `cc -fsyntax-only -Wall -Wextra -Werror` 和
`git diff --check`，全部通过；未在本机运行工作负载或完整套件。

r42 双 guest runner 1/1 PASS，两轮各 64 KiB 双向数据校验、双方 readonly/guard
探针、同代重映射与 6 次真实回收通过。nodeA 两轮 session 用时 46080/45597 ms，
nodeB 为 46162/45476 ms。最终 live_objects、backing_allocated_bytes、
address_reserved_bytes、import_mappings、live_refs、in_flight、quarantined_objects
均为 0。节点 B remote_reads=1048794、remote_writes=131092，read_timeouts、
read_errors、write_errors 均为 0；运行结束检查无 QEMU 遗留。
SDK map 的成功路径必须收到当前 active 确认，guest 日志记录了最终计数归零；
映射存活期间数值为 1 的直接观测来自上述独立进程回归，guest 尚未增加峰值采样。

| r42 冻结输入及产物 | SHA-256 |
| --- | --- |
| 相对 cd94554 的 8 文件源码 archive | `3a453d451d78a5dbd2a077edfe19bff191ceade419a8a99c3385d3b33effdd17` |
| linqu_mem_service-r42 | `efee57ff64560cbfad34bbd548c5c2afdb89b1790991975542dc931508f32f29` |
| managed-share-r42.cpio.gz | `d7abb86194bb38be9c1036db84e7cb0422377f05fdb607a6f5b54fcdc7d1cc69` |

继续使用核验过的 kernel r16，并将不可变 QEMU r16 只读绑定到 runner 使用路径。
runner 执行前先通过 `guest-artifacts-r42.sha256` 校验全部四个输入文件。该组合为
独立开发快照验证，正式源码锁和发布认证状态未改变。

证据包 `out/gva-manager-integration/verified-r42-evidence.tar.gz` 已从远端取回，
SHA-256 为 `5ab8976e784c867c62242c87f76701961528c7dd495768bd7a6dcb55e3385a57`，
本地/远端一致且 archive 成员检查通过。包含 r40/r41/r42 全量测试日志、原始
失败回归、构建日志、容器退出状态、r41/r42 runner 和双方 guest 日志、产物指纹
及冻结变更 archive。大型 QEMU trace 与二进制继续保留在远端；本批测试容器
已全部退出并移除，未停止其他任务容器。

本次仍不提供 daemon 重启恢复、失联 holder fencing 或 kernel 映射枚举。控制
查询或 CLOSE 不可确认时，SDK 清零 base/len/flags 并保留实际 VMA 等待恢复；
不能把 SDK 禁止访问描述为 CPU 页权限已经撤销。原 raw map/unmap API 不自动
记账。现有幂等记录容量仍为 64，长运行容量规划、回收策略及安全地址重用未完成。

## M1.4 续作：endpoint 资源快照与双 guest 存活期核对

mem_service 提交 `b05334b86e645f6bd538f077ba47e4bde0407671` 新增
`mem_service_provider_obmm_endpoint_resources_v1()`。object-session 的 `op=stats`
在 OBMM endpoint 打开时输出 `provider-resources`，包含 export/import handle 数量
及跨度、持有的 VMA 数量/字节、可访问逻辑视图、待清理映射和关闭不确定状态。
接口位于具体 provider，未在服务核心引入平台类型，未修改既有 endpoint/binding
公开结构布局。调用者须与 endpoint 操作串行使用该接口。

统计范围与限制：

- export/import bytes 是各 handle 跨度之和，可能指向同一 backing；不能相加作为
  去重后的物理池占用。worker 全局物理池统计仍需补齐。
- VMA bytes 包含 guard/reservation；accessible bytes 表示 SDK 逻辑范围。
  逻辑访问归零不能证明 CPU 页权限已撤销。endpoint 标记 closing 时，仍存活的
  可访问视图同时计入 accessible 与 cleanup，避免隐藏尚可访问的实际映射。
- 本地 home 使用既有 export FD 映射；peer 才建立 import handle。二者均具有
  VMA，并进入控制面 mapping ledger。服务的 `import_mappings` 字段统计这些
  受管理映射，不能直接等同于 OBMM import ioctl 的次数。
- 快照只枚举当前 endpoint 保留的所有权记录；关闭后的 endpoint 查询返回失败，
  不以有效零快照掩盖诊断对象消失。kernel 孤儿资源与重启核对仍未覆盖。

同时修复 export 成功、descriptor 编码失败、unexport 回滚失败时丢弃所有权记录
的路径：保留 export slot 并进入 closing，拒绝新工作，后续 checked close 可重试。
region handle 耗尽时在 export 前拒绝，防止编号回绕。包装测试覆盖正常/失败回滚、
重试释放、关闭期仍可访问的视图、部分 munmap、close 不确定和计数溢出。

guest `run_app` 每轮在 acquire 后、readonly map 后、readwrite map 后及最终
unmap 后采样，home 在回收后另采样。新增命令行校验器
`guest-linux/aarch64/scripts/validate_mem_service_mapping_resources.py`，接入双节点
runner，校验序列、逻辑/VMA/import 范围及清理残留。新增 3 项纯 Python 测试覆盖
双节点、缺失/伪造/残留/错序日志与 CLI 退出状态；本机 3/3 通过，0.071 秒。
本机 shell 语法、C 交叉语法与 diff whitespace 检查通过；未运行本机完整套件。

远端 n4-910c1 验证结果：

| 验证 | 结果与边界 |
| --- | --- |
| 最终服务 r44 全量 | 16 模块、17 隔离分片、225 个唯一 test ID：221 通过、4 环境跳过、0 失败；所有容器退出 0，最长分片 306.053 秒 |
| 双 guest r44 | runner 失败：初版校验器误要求 home 持有远端 import；保留失败日志，未计为通过 |
| 双 guest r45 | 修正 home/peer 预期并增加回归后，runner 1/1 PASS；两轮 64 KiB 双向共享、双方 readonly/guard 探针、同代重映射、6 次回收通过 |
| root unittest r45 全量 | 432 项：413 通过、5 跳过、5 failures、9 errors，40.741 秒；全量未通过 |
| root layout pytest r45 | 72 通过、2 失败，1.25 秒；全量未通过 |

r45 共核对 home 10 个、peer 8 个资源快照。存活期服务 mapping 数量观察到 2；
home import handles/bytes 为 0，peer 为 1/2097152；各 endpoint 的 VMA 为
2 个、2097152 字节，逻辑视图为 65536 字节。unmap 后该 endpoint 的 import、VMA
及逻辑视图回到 0；每个 session 存活期保留 1 个 2 MiB canary export 基线。
最终服务 live_objects、backing_allocated_bytes、address_reserved_bytes、live_refs、
in_flight、quarantined_objects、quarantined_bytes、import_mappings 均为 0。
nodeB remote_reads=1048795、remote_writes=131092，双方 read_timeouts、read_errors、
write_errors 均为 0；运行后未遗留 QEMU 进程。

root r44 首次归档环境缺少子模块源码，结果为 432 项中 370 通过、5 跳过、4 failures、
53 errors；layout 为 72 通过、2 失败。r45 补齐只读 kernel/QEMU/OBMM 源码依赖，
11 个改动依赖文件与本地 SHA-256 一致，新增资源校验测试通过。剩余失败包括源码
archive 缺少 Git metadata 引发的版本锁检查及依赖入口失败，以及 layout 仍要求
旧锁定版本的 package checksum。该环境无法证明正式 checkout 全量通过；没有
伪造 Git metadata、放宽锁检查或更新旧 pin 对应的黄金值。服务的 4 项跳过仍为
arm64 deb 工具链及独立服务仓库缺少的 3 项下游契约。

| 冻结输入及产物 | SHA-256 |
| --- | --- |
| 最终服务 resources-r44b-changes.tar.gz（基于 8b8ee14） | `67b4f342ac8275e98aa09f7798af6fbfb7162e0573e50ebd9c7f2e138033df9e` |
| root archive（846c63a） | `715a07334019274b9ca8c4c9e8ddd78e3cbdad2b356aec97669b45183331ad9e` |
| 最终 root 四文件变更 resources-r45-root-changes.tar.gz | `17949bbeaa1d2640e304096e92e3f01e255d2df271920b19499c1ae1cf6b2d38` |
| linqu_mem_service-r44（r45 复用） | `16d50f0aec979b4a47b5a0d42aafbb1f0d48b3a2d9cb5bbf093901bd183c94f5` |
| managed-share-r44.cpio.gz（r45 复用） | `148eaf17ac00c33cf3b5a697fa7143e3c9dbd729c567726ed1f4be3d21a6afb4` |
| r45 校验器 | `71faaea96aa190b34faea370f774b41c3047af5f5650fb0d5864cfad2da5ebfd` |

r45 继续使用不可变 kernel/QEMU r16；运行前通过产物及 host harness SHA-256 校验。
证据包 `out/gva-manager-integration/verified-r45-evidence.tar.gz` 的 SHA-256 为
`f017c84492ccb977a262d3b221e613b1d3c45294e4222cdd24f5ace8436060a6`。
包含 r43/r44 服务全量、r44/r45 root 和 guest 成败日志、容器状态、双方 guest
日志、冻结变更及指纹。大体积 root archive、二进制和 QEMU trace 保留在远端
`/home/ll/gva-manager-integration/mapping-20260911.UkR8At` 及配套 reclaim 实验目录。

## M1.4 续作：已注册 kernel region 枚举与 QEMU 崩溃证据

现有 `gva_manager` 增加只读命令：

```text
/bin/linqu_gva_manager --dump-obmm-regions
```

该命令读取 `/sys/devices/obmm/obmm_shmdev*/type` 与 `size`，输出每个 region ID、
类型和字节数，以及版本化汇总；不需要打开 `/dev/obmm`，不执行 bootstrap 或分配。
目录缺失、条目读取失败、非法类型/大小、总字节数溢出均返回失败，且不输出成功的
部分枚举。命令独立使用，混合其他操作参数会被拒绝。

内核依据为 `obmm_shm_dev.c` 的 root device/region 注册，以及 `obmm_sysfs_ub.c`
的只读属性。此范围仅包含 sysfs 已注册 region，输出显式声明
`scope=sysfs consistency=non_atomic`。它不提供 GSVA segment/retired 枚举、进程
VMA、去重物理池占用、正在销毁或已移除 sysfs 的内部对象。两个相同的静止点
观察不能证明并发期间从未产生孤儿资源，也不能证明 CPU fencing 已完成。

双 guest 入口在 conformance 后、worker 启动前记录 baseline；每个节点完成其全部
session 后记录 drained。现有 Python 校验器要求两个阶段顺序完整、region 明细
与汇总相符，且最终 ID/类型/大小集合等于基线；只比较总数不足以排除替换或泄漏。
新测试覆盖正常明细、空目录、缺失/非法属性、溢出、参数冲突、缺失/错序快照、
ID 不一致及伪造汇总。测试使用编译期指定的临时 sysfs fixture，生产构建使用上述
固定真实路径；fixture 不能作为真实 kernel 运行证据。

所有实际构建、完整套件与 guest 执行位于 n4-910c1：

| 验证 | 结果与边界 |
| --- | --- |
| gva_manager 与资源校验器定向 | 11/11 通过，3.152 秒；包含实际编译并执行新增 CLI |
| gva_manager 静态 guest 二进制 | 通过现有 Makefile 构建 |
| root unittest r46 | 433 项：414 通过、5 跳过、5 failures、9 errors，40.203 秒；全量未通过 |
| root layout pytest r46 | 72 通过、2 失败，1.25 秒；全量未通过 |
| guest r46 | 0/1：组包误保留旧 mem_service，缺少 mapping ledger/provider snapshot；校验器拒绝，未计为通过 |
| guest r48 | 0/1，runner exit code 31；nodeB QEMU 在第二轮末尾 SIGABRT，peer 未完成最终枚举 |

root 全量失败项与 r45 相同：归档缺少 Git metadata 触发源码锁及其依赖入口失败，
layout 另有旧锁定 package checksum 预期。未放宽这些检查。本轮未修改 mem_service，
沿用提交 b05334b 及 r44 全量证据，没有将旧服务二进制的 r46 guest 结果计入新组合。
初次测试容器使用了重复 shell entrypoint，退出 126；修正容器启动参数后上述
定向/完整测试才实际运行。该启动失败也已留档。

r48 显式替换 rootfs 中的 manager 与服务 CLI，并保存内部文件指纹。运行前通过
外部 artifact/harness SHA-256 校验，使用不可变 kernel/QEMU r16。双方 baseline
均观察到 0 export、0 import；home 完成两轮 session、6 次回收并输出全零 drained。
peer 第一轮成功，第二轮完成数据校验、unmap、release 及 returned barrier 分配，
日志随后停止；该节点没有第二轮成功或 drained 证据。不得把 home 正例推广到双方。

nodeB QEMU 原始日志末尾为 `corrupted double-linked list`。宿主机 coredumpctl
记录 PID 1236100、SIGABRT、时间 2026-09-11 15:39:50 UTC，匹配本次 nodeB 命令行。
已导出 core 并尝试用同一不可变 QEMU 二进制运行 gdb；系统 core 被截断，线程栈
地址不可读取，当前无法定位原始写坏内存的位置。没有仅凭最后一条 guest 操作就
认定 GSVA unmap 为根因，也没有通过重跑成功来关闭这个缺陷。

| 冻结输入及产物 | SHA-256 |
| --- | --- |
| 五文件变更 kernel-census-r46-changes.tar.gz | `a788c5d5cfe5e5074b31c1cf25db6d7eaf160cdb03d245c53c87ca7766a71a53` |
| linqu_gva_manager-r46（r48 复用） | `cf0334e9d9b340f83da5d229250cce7b99ccab91c4d3cfc9b11b1ea1cf279e23` |
| r48 rootfs 内 linqu_mem_service | `16d50f0aec979b4a47b5a0d42aafbb1f0d48b3a2d9cb5bbf093901bd183c94f5` |
| r48 rootfs 内 run_app | `b446b3910692d165dd7410b23689e67354cad7f7b0b5e2dfb7bd2c2be89480ce` |
| managed-share-r48.cpio.gz | `87beb24912822becc7ec7201408bce829316ffb17f01626a2b0ea254611ae3d6` |
| 资源校验器 | `8cb01bb73393ee2714aa15c2eeecdc7f4481610ebb4c0f68e05a5e316188174d` |

小型证据包 `out/gva-manager-integration/verified-census-r48-evidence.tar.gz`，SHA-256
为 `7fa854d82590834de8221c4fb4fdb96b33bfebaddd441c62730c7ecdf3aa9f65`，包含定向与
完整测试日志、失败 guest 日志、QEMU 崩溃尾部、core 信息/gdb 结果、容器状态、
配置、源码与产物指纹。core、完整 QEMU trace 和二进制仅保留在远端实验目录。
下一步须用可读完整栈或内存检查构建复现 QEMU 损坏，再恢复双方资源回收验收。

## 2026-09-12 续作：CMDQ 日志越界修复与 r52 基线恢复

在 n4-910c1 构建独立诊断镜像 `ub-sim-gva-asan:r49`，通过项目
`build_qemu_binary.sh --with-obmm-tests` 构建同源 QEMU。r49 使用
`--enable-sanitizers --enable-debug`，2066 个构建步骤全部完成，容器退出 0。
镜像指纹 `91938125e2123a51f6ff3c2a66b4b908458bb5e5f093bd18506dc9f177929c32`，
QEMU 指纹 `ae3c50524823fd47f811c6b2534e8b9138b916de6b11a9634292d489258ff10f`。
该二进制运行 5 组 QEMU 单测共 31 项全部通过；不能据此证明实际 guest 回收通过。

r50 使用 kernel r16、initramfs r48，显式核对 artifact/harness 指纹；运行预算
480 秒、最大 650 秒。两端最终日志仍在内核控制队列阶段，未完成共享用例，runner
0/1、exit code 31。ASan 独立日志只有 makecontext/swapcontext 支持范围警告；
QEMU 日志中的 UBSan 则给出确定的数组越界：`ubc_process_cmdq()` 打印
`UBCCmdqDesc.data[6]` 和 `[7]`，而真实数组长度为 6。debug 构建未获得此前
堆损坏的定位结果；该次失败保留，不能记作成功复现或无内存错误。

QEMU 提交 `334fea06b2` 将写入/回读日志限制为 `data[0..5]`，保留全部有效 payload
诊断，不改变描述符布局和协议行为。新增 root 测试
`test_qemu_cmdq_diagnostics.py` 提取真实描述符定义和生产诊断代码，用单个描述符、
成功/失败两种回读结果运行 UBSan。旧代码两个子场景均失败于越界访问，修复后
本机轻量回归通过，0.449 秒；远端通过，0.406 秒。本机未运行完整套件或 guest。
这是独立确认的越界读取，当前没有证据证明它导致 r48 的堆元数据损坏。

r51 root 完整 unittest：434 项中 415 通过、5 跳过、5 failures、9 errors，
50.565 秒。失败项仍为归档环境 Git metadata/源码锁及依赖入口。初次 layout
使用的 QEMU 构建镜像没有 pytest，因此未执行；随后在原 guest-test 镜像执行
r51b，72 通过、2 失败，1.24 秒，仍为旧锁定 checksum 与缺少 Git metadata。
没有将这些结果标记为全量通过。代码和回归文件本地/远端 SHA-256 已核对一致：

- `ub_ubc-r51.c`：`58b5e39f99994f157b76cef2962483ccd201d208d10da750b16781f2454c5846`。
- 新测试：`89b69f5f5e11f48cce03c57a78c2ad998e336d0558d37309831584f744482d73`。

r52 在同一测试环境使用不可变原版 QEMU r16、kernel r16 和 initramfs r48，并在
两端 QEMU 存活时挂接 gdb。runner 1/1 PASS，双方各两轮共享完成；18 个 endpoint
快照与双方 sysfs baseline/drained 校验全部通过，最终各节点已注册 export/import
的数量和字节数均回到 0。gdb 记录两端正常退出，没有捕获崩溃。该结果补上 r48
未取得的 peer 最终枚举证据，但不能关闭 r48 故障，也不能证明调试器不影响时序。
r52 仍使用修复前的 QEMU，不能作为提交 334fea06b2 的集成验收。

优化版内存检查构建 `qemu-asan-build-r51` 使用独立 build-r51 目录，仅将已修复
的 `ub_ubc.c` 只读覆盖到冻结源码；保留 ASan/UBSan 与调试符号、恢复默认优化。
截至本检查点仍在运行，尚无最终产物或该版本的 guest 结果。不得用 r49 的构建
或单测结果替代 r51。初次启动发生在 env 传输完成前，Docker 拒绝创建；确认传输
退出且源码指纹相同后，才启动当前容器。旧二进制、源码快照和失败日志均保留。

证据包 `out/gva-manager-integration/verified-r52-checkpoint-evidence.tar.gz`，SHA-256
为 `01c8003a56985aba11ac7b875c95a8413d1ac6a0bbd25a0730ecb28aab0f5c7f`。
包含已结束的 r49/r50/r51 测试与 r52 运行日志、修复前后回归、gdb 输出、镜像/构建
配置、容器状态及冻结源码。进行中的 r51 构建日志未作为终态证据归档。
远端诊断目录为 `/home/ll/gva-manager-integration/asan-20260911.Z8ORls`；完整 QEMU
trace 和二进制留在远端，未覆盖旧版本。

## 2026-09-12 续作：r53 捕获 route 释放后访问

r51 优化版 ASan/UBSan 构建已完成全部 2066 步，容器退出 0；产物 SHA-256 为
`b84cae533f2e50a1c3fcd1050a92803066b79eadca426daf5208a0f908080a1c`。
r53 实际执行这份二进制的 5 组单测，31/31 通过。双 guest 继续使用 kernel r16
和 initramfs r48，5 项 artifact/harness 指纹全部通过后启动。FM 与 entity ready
通过，nodeB 第一轮 managed view unmap 返回成功后，QEMU 的 ASan 检查终止进程。
runner 0/1、容器退出 1（内部 iteration exit code 31），未完成双方资源验收。

`asan-r53.58` 给出的确定调用链：

- 分配：`gsva_route_map()` 分配 496-byte `GsvaRouteEntry`。
- 释放：vCPU 线程执行 `sim_dec_handle_gsva_unmap()` → `gsva_route_unmap()`，
  直接 `g_free(entry)`，其中包含 `MemoryRegion cpu_window`。
- 后续访问：RCU 线程执行 `flatview_destroy()` → `address_space_dispatch_free()`
  → `phys_section_destroy()`，读取该已释放对象内部第 234 字节，报告
  `heap-use-after-free`。

真实地址空间的 region 引用保留了 UBC device owner，却没有保留独立分配的 route。
从地址空间移除窗口不能证明旧 FlatView 已销毁；立即 unparent/free 导致上述时序。
该报告包含完整的分配、释放和错误栈，独立于日志开头的 swapcontext 支持范围警告。
它确认了当前 managed unmap 的生命周期缺陷；r48 原始 core 已截断，尚无法证明
其堆元数据损坏全部由这一缺陷导致。

修复将 route 转为 QOM 对象，由 route 本身持有 CPU 窗口；表移除只释放表的引用。
窗口在最后一个 owner 引用释放时销毁，延迟 FlatView/DMA 清理期间 route 仍有效。
解除映射将 route 标记为 RETIRED，现有 token 校验拒绝该 route 的后续访问。
tombstone 替换、表销毁和无窗口 route 也使用一致的引用释放规则。该修改不改变
guest wire/UAPI，不给服务增加新的传输依赖，也不证明跨 holder fencing/地址重用完成。

新增 `tests/unit/test-gsva-route-lifetime`，通过项目
`build_qemu_binary.sh --with-obmm-tests` 构建。测试使用真实 QOM 和生产 route 代码，
用最小 memory API 模型控制两个延迟引用，覆盖释放、tombstone 替换、表销毁与无
窗口对象；该单测不能代替真实 MemoryRegion/RCU 集成，后续仍需双 guest 验证。

本地 6 项定向回归通过，2.305 秒；shell 语法与 whitespace 检查通过。远端 root
r54 全量为 434 项：415 通过、5 跳过、5 failures、9 errors，40.315 秒；layout
72 通过、2 失败，1.25 秒。失败集合与 r51 相同，仍不宣称 root 全量通过。
服务代码未变化，沿用 b05334b 的 r44 全量结果。

r53 失败证据包 `out/gva-manager-integration/verified-r53-failure-evidence.tar.gz`，
本地/远端 SHA-256 均为
`398882c483c2e6b074ed37339401ce03148665f9694c7237cae4a0c94476a6c7`。
保存完整 ASan 报告、两端 guest 日志、runner 结果、配置与产物清单、31 项单测和
r51 完成构建记录；原二进制和完整 QEMU trace 保留在远端诊断目录。

修复构建 r54 使用独立缓存副本，未覆盖 r51 产物。首个容器因只读源码目录缺少
新增测试的挂载目标而未能启动；改为挂载完整隔离 unit 目录。第二次构建停在
Cargo 索引更新，主动停止并留档；从 r51 成功容器提取依赖缓存，r54c 使用显式
离线配置继续走项目构建入口。后续构建和 guest 结果见下一节。

## 2026-09-13 续作：关机生命周期修复与同源构建对照

r54 route 修复构建随后完成；r55 双 guest 使用该版本时，ASan 给出第二个确定的
释放后访问：`migration_shutdown()` 已在 `qemu_cleanup()` 中释放 MigrationState，
后续 `vm_shutdown()` 调用 `do_vm_stop()`，仍通过 `migration_downtime_start()`
和迁移耗时统计访问该对象。该错误发生于关机清理，不足以解释关机前的共享超时。

修复令普通 `vm_stop()` 显式传入有效 MigrationState，关机入口传入空值；仅跳过
已失效的迁移统计访问，保留暂停 vCPU、状态通知、block drain/flush 与错误返回。
没有调整迁移清理顺序。新增 root 回归提取实际生产函数，使用 ASan/UBSan 验证
运行中/已暂停的普通停止、运行中/已暂停的关机，以及 vCPU 线程请求转交共 5 个
子场景。旧实现的两个关机场景触发 ASan；修复后 5 个场景通过。
构建 wrapper 同时将 `system/cpus.c`、route 生命周期单测及 unit Meson 文件加入
源码指纹，防止错误复用旧构建。
两处 QEMU 修复已分别提交为 `b415c516ee` 和 `e198611bb2`，未推送；父仓库
gitlink 保持原值。本地源码与远端 r56 冻结修复文件的指纹相同。

| 验证 | 结果与边界 |
| --- | --- |
| r56 ASan/UBSan QEMU 构建 | 容器退出 0，完成项目 wrapper 构建 |
| r56 相关 QEMU 单测 | 6 组 35/35 通过，其中 route 生命周期 4 项 |
| r56 root unittest | 436 项：417 通过、5 跳过、5 failures、9 errors，74.835 秒 |
| r56 layout | 72 通过、2 失败，1.27 秒 |
| r58 root unittest（增加网络审计测试） | 446 项：427 通过、5 跳过、5 failures、9 errors，57.989 秒 |
| r58 layout | 72 通过、2 失败，1.24 秒 |
| 本机最新定向回归 | shutdown、CMDQ、view-release 共 5 项通过，2.050 秒；无 QEMU guest 或模型执行 |
| r57 双 guest | runner 0/1，容器退出 1，内部 iteration exit code 31；未完成验收 |

root 完整测试仍失败于归档缺少 Git metadata 的源码锁/依赖入口，以及旧锁定 package
checksum 预期；集合与 r56 相同。新增网络审计 10 项和 shutdown 回归均通过。
未伪造 Git metadata、放宽门槛或把条件失败记为通过。r58 测试容器禁网，未加载模型。
mem_service 源码未变化，仍沿用 b05334b 的 r44 全量证据。

r57 保持 kernel r16、initramfs r48、64 KiB、两轮、session 等待 60000 ms、请求
超时 30000 ms，runner 预算 480 秒/最大 650 秒。home 完成写入与发布后，等待
`obj-returned` 的调用在 60509 ms 超时。peer 已完成同代重新映射、只读/guard page
探针、读回校验及反向写入，随后 publish 的控制连接被拒绝；home 此时已经退出。
当前没有证据把 peer 延迟唯一归因于某个数据操作。

逐一检查 `asan-r57.26`、`.49`、`.58`，三份日志均只有 123-byte 的
makecontext/swapcontext 支持范围警告，没有新的 ASan 错误报告；未发现独立 UBSan
日志。结束后没有遗留 QEMU guest 进程。此观察不能代替成功完成共享及资源回收，
也不能证明所有潜在内存缺陷已经消除。

| 冻结输入及产物 | SHA-256 |
| --- | --- |
| r56 变更包 gsva-shutdown-r56-changes.tar.gz | `ce72489a5602a1f0946dfeacff81d091b29028c0b98e1dc628e8598558628820` |
| r56 QEMU | `b7dd9cca0d82176f2cf4c440e4fedf36640a07764f9a4e044eab9e6b5d285f3d` |
| r55 失败证据包 verified-r55-failure-evidence.tar.gz | `230cef6ee678ca015618534daf4daf777417a340b83b6b1cdf3dde417e0be688` |

r59 正常优化构建使用同一份冻结源码和独立缓存副本，通过项目 wrapper 显式关闭
sanitizers，保留调试符号。构建容器禁网、禁止拉取镜像，Cargo 使用离线缓存。
源码 6 项 SHA-256 已与本地逐一核对一致。后续双 guest 对照保持 r57 的对象大小、
迭代数和全部等待预算，完成前不把性能开销假设写成根因或通过结论。

### r59/r60 正常构建及 r61 sanitizer 生命周期结果

r59 完成 1743 个构建步骤，构建与单测容器均退出 0；6 组相关 QEMU 单测 35/35
通过。配置输出 `sanitizers=false`，动态依赖中无 ASan/UBSan 运行库。新 QEMU
SHA-256 为 `2ea3357cd3e46794f659dd54f09772dc302436dd0a90c9c112096eefeaede904`。

r60 先验证 kernel、QEMU、initramfs、runner、资源校验器的全部 5 个指纹，再执行
原 64 KiB/两轮共享，保持 60000 ms 屏障、30000 ms 请求、480/650 秒 runner 预算。
runner 1/1 PASS、容器退出 0。home 两轮 session 为 44306/44155 ms，peer 为
44419/44210 ms。双方均完成只读、guard、同代重新映射和双向 payload 校验；
18 个 endpoint 快照及双方 sysfs baseline/drained 比较通过，已注册 export/import
的最终数量与字节数均为 0。QEMU 统计 read_timeouts/read_errors/write_errors
均为 0；运行结束后没有遗留 QEMU guest 进程。

r61 复用 r56 ASan/UBSan QEMU，仅将逻辑对象改为 64 bytes，继续执行两轮全部映射、
页保护、双向读写、回收和关机。它用于生命周期诊断，不能替代 r60 的 64 KiB 结果。
runner 1/1 PASS、容器退出 0；home 两轮为 2083/1817 ms，peer 为 2200/1755 ms。
资源序列与 kernel 基线比较均通过。三份 ASan 日志各 123 bytes，只有既有
makecontext/swapcontext 警告；没有独立 UBSan 日志或新的 sanitizer 错误报告。
结束后没有遗留 QEMU guest 进程。LeakSanitizer 仍按原诊断配置关闭，资源回收结论
来自显式 endpoint/sysfs 检查，不能宣称已检查所有宿主堆泄漏。

正常构建的完整正例和小对象内存检查为两处生命周期修复补上实际 guest 证据。
r57 的 64 KiB sanitizer 用例仍记录为超时；未覆盖失败样本，也未放宽默认等待预算。
根仓库检查点 `bf08108` 已保存构建指纹修订、shutdown 回归及此前失败证据。

## 2026-09-13 续作：SDK 合同负例与 r64 双 guest 验证

新增配置场景 `MEM_SERVICE_OBJECT_SHARE_FAULT=sdk_contract`，复用
`run_ub_dual_node_apps.sh --config <path> --app mem_service_object_share`。
该场景在同一真实 managed session 中加入无效对象/generation、acquire/release
重放、SDK 读写与可见性范围检查、只读写入拒绝和映射存活时 release 拒绝；随后
继续原有双向校验和完整资源回收。默认 `none` 保持原操作序列。结果校验器增加
`--sdk-contract`，按节点、对象及轮次要求全部九类预期拒绝和两类成功重放，仍同时
要求 endpoint 与 kernel 资源校验通过。新增四项测试实际执行配置生成 helper 和
校验器 CLI，覆盖缺失、重复、错误状态及缺少资源证据的拒绝。

本地轻量定向测试 8/8 通过，0.270 秒；`sh -n`、`zsh -n` 与空白检查通过。
远端 r62 全量 unittest 为 450 项：431 通过、5 跳过、5 failures、9 errors，
57.634 秒，新增四项通过。r62 layout 为 71 通过、3 失败，其中新增的解码失败来自
macOS tar 产生的 AppleDouble 元数据文件。保留该失败产物，使用
`tar --no-mac-metadata --no-xattrs` 重建仅含四份源文件的 r63 输入后重新完整运行：

| r63 完整回归 | 实际结果 |
| --- | --- |
| guest unittest | 450 项：431 通过、5 跳过、5 failures、9 errors，59.482 秒；新增四项通过 |
| guest layout pytest | 72 通过、2 失败，1.27 秒；新增元数据解码失败已消除 |

剩余失败仍涉及源码归档缺少 Git 元数据、源码锁及依赖该门槛的 W5 入口和原有包校验。
完整套件未通过；没有改测试期望、伪造 Git 状态或跳过发布门槛。测试容器禁止联网，
使用已有镜像与离线依赖。

真实 `managed-share-20260913-r64` 使用同源正常 QEMU r59、kernel r16 和新开发
initramfs r62；保持 64 KiB、两轮、60000 ms session 等待和 30000 ms 请求预算。
新 initramfs 由已验证 r48 镜像只替换 `/bin/run_app` 得到，服务和 manager 二进制
未改变；该开发产物组合不作为独立发布包或源码锁通过证据。容器退出 0，runner
1/1 PASS；两个节点的 `sdk_contract`、`mapping_resources`、`kernel_regions`
均通过。每节点每轮九次预期拒绝、acquire/release 各一次额外重放，合计 36 次拒绝
和八次重放。双方两轮完整数据校验与回收通过，所有 read/write error 和 timeout
计数为 0，运行后未发现遗留 QEMU guest。

home 两轮分别执行 38 项操作，耗时 45434/44177 ms；peer 各 30 项，耗时
45309/44059 ms。此处记录实际耗时，不据此宣称性能收益。

- r63 四文件源码归档 SHA-256：`d5e53cf8bf7042cc71bbbc471c855e2ed6607f293d7dac40f4de7141e6d4a84e`。
- r62 开发 initramfs SHA-256：`2fe9ee9dbd10cbff096f5d77387059a6a3cd7c1a5fc65b15c9c547b758685b43`。
- r64 配置 SHA-256：`2fd57e86c77fe9bce37d1c5b9d69e77bcf430429ffca94eff4f1b01df3b2e67b`。
- r59 至 r64 的构建、回归、guest 日志及指纹保存在私有归档
  `out/gva-manager-integration/verified-r59-r64-evidence.tar.gz`，本地/远端
  SHA-256 一致：`dd0f3fe9cb9b2ac8c9abecab97f38ae00f301ac2c58e2b1ec810d3108d34d157`。

此范围不包含畸形 GSVA descriptor、实际固定地址冲突或 backing 耗尽；不把 SDK
字节检查解释为 CPU 子页保护。M1/AM1 保持未完成。

## 2026-09-13 续作：固定地址冲突验收

`probe_conflict` 的 r65 初版尝试在目标 VA 创建匿名页。远端定向 3/3 通过，
完整服务 227 个唯一 test ID 中 222 通过、5 跳过、0 失败；r65c 在 bridge 容器
补跑独立 listener 用例通过（9.129 秒），合并为 223 通过、4 原有环境跳过。
四个完整分片耗时 468.301/476.893/448.996/455.019 秒。root r66 全量为
452 项：433 通过、5 跳过、5 failures、9 errors（59.244 秒）；layout r66b
72 通过、2 失败（1.26 秒）。首次 r65 容器入口和 r66 layout 路径错误分别
返回 126/4，未执行对应测试；修正启动命令后保存了上述实际测试结果。

真实 r67 失败于 nodeA 的 `probe_conflict` 预占阶段，输出
`status=internal note=conflict_reservation_failed`；第二次 SDK 映射尚未调用。
检查 kernel `mm/mmap.c:get_unmapped_area()` 确认：非 GSVA 映射与已注册 aperture
重叠时返回 EEXIST；匿名 MAP_GSVA 也被拒绝。随后 PID 1 的 run_app 退出导致
`Attempted to kill init`，peer 的控制连接失效；此运行未取得完整回收证据。
runner 0/1，QEMU 已退出。内核按既定契约拒绝了初版探针，不修改该保护规则。

第二版场景保留已成功建立的可读 GSVA 映射，再经原 managed mapping SDK 请求同一
对象、同一 VA 的第二个视图；两份 SDK 生命周期分别保存。要求第二次请求拒绝、
已确认清理，且原映射首个逻辑页 checksum 不变；继续完整双向读写和最终资源检查。
OBMM provider 记录实际 mmap 失败位置及 errno，runner 要求匹配的 EEXIST 与原
映射/随后重映射同址。服务检查点为 `e7f6a66`，不改变业务 SDK/wire，不接受手工
地址或 descriptor。r68 远端完整服务 227 个唯一 test ID 合并 listener 补跑后，
223 通过、4 原有环境跳过、0 失败。八个分片均退出 0，定向 3/3 通过（41.204 秒），
listener 1/1 通过（17.935 秒）。root 全量 452 项：433 通过、5 跳过、5 failures、
9 errors（113.800 秒）；layout 72 通过、2 失败（1.26 秒），完整根仓库仍未通过。

r69 两 guest 各完成两轮双向读写和回收，home 的实际 mmap EEXIST、首逻辑页保留、
后续同址映射、endpoint 与 kernel 资源检查通过。peer 的第二次 import 在 GSVA
路由注册阶段返回 EIO，尚未执行 mmap；CLI 仅根据 INTERNAL 和 checksum 输出的
成功标记不足以证明 VA 冲突。严格校验器拒绝缺少 mmap EEXIST 的 peer 证据，
runner 0/1，容器退出 1。home session 为 45075/44879 ms、peer 为 45144/44811 ms；
结束后未发现遗留 QEMU guest。此失败保留，不改为接受任意 INTERNAL/EIO。

第三版诊断复用当前映射持有的 OBMM 设备句柄，调用相同的严格映射实现验证
MAP_FIXED_NOREPLACE，无重复 import 或路由注册。只有实际 EEXIST 才通过；
意外创建的 VMA 由独立 probe view 跟踪，清理失败时保留 fd/import、停止接收新工作，
并纳入 endpoint 资源统计。原映射解除前先清理 probe view。新增无效 handle、
其他 errno 拒绝、重复探测、原数据保留、无重复 import、回滚失败保留资源的定向
测试。root 校验器继续要求每轮实际 EEXIST，另核对 `source=retained_handle`。
第三版 r70 远端定向 3/3 通过（19.260 秒），原生静态服务编译成功。完整服务
八个分片均退出 0：227 个唯一 test ID 中 222 通过、5 跳过；listener 单独补跑
1/1 通过（16.493 秒），合并为 223 通过、4 原有环境跳过、0 失败。分片耗时依次
为 291.731、289.204、289.317、296.777、281.079、284.154、269.039、269.187 秒。
四项剩余跳过分别依赖 arm64 deb 工具链，以及独立服务仓库未包含的三处 ub_sim
集成文件；跳过保持显式记录，不计为通过。
root 全量 452 项：433 通过、5 跳过、5 failures、9 errors（107.017 秒）；
layout 为 72 通过、2 失败（1.24 秒）。已逐项比较 root 失败名称，与 r68 相同。
本地只执行 10 项轻量测试（0.466 秒）及静态语法/空白检查，均通过。

- r70 冻结源码归档 SHA-256：`aa803e31ec8687e92c5b56a807619270ab05bfd9bb3845100f0a6984586f16bd`。
- r70 服务二进制 SHA-256：`236f36efaf8de0863896542003923d9fbb0c135061cc89b0828c2b68095bbbd3`。
- r71 开发 initramfs SHA-256：`ade339d69b17608b94c9bb35e49cb60e287bab8397b16cda41a014d530cd6422`。
- r71 冲突配置 SHA-256：`d32cd529ff8929d1f5c51b3d5d1dd41ab0ea18d4518f8da817ee41c728838f50`。
- r72 SDK 配置 SHA-256：`6fb8fcfd4b75466093abfe97f85fb8a49e4bdb0231ec7a1e3e43c0e73df06e74`。

r71/r72 使用同一开发镜像，只替换已验证 r62 镜像中的 service 和 run_app，manager
未改变；不能作为正式源码锁或独立发布通过证据。

r71 实际两 guest、64 KiB、两轮通过：runner 1/1 PASS、容器退出 0。双方各两次
实际 mmap EEXIST，与原映射地址、首逻辑页 checksum 保留及后续同址映射逐项匹配；
`map_conflict`、`mapping_resources`、`kernel_regions` 均通过。完整双向 payload
读写校验及资源回收通过；运行后没有遗留 QEMU guest。home 两轮 28 项操作耗时
44654/44739 ms，peer 两轮 20 项操作耗时 44541/44674 ms，不据此宣称性能收益。
r72 同镜像 SDK 负例回归通过：runner 1/1 PASS、容器退出 0，双方
`sdk_contract`、`mapping_resources`、`kernel_regions` 均通过。共 36 次预期拒绝
与八次幂等重放，双方两轮完整双向数据校验和回收通过，运行后无遗留 QEMU guest。
home 两轮 38 项操作为 44365/44376 ms，peer 两轮 30 项操作为 44282/44499 ms。
两场景双方 QEMU 的 read_timeouts/read_errors/write_errors 均为 0。
服务修订已保存为本地提交 `0ccd397`，其工作区干净；未推送或更新父仓库 pin。
此验收不覆盖畸形描述符或容量耗尽，AM1 仍未关闭。

r65 至 r72 的源快照、完整回归日志、启动错误、r67/r69 失败、r71/r72 通过日志及
输入指纹保存在私有归档
`out/gva-manager-integration/verified-r65-r72-conflict-evidence.tar.gz`。
本地与远端 SHA-256 一致：
`41fcb6150bcdb580cf96fce98ee27e82f47194f8a8044149b084e1ac4535b9ff`，
大小 11321305 bytes，权限 0600。原失败记录未删除或改写成通过。

## 2026-09-13 续作：受管理映射的绑定完整性

继续 A03/A09 时发现：managed SDK 已确认 key/generation 的映射事务，随后直接使用
调用方缓存的 allocation 地址和 descriptor，未核对服务端当前绑定。r73 使用与
当前未修订 SDK 相同的 SHA-256，在远端禁网容器运行新增回归；首个 descriptor
字节变异仍得到映射成功，测试在预期拒绝断言处失败（1 项，2.308 秒）。失败日志
保留为 `binding-repro-r73.log`，没有把 wrapped provider 的结果当作真实 guest 验收。

r73b 在相同旧 SDK 上记录到 `rc=0 provider_maps=1 snapshots=0 state=2`，
回归失败（1 项，2.538 秒）。r74 的首次修订在 pending 下核对服务端完整不透明
绑定，4 项定向通过（28.873 秒）。完整服务 229 项分片无重复/遗漏：224 通过、
5 环境跳过；联网监听补验 1 项通过，合并为 225 通过、4 环境跳过、零失败。
根仓库 454 项为 435 通过、5 跳过、5 失败、9 错误（104.782 秒）；layout 72 通过、
2 失败。14 个 unittest 失败 ID 与 r70 完全相同，源码锁/发布认证仍未通过。

r75 使用 r74 服务运行原预算双 guest，nodeA 29 次变异全部返回 STALE_REF，
第一次合法只读映射/CPU 权限探针/解除通过；第 38 个操作的合法读写映射返回
CAPACITY_EXCEEDED。BEGIN/CANCEL 为每次无效绑定占用两条幂等记录，耗尽现有
64 条全局表；session 失败退出随后导致 PID 1 panic，runner 0/1，不能计为通过。
原始 r75 镜像、配置和日志保留，不减少负例或扩大表容量来改变此次结果。

r77 修订将既有只读 inspect-allocation 核对移到 BEGIN 前，无效绑定或查询失败
均不创建事务。随后 BEGIN 仍检查 generation、ACTIVE 和 holder；同代 ACTIVE
已发布绑定不可改写，所以查询后的退役/换代仍会被事务入口拒绝。provider 仅接收
核对后的快照，引用数、holder 数和 record version 等可变统计不参与比较。
BEGIN 后结果不确定仍保留清理上下文；每次映射增加一次元数据查询，正常 CPU
load/store 不增加控制 RPC。raw API、长期幂等表容量和 AM3 撤销/重用另行验收。

现有 object-session 的 map 增加命名 fault 配置，只修改已 acquire 视图的临时副本，
不接受调用方提供的替代地址或 descriptor。新增 `descriptor_contract` 配置在真实
GSVA descriptor 的 20 个字段/边界字节和 9 类绑定字段上各执行一次变异，随后继续
原有正常映射、双向完整读写和资源回收；root 校验器逐节点/逐轮要求所有拒绝及正常
映射均存在，拒绝进入 provider mmap 才失败的证据。默认配置不增加这些诊断操作。
本地 12 项轻量测试及 shell 语法/空白检查通过（r77：0.642 秒）。远端 r77 的新增
进程用例在旧 r74 实现上于第 33 个操作复现容量拒绝（8.923 秒），修订后定向
4/4 通过（29.042 秒），包括连续 39 次变异后合法映射、读写和释放。C 用例另核对
108 个绑定变异没有创建 BEGIN，查询失败及查询后 admission 拒绝不触发 provider，
原有 11 个丢失应答/清理失败场景继续通过。

r77 完整服务八分片全部退出 0，229 项中 224 通过、5 环境跳过，最长 291.294 秒；
监听用例另通过，合并为 225 通过、4 环境跳过、零失败。r77 root 454 项仍为
435 通过、5 跳过、5 失败、9 错误（105.240 秒）。该次源码打包带入 AppleDouble
附属文件，layout 的脚本扫描触发 UnicodeDecodeError，结果 71 通过、3 失败；
此失败与原归档保留。r77b 归档禁用 macOS metadata/xattrs，共 13 个文件，远端
逐文件比较确认源码字节与 r77 相同；用新目录重跑 root/layout，不修改测试条件。
r77b 的 root 454 项为 435 通过、5 跳过、5 失败、9 错误（57.569 秒），14 个失败 ID
与 r74 一致；layout 恢复为 72 通过、2 个原有失败（1.25 秒）。PTO runner 的失败
记录显示预期的 prebuilt QEMU 缺失后，Git 来源指纹查询因快照无 `.git` 返回 128；
这里没有修改该测试或伪造 Git 信息。
首次 r77 作业启动还曾因不存在的本地镜像名退出 125；确认没有创建作业后，查询
已成功 r74 容器的实际缓存镜像名并重试，未下载镜像。

r78 双 guest 通过（runner 1/1、容器退出 0）：两个节点、两轮 64 KiB 对象，共
116 次绑定变异全部按预期拒绝；每个对象随后完成两次合法映射、CPU 只读/guard
探针、双向逐字节读写和真实释放。双方 endpoint 资源检查与 kernel region 基线
检查通过，read_timeouts/read_errors/write_errors 均为 0，结束后没有 QEMU 遗留。
nodeA 两轮 56 操作用时 45182/44332 ms，nodeB 两轮 48 操作为 45055/44260 ms。
相同镜像的 r79 原 SDK 场景也通过（runner 1/1、容器退出 0），包含 36 次预期
拒绝与 8 次 holder 幂等重放，双方两轮共享及最终资源/内核基线核对通过，数据面
错误和超时计数均为 0。nodeA 两轮 38 操作用时 44300/44543 ms，nodeB 两轮
30 操作为 44218/44585 ms。A03/A09 的完整范围及 AM1 仍未关闭，SDK 对调用方
绑定的核对不替代 kernel/provider 自身的独立身份保护验收。

| 本批冻结输入/产物 | SHA-256 |
| --- | --- |
| r74 源码 archive | `d102dd5706319501fbe01ebc9aab512766bc7e8b0337fbc4eda87bf81ce019b9` |
| r74 服务 | `91fe1c2228e42ed523136a3e0d3c0544fb13ba770a06b888ca082365dfd3d30c` |
| r75 失败 guest 镜像 | `8d6e6e637231c105308ffce266afe1e4c8540dce43e092ab4525236b29442c71` |
| r77 原始源码 archive | `eb4eb361a29682f3e59b96619ebc0b14ecd0337c3519c4f02e66ddf0a76b63ea` |
| r77b 无 macOS 附属元数据 archive | `cb12e4608c7ac79a8e12e9020c929409c11d95a93ff009848a3346497fc683e4` |
| r77 服务 | `680e6e93e8b85591a098982cb377c23e24af557d21dba2053c70a03985096ebe` |
| r78 guest 镜像 | `8ad31eac4aef0d6cae9881a1d4b2d7d7f572a1fe61dfe226b58fceafea01f99b` |

kernel r16、QEMU r59 和 manager 与 r71/r72 保持相同指纹；每次 guest 启动前均
核验六项输入 manifest。r76 配置已生成但未执行，后续回归使用 r79，不计虚拟 run。

本批 mem_service 实现已提交为 `fe3156817e238d1b011f2191f53c4accc575f9b8`，
独立仓库工作区干净，尚未推送。父仓库 gitlink/lock 没有修改。

完整证据包 `out/gva-manager-integration/verified-r73-r79-binding-evidence.tar.gz`
已从远端取回，大小 26953678 字节、65 个成员，本地/远端 SHA-256 一致：
`4127e9e7bc1f83433089b7d147cb34684e26dd509c758ea06a7534cc0fe669b0`。
包含失败复现、r74/r77/r77b 全套日志、r75 失败及 r78/r79 成功 guest 原始日志、
源码快照、服务/guest 镜像、输入指纹及 34 个测试容器终态；gzip 完整性检查通过。
归档权限为 0600，未包含或公开整份网络操作审计日志。

## 下一批的前置条件与未完成项

本地提交已获授权并完成。正式源码锁验收仍需子仓库提交可从远端获取，再同时
更新父仓库 gitlink/lock；当前尚无 push 授权。历史 r19 校验器明确拒绝 dirty
mem_service；提交后旧 lock 与新 checkout 的 revision 不一致同样必须拒绝。
本轮未增加绕过开关，未修改父仓库 pin，也未把已知失败计作通过。

下一批先处理 M1.4 的幂等容量耗尽与清理保证、实际 backing 耗尽、不同 size/alignment
压力及 provider/kernel 独立身份负例。当前 64 条幂等记录的限制仍在；无效绑定的
前置拒绝只消除本批不必要的记录占用，不能证明长运行请求或耗尽后的解除/释放能力。
已有 endpoint 计量和 registered-region 基线核对覆盖正常清理，崩溃、失联和未知资源
仍需独立验收。后续还包括 AM2 CPU/PTO 联合访问、AM3 恢复/重用、AM4 多 home/W5/
独立发布，以及 n4-910c 的独立复现。总计划 M2-M5 的动态身份、恢复、压力与 replay
联合失效同样未关闭。
正式源码锁恢复通过后，仍需逐项实现和运行验收，不能由 r20/r24 的 CPU 正例推定完成。
