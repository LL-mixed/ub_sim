#pragma once

#include "deepseek_v4_flash_pto_ops.hpp"

#include <optional>

namespace lingqu::deepseek_v4_flash_pto {

constexpr uint64_t kRangeStateMagic = 0x4453345253544154ULL;
constexpr uint64_t kRangeStateVersion = 1;

using U8Row = pto::Tile<pto::TileType::Vec, uint8_t, 1, kVectorCapacity,
                        pto::BLayout::RowMajor, 1, pto::DYNAMIC>;

struct ByteView {
    uintptr_t address;
    uint64_t bytes;

    void validate() const {
        if (!address || !bytes || address > UINTPTR_MAX - bytes) {
            throw std::invalid_argument("deepseek_pto_state_byte_view");
        }
    }

    uintptr_t at(uint64_t offset, uint64_t length) const {
        validate();
        if (offset > bytes || length > bytes - offset) {
            throw std::out_of_range("deepseek_pto_state_byte_bounds");
        }
        return address + offset;
    }
};

inline uint64_t load_u64(const ByteView &source, uint64_t offset) {
    std::array<uint8_t, 8> encoded {};
    U8Row tile(8);
    auto input = global_row<uint8_t>(source.at(offset, 8), 8);
    auto output = global_row<uint8_t>(
        reinterpret_cast<uintptr_t>(encoded.data()), encoded.size());
    pto::TLOAD(tile, input);
    pto::TSTORE(output, tile);
    ++operator_counts.tload;
    ++operator_counts.tstore;
    uint64_t value = 0;
    for (uint32_t byte = 0; byte < 8; ++byte) {
        value |= uint64_t(encoded[byte]) << (8 * byte);
    }
    return value;
}

inline void store_u64(const ByteView &destination, uint64_t offset,
                      uint64_t value) {
    std::array<uint8_t, 8> encoded {};
    for (uint32_t byte = 0; byte < 8; ++byte) {
        encoded[byte] = static_cast<uint8_t>(value >> (8 * byte));
    }
    U8Row tile(8);
    auto input = global_row<uint8_t>(
        reinterpret_cast<uintptr_t>(encoded.data()), encoded.size());
    auto output = global_row<uint8_t>(destination.at(offset, 8), 8);
    pto::TLOAD(tile, input);
    pto::TSTORE(output, tile);
    ++operator_counts.tload;
    ++operator_counts.tstore;
}

inline uint32_t compression_ratio(uint32_t layer) {
    if (layer < 2) return 0;
    return layer % 2 == 0 ? 4 : 128;
}

struct RangeStateGeometry {
    uint32_t first;
    uint32_t end;
    uint32_t positions;
    uint32_t head_dim;
    uint32_t indexer_head_dim;
    uint32_t sliding_window;
    uint32_t compressed_capacity;

    void validate() const {
        if (first >= end || end > 43 || !positions || !head_dim ||
            !indexer_head_dim || !sliding_window || !compressed_capacity) {
            throw std::invalid_argument("deepseek_pto_state_geometry");
        }
    }
};

struct LayerStateView {
    uint32_t ratio;
    F32View raw;
    std::optional<F32View> compressed;
    std::optional<F32View> indexer_compressed;
    std::optional<CompressorStateView> attention_compressor;
    std::optional<CompressorStateView> indexer_compressor;
};

struct RangeStateView {
    RangeStateGeometry geometry;
    std::vector<LayerStateView> layers;
};

class StateCursor {
    ByteView bytes_;
    uint64_t offset_ = 0;
    bool write_ = false;

public:
    StateCursor(ByteView bytes, bool write) : bytes_(bytes), write_(write) {
        bytes_.validate();
    }

    uint64_t offset() const { return offset_; }

    void finish() const {
        if (offset_ != bytes_.bytes) {
            throw std::invalid_argument("deepseek_pto_state_trailing_bytes");
        }
    }

    uint64_t u64(uint64_t expected) {
        const uint64_t actual = write_ ? expected : load_u64(bytes_, offset_);
        if (write_) store_u64(bytes_, offset_, expected);
        offset_ += 8;
        if (actual != expected) {
            throw std::invalid_argument("deepseek_pto_state_metadata");
        }
        return actual;
    }

    std::optional<F32View> matrix(uint32_t rows, uint32_t columns) {
        const uint64_t elements = uint64_t(rows) * columns;
        u64(elements);
        if (!elements) return std::nullopt;
        const uint64_t payload_bytes = elements * sizeof(float);
        const uintptr_t address = bytes_.at(offset_, payload_bytes);
        offset_ += payload_bytes;
        return F32View {address, rows, columns, columns, 1};
    }

    CompressorStateView compressor(uint32_t head_dim, uint32_t ratio) {
        const uint32_t lanes = ratio == 4 ? 2 : 1;
        const uint32_t rows = ratio * lanes;
        const uint32_t width = head_dim * lanes;
        u64(head_dim);
        u64(ratio);
        const auto kv = matrix(rows, width);
        const auto score = matrix(rows, width);
        if (!kv || !score) {
            throw std::invalid_argument("deepseek_pto_state_compressor_empty");
        }
        return CompressorStateView {*kv, *score, head_dim, ratio};
    }
};

inline RangeStateView range_state(const ByteView &bytes,
                                  const RangeStateGeometry &geometry,
                                  bool initialize) {
    geometry.validate();
    StateCursor cursor(bytes, initialize);
    cursor.u64(kRangeStateMagic);
    cursor.u64(kRangeStateVersion);
    cursor.u64(geometry.first);
    cursor.u64(geometry.end);
    RangeStateView result {geometry, {}};
    result.layers.reserve(geometry.end - geometry.first);
    const uint32_t raw_rows =
        std::min(geometry.positions, geometry.sliding_window);
    for (uint32_t layer = geometry.first; layer < geometry.end; ++layer) {
        const uint32_t ratio = compression_ratio(layer);
        cursor.u64(ratio);
        const auto raw = cursor.matrix(raw_rows, geometry.head_dim);
        if (!raw) {
            throw std::invalid_argument("deepseek_pto_state_raw_empty");
        }
        LayerStateView view {ratio, *raw, std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt};
        if (ratio) {
            const uint32_t compressed_rows = std::min(
                geometry.positions / ratio, geometry.compressed_capacity);
            view.compressed = cursor.matrix(compressed_rows, geometry.head_dim);
            if (ratio == 4) {
                view.indexer_compressed =
                    cursor.matrix(compressed_rows, geometry.indexer_head_dim);
            }
            view.attention_compressor =
                cursor.compressor(geometry.head_dim, ratio);
            if (ratio == 4) {
                view.indexer_compressor =
                    cursor.compressor(geometry.indexer_head_dim, ratio);
            }
        }
        result.layers.push_back(view);
    }
    cursor.finish();
    return result;
}

inline void copy_prefix_rows(const F32View &source, const F32View &destination,
                             uint32_t rows) {
    if (source.columns != destination.columns || rows > source.rows ||
        rows > destination.rows) {
        throw std::invalid_argument("deepseek_pto_state_copy_rows");
    }
    if (!rows) return;
    const F32View source_prefix {source.address, rows, source.columns,
                                 source.row_stride, source.column_stride};
    const F32View destination_prefix {
        destination.address, rows, destination.columns, destination.row_stride,
        destination.column_stride};
    copy(source_prefix, destination_prefix);
}

inline void copy_compressor(const CompressorStateView &source,
                            const CompressorStateView &destination) {
    source.validate();
    destination.validate();
    if (source.head_dim != destination.head_dim ||
        source.compress_ratio != destination.compress_ratio) {
        throw std::invalid_argument("deepseek_pto_state_compressor_copy");
    }
    copy(source.kv, destination.kv);
    copy(source.score, destination.score);
}

inline void copy_range_state(const RangeStateView &source,
                             const RangeStateView &destination) {
    if (source.geometry.first != destination.geometry.first ||
        source.geometry.end != destination.geometry.end ||
        source.layers.size() != destination.layers.size()) {
        throw std::invalid_argument("deepseek_pto_state_range_copy");
    }
    for (size_t index = 0; index < source.layers.size(); ++index) {
        const LayerStateView &from = source.layers[index];
        const LayerStateView &to = destination.layers[index];
        if (from.ratio != to.ratio) {
            throw std::invalid_argument("deepseek_pto_state_layer_copy");
        }
        copy_prefix_rows(from.raw, to.raw, from.raw.rows);
        if (from.compressed) {
            if (!to.compressed) {
                throw std::invalid_argument("deepseek_pto_state_compressed_copy");
            }
            copy_prefix_rows(*from.compressed, *to.compressed,
                             from.compressed->rows);
        }
        if (from.indexer_compressed) {
            if (!to.indexer_compressed) {
                throw std::invalid_argument("deepseek_pto_state_indexer_copy");
            }
            copy_prefix_rows(*from.indexer_compressed, *to.indexer_compressed,
                             from.indexer_compressed->rows);
        }
        if (from.attention_compressor) {
            if (!to.attention_compressor) {
                throw std::invalid_argument("deepseek_pto_state_attention_copy");
            }
            copy_compressor(*from.attention_compressor,
                            *to.attention_compressor);
        }
        if (from.indexer_compressor) {
            if (!to.indexer_compressor) {
                throw std::invalid_argument("deepseek_pto_state_indexer_compressor_copy");
            }
            copy_compressor(*from.indexer_compressor, *to.indexer_compressor);
        }
    }
}

} // namespace lingqu::deepseek_v4_flash_pto
