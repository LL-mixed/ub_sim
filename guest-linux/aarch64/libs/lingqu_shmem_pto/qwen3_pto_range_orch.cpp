#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" __attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs &) {
    return PTO2OrchestrationConfig{.expected_arg_count = 19};
}

extern "C" __attribute__((visibility("default"))) void
build_qwen3_pto_range_graph(const ChipTaskArgs &args) {
    if (args.tensor_count() != 7 || args.scalar_count() != 12) {
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "Qwen3 range expects seven tensors and twelve scalars");
        return;
    }
    for (size_t i = 0; i < 12; ++i) {
        if (args.scalar(i) > UINT32_MAX) {
            rt_report_fatal(PTO2_ERROR_INVALID_ARGS, "Qwen3 range scalar overflow");
            return;
        }
    }
    CoreTaskArgs task;
    task.add_input(args.tensor(0).ref(), args.tensor(1).ref(), args.tensor(2).ref());
    task.add_output(args.tensor(3).ref());
    task.add_inout(args.tensor(4).ref());
    // Only the terminal range produces logits. Earlier ranges bind a read-only
    // placeholder, so completion never fences an unwritten output.
    if (args.scalar(1) == args.scalar(2)) task.add_output(args.tensor(5).ref());
    else task.add_input(args.tensor(5).ref());
    task.add_input(args.tensor(6).ref());
    for (size_t i = 0; i < 12; ++i) task.add_scalar(args.scalar(i));
    rt_submit_aiv_task(0, task);
}
