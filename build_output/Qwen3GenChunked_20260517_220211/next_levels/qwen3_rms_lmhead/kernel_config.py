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
	"source": str(_ROOT_DIR / "orchestration" / "qwen3_rms_lmhead.cpp"),
	"function_name": "aicpu_orchestration_entry"
}

KERNELS = [
	{"func_id": 0, "name": "rms_lmhead_rmsnorm", "source": str(_ROOT_DIR / "kernels" / "aiv" / "rms_lmhead_rmsnorm.cpp"), "core_type": "aiv"},
	{"func_id": 1, "name": "rms_lmhead_lm_head", "source": str(_ROOT_DIR / "kernels" / "aic" / "rms_lmhead_lm_head.cpp"), "core_type": "aic"},
]
