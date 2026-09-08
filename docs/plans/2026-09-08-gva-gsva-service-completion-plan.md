# GVA / GSVA 服务能力补齐：实施方向与阶段计划

日期：2026-09-08；修订 3：按 `rrr0908.md` 第二轮 review 补齐控制通道设计。
状态：实施决策已明确；远端 readiness、模型 artifact 和发布授权仍待执行时确认。
以下里程碑均未由本次文档工作完成。

目标：mem_service 独立作为基础设施启动，向多个独立客户端提供对象分配、
访问、共享与回收。GSVA 作为地址管理/映射能力，由服务与 provider 协同提供；
W5、其他 serving 和 pretraining 只消费同一套服务 SDK。

建议先交付一个最小但完整的能力：**不启动 infer，两个独立客户端通过服务
分配并共享同一个对象，完成跨节点写读、校验和释放。** 随后推进动态代际、
W5 集成、故障恢复及资源压力验证。每个阶段必须产生可运行能力或明确实验结果。

## 1. 范围与依据

本计划展开 [实现规格第 8 节](../sim_gva_gsva_implementation_spec.md#8-剩余工作与优先级)。
机制基线为 ub_sim `39576eb`、mem_service `e4a8a58`、kernel `85fb35d`、
QEMU `f201ebe`、libobmm `53011ee`。原稿编写时根仓库为 `80cb36f`。
本次修订核对的根仓库为 `ec01ba1ea8b42c11f5bd7ff86996c52197c50645`，
上述子模块 checkout 仍与 gitlink 一致。`ec01ba1` 是本轮设计依据；M1 的执行基线
应为文档获授权提交后选定的干净 commit，届时登记完整 SHA，不自动跟随移动的 HEAD。

review 是问题输入，不等同于本次重测。本文未登录目标机、同步模型、构建或运行 guest。
目标机 readiness 采用“review 报告未就绪、当前待复核”的状态，不能改写成已经恢复。

现有基础可以复用：

| 已有内容 | 源码或证据 | 对后续实施的影响 |
| --- | --- | --- |
| 地址 bootstrap、segment 分配/查询/退役 | [gva_manager](../../guest-linux/aarch64/apps/gva_manager/gva_manager.c) 与 [kernel UAPI](../../guest-linux/kernel_ub/include/uapi/ub/gsva.h) | 接入已有管理机制，不新建第二套地址分配权威 |
| 通用 provider 及映射接口 | [provider API](../../mem_service/components/mem_service/mem_service_provider.h)、[OBMM provider](../../mem_service/components/mem_service/providers/mem_service_provider_obmm.c) | 复用 register/map/publish/invalidate/wait-visible/unmap，保持 SDK 中立 |
| 服务端与存储日志 | [daemon](../../mem_service/components/mem_service/mem_service_daemon.c) | 已有 journal、compaction 和恢复逻辑；先核对覆盖字段，再扩展 GSVA 状态恢复 |
| 服务对象分配 | [OBMM objects](../../mem_service/components/mem_service/mem_service_obmm_objects.c) | payload arena 当前线性推进；record 回收不能直接证明 payload 空间也已回收 |
| W5 GSVA 描述符 | [GSVA access helper](../../mem_service/components/mem_service/mem_service_gsva_access.h) | 当前 epoch 固定、token value 来自 token ID，必须接入真实管理状态 |
| 存储 backing 引用 | [UB-SSD backend](../../mem_service/components/mem_service/mem_service_ub_ssd_gsva_backend.h) | 复用 block 身份/版本/校验，保留独立于内存映射的访问机制 |
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

![职责、生命周期和读取顺序的 SVG](../gva_gsva_current_architecture.svg)

![客户端与并列 backing 的空间架构插画](../gva_gsva_current_architecture.generated.png)

[SVG 原图](../gva_gsva_current_architecture.svg) 保留完整逻辑；
[PNG 插画](../gva_gsva_current_architecture.generated.png) 只展示架构职责，设备外形为示意。
图像来源和模型版本核验限制见 [生成记录](../gva_gsva_current_architecture.imagegen.md)。

1. 服务先启动，provider 和地址基础设施在基础设施阶段准备。停止全部 infer
   客户端不能导致服务退出；服务可在零 provider 时运行，但必须报告数据面未就绪。
2. mem_service core 只管理中立的对象、版本、租约、能力与生命周期。
   GSVA key、CNA、OBMM export/import 和 kernel 调用封装在 provider/平台适配层。
3. OBMM pool 与 UB-SSD 是并列 backing。前者提供映射内存，后者提供块 I/O。
   不具备映射能力的 backing 对 map 请求返回明确不支持；不能伪造固定地址映射。
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
| 物理 backing | 对应 provider 调用 OBMM pool 或存储分配机制 | 中立容量/能力及不透明 backing handle |
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
| read / write | 对块 backing 使用显式 I/O，保持与 map 能力的差异 |

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
不新增同名可执行组件。配置区分 OBMM/SSD、角色、大小、迭代与故障场景。
新选择项及参数属于待实现接口，不作为当前可运行命令列出。

M1 OBMM 和 UB-SSD **都要求两独立客户端跨 guest**：A 写、B 读相同对象后释放。
SSD 由 home 节点的数据 endpoint 执行块 I/O，客户端 buffer 经数据 provider 到达该
endpoint；同一 SDK 管理对象身份，不要求 SSD block 本身 mmap。
可复用 backend 编码与设备 I/O，不能用旧 daemon `put-object` 中转代替新路径。
SSD 是 M1 的独立子交付，不能因 OBMM 用例通过就将整个 M1 标为完成。

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
- 首批覆盖 OBMM 映射共享；同阶段另测 UB-SSD 分配、显式写读和释放。
  两者都由两个客户端跨 guest 访问同一对象，能力查询准确区分 map 与块 I/O。
- 复用现有 example/CLI，增加必要子命令与配置读取；不另起一套 launcher。

验收：两个客户端写读相同数据；重复 allocate/release 幂等；越界、固定地址
冲突、backing 耗尽和中途失败有确定结果；正常释放后新增 stats 与 provider 实际资源
恢复初始值，未确认回滚的资源单列隔离。整段释放必须在真实 unmap/unexport 后确认。
停止客户端后服务仍可接收新的客户端。provider 未就绪时数据请求明确拒绝。

代码落点：`mem_service_client.*`、`mem_service_daemon.c`、对象管理、
`providers/mem_service_provider_obmm.*`、UB-SSD adapter；确有 ABI 缺口才修改 kernel。

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
- 将 W5 prefix/KV/hidden 对象绑定到同一对象生命周期；保留 terminal reader
  发布序列、metadata refresh、payload refresh 和 checksum 检查。
- 显式声明支持的节点数；移除本次路径中新出现的 8-node 假设。已有 helper
  上限如暂保留，必须作为具体路径限制公开，不能包装成协议上限。
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

M1 开始编码时一并确认以下恢复接口缺口，形成有限列表，禁止等到 M3 才检查：

| 状态 | 当前权威与可用查询 | 处理决定 |
| --- | --- | --- |
| 服务分配意图、引用 | journal/record，现有内容不覆盖全部 GSVA 状态 | M1 持久化新增操作意图、资源 ID 和完成状态；M3 扩展崩溃恢复 |
| 已知 active segment | kernel query 按 ID 或 home VA 查找 | M1 可用于回读校验；M3 按 journal ID 对账 |
| 全量 segment、retired 与孤儿分配 | 当前 UAPI 无枚举游标；active query 过滤 retired | M1 确认缺口；M3 需要版本化枚举/状态查询或可证明完整的分配日志；不能以 NOT_FOUND 判定资源已释放 |
| import/mmap、export 所有者 | provider 进程中的 region/mapping slots | M1 给进程加 incarnation 与资源快照查询；重启后不复用原 mapping handle |
| route/coherence/TLB 清理 | QEMU/kernel 各自状态 | 必须建立同一代际的完成证明；没有证明时隔离，不能由 proc 文本推定 |

已有 kernel counter 与初始 epoch 在重启后可能重复，服务日志的单个数字不能防止
ABA。重启后的 provider incarnation 必须变化；旧事务排空或 fencing 完成前，禁止
重新发布同址对象。需要 kernel/QEMU 扩展时限定为上述具体能力，单独附行为测试。

### M4：资源压力、真实请求与分层收益

**交付：**基于已实现路径给出容量回收、命中与性能的稳定观测。该阶段以实验为主，
出现失败时只修对应问题，不能把测量任务扩张成泛化重构。

- 两客户端基础负载先交错分配/释放，再混合 prefix、decode KV、hidden 和 SSD block。
  覆盖不同大小、最长有效 prefix、TTL/版本失效及容量驱逐，记录对象和字节计数。
- 请求按 W5 统一入口顺序接收，允许各 PP stage 处理不同请求；根据实际能力设计
  交错运行。不为了本计划引入第二套入口或未经需求确认的并发调度器。
- 内存 backing 耗尽不能自动变成 SSD backing。显式迁移策略需单独配置和记录，
  且新版本发布前保持旧对象的合法引用或明确阻止访问。
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
| 1 | M1，先 OBMM 两客户端，再同服务的 SSD 分配/I/O | 独立基础设施能够提供真实对象访问 |
| 2 | M2，动态身份后尽早接 W5 | serving 消费同一能力，旧引用失效行为正确 |
| 3 | M3，回收与恢复 | 异常和重启下不会重复分配或返回假有效对象 |
| 4 | M4，压力与收益 | 已运行工作负载下的容量边界和收益报告 |
| 5 | M5，可在 M2 后独立联调 | tokenless replay 共享同一失效与代际保证 |

M1/M2 是最先产生实质能力的两步。测试设计在对应实现阶段同步完成；
不得先花多个阶段只加配置说明、发布模板和静态检查，再延后真实两客户端访问。

不把代码行数、提交数量或文档数量换算为目标百分比。
每阶段只用“未开始、进行中、通过、阻塞”描述，并附带运行证据。
目前 M1 至 M5 均为未开始；此状态指本计划的新增交付，不抹去已有机制和历史测试。

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

M1 按四笔候选提交组织；是否实际 commit 仍按授权执行。每笔都有 CLI/SDK 行为测试，
完整跑受影响仓库测试集；只在相关接口改变时追加对应 QEMU 用例，不每笔重跑所有拓扑。

| 候选批次 | 实现与最小功能证据 | 验证集合 |
| --- | --- | --- |
| M1.1 | 新增 wire-over-TCP 主服务/client、显式接入模式与行为测试；新 wire/SDK 分配、引用、stats、兼容及幂等事务；stub 只用于 backing 单元测试 | T-service（包含新增 T-control）；无需先恢复 QEMU target readiness |
| M1.2 | 跨 guest TCP 控制与 provider 注册、常驻 ownership、地址分配修正、严格 GSVA、双客户端写读和正常回收 | T-service、T-guest、T-provider、T-address、T-share-memory；进入 guest 阶段前完成固定基线 T-provider |
| M1.3 | 同一对象服务的跨 guest SSD 写读释放，控制面无 payload | T-service、受影响的 T-guest、T-share-ssd；共享生命周期有改动则加 T-share-memory |
| M1.4 | 失败回滚、边界、耗尽、幂等、隔离统计；清理全部回归失败 | T-service、T-guest、两种 T-share 及其故障配置；n4-910c 独立复现 |

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

T-share-memory / T-share-ssd 是待实现的 `mem_service_object_share` 选择项，使用同一
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
M1.1 的协议测试不能证明 M1.2 已完成；M1.2 通过也不能跳过 M1.3。

## 7. 提交和推进约束

- 每笔提交形成可测试的小能力，例如分配事务与两客户端访问、动态描述符与
  stale 拒绝、回收与重启恢复。每笔包含相应 CLI、测试和必要使用说明。
- mem_service 修复先在子仓库形成干净、验证完成的提交；获授权发布且确认远端
  可获取后，主仓库才同时更新 gitlink 与 `mem_service.lock`。
- kernel/QEMU 若需修改，同样先发布子模块对象再更新父仓库；不把仅本地存在
  的 pin 当作可供其他机器复现的完成状态。commit/push 均按用户当次授权执行。
- 本次仅授权修订文档，没有授权在 mem_service/master 或任何镜像分支提交、push。
  主仓库 gitlink/lock 的发布也不因子仓库发布自动获得授权；未来申请时明确一次操作
  涉及的子仓库、remote、目标分支、SHA 及父仓库更新，获批后按该范围执行。
- 不提交模型、构建产物、机器配置和运行日志。文档只保存必要摘要和证据路径。
- 每阶段汇报“新增了哪项可调用能力、用哪次运行证明、还缺什么”；
  连续两次迭代未改善能力或定位具体失败时，暂停扩大工作并给出原因与调整方案。

## 8. 跟踪表

| 里程碑 | 当前状态 | 执行基线与证据 | 未完成判据 |
| --- | --- | --- | --- |
| M1 独立分配与访问 | 未开始；控制通道待实现，guest 环境待复核 | 设计依据 ec01ba1 / e4a8a58；执行 SHA、配置指纹、run ID 待登记 | T-control 及两独立客户端 guest 端到端证据均未完成 |
| M2 动态身份与 W5 | 未开始；远端模型待核对 | 待 M1 冻结与模型指纹登记 | 固定 profile 尚未由真实管理状态替换并验证 |
| M3 回收与恢复 | 未开始 | 待登记 journal/kernel/provider 共同基线 | GSVA 状态故障矩阵未形成完整结果 |
| M4 压力与收益 | 未开始 | 待登记代码、配置和样本集 | 新能力缺少混合负载与重复测量 |
| M5 tokenless replay 联合失效 | 未开始 | 当前配对 kernel 85fb35d / QEMU f201ebe；实际联调完整 SHA 待登记 | 旧完成丢弃、pin 与地址重用的联合证据未完成 |

## 9. Review 意见处置

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
| B6 | OBMM 和 SSD 都要求两客户端跨 guest，SSD 保留显式 I/O | 已定，§3.7、M1.3 |
| B7 | M1 就暴露资源 stats，交叉核对 provider/kernel，不把累计计数要求为零 | 已定，§3.7 |
| C1 | provider 读 kernel alloc/query 的真实身份；不够的字段显式声明 profile 或扩展 | 已定，§3.6；M2 完成动态更新 |
| C2 | 已有 3-node 分支；复用配置和 placement 逻辑，按实际失败补公共路径 | 已澄清，M2 |
| C3 | 固定候选 GGUF 和历史 run；目标路径/指纹/profile 仍待实测确认 | 外部事实待确认，M2 |
| C4 | 对齐 09-05 tokenless 方案；不引入 wait_key，PTO dispatch 单独验证 | 已定，M5；联调维护者与执行 SHA 待锁定 |
| C5 | 当前 query 不足以证明全量/retired/进程映射恢复；M1 先列明缺口 | 已澄清，M3 表；实际扩展需测试 |
| D1 | 本次无 commit/push 授权，发布时逐仓库明确范围 | 已定，§7 |
| D2 | M1 不新增 Web UI；复用准备能力，CLI/headless 先交付 | 已定，§6.2 |
| D3 | 四笔候选能力提交，完整受影响测试集加定点真实用例 | 已定，§6.2 |
| R2-1 | 采纳：当前主 wire 服务和 client 仅有 Unix 通道；M1.1 新增 wire-over-TCP 主监听、client、配置与 T-control | 设计已定、实现未开始，§3.5.1、§6.2 |
| R2-2 | 采纳并明确为必需：显式可信 guest 网络模式、精确 bind、非空 peer allowlist、节点身份匹配和网络操作集合；metrics 保持原限制 | 设计已定、实现未开始，§3.5.1、T-control |

可以按上述设计开展 M1 实现；远端 readiness 未确认会阻止真实 guest 验收，
不会阻止轻量编码和单元验证。文档提交、远端模型确认和镜像发布分别处理，
不将它们捆绑成一串需要重复审批的手工步骤。

本次交付仅修订实施计划，没有执行远端验证，
没有修改服务、kernel、QEMU 或 W5 运行代码，也没有 commit 或 push。
