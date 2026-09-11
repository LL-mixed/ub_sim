# GVA Manager Platform Library

本目录承载现有 `linqu_gva_manager` 与 OBMM provider 共用的地址管理实现。
公共头使用 kernel GSVA UAPI；服务 core 不应包含此目录的头文件。

## 目录约定

- `gva_manager.h`：平台 API；借用调用方的 OBMM fd，不关闭 fd。
- `gva_manager.c`：aperture 与 segment 的 kernel 操作及结果验证。
- 测试位于 `../../tests/gva_manager_golden.c` 和
  `../../tests/test_gva_manager_library.py`。
- 构建产物放在调用方的 `out/` 或临时测试目录；本目录不保存二进制。

API 返回 `0` 或 `-1` 并设置 `errno`。分配失败时保留 command 中的 kernel
返回信息：调用方必须核对可能已创建的 reservation，不能因结果校验失败
就推断资源不存在。retire 成功仅证明 kernel registry 确认，跨 holder drain、
映射撤销和实际 backing 回收仍由 provider 的生命周期管理负责。

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

## 内核分配检查

现有 CLI 新增 `--check-allocator --node-id N --node-count C`，在专用、空闲
aperture 中直接调用 ioctl，检查超大分配、不同大小/对齐、固定区间冲突和
aperture 状态保护；结束时只退役自己成功申请的 segment。不要在承载业务的
aperture 上执行此诊断。

两节点 segment CLI 验证入口已包含该检查，并等待两个节点的全部检查结束。
预期成功标记为 `result=done action=gsva-allocator-check checks=11`。
此入口需要实际运行修订后的 guest kernel；编译或 ioctl mock 单测不能替代它。
