// SPDX-License-Identifier: GPL-2.0
#include <asm/mmu_context.h>
#include <linux/cdev.h>
#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/obmm.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <ub/obmm_async.h>
#include <ub/obmm_scc.h>

#define LINQU_UB_REG_IRQ_STATUS 0x078
#define LINQU_UB_REG_IRQ_ACK    0x080
#define LINQU_UB_ENDPOINT1_OFFSET 0x1000
#define LINQU_UB_OBMM_ASYNC_OFFSET 0x2000
#define LINQU_UB_OBMM_SCC_OFFSET 0x3000

#define OBMM_ASYNC_REG_VERSION_CAPS 0x000
#define OBMM_ASYNC_REG_STATUS 0x008
#define OBMM_ASYNC_REG_SQ_BASE 0x010
#define OBMM_ASYNC_REG_SQ_SIZE 0x018
#define OBMM_ASYNC_REG_SQ_HEAD 0x020
#define OBMM_ASYNC_REG_SQ_TAIL 0x028
#define OBMM_ASYNC_REG_CQ_BASE 0x030
#define OBMM_ASYNC_REG_CQ_SIZE 0x038
#define OBMM_ASYNC_REG_CQ_HEAD 0x040
#define OBMM_ASYNC_REG_CQ_TAIL 0x048
#define OBMM_ASYNC_REG_DOORBELL 0x050
#define OBMM_ASYNC_REG_IRQ_STATUS 0x058
#define OBMM_ASYNC_REG_IRQ_ACK 0x060
#define OBMM_ASYNC_REG_LAST_ERROR 0x068
#define OBMM_ASYNC_REG_QUEUE_ID 0x070
#define OBMM_ASYNC_REG_MAP_LOCAL_PA 0x100
#define OBMM_ASYNC_REG_MAP_LENGTH 0x108
#define OBMM_ASYNC_REG_MAP_ID 0x110
#define OBMM_ASYNC_REG_MAP_GENERATION 0x118
#define OBMM_ASYNC_REG_MAP_CMD 0x120
#define OBMM_ASYNC_REG_BUFFER_BASE 0x128
#define OBMM_ASYNC_REG_BUFFER_LENGTH 0x130
#define OBMM_ASYNC_REG_BUFFER_ID 0x138
#define OBMM_ASYNC_REG_BUFFER_GENERATION 0x140
#define OBMM_ASYNC_REG_BUFFER_CMD 0x148
#define OBMM_ASYNC_REG_CANCEL_TOKEN 0x150
#define OBMM_ASYNC_REG_CANCEL_CMD 0x158
#define OBMM_ASYNC_REG_GUEST_MONOTONIC_NS 0x160
#define OBMM_ASYNC_REG_OBSERVABILITY_RESET 0x1f8
#define OBMM_ASYNC_REG_OBSERVABILITY_BASE 0x200
#define OBMM_ASYNC_OBSERVABILITY_VALUES 21

#define OBMM_SCC_REG_VERSION_CAPS 0x000
#define OBMM_SCC_REG_STATUS 0x008
#define OBMM_SCC_REG_LAST_ERROR 0x010
#define OBMM_SCC_REG_OWNER_GENERATION 0x018
#define OBMM_SCC_REG_MAP_GSVA_BASE 0x020
#define OBMM_SCC_REG_MAP_LOCAL_PA 0x028
#define OBMM_SCC_REG_MAP_LENGTH 0x030
#define OBMM_SCC_REG_MAP_ID 0x038
#define OBMM_SCC_REG_MAP_GENERATION 0x040
#define OBMM_SCC_REG_MAP_COMMAND 0x048
#define OBMM_SCC_REG_SESSION_COMMAND 0x0a0
#define OBMM_SCC_REG_CLOCK_MHZ 0x0f8
#define OBMM_SCC_REG_LOAD_TIMEOUT_NS 0x100
#define OBMM_SCC_REG_OWNER_TTBR0 0x108
#define OBMM_SCC_REG_MAP_MODEL_GENERATION 0x110
#define OBMM_SCC_REG_MAP_FLAGS 0x118
#define OBMM_SCC_REG_UPCALL_ENTRY 0x120
#define OBMM_SCC_REG_LOGICAL_CONTEXTS 0x128
#define OBMM_SCC_REG_EVENT_SEQUENCE 0x138
#define OBMM_SCC_REG_EVENT_CONTEXT_ID 0x140
#define OBMM_SCC_REG_EVENT_PLT_TOKEN 0x148
#define OBMM_SCC_REG_EVENT_INTERRUPTED_PC 0x150
#define OBMM_SCC_REG_EVENT_FAULT_PC 0x158
#define OBMM_SCC_REG_EVENT_VA 0x160
#define OBMM_SCC_REG_EVENT_VALUE 0x168
#define OBMM_SCC_REG_EVENT_KIND_STATUS 0x170
#define OBMM_SCC_REG_EVENT_META 0x178
#define OBMM_SCC_REG_EVENT_COMMAND 0x180
#define OBMM_SCC_REG_SCHEDULER_COMMAND 0x188
#define OBMM_SCC_REG_STATS_BASE 0x200
#define OBMM_SCC_REG_OBSERVABILITY_BASE \
	(OBMM_SCC_REG_STATS_BASE + 17 * 8)
#define OBMM_SCC_OBSERVABILITY_VALUES 17
#define OBMM_SCC_MAX_LOAD_TIMEOUT_NS 10000000000ULL

#define OBMM_SCC_STATUS_ACTIVE BIT(0)
#define OBMM_SCC_STATUS_FAIL_STOP BIT(1)
#define OBMM_SCC_STATUS_EVENT_PENDING BIT(3)
#define OBMM_SCC_STATUS_EVENT_DELIVERED BIT(4)
#define OBMM_SCC_STATUS_UPCALL_ACTIVE BIT(5)

#define OBMM_ASYNC_QUEUE_BYTES \
	(2 * OBMM_ASYNC_QUEUE_DEPTH * OBMM_ASYNC_SLOT_BYTES)
#define OBMM_ASYNC_BUFFER_ARENA_BYTES \
	(OBMM_ASYNC_MAX_BUFFERS * OBMM_ASYNC_MAX_BUFFER_BYTES)

static_assert(sizeof(struct obmm_async_sq_entry_v1) ==
	      OBMM_ASYNC_SLOT_BYTES);
static_assert(sizeof(struct obmm_async_cq_entry_v1) ==
	      OBMM_ASYNC_SLOT_BYTES);
static_assert(offsetof(struct obmm_async_sq_entry_v1, token) == 8);
static_assert(offsetof(struct obmm_async_sq_entry_v1, user_data) == 56);
static_assert(offsetof(struct obmm_async_cq_entry_v1, token) == 8);
static_assert(offsetof(struct obmm_async_cq_entry_v1, reserved) == 56);
static_assert(sizeof(struct obmm_async_observability_v1) == 168);
static_assert(sizeof(struct obmm_scc_context_v2) ==
	      OBMM_SCC_CONTEXT_STATE_BYTES);
static_assert(offsetof(struct obmm_scc_context_v2, x) == 16);
static_assert(offsetof(struct obmm_scc_context_v2, sp) == 264);
static_assert(offsetof(struct obmm_scc_context_v2, pc) == 272);
static_assert(offsetof(struct obmm_scc_context_v2, q) == 288);
static_assert(offsetof(struct obmm_scc_context_v2, fpcr) == 800);
static_assert(sizeof(struct obmm_scc_caps_v2) == 64);
static_assert(sizeof(struct obmm_scc_map_register_v1) == 64);
static_assert(sizeof(struct obmm_scc_start_v2) == 40);
static_assert(sizeof(struct obmm_scc_event_v2) == 72);
static_assert(sizeof(struct obmm_scc_stats_v2) == 152);
static_assert(sizeof(struct obmm_scc_observability_v2) == 144);

struct linqu_obmm_scc_map {
	bool allocated;
	u64 generation;
};

struct linqu_scc_file {
	struct linqu_ub_drv *drv;
	struct mutex lock;
	pid_t owner_tgid;
	bool started;
	u64 owner_generation;
	u64 load_timeout_ns;
	u64 next_map_generation;
	u16 context_entries;
	u16 pending_load_entries;
	u16 event_queue_depth;
	struct linqu_obmm_scc_map maps[OBMM_SCC_MAX_PENDING_LOADS];
};

struct linqu_obmm_async_buffer {
	bool allocated;
	u32 generation;
	u64 length;
};

struct linqu_obmm_async_map {
	bool allocated;
	u64 generation;
	u64 length;
};

struct linqu_ub_file {
	struct linqu_ub_drv *drv;
	void *queue_cpu;
	dma_addr_t queue_dma;
	void *buffer_cpu;
	dma_addr_t buffer_dma;
	u32 queue_id;
	struct linqu_obmm_async_buffer buffers[OBMM_ASYNC_MAX_BUFFERS];
	struct linqu_obmm_async_map maps[OBMM_ASYNC_QUEUE_DEPTH];
};

struct linqu_ub_irq_snapshot {
	u64 irq_count;
	u64 irq_status;
};

struct linqu_ub_drv {
	struct device *dev;
	void __iomem *mmio;
	u8 __iomem *ep_mmio;
	u8 __iomem *obmm_async_mmio;
	u8 __iomem *obmm_scc_mmio;
	int irq;
	wait_queue_head_t waitq;
	/* Serializes the legacy endpoint file and interrupt state. */
	struct mutex lock;
	/* Serializes P2A queue and P2B scheduler-core ownership. */
	struct mutex queue_lock;
	struct cdev cdev;
	struct cdev scc_cdev;
	dev_t devt;
	struct class *class;
	bool event_pending;
	u64 irq_count;
	u64 last_irq_status;
	struct linqu_ub_file *active_file;
	struct linqu_scc_file *active_scc_file;
	u32 next_queue_id;
};

static ssize_t linqu_ub_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct linqu_ub_file *ctx = file->private_data;
	struct linqu_ub_drv *drv = ctx->drv;
	struct linqu_ub_irq_snapshot snapshot;
	int ret;

	if (count < sizeof(snapshot))
		return -EINVAL;

	ret = wait_event_interruptible(drv->waitq, READ_ONCE(drv->event_pending));
	if (ret)
		return ret;

	mutex_lock(&drv->lock);
	snapshot.irq_count = drv->irq_count;
	snapshot.irq_status = drv->last_irq_status;
	drv->event_pending = false;
	mutex_unlock(&drv->lock);

	if (copy_to_user(buf, &snapshot, sizeof(snapshot)))
		return -EFAULT;

	return sizeof(snapshot);
}

static __poll_t linqu_ub_poll(struct file *file, poll_table *wait)
{
	struct linqu_ub_file *ctx = file->private_data;
	struct linqu_ub_drv *drv = ctx->drv;
	__poll_t mask = 0;

	poll_wait(file, &drv->waitq, wait);
	if (READ_ONCE(drv->event_pending))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static int linqu_ub_open(struct inode *inode, struct file *file)
{
	struct linqu_ub_drv *drv = container_of(inode->i_cdev, struct linqu_ub_drv, cdev);
	struct linqu_ub_file *ctx;
	u64 device_status;
	u64 last_error;
	u64 version;
	int ret = -ENOMEM;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->drv = drv;

	mutex_lock(&drv->queue_lock);
	if (drv->active_file) {
		mutex_unlock(&drv->queue_lock);
		kfree(ctx);
		return -EBUSY;
	}
	version = readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_VERSION_CAPS);
	if ((u32)version != OBMM_ASYNC_ABI_VERSION ||
	    (u32)(version >> 32) != OBMM_ASYNC_QUEUE_DEPTH) {
		mutex_unlock(&drv->queue_lock);
		kfree(ctx);
		return -ENODEV;
	}
	ctx->queue_cpu = dma_alloc_coherent(drv->dev, OBMM_ASYNC_QUEUE_BYTES,
					    &ctx->queue_dma, GFP_KERNEL);
	if (!ctx->queue_cpu) {
		dev_err(drv->dev, "failed to allocate async queue bytes=%u\n",
			OBMM_ASYNC_QUEUE_BYTES);
		goto err_unlock;
	}
	ctx->buffer_cpu = dma_alloc_coherent(drv->dev,
					     OBMM_ASYNC_BUFFER_ARENA_BYTES,
					     &ctx->buffer_dma, GFP_KERNEL);
	if (!ctx->buffer_cpu) {
		dev_err(drv->dev, "failed to allocate async buffer arena bytes=%u\n",
			OBMM_ASYNC_BUFFER_ARENA_BYTES);
		goto err_queue;
	}
	drv->next_queue_id++;
	if (!drv->next_queue_id || drv->next_queue_id > U16_MAX)
		drv->next_queue_id = 1;
	ctx->queue_id = drv->next_queue_id;
	memset(ctx->queue_cpu, 0, OBMM_ASYNC_QUEUE_BYTES);
	memset(ctx->buffer_cpu, 0, OBMM_ASYNC_BUFFER_ARENA_BYTES);

	writeq(ctx->queue_dma,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_SQ_BASE);
	writeq(OBMM_ASYNC_QUEUE_DEPTH,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_SQ_SIZE);
	writeq(ctx->queue_dma + OBMM_ASYNC_QUEUE_DEPTH * OBMM_ASYNC_SLOT_BYTES,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_CQ_BASE);
	writeq(OBMM_ASYNC_QUEUE_DEPTH,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_CQ_SIZE);
	writeq(ctx->queue_id,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_QUEUE_ID);
	writeq(1, drv->obmm_async_mmio + OBMM_ASYNC_REG_STATUS);
	last_error = readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_LAST_ERROR);
	device_status = readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_STATUS);
	if (last_error || !(device_status & 1)) {
		dev_err(drv->dev,
			"async queue enable failed status=%#llx error=%#llx "
			"queue_dma=%pad queue_id=%u\n",
			device_status, last_error, &ctx->queue_dma,
			ctx->queue_id);
		ret = -EIO;
		goto err_buffer;
	}
	drv->active_file = ctx;
	mutex_unlock(&drv->queue_lock);

	file->private_data = ctx;
	return 0;

err_buffer:
	writeq(0, drv->obmm_async_mmio + OBMM_ASYNC_REG_STATUS);
	dma_free_coherent(drv->dev, OBMM_ASYNC_BUFFER_ARENA_BYTES,
			  ctx->buffer_cpu, ctx->buffer_dma);
err_queue:
	dma_free_coherent(drv->dev, OBMM_ASYNC_QUEUE_BYTES,
			  ctx->queue_cpu, ctx->queue_dma);
err_unlock:
	mutex_unlock(&drv->queue_lock);
	kfree(ctx);
	return ret;
}

static void linqu_obmm_async_unregister_all(struct linqu_ub_file *ctx)
{
	struct linqu_ub_drv *drv = ctx->drv;
	u32 index;

	for (index = 0; index < OBMM_ASYNC_QUEUE_DEPTH; index++) {
		if (!ctx->maps[index].allocated)
			continue;
		writeq(index + 1,
		       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_ID);
		writeq(ctx->maps[index].generation,
		       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_GENERATION);
		writeq(2, drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_CMD);
	}
	for (index = 0; index < OBMM_ASYNC_MAX_BUFFERS; index++) {
		if (!ctx->buffers[index].allocated)
			continue;
		writeq(index + 1,
		       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_ID);
		writeq(ctx->buffers[index].generation,
		       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_GENERATION);
		writeq(2, drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_CMD);
	}
}

static int linqu_ub_release(struct inode *inode, struct file *file)
{
	struct linqu_ub_file *ctx = file->private_data;
	struct linqu_ub_drv *drv = ctx->drv;

	mutex_lock(&drv->queue_lock);
	writeq(0, drv->obmm_async_mmio + OBMM_ASYNC_REG_STATUS);
	linqu_obmm_async_unregister_all(ctx);
	if (drv->active_file == ctx)
		drv->active_file = NULL;
	mutex_unlock(&drv->queue_lock);

	dma_free_coherent(drv->dev, OBMM_ASYNC_BUFFER_ARENA_BYTES,
			  ctx->buffer_cpu, ctx->buffer_dma);
	dma_free_coherent(drv->dev, OBMM_ASYNC_QUEUE_BYTES,
			  ctx->queue_cpu, ctx->queue_dma);
	kfree(ctx);
	return 0;
}

static int linqu_ub_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct linqu_ub_file *ctx = file->private_data;
	struct linqu_ub_drv *drv = ctx->drv;
	unsigned long selector = vma->vm_pgoff;
	unsigned long length = vma->vm_end - vma->vm_start;
	int ret;

	vma->vm_pgoff = 0;
	if (selector == 0 && length == OBMM_ASYNC_QUEUE_BYTES) {
		ret = dma_mmap_coherent(drv->dev, vma, ctx->queue_cpu,
					ctx->queue_dma, OBMM_ASYNC_QUEUE_BYTES);
	} else if (selector == 1 &&
		   length == OBMM_ASYNC_BUFFER_ARENA_BYTES) {
		ret = dma_mmap_coherent(drv->dev, vma, ctx->buffer_cpu,
					ctx->buffer_dma,
					OBMM_ASYNC_BUFFER_ARENA_BYTES);
	} else {
		ret = -EINVAL;
	}
	return ret;
}

static long linqu_obmm_async_get_info(struct linqu_ub_file *ctx,
				      unsigned long arg)
{
	struct obmm_async_info_v1 info = {
		.abi_version = OBMM_ASYNC_ABI_VERSION,
		.queue_id = ctx->queue_id,
		.queue_depth = OBMM_ASYNC_QUEUE_DEPTH,
		.slot_bytes = OBMM_ASYNC_SLOT_BYTES,
		.queue_mmap_offset = 0,
		.queue_mmap_bytes = OBMM_ASYNC_QUEUE_BYTES,
		.buffer_mmap_offset = PAGE_SIZE,
		.buffer_mmap_bytes = OBMM_ASYNC_BUFFER_ARENA_BYTES,
	};

	return copy_to_user((void __user *)arg, &info, sizeof(info)) ?
		-EFAULT : 0;
}

static long linqu_obmm_async_buffer_alloc(struct linqu_ub_file *ctx,
					  unsigned long arg)
{
	struct obmm_async_buffer_alloc_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;
	u32 index;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.length || request.length > OBMM_ASYNC_MAX_BUFFER_BYTES)
		return -EINVAL;
	for (index = 0; index < OBMM_ASYNC_MAX_BUFFERS; index++) {
		if (!ctx->buffers[index].allocated)
			break;
	}
	if (index == OBMM_ASYNC_MAX_BUFFERS)
		return -ENOSPC;
	ctx->buffers[index].generation++;
	if (!ctx->buffers[index].generation)
		ctx->buffers[index].generation++;
	ctx->buffers[index].allocated = true;
	ctx->buffers[index].length = request.length;
	request.buffer_id = index + 1;
	request.generation = ctx->buffers[index].generation;
	request.arena_offset = index * OBMM_ASYNC_MAX_BUFFER_BYTES;

	writeq(ctx->buffer_dma + request.arena_offset,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_BASE);
	writeq(request.length,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_LENGTH);
	writeq(request.buffer_id,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_ID);
	writeq(request.generation,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_GENERATION);
	/* Publish all buffer descriptor fields before the registration command. */
	wmb();
	writeq(1, drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_CMD);
	if (readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_LAST_ERROR)) {
		ctx->buffers[index].allocated = false;
		return -EIO;
	}
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		writeq(request.buffer_id,
		       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_ID);
		writeq(request.generation,
		       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_GENERATION);
		writeq(2, drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_CMD);
		ctx->buffers[index].allocated = false;
		return -EFAULT;
	}
	return 0;
}

static long linqu_obmm_async_buffer_free(struct linqu_ub_file *ctx,
					 unsigned long arg)
{
	struct obmm_async_buffer_free_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;
	u32 index;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.buffer_id || request.buffer_id > OBMM_ASYNC_MAX_BUFFERS)
		return -EINVAL;
	index = request.buffer_id - 1;
	if (!ctx->buffers[index].allocated ||
	    ctx->buffers[index].generation != request.generation)
		return -ESTALE;
	writeq(request.buffer_id,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_ID);
	writeq(request.generation,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_GENERATION);
	writeq(2, drv->obmm_async_mmio + OBMM_ASYNC_REG_BUFFER_CMD);
	if (readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_LAST_ERROR))
		return -EBUSY;
	ctx->buffers[index].allocated = false;
	ctx->buffers[index].generation++;
	if (!ctx->buffers[index].generation)
		ctx->buffers[index].generation++;
	return 0;
}

static long linqu_obmm_async_map_register(struct linqu_ub_file *ctx,
					  unsigned long arg)
{
	struct obmm_async_map_register_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;
	struct vm_area_struct *vma;
	unsigned long pfn;
	u64 local_pa;
	u32 index;
	int ret;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.mem_id || !request.length ||
	    request.mapped_addr > U64_MAX - request.length ||
	    request.flags || request.reserved)
		return -EINVAL;
	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, request.mapped_addr);
	if (!vma || request.mapped_addr < vma->vm_start ||
	    request.mapped_addr + request.length > vma->vm_end) {
		ret = -EFAULT;
		goto out_unlock;
	}
	ret = follow_pfn(vma, request.mapped_addr, &pfn);
	if (ret)
		goto out_unlock;
	local_pa = PFN_PHYS(pfn) + offset_in_page(request.mapped_addr);
out_unlock:
	mmap_read_unlock(current->mm);
	if (ret)
		return ret;

	for (index = 0; index < OBMM_ASYNC_QUEUE_DEPTH; index++) {
		if (!ctx->maps[index].allocated)
			break;
	}
	if (index == OBMM_ASYNC_QUEUE_DEPTH)
		return -ENOSPC;
	ctx->maps[index].generation++;
	if (!ctx->maps[index].generation)
		ctx->maps[index].generation++;
	ctx->maps[index].allocated = true;
	ctx->maps[index].length = request.length;
	request.map_id = index + 1;
	request.map_generation = ctx->maps[index].generation;

	writeq(local_pa, drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_LOCAL_PA);
	writeq(request.length,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_LENGTH);
	writeq(request.map_id,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_ID);
	writeq(request.map_generation,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_GENERATION);
	/* Publish all map descriptor fields before the registration command. */
	wmb();
	writeq(1, drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_CMD);
	if (readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_LAST_ERROR)) {
		ctx->maps[index].allocated = false;
		return -EIO;
	}
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		writeq(request.map_id,
		       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_ID);
		writeq(request.map_generation,
		       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_GENERATION);
		writeq(2, drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_CMD);
		ctx->maps[index].allocated = false;
		return -EFAULT;
	}
	return 0;
}

static long linqu_obmm_async_map_unregister(struct linqu_ub_file *ctx,
					    unsigned long arg)
{
	struct obmm_async_map_unregister_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;
	u32 index;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.map_id || request.map_id > OBMM_ASYNC_QUEUE_DEPTH)
		return -EINVAL;
	index = request.map_id - 1;
	if (!ctx->maps[index].allocated ||
	    ctx->maps[index].generation != request.map_generation)
		return -ESTALE;
	writeq(request.map_id,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_ID);
	writeq(request.map_generation,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_GENERATION);
	writeq(2, drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_CMD);
	if (readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_LAST_ERROR))
		return -EBUSY;
	ctx->maps[index].allocated = false;
	ctx->maps[index].generation++;
	if (!ctx->maps[index].generation)
		ctx->maps[index].generation++;
	return 0;
}

static long linqu_obmm_async_kick(struct linqu_ub_file *ctx,
				  unsigned long arg)
{
	struct obmm_async_kick_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.sq_tail - readq(drv->obmm_async_mmio +
				      OBMM_ASYNC_REG_SQ_HEAD) >
	    OBMM_ASYNC_QUEUE_DEPTH)
		return -EINVAL;
	writeq(request.cq_head,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_CQ_HEAD);
	request.guest_monotonic_ns = ktime_get_ns();
	writeq(request.guest_monotonic_ns,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_GUEST_MONOTONIC_NS);
	/* Publish SQ slots and CQ ownership before ringing the device doorbell. */
	wmb();
	writeq(request.sq_tail,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_SQ_TAIL);
	writeq(1, drv->obmm_async_mmio + OBMM_ASYNC_REG_DOORBELL);
	request.cq_tail = readq(drv->obmm_async_mmio +
				OBMM_ASYNC_REG_CQ_TAIL);
	request.last_error = readq(drv->obmm_async_mmio +
				  OBMM_ASYNC_REG_LAST_ERROR);
	request.sq_head = readq(drv->obmm_async_mmio +
			       OBMM_ASYNC_REG_SQ_HEAD);
	return copy_to_user((void __user *)arg, &request, sizeof(request)) ?
		-EFAULT : 0;
}

static long linqu_obmm_async_cancel(struct linqu_ub_file *ctx,
				    unsigned long arg)
{
	struct obmm_async_cancel_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	writeq(request.token,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_CANCEL_TOKEN);
	writeq(1, drv->obmm_async_mmio + OBMM_ASYNC_REG_CANCEL_CMD);
	return readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_LAST_ERROR) ?
		-ESTALE : 0;
}

static long linqu_obmm_async_get_observability(struct linqu_ub_file *ctx,
					       unsigned long arg)
{
	struct obmm_async_observability_v1 observability = { 0 };
	u64 *values = (u64 *)&observability;
	u32 index;

	for (index = 0; index < OBMM_ASYNC_OBSERVABILITY_VALUES; index++)
		values[index] = readq(ctx->drv->obmm_async_mmio +
				      OBMM_ASYNC_REG_OBSERVABILITY_BASE +
				      index * sizeof(*values));
	return copy_to_user((void __user *)arg, &observability,
			    sizeof(observability)) ? -EFAULT : 0;
}

static long linqu_obmm_async_reset_observability(struct linqu_ub_file *ctx)
{
	writeq(1, ctx->drv->obmm_async_mmio +
	       OBMM_ASYNC_REG_OBSERVABILITY_RESET);
	return readq(ctx->drv->obmm_async_mmio + OBMM_ASYNC_REG_LAST_ERROR) ?
		-EBUSY : 0;
}

static long linqu_ub_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct linqu_ub_file *ctx = file->private_data;

	switch (cmd) {
	case OBMM_ASYNC_IOCTL_GET_INFO:
		return linqu_obmm_async_get_info(ctx, arg);
	case OBMM_ASYNC_IOCTL_BUFFER_ALLOC:
		return linqu_obmm_async_buffer_alloc(ctx, arg);
	case OBMM_ASYNC_IOCTL_BUFFER_FREE:
		return linqu_obmm_async_buffer_free(ctx, arg);
	case OBMM_ASYNC_IOCTL_MAP_REGISTER:
		return linqu_obmm_async_map_register(ctx, arg);
	case OBMM_ASYNC_IOCTL_MAP_UNREGISTER:
		return linqu_obmm_async_map_unregister(ctx, arg);
	case OBMM_ASYNC_IOCTL_KICK:
		return linqu_obmm_async_kick(ctx, arg);
	case OBMM_ASYNC_IOCTL_CANCEL:
		return linqu_obmm_async_cancel(ctx, arg);
	case OBMM_ASYNC_IOCTL_GET_OBSERVABILITY:
		return linqu_obmm_async_get_observability(ctx, arg);
	case OBMM_ASYNC_IOCTL_RESET_OBSERVABILITY:
		return linqu_obmm_async_reset_observability(ctx);
	default:
		return -ENOTTY;
	}
}

static int linqu_scc_open(struct inode *inode, struct file *file)
{
	struct linqu_ub_drv *drv = container_of(inode->i_cdev,
						struct linqu_ub_drv, scc_cdev);
	struct linqu_scc_file *ctx;
	u64 version;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->drv = drv;
	mutex_init(&ctx->lock);
	ctx->owner_tgid = task_tgid_nr(current);
	ctx->next_map_generation = 1;

	mutex_lock(&drv->queue_lock);
	if (drv->active_scc_file) {
		mutex_unlock(&drv->queue_lock);
		kfree(ctx);
		return -EBUSY;
	}
	version = readq(drv->obmm_scc_mmio + OBMM_SCC_REG_VERSION_CAPS);
	ctx->context_entries = version >> 16;
	ctx->pending_load_entries = version >> 32;
	ctx->event_queue_depth = version >> 48;
	if ((u16)version != OBMM_SCC_ABI_VERSION ||
	    !ctx->context_entries ||
	    ctx->context_entries > OBMM_SCC_MAX_CONTEXTS ||
	    !ctx->pending_load_entries ||
	    ctx->pending_load_entries > OBMM_SCC_MAX_PENDING_LOADS ||
	    !ctx->event_queue_depth ||
	    ctx->event_queue_depth > OBMM_SCC_MAX_EVENTS) {
		mutex_unlock(&drv->queue_lock);
		kfree(ctx);
		return -ENODEV;
	}
	ctx->owner_generation = readq(drv->obmm_scc_mmio +
				      OBMM_SCC_REG_OWNER_GENERATION);
	drv->active_scc_file = ctx;
	mutex_unlock(&drv->queue_lock);
	file->private_data = ctx;
	return 0;
}

static bool linqu_scc_owner(const struct linqu_scc_file *ctx)
{
	return task_tgid_nr(current) == ctx->owner_tgid;
}

static void linqu_scc_unregister_all(struct linqu_scc_file *ctx)
{
	struct linqu_ub_drv *drv = ctx->drv;
	u32 index;

	for (index = 0; index < ctx->pending_load_entries; index++) {
		if (!ctx->maps[index].allocated)
			continue;
		writeq(index + 1,
		       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_ID);
		writeq(ctx->maps[index].generation,
		       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_GENERATION);
		writeq(2, drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_COMMAND);
		ctx->maps[index].allocated = false;
	}
}

static int linqu_scc_release(struct inode *inode, struct file *file)
{
	struct linqu_scc_file *ctx = file->private_data;
	struct linqu_ub_drv *drv = ctx->drv;

	mutex_lock(&drv->queue_lock);
	if (ctx->started)
		writeq(2, drv->obmm_scc_mmio + OBMM_SCC_REG_SESSION_COMMAND);
	linqu_scc_unregister_all(ctx);
	if (drv->active_scc_file == ctx)
		drv->active_scc_file = NULL;
	mutex_unlock(&drv->queue_lock);
	kfree(ctx);
	return 0;
}

static long linqu_scc_query_caps(struct linqu_scc_file *ctx,
				 unsigned long arg)
{
	u64 version = readq(ctx->drv->obmm_scc_mmio +
			    OBMM_SCC_REG_VERSION_CAPS);
	struct obmm_scc_caps_v2 caps = {
		.abi_version = version & 0xffff,
		.context_entries = (version >> 16) & 0xffff,
		.pending_load_entries = (version >> 32) & 0xffff,
		.event_queue_depth = (version >> 48) & 0xffff,
		.context_state_bytes = OBMM_SCC_CONTEXT_STATE_BYTES,
		.capabilities = OBMM_SCC_CAP_SCALAR_1 |
			OBMM_SCC_CAP_SCALAR_2 | OBMM_SCC_CAP_SCALAR_4 |
			OBMM_SCC_CAP_SCALAR_8 | OBMM_SCC_CAP_XZR |
			OBMM_SCC_CAP_DIRECT_EL0_UPCALL |
			OBMM_SCC_CAP_EL0_RESUME |
			OBMM_SCC_CAP_FULL_CONTEXT,
		.owner_generation = ctx->owner_generation,
		.clock_mhz = readq(ctx->drv->obmm_scc_mmio +
				  OBMM_SCC_REG_CLOCK_MHZ),
		.resume_hlt_imm = OBMM_SCC_RESUME_HLT_IMM,
	};

	return copy_to_user((void __user *)arg, &caps, sizeof(caps)) ?
		-EFAULT : 0;
}

static long linqu_scc_register_map(struct linqu_scc_file *ctx,
				   unsigned long arg)
{
	struct obmm_scc_map_register_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;
	struct vm_area_struct *vma;
	struct fd obmm_file;
	unsigned long page_addr;
	unsigned long next_pfn;
	unsigned long pfn;
	u64 local_pa;
	u32 index;
	int ret;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!linqu_scc_owner(ctx))
		return -EPERM;
	if (!request.mem_id || !request.gsva_base ||
	    request.mapped_addr != request.gsva_base || !request.length ||
	    request.gsva_base > U64_MAX - request.length ||
	    request.flags & ~OBMM_SCC_MAP_LOGICAL_MIXED ||
	    !IS_ALIGNED(request.gsva_base, PAGE_SIZE) ||
	    !IS_ALIGNED(request.length, PAGE_SIZE) ||
	    request.mapping_fd < 0 || request.policy_id ||
	    request.map_generation ||
	    (request.model_phase_generation != 1 &&
	     request.model_phase_generation != 2))
		return -EINVAL;
	obmm_file = fdget(request.mapping_fd);
	if (!obmm_file.file)
		return -EBADF;

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, request.mapped_addr);
	if (!vma || request.mapped_addr < vma->vm_start ||
	    request.mapped_addr + request.length > vma->vm_end ||
	    vma->vm_file != obmm_file.file ||
	    !obmm_file_matches_region(obmm_file.file, request.mem_id)) {
		ret = -EFAULT;
		goto out_unlock;
	}
	ret = follow_pfn(vma, request.mapped_addr, &pfn);
	if (ret)
		goto out_unlock;
	local_pa = PFN_PHYS(pfn) + offset_in_page(request.mapped_addr);
	next_pfn = pfn + 1;
	for (page_addr = request.mapped_addr + PAGE_SIZE;
	     page_addr < request.mapped_addr + request.length;
	     page_addr += PAGE_SIZE, next_pfn++) {
		ret = follow_pfn(vma, page_addr, &pfn);
		if (ret || pfn != next_pfn) {
			ret = ret ? ret : -EINVAL;
			goto out_unlock;
		}
	}
out_unlock:
	mmap_read_unlock(current->mm);
	fdput(obmm_file);
	if (ret)
		return ret;

	for (index = 0; index < ctx->pending_load_entries; index++) {
		if (!ctx->maps[index].allocated)
			break;
	}
	if (index == ctx->pending_load_entries)
		return -ENOSPC;
	ctx->next_map_generation++;
	if (!ctx->next_map_generation)
		ctx->next_map_generation++;
	request.policy_id = index + 1;
	request.map_generation = ctx->next_map_generation;

	writeq(request.gsva_base,
	       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_GSVA_BASE);
	writeq(local_pa, drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_LOCAL_PA);
	writeq(request.length, drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_LENGTH);
	writeq(request.policy_id, drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_ID);
	writeq(request.map_generation,
	       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_GENERATION);
	writeq(request.model_phase_generation,
	       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_MODEL_GENERATION);
	writeq(request.flags,
	       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_FLAGS);
	/* Publish the complete map descriptor before the command doorbell. */
	wmb();
	writeq(1, drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_COMMAND);
	if (readq(drv->obmm_scc_mmio + OBMM_SCC_REG_LAST_ERROR))
		return -EIO;
	ctx->maps[index].allocated = true;
	ctx->maps[index].generation = request.map_generation;
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		writeq(request.policy_id,
		       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_ID);
		writeq(request.map_generation,
		       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_GENERATION);
		writeq(2, drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_COMMAND);
		ctx->maps[index].allocated = false;
		return -EFAULT;
	}
	return 0;
}

static long linqu_scc_unregister_map(struct linqu_scc_file *ctx,
				     unsigned long arg)
{
	struct obmm_scc_map_unregister_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;
	u32 index;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!linqu_scc_owner(ctx))
		return -EPERM;
	if (!request.policy_id ||
	    request.policy_id > ctx->pending_load_entries)
		return -EINVAL;
	index = request.policy_id - 1;
	if (!ctx->maps[index].allocated ||
	    ctx->maps[index].generation != request.map_generation)
		return -ESTALE;
	writeq(request.policy_id, drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_ID);
	writeq(request.map_generation,
	       drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_GENERATION);
	writeq(2, drv->obmm_scc_mmio + OBMM_SCC_REG_MAP_COMMAND);
	if (readq(drv->obmm_scc_mmio + OBMM_SCC_REG_LAST_ERROR))
		return -EBUSY;
	ctx->maps[index].allocated = false;
	return 0;
}

static long linqu_scc_start(struct linqu_scc_file *ctx,
			    unsigned long arg)
{
	struct obmm_scc_start_v2 request;
	struct linqu_ub_drv *drv = ctx->drv;
	u64 status;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!linqu_scc_owner(ctx))
		return -EPERM;
	if (ctx->started || request.flags || request.owner_generation ||
	    request.reserved0 || !request.upcall_entry ||
	    !IS_ALIGNED(request.upcall_entry, 4) ||
	    !request.logical_contexts ||
	    request.logical_contexts > ctx->context_entries ||
	    request.load_timeout_ns > OBMM_SCC_MAX_LOAD_TIMEOUT_NS ||
	    current->nr_cpus_allowed != 1 ||
	    request.home_cpu != task_cpu(current) ||
	    !access_ok((void __user *)(uintptr_t)request.upcall_entry, 4))
		return -EINVAL;
	writeq(phys_to_ttbr(virt_to_phys(current->mm->pgd)) |
	       (system_supports_cnp() && ASID(current->mm) ? TTBR_CNP_BIT : 0) |
	       (IS_ENABLED(CONFIG_ARM64_SW_TTBR0_PAN) ?
		FIELD_PREP(TTBR_ASID_MASK, ASID(current->mm)) : 0),
	       drv->obmm_scc_mmio + OBMM_SCC_REG_OWNER_TTBR0);
	writeq(request.load_timeout_ns,
	       drv->obmm_scc_mmio + OBMM_SCC_REG_LOAD_TIMEOUT_NS);
	writeq(request.upcall_entry,
	       drv->obmm_scc_mmio + OBMM_SCC_REG_UPCALL_ENTRY);
	writeq(request.logical_contexts,
	       drv->obmm_scc_mmio + OBMM_SCC_REG_LOGICAL_CONTEXTS);
	/* Publish owner identity and deadline before enabling interception. */
	wmb();
	writeq(1, drv->obmm_scc_mmio + OBMM_SCC_REG_SESSION_COMMAND);
	status = readq(drv->obmm_scc_mmio + OBMM_SCC_REG_STATUS);
	if (readq(drv->obmm_scc_mmio + OBMM_SCC_REG_LAST_ERROR) ||
	    !(status & OBMM_SCC_STATUS_ACTIVE))
		return -EIO;
	ctx->started = true;
	ctx->load_timeout_ns = request.load_timeout_ns;
	request.owner_generation = ctx->owner_generation;
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		writeq(2, drv->obmm_scc_mmio + OBMM_SCC_REG_SESSION_COMMAND);
		ctx->started = false;
		return -EFAULT;
	}
	return 0;
}

static long linqu_scc_stop(struct linqu_scc_file *ctx)
{
	if (!linqu_scc_owner(ctx))
		return -EPERM;
	if (!ctx->started)
		return 0;
	writeq(2, ctx->drv->obmm_scc_mmio + OBMM_SCC_REG_SESSION_COMMAND);
	ctx->started = false;
	return 0;
}

static long linqu_scc_get_stats(struct linqu_scc_file *ctx,
				unsigned long arg)
{
	struct obmm_scc_stats_v2 stats = { 0 };
	struct linqu_ub_drv *drv = ctx->drv;
	u64 *values = (u64 *)&stats;
	u32 index;

	if (!linqu_scc_owner(ctx))
		return -EPERM;
	for (index = 0; index < 17; index++)
		values[index] = readq(drv->obmm_scc_mmio +
				      OBMM_SCC_REG_STATS_BASE + index * 8);
	stats.direct_upcalls = readq(drv->obmm_scc_mmio +
		OBMM_SCC_REG_OBSERVABILITY_BASE + 16 * 8);
	stats.fail_stop = !!(readq(drv->obmm_scc_mmio + OBMM_SCC_REG_STATUS) &
			     OBMM_SCC_STATUS_FAIL_STOP);
	return copy_to_user((void __user *)arg, &stats, sizeof(stats)) ?
		-EFAULT : 0;
}

static long linqu_scc_get_observability(struct linqu_scc_file *ctx,
					unsigned long arg)
{
	struct obmm_scc_observability_v2 observability = {
		.abi_version = OBMM_SCC_ABI_VERSION,
	};
	struct linqu_ub_drv *drv = ctx->drv;
	u64 *values = &observability.scc_pending_current;
	u32 index;

	if (!linqu_scc_owner(ctx))
		return -EPERM;
	for (index = 0; index < OBMM_SCC_OBSERVABILITY_VALUES; index++)
		values[index] = readq(drv->obmm_scc_mmio +
				      OBMM_SCC_REG_OBSERVABILITY_BASE +
				      index * 8);
	return copy_to_user((void __user *)arg, &observability,
			    sizeof(observability)) ? -EFAULT : 0;
}

static long linqu_scc_get_event(struct linqu_scc_file *ctx,
				unsigned long arg)
{
	struct linqu_ub_drv *drv = ctx->drv;
	struct obmm_scc_event_v2 event;
	u64 timeout_ns;
	u64 started_ns;
	u64 kind_status;
	u64 event_meta;
	u64 status;

	if (copy_from_user(&event, (void __user *)arg, sizeof(event)))
		return -EFAULT;
	if (!linqu_scc_owner(ctx) || !ctx->started)
		return -EPERM;
	if (event.flags & ~OBMM_SCC_EVENT_GET_WAIT)
		return -EINVAL;
	status = readq(drv->obmm_scc_mmio + OBMM_SCC_REG_STATUS);
	/*
	 * A non-waiting read consumes the event attached to the current EL0
	 * upcall.  A waiting read is issued by the EL0 scheduler when every
	 * logical context is blocked, so it is valid without an active upcall.
	 */
	if (!(status & OBMM_SCC_STATUS_UPCALL_ACTIVE) &&
	    !(event.flags & OBMM_SCC_EVENT_GET_WAIT))
		return -EPERM;
	if (!(status & OBMM_SCC_STATUS_EVENT_DELIVERED)) {
		if (!(event.flags & OBMM_SCC_EVENT_GET_WAIT))
			return -EAGAIN;
		/*
		 * The SCC device owns the per-load deadline and publishes a
		 * COMPLETE or FAULT event.  This loop is only a host-side bound for
		 * waiting on that event.  Reusing the per-load deadline here races
		 * QEMU virtual time against host scheduling and can abandon a valid
		 * completion at the deadline boundary.
		 */
		timeout_ns = OBMM_SCC_MAX_LOAD_TIMEOUT_NS;
		started_ns = ktime_get_ns();
		for (;;) {
			status = readq(drv->obmm_scc_mmio +
				       OBMM_SCC_REG_STATUS);
			if (status & (OBMM_SCC_STATUS_EVENT_PENDING |
				      OBMM_SCC_STATUS_EVENT_DELIVERED))
				break;
			if (!(status & OBMM_SCC_STATUS_ACTIVE) ||
			    status & OBMM_SCC_STATUS_FAIL_STOP)
				return -EIO;
			if (ktime_get_ns() - started_ns >= timeout_ns) {
				pr_err("linqu_scc: GET_EVENT timeout status=0x%llx "
				       "loads_pending=%llu loads_completed=%llu "
				       "events_completed=%llu loads_faulted=%llu "
				       "scc_pending=%llu backend_pending=%llu\n",
				       status,
				       readq(drv->obmm_scc_mmio +
					     OBMM_SCC_REG_STATS_BASE),
				       readq(drv->obmm_scc_mmio +
					     OBMM_SCC_REG_STATS_BASE + 8),
				       readq(drv->obmm_scc_mmio +
					     OBMM_SCC_REG_STATS_BASE + 16),
				       readq(drv->obmm_scc_mmio +
					     OBMM_SCC_REG_STATS_BASE + 24),
				       readq(drv->obmm_scc_mmio +
					     OBMM_SCC_REG_OBSERVABILITY_BASE),
				       readq(drv->obmm_scc_mmio +
					     OBMM_SCC_REG_OBSERVABILITY_BASE + 8));
				return -ETIMEDOUT;
			}
			if (signal_pending(current))
				return -ERESTARTSYS;
			usleep_range(10, 50);
		}
		if (!(status & OBMM_SCC_STATUS_EVENT_DELIVERED)) {
			writeq(2, drv->obmm_scc_mmio +
			       OBMM_SCC_REG_EVENT_COMMAND);
			if (readq(drv->obmm_scc_mmio +
				  OBMM_SCC_REG_LAST_ERROR))
				return -EIO;
		}
	}
	memset(&event, 0, sizeof(event));
	event.sequence = readq(drv->obmm_scc_mmio +
			       OBMM_SCC_REG_EVENT_SEQUENCE);
	event.context_id = readq(drv->obmm_scc_mmio +
				 OBMM_SCC_REG_EVENT_CONTEXT_ID);
	event.plt_token = readq(drv->obmm_scc_mmio +
				OBMM_SCC_REG_EVENT_PLT_TOKEN);
	event.interrupted_pc = readq(drv->obmm_scc_mmio +
				     OBMM_SCC_REG_EVENT_INTERRUPTED_PC);
	event.fault_pc = readq(drv->obmm_scc_mmio +
			       OBMM_SCC_REG_EVENT_FAULT_PC);
	event.effective_va = readq(drv->obmm_scc_mmio +
				   OBMM_SCC_REG_EVENT_VA);
	event.value = readq(drv->obmm_scc_mmio + OBMM_SCC_REG_EVENT_VALUE);
	kind_status = readq(drv->obmm_scc_mmio +
			    OBMM_SCC_REG_EVENT_KIND_STATUS);
	event.kind = kind_status;
	event.status = kind_status >> 32;
	event_meta = readq(drv->obmm_scc_mmio + OBMM_SCC_REG_EVENT_META);
	event.rt = event_meta;
	event.access_bytes = event_meta >> 16;
	event.flags = event_meta >> 32;
	writeq(1, drv->obmm_scc_mmio + OBMM_SCC_REG_EVENT_COMMAND);
	if (readq(drv->obmm_scc_mmio + OBMM_SCC_REG_LAST_ERROR))
		return -EIO;
	return copy_to_user((void __user *)arg, &event, sizeof(event)) ?
		-EFAULT : 0;
}

static long linqu_scc_scheduler_enter(struct linqu_scc_file *ctx)
{
	struct linqu_ub_drv *drv = ctx->drv;

	if (!linqu_scc_owner(ctx) || !ctx->started)
		return -EPERM;
	writeq(1, drv->obmm_scc_mmio + OBMM_SCC_REG_SCHEDULER_COMMAND);
	return readq(drv->obmm_scc_mmio + OBMM_SCC_REG_LAST_ERROR) ?
		-EIO : 0;
}

static long linqu_scc_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	struct linqu_scc_file *ctx = file->private_data;
	long ret;

	mutex_lock(&ctx->lock);
	switch (cmd) {
	case OBMM_SCC_IOCTL_QUERY_CAPS:
		ret = linqu_scc_query_caps(ctx, arg);
		break;
	case OBMM_SCC_IOCTL_REGISTER_MAP:
		ret = linqu_scc_register_map(ctx, arg);
		break;
	case OBMM_SCC_IOCTL_UNREGISTER_MAP:
		ret = linqu_scc_unregister_map(ctx, arg);
		break;
	case OBMM_SCC_IOCTL_START:
		ret = linqu_scc_start(ctx, arg);
		break;
	case OBMM_SCC_IOCTL_STOP:
		ret = linqu_scc_stop(ctx);
		break;
	case OBMM_SCC_IOCTL_GET_STATS:
		ret = linqu_scc_get_stats(ctx, arg);
		break;
	case OBMM_SCC_IOCTL_GET_OBSERVABILITY:
		ret = linqu_scc_get_observability(ctx, arg);
		break;
	case OBMM_SCC_IOCTL_GET_EVENT:
		ret = linqu_scc_get_event(ctx, arg);
		break;
	case OBMM_SCC_IOCTL_SCHEDULER_ENTER:
		ret = linqu_scc_scheduler_enter(ctx);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

static const struct file_operations linqu_ub_fops = {
	.owner = THIS_MODULE,
	.open = linqu_ub_open,
	.release = linqu_ub_release,
	.read = linqu_ub_read,
	.poll = linqu_ub_poll,
	.mmap = linqu_ub_mmap,
	.unlocked_ioctl = linqu_ub_ioctl,
	.llseek = no_llseek,
};

static const struct file_operations linqu_scc_fops = {
	.owner = THIS_MODULE,
	.open = linqu_scc_open,
	.release = linqu_scc_release,
	.unlocked_ioctl = linqu_scc_ioctl,
	.llseek = no_llseek,
};

static irqreturn_t linqu_ub_irq_thread(int irq, void *data)
{
	struct linqu_ub_drv *drv = data;
	u64 irq_status;
	u64 async_irq_status;

	irq_status = readq(drv->ep_mmio + LINQU_UB_REG_IRQ_STATUS);
	async_irq_status = readq(drv->obmm_async_mmio +
				 OBMM_ASYNC_REG_IRQ_STATUS);
	if (!irq_status && !async_irq_status)
		return IRQ_HANDLED;

	if (irq_status)
		writeq(irq_status, drv->ep_mmio + LINQU_UB_REG_IRQ_ACK);
	if (async_irq_status)
		writeq(async_irq_status, drv->obmm_async_mmio +
		       OBMM_ASYNC_REG_IRQ_ACK);

	mutex_lock(&drv->lock);
	drv->irq_count += 1;
	drv->last_irq_status = irq_status | (async_irq_status << 32);
	drv->event_pending = true;
	mutex_unlock(&drv->lock);
	dev_info(drv->dev,
		 "irq handled status=0x%llx async=0x%llx count=%llu\n",
		 irq_status, async_irq_status, drv->irq_count);

	wake_up_interruptible(&drv->waitq);
	return IRQ_HANDLED;
}

static irqreturn_t linqu_ub_irq_primary(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

static int linqu_ub_chrdev_init(struct linqu_ub_drv *drv)
{
	int ret;

	ret = alloc_chrdev_region(&drv->devt, 0, 2, "linqu-ub");
	if (ret)
		return ret;

	cdev_init(&drv->cdev, &linqu_ub_fops);
	ret = cdev_add(&drv->cdev, drv->devt, 1);
	if (ret)
		goto err_chrdev;

	cdev_init(&drv->scc_cdev, &linqu_scc_fops);
	ret = cdev_add(&drv->scc_cdev, drv->devt + 1, 1);
	if (ret)
		goto err_cdev;

	drv->class = class_create("linqu-ub");
	if (IS_ERR(drv->class)) {
		ret = PTR_ERR(drv->class);
		goto err_scc_cdev;
	}

	if (IS_ERR(device_create(drv->class, drv->dev, drv->devt, NULL, "linqu-ub0"))) {
		ret = -EINVAL;
		goto err_class;
	}
	if (IS_ERR(device_create(drv->class, drv->dev, drv->devt + 1,
				 NULL, "linqu-scc0"))) {
		ret = -EINVAL;
		goto err_ub_device;
	}

	return 0;

err_ub_device:
	device_destroy(drv->class, drv->devt);
err_class:
	class_destroy(drv->class);
err_scc_cdev:
	cdev_del(&drv->scc_cdev);
err_cdev:
	cdev_del(&drv->cdev);
err_chrdev:
	unregister_chrdev_region(drv->devt, 2);
	return ret;
}

static void linqu_ub_chrdev_cleanup(struct linqu_ub_drv *drv)
{
	if (drv->class) {
		device_destroy(drv->class, drv->devt + 1);
		device_destroy(drv->class, drv->devt);
		class_destroy(drv->class);
	}
	cdev_del(&drv->cdev);
	cdev_del(&drv->scc_cdev);
	unregister_chrdev_region(drv->devt, 2);
}

static int linqu_ub_probe(struct platform_device *pdev)
{
	struct linqu_ub_drv *drv;
	struct resource *resource;
	int irq;
	int ret;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->dev = &pdev->dev;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	mutex_init(&drv->lock);
	mutex_init(&drv->queue_lock);
	init_waitqueue_head(&drv->waitq);

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource)
		return -ENODEV;
	drv->mmio = devm_ioremap(&pdev->dev, resource->start,
				 resource_size(resource));
	if (!drv->mmio)
		return -ENOMEM;
	drv->ep_mmio = (u8 __iomem *)drv->mmio + LINQU_UB_ENDPOINT1_OFFSET;
	drv->obmm_async_mmio = (u8 __iomem *)drv->mmio +
		LINQU_UB_OBMM_ASYNC_OFFSET;
	drv->obmm_scc_mmio = (u8 __iomem *)drv->mmio +
		LINQU_UB_OBMM_SCC_OFFSET;

	irq = platform_get_irq_optional(pdev, 0);
	drv->irq = irq;

	if (irq >= 0) {
		ret = devm_request_threaded_irq(&pdev->dev, irq,
						linqu_ub_irq_primary,
						linqu_ub_irq_thread,
						IRQF_ONESHOT,
						dev_name(&pdev->dev), drv);
		if (ret)
			return ret;
	} else if (irq != -ENXIO && irq != -ENODEV) {
		return irq;
	}

	ret = linqu_ub_chrdev_init(drv);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, drv);
	dev_info(&pdev->dev, "linqu-ub driver bound irq=%d mmio=%p ep_mmio=%p\n",
		 irq, drv->mmio, drv->ep_mmio);
	return 0;
}

static void linqu_ub_remove(struct platform_device *pdev)
{
	struct linqu_ub_drv *drv = platform_get_drvdata(pdev);

	linqu_ub_chrdev_cleanup(drv);
}

static const struct of_device_id linqu_ub_of_match[] = {
	{ .compatible = "linqu,ub" },
	{ }
};
MODULE_DEVICE_TABLE(of, linqu_ub_of_match);

static struct platform_driver linqu_ub_driver = {
	.probe = linqu_ub_probe,
	.remove_new = linqu_ub_remove,
	.driver = {
		.name = "linqu-ub",
		.of_match_table = linqu_ub_of_match,
	},
};

module_platform_driver(linqu_ub_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal linqu-ub guest platform driver");
MODULE_AUTHOR("OpenAI Codex");
