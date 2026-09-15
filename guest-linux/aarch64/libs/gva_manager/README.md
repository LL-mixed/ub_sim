# GVA Manager Platform Library

本目录承载现有 `linqu_gva_manager` 与 OBMM provider 共用的地址管理实现。
公共头使用 kernel GSVA UAPI；服务 core 不应包含此目录的头文件。

## 目录约定

- `gva_manager.h`：平台 API；借用调用方的 OBMM fd，不关闭 fd。
- `gva_manager.c`：aperture 与 segment 的 kernel 操作及结果验证。
- 测试位于 `../../tests/gva_manager_golden.c` 和
  `../../tests/test_gva_manager_library.py`。
- 构建产物放在调用方的 `out/` 或临时测试目录；本目录不保存二进制。

除下文 checked export 的确认无 backing 结果返回 `1` 外，API 返回 `0` 或 `-1`，
失败时设置 `errno`。分配失败时保留 command 中的 kernel
返回信息：调用方必须核对可能已创建的 reservation，不能因结果校验失败
就推断资源不存在。retire 成功仅证明 kernel registry 确认，跨 holder drain、
映射撤销和实际 backing 回收仍由 provider 的生命周期管理负责。

区间分配的一个明确例外是 `gva_manager_allocate_segment()` 直接转发
ALLOC_SEGMENT ioctl 的 ENOSPC：匹配内核只在尚未保留区间时返回该错误，
输出 descriptor 不改变。调用方必须使用零初始化的 descriptor 并核对其
仍全零，才能记录本次确认无 segment；不能把此契约推广到其他 errno 或
其他 ioctl。成功分配后的 copy_to_user 失败返回 EFAULT，仍可能拥有资源。
requested_home_va=0 请求内核选择空闲区间；结果照常检查大小、对齐和身份。

`gva_manager_unexport_segment` 只接受已经核对的 segment/export 绑定，先查询
真实 segment，再执行 unexport；调用方必须先确认所有 holder 已解除映射。
它不自动退役 segment，调用方记录 unexport 成功后再单独执行 retire。任一步失败
均保留原始资源身份供核对；本接口不提供跨节点强制撤销保证。

当前提取范围为 kernel 操作；原有跨节点 bootstrap/ACK 协议仍在应用中。
本库不宣称已完成常驻 provider、全局区间协调或 mem_service 联合验收。

`gva_manager_export_segment` 在 fixed-UBA export 前查询并核对完整 segment
身份，返回真实 `obmm_cmd_export`。GSVA token 与 OBMM export token 分别保留。
失败时输出仍保留用于资源核对；禁止仅凭函数失败就退役 segment 或假定 backing
不存在。查询与 export 是两个 ioctl，调用方仍须串行化本 segment 的生命周期，
此 API 不提供跨进程原子绑定或跨 holder drain 保证。

## 可确认的 backing 分配结果

`gva_manager_export_segment_checked` 使用独立的、固定宽度 GSVA export v1
ioctl，携带完整 segment 身份。返回 0 表示成功，返回 1 表示内核显式确认
backing 未建立，返回 -1 表示结果无法确认；后两种结果同时设置 errno。
普通 export 的 errno 不参与“无 backing”推断，旧内核拒绝新 ioctl 后不回退。
输出丢失、未知版本、身份或结果矛盾均保留 reservation 等待核对。

内核仅在物理池/sgtable 分配失败且其临时内存已归还、尚未建立设备映射时报告
NO_BACKING。export 执行中及已建立的受管理 export 会阻止 segment retire；
unexport 成功后解除绑定，worker 才能退役 segment。未知的 export 结果继续
阻止重用。本接口的实际验收需要匹配的新 guest kernel 与 worker；当前开发改动
通过前不得宣称物理池耗尽或崩溃恢复验收完成。

## 内核分配检查

现有 CLI 新增 `--check-allocator --node-id N --node-count C`，在专用、空闲
aperture 中直接调用 ioctl，检查超大分配、不同大小/对齐、固定区间冲突和
aperture 状态保护；结束时只退役自己成功申请的 segment。不要在承载业务的
aperture 上执行此诊断。

两节点 segment CLI 验证入口已包含该检查，并等待两个节点的全部检查结束。
预期成功标记为 `result=done action=gsva-allocator-check checks=11`。
此入口需要实际运行修订后的 guest kernel；编译或 ioctl mock 单测不能替代它。

## 实际 export 身份检查

`linqu_gva_manager --check-export-identity --segment-id ID` 复用现有 CLI，
先 query 实际活动 segment，再逐项改变请求副本的 14 个 descriptor 字段及三个
ioctl 外层字段。负例直接调用 checked-export ioctl，必须分别得到 ESTALE 或
EINVAL，随后重新 query 并核对原 segment 完全不变。日志只打印字段名、errno 和
原 segment ID，不打印 token value。

有效 export 请求会尝试分配 backing，因此本诊断不发送完整匹配的 export 请求。
它不会主动 retire、unexport 或修改原 descriptor；任何非预期结果都会停止诊断，
保留返回的资源标识供核对。只在本任务持有并暂停生命周期变化的 segment 上执行；
实际用例同时保留两个客户端的 CPU 映射，结束后继续数据校验并核对资源基线。
这只覆盖 home kernel 的请求校验，consumer 导入、动态轮换和恢复需另行验证。

## 完整 segment 库存

`linqu_gva_manager --dump-segments` 使用 `gva_manager_enumerate_segments()`
读取版本 1 库存，包含 active 与 retired segment、原 aperture generation、
完整 descriptor、地址是否仍保留、export 操作占用状态及 managed export ID/token。
输出包含用于核对的 token 身份，按运行证据保存，不公开整份原始库存。

调用方以全零结构加 `version=1` 开始，后续调用保留返回的 kernel_instance、
revision 和 cursor。本库清除输出字段后再发送下次请求；成功返回 ENTRY 或 END。
同一枚举期间发生分配、退役或 export 状态变更时，旧游标返回 ESTALE，调用方
必须丢弃整个库存。修订号溢出后枚举永久返回 EOVERFLOW；实例标识用于区分
本次内核实例，不能使用可能重复的 segment counter 代替它。

CLI 只在收到 END 后打印全部记录及唯一汇总，最多接受 65,536 条记录；超限、
旧内核、缺失能力、损坏响应或中途变化均失败，不输出貌似完整的部分库存。
库与独立 SDK 的旧 UAPI 头文件一起编译时，枚举明确返回 EOPNOTSUPP，保留旧
worker 的构建能力；使用新 UAPI 构建的 CLI 在旧内核上收到 ENOTTY 后也不回退。

`scope=segment-records fence_verified=0` 明确其证明范围。库存不证明进程 VMA、
QEMU home binding、TLB 或设备在途任务已清理，不能凭一份库存解除服务隔离。
原 `--query` 继续仅返回 active segment；retired 记录通过新枚举核对。
测试包括 `test_gsva_inventory.py` 的实际 kernel 函数夹具，以及
`gva_manager_inventory_golden.c` 的实际库/CLI 响应校验。
