#include "qwen3_pto_ops.hpp"
#include "tensor.h"
#include <cstring>

namespace qp = lingqu::qwen3_pto;

static qp::View matrix_view(const ChipTensor &tensor) {
    qp::require(tensor.ndims == 2 && (tensor.dtype == DataType::FLOAT32 ||
                                     tensor.dtype == DataType::FLOAT16));
    const uint64_t width = tensor.dtype == DataType::FLOAT32 ? 4 : 2;
    qp::require(tensor.start_offset <= tensor.buffer.size / width &&
                tensor.extent_elem() <= tensor.buffer.size / width - tensor.start_offset);
    qp::View view{tensor.buffer.addr + tensor.start_offset * width, tensor.shapes[0],
                   tensor.shapes[1], tensor.strides[0], tensor.strides[1],
                   width == 4 ? qp::DType::F32 : qp::DType::F16};
    view.validate();
    return view;
}

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void kernel_entry(int64_t *args) {
    qp::View views[4];
    for (size_t i = 0; i < 4; ++i)
        views[i] = matrix_view(*reinterpret_cast<const ChipTensor *>(args[i]));
    qp::require(uint64_t(args[4]) <= UINT32_MAX && uint64_t(args[5]) <= UINT32_MAX &&
                uint64_t(args[6]) <= UINT32_MAX);
    const uint32_t scale_bits = uint32_t(args[6]);
    float scale;
    std::memcpy(&scale, &scale_bits, sizeof(scale));
    qp::execute(qp::Operation(args[4]), views[0], views[1], views[2], views[3],
                 uint32_t(args[5]), scale);
}
