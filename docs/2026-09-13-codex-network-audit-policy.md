# 本任务网络操作审计规则

日期：2026-09-13。适用于当前 GVA/GSVA 服务补齐任务及其故障反馈。

## 范围与目录约定

- 执行工具：`guest-linux/aarch64/scripts/codex_network_audit.py`；定向测试：
  `guest-linux/aarch64/tests/test_codex_network_audit.py`。
- 逐项操作日志：`out/codex-network-operations.jsonl`，逻辑上只追加事件。
- 告警审查：`out/codex-network-incident-<date>-<sequence>.md`。
- 官方反馈草稿：`out/codex-network-issue-<date>-<sequence>.md`。
- 日志和告警材料不提交、不自动清理；用户明确要求后才归档或清理。
  审计规则、执行工具和测试可提交。日志保持本地访问权限，不公开整份日志。

覆盖由本任务 agent 发起的 SSH/SCP、远端 Git、HTTP/API、网页/浏览器操作、
连接器请求、依赖解析/下载、远端作业的已知联网行为，以及每次重试。
本地读取已有日志不产生新的网络操作；通过 SSH 读取日志仍须登记。
复用 SSH 连接的每条命令分别登记。启动远端作业时记录其网络配置、预期目标、
依赖下载策略和进程/作业句柄；后续查询分别记录并关联原操作 ID。

这是工具/命令层的操作审计，不提供整机抓包或 Codex 内部请求观测。
浏览器资源加载、DNS、重定向、远端子进程的单条连接只能按实际可见信息记录，
不可见部分写明 unknown，禁止把声明的网络范围写成抓包证据。
在审计启用前发生的操作只能补录为 retrospective，时间不明时保留未知。

## 执行规则

1. 调用前写入 begin：唯一 ID、UTC 记录时间、工具类型、目标、目的、操作、
   发送数据类别、授权来源、预期网络范围、风险判断及理由。
2. 终端网络命令使用 `run`；执行参数直接传给子进程，不增加一层 shell。
   浏览器、web、连接器使用 `begin`，取得 ID 后调用，再用 `finish` 补录结果。
3. 结束时写入 end：成功、失败或未知，退出码/错误类别、证据位置。
   无法取得终态时保持未闭合或标为 unknown；不能推断成功。
4. 无法写入开始记录时不发起网络操作。发生中断后先用 `status` 查看未闭合项，
   检查原句柄；不得因为观察超时重复启动远端作业。
5. 凭据通过原有受保护的配置传递。记录命令前人工检查，禁止直接传入凭据；
   工具对常见密码、token、认证头与 URL 凭据做附加脱敏，无法保证识别任意秘密。
   不记录 stdin、stdout/stderr 全文、模型数据、完整 descriptor 或文件上传内容。
6. 日志中保留诊断所需的本地目标/命令；对外反馈另制摘要，去除主机名、内部地址、
   个人路径及机密代码。未经批准不上传完整日志或整个任务对话。

CLI 示例（从仓库根目录执行）：

```text
python3 guest-linux/aarch64/scripts/codex_network_audit.py status
python3 guest-linux/aarch64/scripts/codex_network_audit.py run --kind ssh --target <host> --purpose <reason> --operation <description> --data-sent <category> --authorization <scope> --network-scope <known-egress> --risk low --risk-reason <reason> -- ssh <host> <command>
python3 guest-linux/aarch64/scripts/codex_network_audit.py begin --kind web --target <url> --purpose <reason> --operation <description> --data-sent <category> --authorization <scope> --network-scope <known-egress> --risk low --risk-reason <reason>
python3 guest-linux/aarch64/scripts/codex_network_audit.py finish --id <id> --status completed --details <result>
```

## 告警审查与反馈

记录截图/可见提示、收到时间、可关联的最后一条操作、尚在运行的句柄和证据缺口。
逐项检查目标是否在授权范围、操作是否具有攻击/破坏行为、是否读取凭据、是否外传
敏感数据、是否改变暴露面，以及远端子命令的真实含义。SSH、TCP、ASan 或内存
错误等名称本身不能作为高危结论。

发现具体风险时指出操作 ID、行为与影响，并暂停该项操作；其余授权工作继续。
未发现高危行为时，将可核实的事实和不确定性补充到既有官方 issue，避免重复建单。
要求维护者说明被拦截的具体操作/输出、判定依据、恢复办法及可用于关联的诊断 ID。
用户授权本任务的此类反馈；该授权不包含公开原始私有日志。

最终记录反馈 URL/回执，只有可确认发布成功才标记 submitted。日志没有高危行为
只能支持所审查操作的判断，不能证明平台内部误报根因。告警可能作用于生成内容，
不能预设一定由网络请求触发，也不尝试绕过产品的限制。
