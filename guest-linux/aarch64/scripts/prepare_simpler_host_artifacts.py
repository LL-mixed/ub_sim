#!/usr/bin/env python3
"""Build HostBuildGraph artifacts for the simulator/simpler C API bridge."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import json
import os
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SIMPLER_CAPI_ABI_VERSION = 5
SIM_AICORE_TLS_ADAPTER_VERSION = 1
HOST_TOOLCHAIN_MARKER = ".sim-host-toolchain.json"
HOST_ARTIFACT_LOCK = ".sim-host-artifacts.lock"

SIM_AICORE_TLS_HEADER = r"""#pragma once

#include <pthread.h>

#if defined(__GNUC__)
#define UB_SIM_HIDDEN __attribute__((visibility("hidden")))
#else
#define UB_SIM_HIDDEN
#endif

extern "C" UB_SIM_HIDDEN int ub_sim_pthread_key_create(
    pthread_key_t *key, void (*destructor)(void *));

#define pthread_key_create ub_sim_pthread_key_create
"""

SIM_AICORE_TLS_SOURCE = r"""#include <array>
#include <cerrno>
#include <cstddef>
#include <pthread.h>

#undef pthread_key_create

namespace {
constexpr std::size_t kMaxTrackedKeys = 64;
std::array<pthread_key_t, kMaxTrackedKeys> g_keys{};
std::size_t g_key_count = 0;
pthread_mutex_t g_key_lock = PTHREAD_MUTEX_INITIALIZER;
}

extern "C" __attribute__((visibility("hidden"))) int
ub_sim_pthread_key_create(
    pthread_key_t *key, void (*destructor)(void *)) {
    const int rc = pthread_key_create(key, destructor);
    if (rc != 0) return rc;

    pthread_mutex_lock(&g_key_lock);
    if (g_key_count == g_keys.size()) {
        pthread_mutex_unlock(&g_key_lock);
        pthread_key_delete(*key);
        return EAGAIN;
    }
    g_keys[g_key_count++] = *key;
    pthread_mutex_unlock(&g_key_lock);
    return 0;
}

__attribute__((destructor)) static void release_tracked_keys() {
    pthread_mutex_lock(&g_key_lock);
    while (g_key_count > 0) {
        pthread_key_delete(g_keys[--g_key_count]);
    }
    pthread_mutex_unlock(&g_key_lock);
}
"""


def atomic_write_bytes(path: Path, contents: bytes) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_bytes(contents)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_write_text(path: Path, contents: str) -> None:
    atomic_write_bytes(path, contents.encode())


def resolve_sim_kernel_compiler(compiler_name: str | None) -> Path | None:
    if not compiler_name:
        return None
    resolved = shutil.which(compiler_name)
    if resolved is not None:
        return Path(resolved)
    if Path(compiler_name).is_absolute():
        return None
    for candidate in (
        Path.home() / ".local/bin" / compiler_name,
        Path.home() / ".local/toolchains/gcc15/bin" / compiler_name,
    ):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


@contextmanager
def artifact_build_lock(simpler_root: Path):
    lock_path = simpler_root / "build" / HOST_ARTIFACT_LOCK
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def host_toolchain_fingerprint() -> dict[str, str]:
    compiler = shutil.which("g++")
    if compiler is None:
        raise SystemExit("Simpler host compiler is unavailable: g++")
    version = subprocess.run(
        [compiler, "-dumpfullversion", "-dumpversion"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    return {"compiler": str(Path(compiler).resolve()), "version": version}


def invalidate_stale_host_toolchain_cache(
    simpler_root: Path, fingerprint: dict[str, str]
) -> bool:
    build_root = simpler_root / "build"
    marker = build_root / HOST_TOOLCHAIN_MARKER
    try:
        current = json.loads(marker.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        current = None
    if current == fingerprint:
        return False

    for generated in (build_root / "cache", build_root / "lib"):
        if generated.exists():
            shutil.rmtree(generated)
    build_root.mkdir(parents=True, exist_ok=True)
    atomic_write_text(marker, json.dumps(fingerprint, sort_keys=True) + "\n")
    return True


@dataclass(frozen=True)
class KernelSpec:
    func_id: int
    source: str
    core_type: str


@dataclass(frozen=True)
class ProfileSpec:
    profile: str
    example: str
    manifest_name: str
    callable_hint: str
    orch_source: str
    orch_function: str
    kernels: tuple[KernelSpec, ...]
    args_template: tuple[dict[str, str], ...]
    generated: bool = False


PROFILE_SPECS = {
    "host_vector": ProfileSpec(
        profile="HostVector",
        example="vector_example",
        manifest_name="host_vector_manifest.json",
        callable_hint="host_vector_example",
        orch_source="kernels/orchestration/example_orch.cpp",
        orch_function="aicpu_orchestration_entry",
        kernels=(
            KernelSpec(0, "kernels/aiv/kernel_add.cpp", "aiv"),
            KernelSpec(1, "kernels/aiv/kernel_add_scalar.cpp", "aiv"),
            KernelSpec(2, "kernels/aiv/kernel_mul.cpp", "aiv"),
        ),
        args_template=(
            {"kind": "input", "name": "a"},
            {"kind": "input", "name": "b"},
            {"kind": "output", "name": "f"},
        ),
    ),
    "host_matmul": ProfileSpec(
        profile="HostMatmul",
        example="matmul",
        manifest_name="host_matmul_manifest.json",
        callable_hint="host_matmul_example",
        orch_source="kernels/orchestration/matmul_orch.cpp",
        orch_function="aicpu_orchestration_entry",
        kernels=(
            KernelSpec(0, "kernels/aiv/kernel_log_sqrt.cpp", "aiv"),
            KernelSpec(1, "kernels/aic/kernel_matmul.cpp", "aic"),
            KernelSpec(2, "kernels/aiv/kernel_add_exp.cpp", "aiv"),
        ),
        args_template=(
            {"kind": "input", "name": "a"},
            {"kind": "input", "name": "w1"},
            {"kind": "input", "name": "w2"},
            {"kind": "output", "name": "f"},
        ),
    ),
    "host_gemm": ProfileSpec(
        profile="HostGemm",
        example="generated_host_gemm",
        manifest_name="host_gemm_manifest.json",
        callable_hint="host_gemm",
        orch_source="host_gemm_orch.cpp",
        orch_function="build_gemm_graph",
        kernels=(KernelSpec(0, "host_gemm_kernel.cpp", "aic"),),
        args_template=(
            {"kind": "input", "name": "a"},
            {"kind": "input", "name": "b"},
            {"kind": "output", "name": "c"},
            {"kind": "scalar_u64", "name": "m"},
            {"kind": "scalar_u64", "name": "k"},
            {"kind": "scalar_u64", "name": "n"},
        ),
        generated=True,
    ),
    "host_fp32_gemm": ProfileSpec(
        profile="HostGemm",
        example="generated_host_fp32_gemm",
        manifest_name="host_fp32_gemm_manifest.json",
        callable_hint="host_fp32_gemm",
        orch_source="host_fp32_gemm_orch.cpp",
        orch_function="build_fp32_gemm_graph",
        kernels=(KernelSpec(0, "host_fp32_gemm_kernel.cpp", "aic"),),
        args_template=(
            {"kind": "input", "name": "a"},
            {"kind": "input", "name": "b"},
            {"kind": "output", "name": "c"},
            {"kind": "scalar_u64", "name": "m"},
            {"kind": "scalar_u64", "name": "k"},
            {"kind": "scalar_u64", "name": "n"},
        ),
        generated=True,
    ),
    "host_quantized_gemm": ProfileSpec(
        profile="HostQuantizedGemm",
        example="generated_host_quantized_gemm",
        manifest_name="host_quantized_gemm_manifest.json",
        callable_hint="host_quantized_gemm",
        orch_source="host_quantized_gemm_orch.cpp",
        orch_function="build_quantized_gemm_graph",
        kernels=(KernelSpec(0, "host_quantized_gemm_kernel.cpp", "aic"),),
        args_template=(
            {"kind": "input", "name": "a"},
            {"kind": "input", "name": "b"},
            {"kind": "output", "name": "c"},
            {"kind": "scalar_u64", "name": "m"},
            {"kind": "scalar_u64", "name": "k"},
            {"kind": "scalar_u64", "name": "n"},
        ),
        generated=True,
    ),
    "host_fp8_gemm": ProfileSpec(
        profile="HostFp8Gemm",
        example="generated_host_fp8_gemm",
        manifest_name="host_fp8_gemm_manifest.json",
        callable_hint="host_fp8_gemm",
        orch_source="host_fp8_gemm_orch.cpp",
        orch_function="build_fp8_gemm_graph",
        kernels=(KernelSpec(0, "host_fp8_gemm_kernel.cpp", "aic"),),
        args_template=(
            {"kind": "input", "name": "activation_fp8"},
            {"kind": "input", "name": "weight_fp8"},
            {"kind": "input", "name": "activation_scale_ue8m0"},
            {"kind": "input", "name": "weight_scale_ue8m0"},
            {"kind": "output", "name": "output_fp32"},
            {"kind": "scalar_u64", "name": "m"},
            {"kind": "scalar_u64", "name": "k"},
            {"kind": "scalar_u64", "name": "n"},
        ),
        generated=True,
    ),
    "host_fp4_gemm": ProfileSpec(
        profile="HostFp4Gemm",
        example="generated_host_fp4_gemm",
        manifest_name="host_fp4_gemm_manifest.json",
        callable_hint="host_fp4_gemm",
        orch_source="host_fp4_gemm_orch.cpp",
        orch_function="build_fp4_gemm_graph",
        kernels=(KernelSpec(0, "host_fp4_gemm_kernel.cpp", "aic"),),
        args_template=(
            {"kind": "input", "name": "activation_fp8"},
            {"kind": "input", "name": "weight_fp4_lowered_fp8"},
            {"kind": "input", "name": "activation_scale_ue8m0"},
            {"kind": "input", "name": "weight_scale_ue8m0_per_32k"},
            {"kind": "output", "name": "output_fp32"},
            {"kind": "scalar_u64", "name": "m"},
            {"kind": "scalar_u64", "name": "k"},
            {"kind": "scalar_u64", "name": "n"},
        ),
        generated=True,
    ),
    "host_q8_block_dot": ProfileSpec(
        profile="HostQuantizedGemm",
        example="generated_host_q8_block_dot",
        manifest_name="host_q8_block_dot_manifest.json",
        callable_hint="host_q8_block_dot",
        orch_source="host_q8_block_dot_orch.cpp",
        orch_function="build_q8_block_dot_graph",
        kernels=(KernelSpec(0, "host_q8_block_dot_kernel.cpp", "aic"),),
        args_template=(
            {"kind": "input", "name": "activation_q8"},
            {"kind": "input", "name": "weight_q8"},
            {"kind": "output", "name": "dot_i32"},
            {"kind": "scalar_u64", "name": "m"},
            {"kind": "scalar_u64", "name": "k"},
            {"kind": "scalar_u64", "name": "n"},
        ),
        generated=True,
    ),
    "host_engram_context": ProfileSpec(
        profile="HostEngramContext",
        example="generated_host_engram_context",
        manifest_name="host_engram_context_manifest.json",
        callable_hint="host_engram_context_example",
        orch_source="host_engram_context_orch.cpp",
        orch_function="build_engram_context_graph",
        kernels=(KernelSpec(0, "host_engram_context_noop.cpp", "aiv"),),
        args_template=(
            {"kind": "input", "name": "table"},
            {"kind": "input", "name": "indices"},
            {"kind": "input", "name": "hidden"},
            {"kind": "input", "name": "gate_weight"},
            {"kind": "inout", "name": "output"},
            {"kind": "inout", "name": "gate_state"},
            {"kind": "scalar_u64", "name": "batch"},
            {"kind": "scalar_u64", "name": "table_rows"},
            {"kind": "scalar_u64", "name": "hidden_size"},
            {"kind": "scalar_u64", "name": "chunk_offset"},
            {"kind": "scalar_u64", "name": "chunk_elems"},
            {"kind": "scalar_f32_bits", "name": "bias"},
        ),
        generated=True,
    ),
    "host_deepseek_vector": ProfileSpec(
        profile="HostVector",
        example="generated_host_deepseek_vector",
        manifest_name="host_deepseek_vector_manifest.json",
        callable_hint="host_deepseek_vector",
        orch_source="host_deepseek_vector_orch.cpp",
        orch_function="build_deepseek_vector_graph",
        kernels=(KernelSpec(0, "host_deepseek_vector_kernel.cpp", "aiv"),),
        args_template=(
            {"kind": "input", "name": "input0"},
            {"kind": "input", "name": "input1"},
            {"kind": "input", "name": "input2"},
            {"kind": "output", "name": "output"},
            {"kind": "scalar_u64", "name": "operation"},
            {"kind": "scalar_u64", "name": "input0_elements"},
            {"kind": "scalar_u64", "name": "input1_elements"},
            {"kind": "scalar_u64", "name": "input2_elements"},
            {"kind": "scalar_u64", "name": "output_elements"},
            {"kind": "scalar_u64", "name": "parameter0"},
            {"kind": "scalar_u64", "name": "parameter1"},
            {"kind": "scalar_u64", "name": "parameter2"},
            {"kind": "scalar_u64", "name": "parameter3"},
            {"kind": "scalar_f32_bits", "name": "float_parameter0"},
            {"kind": "scalar_f32_bits", "name": "float_parameter1"},
        ),
        generated=True,
    ),
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def default_simpler_root() -> Path:
    workspace_simpler = repo_root().parent / "modules" / "simpler"
    if workspace_simpler.exists():
        return workspace_simpler
    return repo_root() / "vendor" / "simpler"


def default_pto_isa_root() -> Path:
    workspace_pto_isa = repo_root().parent / "modules" / "pto-isa"
    if workspace_pto_isa.exists():
        return workspace_pto_isa
    return repo_root() / "vendor" / "pto-isa"


def resolve_pto_isa_root(simpler_root: Path, explicit: str | None) -> Path:
    candidates = []
    if explicit:
        candidates.append(Path(explicit).expanduser().resolve())
    if os.environ.get("PTO_ISA_ROOT"):
        candidates.append(Path(os.environ["PTO_ISA_ROOT"]).expanduser().resolve())
    candidates.extend(
        [
            default_pto_isa_root().resolve(),
            (simpler_root.parent / "pto-isa").resolve(),
            (simpler_root.parent.parent / "modules" / "pto-isa").resolve(),
        ]
    )
    for candidate in candidates:
        if (candidate / "include" / "pto" / "pto-inst.hpp").exists():
            return candidate
    tried = ", ".join(str(candidate) for candidate in candidates)
    raise SystemExit(f"PTO_ISA_ROOT could not be resolved; tried: {tried}")


def resolve_example_root(simpler_root: Path, spec: ProfileSpec) -> Path:
    if spec.generated:
        return Path(f"generated:{spec.example}")
    candidates = [
        simpler_root / "examples" / "a2a3" / "host_build_graph" / spec.example,
        simpler_root / "tests" / "st" / "a2a3" / "host_build_graph" / spec.example,
    ]
    for candidate in candidates:
        if (candidate / spec.orch_source).exists():
            return candidate
    tried = ", ".join(str(candidate) for candidate in candidates)
    raise SystemExit(f"{spec.example} HostBuildGraph sources not found; tried: {tried}")


def path_is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def prefer_explicit_simpler_root(simpler_root: Path) -> None:
    package_root = (simpler_root / "simpler_setup").resolve()

    retained_finders = []
    for finder in sys.meta_path:
        known_sources = getattr(finder, "known_source_files", None)
        package_source = (
            known_sources.get("simpler_setup")
            if isinstance(known_sources, dict)
            else None
        )
        if package_source is not None and not path_is_within(
            Path(package_source).resolve(), package_root
        ):
            continue
        retained_finders.append(finder)
    sys.meta_path[:] = retained_finders

    for module_name, module in list(sys.modules.items()):
        if module_name != "simpler_setup" and not module_name.startswith(
            "simpler_setup."
        ):
            continue
        module_file = getattr(module, "__file__", None)
        if module_file is not None and not path_is_within(
            Path(module_file).resolve(), package_root
        ):
            del sys.modules[module_name]


def load_simpler_build_api(simpler_root: Path):
    prefer_explicit_simpler_root(simpler_root)
    sys.path.insert(0, str(simpler_root))
    sys.path.insert(0, str(simpler_root / "python"))
    try:
        from simpler_setup.kernel_compiler import KernelCompiler  # type: ignore
        from simpler_setup.runtime_builder import RuntimeBuilder  # type: ignore

        return RuntimeBuilder, KernelCompiler, "setup"
    except ModuleNotFoundError:
        from kernel_compiler import KernelCompiler  # type: ignore
        from runtime_builder import RuntimeBuilder  # type: ignore

        return RuntimeBuilder, KernelCompiler, "legacy"


def configure_sim_kernel_libgcc(
    kernel_compiler, build_dir: Path, linkage: str
) -> Path | None:
    if linkage == "shared":
        return None
    if linkage != "static":
        raise ValueError(f"unsupported sim kernel libgcc linkage: {linkage}")

    toolchain = getattr(kernel_compiler, "gxx15", None)
    compiler_name = getattr(toolchain, "cxx_path", None)
    compiler_path = resolve_sim_kernel_compiler(compiler_name)
    if compiler_path is None:
        raise SystemExit(
            "simpler simulation kernel compiler is unavailable: "
            f"{compiler_name or 'g++-15'}"
        )

    wrapper_dir = build_dir / "toolchain"
    wrapper_dir.mkdir(parents=True, exist_ok=True)
    wrapper_path = wrapper_dir / "g++-15-static-libgcc"
    wrapper_path.write_text(
        "#!/bin/sh\n"
        f"exec {shlex.quote(str(compiler_path))} "
        "-static-libgcc -static-libstdc++ \"$@\"\n"
    )
    wrapper_path.chmod(0o755)
    toolchain.cxx_path = str(wrapper_path)
    return wrapper_path


def sim_aicore_arch(platform: str) -> str | None:
    if not platform.endswith("sim"):
        return None
    if platform.startswith("a2a3"):
        return "a2a3"
    if platform.startswith("a5"):
        return "a5"
    raise ValueError(f"unsupported simpler simulation platform: {platform}")


def sim_aicore_tls_policy(simpler_root: Path, platform: str) -> str:
    arch = sim_aicore_arch(platform)
    if arch is None:
        return "not-applicable"
    kernel_source = (
        simpler_root / f"src/{arch}/platform/sim/aicore/kernel.cpp"
    )
    source = kernel_source.read_text()
    if "delete_tls_keys" in source and "pthread_key_delete" in source:
        return "simpler-native"
    return f"ub-sim-adapter-v{SIM_AICORE_TLS_ADAPTER_VERSION}"


def write_sim_aicore_tls_adapter(build_dir: Path) -> tuple[Path, Path]:
    adapter_dir = build_dir / "sim_aicore_tls_adapter"
    adapter_dir.mkdir(parents=True, exist_ok=True)
    header = adapter_dir / "sim_aicore_tls_adapter.h"
    source = adapter_dir / "sim_aicore_tls_adapter.cpp"
    atomic_write_text(header, SIM_AICORE_TLS_HEADER)
    atomic_write_text(source, SIM_AICORE_TLS_SOURCE)
    return adapter_dir, header


class SimAicoreRuntimeCompilerAdapter:
    def __init__(self, delegate, source_dir: Path, forced_include: Path):
        self._delegate = delegate
        self._source_dir = source_dir
        self._forced_include = forced_include

    def __getattr__(self, name):
        return getattr(self._delegate, name)

    def compile(self, target_platform, include_dirs, source_dirs, **kwargs):
        if target_platform != "aicore":
            return self._delegate.compile(
                target_platform, include_dirs, source_dirs, **kwargs
            )

        cmake_defines = dict(kwargs.get("cmake_defines") or {})
        existing_flags = cmake_defines.get("CMAKE_CXX_FLAGS", "").strip()
        forced_include = f"-include {shlex.quote(str(self._forced_include))}"
        cmake_defines["CMAKE_CXX_FLAGS"] = " ".join(
            part for part in (existing_flags, forced_include) if part
        )
        kwargs["cmake_defines"] = cmake_defines
        return self._delegate.compile(
            target_platform,
            include_dirs,
            [*source_dirs, str(self._source_dir)],
            **kwargs,
        )


def configure_sim_aicore_tls_adapter(
    builder, build_dir: Path, simpler_root: Path, platform: str
) -> str:
    policy = sim_aicore_tls_policy(simpler_root, platform)
    if not policy.startswith("ub-sim-adapter-"):
        return policy

    source_dir, forced_include = write_sim_aicore_tls_adapter(build_dir)
    runtime_compiler = getattr(builder, "_runtime_compiler", None)
    if runtime_compiler is None:
        raise RuntimeError("Simpler RuntimeBuilder has no runtime compiler")
    builder._runtime_compiler = SimAicoreRuntimeCompilerAdapter(
        runtime_compiler, source_dir, forced_include
    )
    return policy


def read_runtime_binaries(builder, api_kind: str, runtime_name: str, build_dir: Path):
    if api_kind == "setup":
        runtime_binaries = builder.get_binaries(runtime_name, build=True)
        sim_context = (
            runtime_binaries.sim_context_path.read_bytes()
            if runtime_binaries.sim_context_path is not None
            else None
        )
        simpler_log_path = getattr(runtime_binaries, "simpler_log_path", None)
        simpler_log = simpler_log_path.read_bytes() if simpler_log_path is not None else None
        return (
            runtime_binaries.host_path.read_bytes(),
            runtime_binaries.aicpu_path.read_bytes(),
            runtime_binaries.aicore_path.read_bytes(),
            sim_context,
            simpler_log,
        )
    host_binary, aicpu_binary, aicore_binary = builder.build(runtime_name, str(build_dir))
    return host_binary, aicpu_binary, aicore_binary, None, None


def runtime_binaries_for_manifest(
    builder,
    api_kind: str,
    runtime_name: str,
    build_dir: Path,
    reuse_runtime: dict | None,
):
    if reuse_runtime is not None:
        return None, None, None, None, None
    return read_runtime_binaries(builder, api_kind, runtime_name, build_dir)


def load_reuse_runtime_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text())
    abi_version = manifest.get("simpler_capi_abi_version")
    if abi_version != SIMPLER_CAPI_ABI_VERSION:
        raise SystemExit(
            "reuse runtime manifest has stale simpler C API ABI: "
            f"got={abi_version!r} expected={SIMPLER_CAPI_ABI_VERSION} "
            f"path={path}"
        )
    try:
        runtime = manifest["simpler_runtime"]
    except KeyError as err:
        raise SystemExit(f"reuse runtime manifest is incomplete: {path}") from err
    if not runtime.get("sim_aicore_tls_policy"):
        raise SystemExit(
            f"reuse runtime manifest has no AICore TLS policy: {path}"
        )
    return runtime


def write_vector_kernel_source(build_dir: Path, func_id: int, tile_rows: int, tile_cols: int) -> Path | None:
    op_name = {
        0: "TADD(dstTile, src0Tile, src1Tile);",
        1: "TADDS(dstTile, src0Tile, scalar);",
        2: "TMUL(dstTile, src0Tile, src1Tile);",
    }.get(func_id)
    if op_name is None:
        return None

    second_input = ""
    scalar_input = ""
    load_second = ""
    if func_id in (0, 2):
        second_input = """\
    __gm__ ChipTensor* src1_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ ChipTensor* out_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[2]);
    __gm__ float* src1 =
        reinterpret_cast<__gm__ float*>(src1_tensor->buffer.addr) +
        src1_tensor->start_offset;
    __gm__ float* out =
        reinterpret_cast<__gm__ float*>(out_tensor->buffer.addr) +
        out_tensor->start_offset;
"""
        load_second = """\
    TileData src1Tile(vRows, vCols);
    TASSIGN(src1Tile, 0x10000);
    GlobalData src1Global(src1);
    TLOAD(src1Tile, src1Global);
"""
    else:
        scalar_input = """\
    __gm__ ChipTensor* out_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    union {
        uint64_t u64;
        float f32;
    } converter;
    converter.u64 = args[2];
    float scalar = converter.f32;
    __gm__ float* out =
        reinterpret_cast<__gm__ float*>(out_tensor->buffer.addr) +
        out_tensor->start_offset;
"""

    source = build_dir / f"vector_kernel_func_{func_id}.cpp"
    source.write_text(
        f"""\
#include <cstdint>
#include <pto/pto-inst.hpp>

#include "tensor.h"

using namespace pto;

#include "pipe_sync.h"

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {{
    __gm__ ChipTensor* src0_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ float* src0 =
        reinterpret_cast<__gm__ float*>(src0_tensor->buffer.addr) +
        src0_tensor->start_offset;
{second_input}{scalar_input}
    constexpr int kTRows_ = {tile_rows};
    constexpr int kTCols_ = {tile_cols};
    constexpr int vRows = {tile_rows};
    constexpr int vCols = {tile_cols};

    using DynShapeDim5 = Shape<1, 1, 1, vRows, vCols>;
    using DynStridDim5 = Stride<1, 1, 1, kTCols_, 1>;
    using GlobalData = GlobalTensor<float, DynShapeDim5, DynStridDim5>;
    using TileData = Tile<TileType::Vec, float, kTRows_, kTCols_, BLayout::RowMajor, -1, -1>;

    TileData src0Tile(vRows, vCols);
    TileData dstTile(vRows, vCols);
    TASSIGN(src0Tile, 0x0);
    TASSIGN(dstTile, 0x20000);
    GlobalData src0Global(src0);
    GlobalData dstGlobal(out);
    TLOAD(src0Tile, src0Global);
{load_second}
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    {op_name}
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(dstGlobal, dstTile);

    pipe_sync();
}}
"""
    )
    return source


def write_batched_matmul_orchestration(build_dir: Path, tile_batch: int) -> Path:
    source = build_dir / "matmul_batched_orch.cpp"
    source.write_text(
        f"""\
#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" {{

__attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs& orch_args) {{
    (void)orch_args;
    return PTO2OrchestrationConfig{{.expected_arg_count = 10}};
}}

__attribute__((visibility("default"))) void
aicpu_orchestration_entry(const ChipTaskArgs& orch_args) {{
    if (orch_args.tensor_count() != 4 || orch_args.scalar_count() != 6) {{
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS,
            "expected 4 tensor args and 6 scalar args");
        return;
    }}

    const ChipTensor& a = orch_args.tensor(0).ref();
    const ChipTensor& w1 = orch_args.tensor(1).ref();
    const ChipTensor& w2 = orch_args.tensor(2).ref();
    const ChipTensor& f = orch_args.tensor(3).ref();
    const int tile_size = static_cast<int>(orch_args.scalar(4));
    const int tile_batch = static_cast<int>(orch_args.scalar(5));

    if (tile_batch <= 0 || tile_batch > {tile_batch}) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "invalid tile_batch");
        return;
    }}
    if (tile_size <= 0) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "invalid tile_size");
        return;
    }}
    const uint64_t elements =
        static_cast<uint64_t>(tile_size) * static_cast<uint64_t>(tile_batch);
    if (a.shapes[0] < elements || w1.shapes[0] < elements ||
        w2.shapes[0] < elements || f.shapes[0] < elements) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "tensor payload too short");
        return;
    }}

    uint32_t shape[1] = {{static_cast<uint32_t>(elements)}};
    TensorCreateInfo b_info(shape, 1, DataType::FLOAT16);
    TensorCreateInfo c_info(shape, 1, DataType::FLOAT32);
    TensorCreateInfo d_info(shape, 1, DataType::FLOAT32);

    CoreTaskArgs args_t0;
    args_t0.add_input(a);
    args_t0.add_output(b_info);
    args_t0.add_scalar(tile_size);
    TaskOutputTensors b_outputs = rt_submit_aiv_task(0, args_t0);
    if (rt_is_fatal()) return;
    const ChipTensor& b = b_outputs.get_ref(0);

    CoreTaskArgs args_t1;
    args_t1.add_input(b, w1);
    args_t1.add_output(c_info);
    args_t1.add_scalar(tile_size);
    TaskOutputTensors c_outputs = rt_submit_aic_task(1, args_t1);
    if (rt_is_fatal()) return;
    const ChipTensor& c = c_outputs.get_ref(0);

    CoreTaskArgs args_t2;
    args_t2.add_input(b, w2);
    args_t2.add_output(d_info);
    args_t2.add_scalar(tile_size);
    TaskOutputTensors d_outputs = rt_submit_aic_task(1, args_t2);
    if (rt_is_fatal()) return;
    const ChipTensor& d = d_outputs.get_ref(0);

    CoreTaskArgs args_t3;
    args_t3.add_input(c, d);
    args_t3.add_output(f);
    args_t3.add_scalar(tile_size);
    rt_submit_aiv_task(2, args_t3);
}}

}}  // extern "C"
"""
    )
    return source


def write_batched_matmul_kernel_source(build_dir: Path, func_id: int, tile_batch: int) -> Path | None:
    common_prefix = """\
#include <cstdint>
#include <pto/pto-inst.hpp>

#include "tensor.h"

using namespace pto;

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

"""
    if func_id == 0:
        body = f"""\
extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {{
    __gm__ ChipTensor* src_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ ChipTensor* out_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ half* src = reinterpret_cast<__gm__ half*>(src_tensor->buffer.addr) +
        src_tensor->start_offset;
    __gm__ half* out = reinterpret_cast<__gm__ half*>(out_tensor->buffer.addr) +
        out_tensor->start_offset;

    constexpr int kTRows_ = 128;
    constexpr int kTCols_ = 128;
    constexpr int vRows = 128;
    constexpr int vCols = 128;
    constexpr int tileElems = vRows * vCols;

    using DynShapeDim5Half = Shape<1, 1, 1, vRows, vCols>;
    using DynStridDim5Half = Stride<1, 1, 1, kTCols_, 1>;
    using GlobalDataHalf = GlobalTensor<half, DynShapeDim5Half, DynStridDim5Half>;
    using TileDataHalf = Tile<TileType::Vec, half, kTRows_, kTCols_, BLayout::RowMajor, -1, -1>;

    TileDataHalf srcTile(vRows, vCols);
    TileDataHalf tmpTile(vRows, vCols);
    TileDataHalf dstTile(vRows, vCols);
    TASSIGN(srcTile, 0x0);
    TASSIGN(tmpTile, 0x10000);
    TASSIGN(dstTile, 0x20000);

    #pragma unroll
    for (int tile = 0; tile < {tile_batch}; ++tile) {{
        GlobalDataHalf srcGlobal(src + tile * tileElems);
        GlobalDataHalf dstGlobal(out + tile * tileElems);
        TLOAD(srcTile, srcGlobal);
        set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
        wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
        TLOG(tmpTile, srcTile);
        TSQRT(dstTile, tmpTile);
        set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
        wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
        TSTORE(dstGlobal, dstTile);
    }}
}}
"""
    elif func_id == 1:
        body = f"""\
extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {{
    __gm__ ChipTensor* src0_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ ChipTensor* src1_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ ChipTensor* out_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[2]);
    __gm__ half* src0 = reinterpret_cast<__gm__ half*>(src0_tensor->buffer.addr) +
        src0_tensor->start_offset;
    __gm__ half* src1 = reinterpret_cast<__gm__ half*>(src1_tensor->buffer.addr) +
        src1_tensor->start_offset;
    __gm__ float* out = reinterpret_cast<__gm__ float*>(out_tensor->buffer.addr) +
        out_tensor->start_offset;

    constexpr int validM = 128;
    constexpr int validK = 128;
    constexpr int validN = 128;
    constexpr int M = 128;
    constexpr int K = 128;
    constexpr int N = 128;
    constexpr int halfTileElems = validM * validK;
    constexpr int floatTileElems = validM * validN;

    using GlobalDataSrc0 = GlobalTensor<half, Shape<1, 1, 1, validM, validK>,
        Stride<validM * validK, validM * validK, validM * validK, validK, 1>>;
    using GlobalDataSrc1 = GlobalTensor<half, Shape<1, 1, 1, validK, validN>,
        Stride<validK * validN, validK * validN, validK * validN, validN, 1>>;
    using GlobalDataOut = GlobalTensor<float, Shape<1, 1, 1, validM, validN>,
        Stride<validM * validN, validM * validN, validM * validN, validN, 1>>;

    using TileMatAData = Tile<TileType::Mat, half, M, K, BLayout::ColMajor, validM, validK, SLayout::RowMajor, 512>;
    using TileMatBData = Tile<TileType::Mat, half, K, N, BLayout::ColMajor, validK, validN, SLayout::RowMajor, 512>;
    using LeftTile = TileLeft<half, M, K, validM, validK>;
    using RightTile = TileRight<half, K, N, validK, validN>;
    using AccTile = TileAcc<float, M, N, validM, validN>;

    TileMatAData aMatTile;
    TileMatBData bMatTile;
    TASSIGN(aMatTile, 0x0);
    TASSIGN(bMatTile, 0x20000);
    LeftTile aTile;
    RightTile bTile;
    AccTile cTile;
    TASSIGN(aTile, 0x0);
    TASSIGN(bTile, 0x0);
    TASSIGN(cTile, 0x0);

    #pragma unroll
    for (int tile = 0; tile < {tile_batch}; ++tile) {{
        GlobalDataSrc0 src0Global(src0 + tile * halfTileElems);
        GlobalDataSrc1 src1Global(src1 + tile * halfTileElems);
        GlobalDataOut dstGlobal(out + tile * floatTileElems);
        TLOAD(aMatTile, src0Global);
        TLOAD(bMatTile, src1Global);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
        TMOV(aTile, aMatTile);
        TMOV(bTile, bMatTile);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        TMATMUL(cTile, aTile, bTile);
        set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
        wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
        TSTORE(dstGlobal, cTile);
    }}
}}
"""
    elif func_id == 2:
        body = f"""\
extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {{
    __gm__ ChipTensor* src0_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ ChipTensor* src1_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ ChipTensor* out_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[2]);
    __gm__ float* src0 = reinterpret_cast<__gm__ float*>(src0_tensor->buffer.addr) +
        src0_tensor->start_offset;
    __gm__ float* src1 = reinterpret_cast<__gm__ float*>(src1_tensor->buffer.addr) +
        src1_tensor->start_offset;
    __gm__ float* out = reinterpret_cast<__gm__ float*>(out_tensor->buffer.addr) +
        out_tensor->start_offset;

    constexpr int kTRows_ = 128;
    constexpr int kTCols_ = 128;
    constexpr int vRows = 128;
    constexpr int vCols = 128;
    constexpr int tileElems = vRows * vCols;

    using DynShapeDim5 = Shape<1, 1, 1, vRows, vCols>;
    using DynStridDim5 = Stride<1, 1, 1, kTCols_, 1>;
    using GlobalData = GlobalTensor<float, DynShapeDim5, DynStridDim5>;
    using TileData = Tile<TileType::Vec, float, kTRows_, kTCols_, BLayout::RowMajor, -1, -1>;

    TileData src0Tile(vRows, vCols);
    TileData src1Tile(vRows, vCols);
    TileData dstTile(vRows, vCols);
    TASSIGN(src0Tile, 0x0);
    TASSIGN(src1Tile, 0x10000);
    TASSIGN(dstTile, 0x20000);

    #pragma unroll
    for (int tile = 0; tile < {tile_batch}; ++tile) {{
        GlobalData src0Global(src0 + tile * tileElems);
        GlobalData src1Global(src1 + tile * tileElems);
        GlobalData dstGlobal(out + tile * tileElems);
        TLOAD(src0Tile, src0Global);
        TLOAD(src1Tile, src1Global);
        set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
        wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
        TADD(src0Tile, src0Tile, src1Tile);
        TEXP(dstTile, src0Tile);
        set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
        wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
        TSTORE(dstGlobal, dstTile);
    }}
}}
"""
    else:
        return None
    source = build_dir / f"batched_kernel_func_{func_id}.cpp"
    source.write_text(common_prefix + body)
    return source


def write_host_gemm_orchestration(
    build_dir: Path,
    m: int,
    k: int,
    n: int,
    *,
    quantized: bool = False,
    fp32: bool = False,
) -> Path:
    if quantized and fp32:
        raise ValueError("GEMM cannot be both quantized and FP32")
    source_name = (
        "host_quantized_gemm_orch.cpp"
        if quantized
        else "host_fp32_gemm_orch.cpp" if fp32 else "host_gemm_orch.cpp"
    )
    function_name = (
        "build_quantized_gemm_graph"
        if quantized
        else "build_fp32_gemm_graph" if fp32 else "build_gemm_graph"
    )
    input_type = "int8_t" if quantized else "float" if fp32 else "uint16_t"
    output_type = "int32_t" if quantized else "float"
    source = build_dir / source_name
    source.write_text(
        f"""\
#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" {{

__attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs& orch_args) {{
    (void)orch_args;
    return PTO2OrchestrationConfig{{.expected_arg_count = 6}};
}}

__attribute__((visibility("default"))) void
{function_name}(const ChipTaskArgs& orch_args) {{
    if (orch_args.tensor_count() != 3 || orch_args.scalar_count() != 3) {{
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS,
            "expected 3 tensor args and 3 scalar args");
        return;
    }}

    constexpr uint64_t kM = {m};
    constexpr uint64_t kK = {k};
    constexpr uint64_t kN = {n};
    constexpr uint64_t kTile = 128;
    const uint64_t m = orch_args.scalar(0);
    const uint64_t k = orch_args.scalar(1);
    const uint64_t n = orch_args.scalar(2);
    if (m != kM || k != kK || n != kN) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "artifact geometry mismatch");
        return;
    }}
    if ((m % kTile) != 0 || (k % kTile) != 0 || (n % kTile) != 0) {{
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS, "dimensions must be 128-aligned");
        return;
    }}

    const uint64_t elements_a = m * k;
    const uint64_t elements_b = k * n;
    const uint64_t elements_c = m * n;
    const ChipTensor& a = orch_args.tensor(0).ref();
    const ChipTensor& b = orch_args.tensor(1).ref();
    const ChipTensor& c = orch_args.tensor(2).ref();
    if (a.shapes[0] < elements_a || b.shapes[0] < elements_b ||
        c.shapes[0] < elements_c) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "tensor payload too short");
        return;
    }}

    for (uint64_t tile_m = 0; tile_m < m / kTile; ++tile_m) {{
        for (uint64_t tile_n = 0; tile_n < n / kTile; ++tile_n) {{
            CoreTaskArgs task_args;
            task_args.add_input(a, b);
            task_args.add_output(c);
            task_args.add_scalar(tile_m, tile_n);
            rt_submit_aic_task(0, task_args);
        }}
    }}
}}

}}  // extern "C"
"""
    )
    return source


def write_host_quantized_gemm_orchestration(
    build_dir: Path, m: int, k: int, n: int
) -> Path:
    return write_host_gemm_orchestration(build_dir, m, k, n, quantized=True)


def write_host_fp32_gemm_orchestration(
    build_dir: Path, m: int, k: int, n: int
) -> Path:
    return write_host_gemm_orchestration(build_dir, m, k, n, fp32=True)


def write_host_fp8_gemm_orchestration(
    build_dir: Path, m: int, k: int, n: int
) -> Path:
    source = build_dir / "host_fp8_gemm_orch.cpp"
    source.write_text(
        f"""\
#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" {{

__attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs& orch_args) {{
    (void)orch_args;
    return PTO2OrchestrationConfig{{.expected_arg_count = 8}};
}}

__attribute__((visibility("default"))) void
build_fp8_gemm_graph(const ChipTaskArgs& orch_args) {{
    if (orch_args.tensor_count() != 5 || orch_args.scalar_count() != 3) {{
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS,
            "expected 5 tensor args and 3 scalar args");
        return;
    }}

    constexpr uint64_t kM = {m};
    constexpr uint64_t kK = {k};
    constexpr uint64_t kN = {n};
    constexpr uint64_t kScaleGroup = 32;
    if (orch_args.scalar(0) != kM || orch_args.scalar(1) != kK ||
        orch_args.scalar(2) != kN || kM != 128 || kK != 128 || kN != 128) {{
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS,
            "artifact geometry must be 128x128x128");
        return;
    }}

    constexpr uint64_t kActivationElements = kM * kK;
    constexpr uint64_t kWeightElements = kK * kN;
    constexpr uint64_t kActivationScaleElements = kM * kK / kScaleGroup;
    constexpr uint64_t kWeightScaleElements = kK * kN / kScaleGroup;
    constexpr uint64_t kOutputElements = kM * kN;
    const ChipTensor& activation = orch_args.tensor(0).ref();
    const ChipTensor& weight = orch_args.tensor(1).ref();
    const ChipTensor& activation_scale = orch_args.tensor(2).ref();
    const ChipTensor& weight_scale = orch_args.tensor(3).ref();
    const ChipTensor& output = orch_args.tensor(4).ref();
    if (activation.shapes[0] < kActivationElements ||
        weight.shapes[0] < kWeightElements ||
        activation_scale.shapes[0] < kActivationScaleElements ||
        weight_scale.shapes[0] < kWeightScaleElements ||
        output.shapes[0] < kOutputElements) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "tensor payload too short");
        return;
    }}

    CoreTaskArgs task_args;
    task_args.add_input(activation, weight, activation_scale, weight_scale);
    task_args.add_output(output);
    rt_submit_aic_task(0, task_args);
}}

}}  // extern "C"
"""
    )
    return source


def write_host_fp4_gemm_orchestration(
    build_dir: Path, m: int, k: int, n: int
) -> Path:
    source = write_host_fp8_gemm_orchestration(build_dir, m, k, n)
    text = source.read_text()
    text = text.replace("host_fp8_gemm_orch.cpp", "host_fp4_gemm_orch.cpp")
    text = text.replace("build_fp8_gemm_graph", "build_fp4_gemm_graph")
    text = text.replace(
        " || kM != 128 || kK != 128 || kN != 128", ""
    ).replace(
        "artifact geometry must be 128x128x128",
        "artifact geometry mismatch",
    )
    target = build_dir / "host_fp4_gemm_orch.cpp"
    target.write_text(text)
    source.unlink()
    return target


def write_host_gemm_kernel(
    build_dir: Path,
    m: int,
    k: int,
    n: int,
    *,
    quantized: bool = False,
    fp32: bool = False,
) -> Path:
    if quantized and fp32:
        raise ValueError("GEMM cannot be both quantized and FP32")
    source_name = (
        "host_quantized_gemm_kernel.cpp"
        if quantized
        else "host_fp32_gemm_kernel.cpp" if fp32 else "host_gemm_kernel.cpp"
    )
    input_type = "int8_t" if quantized else "float" if fp32 else "bfloat16_t"
    output_type = "int32_t" if quantized else "float"
    source = build_dir / source_name
    source.write_text(
        f"""\
#include <cstdint>
#include <pto/pto-inst.hpp>

#include "tensor.h"

using namespace pto;

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {{
    __gm__ ChipTensor* a_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ ChipTensor* b_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ ChipTensor* c_tensor = reinterpret_cast<__gm__ ChipTensor*>(args[2]);
    __gm__ {input_type}* a = reinterpret_cast<__gm__ {input_type}*>(a_tensor->buffer.addr) +
        a_tensor->start_offset;
    __gm__ {input_type}* b = reinterpret_cast<__gm__ {input_type}*>(b_tensor->buffer.addr) +
        b_tensor->start_offset;
    __gm__ {output_type}* c = reinterpret_cast<__gm__ {output_type}*>(c_tensor->buffer.addr) +
        c_tensor->start_offset;
    const uint64_t tile_m = static_cast<uint64_t>(args[3]);
    const uint64_t tile_n = static_cast<uint64_t>(args[4]);

    constexpr int M = {m};
    constexpr int K = {k};
    constexpr int N = {n};
    constexpr int TileM = 128;
    constexpr int TileK = 128;
    constexpr int TileN = 128;

    using AValid = TileShape2D<{input_type}, TileM, TileK>;
    using AWhole = BaseShape2D<{input_type}, M, K>;
    using BValid = TileShape2D<{input_type}, TileK, TileN>;
    using BWhole = BaseShape2D<{input_type}, K, N>;
    using CValid = TileShape2D<{output_type}, TileM, TileN>;
    using CWhole = BaseShape2D<{output_type}, M, N>;
    using GlobalA = GlobalTensor<{input_type}, AValid, AWhole>;
    using GlobalB = GlobalTensor<{input_type}, BValid, BWhole>;
    using GlobalC = GlobalTensor<{output_type}, CValid, CWhole>;
    using MatA = Tile<TileType::Mat, {input_type}, TileM, TileK,
                      BLayout::ColMajor, TileM, TileK, SLayout::RowMajor, 512>;
    using MatB = Tile<TileType::Mat, {input_type}, TileK, TileN,
                      BLayout::ColMajor, TileK, TileN, SLayout::RowMajor, 512>;
    using Left = TileLeft<{input_type}, TileM, TileK, TileM, TileK>;
    using Right = TileRight<{input_type}, TileK, TileN, TileK, TileN>;
    using Acc = TileAcc<{output_type}, TileM, TileN, TileM, TileN>;

    MatA a_mat;
    MatB b_mat;
    Left a_tile;
    Right b_tile;
    Acc c_tile;
    TASSIGN(a_mat, 0x0);
    TASSIGN(b_mat, 0x20000);
    TASSIGN(a_tile, 0x0);
    TASSIGN(b_tile, 0x0);
    TASSIGN(c_tile, 0x0);

    for (int k0 = 0; k0 < K; k0 += TileK) {{
        GlobalA a_global(a + tile_m * TileM * K + k0);
        GlobalB b_global(b + k0 * N + tile_n * TileN);
        TLOAD(a_mat, a_global);
        TLOAD(b_mat, b_global);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
        TMOV(a_tile, a_mat);
        TMOV(b_tile, b_mat);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        if (k0 == 0) {{
            TMATMUL(c_tile, a_tile, b_tile);
        }} else {{
            TMATMUL_ACC(c_tile, c_tile, a_tile, b_tile);
        }}
        set_flag(PIPE_M, PIPE_MTE2, EVENT_ID0);
        wait_flag(PIPE_M, PIPE_MTE2, EVENT_ID0);
    }}

    set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    GlobalC c_global(c + tile_m * TileM * N + tile_n * TileN);
    TSTORE(c_global, c_tile);
}}
"""
    )
    return source


def write_host_quantized_gemm_kernel(
    build_dir: Path, m: int, k: int, n: int
) -> Path:
    return write_host_gemm_kernel(build_dir, m, k, n, quantized=True)


def write_host_fp32_gemm_kernel(
    build_dir: Path, m: int, k: int, n: int
) -> Path:
    return write_host_gemm_kernel(build_dir, m, k, n, fp32=True)


def write_host_fp8_gemm_kernel(build_dir: Path, m: int, k: int, n: int) -> Path:
    source = build_dir / "host_fp8_gemm_kernel.cpp"
    source.write_text(
        f"""\
#include <cstdint>
#include <pto/pto-inst.hpp>

#include "tensor.h"

using namespace pto;

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {{
    __gm__ ChipTensor* activation_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ ChipTensor* weight_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ ChipTensor* activation_scale_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[2]);
    __gm__ ChipTensor* weight_scale_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[3]);
    __gm__ ChipTensor* output_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[4]);
    __gm__ float8_e4m3_t* activation =
        reinterpret_cast<__gm__ float8_e4m3_t*>(activation_tensor->buffer.addr) +
        activation_tensor->start_offset;
    __gm__ float8_e4m3_t* weight =
        reinterpret_cast<__gm__ float8_e4m3_t*>(weight_tensor->buffer.addr) +
        weight_tensor->start_offset;
    __gm__ float8_e8m0_t* activation_scale =
        reinterpret_cast<__gm__ float8_e8m0_t*>(activation_scale_tensor->buffer.addr) +
        activation_scale_tensor->start_offset;
    __gm__ float8_e8m0_t* weight_scale =
        reinterpret_cast<__gm__ float8_e8m0_t*>(weight_scale_tensor->buffer.addr) +
        weight_scale_tensor->start_offset;
    __gm__ float* output = reinterpret_cast<__gm__ float*>(output_tensor->buffer.addr) +
        output_tensor->start_offset;

    constexpr int M = {m};
    constexpr int K = {k};
    constexpr int N = {n};
    constexpr int ScaleK = K / 32;

    using GlobalA = GlobalTensor<
        float8_e4m3_t,
        TileShape2D<float8_e4m3_t, M, K, Layout::ND>,
        BaseShape2D<float8_e4m3_t, M, K, Layout::ND>, Layout::ND>;
    using GlobalB = GlobalTensor<
        float8_e4m3_t,
        TileShape2D<float8_e4m3_t, K, N, Layout::DN>,
        BaseShape2D<float8_e4m3_t, K, N, Layout::DN>, Layout::DN>;
    using GlobalScaleA = GlobalTensor<
        float8_e8m0_t,
        TileShape2D<float8_e8m0_t, M, ScaleK, Layout::ND>,
        BaseShape2D<float8_e8m0_t, M, ScaleK, Layout::ND>, Layout::ND>;
    using GlobalScaleB = GlobalTensor<
        float8_e8m0_t,
        TileShape2D<float8_e8m0_t, ScaleK, N, Layout::ND>,
        BaseShape2D<float8_e8m0_t, ScaleK, N, Layout::ND>, Layout::ND>;
    using GlobalC = GlobalTensor<
        float, TileShape2D<float, M, N, Layout::ND>,
        BaseShape2D<float, M, N, Layout::ND>, Layout::ND>;

    using MatA = Tile<TileType::Mat, float8_e4m3_t, M, K,
                      BLayout::ColMajor, M, K, SLayout::RowMajor, 512>;
    using MatB = Tile<TileType::Mat, float8_e4m3_t, K, N,
                      BLayout::ColMajor, K, N, SLayout::RowMajor, 512>;
    using MatScaleA = Tile<TileType::Mat, float8_e8m0_t, M, ScaleK,
                           BLayout::RowMajor, M, ScaleK, SLayout::RowMajor, 32>;
    using MatScaleB = Tile<TileType::Mat, float8_e8m0_t, K, N,
                           BLayout::ColMajor, ScaleK, N, SLayout::ColMajor, 32>;
    using Left = TileLeft<float8_e4m3_t, M, K, M, K>;
    using Right = TileRight<float8_e4m3_t, K, N, K, N>;
    using LeftScale = TileLeftScale<float8_e8m0_t, M, ScaleK, M, ScaleK>;
    using RightScale = TileRightScale<float8_e8m0_t, K, N, ScaleK, N>;
    using Acc = TileAcc<float, M, N, M, N>;

    MatA a_mat;
    MatB b_mat;
    MatScaleA a_scale_mat;
    MatScaleB b_scale_mat;
    Left a_tile;
    Right b_tile;
    LeftScale a_scale_tile;
    RightScale b_scale_tile;
    Acc c_tile;
    size_t addr = 0;
    TASSIGN(a_mat, addr);
    addr += MatA::Numel * sizeof(typename MatA::DType);
    TASSIGN(b_mat, addr);
    addr += MatB::Numel * sizeof(typename MatB::DType);
    TASSIGN(a_scale_mat, addr);
    addr += MatScaleA::Numel * sizeof(typename MatScaleA::DType);
    TASSIGN(b_scale_mat, addr);
    addr += MatScaleB::Numel * sizeof(typename MatScaleB::DType);
    TASSIGN(a_tile, 0x0);
    TASSIGN(b_tile, 0x0);
    TASSIGN(c_tile, 0x0);
    TASSIGN(a_scale_tile, addr);
    addr += LeftScale::Numel * sizeof(typename LeftScale::DType);
    TASSIGN(b_scale_tile, addr);

    GlobalA global_a(activation);
    GlobalB global_b(weight);
    GlobalScaleA global_scale_a(activation_scale);
    GlobalScaleB global_scale_b(weight_scale);
    GlobalC global_c(output);
    TLOAD(a_mat, global_a);
    TLOAD(b_mat, global_b);
    TLOAD(a_scale_mat, global_scale_a);
    TLOAD(b_scale_mat, global_scale_b);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    TMOV(a_tile, a_mat);
    TMOV(b_tile, b_mat);
    TMOV(a_scale_tile, a_scale_mat);
    TMOV(b_scale_tile, b_scale_mat);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    TMATMUL_MX(c_tile, a_tile, a_scale_tile, b_tile, b_scale_tile);
    set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    TSTORE(global_c, c_tile);
}}
"""
    )
    return source


def write_host_fp4_gemm_kernel(build_dir: Path, m: int, k: int, n: int) -> Path:
    source = build_dir / "host_fp4_gemm_kernel.cpp"
    source.write_text(
        f"""\
#include <cstdint>
#include <pto/pto-inst.hpp>

#include "tensor.h"

using namespace pto;

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {{
    __gm__ ChipTensor* activation_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ ChipTensor* weight_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ ChipTensor* activation_scale_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[2]);
    __gm__ ChipTensor* weight_scale_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[3]);
    __gm__ ChipTensor* output_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[4]);
    __gm__ float8_e4m3_t* activation =
        reinterpret_cast<__gm__ float8_e4m3_t*>(activation_tensor->buffer.addr) +
        activation_tensor->start_offset;
    __gm__ float8_e4m3_t* weight =
        reinterpret_cast<__gm__ float8_e4m3_t*>(weight_tensor->buffer.addr) +
        weight_tensor->start_offset;
    __gm__ float8_e8m0_t* activation_scale =
        reinterpret_cast<__gm__ float8_e8m0_t*>(activation_scale_tensor->buffer.addr) +
        activation_scale_tensor->start_offset;
    __gm__ float8_e8m0_t* weight_scale =
        reinterpret_cast<__gm__ float8_e8m0_t*>(weight_scale_tensor->buffer.addr) +
        weight_scale_tensor->start_offset;
    __gm__ float* output = reinterpret_cast<__gm__ float*>(output_tensor->buffer.addr) +
        output_tensor->start_offset;

    constexpr int M = {m};
    constexpr int K = {k};
    constexpr int N = {n};
    constexpr int TileK = 128;
    constexpr int ScaleK = TileK / 32;
    constexpr int FullScaleK = K / 32;

    using GlobalA = GlobalTensor<
        float8_e4m3_t,
        TileShape2D<float8_e4m3_t, M, TileK, Layout::ND>,
        BaseShape2D<float8_e4m3_t, M, K, Layout::ND>, Layout::ND>;
    using GlobalB = GlobalTensor<
        float8_e4m3_t,
        TileShape2D<float8_e4m3_t, TileK, N, Layout::DN>,
        BaseShape2D<float8_e4m3_t, K, N, Layout::DN>, Layout::DN>;
    using GlobalScaleA = GlobalTensor<
        float8_e8m0_t,
        TileShape2D<float8_e8m0_t, M, ScaleK, Layout::ND>,
        BaseShape2D<float8_e8m0_t, M, FullScaleK, Layout::ND>, Layout::ND>;
    using GlobalScaleB = GlobalTensor<
        float8_e8m0_t,
        TileShape2D<float8_e8m0_t, ScaleK, N, Layout::ND>,
        BaseShape2D<float8_e8m0_t, FullScaleK, N, Layout::ND>, Layout::ND>;
    using GlobalC = GlobalTensor<
        float, TileShape2D<float, M, N, Layout::ND>,
        BaseShape2D<float, M, N, Layout::ND>, Layout::ND>;

    using MatA = Tile<TileType::Mat, float8_e4m3_t, M, TileK,
                      BLayout::ColMajor, M, TileK, SLayout::RowMajor, 512>;
    using MatB = Tile<TileType::Mat, float8_e4m3_t, TileK, N,
                      BLayout::ColMajor, TileK, N, SLayout::RowMajor, 512>;
    using MatScaleA = Tile<TileType::Mat, float8_e8m0_t, M, ScaleK,
                           BLayout::RowMajor, M, ScaleK, SLayout::RowMajor, 32>;
    using MatScaleB = Tile<TileType::Mat, float8_e8m0_t, TileK, N,
                           BLayout::ColMajor, ScaleK, N, SLayout::ColMajor, 32>;
    using Left = TileLeft<float8_e4m3_t, M, TileK, M, TileK>;
    using Right = TileRight<float8_e4m3_t, TileK, N, TileK, N>;
    using LeftScale = TileLeftScale<float8_e8m0_t, M, ScaleK, M, ScaleK>;
    using RightScale = TileRightScale<float8_e8m0_t, TileK, N, ScaleK, N>;
    using Acc = TileAcc<float, M, N, M, N>;

    MatA a_mat;
    MatB b_mat;
    MatScaleA a_scale_mat;
    MatScaleB b_scale_mat;
    Left a_tile;
    Right b_tile;
    LeftScale a_scale_tile;
    RightScale b_scale_tile;
    Acc c_tile;
    size_t addr = 0;
    TASSIGN(a_mat, addr);
    addr += MatA::Numel * sizeof(typename MatA::DType);
    TASSIGN(b_mat, addr);
    addr += MatB::Numel * sizeof(typename MatB::DType);
    TASSIGN(a_scale_mat, addr);
    addr += MatScaleA::Numel * sizeof(typename MatScaleA::DType);
    TASSIGN(b_scale_mat, addr);
    addr += MatScaleB::Numel * sizeof(typename MatScaleB::DType);
    TASSIGN(a_tile, 0x0);
    TASSIGN(b_tile, 0x0);
    TASSIGN(c_tile, 0x0);
    TASSIGN(a_scale_tile, addr);
    addr += LeftScale::Numel * sizeof(typename LeftScale::DType);
    TASSIGN(b_scale_tile, addr);

    GlobalC global_c(output);
    for (int k0 = 0; k0 < K; k0 += TileK) {{
        GlobalA global_a(activation + k0);
        GlobalB global_b(weight + k0);
        GlobalScaleA global_scale_a(activation_scale + k0 / 32);
        GlobalScaleB global_scale_b(weight_scale + (k0 / 32) * N);
        TLOAD(a_mat, global_a);
        TLOAD(b_mat, global_b);
        TLOAD(a_scale_mat, global_scale_a);
        TLOAD(b_scale_mat, global_scale_b);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
        TMOV(a_tile, a_mat);
        TMOV(b_tile, b_mat);
        TMOV(a_scale_tile, a_scale_mat);
        TMOV(b_scale_tile, b_scale_mat);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        if (k0 == 0) {{
            TMATMUL_MX(c_tile, a_tile, a_scale_tile, b_tile, b_scale_tile);
        }} else {{
            TMATMUL_MX(c_tile, c_tile, a_tile, a_scale_tile, b_tile, b_scale_tile);
        }}
        set_flag(PIPE_M, PIPE_MTE2, EVENT_ID0);
        wait_flag(PIPE_M, PIPE_MTE2, EVENT_ID0);
    }}
    set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    TSTORE(global_c, c_tile);
}}
"""
    )
    return source


def write_host_q8_block_dot_orchestration(build_dir: Path, blocks: int, n: int) -> Path:
    source = build_dir / "host_q8_block_dot_orch.cpp"
    source.write_text(
        f"""\
#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" {{

__attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs& orch_args) {{
    (void)orch_args;
    return PTO2OrchestrationConfig{{.expected_arg_count = 6}};
}}

__attribute__((visibility("default"))) void
build_q8_block_dot_graph(const ChipTaskArgs& orch_args) {{
    if (orch_args.tensor_count() != 3 || orch_args.scalar_count() != 3) {{
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS,
            "expected 3 tensor args and 3 scalar args");
        return;
    }}

    constexpr uint64_t kBlocks = {blocks};
    constexpr uint64_t kK = 32;
    constexpr uint64_t kN = {n};
    constexpr uint64_t kTileN = 128;
    if (orch_args.scalar(0) != kBlocks || orch_args.scalar(1) != kK ||
        orch_args.scalar(2) != kN) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "artifact geometry mismatch");
        return;
    }}
    if ((kN % kTileN) != 0) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "N must be 128-aligned");
        return;
    }}

    const ChipTensor& activation = orch_args.tensor(0).ref();
    const ChipTensor& weight = orch_args.tensor(1).ref();
    const ChipTensor& output = orch_args.tensor(2).ref();
    if (activation.shapes[0] < kBlocks * 32 ||
        weight.shapes[0] < kBlocks * 32 * kN ||
        output.shapes[0] < kBlocks * kN) {{
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "tensor payload too short");
        return;
    }}

    for (uint64_t block = 0; block < kBlocks; ++block) {{
        for (uint64_t tile_n = 0; tile_n < kN / kTileN; ++tile_n) {{
            CoreTaskArgs task_args;
            task_args.add_input(activation, weight);
            task_args.add_output(output);
            task_args.add_scalar(block, tile_n);
            rt_submit_aic_task(0, task_args);
        }}
    }}
}}

}}  // extern "C"
"""
    )
    return source


def write_host_q8_block_dot_kernel(build_dir: Path, n: int) -> Path:
    source = build_dir / "host_q8_block_dot_kernel.cpp"
    source.write_text(
        f"""\
#include <cstdint>
#include <pto/pto-inst.hpp>

#include "tensor.h"

using namespace pto;

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(
        __gm__ int64_t* args) {{
    __gm__ ChipTensor* activation_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ ChipTensor* weight_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ ChipTensor* output_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[2]);
    __gm__ int8_t* activation =
        reinterpret_cast<__gm__ int8_t*>(activation_tensor->buffer.addr) +
        activation_tensor->start_offset;
    __gm__ int8_t* weight =
        reinterpret_cast<__gm__ int8_t*>(weight_tensor->buffer.addr) +
        weight_tensor->start_offset;
    __gm__ int32_t* output =
        reinterpret_cast<__gm__ int32_t*>(output_tensor->buffer.addr) +
        output_tensor->start_offset;
    const uint64_t block = static_cast<uint64_t>(args[3]);
    const uint64_t tile_n = static_cast<uint64_t>(args[4]);

    constexpr int K = 32;
    constexpr int N = {n};
    constexpr int TileN = 128;
    using AValid = TileShape2D<int8_t, 1, K>;
    using AWhole = BaseShape2D<int8_t, 1, K>;
    using BValid = TileShape2D<int8_t, K, TileN>;
    using BWhole = BaseShape2D<int8_t, K, N>;
    using CValid = TileShape2D<int32_t, 1, TileN>;
    using CWhole = BaseShape2D<int32_t, 1, N>;
    using GlobalA = GlobalTensor<int8_t, AValid, AWhole>;
    using GlobalB = GlobalTensor<int8_t, BValid, BWhole>;
    using GlobalC = GlobalTensor<int32_t, CValid, CWhole>;
    using MatA = Tile<TileType::Mat, int8_t, 1, K,
                      BLayout::RowMajor, 1, K>;
    using MatB = Tile<TileType::Mat, int8_t, K, TileN,
                      BLayout::ColMajor, K, TileN, SLayout::RowMajor, 512>;
    using Left = TileLeft<int8_t, 1, K, 1, K>;
    using Right = TileRight<int8_t, K, TileN, K, TileN>;
    using Acc = TileAcc<int32_t, 1, TileN, 1, TileN>;

    GlobalA global_a(activation + block * K);
    GlobalB global_b(weight + block * K * N + tile_n * TileN);
    GlobalC global_c(output + block * N + tile_n * TileN);
    MatA mat_a;
    MatB mat_b;
    Left tile_a;
    Right tile_b;
    Acc tile_c;
    TASSIGN(mat_a, 0x0);
    TASSIGN(mat_b, 0x20000);
    TASSIGN(tile_a, 0x0);
    TASSIGN(tile_b, 0x0);
    TASSIGN(tile_c, 0x0);

    TLOAD(mat_a, global_a);
    TLOAD(mat_b, global_b);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    TEXTRACT(tile_a, mat_a, 0, 0);
    TMOV(tile_b, mat_b);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    TGEMV(tile_c, tile_a, tile_b);
    set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    TSTORE(global_c, tile_c);
}}
"""
    )
    return source


def write_host_engram_context_orchestration(build_dir: Path) -> Path:
    source = build_dir / "host_engram_context_orch.cpp"
    source.write_text(
        """\
#include "pto_orchestration_api.h"
#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {

__attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs& orch_args) {
    (void)orch_args;
    return PTO2OrchestrationConfig{.expected_arg_count = 12};
}

__attribute__((visibility("default"))) void
build_engram_context_graph(const ChipTaskArgs& orch_args) {
    if (orch_args.tensor_count() != 6 || orch_args.scalar_count() != 6) {
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS,
            "expected 6 tensor args and 6 scalar args");
        return;
    }

    const ChipTensor& table = orch_args.tensor(0).ref();
    const ChipTensor& indices = orch_args.tensor(1).ref();
    const ChipTensor& hidden = orch_args.tensor(2).ref();
    const ChipTensor& gate_weight = orch_args.tensor(3).ref();
    const ChipTensor& output = orch_args.tensor(4).ref();
    const ChipTensor& gate_state = orch_args.tensor(5).ref();

    const uint64_t batch = orch_args.scalar(0);
    const uint64_t table_rows = orch_args.scalar(1);
    const uint64_t hidden_size = orch_args.scalar(2);
    const uint64_t chunk_offset = orch_args.scalar(3);
    const uint64_t chunk_elems = orch_args.scalar(4);
    const uint32_t bias_bits = static_cast<uint32_t>(orch_args.scalar(5));
    float bias = 0.0f;
    std::memcpy(&bias, &bias_bits, sizeof(float));

    constexpr uint64_t kIndicesPerBatch = 8;
    if (batch != 1) {
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "batch must be 1");
        return;
    }
    if (hidden_size == 0 || table_rows < kIndicesPerBatch) {
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS,
            "invalid table_rows or hidden_size");
        return;
    }
    if (chunk_offset > hidden_size || chunk_elems > hidden_size - chunk_offset) {
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "invalid chunk range");
        return;
    }
    if (table.shapes[0] < table_rows * hidden_size ||
        indices.shapes[0] < batch * kIndicesPerBatch ||
        hidden.shapes[0] < batch * hidden_size ||
        gate_weight.shapes[0] < batch * hidden_size ||
        output.shapes[0] < batch * hidden_size ||
        gate_state.shapes[0] < batch) {
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "tensor payload too short");
        return;
    }

    for (uint64_t b = 0; b < batch; ++b) {
        const uint64_t vector_base = b * hidden_size;
        const uint64_t index_base = b * kIndicesPerBatch;
        uint32_t gate_index[1] = {static_cast<uint32_t>(b)};
        float gate = get_tensor_data<float>(gate_state, 1, gate_index);
        if (chunk_offset == 0) {
            float dot = 0.0f;
            for (uint64_t d = 0; d < hidden_size; ++d) {
                uint32_t value_index[1] = {
                    static_cast<uint32_t>(vector_base + d)};
                dot += get_tensor_data<float>(hidden, 1, value_index) *
                       get_tensor_data<float>(gate_weight, 1, value_index);
            }
            gate = 1.0f / (1.0f + expf(-(dot + bias)));
            set_tensor_data(gate_state, 1, gate_index, gate);
        }
        for (uint64_t d = chunk_offset; d < chunk_offset + chunk_elems; ++d) {
            float table_sum = 0.0f;
            for (uint64_t slot = 0; slot < kIndicesPerBatch; ++slot) {
                uint32_t index_index[1] = {
                    static_cast<uint32_t>(index_base + slot)};
                const int32_t row =
                    get_tensor_data<int32_t>(indices, 1, index_index);
                if (row < 0 || static_cast<uint64_t>(row) >= table_rows) {
                    rt_report_fatal(
                        PTO2_ERROR_INVALID_ARGS, "index out of bounds");
                    return;
                }
                uint32_t table_index[1] = {static_cast<uint32_t>(
                    static_cast<uint64_t>(row) * hidden_size + d)};
                table_sum += get_tensor_data<float>(table, 1, table_index);
            }
            const float mean = table_sum / static_cast<float>(kIndicesPerBatch);
            uint32_t value_index[1] = {
                static_cast<uint32_t>(vector_base + d)};
            const float value =
                get_tensor_data<float>(hidden, 1, value_index) + gate * mean;
            set_tensor_data(output, 1, value_index, value);
        }
    }

    CoreTaskArgs noop_args;
    noop_args.add_inout(output, gate_state);
    rt_submit_aiv_task(0, noop_args);
}

}  // extern "C"
"""
    )
    return source


def write_host_engram_context_noop_kernel(build_dir: Path) -> Path:
    source = build_dir / "host_engram_context_noop.cpp"
    source.write_text(
        """\
#include <cstdint>
#include <pto/pto-inst.hpp>

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {
    (void)args;
}
"""
    )
    return source


def write_host_deepseek_vector_orchestration(build_dir: Path) -> Path:
    source = build_dir / "host_deepseek_vector_orch.cpp"
    source.write_text(
        """\
#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" {

__attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs& orch_args) {
    (void)orch_args;
    return PTO2OrchestrationConfig{.expected_arg_count = 15};
}

__attribute__((visibility("default"))) void
build_deepseek_vector_graph(const ChipTaskArgs& orch_args) {
    if (orch_args.tensor_count() != 4 || orch_args.scalar_count() != 11) {
        rt_report_fatal(
            PTO2_ERROR_INVALID_ARGS,
            "expected 4 tensor args and 11 scalar args");
        return;
    }
    const uint64_t len0 = orch_args.scalar(1);
    const uint64_t len1 = orch_args.scalar(2);
    const uint64_t len2 = orch_args.scalar(3);
    const uint64_t out_len = orch_args.scalar(4);
    if (out_len == 0 ||
        orch_args.tensor(0).ref().shapes[0] < (len0 == 0 ? 1 : len0) ||
        orch_args.tensor(1).ref().shapes[0] < (len1 == 0 ? 1 : len1) ||
        orch_args.tensor(2).ref().shapes[0] < (len2 == 0 ? 1 : len2) ||
        orch_args.tensor(3).ref().shapes[0] < out_len) {
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "tensor payload too short");
        return;
    }

    CoreTaskArgs task_args;
    task_args.add_input(
        orch_args.tensor(0).ref(),
        orch_args.tensor(1).ref(),
        orch_args.tensor(2).ref());
    task_args.add_output(orch_args.tensor(3).ref());
    for (uint64_t index = 0; index < 11; ++index) {
        task_args.add_scalar(orch_args.scalar(index));
    }
    rt_submit_aiv_task(0, task_args);
}

}  // extern "C"
"""
    )
    return source


def write_host_deepseek_vector_kernel(build_dir: Path) -> Path:
    source = build_dir / "host_deepseek_vector_kernel.cpp"
    source.write_text(
        """\
#include <cstdint>
#include <cmath>
#include <pto/pto-inst.hpp>

#include "tensor.h"

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

namespace {

enum Operation : uint64_t {
    RMS_NORM = 1,
    HC_SPLIT = 2,
    HC_WEIGHTED_SUM = 3,
    HC_POST = 4,
    ROPE = 5,
    KV_FP8_ROUNDTRIP = 6,
    SINK_ATTENTION = 7,
    INDEXER_QAT = 8,
    SCALE = 9,
    SWIGLU = 10,
    ADD = 11,
    ROUTER = 12,
    TOP_K = 13,
    HC_HEAD_WEIGHTS = 14,
    COMPRESSOR_POOL = 15,
};

inline float from_bits(uint64_t value) {
    union {
        uint32_t bits;
        float value;
    } converted;
    converted.bits = static_cast<uint32_t>(value);
    return converted.value;
}

inline float round_bf16(float value) {
    union {
        uint32_t bits;
        float value;
    } converted;
    converted.value = value;
    const uint32_t exponent = converted.bits & 0x7f800000U;
    if (exponent == 0x7f800000U) {
        converted.bits &= 0xffff0000U;
        return converted.value;
    }
    const uint32_t bias = 0x7fffU + ((converted.bits >> 16U) & 1U);
    converted.bits = (converted.bits + bias) & 0xffff0000U;
    return converted.value;
}

inline float accurate_exp_f32(float value) {
    const float scaled = value * 1.4426950408889634f;
    const int exponent = static_cast<int>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    const float remainder = value - static_cast<float>(exponent) * 0.6931471805599453f;
    float term = 1.0f;
    float sum = 1.0f;
    for (int order = 1; order <= 12; ++order) {
        term *= remainder / static_cast<float>(order);
        sum += term;
    }
    union {
        uint32_t bits;
        float value;
    } power_of_two;
    power_of_two.bits = static_cast<uint32_t>(exponent + 127) << 23U;
    return sum * power_of_two.value;
}

inline float accurate_log1p_f32(float value) {
    int exponent = 0;
    double mantissa = frexp(1.0 + static_cast<double>(value), &exponent);
    if (mantissa < 0.70710678118654752440) {
        mantissa *= 2.0;
        --exponent;
    }
    const double ratio = (mantissa - 1.0) / (mantissa + 1.0);
    const double ratio_squared = ratio * ratio;
    double power = ratio;
    double sum = 0.0;
    for (int order = 1; order <= 61; order += 2) {
        sum += power / static_cast<double>(order);
        power *= ratio_squared;
    }
    return static_cast<float>(2.0 * sum + static_cast<double>(exponent) * 0.69314718055994530942);
}

inline float accurate_sqrt_f32(float value) {
    double estimate = static_cast<double>(value) >= 1.0 ? static_cast<double>(value) : 1.0;
    for (int iteration = 0; iteration < 12; ++iteration) {
        estimate = 0.5 * (estimate + static_cast<double>(value) / estimate);
    }
    return static_cast<float>(estimate);
}

inline float sigmoid(float value) {
    return 1.0f / (1.0f + accurate_exp_f32(-value));
}

inline float separate_mul_add(float left, float right, float addend) {
    volatile float product = left * right;
    return product + addend;
}

inline float separate_add(float left, float right) {
    volatile float value = left + right;
    return value;
}

inline float fp8_positive(uint32_t index) {
    const uint32_t exponent = index >> 3U;
    const uint32_t mantissa = index & 7U;
    if (exponent == 0) {
        return static_cast<float>(mantissa) * 0.001953125f;
    }
    return (1.0f + static_cast<float>(mantissa) * 0.125f) * ldexpf(1.0f, static_cast<int>(exponent) - 7);
}

inline float fp8_round(float value) {
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    const float magnitude = fminf(fabsf(value), 448.0f);
    uint32_t low = 0;
    uint32_t high = 126;
    while (low < high) {
        const uint32_t middle = (low + high + 1U) >> 1U;
        if (fp8_positive(middle) <= magnitude) {
            low = middle;
        } else {
            high = middle - 1U;
        }
    }
    uint32_t best = low;
    if (best < 126U) {
        const float best_difference = fabsf(magnitude - fp8_positive(best));
        const float next_difference = fabsf(magnitude - fp8_positive(best + 1U));
        if (next_difference < best_difference ||
            (next_difference == best_difference && ((best + 1U) & 1U) == 0 && (best & 1U) != 0)) {
            ++best;
        }
    }
    return sign * fp8_positive(best);
}

inline void rms_norm(
        __gm__ const float* input,
        __gm__ const float* weight,
        __gm__ float* output,
        uint64_t groups,
        uint64_t width,
        bool has_weight,
        bool bf16,
        float eps) {
    for (uint64_t group = 0; group < groups; ++group) {
        const uint64_t base = group * width;
        float sum = 0.0f;
        for (uint64_t index = 0; index < width; ++index) {
            const float value = input[base + index];
            sum += value * value;
        }
        const float inverse = 1.0f / accurate_sqrt_f32(sum / static_cast<float>(width) + eps);
        for (uint64_t index = 0; index < width; ++index) {
            float value = input[base + index] * inverse;
            if (has_weight) {
                value *= weight[index];
            }
            output[base + index] = bf16 ? round_bf16(value) : value;
        }
    }
}

}  // namespace

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {
    __gm__ ChipTensor* input0_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[0]);
    __gm__ ChipTensor* input1_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[1]);
    __gm__ ChipTensor* input2_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[2]);
    __gm__ ChipTensor* output_tensor =
        reinterpret_cast<__gm__ ChipTensor*>(args[3]);
    __gm__ const float* input0 =
        reinterpret_cast<__gm__ const float*>(input0_tensor->buffer.addr) +
        input0_tensor->start_offset;
    __gm__ const float* input1 =
        reinterpret_cast<__gm__ const float*>(input1_tensor->buffer.addr) +
        input1_tensor->start_offset;
    __gm__ const float* input2 =
        reinterpret_cast<__gm__ const float*>(input2_tensor->buffer.addr) +
        input2_tensor->start_offset;
    __gm__ float* output =
        reinterpret_cast<__gm__ float*>(output_tensor->buffer.addr) +
        output_tensor->start_offset;
    const uint64_t operation = static_cast<uint64_t>(args[4]);
    const uint64_t len0 = static_cast<uint64_t>(args[5]);
    const uint64_t len1 = static_cast<uint64_t>(args[6]);
    const uint64_t len2 = static_cast<uint64_t>(args[7]);
    const uint64_t out_len = static_cast<uint64_t>(args[8]);
    const uint64_t p0 = static_cast<uint64_t>(args[9]);
    const uint64_t p1 = static_cast<uint64_t>(args[10]);
    const uint64_t p2 = static_cast<uint64_t>(args[11]);
    const uint64_t p3 = static_cast<uint64_t>(args[12]);
    const float f0 = from_bits(static_cast<uint64_t>(args[13]));
    const float f1 = from_bits(static_cast<uint64_t>(args[14]));

    if (operation == RMS_NORM) {
        rms_norm(input0, input1, output, p0, p1, p2 != 0, p3 != 0, f0);
        return;
    }
    if (operation == HC_SPLIT) {
        const uint64_t hc = p0;
        const uint64_t iterations = p1;
        for (uint64_t index = 0; index < hc; ++index) {
            output[index] = sigmoid(separate_mul_add(input0[index], input1[0], input2[index])) + f0;
            output[hc + index] = 2.0f * sigmoid(separate_mul_add(input0[hc + index], input1[1], input2[hc + index]));
        }
        const uint64_t offset = 2 * hc;
        for (uint64_t destination = 0; destination < hc; ++destination) {
            float row_max = -INFINITY;
            for (uint64_t source = 0; source < hc; ++source) {
                const uint64_t index = destination * hc + source;
                const float value = separate_mul_add(input0[offset + index], input1[2], input2[offset + index]);
                output[offset + index] = value;
                row_max = fmaxf(row_max, value);
            }
            float sum = 0.0f;
            for (uint64_t source = 0; source < hc; ++source) {
                const uint64_t index = offset + destination * hc + source;
                output[index] = accurate_exp_f32(output[index] - row_max);
                sum += output[index];
            }
            for (uint64_t source = 0; source < hc; ++source) {
                const uint64_t index = offset + destination * hc + source;
                output[index] = output[index] / sum + f0;
            }
        }
        for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
            if (iteration != 0) {
                for (uint64_t destination = 0; destination < hc; ++destination) {
                    float sum = 0.0f;
                    for (uint64_t source = 0; source < hc; ++source) {
                        sum += output[offset + destination * hc + source];
                    }
                    const float inverse = 1.0f / (sum + f0);
                    for (uint64_t source = 0; source < hc; ++source) {
                        output[offset + destination * hc + source] *= inverse;
                    }
                }
            }
            for (uint64_t source = 0; source < hc; ++source) {
                float sum = 0.0f;
                for (uint64_t destination = 0; destination < hc; ++destination) {
                    sum += output[offset + destination * hc + source];
                }
                const float inverse = 1.0f / (sum + f0);
                for (uint64_t destination = 0; destination < hc; ++destination) {
                    output[offset + destination * hc + source] *= inverse;
                }
            }
        }
        return;
    }
    if (operation == HC_WEIGHTED_SUM) {
        const uint64_t hidden = p0;
        const uint64_t hc = p1;
        for (uint64_t dim = 0; dim < hidden; ++dim) {
            float value = 0.0f;
            for (uint64_t source = 0; source < hc; ++source) {
                value = separate_mul_add(input0[source * hidden + dim], input1[source], value);
            }
            output[dim] = p2 != 0 ? round_bf16(value) : value;
        }
        return;
    }
    if (operation == HC_POST) {
        const uint64_t hidden = p0;
        const uint64_t hc = p1;
        for (uint64_t destination = 0; destination < hc; ++destination) {
            for (uint64_t dim = 0; dim < hidden; ++dim) {
                float value = input0[dim] * input2[destination];
                for (uint64_t source = 0; source < hc; ++source) {
                    value = separate_mul_add(input1[source * hidden + dim], input2[hc + source * hc + destination], value);
                }
                output[destination * hidden + dim] = p2 != 0 ? round_bf16(value) : value;
            }
        }
        return;
    }
    if (operation == ROPE) {
        const uint64_t heads = p0;
        const uint64_t head_dim = p1;
        const uint64_t rope_dim = p2;
        const bool inverse = p3 != 0;
        const uint64_t tail = head_dim - rope_dim;
        for (uint64_t index = 0; index < len0; ++index) {
            output[index] = input0[index];
        }
        for (uint64_t head = 0; head < heads; ++head) {
            for (uint64_t pair = 0; pair < rope_dim / 2; ++pair) {
                const uint64_t index = head * head_dim + tail + pair * 2;
                const float x0 = input0[index];
                const float x1 = input0[index + 1];
                const float sine = inverse ? -input2[pair] : input2[pair];
                output[index] = round_bf16(separate_mul_add(-x1, sine, x0 * input1[pair]));
                output[index + 1] = round_bf16(separate_mul_add(x1, input1[pair], x0 * sine));
            }
        }
        for (uint64_t head = 0; head < heads; ++head) {
            for (uint64_t index = 0; index < tail; ++index) {
                const uint64_t offset = head * head_dim + index;
                output[offset] = round_bf16(output[offset]);
            }
        }
        return;
    }
    if (operation == KV_FP8_ROUNDTRIP) {
        const uint64_t quantized_len = p0;
        const uint64_t block_size = p1;
        for (uint64_t block_start = 0; block_start < quantized_len; block_start += block_size) {
            float absolute_max = 1.0e-4f;
            for (uint64_t index = block_start; index < block_start + block_size; ++index) {
                absolute_max = fmaxf(absolute_max, fabsf(input0[index]));
            }
            int exponent = static_cast<int>(ceilf(log2f(absolute_max / 448.0f)));
            exponent = exponent < -127 ? -127 : (exponent > 127 ? 127 : exponent);
            const float scale = ldexpf(1.0f, exponent);
            for (uint64_t index = block_start; index < block_start + block_size; ++index) {
                output[index] = round_bf16(fp8_round(fmaxf(-448.0f, fminf(448.0f, input0[index] / scale))) * scale);
            }
        }
        for (uint64_t index = quantized_len; index < len0; ++index) {
            output[index] = round_bf16(input0[index]);
        }
        return;
    }
    if (operation == SINK_ATTENTION) {
        const uint64_t heads = p0;
        const uint64_t head_dim = p1;
        const uint64_t rows = len1 / head_dim;
        const float scale = 1.0f / accurate_sqrt_f32(static_cast<float>(head_dim));
        float scores[1024];
        for (uint64_t head = 0; head < heads; ++head) {
            float max_score = input2[head];
            for (uint64_t row = 0; row < rows; ++row) {
                float score = 0.0f;
                for (uint64_t dim = 0; dim < head_dim; ++dim) {
                    score = separate_mul_add(input0[head * head_dim + dim], input1[row * head_dim + dim], score);
                }
                scores[row] = score * scale;
                max_score = fmaxf(max_score, scores[row]);
            }
            float denominator = accurate_exp_f32(input2[head] - max_score);
            for (uint64_t dim = 0; dim < head_dim; ++dim) {
                float value = 0.0f;
                for (uint64_t row = 0; row < rows; ++row) {
                    const float weight = accurate_exp_f32(scores[row] - max_score);
                    if (dim == 0) {
                        denominator += weight;
                    }
                    value = separate_mul_add(weight, input1[row * head_dim + dim], value);
                }
                const float inverse_denominator = 1.0f / denominator;
                output[head * head_dim + dim] = round_bf16(value * inverse_denominator);
            }
        }
        return;
    }
    if (operation == INDEXER_QAT) {
        const float values[8] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
        for (uint64_t index = 0; index < len0; ++index) {
            output[index] = input0[index];
        }
        for (uint64_t head = 0; head < len0; head += 128) {
            for (uint64_t stride = 1; stride < 128; stride *= 2) {
                for (uint64_t base = 0; base < 128; base += 2 * stride) {
                    for (uint64_t index = 0; index < stride; ++index) {
                        const float a = output[head + base + index];
                        const float b = output[head + base + stride + index];
                        output[head + base + index] = a + b;
                        output[head + base + stride + index] = a - b;
                    }
                }
            }
            for (uint64_t index = 0; index < 128; ++index) {
                output[head + index] *= 0.08838834764831845f;
            }
            for (uint64_t block = 0; block < 128; block += 32) {
                float absolute_max = 7.052966104933725e-38f;
                for (uint64_t index = 0; index < 32; ++index) {
                    absolute_max = fmaxf(absolute_max, fabsf(output[head + block + index]));
                }
                const float scale = exp2f(ceilf(log2f(absolute_max / 6.0f)));
                for (uint64_t index = 0; index < 32; ++index) {
                    const uint64_t offset = head + block + index;
                    const float sign = output[offset] < 0.0f ? -1.0f : 1.0f;
                    const float magnitude = fminf(fabsf(output[offset] / scale), 6.0f);
                    uint64_t best = 0;
                    for (uint64_t candidate = 1; candidate < 8; ++candidate) {
                        const float difference = fabsf(magnitude - values[candidate]);
                        const float best_difference = fabsf(magnitude - values[best]);
                        if (difference < best_difference ||
                            (difference == best_difference && (candidate & 1U) == 0 && (best & 1U) != 0)) {
                            best = candidate;
                        }
                    }
                    output[offset] = round_bf16(sign * values[best] * scale);
                }
            }
        }
        return;
    }
    if (operation == COMPRESSOR_POOL) {
        const uint64_t head_dim = p0;
        const uint64_t ratio = p1;
        const uint64_t width = p2;
        const uint64_t rope_dim = p3;
        for (uint64_t dim = 0; dim < head_dim; ++dim) {
            float max_score = -INFINITY;
            if (ratio == 4) {
                for (uint64_t row = 0; row < ratio; ++row) {
                    max_score = fmaxf(max_score, input1[row * width + dim]);
                    max_score = fmaxf(
                        max_score,
                        input1[(ratio + row) * width + head_dim + dim]);
                }
            } else {
                for (uint64_t row = 0; row < ratio; ++row) {
                    max_score = fmaxf(max_score, input1[row * width + dim]);
                }
            }
            float denominator = 0.0f;
            float weighted_sum = 0.0f;
            if (ratio == 4) {
                for (uint64_t row = 0; row < ratio; ++row) {
                    const uint64_t previous = row * width + dim;
                    const uint64_t current = (ratio + row) * width + head_dim + dim;
                    if (input1[previous] > -1.0e30f) {
                        const float weight = accurate_exp_f32(input1[previous] - max_score);
                        denominator += weight;
                        weighted_sum = separate_mul_add(input0[previous], weight, weighted_sum);
                    }
                    if (input1[current] > -1.0e30f) {
                        const float weight = accurate_exp_f32(input1[current] - max_score);
                        denominator += weight;
                        weighted_sum = separate_mul_add(input0[current], weight, weighted_sum);
                    }
                }
            } else {
                for (uint64_t row = 0; row < ratio; ++row) {
                    const uint64_t index = row * width + dim;
                    if (input1[index] > -1.0e30f) {
                        const float weight = accurate_exp_f32(input1[index] - max_score);
                        denominator += weight;
                        weighted_sum = separate_mul_add(input0[index], weight, weighted_sum);
                    }
                }
            }
            output[dim] = denominator == 0.0f ? 0.0f : weighted_sum / denominator;
        }
        float square_sum = 0.0f;
        for (uint64_t dim = 0; dim < head_dim; ++dim) {
            square_sum += output[dim] * output[dim];
        }
        const float inverse = 1.0f /
            accurate_sqrt_f32(square_sum / static_cast<float>(head_dim) + f0);
        for (uint64_t dim = 0; dim < head_dim; ++dim) {
            output[dim] *= inverse * input2[dim];
        }
        const uint64_t tail = head_dim - rope_dim;
        const uint64_t cos_offset = head_dim;
        const uint64_t sin_offset = head_dim + rope_dim / 2;
        for (uint64_t pair = 0; pair < rope_dim / 2; ++pair) {
            const uint64_t index = tail + pair * 2;
            const float x0 = output[index];
            const float x1 = output[index + 1];
            output[index] = separate_mul_add(
                -x1,
                input2[sin_offset + pair],
                x0 * input2[cos_offset + pair]);
            output[index + 1] = separate_mul_add(
                x1,
                input2[cos_offset + pair],
                x0 * input2[sin_offset + pair]);
        }
        if (head_dim == 128) {
            for (uint64_t stride = 1; stride < 128; stride *= 2) {
                for (uint64_t base = 0; base < 128; base += 2 * stride) {
                    for (uint64_t index = 0; index < stride; ++index) {
                        const float a = output[base + index];
                        const float b = output[base + stride + index];
                        output[base + index] = a + b;
                        output[base + stride + index] = a - b;
                    }
                }
            }
            const float values[8] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
            for (uint64_t index = 0; index < 128; ++index) {
                output[index] *= 0.08838834764831845f;
            }
            for (uint64_t block = 0; block < 128; block += 32) {
                float absolute_max = 7.052966104933725e-38f;
                for (uint64_t index = 0; index < 32; ++index) {
                    absolute_max = fmaxf(absolute_max, fabsf(output[block + index]));
                }
                const float scale = exp2f(ceilf(log2f(absolute_max / 6.0f)));
                for (uint64_t index = 0; index < 32; ++index) {
                    const uint64_t offset = block + index;
                    const float sign = output[offset] < 0.0f ? -1.0f : 1.0f;
                    const float magnitude = fminf(fabsf(output[offset] / scale), 6.0f);
                    uint64_t best = 0;
                    for (uint64_t candidate = 1; candidate < 8; ++candidate) {
                        const float difference = fabsf(magnitude - values[candidate]);
                        const float best_difference = fabsf(magnitude - values[best]);
                        if (difference < best_difference ||
                            (difference == best_difference && (candidate & 1U) == 0 && (best & 1U) != 0)) {
                            best = candidate;
                        }
                    }
                    output[offset] = round_bf16(sign * values[best] * scale);
                }
            }
        } else {
            for (uint64_t block_start = 0; block_start < tail; block_start += 64) {
                float absolute_max = 1.0e-4f;
                for (uint64_t index = block_start; index < block_start + 64; ++index) {
                    absolute_max = fmaxf(absolute_max, fabsf(output[index]));
                }
                const float scale = exp2f(ceilf(log2f(absolute_max / 448.0f)));
                for (uint64_t index = block_start; index < block_start + 64; ++index) {
                    output[index] = round_bf16(
                        fp8_round(fmaxf(-448.0f, fminf(448.0f, output[index] / scale))) * scale);
                }
            }
            for (uint64_t index = tail; index < head_dim; ++index) {
                output[index] = round_bf16(output[index]);
            }
        }
        return;
    }
    if (operation == SCALE) {
        for (uint64_t index = 0; index < len0; ++index) {
            volatile float value = input0[index] * f0;
            output[index] = p0 != 0 ? round_bf16(value) : value;
        }
        return;
    }
    if (operation == SWIGLU) {
        for (uint64_t index = 0; index < len0; ++index) {
            const float gate = f0 > 1.0e-6f ? fminf(input0[index], f0) : input0[index];
            const float up = f0 > 1.0e-6f ? fmaxf(-f0, fminf(input1[index], f0)) : input1[index];
            const float value = gate * (1.0f / (1.0f + accurate_exp_f32(-gate))) * up;
            output[index] = p0 != 0 ? round_bf16(value) : value;
        }
        return;
    }
    if (operation == ADD) {
        for (uint64_t index = 0; index < out_len; ++index) {
            const float value = input0[index] + input1[index];
            output[index] = p0 != 0 ? round_bf16(value) : value;
        }
        return;
    }
    if (operation == ROUTER) {
        const uint64_t experts = p0;
        const uint64_t top_k = p1;
        const bool hash = p2 != 0;
        for (uint64_t expert = 0; expert < experts; ++expert) {
            const float logit = input0[expert];
            const float exponential = accurate_exp_f32(logit);
            const float softplus = logit > 20.0f ? logit : (logit < -20.0f ? exponential : accurate_log1p_f32(exponential));
            output[expert] = accurate_sqrt_f32(softplus);
        }
        for (uint64_t slot = 0; slot < top_k; ++slot) {
            output[experts + slot] = hash ? input2[slot] : -1.0f;
        }
        if (!hash) {
            for (uint64_t expert = 0; expert < experts; ++expert) {
                const float score = output[expert] + input1[expert];
                uint64_t insertion = top_k;
                for (uint64_t slot = 0; slot < top_k; ++slot) {
                    const int64_t current = static_cast<int64_t>(output[experts + slot]);
                    if (current < 0 || score > output[current] + input1[current]) {
                        insertion = slot;
                        break;
                    }
                }
                if (insertion < top_k) {
                    for (uint64_t slot = top_k - 1; slot > insertion; --slot) {
                        output[experts + slot] = output[experts + slot - 1];
                    }
                    output[experts + insertion] = static_cast<float>(expert);
                }
            }
        }
        float sum = 0.0f;
        for (uint64_t slot = 0; slot < top_k; ++slot) {
            sum = separate_add(sum, output[static_cast<uint64_t>(output[experts + slot])]);
        }
        sum = fmaxf(sum, 6.1035156e-5f);
        for (uint64_t slot = 0; slot < top_k; ++slot) {
            const uint64_t expert = static_cast<uint64_t>(output[experts + slot]);
            volatile float normalized = output[expert] / sum;
            output[experts + top_k + slot] = normalized * f0;
        }
        return;
    }
    if (operation == TOP_K) {
        const uint64_t top_k = p0;
        for (uint64_t slot = 0; slot < top_k; ++slot) {
            output[slot] = -1.0f;
            output[top_k + slot] = -INFINITY;
        }
        for (uint64_t token = 0; token < len0; ++token) {
            uint64_t insertion = top_k;
            for (uint64_t slot = 0; slot < top_k; ++slot) {
                if (input0[token] > output[top_k + slot]) {
                    insertion = slot;
                    break;
                }
            }
            if (insertion < top_k) {
                for (uint64_t slot = top_k - 1; slot > insertion; --slot) {
                    output[slot] = output[slot - 1];
                    output[top_k + slot] = output[top_k + slot - 1];
                }
                output[insertion] = static_cast<float>(token);
                output[top_k + insertion] = input0[token];
            }
        }
        return;
    }
    if (operation == HC_HEAD_WEIGHTS) {
        for (uint64_t index = 0; index < len0; ++index) {
            const float affine = separate_mul_add(input0[index], f0, input1[index]);
            const float weight = affine >= 0.0f
                ? 1.0f / (1.0f + accurate_exp_f32(-affine))
                : accurate_exp_f32(affine) / (1.0f + accurate_exp_f32(affine));
            output[index] = weight + f1;
        }
    }
}
"""
    )
    return source


def write_wrapped_kernel(
    build_dir: Path,
    spec_key: str,
    kernel: KernelSpec,
    source: Path,
    vector_tile_rows: int,
    vector_tile_cols: int,
    matmul_rows: int,
    matmul_cols: int,
) -> Path:
    wrapped = build_dir / f"w4_kernel_func_{kernel.func_id}.cpp"
    defines = []
    if spec_key == "host_vector":
        defines.extend(
            [
                f"#define SIMPLER_VECTOR_TILE_ROWS {vector_tile_rows}",
                f"#define SIMPLER_VECTOR_TILE_COLS {vector_tile_cols}",
            ]
        )
    else:
        defines.extend(
            [
                f"#define SIMPLER_MATMUL_ROWS {matmul_rows}",
                f"#define SIMPLER_MATMUL_COLS {matmul_cols}",
            ]
        )
    wrapped.write_text("\n".join(defines + [f'#include "{source.as_posix()}"', ""]))
    return wrapped


def describe(args: argparse.Namespace, simpler_root: Path, pto_isa_root: Path) -> int:
    spec = PROFILE_SPECS[args.profile]
    example_root = resolve_example_root(simpler_root, spec)
    manifest_path = Path(args.output_dir).resolve() / spec.manifest_name
    orchestration = (
        f"generated://{spec.orch_source}" if spec.generated else str(example_root / spec.orch_source)
    )
    payload = {
        "profile": spec.profile,
        "runtime_variant": "HostBuildGraph",
        "simpler_root": str(simpler_root),
        "pto_isa_root": str(pto_isa_root),
        "example_root": str(example_root),
        "manifest": str(manifest_path),
        "orchestration": orchestration,
        "sim_kernel_libgcc": args.sim_kernel_libgcc,
        "tile_batch": args.tile_batch if args.profile == "host_matmul" else None,
        "kernels": [
            {
                "func_id": kernel.func_id,
                "core_type": kernel.core_type,
                "source": f"generated://{kernel.source}"
                if spec.generated
                else str(example_root / kernel.source),
            }
            for kernel in spec.kernels
        ],
    }
    if args.profile in (
        "host_gemm",
        "host_fp32_gemm",
        "host_quantized_gemm",
        "host_fp8_gemm",
        "host_fp4_gemm",
        "host_q8_block_dot",
    ):
        payload["gemm"] = {"m": args.gemm_m, "k": args.gemm_k, "n": args.gemm_n}
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def build(args: argparse.Namespace, simpler_root: Path, pto_isa_root: Path) -> int:
    spec = PROFILE_SPECS[args.profile]
    if args.tile_batch < 1:
        raise SystemExit("--tile-batch must be >= 1")
    if args.tile_batch > 1 and args.profile != "host_matmul":
        raise SystemExit("--tile-batch is only supported for --profile host_matmul")
    if args.tile_batch > 1 and not args.reuse_runtime_manifest:
        raise SystemExit(
            "--tile-batch > 1 must use --reuse-runtime-manifest to avoid loading multiple simpler runtime binaries in one process"
        )
    if args.profile in (
        "host_gemm",
        "host_fp32_gemm",
        "host_quantized_gemm",
        "host_fp8_gemm",
    ):
        gemm_dims = (args.gemm_m, args.gemm_k, args.gemm_n)
        if any(dim <= 0 or dim % 128 != 0 for dim in gemm_dims):
            raise SystemExit("--gemm-m/--gemm-k/--gemm-n must be positive and 128-aligned")
    if args.profile == "host_fp8_gemm" and (
        args.platform != "a5sim" or (args.gemm_m, args.gemm_k, args.gemm_n) != (128, 128, 128)
    ):
        raise SystemExit("host_fp8_gemm requires --platform a5sim and 128x128x128 geometry")
    if args.profile == "host_fp4_gemm" and (
        args.platform != "a5sim"
        or args.gemm_m != 128
        or args.gemm_k <= 0
        or args.gemm_k % 128 != 0
        or args.gemm_n != 128
    ):
        raise SystemExit(
            "host_fp4_gemm requires --platform a5sim, M=128, 128-aligned K and N=128"
        )
    if args.profile == "host_q8_block_dot":
        if args.gemm_m <= 0 or args.gemm_k != 32 or args.gemm_n <= 0 or args.gemm_n % 128 != 0:
            raise SystemExit(
                "host_q8_block_dot requires positive --gemm-m, --gemm-k 32 and 128-aligned --gemm-n"
            )
    if args.profile == "host_deepseek_vector" and args.platform != "a5sim":
        raise SystemExit("host_deepseek_vector requires --platform a5sim")
    reuse_runtime = None
    if args.reuse_runtime_manifest:
        reuse_runtime = load_reuse_runtime_manifest(
            Path(args.reuse_runtime_manifest)
        )

    example_root = resolve_example_root(simpler_root, spec)
    os.environ["PTO_ISA_ROOT"] = str(pto_isa_root)

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    build_dir = output_dir / "build"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    tls_policy = (
        reuse_runtime["sim_aicore_tls_policy"]
        if reuse_runtime is not None
        else sim_aicore_tls_policy(simpler_root, args.platform)
    )
    toolchain_fingerprint = host_toolchain_fingerprint()
    toolchain_fingerprint["sim_aicore_tls_policy"] = tls_policy
    if reuse_runtime is None:
        invalidate_stale_host_toolchain_cache(simpler_root, toolchain_fingerprint)

    RuntimeBuilder, KernelCompiler, api_kind = load_simpler_build_api(simpler_root)
    builder = RuntimeBuilder(platform=args.platform)
    if reuse_runtime is None:
        configured_tls_policy = configure_sim_aicore_tls_adapter(
            builder, build_dir, simpler_root, args.platform
        )
        if configured_tls_policy != tls_policy:
            raise RuntimeError("Simpler AICore TLS policy changed during build")
    kernel_compiler = (
        KernelCompiler(platform=args.platform)
        if api_kind == "setup"
        else builder.get_kernel_compiler()
    )
    configure_sim_kernel_libgcc(
        kernel_compiler, build_dir, args.sim_kernel_libgcc
    )

    runtime_name = "host_build_graph"
    (
        host_binary,
        aicpu_binary,
        aicore_binary,
        sim_context_binary,
        simpler_log_binary,
    ) = runtime_binaries_for_manifest(
        builder, api_kind, runtime_name, build_dir, reuse_runtime
    )
    orch_source = example_root / spec.orch_source
    if args.profile == "host_gemm":
        orch_source = write_host_gemm_orchestration(
            build_dir, args.gemm_m, args.gemm_k, args.gemm_n
        )
    if args.profile == "host_fp32_gemm":
        orch_source = write_host_fp32_gemm_orchestration(
            build_dir, args.gemm_m, args.gemm_k, args.gemm_n
        )
    if args.profile == "host_quantized_gemm":
        orch_source = write_host_quantized_gemm_orchestration(
            build_dir, args.gemm_m, args.gemm_k, args.gemm_n
        )
    if args.profile == "host_fp8_gemm":
        orch_source = write_host_fp8_gemm_orchestration(
            build_dir, args.gemm_m, args.gemm_k, args.gemm_n
        )
    if args.profile == "host_fp4_gemm":
        orch_source = write_host_fp4_gemm_orchestration(
            build_dir, args.gemm_m, args.gemm_k, args.gemm_n
        )
    if args.profile == "host_q8_block_dot":
        orch_source = write_host_q8_block_dot_orchestration(
            build_dir, args.gemm_m, args.gemm_n
        )
    if args.profile == "host_engram_context":
        orch_source = write_host_engram_context_orchestration(build_dir)
    if args.profile == "host_deepseek_vector":
        orch_source = write_host_deepseek_vector_orchestration(build_dir)
    if args.profile == "host_matmul" and args.tile_batch > 1:
        orch_source = write_batched_matmul_orchestration(build_dir, args.tile_batch)
    orch_binary = kernel_compiler.compile_orchestration(
        runtime_name,
        str(orch_source),
        build_dir=str(build_dir),
    )
    incore_include_dirs = kernel_compiler.get_orchestration_include_dirs(
        runtime_name
    )

    kernel_entries = []
    for kernel in spec.kernels:
        vector_source = (
            write_vector_kernel_source(
                build_dir,
                kernel.func_id,
                args.vector_tile_rows,
                args.vector_tile_cols,
            )
            if args.profile == "host_vector"
            else None
        )
        batched_source = (
            write_batched_matmul_kernel_source(build_dir, kernel.func_id, args.tile_batch)
            if args.profile == "host_matmul" and args.tile_batch > 1
            else None
        )
        engram_source = (
            write_host_engram_context_noop_kernel(build_dir)
            if args.profile == "host_engram_context"
            else None
        )
        deepseek_vector_source = (
            write_host_deepseek_vector_kernel(build_dir)
            if args.profile == "host_deepseek_vector"
            else None
        )
        gemm_source = (
            write_host_gemm_kernel(build_dir, args.gemm_m, args.gemm_k, args.gemm_n)
            if args.profile == "host_gemm"
            else None
        )
        fp32_gemm_source = (
            write_host_fp32_gemm_kernel(
                build_dir, args.gemm_m, args.gemm_k, args.gemm_n
            )
            if args.profile == "host_fp32_gemm"
            else None
        )
        quantized_gemm_source = (
            write_host_quantized_gemm_kernel(
                build_dir, args.gemm_m, args.gemm_k, args.gemm_n
            )
            if args.profile == "host_quantized_gemm"
            else None
        )
        fp8_gemm_source = (
            write_host_fp8_gemm_kernel(
                build_dir, args.gemm_m, args.gemm_k, args.gemm_n
            )
            if args.profile == "host_fp8_gemm"
            else None
        )
        fp4_gemm_source = (
            write_host_fp4_gemm_kernel(
                build_dir, args.gemm_m, args.gemm_k, args.gemm_n
            )
            if args.profile == "host_fp4_gemm"
            else None
        )
        q8_block_dot_source = (
            write_host_q8_block_dot_kernel(build_dir, args.gemm_n)
            if args.profile == "host_q8_block_dot"
            else None
        )
        source = Path(
            vector_source
            or batched_source
            or engram_source
            or deepseek_vector_source
            or gemm_source
            or fp32_gemm_source
            or quantized_gemm_source
            or fp8_gemm_source
            or fp4_gemm_source
            or q8_block_dot_source
            or (example_root / kernel.source)
        ).resolve()
        wrapped = write_wrapped_kernel(
            build_dir,
            args.profile,
            kernel,
            source,
            args.vector_tile_rows,
            args.vector_tile_cols,
            args.matmul_rows,
            args.matmul_cols,
        )
        blob = kernel_compiler.compile_incore(
            str(wrapped),
            core_type=kernel.core_type,
            pto_isa_root=str(pto_isa_root),
            extra_include_dirs=incore_include_dirs,
            build_dir=str(build_dir),
        )
        out_path = output_dir / f"kernel_func_{kernel.func_id}.bin"
        atomic_write_bytes(out_path, blob)
        kernel_entries.append(
            {
                "func_id": kernel.func_id,
                "binary": {
                    "id": f"{args.profile}_kernel_{kernel.func_id}",
                    "format": "raw-binary",
                    "source": str(out_path),
                },
            }
        )

    host_path = output_dir / "runtime_host.bin"
    aicpu_path = output_dir / "runtime_aicpu.bin"
    aicore_path = output_dir / "runtime_aicore.bin"
    orch_path = output_dir / "orchestration.so"
    sim_context_path = output_dir / "libcpu_sim_context.so"
    simpler_log_path = output_dir / "libsimpler_log.so"
    if reuse_runtime is None:
        assert host_binary is not None
        assert aicpu_binary is not None
        assert aicore_binary is not None
        atomic_write_bytes(host_path, host_binary)
        atomic_write_bytes(aicpu_path, aicpu_binary)
        atomic_write_bytes(aicore_path, aicore_binary)
    atomic_write_bytes(orch_path, orch_binary)

    runtime_env = dict(reuse_runtime.get("runtime_env", {})) if reuse_runtime is not None else {}
    # Load libsimpler_log.so FIRST with RTLD_GLOBAL so libcpu_sim_context.so can resolve
    # unified_log_* symbols against the single process-wide HostLogger instance.
    if reuse_runtime is None and simpler_log_binary is not None:
        atomic_write_bytes(simpler_log_path, simpler_log_binary)
        runtime_env["SIMPLER_LOG_LIBRARY"] = str(simpler_log_path)
    if reuse_runtime is None and sim_context_binary is not None:
        atomic_write_bytes(sim_context_path, sim_context_binary)
        runtime_env["SIMPLER_SIM_CONTEXT_LIBRARY"] = str(sim_context_path)
    args_template = list(spec.args_template)
    if args.profile == "host_matmul":
        args_template.append({"kind": "scalar_tile_batch", "name": "TILE_BATCH"})

    manifest = {
        "simpler_capi_abi_version": SIMPLER_CAPI_ABI_VERSION,
        "host_toolchain": toolchain_fingerprint,
        "profile": spec.profile,
        "platform": args.platform,
        "runtime_variant": "HostBuildGraph",
        "callable_hint": spec.callable_hint,
        "sim_kernel_libgcc": args.sim_kernel_libgcc,
        "simpler_runtime": {
            "sim_aicore_tls_policy": tls_policy,
            "host_runtime_library": reuse_runtime["host_runtime_library"]
            if reuse_runtime is not None
            else {
                "id": f"{args.profile}_runtime_host",
                "format": "shared-object",
                "source": str(host_path),
            },
            "orch_shared_object": {
                "id": f"{args.profile}_orchestration",
                "format": "shared-object",
                "source": str(orch_path),
            },
            "orch_function_name": spec.orch_function,
            "aicpu_binary": reuse_runtime["aicpu_binary"]
            if reuse_runtime is not None
            else {
                "id": f"{args.profile}_runtime_aicpu",
                "format": "runtime-binary",
                "source": str(aicpu_path),
            },
            "aicore_binary": reuse_runtime["aicore_binary"]
            if reuse_runtime is not None
            else {
                "id": f"{args.profile}_runtime_aicore",
                "format": "runtime-binary",
                "source": str(aicore_path),
            },
            "kernels": kernel_entries,
            "launch": {
                "aicpu_thread_num": 3,
                "block_dim": 3,
                "device_id": args.device_id,
                "orch_thread_num": 0,
            },
            "runtime_env": runtime_env,
            "tile_batch": args.tile_batch if args.profile == "host_matmul" else 1,
            "args_template": args_template,
        },
        "note": "args_template is consumed by simulator-side helper to construct SimplerRuntimeArg entries",
    }
    if args.profile == "host_gemm":
        manifest["host_gemm_manifest_version"] = 3
        manifest["host_gemm"] = {
            "m": args.gemm_m,
            "k": args.gemm_k,
            "n": args.gemm_n,
            "input_dtype": "bf16",
            "output_dtype": "fp32",
            "tile": 128,
        }
    if args.profile == "host_fp32_gemm":
        manifest["host_gemm_manifest_version"] = 4
        manifest["host_gemm"] = {
            "m": args.gemm_m,
            "k": args.gemm_k,
            "n": args.gemm_n,
            "input_dtype": "fp32",
            "output_dtype": "fp32",
            "tile": 128,
        }
    if args.profile == "host_quantized_gemm":
        manifest["host_quantized_gemm_manifest_version"] = 2
        manifest["host_quantized_gemm"] = {
            "m": args.gemm_m,
            "k": args.gemm_k,
            "n": args.gemm_n,
            "input_dtype": "int8",
            "output_dtype": "int32",
            "tile": 128,
        }
    if args.profile == "host_fp8_gemm":
        manifest["host_fp8_gemm_manifest_version"] = 1
        manifest["host_fp8_gemm"] = {
            "m": args.gemm_m,
            "k": args.gemm_k,
            "n": args.gemm_n,
            "input_dtype": "fp8_e4m3_ue8m0",
            "output_dtype": "fp32",
            "tile": 128,
        }
    if args.profile == "host_fp4_gemm":
        manifest["host_fp4_gemm_manifest_version"] = 2
        manifest["host_fp4_gemm"] = {
            "m": args.gemm_m,
            "k": args.gemm_k,
            "n": args.gemm_n,
            "input_dtype": "fp8_e4m3+fp4_e2m1_lowered_fp8+ue8m0",
            "output_dtype": "fp32",
            "tile": 128,
        }
    if args.profile == "host_q8_block_dot":
        manifest["host_q8_block_dot_manifest_version"] = 3
        manifest["host_q8_block_dot"] = {
            "m": args.gemm_m,
            "k": 32,
            "n": args.gemm_n,
            "input_dtype": "int8",
            "output_dtype": "int32",
            "tile": 128,
        }
    if args.profile == "host_engram_context":
        manifest["host_engram_context_manifest_version"] = 6
    if args.profile == "host_deepseek_vector":
        manifest["host_deepseek_vector_manifest_version"] = 13

    manifest_path = output_dir / spec.manifest_name
    atomic_write_text(manifest_path, json.dumps(manifest, indent=2, sort_keys=True))
    print(manifest_path)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=sorted(PROFILE_SPECS), required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--simpler-root", default=None)
    parser.add_argument("--pto-isa-root", default=None)
    parser.add_argument("--platform", default="a2a3sim")
    parser.add_argument("--device-id", type=int, default=0)
    parser.add_argument("--vector-tile-rows", type=int, default=32)
    parser.add_argument("--vector-tile-cols", type=int, default=32)
    parser.add_argument("--matmul-rows", type=int, default=128)
    parser.add_argument("--matmul-cols", type=int, default=128)
    parser.add_argument("--gemm-m", type=int, default=128)
    parser.add_argument("--gemm-k", type=int, default=128)
    parser.add_argument("--gemm-n", type=int, default=128)
    parser.add_argument("--tile-batch", type=int, default=1)
    parser.add_argument("--reuse-runtime-manifest", default=None)
    parser.add_argument(
        "--sim-kernel-libgcc",
        choices=("static", "shared"),
        default="static",
        help="link generated simulation kernels to static or host libgcc",
    )
    parser.add_argument("--describe", action="store_true")
    args = parser.parse_args()

    simpler_root = Path(args.simpler_root or default_simpler_root()).expanduser().resolve()
    if not simpler_root.exists():
        raise SystemExit(f"simpler root not found: {simpler_root}")
    pto_isa_root = resolve_pto_isa_root(simpler_root, args.pto_isa_root)

    if args.describe:
        return describe(args, simpler_root, pto_isa_root)
    with artifact_build_lock(simpler_root):
        return build(args, simpler_root, pto_isa_root)


if __name__ == "__main__":
    raise SystemExit(main())
