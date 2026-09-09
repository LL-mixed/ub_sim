#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" __attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs &) {
    return PTO2OrchestrationConfig{.expected_arg_count = 7};
}

extern "C" __attribute__((visibility("default"))) void
build_qwen3_pto_operator_graph(const ChipTaskArgs &args) {
    if (args.tensor_count() != 4 || args.scalar_count() != 3 ||
        args.scalar(0) < 1 || args.scalar(0) > 8 ||
        args.scalar(1) > UINT32_MAX || args.scalar(2) > UINT32_MAX) {
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "Qwen3 PTO expects four matrices and three scalars");
        return;
    }
    for (size_t i = 0; i < 4; ++i) {
        const auto &tensor = args.tensor(i).ref();
        if (tensor.ndims != 2 || !tensor.shapes[0] || !tensor.shapes[1] ||
            (tensor.dtype != DataType::FLOAT32 && tensor.dtype != DataType::FLOAT16)) {
            rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "Qwen3 PTO requires nonempty F16/F32 matrices");
            return;
        }
    }
    CoreTaskArgs task;
    task.add_input(args.tensor(0).ref(), args.tensor(1).ref(), args.tensor(2).ref());
    task.add_output(args.tensor(3).ref());
    task.add_scalar(args.scalar(0), args.scalar(1), args.scalar(2));
    rt_submit_aiv_task(0, task);
}
