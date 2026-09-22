#include "pto_orchestration_api.h"
#include <cstdint>

extern "C" __attribute__((visibility("default"))) PTO2OrchestrationConfig
aicpu_orchestration_config(const ChipTaskArgs &) {
    return PTO2OrchestrationConfig {.expected_arg_count = 30};
}

extern "C" __attribute__((visibility("default"))) void
build_deepseek_v4_flash_pto_range_graph(const ChipTaskArgs &args) {
    if (args.tensor_count() != 7 || args.scalar_count() != 23) {
        rt_report_fatal(PTO2_ERROR_INVALID_ARGS,
                        "DeepSeek range expects seven tensors and 23 scalars");
        return;
    }
    for (size_t index = 0; index < 23; ++index) {
        if (args.scalar(index) > UINT32_MAX) {
            rt_report_fatal(PTO2_ERROR_INVALID_ARGS,
                            "DeepSeek range scalar overflow");
            return;
        }
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
    for (size_t index = 0; index < 23; ++index) {
        task.add_scalar(args.scalar(index));
    }
    rt_submit_aiv_task(0, task);
}
