/*
 * Coherent per-device memory handling.
 * Borrowed from i386
 */
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/dma-attrs.h>
#include <linux/dma-contiguous.h>
#include <linux/debugfs.h>

struct heap_info {
	const char *name;
	/* number of devices pointed by devs */
	unsigned int num_devs;
	/* indices for start and end device for resize support */
	unsigned int dev_start;
	unsigned int dev_end;
	/* devs to manage cma/coherent memory allocs, if resize allowed */
	struct device *devs;
	/* device to allocate memory from cma */
	struct device *cma_dev;
	/* flag that indicates whether heap can resize :shrink/grow */
	bool can_resize;
	/* lock to synchronise heap resizing */
	struct mutex resize_lock;
	/* CMA chunk size if resize supported */
	size_t cma_chunk_size;
	/* heap base */
	phys_addr_t base;
	/* heap size */
	size_t len;
	phys_addr_t cma_base;
	size_t cma_len;
	struct dentry *dma_debug_root;
	void (*update_resize_cfg)(phys_addr_t , size_t);
};

#define DMA_RESERVED_COUNT 8
static struct dma_coherent_reserved {
	const struct device *dev;
} dma_coherent_reserved[DMA_RESERVED_COUNT];

static unsigned dma_coherent_reserved_count;

#ifdef CONFIG_ARM_DMA_IOMMU_ALIGNMENT
#define DMA_BUF_ALIGNMENT CONFIG_ARM_DMA_IOMMU_ALIGNMENT
#else
#define DMA_BUF_ALIGNMENT 8
#endif

struct dma_coherent_mem {
	void		*virt_base;
	dma_addr_t	device_base;
	phys_addr_t	pfn_base;
	int		size;
	int		flags;
	unsigned long	*bitmap;
};

static bool dma_is_coherent_dev(struct device *dev)
{
	int i;
	struct dma_coherent_reserved *r = dma_coherent_reserved;

	for (i = 0; i < dma_coherent_reserved_count; i++, r++) {
		if (dev == r->dev)
			return true;
	}
	return false;
}
static void dma_debugfs_init(struct device *dev, struct heap_info *heap)
{
	if (!heap->dma_debug_root) {
		heap->dma_debug_root = debugfs_create_dir(dev_name(dev), NULL);
		if (IS_ERR_OR_NULL(heap->dma_debug_root)) {
			dev_err(dev, "couldn't create debug files\n");
			return;
		}
	}

	debugfs_create_x32("base", S_IRUGO,
		heap->dma_debug_root, (u32 *)&heap->base);
	debugfs_create_x32("size", S_IRUGO,
		heap->dma_debug_root, (u32 *)&heap->len);
	if (heap->can_resize) {
		debugfs_create_x32("cma_base", S_IRUGO,
			heap->dma_debug_root, (u32 *)&heap->cma_base);
		debugfs_create_x32("cma_size", S_IRUGO,
			heap->dma_debug_root, (u32 *)&heap->cma_len);
		debugfs_create_x32("cma_chunk_size", S_IRUGO,
			heap->dma_debug_root, (u32 *)&heap->cma_chunk_size);
		debugfs_create_x32("num_cma_chunks", S_IRUGO,
			heap->dma_debug_root, (u32 *)&heap->num_devs);
	}
}

static struct device *dma_create_dma_devs(const char *name, int num_devs)
{
	int idx = 0;
	struct device *devs;

	devs = kzalloc(num_devs * sizeof(*devs), GFP_KERNEL);
	if (!devs)
		return NULL;

	for (idx = 0; idx < num_devs; idx++)
		dev_set_name(&devs[idx], "%s-heap-%d", name, idx);

	return devs;
}

int dma_declare_coherent_memory(struct device *dev, dma_addr_t bus_addr,
				dma_addr_t device_addr, size_t size, int flags)
{
	void __iomem *mem_base = NULL;
	int pages = size >> PAGE_SHIFT;
	int bitmap_size = BITS_TO_LONGS(pages) * sizeof(long);

	if ((flags &
		(DMA_MEMORY_MAP | DMA_MEMORY_IO | DMA_MEMORY_NOMAP)) == 0)
		goto out;
	if (!size)
		goto out;
	if (dev->dma_mem)
		goto out;

	/* FIXME: this routine just ignores DMA_MEMORY_INCLUDES_CHILDREN */

	dev->dma_mem = kzalloc(sizeof(struct dma_coherent_mem), GFP_KERNEL);
	if (!dev->dma_mem)
		goto out;
	dev->dma_mem->bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!dev->dma_mem->bitmap)
		goto free1_out;

	if (flags & DMA_MEMORY_NOMAP)
		goto skip_mapping;

	mem_base = ioremap(bus_addr, size);
	if (!mem_base)
		goto out;
	dev->dma_mem->virt_base = mem_base;

skip_mapping:
	dev->dma_mem->device_base = device_addr;
	dev->dma_mem->pfn_base = PFN_DOWN(bus_addr);
	dev->dma_mem->size = pages;
	dev->dma_mem->flags = flags;

	if (flags & DMA_MEMORY_MAP)
		return DMA_MEMORY_MAP;

	if (flags & DMA_MEMORY_NOMAP)
		return DMA_MEMORY_NOMAP;

	return DMA_MEMORY_IO;

 free1_out:
	kfree(dev->dma_mem);
 out:
	if (mem_base)
		iounmap(mem_base);
	return 0;
}
EXPORT_SYMBOL(dma_declare_coherent_memory);

static int declare_coherent_heap(struct device *dev, phys_addr_t base,
					size_t size)
{
	int err;

	BUG_ON(dev->dma_mem);
	dma_set_coherent_mask(dev,  DMA_BIT_MASK(64));
	err = dma_declare_coherent_memory(dev, 0,
			base, size,
			DMA_MEMORY_NOMAP);
	if (err & DMA_MEMORY_NOMAP) {
		dev_dbg(dev, "dma coherent mem base (%pa) size (%zu)\n",
			&base, size);
		return 0;
	}
	dev_err(dev, "failed to declare dma coherent_mem (%pa)\n",
		&base);
	return -ENOMEM;
}

int dma_declare_coherent_resizable_cma_memory(struct device *dev,
					struct dma_declare_info *dma_info)
{
	int err = 0;
	struct heap_info *heap_info = NULL;
	struct dma_coherent_reserved *r =
			&dma_coherent_reserved[dma_coherent_reserved_count];

	if (dma_coherent_reserved_count == ARRAY_SIZE(dma_coherent_reserved)) {
		pr_err("Not enough slots for DMA Coherent reserved regions!\n");
		return -ENOSPC;
	}

	if (!dma_info || !dev)
		return -EINVAL;

	heap_info = kzalloc(sizeof(*heap_info), GFP_KERNEL);
	if (!heap_info)
		return -ENOMEM;

	heap_info->name = dma_info->name;
	dev_set_name(dev, "dma-%s", heap_info->name);

	if (!dma_info->resize) {
		/* No Resize support */
		err =  declare_coherent_heap(dev, dma_info->base,
						dma_info->size);
		if (err)
			goto fail;

		heap_info->base = dma_info->base;
		heap_info->len = dma_info->size;
		heap_info->can_resize = false;
	} else {
		/* Resize Heap memory */
#ifdef CONFIG_CMA
		struct dma_contiguous_stats stats;
		if (!dma_info->cma_dev) {
			err = -EINVAL;
			goto fail;
		}
		heap_info->cma_dev = dma_info->cma_dev;
		dma_get_contiguous_stats(heap_info->cma_dev, &stats);

		heap_info->cma_chunk_size = dma_info->size;
		heap_info->cma_base = stats.base;
		heap_info->cma_len = stats.size;
		heap_info->can_resize = true;
		dev_set_name(heap_info->cma_dev, "cma-%s-heap", dma_info->name);

		if (heap_info->cma_len < heap_info->cma_chunk_size) {
			dev_err(dev, "error cma_len < cma_chunk_size");
			err = -EINVAL;
			goto fail;
		}
		if (heap_info->cma_len % heap_info->cma_chunk_size) {
			dev_err(dev,
				"size is not multiple of cma_chunk_size(%zu)\n"
				"size truncated from %zu to %zu\n",
				heap_info->cma_chunk_size, heap_info->cma_len,
				round_down(heap_info->cma_len,
					heap_info->cma_chunk_size));
				heap_info->cma_len = round_down(
						heap_info->cma_len,
						heap_info->cma_chunk_size);
		}

		mutex_init(&heap_info->resize_lock);
		heap_info->num_devs = div_u64(heap_info->cma_len,
					heap_info->cma_chunk_size);
		heap_info->devs = dma_create_dma_devs(dma_info->name,
					heap_info->num_devs);
		if (!heap_info->devs) {
			dev_err(dev, "failed to alloc devices\n");
			err = -ENOMEM;
			goto fail;
		}
		if (dma_info->notifier.ops)
			heap_info->update_resize_cfg =
				dma_info->notifier.ops->resize;
#else
		err = -EINVAL;
		goto fail;
#endif
	}

	r->dev = dev;
	dma_coherent_reserved_count++;

	dev_set_drvdata(dev, heap_info);
	dma_debugfs_init(dev, heap_info);
	return 0;
fail:
	kfree(heap_info);
	return err;
}
EXPORT_SYMBOL(dma_declare_coherent_resizable_cma_memory);

static phys_addr_t alloc_from_contiguous_heap(
				struct heap_info *h,
				phys_addr_t base, size_t len)
{
	size_t count;
	struct page *page;
	unsigned long order;

	order = get_order(len);
	count = PAGE_ALIGN(len) >> PAGE_SHIFT;
	page = dma_alloc_from_contiguous(h->cma_dev, count, order);
	if (!page) {
		dev_err(h->cma_dev, "failed to alloc dma contiguous mem\n");
		goto dma_alloc_err;
	}
	base = page_to_phys(page);
	dev_dbg(h->cma_dev, "dma contiguous mem base (0x%pa) size (%zu)\n",
		&base, len);
	BUG_ON(base < h->cma_base ||
		base - h->cma_base + len > h->cma_len);
	return base;

dma_alloc_err:
	return DMA_ERROR_CODE;
}

static void release_from_contiguous_heap(
				struct heap_info *h,
				phys_addr_t base, size_t len)
{
	struct page *page = phys_to_page(base);
	size_t count = PAGE_ALIGN(len) >> PAGE_SHIFT;

	dma_release_from_contiguous(h->cma_dev, page, count);
}

static int heap_resize_locked(struct heap_info *h)
{
	int idx;
	phys_addr_t base;
	bool at_bottom = false;

	base = alloc_from_contiguous_heap(h, 0, h->cma_chunk_size);
	if (dma_mapping_error(h->cma_dev, base))
		return 1;

	idx = div_u64(base - h->cma_base, h->cma_chunk_size);
	if (!h->len || base == h->base - h->cma_chunk_size)
		/* new chunk can be added at bottom. */
		at_bottom = true;
	else if (base != h->base + h->len)
		/* new chunk can't be added at top */
		goto fail_non_contig;

	BUG_ON(h->dev_start - 1 != idx && h->dev_end + 1 != idx && h->len);
	dev_dbg(&h->devs[idx],
		"Resize VPR base from=0x%pa to=0x%pa, len from=%zu to=%zu\n",
		&h->base, &base, h->len, h->len + h->cma_chunk_size);

	if (declare_coherent_heap(&h->devs[idx], base, h->cma_chunk_size))
		goto fail_declare;
	dev_dbg(&h->devs[idx],
		"Resize VPR base from=0x%pa to=0x%pa, len from=%zu to=%zu\n",
		&h->base, &base, h->len, h->len + h->cma_chunk_size);
	if (at_bottom) {
		h->base = base;
		h->dev_start = idx;
		if (!h->len)
			h->dev_end = h->dev_start;
	} else {
		h->dev_end = idx;
	}
	h->len += h->cma_chunk_size;
	/* Handle VPR configuration updates*/
	if (h->update_resize_cfg)
		h->update_resize_cfg(h->base, h->len);
	return 0;

fail_non_contig:
	dev_dbg(&h->devs[idx], "Found Non-Contiguous block(0x%pa)\n", &base);
fail_declare:
	release_from_contiguous_heap(h, base, h->cma_chunk_size);
	return 1;
}

static int dma_alloc_from_coherent_dev(struct device *dev, ssize_t size,
				       dma_addr_t *dma_handle, void **ret,
				       struct dma_attrs *attrs)
{
	struct dma_coherent_mem *mem;
	int order = get_order(size);
	int pageno;
	unsigned int count;
	unsigned long align;

	if (!dev)
		return 0;
	mem = dev->dma_mem;
	if (!mem)
		return 0;

	*ret = NULL;

	if (unlikely(size > (mem->size << PAGE_SHIFT)))
		goto err;

	if (order > DMA_BUF_ALIGNMENT)
		align = (1 << DMA_BUF_ALIGNMENT) - 1;
	else
		align = (1 << order) - 1;

	if (dma_get_attr(DMA_ATTR_ALLOC_EXACT_SIZE, attrs))
		count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	else
		count = 1 << order;

	pageno = bitmap_find_next_zero_area(mem->bitmap, mem->size,
			0, count, align);

	if (pageno >= mem->size)
		goto err;

	bitmap_set(mem->bitmap, pageno, count);

	/*
	 * Memory was found in the per-device area.
	 */
	*dma_handle = mem->device_base + (pageno << PAGE_SHIFT);
	if (!(mem->flags & DMA_MEMORY_NOMAP)) {
		*ret = mem->virt_base + (pageno << PAGE_SHIFT);
		memset(*ret, 0, size);
	}

	return 1;

err:
	/*
	 * In the case where the allocation can not be satisfied from the
	 * per-device area, try to fall back to generic memory if the
	 * constraints allow it.
	 */
	return mem->flags & DMA_MEMORY_EXCLUSIVE;
}

static int dma_alloc_from_coherent_heap_dev(struct device *dev, size_t len,
					dma_addr_t *dma_handle, void **ret,
					struct dma_attrs *attrs)
{
	int err = 0;
	int idx = 0;
	phys_addr_t pa;
	struct heap_info *h = NULL;
	struct device *d;

	if (!dma_is_coherent_dev(dev))
		return 0;

	h = dev_get_drvdata(dev);
	if (!h)
		return 0;

	if (!h->can_resize)
		return 0;

	dma_set_attr(DMA_ATTR_ALLOC_EXACT_SIZE, attrs);

	mutex_lock(&h->resize_lock);
retry_alloc:
	/* Try allocation from already existing CMA chunks */
	for (idx = h->dev_start; idx <= h->dev_end && h->len; idx++) {
		d = &h->devs[idx];
		if (d->dma_mem) {
			err = dma_alloc_from_coherent_dev(d, len, &pa,
							ret, attrs);
			if (err) {
				dev_dbg(d, "Allocated addr %pa len %zu\n",
					&pa, len);
				*dma_handle = pa;
				goto out;
			}
		}
	}

	/* Check if a heap can be expanded */
	if (h->dev_end - h->dev_start + 1 < h->num_devs || !h->len) {
		if (!heap_resize_locked(h))
			goto retry_alloc;
	}
out:
	mutex_unlock(&h->resize_lock);
	return err;
}

static int dma_release_from_coherent_dev(struct device *dev, size_t size,
					void *vaddr, struct dma_attrs *attrs)
{
	struct dma_coherent_mem *mem = dev ? dev->dma_mem : NULL;
	void *mem_addr;
	unsigned int count;
	unsigned int pageno;

	if (!mem)
		return 0;

	if (mem->flags & DMA_MEMORY_NOMAP)
		mem_addr =  (void *)mem->device_base;
	else
		mem_addr =  mem->virt_base;


	if (mem && vaddr >= mem_addr &&
	    vaddr - mem_addr < mem->size << PAGE_SHIFT) {

		pageno = (vaddr - mem_addr) >> PAGE_SHIFT;

		if (dma_get_attr(DMA_ATTR_ALLOC_EXACT_SIZE, attrs))
			count = PAGE_ALIGN(size) >> PAGE_SHIFT;
		else
			count = 1 << get_order(size);

		bitmap_clear(mem->bitmap, pageno, count);

		return 1;
	}
	return 0;
}

static int dma_release_from_coherent_heap_dev(struct device *dev, size_t len,
					void *base, struct dma_attrs *attrs)
{
	int idx = 0;
	int err = 0;
	int resize_err = 0;
	void *ret = NULL;
	dma_addr_t dev_base;
	struct heap_info *h = NULL;

	if (!dma_is_coherent_dev(dev))
		return 0;

	h = dev_get_drvdata(dev);
	if (!h)
		return 0;

	dma_set_attr(DMA_ATTR_ALLOC_EXACT_SIZE, attrs);

	if (!h->can_resize)
		return 0;

	mutex_lock(&h->resize_lock);

	idx = div_u64((uintptr_t)base - h->cma_base, h->cma_chunk_size);
	dev_dbg(&h->devs[idx], "dma free base (%pa) size (%zu) idx (%d)\n",
		(void *)(uintptr_t)&base, len, idx);
	err = dma_release_from_coherent_dev(&h->devs[idx], len, base, attrs);

	if (!err)
		goto out_unlock;

check_next_chunk:
	/* Check if heap can be shrinked */
	if ((idx == h->dev_start || idx == h->dev_end) && h->len) {
		/* check if entire chunk is free */
		resize_err = dma_alloc_from_coherent_dev(&h->devs[idx],
					h->cma_chunk_size,
					&dev_base, &ret, attrs);
		if (!resize_err)
			goto out_unlock;
		else {
			resize_err = dma_release_from_coherent_dev(
				&h->devs[idx],
				h->cma_chunk_size,
				(void *)dev_base, attrs);
			if (!resize_err)
				goto out_unlock;

			dma_release_declared_memory(
					&h->devs[idx]);
			BUG_ON(h->devs[idx].dma_mem != NULL);
			h->len -= h->cma_chunk_size;

			if ((idx == h->dev_start)) {
				h->base += h->cma_chunk_size;
				h->dev_start++;
				dev_dbg(&h->devs[idx],
					"Release Chunk at bottom\n");
				idx++;
			} else {
				h->dev_end--;
				dev_dbg(&h->devs[idx],
					"Release Chunk at top\n");
				idx--;
			}

			/* Handle VPR configuration updates*/
			if (h->update_resize_cfg)
				h->update_resize_cfg(h->base, h->len);
			release_from_contiguous_heap(h,
				dev_base, h->cma_chunk_size);
		}
		goto check_next_chunk;
	}
out_unlock:
	mutex_unlock(&h->resize_lock);
	return err;
}

void dma_release_declared_memory(struct device *dev)
{
	struct dma_coherent_mem *mem = dev->dma_mem;

	if (!mem)
		return;
	dev->dma_mem = NULL;

	if (!(mem->flags & DMA_MEMORY_NOMAP))
		iounmap(mem->virt_base);

	kfree(mem->bitmap);
	kfree(mem);
}
EXPORT_SYMBOL(dma_release_declared_memory);

void *dma_mark_declared_memory_occupied(struct device *dev,
					dma_addr_t device_addr, size_t size)
{
	struct dma_coherent_mem *mem = dev->dma_mem;
	int pos, err;

	size += device_addr & ~PAGE_MASK;

	if (!mem)
		return ERR_PTR(-EINVAL);

	pos = (device_addr - mem->device_base) >> PAGE_SHIFT;
	err = bitmap_allocate_region(mem->bitmap, pos, get_order(size));
	if (err != 0)
		return ERR_PTR(err);
	return mem->virt_base + (pos << PAGE_SHIFT);
}
EXPORT_SYMBOL(dma_mark_declared_memory_occupied);

/**
 * dma_alloc_from_coherent_attr() - try to allocate memory from the per-device
 * coherent area
 *
 * @dev:	device from which we allocate memory
 * @size:	size of requested memory area
 * @dma_handle:	This will be filled with the correct dma handle
 * @ret:	This pointer will be filled with the virtual address
 *		to allocated area.
 * @attrs:	DMA Attribute
 * This function should be only called from per-arch dma_alloc_coherent()
 * to support allocation from per-device coherent memory pools.
 *
 * Returns 0 if dma_alloc_coherent_attr should continue with allocating from
 * generic memory areas, or !0 if dma_alloc_coherent should return @ret.
 */
int dma_alloc_from_coherent_attr(struct device *dev, ssize_t size,
				       dma_addr_t *dma_handle, void **ret,
				       struct dma_attrs *attrs)
{
	if (!dev)
		return 0;

	if (dev->dma_mem)
		return dma_alloc_from_coherent_dev(dev, size, dma_handle, ret,
							attrs);
	else
		return dma_alloc_from_coherent_heap_dev(dev, size, dma_handle,
							ret, attrs);
}
EXPORT_SYMBOL(dma_alloc_from_coherent_attr);

/**
 * dma_release_from_coherent_attr() - try to free the memory allocated from
 * per-device coherent memory pool
 * @dev:	device from which the memory was allocated
 * @size:	size of the memory area to free
 * @vaddr:	virtual address of allocated pages
 * @attrs:	DMA Attribute
 *
 * This checks whether the memory was allocated from the per-device
 * coherent memory pool and if so, releases that memory.
 *
 * Returns 1 if we correctly released the memory, or 0 if
 * dma_release_coherent_attr() should proceed with releasing memory from
 * generic pools.
 */
int dma_release_from_coherent_attr(struct device *dev, size_t size, void *vaddr,
				struct dma_attrs *attrs)
{
	if (!dev)
		return 0;

	if (dev->dma_mem)
		return dma_release_from_coherent_dev(dev, size, vaddr, attrs);
	else
		return dma_release_from_coherent_heap_dev(dev, size, vaddr,
			attrs);
}
EXPORT_SYMBOL(dma_release_from_coherent_attr);

/**
 * dma_mmap_from_coherent() - try to mmap the memory allocated from
 * per-device coherent memory pool to userspace
 * @dev:	device from which the memory was allocated
 * @vma:	vm_area for the userspace memory
 * @vaddr:	cpu address returned by dma_alloc_from_coherent
 * @size:	size of the memory buffer allocated by dma_alloc_from_coherent
 * @ret:	result from remap_pfn_range()
 *
 * This checks whether the memory was allocated from the per-device
 * coherent memory pool and if so, maps that memory to the provided vma.
 *
 * Returns 1 if we correctly mapped the memory, or 0 if the caller should
 * proceed with mapping memory from generic pools.
 */
int dma_mmap_from_coherent(struct device *dev, struct vm_area_struct *vma,
			   void *vaddr, size_t size, int *ret)
{
	struct dma_coherent_mem *mem = dev ? dev->dma_mem : NULL;
	void *mem_addr;

	if (!mem)
		return 0;

	if (mem->flags & DMA_MEMORY_NOMAP)
		mem_addr =  (void *)mem->device_base;
	else
		mem_addr =  mem->virt_base;

	if (mem && vaddr >= mem_addr && vaddr + size <=
		   (mem_addr + (mem->size << PAGE_SHIFT))) {
		unsigned long off = vma->vm_pgoff;
		int start = (vaddr - mem_addr) >> PAGE_SHIFT;
		int user_count = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
		int count = size >> PAGE_SHIFT;

		*ret = -ENXIO;
		if (off < count && user_count <= count - off) {
			unsigned pfn = mem->pfn_base + start + off;
			*ret = remap_pfn_range(vma, vma->vm_start, pfn,
					       user_count << PAGE_SHIFT,
					       vma->vm_page_prot);
		}
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(dma_mmap_from_coherent);
