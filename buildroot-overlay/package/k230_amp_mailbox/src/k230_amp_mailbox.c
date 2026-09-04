// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define CPU2DSP_INT_EN0     0x0000
#define CPU2DSP_INT_SET0    0x0004
#define DSP2CPU_INT_EN0     0x0014
#define DSP2CPU_INT_CLEAR0  0x001c
#define MAILBOX_ENABLE      (BIT(16) | BIT(0))

struct k230_amp_mailbox {
	void __iomem *base;
	int irq;
	atomic64_t completions;
	wait_queue_head_t wait;
	struct miscdevice miscdev;
};

struct k230_amp_file {
	struct k230_amp_mailbox *mailbox;
	u64 seen;
};

static irqreturn_t k230_amp_mailbox_irq(int irq, void *data)
{
	struct k230_amp_mailbox *mailbox = data;

	iowrite32(0, mailbox->base + DSP2CPU_INT_CLEAR0);
	atomic64_inc(&mailbox->completions);
	wake_up_interruptible(&mailbox->wait);
	return IRQ_HANDLED;
}

static int k230_amp_mailbox_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct k230_amp_mailbox *mailbox =
		container_of(miscdev, struct k230_amp_mailbox, miscdev);
	struct k230_amp_file *amp_file;

	amp_file = kzalloc(sizeof(*amp_file), GFP_KERNEL);
	if (!amp_file)
		return -ENOMEM;
	amp_file->mailbox = mailbox;
	amp_file->seen = atomic64_read(&mailbox->completions);
	file->private_data = amp_file;
	return nonseekable_open(inode, file);
}

static int k230_amp_mailbox_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static ssize_t k230_amp_mailbox_read(struct file *file, char __user *buffer,
				     size_t length, loff_t *offset)
{
	struct k230_amp_file *amp_file = file->private_data;
	struct k230_amp_mailbox *mailbox = amp_file->mailbox;
	u64 completions;
	int ret;

	if (length < sizeof(completions))
		return -EINVAL;

	for (;;) {
		completions = atomic64_read(&mailbox->completions);
		if (completions != amp_file->seen)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(mailbox->wait,
			atomic64_read(&mailbox->completions) != amp_file->seen);
		if (ret)
			return ret;
	}

	amp_file->seen = completions;
	if (copy_to_user(buffer, &completions, sizeof(completions)))
		return -EFAULT;
	return sizeof(completions);
}

static ssize_t k230_amp_mailbox_write(struct file *file,
				      const char __user *buffer,
				      size_t length, loff_t *offset)
{
	struct k230_amp_file *amp_file = file->private_data;

	if (!length)
		return 0;
	/*
	 * Userspace publishes uncached shared memory before write(). Ensure those
	 * stores are globally ordered before the MMIO doorbell.
	 */
	wmb();
	iowrite32(0, amp_file->mailbox->base + CPU2DSP_INT_SET0);
	return length;
}

static __poll_t k230_amp_mailbox_poll(struct file *file, poll_table *wait)
{
	struct k230_amp_file *amp_file = file->private_data;
	struct k230_amp_mailbox *mailbox = amp_file->mailbox;

	poll_wait(file, &mailbox->wait, wait);
	if (atomic64_read(&mailbox->completions) != amp_file->seen)
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations k230_amp_mailbox_fops = {
	.owner = THIS_MODULE,
	.open = k230_amp_mailbox_open,
	.release = k230_amp_mailbox_release,
	.read = k230_amp_mailbox_read,
	.write = k230_amp_mailbox_write,
	.poll = k230_amp_mailbox_poll,
	.llseek = no_llseek,
};

static ssize_t completions_show(struct device *device,
				struct device_attribute *attribute, char *buffer)
{
	struct k230_amp_mailbox *mailbox = dev_get_drvdata(device);

	return sysfs_emit(buffer, "%lld\n",
			  atomic64_read(&mailbox->completions));
}
static DEVICE_ATTR_RO(completions);

static int k230_amp_mailbox_probe(struct platform_device *pdev)
{
	struct k230_amp_mailbox *mailbox;
	int ret;

	mailbox = devm_kzalloc(&pdev->dev, sizeof(*mailbox), GFP_KERNEL);
	if (!mailbox)
		return -ENOMEM;

	mailbox->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mailbox->base))
		return PTR_ERR(mailbox->base);
	mailbox->irq = platform_get_irq(pdev, 0);
	if (mailbox->irq < 0)
		return mailbox->irq;

	atomic64_set(&mailbox->completions, 0);
	init_waitqueue_head(&mailbox->wait);
	iowrite32(0, mailbox->base + DSP2CPU_INT_CLEAR0);

	ret = devm_request_irq(&pdev->dev, mailbox->irq,
			       k230_amp_mailbox_irq, 0,
			       dev_name(&pdev->dev), mailbox);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request mailbox IRQ\n");

	iowrite32(MAILBOX_ENABLE, mailbox->base + CPU2DSP_INT_EN0);
	iowrite32(MAILBOX_ENABLE, mailbox->base + DSP2CPU_INT_EN0);

	mailbox->miscdev.minor = MISC_DYNAMIC_MINOR;
	mailbox->miscdev.name = "k230-amp-mailbox";
	mailbox->miscdev.fops = &k230_amp_mailbox_fops;
	mailbox->miscdev.parent = &pdev->dev;
	ret = misc_register(&mailbox->miscdev);
	if (ret)
		goto disable_mailbox;

	platform_set_drvdata(pdev, mailbox);
	dev_set_drvdata(mailbox->miscdev.this_device, mailbox);
	ret = device_create_file(mailbox->miscdev.this_device,
				 &dev_attr_completions);
	if (ret)
		goto deregister_misc;

	dev_info(&pdev->dev, "IRQ %d, /dev/%s ready\n",
		 mailbox->irq, mailbox->miscdev.name);
	return 0;

deregister_misc:
	misc_deregister(&mailbox->miscdev);
disable_mailbox:
	iowrite32(0, mailbox->base + DSP2CPU_INT_EN0);
	iowrite32(0, mailbox->base + CPU2DSP_INT_EN0);
	return ret;
}

static int k230_amp_mailbox_remove(struct platform_device *pdev)
{
	struct k230_amp_mailbox *mailbox = platform_get_drvdata(pdev);

	iowrite32(0, mailbox->base + DSP2CPU_INT_EN0);
	iowrite32(0, mailbox->base + CPU2DSP_INT_EN0);
	device_remove_file(mailbox->miscdev.this_device, &dev_attr_completions);
	misc_deregister(&mailbox->miscdev);
	return 0;
}

static const struct of_device_id k230_amp_mailbox_of_match[] = {
	{ .compatible = "canaan,k230-amp-mailbox" },
	{ }
};
MODULE_DEVICE_TABLE(of, k230_amp_mailbox_of_match);

static struct platform_driver k230_amp_mailbox_driver = {
	.probe = k230_amp_mailbox_probe,
	.remove = k230_amp_mailbox_remove,
	.driver = {
		.name = "k230-amp-mailbox",
		.of_match_table = k230_amp_mailbox_of_match,
	},
};
module_platform_driver(k230_amp_mailbox_driver);

MODULE_DESCRIPTION("K230 AMP mailbox notification driver");
MODULE_LICENSE("GPL");
