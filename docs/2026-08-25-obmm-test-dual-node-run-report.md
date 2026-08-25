# obmm-test（上游测试框架）双节点运行报告

- 日期：2026-08-25
- 上游仓库：https://gitcode.com/obmm-ecology/obmm-test（clone 于 /sd_data/repo/obmm-test）
- ub_sim 提交：`55a803a`（7 文件 +679）
- 结论：**框架级打通**——orchestrator/conductor/actor 三级分布式执行在 2 节点
  仿真集群上完整运转；**测试内容级受阻**于上游测试对 M1 真机运行面的假设
  （详见 §5），全量跑通需要仿真面（sysfs 布局等）对齐 + 小时级时长预算

## 1. 交叉构建链（全部一次通过）

| 产物 | 说明 |
|---|---|
| libnuma.a（aarch64） | numactl 上游 autotools 交叉构建 → /tmp/numactl-aarch64 |
| libobmm.a | vendor/obmm 的 libobmm.c + **sim 版 vendor adaptor**（与 ub_sim 内一致）→ /tmp/libobmm-aarch64 |
| obmm-test 全家 | gcc-12 交叉（gcc-11 不识别 `armv8.7-a+lse+ls64`），direct-link libobmm + 静态，约 4.3MB/二进制 |

宿主机 qemu-user（binfmt_misc）冒烟：orchestrator+2×conductor 跑
runtime_smoke+framework_tests，9 passed / 2 error（均为宿主无 OBMM 的预期失败）。

## 2. 上游 obmm-test 的三个构建阻断（已本地修复，待回馈上游）

1. `CMakeLists.txt` 顶层 direct-link 分支与 `src/*/CMakeLists.txt` 的
   `target_link_libraries` **plain/keyword 签名混用**——Linux direct-link
   模式从未被上游构建过；统一为 keyword 签名（3 文件 6 行）
2. 跨目录 keyword 链接需 `CMP0079=NEW`（CLI 传入 `CMAKE_POLICY_DEFAULT_CMP0079=NEW`，未改仓库）
3. 内核 UAPI 头需要 `-D__EXPORTED_HEADERS__ -I kernel_ub/include`（与 ub_sim
   同款处理，走 CFLAGS 未改仓库）

## 3. guest 接线（ub_sim 侧，`55a803a`）

- `apps/obmm_test/obmm_test.c` → `linqu_obmm_test`：拉起 lo、ipourma 数据路
  配置、起 conductor；首节点等待就绪后生成 ini（host0=127.0.0.1）并前台跑
  orchestrator、透传退出码
- `initramfs/run_app`：`linqu_obmm_test=1` 分发 + 8 个 `obmm_test_*` cmdline
  环境变量（port、四个过滤器、timeout scale）
- `scripts/run_ub_dual_node_obmm_test.sh`：pool demo 同款双节点启动 +
  per-run initramfs 注入 conductor/orchestrator 二进制 + 残留 VM 清理 +
  结果摘要；`OBMM_TEST_GROUP_INCLUDE/EXCLUDE/INCLUDE/EXCLUDE` 全可透传
- 契约测试 `test_obmm_test_guest_wiring.py` 5/5 绿；全量 282 项仅剩 3 个
  已知环境 error

## 4. 调试沉淀的四个坑（均已修复，值得记录）

1. **ipourma 不回环自连**：连接本节点 10.0.0.x 不通 → conductor 间通信
   host0 用 127.0.0.1（conductor 绑 0.0.0.0）
2. **initramfs 里 lo 是 down 的**：127.0.0.1 也不通 → app 里 ioctl 拉起 lo
3. **裸 connect+close 探测会杀死 conductor**（accept 读到 EOF 即退出，且
   orchestrator 连接不重试）→ 改为 `/proc/net/tcp` 监听表检查 + 对端固定
   宽限期；注意 0.0.0.0 绑定在表里显示为 `00000000:PORT`
4. **前车之鉴**：`/proc/net/tcp` 行首前导空格让列定位解析全错——用整行
   子串匹配

## 5. 当前边界（下一步工作的准确入口）

框架链路实证（18-49-25 运行日志）：orchestrator 连接双 conductor 成功、
`Found 6 tests matching filters`、`CTRL_MSG_START_TEST` 双节点下发、
conductor fork actor——**分布式执行机制完整可用**。

内容级卡点：
- 首个测试 `runtime_smoke/cleanup_env` 挂起：上游测试读 `/sys/devices/obmm`
  等 M1 真机 sysfs 面（宿主上同测试因路径缺失而快速报错，guest 上路径存在
  但语义不同导致阻塞）——需要对齐 sim 驱动的 sysfs 布局或裁剪该组
- `mami4ubmem` 可选运行库缺失（框架 guard 降级继续，不影响框架面）
- 时长：单测试基线超时即 600~3000s（×5 scale），全量 suite 在仿真上是
  小时级工程，建议过滤子集 + 远程长跑

## 6. 复现命令

```bash
# 1. 交叉构建 obmm-test（见 §1，含上游三修复）
# 2. 跑（默认全量；建议先过滤）：
cd guest-linux/aarch64
OBMM_TEST_GROUP_INCLUDE=runtime_smoke OBMM_TEST_GROUP_EXCLUDE= \
  ./scripts/run_ub_dual_node_obmm_test.sh
```

产物：`logs/<RUN_ID>/node{A,B}_guest.log`（orchestrator 汇总与逐测试行）、
guest 内 `/tmp/obmm-test-results.csv`（随 VM 销毁，以串口日志为准）。
