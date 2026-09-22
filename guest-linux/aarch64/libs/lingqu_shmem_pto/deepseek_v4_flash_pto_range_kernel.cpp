#include "deepseek_v4_flash_pto_range.hpp"
#include "tensor.h"

namespace ds = lingqu::deepseek_v4_flash_pto;

static ds::F32View f32_view(const ChipTensor &tensor) {
    if ((tensor.ndims != 1 && tensor.ndims != 2) ||
        tensor.dtype != DataType::FLOAT32 ||
        tensor.start_offset > tensor.buffer.size / sizeof(float) ||
        tensor.extent_elem() >
            tensor.buffer.size / sizeof(float) - tensor.start_offset) {
        throw std::invalid_argument("deepseek_pto_kernel_f32_view");
    }
    ds::F32View view {
        tensor.buffer.addr + tensor.start_offset * sizeof(float),
        tensor.ndims == 1 ? 1 : tensor.shapes[0],
        tensor.ndims == 1 ? tensor.shapes[0] : tensor.shapes[1],
        tensor.ndims == 1 ? tensor.shapes[0] : tensor.strides[0],
        tensor.ndims == 1 ? tensor.strides[0] : tensor.strides[1],
    };
    view.validate();
    return view;
}

static ds::ByteView byte_view(const ChipTensor &tensor) {
    if (tensor.ndims != 1 || tensor.dtype != DataType::UINT8 ||
        tensor.start_offset > tensor.buffer.size ||
        tensor.extent_elem() > tensor.buffer.size - tensor.start_offset) {
        throw std::invalid_argument("deepseek_pto_kernel_byte_view");
    }
    ds::ByteView view {tensor.buffer.addr + tensor.start_offset,
                       tensor.extent_elem()};
    view.validate();
    return view;
}

static ds::I32View i32_view(const ChipTensor &tensor) {
    if (tensor.ndims != 1 || tensor.dtype != DataType::INT32 ||
        tensor.start_offset > tensor.buffer.size / sizeof(int32_t) ||
        tensor.extent_elem() >
            tensor.buffer.size / sizeof(int32_t) - tensor.start_offset) {
        throw std::invalid_argument("deepseek_pto_kernel_i32_view");
    }
    ds::I32View view {
        tensor.buffer.addr + tensor.start_offset * sizeof(int32_t),
        tensor.shapes[0],
    };
    view.validate();
    return view;
}

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

extern "C" __aicore__ __attribute__((always_inline)) void
kernel_entry(int64_t *args) {
    const auto &hidden_input =
        *reinterpret_cast<const ChipTensor *>(args[0]);
    const auto &previous_state =
        *reinterpret_cast<const ChipTensor *>(args[1]);
    const auto &token_input =
        *reinterpret_cast<const ChipTensor *>(args[2]);
    const auto &hidden_output =
        *reinterpret_cast<const ChipTensor *>(args[3]);
    const auto &next_state =
        *reinterpret_cast<const ChipTensor *>(args[4]);
    const auto &logits = *reinterpret_cast<const ChipTensor *>(args[5]);
    const auto &weight_descriptor =
        *reinterpret_cast<const ChipTensor *>(args[6]);
    const auto &geometry_descriptor =
        *reinterpret_cast<const ChipTensor *>(args[7]);
    const ds::ByteView geometry_bytes = byte_view(geometry_descriptor);
    if (geometry_bytes.bytes != 23 * sizeof(uint32_t)) {
        throw std::invalid_argument("deepseek_pto_kernel_geometry");
    }
    const auto *geometry_payload =
        reinterpret_cast<const uint8_t *>(geometry_bytes.address);
    uint32_t scalars[23];
    for (size_t index = 0; index < 23; ++index) {
        scalars[index] = ds::read_u32(geometry_payload + index * sizeof(uint32_t));
    }
    const ds::RangeGeometry geometry {
        scalars[0],  scalars[1],  scalars[2],  scalars[3],
        scalars[4],  scalars[5],  scalars[6],  scalars[7],
        scalars[8],  scalars[9],  scalars[10], scalars[11],
        scalars[12], scalars[13], scalars[14], scalars[15],
        scalars[16], scalars[17], scalars[18], scalars[19],
        scalars[20], scalars[21], scalars[22],
    };
    const ds::ByteView descriptor = byte_view(weight_descriptor);
    const ds::WeightIndex index(
        reinterpret_cast<const void *>(descriptor.address), descriptor.bytes);
    const ds::MappedWeights weights(index);
    const ds::Iq2Codebook codebook {index.iq2_grid(), index.iq2_signs()};
    ds::decoder_range(weights, codebook, geometry, f32_view(hidden_input),
                      byte_view(previous_state), i32_view(token_input),
                      f32_view(hidden_output), byte_view(next_state),
                      f32_view(logits));
}
