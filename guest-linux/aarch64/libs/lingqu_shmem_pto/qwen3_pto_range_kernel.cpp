#include "qwen3_pto_range.hpp"
#include "tensor.h"
#include <cstring>

namespace qp = lingqu::qwen3_pto;

static qp::View matrix(const ChipTensor &t) {
    qp::require((t.ndims == 1 || t.ndims == 2) &&
                (t.dtype == DataType::FLOAT32 || t.dtype == DataType::FLOAT16));
    const uint64_t width = t.dtype == DataType::FLOAT32 ? 4 : 2;
    qp::require(t.start_offset <= t.buffer.size / width &&
                t.extent_elem() <= t.buffer.size / width - t.start_offset);
    qp::View v{t.buffer.addr + t.start_offset * width,
        t.ndims == 1 ? 1 : t.shapes[0], t.ndims == 1 ? t.shapes[0] : t.shapes[1],
        t.ndims == 1 ? t.shapes[0] : t.strides[0],
        t.ndims == 1 ? t.strides[0] : t.strides[1],
        width == 4 ? qp::DType::F32 : qp::DType::F16};
    v.validate();
    return v;
}

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(int64_t *args) {
    qp::View views[7];
    for (size_t i = 0; i < 7; ++i)
        views[i] = matrix(*reinterpret_cast<const ChipTensor *>(args[i]));
    uint32_t s[12];
    for (size_t i = 0; i < 12; ++i) {
        qp::require(uint64_t(args[7 + i]) <= UINT32_MAX);
        s[i] = uint32_t(args[7 + i]);
    }
    float scale;
    std::memcpy(&scale, &s[11], sizeof(scale));
    qp::RangeGeometry g{s[0], s[1], s[2], s[3], s[4],
                        {s[5], s[6], s[7], s[8], s[9], scale}, s[10]};
    qp::decoder_range(g, views);
}
