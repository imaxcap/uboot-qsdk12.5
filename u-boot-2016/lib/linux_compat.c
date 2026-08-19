
#include <common.h>
#include <linux/compat.h>

struct p_current cur = {
	.pid = 1,
};
__maybe_unused struct p_current *current = &cur;

unsigned long copy_from_user(void *dest, const void *src,
		     unsigned long count)
{
	memcpy((void *)dest, (void *)src, count);
	return 0;
}

void *kmalloc(size_t size, int flags)
{
	void *p;

	p = memalign(ARCH_DMA_MINALIGN, size);
	if (!p)
		return NULL;

	if (flags & __GFP_ZERO)
		memset(p, 0, size);

	return p;
}

struct kmem_cache *get_mem(int element_sz)
{
	struct kmem_cache *ret;

	/* Slab descriptors are CPU-only metadata and do not need DMA alignment. */
	ret = malloc(sizeof(struct kmem_cache));
	if (!ret)
		return NULL;

	ret->sz = element_sz;

	return ret;
}

void *kmem_cache_alloc(struct kmem_cache *obj, int flag)
{
	/*
	 * Objects served by the compatibility slab API are CPU-only metadata
	 * (UBI attach/WL entries and UBIFS inodes in this tree).  Aligning every
	 * small object to ARCH_DMA_MINALIGN wastes a significant amount of the
	 * fixed U-Boot malloc arena on large UBI devices.
	 */
	return malloc(obj->sz);
}
