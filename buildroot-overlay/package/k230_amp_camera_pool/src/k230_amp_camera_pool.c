// SPDX-License-Identifier: GPL-2.0
#include <linux/bitmap.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "amp_shared_buffer_pool.h"

#define K230_LEGACY_POOL_BASE   0x1da00000ULL
#define K230_LEGACY_POOL_SIZE   0x00c00000ULL
#define K230_LEGACY_REMOTE_BASE K230_LEGACY_POOL_BASE

struct amp_shared_buffer_pool;

struct amp_shared_buffer {
	struct amp_shared_buffer_pool *pool;
	phys_addr_t physical;
	u64 remote_token;
	u64 allocation_id;
	size_t capacity;
	unsigned long first_page;
	unsigned long page_count;
};

struct amp_shared_buffer_attachment {
	struct sg_table table;
	bool mapped;
	enum dma_data_direction direction;
};

struct amp_shared_buffer_pool {
	struct miscdevice miscdev;
	struct mutex lock;
	unsigned long *allocation_map;
	phys_addr_t physical_base;
	u64 remote_base;
	size_t pool_size;
	size_t minimum_alignment;
	unsigned long page_count;
	u64 next_allocation_id;
};

static struct amp_shared_buffer_pool shared_pool;

static unsigned long legacy_pool_base = K230_LEGACY_POOL_BASE;
module_param(legacy_pool_base, ulong, 0444);
MODULE_PARM_DESC(legacy_pool_base,
	"temporary fallback physical base when no Device Tree provider exists");

static unsigned long legacy_pool_size = K230_LEGACY_POOL_SIZE;
module_param(legacy_pool_size, ulong, 0444);
MODULE_PARM_DESC(legacy_pool_size,
	"temporary fallback pool size when no Device Tree provider exists");

static unsigned long legacy_remote_base = K230_LEGACY_REMOTE_BASE;
module_param(legacy_remote_base, ulong, 0444);
MODULE_PARM_DESC(legacy_remote_base,
	"temporary fallback remote-visible base when no Device Tree provider exists");

static int amp_shared_buffer_attach(struct dma_buf *dmabuf,
				    struct dma_buf_attachment *attachment)
{
	struct amp_shared_buffer *buffer = dmabuf->priv;
	struct amp_shared_buffer_attachment *state;
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

static void amp_shared_buffer_detach(struct dma_buf *dmabuf,
				     struct dma_buf_attachment *attachment)
{
	struct amp_shared_buffer_attachment *state = attachment->priv;

	if (state->mapped)
		dma_unmap_sgtable(attachment->dev, &state->table,
				  state->direction, DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(&state->table);
	kfree(state);
}

static struct sg_table *
amp_shared_buffer_map(struct dma_buf_attachment *attachment,
		      enum dma_data_direction direction)
{
	struct amp_shared_buffer_attachment *state = attachment->priv;
	int ret;

	if (state->mapped) {
		if (state->direction != direction)
			return ERR_PTR(-EBUSY);
		return &state->table;
	}
	/* This backend exports no-map reserved memory with no cached Linux
	 * mapping. Other cache policies belong in separate provider backends. */
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

static void amp_shared_buffer_unmap(struct dma_buf_attachment *attachment,
				    struct sg_table *table,
				    enum dma_data_direction direction)
{
	struct amp_shared_buffer_attachment *state = attachment->priv;

	if (!state->mapped)
		return;
	dma_unmap_sgtable(attachment->dev, table, direction,
			  DMA_ATTR_SKIP_CPU_SYNC);
	state->mapped = false;
}

static int amp_shared_buffer_begin_cpu_access(
	struct dma_buf *dmabuf, enum dma_data_direction direction)
{
	dma_rmb();
	return 0;
}

static int amp_shared_buffer_end_cpu_access(
	struct dma_buf *dmabuf, enum dma_data_direction direction)
{
	dma_wmb();
	return 0;
}

static int amp_shared_buffer_mmap(struct dma_buf *dmabuf,
				  struct vm_area_struct *vma)
{
	struct amp_shared_buffer *buffer = dmabuf->priv;
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

static void amp_shared_buffer_release(struct dma_buf *dmabuf)
{
	struct amp_shared_buffer *buffer = dmabuf->priv;
	struct amp_shared_buffer_pool *pool = buffer->pool;

	mutex_lock(&pool->lock);
	bitmap_clear(pool->allocation_map, buffer->first_page,
		     buffer->page_count);
	mutex_unlock(&pool->lock);
	kfree(buffer);
}

static const struct dma_buf_ops amp_shared_buffer_ops = {
	.attach = amp_shared_buffer_attach,
	.detach = amp_shared_buffer_detach,
	.map_dma_buf = amp_shared_buffer_map,
	.unmap_dma_buf = amp_shared_buffer_unmap,
	.begin_cpu_access = amp_shared_buffer_begin_cpu_access,
	.end_cpu_access = amp_shared_buffer_end_cpu_access,
	.mmap = amp_shared_buffer_mmap,
	.release = amp_shared_buffer_release,
};

static int amp_shared_buffer_allocate(
	struct amp_shared_buffer_pool *pool,
	struct amp_shared_buffer_alloc *request, struct dma_buf **result)
{
	struct amp_shared_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(export_info);
	struct dma_buf *dmabuf;
	unsigned long capacity;
	unsigned long alignment;
	unsigned long needed_pages;
	unsigned long alignment_pages;
	unsigned long first_page;
	int fd;

	if (request->flags || !request->capacity)
		return -EINVAL;
	if (request->capacity > pool->pool_size)
		return -E2BIG;
	alignment = request->alignment ? request->alignment :
		    pool->minimum_alignment;
	if (alignment < pool->minimum_alignment ||
	    !is_power_of_2(alignment) || !IS_ALIGNED(alignment, PAGE_SIZE))
		return -EINVAL;
	capacity = PAGE_ALIGN(request->capacity);
	needed_pages = capacity >> PAGE_SHIFT;
	alignment_pages = alignment >> PAGE_SHIFT;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	mutex_lock(&pool->lock);
	first_page = bitmap_find_next_zero_area_off(
		pool->allocation_map, pool->page_count, 0, needed_pages,
		alignment_pages - 1UL,
		(unsigned long)(pool->physical_base >> PAGE_SHIFT));
	if (first_page >= pool->page_count) {
		mutex_unlock(&pool->lock);
		kfree(buffer);
		return -ENOSPC;
	}
	bitmap_set(pool->allocation_map, first_page, needed_pages);
	buffer->allocation_id = ++pool->next_allocation_id;
	if (!buffer->allocation_id)
		buffer->allocation_id = ++pool->next_allocation_id;
	mutex_unlock(&pool->lock);

	buffer->pool = pool;
	buffer->first_page = first_page;
	buffer->page_count = needed_pages;
	buffer->capacity = capacity;
	buffer->physical = pool->physical_base +
			   ((phys_addr_t)first_page << PAGE_SHIFT);
	buffer->remote_token = pool->remote_base +
			       ((u64)first_page << PAGE_SHIFT);
	export_info.exp_name = "amp-shared-buffer-pool";
	export_info.owner = THIS_MODULE;
	export_info.ops = &amp_shared_buffer_ops;
	export_info.size = buffer->capacity;
	export_info.flags = O_RDWR;
	export_info.priv = buffer;
	dmabuf = dma_buf_export(&export_info);
	if (IS_ERR(dmabuf)) {
		int ret = PTR_ERR(dmabuf);

		mutex_lock(&pool->lock);
		bitmap_clear(pool->allocation_map, first_page, needed_pages);
		mutex_unlock(&pool->lock);
		kfree(buffer);
		return ret;
	}
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		return fd;
	}
	request->capacity = buffer->capacity;
	request->alignment = alignment;
	request->remote_token = buffer->remote_token;
	request->allocation_id = buffer->allocation_id;
	request->fd = fd;
	request->flags = 0;
	*result = dmabuf;
	return 0;
}

static long amp_shared_buffer_ioctl(struct file *file, unsigned int command,
				    unsigned long argument)
{
	struct amp_shared_buffer_pool *pool = &shared_pool;
	void __user *user = (void __user *)argument;

	if (command == AMP_SHARED_BUFFER_IOC_GET_INFO) {
		const struct amp_shared_buffer_pool_info info = {
			.version = AMP_SHARED_BUFFER_POOL_ABI_VERSION,
			.capabilities = AMP_SHARED_BUFFER_CAP_CONTIGUOUS |
				AMP_SHARED_BUFFER_CAP_REMOTE_LINEAR |
				AMP_SHARED_BUFFER_CAP_CPU_MMAP_WC |
				AMP_SHARED_BUFFER_CAP_NO_CPU_SYNC,
			.pool_size = pool->pool_size,
			.minimum_alignment = pool->minimum_alignment,
			.remote_base = pool->remote_base,
		};

		return copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
	}
	if (command == AMP_SHARED_BUFFER_IOC_ALLOC) {
		struct amp_shared_buffer_alloc request;
		struct dma_buf *dmabuf;
		int ret;

		if (copy_from_user(&request, user, sizeof(request)))
			return -EFAULT;
		ret = amp_shared_buffer_allocate(pool, &request, &dmabuf);
		if (ret)
			return ret;
		if (copy_to_user(user, &request, sizeof(request))) {
			put_unused_fd(request.fd);
			dma_buf_put(dmabuf);
			return -EFAULT;
		}
		fd_install(request.fd, dmabuf->file);
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations amp_shared_buffer_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = amp_shared_buffer_ioctl,
	.llseek = no_llseek,
};

static int amp_shared_buffer_configure(struct amp_shared_buffer_pool *pool)
{
	struct device_node *provider;
	struct device_node *memory;
	struct reserved_mem *reserved;
	u64 remote_base;
	u32 minimum_alignment;

	provider = of_find_compatible_node(
		NULL, NULL, "metalv,amp-shared-buffer-pool");
	if (!provider) {
		pool->physical_base = legacy_pool_base;
		pool->pool_size = legacy_pool_size;
		pool->remote_base = legacy_remote_base;
		pool->minimum_alignment = PAGE_SIZE;
		pr_warn("amp_shared_buffer_pool: using legacy module parameters; add the Device Tree provider\n");
		return 0;
	}
	memory = of_parse_phandle(provider, "memory-region", 0);
	if (!memory) {
		of_node_put(provider);
		return -EINVAL;
	}
	reserved = of_reserved_mem_lookup(memory);
	of_node_put(memory);
	if (!reserved) {
		of_node_put(provider);
		return -ENODEV;
	}
	if (of_property_read_u64(provider, "metalv,remote-base", &remote_base))
		remote_base = reserved->base;
	if (of_property_read_u32(provider, "metalv,minimum-alignment",
				 &minimum_alignment))
		minimum_alignment = PAGE_SIZE;
	if (!of_property_read_bool(provider, "metalv,no-cpu-cache")) {
		of_node_put(provider);
		pr_err("amp_shared_buffer_pool: reserved backend requires metalv,no-cpu-cache\n");
		return -EINVAL;
	}
	of_node_put(provider);
	pool->physical_base = reserved->base;
	pool->pool_size = reserved->size;
	pool->remote_base = remote_base;
	pool->minimum_alignment = minimum_alignment;
	return 0;
}

static int __init amp_shared_buffer_pool_init(void)
{
	struct amp_shared_buffer_pool *pool = &shared_pool;
	unsigned long first_pfn;
	unsigned long page;
	int ret;

	ret = amp_shared_buffer_configure(pool);
	if (ret)
		return ret;
	if (!pool->pool_size || !IS_ALIGNED(pool->physical_base, PAGE_SIZE) ||
	    !IS_ALIGNED(pool->pool_size, PAGE_SIZE) ||
	    pool->pool_size > SIZE_MAX - pool->physical_base ||
	    pool->pool_size > U64_MAX - pool->remote_base ||
	    pool->minimum_alignment < PAGE_SIZE ||
	    !is_power_of_2(pool->minimum_alignment) ||
	    !IS_ALIGNED(pool->minimum_alignment, PAGE_SIZE))
		return -EINVAL;
	pool->page_count = pool->pool_size >> PAGE_SHIFT;
	first_pfn = PHYS_PFN(pool->physical_base);
	for (page = 0; page < pool->page_count; ++page) {
		if (!pfn_valid(first_pfn + page)) {
			pr_err("amp_shared_buffer_pool: invalid reserved PFN 0x%lx\n",
			       first_pfn + page);
			return -ENXIO;
		}
		if (!PageReserved(pfn_to_page(first_pfn + page))) {
			pr_err("amp_shared_buffer_pool: PFN 0x%lx is not reserved\n",
			       first_pfn + page);
			return -EBUSY;
		}
	}
	pool->allocation_map = bitmap_zalloc(pool->page_count, GFP_KERNEL);
	if (!pool->allocation_map)
		return -ENOMEM;
	mutex_init(&pool->lock);
	pool->miscdev.minor = MISC_DYNAMIC_MINOR;
	pool->miscdev.name = "amp-shared-buffer-pool";
	pool->miscdev.fops = &amp_shared_buffer_fops;
	pool->miscdev.mode = 0600;
	ret = misc_register(&pool->miscdev);
	if (ret) {
		bitmap_free(pool->allocation_map);
		return ret;
	}
	pr_info("amp_shared_buffer_pool: %zu-byte pool at %pa, remote 0x%llx, alignment %zu\n",
		pool->pool_size, &pool->physical_base, pool->remote_base,
		pool->minimum_alignment);
	return 0;
}

static void __exit amp_shared_buffer_pool_exit(void)
{
	misc_deregister(&shared_pool.miscdev);
	bitmap_free(shared_pool.allocation_map);
}

module_init(amp_shared_buffer_pool_init);
module_exit(amp_shared_buffer_pool_exit);

MODULE_IMPORT_NS(DMA_BUF);
MODULE_DESCRIPTION("Reserved-memory AMP shared DMA-BUF pool");
MODULE_LICENSE("GPL");
