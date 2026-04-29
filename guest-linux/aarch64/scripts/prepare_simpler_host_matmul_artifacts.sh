#!/bin/zsh
set -euo pipefail

OUT_DIR="${1:-${SIMPLER_HOST_MATMUL_ARTIFACT_DIR:-/tmp/simpler-host-matmul-artifacts}}"
MANIFEST="$OUT_DIR/host_matmul_manifest.json"
RUNTIME_SO="$OUT_DIR/libsimpler_host_matmul_stub.so"
RUNTIME_C="$OUT_DIR/simpler_host_matmul_stub.c"

mkdir -p "$OUT_DIR"

cat > "$RUNTIME_C" <<'EOF'
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    SIMPLER_ARG_SCALAR = 0,
    SIMPLER_ARG_INPUT_PTR = 1,
    SIMPLER_ARG_OUTPUT_PTR = 2,
    SIMPLER_ARG_INOUT_PTR = 3,
    SIMPLER_MAX_ARGS = 64,
};

struct simpler_stub_runtime {
    uint64_t args[SIMPLER_MAX_ARGS];
    uint64_t sizes[SIMPLER_MAX_ARGS];
    int types[SIMPLER_MAX_ARGS];
    int count;
};

size_t get_runtime_size(void)
{
    return sizeof(struct simpler_stub_runtime);
}

int set_device(int device_id)
{
    (void)device_id;
    return 0;
}

void *device_malloc(size_t size)
{
    return malloc(size);
}

void device_free(void *ptr)
{
    free(ptr);
}

int copy_to_device(void *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
    return 0;
}

int copy_from_device(void *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
    return 0;
}

void record_tensor_pair(void *runtime, void *host_ptr, void *dev_ptr, size_t size)
{
    (void)runtime;
    (void)host_ptr;
    (void)dev_ptr;
    (void)size;
}

int enable_runtime_profiling(void *runtime, int enabled)
{
    (void)runtime;
    (void)enabled;
    return 0;
}

int init_runtime(void *runtime,
                 const uint8_t *orch_so_binary,
                 size_t orch_so_size,
                 const char *orch_func_name,
                 uint64_t *func_args,
                 int func_args_count,
                 int *arg_types,
                 uint64_t *arg_sizes,
                 const int *kernel_func_ids,
                 const uint8_t *const *kernel_binaries,
                 const size_t *kernel_sizes,
                 int kernel_count)
{
    struct simpler_stub_runtime *rt = runtime;
    int count = func_args_count;

    (void)orch_so_binary;
    (void)orch_so_size;
    (void)orch_func_name;
    (void)kernel_func_ids;
    (void)kernel_binaries;
    (void)kernel_sizes;
    (void)kernel_count;

    if (count > SIMPLER_MAX_ARGS) {
        count = SIMPLER_MAX_ARGS;
    }
    rt->count = count;
    for (int i = 0; i < count; ++i) {
        rt->args[i] = func_args[i];
        rt->types[i] = arg_types[i];
        rt->sizes[i] = arg_sizes[i];
    }
    return 0;
}

int launch_runtime(void *runtime,
                   int aicpu_thread_num,
                   int block_dim,
                   int device_id,
                   const uint8_t *aicpu_binary,
                   size_t aicpu_size,
                   const uint8_t *aicore_binary,
                   size_t aicore_size,
                   int orch_thread_num)
{
    struct simpler_stub_runtime *rt = runtime;
    uint32_t variant = 0;

    (void)aicpu_thread_num;
    (void)block_dim;
    (void)device_id;
    (void)aicpu_binary;
    (void)aicpu_size;
    (void)aicore_binary;
    (void)aicore_size;
    (void)orch_thread_num;

    for (int i = 0; i < rt->count; ++i) {
        if (rt->types[i] == SIMPLER_ARG_INPUT_PTR && rt->sizes[i] > 0) {
            const uint8_t *input = (const uint8_t *)(uintptr_t)rt->args[i];
            size_t sample = rt->sizes[i] < 256 ? rt->sizes[i] : 256;
            variant ^= (uint32_t)((uintptr_t)input >> 4);
            for (size_t j = 0; j < sample; ++j) {
                variant = (variant * 131u) + input[j] + (uint32_t)(j + 1);
            }
        }
    }

    for (int i = 0; i < rt->count; ++i) {
        if (rt->types[i] == SIMPLER_ARG_OUTPUT_PTR ||
            rt->types[i] == SIMPLER_ARG_INOUT_PTR) {
            float *out = (float *)(uintptr_t)rt->args[i];
            size_t elems = rt->sizes[i] / sizeof(float);
            for (size_t j = 0; j < elems; ++j) {
                out[j] = 1.0f + (float)((variant + j) % 17u + 1u) / 1024.0f;
            }
        }
    }
    return 0;
}

int finalize_runtime(void *runtime)
{
    (void)runtime;
    return 0;
}
EOF

cc -shared -fPIC -O2 "$RUNTIME_C" -o "$RUNTIME_SO"

cat > "$MANIFEST" <<EOF
{
  "simpler_runtime": {
    "host_runtime_library": {
      "id": "simpler-host-matmul-stub-runtime",
      "format": "elf-shared-object",
      "source": "$RUNTIME_SO"
    },
    "orch_shared_object": {
      "id": "simpler-host-matmul-stub-orch",
      "format": "elf-shared-object",
      "source": "$RUNTIME_SO"
    },
    "orch_function_name": "host_matmul_example",
    "aicpu_binary": null,
    "aicore_binary": null,
    "kernels": [],
    "launch": {
      "aicpu_thread_num": 1,
      "block_dim": 1,
      "device_id": 0,
      "orch_thread_num": 1
    },
    "runtime_env": {}
  }
}
EOF

echo "$MANIFEST"
