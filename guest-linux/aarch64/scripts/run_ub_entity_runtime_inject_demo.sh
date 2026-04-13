#!/bin/bash
# M1 运行期动态注入演示脚本
# 演示如何使用 entity plan 进行运行期动态注入

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"

echo "=== M1: 运行期动态注入演示 ==="
echo
echo "当前实现状态:"
echo "1. 启动时可加载 entity plan 文件 (UB_FM_ENTITY_PLAN_FILE)"
echo "2. entity plan 文件定义了每个实体的 state (present/absent)"
echo "3. QEMU 启动时会根据 plan 注入 UB_DEV_REG 消息到 guest"
echo
echo "使用方法:"
echo
echo "Step 1: 准备 entity plan 文件"
cat << 'EOF'
[entity 0]
state=present
entity_idx=0
device_id=0x0541
eid=0x10000
ueid=0x10000
cna=0x200
upi=1

[entity 1]
state=present
entity_idx=1
device_id=0x0542
eid=0x10001
ueid=0x10001
cna=0x201
upi=1
EOF

echo
echo "Step 2: 启动 QEMU 时指定 entity plan"
echo "  UB_SIM_ENTITY_COUNT=2 \\"
echo "  UB_FM_ENTITY_PLAN_FILE=/path/to/entity_plan.ini \\"
echo "  ./run_ub_dual_node_ubcore_urma_e2e.sh"
echo
echo "Step 3: 运行期动态修改（当前需要重启 QEMU）"
echo "  修改 entity plan 文件中的 state=present/absent"
echo "  重启 QEMU 后生效"
echo
echo "=== 未来改进方向 ==="
echo "1. 添加 QMP 命令支持运行期重新加载（无需重启）"
echo "2. 添加文件监控自动检测 entity plan 变化"
echo "3. 支持通过 QMP socket 发送重新加载命令"
