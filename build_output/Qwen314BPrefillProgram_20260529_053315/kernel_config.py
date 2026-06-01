# Kernel and Orchestration Configuration

from pathlib import Path

_ROOT_DIR = Path(__file__).parent

# Runtime configuration for tensormap_and_ringbuffer.
# This runtime requires 4 AICPU threads (3 schedulers + 1 orchestrator on thread 3).
# block_dim is only emitted when the user passes compile(block_dim=...);
# otherwise the runtime default applies (simpler validates against device capacity).
RUNTIME_CONFIG = {
	"runtime": "tensormap_and_ringbuffer",
	"aicpu_thread_num": 4,
}

ORCHESTRATION = {
	"source": str(_ROOT_DIR / "orchestration" / "qwen3_14b_prefill.cpp"),
	"function_name": "aicpu_orchestration_entry"
}

KERNELS = [
	{"func_id": 0, "name": "rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 1, "name": "q_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "q_proj.cpp"), "core_type": "aic"},
	{"func_id": 2, "name": "kv_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "kv_proj.cpp"), "core_type": "aic"},
	{"func_id": 3, "name": "q_norm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "q_norm.cpp"), "core_type": "aiv"},
	{"func_id": 4, "name": "k_norm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "k_norm.cpp"), "core_type": "aiv"},
	{"func_id": 5, "name": "q_pad", "source": str(_ROOT_DIR / "kernels" / "aiv" / "q_pad.cpp"), "core_type": "aiv"},
	{"func_id": 6, "name": "rope_kv_cache", "source": str(_ROOT_DIR / "kernels" / "aiv" / "rope_kv_cache.cpp"), "core_type": "aiv"},
	{"func_id": 7, "name": "qk_matmul", "source": str(_ROOT_DIR / "kernels" / "aic" / "qk_matmul.cpp"), "core_type": "aic"},
	{"func_id": 8, "name": "softmax", "source": str(_ROOT_DIR / "kernels" / "aiv" / "softmax.cpp"), "core_type": "aiv"},
	{"func_id": 9, "name": "sv_matmul", "source": str(_ROOT_DIR / "kernels" / "aic" / "sv_matmul.cpp"), "core_type": "aic"},
	{"func_id": 10, "name": "online_softmax_init", "source": str(_ROOT_DIR / "kernels" / "aiv" / "online_softmax_init.cpp"), "core_type": "aiv"},
	{"func_id": 11, "name": "online_softmax", "source": str(_ROOT_DIR / "kernels" / "aiv" / "online_softmax.cpp"), "core_type": "aiv"},
	{"func_id": 12, "name": "attention_context", "source": str(_ROOT_DIR / "kernels" / "aiv" / "attention_context.cpp"), "core_type": "aiv"},
	{"func_id": 13, "name": "attention_writeback", "source": str(_ROOT_DIR / "kernels" / "aiv" / "attention_writeback.cpp"), "core_type": "aiv"},
	{"func_id": 14, "name": "out_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "out_proj.cpp"), "core_type": "aic"},
	{"func_id": 15, "name": "out_proj_residual", "source": str(_ROOT_DIR / "kernels" / "aiv" / "out_proj_residual.cpp"), "core_type": "aiv"},
	{"func_id": 16, "name": "post_rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "post_rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 17, "name": "gate_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "gate_proj.cpp"), "core_type": "aic"},
	{"func_id": 18, "name": "up_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "up_proj.cpp"), "core_type": "aic"},
	{"func_id": 19, "name": "silu", "source": str(_ROOT_DIR / "kernels" / "aiv" / "silu.cpp"), "core_type": "aiv"},
	{"func_id": 20, "name": "down_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "down_proj.cpp"), "core_type": "aic"},
	{"func_id": 21, "name": "down_proj_residual", "source": str(_ROOT_DIR / "kernels" / "aiv" / "down_proj_residual.cpp"), "core_type": "aiv"},
]
