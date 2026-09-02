// SPDX-License-Identifier: GPL-2.0
#include <asm/mmu_context.h>
#include <asm/ptrace.h>
#include <asm/esr.h>
#include <asm/syscall.h>
#include <asm/sysreg.h>
#include <linux/arm64_remote_load.h>
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
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <ub/obmm_async.h>
#include <ub/obmm_async_load.h>

#define LINQU_UB_REG_IRQ_STATUS 0x078
#define LINQU_UB_REG_IRQ_ACK    0x080
#define LINQU_UB_ENDPOINT1_OFFSET 0x1000
#define LINQU_UB_OBMM_ASYNC_OFFSET 0x2000
#define LINQU_UB_OBMM_ASYNC_LOAD_OFFSET 0x3000

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

#define OBMM_ASYNC_LOAD_REG_VERSION_CAPS 0x000
#define OBMM_ASYNC_LOAD_REG_STATUS 0x008
#define OBMM_ASYNC_LOAD_REG_LAST_ERROR 0x010
#define OBMM_ASYNC_LOAD_REG_OWNER_GENERATION 0x018
#define OBMM_ASYNC_LOAD_REG_MAP_GSVA_BASE 0x020
#define OBMM_ASYNC_LOAD_REG_MAP_LOCAL_PA 0x028
#define OBMM_ASYNC_LOAD_REG_MAP_LENGTH 0x030
#define OBMM_ASYNC_LOAD_REG_MAP_ID 0x038
#define OBMM_ASYNC_LOAD_REG_MAP_GENERATION 0x040
#define OBMM_ASYNC_LOAD_REG_MAP_COMMAND 0x048
#define OBMM_ASYNC_LOAD_REG_SESSION_COMMAND 0x0a0
#define OBMM_ASYNC_LOAD_REG_CLOCK_MHZ 0x0f8
#define OBMM_ASYNC_LOAD_REG_LOAD_TIMEOUT_NS 0x100
#define OBMM_ASYNC_LOAD_REG_OWNER_TTBR0 0x108
#define OBMM_ASYNC_LOAD_REG_MAP_MODEL_GENERATION 0x110
#define OBMM_ASYNC_LOAD_REG_MAP_FLAGS 0x118
#define OBMM_ASYNC_LOAD_REG_UPCALL_ENTRY 0x120
#define OBMM_ASYNC_LOAD_REG_LOGICAL_CONTEXTS 0x128
#define OBMM_ASYNC_LOAD_REG_EVENT_RING_BASE 0x138
#define OBMM_ASYNC_LOAD_REG_EVENT_RING_BYTES 0x140
#define OBMM_ASYNC_LOAD_REG_EVENT_CONSUMER_BASE 0x148
#define OBMM_ASYNC_LOAD_REG_EVENT_CONSUMER_BYTES 0x150
#define OBMM_ASYNC_LOAD_REG_EVENT_SLOT_BYTES 0x158
#define OBMM_ASYNC_LOAD_REG_EVENT_PRODUCER_SEQUENCE 0x160
#define OBMM_ASYNC_LOAD_REG_EVENT_CONSUMER_SEQUENCE 0x168
#define OBMM_ASYNC_LOAD_REG_EVENT_RING_PUBLISHED 0x170
#define OBMM_ASYNC_LOAD_REG_EVENT_WAIT_WAKEUPS 0x180
#define OBMM_ASYNC_LOAD_REG_SCHEDULER_ENTERS 0x188
#define OBMM_ASYNC_LOAD_REG_SESSION_FLAGS 0x190
#define OBMM_ASYNC_LOAD_REG_CAPABILITIES 0x198
#define OBMM_ASYNC_LOAD_REG_IRQ_STATUS 0x1a0
#define OBMM_ASYNC_LOAD_REG_IRQ_ACK 0x1a8
#define OBMM_ASYNC_LOAD_REG_REPLAY_CONTEXT_ID 0x1b0
#define OBMM_ASYNC_LOAD_REG_REPLAY_TOKEN 0x1b8
#define OBMM_ASYNC_LOAD_REG_REPLAY_PC 0x1c0
#define OBMM_ASYNC_LOAD_REG_REPLAY_COMMAND 0x1c8
#define OBMM_ASYNC_LOAD_REG_STATS_BASE 0x200
#define OBMM_ASYNC_LOAD_REG_OBSERVABILITY_BASE \
	(OBMM_ASYNC_LOAD_REG_STATS_BASE + 17 * 8)
#define OBMM_ASYNC_LOAD_OBSERVABILITY_VALUES 17
#define OBMM_ASYNC_LOAD_REG_REPLAY_STATS_BASE 0x400
#define OBMM_ASYNC_LOAD_MAX_LOAD_TIMEOUT_NS 10000000000ULL
#define OBMM_ASYNC_LOAD_PSTATE_NZCV_MASK \
	(PSR_N_BIT | PSR_Z_BIT | PSR_C_BIT | PSR_V_BIT)

#define OBMM_ASYNC_LOAD_STATUS_ACTIVE BIT(0)
#define OBMM_ASYNC_LOAD_STATUS_FAIL_STOP BIT(1)
#define OBMM_ASYNC_LOAD_STATUS_EVENT_PENDING BIT(3)
#define OBMM_ASYNC_LOAD_STATUS_EVENT_DELIVERED BIT(4)
#define OBMM_ASYNC_LOAD_STATUS_UPCALL_ACTIVE BIT(5)
#define OBMM_ASYNC_LOAD_IRQ_COMPLETION BIT(0)

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
static_assert(sizeof(struct obmm_async_load_context_v2) ==
	      OBMM_ASYNC_LOAD_CONTEXT_STATE_BYTES);
static_assert(offsetof(struct obmm_async_load_context_v2, x) == 16);
static_assert(offsetof(struct obmm_async_load_context_v2, sp) == 264);
static_assert(offsetof(struct obmm_async_load_context_v2, pc) == 272);
static_assert(offsetof(struct obmm_async_load_context_v2, q) == 288);
static_assert(offsetof(struct obmm_async_load_context_v2, fpcr) == 800);
static_assert(sizeof(struct obmm_async_load_caps_v4) == 112);
static_assert(sizeof(struct obmm_async_load_map_register_v1) == 64);
static_assert(sizeof(struct obmm_async_load_start_v3) == 40);
static_assert(sizeof(struct obmm_async_load_event_producer_v3) == 64);
static_assert(sizeof(struct obmm_async_load_event_consumer_v3) == 64);
static_assert(sizeof(struct obmm_async_load_event_v3) == 128);
static_assert(sizeof(struct obmm_async_load_stats_v3) == 152);
static_assert(sizeof(struct obmm_async_load_observability_v3) == 144);
static_assert(sizeof(struct obmm_async_load_kernel_task_stats_v1) == 64);

struct linqu_obmm_async_load_map {
	bool allocated;
	u64 generation;
};

struct linqu_async_load_kernel_wait {
	wait_queue_head_t waitq;
	u64 plt_token;
	u64 context_id;
	u64 context_cookie;
	u64 fault_pc;
	u64 effective_va;
	u32 status;
	pid_t task_pid;
	bool pending_seen;
	bool completion_seen;
	bool claimed;
};

struct linqu_async_load_file {
	struct linqu_ub_drv *drv;
	struct mutex lock;
	refcount_t refs;
	void *event_ring_cpu;
	dma_addr_t event_ring_dma;
	size_t event_ring_bytes;
	void *event_consumer_cpu;
	dma_addr_t event_consumer_dma;
	size_t event_consumer_bytes;
	pid_t owner_tgid;
	bool started;
	bool kernel_task_mode;
	u64 owner_generation;
	u64 load_timeout_ns;
	u64 next_map_generation;
	u64 capabilities;
	u16 context_entries;
	u16 pending_load_entries;
	u16 event_queue_depth;
	struct linqu_obmm_async_load_map maps[OBMM_ASYNC_LOAD_MAX_PENDING_LOADS];
	struct linqu_async_load_kernel_wait
		kernel_waits[OBMM_ASYNC_LOAD_MAX_PENDING_LOADS];
	struct obmm_async_load_kernel_task_stats_v1 kernel_stats;
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
	u8 __iomem *obmm_async_load_mmio;
	int irq;
	wait_queue_head_t waitq;
	/* Serializes the legacy endpoint file and interrupt state. */
	struct mutex lock;
	/* Serializes submit/await queue and async-load endpoint ownership. */
	struct mutex queue_lock;
	struct cdev cdev;
	struct cdev async_load_cdev;
	dev_t devt;
	struct class *class;
	bool event_pending;
	u64 irq_count;
	u64 last_irq_status;
	struct linqu_ub_file *active_file;
	struct linqu_async_load_file *active_async_load_file;
	u32 next_queue_id;
};

static struct linqu_ub_drv *linqu_remote_load_drv;
static bool remote_load_event_log = true;
module_param(remote_load_event_log, bool, 0644);
MODULE_PARM_DESC(remote_load_event_log,
		 "log every remote-load pending, completion, block, wake, and IRQ event");
static void linqu_async_load_ctx_put(struct linqu_async_load_file *ctx);

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

static long linqu_obmm_async_map_register_common(
	struct linqu_ub_file *ctx, u64 mem_id, u64 mapped_addr, u64 length,
	u32 flags, u32 reserved, u64 *map_id, u64 *map_generation,
	u64 *local_pa_out)
{
	struct linqu_ub_drv *drv = ctx->drv;
	struct vm_area_struct *vma;
	unsigned long pfn;
	u64 local_pa;
	u32 index;
	int ret;

	if (!mem_id || !length || mapped_addr > U64_MAX - length || flags ||
	    reserved || !map_id || !map_generation || !local_pa_out)
		return -EINVAL;
	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, mapped_addr);
	if (!vma || mapped_addr < vma->vm_start ||
	    mapped_addr + length > vma->vm_end) {
		ret = -EFAULT;
		goto out_unlock;
	}
	ret = follow_pfn(vma, mapped_addr, &pfn);
	if (ret)
		goto out_unlock;
	local_pa = PFN_PHYS(pfn) + offset_in_page(mapped_addr);
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
	ctx->maps[index].length = length;
	*map_id = index + 1;
	*map_generation = ctx->maps[index].generation;
	*local_pa_out = local_pa;

	writeq(local_pa, drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_LOCAL_PA);
	writeq(length,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_LENGTH);
	writeq(*map_id,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_ID);
	writeq(*map_generation,
	       drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_GENERATION);
	/* Publish all map descriptor fields before the registration command. */
	wmb();
	writeq(1, drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_CMD);
	if (readq(drv->obmm_async_mmio + OBMM_ASYNC_REG_LAST_ERROR)) {
		ctx->maps[index].allocated = false;
		return -EIO;
	}
	return 0;
}

static long linqu_obmm_async_map_register(struct linqu_ub_file *ctx,
					  unsigned long arg)
{
	struct obmm_async_map_register_v1 request;
	u64 local_pa;
	int ret;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	ret = linqu_obmm_async_map_register_common(
		ctx, request.mem_id, request.mapped_addr, request.length,
		request.flags, request.reserved, &request.map_id,
		&request.map_generation, &local_pa);
	if (ret)
		return ret;
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		writeq(request.map_id,
		       ctx->drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_ID);
		writeq(request.map_generation,
		       ctx->drv->obmm_async_mmio +
		       OBMM_ASYNC_REG_MAP_GENERATION);
		writeq(2, ctx->drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_CMD);
		ctx->maps[request.map_id - 1].allocated = false;
		return -EFAULT;
	}
	return 0;
}

static long linqu_obmm_async_map_register_v2(struct linqu_ub_file *ctx,
					     unsigned long arg)
{
	struct obmm_async_map_register_v2 request;
	int ret;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	ret = linqu_obmm_async_map_register_common(
		ctx, request.mem_id, request.mapped_addr, request.length,
		request.flags, request.reserved, &request.map_id,
		&request.map_generation, &request.local_pa);
	if (ret)
		return ret;
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		writeq(request.map_id,
		       ctx->drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_ID);
		writeq(request.map_generation,
		       ctx->drv->obmm_async_mmio +
		       OBMM_ASYNC_REG_MAP_GENERATION);
		writeq(2, ctx->drv->obmm_async_mmio + OBMM_ASYNC_REG_MAP_CMD);
		ctx->maps[request.map_id - 1].allocated = false;
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
	case OBMM_ASYNC_IOCTL_MAP_REGISTER_V2:
		return linqu_obmm_async_map_register_v2(ctx, arg);
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

static int linqu_async_load_open(struct inode *inode, struct file *file)
{
	struct linqu_ub_drv *drv = container_of(inode->i_cdev,
						struct linqu_ub_drv, async_load_cdev);
	struct linqu_async_load_file *ctx;
	struct obmm_async_load_event_consumer_v3 *consumer;
	u32 index;
	u64 version;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->drv = drv;
	mutex_init(&ctx->lock);
	refcount_set(&ctx->refs, 1);
	ctx->owner_tgid = task_tgid_nr(current);
	ctx->next_map_generation = 1;
	for (index = 0; index < OBMM_ASYNC_LOAD_MAX_PENDING_LOADS; index++)
		init_waitqueue_head(&ctx->kernel_waits[index].waitq);

	mutex_lock(&drv->queue_lock);
	if (drv->active_async_load_file) {
		mutex_unlock(&drv->queue_lock);
		kfree(ctx);
		return -EBUSY;
	}
	version = readq(drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_VERSION_CAPS);
	ctx->context_entries = version >> 16;
	ctx->pending_load_entries = version >> 32;
	ctx->event_queue_depth = version >> 48;
	if ((u16)version != OBMM_ASYNC_LOAD_ABI_VERSION ||
	    !ctx->context_entries ||
	    ctx->context_entries > OBMM_ASYNC_LOAD_MAX_CONTEXTS ||
	    !ctx->pending_load_entries ||
	    ctx->pending_load_entries > OBMM_ASYNC_LOAD_MAX_PENDING_LOADS ||
	    !ctx->event_queue_depth ||
	    ctx->event_queue_depth > OBMM_ASYNC_LOAD_MAX_EVENTS) {
		mutex_unlock(&drv->queue_lock);
		kfree(ctx);
		return -ENODEV;
	}
	ctx->owner_generation = readq(drv->obmm_async_load_mmio +
				      OBMM_ASYNC_LOAD_REG_OWNER_GENERATION);
	ctx->capabilities = OBMM_ASYNC_LOAD_CAP_SCALAR_1 |
		OBMM_ASYNC_LOAD_CAP_SCALAR_2 | OBMM_ASYNC_LOAD_CAP_SCALAR_4 |
		OBMM_ASYNC_LOAD_CAP_SCALAR_8 | OBMM_ASYNC_LOAD_CAP_XZR |
		OBMM_ASYNC_LOAD_CAP_DIRECT_EL0_UPCALL | OBMM_ASYNC_LOAD_CAP_EL0_RESUME |
		OBMM_ASYNC_LOAD_CAP_FULL_CONTEXT |
		readq(drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_CAPABILITIES);
	ctx->event_ring_bytes = PAGE_ALIGN(
		OBMM_ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES +
		(size_t)ctx->event_queue_depth * OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES);
	ctx->event_consumer_bytes = PAGE_SIZE;
	ctx->event_ring_cpu = dma_alloc_coherent(
		drv->dev, ctx->event_ring_bytes, &ctx->event_ring_dma, GFP_KERNEL);
	if (!ctx->event_ring_cpu) {
		mutex_unlock(&drv->queue_lock);
		kfree(ctx);
		return -ENOMEM;
	}
	ctx->event_consumer_cpu = dma_alloc_coherent(
		drv->dev, ctx->event_consumer_bytes,
		&ctx->event_consumer_dma, GFP_KERNEL);
	if (!ctx->event_consumer_cpu) {
		dma_free_coherent(drv->dev, ctx->event_ring_bytes,
				  ctx->event_ring_cpu, ctx->event_ring_dma);
		mutex_unlock(&drv->queue_lock);
		kfree(ctx);
		return -ENOMEM;
	}
	memset(ctx->event_ring_cpu, 0, ctx->event_ring_bytes);
	memset(ctx->event_consumer_cpu, 0, ctx->event_consumer_bytes);
	consumer = ctx->event_consumer_cpu;
	consumer->abi_version = OBMM_ASYNC_LOAD_EVENT_ABI_VERSION;
	consumer->owner_generation = ctx->owner_generation;
	drv->active_async_load_file = ctx;
	mutex_unlock(&drv->queue_lock);
	file->private_data = ctx;
	return 0;
}

static bool linqu_async_load_owner(const struct linqu_async_load_file *ctx)
{
	return task_tgid_nr(current) == ctx->owner_tgid;
}

static void linqu_async_load_kernel_wait_reset(
	struct linqu_async_load_kernel_wait *wait)
{
	wait->plt_token = 0;
	wait->context_id = 0;
	wait->context_cookie = 0;
	wait->fault_pc = 0;
	wait->effective_va = 0;
	wait->status = OBMM_ASYNC_LOAD_STATUS_INTERNAL;
	wait->task_pid = 0;
	wait->pending_seen = false;
	wait->completion_seen = false;
	wait->claimed = false;
}

static int linqu_async_load_kernel_event_locked(
	struct linqu_async_load_file *ctx,
	const struct obmm_async_load_event_v3 *event)
{
	struct linqu_async_load_kernel_wait *wait;
	u64 owner_generation = le64_to_cpu(event->owner_generation);
	u64 context_id = le64_to_cpu(event->context_id);
	u64 context_cookie = le64_to_cpu(event->reserved[0]);
	u64 plt_token = le64_to_cpu(event->plt_token);
	u64 fault_pc = le64_to_cpu(event->fault_pc);
	u64 effective_va = le64_to_cpu(event->effective_va);
	u32 kind = le32_to_cpu(event->kind);
	u32 status = le32_to_cpu(event->status);
	u32 flags = le32_to_cpu(event->flags);
	u16 slot = plt_token & 0xffff;

	if (owner_generation != ctx->owner_generation ||
	    le64_to_cpu(event->reserved[1]) ||
	    le64_to_cpu(event->reserved[2]) ||
	    slot >= ctx->pending_load_entries)
		goto protocol_error;
	wait = &ctx->kernel_waits[slot];

	switch (kind) {
	case OBMM_ASYNC_LOAD_EVENT_PENDING:
		if (status != OBMM_ASYNC_LOAD_STATUS_SUCCESS || flags ||
		    wait->pending_seen || wait->completion_seen)
			goto protocol_error;
		wait->plt_token = plt_token;
		wait->context_id = context_id;
		wait->context_cookie = context_cookie;
		wait->fault_pc = fault_pc;
		wait->effective_va = effective_va;
		wait->status = status;
		wait->pending_seen = true;
		ctx->kernel_stats.pending_events++;
		if (remote_load_event_log)
			dev_info(ctx->drv->dev,
				 "remote-load pending context=0x%llx token=0x%llx pc=0x%llx va=0x%llx\n",
				 context_id, plt_token, fault_pc, effective_va);
		return 0;
	case OBMM_ASYNC_LOAD_EVENT_COMPLETE:
	case OBMM_ASYNC_LOAD_EVENT_FAULT:
		if (!wait->pending_seen || wait->plt_token != plt_token ||
		    wait->context_id != context_id ||
		    wait->context_cookie != context_cookie ||
		    wait->fault_pc != fault_pc ||
		    wait->effective_va != effective_va ||
		    wait->completion_seen ||
		    (kind == OBMM_ASYNC_LOAD_EVENT_COMPLETE &&
		     !(flags & OBMM_ASYNC_LOAD_EVENT_RETIRE_REPLAY)) ||
		    flags & ~OBMM_ASYNC_LOAD_EVENT_RETIRE_REPLAY)
			goto protocol_error;
		wait->status = status;
		wait->completion_seen = true;
		ctx->kernel_stats.completion_events++;
		if (remote_load_event_log)
			dev_info(ctx->drv->dev,
				 "remote-load completion context=0x%llx token=0x%llx status=%u\n",
				 context_id, plt_token, status);
		wake_up(&wait->waitq);
		return 0;
	default:
		goto protocol_error;
	}

protocol_error:
	ctx->kernel_stats.protocol_errors++;
	return -EPROTO;
}

static int linqu_async_load_drain_kernel_events_locked(
	struct linqu_async_load_file *ctx)
{
	struct obmm_async_load_event_producer_v3 *producer =
		ctx->event_ring_cpu;
	struct obmm_async_load_event_consumer_v3 *consumer =
		ctx->event_consumer_cpu;
	u64 consumer_sequence = le64_to_cpu(READ_ONCE(consumer->consumer_sequence));
	u64 producer_sequence = le64_to_cpu(
		READ_ONCE(producer->producer_sequence));
	int ret = 0;

	dma_rmb();
	if (le32_to_cpu(READ_ONCE(producer->abi_version)) !=
		    OBMM_ASYNC_LOAD_EVENT_ABI_VERSION ||
	    le16_to_cpu(READ_ONCE(producer->event_depth)) !=
		    ctx->event_queue_depth ||
	    le16_to_cpu(READ_ONCE(producer->event_slot_bytes)) !=
		    OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES ||
	    le64_to_cpu(READ_ONCE(producer->owner_generation)) !=
		    ctx->owner_generation ||
	    producer_sequence < consumer_sequence ||
	    producer_sequence - consumer_sequence > ctx->event_queue_depth) {
		ctx->kernel_stats.protocol_errors++;
		return -EPROTO;
	}

	while (consumer_sequence < producer_sequence) {
		struct obmm_async_load_event_v3 *event;
		u64 sequence = consumer_sequence + 1;
		u64 slot = (sequence - 1) % ctx->event_queue_depth;
		int event_ret;

		event = ctx->event_ring_cpu +
			OBMM_ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES +
			slot * OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES;
		dma_rmb();
		if (le64_to_cpu(READ_ONCE(event->sequence)) != sequence) {
			ctx->kernel_stats.protocol_errors++;
			return -EPROTO;
		}
		event_ret = linqu_async_load_kernel_event_locked(ctx, event);
		if (event_ret && !ret)
			ret = event_ret;
		consumer_sequence = sequence;
	}
	dma_wmb();
	WRITE_ONCE(consumer->consumer_sequence, cpu_to_le64(consumer_sequence));
	return ret;
}

static int linqu_async_load_resume_command(
	struct linqu_async_load_file *ctx, u64 context_id,
	u64 replay_token, u64 replay_pc)
{
	struct linqu_ub_drv *drv = ctx->drv;

	dma_wmb();
	writeq(context_id, drv->obmm_async_load_mmio +
	       OBMM_ASYNC_LOAD_REG_REPLAY_CONTEXT_ID);
	writeq(replay_token, drv->obmm_async_load_mmio +
	       OBMM_ASYNC_LOAD_REG_REPLAY_TOKEN);
	writeq(replay_pc, drv->obmm_async_load_mmio +
	       OBMM_ASYNC_LOAD_REG_REPLAY_PC);
	writeq(1, drv->obmm_async_load_mmio +
	       OBMM_ASYNC_LOAD_REG_REPLAY_COMMAND);
	return readq(drv->obmm_async_load_mmio +
		     OBMM_ASYNC_LOAD_REG_LAST_ERROR) ? -EPROTO : 0;
}

static int linqu_async_load_scheduler_enter_command(
	struct linqu_async_load_file *ctx)
{
	struct linqu_ub_drv *drv = ctx->drv;

	dma_wmb();
	writeq(2, drv->obmm_async_load_mmio +
	       OBMM_ASYNC_LOAD_REG_REPLAY_COMMAND);
	return readq(drv->obmm_async_load_mmio +
		     OBMM_ASYNC_LOAD_REG_LAST_ERROR) ? -EPROTO : 0;
}

static int linqu_remote_load_svc(unsigned int imm, struct pt_regs *regs)
{
	struct obmm_async_load_context_v2 context;
	struct linqu_async_load_file *ctx;
	struct linqu_ub_drv *drv = READ_ONCE(linqu_remote_load_drv);
	u64 replay_token;
	int ret = -ENOENT;

	if (imm != OBMM_ASYNC_LOAD_RESUME_SVC_IMM &&
	    imm != OBMM_ASYNC_LOAD_SCHEDULER_ENTER_SVC_IMM)
		return -ENOENT;
	if (!drv || !user_mode(regs))
		return -ENODEV;

	mutex_lock(&drv->queue_lock);
	ctx = drv->active_async_load_file;
	if (!ctx || !ctx->started || ctx->kernel_task_mode ||
	    !linqu_async_load_owner(ctx) ||
	    !refcount_inc_not_zero(&ctx->refs)) {
		mutex_unlock(&drv->queue_lock);
		return -EPERM;
	}
	mutex_unlock(&drv->queue_lock);

	mutex_lock(&ctx->lock);
	if (imm == OBMM_ASYNC_LOAD_SCHEDULER_ENTER_SVC_IMM) {
		ret = linqu_async_load_scheduler_enter_command(ctx);
		goto out_unlock;
	}
	if (!IS_ALIGNED(regs->regs[0], 16) ||
	    copy_from_user(&context,
			   (void __user *)(uintptr_t)regs->regs[0],
			   sizeof(context))) {
		ret = -EFAULT;
		goto out_unlock;
	}
	if (!context.context_id || !context.pc || !context.sp ||
	    !IS_ALIGNED(context.sp, 16) || context.reserved ||
	    (context.context_id >> 32) != ctx->owner_generation ||
	    ((context.context_id >> 16) & 0xffff) != smp_processor_id() ||
	    (context.context_id & 0xffff) >= ctx->context_entries) {
		ret = -EINVAL;
		goto out_unlock;
	}
	replay_token = regs->regs[1];
	ret = linqu_async_load_resume_command(
		ctx, context.context_id, replay_token,
		replay_token ? context.pc : 0);
	if (ret)
		goto out_unlock;

	memcpy(regs->regs, context.x, sizeof(context.x));
	regs->sp = context.sp;
	regs->pc = context.pc;
	regs->pstate = (regs->pstate & ~OBMM_ASYNC_LOAD_PSTATE_NZCV_MASK) |
		(context.nzcv & OBMM_ASYNC_LOAD_PSTATE_NZCV_MASK);
	regs->orig_x0 = 0;
	regs->syscallno = NO_SYSCALL;

out_unlock:
	mutex_unlock(&ctx->lock);
	linqu_async_load_ctx_put(ctx);
	return ret;
}

static int linqu_remote_load_fault(unsigned long far, unsigned long esr,
				    struct pt_regs *regs)
{
	struct linqu_async_load_kernel_wait *wait = NULL;
	struct linqu_async_load_file *ctx;
	struct linqu_ub_drv *drv = READ_ONCE(linqu_remote_load_drv);
	u64 context_cookie = read_sysreg(tpidr_el0);
	long timeout;
	long wait_ret;
	u32 index;
	int ret = 0;

	if (!drv || (esr & ESR_ELx_FSC) != ESR_ELx_FSC_REMOTE_LOAD)
		return -ENODEV;

	mutex_lock(&drv->queue_lock);
	ctx = drv->active_async_load_file;
	if (!ctx || !ctx->started || !ctx->kernel_task_mode ||
	    !linqu_async_load_owner(ctx) ||
	    !refcount_inc_not_zero(&ctx->refs)) {
		mutex_unlock(&drv->queue_lock);
		return -EPERM;
	}
	mutex_unlock(&drv->queue_lock);

	mutex_lock(&ctx->lock);
	ctx->kernel_stats.faults++;
	ret = linqu_async_load_drain_kernel_events_locked(ctx);
	if (ret)
		goto out_unlock;
	for (index = 0; index < ctx->pending_load_entries; index++) {
		struct linqu_async_load_kernel_wait *candidate =
			&ctx->kernel_waits[index];

		if (candidate->pending_seen && !candidate->claimed &&
		    candidate->context_cookie == context_cookie &&
		    candidate->fault_pc == regs->pc &&
		    candidate->effective_va == far) {
			wait = candidate;
			break;
		}
	}
	if (!wait) {
		ctx->kernel_stats.protocol_errors++;
		ret = -EPROTO;
		goto out_unlock;
	}
	wait->claimed = true;
	wait->task_pid = task_pid_nr(current);
	if (!wait->completion_seen)
		ctx->kernel_stats.task_sleeps++;
	if (remote_load_event_log)
		dev_info(drv->dev,
			 "remote-load block pid=%d token=0x%llx pc=0x%llx\n",
			 wait->task_pid, wait->plt_token, wait->fault_pc);
	mutex_unlock(&ctx->lock);

	if (ctx->load_timeout_ns) {
		timeout = max_t(long, 1, nsecs_to_jiffies(ctx->load_timeout_ns));
		timeout += HZ;
		wait_ret = wait_event_killable_timeout(
			wait->waitq, READ_ONCE(wait->completion_seen), timeout);
	} else {
		wait_ret = wait_event_killable(
			wait->waitq, READ_ONCE(wait->completion_seen));
	}

	mutex_lock(&ctx->lock);
	if (wait_ret < 0) {
		ctx->kernel_stats.interrupted_waits++;
		ret = wait_ret;
		goto out_unlock;
	}
	if (!wait_ret && !wait->completion_seen) {
		ctx->kernel_stats.timeouts++;
		ret = -ETIMEDOUT;
		goto out_unlock;
	}
	ctx->kernel_stats.task_wakeups++;
	if (wait->status == OBMM_ASYNC_LOAD_STATUS_SUCCESS)
		ret = linqu_async_load_resume_command(
			ctx, wait->context_id, wait->plt_token, wait->fault_pc);
	else
		ret = -EIO;
	if (remote_load_event_log)
		dev_info(drv->dev,
			 "remote-load wake pid=%d token=0x%llx status=%u replay=%u\n",
			 wait->task_pid, wait->plt_token, wait->status,
			 ret == 0);
	linqu_async_load_kernel_wait_reset(wait);

out_unlock:
	mutex_unlock(&ctx->lock);
	linqu_async_load_ctx_put(ctx);
	return ret;
}

static const struct arm64_remote_load_fault_ops linqu_remote_load_fault_ops = {
	.handle = linqu_remote_load_fault,
	.handle_svc = linqu_remote_load_svc,
	.owner = THIS_MODULE,
};

static void linqu_async_load_unregister_all(struct linqu_async_load_file *ctx)
{
	struct linqu_ub_drv *drv = ctx->drv;
	u32 index;

	for (index = 0; index < ctx->pending_load_entries; index++) {
		if (!ctx->maps[index].allocated)
			continue;
		writeq(index + 1,
		       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_ID);
		writeq(ctx->maps[index].generation,
		       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_GENERATION);
		writeq(2, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_COMMAND);
		ctx->maps[index].allocated = false;
	}
}

static void linqu_async_load_ctx_put(struct linqu_async_load_file *ctx)
{
	struct linqu_ub_drv *drv = ctx->drv;

	if (!refcount_dec_and_test(&ctx->refs))
		return;
	dma_free_coherent(drv->dev, ctx->event_consumer_bytes,
			  ctx->event_consumer_cpu, ctx->event_consumer_dma);
	dma_free_coherent(drv->dev, ctx->event_ring_bytes,
			  ctx->event_ring_cpu, ctx->event_ring_dma);
	kfree(ctx);
}

static int linqu_async_load_release(struct inode *inode, struct file *file)
{
	struct linqu_async_load_file *ctx = file->private_data;
	struct linqu_ub_drv *drv = ctx->drv;

	mutex_lock(&drv->queue_lock);
	if (ctx->started)
		writeq(2, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_SESSION_COMMAND);
	linqu_async_load_unregister_all(ctx);
	if (drv->active_async_load_file == ctx)
		drv->active_async_load_file = NULL;
	mutex_unlock(&drv->queue_lock);
	linqu_async_load_ctx_put(ctx);
	return 0;
}

static int linqu_async_load_mmap(struct file *file,
				 struct vm_area_struct *vma)
{
	struct linqu_async_load_file *ctx = file->private_data;
	struct linqu_ub_drv *drv = ctx->drv;
	unsigned long selector = vma->vm_pgoff;
	unsigned long length = vma->vm_end - vma->vm_start;
	unsigned long consumer_selector =
		ctx->event_ring_bytes >> PAGE_SHIFT;

	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTDUMP);
	vma->vm_pgoff = 0;
	if (selector == 0 && length == ctx->event_ring_bytes) {
		if (vma->vm_flags & VM_WRITE)
			return -EPERM;
		return dma_mmap_coherent(drv->dev, vma, ctx->event_ring_cpu,
					 ctx->event_ring_dma,
					 ctx->event_ring_bytes);
	}
	if (selector == consumer_selector &&
	    length == ctx->event_consumer_bytes &&
	    vma->vm_flags & VM_WRITE)
		return dma_mmap_coherent(drv->dev, vma,
					 ctx->event_consumer_cpu,
					 ctx->event_consumer_dma,
					 ctx->event_consumer_bytes);
	return -EINVAL;
}

static long linqu_async_load_query_caps(struct linqu_async_load_file *ctx,
				 unsigned long arg)
{
	u64 version = readq(ctx->drv->obmm_async_load_mmio +
			    OBMM_ASYNC_LOAD_REG_VERSION_CAPS);
	struct obmm_async_load_caps_v4 caps = {
		.abi_version = version & 0xffff,
		.context_entries = (version >> 16) & 0xffff,
		.pending_load_entries = (version >> 32) & 0xffff,
		.event_queue_depth = (version >> 48) & 0xffff,
		.context_state_bytes = OBMM_ASYNC_LOAD_CONTEXT_STATE_BYTES,
		.capabilities = ctx->capabilities,
		.owner_generation = ctx->owner_generation,
		.clock_mhz = readq(ctx->drv->obmm_async_load_mmio +
				  OBMM_ASYNC_LOAD_REG_CLOCK_MHZ),
		.resume_svc_imm = OBMM_ASYNC_LOAD_RESUME_SVC_IMM,
		.scheduler_enter_svc_imm =
			OBMM_ASYNC_LOAD_SCHEDULER_ENTER_SVC_IMM,
		.event_slot_bytes = OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES,
		.event_producer_header_bytes =
			OBMM_ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES,
		.event_ring_mmap_offset = 0,
		.event_ring_mmap_bytes = ctx->event_ring_bytes,
		.event_consumer_mmap_offset = ctx->event_ring_bytes,
		.event_consumer_mmap_bytes = ctx->event_consumer_bytes,
	};

	return copy_to_user((void __user *)arg, &caps, sizeof(caps)) ?
		-EFAULT : 0;
}

static long linqu_async_load_register_map(struct linqu_async_load_file *ctx,
				   unsigned long arg)
{
	struct obmm_async_load_map_register_v1 request;
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
	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	if (!request.mem_id || !request.gsva_base ||
	    request.mapped_addr != request.gsva_base || !request.length ||
	    request.gsva_base > U64_MAX - request.length ||
	    request.flags & ~OBMM_ASYNC_LOAD_MAP_LOGICAL_MIXED ||
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
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_GSVA_BASE);
	writeq(local_pa, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_LOCAL_PA);
	writeq(request.length, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_LENGTH);
	writeq(request.policy_id, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_ID);
	writeq(request.map_generation,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_GENERATION);
	writeq(request.model_phase_generation,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_MODEL_GENERATION);
	writeq(request.flags,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_FLAGS);
	/* Publish the complete map descriptor before the command doorbell. */
	wmb();
	writeq(1, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_COMMAND);
	if (readq(drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_LAST_ERROR))
		return -EIO;
	ctx->maps[index].allocated = true;
	ctx->maps[index].generation = request.map_generation;
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		writeq(request.policy_id,
		       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_ID);
		writeq(request.map_generation,
		       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_GENERATION);
		writeq(2, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_COMMAND);
		ctx->maps[index].allocated = false;
		return -EFAULT;
	}
	return 0;
}

static long linqu_async_load_unregister_map(struct linqu_async_load_file *ctx,
				     unsigned long arg)
{
	struct obmm_async_load_map_unregister_v1 request;
	struct linqu_ub_drv *drv = ctx->drv;
	u32 index;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	if (!request.policy_id ||
	    request.policy_id > ctx->pending_load_entries)
		return -EINVAL;
	index = request.policy_id - 1;
	if (!ctx->maps[index].allocated ||
	    ctx->maps[index].generation != request.map_generation)
		return -ESTALE;
	writeq(request.policy_id, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_ID);
	writeq(request.map_generation,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_GENERATION);
	writeq(2, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_MAP_COMMAND);
	if (readq(drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_LAST_ERROR))
		return -EBUSY;
	ctx->maps[index].allocated = false;
	return 0;
}

static long linqu_async_load_start(struct linqu_async_load_file *ctx,
			    unsigned long arg)
{
	struct obmm_async_load_start_v3 request;
	struct obmm_async_load_event_consumer_v3 *consumer;
	struct linqu_ub_drv *drv = ctx->drv;
	bool kernel_task_mode;
	u64 required_capabilities;
	u32 index;
	u64 status;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	kernel_task_mode = request.flags & OBMM_ASYNC_LOAD_START_KERNEL_TASK;
	required_capabilities = OBMM_ASYNC_LOAD_CAP_KERNEL_FREE_EVENT_RING |
		OBMM_ASYNC_LOAD_CAP_REPLAY_RETIRE |
		OBMM_ASYNC_LOAD_CAP_NC_REPLAY_TOKEN;
	if (kernel_task_mode)
		required_capabilities |= OBMM_ASYNC_LOAD_CAP_KERNEL_TASK_REPLAY;
	else
		required_capabilities |= OBMM_ASYNC_LOAD_CAP_EL0_WAIT_WAKE |
			OBMM_ASYNC_LOAD_CAP_EL0_SCHEDULER_ENTER |
			OBMM_ASYNC_LOAD_CAP_SVC_CONTEXT_RESUME |
			OBMM_ASYNC_LOAD_CAP_WFE_WAIT;
	if (ctx->started ||
	    (ctx->capabilities & required_capabilities) !=
	     required_capabilities ||
	    request.flags & ~(OBMM_ASYNC_LOAD_START_REPLAY_RETIRE |
			      OBMM_ASYNC_LOAD_START_KERNEL_TASK) ||
	    !(request.flags & OBMM_ASYNC_LOAD_START_REPLAY_RETIRE) ||
	    (kernel_task_mode &&
	     drv->irq < 0) ||
	    request.owner_generation ||
	    request.reserved0 ||
	    (!kernel_task_mode &&
	     (!request.upcall_entry || !IS_ALIGNED(request.upcall_entry, 4) ||
	      !access_ok((void __user *)(uintptr_t)request.upcall_entry, 4))) ||
	    (kernel_task_mode && request.upcall_entry) ||
	    !request.logical_contexts ||
	    request.logical_contexts > ctx->context_entries ||
	    request.load_timeout_ns > OBMM_ASYNC_LOAD_MAX_LOAD_TIMEOUT_NS ||
	    current->nr_cpus_allowed != 1 ||
	    request.home_cpu != task_cpu(current)) {
		dev_err(drv->dev,
			"remote-load start rejected flags=0x%x caps=0x%llx owner=%llu reserved=%u upcall=0x%llx contexts=%u/%u timeout_ns=%llu allowed_cpus=%u home_cpu=%u task_cpu=%u irq=%d\n",
			request.flags,
			(unsigned long long)ctx->capabilities,
			(unsigned long long)request.owner_generation,
			request.reserved0,
			(unsigned long long)request.upcall_entry,
			request.logical_contexts, ctx->context_entries,
			(unsigned long long)request.load_timeout_ns,
			current->nr_cpus_allowed, request.home_cpu,
			task_cpu(current), drv->irq);
		return -EINVAL;
	}
	memset(ctx->event_ring_cpu, 0, ctx->event_ring_bytes);
	memset(ctx->event_consumer_cpu, 0, ctx->event_consumer_bytes);
	consumer = ctx->event_consumer_cpu;
	consumer->abi_version = OBMM_ASYNC_LOAD_EVENT_ABI_VERSION;
	consumer->owner_generation = ctx->owner_generation;
	for (index = 0; index < ctx->pending_load_entries; index++)
		linqu_async_load_kernel_wait_reset(&ctx->kernel_waits[index]);
	memset(&ctx->kernel_stats, 0, sizeof(ctx->kernel_stats));
	writeq(phys_to_ttbr(virt_to_phys(current->mm->pgd)) |
	       (system_supports_cnp() && ASID(current->mm) ? TTBR_CNP_BIT : 0) |
	       (IS_ENABLED(CONFIG_ARM64_SW_TTBR0_PAN) ?
		FIELD_PREP(TTBR_ASID_MASK, ASID(current->mm)) : 0),
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_OWNER_TTBR0);
	writeq(request.load_timeout_ns,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_LOAD_TIMEOUT_NS);
	writeq(request.upcall_entry,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_UPCALL_ENTRY);
	writeq(request.logical_contexts,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_LOGICAL_CONTEXTS);
	writeq(ctx->event_ring_dma,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_EVENT_RING_BASE);
	writeq(ctx->event_ring_bytes,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_EVENT_RING_BYTES);
	writeq(ctx->event_consumer_dma,
	       drv->obmm_async_load_mmio +
	       OBMM_ASYNC_LOAD_REG_EVENT_CONSUMER_BASE);
	writeq(ctx->event_consumer_bytes,
	       drv->obmm_async_load_mmio +
	       OBMM_ASYNC_LOAD_REG_EVENT_CONSUMER_BYTES);
	writeq(request.flags,
	       drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_SESSION_FLAGS);
	/* Publish owner identity and deadline before enabling interception. */
	wmb();
	writeq(1, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_SESSION_COMMAND);
	status = readq(drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_STATUS);
	if (readq(drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_LAST_ERROR) ||
	    !(status & OBMM_ASYNC_LOAD_STATUS_ACTIVE))
		return -EIO;
	ctx->started = true;
	ctx->kernel_task_mode = kernel_task_mode;
	ctx->load_timeout_ns = request.load_timeout_ns;
	request.owner_generation = ctx->owner_generation;
	if (copy_to_user((void __user *)arg, &request, sizeof(request))) {
		writeq(2, drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_SESSION_COMMAND);
		ctx->started = false;
		ctx->kernel_task_mode = false;
		return -EFAULT;
	}
	return 0;
}

static long linqu_async_load_stop(struct linqu_async_load_file *ctx)
{
	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	if (!ctx->started)
		return 0;
	writeq(2, ctx->drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_SESSION_COMMAND);
	ctx->started = false;
	ctx->kernel_task_mode = false;
	return 0;
}

static long linqu_async_load_get_stats(struct linqu_async_load_file *ctx,
				unsigned long arg)
{
	struct obmm_async_load_stats_v3 stats = { 0 };
	struct linqu_ub_drv *drv = ctx->drv;
	u64 *values = (u64 *)&stats;
	u32 index;

	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	for (index = 0; index < 17; index++)
		values[index] = readq(drv->obmm_async_load_mmio +
				      OBMM_ASYNC_LOAD_REG_STATS_BASE + index * 8);
	stats.direct_upcalls = readq(drv->obmm_async_load_mmio +
		OBMM_ASYNC_LOAD_REG_OBSERVABILITY_BASE + 16 * 8);
	stats.fail_stop = !!(readq(drv->obmm_async_load_mmio + OBMM_ASYNC_LOAD_REG_STATUS) &
			     OBMM_ASYNC_LOAD_STATUS_FAIL_STOP);
	return copy_to_user((void __user *)arg, &stats, sizeof(stats)) ?
		-EFAULT : 0;
}

static long linqu_async_load_get_observability(struct linqu_async_load_file *ctx,
					unsigned long arg)
{
	struct obmm_async_load_observability_v3 observability = {
		.abi_version = OBMM_ASYNC_LOAD_ABI_VERSION,
	};
	struct linqu_ub_drv *drv = ctx->drv;
	u64 *values = &observability.async_load_pending_current;
	u32 index;

	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	for (index = 0; index < OBMM_ASYNC_LOAD_OBSERVABILITY_VALUES; index++)
		values[index] = readq(drv->obmm_async_load_mmio +
				      OBMM_ASYNC_LOAD_REG_OBSERVABILITY_BASE +
				      index * 8);
	return copy_to_user((void __user *)arg, &observability,
			    sizeof(observability)) ? -EFAULT : 0;
}

static long linqu_async_load_get_replay_stats(struct linqu_async_load_file *ctx,
					unsigned long arg)
{
	struct obmm_async_load_replay_stats_v1 stats = { 0 };
	struct linqu_ub_drv *drv = ctx->drv;
	u64 *values = (u64 *)&stats;
	u32 index;

	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	for (index = 0; index < 3; index++)
		values[index] = readq(drv->obmm_async_load_mmio +
				      OBMM_ASYNC_LOAD_REG_REPLAY_STATS_BASE + index * 8);
	return copy_to_user((void __user *)arg, &stats, sizeof(stats)) ?
		-EFAULT : 0;
}

static long linqu_async_load_get_kernel_task_stats(
	struct linqu_async_load_file *ctx, unsigned long arg)
{
	struct obmm_async_load_kernel_task_stats_v1 stats;

	if (!linqu_async_load_owner(ctx))
		return -EPERM;
	stats = ctx->kernel_stats;
	return copy_to_user((void __user *)arg, &stats, sizeof(stats)) ?
		-EFAULT : 0;
}

static long linqu_async_load_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	struct linqu_async_load_file *ctx = file->private_data;
	long ret;

	mutex_lock(&ctx->lock);
	switch (cmd) {
	case OBMM_ASYNC_LOAD_IOCTL_QUERY_CAPS:
		ret = linqu_async_load_query_caps(ctx, arg);
		break;
	case OBMM_ASYNC_LOAD_IOCTL_REGISTER_MAP:
		ret = linqu_async_load_register_map(ctx, arg);
		break;
	case OBMM_ASYNC_LOAD_IOCTL_UNREGISTER_MAP:
		ret = linqu_async_load_unregister_map(ctx, arg);
		break;
	case OBMM_ASYNC_LOAD_IOCTL_START:
		ret = linqu_async_load_start(ctx, arg);
		break;
	case OBMM_ASYNC_LOAD_IOCTL_STOP:
		ret = linqu_async_load_stop(ctx);
		break;
	case OBMM_ASYNC_LOAD_IOCTL_GET_STATS:
		ret = linqu_async_load_get_stats(ctx, arg);
		break;
	case OBMM_ASYNC_LOAD_IOCTL_GET_OBSERVABILITY:
		ret = linqu_async_load_get_observability(ctx, arg);
		break;
	case OBMM_ASYNC_LOAD_IOCTL_GET_REPLAY_STATS:
		ret = linqu_async_load_get_replay_stats(ctx, arg);
		break;
	case OBMM_ASYNC_LOAD_IOCTL_GET_KERNEL_TASK_STATS:
		ret = linqu_async_load_get_kernel_task_stats(ctx, arg);
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

static const struct file_operations linqu_async_load_fops = {
	.owner = THIS_MODULE,
	.open = linqu_async_load_open,
	.release = linqu_async_load_release,
	.mmap = linqu_async_load_mmap,
	.unlocked_ioctl = linqu_async_load_ioctl,
	.llseek = no_llseek,
};

static irqreturn_t linqu_ub_irq_thread(int irq, void *data)
{
	struct linqu_ub_drv *drv = data;
	u64 irq_status;
	u64 async_irq_status;
	u64 async_load_irq_status;
	int drain_ret = 0;

	irq_status = readq(drv->ep_mmio + LINQU_UB_REG_IRQ_STATUS);
	async_irq_status = readq(drv->obmm_async_mmio +
				 OBMM_ASYNC_REG_IRQ_STATUS);
	async_load_irq_status = readq(drv->obmm_async_load_mmio +
				      OBMM_ASYNC_LOAD_REG_IRQ_STATUS);
	if (!irq_status && !async_irq_status && !async_load_irq_status)
		return IRQ_HANDLED;

	if (async_load_irq_status) {
		struct linqu_async_load_file *ctx;

		mutex_lock(&drv->queue_lock);
		ctx = drv->active_async_load_file;
		if (ctx && ctx->started && ctx->kernel_task_mode) {
			mutex_lock(&ctx->lock);
			drain_ret =
				linqu_async_load_drain_kernel_events_locked(ctx);
			mutex_unlock(&ctx->lock);
		}
		mutex_unlock(&drv->queue_lock);
	}

	if (irq_status)
		writeq(irq_status, drv->ep_mmio + LINQU_UB_REG_IRQ_ACK);
	if (async_irq_status)
		writeq(async_irq_status, drv->obmm_async_mmio +
		       OBMM_ASYNC_REG_IRQ_ACK);
	if (async_load_irq_status)
		writeq(async_load_irq_status, drv->obmm_async_load_mmio +
		       OBMM_ASYNC_LOAD_REG_IRQ_ACK);

	mutex_lock(&drv->lock);
	drv->irq_count += 1;
	drv->last_irq_status = irq_status | (async_irq_status << 32) |
		(async_load_irq_status ? BIT_ULL(63) : 0);
	drv->event_pending = true;
	mutex_unlock(&drv->lock);
	if (!async_load_irq_status || remote_load_event_log)
		dev_info(drv->dev,
			 "irq handled status=0x%llx async=0x%llx remote-load=0x%llx count=%llu drain=%d\n",
			 irq_status, async_irq_status, async_load_irq_status,
			 drv->irq_count, drain_ret);

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

	cdev_init(&drv->async_load_cdev, &linqu_async_load_fops);
	ret = cdev_add(&drv->async_load_cdev, drv->devt + 1, 1);
	if (ret)
		goto err_cdev;

	drv->class = class_create("linqu-ub");
	if (IS_ERR(drv->class)) {
		ret = PTR_ERR(drv->class);
		goto err_async_load_cdev;
	}

	if (IS_ERR(device_create(drv->class, drv->dev, drv->devt, NULL, "linqu-ub0"))) {
		ret = -EINVAL;
		goto err_class;
	}
	if (IS_ERR(device_create(drv->class, drv->dev, drv->devt + 1,
				 NULL, "linqu-async-load0"))) {
		ret = -EINVAL;
		goto err_ub_device;
	}

	return 0;

err_ub_device:
	device_destroy(drv->class, drv->devt);
err_class:
	class_destroy(drv->class);
err_async_load_cdev:
	cdev_del(&drv->async_load_cdev);
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
	cdev_del(&drv->async_load_cdev);
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
	drv->obmm_async_load_mmio = (u8 __iomem *)drv->mmio +
		LINQU_UB_OBMM_ASYNC_LOAD_OFFSET;

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
	ret = arm64_register_remote_load_fault_handler(
		&linqu_remote_load_fault_ops);
	if (ret) {
		linqu_ub_chrdev_cleanup(drv);
		return ret;
	}
	WRITE_ONCE(linqu_remote_load_drv, drv);

	platform_set_drvdata(pdev, drv);
	dev_info(&pdev->dev, "linqu-ub driver bound irq=%d mmio=%p ep_mmio=%p\n",
		 irq, drv->mmio, drv->ep_mmio);
	return 0;
}

static void linqu_ub_remove(struct platform_device *pdev)
{
	struct linqu_ub_drv *drv = platform_get_drvdata(pdev);

	WRITE_ONCE(linqu_remote_load_drv, NULL);
	arm64_unregister_remote_load_fault_handler(
		&linqu_remote_load_fault_ops);
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
