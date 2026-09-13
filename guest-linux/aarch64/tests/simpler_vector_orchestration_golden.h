// Metadata-only test double for generated orchestration. No model or runtime
// is loaded. The test exposes this file as pto_orchestration_api.h in a temp dir.
#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

enum class DataType { FLOAT32, INT32 };
struct ChipTensor {
    std::array<uint32_t, 5> shapes{};
    uint32_t ndims = 2;
    DataType dtype = DataType::FLOAT32;
    int id = 0;
    uint64_t numel() const {
        uint64_t result = 1;
        for (uint32_t i = 0; i < ndims; ++i) result *= shapes[i];
        return result;
    }
    const ChipTensor& ref() const { return *this; }
};
struct ChipTaskArgs {
    std::array<ChipTensor, 3> tensors;
    int count = 3;
    int scalars = 0;
    int tensor_count() const { return count; }
    int scalar_count() const { return scalars; }
    const ChipTensor& tensor(int index) const {
        assert(index < count);
        return tensors.at(index);
    }
};
struct PTO2OrchestrationConfig { int expected_arg_count; };
struct TensorCreateInfo {
    ChipTensor tensor;
    TensorCreateInfo(const uint32_t* shapes, uint32_t ndims, DataType dtype) {
        tensor.ndims = ndims;
        tensor.dtype = dtype;
        for (uint32_t i = 0; i < ndims; ++i) tensor.shapes[i] = shapes[i];
    }
};
struct CoreTaskArgs {
    std::vector<ChipTensor> inputs;
    std::vector<uint64_t> scalars;
    ChipTensor output;
    bool created = false;
    void add_input(const ChipTensor& tensor) { inputs.push_back(tensor); }
    void add_output(const ChipTensor& tensor) { output = tensor; }
    void add_output(const TensorCreateInfo& info) {
        output = info.tensor;
        created = true;
    }
    void add_scalar(uint64_t scalar) { scalars.push_back(scalar); }
};
struct TaskOutputTensors {
    ChipTensor tensor;
    const ChipTensor& get_ref(int index) const & { assert(index == 0); return tensor; }
    const ChipTensor& get_ref(int) const && = delete;
};
constexpr int PTO2_ERROR_INVALID_ARGS = 17;
static int fatal_code;
static unsigned submitted;
static uint64_t expected_storage;
static uint64_t expected_logical;
static void rt_report_fatal(int code, const char*) { fatal_code = code; }
#define LOG_INFO(...) ((void)0)

static TaskOutputTensors rt_submit_aiv_task(int function, const CoreTaskArgs& args) {
    const std::array<int, 4> functions{0, 1, 1, 2};
    const std::array<std::vector<int>, 4> dependencies{{{0, 1}, {3}, {3}, {4, 5}}};
    assert(submitted < 4 && function == functions[submitted]);
    assert(args.created == (submitted < 3));
    assert(args.output.dtype == DataType::FLOAT32);
    assert(args.output.numel() == (args.created ? expected_storage : expected_logical));
    assert(args.inputs.size() == dependencies[submitted].size());
    for (unsigned i = 0; i < args.inputs.size(); ++i) {
        assert(args.inputs[i].id == dependencies[submitted][i]);
        if (args.inputs[i].id >= 3) assert(args.inputs[i].numel() == expected_storage);
    }
    if (submitted == 1 || submitted == 2) {
        assert(args.scalars.size() == 1);
        assert(args.scalars[0] == (submitted == 1 ? 0x3f800000ULL : 0x40000000ULL));
    } else {
        assert(args.scalars.empty());
    }
    ChipTensor output = args.output;
    if (args.created) output.id = 3 + submitted;
    else assert(output.id == 2);
    ++submitted;
    return {output};
}

extern "C" PTO2OrchestrationConfig aicpu_orchestration_config(const ChipTaskArgs&);
extern "C" void aicpu_orchestration_entry(const ChipTaskArgs&);

int main(int argc, char** argv) {
    assert(argc == 4);
    const auto rows = static_cast<uint32_t>(std::stoul(argv[1]));
    const auto cols = static_cast<uint32_t>(std::stoul(argv[2]));
    expected_logical = uint64_t(rows) * cols;
    expected_storage = std::stoull(argv[3]);
    ChipTaskArgs valid;
    for (int rank : {1, 2, 5}) {
        for (int i = 0; i < 3; ++i) {
            valid.tensors[i].id = i;
            valid.tensors[i].ndims = rank;
            valid.tensors[i].shapes = rank == 1
                ? std::array<uint32_t, 5>{static_cast<uint32_t>(expected_logical), 0, 0, 0, 0}
                : rank == 2 ? std::array<uint32_t, 5>{rows, cols, 0, 0, 0}
                            : std::array<uint32_t, 5>{1, 1, 1, rows, cols};
        }
        submitted = 0;
        fatal_code = 0;
        assert(aicpu_orchestration_config(valid).expected_arg_count == 3);
        aicpu_orchestration_entry(valid);
        assert(submitted == 4 && fatal_code == 0);
    }
    for (int fault = 0; fault < 8; ++fault) {
        ChipTaskArgs bad = valid;
        if (fault == 0) bad.count = 2;
        else if (fault == 1) bad.scalars = 1;
        else if (fault < 5) bad.tensors[fault - 2].shapes[0] = 0;
        else bad.tensors[fault - 5].dtype = DataType::INT32;
        submitted = 0;
        fatal_code = 0;
        aicpu_orchestration_entry(bad);
        assert(submitted == 0 && fatal_code == PTO2_ERROR_INVALID_ARGS);
    }
    std::cout << "quadratic metadata: positive=3 negative=8 storage="
              << expected_storage << '\n';
}
