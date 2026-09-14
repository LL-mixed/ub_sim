# 受管理 GVA 服务实施与验证记录

始记日期：2026-09-11。对应总计划 M1 与地址管理补充计划 AM1/AM2；续作按日期和 run ID 追加。

## 当前验收边界

本轮接通常驻 worker、真实 backing 的发布/正常释放、客户端可见性操作及双 guest
组合入口。64-byte 与 64 KiB CPU 双向共享及正常回收用例均已通过；后者在修复
QEMU 发送队列计数错误后通过。r20 另通过双方只读/guard page CPU 故障探针、
同代重新映射及连续两轮共享回收。AM1 权限/边界等完整矩阵仍待补齐，M1 和 AM1
均未完成整阶段验收。
CPU/PTO 联合访问的单线程、原 completion 恢复及四线程 CPU/shared owner 子集
已通过，最新证据见末尾 r146-r148；安全重用、完整恢复、多 home 和 W5 新路径
仍未通过。禁止以这些子集或历史 W5 结果代替完整阶段验收。
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

## 2026-09-13 续作：清理容量接纳保护

先在远端冻结旧代码并复现故障，再实施派生清理预留。r80 三个用例均失败于
CANCEL/CLOSE/FINISH 的 CAPACITY_EXCEEDED（26.307 s）；r81 修复后这三项及
独立 SDK 映射进程的满容量退出，共 4 项通过（36.197 s）。这只是定向结果，
后续增加了满容量 CONFIRM、快照恢复、通用记录保留和旧请求重放测试；不能据此
关闭 M1.4 或 AM1。

新增 OBMM guest profile `MEM_SERVICE_OBJECT_SHARE_FAULT=capacity`，限定单轮：
双方完成真实共享写读且都保留 RW 映射后，通过已有 CLI 填满非预留幂等空间。
返回 barrier 的 allocation 提前接纳；压力达到容量后，通过其 retire/reclaim
释放 barrier，双方才解除映射和 holder。整个过程保留历史幂等结果，校验 saturated
时的两条 live mapping、零非预留容量、无预留缺口及最终 backing/kernel 基线。
该 profile 仅验证已接纳资源的清理，单轮限制明确暴露尚未实现的历史回收边界。

r82 扩展测试曾因测试配置使用内部字段 `max_retained_records` 而出现 13 项
启动失败；改为实际 CLI 支持的 `record_retention=latest:1` 后继续验证。
r83 完整服务 236 项中仅旧 `test_idempotency_store_exhaustion` 失败：旧测试要求
64 个 allocation 全部接纳，随后 RETIRE 也返回容量错误。这与清理预留契约冲突。
更新后的测试接纳 32 个 allocation（32 条应答加 32 条 retire 预留），验证第 33 个
拒绝、全部取消/回收成功，以及全部退役后旧 ALLOCATE 重放不会创建新代际。
这项用例覆盖尚未 publish 的分配意图，home 取消确认由 fixture 发送；真实 backing
释放的证据来自下述 r85 guest，真实 backing 耗尽仍待独立验收。
最终完整回归使用 r86 冻结快照：236 项，231 PASS、5 SKIP、0 FAIL；最长分片
289.278 s。相同实现的独立 listener 测试在 bridge 容器通过（16.696 s），对应
合计 232 PASS、4 环境 SKIP，236 个 test ID 无重复、无遗漏。剩余跳过为缺少
arm64 deb 构建工具链，以及 3 项依赖 standalone 仓库不包含的 ub_sim 文件。
具体条件与完整日志保留在证据包中。

首次 guest r84 在 session 配置阶段失败：`timeout_ms=120000` 超出 CLI 的
60000 上限，双方均未进入应用映射流程；入口退出导致 guest PID 1 panic。
修复生成器并增加超时上限契约检查，未改变服务、kernel、QEMU 或 manager。
r85 真实双 guest 容量场景通过（runner 1/1，容器退出 0）：

- 39 次压力请求触及接纳边界；饱和时 `idempotency_used=55`、清理预留 9、
  非预留可用量 0、预留缺口 0；3 个真实对象保留 6291456 字节 backing/address。
- 此时双方持有 2 个 mapping、2 个 holder，`in_flight=0`；随后 barrier 的
  retire/reclaim 放行双方清理，最终幂等表保留 64 条历史记录，清理预留归零。
- 主对象双向 CPU 写读、publish/wait-visible、只读及 guard page 探针通过，
  服务、endpoint 与 kernel region 回到基线；read/write error 和 timeout 为 0。
- nodeA 26 操作用时 46828 ms，nodeB 20 操作用时 46753 ms；整个容器运行
  59.522610 s。nodeB 的 QEMU 统计 remote_reads=524503、remote_writes=65556；
  nodeA 对应 162/45；双方 read_timeouts/read_errors/write_errors 均为 0。

父仓库 r85 全套为 457 项：438 PASS、5 SKIP、5 FAIL、9 ERROR（57.534 s）；
14 个失败/错误 ID 与 r77b/r83 基线一致，仍涉及 Git metadata、source-lock 和
冻结依赖环境，未修改或绕过这些门禁。r83 布局检查 72 PASS、2 FAIL（1.24 s），
与原基线相同。轻量生成器/校验器 15 项通过（0.957 s）。

同一服务/镜像的 r87 原 SDK 双轮场景通过（runner 1/1，容器退出 0），双方的
两轮共享、36 次预期拒绝、8 次 holder 幂等重放及最终资源/kernel 基线通过。
nodeA 每轮 38 操作用时 44167/44182 ms，nodeB 每轮 30 操作为 44263/44067 ms。
本批 mem_service 已提交 `36292e473a3e5016a934e88d43fae9cfc14ebc70`，工作树
干净，未推送；父仓库 gitlink/lock 保持原值。

本批输出 manifest 均由远端当前 CLI 的 `print-*` 目标生成并取回同步，未手工编辑。
admin schema 为 7411 bytes / `0x5272af77`，upgrade policy 为
2144 bytes / `0x06a074b3`，package manifest 为 9703 bytes / `0x375851a6`；
对应运行时 fixtures 全部通过。wire opcode、版本及公开 client record 布局未改动。

| 冻结输入/产物 | SHA-256 |
| --- | --- |
| r80 旧实现复现测试 | `d8fa92128ae1ddc8978a202c594995181d86d14ee3d9b645bfc1a4463f2aa9cd` |
| r83 源码 | `141529dd1d4fdeeac62fcb9b88166e12724ff69d5e2a906060b905f5d65e848a` |
| r83 服务二进制 | `ef130499c1582cfa55c0f45a327367c3bfcb0aae7a60f61c1207522a9d635662` |
| r84 失败 guest 镜像 | `5448d5df1bdede189bc7b15c5648428f2746b7c978f2c31749de8d6b7db48ed2` |
| r85 入口修正 | `8b3eb9d0fc6475ab26c30939542e3d1dc67126d3f1278a339b2365df96271069` |
| r85 通过的 guest 镜像 | `58dc20ef02ed4717cb84ac6821b8924217885c41fe2084b453abe1579627ac7a` |
| r86 容量测试修订 | `e1b482ce2f464186af31365912f3357d3432963254024d3e2ef6d935feb79cc6` |

完整证据包 `out/gva-manager-integration/verified-r80-r87-capacity-evidence.tar.gz`
已取回并核验：33006628 字节，91 个成员，gzip 检查通过，本地与远端 SHA-256
均为 `dc369349c5a5c8659f6b6b9149bfd94513349d580727691874bbbf795d93bd59`。
其中保留全部失败复现、成功运行、完整回归日志、冻结源码、生成的 manifest、
实际服务/guest 镜像、输入指纹及 31 个测试容器终态；权限 0600，未包含网络操作
审计原始日志。SSH 握手超时及重试已逐项审计，重试前核对原句柄，未重复启动 guest。

## 2026-09-13：地址容量拒绝保留 worker 可用性（r88-r91）

本批完成 AM1/M1.4 的单 home 地址容量子集。修复前，home worker 在 size 向上
取整溢出、alignment 无法落入 aperture 或地址空间不足时直接退出并注销 provider，
已有对象的后续处理也随之中断。r88 的可执行 worker 定向测试复现了此行为。

mem_service 提交 `3e4ccb4951524ecccadf49849840f6dae10adf9f` 将“确定未创建
资源的容量拒绝”和结果不确定的内核操作分开处理：

- 仅在调用任何资源创建 ioctl 之前返回容量结果；reservation 与 next_address
  均不改变。无效的已有身份、aperture 或 provider 绑定仍中止处理。
- 先持久记录 `capacity-reject-intent`，再通过已有 SDK 发出携带 generation 的
  retire，以及绑定 home/incarnation 的空资源 reclaim 确认。请求最终为 RETIRED，
  无 address/descriptor；worker 记录 `reason=address_capacity` 并继续服务。
- 取消回包身份不符、回包已有 backing、RPC/日志持久化失败时保留状态并停止；
  kernel/export 执行失败继续要求 reconciliation，不能仅凭 errno 确认资源不存在。
- 沿用既有 wire/CLI 与幂等清理保留额度，无 opcode、manifest 或公开结构布局变化。
  此异步取消的原因保存在 worker 日志，现有 inspect 返回 terminal state；尚无
  单独的持久化分配失败原因字段。

### 实际双 guest 地址容量用例

现有 headless 入口使用 `MEM_SERVICE_OBJECT_SHARE_FAULT=address_capacity`，
只接受一轮生命周期。r90 的逻辑对象大小为 65537 字节、请求 alignment 为 4 MiB；
worker 保留 2 MiB backing。两个客户端都持有该对象的读写映射时，辅助客户端执行：

1. `UINT64_MAX` size 与 `2^63` alignment 请求，均在内核资源创建前取消。
2. 分配一个 8 MiB、8 MiB 对齐的真实对象，其末端抵达 16 MiB aperture 边界。
   此时 4 个活对象实际保留 14 MiB，另有 2 MiB 对齐间隙；不存在区间重叠。
3. 申请一个字节，确认因地址耗尽进入 RETIRED；释放 8 MiB 对象后再申请一个字节，
   仍被取消，符合当前 `address_reuse=disabled` 契约。
4. 两个原客户端继续执行逐字节双向读写、可见性校验和正常 unmap/release/retire；
   独立 endpoint 计量和 kernel 已登记 region 的非原子静态快照回到基线。

校验器的 `--address-capacity` 同时检查实际几何范围、取消 generation、无
reserve-intent、空 descriptor、压力前后 backing/ref/import 计数；缺失、重复、
区间重叠、错误对齐、已有映射丢失或未知拒绝原因均令验证失败。
该结果覆盖地址容量；物理 backing 池耗尽及失败后的资源核对仍需单独完成。

### 验证结果

| 入口/运行 | 结果 |
| --- | --- |
| r88 修复前 worker 定向回归 | 4 项：3 PASS、1 FAIL；15.716 秒，失败日志保留 |
| r89 worker/资源校验/SDK 入口定向回归 | 20 PASS；16.412 秒；静态 worker 的 `-Werror` 构建 PASS |
| r89 完整 mem_service 八分片 | 236 个不同测试 ID，无重复；231 PASS、5 SKIP、0 FAIL；最长分片 302.986 秒 |
| r89 独立监听补跑 | 1 PASS，11.551 秒；与完整套件合并为 232 PASS、4 项环境 SKIP |
| r89 父仓库完整测试发现 | 458 项：439 PASS、5 SKIP、5 FAIL、9 ERROR；103.137 秒 |
| r89 layout 门禁 | 74 项：72 PASS、2 FAIL；1.25 秒 |
| r90 真实地址容量双 guest | PASS 1/1，容器退出码 0；59.059831 秒 |
| r91 原有 SDK 两轮双 guest | PASS 1/1，容器退出码 0；100.652821 秒 |
| 本地轻量检查 | 16 PASS，0.987 秒；run_app/sh、runner/zsh 语法与 diff 检查 PASS |

完整服务套件余下 SKIP 仍为一项发行包工具链和三项独立仓库缺失的父仓库路径。
父仓库的 14 个失败/错误 ID 以及 layout 的两个失败 ID 与 r85/r83 一致；未将
缺少 Git 元数据、源码锁及环境路径的失败算作通过，也未修改锁或伪造元数据。

r90 两客户端分别完成 26/20 个操作，耗时 46582/46173 ms。nodeA 的 QEMU
remote_reads/remote_writes 为 162/45，nodeB 为 524511/65557；两节点
read_timeouts/read_errors/write_errors 均为 0。r91 复用同一 worker/guest 镜像，
原有 SDK 拒绝、重复请求、双向共享、页保护及资源基线校验均通过。运行后单独
查询进程列表，确认无遗留 QEMU；进程查询与包含二进制路径的命令分开执行。
r90 最终服务统计为 8 个分配意图、4 次实际 publish、8 次 reclaim；活对象、
backing/address 字节、export/import、holder、in-flight 和 quarantine 均为 0。
幂等结果已用 36 条、可用 28 条、清理保留额度与 deficit 均为 0。

### 冻结输入与证据

| 输入 | SHA-256 |
| --- | --- |
| r88 失败复现源码增量 | `140dd3db02271c302b8deb5799641a1a0d8ab0bc36c90fcf5b29f531fd9cd006` |
| r89 修复与测试源码增量 | `5d99f3cb672bad096eba4e7c9d2a810e88b27b57a00d9cff44d51f7760923d1a` |
| worker 源码 | `76170b2689f0e05def095ac9a7c968fa17baceee3bdec22a11536f1a53679e73` |
| 静态 worker | `339c6ca382ee6f98e04d17a9362a7c07e742887bb5918802aa0ae48c76d3ee4e` |
| run_app | `a28c43e11e3a53c83f725524b74a8b4f722ad679b662b4b9aad73e5d37ce362c` |
| runner | `3c8d4db9bdd4df45557bb93fdd3ad3fbf5cddf6424e185dea354288e37ec8bfc` |
| 资源/容量校验器 | `b3c007d71be317274a2c0a3ec477cd08e2070e0469b470a6cf466e3d9bb5984d` |
| r90/r91 guest 镜像 | `2222e37d253de64c59dd891baac0d04f1e19fac66eb2f0cae9441ef247aa930d` |
| r90 配置 | `88ab6b6d6edfe2d3264dff8b171a428f3c762ae74b2e2ca5fed1c54c1849093d` |
| r91 配置 | `ab92d3200127324c9ae7045d08c4a05f339fdcad374cb5d661b326fa4d66a28a` |

服务主二进制、manager、kernel 与 QEMU 延用前述 r83/r85 的已记录基线；镜像内
文件哈希、六项启动输入清单和实际命令均保留。源代码增量与此前证据包共同构成
开发快照证据，正式源码锁/发布验收尚未通过。

证据包 `out/gva-manager-integration/verified-r88-r91-address-capacity-evidence.tar.gz`
包含失败复现、完整回归日志、源码增量、静态 worker、guest 镜像、两个运行的原始
双节点日志与报告，以及 16 个测试容器终态和缓存镜像身份。远端封存为 0600，
18456978 字节、37 个成员，SHA-256 为
`17d591ab6b004dff234438f32632ea157ee5b7b97a54924575331554ee5a8310`。
已取回本地并核验 gzip、成员数量、长度与 SHA-256，均与远端一致；本地权限
同为 0600。不包含原始网络操作审计日志。

## 2026-09-13 续作：checked backing 分配结果与 segment pin（局部实测通过）

本批补齐物理 backing 分配失败的确认边界。新增固定宽度、120-byte 的
`OBMM_CMD_GSVA_EXPORT_SEGMENT` v1 ioctl；原 export opcode 与中立服务 wire
保持不变。新调用携带完整 segment 身份，内核校验后在物理池分配前 pin segment。
成功建立 export 后记录实际 export ID；执行中、已绑定及结果未知的 segment
均拒绝直接 retire。正常 unexport 成功后才解除该绑定。

结果分为 CREATED、NO_BACKING、UNKNOWN。只有池/sgtable 分配失败且临时内存已
归还、设备映射尚未开始的路径可以返回 NO_BACKING。普通 ioctl errno、回包丢失、
未知版本、结果字段矛盾和后续映射阶段错误均不能授权释放 reservation。
平台库返回 0、1、-1 分别表示这三类结果；旧内核拒绝新 ioctl 后不回退普通 export。

worker 收到确认无 backing 的结果后，先记录结果，再退役对应的实际 segment，
最后调用原有 generation-bound retire/reclaim，取消未发布的服务对象。任一步无法
确认则停止并保留状态文件；成功后继续服务，地址游标不回退。该实现未提供崩溃后
自动 reconciliation、未知映射的强制撤销或地址安全重用。

CLI 新增可选配置 `fast_allocation=0|1`，默认 0。值为 1 时只使用已清零的缓存池页，
不走同步清零或扩容慢路径。已编写 `MEM_SERVICE_OBJECT_SHARE_FAULT=backing_capacity`
的单轮双 guest 用例：保留双方真实映射，验证 live segment 的提前 retire 被拒绝，
申请 2 GiB backing 并等待明确拒绝，再成功申请/释放小对象，最后继续跨节点读写。
压力用例使用 4 GiB 虚拟 aperture，使 backing 申请可以到达内核物理池；虚拟地址
保留本身不分配对应大小的 payload。它验证已清零缓存不足，不代表整机 RAM 耗尽。

验证器要求同时具备服务账本、内核 pool used 值、实际对象身份、worker 有序回滚
和现有映射保留证据。池统计按 `consistency=non_atomic` 记录，不宣称原子快照。
实际运行结果和中间失败见本节后文；通过范围限定为 r118 backing 与 r119 SDK 用例。

已取得结果：

| 验证 | 实际结果 |
| --- | --- |
| r92 GVA library / worker 定向回归 | 11 项通过，19.469 秒 |
| r92 worker AArch64 静态构建 | `-O2 -Wall -Wextra -Werror -static` 通过 |
| 本地网络审计及资源验证器轻量测试 | 17 项通过，0.703 秒 |
| r93 root 全量 unittest | 459 项，438 通过、5 跳过、5 失败、11 错误，292.689 秒 |
| r98 编译诊断 | 确认 r93 额外两个错误来自测试镜像缺少 libasan/libubsan；原始链接器错误已保存 |
| r98 root 全量 unittest，既有 sanitizer 镜像 | 459 项，440 通过、5 跳过、5 失败、9 错误，272.330 秒；失败集合回到既有 14 项 |
| r93 layout | 74 项，72 通过、原有 2 项失败，1.27 秒 |
| r93 非 loopback listener | 1 项通过，25.320 秒；仅容器自身 IPv4，无公开端口 |
| r100 服务完整回归 | 8 个分片全部结束，共执行 236 项；vendored 校验测试的两个头文件子断言失败，其余分片无失败；保留 5 项环境跳过 |
| 本地 vendored 清单定向检查 | 更新真实头文件哈希与对应固定测试值后，1 项通过，0.001 秒；未放宽校验断言 |
| r110 layout | 74 项，72 通过、同样 2 项既有失败，1.24 秒 |
| r110 服务完整回归 | 236 个唯一测试编号，8 个分片全部 exit 0；231 通过、5 项环境跳过，无失败；分片耗时 187.011–205.422 秒 |
| r109 内核与匹配驱动构建 | Image、所有配置模块、`linqu_ub_drv.ko` 构建完成，容器 exit 0，304.348 秒；最终 config 与输入 SHA-256 相同 |
| r114 非 loopback listener 补验 | 1 项通过，7.880 秒；合并 r110 后为 232 通过、4 项环境跳过 |
| r104b root 完整 unittest | 459 项，433 通过、5 跳过、5 失败、16 错误，609.274 秒；新增 7 个错误均为原有 10 秒限时内 C 编译未结束 |
| r116 root 完整 unittest | 459 项，440 通过、5 跳过、5 失败、9 错误，43.636 秒；七个编译超时消失，失败编号集合与 r98 的既有 14 项相同 |

最初内核构建在 n4-910c1 的隔离容器执行。保留原源码/构建缓存，在新目录叠加本批
源文件；基础 obmm_core.c、obmm_import.c、ub_sim_decoder_main.c 的 SHA-256 与
本地基线一致。r92 磁盘构建出现大量 I/O 等待，保留日志和构建目录后主动停止重复
作业（exit 137）。r95 临时目录带 noexec，无法执行内核构建脚本；r95b 的前置检查
误用了不存在的脚本名，exit 127。两次失败日志均保留。r95c 改为实际执行复制的
shell 探针，使用显式 exec 的 32 GiB 临时工作区及 `make -j32 Image modules`，
曾继续构建，最终在 r109 同源构建完成后停止。没有修改主机挂载设置或删除原缓存。

后续验证转到另一授权目标 n4-910c 的独立临时目录。跨目标的源码归档排除 `.git`，
五项输入哈希均与 n4-910c1 一致；源码不写入该目标已有工程。r99 构建发现其现有
镜像缺少 OpenSSL 开发头文件。r101 检查发现原完整构建镜像达到 94421607363 字节，
已中止该镜像的整体导出，未将它跨目标传输。导出管道最终返回 0，但仅生成
20-byte 空 gzip；该文件明确记录为无效产物，未执行 image load。
改用独立容器补齐开发依赖；
`deps-r102/` 专门保存 DNF 事务、仓库请求日志及安装后包版本，不保存用户输入或
模型数据，在证据归档前不清理。该安装只访问已核对的现有 HTTPS 软件仓库镜像
（TUNA、华为云、USTC），保留 GPG 包验证；不修改主机软件包和全局挂载配置。
命令级网络操作与重试继续由原审计工具记录，DNS/重定向等连接级行为不宣称已抓包。
依赖事务退出 0，实际下载 7.5 MB，安装 OpenSSL、libelf、zstd 开发包；包版本及
请求日志已保留。新镜像仅保存在该目标本地，随后 r103 在 `network=none` 的容器
中先构建 OBMM 目录，再构建 Image/modules。该作业因下面定位的 CPU 信息读取
问题继续缓慢执行，最终由同源码、同 config 的 r109 成功构建替代并停止。

r93 与 r98 的服务完整回归均触发 600 秒外层超时，八个分片均未完成，不能计作通过。
原始日志保留；n4-910c 上的 r100 使用同源快照和 1500 秒外层限时完成执行，暴露
本批 UAPI 头文件更新后未同步 vendored 校验清单的问题。r104 已更新清单和精确
哈希测试值，并开始重新执行完整服务与 guest 契约套件。源码 revision 暂时保留
扩展前基线并明确注明开发差量；待内核测试和提交后填写实际新 revision。
上述内核、服务和真实 guest 的本批最终验收仍未完成。

后续定位到构建环境的独立瓶颈：r107b 的空 C 编译耗时 19.081 秒，实际 user/sys
分别为 0.013/0.298 秒；三个本任务 GCC 进程均停在宿主 `/proc/cpuinfo` 的
`c_show` 读取路径。cgroup v1 无 CPU quota 且没有 throttling。r108 保存同一宿主
未经修改的 CPU 信息，核对 320 个 processor 条目与在线处理器数相同，然后仅在
新容器中只读绑定该原始快照；同一空编译降至 0.022 秒。快照 SHA-256 为
`3b6adba1371c7de6b47568ca19a650784394e10f0faf1743dc3419d6f104e7b8`。
该调整不改变宿主 `/proc`、CPU 特性、源码、编译选项或断言，不属于运行性能数据。
两个空编译对象的 SHA-256 完全相同；r109 完整构建随后实测使用约 32 个 CPU 核心。
新 r109 构建采用相同源码/config 与本地依赖镜像，使用该只读快照消除重复读取等待。
此前所有慢作业和失败证据仍保留，最终结果按实际终态记录。

r104 root 容器曾因重复传入 shell 入口退出 126，没有执行测试；启动日志已保留，
r104b 显式指定入口后开始完整 unittest。r104b 的独立 layout 容器缺少 pytest，
同样未执行对应测试；随后 r110 使用原有含 pytest 的 r14 镜像完成 74 项验证。
没有为这些环境故障修改测试内容或安装新的宿主依赖。
r116 沿用完全相同的源码、断言与限时，改用 n4-910c1 自己的原始 CPU 信息快照，
重新执行完整 unittest；七个对应测试实际通过，原超时运行仍保留为失败记录。
r109/r110 成功后，精确停止了本任务的 r95c/r103 慢构建及 r104 八个慢分片；
均记录 exit 137，保留容器、日志、config 和可重建的源码快照，未删除用户源码。

r111 首次实际双 guest 验证失败：新内核完成 bootstrap、provider conformance、
两端同址 mapping；B 逐字节读取 A 发布的 64 KiB 数据，并写回、发布。
随后压力辅助流程因 guest 没有 `awk` 命令退出，尚未执行大额 backing 分配及
live segment retire 检查；A 等待压力屏障超时，整个用例判为 FAIL，runner exit 1。
未将部分成功计作完整验收。日志和报告保留，退出后独立查询没有遗留 QEMU。
r117 将三处字段提取改为既有 guest 流程使用的 `/bin/busybox sed`，未安装新依赖。
新增定向测试从真实脚本读取表达式，验证 64-bit segment/地址/epoch 提取及无匹配
输出；本地资源验证器与网络审计合计 18 项通过，0.713 秒。后续重跑完整套件和
r118 backing/r119 SDK 真实用例，仍使用相同 r109 内核和 r92 worker。

r118 backing 用例已实际通过，两个 guest 的 `mapping_resources`、`kernel_regions`
和 `backing_capacity` 校验均 PASS，runner exit 0。活跃 segment 的直接 retire
返回 errno 16。2 GiB 请求取得 NO_BACKING，真实 segment 退役及服务取消确认完成；
小对象随后分配成功并回收，A 再次逐字节读到 B 发布的 64 KiB 写回数据。
内核 pool used 在 before/rejected/recovered/drained 四个时点分别为
8388608/8388608/10485760/8388608 bytes；失败时 ready 为 0，后台补充缓存后恢复。
pool total 会随缓存归还和补充变化，未将它描述为固定容量或原子快照。
结束时双方真实 export/import 计数回到基线，独立 `pgrep` 检查没有遗留 QEMU。

r117 最新完整回归：root 460 项，441 通过、5 跳过、5 失败、9 错误，44.051 秒；
失败集合仍为既有 14 项。layout 72/74 通过，1.23 秒，失败同前。
服务 8 分片全部 exit 0，236 项、231 通过、5 跳过，184.180–200.882 秒；
结合相同服务源码的 r114 独立 listener 补验为 232 通过、4 项环境跳过。
两次 SSH banner 超时发生在启动前；恢复后先确认相应容器不存在，再重试。
所有尝试及重新核对分别记录到网络审计，未因超时启动重叠作业。

r119 使用完全相同内核、worker 和 rootfs 完成两轮 SDK 合同及双向 CPU 读写；
双方 `sdk_contract`、`mapping_resources`、`kernel_regions` 均 PASS，runner exit 0，
101.286 秒。r118 用时 62.078 秒。两轮运行后的独立 QEMU 残留查询均无结果。
构建源基线为 `f3f7aa992495da7c2d129629779ce60e227bc3cf`；基线已有八文件开发
差量，本批叠加后共十三个差量文件。r120 对这十三个文件逐一核对实际构建源与
本地工作区 SHA-256，全部一致，包含未在 r92 中重复打包的 `obmm_async_load.h`。

证据归档约定：`build-tests-r120.tar.gz` 保存第二目标的完整测试、构建和依赖事务
记录；最终 `verified-r111-r120-backing-evidence.tar.gz` 根层保存该嵌套归档、源码
增量、输入清单、config、镜像及专属报告，guest 日志按各自 run ID 目录保存。
不归档 `.git`、模型文件、FM payload 目录或完整容器镜像。归档需校验 gzip、成员
清单、远端与本地 SHA-256，权限设为 0600；验证前不清理已有日志和容器。
本节未关闭 AM1 全部出口，更未关闭 AM2/AM3/AM4 或正式发布验收。

本批本地提交：kernel `d7898f5e0b682d5af7ff76de9a8592c717f544b7`，
mem_service `3c60206d9ac0387c0d487461aa9e308744cb5013`。
VENDORED 来源记录已更新到该实际内核提交，两个头文件 SHA 保持与构建源相同。
随后 r120 完整服务套件及独立 listener 再次得到 232 通过、4 项环境跳过；
root 460 项，441 通过、5 跳过及同样 14 项既有失败，43.617 秒；
layout 72/74 通过，1.25 秒。没有执行 push，父仓库 gitlink/lock 均未更新。

第二目标的 `build-tests-r120.tar.gz` 已归档 70 个精确成员，1171724 bytes，
SHA-256 为 `18b51dc7a2d91e7dc37ee8b5683ac4e6fdf4a79931f80de2959a82d4e018eb67`。
最终 `verified-r111-r120-backing-evidence.tar.gz` 包含该嵌套归档，共 83 个归档
成员，91165071 bytes，SHA-256 为
`8000bf14c0b747099f663b0138054ac8e6d9c8e9f23fc620137496bb3b792f07`。
其中保留失败的 r111 rootfs/日志和通过的 r118/r119、实际 QEMU 二进制、kernel、
匹配模块、配置及源码增量；未清理中间失败证据。归档已取回本地，gzip、成员数、
长度及 SHA-256 均与远端一致，权限为 0600。

## 2026-09-13 续作：并发分配压力契约（局部实测通过）

新增配置场景 `MEM_SERVICE_OBJECT_SHARE_FAULT=allocation_pressure`，仍复用现有
双节点入口，仅执行一轮；使用 64 MiB aperture。三个独立 `object-session` 进程
通过 SDK 请求 1、2097153、4194305 bytes，alignment 分别为 2、4、8 MiB。
它们各自重放同一幂等分配请求，并等待自身对象退役；协调流程确认三个进程仍
存活、两次分配结果均已输出后，查询全部对象、真实 kernel segment 和资源统计。
验证不依赖分配先后顺序，也不把进程同时存活等同于服务器并行执行 RPC。

原有两个客户端保持同一对象的 CPU mapping。新增对象按交错顺序退役，先释放
中间大小的对象，再分配一个 3 MiB/4 MiB-aligned 对象，核对未提前重用地址。
压力结束后，服务和物理池 used 计数回到压力前状态，原对象继续双向校验，最后
完整 kernel export/import 清单回到启动基线。并发子进程输出先分别保存，结束后
按客户端输出并保留退出码；异常退出时回收本流程持有的精确子进程 PID。

此用例验证并发服务请求、真实 backing 和区间几何；新分配对象未逐一执行 payload
访问，CPU 数据验证由持续持有的原对象提供。它不承担多 home、100 次循环、跨线程
SDK mapping 或 AM3 地址重用验收。

r121 已完成真实双 guest 运行，runner exit 0，59.834 秒。双方的
`mapping_resources`、`kernel_regions`、`allocation_pressure` 均 PASS；三个实际
客户端进程同时存活并最终全部 exit 0。新增对象的实际起始 VA 分别为
`0x700000600000`、`0x700000800000`、`0x700001000000`，backing 为 2/4/6 MiB。
释放第二个对象后，再次分配得到 `0x700001800000`、4 MiB backing；没有复用孔洞。
五个检查点的物理池 used 为 8/20/16/20/8 MiB，服务账本保持两份 CPU mapping
及两个 live refs；原对象最终 65537-byte 读回 checksum 为 `0xf020110145c7382b`。
新增四个对象各只有一次真实 reserve/export 和一次完整 reclaim 序列。

本地轻量验证 20 项通过，0.763 秒。r121 完整 unittest 为 462 项，443 通过、
5 跳过及同样 14 项既有失败，43.802 秒；layout 首次新增一个 UTF-8 读取错误：
macOS 默认 tar 夹带 AppleDouble 元数据文件，扫描脚本目录时读到了二进制边车文件。
保留该失败归档/日志，使用 `tar --no-mac-metadata --no-xattrs` 重新打包相同五个文件。
r121b 完整 unittest 再次为 443 通过、5 跳过和相同 14 项失败，43.872 秒；
layout 恢复为 72/74 通过，1.22 秒。源码及测试断言未为打包问题修改。
后续传输源码增量统一禁用这两类 macOS 元数据。

本批运行沿用 kernel `d7898f5e0b68`、服务 `3c60206d9ac0` 和 r109/r92 二进制；
根仓库变更基于 `aa656aca72b9`。r121 rootfs SHA-256 为
`72e503627c6091c6e52034ecd13e548661300701f8a0ecdffce9d33d86d7336f`。
证据归档采用 `verified-r121-r122-allocation-pressure-evidence.tar.gz`，保存本批
rootfs、源码增量、配置、输入清单、完整套件及实际 guest 日志；kernel/QEMU/服务
二进制的共同来源引用上一批已核验的 `verified-r111-r120-backing-evidence.tar.gz`。
r122 普通 SDK 两轮 64 KiB 回归已通过，101.035 秒，runner exit 0；双方
`sdk_contract`、`mapping_resources`、`kernel_regions` 均 PASS，QEMU 末尾
read/write error 与 timeout 计数为 0。本批未改变 kernel、服务或 QEMU 源码，
服务完整套件沿用相同 `3c60206d9ac0` 源码的 r120 结果：232 通过、4 项环境跳过。
两轮结束后的独立进程查询均无 QEMU 遗留。最终归档已取回本地，共 31 个成员，
18221563 bytes，权限 0600，SHA-256 为
`786592712f6a1799abaf8a421e2422e3cc9b7a5f6f4a399be65e2ddc182669c1`。
gzip、成员数、长度和 SHA 均与远端一致；归档包含两次源码打包、原始失败与复验
日志、实际 guest 证据和明确的来源清单。四个执行/测试源文件在 r121/r121b
归档之间逐个 `cmp` 相同；r120 与 r121b 的 14 个失败编号排序比较无差异。

## 2026-09-13 续作：home kernel export 身份检查（局部实测通过）

现有 manager CLI 新增 `--check-export-identity --segment-id ID`。实际 query
取得原始 segment 后，分别改变副本的 14 个 descriptor 字段及三个 ioctl 外层字段，
直接执行 checked-export ioctl；descriptor 不匹配须得到 ESTALE，外层格式错误须
得到 EINVAL。每次随后 query 并核对原始 descriptor 完全不变，日志不打印 token value。
所有 export 请求都包含一个不匹配字段，不发送可能分配 backing 的有效 export 请求。
有效重复 export 的拒绝、consumer 导入以及动态轮换仍需独立证据。

诊断接入既有 `backing_capacity` profile，在两端 CPU mapping 保持期间执行。
最终仍需完成原对象读写、实际 backing 分配失败后的恢复和完整资源清单归零。
新增 CLI 定向测试覆盖 17 组调用、非预期 errno、非预期成功、query 失败及身份变化；
它使用边界替身，不作为实际 kernel 验收。轻量资源验证器/网络审计 20 项通过，
0.751 秒；远端 manager 8 项定向测试通过，3.109 秒，静态 AArch64 编译通过。
r123 完整 unittest 为 463 项，444 通过、5 跳过、5 失败、9 错误，45.212 秒；
14 个失败 ID 与 r121b 排序比较无差异。layout 72/74 通过，1.23 秒，失败同前。

r123 实际双节点 `backing_capacity` 用例已通过，runner exit 0，62.042 秒。
真实 home kernel 的 17 组检查全部取得预期 errno，每次 query 的原始身份均不变；
两个 guest 的 `backing_capacity`、`mapping_resources`、`kernel_regions` 均 PASS。
检查期间保持原有 CPU mapping，最后 A 读回 B 发布的 65536 bytes，checksum 为
`0xbaadacabd8bb0383`；独立进程查询确认无 QEMU 遗留。
本批 kernel、worker、服务和 QEMU 源码未变；服务套件沿用 r120 相同提交的
232 通过、4 项环境跳过结果。新增 manager SHA-256 为
`abc22c1aad80fed082f4a4c0cccd1caa6044bc90a7081ac3950cf7283a7f3915`，
rootfs 为 `1df35865eaf355b892cf299cd7051dd009826526a3c454cf4bd6a5998a0e3e84`。
本批证据约定保存为 `verified-r123-export-identity-evidence.tar.gz`，包括实际
rootfs、manager、八文件源码增量、输入/终态清单、完整测试及双 guest 原始日志。
共用 kernel/QEMU/服务的二进制来源仍引用已核验的 r111–r120 归档。
最终归档已取回本地，20 个成员、14900876 bytes、权限 0600，SHA-256 为
`424ca0c9d2134208c1a235bb4163468ea11c7c5544f5481ed9d41e3dd0dd6738`。
本地与远端的 gzip、成员清单、大小及 SHA 核对一致，未清理原始日志或容器。

## 2026-09-13 续作：consumer provider descriptor 检查（局部实测通过）

现有 object-session 增加 `op=probe_descriptor key=<key>`，在已 acquire 的可读
映射上调用 provider 专属诊断。源 descriptor 来自同一 endpoint 持有的真实 handle，
24 组格式、必填身份、对齐与溢出负例直接进入生产 map callback，不走 SDK 绑定
预检查；不提交有效 map 请求。先要求 endpoint 正常、有空闲 slot 和 handle 容量，
每次拒绝后核对 handle 计数和实际资源账本，意外变化保留清理归属并关闭新接纳。
CLI 核对原映射首个逻辑页 checksum；既有 `descriptor_contract` profile 随后继续
全对象双向 CPU 读写和最终 kernel 清单核对。有效格式的旧身份及动态轮换仍属
独立验收范围，不由本诊断关闭。

本地 31 项轻量测试通过，1.809 秒；远端三个 provider/CLI 定向测试通过，
17.332 秒，静态 AArch64 服务二进制构建通过。r124 实际双节点运行 exit 0，
57.678 秒，两端各 24 条 provider 拒绝记录及 session 原映射保留记录齐全。
双方 descriptor/resource/kernel census 校验均 PASS；A 最后读回 65536 bytes，
checksum 为 `0xbaadacabd8bb0383`，独立查询无 QEMU 遗留。

首轮 root 完整回归出现 3 项新增失败：既有 `test_mem_service_sdk_contract_guest`
的样例没有包含新增探针，且配置测试仍要求 readonly 阶段没有操作。补齐完整
provider 记录及对应阶段预期，同时新增缺失/错误探针的拒绝断言；未放宽校验。
r124b 完整 unittest 为 464 项，445 通过、5 跳过、5 失败、9 错误，45.496 秒，
失败 ID 与 r123 排序比较无差异；layout 72/74 通过，1.23 秒，失败仍为原有两项。
原失败日志和 source-r124 保留，修订样例及验证器使用 source-r124b。
八个完整服务分片全部 exit 0，共 237 项，232 通过、5 跳过，最长分片 828.432 秒。
结合独立 listener 补验后为 233 通过、4 项环境跳过；listener 用时 7.904 秒。首次 listener
命令误写类名单复数而未执行测试，保留原错误后用正确类名完成，源码和断言不变。

r125 同一 rootfs 的普通 SDK 两轮回归通过，100.856 秒，两端 SDK/resources/kernel
校验均 PASS；read/write errors 和 timeouts 为 0，独立查询无 QEMU 遗留。
r126 固定地址冲突回归通过，57.203 秒，两端实际 mmap 均返回 EEXIST，原映射
checksum 保持不变，后续完整读写与资源清理全部通过；独立查询无 QEMU 遗留。
新服务二进制 SHA-256 为
`6bc7f12e6c4fb2d5cbefc230c39a2bf9209bddb1ba6b275023f6130c2ed4f3c3`，
rootfs 为 `d11c0a2c81a5f521a723ee26ade516e2561bd8d5f7a38c9f26d63d6c15ec0782`。
服务提交为 `9d8245da9557e918a7cd2877a151fe6e6fdec1d6`，工作区干净。
九个服务文件和 run_app 在 r124/r124b 间逐个 cmp 一致，13 个最终源码/测试文件
SHA 均与远端核对一致。未 push，父仓库 gitlink/lock 继续保持原固定值。
本批归档约定为 `verified-r124-r126-provider-descriptor-evidence.tar.gz`，保存
新服务/rootfs、两次源码增量、配置、输入/终态清单、完整测试和三次 guest 原始日志；
也保留首轮 root 与错误 listener 命令的失败日志。共同 kernel/QEMU/worker 来源
引用 r111–r120 归档，manager 来源引用 r123 归档，不重复归档完整容器镜像。
最终归档已取回本地，51 个精确成员、20872367 bytes、权限 0600，SHA-256 为
`4c82838030445f1c880633017592eda8b5aaa601faed6f67d8b273c3b1bbeb20`。
本地与远端的 gzip、完整成员列表、长度及 SHA 核对一致，原始证据继续保留。

下一数据能力为 AM2 的 CPU/PTO 联合访问。源码检查发现 QEMU 的
`ubc_obmm_resolve_async_map()` 仅查询旧 `SimDecMapEntry`，PTO 的 preflight 和
逐次访问均依赖此结果；严格 GSVA CPU window 使用独立的 route 和 backing token。
因此，PTO 接入必须补全 GSVA-aware 的实际映射解析及完整身份保持，再连接同一
managed holder/mapping 的 CPU/PTO pins。当前 PTO lease 在 backend release 失败
后仍丢弃归属，相关清理路径也须补齐；本段属于源码发现，尚无 AM2 实测通过结论。

## 2026-09-13 续作：严格 GSVA 的 PTO 映射解析（开发验证中）

新增值类型 route 快照及实际 PA 范围解析，保留完整 GSVA key、route ID、lease
epoch、权限、home/owner 和独立 backing token。PTO 延迟授权及逐次访问重新查询
真实 route，身份变化后拒绝旧绑定；严格数据访问复用 CPU window 的 backing I/O。
正常旧映射仍走原路径；异步 read 队列拒绝严格 GSVA，避免使用错误凭证。

r127 原型已通过项目入口构建，6 组 QEMU 原生测试共 39 项通过，包含新增四组
route 行为测试。root 首次因只读 unit 目录缺少挂载文件而未启动，原容器保留。
r127b 修正挂载后完整运行 465 项：445 通过、5 跳过、6 失败、9 错误，45.252 秒。
较 r124b 新增一项旧源码断言失败：generation 比较已移到公共身份函数。修订断言
同时检查调用关系、generation、映射类型及完整 GSVA 比较；尚待最终完整复验。
layout 仍为 72/74 通过。r128 配置已上传，但失败集合比较拦住了 guest 启动；
不能把该配置文件当作已完成的实际运行。首次远端比较因缺少 rg 未执行，随后用
grep 和 pipefail 重做，确实检测到上述新增失败，所有尝试保留审计记录。

guest 接入还暴露出旧注册接口没有传播 VMA 权限。正在补齐 V2 ioctl 的
`OBMM_ASYNC_MAP_STRICT_PTO` opt-in 和 MMIO command 3：driver 从实际 VMA
提取读写权限，QEMU 与 route 权限取交集；旧 command 1 对严格映射拒绝。
EL0 新注册函数不重试旧接口，异常回包仍保留可识别的 registration 清理归属。
r129 已通过新 QEMU 构建及 40 项原生测试；完整 root 为 467 项，448 通过、
5 跳过及原有 14 项失败，45.776 秒；layout 72/74 通过，1.22 秒。EL0 边界替身
实际调用生产注册函数，核对四种错误均只发一次 ioctl、没有 legacy 重试，异常
回包可识别的 registration 可继续注销；该测试未访问真实设备。guest 模块在
第二目标通过构建，使用原 r109 config/Module.symvers，vermagic 同为
`6.6.0 SMP mod_unload modversions aarch64`。随后补充 V1 ioctl 继续拒绝非零
flags 的兼容检查，r130 最终模块构建通过，10.065 秒，vermagic 与 r109 一致。
r130 首次 layout 命令使用错误路径，未执行测试；首次 root 使用另一测试镜像，
保留结果但不作为最终基线。r130b 复用 r129 的镜像及完整挂载后运行 467 项，
448 通过、5 跳过、5 失败、9 错误，45.734 秒。终态后对 14 个失败 ID 排序，
与 r124b 完全一致；layout 72/74 通过，1.23 秒。首次在测试进行中取得的部分
失败集合不作为最终比较证据，最终使用 `failures-r124b-r130b-final.diff`。
r127 原型不作为最终部署物。

最终 QEMU SHA-256 为
`75e551eff25b87874d9e6cb911b9c650c89b22193412a11869ee481cc2c6d6f9`，
r130 驱动为 `474def353211aa40c003ae55d3f59d929b608e58143fcffb48367f4e641c8ada`。
新 rootfs 仅替换 r124 的驱动，服务、worker、manager 和 run_app 指纹保持不变；
SHA-256 为 `6148f3d8f032775a6065857e2fb6cbce68d8c222b46166891dbec52a9c7aea60`。
每次 SSH/SCP 和重试均经过审计 CLI；未改变远端系统网络配置或上传模型。
r131 使用最终 QEMU/driver/rootfs 执行双节点普通 SDK 两轮回归，runner exit 0，
101.102 秒；两端 `sdk_contract`、`mapping_resources`、`kernel_regions` 均 PASS，
末尾 read/write errors 和 timeouts 为 0。本次确认 CPU/SDK 兼容性；未提交 PTO
dispatch，不能将结果记作 A07/A08 或统一寿命验收。
独立查询确认无 QEMU 遗留。最终归档
`verified-r127-r131-strict-pto-platform-evidence.tar.gz` 已取回本地，38 个精确
成员、61516733 bytes、权限 0600，本地与远端的 gzip、成员列表、长度及 SHA
核对一致；SHA-256 为
`36aff6396a4342326bc461798d8a6f42a2e677981a3f8a237e927585ce0f8885`。
归档保留 r127/r130 首次失败、r128 未运行配置、最终二进制、源码和原始 guest 日志。
14 个最终平台源码/测试文件与远端快照逐项核对相同。
QEMU 提交为 `175d95a405b4681078579b4119b6ed30eb542d8e`，kernel UAPI 提交为
`1bd7af7ffc92c8aaad161bf04d0d0cefe045ca0f`；未 push、未更新父仓库 gitlink/lock。
本批仍需接通 managed mapping 的统一 CPU/PTO lease，并取得真实 TLOAD/TSTORE
与 CPU 四种交叉读写证据；本节未关闭 AM2 或任何完整阶段。

## 2026-09-13 续作：PTO lease 的失败清理归属（局部单测通过）

Memory Service/PTO bridge 在调用 backend acquire 前建立 owner，三个 acquire
入口的失败回滚都使用同一 release 状态机。注销失败保留 backend lease 和活动
计数，已清理的 memref/region 不重复销毁；失败 acquire 返回的非空 lease 仅可用于
重试清理。OBMM backend 的实际注销 callback 同样在失败时保留 registration。
Qwen3 操作保留清理失败的 operation，逐项清空已释放指针，避免重试造成重复释放；
准备失败或开始清理后的 operation 拒绝 submit。该修改保持原单线程使用约定，
没有声称完成并发 owner、PTO 超时 completion 恢复或 managed mapping 接入。

r132 以生产 bridge/region/memref 实现覆盖正常 release 失败、三个 acquire 入口
的 backend 部分失败、无效 binding、memref 创建失败和回滚重试。独立 Linux fixture
调用生产 OBMM release callback，EIO/EBUSY 后核对 registration 原样保留，第三次
成功才释放。Qwen3 fixture 验证 prepare/release 重试及清理后拒绝 submit；实际
设备/MMIO 由替身承接，结果属于生命周期单测，未执行模型或真实 PTO dispatch。

本地 10 项轻量检查，9 通过、1 项 Linux 环境跳过，1.306 秒。远端完整 root 为
468 项，449 通过、5 项环境跳过、5 失败、9 错误，46.367 秒；14 个失败编号与
r130b 排序比较相同。layout 72/74 通过，1.23 秒。实际 OBMM callback fixture
和 Qwen3 保留清理归属的用例均通过；12 个源码/测试文件与执行快照逐项一致。
本批没有修改服务、kernel、QEMU 或原 guest rootfs；不沿用 CPU 回归声称 PTO
联合路径已通过。下一步将 managed mapping 接到 provider pin 和 compute lease。
最终归档 `verified-r132-pto-cleanup-evidence.tar.gz` 已取回本地，7 个精确成员、
43374 bytes、权限 0600；gzip、列表、大小和 SHA 与远端一致，SHA-256 为
`652c7997ba1abf5430735a6ad9a1302ec1fbd6da45e368088eb7794963f37e31`。

## 2026-09-13 续作：managed mapping 接入 PTO（r133/r134 局部验证）

OBMM provider 为已存在的严格 import 增加进程内 compute pin，校验真实 provider
归属、mapping handle、逻辑范围及实际 VMA 权限；不额外 import、不建立新 alias。
pin 存在时 unmap/close 保留原映射；endpoint 进入 closing 后拒绝新 pin，已有 pin
仍可释放。该 API 为平台私有接口，不进入 core/wire，保持外部串行化约定。

Memory Service/PTO bridge 新增 `acquire_managed()`，复用 SDK mapping/lifecycle
及 key/generation。平台先 pin 实际视图，再严格注册 PTO；注销失败保留注册与 pin，
pin 释放失败保留清理 lease。未启用该平台构建选项的旧 W5 接口仍兼容，不执行降级。

r133 完整服务 237 项：离线容器 232 通过、5 跳过；补跑独立 listener 用例通过，
最终 233 通过、4 项环境跳过。8 个分组均 exit 0，最长 820.620 秒。
provider 原生 pin fixture 通过，1.613 秒。r134c 的 bridge/OBMM callback/Qwen3
相关 11 项为 10 通过、1 项交叉编译器环境跳过，1.867 秒；新增 fixture 实际调用
生产 adapter，以边界替身验证注册失败、注销失败及 unpin 失败的归属保持。
本地对应 11 项为 9 通过、2 项 Linux fixture 跳过，1.676 秒。

远端 root r133 为 468 项，449 通过、5 跳过、5 失败、9 错误，46.465 秒；
r134c 为 469 项，450 通过、5 跳过、5 失败、9 错误，46.601 秒。两轮终态的
14 个失败 ID 与 r132 排序比较一致。layout 仍为 72/74 通过，r134c 1.24 秒。
15 个实际源码/测试/说明文件与远端执行快照逐项 SHA 核对一致。
r134 首轮误配了 noexec 临时目录和 layout 模块路径；失败日志全部保留，
r134c 复用原镜像、可执行 tmpfs 及正确 pytest 入口，最终结论只引用终态复验。

本批未构建或运行新的 guest rootfs，没有真实 managed PTO 数据结果。
下一项是现有 direct CLI 的 managed-config 模式及同对象两次 callable-1 dispatch，
覆盖 CPU/CPU、CPU/TLOAD、TSTORE/CPU、TSTORE/TLOAD；并发 owner、超时恢复及
AM2 全部出口仍未完成。未 push、未更新父仓库 gitlink/lock。

## 下一批的前置条件与未完成项

r133/r134 最终归档 `verified-r133-r134-managed-attachment-evidence.tar.gz`
已取回本地，29 个精确成员、145584 bytes、权限 0600；gzip、完整列表、长度和 SHA
均与远端一致。SHA-256 为
`6702bbf26ea1b0e2c15ba4bc7ad8adb56bc48a382147b76555d161e7bf577d5c`。
服务 pin 提交为 `e9c7d3db8d51cff6b79cbeec76d26eb8d0aa7f60`，root 接入层提交为
`cc71de12751e4885532260f2d18b838973579f1c`。

r135/r136 新增无模型 managed CLI、仅依赖 managed mapping 的 compute context，
并接入已有双节点 runner 的 `MEM_SERVICE_OBJECT_SHARE_ACCESS=cpu-pto`。
初始子集固定 64 KiB、cross-page 1x64 工件、两次顺序 dispatch。日志校验器核对
两端 CPU 同址、六个严格 GSVA binding、四次 LOAD/两次 STORE/两次 fence、
同一 backing 及前次输出为后次输入、原生 completion 和 kernel 资源回归；
缺失/重复/错误身份、校验和、清理及 staging 计数均由 parser 负例覆盖。

无模型 CLI 在远端原生构建通过，SHA-256 为
`f4fbf76d10f7d921b39124ceef8c7f26560d72ed20229d78f11a88417ff21a77`。
r135 相关 15 项为 14 通过、1 项交叉编译环境跳过，5.976 秒；完整 root 为
473 项，454 通过、5 跳过及原 14 项失败，50.762 秒。r136 相关 20 项为
19 通过、1 跳过，6.065 秒；完整 root 为 478 项，459 通过、5 跳过及原
14 项失败，50.710 秒。两轮终态失败 ID 与 r134c 排序核对相同；layout 均为
72/74 通过，1.23 秒。新的测试覆盖配置只读验证、无模型链接、同映射接入及
日志判据；这些结果还不能证明真实 guest 数据访问通过。

r136 rootfs 仅替换 r130 的 direct CLI 和 run_app，现有 daemon、worker、manager、
driver 指纹不变；SHA-256 为
`4e77276a6669348f5244a5c39a3c0e7d8fd9a33207c38ff4d5614c7d5dfe4628`。
现有 cross-page callable 的九个文件已核对历史 SHA，并用项目 snapshot 工具
复制为独立工件集；指纹 CLI 确认 callable=1，fingerprint=`0xc8bd48ef54c247e5`。
真实联合 guest 验证待执行，AM2 保持未完成；本条不引用旧 legacy PTO 结果。

本地提交已获授权并完成。正式源码锁验收仍需子仓库提交可从远端获取，再同时
更新父仓库 gitlink/lock；当前尚无 push 授权。历史 r19 校验器明确拒绝 dirty
mem_service；提交后旧 lock 与新 checkout 的 revision 不一致同样必须拒绝。
本轮未增加绕过开关，未修改父仓库 pin，也未把已知失败计作通过。

幂等容量耗尽后的正常清理保证、单 home 的真实地址耗尽/拒绝后继续访问，以及已清零
backing 缓存不足后的确认回滚、资源核对和恢复分配均已有通过证据。
三个并发客户端、不同 size/alignment、交错释放及保持原映射的 r121 子集已通过。
home kernel checked-export 的 17 组身份/格式负例、consumer provider 的 24 组
格式/必填身份负例已通过；有效格式的过期身份检查及更长分配循环所需的幂等历史
回收协议仍待实现验证。当前仍保留最多 64 条结果；完成清理后不会自动抹去历史，不能把这些
容量子集当作 100 次循环或无限请求的验收。
已有 endpoint 计量和 registered-region 基线核对覆盖正常清理，崩溃、失联和未知资源
仍需独立验收。后续还包括 AM2 CPU/PTO 联合访问、AM3 恢复/重用、AM4 多 home/W5/
独立发布，以及 n4-910c 的独立复现。总计划 M2-M5 的动态身份、恢复、压力与 replay
联合失效同样未关闭。
正式源码锁恢复通过后，仍需逐项实现和运行验收，不能由 r20/r24 的 CPU 正例推定完成。

## 2026-09-14 续作：managed PTO 首次实跑与逐层修复（单线程子集通过）

r136 实际双节点 CPU 同址映射和输入校验通过，首次严格 PTO 注册返回 EACCES。
OBMM 在设置 PTE 后清空 VMA 的读写标志，原 driver 因而错误拒绝实际可读写映射。
r137 改为持有 mmap/PTE 锁，逐页核对 OBMM file/region 身份、物理连续性及页表权限；
当前只接受页粒度 O_SYNC import，cacheable 和大页视图明确返回 unsupported。
这些视图仍需动态权限寿命协议。13 项 driver 定向测试全部通过，0.533 秒；
模块在第二目标编译通过。提交为 `190befcce6c78f37f246553d4349eaef54e9cd96`。

r137 实跑成功取得三个 compute lease，并通过在用 release 返回 EBUSY 的检查，
首次 dispatch 仍以 `pto_ub_gm_access_denied` 结束。OBMM 的 AArch64 RW PTE
清除硬件 PTE_RDONLY，却不设置软件 PTE_WRITE/DBM；r138 改为检查实际 user/AP
权限，增加软件写位与硬件只读位不一致的 fixture。13 项测试通过，0.547 秒；
提交为 `3ac17bb34dbcb9a51b5b9d28336005084b87ce31`。

r138 实跑建立三个严格 GSVA binding：segment `0xc4c2000000000001`、
home VA `0x700000000000`、epoch/lease epoch 均为 1、实际 backing token 97。
第一次跨页 TLOAD 请求长度 256 bytes，接收端仍只允许 8 bytes，请求被拒绝后
发送端超时；completion 为 `pto_ub_gm_callback_failed`。未产生成功 LOAD/STORE，
不能据此认定 CPU/PTO 数据共享通过。r136/r137/r138 runner 均 exit 1，三轮结束后
分别独立查询确认无 QEMU 遗留。保留全部失败日志，不用旧 CPU 或 legacy PTO 结果替代。

r138 同时修复 serial CRLF 的证据解析：仅对解析文本归一化，SHA 继续基于原始
日志字节计算。先复现拒绝有效 CRLF 的失败，再通过 4 项 parser/CLI 测试，0.075 秒；
提交为 `926561095dfea6bd9a63cbad95f60adb1816a19c`。该结果仅验证日志工具。

r139 使严格 CPU/PTO backing I/O 共用有界分片上限，完整计入请求及响应头部，
保留精确长度、token、translation 与地址溢出检查。生产 receiver 的原生 fixture
覆盖 1、8、256 bytes 和最大合法分片，以及非法长度、跨页地址和失败不传数据。
3 项测试通过，0.145 秒；QEMU 通过项目 wrapper 编译，6 组原生测试共 40 项通过。
QEMU SHA-256 为 `e850e2d57057886442a402e94db2e663d562a26a082ca94571a98d469bd0baf7`。
复用 r138 rootfs，SHA-256 为
`7a6e6703d6101d3188b9a17589a126dd2f7cd25d8761cd5bea071ed992a8a286`。
完整 root r137 为 479 项，460 通过、5 跳过、5 失败、9 错误，51.402 秒；
r138/r139 均为 480 项，461 通过、5 跳过、5 失败、9 错误，分别 50.913/51.005 秒。
终态失败 ID 连续排序比较相同；layout 均为 72/74 通过。完整测试尚未全绿。
r139 双节点运行最终 runner/container 均 exit 0。两端 CPU VA 都为
`0x700000000000`，持有同一 key/generation；六个严格 binding 指向同一实际
segment/epoch/lease epoch/backing token。两次原生 completion 成功，合计
4 次 LOAD、2 次 STORE、2 次 fence，每次 dispatch 读取 512 bytes、写入 256 bytes，
`segment_payload_staging_bytes=0`。第二次 TLOAD 输入地址等于第一次 TSTORE 输出
地址。CPU/CPU、CPU/TLOAD、TSTORE/CPU、TSTORE/TLOAD 的六次 CPU 校验均符合
独立浮点 oracle：C checksum `d602274b27b2bb23`，D 为 `c90bc7e9a551a6b3`。

两个客户端均关闭 mapping、释放 holder，compute leases 归零；home segment 退役，
`live_objects=0`，两端 kernel region 清单回到基线。控制服务 put/materialize 次数
均为 0。独立进程查询无 QEMU 遗留；运行后重新核对输入、源码和九个 callable
工件 SHA 全部相同，fingerprint 仍为 `0xc8bd48ef54c247e5`。
QEMU 修复提交为 `280082d7b7c71a8ac802767a39974e4fd5781805`。
校验结果明确记录 `scope=single-thread-cross-page`、`am2_complete=false`。
这次仅覆盖一个 home、一个对象、两次顺序 dispatch；home 互换、并发 owner、
超时恢复、PTO 权限/越界实际负例仍待完成，AM2、A07/A08 完整出口保持未完成。

最终归档 `verified-r135-r139-managed-cpu-pto-evidence.tar.gz` 已取回本地，
121 个精确成员、101212798 bytes、权限 0600，gzip、完整成员顺序、大小及 SHA
与远端一致。SHA-256 为
`a3b4d9d1e5882e8800ec4427f784b5aaeae87157e33d3dc53d8eca03866dd283`。
归档包含三次失败及 r139 成功的原始 guest 日志、源码增量、配置、完整测试、
driver/rootfs/QEMU 和不可变 callable 工件；原始证据继续保留。共用 kernel、
daemon、worker、manager 的来源仍引用前批已核验归档。未 push 或改变父仓库 pin。

## 2026-09-14 续作：原 PTO completion 的超时恢复（r140 子集通过）

endpoint 保存 pending op ID 和原 CQ cursor。超时后 `wait()` 只等待同一操作，
新 submit 与 checked close 返回 EBUSY，队列保持；错误 op/source、畸形或多条
completion 进入保留资源的协议错误状态，不推进 CQ。取消请求只发送一次，
只有有效原 completion 才解除 pending。时间溢出与取时失败在提交前拒绝。
此实现仍要求调用方串行化 endpoint，尚未建立多线程 shared owner。

managed-config 增加可选 `submit_timeout_ms`，短等待超时后核对三条实际 compute
lease 均拒绝 release、endpoint 拒绝 close，再调用原 op ID 的 wait。原有 callable
及数据判据不变。r140 使用 5 ms submit 等待、100 ms authorization delay 和
120000 ms 恢复等待，两次操作 `20260914151` / `20260914152` 均真实超时，
随后恢复对应成功 completion。两次 C/D CPU 校验通过，runner/container exit 0；
两端 holder/mapping/compute lease 和 kernel region 均完成清理，独立查询无 QEMU。
本次复用 r139 QEMU、r138 driver；仅更新 CLI 与 run_app，未运行模型。

16 项远端定向测试为 15 通过、1 项交叉编译器环境跳过，4.732 秒；完整 root 为
483 项，464 通过、5 跳过、5 失败、9 错误，51.138 秒，14 个失败 ID 与 r139b
相同。layout 仍为 72/74 通过，1.23 秒。原生 CLI 构建通过，SHA-256 为
`a8cdafc24de785b977085c26417853f427c0bf62ef6796b9f7c32adda38a5e35`，
rootfs 为 `cc3f260d10b29fc591627a514d68e889f63b5bf5356afa9d7f50222e38f44889`。
边界 fixture 覆盖 repeated wait、错误身份/来源/格式、重复 CQ、取消后等待及取时
错误保留归属；fixture 的 completion 来自本地测试替身，实际证据引用上述 guest。

后续 review 发现旧 direct consumer 的失败出口仍无条件 finish/unmap，r141 已将
checked close 提前到 metadata/memref 释放之前；pending 时保留资源并报告失败，
不进入旧清理路径。日志校验同时核对 callback 地址、map/generation 及每次
submit/timeout/recover/completion 的严格顺序。r140 仅证明此前源码，最终结果如下。

### 最终源码复验与旧入口兼容（r141–r143）

r141 最终 CLI 构建通过，SHA-256 为
`7cdd26ec381b92c10ea9c991008ebf0b7695482c06a5f6bf60e696032c43cf3d`；
最终 rootfs 为 `52e1f76665c4ae234c597804e679dca2ec2f372812c86535eacca5f4be6e1602`。
17 项远端定向测试为 16 通过、1 项交叉编译器环境跳过，4.646 秒；完整 root 为
483 项，464 通过、5 跳过、5 失败、9 错误，51.085 秒。14 个失败 ID 与 r140
排序比较一致；layout 为 72 通过、2 个既有失败，1.24 秒。完整套件仍未全绿。
轻量 endpoint 原生 fixture 的 ASan/UBSan 检查通过，无诊断；它使用替身 completion，
不充当真实设备证据。更严格的最终 parser 重新检查 r139/r140 原始日志均通过。

- r141 延迟路径：5 ms 初次等待、100 ms authorization delay；原操作
  `20260914161` / `20260914162` 各超时一次，3 条 compute lease 和 endpoint
  均拒绝提前释放，再等待同一 op ID 得到成功 completion，没有再次 submit。
- r142 普通路径：相同最终二进制与工件，不设置短等待或授权延迟；操作
  `20260914171` / `20260914172` 直接成功，没有 timeout/recovery 记录。
- 两轮 managed 运行各自通过同 backing、四种 CPU/PTO 读写组合、C/D 独立 oracle、
  原生指令/completion、零 staging、holder/mapping/compute lease 清理和 kernel
  region 回到基线的全部现有判据，runner/container 均 exit 0。
- r143 使用相同最终 rootfs 运行既有 `lingqu_shmem_pto_direct` 入口，producer 的
  `lanes_verified=1 completion_acks=1`，consumer 的 `status=pass elements=64`；
  runner/container 均 exit 0。该结果只证明旧入口正常路径兼容。

三轮结束后均独立查询确认无 QEMU 遗留，运行后重新核对输入、最终 16 个源码文件、
九个 callable 工件 SHA 全部一致。一次 rootfs 启动请求遭遇 SSH banner timeout，
只读核对确认远端作业未启动后才重试；所有尝试分别进入网络审计，原失败记录保留。
本批未修改服务、kernel 或 QEMU，没有运行模型。当前 endpoint 仍要求外部串行化；
并发 owner、home 互换及真实 PTO 权限/范围负例尚未完成，AM2 完整出口保持未完成。

最终归档 `verified-r139b-r143-pto-recovery-evidence.tar.gz` 已取回本地，89 个
精确成员、26890352 bytes、权限 0600；gzip、完整成员顺序、大小及 SHA 与远端
一致。SHA-256 为
`2820250a83dddb4013aab4c5b609cbe43a4914df25817c5d1feee9c1abd39457`。
归档包含 r139b/r140/r141 完整测试、最终源码、两版 CLI/rootfs、r140–r143 原始
guest 日志及配置、容器配置和前后校验记录。原始材料继续保留，不包含整份网络
审计日志。共用 kernel/QEMU/driver/callable 的来源引用前批已核验归档。

## 2026-09-14 续作：共享 mapping owner 与四线程 CPU/PTO（r144-r148 子集通过）

中立 SDK 新增可选 mapping owner，接管已有 managed mapping/lifecycle 和同一
holder；成功后清空旧句柄，失败保留原归属，不重复 map/acquire。同一 provider
channel 的多个对象共用 access domain 锁；CPU/compute borrow 检查精确范围、
权限及 pin 上限。计算 pin 记录原 operation ID，超时与取消不能完成它，in-flight
时拒绝 release 和可能注销 registration 的 apply。close 停止新接纳，等全部
pin 退出后才 unmap/release；清理失败保留 owner 供重试。所有调用线程 join 后
才 destroy；旧 raw API 继续要求外部串行化。此约定管理寿命，不替代数据发布、
跨进程撤销或通用裸指针的使用纪律。

SDK 安装新增可选 source/header 和 pkg-config 字段，旧三份 SDK sources 及
wire/record ABI 保持不变；调用方显式链接 mapping owner source 与 `-pthread`。
完整包清单同时补入此前漏列的 allocation/provider-directory 公共头；由真实
CLI 生成的 package/release manifests 已同步。最终为 14 headers、4 client
sources、56 installed files，package 长度 9814、checksum `0x9462b0e1`。

根仓库 compute adapter 新增 `acquire_owned`、`lease_begin/complete`，注册与
清理进入同一 domain 锁；提交/等待在锁外进行。失败可返回 cleanup-only lease，
保留 backend/provider/owner pin，不能重新 begin。managed CLI 使用同一 owner
完成主 CPU 访问和两次顺序 PTO；`access_threads=0..8` 控制额外只读线程。
线程在已发布、不再修改的输入 A 上持续执行 load 和独立校验，并确认 close
停止接纳之后每个线程仍完成一次原映射读取，随后 join 和清理。

### 实测结果与验收边界

| 运行 | 实际结果 |
| --- | --- |
| r146 延迟四线程 | 5 ms 初次等待、100 ms authorization delay；原 op `20260914201/20260914202` 各恢复一次，未重复提交。第二次 pending 时 CPU/compute/in-flight pins 为 5/3/3，close 返回 EBUSY、新借用返回 ECANCELED。4 线程完成 285 次输入校验，全部在停止接纳后继续读取 |
| r147 普通四线程 | 同一二进制和工件，无短等待或授权延迟；原 op `20260914211/20260914212` 直接完成，无 timeout/recovery。计算完成后 CPU/compute/in-flight pins 为 5/0/0，仍拒绝关闭及新借用。4 线程完成 183 次输入校验，全部确认停止接纳后的读取 |
| r148 旧 direct 入口 | 同一最终 rootfs；consumer 原 op `1327819274321921` 成功，producer `lanes_verified=1 completion_acks=1`，只证明旧入口正常路径兼容 |

两轮 managed 用例继续通过同 key/generation、CPU 严格同址、同一实际 backing、
4 LOAD/2 STORE/2 fence、C/D 独立 oracle、零 staging 和 kernel region 回到
基线的全部判据。两端 owner 最终 CPU/compute/in-flight/mapping/holder 均归零。
三轮 runner/container 均 exit 0，分别独立检查无 QEMU 遗留；源码、七项运行
输入及九个 callable 文件的运行后 SHA 均保持不变。

实际规模仍为两个 guest、一个 home、一个 64 KiB 对象、四个 CPU 线程、
cross-page 256-byte 视图和两次顺序计算。`owner_verified=true`、
`scope=multi-thread-cross-page`、`am2_complete=false`。多请求计算并发、
home 互换、PTO 权限/范围实际负例和更大分片矩阵仍未完成；A07-A11 整项未关闭。

### 测试、已保留失败和源码组合

- SDK 八线程/双 owner 原生 fixture 完成 800 次受锁回调；权限、bounds、容量、
  adopt 身份、错误 completion、pending release/apply、unmap 失败及 release
  回包丢失重试均有行为测试。SDK 和根适配层的轻量 ASan/UBSan fixture 通过，
  无诊断；这些使用边界替身，真实数据证据引用上面的 guest。
- r144c/r145 完整服务测试先暴露两处漏改的旧清单计数断言（52 vs 56），
  均已修正。失败日志保留；未修改测试期望来掩盖未安装文件。
- r145b 最终服务完整 240 项分 14 组执行，全部容器 exit 0：235 通过、5 跳过。
  77 个 runtime/install 方法的执行清单与原测试清单逐项相同。自网卡监听项另在
  所需网络环境通过（1 项、7.824 秒），总计 236 个不同测试已通过、4 项仍跳过：
  deb 交叉打包工具链缺失，以及三项 standalone 环境缺少 ub_sim 布局。
- r145b 首次清单提取把 Python 输出中的方法名重复拼接，门禁拒绝并未启动
  下游作业；修正提取方式后精确匹配。错误与正确清单都保留，未改原测试日志。
- r145 根适配层/CLI/parser/endpoint 定向 27 项为 25 通过、2 跳过，6.853 秒。
  根完整 487 项为 468 通过、5 跳过、5 失败、9 错误，52.284 秒；14 个终态
  ID 与 r143c 完全相同。layout 为 72 通过、2 个既有失败，1.23 秒。完整根
  测试仍未全绿，未关闭锁定源码/发布门禁。
- r143b/r143c 是上一批提交前文档复验：完整根均为 483 项及同 14 个失败 ID。
  r143b 的归档混入 AppleDouble 文件，layout 额外失败；使用明确禁止 macOS
  metadata/xattrs 的归档参数后，r143c 恢复为 72 通过、原 2 失败。两轮都保留。

服务源码已形成干净提交
`6f090162536f0a92235587b13519306f28f3eaec`，在最终根构建和 guest 前完成。
CLI SHA-256 为 `392f9237f9eafc45607f01f0ac134ea1a0a3c7bd74e433fafc45d808b8e703d6`，
rootfs 为 `04af5fb43e6e2d665050bb33c59a29e310d0e2ec3a985a8a5a4d21f091f70729`。
复用 r139 QEMU、r109 kernel、r138 driver 和 r136 callable；daemon、worker、
manager 指纹未变化。实际使用冻结源码增量验证组合；父仓库 gitlink/lock 未更新，
未 push，不能宣称 fresh clone 已可复现。没有在本地运行模型、QEMU 或完整套件。

归档 `verified-r143b-r148-mapping-owner-evidence.tar.gz` 已取回本地，141 个
精确成员、14908265 bytes、权限 0600；gzip、成员顺序、大小和 SHA 与远端一致。
SHA-256 为 `e87a0b05dae7fc84f9930a76fde88d0e9416d4bc2c6c6daf4e4147ffe7ca7a1f`。
其中包含本批成功和失败的完整测试、源码快照、CLI/rootfs、三轮原始 guest/QEMU
日志与配置、清单提取失败材料、47 个已结束容器的配置/状态及前后校验记录。
原始材料继续保留，未上传或纳入整份网络审计日志；不可变共用工件引用前批归档。
下一项为同一 managed backing 的实际 PTO 权限/范围负例，保持原数据路径和严格
completion/资源判据。完整目标继续进行，尚未达到发布或阶段关闭条件。

## 2026-09-14 续作：managed PTO 预检负例与失败后继续使用（r150 子集通过）

现有 managed-config CLI 增加 `negative_checks=0/1`，默认关闭；host 配置为
`LINGQU_SHMEM_PTO_NEGATIVE_CHECKS`。开启后在两次正常计算之前依次提交四个
受控负例，只修改已准备好的测试 metadata，并复用旧 direct 入口的有界 CRC
重算函数；实际 mapping、provider pin、holder 和 backing 不改变。负例暂不与
短 submit 等待组合，未知或超时 completion 仍保留全部资源并使本次用例失败。

| r150 负例 | 原 operation ID | 实际失败 completion |
| --- | --- | --- |
| 合法编码的错误 mapping generation | `20260914233` | `status=3 pto_ub_gm_bad_memref` |
| 不匹配的有效 requester CNA | `20260914234` | `status=3 pto_ub_gm_access_denied` |
| output 超出注册范围 1 byte | `20260914235` | `status=3 pto_ub_gm_bad_memref` |
| OUTPUT role 配只读 access | `20260914236` | `status=3 pto_ub_gm_bad_memref` |

每次消费原 completion 后才结束 in-flight、注销计算绑定，并核对四个原视图。
共 16 次数据不变检查通过，A/B checksum 为 `388f57be05a10383` /
`2da94366c70fbd83`，C/D 的 NaN sentinel checksum 均为 `3625c1e461d4b383`。
四个负例均在预检阶段结束，没有 binding register、TLOAD/TSTORE 或 fence
数据回调。随后原 owner、key/generation 和 mapping 上的两个正常操作
`20260914231/20260914232` 成功，原 C/D oracle、4 LOAD/2 STORE/2 fence、
零 staging 及两端真实资源回到基线的判据全部通过。

同轮 4 个 CPU 线程执行 795 次只读核对，停止接纳后各线程仍完成原映射读取，
最终 CPU/compute/in-flight pins、mapping、holder 均归零。runner/container
exit 0，独立查询无 QEMU 遗留，运行后所有源码/输入及九个 callable 文件的
SHA 不变。报告为 `dispatches=6 successful_dispatches=2 expected_rejections=4`，
继续明确 `am2_complete=false`。这只覆盖一个 home、一个对象及预检负例；
实际只读 mapping 的 PTO 保护、执行阶段权限、home 互换和多请求计算仍未验收。

`--require-negative-checks` 要求完整四次提交/拒绝/数据核对顺序及同一 mapping，
并交叉检查 QEMU 的八条精确拒绝记录。仅这八条已验证记录被排除出正常成功门禁；
多余 failure、CRC 错误、binding 或数据回调继续使校验失败。保存和散列原始
日志字节，解析过滤不会改写原始证据。配置及 parser 负例均有对应测试。

本地轻量测试共 30 项，22 通过、8 项 Linux/设备边界环境跳过；两份应用 C 源码
的 AArch64 `-Wall -Wextra -Werror -fsyntax-only` 通过。远端 r150 定向 30 项为
28 通过、2 跳过，7.002 秒；完整根 490 项为 471 通过、5 跳过、5 失败、9 错误，
52.451 秒。14 个失败 ID 与 r149 精确相同；layout 为 72 通过、原 2 失败，
1.25 秒。根完整套件仍未全绿，未弱化源码锁或布局检查。

原生 CLI SHA-256 为
`d017c2233fabfa59442c75e710e46658ccee9e418ea368d9aaae8f10f013108a`，
rootfs 为 `f990d96e3fcd8585f8e69ed9d50bccd2c1f33611489b4511477d514e72960e3f`。
同一二进制的 r151 关闭负例开关，以 5 ms submit 等待及 100 ms authorization
delay 恢复原操作 `20260914241/20260914242`，两次完成均未重复提交。第二次
pending 时 pin 为 5/3/3，4 个 CPU 线程共 283 次校验，原数据和资源判据继续
通过，runner/container exit 0、无 QEMU 遗留，源码和工件后验 SHA 不变。
本批未修改服务 SDK、kernel 或 QEMU，也未运行模型、push 或更新父仓库 pin。

r152 以相同最终 rootfs 运行旧 direct 入口。consumer 原 operation
`1327819276288001` 成功，producer `lanes_verified=1 completion_acks=1`；
runner/container exit 0，独立检查无 QEMU 遗留，原始源码、运行输入和 callable
后验 SHA 均不变。该回归确认共享 CRC helper 后的旧正常入口兼容，不扩大 AM2
验收范围。

本批归档 `verified-r150-r152-managed-negative-evidence.tar.gz` 已取回本地：
79 个精确成员、13713714 bytes、权限 0600，gzip、成员顺序、大小及 SHA 均
核对一致。SHA-256 为
`01ee26f767f85679d1136d86a85754cb4efa39d280a36395da70ea87d74a5f76`。
包含源码、完整测试、CLI/rootfs、三轮原始 guest/QEMU 日志及配置、8 个结束
容器的状态和前后指纹。原始材料保留；整份网络审计日志未纳入或上传。

## 2026-09-14 续作：执行权限拒绝后的同映射恢复（r157-r159 子集通过）

`negative_checks=1` 扩展为四个预检负例、两个执行负例，然后继续原来的两次
正常计算。执行负例保持正常 memref metadata/signature，通过工件生成 CLI
`--managed-access-fault-request-id N` 在准确的 request N/N+1 中分别执行
TSTORE 只读输入、TLOAD 只写输出；其他 request 使用正常 quadratic kernel。
manifest 记录两个 request ID，runner 要求它们与 generation+7/+8 对应。
没有全局故障计数器，未改变生产权限检查，也未把 CPU 只读页表纳入本次结论。

### 实际失败定位与修复

- r154 最初同时修改 role/access，实际得到 `pto_ub_gm_bad_memref`，数据回调
  为 0，不满足执行阶段 access-denied 判据。该尝试判定失败并保留原始材料。
- r155 改用 request-scoped kernel，第一个执行负例按预期读取 512 bytes、
  零写入并返回 access-denied；第二个操作暴露
  `record_host_orch_callable: callable_id=0 already registered`。
  Rust 只在整次执行成功后保存注册状态，底层注册已成功、执行已确认失败时，
  下一次请求重复注册相同 ID。r155 整轮仍判失败，没有把部分成功计入验收。
- r156 将注册与执行分别管理：底层注册成功即保存 callable ID，已确认的执行
  错误保留注册；注册失败不占用 ID。参数 signature 纳入缓存键，64 项容量
  达到上限后仍允许复用已有项。新增单测覆盖错误后复用、签名分离、注册失败
  和容量边界。用户可继续用同一工件处理后续请求，无需因已完成的计算错误
  重启 runtime。未改变未知 completion 的资源保留规则。

### 真实双 guest 结果

| 运行 | 实际结果 |
| --- | --- |
| r157 六负例及恢复 | 原 op `20260914273..20260914278` 六次精确拒绝；后两次为执行阶段 access-denied，各注册 3 binding、读取 512 bytes、零写入/零 fence、失败完成后注销。24 次原数据不变检查通过。随后同映射 `20260914271/20260914272` 正常计算成功 |
| r158 普通工件延迟恢复 | 5 ms 首次等待、100 ms authorization delay；原 op `20260914281/20260914282` 各超时并恢复一次，没有重复提交。4 线程完成 263 次校验，原数据、owner 和资源判据通过 |
| r159 旧 direct 入口 | 相同最终 CLI/rootfs/桥接，普通工件；consumer 原 op `1327819278909441` 成功，producer `lanes_verified=1 completion_acks=1` |

r157 四个 CPU 线程完成 1174 次只读校验，停止接纳后各线程仍读取原映射。
报告为 8 dispatch、2 successful dispatch、6 expected rejection、2 execution
rejection、8 LOAD、2 STORE、2 fence。全部 payload staging 为 0，C/D 独立
oracle 正确，两端 CPU/compute/in-flight/mapping/holder 归零，kernel region
回到基线。三轮 runner/container 均 exit 0，独立检查无 QEMU 遗留；源码、
运行输入和 callable 的后验 SHA 不变。r154/r155 失败现场由各自隔离 runner
终止，日志及输入继续保留，后续运行使用全新的 run ID、incarnation 和 FM 目录。

本轮仍为一个 home、一个 64 KiB managed 对象、cross-page 256-byte 视图、
四 CPU 线程与两次顺序正常计算，明确 `am2_complete=false`。只读 mapping、
home 互换、更多分片及多请求计算并发仍未完成，A07-A11 整项未关闭。

### 验证与源码组合

r156 通过项目 wrapper 重建 QEMU，六个底层测试程序合计 40 项通过。
Python 定向 34 项为 32 通过、2 环境跳过，7.421 秒；完整根 494 项为
475 通过、5 跳过、5 失败、9 错误，53.557 秒，14 个失败 ID 与 r153 精确
一致。layout 为 72 通过、原 2 失败，1.24 秒。完整根套件仍未全绿。

Rust 完整工作区使用 `cargo test --offline --locked --workspace --no-fail-fast
--jobs 8`，75 个依赖包均来自现有缓存，并逐个核对 Cargo.lock checksum；
测试容器断网。r156e 的 43 个顶层测试目标共 931 通过、32 失败、16 ignored，
没有重复累加隔离子进程的测试结果。两项新增注册回归通过。失败涉及缺失
PTO-ISA 工件准备环境，以及无 Git 元数据的 mem_service 冻结源码。它们继续
作为未通过项保留，没有改为 skip 或弱化检查。

r156g 以父提交 `27ec9dea6e81fcec2d4cdc5d59ffbe06601c2b6b` 的同一批文件覆盖
相同环境，并在独立 build cache 清除两个受影响 crate 的产物后重新编译。
父版本为 929 通过、同 32 失败、16 ignored；当前版本增加的两个通过项恰好
是新增注册回归，32 个失败 ID 完全相同。首次 r156f 复用了当前版本的旧缓存，
仍执行了新增测试，该轮不能作为父版本基线；错误对比与修正后的结果都保留。
Cargo 路径、离线缓存缺包等前置失败同样留档；75 个完整缓存包校验完成后才
形成上述全套测试结果。

CLI SHA-256 为 `88858bfd88af7a16e87e85f97837695827b5408987664ccb1b686761ea1558d1`，
rootfs 为 `e36ca2f5902f20520c5aa46454c0190596e26d7d0ea4de8c03a4bf7e31758c62`。
r156 QEMU 为 `2f13da08412bc75b106f041320779fb162ebe63b6f97c5b13ce67d97bd16e7e2`，
Rust staticlib 为 `b587ffa5fe5dccbdf3b17e8397b66142a883255a8eb5cedc1bf63300a4cf767a`。
r155 专用工件 manifest 为
`ab212c11705b43c9424ba1e667e841382837f1a33b5e4ade82527afad91cdd0b`，
fingerprint 为 `0x5a1f3644ea189c93`，两个故障 request 为 20260914277/20260914278。
普通回归复用 r136 工件。kernel r109、driver r138、服务提交
`6f090162536f0a92235587b13519306f28f3eaec` 保持不变；没有运行真实模型权重。
本批未 push、未更新父仓库 gitlink/lock；冻结增量组合保持开发验证属性，
不能宣称 fresh clone 或发布验收已经通过。

归档 `verified-r154-r159-runtime-recovery-evidence.tar.gz` 已取回本地，192 个
精确成员、112252470 bytes、权限 0600；gzip、成员顺序、大小及 SHA 均与
远端一致。SHA-256 为
`1ef58c83a1bed774e551e2d42f2e4b17c12c9df519a3b1c63bfcf8bebf4ac099`。
其中包含五轮成功/失败的完整 guest/QEMU 日志、两版 CLI/rootfs、新桥接及
staticlib、专用 callable 与生成源码、完整测试、父版本对比和 26 个结束容器
的配置/状态。原始材料继续保留；整份网络审计日志未上传或纳入归档。
下一项为真实只读 managed mapping 的 CPU/PTO 权限与寿命验证。

## 2026-09-14 续作：只读输入与独立输出的联合访问（r162c-r165 子集通过）

managed CLI 新增 `readonly_inputs=0/1`，host 配置使用
`LINGQU_SHMEM_PTO_READONLY_INPUTS`；默认仍为原单对象模式。开启后主 key
保存输入，`<key>-output` 保存结果，两者通过现有 SDK 分别分配、acquire/map
和接管 holder。consumer 输入只请求 READ，输出请求 READ|WRITE；两 owner
共用同一 access domain，各自管理 CPU/compute/in-flight pins 和清理状态。
调用方可保持输入只读，同时让 PTO 把结果写入独立对象，随后直接 TLOAD 原结果。
没有新增 transport 接口，没有向输出复制 CPU oracle，没有加载模型权重。

CPU fault probe 位于 `lingqu_shmem_pto_cpu_probe.h`，在映射所属进程、工作线程
启动前执行；临时信号 handler 只接纳目标地址的同步 SIGSEGV/SIGBUS，并恢复
原 handler。OBMM VMA 不支持 fork 继承或 mprotect，本用例直接创建只读映射。
SDK CPU/compute WRITE borrow 同时要求 `-EACCES` 且不产生 pin。计算前后对
整个 64 KiB 输入比较 checksum，包含未参与运算的区域。验收器使用
`--require-readonly-inputs --access-threads 4`，分别核对输入/输出权限、对象
generation、CPU 地址、每个 binding 的 backing 和每个 owner 的关闭顺序。

### 实际失败与修复

r162 已完成只读 CPU 故障、六个 PTO 负例及两次正常计算，但输出 owner 的
关闭探针把预期新借用拒绝 `-ECANCELED` 继续作为函数错误返回。consumer 保留
资源并报 `rc=-125`，producer 等待 holder drain 后超时；整轮 runner exit 1。
失败场景的源码、二进制、完整 guest/QEMU 日志均保留，没有计入通过结论。

修复在确认预期拒绝后清除该局部返回值，保留所有 close、pin、错误传播检查。
新增原生 fixture 直接编译应用实现，内部覆盖普通/pending 的单 owner 与双 owner，
以及错误 close、inspect、admission、异常 lease、pin 计数和工作线程错误。
相同 fixture 对 r161 源码触发 `probe_owner_close(input, pending) == expected`
断言，对修复后源码通过。fixture 初版未初始化替身的输出 view，被 `-Werror`
拒绝；补齐初始化后通过，未禁用警告。首次旧源码验证任务的封装语法错误另行
保留，真正的旧源码断言由 r162d 独立任务确认。

### n4-910c1 实际双 guest 结果

| 运行 | 实际结果 |
| --- | --- |
| r162c 只读双对象、六负例及恢复 | 输入/输出对象 generation 为 1/2，VA 分别为 `0x700000000000` / `0x700000200000`。CPU 写入在 `0x700000000f80` 收到 signal 11、code 2；CPU/compute 写借用均返回 -13。六次精确 PTO 拒绝、24 次数据不变检查后，原 op `20260914271/20260914272` 正常完成；4 线程完成 4922 次校验 |
| r163 只读双对象、迟到 completion | 普通工件、5 ms 首次等待、100 ms authorization delay；原 op `20260914311/20260914312` 各恢复一次。第二次等待时输入 owner 保留 1 个 compute/in-flight pin，输出保留 2 个，均拒绝关闭及新借用；4 线程完成 4058 次校验 |
| r164 默认单对象回归 | `readonly_inputs=0`，同一最终 CLI/rootfs/runner 通过六次预期拒绝及两次正常计算，4 线程完成 1185 次校验，旧单 owner 判据保留 |
| r165 旧 direct 入口 | consumer 原 op `1327819281530881` 成功；producer `lanes_verified=1 completion_acks=1`，旧入口 runner exit 0 |

r162c/r163 的完整输入 checksum 均保持 `d63638b5e3d6ed83`。各对象的 CPU/PTO
使用对应的同一 backing，输出 C 的 TSTORE 地址等于下次输入 C 的 TLOAD 地址。
r162c 为 8 dispatch、6 expected rejection、2 execution rejection、8 LOAD、
2 STORE、2 fence；r163 为 2 dispatch、2 timeout recovery、4 LOAD、2 STORE、
2 fence。payload staging 均为 0。输入/输出 owner 依次在已有访问结束后 drain，
output 先释放、input 后释放，最后销毁共同 domain；两端 kernel region 回到基线。
四轮成功 runner/container 均 exit 0，逐轮独立检查无 QEMU 遗留，运行输入、源码
及工件后验 SHA 不变。r162 失败 runner 结束后同样确认无 QEMU 遗留。

### 完整回归与交付边界

r162d Python 定向 40 项：38 通过、2 环境跳过，8.202 秒；完整根 500 项：
481 通过、5 跳过、5 失败、9 错误，53.667 秒，14 个失败 ID 与 r160 完全一致。
layout 为 72 通过、原 2 失败，1.26 秒。Rust 完整工作区仍是 43 个顶层目标，
931 通过、32 失败、16 ignored；失败集合与重新编译的父版本 r156g 精确一致。
完整套件未全绿；原有源码锁、缺少 Git 元数据和 PTO 工件准备环境的失败继续保留。
所有完整套件与实际 guest 运行均在远端，测试容器使用缓存镜像和离线依赖。

最终 CLI SHA-256 为
`444dbda3fd45145fd640bfab4609c8044492d66252e40bbeb89477ce4863c07e`，
rootfs 为 `ee36cdddc98d3953ea4fc0674dfbb9a3d212b96be438f3f97109e40a16d7a278`。
QEMU r156、kernel r109、driver r138、服务提交
`6f090162536f0a92235587b13519306f28f3eaec` 与前一轮一致；专用 r155 和普通 r136
工件保持各自原 SHA。未 push，未更新父仓库 gitlink/lock，不宣称发布或 fresh-clone
验收完成。全部网络操作按任务规则登记；没有上传整份网络审计日志。

本轮扩展 A09/A11 的只读权限与双 owner 子集。范围仍是两节点、一个 home、
两个 64 KiB 对象、256-byte cross-page 视图及两次顺序正常计算，
`am2_complete=false`。下一项是更大实际分片；home 互换、多请求计算并发、
现有全局对象引用统一绑定，以及 AM1/AM3/AM4 的其余出口仍待完成。

归档 `verified-r161-r165-readonly-evidence.tar.gz` 已取回本地，141 个精确成员、
35302902 bytes、权限 0600；gzip、成员顺序、大小及 SHA 均与远端一致。
SHA-256 为 `fe85145a5d9071eda8529e7a37bfb6ef9f6ffc9bf16d025ab251eb404bf01c62`。
归档包含首轮失败与四轮成功的原始日志、两版 CLI/rootfs、源码快照、完整测试、
修复前后的原生回归、23 个结束容器状态以及前后指纹。原始材料继续保留。

## 2026-09-14 续作：大张量分片与 quadratic 编排修复（验证推进中）

managed CLI 沿用原入口，增加 `layout=cross-page|nd|tail`，省略时保持原行为。
可选 `elements`、`size_bytes` 分别作为精确一致性断言；错误组合在连接前拒绝。
host 配置使用原有 layout/elements/size 字段，guest 启动前再次校验相同组合。
ND 为 `128x128`、65,536-byte 张量；tail 为 `128x127`、65,024-byte 张量，
tile 仍为 `128x128`。大尺寸每对象逻辑大小为 262,144 bytes，四个视图不重叠；
原 cross-page 的 65,536-byte 对象和四个跨页 256-byte 视图保持不变。
只读模式分别管理输入/输出对象，CPU oracle、负例和 completion 均计量完整张量。
这些逻辑尺寸与 OBMM 按 2 MiB 分配的物理 region 分开记录。

### 实际发现与修复

- r169 在启动前暴露 `run_app` 遗留的 65,536-byte 固定尺寸条件。补齐三种组合的
  preflight，并用可执行 shell 合同覆盖 21 个有效/无效配置；保留原失败运行。
- r169b 的传输和 fence 成功，CPU 输出校验失败。原 quadratic 示例仅按
  `a.shapes[0]` 分配中间张量：`128x128` 输入只申请 128 个元素，内核访问
  16,384 个。仓库生成器现按 manifest 的完整 storage extent 分配，包含 stride
  间隙；输入逻辑尺寸和 FP32 类型须匹配，四个 add/add-scalar/add-scalar/multiply
  task 保持不变。六种布局的原生元数据 fixture 验证三种 rank、依赖、标量和
  八种错误入参；退回旧分配逻辑后会触发尺寸断言。r175 的实际编译还校正了
  Simpler `get_ref` 的 lvalue 寿命要求，替身同步此约束。计算修复已提交为
  `33ed611e1fb95422105c3f4f4031c1e86737aafe`。
- r176 两端数据、只读输入与资源回收通过，原 runner 因分片解析失败 exit 1。
  原日志还包含启动阶段 legacy bootstrap 的 `4079+17` 分片，旧校验器将它们
  套入 strict PTO 的 4055-byte 上限。r174c 按实际 register/unbind 窗口计量，
  单独记录非 PTO bulk send；窗口外访问 managed backing、重放、窗口缺失、
  缺片和精确长度错误继续拒绝。原报告和完整原始日志保留，未覆盖为成功。

### 已核实的 ND 运行证据

r176 在 n4-910c1 运行两个 guest、两个 256 KiB 逻辑对象和四个 CPU 读取线程。
home 与 consumer 均核对完整 C/D，checksum 分别为 `5ea1f9e7e38ac0b5` /
`f97716c2ca962900`；完整输入 checksum 前后均为 `61876dc76aebee83`。
CPU 写保护收到 signal 11、code 2，SDK CPU/compute 写借用均返回 -13。
四线程完成 148 次完整输入校验，均在 owner sealed 后继续完成检查再退出。
两端所有 owner、holder、compute pin 及 kernel region 回到基线。

独立断网容器用 r174c 校验器读取未改动的三份原始日志，exit 0，报告
`managed-pto-revalidated-r176-r174c.json` 为 pass：4 LOAD、2 STORE、2 fence，
共 68 个读取分片和 34 个写入分片；读取 262,144 bytes，写入 131,072 bytes。
每次 callback 均为 16 个 4055-byte 分片加 656-byte 尾片，逐片请求/应答、
backing、地址和 peer 一致，staging 为 0；另有 17 次非 PTO bulk send。
原 runner exit 1 的事实保留，此处结论来自明确标识的独立原日志复核。
独立进程查询确认无 QEMU 遗留；所有运行输入、源码及工件后验 SHA 不变。
修复前后 ND 的三份生成内核源码完全相同。

r174c 代码快照的 Python 定向 89 项为 87 通过、2 环境跳过，15.203 秒；
完整根 510 项为 491 通过、5 跳过、5 失败、9 错误，60.210 秒。14 个失败 ID
与既有基线精确一致。layout 为 72 通过、原 2 失败，1.27 秒。Rust 43 个顶层
目标为 931 通过、32 失败、16 ignored，失败集合与重新编译的 r156g 父版本
一致。完整套件未全绿；没有将基线失败改为 skip。所有完整回归均在远端执行。

CLI SHA-256 为 `ec5ba009f9777fb615218ed5571d4dd59516ca90c4bf45dc1c4b73be168a8e52`，
rootfs 为 `71e758ec76a5c52c1c7e106b31869766dfa6c463988c59f275abf54922978142`。
ND manifest 为 `06a3fae59983acc71a7f5ee681c13de382851227a335c947fc15b53cc09b7d01`，
fingerprint 为 `0x6d3ee0deb45753db`。QEMU r156、kernel r109、driver r138、
服务提交 `6f090162536f0a92235587b13519306f28f3eaec` 保持不变；未改 Simpler
算子，未加载模型，未 push 或更新父仓库 gitlink/lock。

### 大尺寸权限矩阵与原日志复验

r177b 的 ND 六负例整轮 runner/container 均 exit 0。该轮关闭后台 CPU 读取线程，
单独核对权限矩阵：六次精确拒绝、24 次完整 65,536-byte 视图不变检查，随后原
operation `20260914471/20260914472` 完成两次正常计算。两个执行权限负例
`20260914477/20260914478` 各读取完整 A/B 后拒绝，未写入、未 fence；C/D、
完整只读输入和 CPU 写保护沿用 r176 的结果及判据，两端资源回到基线。
实际 8 LOAD、2 STORE、2 fence，共 136 个读取分片、34 个写入分片，读取
524,288 bytes、写入 131,072 bytes，10 个 callback，staging 为 0；另有
15 次非 PTO bulk send。运行前后输入、源码和工件 SHA 不变，无 QEMU 遗留。

r177b 的 QEMU 原日志为 1,575,289,291 bytes。r181 优化校验器对无关 scalar I/O
的元数据重复解析，保持权限、完成、几何、分片和原始日志 SHA 判据。
新增 spy 用例确认 scalar I/O 不进入 PTO 元数据解析，原 136/34 分片计数仍须满足。
首次两项 r181 复验因容器没有 `/usr/bin/time` 在执行校验器前 exit 127；保留日志，
改用已有 bash 计时的 r181b 两项均 exit 0，耗时分别为 65.175 / 123.878 秒。
它们对 r176/r177b 的完整原日志重新校验，只规范化报告中的文件路径字符串后，
所有报告字段与先前结果完全一致，包括原始文件 SHA。未改写原日志或失败报告，
这些耗时也不作为受控性能对比。

r181 运行代码快照的 Python 定向 90 项为 88 通过、2 环境跳过，15.031 秒；
完整根 511 项为 492 通过、5 跳过、5 失败、9 错误，59.899 秒。14 个失败 ID
与基线精确一致；layout 为 72 通过、原 2 失败，1.28 秒。Rust 仍为 43 个顶层
目标、931 通过、32 失败、16 ignored，失败集合与重编译父版本 r156g 完全相同。
完整套件未全绿，未弱化或跳过失败项；源码快照 SHA 在本地与远端均核对一致。

### Tail 四线程迟到完成恢复

r178 使用 `128x127` 张量、原 `128x128` tile 及两个 256 KiB 逻辑对象，
runner/container 均 exit 0。原 operation `20260914491/20260914492` 在首次
5 ms 等待超时后分别恢复，未重复提交。第二次等待时输入 owner 保留 1 个
compute/in-flight pin、输出保留 2 个，关闭和新借用均拒绝。四个 CPU 读取
线程完成 144 次完整输入检查，每个线程在 sealed 后继续校验，再全部退出。

home 和 consumer 的完整 65,024-byte C/D checksum 分别为
`8c89fd43a4864ee0` / `d642f2dc24df838c`；整个只读输入在计算前后保持
`29a268eceafb0343`。原进程 CPU 写保护、SDK 写借用拒绝、各 binding 对应的
backing、零 staging 与两端资源归零全部通过。4 LOAD、2 STORE、2 fence 共
68 个读取分片、34 个写入分片，读取 260,096 bytes、写入 130,048 bytes；
每 callback 的 16 个 4055-byte 分片和 144-byte 尾片均有精确请求/响应证据。
另有 15 次非 PTO bulk send；所有输入、源码、工件后验 SHA 不变，无 QEMU 遗留。

r179 的默认 cross-page 单对象回归同样 runner/container exit 0：六次精确拒绝、
24 次 256-byte 视图不变检查后，两次正常计算通过；4 线程完成 1,176 次检查，
每个线程在 sealed 后继续校验。8 LOAD、2 STORE、2 fence、零 staging 和
两端资源归零保持原判据，所有输入/源码/工件 SHA 不变，无 QEMU 遗留。
r180 的旧 direct 入口 runner/container 均 exit 0：consumer 原 operation
`1327819297914881` 成功，producer `lanes_verified=1 completion_acks=1`，
import/export 正常释放。该轮使用同一最终 CLI/rootfs 和重建的普通 cross-page
编排；所有输入、源码、工件后验 SHA 不变，无 QEMU 遗留。

大尺寸正常、负例及四线程迟到完成的组合已通过。home 互换、多请求计算并发、
全局对象引用绑定及后续阶段仍待补齐，`am2_complete=false`。

## 2026-09-14 续作：两节点 home 互换与双方向完成恢复

managed CLI 新增 `home_node=0|1`，默认 0；host 配置为
`LINGQU_SHMEM_PTO_HOME_NODE`。选中的 home 实际执行分配、输入初始化、远端
输出核对及退役，另一节点执行 PTO。控制服务保持 node 0，通过已有
`allocation_home_provider` 选择 worker；CPU-only 和 legacy direct 角色不变。
CLI 在等待分配和 acquire 后核对返回的 home provider。实际 node ID、requester
CNA 和原始日志编号保持不变，验收器按角色选择原 nodeA/nodeB 文件，显式核对
home worker 导出、计算节点 GSVA backing segment、requester 及原日志 SHA。

### 实跑发现与修复

- r186 两端 FM/entity 就绪，但串口均为空，600 秒等待后 runner exit 1，迭代
  exit 31。r186b 用相同 kernel/QEMU/CLI/rootfs 开启 QMP，实际 CPU 寄存器
  保留 `Too many boot env vars` 和两端 `linqu_node_idx` 文本。对照实际内核
  `unknown_bootoption()`，新增的下划线 home 参数消耗 init 环境槽位，触及
  32 项限额，在服务启动前 panic。诊断证据保留后主动停止该容器，exit 137；
  两轮均确认无 QEMU 遗留，未将诊断退出写成通过。
- r190 改用内部 `lingqu_shmem_pto.home_node`；带点的应用参数由 `run_app`
  读取 `/proc/cmdline`，不占未知参数转交 init 的槽位。host/native CLI 字段
  保持原接口，kernel 限额不变。新增轻量回归直接编译仓库内核参数接纳函数，
  旧写法在边界触发失败，新写法通过；真实 guest 参数解析覆盖 home 0/1。
- r186c 恢复正常启动，双方完成数据与资源检查，但原 runner exit 1：校验器
  将 `obmm-worker starting provider refresh ...` 等普通提示当成结构化行。
  r190a 仅对 export 事件应用导出合同；格式损坏、重复字段、缺失/重复导出、
  segment、home 和 requester 错配继续拒绝。新增 mixed-log 回归覆盖两方向。
  独立断网容器读取未改动的三份原始日志，exit 0；独立报告为
  `managed-pto-revalidated-r186c-r190a.json`，原失败报告和日志保留。

### 实际运行结果

所有运行均在 n4-910c1 的两 guest 环境，cross-page 每视图 256 bytes，每对象
逻辑大小 65,536 bytes。两个正常计算的 C/D checksum 为
`d602274b27b2bb23` / `c90bc7e9a551a6b3`；只读模式的完整输入前后均为
`d63638b5e3d6ed83`。下表的“线程检查”统计 CPU reader，正常 PTO 仍为两次
顺序 dispatch，不将其解释为并发计算吞吐。

| 运行 | 实际 home / compute | 模式与结果 | 线程检查 |
| --- | --- | --- | --- |
| r186c / r190a 复核 | 1 / 0 | 两对象只读输入、两正常计算，原日志独立复核通过；原 runner 解析失败保留 | 4 线程、3,962 次 |
| r191 | 0 / 1 | 默认单对象、六拒绝、24 次视图不变、两正常计算；runner/container exit 0 | 4 线程、1,175 次 |
| r192 | 1 / 0 | 只读双对象、六拒绝、24 次视图不变、两正常计算；runner/container exit 0 | 关闭后台 reader |
| r193 | 1 / 0 | 只读双对象、两次原 completion 恢复；runner/container exit 0 | 4 线程、4,015 次 |
| r194 | 0 / 1 | 同版程序的匹配只读双对象恢复；runner/container exit 0 | 4 线程、3,988 次 |

r193 的原 operation 为 `20260914631/20260914632`，r194 为
`20260914641/20260914642`。初始 5 ms 等待超时后继续等待原操作，无重复提交。
第二次操作等待期间，输入/输出 owner 分别保留 1/2 个 compute 与 in-flight pin，
关闭及新借用均拒绝；四线程均在 sealed 后继续检查，然后全部退出。只读输入的
CPU 写探针得到 signal 11/code 2，SDK CPU/compute 写借用返回 -13。
正常组合为 4 LOAD、2 STORE、2 fence；六负例组合为 8 LOAD、2 STORE、
2 fence。两方向实际 export segment 与计算 binding 一致，staging 为 0；
两端 holder、owner/pin、kernel region 回到基线，无 QEMU 遗留。
r191-r194 的输入、源码和算子工件后验 SHA 均不变。

r190a 的 Python 定向 96 项为 94 通过、2 环境跳过，16.397 秒；完整根 517 项
为 498 通过、5 跳过、5 失败、9 错误，61.399 秒。14 个失败 ID 与原基线一致。
layout 为 72 通过、原 2 失败；Rust 43 个顶层目标为 931 通过、32 失败、
16 ignored，与重新编译父版本的失败集合一致。完整套件未全绿。修复前 r184、
r185、r190 的完整回归和失败记录继续保留，未隐藏失败或以 skip 替代。

最终 native CLI SHA 为
`5a5b066303d4c47876b1677dc3af362e8437dd7d9fb21743831c813205fa529e`，
rootfs r190 为 `0abb19cdd0a1dee5113d3cee5a0fd2e5675c258d56e27ebe4f37ecf9d55adf09`。
QEMU r156、kernel r109、driver r138、服务提交
`6f090162536f0a92235587b13519306f28f3eaec` 及 r175b cross-page 普通/负例
算子工件保持原 SHA。本轮不修改服务子模块、不 push、不更新父仓库 gitlink/lock。
完整回归和 guest 运行均在远端，全部网络操作记录到任务审计日志。

该结果推进 A07-A11 的 cross-page home 互换子集，`am2_complete=false`。
ND/tail 反向 home、多请求计算并发、现有全局对象引用绑定、同时多 home 协调，
以及 AM1/AM3/AM4 与原总计划剩余出口继续保留，不能由本轮双节点结果替代。

## 2026-09-14 续作：有界多请求队列及同步恢复兼容回归

endpoint 增加非阻塞 `enqueue/poll/queued_count`。一次成功提交保留原 op ID，
可同时接纳多条请求；容量为 CMDQ 深度减一、CQ 深度减一和 63 的最小值，
已经完成但未被调用者领取的记录仍占容量。完成按原 ID 关联，可批量及乱序到达。
重复、未知、畸形及已领取请求的迟到回放触发持续协议故障；其余请求记录和
队列页保留，close 返回 `-EBUSY`。新提交 ID 严格递增，容量等拒绝不消耗 ID。
门铃通知整个尚占用的命令区间，使等待授权请求的恢复预算包含随后提交的请求。

endpoint 只复制 dispatch slot，不接管 metadata、memref 或 SDK lease 寿命。
调用者必须持续持有这些资源，直到领取对应的有效 completion，并串行化单次
队列接口调用；等待期间释放该锁。同步请求和队列请求有未完成记录时不能混用。
最终 close 前须等待调用线程退出。旧同步提交及按原请求继续等待的接口保留。

### 队列合同及完整回归

新增原生 `endpoint-queue --case` fixture 的七组场景：完成关联、容量、协议
错误、旧同步恢复、迟到回放、参数边界和双调用线程。线程场景执行 100 轮，
每轮首次领取完成前核对两条请求均被保留，随后由本地页夹具注入乱序完成，
逐请求核对原 ID、两线程退出及记录归零；跨轮复用同一 endpoint 以覆盖回绕。
容量覆盖 CMDQ/CQ 深度 2/4/64 的九种组合。ID 上限不允许绕回重用。
这些夹具不运行设备、不产生真实算子结果，不替代 managed 计算并发验收。

本地轻量 endpoint suite 为 5 项通过，包括 AArch64 编译检查。最初畸形错误长度
负例仍使用成功状态，未进入错误信息解码分支；修正为错误状态后通过，生产解码
判据没有放宽。远端 r197 的 ASan/UBSan 七组 fixture 全部通过，容器 exit 0。

r197 的完整回归暴露源码包夹带 macOS AppleDouble `._` 文件：layout 的脚本
遍历读取二进制元数据时出现 UnicodeDecodeError。r197b 打包显式禁用 macOS
metadata 和 xattrs，18 个源码文件 SHA 完全不变；原失败包和日志保留。
未修改测试过滤或忽略解码错误。r197b 完整结果如下：

| 套件 | 实际结果 | 基线核对 |
| --- | --- | --- |
| 相关 Python | 97 项：95 通过、2 环境跳过，16.843 秒 | 新增队列测试通过 |
| 根 Python | 518 项：499 通过、5 跳过、5 失败、9 错误，61.939 秒 | 14 个失败 ID 与原基线逐项一致 |
| layout | 72 通过、2 失败，1.25 秒 | 恢复原 package checksum 与 Git 元数据两项失败 |
| Rust workspace | 43 个目标：931 通过、32 失败、16 ignored | 失败集合与重新编译的父版本逐项一致 |

完整套件仍未全绿。无平台元数据源码包 SHA 为
`335429c7334e0071c1b03e2716cfb28ee90015de9c9da388e256a01d975d3d50`，
18 文件 manifest SHA 为
`4824395a9f4811686243a6a2be558de6e43c966dbf0a8560f71edc4a6c1b4d16`。

### 重建程序的真实同步路径回归

r198 使用该源码重建 native AArch64 CLI，并重新封装 rootfs；现有服务、worker、
manager、driver、kernel、QEMU 和算子工件保持原 SHA。CLI SHA 为
`c6b6719e58770afce481b58cd4fa7942fd9e5fffd10d830347a1a31c8c7b7639`，
rootfs SHA 为
`3c727233fb23e0af8f7c17fa1e5e59790304f873cc84aaedfc88ffc8962c674f`。

r199 在指定远端运行 home 1 / compute 0、只读双对象、四 CPU reader、cross-page
256-byte 视图和两次顺序正常计算，runner/container 均 exit 0。两次初始短等待
超时后恢复原 completion；4 LOAD、2 STORE、2 fence、CPU 数据、只读输入及
owner 判据均通过。四线程执行 4,068 次完整输入校验，全部在 owner sealed 后
继续检查，再退出。输入、源码、算子工件后验 SHA 不变；报告引用的三份原日志
SHA 逐一复核一致，无 QEMU 遗留。

原请求 ID 为 `20260914691/20260914692`，两次恢复均领取相同 ID；第二次等待
期间输入/输出 owner 分别保留 1/2 个 compute 及 in-flight pin，close 和新借用
均拒绝。两端 C/D checksum 分别为 `d602274b27b2bb23` /
`c90bc7e9a551a6b3`，完整只读输入前后保持 `d63638b5e3d6ed83`。最终两端两个
owner 的 CPU/compute/in-flight pin、mapping pending、holder pending 均归零，
两端 CLI 明确报告 mapping 已关闭、holder 已释放、compute leases 为零。

本轮交付队列层基础实现和现有同步路径兼容证据。managed CLI 尚未接入两个独立
计算请求的 metadata/lease 集合、提交门控及完成领取；真实多请求计算并发和
设备并行性均未验收。下一步接入共享 A/B、分离 C/D 输出的两个独立 quadratic
请求，保留原顺序依赖模式。`am2_complete=false`，总目标保持未完成。

## 2026-09-14 续作：共享会话的双请求 managed 计算

managed CLI 新增 `concurrent_requests=1|2`，默认 1 保留原 C→D 依赖链。
模式 2 由两个 pthread 提交独立 quadratic 请求，共享 A/B 输入，在不重叠 C/D
视图输出相同算子的结果。host 的 `LINGQU_SHMEM_PTO_CONCURRENT_REQUESTS`
经带点 guest boot 参数传递，验收器使用 `--concurrent-requests`。支持现有三种
geometry、两种 home 和单对象/只读双对象；当前拒绝与六负例、短等待恢复组合。
多请求故障恢复、设备并行性及完整 AM2 仍待独立验收。

### r204 失败与会话修订

首次实际运行 r204 在提交前返回 `rc=-16`，`completion_unknown=0`，没有队列
提交或 dispatch 记录。原实现每请求调用一次 `open_managed`；驱动的
`linqu_ub_open` 在存在 `active_file` 时返回 `-EBUSY`，第二个 backend 打开
与真实独占合同冲突。该轮 runner/container exit 1，原 guest/QEMU 日志、配置、
源码和工件摘要保留，未计为通过。

修订后主线程仅打开一个 compute/backend context，两个请求借用同一会话，
分别持有 metadata 页、三份 lease、inflight 和原 operation ID。此路径的 strict
mapping 注册/注销经共享 access domain 串行化；不宣称通用 async SQ/CQ 和
future API 支持无锁并发。两个提交完成前禁止领取 CQ，主线程检查六份 lease、
两个 metadata 物理页及全部 owner pins，验证 close/release/admission 拒绝后
允许两调用线程领取各自原 completion。所有线程 join、lease 归零后释放请求
状态，共享会话由主线程统一关闭。未能确认完成时保留资源。

原生 owner fixture 增加驱动独占打开替身、共享会话六 lease 检查，并拒绝错误
会话绑定、0..5/7 个 lease 和重复 metadata 页。验收器增加独立 D oracle、
队列记录/关闭保护/完成/CPU 校验的时序检查，以及多请求 callback 关联；
mutation 测试删除事件、改变 ID、复用 metadata、破坏计数、提前释放和使用
原 C→D 输入都必须失败。软件请求重叠与设备并行执行使用独立字段。

### r205 完整回归与 r206 原生工件

本地轻量测试 45 项：34 通过、11 环境跳过；AArch64 `-Wall -Wextra -Werror`
语法检查和 shell 解析通过。远端 r205 结果：

| 套件 | 实际结果 | 基线核对 |
| --- | --- | --- |
| 相关 Python | 102 项：100 通过、2 跳过，17.537 秒 | 共享会话 fixture 与新增队列验收用例通过 |
| 根 Python | 523 项：504 通过、5 跳过、5 失败、9 错误，62.500 秒 | 14 个失败 ID 与既有基线完全一致 |
| layout | 72 通过、2 失败，1.26 秒 | 两个失败 ID 与既有基线一致 |
| Rust workspace | 43 个目标：931 通过、32 失败、16 ignored | 与重编译父版本的失败集合完全一致 |

完整套件仍未全绿。r205 的 18 文件源码包 SHA 为
`e2d6eebf9a4daef2e747e59389502d8b7b9112c9f4c991e423b022b773d32e6b`，
manifest SHA 为 `f040767adfd9d2af7ae994100feb2245e7e147eca3bba01f0b4afea7b6941197`。
r206 原生 AArch64 CLI SHA 为
`0b6acc148d8668952a3da6ba8dc93508ee2c7823ac5a0c64390471fa8cec40c2`，
rootfs SHA 为 `3e6f85c34a445cbd02c340a7ae08744402e14c3c9d04f46adb167a8d3c13e9e9`。
原 kernel、QEMU、driver、service、worker、manager 和 quadratic 工件 SHA 保持不变。
r211 在同一原生环境以 `-fsanitize=address,undefined` 运行 owner/shared-context
fixture，容器 exit 0，owner 和 shared compute guards 全部通过，无 sanitizer
错误。该夹具只证明局部守卫行为，不替代下面的实际设备数据验收。

### 真实 cross-page 双请求结果

r207：home 1 / compute 0、只读双对象、四 CPU reader；r208：home 0 / compute 1、
单对象、无 CPU reader。两轮 runner/container 均 exit 0，每轮均有两条同时被
持有的请求、两份 metadata、六份 lease，4 LOAD / 2 STORE / 2 fence，独立 C/D
结果及两端 owner 回收全部通过。r207 原 ID 为 `20260914771/20260914772`，
r208 为 `20260914781/20260914782`。每轮 C/D 均为 `d602274b27b2bb23`，
与默认依赖链的 D oracle 明确分离。

r207 四线程完成 3,984 次输入校验，全部在 owner sealed 后继续校验并退出；
只读输入 64 KiB 的前后 checksum 保持 `d63638b5e3d6ed83`。其输入/输出 owner
重叠时分别保留 4/2 个 compute 和 in-flight pin；r208 的共享 owner 保留六个。
完成后两端 CPU/compute/in-flight pin、mapping pending、holder pending 均归零。
原日志 SHA、输入/源码/工件后验摘要通过，无 QEMU 遗留。

报告明确 `queued_overlap_verified=true`、`device_parallel_execution_verified=false`、
`am2_complete=false`。上述结果不关闭多请求故障恢复、全局对象引用绑定、
同时多 home 或 AM3/AM4，也不将两请求的软件寿命重叠计为设备并行性能结果。

### r209 反向 home 的 ND 双请求

r209 使用同一 r206 CLI/rootfs，home 1 / compute 0、只读双对象、四 CPU reader，
每个对象逻辑长度 256 KiB，张量 128×128、64 KiB。两个请求原 ID 为
`20260914791/20260914792`，双端 C/D checksum 均为 `5ea1f9e7e38ac0b5`。
runner/container exit 0，102 个真实分片逐一通过：68 LOAD / 34 STORE，
分别传输 262,144 / 131,072 bytes，覆盖六个 callback 的请求、响应和边界。
17 个非 PTO bulk send 单独记录，不并入请求分片统计。

四 CPU reader 完成 146 次完整输入校验，全部观察 sealed 后继续读取并退出。
两个请求完成前的六份 lease 与 4/2 个输入/输出 owner pins 保留，原 completion
后均释放，两个 guest 的 mapping/holder/pin 归零。完整只读输入及双端结果通过；
原始三日志 SHA、输入/源码/工件后验 SHA 验证一致，无 QEMU 遗留。
中途一次 SSH 状态查询在 banner exchange 超时，已独立登记重试并读取原作业；
未因此重复启动 guest。CPU 逐元素校验耗时不作为算子并行性能数据。

### r210 tail 双请求与本轮边界

r210 使用 home 0 / compute 1、只读双对象、四 CPU reader，张量 128×127、
65,024 bytes。原 ID 为 `20260914801/20260914802`，双端 C/D checksum 均为
`8c89fd43a4864ee0`；完整 256 KiB 输入前后 checksum 为 `29a268eceafb0343`。
68 LOAD / 34 STORE 分片共传输 260,096 / 130,048 bytes，六个 callback 的
实际请求、响应及每个视图末尾 144-byte 分片通过；15 个非 PTO bulk send
单独记录。四线程完成 147 次完整输入校验，全部在 sealed 后继续读取并退出。

runner/container exit 0，六份 lease、原 ID 完成关联、两端 CPU 结果、只读输入
和资源归零全部通过。原日志 SHA 与输入/源码/工件后验摘要一致，无 QEMU 遗留。
首次归档检查发生在 runner 尚未退出时，终态守卫拒绝执行；等待原作业终结后
重新复核，没有重启 guest 或将中间态计为通过。

本轮实际矩阵为 r207/r208 的两方向 cross-page、r209 的反向 ND、r210 的原方向
tail；这些用例均持有两个独立计算请求。tail 反向 home、多请求故障恢复、现有
全局对象引用的 generation 绑定、设备并行性与 AM3/AM4 仍待完成。

## 2026-09-14 续作：全局对象引用 V2 编码基础

独立 mem_service 在保留 V1 的 64-byte 布局与版本常量的前提下，新增固定
256-byte、little-endian 的 V2 编码。新增完整 allocation key、generation、
home/incarnation、逻辑大小与访问权限；offset 明确相对 allocation。header-only
API 不增加 SDK 链接依赖，旧消费者保持 V1；禁止直接修改旧引用版本号升级。
解码拒绝截断、尾随字节、未知版本、非法权限、非规范字符串和范围溢出，失败
保留输出不变；支持未对齐输入及重叠输入/输出。

本地仅运行 4 项轻量测试，全部通过。C 夹具使用冻结 golden bytes，覆盖所有
256 种截断长度、16 个结构字段损坏、权限与溢出边界；CLI 的小写/大写 hex
输入均核对完整身份数值。C++11 头文件编译通过。首次 home 字符替换仍为有效
token，夹具错误地期望拒绝；已改用明确无效的空 home，并保留有效身份替换
可通过结构校验的断言。编码校验不提供服务身份或 payload 内容的证明。

r214 与最终 r215 的远端 artifact 容器均 exit 0，包含原生 codec smoke、完整
`host-artifact-smoke`、实际安装后仅引用安装头的 codec smoke、ASan/UBSan，
以及 CLI 生成的 api-abi-policy、wire-schema、admin-output-schema、release-manifest、
package-manifest 与原清单逐字节比对。最终源码归档 SHA-256：
`54c2c585baccc689d5423073ab3aee6a1051974e290c91d1169c5aebfd93197c`；
206 项源码清单 SHA-256：
`4a015beecd364c1563cfbdb78a00a83a08d90bdef045467aa8b10143ab4fba5c`；
最终 codec 二进制 SHA-256：
`88df99dcbc5833bd88f6cb0e95b885d6fb204bc6676d387403a0e82bd7d96b2e`。
完整 Python 回归 r216 使用同一最终源码的八个隔离分片，全部容器 exit 0。
全部 244 项无重复、无遗漏地覆盖原发现清单：239 通过、5 项环境跳过、零失败、
零错误。跳过项为三项独立仓库缺少 ub_sim 布局、一项缺少 AArch64 deb 打包
工具链、一项离线容器缺少非 loopback 网卡；本轮未将这些跳过项计作通过。
汇总同时核对每分片实际运行数量、完整清单及退出码，摘要 SHA-256 为
`05f7bca21f1bbcf15bd25529db731897f9d1da8dc2e61e58adf393458c64cec2`。
早先串行 r214 current/parent 与 r215 current 在完整分片验证后停止，保留原始
日志与停止前后容器状态；它们仅作为未完成的串行尝试，不计作完整回归通过。

**接入边界：** 当前 allocation version 仍在分配时初始化为 1；既有数据发布
没有同步推进服务端 content version。V2 目前仅冻结编码，未接通权威内容发布、
绑定解析、holder acquire/map 或 W5。有效 generation/home/checksum 替换可以通过
结构校验，必须在后续服务端绑定层拒绝不匹配引用。此批未用于根仓库 guest 构建，
gitlink/lock 保持原状；完整 AM2、AM3 与总目标均未验收。

服务改动已形成干净提交
`503d6907aa299383761103531e9f73d5a0d8a224`，提交前本地 206 项源码 SHA
与 r215 冻结验证清单逐项一致。未 push，父仓库不更新 gitlink/lock。
证据归档 `verified-r214-r216-object-ref-evidence.tar.gz` 包含冻结源码、完整
分片日志、安装/契约验证、汇总及未完成串行尝试的停止前后状态，共 2,041,972
bytes；SHA-256 为
`49a8a6cbfe05f588bdb7fc2d265795afe53c09e83e3b8ed501d253b92b3f35dd`。

## 2026-09-14 续作：V2 服务 core 的内容发布与显式绑定

新增 core begin/stage/seal 与 resolve/acquire，使用同一个 managed allocation
和现有 record table 的专用 managed-view kind。完整 V2 与逻辑 object key 显式
登记；拒绝只按 hash、相同字符串或调用方自报版本取得 holder。V1 projection
明确拒绝 managed-view，避免 allocation-relative offset 被当作旧 arena offset。

begin 要求 ACTIVE provider backing、唯一 owner holder 和零遗留 mapping，
推进 content version 并进入 writing；旧引用立即失效。stage 可登记同一版本
的多个视图，同版本相同内容可重复登记，冲突被拒绝。seal 要求至少一份有效登记
以及全部写入 mapping 完成清理；pending、active、closing 均阻止 seal。失败
不恢复旧内容版本；放弃写入须沿既有退役/释放/回收路径处理。V2 模式拒绝旧 raw
acquire；旧 mapping BEGIN 仅允许 writing 状态的唯一 owner，封存后拒绝。

本地 6 项轻量测试通过，包括既有 codec 的 4 项和新增 catalog 的 2 项。
core CLI 夹具覆盖双视图、12 种格式合法但未登记的身份/视图替换、100 轮内容
版本更新、读 holder 阻止重写、record 容量、冲突、元数据损坏、版本溢出及同地址
数值的新 generation/home/incarnation 替换。测试使用纯元数据 backing 描述，
没有执行实际 kernel 地址重用或读取 payload。CLI 只在全部断言通过后打印 pass。

r218 以该轮独立服务源码启动八个完整 Python 回归分片及 artifact/install/sanitizer
门禁。源码归档 SHA-256 为
`28dc5393d307cee8d013e9de0dbe57c649d666c5fa37e1967525c454faecd8aa`；
208 项源码清单 SHA-256 为
`0ed0d061251b225b23b9857a601813bea5a3614c1da0971c5e20ee4db1ebaa4c`。
r217 仅为先前上传的候选快照，没有启动验证或计作通过；夹具将成功提示移到
所有检查之后，该轮完整验证采用 r218。

r218 artifact 门禁在 `restore-policy-fixtures` 段错误退出；独立 ASan 重建确认
`mem_service_restore_snapshot()` 栈溢出，调用链同时包含夹具的三份完整 service
与恢复函数的临时 service。新增 record 字段增大了这些有界表，引出了大栈帧问题。
修复将生产恢复临时快照改为检查分配失败且所有出口释放的堆对象；单进程 CLI
夹具使用每次显式初始化的静态 scratch。新增 1 MiB 栈下完整恢复夹具回归，没有
提高环境栈限制或删减恢复断言。原失败日志及 ASan 调用栈保留。

r218 原始完整回归共 246 项：240 通过、5 项环境跳过、1 项失败、零错误，
唯一失败为 `test_restore_policy_fixtures_fail_closed_until_commit`；其余分片通过。

修复后的 r219 再次启动完整八分片、artifact/install/core sanitizer，以及
1 MiB 栈下的同一 ASan restore fixture。artifact 与独立 ASan 容器均 exit 0，
完整 CLI、安装态 SDK runtime（包含服务重启）、V2 codec、catalog ASan/UBSan、
五份生成清单比对及低栈恢复全部通过。完整八分片均 exit 0，247 项为 242 通过、
5 项环境跳过、零失败、零错误；执行清单逐项核对，无遗漏或重复。五项跳过仍为
三项独立仓库缺少 ub_sim 布局、一项 deb 交叉工具链、一项离线网卡条件。
最终源码 SHA-256 为
`0efb4a7b014ad7d1e8ce73832aa415d3d196a9dabede0c8f4bdb979ba055e4c0`，
208 项源码清单 SHA-256 为
`dc14d206ae079d8a642171e57b1186b09b07ac4ef5e461183491a5e721862a19`。

**边界：** 以上 API 仅接通串行化 service core。wire 幂等/目录 readiness 门禁、
SDK provider publish 与可见性、V2 reader mapping 和 W5 仍待接入；seal 接受
可信 writer 的元数据，不提供实际 payload checksum 证明。core 内存结构变化
不改变既有 client/provider SDK ABI。record/managed 的重启恢复、独立逻辑 key
的回收以及完整并发/性能矩阵仍未验收，AM2/AM3/总目标保持未完成。

本批服务已提交为 `49ad632b69d5c860656c2c78317f31ff57ee3919`，提交前
208 项本地源码 SHA 与最终 r219 清单逐项一致，子仓库工作区干净。未 push，
父仓库 gitlink/lock 未更新；本批源码未用于根仓库 guest 构建。

`verified-r217-r219-reference-catalog-evidence.tar.gz` 归档保留失败与修复后
源码、完整分片日志、安装/契约测试、ASan 前后诊断和 20 个已结束作业的状态，
大小 2,198,934 bytes；SHA-256 为
`54cb624cd9c156d1c9a6f3921dab3c5789f624a22e05a3a5db78a50e51be11a7`。
最终 247 项完整回归摘要 SHA-256 为
`423ebe4ca88bac76a6940bf60f38eb5da0913b084803b438449d26e3243b9316`。

## 2026-09-14 续作：V2 发布目录的 RPC、安装 SDK 与 CLI

独立服务新增 metadata-only `0x7e reference_transition`，提供 begin、stage、
seal、resolve、acquire；公开 `mem_service_client_reference_transition()` 和
`mem_service_reference_protocol.h`，CLI 入口为 `reference-transition`。
begin/seal 使用 allocation key，stage/resolve 使用逻辑 object key，acquire
核对完整 V2 内的 allocation key。V1 operation ID、64-byte ObjectRef 和既有
808-byte client record ABI 保持不变；新接口显式 opt-in，旧端不支持时拒绝。

本批补齐以下控制边界：

- 幂等回执返回前重新验证 ACTIVE allocation、generation、content version、
  provider readiness 与 home incarnation；resolve/acquire 使用登记的完整引用。
  已释放 holder 的旧 ACQUIRE 回执不能重新取得访问资格。
- BEGIN 预留首次 STAGE 和 SEAL 的回执容量，ACQUIRE 预留 RELEASE。
  表满时，额外 STAGE 在变更前被拒绝；无效请求不占用他人的清理预留。
- 通用 retention 不删除 managed-view 或该协议的幂等记录；旧对象/artifact/
  prefix/block 写入拒绝覆盖 managed-view。
- 公共请求解析拒绝重复/越长/动作不匹配字段、无效整数与非法引用；wire 入口
  拒绝嵌入 NUL，避免尾部字段被忽略。SDK 拒绝不一致的响应，失败时保留输出原值。
- 真实 CLI/SDK/daemon 用例覆盖双视图、版本更新、holder 与映射清理、容量耗尽、
  home 替换、readiness 恢复、UNIX 及受信网络控制端点和原始报文拒绝。

验证过程与保留的失败记录：

| 批次 | 范围 | 结果 |
| --- | --- | --- |
| r220 | 新控制协议定向测试 | 8 项中 3 通过、5 失败；fixture 用 `owner` 写入由 `producer` 分配的对象，按归属规则被拒绝，随后修正 fixture 会话 |
| r221 | 修正后控制协议/core/codec、原生和安装态 | 定向 10 项通过，host artifact 与 installed SDK 门禁通过 |
| r222 | 完整独立 Python 回归 | 256 项，249 通过、5 跳过、2 失败；遗漏了安装文件数与兼容 payload 数的字面量断言更新 |
| r223 | 加入 wire NUL 拒绝后完整回归 | 257 项，250 通过、5 跳过，同两项计数断言失败；新增功能用例通过 |
| r224 | 同步三个计数断言后完整回归 | **257 项，252 通过、5 跳过、零失败/错误**；16 分片执行清单无遗漏、无重复，全部 exit 0 |

五项环境跳过为三个 ub_sim 专属文件契约、缺交叉工具链的 arm64 deb 门禁，以及
隔离容器缺少 non-loopback IPv4 的监听策略用例。跳过保持可见，不计为通过。
完整 suite 在指定远端缓存容器运行，`--network none --pull never`，不下载依赖。
本地仅运行 8 项轻量 codec/core/mock-wire 测试、语法检查与指纹核对。

r224 的 `reference-protocol-smoke`、`reference-catalog-smoke`、
`object-ref-v2-smoke`、`host-artifact-smoke`、`installed-sdk-runtime-smoke`、
`installed-reference-protocol-smoke`、`installed-object-ref-v2-smoke` 均通过。
core catalog 与新 SDK mock-wire 的 ASan/UBSan、1 MiB 栈限制下的 restore ASan
夹具通过。九份 CLI 生成契约逐字比对通过；wire schema 现为 39 operations、
224 fields，安装 manifest 现为 57 files、15 public headers。
启动校验有一条 runner 校验串格式错误，随后以 `sha256sum --strict` 单独核对
冻结 runner 与汇总工具，均通过；补验日志纳入归档。

服务提交 `8ba34a02d80b5a207a51d7215083c7e0990711b4`，tree 为
`80f5b7f3d52c72f4a3a80c5330488b446d4ac443`。提交前 213 项本地源码 SHA
与最终 r224 清单逐项一致。源码归档 SHA-256 为
`8c304b2802aaf029a7380fb6709872d6fb36582482c9faaba60b9f914d9edda2`；
源码清单 SHA-256 为
`dac69d1cb33bc54abd195b10425752a0560cf68a6065bca4c52a6342d699468a`。

证据归档 `out/verified-r220-r224-reference-rpc-evidence.tar.gz` 为 3,780,530
bytes，包含前后源码、完整分片日志、安装/契约/sanitizer 输出及 40 个已结束
作业的状态。归档 SHA-256 为
`b5eda96b0fa539bcf0215fed6ea5280f57b5c60e5681787603c0a5ab54b0b4ab`；
最终完整回归摘要 SHA-256 为
`f8cf3801239eb6be15f44fb59aa99be54f562fccca12654ed065da4a1f18a8e3`。

**边界：** 本批证明当前 daemon 生命周期内的控制协议，不证明 payload 已完成
写入或 checksum 正确。V2 reader mapping、provider 发布/可见性闭环、W5、
AM3 重启身份域及回收仍待实现与实际验证，AM2/AM3/总目标保持未完成。
未 push，父仓库 gitlink/lock 未更新，本批源码未用于根仓库 guest 构建。

## 2026-09-14 续作：严格 provider 固定地址子区间

reader mapping 接入核对发现：生产 OBMM 已能建立子区间的保护视图，但 map
入口拒绝固定地址请求的非零 offset。中立 channel 又要求返回 base 精确等于
requested_address，两者使 V2 allocation 中间视图无法接入。

本批将严格 descriptor 的固定地址检查改为
`requested_address == remote_uba + offset`。完整 descriptor 校验与范围检查
先于加法和资源分配；非严格 descriptor 的固定地址非零 offset 继续拒绝。
映射仍保留完整 backing 地址 reservation，仅开放覆盖视图的页，返回精确的
view base/len；同页内额外字节依赖 SDK 范围检查，不宣称子页 CPU 隔离。

新增 `--fixed-subrange` 定向 fixture，经生产中立 channel 和生产 provider
执行四类视图：页对齐中间区间、非页对齐中间区间、末页尾部和旧全区间。
设备边界由文件 backing 替身承接，使用真实共享 VMA；原 backing 写入后，视图
立即观察到对应字节更新。只读写入和视图外整页读取产生预期同步信号，10 组
无效地址、范围、权限和 legacy 请求在 import/open/mmap/handle 分配前拒绝。
原有部分映射失败、解除失败、endpoint 清理和 compute pin 用例也改用固定地址
非零 offset 请求，继续核对资源归属与最终回收。

验证在指定远端的缓存 AArch64 容器执行，`--network none --pull never`；
本地仅做一次轻量交叉编译、diff 检查与源码/证据指纹核对。

| 验证 | 结果与范围 |
| --- | --- |
| 原生 OBMM provider 完整及定向 fixture | 通过；文件共享视图、页保护、10 组提前拒绝、清理和 pin 生命周期均执行 |
| UBSan 与 host artifact | 通过；无 UBSan runtime error，完整 CLI 夹具通过 |
| 九份 CLI 生成契约清单 | 与提交中的 manifest 逐字一致，未改 wire/SDK 布局 |
| r225 完整 Python 回归首轮 | 选中 257 项，执行 255 项：250 通过、5 跳过；两个 setUpClass 编译超过既有 30 秒期限，另两项未执行 |
| 同源码定向补跑 | C++ header 无链接依赖用例、线程化 mapping owner 用例均通过；源码、断言和超时设置不变 |
| 合并执行清单 | 精确覆盖 257 个唯一 test ID，252 通过、5 环境跳过，无未解决失败/错误；首轮两次 setup error 保留 |

跳过项沿用三个 ub_sim 专属源码契约、缺交叉工具链的 arm64 deb 门禁及隔离
容器缺少 non-loopback IPv4 的监听策略用例，均不计为通过。18 个作业全部终止：
首轮两个 setup-error 分片 exit 1，其余 14 个分片、artifact 与补跑作业 exit 0。
汇总校验明确排除首轮未执行的两个 ID，再接入对应补跑结果，不重复计数或隐藏错误。

服务提交为 `41886ad5a8ad2bd51a0409d1b0edd2a0ca1ffe1f`，tree 为
`9dd9dc9109c82c3400a2108a1f422f357d72d8d7`；提交前 213 项本地文件 SHA
与远端冻结源码清单逐项相同，子仓库工作区干净。源码归档 SHA-256 为
`24a0e6040c37ed06bb05c31f5e5a94d83926ed78f4bd68e6eb675a6f508ef706`；
源码清单 SHA-256 为
`6efbff9825a055e79ec2db63f84ac5a2463fa496b90185c0c2769a2f734b18e1`。

归档 `out/verified-r225-fixed-subrange-evidence.tar.gz` 已回收本地，大小
1,000,294 bytes；37 个 regular members 无重名，36 个 payload SHA 逐项核对。
归档 SHA-256 为
`188880d92c4625bdfdb55a5354489ea4d28e923d1627c9dd38ab375747c01c08`；
合并回归摘要 `out/ref-results-r225-reconciled.json` 的 SHA-256 为
`95e0e76d51703839251de3cf5cc4c1903993cc77e16a041556ed7b95b8197ebc`。

**边界：** 文件替身不构成真实 OBMM guest 的 V2 共享可见性证明。已封存对象的
raw mapping BEGIN 继续被拒绝；下一步须接通完整登记引用、当前 content version、
实际 holder 和活动 home 的 reader mapping 准入，以及 SDK 的精确视图与失败清理。
V2 provider 发布/可见性闭环、W5、AM3 与总目标均未完成。未 push，父仓库
gitlink/lock 未更新，本批源码未用于根仓库 guest 构建。

## 2026-09-14 续作：V2 reader 候选、失败定位与修复验证

本批候选新增 `0x7e` 的 `map-begin` action，服务核对已登记的完整 V2、当前
内容版本、home/incarnation 与实际 holder。pending/active mapping 保留原有
内容换版和 holder release 限制，清理沿用原 mapping 事务；SDK 通过独立
reference lifecycle 保留不确定 BEGIN 的完整引用和 operation ID。CLI 增加
`acquire_reference` 与 `map_reference`，provider 获得精确只读子区间。
以上实现已形成独立服务提交；完整 AM2 仍须由真实跨节点数据链路证明。

r226/r227 的真实 CLI 用例在首个 `acquire_reference` 返回 `invalid_session`。
定位到同一请求先填写 `logical/a` 再覆盖为 `obj-1`，固定宽度 key 的 NUL 后
仍残留旧字节；SDK 规范字段检查在 dispatch 前拒绝该请求。修复为重新清零
acquire 请求，保留原校验。r228 的原失败用例在指定远端缓存 AArch64 容器
执行 97.037 秒，测试及容器 exit 0；覆盖 acquire、固定子区间映射、可见性、
越界/只读拒绝、CPU 页只读探针、unmap/release 和最终映射计数归零。
该用例的 payload 来自匿名 loopback，未证明真实跨 guest 共享。

继续检查发现 loopback range callback 使用完整 reservation 的 base/len。
这与非零子区间的可见性 offset 语义不一致，跨页时会访问前缀保护页。修复后
reservation 仅用于 unmap，数据操作使用独立 view_base/view_len；新增同一
封存 allocation 中 offset 128 和 4224 的两个视图顺序读取。真实 OBMM provider
未改动。中间候选为 r229，源码归档 SHA-256：
`4a823b60fac3f32d643788f2a1c76068836aa659b2742133405ee2fed28b8b77`；
213 文件清单 SHA-256：
`059d32da23666b4505c46b315883fc82e77a1e9b86e347ca590f8eed038db680`。
16 个完整测试分片执行清单精确覆盖 260 项：254 通过、5 环境跳过、1 失败，
无 error。两个子区间的 CLI 访问与清理通过；独立 sanitizer/安装/九份契约
作业 exit 0。唯一失败为 TCP 夹具分配 4096 bytes，但扩展后的引用描述
8192 bytes，服务在 stage 返回预期的身份不匹配拒绝。已将 Unix 分配、引用
构造和 TCP 分配统一使用 `ALLOCATION_BYTES`，未修改服务端身份检查。
最终候选 r230 的原失败用例在指定远端以 9.776 秒通过，完整回归精确覆盖
260 个唯一 test ID：255 通过、5 环境跳过，零失败/错误。跳过项沿用三个
ub_sim 专属源码契约、缺交叉工具链的 arm64 deb 门禁及隔离容器缺少
non-loopback IPv4 的监听用例，均不计通过。sanitizer、安装态 SDK 和九份
契约检查通过，定向作业、16 个完整分片与 artifact 作业共 18 个均 exit 0。
远端实际页大小为 4096 bytes，offset 4224 的视图跨入第二页。
其源码归档 SHA-256 为
`0db08a1f833d9dc9252515cb27de2bcfde6f86b1981a07cab9b4b8ddb492580d`，
213 文件清单 SHA-256 为
`96131ae27ecb7217b0e106956c0b35054180011cfd8e24334ba5fc2a26902319`。
原 r226/r227 的断言失败、编译超时及 r227 单项复验超时全部保留，不计通过。

前后证据已回收为 `out/r226-r229-reference-reader-evidence.tar.gz`，大小
4,110,518 bytes，SHA-256 为
`5f37b12ffb4ce705dc1872a1e7b3671157cbd6892b1ea6d0fde5ba367a35913c`。
本地验证 103 个 regular members 无重名，102 个 payload SHA 逐项一致；
含 53 个已结束作业状态、原始失败、r228 定向通过与 r229 完整结果。
该归档的完整性校验不改变其中失败测试的结论。

最终证据 `out/verified-r230-reference-reader-evidence.tar.gz` 已回收本地，
大小 1,132,818 bytes，38 个 regular members 无重名，37 个 payload SHA
逐项一致。归档 SHA-256 为
`ef1d7e4357e0e2468c302c559a2bd0b401a41f8cbd9a70623436b9ab672fe6ab`；
完整回归摘要 SHA-256 为
`ffe9df4deb36f5604f3f5aefccdee22d46a491bd462d26d2ca767dc527d4ff4d`。
提交前 213 项本地源码 SHA 与最终冻结清单逐项一致。独立服务提交为
`b5fff0d6d2a6fe0042fc0756c29b17d9312917a6`，tree 为
`3fb3815d2afa062cc2e3bf5f3faa392f6e0d86fd`，提交后服务工作区干净。

用户随后明确授权的推送只涉及独立服务的
`origin/codex/gva-managed-service-completion`。普通推送创建该远端分支，
远端核验为 `41886ad5a8ad2bd51a0409d1b0edd2a0ca1ffe1f`。未提交 reader
修改未被上传；无 force push，无父仓库、其他子仓库、分支或标签推送。
该次 push 与核验均经网络审计登记。reader 完整验证并提交后，用户再次明确
授权同一独立服务分支推送及父仓库本地锁定更新。随后普通推送从 `41886ad`
推进至 `b5fff0d`，远端完整 SHA 核验一致；父仓库 gitlink 与
`guest-linux/aarch64/mem_service.lock` 同时锁定此提交。10 项轻量下游契约
测试通过，父仓库未推送。根仓库完整回归和真实 guest 数据验证没有被本轮
契约检查替代；V2 writer/provider 发布闭环、W5、AM3/AM4 与总目标均未完成。

## 2026-09-14 V2 writer 发布准备与命令序列（r231）

独立服务候选新增 `mem_service_client_prepare_managed_reference()`：从 begin
回执、已确认的完整读写映射与逻辑视图构造 V2。准备前核对当前 allocation
绑定、内容版本与 active mapping；从真实映射字节计算 checksum，确认中立
provider publish 完成后才返回引用。失败保留原输出，不发送 stage/seal。
调用方须冻结 payload，保存完整引用用于 stage 的同一幂等请求重试，解除映射
后才能 seal；该 API 不提供崩溃恢复或跨进程写入仲裁。

`object-session` 新增 begin_reference、publish_reference、seal_reference，
不要求使用者手填 reference_hex。CLI 按 publication ID 保留完整 stage 请求，
重复请求改变视图参数会拒绝；已准备的同代同版本 payload 禁止继续 write。
冻结记录与 allocation/version 绑定，切换最近查询对象或重放 begin 不解除冻结。
未确认的 stage 阻止本地 seal，服务继续执行其自身的版本、holder 和映射准入。
原 SDK 结构、V1/V2 编码、wire opcode 与九份契约清单保持不变。

冻结源码归档为 `out/mem-service-ref-source-r231.tar.gz`，SHA-256：
`96a0b3550738f8869a642e6c162c685433ab7b7e421edf9db26a8cf08622acd2`；
逐文件清单 SHA-256：
`8d743b7d41cc9a8b2815332b588afe6100278cfe52359cac24ef00831ec08a28`。
远端使用既有 AArch64 验证目录与缓存镜像，每作业 4 CPU，
`--network none --pull never`，未下载依赖或更改宿主配置。源码与清单已逐项校验。

- 本地语法检查、diff 空白检查及两项轻量协议测试通过。
- `gva-ref-targeted-r231`：3 项通过，13.030 秒，退出码 0。实际 CLI 写入
  offset=128/4224 的两段不同非零数据，自动生成两个引用；输出及服务 resolve
  与独立编码/checksum oracle 一致。发布重放、参数冲突与冻结后写入拒绝通过，
  最终 live_refs/import_mappings/in_flight 均为 0。
- SDK 夹具覆盖 10 个准备失败情形并核对输出不变；wire/provider 使用替身。
- `gva-ref-artifacts-r231` 已退出 0：原生/安装态 SDK、完整 CLI 夹具、
  九份契约清单逐字节比对、mapping/catalog/client ASan+UBSan 通过。
- 完整 Python 回归的 16 个原始分片均退出 0；精确覆盖 261 个测试 ID，
  256 通过、5 个环境相关跳过、零失败/错误，无遗漏或重复。跳过维度沿用
  r230 的三项父仓库契约、缺少交叉编译器的 deb 构建及隔离网络监听限制，
  未计为通过。

独立服务提交为 `d9b60efb2b4de5a4149264c61561cca33f570dce`，tree 为
`adeeb9126037f4c6380223fcb7a6c4de20d00f3c`，提交后工作区干净；本地
213 项文件与远端冻结源码清单逐项一致。18 个定向、完整回归及产物作业
全部正常结束，无 OOM 或非零退出。

证据已回收至 `out/verified-r231-reference-writer-evidence.tar.gz`，大小
1,146,883 bytes，SHA-256：
`a4ccfb97f7e28ec8ed9a05ced51339d5d109ac583ed12e8e8339b1631f0d5beb`。
本地核验 38 个 regular members 无重名，37 个 payload SHA 逐项一致；
完整回归摘要 SHA-256：
`3b826e45948565e393c13c1d0a326a0e638a66dfdfa34d1a120549ecba2818eb`。
归档含冻结源码、原始测试日志、精确覆盖报告、作业状态与九份契约清单。

本轮 CLI payload 来自 loopback 匿名映射；它证明 writer 服务流程与引用数据
绑定，不能证明两个进程或两个 guest 共享该 payload。实际 OBMM guest V2
发布/读取、W5、AM3/AM4、性能及总目标均未完成。子仓库已提交、尚未推送，
父仓库 gitlink/lock 继续锁定已发布的 reader `b5fff0d`。

### 下一批真实 V2 guest 接入约定

沿用 `run_ub_dual_node_apps.sh --config <file> --app mem_service_object_share`，
增加显式 `MEM_SERVICE_OBJECT_SHARE_ACCESS=cpu-reference`。该诊断组合选择 OBMM、
home 0、64 KiB allocation、1 至 2 轮及 fault=none；既有 cpu/cpu-pto 行为保留。
每轮在 offset=128/4224 写入两段不同的 512-byte 数据，先全部写入，再分别发布、
解除写映射并 seal。peer 通过两个逻辑 key 分别 acquire_reference/map_reference，
等待可见性后读校验，并检查只读 CPU 保护和逻辑边界。

generation 从实际 allocation 回执读取，不从轮次猜测。服务对象充当有界的
封存/消费完成屏障，屏障不传 payload。两端日志交叉核对 V2 编码、generation、
home/incarnation、allocation-relative offset、实际基址、checksum 与操作顺序；
provider 和 kernel 资源分别核对。该配置仅为阶段用例，完整多 home、2/3/8-node、
长期回收与 CPU/PTO 联合验收保持原要求。代码接入与静态测试不计作 guest 通过。

该接入已形成开发修改：run_app 生成上述两端命令序列，原 runner 显式选择
新模式，新增 `validate_mem_service_reference_guest.py` 读取两份 guest 日志，
输出带原日志 SHA 的 JSON 验收结果。实际分配回执、完整 V2、writer/reader
地址和 checksum、只读探针、字节边界、provider 映射序列和 kernel census
均参与核对。保留旧 cpu/cpu-pto 模式及原资源验收器。

定向回归曾发现新回执解析引入了 guest 未保证提供的工具；已统一改用打包的
BusyBox sed 与 shell 校验。缺失、重复、零值或非法版本字段均拒绝，不猜测
generation。新增测试的坏回执替身最初因转义不一致未生效，也已修正；上述
失败原因在此记录，最终重新执行全部相关轻量测试。

本地新增 5 项、原 SDK guest 契约 10 项、原资源验收器 11 项全部通过；
sh/zsh 语法与 diff 空白检查通过。r232 在指定远端缓存容器中执行新增及
资源验收器共 16 个唯一 test ID，16 通过、无跳过/失败/错误，0.294 秒，
容器退出 0 且无 OOM。该作业为 scripts-only，未编译或消费未锁定的服务 SDK，
没有启动 QEMU 或模型。源码归档 SHA-256：
`a30d662391a1ef72dd96fe8b1f29d48ee142a1cdc64318f4ef1e6742330b3a88`；
七文件清单 SHA-256：
`3e8f1cbe08fe90be04c680d093020ebdd615dfaf2931a25149402add520ef49f`。
原日志与测试清单位于 `out/ref-guest-fixtures-r232.log` 和
`out/ref-guest-results-r232.json`，SHA-256 分别为：
`db18b62c15950901e9aecd3271cad4514835823196bf476de4e41b57c440e772`、
`1d831cd30e4e9ada292b0e16d7db0c55f7ec7102ca7538cef96febf7e032e977`。

此批根仓库代码尚未完成完整回归或真实 guest 验证，保持未提交。不以脚本
替身通过替代实际双 guest V2 数据闭环。

### 已授权分支发布与本地锁定（2026-09-14）

重新核对用户的“允许该分支推送及本地锁定更新”授权，其范围为指定服务
分支，未限定逐提交确认。此前额外等待 writer 授权的判断已纠正。普通推送
将 `origin/codex/gva-managed-service-completion` 从 `b5fff0d` 推进至
`d9b60efb2b4de5a4149264c61561cca33f570dce`，后续 `ls-remote` 完整 SHA
核验一致。推送及核验均写入网络操作审计；没有 force push、标签推送、
递归子模块推送或父仓库推送。本地 gitlink 与 lock 同步到该已验证、已发布
writer 提交，以便开展实际双 guest 验证。

本地锁定提交为 `c675c9a`（`Pin validated managed reference writer SDK`），
只包含 gitlink 与 lock 两项更新。10 项轻量下游契约重新执行全部通过，
暂存 diff 空白检查通过；未运行本地重型或完整测试。根仓库 guest 接入修改
继续保持未提交，等待指定远端的真实 guest 验证及受影响回归。

### r233 首次实际 V2 guest 接入与 readiness 修正

以根提交 `c675c9a5ec5cbe74df77986c4a7d4f1fd8c81329` 的完整源码归档加
四文件开发 overlay，消费已锁定的 `d9b60ef`。独立服务及 libobmm 用本地 Git
bundle 同步到指定远端，在离线缓存容器内构建静态 AArch64 CLI 和常驻 worker。
首个构建因容器用户与 checkout 所有者不同触发 Git dubious ownership，未进入
编译；仅在临时容器内登记两个明确 checkout 为 trusted 后，r233b 构建退出 0，
构建前后源码锁定检查通过。未改宿主 Git 配置或跳过检查。

r233 实际双 guest 用例退出 1（runner iteration code 31）。双端 FM、entity、
OBMM conformance 与 worker 注册通过；新脚本误等控制进程本地 registry 的
`data_plane_ready`，未进入 V2 对象操作，在有界等待结束后退出。消费者随后有
init 退出引发的 kernel panic，完整失败日志保留，不计作 guest 通过。

新增流程实际需要等待所需远端 worker 的 `provider_directory_ready`。已修正
字段选择，并补回归：本地 data plane 为 0、目录为 1 时可继续；本地为 1、
目录为 0 时到期拒绝且不分配。实际数据面仍由双方 conformance、SDK peer
canary、publish/read checksum 及最终资源核对验证。本地 26 项相关测试重新
通过。该修正不改变服务全局 readiness 的现有定义，亦不关闭总计划的 readiness
验收。r234 使用独立 overlay 重跑最小失败用例，结果待实测。

### r234/r235/r236 严格 V2 双 guest 数据证据

r234 修正 readiness 后，两个真实 guest 的对象操作及配对数据验收通过。
整轮仍退出 1：新 runner 复用了 legacy bootstrap 路由的日志断言，而此用例
实际通过带 `map_id/segment_id/home_va/epoch/source/profile` 的严格 GSVA_MAP
路径建立读视图。原始失败日志保留，不把配对子项通过写成整轮通过。

V2 验收器现要求 reader QEMU 原日志，对每个视图将 allocation 基址与严格
route 对应，并核对 2 MiB backing route、地址 profile、正值身份、唯一 map ID、
一致的解除映射身份及 CPU window 创建/清理。缺行、错地址、错 profile/source、
错 epoch、错清理 ID、tombstone 或只有 bootstrap 标记的输入均拒绝。原 cpu
与 cpu-pto 验收入口保持不变。r235 在指定远端执行全部三个相关测试模块，
27 项通过，无失败/错误；r234 原始双 guest 与 QEMU 日志用当前检查独立复核
通过，复核 JSON 单独保存，不覆盖其原失败报告。

以下两次使用同一根代码开发快照和已锁定服务 SDK，未启动模型或 infer：

| 实跑 | 操作范围 | 结果 |
| --- | --- | --- |
| r235 | 一轮 64 KiB allocation，offset=128/4224 两个 512-byte V2 视图 | 配对数据及 route 检查通过，runner/container exit 0 |
| r236 | 连续两轮、共四个视图，每轮独立 allocation generation | 全部身份、数据、route 与回收检查通过，runner/container exit 0 |

两次均核对 writer 实际写入 checksum、冻结后的写入拒绝、reader 的 acquire 与
固定视图地址、可见性和实际读取、只读 CPU 探针、逻辑越界拒绝。最终 service
活跃对象/引用/映射/在途计数与 backing/address/quarantine bytes 为零，provider
资源归零，kernel census 回到基线；运行后进程检查未发现遗留 QEMU。容器均
使用缓存镜像、`--network none --pull never`，未开放宿主端口或下载依赖。

根源码基线为 `c675c9a5ec5cbe74df77986c4a7d4f1fd8c81329`，服务为
`d9b60efb2b4de5a4149264c61561cca33f570dce`，libobmm 为
`53011eed10716b422d2ac29199f68b55f7c5bdc5`。r235/r236 使用相同四文件
overlay，SHA-256 为
`fdb6b86ed1c9bf8c0da7ec857e44c351e8e623b7d0e60d9241e131e93f0608c4`；
相同 initramfs SHA-256 为
`89526ad90e221cae4eb746dc48561b64aa2b7971f1bb2112d5fedef73ca8384c`。
kernel、QEMU、配置及 rootfs 各文件指纹保存在每轮 evidence 清单。

远端证据归档 `verified-reference-guest-r233-r236.tar.gz` 为 16,082,140 bytes，
含 50 项 payload 及逐文件 SHA 清单；归档 SHA-256：
`a402f10e6c9445d24213fc3ee3f8492c39806f292aab9e35eb7463750b1b1b8c`。
8 个原始构建/guest 作业均已终态且无 OOM，r233 构建失败、r233/r234 guest
失败与后续成功记录全部保留。归档已回收到
`out/verified-reference-guest-r233-r236.tar.gz`，本地核验 51 个 regular members
无重名，50 项 payload SHA 与清单逐项一致；四个源码文件与当前工作区逐字节
一致。overlay 另含 macOS tar 的四个 AppleDouble metadata sidecars，核验时
显式区分，未作为源码。r236 两轮实际 generation 为 1、4，对应不同 segment，
每轮两个视图共享同一个 home VA/segment/epoch，四个 map ID 均完成匹配 unmap。

该证据关闭本次一轮/两轮 CPU V2 writer-to-reader 用例，未关闭多 home、
2/3/8-node 全矩阵、CPU/PTO V2 联合绑定、W5、AM3/AM4 或总目标。根仓库完整
回归仍待执行，代码保持未提交；本批轻量/定向回归不替代根仓库完整回归。

### r237/r238 根仓库完整回归与环境诊断

根源码使用 `a43a229f5431c78499700f5c55c4085967249704` 的真实浅层 Git
metadata/index，叠加开发修改，独立服务固定为 `d9b60ef`。容器复制源码到
私有工作目录，保留精确 kernel/QEMU overlay，未更新父仓库的其他 gitlink。
补齐真实 Git metadata 后，早期归档环境中的源码锁定等失败已消除。

r237 完整 Python 发现并执行 529 个唯一测试：520 通过、5 跳过、3 失败、
1 错误。4 项 W5 配置测试在 validate-only 阶段调用真实 Cargo，容器虽为
network-none，Cargo 仍尝试解析 crates.io 并报告 DNS 失败。该内部依赖尝试
已按 retrospective 补入网络审计；无法从日志获得准确连接/重试次数，不把
隔离网络配置写成“未尝试联网”的证据。后续显式设置 Cargo offline 并复用
已有 registry/cache，未下载依赖或改宿主配置。

r238 同样执行 529 项，结果仍为 520 通过、5 跳过、3 失败、1 错误，首个
异常已变为实际构建产物路径不一致：Cargo 按 CARGO_TARGET_DIR 输出，W5
硬编码查找工作区 target/debug/sim-cli。全部 4 项失败均停在该路径检查。

独立布局 pytest 首轮缺少 pytest，未执行测试；改用已有测试镜像后实际执行
74 项，其中 2 项失败。已将只读探针计数限定到对应函数，并按已锁定、已验证
服务 SDK 更新 package/wire/compat 清单的精确长度、checksum、字段和文件
数量，显式覆盖 mapping/reference 操作；没有改动 SDK 清单或放宽比较。
r238 完整布局测试 74/74 通过，容器 exit 0、无 OOM。

完整 Rust r237 首次因离线 registry 未挂载而未进入测试。补齐缓存后的
r237b 实际执行完整 workspace，3 个失败 target 为 sim-cli、sim-console、
sim-uapi，涉及 Simpler/PTO-ISA 源码、已提交 pin 与原生工件准备。保持失败
记录，不将缺少依赖改成跳过，也不以 Python 通过代替 Rust 通过。

### r239/r240 W5 Cargo 产物定位与完整 Python 回归

W5 自动构建改为读取 Cargo compiler-artifact JSON 中唯一的 sim-cli bin
executable，兼容输出目录、target triple 和带空格路径；Cargo 非零退出、
无产物、不同产物歧义或文件不可执行均拒绝。显式 SIM_CLI_BIN 仍跳过构建。
CLI 入口不变，新增 7 项轻量隔离测试，不编译真实代码或加载模型。

本地首次夹具的路径比较受 macOS /var 符号链接影响，改为解析后的路径。
r239 的原 W5 配置失败用例已通过，但目标镜像的 Zsh 启动配置覆盖 PATH，
导致两项夹具误调用真实 Cargo；完整回归因前置失败未执行。修正为在待测
shell 内设置替身路径，并逐 shell 检查调用标记。保留原失败日志。

r240 在指定远端缓存容器、network-none、Cargo offline 下执行：

- 前置 7 项隔离测试及原 W5 配置失败用例，共 8 项通过。
- 完整 `python3 -m unittest discover -s guest-linux/aarch64/tests` 等价发现
  流程执行 536 个唯一 test ID：531 通过、5 环境跳过、零失败/错误。
- 执行数量与预先枚举的唯一 ID 数量一致，无 loader error。5 项跳过均为
  未提供 AArch64 交叉编译器的编译/链接测试；跳过不算通过。
- 容器退出 0、无 OOM。此轮未启动模型推理，不替代 W5 真实推理验收。

r240 八文件开发 overlay SHA-256：
`fba5f32d21ac29c5a63a5a064db5aec50ba35ae0570cde746a4299fb42d9269c`。
完整清单与日志已取回本地，逐项与远端 SHA 核对一致：

| 本地证据 | SHA-256 |
| --- | --- |
| `out/root-results-r240.json` | `d3a326b3e6268b66c19d05e75c097c9c86f742270c5afa48231edc5bd4a755e4` |
| `out/root-full-r240.log` | `adf5c7e1af8f0293b192ab7a3493d724fbb2cda59e280a430b1ff05349b099e0` |
| `out/layout-r238.log` | `3a13e742757d21b0020012e700321c368e106f28457ddc0cdbe28e1a5a3390b6` |

原生依赖准备继续使用准确 Simpler `3794c430`、根 PTO-ISA `6a9da1e1` 及
Simpler 自身的嵌套 pin `594817f2`，三者用途分别登记。现有目标机 PTO 工作树
含无关修改，仅从其 Git 对象克隆已提交 pin 到专用目录，不复制这些修改。
目标镜像名为 g++-15 的入口实际指向 GCC 14，原生验证改用目标机已有的
真实 GCC 15.3.0，只在临时容器内配置。首次 r240 原生准备因只读 Git 目录
ownership 检查退出 128，尚未编译；补齐该明确目录的临时信任配置后以 r240b
重试，host-vector 工件实际构建通过，容器 exit 0、无 OOM，manifest 位于远端
`native-artifacts-r240b/host_vector_manifest.json`。没有安装宿主软件、修改
宿主 Git 配置或发布其他仓库。该构建只证明原生准备路径，不替代算子执行。

随后 r241 在同一依赖组合启动完整
`cargo test --offline --locked --workspace --no-fail-fast --jobs 8 -- --test-threads=4`，
在指定远端限制 8 CPU，最长 1,800 秒。观察到原生算子实际执行日志，整轮终态
尚待取得，不预先记为通过；后续继续观察原作业 `gva-rust-workspace-r241`。

在 r240 结束时，本批代码保持未提交，等待完整 Rust 结果；后续终态见下节。

### r241 完整 Rust 回归与 guest 接入提交门禁

原作业 `gva-rust-workspace-r241` 于 09:39:24 UTC 开始、09:48:21 UTC 结束，
容器 exit 0、无 OOM。完整 Cargo workspace 的 43 个 harness（含 doc-tests）
汇总为 963 通过、0 失败、16 ignored、0 filtered out。逐 Cargo target 仅取
该 harness 的最终汇总，排除内部隔离子进程打印的重复单测试结果；各 target
汇总保存在 `rust-results-r241.json`，完整日志 SHA-256 为
`524f01e726a9567d3adfc0d7fdf0d93e048a21d3d215c9337ed2d4200b0dda66`。
先前 3 个失败 target 在完整执行中通过，未通过跳过用例来消除失败。

16 个 ignored 保持源码原有标注：11 项需要外部官方 checkpoint、DS4 GGUF/
相关 oracle 或指定模型运行环境；5 项要求专用预构建 Simpler manifest。
这些用例没有执行，不能视为本计划模型矩阵通过。r241 的原生算子测试使用
其自身测试输入，不能冒充指定真实 DeepSeek 权重的 W5 运行证据。

r237-r241 的成功/失败日志及结构化结果共 14 文件形成一个证据包，不含模型、
密钥或网络审计日志。归档名 `root-regression-evidence-r237-r241.tar.gz`，大小
162,475 bytes，SHA-256：
`a756f5898fd4cb8da319e4dc82aca12ce02ba8d9d384a2eed0bc6162e05c1553`；
逐文件 manifest SHA-256：
`dd2a2d2431e9fdb00e55ff133977db15b496e70ed1f404112197b314c87c09ec`。
归档已取回本地 `out/`，15 个 regular members（含 manifest）无重复，14 项
文件长度和 SHA 与清单逐一核对通过，Rust 结构化结果与上述汇总一致。
本批八个代码/测试文件与 r240 受测 overlay 逐字节相同；r241 使用同一 overlay。
对应 r235/r236 实际 guest 的四个运行/验收文件也保持不变。提交仅纳入这些
已验证文件和本计划/记录，不纳入其他工作树修改，不推送父仓库。

V2/PTO 后续接入检查修正了一项早期推断：managed SDK 的 binding 已返回逻辑
视图，owner/PTO 的相等比较本身不会拒绝非零 allocation offset；driver 也已
处理页内偏移。无需据该推断新增底层 API。当前 managed PTO CLI 的 map_object
仍调用 raw acquire/map，需要接通 V2 发布、resolve/acquire/map 流程后，以
实际 PTO 数据、身份、权限和原 completion 验证现有 owner/provider 路径。
完整 AM2、V2 CPU/PTO 联合绑定、W5、故障矩阵和总目标仍未完成。

### V2 引用输入接入 managed PTO：r242-r251 验证

现有 managed CLI 增加 `reference_inputs=1`，同时要求 `readonly_inputs=1`。
home 使用实际 allocation generation/version 执行 BEGIN，在完整 RW 映射写入
A/B 并由 SDK prepare/STAGE 发布精确子区间，unmap 后 SEAL。两端解析同一完整
V2；consumer 通过引用 ACQUIRE，两端都 map_reference 为 RO，再交给既有 owner。
输出继续使用独立 RW allocation。跨页输入为 allocation offset=3968、len=4352，
输入 memref offset=0/4096；输出 C/D 保留其独立对象 offset=12160/16256。
CPU pin 使用映射实际长度，多请求状态传递输入 view offset。未知引用映射保留
完整 reference lifecycle；失败路径不提前 release，也不自动重发元数据变更。

CLI、runner、initramfs、校验器及测试形成同一候选；未修改独立服务 d9b60ef，
没有推送父仓库。新增 C 隔离测试只验证应用编排与失败保留，使用明确 SDK 替身；
它不能充当真实服务/provider/guest 验收。具体配置见应用 README 的 V2 引用输入节。

| 批次 | 已核实结果 | 范围 |
| --- | --- | --- |
| r242 | managed CLI 原生编译及 20 项定向测试通过；guest 目录 unittest 538 项，533 通过、5 跳过、零失败/错误 | 首版 CLI 与隔离编排，尚未接完 runner/验收器 |
| r243 | guest 目录 unittest 540 项，535 通过、5 跳过、零失败/错误/加载错误；容器 exit 0 | 加入 runner 和 V2 日志校验的候选 |
| r244 build | 默认镜像缺少 pytest，exit 1，尚未编译 | 保留失败日志，未下载依赖 |
| r244 layout / r244b build | 使用既有 pytest 镜像 74/74 通过；原编译镜像生成静态 AArch64 CLI 与 initramfs，exit 0 | 两个缓存容器均 network-none、无 OOM |
| r244 guest | 两端应用 status=pass；整轮 runner/container exit 1，校验器报 backing/CPU 对象不匹配 | 真实双 guest，不能计整轮通过 |
| r246 | 原日志复核推进到 owner 生命周期检查后失败，完整 unittest 尚未执行 | 第二个校验器旧路径假设被实际日志暴露 |

r244 的实际数据：引用 generation=1、version=2、view offset=3968、len=4352，
两端完整引用相同；CPU 输入视图 checksum=`9e0d6647b66b7183`。两次原 PTO
completion 为 `202609152441/202609152442`，均 status=1；C/D CPU checksum
分别为 `d602274b27b2bb23`、`c90bc7e9a551a6b3`。四个 reader 合计 444 次校验，
停止接纳后四线程均继续完成检查；两对象 owner 最终各项 pin/mapping/holder 归零。
QEMU 两次 unbind 均 completion_success，每次 load=512/store=256/fence=1，
segment_payload_staging_bytes=0。这些为原始日志中的局部事实，整轮失败仍保留。

校验修正有两点：QEMU GSVA_BIND.home_va 是完整 backing 起点，必须与 allocation
address 比较；CPU view 另核对 address+offset。V2 home 的 writer 在 owner 接管
只读视图前已完成写入、unmap 和 seal；校验器分别约束 writer 发布顺序及后续
reader owner 生命周期，旧路径的 owner 内初始化判据保持原样。新增拒绝测试
要求混淆 backing/view、损坏 V2 字段、错误计算偏移或完成身份均失败，并将 CPU
视图 checksum 与 V2 payload checksum 直接关联。

r246b 候选的 8 文件 overlay SHA-256：
`385b162537ac7e8965ad65e51b661cfd243da987fcf68d2b6b205c84154cc8c7`。
本地日志解析定向测试 29/29 通过。r246b 原日志复核通过，原失败报告未覆盖；
guest 目录 unittest 完整发现 540 个唯一 ID，535 通过、5 跳过、零失败/错误。
r245 Rust workspace 原作业退出 0：43 个 harness、963 通过、16 ignored、零失败。
r245 使用 r243 源码，后续变更仅在验收器、相应测试与 README；最终 r246b 源码
由 r248 独立完成 Rust workspace 回归：43 个 harness、963 通过、16 ignored、
零失败、零 filtered。原容器于 11:20:31 UTC 开始、11:29:26 UTC 结束，exit 0、
无 OOM。仍逐 target 取最后汇总，排除嵌套子测试的重复输出；ignored 不计通过。
完整日志 SHA-256 为
`f88c857bbb9d8e8de2560b67e136f7b2cb1252bbf63dbcb763359fe4ebc4e268`，
结构化结果 SHA-256 为
`e9224ec33aa563ee50923075d05d8ed28e48cf9397d736db94cfd927f5193103`。
两文件已取回本地并核对哈希，不能将 r245 与 r248 的源码身份混用。

r247 和 r249 使用同一静态 CLI/initramfs，分别完成 home 0 / compute 1 与
home 1 / compute 0 的实际 cross-page V2/PTO 验收，runner/container 均 exit 0，
无 OOM。r247 的原始三份输入日志 SHA 已取回并独立核验，本地复核相同完整门槛
通过；结构化结果为两个对象、两次成功 dispatch、4 LOAD、2 STORE、2 fence，
完整 V2 及四线程 owner 验证通过，`am2_complete=false`。未执行真实模型权重。

| 证据 | SHA-256 |
| --- | --- |
| r244 原失败 campaign 归档 | `780ff01af526c8de9661e854da944390afe7497d9707413e660d6c4353693a6e` |
| r247 成功 campaign 归档 | `35ad75e5fe1067f5e98450a3dfc22f2e2268e43d772eec92be7ef699af2b22a0` |
| r246b guest unittest JSON | `e1a67d8cdee23cc3f6c6ecb097705455dc946d24efecbd59c5c3cde4e58e7580` |
| r246b 回归日志 | `bd841e26b68098aafa1cbabdd4101e2c96e8f2686ef129482934cacffb1c75e5` |
| 实际 guest initramfs | `e7925884fc78271dbbc6174e11685ca5bd1ede1526f7ef6a47158df43ba471bd` |

r250/r251 使用同一最终源码、静态 CLI 和 initramfs 完成大视图实际双 guest
验收，两个原容器均 exit 0、无 OOM；11:34:50 UTC 后检查无遗留 QEMU 进程。
每轮均为四个 CPU reader、两个依赖 dispatch，核对完整 V2、真实 home export、
4 LOAD、2 STORE、2 fence、原 completion 及最终 owner 资源归零。

| 批次 | home / compute | 输入 V2 offset / bytes | 单张量 bytes | V2 与 CPU 输入 checksum | 实际 LOAD / STORE 分片 |
| --- | --- | --- | --- | --- | --- |
| r250 ND | 0 / 1 | 0 / 131072 | 65536 | `197bb70c638bee83` | 68 / 34 |
| r251 tail | 1 / 0 | 0 / 130048 | 65024 | `ae0db774e0986343` | 68 / 34 |

r250 原容器运行区间为 11:23:38 至 11:28:22 UTC；r251 为 11:28:59 至
11:34:50 UTC。两个输入引用均 generation=1、version=2，incarnation 分别为
20260915250/20260915251。ND 的 LOAD/STORE 字节数为 262144/131072，tail 为
260096/130048，逐分片请求/应答核对通过，分片上限为 4055 bytes。
三个 r249-r251 报告的各原始输入日志已在远端重新逐字节散列，与报告记录一致。
大尺寸 QEMU 日志保留远端，未在本地执行大日志回归。证据归档如下：

| 归档 | SHA-256 |
| --- | --- |
| `pto-reference-guest-evidence-r249.tar.gz` | `cc12400aff688548fa0779ca4425f01d87d06bfa00973875813a4f21ff2e1fd2` |
| `pto-reference-guest-evidence-r250.tar.gz` | `1e011ef35b9708e4d97ebda397f7ae6f8bc45d307a1b41d1a30a89431ddb17fe` |
| `pto-reference-guest-evidence-r251.tar.gz` | `df1c9c2af86039b8fdb39707769cc982788986af62b340c238aefdf3bd67086e` |

两轮均为单请求序列，`queued_overlap_verified=false`、
`device_parallel_execution_verified=false`、`am2_complete=false`。它们不证明
多请求设备并行、故障恢复矩阵或真实模型执行。最终八文件与受测 r246b overlay
逐字节一致，提交范围仅包含该八文件及本计划/实施记录；父仓库不推送。
完整 AM2、W5、故障矩阵及总目标仍未完成。

### r252：反向 tail 的 V2 双请求实际验收

代码已提交 `62b40450dc27db34405754aa80afd5b7b7ebe8df`，本轮使用同一受测
源码和 r244 initramfs，只把现有配置设为 `concurrent_requests=2`，不新增代码。
原容器从 11:39:26 UTC 运行至 11:45:29 UTC，exit 0、无 OOM；之后检查没有
遗留 QEMU。home 1 / compute 0、四 CPU reader、tail 65,024-byte 张量，两次
独立请求共享同一只读 V2 输入并写入独立 C/D。两个原 op ID 为
202609152521/202609152522，均成功完成；线程 join、队列、六条 compute lease
与最终 mapping/holder 清理均通过。输入 generation=1、version=2、
incarnation=20260915252，view offset=0、bytes=130048、checksum=
`ae0db774e0986343`。4 LOAD、2 STORE、2 fence，实际分片为 68 LOAD/34 STORE，
分片请求/应答和完整 CPU 数据核对通过。

结构化报告明确 `concurrent_requests=2`、`queued_overlap_verified=true`，同时
`device_parallel_execution_verified=false`、`am2_complete=false`。本轮补齐
V2 反向 tail 双请求子集，未扩大为多请求故障或设备并行执行结论。
三个原始输入日志已在远端独立重算 SHA，与报告一致。报告 SHA-256：
`3f4a42e7e0d9859f7bf948fb2cb34071713cabb49b95cab6ca54b726c04c4921`。
原始证据归档 `pto-reference-guest-evidence-r252.tar.gz` 为 59,037,858 bytes，
SHA-256：`bc4386b51d27f3826660d26b57dd35b5d79b92e49912864984b5939db4511255`。
原观察连接返回较慢期间，通过独立只读连接核对相同容器，未重启 guest；原观察
最终也返回 exit 0。所有网络操作继续单列审计，日志不提交。

### W5 接入前的文件与源码检查（r252，未执行模型）

已在指定验证目标找到 Qwen3-0.6B 的 `model.safetensors`，大小为
1,503,300,328 bytes；配置声明 28 层、hidden=1024、vocab=151936、bfloat16。
config、tokenizer 和 tokenizer_config 的 SHA-256 已记录于本地
`out/w5-model-readiness-r252.json`，权重完整 SHA 尚未计算。
同时找到计划指定的
`DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf`，
大小为 86,720,111,488 bytes。仅文件名和 stat 已核对，GGUF 元数据、tokenizer、
完整权重 SHA 与当前 Rust/Simpler profile 尚未验收。没有下载或替换量化版本。
本地准备记录保留目标路径和不可变身份候选，不将这些机器配置提交到源码。

源码检查确认 W5 仍有待迁移的实际接入点：`llm_infer.c` 的 hidden/previous-KV
解析结果为旧 payload view；`qwen3_pto_guest_prepare()` 输入使用 legacy acquire，
输出使用 local arena 和 local KV reservation。必须统一输入引用、输出 allocation、
原 completion、就地发布和后续读取的生命周期；仅替换 acquire 函数无法完成 M2。
Rust 的 `model_runtime_object_payload_view()` 也仍消费旧引用并具有 snapshot/
registry 路径，需按受管理对象身份检查相应模型执行接入。上述为源码缺口定位，
没有声明 W5 接入、模型正确性或多节点矩阵通过。

### r253-r259：百轮入口与真实第三轮容量失败

沿用 `cpu-reference`，host/guest/验收器统一接受规范十进制 1 至 100 轮，拒绝
零、前导零、越界和非数字。每轮创建真实数据 allocation 及两个同步 allocation，
发布两段 V2，只读映射读取后退役三对象。验收器新增每轮 writer 服务资源归零
检查及 allocation cycle 计数；reader 的中间全局快照允许观察到下一轮，自己的
provider 映射仍逐次检查。新增合成 100 轮夹具、缺轮/中间资源残留拒绝和
host/guest 边界一致性测试。夹具通过不计作实际百轮验证。

r253 四文件 overlay SHA-256：
`94486685d775d89ba1f6e24cb4445514780fa97b53a74295ad433ed97f45ed86`。
远端 guest Python 目录完整发现/执行 543 个唯一 test ID：538 通过、5 项
交叉编译器环境跳过、零失败/错误/加载错误，容器 exit 0、无 OOM。r254 在同一
源码完成完整 Rust workspace：43 harness、963 通过、16 ignored、零失败或
filtered；11:52:16 至 12:01:08 UTC，exit 0、无 OOM。

r255 的实际 100 轮配置在第三轮失败，原容器 11:54:53 至 11:55:08 UTC，
exit 1、无 OOM。两端仅完成两轮、四个 V2 reader 视图，第三轮数据 allocation
generation=7 已 active，writer map 返回 `capacity_exceeded`，holder 保留。
不能计为百轮通过，也不能把 worker 退出改写为资源已安全回收。原失败归档
`reference-pressure-evidence-r255-failed.tar.gz` 为 90,454 bytes，SHA-256：
`c87c84c20658a6fda915584f64b72c74ae1ec4ef68d550a7f5ead0f1ade8e6c7`，
已取回本地核对。

随后只增加失败时的 `allocation-stats` 诊断，保持服务、provider、内核和容量
配置不变。最终 r256 overlay SHA-256：
`025273eb4c07f391b17c384cdb71ae9ec191d62494feec8b3748d5bc3c830e99`。
9 项本地轻量测试通过；r257 再次完整执行上述 543 项并取得相同结果，exit 0、
无 OOM。测试结果 JSON SHA-256：
`8acc08c79dfc9470ad2073eb388acf723174bef0cc4751724fa379652c76c23a`。
最终源码的 r258 Rust 原任务已完成：43 个独立 harness、963 通过、16 ignored、
零失败或 filtered，12:03:28 至 12:12:14 UTC，exit 0、无 OOM。计数逐个取
外层 harness 的最终结果，避免重复计算嵌套测试输出；日志 SHA-256：
`4e682b24f0a736601fbc08570307f139a76cb96a2361265cb3704061fafb4dae`。
Python 的五项跳过与 Rust 的 ignored 均不计作通过；上述回归不替代实际百轮验收。

r259 使用三轮最小复现采集额外诊断，原容器 12:03:31 至 12:03:45 UTC，
仍 exit 1、无 OOM。失败时服务实际报告：

| 计数 | 实际值 |
| --- | ---: |
| allocate / publish 成功 | 7 / 7 |
| retire / reclaim 成功 | 6 / 6 |
| live objects / refs / import mappings | 1 / 1 / 0 |
| backing / address reserved bytes | 2097152 / 2097152 |
| idempotency capacity / used | 64 / 59 |
| cleanup reserved / available / reservation deficit | 4 / 1 / 0 |

诊断归档 `reference-pressure-evidence-r259-failed.tar.gz` 为 90,609 bytes，
SHA-256：`dbf2d5c2e235db93a65b41058862aa7ef197148d2c974f79752464bfe36d6fc4`。
该轮结束后未发现遗留 QEMU，失败对象的隔离/恢复仍需按 AM3 验证。

源码 `mem_service_new_cleanup_reservations()` 对新 mapping BEGIN 预留后续三条
应答，另需 BEGIN 自身一条，超过当前可用一条，准入返回容量错误。provider map
尚未执行；这与前两轮资源归零、第三轮无 import mapping 的实际计数一致。
历史应答表没有随已退役生命周期回收是当前百轮的首个实际限制。必须保留旧请求
的重放/拒绝语义及在用清理容量，不能通过放大表或丢弃旧请求历史宣称解决。
worker 日志也明确 `address_reuse=disabled`，后续地址重用仍须按 AM3 的确认、
drain、epoch 与旧上下文隔离要求实现；当前百轮、完整压力矩阵和总目标均未完成。

### r260-r262：持久重放历史边界层（daemon 尚未接入）

压力诊断六文件已提交根仓库 `85df223`，父仓库未推送。继续检查发现原
`<store>.journal` 会压缩为仅保留头部，snapshot 仅保存当前内存幂等表；单独
删除缓存记录会丢失旧请求结果。独立服务新增内部
`mem_service_replay_history.h/.c`，保持原 64 项表和服务运行行为不变。

历史文件采用显式小端帧、递增序号和前帧校验链，保存精确请求标识及结果；
文件长度不受内存表大小限制，扫描内存占用固定。append 确认文件同步，创建
同时确认目录同步；不确定写入使句柄停止服务，保留原 checkpoint 和文件供核对。
打开时验证完整文件与调用方指定前缀，查询也验证完整历史；截断、损坏、替换
路径或竞争写者均拒绝。校验仅检测意外损坏，不提供对抗篡改认证。

r260 首版九项测试通过，随后 r260b 增加独立 Python 字节格式/校验 oracle。
最终五文件 overlay SHA-256：
`a8bd4b77e4151ad236e21879c81cf2486d8329886bb00aed60e608fe4338a0c7`。
r262 在 n4-910c1 的隔离容器完成最终九项边界测试、八种 ASan/UBSan 场景及
`host-artifact-smoke`；12:23:53 至 12:24:58 UTC，exit 0、无 OOM。日志 SHA-256：
`c4c67b173e6f2302ae883b3a5483373157fe680a0c4a930a4207402a32af8abe`。
其中 300 条历史逐项重放及文件重开为存储边界测试，未执行实际 guest 生命周期。
r261 独立仓库全量 Python 回归的 16 个确定性分片均 exit 0、无 OOM。
逐项核对 270 个唯一 test ID 无遗漏/重复，265 通过、5 跳过、零失败/错误。
跳过为三项依赖 ub_sim 源码布局的契约、一项打包工具链要求和一项无非回环网卡
的监听策略测试；这些跳过不计作通过。完整结果 JSON SHA-256：
`50d9c237b9ca2bb691e005c3f688157ffe1865c951463c25d34c364b4ee9d695`。
五个本地提交文件与最终 r260b overlay 逐字节相同；其余 212 个基线文件与
r231 来源清单一致，原组件 README 由本次 overlay 显式替换。
独立服务提交为 `4bd65b8`，工作树干净，尚未推送。父仓库 gitlink 与 lock
继续固定 `d9b60ef`；在完成后续服务接入、测试和授权分支发布前不升级消费版本。

下一步须把历史层接到 daemon 的缓存查找/准入，并持久化历史前缀 checkpoint，
核对 snapshot/journal 压缩与重启导入的顺序。当前 save-store 的 rename 前后
持久化顺序也须补齐后才能据此回收缓存。未配置 store 仍应保留容量拒绝，
不能对已存在的 managed identity 提供未经验证的恢复或 snapshot 替换。
当前 daemon 尚未采用新历史层，第三轮容量限制、地址重用和真实百轮均未解决。

### r263-r269：daemon 历史接入与独立服务验证

daemon 已接入持久历史：先同步已完成应答和 snapshot 前缀 checkpoint，再清除
相应缓存。内存表仍为 64 项，无 store 时保持容量拒绝。历史 store 使用独立
magic，拒绝缺失/损坏历史和不完整备份恢复；重启报告
`managed_recovery_required=1`、`data_plane_ready=0`，等待实际资源对账。
旧 cleanup 应答可重放，新 managed 数据工作保持关闭。新增四项
`allocation-stats` 字段，九份契约清单均由实际 CLI 渲染并逐字节核对。

r263 首轮六项 daemon 测试为五通过、一失败：恢复门禁拦住旧 release 应答。
修正后 r264 六项全部通过，65.794 秒，`host-artifact-smoke` 通过；原容器
12:57:47 至 12:59:03 UTC，exit 0、无 OOM。100 次映射控制事务保留旧请求
原始应答且不新增 holder/mapping；该用例使用单个活动对象，不证明实际 guest
分配/回收百轮通过。还覆盖 checkpoint 写入失败保留缓存、损坏/缺失历史拒绝、
重启后的 cleanup 重放与资源恢复门禁、完整和分页 snapshot restore 拒绝。

r265 在 n4-910c1 完整发现并执行 276 个唯一 Python test ID，271 通过、
5 项环境跳过、零失败/错误；16 分片均 exit 0、无 OOM，无遗漏或重复。
跳过仍为三个 ub_sim 源码契约、一个打包工具链要求和一个非回环 IPv4 环境要求。
结果文件 `replay-results-r265.json` SHA-256：
`c290c89c4c5b1875eb310316144d4e765d827e4392a4288fe3fed290ddc96967`。

r266/r267 同时插桩短命 CLI 和 daemon 时六项均失败，原日志保留，失败表现为
provider 就绪拒绝；未观察到 sanitizer 内存诊断。r269 保持原测试断言、30 秒
provider 租约和刷新频率，改用原生客户端调用 ASan/UBSan daemon，六项全部
通过，128.205 秒。完整检查捕获的客户端/daemon/退出阶段输出，无 sanitizer
诊断；之后安装态 SDK、`install-smoke` 和九份清单比对通过。原容器
13:10:03 至 13:12:49 UTC，exit 0、无 OOM；日志 SHA-256：
`6d79ed1a43ea0accdfe1db6d902a9b2727205c68eaa8806abcf7b5aa475d5451`。
r268 独立安装态验证也通过。上述对照支持插桩客户端开销影响租约的判断，
不把失败轮次计为通过，也不宣称所有客户端路径均完成 sanitizer 验收。

独立服务已提交 `3e5b35b`，仅推送获授权的同名 `codex/` 分支，远端完整 SHA
核验一致。父仓库本地 gitlink/lock 同时更新为
`3e5b35b0f1aeb6e50e84ec8024e2a9b886b214ae`，补齐直接链接 daemon 的三个
构建入口及缓存签名。轻量源码/链接/manifest 契约通过，下游全量和实际 guest
第三轮/百轮验证待执行；worker 仍禁用地址复用，AM3 及完整目标保持未完成。

### r270-r273：下游锁定验证与真实下一处容量限制

r270 在隔离容器从已发布 Git bundle 检出干净 `3e5b35b`，同时核对父仓库
gitlink/lock，执行 543 个唯一 Python 用例：538 通过、5 项交叉编译器环境
跳过、零失败/错误/加载错误；原容器 exit 0、无 OOM。r271 的完整 Rust
workspace 原作业已完成，13:19:35 至 13:28:30 UTC，exit 0、无 OOM。
逐个核对 43 个外层 harness 的最终结果：963 通过、16 ignored、零失败或
filtered；harness 清单与上一轮一致，未重复计算嵌套输出。日志 SHA-256：
`7cdd325ba154bf2bc6e2c4f4f63477e2dc466991f4f28e08ff4463c5703ce269`。
Python 跳过和 Rust ignored 均不计为通过，父仓库本批不推送。

r272 通过独立服务 Makefile 构建启用 OBMM 的 AArch64 静态 CLI，替换
r259 initramfs 中的服务程序，其余平台输入保持原基线；构建和镜像检查均通过。
新镜像 `managed-replay-r272.cpio.gz` SHA-256：
`205c5a89b738cd976d88d202225e6e1d69c3894be91f60ba90478684d0b4eee0`。

r273 实际两 guest 使用相同 64 KiB、三轮、seed=41、64 项幂等表及 16 MiB
aperture。第三轮 generation=7 已完成真实映射、两段 V2 发布与 seal，越过
r259 的 mapping BEGIN 容量失败点。随后同步对象 `ref-done-3` generation=9
因 `address_capacity` 被拒绝，worker 记录 empty cancel 与确认后将其退役。
整轮 13:23:06 至 13:24:21 UTC，exit 1、无 OOM；不能计为三轮或百轮通过。
失败时实际统计为 `idempotency_used=23`、`cleanup_reserved=2`、`available=39`、
`reservation_deficit=0`、`history_records=59`、`history_failed=0`，表容量仍为 64。
worker 停止时保留 `reconciliation_required=1`，不能据此声称残留资源已回收。
guest 的 init 随失败退出产生相应 panic，原始失败保留，未过滤为成功。

失败证据归档 `replay-guest-evidence-r273-failed.tar.gz` 为 109,307 bytes，
SHA-256：`6f3888583385bdad527d5ef1cbb4703ddf0f6ec04d710259f96057619f07e28e`。
原作业结束后未发现遗留 QEMU。首个幂等历史限制已被实际越过，地址重用、
完整恢复及全部后续验收仍需继续实现；不扩大 aperture 或改低百轮要求。

### r274 起：退役协调端 ACK 与超时隔离

源码检查确认 QEMU `gsva_coh_retire_tx` 原来在未配置 hold-pending 时，发送
RETIRE 后立即清空待确认 holder；TIMEOUT 也可以经再次 retire 转为 RETIRED。
这是地址复用完成证明的缺口。当前修订要求逐 holder 确认，发送失败或未启用
传输均保持 pending；超时后禁止 retire 和普通 object-remove 清除隔离状态。

五类事务 ACK 共用校验入口，核对版本、消息类型、目标节点、事务类型及成功状态。
完成计数前再核对完整 key（包含 epoch、size、flags）、sequence 和待确认 holder。
旧 epoch、错误事务类型、重复 ACK 或超时后的迟到 ACK 均不能完成退役。
ACK 到达时独立检查超时，不依赖定时查询先触发。该改动通过现有 GSVA event/
query CLI 路径消费，没有增加服务 core 的传输依赖。

r274 首次构建因新增测试桩缺少 access 参数失败；r276 修正签名后通过项目
`build_qemu_binary.sh --with-obmm-tests` 重建生产 QEMU，七个底层测试程序共
44 项全部通过，包括新增四项退役事务测试。测试直接链接生产 coherence 源码，
模拟传输投递，覆盖 16 种错误应答、两个 holder（含 CNA=100）、重复 ACK、
发送失败、禁用传输、ACK 缺失及迟到应答；无远端 holder 的退役仍可完成。
容器 13:51:17 至 13:51:34 UTC，exit 0、无 OOM。单测日志 SHA-256：
`f822ad03478b0c0f3c7115fb6c0c886ad0d8a01d51f18bd5c7c38b96f94c9ce8`。
QEMU binary SHA-256：
`ce6ade3b339b58a4bdd2aeae7752a25f27752d668ab2d8e4570dad58758e2c6d`。

r275/r277 的全量 Python 作业在准备阶段因只读 QEMU 子模块目录解包失败，
未执行到完整测试集，失败记录保留。已按真实挂载布局将 QEMU 修改作为只读
文件挂载，并只解包父仓库文件；后续回归结果单独记录，不覆盖失败轮次。

以上测试只证明协调端接受完成回执的规则。接收端 RETIRE 仍需接入真实的
CPU/PTO 排空、映射撤销、fence/TLB 完成，再与 worker 的持久化清理和 kernel
retire 绑定；worker 仍禁用地址复用。不能据此关闭 AM3 或宣称百轮通过。

r279 的父仓库 guest Python 全集已执行：543 个唯一用例，538 通过、5 项
交叉编译器环境跳过、零失败/错误/加载错误，容器 exit 0、无 OOM。
执行结果清单与 r270 相同，SHA-256：
`8acc08c79dfc9470ad2073eb388acf723174bef0cc4751724fa379652c76c23a`。
QEMU 文件通过只读挂载使用本轮源码，父仓库两份构建/测试文件使用 r276 overlay。

r278 在该 QEMU binary 上完成真实两 guest、64 KiB、两轮 `cpu-reference`
回归；保持 16 MiB aperture、64 项幂等表、seed=41、独立服务 `3e5b35b`、
kernel r109 与 r272 initramfs。验证器确认四个 V2 视图（offset=128/4224）、
generation=1/4、完整 checksum、真实 route 和六个退役 allocation；原容器
13:54:45 至 13:55:00 UTC，exit 0、无 OOM，结束后未发现遗留 QEMU。
原日志归档 `gsva-retire-evidence-r278.tar.gz` SHA-256：
`eba943b1fb2638853c81d9c719bf38f2db90bdfc84d147a5048fb5d7f1505c16`。
这是已有两轮正常流程在新 QEMU 上的回归，不代替 r273 的三轮失败、真实
跨 holder retire 排空或百轮地址复用验收。

r280 完整 Rust workspace 原作业 13:54:48 至 14:03:37 UTC，exit 0、无 OOM。
逐个核对 43 个外层 harness 的最终结果：963 通过、16 ignored、零失败、零
filtered，清单与 r271 完全一致；未计入内嵌测试进程的重复结果。日志 SHA-256：
`2c39efc6f5300dcdda3e762db535d9732820583d8ec9057ebab099ba2b66cb08`。
ignored 和 Python 的环境跳过均不算通过。

QEMU 三个源码/测试文件已本地提交为 `924148cf5c`，未推送；父仓库未更新
QEMU gitlink。上述运行使用明确分层挂载的任务源码和构建产物，不能当作
当前已发布平台 pins 的 fresh-clone 验收。`mem_service` 保持已发布并锁定的
`3e5b35b`，本批没有新增独立服务改动或任何仓库推送。

### r281-r286：完整严格 I/O 寿命与 managed unmap 冻结

严格 CPU/PTO 访问现在按整个同步调用计数，覆盖多个分片及 RX 重入；单个
应答的 pending 清零不再允许提前解除映射。managed V2 unmap 先将 route
冻结，保留 MMIO 所有权和地址区间；仍有完整调用时返回 pending。fence
期间拒绝新严格访问和 V1/V2 重入 unmap，失败保留冻结状态，确认完成后
才释放 route 并刷新 TLB。被冻结区间拒绝其他 segment 的重叠映射。

r281 通过项目构建入口重建 QEMU，七个底层测试程序共 45 项通过；新增
真实 route/QOM 测试验证冻结期间的 token/PTO 拒绝、区间占用及解除后的
替换映射。Python 的生产函数提取夹具还覆盖完整调用与嵌套调用寿命、
读写错误、计数溢出、fence 失败及重入清理。该夹具模拟 RX 回调，不能
计为真实 guest 的在途撤销故障注入。构建容器 14:15:52 至 14:16:14 UTC，
exit 0、无 OOM；已有 `ub_ubc.c` alias 初始化警告保留，未改无关路径。
QEMU binary SHA-256：
`3df8c9edb49152a2412b040b80a326f0a18e0bf94ca5589f74a8fa66bf1cd230`。
七文件源码 overlay SHA-256：
`0f8c28ff5cffcb62d905ae3a68c4b1fe3871725f4520feda6e467618a32afe87`。
提交前逐字节核对本地七文件与该受测 overlay 一致。

r282 远端 Python 脚本与接口全集为 544 个唯一用例：539 通过、5 项
AArch64 交叉编译器环境跳过、零失败/错误/加载错误。结果 SHA-256：
`1e9c444c269a0996fbf4d7b5db32fdc9a1e5e1f6ba11dd20a5df90def65fb6cb`。
r283 原 Rust workspace 作业 14:16:52 至 14:25:39 UTC 完成，exit 0、
无 OOM；43 个外层 harness 共 963 通过、16 ignored、零失败/filtered，
清单与 r280 一致。日志 SHA-256：
`80ceef68480857bb59e7336587baaf027404f8a06ab1af619774ad7065b97682`。
跳过和 ignored 均不计为通过。

r284 在新 QEMU、kernel r109、r272 initramfs 及服务 `3e5b35b` 上完成
实际双 guest 的两轮 64 KiB V2 CPU 共享：四个非零偏移视图、generation
1/4、真实 reader route、checksum 与六个 allocation 退役通过。容器
14:19:13 至 14:19:28 UTC，exit 0、无 OOM。保持 16 MiB aperture 和
64 项幂等表，未启用地址复用。报告 SHA-256：
`47b99335680ec5ac0b40311b71a0043e99c7c43bab3eb47ac4eb2a185fa7fff7`。
原始日志归档 `gsva-drain-evidence-r284.tar.gz` 为 92,491 bytes，SHA-256：
`7f389b230a408e759b538363bd842c9a114ff12d3866163f43067f979adf4e26`。

r285 从既有 PTO 镜像保留已编译 fixture/driver/provider worker，只换入
r272 的服务 CLI 与 run_app；CLI SHA 已核对，对应已发布 `3e5b35b`。
派生镜像 SHA-256：
`eae4cc12c16028f3d993d795203f45d8ab94c5b33507b4cdba5659ce92c239aa`。
r286 复用 r252 的 home 1 / compute 0、tail 65,024-byte 张量、四线程、
只读 V2 输入双请求配置，在新 QEMU 上整轮通过；14:25:05 至 14:31:08 UTC，
exit 0、无 OOM，结束后未发现遗留 QEMU。两请求同时持有、六条 lease、
4 LOAD/2 STORE/2 fence、68 LOAD 和 34 STORE 实际分片、独立数据核对及
最终清理通过。报告中的三个原始日志 SHA 已独立重算一致，报告 SHA-256：
`780f2f757618ba164b1566b8433f48561272211918faf47327ced860ca617615`。
原始日志归档 `gsva-drain-evidence-r286.tar.gz` 为 63,329,417 bytes，SHA-256：
`d84f59712309f521aabdb5e4ac7627f9b2830e24780550b1643efcdd868afda2`。
首次证据归档因目标 Python 缺少 `hashlib.file_digest` 失败，改用已有
sha256sum 后完成；未重新启动 guest，失败观察记录保留。

该批验证覆盖正常 managed CPU/PTO 清理和隔离的重入边界。远端 RETIRE
接收端强制撤销、真实在途故障、地址复用及百轮压力仍未完成，r273 三轮
容量失败仍有效。设备并行执行、W5 模型矩阵与全部后续阶段不计为通过。
QEMU 已本地提交 `d48c2c7634`；父仓库另行提交对应测试和本记录。
本批不推送、不更新未发布平台 gitlink。

### r287-r293：内核选址复用、容量结果分类与可配置地址窗口

worker 已删除单调地址游标，设置 `requested_home_va=0`，由现有内核
区间分配器选择空闲区间。完整正常回收仍要求 holder/ref 归零、实际
unexport、内核 retire 和服务 reclaim 确认；新分配使用全新身份。
只有直接 ALLOC_SEGMENT 返回 ENOSPC 且 descriptor 全零，才按平台的
保留前失败契约取消无资源请求。EFAULT、其他 I/O 错误、矛盾 descriptor
和日志同步失败保留占用意图并停止处理，不能仅凭全零输出声称没有资源。

新增边界测试直接提取实际内核分配函数和 ioctl 分支，验证 ENOSPC 在
预留前发生，以及成功分配后 copyout 失败返回 EFAULT 并保留真实预留。
worker fixture 包含同一地址百次分配/回收、身份更新和旧 reclaim 拒绝。
这组包装边界测试不计为实际 guest 百轮。

r287 原生测试在已有 worker-loop fixture 处失败：新用例留下的
`empty_cancel` 状态未恢复。显式初始化后，r289 原生 worker 和 manager
通过；r289 随后因新内核 fixture 缺少 ioctl include，以及在不完整平台
目录执行全模块而失败。保留两轮原始失败日志，没有将其计为全量通过。

r288 独立服务全量共 276 个唯一用例，16 个隔离分片逐项核对覆盖一次：
271 通过、5 跳过、零失败/错误。跳过包括 3 项下游源码契约、1 项缺少
交叉编译器的打包测试、1 项缺少非 loopback 接口的测试；均不计为通过。
结果 `reuse-results-r288.json` SHA-256：
`d015024532f6d7bbfbdef7f84a2bbdc65ba47c5373d14cdc58f04360d9fbb007`。

r290 修正 fixture 后原生及 ASan/UBSan worker、manager、host artifact、
安装态 SDK、9 份 CLI manifest 对照均通过；13 项定向测试通过，包括
实际内核分配/copyout 边界和地址容量验证器。原作业 14:52:43 至
14:53:37 UTC，exit 0、无 OOM。日志 SHA-256：
`114312a05e7868ec623dabc04761977409ecd166f24acb93527fe43952ee1d94`。

独立服务提交 `3280c64324d34d32e86b8d75b88d7be9bf7f326c` 已按授权仅
推送至该仓库 `origin/codex/gva-managed-service-completion`，ls-remote
确认完整 SHA 一致。父仓库本地 gitlink/lock 同步，源码锁定检查通过；
父仓库及平台子仓库未推送。

根据用户确认，正常业务允许扩大地址窗口。双节点 `--config` 增加
`MEM_SERVICE_OBJECT_SHARE_APERTURE_SIZE`，默认 auto 保留原场景行为；
支持 2 MiB 对齐、2 MiB 至 4 GiB 的十进制字节数，故障容量场景保留 auto。
此范围属于测试入口验证范围。不同对象可使用不相邻区间；当前每个
segment 和单节点 aperture 仍各自连续，多 aperture 池尚未实现。
22 项轻量定向测试及 shell 语法检查通过。原 r273 的“不扩大 aperture”
决定限定为小窗口压力测试，不再约束正常业务验证；原失败证据保持有效。

r293 从已发布的干净服务源码同时重建静态 AArch64 服务 CLI 和 provider
worker，替换 initramfs 中两者与 run_app，避免只更新 CLI 而运行旧 worker。
构建、ELF 架构、静态链接、脚本语法及压缩包校验通过，15:05:17 至
15:05:33 UTC，exit 0、无 OOM。镜像 `managed-replay-r293.cpio.gz` SHA-256：
`f579dd6235ba573ff77be26b20444ccd5b493ff0ae3e9839fe9f371358d2fb80`。
内核使用既有 r109 镜像，QEMU 使用 r281 严格访问保护产物；两者未改源码。
后续实际运行结果独立记录，以上编译和 fixture 结果不替代运行验收。

### r291-r298：三轮复用通过，百轮转入控制面延迟问题

r291 父仓库 Python 全量 547 个唯一用例：542 通过、5 项交叉编译器
环境跳过，零失败/错误/加载错误；清单逐项唯一，15:05:17 至 15:06:39
UTC，exit 0、无 OOM。结果 SHA-256：
`4f4802d06848bcb09f0ce1b79e4c024f80c502586e3a7abe07f2ec908717bc44`。
r292 完整 Rust workspace 原作业 15:05:17 至 15:14:04 UTC，exit 0、
无 OOM。43 个外层 harness 的最终结果合计 963 通过、16 ignored、零失败；
嵌套测试输出不重复累计。原始日志 SHA-256：
`ba8a3117ed00fd032f59a58642d5410d162475f8d8667d820f4600e0316607f0`。

r294 实际双 guest 使用 16 MiB aperture、64 项幂等表、64 KiB 对象及
seed=41，三轮全流程通过，15:06:40 至 15:06:58 UTC，exit 0、无 OOM。
三轮数据对象都位于 `0x700000000000`，generation 为 1/4/7，segment
分别不同；各轮 payload seed 递增，6 个 reader 视图与独立 checksum
核对通过。9 次分配完成实际回收，每轮服务资源归零，最终 kernel census
恢复基线。该结果越过 r273 的第三轮地址容量失败点，尚未证明旧 token
再次访问被拒绝、强制撤销或跨重启恢复。

r294 报告 SHA-256：
`e5adb26667be3083f22b5fdd6597af1eaec27fae02b9402f56925036f3b153a6`。
首次归档核对发现报告中的 guest 日志指纹采用 Python read_text 的换行
归一化文本，与串口原始字节不同。按验证器现有实现独立重算后完全一致；
原始文件另记 raw SHA，未修改日志或运行报告。归档 `reuse-evidence-r294.tar.gz`
为 111,060 bytes，SHA-256：
`8fd915c777c6a7ae0a4071036be9e6e4d8eddb008e34ea03704343712e825ee9`。

r295 将 aperture 扩到 1 GiB，运行 100 轮；r296 以同一镜像、QEMU、
对象大小和轮数恢复到 16 MiB 作对照。两者均完成 26 轮，第 27 轮
generation=79 分配和 begin_reference 成功，随后 map 返回 internal /
map_cleanup_required；两 worker 记录 wire io timeout 并停止，保留
reconciliation_required。最终均有 1 个对象及 holder 未确认清理，
allocate_rejected_count=0、idempotency_available=59、history_records=730。
两次均不能计为百轮通过，也不能计为最终资源回收通过。r296 的 init
退出还导致 kernel panic，保留原始失败，不过滤为成功。

r295 15:07:59 至 15:09:32 UTC；r296 15:14:58 至 15:16:31 UTC，均 exit 1、
无 OOM。r296 在本批 Rust 作业结束后启动，仍复现同一轮次失败。
原始归档分别为：

- `reuse-evidence-r295.tar.gz`：445,735 bytes，SHA-256
  `bf91f3dd765698b323f83b3cb7f430aa85f98442ad599ab74876817a2493ab63`。
- `reuse-evidence-r296.tar.gz`：446,266 bytes，SHA-256
  `c749c05f44abcdfc54ab1b295b4c4e8be83f1b79feac685947ccdeacc60f792e`。

r297 为单独的真实 address_capacity 场景：5 个对象无空洞覆盖 16 MiB，
超额请求得到 reserve-empty/cancel-confirmed；释放 8 MiB filler 后，
新对象复用其地址并获得新 descriptor/generation。再释放新增对象，原始
读写流程继续，最终新增资源归零。两节点 mapping_resources、kernel_regions、
address_capacity 验证均通过，15:11:35 至 15:12:32 UTC，exit 0、无 OOM。
原始归档 `reuse-evidence-r297.tar.gz` 为 1,775,993 bytes，SHA-256：
`d5a7d70a19d41dbb9441836cec3d896cb17db7cec0b9c772789d5afa3693d749`。
所有 guest 原作业结束后均核对无遗留 QEMU。

r298 在隔离原生容器对已发布的 replay history 源码包装 read 计数，
固定 128-byte 合成响应：从 671 条追加 59 条至 730 条，触发 82,718 次
read、8,047,954 bytes，耗时 36.642 ms；一次不存在 key 查询触发 1,462 次
read、142,256 bytes，耗时 0.651 ms。这是存储边界成本诊断，不能当作
guest 或业务性能。源码确认 make_room 对每条缓存记录分别 append，
append 每次重新全文件校验；worker TCP 默认截止时间为 3 秒。
该证据证明归档的重复扫描放大，具体 guest 超时因果仍需修复前后测量。
诊断 exit 0、无 OOM，日志 SHA-256：
`05dd35f034325d8000afd2f2e676c46290a2df2a0c4b496689c12746d8f5bac0`。

下一项优先补有界批量归档，在维持整份历史完整性校验、冲突拒绝、原缓存
持久化后才淘汰等语义的前提下减少重复扫描，然后重跑两种窗口百轮。
M2/M3、旧身份实际拒绝、W5 模型矩阵、故障恢复及完整目标保持未完成。

### r299-r305：有界批量归档与干净发布

独立服务增加内部 `mem_service_replay_history_append_batch()`，每批最多
等于现有幂等缓存容量。先校验全部输入及批内重复/冲突，再完整扫描历史
一次，对所有候选 key 核对精确响应。历史尾部即使在已命中 key 后损坏，
仍拒绝整个批次。预检完成才追加缺失记录，统一 fsync；全部成功才更新
内存 checkpoint，daemon 持久化含 checkpoint 的 snapshot 后才清理缓存。
写入失败可能留下完整或半写后缀，保留旧 checkpoint 和整个原缓存，句柄
标记 failed；重新打开验证完整后缀，半写不自动截断。

原单条 append 复用批量接口，历史帧字节、wire、SDK、请求截止时间及
无 store 时的容量拒绝保持不变。本次修改限于历史库、daemon 接入、测试
及契约文档，没有改 kernel、QEMU 或增加自动重试。

r299 第一次测试编排在 shell 分组结尾出现多余分号，原作业 exit 2，测试
未执行；修正编排后的 r299b 在 15:27:06 至 15:27:08 UTC 完成 15 项
历史库测试，全部通过。覆盖精确格式、旧应答重新打开、批内/历史冲突、
坏尾部、第二条写入半写、批量同步失败和全部失败后保留 checkpoint。
671 条历史上追加 59 条仅 1,344 次 read、1 次 fsync；原 r298 同规模为
82,718 次 read。读次数下降约 98.4%，该数字属于边界测试的系统调用计数，
不作为 guest 或模型加速比。

r300 独立服务全量 16 分片逐项核对：282 个唯一用例，277 通过、5 跳过，
零失败/错误，覆盖清单完全一致且无重复。跳过为 3 项下游源码契约、1 项
交叉编译打包和 1 项非 loopback 接口测试，均不计通过。结果文件
`batch-results-r300.json` SHA-256：
`081dc632ec9646fd96591f842c596b9eb6fb88c574a59abdbf60de35a5568e3e`。

r301 原生/ASan/UBSan worker、manager、host artifact、13 项平台定向
测试、安装态 SDK、9 份 CLI manifest 对照，以及历史库全部 15 个 ASan/
UBSan 模式均通过。15:28:01 至 15:31:09 UTC，exit 0、无 OOM。原始日志
`batch-artifacts-r301.log` SHA-256：
`8737b99a57930f3e1fcc3b8a314c1814d3ff54e948a5c383765249bd7cf19d17`。

8 个服务文件逐字节核对与测试快照一致后，提交
`9e6aa1c3adc5dcf09fc3c972730709154ba57f8a`，仅推送已授权服务分支并用
ls-remote 核验完整 SHA。父仓库本地 gitlink/lock 同步且锁定检查通过；
父仓库未推送。后续基于这个干净提交分别验证父仓库和实际 guest，不能
用以上测试宣布 r295/r296 超时已解决。

### r303-r307：干净锁定回归与两种窗口百轮对照

r303 使用已发布服务 `9e6aa1c`、同步的 gitlink/lock 和相同下游源码执行
完整 guest 目录 Python 回归：547 个唯一用例，542 通过、5 项交叉编译器
环境跳过，零失败/错误/加载错误。15:38:02 至 15:39:25 UTC，exit 0、无 OOM。
结果清单与上一版一致，原始本轮日志 `batch-root-r303.log` SHA-256：
`8a8c2ee9bdfffcb501bd6a5730892b5dda2e13cfaf382d96745efb3caa2fea68`。

r304 完整 Rust workspace 的 43 个外层 harness 最终结果合计 963 通过、
16 ignored、零失败；嵌套用例输出不重复累计。15:38:02 至 15:46:57 UTC，
原容器 exit 0、无 OOM。原始日志 `rust-workspace-r304.log` SHA-256：
`4c261aff3b4323278245b63179f19bce77083a18c73966d350278393a62ff52b`。
逐 harness 结果 `rust-results-r304.json` SHA-256：
`2c503f2ccfc010f2177ee146604a3ae9d38139fc93ddc1e8529bb172ea6782dc`。
本节所有完整回归及实际 guest 均在指定远端执行，本地未运行全套测试。

r305 从干净发布源码同时构建静态 AArch64 CLI 和 worker，替换 initramfs
两项二进制与 run_app；内核 r109 和 QEMU r281 保持不变。15:38:03 至
15:38:18 UTC，exit 0、无 OOM。`managed-batch-r305.cpio.gz` SHA-256：
`ed54a4e464cd03a901b9732256844607611d2bcdb73ae60bbeae1ea46595ab2c`。

r306 在 16 MiB aperture、64 项幂等表、64 KiB 对象及 seed=41 下完成
100 轮实际双 guest 全流程，15:39:33 至 15:47:44 UTC，exit 0、无 OOM。
300 次 allocation 退役、200 个只读视图、100 个独立 A payload checksum
通过核对；100 个不同 generation/segment 的数据对象复用同一 home VA
`0x700000000000`，最终资源和 kernel census 验收通过。原作业结束后确认
无遗留 QEMU。请求截止时间仍为原默认 3 秒，没有增加容量、超时或自动重试。

r306 原报告 SHA-256：
`a207e92fcbe2838906f0ce223d39dae9daa104e1877f55eac34b463016d5e697`。
`batch-evidence-r306.tar.gz` 为 1,540,936 bytes，SHA-256：
`dddcb5fc0e0eb06e38060c8efce75a5b939215ffae1402513c2dff51f5e62dee`。
报告的换行归一化日志指纹与原始字节指纹分别核对，未改原日志或判据。
该结果越过相同小窗口 r296 的第 27 轮失败点，关闭固定尺寸百轮循环子项。
混合尺寸、对齐、交错释放、旧身份实际拒绝及完整 M4 仍未完成。

r307 使用同一镜像和默认请求截止时间，将 aperture 扩大到 1 GiB，同配置
百轮对照通过：15:51:34 至 15:59:48 UTC，exit 0、无 OOM，300 次退役、
200 个视图及 100 个 A checksum 核对通过。100 代数据对象复用相同 home VA，
segment 各不相同；最终资源归零，确认无遗留 QEMU。原报告 SHA-256：
`ca27c2e87c73a1d3e1912bc685f3c9cab5eb2b6950d0f6092282f76e84725704`。
`batch-evidence-r307.tar.gz` 为 1,540,907 bytes，SHA-256：
`037bd5a37316228b78c8fc2cdadea75e278a06e9e5f16adce2155126311a19fa`。

两种窗口均保持原 64 项缓存与 3 秒请求截止时间。批量归档后均完成固定
尺寸百轮，原 r295/r296 的第 27 轮超时在这两次对照中未复现；该结果支持
本次修复消除这一已观察到的失败，不证明任意历史规模下的长期延迟上界。
扩大预留窗口、窗口内分散放置和确认回收后复用是互补能力；多 aperture 池
尚未实现，混合碎片压力、故障恢复、旧身份实际拒绝和完整模型矩阵继续保留。

### r308-r321：实际旧 V2 引用拒绝与完成屏障

沿用 `cpu-reference`、已发布服务 `9e6aa1c` 和现有 `reference-transition`
CLI，reader 在 ready 对象 active 后 resolve 并保存两份真实完整 V2。从
第二轮起，新对象已发布时，提交上一轮原始引用执行 resolve、acquire 和
map-begin；只接受 stale_ref 或已移除身份的 not_found，任何成功、internal、
超时或解析失败均失败。校验器逐字节对照上一轮 writer 发布，核对操作顺序、
新旧 generation、实际地址和随后新引用的数据读取。该组属于控制面旧引用
准入证据，不替代旧 provider token、CPU 映射或在途撤销的实际访问拒绝。

检查最初置于 reader session 启动前。源码核对发现双方 endpoint canary
需要同时进入，故在实际 guest 启动前改为并行检查与 `ref-checked` 屏障。
reader 必须等屏障 active 才 acquire 新引用，writer 负责最终退役四个对象；
helper 异常退出保持失败，父流程退出时等待或终止本次 helper。r308 初版
镜像和 r309 Python 回归通过，但未执行实际 guest；r310 初版 Rust 的 43 个
harness 为 963 通过、16 ignored、零失败，16:10:34 至 16:19:29 UTC，exit 0。
这些初版结果不作为最终源代码的回归证据。

r312 镜像、r313 完整 Python 545 通过、5 跳过后，r315 实际三轮发现新增
检查屏障排在原完成屏障之后释放。reader 观察 done 已退役就检查资源，此时
checked 仍存活，完整验收报 `service resources not drained: live_objects`。
原作业 16:15:20 至 16:15:39 UTC，exit 1、无 OOM；两端流程最终均结束并清理，
不能把最终清理结果覆盖中间顺序错误。原始归档 `stale-evidence-r315.tar.gz`
为 111,304 bytes，SHA-256：
`2805cb0eeed007309c29bea51256cd48e0a6b4d077283f9c50cd91b58b61d09f`。

修正为 key、ready、checked、done 的退役顺序，使 done 始终最后确认；
资源判据保持不变，增加顺序与并发启动回归。15 项本地轻量测试及 shell
语法检查通过。r317 冻结四文件快照 SHA-256：
`c4c9ebc427a032578068e60480d39949e565a6569a2821df23860c57fe245f17`。
r318 用该快照重建镜像，服务二进制、kernel r109 与 QEMU r281 均未改源码；
16:18:37 至 16:18:52 UTC，exit 0。`managed-stale-r318.cpio.gz` SHA-256：
`234d2680e92cf9436e347d9cd62acdeb9e332a76f06059c8edec9cf3150bfb06`。

r319 最终 Python 完整回归精确覆盖 551 个唯一用例，546 通过、5 项交叉
编译器环境跳过，零失败/错误/加载错误；16:18:37 至 16:20:00 UTC，exit 0、
无 OOM。`root-results-r319.json` SHA-256：
`76ce901a698fd5c9b9e9a66367163fba093e0c9338a9fa3b402d68e322fc5fb5`。

r321 同配置三轮真实 guest 重跑通过，16:19:36 至 16:19:54 UTC，exit 0、
无 OOM。12 次分配退役、6 个新视图、3 个独立 A checksum 核对通过；数据
generation 为 1/5/9，segment 各异，home VA 均为 `0x700000000000`。
第二、三轮合计 12 个原引用 resolve/acquire/map-begin 都返回 stale_ref；
每项提交的完整引用与上一轮实际发布一致，新引用随后读到新数据。最终
服务/provider/kernel 资源验收通过，确认无遗留 QEMU。报告 SHA-256：
`c73386e5efe52b1f324cf9765a6f6aace3ae614d7b97b710a0513ff9b74fe925`。
`stale-evidence-r321.tar.gz` 为 112,596 bytes，SHA-256：
`76d8b392699518105ea060ba25628923272b746e11b3e7edd2272fc9c80a14de`。

r320 最终快照的完整 Rust workspace 原作业 16:20:26 至 16:29:13 UTC，
exit 0、无 OOM；43 个外层 harness 最终结果合计 963 通过、16 ignored、
零失败，未重复累计嵌套测试输出。原始日志 SHA-256：
`d8da06bb97a92ac466c0d2a1902704c2f34cea434ff154bb17be01cf6cedd6b1`。
逐 harness 结果 `rust-results-r320.json` SHA-256：
`fc4b47c6c97e101b846eeff23930224f2f1979ea6eda891a985e6dd42470eea4`。
r322 使用同一最终镜像、16 MiB aperture、64 KiB 对象和 seed=41，百轮
带旧引用检查的实际双 guest 验证通过；16:21:15 至 16:31:16 UTC，exit 0、
无 OOM。100 代不同 segment 的数据对象复用 `0x700000000000`，200 个
新视图及 100 个独立 A checksum 通过。400 次分配全部退役，包含新增的
检查屏障；最终服务/provider/kernel 资源归零，确认无遗留 QEMU。

99 次轮间复用各检查两份旧引用的三个入口，共 594 次明确拒绝：192 次
stale_ref、402 次 not_found。每次请求都与上一轮真实发布的完整 V2 一致，
并关联新代同址对象和后续新数据读取；任何内部错误或超时都没有计入通过。
worker 控制 RPC 仍使用原默认 3 秒，object-session 和新增 reference CLI
使用显式 30 秒客户端超时；两层截止时间分别记录，不混称为单一超时。
报告 SHA-256：
`4c67145c868f64490b67b4dbe4b21e4852bad59a09e371b24f71b1edeb327815`。
`stale-evidence-r322.tar.gz` 为 1,572,900 bytes，SHA-256：
`63d4cfd426c6ef2802a4867b59386a54a53f8dd5ec21c2adc2b9d104f9fe321c`。
原始字节与报告采用的换行归一化日志指纹分开核对，原失败 r315 保留。
M2/AM3 的 provider token、旧 CPU 映射、在途 drain 和故障恢复仍未关闭，
W5 模型矩阵及完整目标继续保持未完成。

### r323-r330：同进程解除映射后的实际 CPU 故障检查

独立服务 `423665411a9fbf14b251e44d7cb177b149956e87` 新增可选诊断
`unmap key=<key> probe_unmapped=1`。当前可读映射先被实际读取，managed
unmap 确认后再在原进程读取保存的地址，只接受该地址的同步 SIGSEGV/SIGBUS。
清理失败时不执行探针且保留重试归属；默认 unmap 不增加 CPU 访问。
新增测试覆盖未映射、错误 key、重新映射、非法参数、munmap 假成功以及
munmap 失败后重试。r323 四项定向测试通过；r324 最终候选包含明确的
`unmap_confirmed=1` 回执，不改变原 cleanup_pending 失败检查。

r324 独立仓库全量 286 个唯一用例，281 通过、5 环境跳过，零失败/错误；
执行清单逐项覆盖四个新测试，2026-09-14 16:49:27 至 16:56:08 UTC。
结果 SHA-256：
`50e1426d5fb09f0a5af5fb4fa4f04a73cec1d2da01a1114d22a7084e44a30229`。
r325 构建、ASan/UBSan worker/history、安装 SDK 及九份 manifest 比对均
通过，16:49:30 至 16:52:39 UTC，exit 0、无 OOM。日志 SHA-256：
`51f628de17eb54af84c36c65ffd3f86515fc7fb7e862d5a77d27dafbb20ac323`。
送测六文件与最终工作树逐字节一致。该提交已仅发布到授权的 mem_service
分支，远端 ref 完整 SHA 核对一致；父仓库 gitlink/lock 本地同步并通过
source check，父仓库未推送。

r327 使用干净锁定源码构建 AArch64 静态 CLI/worker，保留 kernel r109
和 QEMU r281。镜像 `managed-unmapped-r327.cpio.gz` SHA-256：
`3f9303bb892a926401afd56e2f55e606365ed1bfae87fd421f31efa7426051c2`。
父仓库送测快照 `unmapped-root-r326.tar.gz` SHA-256：
`fdc696c832a1c021e12d1db4128612ed58a66905dc0b672fa3f7b8851f8c0609`。
r328 Python 全集 553 个唯一用例，548 通过、5 交叉编译器环境跳过，零失败、
错误及加载错误；16:59:37 至 17:01:00 UTC，exit 0、无 OOM。结果 SHA-256：
`604b40098e38977ecc545fe20411f6f226f4994ad2953605f0c33ce339711fdd`。

r330 在指定实际验证环境运行双 guest 三轮，17:01:02 至 17:01:21 UTC，
exit 0、无 OOM。16 MiB aperture、64 KiB 对象、seed=41：writer 的三次
和 reader 的六次解除映射后 CPU 访问均出现实际同步故障，validator 核对
对应 session/key/generation/address 及 map/unmap 位置。六个新视图、
三代同址不同 segment/checksum、12 次旧 V2 控制面拒绝、12 次分配退役
及最终服务/provider/kernel 资源回收均通过，原作业结束后确认无遗留 QEMU。
报告 SHA-256：
`6648d34660573283641d7acdf93973f02e08ab69342711150a9c0c1e9372f570`。
归档 `unmapped-evidence-r330.tar.gz` 为 112,677 bytes，SHA-256：
`a7080458451b03c2ec601154cfbb885904f1c5c26ef28e9876e4d2775d076b08`。

上述证据只关闭正常 unmap 后、新映射建立前的 CPU 不可访问子项。旧
provider descriptor/token、强制撤销、在途访问、同址新映射后的旧访问
上下文以及跨重启恢复保持待验收；不能用正常解除映射的故障取代这些证据。

r329 完整 Rust workspace 在 16:59:37 至 17:08:23 UTC 运行，exit 0、
无 OOM。逐个核对 43 个外层 harness 最终结果，963 通过、16 ignored、
零失败，未累计嵌套用例输出。日志 SHA-256：
`c977241f5d3476e53d902d6cc6940fed80977946917f90bbe7a2943e75de78a5`；
`rust-results-r329.json` SHA-256：
`c793d2a68e59232903b43bdb4d8e792f0911345ed975ca194ca5fd5f2531d905`。
r323-r329 的定向/完整 Python、构建、安装验证及 Rust 原始日志归档
`unmapped-unit-evidence-r329.tar.gz` 为 196,571 bytes，SHA-256：
`965b15485d05e2304ba1936fb19d46dc07c4d64e43948d6c7997838b0cf0123f`。

r331 使用同一镜像、16 MiB aperture、64 KiB 对象及 seed=41 完成实际
双 guest 百轮，17:02:34 至 17:12:42 UTC，exit 0、无 OOM。writer 100 次、
reader 200 次原地址访问均在正常 unmap 后发生预期 CPU 故障；每次关联
实际映射和完整代际。100 个新 generation/segment 复用同一 home VA，
200 个新视图和 100 个 A checksum 均核对通过。594 次旧 V2 请求拒绝
包括 192 次 stale_ref 和 402 次 not_found；后者对应退役身份槽位回收。
400 次分配完成退役，最终服务/provider/kernel 资源回到基线，无遗留 QEMU。
报告 SHA-256：
`d6a043b1645d32c6d97b8ca5f8b060ad8f2c2eb369560738e11385e4589c0f86`。
`unmapped-evidence-r331.tar.gz` 为 1,576,821 bytes，SHA-256：
`d3f56e703dc40317fa756aaa1d5556b27350c46853ffc7e1ac7f2531b117d1a8`。
完整原始日志保留；报告的换行归一化指纹和原始字节指纹分别核对。
证据回传曾出现两次 SCP 和一次状态查询的 SSH 握手超时；仅重试传输和
查询，原 r329/r331 作业未重启，网络审计保留各次结果。扩大的是 SSH
握手等待预算，guest 业务请求截止时间、测试内容和通过条件保持不变。
完整计划仍未完成；上述正常 unmap 验收不关闭旧 provider 身份、强制
撤销、恢复、W5 模型矩阵及 tokenless 联合失效的剩余要求。
