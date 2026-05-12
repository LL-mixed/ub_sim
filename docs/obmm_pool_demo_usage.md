# OBMM Pool Demo 使用说明

## 环境变量

### Guest 侧（通过 serial payload 传入）

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `OBMM_POOL_EXPORT_SIZE_MB` | 7680 | 每个节点 export 的共享内存大小（MB） |
| `OBMM_IMPORT_CACHE_MODE` | `auto` | Import 缓存模式：`auto`/`nc`（non-cacheable）/`cc`（cacheable） |
| `OBMM_POOL_STRESS_ITERS` | 不设置（不跑） | 压力测试迭代次数，仅显式设置时才运行 |

### 压力测试

压力测试为 opt-in 模式。每个节点对所有 remote slot 执行写+msync 循环，测量 import 内存吞吐。

- 未设置 `OBMM_POOL_STRESS_ITERS` 时跳过
- 设置 `OBMM_POOL_STRESS_ITERS=0` 同样跳过
- 有效范围：1~100000，超出范围使用默认值 200
- 每次迭代写入 64KB（`STRESS_TOUCH_SIZE`）并 msync

示例：

```bash
# 独立运行 pool demo，启用 100 次压力测试
OBMM_POOL_STRESS_ITERS=100 \
  zsh guest-linux/aarch64/scripts/run_ub_four_node_obmm_pool.sh
```

### Import 缓存模式

- `nc`：Non-cacheable，通过 `O_SYNC` + `pgprot_writecombine` 映射
- `cc`：Cacheable，普通 `mmap` 映射
- `auto`：默认使用 `nc`

```bash
# 使用 cacheable import
OBMM_IMPORT_CACHE_MODE=cc \
  zsh guest-linux/aarch64/scripts/run_ub_four_node_obmm_pool.sh
```

## 与 W4 Guest 的交互

W4 guest decode loop 在每个 step 开始时调用 `run_obmm_backing_stage()`，内部执行 `linqu_ub_obmm_demo`。此场景下不设置 `OBMM_POOL_STRESS_ITERS`，压力测试自动跳过，避免阻塞 decode 流程。
