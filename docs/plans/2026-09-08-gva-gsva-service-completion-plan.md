# GVA / GSVA 服务能力补齐：实施方向与阶段计划

日期：2026-09-08；修订 4（2026-09-11）：将计划范围收敛到 OBMM 内存 backing
及 GVA/GSVA 服务主线，保留修订 3 的控制通道设计与历史审计依据。
状态：实施决策已明确；远端 readiness、模型 artifact 和发布授权仍待执行时确认。
以下里程碑均未由本次文档工作完成。

2026-09-09 补充：[gva_manager 接入实施计划](2026-09-09-mem-service-gva-manager-integration-plan.md)
细化地址管理主线，记录后续 managed allocation/provider 代码进展，并把同一全局对象
的 CPU load/store、TLOAD/TSTORE 联合访问与安全地址重用纳入硬验收。地址管理按该文
AM1 至 AM4 推进；下文原有源码计数和里程碑状态保留为修订 3 的历史盘点，
不能用于判断最新代码尚未实现这些控制接口。性能等其他工作仍由本总计划追踪。

2026-09-11 实施记录见
[受管理 GVA 服务实施与验证记录](../2026-09-11-gva-managed-service-implementation-progress.md)。
本轮接通常驻 worker、正常释放、取消与迟到发布、session 可见性操作及双 guest
入口；r20 已通过两轮 64 KiB 双向 CPU 共享、可见性、同代重新映射、双方只读及
guard page 探针与正常回收。后续 r24 在新增多 key/session holder 跟踪后再次通过
同一双 guest 组合；新增 6 项跨对象、session 隔离、旧应答重放及异常退出回归通过。
完整负例和实际资源统计仍待补齐；M1/AM1 保持未完成，后续阶段未被计为通过。
本次范围调整不改变已有验收结论。

2026-09-14 续作：AM2 已扩展 managed 双对象、只读输入及四线程访问寿命。
ND 的 64 KiB 张量实跑发现并修复 quadratic 中间张量分配缺陷；r176 的两轮输出、
权限、回收和 102 个实际分片已由 r174c 从完整原日志独立复核通过。原 runner
因分片解析缺陷失败的记录保留。r177b 的大尺寸六负例、24 次完整视图不变和
两次正常计算整轮 exit 0，实际核对 170 个分片；r181b 对两轮原日志复验的判据、
计数及 SHA 与先前结果一致。r178 的 tail 四线程、两次原 completion 恢复、
102 个分片及完整数据/回收判据整轮 exit 0。多 home、多请求计算并发及后续阶段
仍待验收；完整总计划未完成，最新边界以实施记录和 AM1-AM4 追踪表为准。

同日 home 互换续作：managed CLI 和 runner 可选择实际 home 0/1，并核对
真实 home 导出、计算节点及 requester。r186c 的反向只读双对象四线程经
独立原日志复核通过；r191/r192 的原方向单对象与反向只读双对象权限矩阵、
r193/r194 的两方向四线程原 completion 恢复均整轮通过。启动环境槽位溢出与
worker 提示解析失败均保留诊断及修复前记录。该子集使用 cross-page 视图，
不关闭同时多 home、多请求计算并发、全局对象引用绑定或完整 AM2。

同日多请求续作：endpoint 已实现有界 enqueue/poll 及按原 op ID 领取完成，
覆盖容量、乱序完成、迟到回放、协议故障保留、同步恢复兼容和双调用线程夹具。
managed CLI 已接入 `concurrent_requests=2`：共享 SDK 会话及 A/B 输入，每请求
独立 metadata、三份 lease 和 C/D 输出。r207 的反向只读双对象四线程、r208 的
原方向单对象无 CPU reader 双请求均整轮通过，六份 lease、原 completion、独立
数据 oracle 及最终资源归零通过。r204 的重复打开会话失败记录保留。
r209 的反向 ND 64 KiB 双请求、r210 的原方向 tail 65,024-byte 双请求及各自
102 个实际分片也整轮通过。tail 反向 home 和多请求故障恢复仍待完成。
队列同时持有两个请求的证据不证明设备并行执行，后者仍未验收。
完整相关回归及本轮运行边界见实施记录，AM2 与总目标保持未完成。

同日全局引用续作：独立服务新增 opt-in 的 256-byte V2 编码，保留 V1，明确
allocation-relative offset、完整 key、generation、home/incarnation 和访问权限。
r215 原生/安装态 codec、完整 CLI 夹具、ASan/UBSan 与五份契约清单比对通过；
r216 完整 Python 回归 244 项为 239 通过、5 项环境跳过、零失败/错误，执行
清单无遗漏或重复。权威内容版本与 allocation 绑定、SDK acquire/map、W5
仍未接通，编码通过不关闭 AM2；详细源码指纹与验收边界见实施记录。

随后已补上 service core 的显式 record/allocation 绑定与 begin/stage/seal、
resolve/acquire：写入前推进版本，按登记的完整 V2 和当前 allocation 核对引用，
拒绝旧 raw 入口绕过。r219 完整 247 项为 242 通过、5 项环境跳过、零失败/错误；
原生、安装态 SDK 与 sanitizer 通过。本轮同时定位并修复 snapshot restore 的
大栈帧溢出，原失败记录保留，新增 1 MiB 栈回归通过。wire、SDK reader mapping、
provider 发布/可见性与 W5 仍未接通，core metadata 结果不代替实际数据闭环验收。

同日控制协议续作：已将 V2 begin/stage/seal/resolve/acquire 接入独立 `0x7e`
RPC、类型化安装 SDK 与 `reference-transition` CLI。重放前核对当前 generation、
内容 version、活动 home incarnation 与已登记视图；保留首次 stage、seal 和
release 的结果容量，拒绝旧 metadata 写入覆盖受管理引用。r224 完整 257 项为
252 通过、5 项环境跳过、零失败/错误；16 分片执行清单精确覆盖全部测试，
原生/安装态、sanitizer 和九份契约清单比对通过。独立服务提交为 `8ba34a0`。
V2 reader mapping、provider 发布/可见性闭环、W5 及重启恢复仍待完成；本轮
控制协议验收不关闭 AM2、AM3 或总目标。未 push，父仓库 gitlink/lock 未更新。

同日 reader mapping 前置续作：严格 OBMM provider 已允许固定地址的非零
allocation-relative offset，按 backing 基址加 offset 核对视图地址，保留完整
reservation、只读权限和视图外整页保护。服务提交 `41886ad`。r225 的文件共享
映射夹具、UBSan、完整 CLI 夹具和九份契约清单比对通过。完整回归首轮两项
fixture 编译超时；同源码、同断言与同超时设置补跑通过，合并执行清单精确覆盖
257 项，252 通过、5 项环境跳过，原超时记录保留。该结果仅证明 provider 边界，
V2 reader 的完整引用/版本/holder 映射准入仍待接通，未关闭 AM2 或总目标。

随后 reader 候选实现接入 `map-begin`、类型化 SDK 和 `object-session`。r226/r227
完整回归发现 CLI 在 resolve 后复用请求时残留较长逻辑 key 的尾部字节，导致
acquire 被本地规范字段校验拒绝；同时出现独立编译超时。原失败记录保留。
重新初始化 acquire 请求后，r228 原失败用例在指定远端完整通过。继续检查发现
loopback 可见性需要使用精确视图起点，已分离完整 reservation 与数据视图，并
增加跨页 offset=4224 的回归。r229 完整 260 项为 254 通过、5 环境跳过、
1 项失败：TCP 夹具仍分配 4 KiB，与跨页扩展后的 8 KiB 引用不符。已统一夹具
大小常量，r230 原失败用例及完整回归通过：260 个 test ID 精确覆盖，255 通过、
5 环境跳过，零失败/错误。r230 的 sanitizer、安装态 SDK 和九份契约清单通过；
两个子区间的真实 CLI 访问及清理用例通过，payload 为 loopback 合成数据。
reader 已提交为 `b5fff0d`，用户再次授权后仅推送独立服务同一分支，远端 SHA
核验一致；父仓库 gitlink/lock 已在本地同时更新，10 项下游契约测试通过。
父仓库未推送，真实 guest V2 发布/读取、W5 与完整后续阶段仍待完成。

writer 续作已提交独立服务 `d9b60ef`：SDK 从当前 allocation 和完整活动映射
生成只读 V2，确认 provider publish 后返回引用；object-session 接入
begin_reference/publish_reference/seal_reference，保留完整登记请求供重试，
已发布数据禁止继续写入。r231 定向 CLI 用两段不同非零数据核对完整引用，
独立服务完整 261 项为 256 通过、5 环境跳过、零失败/错误，执行清单精确覆盖；
安装态 SDK、ASan/UBSan 和九份契约清单通过。用户授权分支发布后已推送
`d9b60ef` 并核验远端 SHA，父仓库本地锁定提交为 `c675c9a`，父仓库未推送。

实际 V2 guest 续作：r233 暴露新增脚本等待了错误的 readiness 字段；r234
修正后配对数据验收通过，整轮因错误引用 bootstrap 路由标记而失败。修正为
逐一核对严格受管理 GSVA route、reader 地址、map/unmap 身份及 CPU window
清理后，r234 原始日志独立复核通过。r235 的一轮两视图、r236 的连续两轮四
视图实际 OBMM 双 guest 用例均整轮 exit 0，未发现遗留 QEMU；当前 27 项相关
远端测试通过。该结果仍不关闭完整 AM2、W5、后续故障矩阵或总目标。根仓库
完整回归待执行，guest 接入代码保持开发修改，源码及证据边界见实施记录。

根仓库回归续作：r237/r238 定位了旧布局契约和 W5 构建路径问题。布局断言已
对齐锁定 SDK 的完整清单，并分别检查旧共享流程与 V2 引用流程，r238 全部
74 项通过。W5 现从 Cargo 的 compiler-artifact 消息读取实际可执行文件路径，
保留显式 SIM_CLI_BIN；r240 先通过 7 项隔离测试与原失败配置用例，再完成
guest 目录 unittest 回归共 536 项：531 通过、5 项交叉编译器环境跳过、
零失败/错误。r241 补齐精确原生依赖和真实 GCC 15 后，完整 Rust workspace
的 43 个测试目标执行 963 通过、16 个既有 ignored、零失败，容器 exit 0。
本批根仓库回归已执行完成，ignored 不算通过；该结果不关闭 AM2、W5 实际
推理或总目标。代码提交范围及完整证据见实施记录。

V2/PTO 接入续作已形成候选：`reference_inputs=1` 通过现有 managed CLI 发布、
解析并只读映射完整 V2，输出保留独立 RW allocation。r243 的 guest 目录
unittest 为 535 通过、5 跳过、零失败/错误，r244 layout 74/74 通过。r244 实际
双 guest 的两次计算和 CPU 检查完成，但整轮因校验器混淆 backing 起点与视图
起点失败；r246 复核进一步暴露旧 owner 内初始化判据不适用于 V2 writer。
两项校验已按实际 SDK/QEMU 语义修订并添加拒绝测试，原始失败记录保留。
随后 r246b 原日志独立复核及 guest 目录 unittest 通过，r247/r249 分别在两个
home 方向完成实际 cross-page V2/PTO 整轮验收，runner/container 均 exit 0。
r245 Rust 963 通过、16 ignored、零失败；最终 r246b 源码的 r248 Rust 回归独立
完成相同计数，exit 0、无 OOM。r250 的 home 0 ND 与 r251 的 home 1 tail
大视图实际双 guest 整轮均 exit 0，各自核对完整 V2、四线程 owner、两次依赖
dispatch、102 个实际分片和最终资源归零。该批代码与最终受测八文件一致，
按限定范围本地提交；证据详见实施记录。多请求设备并行、W5 实际模型及完整
故障矩阵仍待完成，完整 AM2/W5/总目标仍未完成。

上述 V2/PTO 接入已提交 `62b4045`，父仓库未推送。随后 r252 使用同一代码在
home 1 / compute 0 完成 tail 四线程、V2 只读输入双请求实际验收，原容器
exit 0，无 OOM 或遗留 QEMU；两请求同时持有、六条 lease、原 completion、
102 个分片及最终清理通过。设备并行执行和多请求故障恢复仍未验收。
指定目标已找到本计划两份模型文件，文件大小及部分 tokenizer 指纹已记录；
完整权重 SHA、GGUF 元数据、当前 backend profile 和 W5 生命周期接入仍待完成。
文件存在和该 PTO 用例通过均不构成实际模型矩阵通过，详见实施记录 r252。

分配压力续作约定：沿用 `cpu-reference` 和现有 iterations 配置，扩展为
1 至 100 轮。每轮须取得新的真实 allocation generation、发布两个 V2 视图，
完成远端读取、解除映射并退役该 allocation，writer 在进入下一轮前核对服务
资源归零。reader 的中间全局快照可能观察到下一轮分配，仅最后一轮要求全局
归零；每次 reader 映射仍须独立核对 provider 资源。100 轮固定尺寸仅关闭
该循环子项，不覆盖不同尺寸、对齐、交错释放或完整压力矩阵；实际结果待验收。
循环失败时在退出服务前保留原 session 错误，并查询 `allocation-stats` 的
资源与幂等记录容量，区分映射、backing 和控制面保留容量，不增加容量绕过失败。
r255 百轮实跑在第三轮 map 被拒；r259 三轮最小复现确认幂等表 capacity=64、
used=59、cleanup_reserved=4、available=1，无法接纳需要四条应答空间的新 map。
前两轮六对象已确认回收，第三轮持有者保留，原失败记录保留。下一实现项为
保留重放语义的历史容量管理及 AM3 地址重用，禁止通过放大容量替代生命周期
实现。百轮压力子项仍未通过；测试与源码边界详见实施记录 r253-r259。

容量续作已提交独立服务 `4bd65b8`：新增内部持久重放历史边界层，保留精确
结果、校验完整历史与已提交前缀，拒绝不确定写入、截断和替换文件。r261
独立仓库 Python 回归 270 项中 265 通过、5 跳过、零失败/错误，执行清单
精确覆盖；r262 的九项边界测试、八种 ASan/UBSan 场景及 CLI 夹具通过。
该层尚未接入 daemon，未释放运行服务的内存幂等表容量；checkpoint 持久化、
缓存回收/重放接入、地址重用及真实百轮仍待完成。子仓库尚未推送，父仓库
gitlink/lock 仍固定 `d9b60ef`，不消费该未发布候选；详见实施记录 r260-r262。

最新状态（r263-r280；上文各轮状态保留为历史）：历史存储已接入 daemon，
独立服务 `3e5b35b` 已按授权发布，父仓库 gitlink/lock 已一致锁定。r273 已
越过幂等表限制，第三轮仍因地址容量失败。QEMU 退役协调端现等待精确的逐
holder 成功 ACK，并保留超时隔离，本地提交 `924148cf5c`；44 项底层测试、
r278 真实双 guest 两轮正常回归通过。r279 的 Python 全集为 538 通过、5 跳过；
r280 的 Rust 全集为 963 通过、16 ignored，均零失败。QEMU 未推送、平台
gitlink 未更新；接收端排空、地址复用、百轮压力及所有后续阶段仍未完成。
详细来源、失败记录和验证边界见实施记录的 r274-r280 小节。

r281-r286 续作：managed V2 unmap 已冻结 route 和地址区间，完整严格 CPU/PTO
调用退出后才允许 fence 与清理；失败保持冻结，拒绝重入 unmap。45 项 QEMU
底层测试、539 项 Python 脚本/接口测试通过（另 5 项环境跳过）；完整 Rust
为 963 通过、16 ignored、零失败。真实双 guest 的两轮 V2 CPU 共享及反向
tail 四线程 V2 双请求 PTO 正常回归均通过。真实在途撤销故障、远端 RETIRE
接收端、地址复用、百轮压力及后续阶段仍未完成；本批未推送平台子仓库。
源码、运行时间、日志指纹及验收边界见实施记录 r281-r286。

地址复用接入约定：worker 用 requested_home_va=0 请求现有 kernel 区间
分配器选址，删除单调游标，不增加第二套地址分配权威。正常回收仍要求
全部 SDK mapping/holder 终结、实际 unexport、segment retire 和 service
reclaim 确认。区间 ENOSPC 只在平台确认保留前失败且输出 descriptor 全零时
按无资源取消；EFAULT、非零结果及其他不确定错误保留意图并停止处理。
复用使用全新 segment/token/generation，跨重启和强制撤销仍需 AM3 验收。

地址容量验证约定：正常业务运行允许扩大预留 aperture，按实际地址布局与
并发对象容量配置；16 MiB 只作为耗尽及回收复用专项测试窗口，不限制正常
业务验收。不同对象可使用窗口内不相邻的空闲区间，每个 segment 保持连续
地址契约。多 aperture 地址池属于后续容量扩展，当前单 aperture 实现尚未
提供该能力；扩容结果和小窗口复用结果分开记录，均不得替代实际 backing
容量、旧身份拒绝、异常回收和百轮运行的验收。

CPU 解除映射验收补齐约定：V2 writer 和两个 reader 视图的显式 unmap
启用 `probe_unmapped=1`。探针在持有映射的同一进程读取有效地址，确认
managed unmap 后再读原地址，要求同步目标地址故障。validator 必须将每次
探针的 session、key、generation、地址和位置绑定到对应 map/unmap；缺失、
重复、错序或无故障均拒绝。此项不替代旧 provider descriptor/token 拒绝、
强制撤销、跨重启或同址新映射建立后的裸指针隔离验收。

真实旧 descriptor 验收补齐约定：新增独立 `cpu-retired` 双 guest 场景，每次
运行在同一对 object-session 中完成两代 64 KiB 对象。reader 校验旧数据后
捕获实际完整 allocation descriptor；旧代及其屏障对象全部退役后才分配新代。
reader 保持新代 holder，校验同址新数据、unmap，再把原 descriptor 交给 provider
尝试映射和读取；要求目标地址 CPU 故障及清理成功，随后重新映射并校验新数据。
普通 map 错误单独报告且不算通过；所有资源应回到基线。此项不替代错误 owner、
epoch/token 组合、强制撤销、重启恢复或同址新映射存续时的裸指针隔离验收。

r332-r339：上述诊断已接通并实际运行。服务 `ff2d50c` 已验证、按授权发布并
在父仓库本地锁定；完整服务 Python 为 286 通过、5 跳过，下游 Python 为
554 通过、5 跳过，Rust 为 963 通过、16 ignored，均零失败。r339 实际
双 guest 在 reader 第 17 步失败：旧 descriptor 映射后 CPU 读取未故障。
旧、新 GSVA 身份不同，但 backing export token 同为 97；严格数据请求仅
携带 backing token/地址，home 未收到完整 GSVA 身份供核对。此项未通过，
不能以控制面旧 V2 拒绝代替。下一修复须将完整身份与当前 export 的权威绑定
贯穿 home 数据访问；已有 wire 按版本保留，受管理访问不得回退 token-only。
相同约束覆盖 CPU/PTO，未知绑定或无法确认撤销一律拒绝；绑定生命周期和
在途请求还需真实运行验证，不能仅加本地 descriptor 自校验。原失败日志、
测试清单及源码指纹见实施记录 r332-r339。

home 绑定接口约定：新增本机 SIM_DEC `0x10` / v1 控制操作，112-byte
请求登记完整 72-byte key、home CNA、GSVA token ID/value、backing token、
export mem ID 和 GSVA READ/WRITE 权限位。仅 home kernel 的 checked export
成功登记后返回 CREATED；登记确认丢失保留实际 export 身份与 UNKNOWN。
unexport 在物理撤销前先关闭该绑定，在途访问未排空时返回 busy 并保留 backing。
CPU/PTO strict I/O 使用 128-byte v2 请求头，保留旧 v1 前缀，逐次核对当前
home 绑定；旧 v1 和普通 SIM_DEC token-only 请求不得访问已登记的 managed
区间。binding tombstone 保留在当前 QEMU 生命周期内，原身份不能重新激活。
此接口仍需实际旧 descriptor、正常 CPU/PTO 与故障矩阵验证；跨进程重启、
设备 reset 的绑定 reconciliation、token rotation 和长期历史容量仍待完成。

r340-r348：home 权威绑定及 strict I/O v2 已实现。r347 保持 r339 的原
CLI、探针和判据，实际双 guest 中旧 descriptor 读取产生 SIGBUS（signal=7），
随后新对象重新映射、完整数据校验及最终资源回收通过。当前 kernel 完整构建、
45 项 QEMU 单元、父仓库 Python 559 通过/5 跳过及 Rust 963 通过/16 ignored
均完成。r348 正常 V2/PTO 回归在 guest 启动阶段超时，两个串口日志为空，
尚未进入对象访问；原作业已结束且无遗留 QEMU，继续诊断，未计为通过。
该结果关闭本次旧 descriptor CPU 访问缺陷，不关闭正常 PTO 回归、完整身份
故障矩阵、重启/reset、W5 或总目标。来源与指纹见实施记录 r340-r348。

r349 启动诊断确认 init 环境槽位耗尽。object-share 的 backend/access/size、
fault/iterations/seed/port/incarnation、aperture/address/descriptor 参数统一
使用 `mem_service.object_share_*` 内核命令行键，由 run_app 读取；不转交 init
环境。配置文件中的 `MEM_SERVICE_OBJECT_SHARE_*` 名称不变，guest 保留旧
下划线参数兼容，新键优先。测试须覆盖全组参数不占槽位及新旧读取顺序，
实际正常 PTO 修复验收通过前仍保持未完成。

strict 分片验收按显式 wire 版本核对：v1 请求头 40 bytes、单片上限 4055；
v2 请求头 128 bytes、单片上限 3967。当前 runner 选择 v2；独立校验 CLI
保留显式 v1 历史复核。两版都逐片核对连续 offset、精确长度、请求应答配对、
backing/peer 身份及回调总长度，不自动猜测版本，也不放宽为只核对累计字节数。

r353-r358 续作：启动修复后原 PTO 的计算、数据及回收完成，runner 因旧分片
校验失败；显式 v2 原日志独立复核通过，102 个分片完整。r354 旧 descriptor
CPU 整轮回归通过，r356 最终 Python 为 562 通过、5 跳过，零失败/错误。
r357 完整 Rust 已通过：963 通过、16 ignored、零失败。r358 最终 PTO
整轮 exit 0，核对两请求、六条 lease、102 个 v2 分片及最终资源回收，无
遗留 QEMU。启动和校验器修复达到本批提交条件，后续完整矩阵仍未完成。

设备 reset 绑定失效约定：QEMU 的现有设备 reset 入口在清理旧计算状态前，
先关闭本 home 所有活动 export 绑定。保留绑定 tombstone 和在途 pin，禁止
把 reset 当作物理资源已回收的证明；旧身份无法通过重复 BIND 重新激活。
在途 pin 仍须正常退出，未排空前同址新绑定拒绝。该能力使用既有 QMP
system_reset 入口，不提供清空旧身份历史的恢复捷径；kernel/provider 重启
reconciliation、重新发布准入和实际 reset 故障矩阵继续单独验收。

r359-r363 已验证上述 reset 关闭子项：45 项 QEMU 底层测试通过，完整
Python 563 通过/5 跳过，Rust 963 通过/16 ignored，均零失败；r362 正常
旧 descriptor CPU 场景整轮通过。r363 实际映射两个 home 对象后执行 QMP
reset，关闭 2 个活动绑定，随后远端 CPU 读失败并报告 reconciliation_required。
原正常 PTO runner 保持 exit 1；该诊断未证明故障后的回收、重新加入或推理恢复。
完整 M3、其他身份故障矩阵及总目标保持未完成，来源见实施记录 r359-r363。

故障对账观测约定：managed 客户端失败时，在停止本节点 worker 前后分别
通过既有 CLI 读取 allocation stats、provider directory 和 kernel region
统计，再停止测试控制面。每项查询使用独立的 5 秒上限并记录退出码；
查询失败表示未知，不能改写为零资源或掩盖原客户端失败。快照为非原子观察，
不输出 descriptor/token，不执行 reclaim 或其他恢复变更，不构成恢复完成证明。

r369-r375 运行期失联隔离已在独立服务 `87e7a5a` 实现、验证并按授权发布。
目录过期、注销及实例替换会保留相关分配、holder 与映射，锁存恢复要求；
重新注册不恢复旧数据准入，已有清理仍检查原事务。当前 holder 缺少节点归属，
其他有 holder 的 provider 分配也保守隔离。完整服务回归 295 项中 290 通过、
5 项环境跳过，零失败/错误；原输出断言失败保留。原生、安装态及 sanitizer
门禁通过。父仓库本地同步 gitlink/lock；r380 实际 reset 后本地 worker 注销
触发 2 个对象/4 MiB 的隔离，refs/import/export 保留；home expiry 尚未实测。
r381 正常 CPU、r384 tail 四线程 V2 双请求 PTO 整轮对照通过；后者完整核对
102 个实际分片和最终资源回收，r382 配额/预算不同的超时记录保留。
r383 确认未触发历史归档的小型 store 在重启后丢失
分配与隔离状态并允许新分配，须补完整版本化持久状态及对账，不能依赖归档触发。
持久分配表、节点归属、fencing 与重新加入仍未完成，不关闭 M3 或总目标。

r323-r331 已补齐上述正常 unmap 子项：独立服务 `4236654` 已按授权
发布并本地锁定；独立 Python 281 通过、5 跳过，父仓库 Python 548
通过、5 跳过，完整 Rust 963 通过、16 ignored，均零失败。真实双 guest
三轮和百轮分别核对 9/300 次同进程原地址故障；百轮包含 594 次旧 V2
控制面拒绝、200 个新视图及 400 次分配退役，最终资源检查通过，无遗留
QEMU。旧 provider descriptor/token、强制撤销、在途访问和恢复保持未验收；
详见实施记录 r323-r331 的源码、镜像、运行及原始证据指纹。

双节点 CLI 的 `--config` 文件可指定
`MEM_SERVICE_OBJECT_SHARE_APERTURE_SIZE=1073741824`（1 GiB）。当前配置
入口接受 2 MiB 对齐的十进制字节数，范围 2 MiB 至 4 GiB；这是该测试入口
的验证范围，不代表平台最大地址容量。默认 `auto` 保留各场景原窗口；故障
场景要求 `auto`，避免改变容量断言。实际 kernel bootstrap 仍负责地址布局
和平台能力验证，配置被拒绝时不自动回退。

r287-r298 当前进展：独立服务 `3280c64` 已发布并本地锁定。真实 16 MiB
三轮 CPU V2 共享、同址新身份复用及单独填满/拒绝/释放后复用均通过。
父仓库 Python 542 通过、5 跳过，Rust 963 通过、16 ignored。1 GiB 和
16 MiB 两组百轮均在第 27 轮控制请求超时，分配拒绝计数为零；百轮及
异常资源恢复仍未完成。重复历史扫描放大已有原生计数证据，下一步补
批量归档并实际核对超时因果；完整模型和故障矩阵继续保留原验收要求。
具体轮次、原始失败与日志指纹见实施记录 r287-r298。

r299-r307 续作：独立服务 `9e6aa1c` 的有界批量归档已测试、发布并在父仓库
本地锁定。相同 671+59 条归档的 read 次数由 82,718 降至 1,344，保留完整
历史校验、持久化顺序和失败保留语义；该数字只用于边界成本说明。完整服务
回归 277 通过、5 跳过，父仓库 Python 542 通过、5 跳过，Rust 963 通过、
16 ignored，均零失败。真实 16 MiB 小窗口及 1 GiB 对照各自完成 100 轮：
每组 300 次分配退役、200 个视图及 100 代同址不同 segment 的数据核对通过，
未放宽默认 3 秒截止时间，最终资源归零。两种窗口的固定尺寸循环子项已有
证据；混合压力、旧身份拒绝、异常恢复和完整模型矩阵仍须分别验收，不能
据此关闭 M4 或总目标。原失败证据和长期历史规模限制保留，见实施记录。

旧引用实跑约定：沿用 `cpu-reference`，reader 在每轮 ready 对象确认为
active 后，经现有 `reference-transition resolve` 保存两份完整真实 V2。
从第二轮起，在新对象已发布且尚未释放时，提交上一轮的原始引用执行
resolve、acquire 和 map-begin，要求明确 stale_ref 或已清除身份的 not_found；
超时、internal、解析失败及任何成功都不能计入拒绝通过。完整原引用与上一轮
writer 发布逐字节比对，核对新旧 generation、实际映射地址和后续新数据读取。
两端 session 的 provider canary 需要同时运行；reader 的捕获/拒绝检查与其
session 并行启动，完成后分配 `ref-checked` 屏障，session 等该对象 active
才 acquire 新引用。writer 最后同时退役屏障；此模式每轮共回收四个 allocation。
这组证据覆盖控制面旧引用准入；旧 provider token、已解除 CPU 映射和在途
撤销的实际访问拒绝仍需独立验收，不用控制面结果替代。

r308-r322 续作：r315 暴露新增检查屏障晚于完成屏障退役的顺序问题，已修正
为完成屏障最后退役并保留失败记录。r321 三轮及 r322 百轮实际验证通过；
百轮包含 400 次退役、200 个新视图读取和 594 次同址复用后的原 V2 拒绝，
分别为 192 次 stale_ref、402 次 not_found。最终代码的 Python 全集为
546 通过、5 跳过，Rust 全集为 963 通过、16 ignored，均零失败。
本轮只修改下游编排、验证器和测试，独立服务仍锁定已发布 `9e6aa1c`；
旧 provider token、旧 CPU 映射、在途撤销、多 home 与 W5 矩阵继续待验收。

r386-r399 持久化续作：独立服务 `e6f261e` 已提交并仅推送授权分支。
从首次分配起，版本化 checkpoint 同时保存 allocation、holder、mapping、
代际计数、隔离状态、完整 V2 record 和幂等应答；重启保留待对账资源，写入
不确定时停止状态变更。r397 九项定向、r398 全量 298 项（293 通过、5 环境
跳过）及 r399 原生/安装态、三项 checkpoint ASan/UBSan 全部通过，原失败
记录保留。父仓库本地同步 gitlink/lock，尚未推送。本批验证使用真实服务/CLI
进程及合成 descriptor，不证明物理 backing 恢复；实际 guest 重启、provider/
kernel 对账、fencing/rejoin 和完整 M3 仍待验收。详见实施记录 r386-r399。

下一实际验证沿用 headless 双 guest 及现有 `--interactive-after-pass` 入口：
正常退役用例通过后，单独启动已锁定服务/worker，两个客户端以真实 OBMM
映射写读同一对象。采样完整身份和 kernel 区域，暂停测试客户端与 worker，
连续重启控制服务两次，核对隔离资源、holder/mapping 及 descriptor 未丢失，
新数据准入被拒绝。该受控静止点只验证 daemon 重启的资源保留，不证明在途
drain、worker 重启、物理回收或重新加入；测试结束必须清理本轮 guest 进程。
该入口必须将显式交互选项传入 guest；正常客户端完成并报告资源后进入 shell，
保留 daemon/worker，shell 退出时执行原基础设施清理。默认 headless 仍等待
worker，由 runner 负责结束。仅存在串口 socket 或业务成功标记不代表 shell
已经可接收命令，交互 runner 必须等到两端 guest 的实际 shell-ready 标记。

r409 实际验证通过上述静止点：两个客户端写读后，暂停客户端与 worker，
连续两次强制重启 daemon；保留 refs=2、服务 mappings=2、export=1，隔离
2 MiB backing，完整对象身份/descriptor 和两端 kernel 区域不变，新 acquire
明确拒绝。r405 Python 全集为 567 通过、5 跳过，r406 Rust 为 963 通过、
16 ignored；原 r403/r407/r408 诊断失败保留。该结果不关闭 worker 恢复、
生产 fencing/rejoin、物理回收和完整 M3。详见实施记录 r403-r409。

r410-r413 续作：独立服务 `1f39f2a` 已提交并仅发布授权分支，父仓库本地
同步锁定。worker 每阶段持久保存完整对象/provider、segment、export 回执及
descriptor，新增只读 `inspect-allocation-state`；已有日志仍阻止直接重启，
日志校验不等于物理对账。七项定向、301 项服务全集（296 通过、5 跳过）及
原生/安装态、worker ASan/UBSan 均通过。实际 worker 崩溃后验证、完整恢复
对账及 fencing/rejoin 继续待完成，详见实施记录 r410-r413。

2026-09-15 执行补记：r414-r416 核验已发布 `1f39f2a` 的下游锁定、guest
镜像与完整回归：Python 572 项、567 通过、5 环境跳过；Rust 43 harness、
963 通过、16 ignored，零失败。r417 因观察器接收块队列溢出失败，保留原证据；
r418 仅修正观察缓存后，实际双 guest 连续 daemon 重启及 worker 崩溃检查通过。
完整持久身份与 96-byte descriptor 保留，未经 reconciliation 的 worker 重启
被拒绝，日志未改写；worker 退出后 kernel 资源仍存在。物理核对、回收、生产
fencing/rejoin 和完整 M3 继续未完成，详见实施记录 r414-r418。

2026-09-15 执行补记：kernel 本地提交 `d9492ed29eb8` 新增包含 active/retired
及 managed export 身份的版本化枚举；现有 CLI 增加 `--dump-segments`。
r423 实际核对 worker 日志与 kernel 库存，额外分配/退役后旧 query 拒绝、
retired 库存仍可见。最终 Python 579 项、574 通过、5 跳过；Rust 963 通过、
16 ignored。kernel 未发布，父仓库平台 pin 未更新；生产 reconciliation、
fencing/rejoin 继续未完成。原失败和最终证据见实施记录 r419-r428。

2026-09-15 执行补记：独立服务 `e4bc52a` 已持久化 worker 的内核实例身份，
新增只读 `reconcile-allocation-state`，将日志与一致 kernel segment/export
库存关联。r436 完整服务回归为 297 通过、5 跳过、零失败；修正前的 vendored
指纹遗漏保留原失败证据。仅授权服务分支已发布，本地 gitlink/lock 已同步。
r435 实际双 guest 在 worker 崩溃后核对成功，跨内核日志被明确拒绝，原日志
不变；214 条原始回执已独立复核。r433 下游 Python 为 574 通过、5 跳过。
r434 Rust workspace 为 963 通过、16 ignored，零失败，43 个目标与基线一致。
本次核对仍不执行资源回收或解除隔离；生产 fencing/rejoin、worker 恢复和
完整故障矩阵尚未完成，详见实施记录 r429-r436。

目标：mem_service 独立作为基础设施启动，向多个独立客户端提供对象分配、
访问、共享与回收。GSVA 作为地址管理/映射能力，由服务与 provider 协同提供；
W5、其他 serving 和 pretraining 只消费同一套服务 SDK。

建议先交付一个最小但完整的能力：**不启动 infer，两个独立客户端通过服务
分配并共享同一个对象，完成跨节点写读、校验和释放。** 随后推进动态代际、
W5 集成、故障恢复及资源压力验证。每个阶段必须产生可运行能力或明确实验结果。

2026-09-15 已排空重启续作：服务 `fd2ff3d` 已发布并同步父仓库本地锁定。
r443 实际双 guest 经两次 daemon 重启，三轮新代际分配、跨节点写读与物理
回收通过，443 条回执独立复核，旧 acquire 重放拒绝。r444 独立服务完整回归
301 通过、5 跳过；r441 下游 Python 574 通过、5 跳过；r442 Rust 963 通过、
16 ignored。实际 daemon sanitizer 的 r448 原作业随后全部七项通过，exit 0；
先前测试 lease 到期的失败和诊断记录保留。此路径只准入已排空 checkpoint，不恢复活跃
payload，不解除故障隔离；完整 M3 与总目标未完成，详细边界见实施记录。

2026-09-16 W5 PP/TP 续作：Qwen guest 的 range pipeline 准入已改用独立的
`SIM_W5_CLUSTER_NODE_COUNT`，公共运行器在 2/3/8-node PP 下保留模型默认 TP8；
实际 `linqu_llm_infer` 构建补齐 managed allocation 与 provider directory 源码，
远端 AArch64 静态链接通过。r566 下游 Python 全集 597 项中 592 通过、5 跳过；
r567 Rust workspace 零失败；PP3/TP8 门禁与配置测试通过。该项只移除真实 W5
启动和调用路径的 PP/TP 耦合，不证明 callable 4 serving 闭环或任一模型矩阵；
两模型 2/3/8-node、4/8-step 实际 guest 验收仍待执行。

2026-09-16 M3 恢复续作：独立服务提交 `f402b13` 增加具名 recovery reclaim，
要求精确旧 home 身份、全部 required provider ready，并逐个核对 holder 节点已
使用新 incarnation；`0cb0401` 增加按旧 home incarnation 枚举 QUARANTINED
义务的 recovery poll。`ed80575` 接入 OBMM lost-home worker 恢复：完整校验并
独占旧 ledger，要求 replacement incarnation 与 kernel instance 均变化、新库存
未复用旧 segment ID，逐项完成服务退役后原子归档旧 ledger。三个提交已仅推送
授权服务分支，远端精确指向
`ed80575160221c60a5f586f99bd660976670e10e`；父仓库 gitlink/lock 本地同步。
本地 host/schema/package/release/API ABI 门禁、26 项 provider-backed allocation
契约及 AArch64 worker `-Werror` 交叉编译通过。n4-910c1 的精确隔离 checkout
随后原生执行 worker golden，修正 mock scan cursor 后 1/1 通过、6.351 秒；该
golden 覆盖同 kernel 拒绝、replacement kernel、service reclaim、ledger 归档及
归档后重试。后续独立服务 `3cffb43` 将 object-session 的 acquire、reference
transition、map/unmap 与清理绑定到显式 holder node/provider incarnation；父仓库
`69097b0` 在三个 managed guest session 配置中注入该身份。服务提交已只推送授权
分支并核验远端 SHA，父仓库未推送。

n4-910c1 的 r608 实际双 QEMU/guest 故障场景使用上述精确提交通过，119.267 秒，
observer exit 0、无残留 QEMU。两个 kernel reset 后 instance 均变化，新代际
`20260916601` provider 目录 ready；首次 lost-home recovery 回收 1 个对象并归档
旧 ledger，第二次回收 0 个对象。最终对象、backing/address reservation、holder、
export/import mapping、quarantine 和 recovery gate 全部归零；replacement worker
ledger 绑定新 kernel instance。该运行关闭 home guest/kernel 已替换、旧 backing
随旧 kernel 消失的实际恢复子集。同 kernel reservation 续作、跨节点映射/计算
排空证明、强制撤销及完整重新加入运行矩阵继续保持未完成，M3 与总目标仍为
进行中。

2026-09-16 M3 同 kernel 续作：独立服务 `5ad3ad0` 新增显式
`resume-allocations`。命令独占原 ledger，只恢复最终阶段为 `published`、同一
kernel instance 的 segment/export/descriptor 与服务 ACTIVE/RETIRING 身份逐项
一致的 reservation slot；完整终态只核对，意图及部分完成阶段继续 fail-closed。
n4-910c1 r610 原生 AArch64 worker 测试 4/4 通过、19.834 秒，覆盖正常恢复及
kernel/service identity mismatch 拒绝。提交已只推送授权服务分支并核验远端 SHA；
父仓库仅本地同步 gitlink/lock。同 kernel 稳态 worker 崩溃续作已关闭，部分完成
事务处置、跨节点排空/强制撤销和完整重新加入仍未完成。

2026-09-17 M3 部分事务续作：独立服务 `b84b8e6` 将同-kernel 恢复扩展到
`reserved`、`reserve-unknown`、`export-unknown`、`exported`、
`release-intent`、`unexported`、`retired`、无 backing retire/cancel 及其已完成
阶段。命令先核对全部 ledger、稳定 kernel inventory 和同代服务对象，再按实际
resource 状态继续 export/publish、unexport/retire/reclaim 或取消；每次外部完成
先追加同步 ledger 帧。没有 segment 身份的裸 `reserve-intent` 与 torn frame 保持
隔离。n4-910c1 r612 原生 AArch64 定向测试 4/4 通过、14.714 秒；r611 暴露并修正
了测试替身把服务 descriptor 错误绑定到实时 kernel export 的问题。提交已只推送
授权服务分支并以远端 ref 核验。实际进程杀死/重启注入矩阵、跨节点排空/强制撤销
和完整重新加入继续未完成。

同日 M3 隔离续作：独立服务 `3e21a04` 新增显式
`quarantine-allocation-state`。完整 `worker-start` 首帧绑定旧 provider 的 endpoint、
state file、node/incarnation、粒度和分配模式；命令取得 ledger 独占锁后精确注销该
incarnation，使 daemon 立即执行 provider-loss 隔离。后续 torn/invalid 帧只影响
完整性报告，不被解释为事务结果；命令不打开 OBMM、不改写 ledger、不回收 backing。
旧 provider 已过期或被新 incarnation 替换时按幂等完成。首帧损坏、配置错配、活动
writer 或控制结果不确定均拒绝。本地定向门禁 4 项中 3 项通过、1 项平台跳过，两个
AArch64 binary 以 `-Werror` 交叉编译成功；提交已只推送授权服务分支，远端精确 SHA
为 `3e21a0497c5eefddde6dcb908831ba458685250a`。真实 guest 的裸 reserve-intent/torn
隔离注入、物理对账、跨节点排空/强制撤销及完整重新加入仍待最终目标验收。

同日 M3 远端 holder 排空续作：QEMU `044b759e4b` 将 RETIRE 接收端与实际 route
撤销绑定。接收端先冻结精确 route、刷新 TLB，再等待严格 I/O 退出、脏行 writeback
和 home fence；随后失效本地缓存、解除 CPU window 并保留同 epoch tombstone，全部
成功后才发送成功 `RETIRE_ACK`。pending、timeout、旧 epoch、错 home/range 和
不完整 strict route 均保留隔离并返回失败。轻量本地生产函数门禁 5/5 通过；没有
启动本地 QEMU 或模型。该实现仍须在 n4 双 guest 上证明 dirty writeback、旧
CPU/PTO 访问拒绝和协调端精确回执闭环；实际强制撤销、重新加入和重复故障矩阵继续
保持未完成。

同日将旧 `coh_remote_retire` 从伪造远端 CNA 的消息往返测试改成真实双 guest
验收入口。node B 导入 node A 的严格 GSVA、写入脏值并保留 CPU VMA；独立 barrier
后 node A 发起 RETIRE。成功判据同时要求 node B writeback/fence、两次 TLB flush、
CPU window unmap 和 tombstone，node A 在 ACK 后从 home backing 读到脏值，node B
旧 VMA 读取触发 `SIGBUS/SIGSEGV`。AArch64 `-Werror` 构建、runner 语法和两个定向
契约测试通过；没有本地 QEMU 运行。该入口留到最终 n4 验收执行，当前不构成实际
跨节点物理排空证据，PTO 拒绝和强制撤销仍待补齐。

同日 M3 fencing receipt 续作：独立服务 `4ba90ac` 新增中立 wire operation
`0x7f fence-allocation-holder`、typed SDK 与诊断 CLI。replacement provider 的新
incarnation 只证明新身份在线；服务不再据此自动清除旧 holder。provider 完成或
证明本地 route、VMA、cache、TLB 与设备访问均已撤销后，提交精确 allocation
generation、旧 holder incarnation 和幂等键；服务原子删除该旧 provider 身份拥有
的 holder sessions 及对应 mappings，并把 receipt 与精确回复持久化。普通 recovery
在 holder 或 mapping 义务非零时继续失败关闭。定向 daemon/CLI 用例验证
replacement registration 单独不足、错误旧身份拒绝、1 个 holder/1 个 mapping
精确删除、重启后 receipt 幂等重放，以及 receipt 后 recovery/reclaim 完成。该提交
已只推送授权服务分支并核验远端 ref。此项补齐控制面的物理证明消费契约；OBMM/QEMU
实际强制撤销生产者、PTO 拒绝、完整重新加入和 n4 故障注入仍未完成。

同日 M3 本地 holder 物理撤销原语续作：kernel `dd07fe43c50d` 在现有
`OBMM_CMD_GSVA_EVENT_V1` 结构中定义 `LOCAL_REVOKE=8`，不扩展 ABI 布局；QEMU
`854a8d8449` 仅接受本机 CNA、完整 key/epoch 及精确 GSVA token 匹配的请求。活动
route 先进入 `STALE` 并刷新 TLB；coherence pending、timeout、严格 I/O 在途或 unmap
在途均保持隔离并拒绝成功。通过门禁后执行 home fence/writeback、缓存失效、CPU
window 解除映射并保留精确 tombstone，最后才把 coherence 对象置为 `RETIRED`。
精确 tombstone 支持幂等重放；错 CNA、错 token、旧 epoch、缺失 route/coherence
对象全部失败关闭。生产函数抽取门禁覆盖上述正负路径并通过，没有启动本地 QEMU
或模型。该原语尚未接入 replacement provider 的恢复命令，因此 receipt 自动提交、
PTO 实际拒绝、完整重新加入和 n4 故障注入仍未完成。

同日 M3 provider 撤销闭环续作：独立服务 `6d3ebe3` 增加 holder-scoped recovery
poll 和 `fence-holder-state`。replacement provider 按精确旧 holder identity 获取
QUARANTINED descriptor，先通过 `/dev/obmm` 调用 `LOCAL_REVOKE`；只有 QEMU 返回
完整物理排空成功后才提交 holder fencing receipt，失败路径保持
`receipt_state=not-submitted`，RPC 不确定路径显式标记可幂等重试。lost-home HOME
holder 使用“kernel instance 已替换、两次稳定 inventory 不含旧 segment 且没有身份
复用”的证明先提交独立 receipt，再执行 reclaim；其他节点 remote holder 仍须各自
排空。相关 daemon 行为、provider adapter 和 worker 构建定向门禁通过；提交已只
推送授权的 mem_service 分支并核验远端完整 SHA。kernel/QEMU 仍未推送。该批完成
生产调用链实现，n4 原生 ioctl、旧 CPU/PTO 拒绝、重新加入和重复故障矩阵仍待实际
目标验收。

首次 n4-910c1 原生 worker 定向运行在进入 holder 撤销用例前暴露既有隔离入口缺陷：
私有 ledger v2 声明核对 endpoint、state file 与分配模式，但 codec 从未持久化这些
字段，`quarantine-allocation-state` 因而必然报告 `ledger-start-invalid`。原失败日志
保留。独立服务 `688db48` 将配置长度、双摘要和分配模式写入定长帧保留区并升级为
ledger v3；每帧核对同一绑定，旧 v2 仍可供 inspect/resume/recovery 读取，但隔离
操作失败关闭。golden 新增旧 v2 首帧零 deregister 拒绝，本地 AArch64 交叉编译
通过。n4-910c1 使用精确父/子提交重跑原失败项，原生执行 1/1 通过、7.179 秒；
同一 golden 同时执行 holder 物理优先 receipt、物理失败零 receipt、lost-home HOME
holder receipt 先于 reclaim 及旧 v2 隔离拒绝。该结果关闭本批原生逻辑门禁；实际
QEMU ioctl 与旧 CPU/PTO 拒绝仍待 guest 验收。

同日 n4-910c1 holder-recovery 续作：独立服务 `b0d5a8a` 补齐 recovery poll 的
完整 holder identity 并仅推送授权分支，父仓库本地 gitlink/lock 同步。r7 实际
双 guest 中 replacement incarnation 通过真实 `/dev/obmm` `LOCAL_REVOKE` 完成
quarantine、home fence/ACK、两次 TLB flush、CPU window tombstone 和本地 holder
撤销，随后提交唯一 allocation 的 fencing receipt；home 完成 unexport、kernel
segment retire 与 service reclaim，旧 CPU mapping 再访问以 exit 135 被拒绝。
原 runner 因混用跨节点 RETIRE 的 `home retire route finalized` 标记退出 1；
`c8155a5` 改为关联 guest COMMITTED retire 与 QEMU 同一
segment/epoch/export 的 `GSVA_HOME_REVOKE=2 error=0`。它直接复核原 r7 日志通过，
没有重跑 guest，终态无遗留 QEMU。该结果关闭 replacement provider 实际强制撤销、
receipt、旧 CPU VMA 拒绝和 home 退役子项；旧 PTO、裸 reserve/torn、完整重启/rejoin
及重复故障矩阵继续待验收。

同日 holder-recovery CPU/PTO 同 owner 续作：父仓库 `219a804` 让一个实际 GSVA
mapping owner 同时保留 CPU pin、三份 compute lease 和 fence 前准备的 PTO op；
`62a28e0` 将撤销后的验证顺序固定为 PTO submit/reject 后再执行有界 CPU 探针。
r10 进一步暴露 PTO endpoint 与 provider worker 复用了 incarnation `7` 作为 OBMM
bootstrap generation，node B 因读到旧 worker descriptor 而等待错误 canary；
`eadaff1` 改用 key、incarnation 和 artifact fingerprint 派生的独立 discovery
generation。r11 证明旧 PTO op 已被拒绝，并定位 owner adopt 后源 mapping 被清零；
`c0f0c09` 改为通过 retained CPU lease view 执行旧地址探针。

n4-910c1 r12 使用父仓库 `c0f0c09`、QEMU `5809bc8a1a`、kernel
`dd07fe43c50d` 和服务 `b0d5a8a`，一次双 guest 运行在 44.393 秒内 exit 0。
同一 op `8`、mapping `1` 在物理撤销后先得到
`pto_ub_gm_bad_memref`，随后旧 CPU 地址 `0x700000000000` 得到 `SIGBUS(7)`；
两端 canary 使用相同的独立 generation `10690227482818386410`。QEMU 同时记录
quarantine、成功 home fence ACK、drain、tombstone 和 local revoke；home 完成
unexport、segment retire 和 reclaim。机器报告为
`replacement-holder-cpu-pto-revoke-and-home-retire: pass`，终态无遗留 QEMU。
该结果关闭旧 PTO 访问拒绝子项；裸 reserve/torn、完整杀死/重启/rejoin 及重复
故障矩阵继续待验收，M3 和总目标保持进行中。

同日 holder replacement rejoin 续作：r13 在新 reader peer canary 中暴露固定本地
import PA 与已有 mapping 冲突，kernel 明确返回 `EEXIST`。独立服务 `3725f0d`
将本地 import backing 扩展为最多 64 个、可跨不连续窗口的对齐候选，仅对
`EEXIST` 尝试下一候选；`2c0e897` 稳定原生 wrap 测试。两笔均经定向测试并只发布
授权分支，父仓库同步 gitlink/lock。r14 越过地址冲突后，真实 reader 已完成
acquire/map/read/unmap/release/retire，失败来自 guest harness 从不携带 generation 的
`read` 行提取代际；父仓库 `32c51cc` 改从 `acquire` 行提取。

r15 使用服务 `2c0e897`、父仓库业务提交 `32c51cc`、QEMU `5809bc8a1a` 和 kernel
`dd07fe43c50d` 完成一次实际 worker `SIGKILL`、replacement fencing 和 rejoin。
writer、publish 与 reader 对 65536 bytes 得到相同 checksum；旧 incarnation 7 被
拒绝，新 incarnation 8 发布 generation 6，并完成两端对象退役。原 runner exit 1
仅由验证器仍假设 write/publish 携带 state/generation、并要求全日志只有一个
COMMITTED retire。`8667d45` 与 `607998e` 将身份绑定到 acquire，关联 writer/
publish/reader payload，并按恢复对象 generation 选择 home retire。提交后的验证器
直接复核 r15 四份原始日志通过，报告 `physical_revoke=1 old_pto_rejected=1
home_retire=1 rejoin=1`，终态无遗留 QEMU。该结果关闭一个实际 worker
kill/replacement/rejoin 闭环；其他注入点、管理进程与 guest 故障、裸
reserve-intent/torn 隔离和重复故障矩阵继续待验收。

同日 M3 裸事务隔离续作：独立服务 `0313f1f` 为 allocation worker 增加受配置
约束的确定性故障注入。`reserve-intent/sync-exit` 在同步写入完整意图帧后以 86
退出，`reserve-intent/torn-exit` 只写入半帧后以同一码退出；默认配置不注入，
未知 phase/mode 严格拒绝。父仓库 `b6c9c9f` 增加实际双 guest 驱动与验证器，
`e416f34` 修正父 shell 等待注入 worker，`1f62b42` 将解析范围限定为故障回执。
n4-910c1 使用父仓库 `1f62b42`、服务 `0313f1f`、QEMU `5809bc8a1a`、kernel
`dd07fe43c50d` 及 initramfs
`d0400c9b56b06051bfd1fdb300cab932a48e7ec2d626972306c2f673318c6928` 完成
两种实际故障。r19 的完整意图帧得到 `ledger_integrity=complete`，r20 的半帧得到
`ledger_integrity=torn`；两者均观察到 worker exit 86、generation 1、provider
`isolated`、`resource_reconciliation_required=1`、零 live object/backing/address/
reference/import mapping、一项 quarantined/in-flight 义务、目录 data plane 未就绪，
且前后 kernel region 基线一致。r19 原 runner 仅因验证器误解析普通 worker 日志
退出 1；修正后的验证器直接复核同一组不可变原始日志通过，没有重跑 guest。r20
完整 runner 1/1 通过。两轮终态均无遗留 QEMU。该结果关闭裸
`reserve-intent/torn` 实际隔离子项；管理进程、整个 guest、其他事务阶段及重复
故障矩阵继续待验收，M3 与总目标保持进行中。

同日 M3 管理进程故障续作：父仓库 `097f9b4` 增加实际 daemon 双次
`SIGKILL`、worker 失败关闭、持久对象身份核对及 peer down/up 观测场景，
`9db4350` 接通 OBMM profile allowlist，`1b0ba60` 接受第二次重启时 peer 已先注册的
合法时序，`285321f` 将失去活跃所有权的资源按隔离统计核对。n4-910c1 r30 使用
父仓库 `285321f`、服务 `0313f1f`、QEMU `5809bc8a1a`、kernel
`dd07fe43c50d` 和 initramfs
`333669291029e5e40a87d857c24cfceedaeda6fa2fc822d43dadba4ca1b62828`
完成 1/1 实际双 guest 验收。两次 daemon 均以 137 退出；两个 provider worker
失败关闭；同一 generation、descriptor、地址、home/provider 身份和 live ref 在
两次重启后保持，状态进入 quarantined 且 `managed_recovery_required=1`。provider
重新注册后目录可 ready，但 data plane 继续为 0；旧 acquire 重放和新 allocation
均以 `managed_reconciliation_required` 拒绝。活跃 backing/address 统计均为 0，
`quarantined_bytes=2097152` 与唯一 kernel export 精确相等。peer 完整观察两轮
down/up，终态无遗留 QEMU。该结果关闭管理进程故障的重复重启隔离子项；整个
guest 故障、其他事务阶段及重复故障矩阵继续待验收，M3 与总目标保持进行中。

同日 M3 整个 guest 故障续作：父仓库 r31-r36 增加 node B 实际 `SIGKILL`、
replacement guest 启动、存量对象隔离和双端重新加入。逐轮修正 holder 等待上限、
replacement peer 配对、UB/FM link 判定、存活 node 的重新握手，以及把稳定 GSVA
aperture generation 与本轮 bootstrap/provider incarnation 分离。n4-910c1 r36 使用
父仓库 `8a4061d5`、服务 `0313f1f`、QEMU `5809bc8a1a`、kernel
`dd07fe43c50d` 和 initramfs
`7e1e9179bc239b4af9e3457924c6e48ecc5e8beddbd1b76f92edbdc91fe74380`
完成实际 guest 退出码 137 及 incarnation 7→8 的替换。两端以 bootstrap
generation 8 重建 manager/provider 通道；存活 node 复用 generation 7 的既有
kernel aperture，replacement node 以同一稳定 aperture generation 注册。双方
provider conformance 均通过。旧对象保持 1 个 live ref、1 个 import mapping 与
2097152 bytes 隔离义务，旧 replay 和新 allocation 均以
`managed_reconciliation_required` 拒绝，data plane 保持未就绪。原 runner 因把
QEMU `cpu_window` 诊断行当成结构化 `GSVA_MAP` 而记录失败；`e6f91ecb` 限定映射
记录解析后直接复核原始日志，得到
`scope=actual-guest-sigkill-restart-isolation status=pass`，未重跑 guest。终态无
遗留 QEMU。该结果关闭单次整个 guest 杀死、替换和隔离子项；其他事务阶段、
重复 guest 故障与完整重复故障矩阵继续待验收，M3 与总目标保持进行中。

同日 M3 worker 事务边界续作：独立服务 `48a542e` 将确定性 `sync-exit`
注入扩展到 `reserved`、`exported`、`published`、`release-intent`、
`unexported`、`retired` 和 `reclaimed`；父仓库 `01cda3aa` 增加双 guest 驱动、
恢复控制器和逐阶段验证器。allocation-side 的三个阶段从同一 kernel ledger
恢复 reservation，补完 publish 后由控制器退役；release-side 的四个阶段继续
unexport、retire 和 reclaim。两类路径均要求 provider directory 恢复到两个
active provider、控制面 ready、data plane 保持 0，最终服务与 kernel 资源归零。
服务提交已只推送授权分支，父仓库未推送。

n4-910c1 的 r37 使用父仓库 `01cda3aa`、服务 `48a542e`、QEMU
`5809bc8a1a`、kernel `dd07fe43c50d` 和 initramfs
`9cd35c36dcbd6fdbedc8214a705753bac5cf750b4174f20df7ab662ae3823d0d`
分别运行七个实际双 guest case；每个 case 只执行一次，全部 1/1 PASS。七次
worker 均在目标 ledger phase 同步完成后以 86 退出，恢复报告均为 generation 1、
`recovery=resumed`、`final_state=retired`、`provider_state=ready` 和
`resource_reconciliation_required=false`。allocation-side 恢复时精确重建一个
reservation；release-side 恢复时 reservation 为零。每轮最终 live object、
backing/address、reference、in-flight、quarantine、import/export 与 kernel
region 均归零，终态无遗留 QEMU。该矩阵关闭单次 worker 在七个可续作事务边界的
实际杀死/重启注入子项；ACK 缺失、重复事务故障、重复 guest 故障及完整重复矩阵
继续待验收，M3 与总目标保持进行中。

同日 M3 RETIRE ACK 缺失续作：父仓库 `384d99d9` 增加
`coh_remote_retire_timeout` CLI mode 和 headless runner 注入。两个 guest 先建立
真实 managed home、远端 holder、CPU VMA 与脏写；host 在双方进入专用 barrier
后以 `SIGKILL` 终止 holder QEMU。home 随后发送真实 RETIRE，必须在缺少精确
RETIRE_ACK 时进入 terminal coherence timeout；RETRY、query 和后续 access 均
返回 `GSVA_ERR_COH_TIMEOUT`，禁止 home retire、route finalize 或迟到状态复活。

n4-910c1 r38 只运行一次该 case。node B 在脏写后以 137 退出；node A 的 RETIRE
传输成功，1011 ms 后记录 TIMEOUT，三类 guest 观察均为 -7，最终报告
`home_retired=0 resource_isolated=1`。node A 日志中没有 RETIRE_ACK apply、home
finalize 或 retire 完成；终态无遗留 QEMU。实际运行 initramfs SHA-256 为
`40f6dae6ef8ec804d86341e59b21f1e7087de23369bcbe2e69fe1fc0daabc7d7`。
该结果关闭跨 guest RETIRE ACK 缺失的物理 fail-closed 子项；ACK 缺失后的服务层
对账/重新加入、重复事务故障、重复 guest 故障及完整重复矩阵继续待验收，M3 与
总目标保持进行中。

同日 M3 holder receipt 缺失与重新加入续作：父仓库 `f725ee8c` 为既有
holder-recovery 增加明确的 receipt gate，要求 node A 在 replacement 在线后先取得
`holder_fence_receipt_required` 和正数 `remaining_holders`，node B 只有观察到该
gate 才能执行真实 `LOCAL_REVOKE`。r39 暴露 quarantined allocation 存在时禁止新
allocation 的正确 fail-closed 行为；`2bda887b` 将 gate 改为故障前预创建、拒绝后
退役。r40 已越过 receipt、旧 CPU/PTO 拒绝和 home retire，最终因误用
`nd/262144` 配置使 rejoin reader 用时 84.8 秒、超过 writer 的 60 秒等待而失败；
该配置不作为验收结果。`d1f0c7ba` 将验证器接受条件绑定到真实
`recover-allocation` 的 key、state 和 generation，避免混淆 gate 自身退役。

n4-910c1 r41 复用历史已通过的 `cross-page/65536` 工件执行一次实际双 guest。
node A 先确认 generation 1 尚有 1 个 holder、退役 receipt gate；node B 观察 gate
后完成物理撤销和 fencing receipt，旧 PTO op 8 返回 `pto_ub_gm_bad_memref`，旧 CPU
pin 触发 signal 7。node A 随后接受同一 generation 的 recovery 并完成 home retire；
incarnation 8 replacement 发布 generation 7，新 writer/reader 对 65536 bytes 得到
相同 checksum，旧 incarnation 7 被拒绝，最终 rejoin 成功。业务路径完成后原 runner
仅因验证器把 CLI 字段错误假设为相邻行而 exit 1；修复后的验证器直接复核同一组
原始日志通过，报告 `physical_revoke=1 old_pto_rejected=1 home_retire=1 rejoin=1`，
没有重跑 guest，终态没有遗留 QEMU。该结果关闭“缺少 holder fencing receipt 时服务
拒绝恢复、receipt 到达后恢复并允许 replacement 重新加入”子项。整个 holder guest
在 RETIRE ACK 前消失后的 service-managed 对账/rejoin、重复事务故障、重复 guest
故障及完整重复矩阵继续待验收，M3 与总目标保持进行中。

同日 M3 整个 holder guest 终止后的对账/rejoin 续作：独立服务
`62ccd379986ee8da62c3e3711a7fcd1271e6e236` 增加显式
`fence-terminated-holder-state`。入口要求平台 supervisor 已确认旧 guest/QEMU
终止，并同时核对旧 kernel instance、replacement kernel instance 已变化、两次
稳定的 replacement kernel inventory 为空、精确旧 route 只因 kernel replacement
消失，随后才持久化 holder fencing receipt。单独的 lease expiry、provider
replacement 或命令行传入的旧 kernel identity 均不能充当终止证明。服务提交已只
推送授权分支；父仓库 `df5c6bfc` 接通 node B QEMU `SIGKILL`、退出回执、
replacement guest fencing、旧 home reclaim、stale identity 拒绝和新对象重新加入，
`e43327d7` 修正原生测试中一个既有的合法 fault 配置预期。

n4-910c1 r42 已完成 guest 退出码 137、kernel instance 更换、空 replacement
inventory、holder receipt 和旧 generation 1 对象 RETIRED；新 generation 2 对象也
已由 replacement worker 发布。该轮 writer/reader 在数据操作前因两端 peer-canary
generation 不一致而报告 provider unavailable，0/1 结果保留为失败证据。
`277ba6a9` 统一双方 generation 并增加契约断言。r43 只运行一次同一实际双 guest
case，runner 1/1 PASS：旧 incarnation 7 被拒绝，incarnation 8 的 writer/reader 对
65536 bytes 得到相同 checksum `0x71c4d15c79b10383`，rejoin generation 2 完成
退役；最终 backing/address、holder、import mapping、quarantine 和 kernel region
全部归零，`managed_recovery_required=0`，终态无遗留 QEMU。该结果关闭“整个
holder guest 在 RETIRE ACK 前消失后的 service-managed 对账/rejoin”子项；重复
guest 故障、重复事务故障及完整重复故障矩阵继续待验收，M3 与总目标保持进行中。

同日 M3 重复整 guest 故障续作：父仓库 `df911c09` 将同一次 node A daemon/home
worker 生命周期扩展为 1 至 8 个 node B 整 guest 终止/替换周期，并为每周期分配独立
对象、session、idempotency key、restart index、incarnation 和 kernel identity
证据。r44 在故障注入前暴露 shell role 判定的左结合错误，`b1304ff8` 改为显式
分支；r45 已完成两次 QEMU 退出 137、replacement 启动和 holder fencing，第二周期
在 home reclaim 时暴露 home provider incarnation 与已终止 holder incarnation 混用，
`9c89e26b` 将 `recover-allocation` 的恢复身份绑定回对象的 home provider。两次失败
均定位到唯一控制流/身份参数并形成修复，没有原样重跑。

n4-910c1 `managed-guest-recovery-r46` 只运行一次修复后的双周期 case，runner
1/1 PASS。两个周期分别验证 incarnation 7→8、8→9；第二个 replacement 的旧
kernel identity 精确等于第一 replacement 的当前 kernel identity，三次 kernel
identity 均不同。generation 1/5 的故障对象都在 fencing receipt 后完成 home
reclaim；generation 2/6 的重新加入对象分别完成跨 guest 写读、旧 incarnation
拒绝、unmap、retire 和物理 reclaim。最终服务与 kernel 的 backing、address、
holder、mapping、quarantine 和 region 计数全部归零，终态无遗留 QEMU。该结果关闭
“同一服务生命周期内重复整 guest 故障”子项；重复事务故障与完整重复故障矩阵继续
待验收，M3 与总目标保持进行中。

同日 M3 重复 worker 事务故障续作：父仓库 `87d5a66f` 为七个可续作 ledger
phase 增加 1 至 8 个连续注入周期，并按周期隔离对象、session、idempotency key
与验证 generation。r47 首轮恢复完成，第二轮因复用已存在的上一轮 ledger 被新
worker 正确拒绝；`c2c56c19` 为每周期生成独立 ledger。r48 的第二 worker 已使用
新 ledger 启动，随后因上一 resume worker 正常退出时注销 provider、而新 worker
只执行 refresh 而失败；`5a1d75a3` 在后续周期启动前显式重新注册同一稳定 provider
incarnation。两个失败均无资源或 QEMU 残留，也没有原样重跑。

n4-910c1 `managed-transaction-repeat-r49` 只运行一次修复后的
`published_crash × 2` case，runner 1/1 PASS。两个 worker 分别在 generation 1、2
写完 published ledger 后退出 86；两次恢复均从完整独立 ledger 重建一个 reservation，
把对象推进到 active，再完成 retire 和物理 reclaim。第二周期前 provider 以同一
incarnation 7 重新注册；最终对象、backing/address、mapping、reference、in-flight、
quarantine 和 kernel region 全部归零，`managed_recovery_required=0`。该结果关闭
“同一服务生命周期内重复可续作事务故障”子项；七 phase 全部重复的完整故障矩阵
仍待验收，M3 与总目标保持进行中。

同日 M3 重复事务完整矩阵续作：n4-910c1 r50-r55 复用 r49 已固定的 parent、
`mem_service`、QEMU、kernel 和 initramfs，未重新构建，也未重跑 `published`。
六次唯一 case 分别覆盖 `reserved`、`exported`、`release-intent`、`unexported`、
`retired` 和 `reclaimed`，每次均连续注入两个 worker 退出 86。六次 runner 均为
1/1 PASS；每个 phase 的验证器均观察到 generation 1、2、完整独立 ledger、
`recovery=resumed`、`provider_state=ready`、`final_state=retired` 和
`resource_reconciliation_required=false`。逐 case 终态对象、backing/address、
mapping、reference、in-flight、quarantine 与 kernel region 均归零，矩阵结束后
无 QEMU 残留。结合 r49 的 `published × 2`，七个可续作 phase 的重复故障矩阵
全部通过。此前已分别覆盖 reserve-intent 完整/半写隔离、daemon 双次退出、整
guest 连续两次退出、ACK/receipt 缺失、旧 descriptor/CPU/PTO 拒绝与 replacement
incarnation 重新加入，因此 M3“回收及 GSVA 状态恢复”阶段验收通过。该结论不关闭
M1、M2、M4、M5 或总目标。

同日对 M1 按本文 §3.7、§4 与地址管理补充计划的明确阶段出口重新核对。M1 要求
两节点、单 home 的 A01-A06，以及 CPU 页权限、guard、无效引用和畸形 descriptor
组成的 A09 子集；2/3/8-node、每节点作为 home、三轮冷启动和独立发布分别属于
A16/A18，继续由 M2/M4 和最终验收承担。既有实际证据已经覆盖 M1 出口：r306 在
同一基础设施生命周期内完成 100 个顺序独立 object-session 客户端及双 guest 数据
闭环；r19-r20 在 provider 隔离时保持 directory/data plane 未就绪；r121 覆盖不同
size/alignment 的并发与交错释放；r118/r297 分别覆盖 backing/address 耗尽及恢复；
r126 覆盖实际固定 VA 冲突；r124、r321、r381 覆盖畸形 descriptor、旧全局引用及
退役 descriptor 的拒绝；正常与故障终态均有实际资源清单。由此 M1“独立服务的
两客户端分配与访问”阶段验收通过。A16/A18 及两模型运行矩阵没有降级或删除。

同日 M2 managed serving 续作：QEMU `d1307d0f8d2464c64df9f0942685b381cf33e2d8`
把 managed home 的严格访问改为直接 RAM backing，远端 import 继续使用 CPU
window；resolver 只接受两种模式中的精确一种，避免 home 访问经 MMIO 回调递归。
独立服务 `163dc8c321449a420e389c53ef7ccffd9211e75e` 按 kernel aperture、节点编号和
节点总数划分候选区间，并由 kernel 对每个候选地址做最终准入；`EBUSY` 且返回
descriptor 全零时才继续探测下一候选。该分支已按授权推送，父仓库 gitlink 与
`mem_service.lock` 本地同步，父仓库未推送。

n4-910c1 r81 在 2-node、4-step Qwen3-0.6B managed serving 上完成一次实际功能
验收：2/2 节点、4/4 step、8 条 worker completion 全部成功，terminal token 为
`[264, 3644, 7653, 304]`，文本为 `" a global leader in"`；四轮 round time 分别为
5798、4713、4104、4636 ms。node A 的四个 home 位于 `0x700000000000` 至
`0x700018000000`，node B 的四个 home 位于 `0x700104000000` 至
`0x70011c000000`，证明同一运行内的节点分区和多 home 数据路径。最终对象、
backing/address、export/import mapping、reference、in-flight 与 quarantine 全部
归零，无 QEMU 残留。原 runner 在完成计算和清理后仍执行 legacy artifact 检查而
返回失败；`c4c2d06c` 与 `361a8c0e` 将 managed terminal observation 计入 decode
证据，并跳过只属于 legacy range publish 的 artifact gate。同一份 r81 summary 经
更新后的报告器复核为 `status=pass`，没有为验证器改动重复运行 guest。

该结果关闭 callable 4 连续 decode、Qwen 2-node/4-step 及每节点独立 home 子项。
M2 仍须完成 Qwen 3/8-node、核心 8-step，以及 DeepSeek 2/3/8-node 的 4-step 与
核心 8-step；模型身份、prompt、采样和 topology 仍按正文要求锁定。M4 的重复性能
样本和混合压力未由单次 r81 关闭，M5 仍未开始，总目标继续进行。

随后 n4-910c1 r82 复用同一锁定代码和模型，以参数化 launcher 唯一执行一次
Qwen 3-node/4-step managed serving。runner exit 0，3/3 节点、4/4 step、12 条
worker completion 和报告器 `status=pass` 全部通过；terminal token 与 r81 相同。
三个节点分别从 `0x700000000000`、`0x7000b0000000`、`0x700160000000` 起分配
四个 64 MiB segment，各节点地址区间互不重叠。三个 cleanup record 均报告
12 allocations 后对象、backing/address、mapping、reference、in-flight 和
quarantine 全部归零，终态无 QEMU。该结果关闭 Qwen 3-node/4-step 子项；Qwen
8-node 与核心 8-step、DeepSeek 全部 2/3/8-node 和核心 8-step 继续待验收。

## 1. 范围与依据

2026-09-15 W5 前置续作：两份指定模型完整权重 SHA 已固定，当前 Rust parser
已核对 DeepSeek GGUF geometry/tokenizer 和 Qwen3 配置；未执行模型推理。
Rust V2 解码器、CLI 及锁定 C SDK 互操作测试已接入，最终完整回归为 Rust
965 通过、16 ignored，Python 574 通过、5 跳过，零失败；CLI 一正四负通过。
V2 到服务 acquire/map、模型消费与输出发布仍待接通，2/3/8-node PP 矩阵未
验收；TP profile 的整除检查不能代替 PP 拓扑验收。完整目标保持未完成。

同日 r462-r464 续作已将完整 V2 输入接入 W5 Rust 命令队列及 Qwen/DeepSeek
消费函数，独立 C enqueue 与 CLI 保持 V1 路径分离。最终 Rust 全集 970 通过、
16 ignored，Python 574 通过、5 跳过，均零失败。固定 Qwen3-0.6B 的真实单层
计算两次通过，改变旧 inline 输入后完整输出逐字节一致；四项引用/数据负例
被拒绝。该验证使用离线输入和单个 fixture token，不属于 PP/decode 矩阵。
真实运行同时发现并修复 KV 容量漏算每层 40-byte 头部。SDK acquire/map、
guest/provider 调用点、lease 保持及受管理输出发布仍待接通，完整 M2 未完成。

W5 managed 执行接入约定：复用已有 tag-10 注册映射、请求绑定及原 CQ completion，
新增 callable 4 调用当前 Rust/Simpler model-range，不切换 Qwen PTO graph 或 DS4
动态库。参数依次为只读控制视图、零至两个只读 V2 payload 视图和独立可写输出。
控制视图携带版本、原 op/request ID、range 坐标、8192-byte prompt 控制前缀及
完整 V2；禁止携带旧 inline hidden/KV。访问使用已注册的 read/write/fence 回调，
先校验全部视图再读，输出边界确认后写，fence 成功后才返回成功 completion。
SDK owner/compute lease 由 guest 保持至原 completion；超时保留所有权。callable
指纹标识编入的执行适配代码和 backend profile，模型权重另行按原要求固定指纹。
此接口实现和模拟回调测试不替代真实 guest 映射、输出发布及完整 PP 验收。

r467-r471 已实现并验证上述执行适配层及 guest owner operation helper：Rust
全集 971 通过、16 ignored；Python 全集 575 通过、5 环境跳过；24 个原生
生命周期夹具组合及 ASan/UBSan 通过。Qwen3-0.6B 真实单层经注册回调完成
两次 read、一次 write 和一次 fence，完整输出与 r464 逐字节一致，四项负例
继续拒绝。guest helper 已纳入构建签名和源文件列表，支持原请求等待/重试、
失败 completion 清理及 release 重试。W5 serving loop 尚未调用该 helper；
当前实际模型证据仍使用离线 provider callback，未验证物理 SDK lease、QEMU
guest 映射和受管理输出发布，不关闭 M2、PP 矩阵或总目标。

真实 provider/model 接入诊断复用双 guest managed-config：home 发布一份 2048-byte
Qwen hidden V2，consumer 使用实际 SDK reader owner。独立输出 allocation 的
前 8768 bytes 放只读 compute 控制视图，结果区从 offset 16384 开始；两视图
不重叠，共用该输出 owner，禁止新建 arena。新增有界 owner-relative offsets
接口保留旧零 offset prepare。诊断以固定 token 42、Qwen3-0.6B 的 `[1,2)`
range 核对同配置离线数值 oracle，必须记录真实 read/write/fence、原 completion、
CPU 可见性与最终回收。该诊断只验证实际映射到模型调用的连接，不代替完整
W5 serving loop、跨 range 发布/读取、PP 或 decode 矩阵。

V2 的 `payload_checksum` 必须逐字节遵守已锁定 SDK 的
`mem_service_provider_checksum64`：初值 `1469598103934665603`，按 byte XOR 后
乘 `1099511628211`，采用 64-bit wrapping。W5 数值输出的 positional checksum
保留原算法，两类校验值分别保存；不能修改完整 V2 来适配模型内部 metadata。
Rust 互操作测试编译 SDK 的实际实现作为 oracle，拒绝填入旧 W5 checksum 的 V2。

上述实际映射诊断的 CLI 参数为 `model_probe=1`、`model_output_bytes` 与
`model_output_checksum`；后两者来自相同模型配置的离线 oracle。限定 home 1、
consumer 0、ND 262144-byte backing、只读 V2 输入、单请求及无故障注入。
逻辑模型配置为两节点、range `[1,2)`、非 terminal 输出。r500 已通过实际双
guest 的 V2 映射、模型调用、原 completion、双端输出校验与对象回收；runner
配置透传及独立日志验收已接通。此单层诊断不关闭 W5 serving/PP/decode 门禁。

受管理结果解析续作：公共 `llm_infer_decode_range_result` 从完成后的 CPU
视图解析 hidden/KV，返回原 allocation 内的 offset、长度及两类 payload
checksum；不复制 payload，不授予引用访问权限。接口核对显式表指针、完整
entry checksum、预期 range 身份和边界，并保留模型数值 metadata。数值层
checksum 与序列化 payload checksum 的含义分开，禁止强制二者相等。实际
model probe 在 SDK CPU lease 内调用该接口；V2 stage/seal、下游 reader
重新 acquire/map 以及 W5 serving-loop 接入仍为后续必要工作。
r511 实际双 guest 及原日志独立复核通过：hidden 2048 bytes、KV 8232 bytes
均来自原 allocation，校验值与数值 oracle 一致，原 completion 和回收门禁
保持通过。r513 Python 全集 585 通过、5 环境跳过；r510 Rust 全集 973 通过、
16 ignored，均零失败。失败记录与当前验收边界详见实施记录，本项不关闭 M2。

输出发布接入使用现有 model probe 的 `model_publish=1` 选项。计算端作为
输出 allocation 的唯一 owner，先 BEGIN 再建立写映射；home 只在 SEAL 后
获得只读 V2。计算完成后在原 CPU lease 内准备并 stage result、hidden、KV
三个视图，解除写映射时保留 holder，SEAL 成功后再释放 owner。下游分别
acquire/map 三份完整引用，每次使用独立 session；不得在同一进程保留重复
holder 归属或使用旧 raw acquire。三份引用共享 backing，发布不复制 payload。
该实际双 guest 发布诊断仍不代替 W5 serving-loop 及多步 PP 验收。

r540 已完成上述实际发布闭环：三个视图在 writer unmap 后封存，home 分别以
独立 session 和只读映射消费，完整输出、hidden、KV 校验与数值 oracle 一致，
最终资源归零。服务 `e21d1f4` 提供保留 holder 的 unmap；`93f5dd8` 修复只读
home cache refresh 错误请求写权限的问题，两笔均已验证并按授权发布。
父仓库锁定 `93f5dd8`。本轮仍为固定 Qwen 单层验证，完整 W5 serving、两模型
2/3/8-node 多步 PP、多 home 和后续阶段继续未完成，详见实施记录。

控制面多 home 前置续作：独立服务 `e133b01` 为 `allocate_object` 增加可选、
provider 中立的 `home_node`，并通过新的
`mem_service_client_allocate_object_at_home()` 保持原 v1 request struct ABI。
新分配只绑定目标节点的活动 provider incarnation；未知、过期、超长 home
以及把已有 key 重放到另一 home 均 fail-closed。省略字段时继续使用
`allocation_home_provider` 默认值。wire、compat、baseline、old/new、upgrade、
package 与 release 契约已按依赖顺序更新。n4-910c1 的完整服务回归执行 307 项，
304 项实际通过、3 项既有环境跳过、零失败，耗时 2208.130 秒；完整
`host-artifact-smoke` 同样通过。该提交已按授权推送独立服务目标分支，父仓库
gitlink/lock 本地同步。此项只关闭按请求选择 home 的控制面能力；同时多 home
W5 serving、物理地址区间协调及 2/3/8-node 多步矩阵继续未完成。

受管理结果边界续作：`llm_infer_decode_range_result()` 现在从已校验的 range
table 返回精确 `serialized_bytes`，把模型实际写入长度与 output allocation
预留容量分开。managed model CLI 记录并校验该长度；完整 result V2 以精确长度
和实际 Memory Service checksum stage，不能把未写入的尾部容量发布给 reader。
保留固定单层 probe 对离线 oracle 长度的严格相等检查，额外容量夹具验证解析器
只返回实际结果边界。n4-910c1 的完整根仓库 Python 回归为 598 项、593 通过、
5 项既有环境跳过、零失败/错误；Rust workspace 的 43 个顶层 harness 为
976 通过、16 ignored、零失败。两套门禁均使用 network-none 容器、冻结 Cargo
registry 和锁定 Simpler PTO pin。该项为 serving loop 提供精确发布边界，仍未
接通连续 decode、terminal token、跨 range/step 生命周期或两模型运行矩阵。

terminal 结果续作：range identity 增加请求 `step` 与 `vocab_size`，公共结果
解码器在 terminal range 内要求唯一、完整宽度的 logits table 及紧随其后的
token-text table。全部 entry 必须匹配请求 step 和 vocab；采样 token、候选表、
文本 checksum、piece 长度及两个 piece word 必须互相一致，缺表、重复表、错 step、
错 vocab 或内部元数据不一致均清空结果并拒绝。非 terminal range 明确返回
`terminal_present=0`；managed CLI 同步输出 present/step/token，validator 固定
单层诊断继续要求三项为零。n4-910c1 的代码候选完整 Python 回归执行 601 项，
596 通过、5 项既有环境跳过、零失败/错误；Rust workspace 43 个顶层 harness
为 976 通过、16 ignored、零失败。该项消除了 callable 4 连续 decode 的 terminal
结果解析缺口；W5 serving loop 尚未切换到 callable 4，也未完成 terminal V2
发布、跨 step 输入接续和两模型实际矩阵。

精确结果发布续作：公共 decoder 在 `serialized_bytes` 之外直接返回该精确区间的
Memory Service checksum。result V2 发布路径消费这个已解码边界的 checksum，禁止
调用点重新按 output allocation 容量计算并把预留尾部纳入身份。固定 managed probe
日志和 validator 同时核对长度、checksum 及 producer/reader 一致性；该项补全
callable 4 结果发布的长度/内容绑定。n4-910c1 最终 Python 全集为 601 项、
596 通过、5 项既有环境跳过、零失败/错误；Rust workspace 的 43 个顶层
harness 为 976 通过、16 ignored、零失败。两套门禁均使用 network-none、
Cargo offline、冻结 registry/target 及 PTO ISA `594817f`。serving loop 的
owner 接入仍继续推进。

owner/runtime 续作：guest 侧现已提供 provider-neutral managed object、runtime 与
serving 状态机。reader 完成 RESOLVE/ACQUIRE/map/adopt，writer 完成指定 home
allocate、BEGIN、map/adopt、result/hidden/KV/terminal 多 view STAGE、unmap、SEAL
和 holder 释放；callable 4 保持全部 owner 到原 completion 后再解码和发布。
terminal V2 使用独立 kind 13，公共 decoder 返回其精确连续表 view。独立服务提交
`b5812c4` 已推送并锁定，18 项定向测试通过。该 building block 尚未替换
`apps/llm_infer` 的 callable 3 分支，不能据此关闭 W5 serving 或 M2。

holder 身份续作：独立服务把每个 managed holder 绑定到
`(node_id, provider_incarnation)`，checkpoint 升级为 v2 并兼容读取 v1；provider
失联、注销或实例替换时按精确 home/holder 身份隔离相关资源。node-aware SDK
把同一 holder 身份贯穿 reference acquire、map-begin、unmap 及不确定清理重试。
W5 writer 在 allocate/ACTIVE 后显式 acquire，不能再把 allocation 误当成 holder；
reader 和最终 token 栅栏同样核对完整节点身份。独立服务提交 `69082dd`、
`f2bf359` 已仅推送授权分支，父仓库 gitlink/lock 本地锁定到 `f2bf359`。独立
mapping/reference 定向测试 5 项通过；父仓库受影响的 managed runtime/object/
request/serving/topology/PTO 契约共 49 项，34 项通过、15 项环境跳过。没有启动
本地 QEMU、模型或多节点 workload。该项补齐生产 fencing 的 holder 归属前提；
恢复协调、可执行 fencing、worker 恢复、重新加入及实际 W5 矩阵继续未完成。

W5 serving 对象命名约定：输出 allocation 使用
`w5v1/<run:16hex>/<request:16hex>/<producer:8hex>/<step:16hex>`，均为固定宽度
小写十六进制；run/request 非零。该完整 key 由 producer 在 allocate 时确定，
随服务封存的 V2 保持不变，逻辑 result/hidden/KV 名称另加视图后缀。消费方
按预期 run/request/producer/step 查询完整逻辑 key，禁止仅按最近版本查找。
managed KV 的前驱校验使用该 key 中的 producer step 和 V2 producer/owner，
同时核对当前 range 的 node；内容 version 继续表示修改代际，不能充当 step。
DeepSeek managed decode 不读取旧 inline KV 头；legacy 路径保持既有头部校验。
该命名与来源校验须先通过 C/Rust 互操作及多步输入用例，再接入 serving 调用点。
固定双 guest 发布诊断的 output allocation 也使用该格式；两端依据共享 generation
和 home 配置得到相同 producer/step-zero key，`--validate-only` 打印该 key，
便于与实际 V2 引用核对。此诊断仍不覆盖 W5 serving loop 的请求身份与连续 decode。
实际接入修正了两个夹具未覆盖的问题：SDK adopt 清空原 mapping 后，检查必须
经 owner 的活动 lease 执行；provider 回调地址使用 `ub_gm_base + view_offset`，
与 runtime tensor aperture 分开校验，不能假设两种地址相等。

本计划展开 [实现规格第 8 节](../sim_gva_gsva_implementation_spec.md#8-剩余工作与优先级)。
机制基线为 ub_sim `39576eb`、mem_service `e4a8a58`、kernel `85fb35d`、
QEMU `f201ebe`、libobmm `53011ee`。原稿编写时根仓库为 `80cb36f`。
修订 3 核对的根仓库为 `ec01ba1ea8b42c11f5bd7ff86996c52197c50645`，
当时上述子模块 checkout 与 gitlink 一致。`ec01ba1` 是原设计依据；M1 的执行基线
应为文档获授权提交后选定的干净 commit，届时登记完整 SHA，不自动跟随移动的 HEAD。

review 是问题输入，不等同于本次重测。本文未登录目标机、同步模型、构建或运行 guest。
目标机 readiness 采用“review 报告未就绪、当前待复核”的状态，不能改写成已经恢复。

现有基础可以复用：

| 已有内容 | 源码或证据 | 对后续实施的影响 |
| --- | --- | --- |
| 地址 bootstrap、segment 分配/查询/退役 | [gva_manager](../../guest-linux/aarch64/apps/gva_manager/gva_manager.c) 与 [kernel UAPI](../../guest-linux/kernel_ub/include/uapi/ub/gsva.h) | 接入已有管理机制，不新建第二套地址分配权威 |
| 通用 provider 及映射接口 | [provider API](../../mem_service/components/mem_service/mem_service_provider.h)、[OBMM provider](../../mem_service/components/mem_service/providers/mem_service_provider_obmm.c) | 复用 register/map/publish/invalidate/wait-visible/unmap，保持 SDK 中立 |
| 服务端与持久化日志 | [daemon](../../mem_service/components/mem_service/mem_service_daemon.c) | 已有 journal、compaction 和恢复逻辑；先核对覆盖字段，再扩展 GSVA 状态恢复 |
| 服务对象分配 | [OBMM objects](../../mem_service/components/mem_service/mem_service_obmm_objects.c) | payload arena 当前线性推进；record 回收不能直接证明 payload 空间也已回收 |
| W5 GSVA 描述符 | [GSVA access helper](../../mem_service/components/mem_service/mem_service_gsva_access.h) | 当前 epoch 固定、token value 来自 token ID，必须接入真实管理状态 |
| reader 可见性修复 | [2026-09-07 调查](../2026-09-07-w5-terminal-token-visibility-investigation.md) | 保留 metadata/payload refresh 与发布序列检查，作为必须保住的回归 |

源码核对对 review 作三项更正：

- [wire 枚举](../../mem_service/components/mem_service/mem_service_wire.h) 共 24 个操作，
  review 所写“16 个”不准确；缺少受管理分配/引用操作这一判断成立。
- [W5 配置入口](../../guest-linux/aarch64/scripts/run_w5_cluster_config.sh) 和
  [既有 launcher](../../guest-linux/aarch64/scripts/run_llm_infer_eight_node_guest.sh)
  已接受 2/3/4/8-node，且提交中的 launcher 已有 nodeA/B/C 分支。
  仍需验证 3-node 的 placement、启动与运行，不能据文件名判断缺少入口。
- [kernel allocator](../../guest-linux/kernel_ub/drivers/ub/obmm/obmm_core.c)
  当前自动地址按 counter 与本次 size 计算，没有完整区间冲突/对齐分配检查；
  新分配 epoch 为 1，retire 处理主要更新 registry 标志。
  M1 必须验证并补足区间分配安全性；registry retire 成功不能替代 M2 的跨 holder
  fencing、ACK 与 TLB 清理。这些机制已有代码不等于已具备通用 allocator 保证。

本计划把工作分为三类：明确缺失的实现、已有机制之间的集成、待补的运行证据。
“尚未完成验收”不等于“整个功能没有实现”。尤其不能据此重写已有持久化服务。

## 2. 架构约束

1. 服务先启动，provider 和地址基础设施在基础设施阶段准备。停止全部 infer
   客户端不能导致服务退出；服务可在零 provider 时运行，但必须报告数据面未就绪。
2. mem_service core 只管理中立的对象、版本、租约、能力与生命周期。
   GSVA key、CNA、OBMM export/import 和 kernel 调用封装在 provider/平台适配层。
3. 本计划使用 OBMM pool 提供内存 backing，验证真实 export/import 与有界映射。
   provider 不具备所需映射能力时返回明确不支持；不能伪造固定地址映射。
4. 服务返回描述符和状态；payload 在客户端/provider endpoint 之间直接访问。
   daemon 不能注册另一个进程的普通 heap 指针，也不能充当 payload 中转站。
5. 复用现有 gva_manager 的地址权威、kernel registry 与 libobmm 物理分配。
   GSVA 能力不能依靠 URMA 模拟映射；显式传输 provider 单独处理其传输职责。
6. 严格同址申请失败必须失败。provider 不可用、stale descriptor 或 checksum
   错误不能触发自动换址、换传输、读旧数据或伪造成功。
7. 节点数量来自配置。基础能力先测 2-node，W5 覆盖 2/3/8-node；4-node 保留
   近期回归用例。不得增加 8-node 专属的新接口、数组上限或循环边界。
8. 本阶段不引入 Mooncake，不修改 simpler 算子，不改模型计算、不重做 Web，
   不借任务扩大为所有组件重构，也不承诺真实硬件性能。

## 3. 统一管理的具体含义

### 3.1 三类资源的责任

| 资源 | 唯一管理责任 | 对外可见内容 |
| --- | --- | --- |
| 物理 backing | 对应 provider 调用 OBMM pool 分配机制 | 中立容量/能力及不透明 backing handle |
| 全局地址 segment | 现有地址管理端和 kernel registry | 经校验的 segment 绑定；GSVA 专用字段由映射 provider 解释 |
| 对象及其引用 | mem_service 对象管理 | object ID、内容版本、分配代际、引用/租约状态、访问能力 |

服务编排三类资源的获得与释放，不能把三套 allocator 的内部实现合成一个巨型模块。
可先采用单一地址分配权威；高可用、多主分配与透明迁移不作为首个交付要求。

### 3.2 申请和释放

申请先校验容量、对齐、能力与请求身份，记录操作意图，再保留地址和 backing。
两者均成功、provider 描述符可用后发布对象。中途失败撤销本次资源；重复请求
通过 operation ID 返回同一结果。无法确认回滚完成的资源进入隔离状态。

释放先阻止新引用，排空或撤销现有映射/在途访问，完成所需 ACK 与 TLB 清理，
再回收 backing 和地址。持有者失联或事务超时不能作为“已经释放”的证据。
地址重用必须产生新 generation/epoch。租约到期仍需完成 provider fencing。

内容 version、对象 allocation generation、mapping generation、segment epoch、
token lease 是不同身份；需要定义关联及校验规则，不能简单复制同一个整数。

### 3.3 接口与兼容

下列是待实现或补齐的操作语义，具体函数和命令名沿用现有接口风格，尚不可直接执行：

| 操作语义 | 需要满足的行为 |
| --- | --- |
| allocate / inspect | 返回对象身份、大小、能力、当前版本和中立访问描述符 |
| map / unmap | 实际客户端进程获得/释放有界映射；严格地址失败不可降级 |
| publish / wait-visible | 对指定范围确认发布、可见字节及 checksum |
| acquire / release | 管理持有引用和释放幂等性，防止提前回收 |
| retire / revoke | 暂停新访问并使旧身份失效，暴露 pending/timeout 状态 |
| read / write | 在客户端有界内存映射上读写对象；payload 不经过控制面 |

复用 [客户端](../../mem_service/components/mem_service/mem_service_client.h)、
[wire client](../../mem_service/components/mem_service/mem_service_wire_client.h)
和 provider channel。新增字段必须检查 wire 版本、长度上限和旧端兼容性；
不能静默扩大已有结构或改变 opcode 负载。允许有版本控制的新增操作，具体决定见下节。
GSVA 私有字段只进入不透明 provider 描述符。

现有 CLI 和 [serving/pretraining examples](../../mem_service/apps/mem_service/examples/)
承载新操作的用户入口。映射客户端需保持进程存活直到 unmap；
CLI 打印出的指针只属于该进程，后续独立进程不得把它当作自己的有效映射。

### 3.4 M1 wire 与 SDK 决策

选择新增受管理对象操作，禁止向旧 `PUT_OBJECT` 塞分配/映射语义。
保留现有 wire v1 的 48-byte header、4096-byte 负载上限、旧 opcode 和旧 record ABI。
新操作从当前未占用的 `0x70` 段分配，编码前核对执行基线有无冲突。
沿用 text-KV 编解码，每个新操作独立定义必选字段及 `schema_version=1`；
新增操作不要求把所有旧操作的 `MEM_SERVICE_WIRE_SCHEMA_VERSION` 一并升级。
后续修改某个新操作的字段语义时单独升级其 schema，并提供明确兼容检查。

M1 最小控制操作集合：allocate、acquire、release、retire、inspect-allocation、
allocation-stats；provider 的注册、分配完成和发布状态使用中立内部控制消息。
map/unmap、publish/wait-visible 在 SDK 所在进程调用 provider channel；
控制 RPC 只管理引用、句柄和发布状态，不把所有动作包装成 daemon 执行的 mmap。

最小记录包含 object ID、operation/idempotency ID、owner session、allocation
generation、内容 version、大小/对齐/能力、持有者、状态、provider instance incarnation
与不透明描述符。映射句柄属于进程，不能通过恢复记录让旧指针重新有效。
新 SDK 使用独立结构和函数，保留现有 808-byte client record 布局。

兼容验收至少覆盖：旧 client 调新 server 的旧操作；新 client 遇旧 server 返回
UNSUPPORTED；未知版本、超长/截断/损坏 descriptor 被拒绝。新 client 不得把
UNSUPPORTED 自动改成旧 put/materialize 路径。现有 128-byte opaque descriptor
上限必须实测编码长度；超出时单独版本化扩展，禁止截断或无版本修改常量。

### 3.5 M1 部署与 payload ownership 决策

选择**单个控制面 daemon，加每 guest 的常驻 provider 服务模式**。
控制面默认放在 nodeA；其他 guest 不各建一个独立对象目录。具体节点由配置指定。

| 进程 | 部署与职责 | 存活期 |
| --- | --- | --- |
| 控制面 `linqu_mem_service serve` | 目录、操作事务、引用、placement；客户端直连同一配置的控制 endpoint | cluster 基础设施存活期 |
| 节点 provider 服务 | 复用 OBMM provider CLI 增加受管理服务模式；持有本节点 export 与设备句柄 | 独立于客户端，直到基础设施停止或故障 |
| 客户端 | 同一 SDK 获取引用，在自身进程 import/map、访问和 unmap | 从 acquire 到 release 的完整 session |

当前 wire client 和 daemon 主服务只支持 AF_UNIX。现有 TCP payload provider 与
loopback metrics listener 都不能用于跨 guest 的控制 RPC；修订 2 中“已有 TCP
control transport”的表述有误。M1.1 显式新增 wire-over-TCP，复用 wire 编解码和
操作语义，不复用 payload provider 充当控制通道；此选择不改变 OBMM 数据面。
常驻 provider 模式、与 daemon 的控制绑定及受管理分配均属于新增实现，不能声称
现有 conformance 已经包含这套部署。优先扩展现有 provider executable，避免新增管理平台。

对象 backing 的 home provider 调用 libobmm 分配/export，持有资源所有权。
客户端 A 写、客户端 B 读同一个 home export；本地/远端映射由各自 SDK provider 处理。
独立客户端退出不能自动销毁 home export；引用释放后由服务驱动回收。
客户端普通 heap 不作为受管理对象的长期 backing。

descriptor 从 home provider 经控制面以 opaque bytes 返回给客户端，控制面不解析
CNA/GSVA 私有字段。共享目录只允许存放启动配置或历史 conformance 交换物，
不能作为 M1 每个对象 descriptor 的运行时交换通道。payload 不经过控制 RPC。

现有 daemon 的 inline/path put 与 materialize 会经过 daemon payload 路径。
M1 保留其旧客户端兼容行为，但新分配模式禁止调用它们，也不得将它们的 PASS
计入直接数据面验收。通过计数/trace 验证 managed 模式控制消息只承载元数据。

#### 3.5.1 M1.1 控制通道与接入策略

保持单控制面架构，不增加每 guest 的控制代理或第二套对象目录。
M1 首版明确支持 `unix:<path>` 和 `tcp:<IPv4>:<port>`；IPv6、DNS 解析、自动隧道、
自动端口转发和连接池不在首版范围内。一个 daemon 实例选择一个主监听 endpoint，
TCP 部署时本节点客户端也连接这个配置的 endpoint，无需同时实现双主监听。

| 落点 | M1.1 新增工作 | 保持不变 |
| --- | --- | --- |
| [wire client](../../mem_service/components/mem_service/mem_service_wire_client.c) | 严格解析 TCP endpoint、AF_INET connect、连接/读写截止时间及错误结果 | 旧 Unix endpoint 与已有本地客户端兼容 |
| [daemon](../../mem_service/components/mem_service/mem_service_daemon.c) 的监听适配 | 配置驱动的 TCP 主监听、accept 对端校验、拒绝策略和连接清理 | wire v1 header/旧操作的本地语义；metrics 单独保持 loopback-only |
| [配置 schema](../../mem_service/apps/mem_service/configs/mem_service.conf.schema) 与 CLI | 新增显式网络接入模式、节点 allowlist、限额/超时；更新 serve/connect usage 和配置测试 | `auth_mode=none` 继续限制为本地 Unix 部署 |
| SDK 与常驻 provider 客户端 | 都通过同一 wire client 连接主服务，交换中立控制消息 | 不引入 provider 特有的控制协议副本 |

socket 地址、bind/connect/accept 及接入策略封装在 wire I/O/启动适配边界，
不把 AF_INET/TCP 类型传播到对象 core、placement、GSVA records 或模型客户端。
不能通过放宽现有 metrics parser 的 loopback 校验来完成主监听；两种监听的安全规则独立。

配置决策（以下是待新增语义，当前配置解析器不支持，不能直接作为可运行配置）：

- 默认仍为 Unix-only。TCP 必须显式选择 `auth_mode=trusted-guest-network`，并在
  目标配置中给出主监听的精确 IPv4、端口及非空 peer allowlist；漏配则启动失败。
- allowlist 使用稳定 node ID 与精确 guest IPv4 的一一对应关系，不使用通配或 CIDR。
  主监听拒绝 wildcard、广播/组播地址和无效端口，不默认绑定 `0.0.0.0`。
- accept 后先将实际 source IP 与 allowlist 匹配，再解析/执行控制操作。
  请求声明的 node ID 必须与该连接的节点身份一致；客户端与 provider 注册均执行此检查。
- 首版只用于隔离的 QEMU guest 网络，禁止对公网、共享非可信 LAN 开放，禁止借 NAT
  或代理把多个节点折叠成同一个来源身份。宿主转发和防火墙不由该服务自动修改。
- 此模式没有 TLS、密码学身份认证或进程级认证；IP allowlist 只约束可信网络内的
  来源，同节点进程属于同一信任域。wire checksum 只能查损坏，无法认证 peer。
  不将该模式作为生产网络安全能力验收；非可信网络部署需要另行补认证与加密。
- TCP 端只开放健康/就绪查询和本阶段受管理对象及 provider 控制操作的显式集合。
  旧 inline/path put、materialize、snapshot/restore 等本机文件或管理操作不在网络
  allowlist 内，调用返回 UNSUPPORTED。旧 Unix 部署继续按原有规则提供这些操作。
- 拒绝结果记录原因、peer 和 operation，日志不输出 token value、完整 descriptor
  或 payload。provider 未注册/未验证时，即使 TCP 已监听，仍报告数据面未就绪。

M1.1 沿用一次连接完成一次 RPC 的简单模型；逻辑 object-session 和 provider
incarnation 独立于单条 socket 存活期。断开 RPC 不能自动释放对象或销毁 export。
精确读取 48-byte header 后校验版本/长度，再读取有界 body；处理短读、短写、EINTR、
EOF 和断连。头部、body 与 connect 使用单次尝试的总体截止时间，不能因慢速分片
不断延长；异常连接关闭并回收 fd，不能长期阻塞其他客户端的 health/ready 请求。
连接数和未完成请求数受配置限额控制。

已有重试选项只在明确允许的查询或幂等操作上使用，受总次数和总等待预算限制。
变更操作在“请求可能执行、回复丢失”后保留同一 operation/idempotency ID，
禁止另造 ID 重试；无法确认状态就查询或报告不确定结果，不能自动回退 Unix、
payload TCP provider 或旧 put/materialize 语义。迟到响应不得交给其他 request。

M1.1 至少要以两个真实进程完成 TCP wire 请求与应答；协议结构检查或 payload
TCP canary 不能替代该证据。跨 guest 的 client/provider 注册与对象访问在 M1.2
继续验证，不能把 loopback 进程测试当作跨 guest 验收。

### 3.6 地址来源、启动与真实身份

固定地址由服务分配响应携带的 provider 描述符确定。客户端只提出大小、对齐和
能力要求，不自己 query 或挑选全局地址；SDK provider 解码描述符后设置
`MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS` 与 `requested_address`。

服务将分配请求交给 home provider；地址请求复用 gva_manager 使用的 kernel segment
UAPI，由已有地址管理机制分配并回读确认。M1 使用配置指定的单一地址分配 owner，
串行化跨节点地址请求，避免多个独立 kernel counter 造成全局冲突；初始对象 home
固定为该 owner。多 home 分配后续扩展时仍须经过同一全局区间权威。
服务自身不维护第二套可绕开 kernel 检查的地址池。M1 以整段分配/整段回收为单位，
避免依赖 W5 线性 arena 的对象子分配；基础回收属于 M1，W5 arena 回收在 M3。

当前 OBMM provider 的 48-byte v1 descriptor 和普通 export 不能直接证明严格 GSVA。
M1 需要版本化的 GSVA descriptor 与 fixed-UBA export/import，实际 VA 必须与
segment home VA 相符；单独设置客户端 fixed mmap 标志不足以通过验收。

启动编排放在 guest 基础设施启动段（initramfs 的 `run_app` 与 openEuler 对应启动部署），
由同一生命周期入口管理，launcher 只提供配置和启动 guest。依赖顺序为：

1. 驱动与 OBMM 基础通信可用；控制面可启动并报告数据面未就绪。
2. 每节点完成 gva_manager aperture bootstrap。运行时分配按请求进行，启动无需预造对象。
3. 节点 provider 通过 ioctl 查询并验证 active aperture、generation、节点集合及 capabilities。
4. provider 完成所需 peer mapping canary 与 checksum，再向控制面报告有代际的 readiness。
5. 控制面汇总配置中必需 provider 的状态后开放数据操作；客户端不会补 bootstrap。

等待以查询结果和有界超时为准，禁止固定 sleep、只 grep 日志或只检查 `/proc` 文件存在。
provider 失联时撤销对应数据面就绪状态；已有引用不能因此被静默改绑到其他 endpoint。

GSVA 身份的首选权威是 home kernel alloc/query 返回的 `obmm_gsva_segment_desc_v1`，
由 provider 读取并携带真实 epoch、token ID/value、policy、p_tag、owner 和范围。
gva_manager CLI 供运维诊断；QEMU QUERY 用于 capability、route/coherence 交叉核对，
不能成为 serving SDK 直接调用的私有入口。M1 就要读取真实初始身份；M2 再完成
rotate/revoke/reuse 的端到端更新。当前 UAPI 不包含完整 VMID/ASID 与 lease epoch，
这些字段必须来自验证过的 route/profile/provider 管理状态，禁止用猜测值填满结构。
M1 仅支持显式声明的初始 profile；不具备能力时拒绝请求。

### 3.7 CLI、用例与 M1 验收口径

扩展现有 `linqu_mem_service` CLI，计划新增 `object-session --config <path>` 模式；
该命令目前尚不存在。它执行 SDK 操作并保持进程存活直到 unmap/release，支持生产者、
消费者的确定性操作序列、有界等待和机器可读结果，不增加交互 REPL 或新的 guest app。
底层操作同样提供 SDK 函数，CLI 不能内嵌另一套服务实现。

在现有 `run_ub_dual_node_apps.sh` 增加一个 `mem_service_object_share` app 选择项及
配置文件参数，接通上述基础设施启动和两个 session；这个 app 名称表示组合用例，
不新增同名可执行组件。配置指定 OBMM provider、角色、大小、迭代与故障场景。
新选择项及参数属于待实现接口，不作为当前可运行命令列出。

M1 的 OBMM 共享**要求两独立客户端跨 guest**：A 写、B 读相同对象，再由 B 写回、
A 校验，最后解除映射并释放。payload 直接访问同一 home export，不能用旧 daemon
`put-object` 中转代替新路径。正常共享通过后，仍须完成边界、失败和资源统计验收。

M1 就增加可查询的 resource stats，使用新增 allocation-stats 操作，保持旧 status
响应兼容。至少区分 live objects、backing allocated bytes、address reserved bytes、
export/import mappings、live refs、in-flight、quarantined bytes 与累计操作计数。
provider 上报实际资源结果；客户端断连的映射不能仅根据 RPC 消失就记为已解除。
以本次 allocation ID 的差值及 guest/provider 计数交叉核对；累计计数无需归零。
正常 release 后资源恢复基线；故障未确认的隔离资源单列，不能混入 free bytes。

## 4. 实施阶段

### M1：独立服务的两客户端分配与访问

**交付：**服务独立启动，两个不同进程、位于两个 guest 的客户端，通过同一 SDK
完成对象分配、固定映射、写入、远端可见性与 checksum 校验、解除映射和释放。
整条流程不启动 llm_infer 或 sim-cli inference。部署、wire、地址来源、CLI 和统计
按 §3.4 至 §3.7 实施，不留给编码阶段重新选择另一种架构。

实施方向：

- 在现有对象管理/daemon 中接入分配事务和引用 ownership；通过 provider 调用
  地址分配、backing 和注册操作。先实现一种确定的分配策略，避免提前做复杂调度。
- OBMM provider 复用现有 mapping API；将 GSVA 专用绑定留在 provider/adapter。
  实现并验证 §3.6 的 bootstrap、固定 export 与 descriptor 版本化。
  先补齐 kernel 区间冲突、对齐和并发分配检查，再接受变长分配请求。
- 覆盖 OBMM 映射共享，由两个客户端跨 guest 访问同一对象，核对实际 VA、
  访问权限、可见性与回收结果。
- 复用现有 example/CLI，增加必要子命令与配置读取；不另起一套 launcher。

验收：两个客户端写读相同数据；重复 allocate/release 幂等；越界、固定地址
冲突、backing 耗尽和中途失败有确定结果；正常释放后新增 stats 与 provider 实际资源
恢复初始值，未确认回滚的资源单列隔离。整段释放必须在真实 unmap/unexport 后确认。
停止客户端后服务仍可接收新的客户端。provider 未就绪时数据请求明确拒绝。

代码落点：`mem_service_client.*`、`mem_service_daemon.c`、对象管理、
`providers/mem_service_provider_obmm.*`；确有 ABI 缺口才修改 kernel。

### M2：真实代际与 W5 客户端集成

**交付：**GSVA 描述符反映管理端真实身份，回收重用后旧引用拒绝、新引用成功；
W5 用新分配/访问链路完成连续 decode。

实施方向：

- 扩展现有元数据生产端与消费端，传递实际 epoch、token value/lease、地址上下文
  和允许的 cache policy。保留合法默认值的明确来源，删除无来源的固定 epoch 拼装。
- 收敛 `mem_service_gsva_access.h` 的旧平台辅助逻辑到合适的 provider/adapter；
  SDK 及模型运行时只消费中立描述符，不新增 model-specific GSVA 管理。
- 先实现不依赖重启的 rotate、revoke、retire、reuse；保持地址清理与 epoch
  更新的顺序。确认对未知状态拒绝访问，禁止静默兼容旧身份。
- retire 协调端有远端 holder 时必须等待逐节点、同 key/epoch/sequence 的成功
  ACK；发送成功、缺 ACK、失败 ACK 和超时均不能提交退役。TIMEOUT 保留为
  隔离状态，禁止通过再次 retire 将其转换为可复用状态。接收端 ACK 还须关联
  实际映射撤销、CPU/PTO 排空和 fence/TLB 完成；协调端单测不证明接收端完成。
- managed V2 unmap 先冻结 route 并保留区间占用，再等待完整严格 I/O 调用退出。
  单次响应的 pending 清零不能替代整个分块调用返回。fence 期间拒绝重入 unmap
  和新严格 I/O；任何未确认结果保留冻结状态，重试完成后才释放 route/地址。
- 将 W5 prefix/KV/hidden 对象绑定到同一对象生命周期；保留 terminal reader
  发布序列、metadata refresh、payload refresh 和 checksum 检查。
- Rust 消费端先接入与已锁定 C SDK 逐字节一致的 256-byte V2 解码器，保留
  完整 key/home 字节、generation、incarnation、内容版本及 allocation-relative
  范围；V1 类型和调用路径保持独立，禁止隐式降级。CLI 检查入口只报告结构
  有效性，不能授予访问权限。互操作测试须由 C SDK 生成编码，并共同拒绝错误
  版本、范围、权限与非规范 key；后续服务 acquire/map 和模型调用仍须独立接通。
- 显式声明支持的节点数；移除本次路径中新出现的 8-node 假设。已有 helper
  上限如暂保留，必须作为具体路径限制公开，不能包装成协议上限。
- W5 Rust 输入接入使用独立的 managed enqueue API，提交时将完整 V2 引用与
  已读取的不可变 payload 一起转移给命令队列；普通 enqueue 保持 V1 语义。
  managed 表按 256 bytes 解析，按完整引用精确匹配，验证 READ、长度、checksum、
  输入角色及必需输入，禁止访问旧全局缓存、snapshot、registry 或 inline fallback。
  命令持有输入至同步 backend 完成，失败/取消随命令释放，不建立跨请求 payload
  注册表。API 只接收 provider 已授权读取的数据，不授予 holder；调用方仍须通过
  SDK acquire/map 并维持实际 lease 至 completion。此接入不证明物理 pin、输出
  发布或整套 W5 推理完成；真实 QEMU provider 与 serving 调用点须继续接通。
- 将 M1 的单 home 分配扩展到 W5 配置中的多 home，对全局区间分配、各 home
  kernel registry 和 descriptor 归属做一致性验证。kernel retire 标志与 QEMU
  coherence/fencing 的完成必须相互核对，禁止把任一单边状态当成可重用证明。

验收：旧 key、旧 token、旧映射、错误 owner 和错 epoch 均不能读到重用对象；
新引用可写读。基于固定配置，Qwen3-0.6B 和已跑通的 2-bit GGUF DeepSeek V4 Flash
分别完成 2/3/8-node 的 4-step，核心配置推进到 8-step。
4-node 保留 terminal visibility 回归，不在每次小修改后扩展全矩阵。

必须保留：相同 prompt 原文、模型路径/指纹、采样设置及 topology，完整记录
range forwards、terminal tokens 和退出状态。GVA/GSVA 的功能不能由模型名称推定。

3-node 复用已有参数化 launcher，通过 `SIM_W5_CLUSTER_NODE_COUNT=3` 的配置文件选择；
不复制三节点 shell 脚本。用现有 placement 生成逻辑验证 layer ranges 连续、不重叠，
总和分别覆盖 Qwen profile 层数与 DeepSeek 43 层，并验证 terminal node 为 nodeC。
只在发现具体 range/launch 缺口后修已有公共路径，不先重新定义 PP 分层算法。

DeepSeek 候选 artifact 为
`DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf`。
[历史 8-node、4-step 报告](../2026-07-13-w5-deepseek-v4-flash-huawei-plain-4step-report.md)
的 run ID `2026-07-13_12-59-26_w5_deepseek_v4_flash_decode_6277` 是模型路径历史证据，
不构成 n4-910c1 当前 artifact 或当前子模块组合的验收。
本次没有确认远端路径、字节数、SHA-256、tokenizer 与 backend profile，均标为待核对。
执行 M2 前由实施者把这些值固定到目标配置和运行记录，并保持使用 Rust/simpler
模型执行路径；缺文件时只报告该项阻塞，不擅自切量化版本、下载权重或调用 DS4 动态库。
不为每轮重新散列大文件：准备阶段生成指纹并记录不可变文件身份，文件变更后重算。

### M3：回收及 GSVA 状态恢复

**交付：**客户端异常退出、服务重启或节点失联后，分配与映射状态可被可靠处理，
旧状态不会污染新对象。首版允许安全拒绝和隔离，不要求故障中的推理自动续跑。

实施方向：

- 复用 daemon 已有 journal、snapshot、compaction、torn-write recovery。
  对照持久化记录明确哪些对象、分配代际、操作意图和完成状态已覆盖，只补缺项。
- 明确 journal 写入与对客户端确认的顺序；重启时对照地址权威、kernel segment
  和 provider 有效状态进行 reconciliation，禁止仅靠恢复 JSON 宣称映射有效。
- 对未确认事务维持隔离资源；完成 fencing 后才能回收或重新发布。
  临时内存 payload 丢失时将对象标为不可用，不能仅恢复 metadata 后返回成功。
- 补引用计数、in-flight 计数和 payload 空间回收；利用已有 record recycler，
  但独立验证物理字节与地址容量回收。M1 已覆盖整段基础回收；这里补 W5 arena、
  崩溃与跨节点异常场景。暂不实现有活跃映射时的透明压缩搬迁。

验收注入点：意图记录后、地址保留后、backing 分配后、发布前后、撤销期间、
ACK 缺失及日志半写。分别杀死客户端、管理进程和一个 guest，核对重启结果。
重复重启不重复分配；旧 descriptor 拒绝；无法恢复 payload 时返回明确不可用。
失联节点重新加入前必须校验新的有效身份。

已排空服务的重启路径与活跃资源恢复分别验收：checkpoint 中所有分配均为
RETIRED、无 holder/descriptor/address/backing 或 mapping 义务、且未设置既有
recovery 标记时，允许重新接纳新代际分配；provider 注册及 readiness 仍须重新
建立。旧分配/获取/发布的成功重放必须核对当前分配代际，已退役或被替换的
代际返回冲突，不能返回可被当成当前资源的旧 descriptor。存在未完成义务或
既有 recovery 标记时继续隔离。此路径不恢复活跃 payload，也不解除故障隔离。

M1 开始编码时一并确认以下恢复接口缺口，形成有限列表，禁止等到 M3 才检查：

| 状态 | 当前权威与可用查询 | 处理决定 |
| --- | --- | --- |
| 服务分配意图、引用 | journal/record，现有内容不覆盖全部 GSVA 状态 | M1 持久化新增操作意图、资源 ID 和完成状态；M3 扩展崩溃恢复 |
| 已知 active segment | kernel query 按 ID 或 home VA 查找 | M1 可用于回读校验；M3 按 journal ID 对账 |
| 全量 segment、retired 与孤儿分配 | 新增版本 1 segment 枚举，原 active query 继续过滤 retired；r423 实际验证 active/retired/空库存 | 通过内核实例、修订号与游标读取一致库存；worker 日志自动核对与未登记资源处置继续实现，不能以 NOT_FOUND 判定资源已释放 |
| import/mmap、export 所有者 | provider 进程中的 region/mapping slots | M1 给进程加 incarnation 与资源快照查询；重启后不复用原 mapping handle |
| route/coherence/TLB 清理 | QEMU/kernel 各自状态 | 必须建立同一代际的完成证明；没有证明时隔离，不能由 proc 文本推定 |

已有 kernel counter 与初始 epoch 在重启后可能重复，服务日志的单个数字不能防止
ABA。重启后的 provider incarnation 必须变化；旧事务排空或 fencing 完成前，禁止
重新发布同址对象。需要 kernel/QEMU 扩展时限定为上述具体能力，单独附行为测试。

M3 物理核对接口约定：新增版本化 kernel segment 枚举，覆盖 active 和 retired
记录，输出原 aperture generation、完整 descriptor、地址保留状态、export
操作占用状态及 managed export ID/token。游标绑定本次内核实例的随机标识与
资源表修订号；分配、退役或 export 状态变化使旧游标返回 ESTALE，禁止拼接
不同快照。旧 active query 的 ABI 与行为保持不变。现有 gva_manager CLI 提供
完整枚举，收到明确结束标记后才输出库存；内核不支持、并发变化或数量超限均
明确失败。此库存仅证明 kernel segment/export 记录，不证明进程 VMA、QEMU
绑定、TLB 或排空完成；后续恢复必须继续核对这些状态，不能直接解除隔离。

### M4：资源压力、真实请求与分层收益

**交付：**基于已实现路径给出容量回收、命中与性能的稳定观测。该阶段以实验为主，
出现失败时只修对应问题，不能把测量任务扩张成泛化重构。

- 两客户端基础负载先交错分配/释放，再混合 prefix、decode KV 和 hidden 内存对象。
  覆盖不同大小、最长有效 prefix、TTL/版本失效及容量驱逐，记录对象和字节计数。
- 请求按 W5 统一入口顺序接收，允许各 PP stage 处理不同请求；根据实际能力设计
  交错运行。不为了本计划引入第二套入口或未经需求确认的并发调度器。
- 内存 backing 耗尽必须返回明确错误，不能自动切换 backing 类型或提前回收
  仍有合法引用的对象。
- 先记录每轮的 allocation、resolve、map、publish/visibility、transfer、
  TTFT、TPOT、总耗时、有效 cache hit 和实际重算 token 数，再解释收益来源。
- 复用已拆分的 legacy baseline 与明确选定的传统网络 provider；分别记录
  模拟参数、cache 状态和连接复用，不能用有额外软件开销的单一基线概括硬件优势。

验收：每组先完成一轮功能核对，再进行 2 轮预热、至少 5 轮正式重复，保留全部
成功和失败样本。报告样本数、median、min/max、均值和离散程度；小样本不宣称
稳定 p95/p99。资源在释放并完成事务后回到基线，稳定负载下不出现单调泄漏。
冷 cache、热 prefix、不同 suffix 独立分组，不混算。无显著收益也如实报告。

### M5：tokenless replay 与 GSVA 联合失效

**交付：**voided 事务的迟到 payload 被丢弃，replay 重新验证有效映射，
地址或 mapping handle 重用不能让旧访问作用于新对象。
本阶段可独立于性能测量推进；同步 GVA/GSVA 主线不等待 async-load 才交付。

- 以 [2026-09-05 policy 设计](2026-09-05-ub-void-response-predicate-policy-design.md)
  和 [source-owned 验证报告](../2026-09-05-source-owned-void-response-validation-report.md)
  为当前语义依据；本轮代码配对是 kernel `85fb35d`、QEMU `f201ebe`。
  2026-09-02 的 wait-key/PLT 叙述及 2026-09-03 wait_key 文档属于历史方案。
- 普通 LDR 的 void 由 source UBC 仲裁；Linux task 保持 runnable，yield 后返回
  原 LDR，重放发起新事务。旧 voided 事务只 drop payload 并清理，不能写入 cache、
  PLT、event/CQ 或唤醒软件 waiter。禁止为 GSVA 再引入 software-visible wait_key。
- 将 source 内部 transaction identity、provider incarnation 与访问时的 mapping/
  object generation、key/epoch、lease 关联；replay 走新访问的权限检查。
  仅检查新访问仍不足以保护旧指令语义：挂起/重放中的有效引用必须保留 mapping pin，
  或在撤销后让旧上下文得到终态访问错误；不能先重用 VA，再让旧 LDR 成功读新对象。
- 增加“挂起访问、撤销、释放、同地址重用、旧完成到达”的对抗序列，并覆盖重复完成。
  使用真实映射路径的证据验证；仅靠结构体字段相等无法证明实际旧写入已被阻止。

PTO 显式异步 dispatch 的 completion identity 单独验收，不将其接口套到普通 LDR。
M1 冻结 descriptor 字段时由本计划实施者与 async-load 改动维护者核对 generation/
incarnation 边界；M2 完成后、M5 首次联调前再锁定双方完整 SHA 与能力位。
当前未指定具体维护者姓名，不虚构接口人；存在进行中改动时由当前改动所属任务确认。
本计划不改另一条工作文档，不直接纳入其未提交代码。

## 5. 顺序与阶段退出条件

| 顺序 | 工作 | 可以交付给使用者的结果 |
| --- | --- | --- |
| 1 | M1，OBMM 两客户端共享、正常回收与失败矩阵 | 独立基础设施能够提供真实内存对象访问 |
| 2 | M2，动态身份后尽早接 W5 | serving 消费同一能力，旧引用失效行为正确 |
| 3 | M3，回收与恢复 | 异常和重启下不会重复分配或返回假有效对象 |
| 4 | M4，压力与收益 | 已运行工作负载下的容量边界和收益报告 |
| 5 | M5，可在 M2 后独立联调 | tokenless replay 共享同一失效与代际保证 |

M1/M2 是最先产生实质能力的两步。测试设计在对应实现阶段同步完成；
不得先花多个阶段只加配置说明、发布模板和静态检查，再延后真实两客户端访问。

不把代码行数、提交数量或文档数量换算为目标百分比。
每阶段只用“未开始、进行中、通过、阻塞”描述，并附带运行证据。
初始排期将 M1 至 M5 记为未开始；当前状态以第 8 节跟踪表及对应实际运行
记录为准。阶段内的单项通过不自动关闭整个阶段。

## 6. 验证与运行管理

主验证目标使用 n4-910c1，n4-910c 做发布前独立复现。工作与日志放在目标机
`/home/ll` 下专用目录，记录 source repo、workspace 和精确 gitlink，避免临时目录漂移。
进程级服务测试在容器中隔离；需要真实 OBMM 设备的 conformance 在 QEMU guest 内跑。
不为本计划更改宿主内核或安装系统依赖。既有 openEuler 路径在目标配置中显式选定。

本地仅运行文档、格式和已知轻量单元检查。集成、完整测试集、QEMU 和模型验证
全部在远端执行。每个阶段形成可交付修改后，完整运行受影响仓库的测试集，
记录通过、失败及条件跳过；跳过项不能算通过。

| 层次 | 必须覆盖 | 证据 |
| --- | --- | --- |
| 服务和 provider | bounds、ownership、completion、checksum、失败拒绝、readiness | 复用 [OBMM conformance](../../mem_service/tests/mem_service_obmm_provider_conformance.c) 与服务测试，新增行为用例 |
| 地址与映射 | fixed VA、冲突、不同大小、revoke/retire/reuse | 现有 2/4/8-node GSVA 验收入口和相关 kernel/QEMU 日志 |
| 独立使用 | 两进程/两 guest、同 SDK、无 infer、退出后服务继续工作 | 客户端日志、实际 checksum、容量与映射计数 |
| W5 | 2/3/8-node、4/8-step、同一模型配置；4-node 定点回归 | 各节点日志、汇总结果、token 数和错误状态 |
| 异常 | 故障点与恢复判据一一对应 | 注入时刻、恢复决策、隔离/释放统计、旧引用结果 |

沿用项目已有 launcher 和 sim-console 目标配置，新增参数放配置文件；
禁止临时拼一长串环境变量。若现有入口确实无法覆盖新操作，优先扩展已有 CLI
并附测试，不为每个用例复制一个 shell launcher。

每次运行保存：完整主仓库与子模块 SHA、配置快照、节点数、输入、run ID、
开始/结束时间、退出码、各节点日志和资源统计。失败先定位首个异常阶段，
修复后重跑最小失败用例，再跑阶段回归，避免无变化地反复跑完整推理矩阵。

### 6.1 readiness、工作区与权限

review 记录 n4-910c1 有子模块落后和 readiness 超时问题；这是待复核的外部状态。
未来实施者先在现有控制端用 CLI 取得新结果，不把 Web 历史 blocked 状态当作当前事实。
以下是已有命令，本次修订没有执行：

```sh
./target/release/sim-console readiness --target n4-910c1
./target/release/sim-console prepare-target n4-910c1
./target/release/sim-console readiness --target n4-910c1
```

顺序与判断：

1. 记录目标实际 source/workspace 路径、submodule URL、完整 gitlink、checkout 和
   fetch 结果，区分对象缺失、checkout 落后、构建缺失与探测超时。
2. 对象已可获取时复用 Prepare target farm 同步精确提交；该机制能完成的对象播种
   不要求额外 push 所有镜像，也不依赖用户手动刷新前端。
3. 对象不可获取时只列出缺失 SHA 对应的仓库、配置 remote URL 与所需分支。
   已在 GitHub 的对象优先通过现有同步路径使用；需要发布或更新镜像时另取明确 push
   授权，禁止擅自改远端、force push 或更新不相关子模块。
4. readiness 恢复后做一次固定基线 provider 2-node 验收，保存 run ID，再执行 M1 guest 用例。
   仍不可用时记录具体阻塞和下一动作；可继续轻量编码/单元测试，不排队盲跑 guest，
   不标记 M1 验收完成，不转到本地 Mac 运行集成。

职责：本计划实施者负责上述诊断、已授权的准备与结果记录；用户只处理确需新增的
发布授权或无法从配置推断的目标选择。本轮文档修订不包含环境恢复或镜像发布授权。

工作区无需整体干净才能写计划或编码，但可复现运行和提交必须有隔离边界。
本计划、三份设计文档、索引与图像构成拟议 docs 批次，等待独立 commit 授权。
sim-console、PTO/async-load、Rust 与其他计划的未提交改动由其所属任务继续处理；
不推定它们已完成，不 stash、不清理、不混入本批。实施期使用干净基线工作区，
或记录精确 patch 指纹的开发 workspace；后者不能冒充最终已提交发布证据。

### 6.2 M1 小批次与命令集

M1 按三笔候选提交组织；沿用既有批次标识，移除的批次不复用编号，避免历史记录歧义。
是否实际 commit 仍按授权执行。每笔都有 CLI/SDK 行为测试，
完整跑受影响仓库测试集；只在相关接口改变时追加对应 QEMU 用例，不每笔重跑所有拓扑。

| 候选批次 | 实现与最小功能证据 | 验证集合 |
| --- | --- | --- |
| M1.1 | 新增 wire-over-TCP 主服务/client、显式接入模式与行为测试；新 wire/SDK 分配、引用、stats、兼容及幂等事务；stub 只用于 backing 单元测试 | T-service（包含新增 T-control）；无需先恢复 QEMU target readiness |
| M1.2 | 跨 guest TCP 控制与 provider 注册、常驻 ownership、地址分配修正、严格 GSVA、双客户端写读和正常回收 | T-service、T-guest、T-provider、T-address、T-share-memory；进入 guest 阶段前完成固定基线 T-provider |
| M1.4 | 失败回滚、边界、耗尽、幂等、隔离统计；清理全部回归失败 | T-service、T-guest、T-share-memory 及其故障配置；n4-910c 独立复现 |

以下命令在准备好的远端执行；T-service 的工作目录为 mem_service，其他为 ub_sim：

```sh
# T-service
python3 -m unittest discover -s tests
```

```sh
# T-guest
python3 -m unittest discover -s guest-linux/aarch64/tests
# T-provider
./guest-linux/aarch64/scripts/run_ub_dual_node_mem_service_obmm_provider_conformance.sh
# T-address
./guest-linux/aarch64/scripts/run_ub_two_node_gsva_arm_mmu_acceptance.sh
```

T-share-memory 使用 `mem_service_object_share` 的 OBMM 共享用例与既有
runner 的配置文件参数。M1.2 必须随代码登记最终可直接执行的命令和默认配置；
此处不把尚未存在的参数写成可运行范例。配置包含有限故障集，不另增故障脚本集合。
kernel/QEMU 若有实际修改，同批补其受影响单元测试与真实 T-address 行为验收。

T-control 随 M1.1 加入 mem_service 的现有 Python 测试发现流程，复用 daemon runtime
测试方法并覆盖真实 server/client 子进程。它是新增控制通道行为用例组，当前尚未实现；
不新建一个长驻测试服务或用 TCP payload conformance 冒名替代。

| T-control 场景 | 必须观察到的结果 |
| --- | --- |
| Unix 兼容、TCP 两进程正常调用 | 原有本地操作保持兼容；新 client 经真实 TCP 主监听收到正确 request ID、status 与内容；并非 metrics HTTP 响应 |
| 配置和来源拒绝 | auth_mode=none 配 TCP、缺 allowlist、wildcard/非法 bind、未知 endpoint scheme 均拒绝；不在名单的 source IP、伪造 node ID 不执行操作 |
| 独立监听策略 | 控制监听可在隔离容器网络绑定配置内非 loopback IPv4；同样地址用于 metrics 仍被拒绝；不自动暴露宿主端口 |
| framing 和兼容错误 | 分片 header/body、短写正确处理；超长/损坏/截断、错误版本、未知操作有确定错误并清理；不回退旧 payload 路径 |
| 超时与资源限制 | 拒连、半包、缓慢 peer、并发上限与断连均在预算内结束；失败不单调泄漏 fd，其他 peer 的就绪查询仍有界完成 |
| 回复丢失和幂等 | 使用同一 operation ID 重试只产生一次变更；断开控制 socket 不释放有效对象；网络禁用的旧文件/管理操作返回 UNSUPPORTED |

以上网络行为验证在远端隔离容器环境执行。没有可用远端时可先完成轻量纯解析/
schema 单元验证，不能将 T-control 或 M1.1 标记为通过。M1.2 再以真实两个 guest
核对 allowlist 来源、provider 注册、readiness 汇总及双客户端对象流程。

M1 无需新增 sim-console 页面或 Web run 类型；先交付 CLI、现有 headless runner 和
报告。复用 target 准备能力即可，Web 展示可在后续单独授权任务中做 catalog 接入。
M1.1 的协议测试不能证明 M1.2 已完成；M1.2 通过也不能跳过 M1.4 的完整失败矩阵。

## 7. 提交和推进约束

- 每笔提交形成可测试的小能力，例如分配事务与两客户端访问、动态描述符与
  stale 拒绝、回收与重启恢复。每笔包含相应 CLI、测试和必要使用说明。
- mem_service 修复先在子仓库形成干净、验证完成的提交；获授权发布且确认远端
  可获取后，主仓库才同时更新 gitlink 与 `mem_service.lock`。
- kernel/QEMU 若需修改，同样先发布子模块对象再更新父仓库；不把仅本地存在
  的 pin 当作可供其他机器复现的完成状态。commit/push 均按用户当次授权执行。
- 当前用户已授权继续实现和及时提交；推送范围仅为 mem_service 的
  `origin/codex/gva-managed-service-completion`，并允许本地同步父仓库
  gitlink/lock。父仓库、kernel、QEMU、master 及其他分支不在推送授权内。
  扩展发布范围仍需明确仓库、remote、分支及 SHA 后取得授权。
- 不提交模型、构建产物、机器配置和运行日志。文档只保存必要摘要和证据路径。
- 每阶段汇报“新增了哪项可调用能力、用哪次运行证明、还缺什么”；
  连续两次迭代未改善能力或定位具体失败时，暂停扩大工作并给出原因与调整方案。

## 8. 跟踪表

| 里程碑 | 当前状态 | 执行基线与证据 | 未完成判据 |
| --- | --- | --- | --- |
| M1 独立分配与访问 | 通过 | r306 同一基础设施生命周期完成百个独立 session 与双 guest 直接数据闭环；r19-r20/r30 验证 readiness fail-closed；r121/r118/r297 覆盖并发、不同 size/alignment 及 backing/address 耗尽恢复；r126/r124/r321/r381 覆盖固定 VA 冲突、畸形 descriptor、旧引用与退役 descriptor 拒绝 | 无；本文定义的两节点、单 home A01-A06 与 CPU A09 子集已满足。A16/A18 继续由 M2/M4 和最终验收承担 |
| M2 动态身份与 W5 | 进行中 | r322 百轮旧 V2 拒绝、r381 旧 descriptor CPU 拒绝、r384 正常 PTO v2、r540 实际单层 result/hidden/KV 发布与只读消费通过；PP/TP 已解耦并通过 PP3/TP8 准入测试；当前锁定服务 `1107b067`；n4-910c1 r81/r82 完成 Qwen 2/3-node 4-step；r84 完成 Qwen 8-node/8-step callable 4 连续 decode、64 条 worker completion、每节点滚动退役 6 个历史 allocation 和终态资源归零；r87 完成 DeepSeek 2-node/4-step、长候选 result decode、holder-only 终态屏障和资源归零 | DeepSeek 3/8-node 的 4-step 与核心 8-step、Qwen 4-node 定点回归，以及完整身份/故障矩阵未完成 |
| M3 回收与恢复 | 通过 | r19-r20 覆盖完整/半写 ledger 隔离；r30 覆盖 daemon 双次退出；r37 覆盖七个可续作边界；r38/r41 覆盖 ACK 与 receipt 缺失、旧 CPU/PTO 拒绝和 rejoin；r46 覆盖连续两次整 guest 退出；`87d5a66f`/`c2c56c19`/`5a1d75a3` 与 n4-910c1 r49-r55 覆盖七 phase 各两周期的完整重复事务矩阵，逐 case generation 1→2、恢复/退役/物理回收及终态资源归零 | 无；本计划 M3 验收条件已满足。M1、M2、M4、M5 与总目标不受此状态变更影响 |
| M4 压力与收益 | 进行中 | r297 地址耗尽/复用、r306/r307 两种窗口百轮、r322 含旧引用拒绝的百轮通过，旧失败保留 | 混合尺寸/对齐/交错释放及重复性能样本未完成；A16 的 2/3/8-node 每节点 home 与三轮冷启动、A18 独立包跨机复现仍待验收 |
| M5 tokenless replay 联合失效 | 未开始 | 联合失效执行组合尚未锁定；现有正常共享基线不构成此项证据 | 旧完成丢弃、pin 与地址重用的联合证据未完成 |

2026-09-17 M2 补记：Qwen 8-node/8-step 的首次 r83 在 32 个历史
allocation 全部仍存活时，于 node C 的 step 4 previous-token 映射前失败；此前四个
terminal token 已完成，失败后没有生成新的 GSVA map。提交 `f527a84f` 在每轮完成
后退役本节点两步之前的 allocation，并等待 provider 确认 `retired`，把长期请求的
home backing 保留量限制为最近两个 step。定向生命周期测试通过。n4-910c1 r84
仅重跑该受影响 case，8 个节点各完成 8/8 step、共 64 条 worker 记录，token 为
`264,3644,7653,304,279,2070,315,1995`；每节点观察到 6 次滚动退役，最终 64 个
allocation 及 backing/address、export/import mapping、reference、in-flight 和
quarantine 全部归零，报告 `status=pass`，无 QEMU 残留。完整证据见实施进度记录。

2026-09-18 M2 补记：DeepSeek 2-node/4-step 的 r85 在 step 2 暴露 terminal
decoder 把所有候选完整 piece 长度误限为 16 byte；父仓库 `3a302481` 只对采样
token 执行固定 token record 上限，长度 19 的未采样候选继续以完整长度、checksum
和 16-byte preview 验证。r86 四个 token 均已产生，最终 cleanup 又暴露 holder
acquire 与 reader mapping confirm 的竞态：terminal node 看到全部 holder 后 retire，
peer 尚未完成的 mapping 随即被 `RETIRING` 拒绝。`6e4480d2` 把 final-token
rendezvous 改为纯控制面 resolve/acquire/release holder 屏障，不建立无读取用途的
数据映射，并增加 cleanup phase 诊断。定向测试 1/1 通过。

n4-910c1 r87 使用真实 DeepSeek V4 Flash IQ2XXS 模型、独立服务
`1107b0677e7d1558b185e6440582c2b409fb3771` 和 QEMU `d1307d0f` 完成唯一一次
修复后复验。run
`2026-09-18_01-03-43_w5_deepseek_v4_flash_decode_24558_headless8` 为 2/2 节点、
4/4 step，输出 token `260,6646,5217,1951`，文本
`" a leading global information"`。两个节点的 cleanup 均核对 8 个 allocation，
对象、backing/address、export/import mapping、holder、in-flight 与 quarantine
全部归零，终态无 QEMU。该结果关闭 DeepSeek 2-node/4-step；M2 仍缺 DeepSeek
3/8-node 4-step、核心 8-step、Qwen 4-node 定点回归及完整身份/故障矩阵。

## 9. Review 意见处置

2026-09-14 执行补记：独立服务 `3e5b35b` 已接入持久幂等历史与缓存回收，
并仅发布用户授权的分支。r265 全量 Python 测试为 276 项、271 通过、5 环境
跳过；r264 原生与 r269 插桩 daemon 的六项生命周期测试均通过，安装态 SDK
和实际 CLI 生成清单通过。父仓库本地锁定及链接清单已同步，r270 下游 Python
为 543 项、538 通过、5 环境跳过；r271 Rust 为 43 harness、963 通过、
16 ignored、零失败。r273 实际三轮已越过原幂等容量限制，第三轮同步对象
因 address_capacity 拒绝，整轮失败。
100 次控制事务不计为真实 allocation 百轮；地址重用、AM3 恢复和完整后续
矩阵仍未完成。失败轮次和证据指纹见实施记录 r263-r273。

以下对应 `rrr0908.md` 原编号。第一轮按编号共有 18 项，第二轮新增 R2-1、R2-2。
原 review 文件保留原样，不把待确认事项改写为已完成。

| 编号 | 澄清或决定 | 状态与落点 |
| --- | --- | --- |
| A1 | 使用 readiness/prepare/readiness 定位并恢复；按实际缺失对象处理，未默认授权 push 镜像 | 执行时待复核，§6.1 |
| A2 | ec01ba1 为修订审计依据；实际运行另锁干净 commit | 已定，§1、§8 |
| A3 | docs 独立候选批次；不处理其他任务未提交文件 | 规则已定，提交待授权，§6.1、§7 |
| B1 | 新 opcode、新 SDK 结构；旧 wire header/操作不变，schema 按操作版本化 | 已定，§3.4；更正当前为 24 个操作 |
| B2 | 单控制面与每节点常驻 provider；provider 持有 export；控制面仅交换描述符 | 已定，§3.5；跨 guest wire-over-TCP 属于待新增工作，见 R2-1 |
| B3 | kernel 地址权威分配，provider 传回描述符；客户端不自行挑址 | 已定，§3.6 |
| B4 | guest 基础设施编排依赖与查询；provider 完成真实 canary 才就绪 | 已定，§3.6 |
| B5 | 现有 CLI 新增有界 session 模式，现有 dual-node runner 增加组合用例 | 已定，§3.7；接口待实现 |
| B6 | OBMM 共享要求两客户端跨 guest，通过同一对象完成双向写读与释放 | 修订 4 收敛范围，§3.7、M1.2 |
| B7 | M1 就暴露资源 stats，交叉核对 provider/kernel，不把累计计数要求为零 | 已定，§3.7 |
| C1 | provider 读 kernel alloc/query 的真实身份；不够的字段显式声明 profile 或扩展 | 已定，§3.6；M2 完成动态更新 |
| C2 | 已有 3-node 分支；复用配置和 placement 逻辑，按实际失败补公共路径 | 已澄清，M2 |
| C3 | 固定候选 GGUF 和历史 run；目标路径/指纹/profile 仍待实测确认 | 外部事实待确认，M2 |
| C4 | 对齐 09-05 tokenless 方案；不引入 wait_key，PTO dispatch 单独验证 | 已定，M5；联调维护者与执行 SHA 待锁定 |
| C5 | 当前 query 不足以证明全量/retired/进程映射恢复；M1 先列明缺口 | 已澄清，M3 表；实际扩展需测试 |
| D1 | 本次无 commit/push 授权，发布时逐仓库明确范围 | 已定，§7 |
| D2 | M1 不新增 Web UI；复用准备能力，CLI/headless 先交付 | 已定，§6.2 |
| D3 | 三笔候选能力提交，保留既有批次标识；完整受影响测试集加定点真实用例 | 修订 4 调整，§6.2 |
| R2-1 | 采纳：当前主 wire 服务和 client 仅有 Unix 通道；M1.1 新增 wire-over-TCP 主监听、client、配置与 T-control | 设计已定、实现未开始，§3.5.1、§6.2 |
| R2-2 | 采纳并明确为必需：显式可信 guest 网络模式、精确 bind、非空 peer allowlist、节点身份匹配和网络操作集合；metrics 保持原限制 | 设计已定、实现未开始，§3.5.1、T-control |

可以按上述设计开展 M1 实现；远端 readiness 未确认会阻止真实 guest 验收，
不会阻止轻量编码和单元验证。文档提交、远端模型确认和镜像发布分别处理，
不将它们捆绑成一串需要重复审批的手工步骤。

本次交付仅修订实施计划，没有执行远端验证，
没有修改服务、kernel、QEMU 或 W5 运行代码，也没有 commit 或 push。
