#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lingqu::deepseek_v4_flash_pto {

constexpr uint64_t kWeightIndexMagic = 0x32494f5450345344ULL;
constexpr uint32_t kWeightIndexVersion = 2;
constexpr uint32_t kWeightIndexHeaderBytes = 64;
constexpr uint32_t kWeightIndexRecordBytes = 112;
constexpr uint32_t kWeightIndexMaxRank = 8;
constexpr uint32_t kIq2GridEntries = 256;
constexpr uint32_t kIq2SignEntries = 128;
constexpr uint64_t kIq2CodebookBytes =
    uint64_t(kIq2GridEntries) * sizeof(uint64_t) + kIq2SignEntries;

inline uint32_t read_u32(const uint8_t *bytes) {
    uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap32(value);
#endif
    return value;
}

inline uint64_t read_u64(const uint8_t *bytes) {
    uint64_t value;
    std::memcpy(&value, bytes, sizeof(value));
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    return value;
}

inline uint64_t fnv1a(std::string_view value) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 0x00000100000001b3ULL;
    }
    return hash;
}

struct WeightTensor {
    const uint8_t *payload;
    uint64_t bytes;
    uint64_t elements;
    uint64_t dimensions[kWeightIndexMaxRank];
    uint32_t tensor_type;
    uint32_t rank;
};

class WeightIndex {
public:
    WeightIndex(const void *descriptor, uint64_t descriptor_bytes)
        : descriptor_(static_cast<const uint8_t *>(descriptor)),
          descriptor_bytes_(descriptor_bytes) {
        if (!descriptor_ || descriptor_bytes_ < kWeightIndexHeaderBytes ||
            read_u64(descriptor_) != kWeightIndexMagic ||
            read_u32(descriptor_ + 8) != kWeightIndexVersion ||
            read_u32(descriptor_ + 12) != kWeightIndexHeaderBytes ||
            read_u32(descriptor_ + 36) != kWeightIndexRecordBytes) {
            throw std::invalid_argument("deepseek_pto_weight_index_header");
        }
        model_bytes_ = read_u64(descriptor_ + 16);
        catalog_fingerprint_ = read_u64(descriptor_ + 24);
        record_count_ = read_u32(descriptor_ + 32);
        records_offset_ = read_u64(descriptor_ + 40);
        strings_offset_ = read_u64(descriptor_ + 48);
        strings_bytes_ = read_u32(descriptor_ + 56);
        path_bytes_ = read_u32(descriptor_ + 60);
        if (!model_bytes_ || !catalog_fingerprint_ || !record_count_ ||
            records_offset_ != kWeightIndexHeaderBytes) {
            throw std::invalid_argument("deepseek_pto_weight_index_control");
        }
        const uint64_t records_bytes = checked_mul(record_count_, kWeightIndexRecordBytes);
        if (checked_add(records_offset_, records_bytes) != strings_offset_) {
            throw std::invalid_argument("deepseek_pto_weight_index_layout");
        }
        path_offset_ = checked_add(strings_offset_, strings_bytes_);
        codebook_offset_ = checked_add(path_offset_, path_bytes_);
        if (!path_bytes_ ||
            checked_add(codebook_offset_, kIq2CodebookBytes) !=
                descriptor_bytes_) {
            throw std::invalid_argument("deepseek_pto_weight_index_layout");
        }
        const auto path = model_path();
        if (path.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("deepseek_pto_weight_index_path");
        }
        for (uint32_t index = 0; index < record_count_; ++index) {
            validate_record(record(index));
        }
        for (uint32_t index = 0; index < kIq2GridEntries; ++index) {
            iq2_grid_[index] =
                read_u64(descriptor_ + codebook_offset_ + uint64_t(index) * 8);
        }
        std::memcpy(iq2_signs_.data(),
                    descriptor_ + codebook_offset_ + kIq2GridEntries * 8,
                    iq2_signs_.size());
    }

    uint64_t model_bytes() const { return model_bytes_; }
    uint64_t catalog_fingerprint() const { return catalog_fingerprint_; }
    uint32_t tensor_count() const { return record_count_; }
    const std::array<uint64_t, kIq2GridEntries> &iq2_grid() const {
        return iq2_grid_;
    }
    const std::array<uint8_t, kIq2SignEntries> &iq2_signs() const {
        return iq2_signs_;
    }

    std::string_view model_path() const {
        return std::string_view(
            reinterpret_cast<const char *>(descriptor_ + path_offset_), path_bytes_);
    }

    const uint8_t *find_record(std::string_view name) const {
        const uint64_t hash = fnv1a(name);
        for (uint32_t index = 0; index < record_count_; ++index) {
            const uint8_t *candidate = record(index);
            if (read_u64(candidate) != hash) continue;
            const uint32_t offset = read_u32(candidate + 96);
            const uint32_t bytes = read_u32(candidate + 100);
            if (bytes == name.size() &&
                std::memcmp(descriptor_ + strings_offset_ + offset,
                            name.data(), bytes) == 0) {
                return candidate;
            }
        }
        throw std::invalid_argument("deepseek_pto_weight_tensor_missing");
    }

    bool contains(std::string_view name) const {
        try {
            (void)find_record(name);
            return true;
        } catch (const std::invalid_argument &) {
            return false;
        }
    }

private:
    static uint64_t checked_add(uint64_t left, uint64_t right) {
        if (right > std::numeric_limits<uint64_t>::max() - left) {
            throw std::invalid_argument("deepseek_pto_weight_index_overflow");
        }
        return left + right;
    }

    static uint64_t checked_mul(uint64_t left, uint64_t right) {
        if (left && right > std::numeric_limits<uint64_t>::max() / left) {
            throw std::invalid_argument("deepseek_pto_weight_index_overflow");
        }
        return left * right;
    }

    const uint8_t *record(uint32_t index) const {
        return descriptor_ + records_offset_ + uint64_t(index) * kWeightIndexRecordBytes;
    }

    void validate_record(const uint8_t *record) const {
        const uint64_t offset = read_u64(record + 8);
        const uint64_t bytes = read_u64(record + 16);
        const uint64_t elements = read_u64(record + 24);
        const uint32_t name_offset = read_u32(record + 96);
        const uint32_t name_bytes = read_u32(record + 100);
        const uint32_t rank = read_u32(record + 108);
        if (!bytes || !elements || !name_bytes || !rank || rank > kWeightIndexMaxRank ||
            offset > model_bytes_ || bytes > model_bytes_ - offset ||
            name_offset > strings_bytes_ || name_bytes > strings_bytes_ - name_offset) {
            throw std::invalid_argument("deepseek_pto_weight_index_record");
        }
        uint64_t product = 1;
        for (uint32_t dimension = 0; dimension < kWeightIndexMaxRank; ++dimension) {
            const uint64_t value = read_u64(record + 32 + dimension * 8);
            if (dimension < rank) {
                if (!value) {
                    throw std::invalid_argument("deepseek_pto_weight_index_dimension");
                }
                product = checked_mul(product, value);
            } else if (value) {
                throw std::invalid_argument("deepseek_pto_weight_index_dimension");
            }
        }
        if (product != elements) {
            throw std::invalid_argument("deepseek_pto_weight_index_elements");
        }
        const std::string_view name(
            reinterpret_cast<const char *>(descriptor_ + strings_offset_ + name_offset),
            name_bytes);
        if (read_u64(record) != fnv1a(name)) {
            throw std::invalid_argument("deepseek_pto_weight_index_name_hash");
        }
    }

    const uint8_t *descriptor_;
    uint64_t descriptor_bytes_;
    uint64_t model_bytes_ = 0;
    uint64_t catalog_fingerprint_ = 0;
    uint64_t records_offset_ = 0;
    uint64_t strings_offset_ = 0;
    uint64_t path_offset_ = 0;
    uint64_t codebook_offset_ = 0;
    uint32_t record_count_ = 0;
    uint32_t strings_bytes_ = 0;
    uint32_t path_bytes_ = 0;
    std::array<uint64_t, kIq2GridEntries> iq2_grid_ {};
    std::array<uint8_t, kIq2SignEntries> iq2_signs_ {};
};

class MappedWeights {
public:
    explicit MappedWeights(const WeightIndex &index) : index_(index) {
        const std::string path(index.model_path());
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error("deepseek_pto_weight_open");
        }
        struct stat metadata {};
        if (::fstat(fd_, &metadata) != 0 || metadata.st_size < 0 ||
            uint64_t(metadata.st_size) != index_.model_bytes()) {
            close_file();
            throw std::runtime_error("deepseek_pto_weight_file_identity");
        }
        mapping_ = ::mmap(nullptr, index_.model_bytes(), PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            close_file();
            throw std::runtime_error("deepseek_pto_weight_mmap");
        }
    }

    MappedWeights(const MappedWeights &) = delete;
    MappedWeights &operator=(const MappedWeights &) = delete;

    ~MappedWeights() {
        if (mapping_) ::munmap(mapping_, index_.model_bytes());
        close_file();
    }

    WeightTensor tensor(std::string_view name) const {
        const uint8_t *record = index_.find_record(name);
        WeightTensor result {
            .payload = static_cast<const uint8_t *>(mapping_) + read_u64(record + 8),
            .bytes = read_u64(record + 16),
            .elements = read_u64(record + 24),
            .dimensions = {},
            .tensor_type = read_u32(record + 104),
            .rank = read_u32(record + 108),
        };
        for (uint32_t index = 0; index < kWeightIndexMaxRank; ++index) {
            result.dimensions[index] = read_u64(record + 32 + index * 8);
        }
        return result;
    }

    bool contains(std::string_view name) const { return index_.contains(name); }

private:
    void close_file() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    const WeightIndex &index_;
    int fd_ = -1;
    void *mapping_ = nullptr;
};

} // namespace lingqu::deepseek_v4_flash_pto
