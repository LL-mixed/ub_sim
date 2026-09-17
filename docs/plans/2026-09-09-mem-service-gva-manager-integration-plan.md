# 将 gva_manager 接入 mem_service：地址管理能力补齐计划

日期：2026-09-09。状态：实施计划，附局部实现记录；整体目标尚未验收。

阅读说明：第 1 至 6 节定义目标、缺口、实施范围和验收要求；第 7 节单独记录
局部进展。其中的编译及单测结果只覆盖明确列出的范围，不代表跨节点服务能力
通过验收。后续仅凭实际运行证据更新对应验收项。

**目标：复用并扩展现有 `gva_manager`，使 mem_service 独立管理全局对象的地址、
实际 backing 和访问寿命。不同节点通过同一个 global object reference 获取同一对象，
既能使用 CPU load/store，也能使用接收 memref 的 TLOAD/TSTORE 访问同一份 OBMM 数据。**

本文细化 [GVA/GSVA 服务补齐总计划](2026-09-08-gva-gsva-service-completion-plan.md)
中的地址管理工作，并提升 CPU/PTO 交叉访问为本目标的必验能力。性能分析等工作
继续由总计划追踪，范围以总计划修订 4 为准。后续按本文 AM1 至 AM4 推进地址管理；总计划中较早的
源码盘点和“未开始”状态不能覆盖本文记录的最新代码进展。

## 1. 目标与边界

### 1.1 最终应交付的使用方式

1. 集群基础设施启动 mem_service 控制服务和各节点 provider，协商 aperture，
   建立 OBMM pool、地址管理和路由。全过程不启动 `llm_infer` 或 `sim-cli` infer 命令。
2. 客户端通过 Memory Service SDK 分配对象，获得可跨节点传递的 global object
   reference。该引用标识对象及其分配代际；各节点不重新生成一个本地对象替代它。
3. 客户端用该引用取得 CPU mapping 或 PTO memref。相同对象字节在参与进程中的
   CPU 指针值一致；PTO 描述符指向相同 home segment 和实际 backing。
4. 映射有效期间，CPU 直接执行 load/store，PTO 执行 TLOAD/TSTORE；普通内存访问
   不逐条向控制服务发送解析 RPC，payload 不由控制服务中转。
5. SDK 提供发布、等待可见、释放与退役操作。服务阻止过期引用重新取得访问能力，
   在所有映射和执行中的访问安全退出后才允许回收地址及 backing。
6. 客户端退出后基础设施继续服务其他客户端；部署物、启动、停止、诊断和 CLI
   不依赖任何模型或 infer runtime。

### 1.2 必须保持的架构约束

| 事项 | 本计划的明确选择 |
| --- | --- |
| 地址语义 | 严格 GSVA 模式下，segment 满足 `user_va = UBA = home_va`；对象字节指针为 `home_va + object_offset + byte_offset`。地址冲突直接拒绝，禁止悄悄换成本地 VA |
| 对象语义 | 跨节点引用具有相同对象身份和分配 generation；内容 version、地址 segment epoch、provider incarnation 分别管理，不能相互代替 |
| 数据来源 | CPU 与 PTO 共用实际 OBMM backing；禁止用相同内容的两份 buffer 冒充同一对象 |
| core 边界 | core 保留中立的对象、租约、版本、生命周期和不透明 descriptor；GSVA、CNA、OBMM、kernel 调用留在 provider/平台适配层 |
| 管理权威 | 复用现有 manager 的协调机制，形成一个地址分配权威；各 home kernel 执行受协调的本地保留与检查，不各自随意选择全局地址 |
| 物理分布 | global pool 由多个节点的 OBMM 资源组成；支持不同对象具有不同 home，禁止将所有对象固定归 nodeA 所有 |
| backing 关系 | OBMM memory 与 UB-SSD 保持并列。本计划验证 OBMM 可寻址内存，不要求把 SSD block 直接映射成 CPU 指针 |
| 故障处理 | 回收结果未知时隔离资源；拒绝新的不安全访问，保留故障原因。禁止复制数据、改用旧路径或提前复用地址来掩盖失败 |

### 1.3 本轮不承担的工作

- 不重写已存在的服务 wire、TCP 控制通道、provider directory 或通用 managed allocation。
- 不实现自动主节点选举、跨地址域迁移、任意进程透明接管或跨节点通用原子操作。
- 不宣称 CPU 缓存天然跨设备一致；可见性由明确的发布/获取语义及平台机制保证。
- 不以性能收益、1M context、完整模型训练适配为本目标的完成条件。
- 不要求更改 Simpler/PTO ISA 算子 ABI；先复用现有能力，确有阻塞再单列证据和变更范围。

## 2. 当前实现基线

以下是计划建立时的源码与 Git 盘点，属于历史基线，**不代表当前工作区状态或运行验证通过**。
后续局部实现见第 7 节；执行每个阶段前重新记录实际源码组合，不能沿用本表 SHA
为更新后的代码出具验收结论。

| 仓库 | 核对状态 |
| --- | --- |
| ub_sim | HEAD `6b4909a` |
| 根仓库固定的 mem_service | gitlink `c0f0138` |
| 本地 mem_service checkout | HEAD `2deaa71`，领先根仓库 gitlink；daemon 另有未提交修改，不纳入已交付基线 |
| kernel_ub / QEMU | 根仓库 gitlink 分别为 `85fb35d` / `f201ebe` |

执行阶段先选定干净的组合，记录完整 SHA、lock 和配置摘要。禁止把本地子模块
领先的实现直接描述为“从根仓库 clone 即可得到”。本文不修改任何 gitlink。

### 2.1 已有机制与可以复用的进展

| 能力 | 代码证据 | 当前边界 |
| --- | --- | --- |
| aperture bootstrap、segment 协调 | [gva_manager.c](../../guest-linux/aarch64/apps/gva_manager/gva_manager.c) 的 `register_kernel_aperture`、`run_segment_protocol` | 有真实机制；主程序是有限时长的 CLI/验证流程，尚未成为对象服务的常驻地址管理后端 |
| 固定 UBA export、peer import、rotate/retire | 同文件的 `obmm_do_export_fixed_uba` 调用及 segment 生命周期代码 | 可抽取复用；验证退出时保留某些映射不能替代服务化 ownership |
| kernel segment registry | [gsva.h](../../guest-linux/kernel_ub/include/uapi/ub/gsva.h)、[obmm_core.c](../../guest-linux/kernel_ub/drivers/ub/obmm/obmm_core.c) | 已有 alloc/query/retire；自动地址选择、重用和跨 holder 退役保证不完整 |
| 受管理对象与 holder | [mem_service_allocation.h](../../mem_service/components/mem_service/mem_service_allocation.h)，提交 `82d0f96` | 已有 ALLOCATING、ACTIVE、RETIRING、RETIRED、QUARANTINED；不能重新按“没有对象分配 API”立项 |
| provider 注册与服务端门控 | [mem_service_provider_directory.h](../../mem_service/components/mem_service/mem_service_provider_directory.h)，提交 `14080e8` | 已管理 provider incarnation/readiness；注册成功本身不证明实际 aperture/mapping 可用 |
| backing 发布与回收确认 | 提交 `8b15431`、`2deaa71`；[client API](../../mem_service/components/mem_service/mem_service_client.h) 的 `mem_service_client_publish_allocation` / `mem_service_client_reclaim_allocation` | 已有 home provider 确认入口；尚需接到真实 manager/export/import 和持久资源状态 |
| OBMM provider 映射 | [mem_service_provider_obmm.c](../../mem_service/components/mem_service/providers/mem_service_provider_obmm.c) | 当前分配调用普通 `obmm_do_export`；48-byte v1 descriptor 与固定 VA 参数尚不能证明严格 GSVA |
| Memory Service/PTO 对象适配 | [lingqu_shmem_pto](../../guest-linux/aarch64/libs/lingqu_shmem_pto/README.md)、[acquire API](../../guest-linux/aarch64/libs/lingqu_shmem_pto/lingqu_shmem_mem_service.h) | 已校验对象引用并以 lease pin mapping；已有 in-place KV 支持。尚未与 managed allocation、动态 segment 寿命统一 |

### 2.2 点名缺口

| 编号 | 缺口与当前影响 | 必须补齐的内容 | 交付阶段 |
| --- | --- | --- | --- |
| G1 | manager 以一次性流程存在，mem_service 没有调用它来完成对象地址分配 | 提取共享实现，由常驻 provider 持有 manager session；保留原 CLI 和回归入口 | AM1 |
| G2 | managed key/generation、现有 global object reference 与 GSVA segment 尚无统一绑定 | 建立一条可查询、可校验的对象到 segment/backing 绑定；兼容已有引用，不另造平行对象空间 | AM1、AM2 |
| G3 | kernel 自动 VA 按 counter 和当前 size 推导；不同大小、对齐、并发与重用缺少完整区间保证 | 全局区间分配、home kernel 原子保留及冲突检查；统一地址域，处理耗尽、碎片与隔离区 | AM1、AM3 |
| G4 | 普通 OBMM export/import 未绑定 manager 返回的真实 GSVA 身份 | fixed-UBA export、版本化私有 descriptor、严格导入、映射/路由实测；不得合成 epoch/token | AM1 |
| G5 | CPU mapping、PTO memref、managed holder 分属不同寿命管理 | SDK 统一 acquire/release，绑定同一对象、backing、权限和执行中引用；处理多线程/多请求 | AM2、AM3 |
| G6 | 同一内容或 checksum 无法证明 CPU/PTO 共用 backing，也无法证明交叉写入可见 | 明确 publish/acquire 顺序、cache 属性与维护；验证四种读写组合及 backing 身份 | AM1、AM2 |
| G7 | retire 标志和 RPC 超时不能证明旧 CPU 映射、PTO 操作及 tokenless replay 已退出 | holder/context 撤销、映射失效、TLB 清理和执行 drain；不确定资源隔离，防止地址 ABA | AM3 |
| G8 | 真实 reservation 的故障恢复尚未闭环；当前 managed header 仍标注 volatile，发布前资源也可能已分配 | 基于现有 journal 补全意图/结果、取消确认、重启核对与孤儿资源处理；核对在途 daemon 改动后补差量 | AM1、AM3 |
| G9 | 已有 GSVA 与 PTO 分立验证入口，缺少同一服务对象统一访问的完整证据 | 不依赖 infer 的端到端测试，加多 home、2/3/8-node 和 W5 消费者回归 | AM1 至 AM4 |
| G10 | segment descriptor 不能单独表达每个进程的 VA 占用、上下文与访问权限 | provider/平台侧维护进程 mapping、地址空间上下文和权限绑定；CPU 页级保护与 PTO 字节范围分别验收 | AM1 至 AM3 |

## 3. 补齐实现的设计

### 3.1 复用 manager 的位置与发布边界

地址管理分为三个职责，不能出现三个各自分配同一地址域的 allocator：

| 职责 | 唯一负责方 | 实施方式 |
| --- | --- | --- |
| 对象分配意图、引用与生命周期 | mem_service core | 扩展已有 managed allocation，保存中立绑定和操作状态；不解析 GSVA 字段 |
| 全局区间选择和 segment 协调 | 指定 provider 中的 manager 协调角色 | 复用 gva_manager 协商/分发机制；首版静态指定一个协调者，无自动竞争选主；协调者失联时停止新分配 |
| 物理 OBMM 分配、固定导出、导入与页表/路由安装 | 各 home/consumer provider 及 kernel | 使用 upstream libobmm 与本仓库平台适配层；根据权威区间执行，返回真实 kernel descriptor |

将 `gva_manager.c` 中协议机制、segment 操作和错误处理提取为共享平台库；
建议新增目录 `guest-linux/aarch64/libs/gva_manager/`，实施时先写目录 README，
约定公共 API、平台私有实现、测试位置及构建产物目录。原 `linqu_gva_manager`
和常驻 provider 链接同一实现；不复制两份协议、不为每次 allocate 启动 CLI 子进程。
提取限于本目标需要的机制，不进行无关的全文件重构。

通用服务代码留在 mem_service 子模块；OBMM provider 通过显式平台接口链接该库。
独立发布时，OBMM 部署包包含 provider、平台库及必要 UAPI 版本信息；运行时不要求
安装整个 ub_sim 源码仓库。通用 mem_service 构建不能反向依赖 QEMU 或 infer。
复用现有 provider 常驻进程承载 manager session，不增加一个用途重叠的管理 daemon。

provider 必须自动取得分配/回收任务。首版在已有控制连接上，由绑定的 home provider
拉取属于自身 incarnation 的待执行操作；每项携带操作 ID、对象 generation 和意图，
执行结果经已有 publish/reclaim API 确认。现有接口若缺少任务领取能力，只补充
版本化的中立操作投递，不再建设另一套控制通道。重试先查询已有 reservation，
禁止重复分配；provider 无权领取其他 home 的任务。客户端无需手工触发内部确认。

### 3.2 统一对象绑定与地址描述

| 绑定层次 | 必须保存/校验的信息 |
| --- | --- |
| SDK/global object reference | 现有对象身份、分配 generation、合法 offset/length、访问权限；序列化后可由其他节点直接 acquire |
| managed record | key 与现有 object identity 的确定映射、owner session、generation、version、home provider/incarnation、生命周期和 holders |
| provider 私有绑定 | segment ID、真实 home VA/UBA、范围、segment epoch、home CNA、token ID/value、cache policy、p_tag、export 标识 |
| 进程 mapping | 进程/地址空间身份、实际映射 VA、权限、import handle、映射代际、CPU/PTO pins、执行中计数及撤销状态 |

以现有 `lingqu_object_ref_wire` 和 managed allocation 为基础增加绑定适配；不能只按
相同 key 猜测属于同一代对象，也不能让 W5 和独立客户端拥有互不相认的引用。
确需扩展编码时使用明确的版本与长度检查，并测试旧端拒绝未知版本的行为。

V2 引用接入先冻结中立编码：保留 V1 的 64-byte 字段顺序及版本 1 常量，
新增 256-byte V2，在原字段后保存编码长度、访问权限、allocation generation、
home provider incarnation、allocation 逻辑大小、完整 allocation key 和 home ID。
V2 的 payload offset 相对该 allocation，明确区别于 V1 的 arena offset；V1
引用不能通过修改版本号自动升级。key hash 延续既有算法，同时保留完整 key，
禁止只靠 hash 判定身份。跨进程编码逐字段采用 little-endian，不直接发送 C 结构。
未知版本、长度、权限位、非规范字符串和越界必须拒绝；失败不改写调用方输出。

编码/解码仅验证结构和内部一致性，不发起 RPC，不授予 holder，不证明 payload
checksum、当前 content version 或 backing 归属。后续 SDK 必须通过服务端权威
记录核对对象/视图与 allocation 的绑定，再以原 generation 获取 holder 并进入
现有 managed mapping 生命周期；内容发布和重启后的身份域也须由服务端证明。
编码夹具通过不能关闭全局引用绑定、W5 消费者接入、AM2 或 AM3。

服务权威状态先复用现有 record table 保存逻辑 object key 与完整 V2 的显式绑定，
不新增第二套对象目录。写入按 begin/stage/seal 组织：begin 要求 allocation owner
是唯一 holder 且所有映射已解除，推进 content version 并立即拒绝旧引用；stage
可登记多个视图，seal 在写入映射确认清理后发布这一版本。中途失败不恢复旧版本，
放弃该轮写入通过既有退役与回收处理。resolve/acquire 必须同时核对登记视图及
当前 allocation；V2 模式拒绝绕过版本检查的旧 raw acquire/map。core、RPC、SDK
provider 发布/可见性与 W5 逐层接通，只有完整数据链路验证后才能关闭该项。

私有 descriptor 从 home kernel alloc/query 结果生成；完整传播实际 epoch 与 token，
不填固定 epoch，不以 token ID 推导 token value。现有 UAPI 未包含的上下文状态由
provider/平台绑定显式维护；确需跨进程传递时再做版本化 UAPI 扩展。

PTO memref 可以使用该 ISA 定义的地址表达和必要 alias，但必须证明落到同一 home
segment/backing。CPU 指针仍须满足严格 GSVA 等式，PTO alias 不能替代 CPU 同址验收。

### 3.3 分配、映射与发布顺序

1. 基础设施检查参与节点支持的用户 VA 范围、页大小、aperture、cache 属性和路由。
   每个客户端进程在交付指针前保留对应 VA；拒绝冲突，禁止覆盖已有匿名内存或代码。
2. core 记录分配意图和 home provider/incarnation。协调者分配全局不重叠区间，
   home kernel 原子检查并保留该固定范围，home provider 分配实际 OBMM backing。
3. home provider 固定 UBA export，查询真实 descriptor，安装 home 访问所需状态；
   记录可恢复的 reservation 身份后调用现有 backing publish 确认 API。
4. core 提交 ACTIVE。ACTIVE 仅表示 backing 已提交；各 consumer mapping 另有状态。
   客户端 acquire 完成严格 import、页表/路由/权限验证后，才得到可用 CPU 指针或 memref。
5. 写入客户端执行数据 publish；读取客户端执行 acquire/wait-visible。这里的数据发布
   与步骤 3 的“提交分配 descriptor”含义不同，接口和日志必须区分。

区间实现须检查整数溢出、页对齐、请求 alignment、整个范围包含关系和重叠。
首版为每个对象分配独立的整页 segment，简化 CPU 隔离与寿命；页内 suballocation
留待后续独立优化。不同 home 共用全局区间账本，各 home 返回自己的真实 CNA/token。
协调者日志与 home kernel reservation 要能按操作 ID 对账，不能让两方各自推进计数
并把巧合相同的地址视为协调成功。

### 3.4 CPU 与 PTO 的访问、可见性和保护

- CPU SDK mapping 在有效 lease 内交付指针；已安装的页表与模拟硬件处理正常 LDR/STR。
  控制服务仅参与分配、映射建立、发布/获取同步及生命周期，不代理每次数据访问。
- PTO 复用 `lingqu_shmem_mem_service_acquire` 和现有 memref/lease；将其对象来源接到
  上述绑定，提交后 pin 至完成或已确认取消。并发计数和状态迁移必须线程安全。
- 明确 CPU 写入后的 release/publish、PTO 完成后的 publish、读者 acquire 及必要
  cache refresh。采用平台已支持的 cache 属性，不能用全局强刷掩盖属性不一致。
- CPU 跨节点读写、CPU store 后 TLOAD、TSTORE 后 CPU load、TSTORE 后 TLOAD，
  都必须读回同一对象的最新已发布字节。同步操作只携带元数据，不拷贝整个 payload。
- 原始 CPU 指针保护按页执行。SDK/PTO 检查精确字节范围；CPU 越界测试越过对象分配
  页及 guard page。禁止声称普通 LDR/STR 能自动检查同一映射页内每个对象字节边界。
- 只读 mapping 必须同时限制 CPU 页表权限和 PTO descriptor 能力，不能只检查 SDK 参数。

### 3.5 退役、回收与重启

1. retire 使对象拒绝新 acquire。已存在 holder 的 drain 或撤销策略明确可查询。
2. provider 等待 CPU 使用区间结束和 PTO completion；需要强制撤销时，完成进程映射
   失效、相关上下文 fencing 与 TLB 清理。所有参与者给出可核对的完成结果。
3. home provider 释放 import/export/backing，协调者确认区间已无旧使用者，再调用
   reclaim 确认 API。缺少任一确认就维持 RETIRING 或 QUARANTINED，不进入可重用列表。
4. 发布前取消也需要处理。ALLOCATING 对象可能已经存在未确认的远端 reservation；
   不能因 core 尚未收到 publish 就认定资源从未分配。取消与迟到 publish 必须幂等且可对账。
5. 重启先核对 journal、provider reservation、kernel segment 与映射持有者。补足
   可枚举状态或完整分配记录，保留 retired/tombstone 证据；NOT_FOUND 不等于已安全释放。
   provider incarnation 变化不能直接覆盖旧状态；未知资源先隔离，核对后再恢复 readiness。

**原始 CPU 指针没有 generation。** 同一数值 VA 一旦映射新对象，硬件无法仅凭该
指针判断调用者原先想访问哪一代对象。因此，只要旧 CPU 上下文仍可使用该地址，
就不能复用该范围；必要时终止/隔离旧上下文。应用在 release 后继续持有裸指针属于
违反 SDK 寿命约定，不能承诺对这种任意悬空指针实现代际识别。

tokenless replay 按 [source-owned void-response 设计](2026-09-05-ub-void-response-predicate-policy-design.md)
执行。尚可 ERET 重放的旧 LDR 视为未退出访问：保留原对象 pin，或先使旧上下文安全
失效，再允许地址重用。旧 response 必须丢弃并清理，不能填充新代对象；不得为此恢复
software-visible wait_key/CQ 唤醒协议。PTO 显式 completion 单独按其执行协议处理。

## 4. 分阶段实施计划

按 AM1、AM2、AM3、AM4 顺序交付。每阶段都有实际数据访问结果；纯接口声明、
日志标签或 grep 检查不作为阶段完成依据。发现前序缺陷时按失败证据修复，
不把不同阶段的未完成项合并描述为“主体已完成”。

### AM1：两个独立客户端通过服务分配并以 CPU 共享读写

**补齐 G1/G2/G3/G4，并交付 G6/G8/G9/G10 的最小闭环。**

- 接纳并测试当前 managed allocation / provider directory / publish-reclaim 进展；
  核对 daemon 在途修改，列出尚缺字段，避免重写相同能力。
- 提取 manager 共享库，将常驻 provider 的 reserve/map/release 接到真实 manager。
  修改 home kernel 的区间保留检查，接通 fixed-UBA export 与真实 descriptor。
- 接通分配意图、实际 reservation、provider 发布、客户端同址映射及正常释放。
  实现取消与迟到发布处理。未完成重启核对前，重启后拒绝新分配及旧对象重新 acquire，
  保留资源隔离；禁止重启后清空表就重新使用 aperture。
- 在两个 QEMU guest 各启动独立客户端，由 A 分配并写入，B 接收相同引用、同 VA
  读取并校验，再由 B 写回、A 校验。服务必须在客户端启动前已就绪。

**阶段出口：**A01 至 A06 的两节点、单 home 子集通过；A09 先覆盖 CPU 页权限、
guard page、无效引用与畸形 descriptor。没有虚构 backing、人工抄写 descriptor
或按节点手工分配 VA。首阶段可以限制单 home、禁用退役区间重用，这两个限制必须
显示在能力输出中。多 home、轮换及重用负例留给 AM3/AM4，不将子集标成最终验收通过。

### AM2：同一对象接入 TLOAD/TSTORE 并统一访问寿命

**补齐 G2/G5/G6/G10 的 CPU/PTO 联合路径。**

QEMU 接入约定：现有 mapping register 接纳已安装 CPU window 的严格 GSVA V2
route，按实际 guest PA 查询，不从数值相同的旧 SIM_DEC map ID 推测身份。
保留完整 key、route ID、lease epoch/权限、home/owner 及独立 backing token；
PTO 绑定、延迟授权复查及逐次访问均比较同一身份快照，退役或轮换后拒绝旧绑定。
严格 PTO 注册要求显式 opt-in 的 V2 ioctl 标志及新 MMIO command；driver 校验
OBMM region 身份、逐页物理连续性及实际页表权限，与 route 权限取交集。OBMM
会清空 VMA 读写标志；AArch64 可写页也不设置软件 PTE_WRITE 位，须检查实际
user/AP 权限。当前子集只接纳页粒度 O_SYNC import，避免注册后 ownership 更新
使权限快照过期；cacheable 和大页视图明确返回 unsupported。扩展这些视图前须
补齐动态权限寿命协议。旧 driver/QEMU 拒绝新请求，SDK 不降级；
旧注册 command 继续服务 legacy mapping，对严格 GSVA 返回 unsupported。
TLOAD/TSTORE 使用与严格 CPU window 相同的实际 backing I/O；旧异步 read 队列
暂不接纳 GSVA register，返回明确 unsupported，不改变 tokenless CPU replay 协议。
严格 backing I/O 同时承接 CPU 标量及 PTO 张量分片。发送与接收采用同一分片上限，
从现有消息 payload 上限扣除请求/响应中较大的头部；每片完整检查长度和地址溢出。
失败不得改走裸地址或旧 route，跨页和最大合法分片须由实际读写及负例验证。
此约定仅定义实现边界，完成还需下述统一 holder 和实际四种读写组合的验证。

超时恢复先补单 endpoint 的 pending operation 归属：超时保留原 CQ cursor、队列、
metadata 与 compute leases，只等待原 op ID，不再次提交 slot。待完成时拒绝新 submit
及 checked close；错误身份或畸形 completion 保留资源并报告协议错误。现有 CLI
用独立的短 submit 等待和总恢复时限验证迟到 completion，继续保留实际数据及清理
判据。这一增量仍采用外部串行化，多线程 shared owner 单独实现和验收。

统一 owner 在中立 SDK 中实施：同一 provider channel 的多个对象共用 access
domain 锁，接管现有 managed mapping/lifecycle 和 holder，不重复 map 或 acquire。
有界 CPU/compute borrow 计量同一对象的访问寿命，计算 pin 另记原 operation 的
in-flight。close 先停止新接纳，已有访问退出后才 unmap/release；清理失败继续
持有原上下文。线程全部退出后再 destroy 进程内句柄，不能让 concurrent close
与 API 调用竞争 free。raw provider 只在 domain 运行前后使用；数据竞争和跨进程
撤销仍需各自的可见性与恢复协议。实现出口同时要求原生并发 fixture 和真实
双节点 CLI 验证，现有单线程结果不能覆盖它。

只读联合验收沿用 managed CLI，增加显式 `readonly_inputs=0/1`，默认关闭。
开启时输入与输出使用两个真实 managed 对象，各节点分别 acquire/map；consumer
对输入请求 READ，对输出请求 READ|WRITE。OBMM 不支持 mprotect 或 VMA 拆分，
直接创建只读映射；CPU fault probe 在映射所属进程、工作线程启动前完成，核对
实际 fault 地址、同步信号及原数据不变，不用 fork 子进程替代页保护证据。
两个 owner 共用同一 access domain，但 holder、CPU/compute/in-flight pins 和
清理状态各自计数。A/B 从输入对象 TLOAD，C/D 在输出对象 TSTORE，第二次计算
直接 TLOAD 原 C；逐 binding 核对 CPU/PTO 对应对象及 backing，不要求两个
不同对象具有相同 backing。只读输入的 CPU/compute 写借用必须拒绝，既有执行
权限负例及迟到 completion 恢复仍按独立开关验证，两对象全部 drain 后才退役。
r162c 已通过上述只读页表、六次 PTO 拒绝及后续两次正常计算；r163 已通过
双 owner 各自保留 1/2 个计算 pin 的原 completion 恢复与资源归零。r162 的
guard 返回值失败及修复前后原生回归均保留。此子集不关闭多 home、多请求并发
及更大分片的阶段出口，详细来源和边界见实施记录。

大尺寸 managed CLI 现支持 ND `128x128` 和 tail `128x127`，每对象逻辑大小
256 KiB，原 cross-page 保持默认。quadratic 编排按完整 storage extent 申请
中间张量，保持既有算子和四步计算。r176 的 ND 双对象、四线程、两轮完整输出、
只读保护及回收已产生实际证据；其原 runner 被分片解析缺陷判为失败。r174c
校验器从未改动的完整原始日志核实了 102 个分片和全部原判据，独立复核 exit 0；
原失败报告保留。分片计量按 register/unbind 窗口区分启动阶段 legacy I/O，
窗口外访问 managed backing、重放和缺片继续失败。r177b 的大尺寸权限矩阵在
关闭后台 CPU 读取线程后整轮 exit 0：六拒绝、24 次完整视图不变、两次正常计算，
实际核对 170 个分片和资源归零。r181b 使用优化解析器重新校验两轮完整原日志，
全部判据、计数及 SHA 与先前结果一致。r178 的 tail 四线程完成两次原 completion
恢复、144 次完整输入检查、102 个实际分片及资源归零，整轮 exit 0；各 callback
尾片为 144 bytes。r179 默认单对象四线程六负例和 r180 旧 direct 入口回归均
runner/container exit 0，无 QEMU 遗留。完整 AM2 保持未完成。

两节点 home 互换的接入约定：managed CLI 增加 `home_node=0|1`，默认 0。
home 节点负责分配、初始化、核对远端写回和退役，另一节点执行 PTO；实际
node ID、requester CNA 和日志来源保持不变。测试基础设施仍在 node 0 启动
控制服务，通过现有 `allocation_home_provider` 选择实际 home worker，两个
worker 保持运行到 runner 收到双方完成。CPU-only 与 legacy direct 不改变。
CLI 必须核对服务返回的 home provider；验收器分别识别角色与物理节点，选择
实际计算节点的 QEMU 日志，并核对 home 端导出及远端 backing 身份。不得改写
原始日志中的节点编号。两方向均须通过数据、权限、原 completion 和资源判据；
此项不替代同时多 home 协调或多请求并发验收。

guest 启动参数中的 home 选项使用 `lingqu_shmem_pto.home_node` 命名空间，
由 `run_app` 从 `/proc/cmdline` 读取；host 配置和 managed CLI 字段保持不变。
带点的应用参数不占用 Linux 未知参数转交 init 时的环境槽位。r186b 诊断
暴露原下划线写法超出 `CONFIG_INIT_ENV_ARG_LIMIT=32`，在服务启动前 panic；
修复须覆盖 guest 解析与环境槽位回归，不调整 kernel 限额或放宽验收判据。

多请求接入先补 endpoint 的有界 `enqueue/poll` 接口：一个 endpoint 独占一套
CMDQ/CQ，多个请求各自保留原 op ID、metadata 和 compute leases。队列容量取
CMDQ/CQ 可用深度及本地记录上限的最小值；CQ 可批量到达，按 op ID 分发，
调用者可按任意顺序领取完成。重复、未知或畸形 completion 使 endpoint 保持
协议故障并保留剩余上下文，不能确认或释放无法证明已完成的请求。新接口要求
提交 ID 单调递增以拒绝 endpoint 寿命内重用；拒绝不消耗 ID。旧同步接口及其
单 pending 超时恢复合同保持不变，两类接口有在途请求时不能混用。短队列操作
仍由调用层串行化，等待期间不持有阻塞其他请求提交的锁；close 要求所有已提交
请求的 completion 已被领取，并由调用层保证访问线程均退出。

实际计算验收在 managed CLI 增加独立多请求模式，默认保留原 C→D 顺序依赖。
多请求模式使用相同 A/B 及不重叠的 C/D 输出视图，两个请求分别执行原 quadratic
算子，oracle 明确按独立请求计算；禁止在缺少依赖协议时并发提交原 C→D 链。
两个提交线程共用 SDK owner/access domain，各有独立 metadata、lease 集合和
原 operation；在允许领取完成前核对两个在途请求及全部 pins。原四种读写组合、
只读/负例、迟到恢复与两方向 home 验证继续保留。队列并发接纳和设备并行执行
分别计量；接口单测或单一两请求子集不关闭完整 AM2 的多请求出口。

本轮 CLI 配置新增 `concurrent_requests=1|2`，默认 1；host 使用
`LINGQU_SHMEM_PTO_CONCURRENT_REQUESTS`，guest 参数使用带点命名空间
`lingqu_shmem_pto.concurrent_requests`。模式 2 同时适用于单对象及只读双对象，
两个请求共用一个 compute/backend context，各自持有独立 metadata 页及三份 lease；
owner 的容量预留六个计算借用
与所有 CPU reader。两个请求完成全部借用及提交后，主线程检查六个在途 pin、
两份不同 metadata、endpoint close 拒绝和 owner close/admission 拒绝，再允许
两个调用线程领取完成。所有线程加入且共享 context 的六份 lease 全部释放后，
才释放请求状态，随后执行原 owner drain；共享 context 由主线程统一关闭。
准备、提交或完成失败须唤醒其余等待线程并保留未知完成请求的全部资源。
现有六负例及短等待恢复仍用默认顺序模式；模式 2 与这两种选项组合暂时拒绝，
相关多请求故障恢复出口继续保留。验收按原请求关联输出、回调和完成，允许跨请求
乱序；必须检查独立模式的 D oracle、输出不重叠及真实数据 backing，不能只检查
新增日志标记。设备是否并行执行保留独立判断，不由两个软件请求被持有推导。

r204 的首次真实双请求运行在提交前返回 `-EBUSY`，未产生队列提交记录。
驱动 `linqu_ub_open` 对 `active_file` 实施独占，原先每请求打开 backend 的设计
与该限制冲突。修订为上述共享会话；本路径只使用 strict mapping 注册/注销，
调用经共享 owner/access domain 串行化，runtime 的 fd 在所有请求退出前保持有效。
不扩展此结论到会修改 SQ/CQ 或 future 状态的通用 async API。r204 保留为失败证据，
修订后的运行须重新验证六份 lease、两页 metadata、完成关联及资源归零。

- 将现有 Memory Service/PTO acquire 接到 managed 对象绑定，保留现有 W5 引用兼容。
  两种访问方式共用 holder；CPU mapping 与 PTO 操作各自贡献可观察的 pin/in-flight。
- 完成 CPU/PTO 双向写读及 PTO/PTO 写读的 publish/acquire，核对真实指令和 backing。
- 实现并测试只读、越界、映射冲突、并发请求、PTO 超时仍有未完成访问等行为。
  指针/memref 交付后不允许通过临时 buffer copy 补全正常数据路径。

**阶段出口：**A07/A08/A10、A09 的 PTO 权限/范围子集及 A11 的在用资源拒绝回收
用例通过；四种数据访问组合全部有端到端证据。动态身份轮换与完整重用由 AM3 验收。
现有独立 CPU GSVA/PTO 验证继续通过。

### AM3：安全重用、失效与故障恢复

**补齐 G3/G5/G7/G8/G10 的完整寿命保证。**

- 实现全局区间释放/重用、真实 segment 代际和 token 轮换；对象 generation
  与 segment epoch 分别核对，旧引用不得重新取得新代对象。
- 接通进程 mapping 撤销、CPU/PTO drain、ACK、TLB 清理、reclaim 确认及 quarantine。
  retire 超时只改变可诊断状态，不允许改变资源为“已安全释放”。
- 扩展现有 journal 并执行故障注入：意图写入后、物理分配后、export 后、publish
  回包丢失、最后 holder 释放后、实际释放后确认丢失，以及分配中取消。
- 核对控制服务和 provider 独立重启、客户端崩溃、迟到旧 incarnation 操作。
  自动恢复无法证明安全时，给出有 CLI 的 reconcile/quarantine 查询与回收操作。
- 验证 tokenless replay 与地址重用相遇时的旧上下文隔离；保留 source-owned
  void-response 路径，不引入 token/CQ 等候替代协议。

**阶段出口：**A11 至 A15 通过；旧 context 尚能重放时地址绝不被分配给新对象。
退出或确认回收后资源计数回到基线；未知状态明确计入 quarantine，不能从统计中消失。

### AM4：多 home、拓扑扩展、独立部署与 W5 消费者回归

**关闭 G9，并完成其余缺口的集成验收。**

- 用相同 SDK/对象格式运行 2/3/8-node。各节点轮流作为 home，校验不同物理 home
  共享全局区间协调；8-node 不能只验证 nodeA 与 nodeB。
- 在支持的进程/holder 数范围内施加并发与容量压力。当前 holder 上限为 8，
  八节点以上客户端若需要扩容须版本化；本轮至少验证达到上限后确定拒绝且无泄漏。
- 独立发布包在目标机无 infer/模型进程时启动、分配、跨节点访问、释放和诊断；
  客户端断开后服务继续运行。通用包不携带模拟器源码依赖。
- W5 Qwen3 与既有 2-bit GGUF DeepSeek V4 Flash 仅作为 SDK 消费者做回归；
  保持既有真实算子执行路径，不引入 DS4 动态库或 synthetic 结果替代。
- 补齐部署说明、用户 CLI、能力/错误输出、源代码索引与测试结果；随后才申请发布。

**阶段出口：**A01 至 A18 全部满足，完成记录中没有未解释的失败或 skip。

## 5. CLI、测试与执行方式

### 实施依赖与停止条件

| 顺序 | 可独立核对的交付 | 前置依赖 | 停止扩大范围的条件 |
| --- | --- | --- | --- |
| 1 | 共享 manager 平台接口与原 CLI 回归 | 明确地址域、唯一协调者及平台/服务边界 | 原有 bootstrap 或区间操作回归失败 |
| 2 | 基础设施启动常驻 provider，自动 bootstrap 并上报真实 readiness | 共享接口、实际 aperture 与路由探测 | 仍需 infer 初始化，或需要手工预注册可用 provider |
| 3 | SDK 分配自动产生真实 reservation、固定导出和对象绑定 | 常驻 provider、任务获取、持久意图与结果 | 需要人工填写 descriptor，或结果未知的资源无法追踪 |
| 4 | 两个独立 guest 客户端同引用、同 VA 双向读写 | 严格映射、权限检查、发布/获取同步 | 无实际 CPU payload 证据；此时不扩大 W5 或多拓扑矩阵 |
| 5 | 同对象 CPU/PTO 四种访问组合与统一 pin | AM1 数据闭环 | 两条路径 backing 身份不一致，或 PTO 完成前可能释放 |
| 6 | 退役、上下文退出、重启对账及安全重用 | 统一访问寿命和真实资源统计 | 无法证明旧访问退出；保留隔离，禁止重用 |
| 7 | 多 home、2/3/8-node、独立包和 W5 回归 | AM1-AM3 对应出口通过 | 前序安全或数据一致性失败；先修最小复现 |

每项交付同时包含实现、可调用 CLI、对应测试和日志。共享库编译、descriptor
编解码测试、使用替身的事务测试只确认局部行为；AM1 出口必须包含真实服务、kernel
和两个客户端共同参与的数据读写。已有 aperture 或手工注册的 provider 可以用于
开发定位，但不能据此关闭独立 bootstrap 验收项。

不按运行轮数承诺完成时间。每阶段以出口证据决定进入下一阶段；遇到硬件或
接口阻塞时记录具体失败、影响的验收编号和最小补齐动作，禁止持续扩大辅助代码
而不推进下一项可演示的数据能力。

### 5.1 用户入口和测试入口

| 用途 | 复用或补齐位置 | 要求 |
| --- | --- | --- |
| 分配、acquire/release/retire、查询、资源统计 | [现有 mem_service CLI](../../mem_service/apps/mem_service/mem_service.c) | 复用 `allocate-object`、`inspect-allocation`、`allocation-stats` 等操作；provider 发布/回收由服务自动完成，用户不手写 descriptor hex |
| 独立启动和 provider 管理 | mem_service 现有服务/provider 入口与部署配置 | 补 bootstrap、provider/地址管理状态、reconcile/quarantine 操作；不得依赖 infer 命令初始化 |
| CPU/PTO 同对象读写 | 现有 guest 验证 app 与 `lingqu_shmem_pto` 测试 | 补可通过命令行选择的场景，进程内保留 mapping 到验证结束；引用可跨进程传递，payload 不借助共享文件传递 |
| GSVA 机制回归 | `run_ub_two_node_gsva_arm_mmu_acceptance.sh`、four/eight-node 同类入口 | 验证页表、路由、上下文和 kernel 生命周期；继续保留原机制覆盖 |
| provider/PTO 联合回归 | `run_ub_dual_node_mem_service_obmm_provider_conformance.sh`、`run_ub_dual_node_lingqu_shmem_pto_direct.sh`、eight-node PTO 入口 | 扩展为真实 managed 对象场景；不为每个负例新增一支 launcher |
| W5 消费者回归 | [run_w5_cluster_config.sh](../../guest-linux/aarch64/scripts/run_w5_cluster_config.sh) | 同一入口与配置文件选择拓扑/模型；不再追加独立 bootstrap 环境变量链 |

新增 CLI 能力随对应阶段实现，本文没有声称这些补齐项已经可调用。CLI 参数解析、
错误码、帮助和 API 行为同步测试。启动配置放入现有配置文件；日志/产物进入
`logs/`、`out/`，不修改或提交生成物充当源代码修复。

### 5.2 固定验证矩阵

| 矩阵 | 规模与通过要求 |
| --- | --- |
| 最小功能 | 2-node；两个独立进程，同一 global reference，双向 CPU 读写；AM1 首先完成 |
| 四种访问组合 | CPU/CPU、CPU/TLOAD、TSTORE/CPU、TSTORE/TLOAD；两节点互换 home，使用确定性字节模式逐字节比对 |
| 全局地址/多 home | 2/3/8-node；每个节点至少作为一次 home，每个 home 对象被所有其他节点访问；每种组合至少 10 次发布/获取 |
| 分配压力 | 整页及不同多页 size、较大 alignment、交错释放、100 次分配/释放、并发申请、容量耗尽；无重叠，失败不改变已有对象 |
| 生命周期故障 | 在 2-node 执行 AM3 所列全部故障点；在 8-node 复核 holder 延迟/死亡和 reclaim 阻塞；包含有 CPU replay 和 PTO in-flight 的回收 |
| 重复性 | 独立服务数据矩阵执行 3 次冷启动；每次归档配置与状态，全部通过，不把重跑覆盖前次失败 |
| W5 | Qwen3 与既有 2-bit DeepSeek，各执行 2/3/8-node 的 4-step，另执行两模型 8-node 的 8-step；直接使用原样 `Huawei is`，不拼接 prompt |
| 既有回归 | 2/4/8-node GSVA、OBMM provider conformance、PTO、终端 token 可见性、相关完整单测/集成套件 |

W5 的期望步数、range forwards、terminal tokens 和数值校验沿用已验证模型路径的
判定方式，运行前固定对应基线与容差。缺少模型文件时记录该验收项阻塞，
不得擅自下载、换权重或把几何 smoke 标成推理通过。

### 5.3 运行环境和成本控制

- 主验证在 `n4-910c1` 的 `/home/ll` 工作区；最终在 `n4-910c` 独立复现。
  可容器化的服务/单测在容器中执行；真实 mapping 用远端 QEMU guest，宿主不散装依赖。
- 本地 macOS 仅做轻量文档、静态和已知不加载模型的定向检查。
  每阶段改动后运行完整相关测试套件，资源较重或行为不明的套件安排远端。
- 先跑最小失败用例，修复后再扩大矩阵。相同失败不重复提交整套 W5；
  每次运行设置进度与总时限，停在明确失败点并保存日志，避免无界轮询。
- 每个里程碑报告已通过的验收编号、剩余编号、失败根因和下一项实际数据能力，
  不按文件数、测试条数或耗时估算目标达成度。

## 6. 验收条件与标准

**判定规则：以下是最终硬条件。源码存在、控制 RPC 返回成功、单独组件通过，
均不足以替代跨节点数据路径验收。所有结果必须绑定同一套可复现的源码和配置。**

| 编号 | 验收项 | 通过标准与必须保留的证据 |
| --- | --- | --- |
| A01 | 独立基础设施 | 不启动 infer 即完成 bootstrap 与两个客户端共享对象；客户端退出后新客户端仍可使用服务；保存进程和启动日志 |
| A02 | readiness 真实性 | 无 provider、aperture/路由/canary 失败时 `data_plane_ready=0`；不能仅凭 directory 注册或 core ready 返回可用指针 |
| A03 | 同一全局引用 | 同一序列化引用在各节点 acquire 成功；object identity/generation、home segment、backing 标识一致，节点不另分配 payload |
| A04 | 严格同址 | 参与进程实际 CPU VA、UBA、home VA 及对象 offset 等式全部成立；记录映射和路由证据；VA 冲突不能降级或覆盖已有内存 |
| A05 | 区间分配正确 | 不同 size/alignment、并发、多 home、耗尽测试无重叠/溢出；幂等重试不重复分配；隔离区不进入 free list |
| A06 | CPU 真实直访 | guest 执行 LDR/STR，有反汇编和地址路径证据；双向逐字节正确；正常循环无逐 load/store 控制 RPC，无 payload 中转 |
| A07 | PTO 真实直访 | TLOAD/TSTORE 经既有 dispatch 和真实后端执行；追踪 memref 所指 backing 与 CPU 对象一致，不以 host memcpy 或新 buffer 代替 |
| A08 | 交叉可见性 | 四种访问组合逐字节一致，写入版本和发布/获取顺序可核对；平台要求的同步明确，控制服务 payload 字节计数为零 |
| A09 | 权限与边界 | 只读写入、错误 generation/epoch/token/incarnation、越界 descriptor、CPU guard-page 访问均按规范拒绝或 fault；相邻合法对象不受影响 |
| A10 | 统一引用寿命 | 多请求/线程引用计数准确，CPU mapping 或 PTO in-flight 存在时不能提前回收；上限拒绝不泄漏，PTO 超时不伪造 completion |
| A11 | 退役和重用 | retire 后新 acquire 被拒绝；全 holder drain/撤销和 TLB 清理后才回收；同 VA 重用的新对象具有新 generation，旧引用不可 acquire |
| A12 | CPU replay 与 ABA | 旧上下文仍可 ERET 重放时重用申请被阻止，或上下文先被安全隔离；迟到旧响应不填充新代对象，无 software wait_key 替代路径 |
| A13 | 分配中断 | reservation 前后、publish 丢包与取消竞态均能重试/核对；已分配资源不因 ALLOCATING 被删除而丢失追踪 |
| A14 | 重启和未知状态 | daemon/provider/客户端故障后安全恢复或明确隔离；旧 incarnation 请求拒绝；只有实际重新验证的 provider 恢复 data readiness |
| A15 | 实际资源回收 | 正常循环后实际 backing 字节、保留 VA 字节、export/import、holder、in-flight 回到基线；未知资源计入 quarantine，不能用 ACTIVE 数量冒充实际映射计数 |
| A16 | 拓扑和 home 无硬绑定 | 2/3/8-node 按矩阵全通过，每节点作为 home；8-node 覆盖全部有向节点对，三轮冷启动无数据错误；4-node 原有机制无回归 |
| A17 | W5 消费者完整运行 | 指定模型/权重、原样 prompt、4/8-step 矩阵达到期望输出和退出状态；infer 不自行初始化 manager，不直接配置 transport，不触发隐式替代路径 |
| A18 | 独立发布与复现 | CLI 和完整相关测试通过；部署包无需 infer/模型/源码仓库即可提供服务；记录所有 SHA/lock/配置/日志，两台远端均可复现关键验收 |

最终验收记录至少包含：源码完整 SHA 与 gitlink/lock、构建及包版本、配置摘要、
run ID、拓扑/home 分布、object/generation/segment 标识、实际 VA/UBA、发布版本、
数据比对、实际资源前后计数、失败注入点与恢复状态、命令退出码、日志位置。
token value 等敏感授权材料不直接写入可发布报告，使用脱敏标识。

A18 的跨机最小复现集合为 A01 至 A04、四种访问组合、一次重用/旧引用拒绝、
一次 provider 重启隔离，以及两模型的 8-node 4-step。其余完整矩阵可在主验证机
执行，但两机必须使用同一发布组合。时间和吞吐可以记录，未设性能收益通过门槛。

## 7. 追踪与提交顺序

| 阶段 | 当前状态 | 下一项可演示结果 | 完成记录 |
| --- | --- | --- | --- |
| AM1 | 通过 | 两节点、单 home 的 A01-A06 与 CPU A09 子集已满足；多 home、拓扑、冷启动和独立发布保留在 AM4/A16/A18 | [实施记录](../2026-09-11-gva-managed-service-implementation-progress.md)：r306 百个独立 session；r19-r20/r30 readiness fail-closed；r121/r118/r297 区间、并发与容量；r126/r124/r321/r381 冲突、descriptor、旧引用拒绝 |
| AM2 | 进行中：四种读写组合、四线程 owner、只读双对象、大尺寸、home 互换及共享会话双请求子集通过；完整阶段未完成 | 多请求故障恢复、现有全局对象引用绑定、tail 反向 home 及设备并行性独立验收 | [实施记录](../2026-09-11-gva-managed-service-implementation-progress.md)：r176/r177b/r178 与 r186c/r191-r194 的既有证据保留；r207/r208 的双方向 cross-page、r209 的反向 ND、r210 的原方向 tail 双请求整轮通过，大尺寸两轮各核对 102 个分片；r204 的重复打开会话失败及既有启动/解析失败原记录保留；A07-A11 整项仍未关闭 |
| AM3 | 未完成 | drain 后重用、旧引用拒绝、崩溃隔离及 replay 安全 | 待填写 A11-A15 结果 |
| AM4 | 未完成 | 多 home、多拓扑、独立部署与 W5 全部通过 | 待填写 A01-A18 汇总 |

### 2026-09-09 首次实现记录

- [共享库](../../guest-linux/aarch64/libs/gva_manager/README.md) 已承接 aperture
  register/query 和 segment alloc/query/retire，现有 manager CLI 调用同一实现。
  库检查真实返回的身份、对齐和退役完成状态，失败时保留 command 供后续核对。
- 已更新应用 Makefile 与 initramfs 链接/输入签名；UB 公共头和标准 Linux 头分开
  选择，避免 openEuler 的旧系统 OBMM 头覆盖项目扩展。
- `n4-910c1` 的 `/home/ll/gva-manager-integration` 中，以
  `openeuler-2403:v0.0.5` 容器执行 `test_gva_manager_library.py`，3 项通过。
  本地 AArch64 交叉编译两项通过；原 manager 入口检查两项、脚本语法检查通过。
- 这些是编译与 ioctl 边界单测，未运行真实 `/dev/obmm` 数据访问；完整项目回归、
  两 guest 数据闭环及 A01-A18 均未据此通过。跨节点 bootstrap/ACK 尚未提取，
  常驻 provider 尚未调用该库，kernel 区间分配缺口仍在。
- 本轮未修改 mem_service 子模块及其并行改动，未 commit/push。

### 2026-09-09 内核区间分配修订

- `obmm_core.c` 改用 Linux resource 区间管理，检查 alignment、固定地址冲突、
  范围与耗尽；地址选择和登记由同一锁域保护，取消 counter/size 取模分配。
- aperture 有活动 segment 时拒绝 clear；注册拒绝改变既有节点身份，冲突失败
  不清除原保留状态。kernel retire 释放其区间登记，跨 holder drain 仍待 AM3。
- 原 manager CLI 新增 `--check-allocator`，包含 11 项实际 ioctl 检查；原两节点
  segment CLI 入口已调用它，并修正为等待完整结束，避免仅看到 alloc 就停止 guest。
- 在 `n4-910c1` 的独立目录
  `/home/ll/gva-manager-integration/kernel-validation` 复制源码及构建缓存，
  将副本挂载到容器内原构建路径以复用缓存；原验证源码/产物未覆盖。
  副本以 `f3f7aa9` 加当前 `85fb35d` 的 UAPI 差量和本次 `obmm_core.c` 修订组成。
- openEuler 容器内 `drivers/ub/obmm/obmm_core.o` 编译通过；缓存配置因工具链
  差异重新配置，随后 `olddefconfig` 返回 `No change to .config`。
  manager 定向测试 3/3 再次通过，入口与脚本语法检查通过。
- **未运行 11 项真实 guest 检查，未完成 A05 或 AM1。** 下一次首先构建 Image，
  使用更新后的 CLI/run_app 运行原两节点 segment CLI 测试，归档配置及完整日志；
  随后继续常驻 provider 与实际 backing 连接。尚未进行完整项目回归或发布。

### 2026-09-09 两节点真实 kernel allocator 验证

- 上节待构建的完整 Image 已在同一隔离容器构建中成功生成，退出码 0。
  本地及远端副本的 `obmm_core.c` SHA-256 相同：
  `89e298e0840ab0d743a57bae798aaeed2e27182d1531bf81d47642beaa0cd40e`。
- 首次运行 `gva-manager-allocator-20260909-r1` 在写 PID 文件时失败：runner
  使用外部内核/initramfs 后没有创建自身 `out/`。已修复源脚本，补充使用假 QEMU
  的目录初始化/失败清理单测；未用手工创建远端目录代替源代码修复。
- 重跑 `gva-manager-allocator-20260909-r2` 使用原两节点 segment CLI 入口及
  `out/gva-manager-integration/allocator.env`，在 `ub-sim-gva-test:20260909`
  容器中通过。QEMU 使用原验证二进制，并只读挂载其所需的宿主 `liburing.so.1`；
  未安装宿主依赖，未覆盖原验证仓库。
- nodeA、nodeB 均打印 `result=done action=gsva-allocator-check checks=11`
  和 `linqu_gva_manager segment_cli done`；runner 打印
  `PASS: both nodes completed`。容器 `status=exited exit=0 pid=0`。
  两份 guest 日志未发现 `result=fail`、kernel panic 或 call trace；按本运行
  独立路径查询无残留 QEMU。
- 远端定向测试 `test_gva_manager_library.py` 为 6/6 通过；本地 5 项通过，
  Linux 原生 ioctl 单测跳过。本次没有运行全项目测试或模型回归。
- 完整记录位于 `n4-910c1:/home/ll/gva-manager-integration/`：
  `kernel-build.log`、`out/gva-manager-integration/allocator.env` 及
  `guest-linux/aarch64/logs/gva-manager-allocator-20260909-r2/` 下的
  `control.log`、`nodeA_guest.log`、`nodeB_guest.log` 和两份 QEMU 日志。

| 验证产物 | SHA-256 |
| --- | --- |
| Image | `bf72b2e8eb0252435a55068dfc683d1521f971564a3d0c04073c37aae1de3cbf` |
| kernel .config | `561c251f3df771546e9d9ecfddeacad69a2fd4f0e819bbb95c4dcaf12d867f97` |
| allocator.cpio.gz | `2cb4a4ff64258aa472dd01600cd48f29ddc55082216a13aee695592abe748502` |
| linqu_gva_manager | `3098257b671142590c3a2e64305637f394658a2b950df38caabf85d097fb9477` |

**验收边界：** 两节点各自执行真实内核区间检查，没有进行跨节点 payload 访问。
A05 的并发、全局多 home 协调、幂等和隔离区仍缺证据；A05 整项及 AM1 均保持
未完成。下一项为常驻 provider 接入共享 manager、实际 fixed-UBA backing 发布和
两个独立客户端的 CPU 数据闭环。未 commit/push，也未变更根仓库 gitlink/lock。

### 2026-09-09 provider 待办查询接入

- mem_service 新增 `poll-allocation` CLI、
  `mem_service_client_poll_allocation()` 和 wire operation `0x7c`。
  查询由现有 daemon 控制连接承载，按绑定的 node/incarnation 筛选，按 generation
  扫描 ALLOCATING 以及零 holder 的 RETIRING 对象。查询不改变状态，不授予独占
  执行权；worker 必须用 key/generation 核对 reservation，并通过现有 publish/reclaim
  确认。无待办返回 NOT_FOUND，扫描结束后从零重新扫描。
- 查询校验目录中的活动 incarnation；新增路径明确拒绝被截断的节点名、负数、
  零 incarnation 和数值溢出。该身份检查沿用可信控制端点边界，不宣称提供身份认证。
- 在 `n4-910c1` 的隔离源码目录 `service-poll` 中，真实 daemon/client 测试
  `test_mem_service_provider_backed_allocation.py` 第三轮 14/14 通过，116.496 秒。
  首轮暴露新增断言误用 `holder_count`（实际字段为 `live_refs`）；第二轮暴露
  过长字段被公共读取函数截断。已修复断言，并在新查询路径增加严格解析；两轮失败
  容器日志保留。最终容器为 `gva-service-poll-20260909-r3`，日志为
  `/home/ll/gva-manager-integration/service-poll-r3.log`。
- wire-schema 清单由远端构建的 CLI 生成，37 个操作、210 个字段；SDK 消费说明同步。
  保留 CLI 文件原有并行修改。未执行全项目测试，未提交、发布或更新 gitlink/lock。
- **仍未闭环：** 常驻 OBMM provider 尚未轮询并执行真实 GSVA reservation/backing
  创建；两个客户端同址 CPU 数据访问仍缺证据。AM1 及最终 A01-A18 均未因此通过。

### 2026-09-09 共享 fixed-UBA backing 导出与两节点 import

- 共享平台库新增 `gva_manager_export_segment()`，导出前 query 并核对 segment
  身份、范围、epoch、token、权限及策略，随后执行 fixed-UBA export，保留真实
  `obmm_cmd_export`。原 manager 已调用该实现。失败输出保留用于核对，查询与
  export 尚未合为原子操作，调用方仍须串行化该 segment 生命周期。
- 真实验证在 n4-910c1 的同一隔离环境执行，使用上一轮 Image 和更新后的 manager。
  复用 `run_ub_dual_node_gsva_manager_bootstrap.sh`，补配置文件参数和 out 目录
  初始化；配置为 `out/gva-manager-integration/backing.env`。
- run `gva-manager-backing-20260909-r1` 通过，退出码 0。nodeA 导出 4 MiB，
  `segment_id=0xc4c2000000000001`、`home_va=0x700000000000`、export mem ID 3；
  nodeB 导入同一 segment/VA。QEMU 记录 `map_id=2`、同一 segment、size `0x400000`
  及 CPU window PA `0x40000400000`。GSVA token ID 2 与 OBMM export token 97
  分别来自真实返回值，两者不能互相替换。
- 两份 guest 日志未发现失败或内核异常；按隔离运行路径查询无残留 QEMU。
  远端定向测试 7/7 通过，本地 6 项通过、Linux 原生执行项跳过。
  单测覆盖 stale segment 拒绝导出、错误返回 UBA、失败后保留 export ID，以及
  配置参数拒绝；真实运行覆盖 bootstrap/export/import。
- 日志根目录仍为 `/home/ll/gva-manager-integration`：`backing-r1.log` 和
  `guest-linux/aarch64/logs/gva-manager-backing-20260909-r1/`。
  `backing.cpio.gz` SHA-256 为
  `7766ee3a4bd659673c7ba0ff8a07b417403819467da3987576e2bbe26c6f742c`，
  manager SHA-256 为
  `3e43ee8cb6285c7381ee138eef946b1d1730d9b9a96cd0d0aa14706a460b8f86`。
- **边界：** 本运行沿用原验证模式保留 backing/import 到 guest 结束，未验证安全
  回收或 payload 读写；mem_service 常驻 provider 尚未调用此库执行待办。
  A01-A18 及 AM1 仍未完成，未运行全项目回归，未提交或发布。

### 2026-09-09 OBMM provider GSVA 描述符与映射接入

- provider 新增 96-byte v2 opaque descriptor，保留 export mem ID/token 与独立
  GSVA segment ID/epoch/token、home 地址、节点身份、cache policy 和权限。
  `mem_service_provider_obmm_encode_gsva()` 从真实 kernel descriptor/export 返回值
  构造编码；core 不解析这些字段。普通 OBMM v1 编码继续独立支持。
- v2 映射分支使用 GSVA import，CPU mmap 使用 MAP_GSVA 与
  MAP_FIXED_NOREPLACE，按请求设置页面读写保护，地址或权限冲突拒绝。
  验证 descriptor 大小不超过配置的 import window。旧 bootstrap metadata
  交换只支持 v1，收到 v2 明确失败，避免丢失身份字段。
- 本地和 n4-910c1 Linux 容器的 `test_mem_service_obmm_provider.py` 均为 6/6
  通过。覆盖 v2 编解码、损坏与身份差异、native 编码 API、不可用设备状态，
  并编译 Linux provider/conformance 程序；本轮没有在 guest 执行 v2 payload 访问。
  修正测试编译入口：macOS protocol fixture 不链接 Linux-only libobmm，Linux
  静态链接补 pthread。未修改 vendor/libobmm 源码。
- 远端容器 `gva-provider-v2-20260909-r1`，日志
  `/home/ll/gva-manager-integration/provider-v2-r1.log`，运行 5.089 秒。
- **未验收：** 实际 v2 CPU 映射/权限、跨节点 payload，以及常驻 worker 自动
  分配/发布仍缺 guest 运行证据；A01-A18/AM1 均保持未完成。源码映射分支存在
  与编译通过不能代替该证据。未全量回归、提交、发布或更新 gitlink/lock。

推荐按可验证能力提交：manager/provider 加两节点 CPU；PTO 联合访问；回收恢复；
多拓扑与发布。kernel 修复单独形成对应子模块提交，主仓库不靠未提交子模块运行。
每个变更范围都须完成测试。用户授权发布后，先发布经过验证的子模块 commit，
确认可从远端取得，再同步根仓库 gitlink 与 `mem_service.lock`；本文不授权 commit/push。

**最近一步只做 AM1 的实际数据闭环。** 地址库提取、接口和状态记录服务于这个结果；
完成该演示后再进入 CPU/PTO 联合寿命与故障矩阵，避免长期停留在接口建设。
