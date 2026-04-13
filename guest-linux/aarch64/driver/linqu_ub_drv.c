#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define LINQU_UB_REG_IRQ_STATUS 0x078
#define LINQU_UB_REG_IRQ_ACK    0x080
#define LINQU_UB_ENDPOINT1_OFFSET 0x1000

struct linqu_ub_irq_snapshot {
	u64 irq_count;
	u64 irq_status;
};

struct linqu_ub_drv {
	struct device *dev;
	void __iomem *mmio;
	u8 __iomem *ep_mmio;
	int irq;
	wait_queue_head_t waitq;
	struct mutex lock;
	struct cdev cdev;
	dev_t devt;
	struct class *class;
	bool event_pending;
	u64 irq_count;
	u64 last_irq_status;
};

static ssize_t linqu_ub_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct linqu_ub_drv *drv = file->private_data;
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
	struct linqu_ub_drv *drv = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &drv->waitq, wait);
	if (READ_ONCE(drv->event_pending))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static int linqu_ub_open(struct inode *inode, struct file *file)
{
	struct linqu_ub_drv *drv = container_of(inode->i_cdev, struct linqu_ub_drv, cdev);

	file->private_data = drv;
	return 0;
}

static const struct file_operations linqu_ub_fops = {
	.owner = THIS_MODULE,
	.open = linqu_ub_open,
	.read = linqu_ub_read,
	.poll = linqu_ub_poll,
	.llseek = no_llseek,
};

static irqreturn_t linqu_ub_irq_thread(int irq, void *data)
{
	struct linqu_ub_drv *drv = data;
	u64 irq_status;

	irq_status = readq(drv->ep_mmio + LINQU_UB_REG_IRQ_STATUS);
	if (!irq_status)
		return IRQ_HANDLED;

	writeq(irq_status, drv->ep_mmio + LINQU_UB_REG_IRQ_ACK);

	mutex_lock(&drv->lock);
	drv->irq_count += 1;
	drv->last_irq_status = irq_status;
	drv->event_pending = true;
	mutex_unlock(&drv->lock);
	dev_info(drv->dev, "irq handled status=0x%llx count=%llu\n",
		 irq_status, drv->irq_count);

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

	ret = alloc_chrdev_region(&drv->devt, 0, 1, "linqu-ub");
	if (ret)
		return ret;

	cdev_init(&drv->cdev, &linqu_ub_fops);
	ret = cdev_add(&drv->cdev, drv->devt, 1);
	if (ret)
		goto err_chrdev;

	drv->class = class_create("linqu-ub");
	if (IS_ERR(drv->class)) {
		ret = PTR_ERR(drv->class);
		goto err_cdev;
	}

	if (IS_ERR(device_create(drv->class, drv->dev, drv->devt, NULL, "linqu-ub0"))) {
		ret = -EINVAL;
		goto err_class;
	}

	return 0;

err_class:
	class_destroy(drv->class);
err_cdev:
	cdev_del(&drv->cdev);
err_chrdev:
	unregister_chrdev_region(drv->devt, 1);
	return ret;
}

static void linqu_ub_chrdev_cleanup(struct linqu_ub_drv *drv)
{
	if (drv->class) {
		device_destroy(drv->class, drv->devt);
		class_destroy(drv->class);
	}
	cdev_del(&drv->cdev);
	unregister_chrdev_region(drv->devt, 1);
}

static int linqu_ub_probe(struct platform_device *pdev)
{
	struct linqu_ub_drv *drv;
	int irq;
	int ret;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->dev = &pdev->dev;
	mutex_init(&drv->lock);
	init_waitqueue_head(&drv->waitq);

	drv->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(drv->mmio))
		return PTR_ERR(drv->mmio);
	drv->ep_mmio = (u8 __iomem *)drv->mmio + LINQU_UB_ENDPOINT1_OFFSET;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	drv->irq = irq;

	ret = devm_request_threaded_irq(&pdev->dev, irq,
					linqu_ub_irq_primary, linqu_ub_irq_thread,
					IRQF_ONESHOT, dev_name(&pdev->dev), drv);
	if (ret)
		return ret;

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
