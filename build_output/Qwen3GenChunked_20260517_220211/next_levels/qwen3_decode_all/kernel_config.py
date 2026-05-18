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
	"source": str(_ROOT_DIR / "orchestration" / "qwen3_decode_all.cpp"),
	"function_name": "aicpu_orchestration_entry"
}

KERNELS = [
	{"func_id": 0, "name": "decode_copy_hidden", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_copy_hidden.cpp"), "core_type": "aiv"},
	{"func_id": 1, "name": "decode_rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 2, "name": "decode_q_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "decode_q_proj.cpp"), "core_type": "aic"},
	{"func_id": 3, "name": "decode_kv_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "decode_kv_proj.cpp"), "core_type": "aic"},
	{"func_id": 4, "name": "decode_qk_norm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_qk_norm.cpp"), "core_type": "aiv"},
	{"func_id": 5, "name": "decode_q_pad", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_q_pad.cpp"), "core_type": "aiv"},
	{"func_id": 6, "name": "decode_rope_kv_cache", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_rope_kv_cache.cpp"), "core_type": "aiv"},
	{"func_id": 7, "name": "decode_qk_matmul", "source": str(_ROOT_DIR / "kernels" / "aic" / "decode_qk_matmul.cpp"), "core_type": "aic"},
	{"func_id": 8, "name": "decode_softmax", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_softmax.cpp"), "core_type": "aiv"},
	{"func_id": 9, "name": "decode_sv_matmul", "source": str(_ROOT_DIR / "kernels" / "aic" / "decode_sv_matmul.cpp"), "core_type": "aic"},
	{"func_id": 10, "name": "decode_online_softmax", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_online_softmax.cpp"), "core_type": "aiv"},
	{"func_id": 11, "name": "decode_attention_writeback", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_attention_writeback.cpp"), "core_type": "aiv"},
	{"func_id": 12, "name": "decode_out_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "decode_out_proj.cpp"), "core_type": "aic"},
	{"func_id": 13, "name": "decode_out_proj_residual", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_out_proj_residual.cpp"), "core_type": "aiv"},
	{"func_id": 14, "name": "decode_post_rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_post_rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 15, "name": "decode_gate_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "decode_gate_proj.cpp"), "core_type": "aic"},
	{"func_id": 16, "name": "decode_up_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "decode_up_proj.cpp"), "core_type": "aic"},
	{"func_id": 17, "name": "decode_silu", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_silu.cpp"), "core_type": "aiv"},
	{"func_id": 18, "name": "decode_down_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "decode_down_proj.cpp"), "core_type": "aic"},
	{"func_id": 19, "name": "decode_down_proj_residual", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_down_proj_residual.cpp"), "core_type": "aiv"},
	{"func_id": 20, "name": "decode_copy_out", "source": str(_ROOT_DIR / "kernels" / "aiv" / "decode_copy_out.cpp"), "core_type": "aiv"},
]
