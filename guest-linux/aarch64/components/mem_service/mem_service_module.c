#include "mem_service_internal.h"
#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_observe.h"
#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_read.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_obmm_object_flow.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_object_refs.h"
#include "mem_service_record_table.h"

static struct mem_service_cluster_runtime g_mem_service_cluster_runtime;
static bool g_mem_service_cluster_runtime_initialized;

struct mem_service_cluster_runtime *mem_service_cluster_runtime_current(void)
{
    int i;

    if (!g_mem_service_cluster_runtime_initialized) {
        g_mem_service_cluster_runtime.obmm_fd = -1;
        g_mem_service_cluster_runtime.local_idx = -1;
        for (i = 0; i < MEM_SERVICE_CLUSTER_MAX_NODES; ++i) {
            g_mem_service_cluster_runtime.slots[i].region.fd = -1;
            g_mem_service_cluster_runtime.egress_import[i].fd = -1;
        }
        g_mem_service_cluster_runtime_initialized = true;
    }
    return &g_mem_service_cluster_runtime;
}
