// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "k230_amp_camera_pool.h"

struct k230_amp_camera_pool;

struct k230_amp_camera_buffer {
	struct k230_amp_camera_pool *pool;
	struct dma_buf *dmabuf;
	phys_addr_t physical;
	size_t capacity;
	u32 id;
};

struct k230_amp_camera_attachment {
	struct sg_table table;
	bool mapped;
	enum dma_data_direction direction;
};

struct k230_amp_camera_pool {
	struct miscdevice miscdev;
	struct mutex lock;
	unsigned long exported_mask;
};

static struct k230_amp_camera_pool camera_pool;

static int k230_amp_camera_attach(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *attachment)
{
	struct k230_amp_camera_buffer *buffer = dmabuf->priv;
	struct k230_amp_camera_attachment *state;
	int ret;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	ret = sg_alloc_table(&state->table, 1, GFP_KERNEL);
	if (ret) {
		kfree(state);
		return ret;
	}
	sg_set_page(state->table.sgl, pfn_to_page(PHYS_PFN(buffer->physical)),
		    buffer->capacity, offset_in_page(buffer->physical));
	attachment->priv = state;
	return 0;
}

static void k230_amp_camera_detach(struct dma_buf *dmabuf,
				   struct dma_buf_attachment *attachment)
{
	struct k230_amp_camera_attachment *state = attachment->priv;

	if (state->mapped)
		dma_unmap_sgtable(attachment->dev, &state->table,
				  state->direction, DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(&state->table);
	kfree(state);
}

static struct sg_table *
k230_amp_camera_map(struct dma_buf_attachment *attachment,
		    enum dma_data_direction direction)
{
	struct k230_amp_camera_attachment *state = attachment->priv;
	int ret;

	if (state->mapped) {
		if (state->direction != direction)
			return ERR_PTR(-EBUSY);
		return &state->table;
	}
	/* The carveout has no cached Linux mapping. Avoid phys_to_virt cache
	 * maintenance on its no-map pages; VI is the sole writer while queued. */
	ret = dma_map_sgtable(attachment->dev, &state->table, direction,
			      DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		return ERR_PTR(ret);
	if (state->table.nents != 1 ||
	    sg_dma_len(state->table.sgl) < attachment->dmabuf->size) {
		dma_unmap_sgtable(attachment->dev, &state->table, direction,
				  DMA_ATTR_SKIP_CPU_SYNC);
		return ERR_PTR(-ERANGE);
	}
	state->mapped = true;
	state->direction = direction;
	return &state->table;
}

static void k230_amp_camera_unmap(struct dma_buf_attachment *attachment,
				  struct sg_table *table,
				  enum dma_data_direction direction)
{
	struct k230_amp_camera_attachment *state = attachment->priv;

	if (!state->mapped)
		return;
	dma_unmap_sgtable(attachment->dev, table, direction,
			  DMA_ATTR_SKIP_CPU_SYNC);
	state->mapped = false;
}

static int k230_amp_camera_begin_cpu_access(struct dma_buf *dmabuf,
					    enum dma_data_direction direction)
{
	/* DQBUF/device completion is the ownership token. Order every following
	 * CPU/control-plane operation after the device's completed DMA writes.
	 * Cache maintenance is intentionally absent: the pool has no cached
	 * Linux mapping and the remote invalidates its own cache before reading. */
	dma_rmb();
	return 0;
}

static int k230_amp_camera_end_cpu_access(struct dma_buf *dmabuf,
					  enum dma_data_direction direction)
{
	/* Order ownership publication before a subsequent device submission. */
	dma_wmb();
	return 0;
}

static int k230_amp_camera_mmap(struct dma_buf *dmabuf,
				struct vm_area_struct *vma)
{
	struct k230_amp_camera_buffer *buffer = dmabuf->priv;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long length = vma->vm_end - vma->vm_start;
	phys_addr_t physical;

	if (offset > buffer->capacity || length > buffer->capacity - offset)
		return -EINVAL;
	physical = buffer->physical + offset;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, PHYS_PFN(physical), length,
			       vma->vm_page_prot);
}

static void k230_amp_camera_release(struct dma_buf *dmabuf)
{
	struct k230_amp_camera_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->pool->lock);
	clear_bit(buffer->id, &buffer->pool->exported_mask);
	mutex_unlock(&buffer->pool->lock);
	kfree(buffer);
}

static const struct dma_buf_ops k230_amp_camera_dmabuf_ops = {
	.attach = k230_amp_camera_attach,
	.detach = k230_amp_camera_detach,
	.map_dma_buf = k230_amp_camera_map,
	.unmap_dma_buf = k230_amp_camera_unmap,
	.begin_cpu_access = k230_amp_camera_begin_cpu_access,
	.end_cpu_access = k230_amp_camera_end_cpu_access,
	.mmap = k230_amp_camera_mmap,
	.release = k230_amp_camera_release,
};

static long k230_amp_camera_ioctl(struct file *file, unsigned int command,
				  unsigned long argument)
{
	void __user *user = (void __user *)argument;

	if (command == K230_AMP_CAMERA_IOC_GET_INFO) {
		const struct k230_amp_camera_pool_info info = {
			.version = K230_AMP_CAMERA_POOL_ABI_VERSION,
			.buffer_count = K230_AMP_CAMERA_BUFFER_COUNT,
			.buffer_size = K230_AMP_CAMERA_BUFFER_SIZE,
			.physical_base = K230_AMP_CAMERA_POOL_BASE,
			.pool_size = K230_AMP_CAMERA_POOL_SIZE,
		};

		return copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
	}
	if (command == K230_AMP_CAMERA_IOC_GET_BUFFER) {
		struct k230_amp_camera_pool_buffer request;
		struct k230_amp_camera_buffer *buffer;
		DEFINE_DMA_BUF_EXPORT_INFO(export_info);
		struct dma_buf *dmabuf;
		int fd;

		if (copy_from_user(&request, user, sizeof(request)))
			return -EFAULT;
		if (request.id >= K230_AMP_CAMERA_BUFFER_COUNT || request.flags)
			return -EINVAL;
		mutex_lock(&camera_pool.lock);
		if (test_bit(request.id, &camera_pool.exported_mask)) {
			mutex_unlock(&camera_pool.lock);
			return -EBUSY;
		}
		buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
		if (!buffer) {
			mutex_unlock(&camera_pool.lock);
			return -ENOMEM;
		}
		buffer->pool = &camera_pool;
		buffer->id = request.id;
		buffer->physical = K230_AMP_CAMERA_POOL_BASE +
				   request.id * K230_AMP_CAMERA_BUFFER_SIZE;
		buffer->capacity = K230_AMP_CAMERA_BUFFER_SIZE;
		export_info.exp_name = "k230-amp-camera-pool";
		export_info.owner = THIS_MODULE;
		export_info.ops = &k230_amp_camera_dmabuf_ops;
		export_info.size = buffer->capacity;
		export_info.flags = O_RDWR;
		export_info.priv = buffer;
		dmabuf = dma_buf_export(&export_info);
		if (IS_ERR(dmabuf)) {
			int ret = PTR_ERR(dmabuf);

			kfree(buffer);
			mutex_unlock(&camera_pool.lock);
			return ret;
		}
		set_bit(request.id, &camera_pool.exported_mask);
		mutex_unlock(&camera_pool.lock);

		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0)
			goto put_dmabuf;
		request.physical = buffer->physical;
		request.capacity = buffer->capacity;
		request.fd = fd;
		request.reserved = 0;
		if (copy_to_user(user, &request, sizeof(request))) {
			put_unused_fd(fd);
			fd = -EFAULT;
			goto put_dmabuf;
		}
		fd_install(fd, dmabuf->file);
		return 0;

put_dmabuf:
		dma_buf_put(dmabuf);
		return fd;
	}
	return -ENOTTY;
}

static const struct file_operations k230_amp_camera_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = k230_amp_camera_ioctl,
	.llseek = no_llseek,
};

static int __init k230_amp_camera_pool_init(void)
{
	unsigned long first_pfn = PHYS_PFN(K230_AMP_CAMERA_POOL_BASE);
	unsigned long page_count = K230_AMP_CAMERA_POOL_SIZE >> PAGE_SHIFT;
	unsigned long page;
	int ret;

	if (!IS_ALIGNED(K230_AMP_CAMERA_POOL_BASE, PAGE_SIZE) ||
	    !IS_ALIGNED(K230_AMP_CAMERA_BUFFER_SIZE, PAGE_SIZE))
		return -EINVAL;
	for (page = 0; page < page_count; ++page) {
		if (!pfn_valid(first_pfn + page)) {
			pr_err("k230_amp_camera_pool: invalid reserved PFN 0x%lx\n",
			       first_pfn + page);
			return -ENXIO;
		}
		if (!PageReserved(pfn_to_page(first_pfn + page))) {
			pr_err("k230_amp_camera_pool: PFN 0x%lx is not reserved\n",
			       first_pfn + page);
			return -EBUSY;
		}
	}
	mutex_init(&camera_pool.lock);
	camera_pool.miscdev.minor = MISC_DYNAMIC_MINOR;
	camera_pool.miscdev.name = "k230-amp-camera-pool";
	camera_pool.miscdev.fops = &k230_amp_camera_fops;
	camera_pool.miscdev.mode = 0600;
	ret = misc_register(&camera_pool.miscdev);
	if (ret)
		return ret;
	pr_info("k230_amp_camera_pool: %u x %llu-byte DMA-BUF slots at 0x%llx\n",
		K230_AMP_CAMERA_BUFFER_COUNT, K230_AMP_CAMERA_BUFFER_SIZE,
		K230_AMP_CAMERA_POOL_BASE);
	return 0;
}

static void __exit k230_amp_camera_pool_exit(void)
{
	misc_deregister(&camera_pool.miscdev);
}

module_init(k230_amp_camera_pool_init);
module_exit(k230_amp_camera_pool_exit);

MODULE_IMPORT_NS(DMA_BUF);
MODULE_DESCRIPTION("K230 AMP fixed camera DMA-BUF pool");
MODULE_LICENSE("GPL");
