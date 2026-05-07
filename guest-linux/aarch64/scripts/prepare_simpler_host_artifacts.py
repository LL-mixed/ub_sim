#!/usr/bin/env python3
"""Build HostBuildGraph artifacts for the simulator/simpler C API bridge."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path


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


PROFILE_SPECS = {
    "host_vector": ProfileSpec(
        profile="HostVector",
        example="vector_example",
        manifest_name="host_vector_manifest.json",
        callable_hint="host_vector_example",
        orch_source="kernels/orchestration/example_orch.cpp",
        orch_function="build_example_graph",
        kernels=(
            KernelSpec(0, "kernels/aiv/kernel_add.cpp", "aiv"),
            KernelSpec(1, "kernels/aiv/kernel_add_scalar.cpp", "aiv"),
            KernelSpec(2, "kernels/aiv/kernel_mul.cpp", "aiv"),
        ),
        args_template=(
            {"kind": "input", "name": "a"},
            {"kind": "input", "name": "b"},
            {"kind": "output", "name": "f"},
            {"kind": "scalar_size", "name": "size_a"},
            {"kind": "scalar_size", "name": "size_b"},
            {"kind": "scalar_size", "name": "size_f"},
            {"kind": "scalar_elems", "name": "SIZE"},
        ),
    ),
    "host_matmul": ProfileSpec(
        profile="HostMatmul",
        example="matmul",
        manifest_name="host_matmul_manifest.json",
        callable_hint="host_matmul_example",
        orch_source="kernels/orchestration/matmul_orch.cpp",
        orch_function="build_matmul_graph",
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
            {"kind": "scalar_size", "name": "size_a"},
            {"kind": "scalar_size", "name": "size_w1"},
            {"kind": "scalar_size", "name": "size_w2"},
            {"kind": "scalar_size", "name": "size_f"},
            {"kind": "scalar_elems", "name": "SIZE"},
        ),
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
    candidates = [
        simpler_root / "examples" / "a2a3" / "host_build_graph" / spec.example,
        simpler_root / "tests" / "st" / "a2a3" / "host_build_graph" / spec.example,
    ]
    for candidate in candidates:
        if (candidate / spec.orch_source).exists():
            return candidate
    tried = ", ".join(str(candidate) for candidate in candidates)
    raise SystemExit(f"{spec.example} HostBuildGraph sources not found; tried: {tried}")


def load_simpler_build_api(simpler_root: Path):
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


def read_runtime_binaries(builder, api_kind: str, runtime_name: str, build_dir: Path):
    if api_kind == "setup":
        runtime_binaries = builder.get_binaries(runtime_name, build=True)
        sim_context = (
            runtime_binaries.sim_context_path.read_bytes()
            if runtime_binaries.sim_context_path is not None
            else None
        )
        return (
            runtime_binaries.host_path.read_bytes(),
            runtime_binaries.aicpu_path.read_bytes(),
            runtime_binaries.aicore_path.read_bytes(),
            sim_context,
        )
    host_binary, aicpu_binary, aicore_binary = builder.build(runtime_name, str(build_dir))
    return host_binary, aicpu_binary, aicore_binary, None


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
    __gm__ float* src1 = reinterpret_cast<__gm__ float*>(args[1]);
    __gm__ float* out = reinterpret_cast<__gm__ float*>(args[2]);
    int size = static_cast<int>(args[3]);
"""
        load_second = """\
    TileData src1Tile(vRows, vCols);
    TASSIGN(src1Tile, 0x10000);
    GlobalData src1Global(src1);
    TLOAD(src1Tile, src1Global);
"""
    else:
        scalar_input = """\
    union {
        uint64_t u64;
        float f32;
    } converter;
    converter.u64 = args[1];
    float scalar = converter.f32;
    __gm__ float* out = reinterpret_cast<__gm__ float*>(args[2]);
    int size = static_cast<int>(args[3]);
"""

    source = build_dir / f"vector_kernel_func_{func_id}.cpp"
    source.write_text(
        f"""\
#include <cstdint>
#include <pto/pto-inst.hpp>

using namespace pto;

#include "pipe_sync.h"

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(__gm__ int64_t* args) {{
    __gm__ float* src0 = reinterpret_cast<__gm__ float*>(args[0]);
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
#include "orchestration_api.h"
#include <cstdint>
#include <iostream>

extern "C" {{

int build_matmul_graph(OrchestrationRuntime* runtime, const ChipStorageTaskArgs& orch_args) {{
    if (orch_args.tensor_count() < 4) {{
        std::cerr << "build_matmul_graph: Expected at least 4 tensor args, got "
                  << orch_args.tensor_count() << '\\n';
        return -1;
    }}

    auto* host_a = orch_args.tensor(0).data_as<uint8_t>();
    auto* host_w1 = orch_args.tensor(1).data_as<uint8_t>();
    auto* host_w2 = orch_args.tensor(2).data_as<uint8_t>();
    auto* host_f = orch_args.tensor(3).data_as<uint8_t>();
    size_t size_a = static_cast<size_t>(orch_args.tensor(0).nbytes());
    size_t size_w1 = static_cast<size_t>(orch_args.tensor(1).nbytes());
    size_t size_w2 = static_cast<size_t>(orch_args.tensor(2).nbytes());
    size_t size_f = static_cast<size_t>(orch_args.tensor(3).nbytes());
    int tile_size = 0;
    int tile_batch = {tile_batch};
    if (orch_args.scalar_count() >= 6) {{
        tile_size = static_cast<int>(orch_args.scalar(4));
        tile_batch = static_cast<int>(orch_args.scalar(5));
    }} else if (orch_args.scalar_count() >= 1) {{
        tile_batch = static_cast<int>(orch_args.scalar(orch_args.scalar_count() - 1));
    }}

    if (tile_batch <= 0 || tile_batch > {tile_batch}) {{
        std::cerr << "build_matmul_graph: invalid tile_batch=" << tile_batch << '\\n';
        return -1;
    }}
    if (size_a % static_cast<size_t>(tile_batch) != 0 ||
        size_w1 % static_cast<size_t>(tile_batch) != 0 ||
        size_w2 % static_cast<size_t>(tile_batch) != 0 ||
        size_f % static_cast<size_t>(tile_batch) != 0) {{
        std::cerr << "build_matmul_graph: batch sizes must divide evenly by tile_batch\\n";
        return -1;
    }}
    if (tile_size <= 0) {{
        tile_size = static_cast<int>(orch_args.tensor(0).shapes[0] / static_cast<uint32_t>(tile_batch));
    }}

    std::cout << "\\n=== build_matmul_graph: Creating Batched Task Runtime ===\\n";
    std::cout << "Formula: F = exp(sqrt(log(A)) @ W1 + sqrt(log(A)) @ W2)\\n";
    std::cout << "Tile SIZE: " << tile_size << " elements, tile_batch=" << tile_batch << "\\n";

    void* dev_a = device_malloc(runtime, size_a);
    void* dev_w1 = device_malloc(runtime, size_w1);
    void* dev_w2 = device_malloc(runtime, size_w2);
    void* dev_f = device_malloc(runtime, size_f);
    void* dev_b = device_malloc(runtime, size_a);
    void* dev_c = device_malloc(runtime, size_f);
    void* dev_d = device_malloc(runtime, size_f);
    if (!dev_a || !dev_w1 || !dev_w2 || !dev_f || !dev_b || !dev_c || !dev_d) {{
        std::cerr << "Error: Failed to allocate batched device memory\\n";
        if (dev_a) device_free(runtime, dev_a);
        if (dev_w1) device_free(runtime, dev_w1);
        if (dev_w2) device_free(runtime, dev_w2);
        if (dev_f) device_free(runtime, dev_f);
        if (dev_b) device_free(runtime, dev_b);
        if (dev_c) device_free(runtime, dev_c);
        if (dev_d) device_free(runtime, dev_d);
        return -1;
    }}
    if (copy_to_device(runtime, dev_a, host_a, size_a) != 0 ||
        copy_to_device(runtime, dev_w1, host_w1, size_w1) != 0 ||
        copy_to_device(runtime, dev_w2, host_w2, size_w2) != 0) {{
        std::cerr << "Error: Failed to copy batched inputs to device\\n";
        return -1;
    }}
    record_tensor_pair(runtime, host_f, dev_f, size_f);

    uint64_t args_t0[3];
    args_t0[0] = reinterpret_cast<uint64_t>(dev_a);
    args_t0[1] = reinterpret_cast<uint64_t>(dev_b);
    args_t0[2] = tile_size;
    int t0 = add_task(runtime, args_t0, 3, 0, CoreType::AIV);

    uint64_t args_t1[4];
    args_t1[0] = reinterpret_cast<uint64_t>(dev_b);
    args_t1[1] = reinterpret_cast<uint64_t>(dev_w1);
    args_t1[2] = reinterpret_cast<uint64_t>(dev_c);
    args_t1[3] = tile_size;
    int t1 = add_task(runtime, args_t1, 4, 1, CoreType::AIC);

    uint64_t args_t2[4];
    args_t2[0] = reinterpret_cast<uint64_t>(dev_b);
    args_t2[1] = reinterpret_cast<uint64_t>(dev_w2);
    args_t2[2] = reinterpret_cast<uint64_t>(dev_d);
    args_t2[3] = tile_size;
    int t2 = add_task(runtime, args_t2, 4, 1, CoreType::AIC);

    uint64_t args_t3[4];
    args_t3[0] = reinterpret_cast<uint64_t>(dev_c);
    args_t3[1] = reinterpret_cast<uint64_t>(dev_d);
    args_t3[2] = reinterpret_cast<uint64_t>(dev_f);
    args_t3[3] = tile_size;
    int t3 = add_task(runtime, args_t3, 4, 2, CoreType::AIV);

    add_successor(runtime, t0, t1);
    add_successor(runtime, t0, t2);
    add_successor(runtime, t1, t3);
    add_successor(runtime, t2, t3);

    std::cout << "Created batched runtime with " << get_task_count(runtime) << " tasks\\n";
    return 0;
}}

}}  // extern "C"
"""
    )
    return source


def write_batched_matmul_kernel_source(build_dir: Path, func_id: int, tile_batch: int) -> Path | None:
    common_prefix = """\
#include <cstdint>
#include <pto/pto-inst.hpp>

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
    __gm__ half* src = reinterpret_cast<__gm__ half*>(args[0]);
    __gm__ half* out = reinterpret_cast<__gm__ half*>(args[1]);

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
    __gm__ half* src0 = reinterpret_cast<__gm__ half*>(args[0]);
    __gm__ half* src1 = reinterpret_cast<__gm__ half*>(args[1]);
    __gm__ float* out = reinterpret_cast<__gm__ float*>(args[2]);

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
    __gm__ float* src0 = reinterpret_cast<__gm__ float*>(args[0]);
    __gm__ float* src1 = reinterpret_cast<__gm__ float*>(args[1]);
    __gm__ float* out = reinterpret_cast<__gm__ float*>(args[2]);

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
    payload = {
        "profile": spec.profile,
        "runtime_variant": "HostBuildGraph",
        "simpler_root": str(simpler_root),
        "pto_isa_root": str(pto_isa_root),
        "example_root": str(example_root),
        "manifest": str(manifest_path),
        "orchestration": str(example_root / spec.orch_source),
        "tile_batch": args.tile_batch if args.profile == "host_matmul" else None,
        "kernels": [
            {
                "func_id": kernel.func_id,
                "core_type": kernel.core_type,
                "source": str(example_root / kernel.source),
            }
            for kernel in spec.kernels
        ],
    }
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
    reuse_runtime = None
    if args.reuse_runtime_manifest:
        reuse_manifest = json.loads(Path(args.reuse_runtime_manifest).read_text())
        reuse_runtime = reuse_manifest["simpler_runtime"]

    example_root = resolve_example_root(simpler_root, spec)
    os.environ["PTO_ISA_ROOT"] = str(pto_isa_root)

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    build_dir = output_dir / "build"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    RuntimeBuilder, KernelCompiler, api_kind = load_simpler_build_api(simpler_root)
    builder = RuntimeBuilder(platform=args.platform)
    kernel_compiler = (
        KernelCompiler(platform=args.platform)
        if api_kind == "setup"
        else builder.get_kernel_compiler()
    )

    runtime_name = "host_build_graph"
    host_binary, aicpu_binary, aicore_binary, sim_context_binary = read_runtime_binaries(
        builder, api_kind, runtime_name, build_dir
    )
    orch_source = example_root / spec.orch_source
    if args.profile == "host_matmul" and args.tile_batch > 1:
        orch_source = write_batched_matmul_orchestration(build_dir, args.tile_batch)
    orch_binary = kernel_compiler.compile_orchestration(
        runtime_name,
        str(orch_source),
        build_dir=str(build_dir),
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
        source = Path(vector_source or batched_source or (example_root / kernel.source)).resolve()
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
            extra_include_dirs=[
                str((simpler_root / "src" / "a2a3" / "runtime" / runtime_name / "runtime").resolve())
            ],
            build_dir=str(build_dir),
        )
        out_path = output_dir / f"kernel_func_{kernel.func_id}.bin"
        out_path.write_bytes(blob)
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
    host_path.write_bytes(host_binary)
    aicpu_path.write_bytes(aicpu_binary)
    aicore_path.write_bytes(aicore_binary)
    orch_path.write_bytes(orch_binary)

    runtime_env = dict(reuse_runtime.get("runtime_env", {})) if reuse_runtime is not None else {}
    if reuse_runtime is None and sim_context_binary is not None:
        sim_context_path.write_bytes(sim_context_binary)
        runtime_env["SIMPLER_SIM_CONTEXT_LIBRARY"] = str(sim_context_path)
    args_template = list(spec.args_template)
    if args.profile == "host_matmul":
        args_template.append({"kind": "scalar_tile_batch", "name": "TILE_BATCH"})

    manifest = {
        "profile": spec.profile,
        "runtime_variant": "HostBuildGraph",
        "callable_hint": spec.callable_hint,
        "simpler_runtime": {
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

    manifest_path = output_dir / spec.manifest_name
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True))
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
    parser.add_argument("--tile-batch", type=int, default=1)
    parser.add_argument("--reuse-runtime-manifest", default=None)
    parser.add_argument("--describe", action="store_true")
    args = parser.parse_args()

    simpler_root = Path(args.simpler_root or default_simpler_root()).expanduser().resolve()
    if not simpler_root.exists():
        raise SystemExit(f"simpler root not found: {simpler_root}")
    pto_isa_root = resolve_pto_isa_root(simpler_root, args.pto_isa_root)

    if args.describe:
        return describe(args, simpler_root, pto_isa_root)
    return build(args, simpler_root, pto_isa_root)


if __name__ == "__main__":
    raise SystemExit(main())
