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
	"source": str(_ROOT_DIR / "orchestration" / "qwen3_prefill_all.cpp"),
	"function_name": "aicpu_orchestration_entry"
}

KERNELS = [
	{"func_id": 0, "name": "prefill_copy_hidden", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_copy_hidden.cpp"), "core_type": "aiv"},
	{"func_id": 1, "name": "prefill_rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 2, "name": "prefill_q_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "prefill_q_proj.cpp"), "core_type": "aic"},
	{"func_id": 3, "name": "prefill_kv_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "prefill_kv_proj.cpp"), "core_type": "aic"},
	{"func_id": 4, "name": "prefill_q_norm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_q_norm.cpp"), "core_type": "aiv"},
	{"func_id": 5, "name": "prefill_k_norm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_k_norm.cpp"), "core_type": "aiv"},
	{"func_id": 6, "name": "prefill_q_pad", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_q_pad.cpp"), "core_type": "aiv"},
	{"func_id": 7, "name": "prefill_rope_kv_cache", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_rope_kv_cache.cpp"), "core_type": "aiv"},
	{"func_id": 8, "name": "prefill_qk_matmul", "source": str(_ROOT_DIR / "kernels" / "aic" / "prefill_qk_matmul.cpp"), "core_type": "aic"},
	{"func_id": 9, "name": "prefill_softmax", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_softmax.cpp"), "core_type": "aiv"},
	{"func_id": 10, "name": "prefill_sv_matmul", "source": str(_ROOT_DIR / "kernels" / "aic" / "prefill_sv_matmul.cpp"), "core_type": "aic"},
	{"func_id": 11, "name": "prefill_online_softmax_init", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_online_softmax_init.cpp"), "core_type": "aiv"},
	{"func_id": 12, "name": "prefill_online_softmax", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_online_softmax.cpp"), "core_type": "aiv"},
	{"func_id": 13, "name": "prefill_attention_context", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_attention_context.cpp"), "core_type": "aiv"},
	{"func_id": 14, "name": "prefill_attention_writeback", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_attention_writeback.cpp"), "core_type": "aiv"},
	{"func_id": 15, "name": "prefill_out_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "prefill_out_proj.cpp"), "core_type": "aic"},
	{"func_id": 16, "name": "prefill_out_proj_residual", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_out_proj_residual.cpp"), "core_type": "aiv"},
	{"func_id": 17, "name": "prefill_post_rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_post_rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 18, "name": "prefill_gate_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "prefill_gate_proj.cpp"), "core_type": "aic"},
	{"func_id": 19, "name": "prefill_up_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "prefill_up_proj.cpp"), "core_type": "aic"},
	{"func_id": 20, "name": "prefill_silu", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_silu.cpp"), "core_type": "aiv"},
	{"func_id": 21, "name": "prefill_down_proj", "source": str(_ROOT_DIR / "kernels" / "aic" / "prefill_down_proj.cpp"), "core_type": "aic"},
	{"func_id": 22, "name": "prefill_down_proj_residual", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_down_proj_residual.cpp"), "core_type": "aiv"},
	{"func_id": 23, "name": "prefill_copy_out", "source": str(_ROOT_DIR / "kernels" / "aiv" / "prefill_copy_out.cpp"), "core_type": "aiv"},
]
