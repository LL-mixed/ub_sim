# UB 仿真 `entity_idx > 0` 全链路支持实施方案（可执行版）

## 1. 背景与目标

目标不是“再加一个独立 qdev 设备”，而是**在同一 UBC 设备内真实支持 FE1/UE（`entity_idx > 0`）**，让内核枚举、驱动匹配、UE 激活、URMA 数据面都走通。

本方案落地后应满足：

1. `entity_idx=0` 继续作为 FE0/MUE（兼容现状）。
2. `entity_idx=1..N-1` 在同一设备内可被当作 UE 访问（cfg/msg/mailbox 全链路）。
3. guest 内看到 UE 设备（`0xCC08:0x0542`）并触发 ubase probe + auxiliary 设备创建。
4. 双节点场景可进入 URMA dataplane 验证。

## 2. 当前阻断点（代码证据）

### 2.1 QEMU 侧硬阻断

1. `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_config.c:258`
   - 明确拒绝 `entity_idx != 0`（`vm support only FE0`）。
2. `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c:231-234`
   - `UB_OBTAIN_ENTITY_INFO` 固定返回 `entity_nums=1,mue_nums=1,map[0]=0..0`。
3. `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c:1445,1452,1775,1776,3415`
   - `total_ue_num/ue_num/ue_cnt/total_num_of_ue` 都固定为 1。

### 2.2 Guest kernel 侧关键 shortcut

1. `simulator/guest-linux/kernel_ub/drivers/ub/ubus/ubus_entity.c:667-671`
   - 在 `is_ibus_controller()` 下直接把 entity map 固定为 `0..0`，不向设备发 `UB_OBTAIN_ENTITY_INFO`。
2. `simulator/guest-linux/kernel_ub/drivers/ub/ubus/enum.c:1496-1502`
   - cluster ICONTROLLER 分支直接 `continue`，跳过 `ub_start_ent()`。

### 2.3 驱动匹配前提（必须满足）

1. `simulator/guest-linux/kernel_ub/drivers/ub/ubase/ubase_ubus.h:16,35,36`
   - ubase 需要 `vendor=0xCC08`，MUE 用 `0x0541`，UE 用 `0x0542`。
2. `simulator/guest-linux/kernel_ub/drivers/ub/ubus/ubus_driver.c:148-153`
   - 匹配由 `vendor/device/module/class_mask` 共同决定。

## 3. 设计原则

1. **同一 qdev，逻辑多 entity**：不新增额外 UBC qdev；通过 `entity_idx` 访问同一控制器内不同 FE。
2. **默认兼容**：默认 `entity_count=1`，行为保持现状。
3. **先控制面可用，再数据面增强**：先打通枚举 + probe，再扩展能力字段和 dataplane 稳定性。
4. **开关化上线**：所有新行为有 feature flag，可快速回滚。

## 4. 目标模型（统一定义）

单 UBC 的实体模型定义为：

1. `entity_idx=0`：FE0/MUE（`device_id=0x0541`，class=bus-controller）。
2. `entity_idx=1..N-1`：FEi/UE（`device_id=0x0542`，class=non-bus-controller）。
3. `N` 由配置给出（建议参数名：`entity_count`，最小 1，默认 1）。

`UB_OBTAIN_ENTITY_INFO` 响应约定：

1. `entity_nums = N`
2. `mue_nums = 1`
3. `map[0] = {start_entity_idx=0, end_entity_idx=N-1}`

## 5. 分阶段实施（按提交可切分）

## M1：QEMU 支持 `entity_idx > 0` 的 cfg/实体信息链路（必做）

涉及文件：

1. `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h`
2. `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`
3. `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_config.c`
4. `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c`

改动点：

1. 在 `BusControllerDev` 增加逻辑 entity 配置与影子 cfg（shadow cfg）：
   - `entity_count`
   - `entity_cfg[N]`（至少存 `cfg`，可选 `wmask`）
2. 新增 qdev 属性（建议）：
   - `entity_count`（默认 1）
3. 初始化阶段构造每个 entity 的 cfg 视图：
   - FE0 继承现有 cfg。
   - FE1..N-1 基于 FE0 cfg 拷贝并打补丁：
     - GUID device id -> `0x0542`
     - class_code base != `UB_BASE_CODE_BUS_CONTROLLER`
     - `UB_CFG0_DEV_UEID_OFFSET` -> `entity_idx`
4. `ub_cfg_rw()` 去掉“`entity!=0`直接失败”的逻辑：
   - 根据 `entity_idx` 选择对应 cfg 视图。
   - 越界时返回 `UB_MSG_RSP_REG_ATTR_MISMATCH`。
5. `UB_OBTAIN_ENTITY_INFO` 按 `entity_count` 动态填充，不再固定 `0..0`。
6. `QUERY_COMM_RSRC_PARAM/QUERY_UE_RES/cfg0 total_num_of_ue` 改为动态值。

验收标准：

1. `entity_count=1` 与现网行为一致。
2. `entity_count=2` 时，guest 发 `entity_idx=1` 的 cfg read 不再报错。
3. `UB_OBTAIN_ENTITY_INFO` 报文可见 `entity_nums=2,map=0..1`。

## M2：Guest kernel 去除 cluster 场景下的 FE0-only shortcut（必做）

涉及文件：

1. `simulator/guest-linux/kernel_ub/drivers/ub/ubus/ubus_entity.c`
2. `simulator/guest-linux/kernel_ub/drivers/ub/ubus/enum.c`

改动点：

1. `ub_obtain_entity_info()`：
   - `is_ibus_controller()` 时不再无条件返回 `map=0..0`。
   - 改为可配置策略：
     - 默认兼容：旧行为
     - `UB_SIM_MULTI_ENTITY=1` 时：走真实 `UB_OBTAIN_ENTITY_INFO` 请求
2. `ub_enum_entities_active()`：
   - cluster ICONTROLLER 不再无条件跳过 `ub_start_ent()`。
   - 增加条件开关：仅在 `UB_SIM_MULTI_ENTITY=1` 启用 `ub_start_ent()`。

验收标准：

1. `UB_SIM_MULTI_ENTITY=0` 时行为不变。
2. `UB_SIM_MULTI_ENTITY=1 && entity_count=2` 时：
   - `ub_start_ent()` 执行；
   - `ub_mue_enable_and_map()` 使用设备返回的 `0..1` map。

## M3：设备身份与驱动匹配打通（必做）

涉及文件：

1. `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`
2. （必要时）`simulator/guest-linux/kernel_ub/drivers/ub/ubase/*.c`

改动点：

1. FE1 的 cfg GUID/device_id 必须是 `0x0542`，满足 ubase id table。
2. `QUERY_UE_RES` / `QUERY_CHIP_INFO` 的 `ue_id` 返回与 `entity_idx`一致（建议）。
3. `ue_num/total_ue_num` 与 `entity_count`一致，确保 `mbx_ue_id` 校验不过早失败。

验收标准：

1. `/sys/bus/ub/devices` 出现 UE 实体（`entity_idx=1`）。
2. ubase 对 UE 设备触发 probe。
3. auxiliary 设备（udma/ipourma 相关）被创建。

## M4：双节点 dataplane 联调与回归（必做）

涉及文件：

1. `simulator/guest-linux/aarch64/scripts/run_ub_dual_node_probe.sh`
2. `simulator/guest-linux/aarch64/scripts/run_ub_dual_node_ubcore_urma_e2e.sh`
3. `simulator/guest-linux/aarch64/scripts/run_ub_dual_node_urma_dataplane_workload_test.sh`

改动点：

1. 启动参数新增并透传：
   - QEMU：`entity_count=2`
   - guest kernel cmdline：`UB_SIM_MULTI_ENTITY=1`（或等价 module param）
2. 在 probe/e2e 脚本增加断言：
   - `entity_idx=1` 存在
   - UE 设备 id 为 `0x0542`
   - ubase auxiliary 节点存在

验收标准：

1. probe 脚本稳定通过（>=20 次）。
2. ubcore+URMA e2e 稳定通过（>=20 次）。
3. workload 数据面稳定通过（>=20 次）。

## 6. 关键实现细节（避免踩坑）

1. **不要直接改全局 `config_read/config_write` 回调签名**。
   - 先在 `ub_cfg_rw()` 内按 `entity_idx` 做选择，风险最低。
2. **FE1 class_code 必须不是 bus-controller 基码**。
   - 否则 kernel 仍会把它当 ICONTROLLER，不会走 UE 路径。
3. **`entity_count` 建议上限先做小值（2/4）**。
   - 先用 2 打通，再扩规模。
4. **remote snapshot 先保持 entity0 语义**。
   - 先解本地枚举/probe；跨节点 entity 细化可放后续迭代。

## 7. Feature Flag 与回滚

新增开关建议：

1. QEMU：`UB_SIM_ENTITY_COUNT`（或 qdev property `entity_count`）
2. Guest：`UB_SIM_MULTI_ENTITY=0/1`

回滚策略：

1. 任一问题先将两侧开关恢复为默认（`entity_count=1`，`UB_SIM_MULTI_ENTITY=0`）。
2. 保证旧路径可一键恢复，不依赖代码回退。

## 8. 执行清单（可直接按此开工）

### 提交 1：QEMU 基础能力

1. [ ] 增加 `entity_count` 属性与存储结构。
2. [ ] 在 `ubc_msgq.c` 动态返回 entity map。
3. [ ] 在 `ub_config.c` 支持 `entity_idx>0` cfg read/write。
4. [ ] 将 `total_num_of_ue/ue_num/ue_cnt` 改为动态。

### 提交 2：Kernel multi-entity 启用

1. [ ] `ub_obtain_entity_info()` 去 shortcut（开关化）。
2. [ ] cluster ICONTROLLER 条件化执行 `ub_start_ent()`。
3. [ ] 增加日志：打印 map、entity count、启用路径。

### 提交 3：匹配与枚举验证

1. [ ] 校验 FE1 GUID/device/class 填充正确。
2. [ ] 校验 ubase probe + auxiliary 创建设备。
3. [ ] 增加脚本断言并固化到 probe 脚本。

### 提交 4：双节点 e2e 回归

1. [ ] 连跑 `probe/e2e/workload` 各 20 轮。
2. [ ] 记录失败率、失败日志、平均建链耗时。
3. [ ] 确认开关回退路径可用。

## 9. 最小验收命令集

```bash
# 1) 双节点 probe（含 FE1 枚举断言）
cd simulator/guest-linux/aarch64
AARCH64_LINUX_CC=/opt/homebrew/bin/aarch64-unknown-linux-gnu-gcc \
UB_SIM_MULTI_ENTITY=1 UB_SIM_ENTITY_COUNT=2 \
./scripts/run_ub_dual_node_probe.sh

# 2) ubcore + URMA e2e
AARCH64_LINUX_CC=/opt/homebrew/bin/aarch64-unknown-linux-gnu-gcc \
UB_SIM_MULTI_ENTITY=1 UB_SIM_ENTITY_COUNT=2 ITERATIONS=20 RUN_SECS=180 \
./scripts/run_ub_dual_node_ubcore_urma_e2e.sh

# 3) dataplane workload
AARCH64_LINUX_CC=/opt/homebrew/bin/aarch64-unknown-linux-gnu-gcc \
UB_SIM_MULTI_ENTITY=1 UB_SIM_ENTITY_COUNT=2 ITERATIONS=20 RUN_SECS=180 \
./scripts/run_ub_dual_node_urma_dataplane_workload_test.sh
```

## 10. DoD（完成定义）

1. `entity_idx=1` 在 guest 中稳定出现并可被 ubase 匹配。
2. auxiliary 设备稳定创建，udma/ipourma 可工作。
3. 双节点 URMA 数据面测试稳定通过（20/20）。
4. 开关关闭后可恢复到当前单 FE0 路径，无回归。
