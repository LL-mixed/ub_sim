# Kernel and Orchestration Configuration

from pathlib import Path

_ROOT_DIR = Path(__file__).parent

# Runtime configuration for tensormap_and_ringbuffer
# This runtime requires 4 AICPU threads (3 schedulers + 1 orchestrator on thread 3)
RUNTIME_CONFIG = {
	"runtime": "tensormap_and_ringbuffer",
	"aicpu_thread_num": 4,
	"block_dim": 24,
}

ORCHESTRATION = {
	"source": str(_ROOT_DIR / "orchestration" / "qwen3_decode.cpp"),
	"function_name": "aicpu_orchestration_entry"
}

KERNELS = [
	{"func_id": 0, "name": "copy_hidden", "source": str(_ROOT_DIR / "kernels" / "aiv" / "copy_hidden.cpp"), "core_type": "aiv"},
	{"func_id": 1, "name": "rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 2, "name": "q_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "q_proj.cpp"), "core_type": "aic"},
	{"func_id": 3, "name": "kv_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "kv_proj.cpp"), "core_type": "aic"},
	{"func_id": 4, "name": "qk_norm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "qk_norm.cpp"), "core_type": "aiv"},
	{"func_id": 5, "name": "q_pad", "source": str(_ROOT_DIR / "kernels" / "aiv" / "q_pad.cpp"), "core_type": "aiv"},
	{"func_id": 6, "name": "rope_kv_cache", "source": str(_ROOT_DIR / "kernels" / "aiv" / "rope_kv_cache.cpp"), "core_type": "aiv"},
	{"func_id": 7, "name": "qk_matmul", "source": str(_ROOT_DIR / "kernels" / "aic" / "qk_matmul.cpp"), "core_type": "aic"},
	{"func_id": 8, "name": "softmax", "source": str(_ROOT_DIR / "kernels" / "aiv" / "softmax.cpp"), "core_type": "aiv"},
	{"func_id": 9, "name": "sv_matmul", "source": str(_ROOT_DIR / "kernels" / "aic" / "sv_matmul.cpp"), "core_type": "aic"},
	{"func_id": 10, "name": "online_softmax", "source": str(_ROOT_DIR / "kernels" / "aiv" / "online_softmax.cpp"), "core_type": "aiv"},
	{"func_id": 11, "name": "attention_writeback", "source": str(_ROOT_DIR / "kernels" / "aiv" / "attention_writeback.cpp"), "core_type": "aiv"},
	{"func_id": 12, "name": "out_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "out_proj.cpp"), "core_type": "aic"},
	{"func_id": 13, "name": "out_proj_residual", "source": str(_ROOT_DIR / "kernels" / "aiv" / "out_proj_residual.cpp"), "core_type": "aiv"},
	{"func_id": 14, "name": "post_rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "post_rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 15, "name": "gate_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "gate_proj.cpp"), "core_type": "aic"},
	{"func_id": 16, "name": "up_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "up_proj.cpp"), "core_type": "aic"},
	{"func_id": 17, "name": "silu", "source": str(_ROOT_DIR / "kernels" / "aiv" / "silu.cpp"), "core_type": "aiv"},
	{"func_id": 18, "name": "down_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "down_proj.cpp"), "core_type": "aic"},
	{"func_id": 19, "name": "down_proj_residual", "source": str(_ROOT_DIR / "kernels" / "aiv" / "down_proj_residual.cpp"), "core_type": "aiv"},
	{"func_id": 20, "name": "copy_out", "source": str(_ROOT_DIR / "kernels" / "aiv" / "copy_out.cpp"), "core_type": "aiv"},
]
