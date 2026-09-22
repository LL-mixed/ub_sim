#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" __attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs &) {
    return PTO2OrchestrationConfig {.expected_arg_count = 8};
}

extern "C" __attribute__((visibility("default"))) void
build_deepseek_v4_flash_pto_range_graph(const ChipTaskArgs &args) {
    if (args.tensor_count() != 8 || args.scalar_count() != 0) {
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS,
                        "DeepSeek range expects eight tensors");
        return;
    }
    CoreTaskArgs task;
    task.add_input(args.tensor(0).ref(), args.tensor(1).ref(),
                   args.tensor(2).ref());
    task.add_output(args.tensor(3).ref(), args.tensor(4).ref());
    if (args.scalar(1) == args.scalar(2)) {
        task.add_output(args.tensor(5).ref());
    } else {
        task.add_input(args.tensor(5).ref());
    }
    task.add_input(args.tensor(6).ref());
    task.add_input(args.tensor(7).ref());
    rt_submit_aiv_task(0, task);
}
