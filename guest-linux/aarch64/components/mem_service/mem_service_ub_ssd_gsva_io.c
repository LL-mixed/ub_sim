#include "mem_service_ub_ssd_gsva_io.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/ioctl.h>
#endif

#define MEM_SERVICE_UB_SSD_IOC_MAGIC 'S'
#define MEM_SERVICE_UB_SSD_OK 0
#define MEM_SERVICE_UB_SSD_ERR_STALE_EPOCH (-6)
#define MEM_SERVICE_UB_SSD_ERR_SEGMENT_RETIRED (-7)
#define MEM_SERVICE_UB_SSD_ERR_COH_TIMEOUT (-8)
#define MEM_SERVICE_UB_SSD_ERR_CHECKSUM (-10)
#define MEM_SERVICE_UB_SSD_ERR_VERSION_CONFLICT (-11)

#ifdef __linux__
struct mem_service_ub_ssd_key_v1 {
    uint32_t version;
    uint32_t flags;
    uint64_t segment_id;
    uint64_t home_va;
    uint64_t size;
    uint64_t vmid;
    uint64_t asid;
    uint64_t pte_offset;
    uint32_t p_tag;
    uint32_t cache_policy;
    uint64_t epoch;
} __attribute__((packed));

struct mem_service_ub_ssd_block_ref_v1 {
    uint64_t block_hi;
    uint64_t block_lo;
    uint64_t version;
    uint64_t offset;
    uint64_t bytes;
    uint64_t checksum64;
} __attribute__((packed));

struct mem_service_ub_ssd_buffer_desc_v1 {
    uint64_t gsva_base;
    uint64_t bytes;
    struct mem_service_ub_ssd_key_v1 key;
    uint32_t token_id;
    uint32_t token_value;
} __attribute__((packed));

struct mem_service_ub_ssd_cmd_v1 {
    uint32_t version;
    uint32_t opcode;
    uint64_t req_id;
    uint32_t source_cna;
    uint32_t target_ssd_cna;
    uint32_t flags;
    struct mem_service_ub_ssd_block_ref_v1 block_ref;
    struct mem_service_ub_ssd_buffer_desc_v1 buffer;
} __attribute__((packed));

struct mem_service_ub_ssd_cpl_v1 {
    uint32_t version;
    uint32_t status;
    uint64_t req_id;
    struct mem_service_ub_ssd_block_ref_v1 committed_ref;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t checksum64;
    uint64_t error_detail;
} __attribute__((packed));

#define MEM_SERVICE_UB_SSD_SUBMIT \
    _IOW(MEM_SERVICE_UB_SSD_IOC_MAGIC, 1, struct mem_service_ub_ssd_cmd_v1)
#define MEM_SERVICE_UB_SSD_WAIT \
    _IOR(MEM_SERVICE_UB_SSD_IOC_MAGIC, 2, struct mem_service_ub_ssd_cpl_v1)

static enum mem_service_ub_ssd_gsva_io_status mem_service_ub_ssd_status_to_io(
    int32_t status)
{
    if (status == MEM_SERVICE_UB_SSD_OK) {
        return MEM_SERVICE_UB_SSD_GSVA_IO_OK;
    }
    if (status == MEM_SERVICE_UB_SSD_ERR_STALE_EPOCH ||
        status == MEM_SERVICE_UB_SSD_ERR_SEGMENT_RETIRED) {
        return MEM_SERVICE_UB_SSD_GSVA_IO_STALE_REF;
    }
    if (status == MEM_SERVICE_UB_SSD_ERR_COH_TIMEOUT) {
        return MEM_SERVICE_UB_SSD_GSVA_IO_TIMEOUT;
    }
    if (status == MEM_SERVICE_UB_SSD_ERR_CHECKSUM) {
        return MEM_SERVICE_UB_SSD_GSVA_IO_CHECKSUM_MISMATCH;
    }
    if (status == MEM_SERVICE_UB_SSD_ERR_VERSION_CONFLICT) {
        return MEM_SERVICE_UB_SSD_GSVA_IO_VERSION_CONFLICT;
    }
    return MEM_SERVICE_UB_SSD_GSVA_IO_INTERNAL;
}

static void mem_service_fill_ioctl_block_ref(
    struct mem_service_ub_ssd_block_ref_v1 *dst,
    const struct mem_service_ub_ssd_gsva_block_ref *src)
{
    dst->block_hi = src->block_hi;
    dst->block_lo = src->block_lo;
    dst->version = src->version;
    dst->offset = src->offset;
    dst->bytes = src->bytes;
    dst->checksum64 = src->checksum64;
}

static void mem_service_fill_ioctl_buffer_desc(
    struct mem_service_ub_ssd_buffer_desc_v1 *dst,
    const struct mem_service_ub_ssd_gsva_buffer_desc *src)
{
    dst->gsva_base = src->gsva_base;
    dst->bytes = src->bytes;
    dst->key.version = src->key_version;
    dst->key.flags = src->key_flags;
    dst->key.segment_id = src->key_segment_id;
    dst->key.home_va = src->key_home_va;
    dst->key.size = src->key_size;
    dst->key.vmid = src->key_vmid;
    dst->key.asid = src->key_asid;
    dst->key.pte_offset = src->key_pte_offset;
    dst->key.p_tag = src->key_p_tag;
    dst->key.cache_policy = src->key_cache_policy;
    dst->key.epoch = src->key_epoch;
    dst->token_id = src->token_id;
    dst->token_value = src->token_value;
}

static void mem_service_copy_completion(
    struct mem_service_ub_ssd_gsva_io_completion *dst,
    const struct mem_service_ub_ssd_cpl_v1 *src)
{
    dst->device_status = (int32_t)src->status;
    dst->request_id = src->req_id;
    dst->committed_ref.block_hi = src->committed_ref.block_hi;
    dst->committed_ref.block_lo = src->committed_ref.block_lo;
    dst->committed_ref.version = src->committed_ref.version;
    dst->committed_ref.offset = src->committed_ref.offset;
    dst->committed_ref.bytes = src->committed_ref.bytes;
    dst->committed_ref.checksum64 = src->committed_ref.checksum64;
    dst->bytes_read = src->bytes_read;
    dst->bytes_written = src->bytes_written;
    dst->checksum64 = src->checksum64;
    dst->error_detail = src->error_detail;
}
#endif

enum mem_service_ub_ssd_gsva_io_status mem_service_ub_ssd_gsva_submit(
    const struct mem_service_ub_ssd_gsva_io_request *request,
    struct mem_service_ub_ssd_gsva_io_completion *completion)
{
#ifndef __linux__
    (void)request;
    (void)completion;
    return MEM_SERVICE_UB_SSD_GSVA_IO_UNSUPPORTED;
#else
    struct mem_service_ub_ssd_cmd_v1 cmd;
    struct mem_service_ub_ssd_cpl_v1 cpl;
    const char *device_path;
    int fd;
    int rc;

    if (!request || !completion || request->buffer.gsva_base == 0 ||
        request->buffer.bytes == 0 || request->buffer.key_segment_id == 0 ||
        request->buffer.key_home_va == 0 || request->buffer.key_size == 0 ||
        request->buffer.key_epoch == 0 || request->buffer.token_id == 0 ||
        request->buffer.token_value == 0) {
        return MEM_SERVICE_UB_SSD_GSVA_IO_INVALID;
    }
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    memset(completion, 0, sizeof(*completion));
    cmd.version = 1U;
    cmd.opcode = request->opcode;
    cmd.req_id = request->request_id;
    cmd.source_cna = request->source_cna;
    cmd.target_ssd_cna = request->target_ssd_cna;
    cmd.flags = request->flags;
    mem_service_fill_ioctl_block_ref(&cmd.block_ref, &request->block_ref);
    mem_service_fill_ioctl_buffer_desc(&cmd.buffer, &request->buffer);
    device_path = request->device_path && request->device_path[0] != '\0' ?
        request->device_path : MEM_SERVICE_UB_SSD_GSVA_DEFAULT_DEVICE;
    fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENODEV || errno == ENOTTY) {
            return MEM_SERVICE_UB_SSD_GSVA_IO_UNSUPPORTED;
        }
        return MEM_SERVICE_UB_SSD_GSVA_IO_INTERNAL;
    }
    rc = ioctl(fd, MEM_SERVICE_UB_SSD_SUBMIT, &cmd);
    if (rc == 0) {
        rc = ioctl(fd, MEM_SERVICE_UB_SSD_WAIT, &cpl);
    }
    close(fd);
    if (rc != 0) {
        if (errno == ENOTTY || errno == ENODEV) {
            return MEM_SERVICE_UB_SSD_GSVA_IO_UNSUPPORTED;
        }
        return MEM_SERVICE_UB_SSD_GSVA_IO_INTERNAL;
    }
    mem_service_copy_completion(completion, &cpl);
    return mem_service_ub_ssd_status_to_io(completion->device_status);
#endif
}
