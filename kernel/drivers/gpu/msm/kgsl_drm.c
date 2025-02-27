#include "drmP.h"
#include "drm.h"
#include <linux/android_pmem.h>
#include <linux/notifier.h>

#include "kgsl.h"
#include "kgsl_device.h"
#include "kgsl_drawctxt.h"
#include "kgsl_drm.h"
#include "kgsl_mmu.h"
#include "kgsl_yamato.h"
#include "kgsl_sharedmem.h"

#define DRIVER_AUTHOR           "Qualcomm"
#define DRIVER_NAME             "kgsl"
#define DRIVER_DESC             "KGSL DRM"
#define DRIVER_DATE             "20100127"
#define DRIVER_MAJOR            2
#define DRIVER_MINOR            1
#define DRIVER_PATCHLEVEL       1
#define DRM_KGSL_GEM_FLAG_MAPPED (1 << 0)
#define ENTRY_EMPTY -1
#define ENTRY_NEEDS_CLEANUP -2
#define DRM_KGSL_NUM_FENCE_ENTRIES (DRM_KGSL_HANDLE_WAIT_ENTRIES << 2)
#define DRM_KGSL_HANDLE_WAIT_ENTRIES 5

#ifdef CONFIG_KERNEL_PMEM_SMI_REGION
#define TYPE_IS_PMEM(_t) \
  (((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_EBI) || \
   ((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_SMI) || \
   ((_t) & DRM_KGSL_GEM_TYPE_PMEM))
#else
#define TYPE_IS_PMEM(_t) \
  (((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_EBI) || \
   ((_t) & (DRM_KGSL_GEM_TYPE_PMEM | DRM_KGSL_GEM_PMEM_EBI)))
#endif

/* Returns true if the memory type is regular */

#define TYPE_IS_MEM(_t) \
  (((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_KMEM) || \
   ((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_KMEM_NOCACHE) || \
   ((_t) & DRM_KGSL_GEM_TYPE_MEM))

#define TYPE_IS_FD(_t) ((_t) & DRM_KGSL_GEM_TYPE_FD_MASK)

/* Returns true if KMEM region is uncached */

#define IS_MEM_UNCACHED(_t) \
  ((_t == DRM_KGSL_GEM_TYPE_KMEM_NOCACHE) || \
   (_t == DRM_KGSL_GEM_TYPE_KMEM) || \
   (TYPE_IS_MEM(_t) && (_t & DRM_KGSL_GEM_CACHE_WCOMBINE)))

struct drm_kgsl_gem_object_wait_list_entry {
	struct list_head list;
	int pid;
	int in_use;
	wait_queue_head_t process_wait_q;
};

struct drm_kgsl_gem_object_fence {
	int32_t fence_id;
	unsigned int num_buffers;
	int ts_valid;
	unsigned int timestamp;
	int ts_device;
	int lockpid;
	struct list_head buffers_in_fence;
};

struct drm_kgsl_gem_object_fence_list_entry {
	struct list_head list;
	int in_use;
	struct drm_gem_object *gem_obj;
};

static int32_t fence_id = 0x1;

static struct drm_kgsl_gem_object_fence
			  gem_buf_fence[DRM_KGSL_NUM_FENCE_ENTRIES];

struct drm_kgsl_gem_object {
	struct drm_gem_object *obj;
	uint32_t cpuaddr;
	uint32_t type;
	uint32_t size;
	struct kgsl_pagetable *pagetable;
	uint64_t mmap_offset;
	int bufcount;
	int flags;
	struct list_head list;
	int active;

	struct {
		uint32_t offset;
		uint32_t gpuaddr;
	} bufs[DRM_KGSL_GEM_MAX_BUFFERS];

	int bound;
	int lockpid;
	/* Put these here to avoid allocing all the time */
	struct drm_kgsl_gem_object_wait_list_entry
	wait_entries[DRM_KGSL_HANDLE_WAIT_ENTRIES];
	/* Each object can only appear in a single fence */
	struct drm_kgsl_gem_object_fence_list_entry
	fence_entries[DRM_KGSL_NUM_FENCE_ENTRIES];

	struct list_head wait_list;
};

/* This is a global list of all the memory currently mapped in the MMU */
static struct list_head kgsl_mem_list;

static void kgsl_gem_mem_flush(void *addr,
		unsigned long size, uint32_t type, int op)
{
	int flags = 0;

	switch (op) {
	case DRM_KGSL_GEM_CACHE_OP_TO_DEV:
		if (type & (DRM_KGSL_GEM_CACHE_WBACK |
			    DRM_KGSL_GEM_CACHE_WBACKWA))
			flags |= KGSL_MEMFLAGS_CACHE_CLEAN;

		break;

	case DRM_KGSL_GEM_CACHE_OP_FROM_DEV:
		if (type & (DRM_KGSL_GEM_CACHE_WBACK |
			    DRM_KGSL_GEM_CACHE_WBACKWA |
			    DRM_KGSL_GEM_CACHE_WTHROUGH))
			flags |= KGSL_MEMFLAGS_CACHE_INV;
	}

	if (!flags)
		return;

	if (TYPE_IS_PMEM(type) || type == DRM_KGSL_GEM_TYPE_FD_FBMEM) {
		flags |= KGSL_MEMFLAGS_CONPHYS;
		addr = __va(addr);
	}
	else if (TYPE_IS_MEM(type))
		flags |= KGSL_MEMFLAGS_VMALLOC_MEM;
	else
		return;

	kgsl_cache_range_op((unsigned long) addr, size, flags);
}

/* Flush all the memory mapped in the MMU */

void kgsl_gpu_mem_flush(int op)
{
	struct drm_kgsl_gem_object *entry;
	int index;

	list_for_each_entry(entry, &kgsl_mem_list, list) {
		for (index = 0;
		    entry->cpuaddr && (index < entry->bufcount); index++)
			kgsl_gem_mem_flush((void *)(entry->cpuaddr +
					    entry->bufs[index].offset),
					    entry->size, entry->type, op);
	}

	/* Takes care of WT/WC case.
	 * More useful when we go barrierless
	 */
	dmb();
}

/* TODO:
 * Add vsync wait */

static int kgsl_drm_load(struct drm_device *dev, unsigned long flags)
{
	return 0;
}

static int kgsl_drm_unload(struct drm_device *dev)
{
	return 0;
}

struct kgsl_drm_device_priv {
	struct kgsl_device *device[KGSL_DEVICE_MAX];
	struct kgsl_device_private *devpriv[KGSL_DEVICE_MAX];
};

static int kgsl_ts_notifier_cb(struct notifier_block *blk,
			       unsigned long code, void *_param);

static struct notifier_block kgsl_ts_nb[KGSL_DEVICE_MAX];

static int kgsl_drm_firstopen(struct drm_device *dev)
{
	int i;

	for (i = 0; i < KGSL_DEVICE_MAX; i++) {
		struct kgsl_device *device = kgsl_get_device(i);

		if (device == NULL)
			continue;

		kgsl_ts_nb[i].notifier_call = kgsl_ts_notifier_cb;
		kgsl_register_ts_notifier(device, &kgsl_ts_nb[i]);
	}

	return 0;
}

void kgsl_drm_lastclose(struct drm_device *dev)
{
	int i;

	for (i = 0; i < KGSL_DEVICE_MAX; i++) {
		struct kgsl_device *device = kgsl_get_device(i);
		if (device == NULL)
			continue;

		kgsl_unregister_ts_notifier(device, &kgsl_ts_nb[i]);
	}
}

void kgsl_drm_preclose(struct drm_device *dev, struct drm_file *file_priv)
{
}

static int kgsl_drm_suspend(struct drm_device *dev, pm_message_t state)
{
	return 0;
}

static int kgsl_drm_resume(struct drm_device *dev)
{
	return 0;
}

static void
kgsl_gem_free_mmap_offset(struct drm_gem_object *obj)
{
	struct drm_device *dev = obj->dev;
	struct drm_gem_mm *mm = dev->mm_private;
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	struct drm_map_list *list;

	list = &obj->map_list;
	drm_ht_remove_item(&mm->offset_hash, &list->hash);
	if (list->file_offset_node) {
		drm_mm_put_block(list->file_offset_node);
		list->file_offset_node = NULL;
	}

	kfree(list->map);
	list->map = NULL;

	priv->mmap_offset = 0;
}

static int
kgsl_gem_memory_allocated(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	return priv->cpuaddr ? 1 : 0;
}

static int
kgsl_gem_alloc_memory(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	int index;

	/* Return if the memory is already allocated */

	if (kgsl_gem_memory_allocated(obj) || TYPE_IS_FD(priv->type))
		return 0;

	if (TYPE_IS_PMEM(priv->type)) {
		int type;

		if (priv->type == DRM_KGSL_GEM_TYPE_EBI ||
		    priv->type & DRM_KGSL_GEM_PMEM_EBI)
			type = PMEM_MEMTYPE_EBI1;
		else
			type = PMEM_MEMTYPE_SMI;

		priv->cpuaddr = pmem_kalloc(obj->size * priv->bufcount,
						type | PMEM_ALIGNMENT_4K);

		if (IS_ERR((void *) priv->cpuaddr)) {
			priv->cpuaddr = 0;
			return -ENOMEM;
		}
	} else if (TYPE_IS_MEM(priv->type)) {
		priv->cpuaddr = (uint32_t) vmalloc_user(obj->size *
			priv->bufcount);

		if (priv->cpuaddr == 0) {
			return -ENOMEM;
		}
	} else
		return -EINVAL;

	for (index = 0; index < priv->bufcount; index++)
		priv->bufs[index].offset = index * obj->size;

	return 0;
}

static void
kgsl_gem_unmap(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	int index;

	if (!priv->flags & DRM_KGSL_GEM_FLAG_MAPPED)
		return;

	for (index = 0; index < DRM_KGSL_GEM_MAX_BUFFERS; index++) {
		if (!priv->bufs[index].gpuaddr)
			continue;

		kgsl_mmu_unmap(priv->pagetable,
			       priv->bufs[index].gpuaddr,
			       obj->size);

		priv->bufs[index].gpuaddr = 0;
	}

	kgsl_mmu_putpagetable(priv->pagetable);
	priv->pagetable = NULL;

	if ((priv->type == DRM_KGSL_GEM_TYPE_KMEM) ||
	    (priv->type & DRM_KGSL_GEM_CACHE_MASK))
		list_del(&priv->list);

	priv->flags &= ~DRM_KGSL_GEM_FLAG_MAPPED;
}

static void
kgsl_gem_free_memory(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	int index;

	if (!kgsl_gem_memory_allocated(obj) || TYPE_IS_FD(priv->type))
		return;

	/* invalidate cached region before releasing */
	kgsl_gem_mem_flush((void *)priv->cpuaddr, priv->size,
		priv->type, DRM_KGSL_GEM_CACHE_OP_FROM_DEV);

	kgsl_gem_unmap(obj);

	if (TYPE_IS_PMEM(priv->type))
		pmem_kfree(priv->cpuaddr);
	else if (TYPE_IS_MEM(priv->type))
		vfree((void *) priv->cpuaddr);

	priv->cpuaddr = 0;

	for (index = 0; index < DRM_KGSL_GEM_MAX_BUFFERS; index++)
		priv->bufs[index].offset = 0;
}

int
kgsl_gem_init_object(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv;
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (priv == NULL) {
		return -ENOMEM;
	}

	obj->driver_private = priv;
	priv->obj = obj;

	return 0;
}

void
kgsl_gem_free_object(struct drm_gem_object *obj)
{
	kgsl_gem_free_memory(obj);
	kgsl_gem_free_mmap_offset(obj);
	kfree(obj->driver_private);
}

static int
kgsl_gem_create_mmap_offset(struct drm_gem_object *obj)
{
	struct drm_device *dev = obj->dev;
	struct drm_gem_mm *mm = dev->mm_private;
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	struct drm_map_list *list;
	int msize;

	list = &obj->map_list;
	list->map = kzalloc(sizeof(struct drm_map_list), GFP_KERNEL);
	if (list->map == NULL) {
		return -ENOMEM;
	}

	msize = obj->size * priv->bufcount;

	list->map->type = _DRM_GEM;
	list->map->size = msize;
	list->map->handle = obj;

	/* Allocate a mmap offset */
	list->file_offset_node = drm_mm_search_free(&mm->offset_manager,
						    msize / PAGE_SIZE,
						    0, 0);

	if (!list->file_offset_node) {
		kfree(list->map);
		return -ENOMEM;
	}

	list->file_offset_node = drm_mm_get_block(list->file_offset_node,
						  msize / PAGE_SIZE, 0);

	if (!list->file_offset_node) {
		kfree(list->map);
		return -ENOMEM;
	}

	list->hash.key = list->file_offset_node->start;
	if (drm_ht_insert_item(&mm->offset_hash, &list->hash)) {
		drm_mm_put_block(list->file_offset_node);
		kfree(list->map);
		return -ENOMEM;
	}

	priv->mmap_offset = ((uint64_t) list->hash.key) << PAGE_SHIFT;

	return 0;
}

int
kgsl_gem_obj_addr(int drm_fd, int handle, unsigned long *start,
			unsigned long *len)
{
	struct file *filp;
	struct drm_device *dev;
	struct drm_file *file_priv;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = 0;

	filp = fget(drm_fd);
	if (unlikely(filp == NULL)) {
		return -EINVAL;
	}
	file_priv = filp->private_data;
	if (unlikely(file_priv == NULL)) {
		fput(filp);
		return -EINVAL;
	}
	dev = file_priv->minor->dev;
	if (unlikely(dev == NULL)) {
		fput(filp);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(dev, file_priv, handle);
	if (unlikely(obj == NULL)) {
		fput(filp);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	/* We can only use the MDP for PMEM regions */

	if (priv->cpuaddr && TYPE_IS_PMEM(priv->type)) {
		/* Return the address for the currently active buffer */
		*start = priv->cpuaddr + priv->bufs[priv->active].offset;
		/* priv->mmap_offset is used for virt addr */
		*len = obj->size;
		/* flush cached obj */
		kgsl_gem_mem_flush((void *)*start, *len, priv->type,
			DRM_KGSL_GEM_CACHE_OP_TO_DEV);
	} else {
		*start = 0;
		*len = 0;
		ret = -EINVAL;
	}

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	fput(filp);
	return ret;
}

static int
kgsl_gem_init_obj(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct drm_gem_object *obj,
		  int *handle)
{
	struct drm_kgsl_gem_object *priv;
	int ret, i;

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	priv->cpuaddr = 0;
	priv->size = obj->size;
	priv->bufcount = 1;
	priv->active = 0;
	priv->bound = 0;

	/* To preserve backwards compatability, the default memory source
	   is EBI */

	priv->type = DRM_KGSL_GEM_TYPE_PMEM | DRM_KGSL_GEM_PMEM_EBI;

	ret = drm_gem_handle_create(file_priv, obj, handle);

	drm_gem_object_handle_unreference(obj);
	INIT_LIST_HEAD(&priv->wait_list);

	for (i = 0; i < DRM_KGSL_HANDLE_WAIT_ENTRIES; i++) {
		INIT_LIST_HEAD((struct list_head *) &priv->wait_entries[i]);
		priv->wait_entries[i].pid = 0;
		init_waitqueue_head(&priv->wait_entries[i].process_wait_q);
	}

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		INIT_LIST_HEAD((struct list_head *) &priv->fence_entries[i]);
		priv->fence_entries[i].in_use = 0;
		priv->fence_entries[i].gem_obj = obj;
	}

	mutex_unlock(&dev->struct_mutex);
	return ret;
}

int
kgsl_gem_create_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file_priv)
{
	struct drm_kgsl_gem_create *create = data;
	struct drm_gem_object *obj;
	int ret, handle;
	create->size = ALIGN(create->size, 4096);
	obj = drm_gem_object_alloc(dev, create->size);

	if (obj == NULL) {
		return -ENOMEM;
	}

	ret = kgsl_gem_init_obj(dev, file_priv, obj, &handle);
	if (ret)
		return ret;

	create->handle = handle;
	return 0;
}

int
kgsl_gem_create_fd_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct drm_kgsl_gem_create_fd *args = data;
	struct file *file;
	dev_t rdev;
	struct fb_info *info;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret, put_needed, handle;

	file = fget_light(args->fd, &put_needed);

	if (file == NULL) {
		return -EBADF;
	}

	rdev = file->f_dentry->d_inode->i_rdev;

	if (MAJOR(rdev) != FB_MAJOR) {
		ret = -EBADF;
		goto error_fput;
	}

	info = registered_fb[MINOR(rdev)];

	if (info == NULL) {
		ret = -EBADF;
		goto error_fput;
	}

	obj = drm_gem_object_alloc(dev, info->fix.smem_len);

	if (obj == NULL) {
		ret = -ENOMEM;
		goto error_fput;
	}

	ret = kgsl_gem_init_obj(dev, file_priv, obj, &handle);

	if (ret)
		goto error_fput;

	mutex_lock(&dev->struct_mutex);

	priv = obj->driver_private;
	priv->cpuaddr = info->fix.smem_start;
	priv->type = DRM_KGSL_GEM_TYPE_FD_FBMEM;

	mutex_unlock(&dev->struct_mutex);
	args->handle = handle;

error_fput:
	fput_light(file, put_needed);

	return ret;
}

int
kgsl_gem_setmemtype_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_memtype *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = 0;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (TYPE_IS_FD(priv->type))
		ret = -EINVAL;
	else {
		if (TYPE_IS_PMEM(args->type) || TYPE_IS_MEM(args->type))
			priv->type = args->type;
		else
			ret = -EINVAL;
	}

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int
kgsl_gem_getmemtype_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_kgsl_gem_memtype *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	args->type = priv->type;

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return 0;
}

int
kgsl_gem_unbind_gpu_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_bind_gpu *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (--priv->bound == 0)
		kgsl_gem_unmap(obj);

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);
	return 0;
}

static int
kgsl_gem_map(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	int index;
	int ret = -EINVAL;
	int flags = KGSL_MEMFLAGS_CONPHYS;

	if (priv->flags & DRM_KGSL_GEM_FLAG_MAPPED)
		return 0;

	if (TYPE_IS_PMEM(priv->type) ||
	    priv->type == DRM_KGSL_GEM_TYPE_FD_FBMEM)
		flags = KGSL_MEMFLAGS_CONPHYS;
	else
		flags = KGSL_MEMFLAGS_VMALLOC_MEM;

	if (priv->pagetable == NULL) {
		struct kgsl_device *kgsldev =
			kgsl_get_device(KGSL_DEVICE_YAMATO);
		struct kgsl_mmu *mmu = kgsl_get_mmu(kgsldev);

		if (mmu == NULL) {
			return -EINVAL;
		}

		priv->pagetable =
			kgsl_mmu_getpagetable(mmu, KGSL_MMU_GLOBAL_PT);

		if (priv->pagetable == NULL) {
			return -EINVAL;
		}
	}

	for (index = 0; index < priv->bufcount; index++) {
		ret = kgsl_mmu_map(priv->pagetable,
				   (unsigned long) priv->cpuaddr +
				   priv->bufs[index].offset,
				   obj->size,
				   GSL_PT_PAGE_RV | GSL_PT_PAGE_WV,
				   &priv->bufs[index].gpuaddr,
				   flags | KGSL_MEMFLAGS_ALIGN4K);
	}

	if (priv->type == DRM_KGSL_GEM_TYPE_KMEM ||
	    priv->type & DRM_KGSL_GEM_CACHE_MASK)
		list_add(&priv->list, &kgsl_mem_list);

	priv->flags |= DRM_KGSL_GEM_FLAG_MAPPED;

	return ret;
}

int
kgsl_gem_bind_gpu_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_bind_gpu *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = 0;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (priv->bound++ == 0) {

		if (!kgsl_gem_memory_allocated(obj)) {
			ret = -ENOMEM;
			goto out;
		}

		ret = kgsl_gem_map(obj);

		/* This is legacy behavior - use GET_BUFFERINFO instead */
		args->gpuptr = priv->bufs[0].gpuaddr;
	}
out:
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

/* Allocate the memory and prepare it for CPU mapping */

int
kgsl_gem_alloc_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file_priv)
{
	struct drm_kgsl_gem_alloc *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	ret = kgsl_gem_alloc_memory(obj);

	if (ret) {
	} else if (!priv->mmap_offset) {
		ret = kgsl_gem_create_mmap_offset(obj);
	}

	args->offset = priv->mmap_offset;
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

int
kgsl_gem_mmap_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_mmap *args = data;
	struct drm_gem_object *obj;
	unsigned long addr;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	down_write(&current->mm->mmap_sem);

	addr = do_mmap(obj->filp, 0, args->size,
		       PROT_READ | PROT_WRITE, MAP_SHARED,
		       args->offset);

	up_write(&current->mm->mmap_sem);

	mutex_lock(&dev->struct_mutex);
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	if (IS_ERR((void *) addr))
		return addr;

	args->hostptr = (uint32_t) addr;
	return 0;
}

/* This function is deprecated */

int
kgsl_gem_prep_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_prep *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	ret = kgsl_gem_alloc_memory(obj);
	if (ret) {
		drm_gem_object_unreference(obj);
		mutex_unlock(&dev->struct_mutex);
		return ret;
	}

	if (priv->mmap_offset == 0) {
		ret = kgsl_gem_create_mmap_offset(obj);
		if (ret) {
			drm_gem_object_unreference(obj);
			mutex_unlock(&dev->struct_mutex);
			return ret;
		}
	}

	args->offset = priv->mmap_offset;
	args->phys = priv->cpuaddr;
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);
	return 0;
}

int
kgsl_gem_get_bufinfo_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct drm_kgsl_gem_bufinfo *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = -EINVAL;
	int index;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (!kgsl_gem_memory_allocated(obj)) {
		goto out;
	}

	for (index = 0; index < priv->bufcount; index++) {
		args->offset[index] = priv->bufs[index].offset;
		args->gpuaddr[index] = priv->bufs[index].gpuaddr;
	}

	args->count = priv->bufcount;
	args->active = priv->active;

	ret = 0;

out:
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int
kgsl_gem_set_bufcount_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_kgsl_gem_bufcount *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = -EINVAL;

	if (args->bufcount < 1 || args->bufcount > DRM_KGSL_GEM_MAX_BUFFERS)
		return -EINVAL;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (kgsl_gem_memory_allocated(obj)) {
		goto out;
	}

	priv->bufcount = args->bufcount;
	ret = 0;

out:
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int
kgsl_gem_set_active_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_kgsl_gem_active *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = -EINVAL;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (args->active < 0 || args->active >= priv->bufcount) {
		goto out;
	}

	priv->active = args->active;
	ret = 0;

out:
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int kgsl_gem_kmem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_device *dev = obj->dev;
	struct drm_kgsl_gem_object *priv;
	unsigned long offset, pg;
	struct page *page;

	mutex_lock(&dev->struct_mutex);

	priv = obj->driver_private;

	offset = (unsigned long) vmf->virtual_address - vma->vm_start;
	pg = (unsigned long) priv->cpuaddr + offset;

	page = vmalloc_to_page((void *) pg);
	if (!page) {
		mutex_unlock(&dev->struct_mutex);
		return VM_FAULT_SIGBUS;
	}

	get_page(page);
	vmf->page = page;

	mutex_unlock(&dev->struct_mutex);
	return 0;
}

int kgsl_gem_phys_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_device *dev = obj->dev;
	struct drm_kgsl_gem_object *priv;
	unsigned long offset, pfn;
	int ret = 0;

	offset = ((unsigned long) vmf->virtual_address - vma->vm_start) >>
		PAGE_SHIFT;

	mutex_lock(&dev->struct_mutex);

	priv = obj->driver_private;

	pfn = (priv->cpuaddr >> PAGE_SHIFT) + offset;
	ret = vm_insert_pfn(vma,
			    (unsigned long) vmf->virtual_address, pfn);
	mutex_unlock(&dev->struct_mutex);

	switch (ret) {
	case -ENOMEM:
	case -EAGAIN:
		return VM_FAULT_OOM;
	case -EFAULT:
		return VM_FAULT_SIGBUS;
	default:
		return VM_FAULT_NOPAGE;
	}
}

static struct vm_operations_struct kgsl_gem_kmem_vm_ops = {
	.fault = kgsl_gem_kmem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static struct vm_operations_struct kgsl_gem_phys_vm_ops = {
	.fault = kgsl_gem_phys_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/* This is a clone of the standard drm_gem_mmap function modified to allow
   us to properly map KMEM regions as well as the PMEM regions */

int msm_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct drm_gem_mm *mm = dev->mm_private;
	struct drm_local_map *map = NULL;
	struct drm_gem_object *obj;
	struct drm_hash_item *hash;
	struct drm_kgsl_gem_object *gpriv;
	int ret = 0;

	mutex_lock(&dev->struct_mutex);

	if (drm_ht_find_item(&mm->offset_hash, vma->vm_pgoff, &hash)) {
		mutex_unlock(&dev->struct_mutex);
		return drm_mmap(filp, vma);
	}

	map = drm_hash_entry(hash, struct drm_map_list, hash)->map;
	if (!map ||
	    ((map->flags & _DRM_RESTRICTED) && !capable(CAP_SYS_ADMIN))) {
		ret =  -EPERM;
		goto out_unlock;
	}

	/* Check for valid size. */
	if (map->size < vma->vm_end - vma->vm_start) {
		ret = -EINVAL;
		goto out_unlock;
	}

	obj = map->handle;

	gpriv = obj->driver_private;

	/* VM_PFNMAP is only for memory that doesn't use struct page
	 * in other words, not "normal" memory.  If you try to use it
	 * with "normal" memory then the mappings don't get flushed. */

	if (TYPE_IS_MEM(gpriv->type)) {
		vma->vm_flags |= VM_RESERVED | VM_DONTEXPAND;
		vma->vm_ops = &kgsl_gem_kmem_vm_ops;
	} else {
		vma->vm_flags |= VM_RESERVED | VM_IO | VM_PFNMAP |
			VM_DONTEXPAND;
		vma->vm_ops = &kgsl_gem_phys_vm_ops;
	}

	vma->vm_private_data = map->handle;


	/* Take care of requested caching policy */
	if (gpriv->type == DRM_KGSL_GEM_TYPE_KMEM ||
	    gpriv->type & DRM_KGSL_GEM_CACHE_MASK) {
		if (gpriv->type & DRM_KGSL_GEM_CACHE_WBACKWA)
			vma->vm_page_prot =
			pgprot_writebackwacache(vma->vm_page_prot);
		else if (gpriv->type & DRM_KGSL_GEM_CACHE_WBACK)
				vma->vm_page_prot =
				pgprot_writebackcache(vma->vm_page_prot);
		else if (gpriv->type & DRM_KGSL_GEM_CACHE_WTHROUGH)
				vma->vm_page_prot =
				pgprot_writethroughcache(vma->vm_page_prot);
		else
			vma->vm_page_prot =
			pgprot_writecombine(vma->vm_page_prot);
	} else {
		if (gpriv->type == DRM_KGSL_GEM_TYPE_KMEM_NOCACHE)
			vma->vm_page_prot =
			pgprot_noncached(vma->vm_page_prot);
		else
			/* default pmem is WC */
			vma->vm_page_prot =
			pgprot_writecombine(vma->vm_page_prot);
	}

	/* flush out existing KMEM cached mappings if new ones are
	 * of uncached type */
	if (IS_MEM_UNCACHED(gpriv->type))
			kgsl_cache_range_op((unsigned long) gpriv->cpuaddr,
					    (obj->size * gpriv->bufcount),
					    KGSL_MEMFLAGS_CACHE_FLUSH |
					    KGSL_MEMFLAGS_VMALLOC_MEM);

	/* Add the other memory types here */

	/* Take a ref for this mapping of the object, so that the fault
	 * handler can dereference the mmap offset's pointer to the object.
	 * This reference is cleaned up by the corresponding vm_close
	 * (which should happen whether the vma was created by this call, or
	 * by a vm_open due to mremap or partial unmap or whatever).
	 */
	drm_gem_object_reference(obj);

	vma->vm_file = filp;	/* Needed for drm_vm_open() */
	drm_vm_open_locked(vma);

out_unlock:
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

void
cleanup_fence(struct drm_kgsl_gem_object_fence *fence, int check_waiting)
{
	int j;
	struct drm_kgsl_gem_object_fence_list_entry *this_fence_entry = NULL;
	struct drm_kgsl_gem_object *unlock_obj;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object_wait_list_entry *lock_next;

	fence->ts_valid = 0;
	fence->timestamp = -1;
	fence->ts_device = -1;

	/* Walk the list of buffers in this fence and clean up the */
	/* references. Note that this can cause memory allocations */
	/* to be freed */
	for (j = fence->num_buffers; j > 0; j--) {
		this_fence_entry =
				(struct drm_kgsl_gem_object_fence_list_entry *)
				fence->buffers_in_fence.prev;

		this_fence_entry->in_use = 0;
		obj = this_fence_entry->gem_obj;
		unlock_obj = obj->driver_private;

		/* Delete it from the list */

		list_del(&this_fence_entry->list);

		/* we are unlocking - see if there are other pids waiting */
		if (check_waiting) {
			if (!list_empty(&unlock_obj->wait_list)) {
				lock_next =
				(struct drm_kgsl_gem_object_wait_list_entry *)
					unlock_obj->wait_list.prev;

				list_del((struct list_head *)&lock_next->list);

				unlock_obj->lockpid = 0;
				wake_up_interruptible(
						&lock_next->process_wait_q);
				lock_next->pid = 0;

			} else {
				/* List is empty so set pid to 0 */
				unlock_obj->lockpid = 0;
			}
		}

		drm_gem_object_unreference(obj);
	}
	/* here all the buffers in the fence are released */
	/* clear the fence entry */
	fence->fence_id = ENTRY_EMPTY;
}

int
find_empty_fence(void)
{
	int i;

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		if (gem_buf_fence[i].fence_id == ENTRY_EMPTY) {
			gem_buf_fence[i].fence_id = fence_id++;
			gem_buf_fence[i].ts_valid = 0;
			INIT_LIST_HEAD(&(gem_buf_fence[i].buffers_in_fence));
			if (fence_id == 0xFFFFFFF0)
				fence_id = 1;
			return i;
		} else {

			/* Look for entries to be cleaned up */
			if (gem_buf_fence[i].fence_id == ENTRY_NEEDS_CLEANUP)
				cleanup_fence(&gem_buf_fence[i], 0);
		}
	}

	return ENTRY_EMPTY;
}

int
find_fence(int index)
{
	int i;

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		if (gem_buf_fence[i].fence_id == index)
			return i;
	}

	return ENTRY_EMPTY;
}

void
wakeup_fence_entries(struct drm_kgsl_gem_object_fence *fence)
{
    struct drm_kgsl_gem_object_fence_list_entry *this_fence_entry = NULL;
	struct drm_kgsl_gem_object_wait_list_entry *lock_next;
	struct drm_kgsl_gem_object *unlock_obj;
	struct drm_gem_object *obj;

	/* TS has expired when we get here */
	fence->ts_valid = 0;
	fence->timestamp = -1;
	fence->ts_device = -1;

	list_for_each_entry(this_fence_entry, &fence->buffers_in_fence, list) {
		obj = this_fence_entry->gem_obj;
		unlock_obj = obj->driver_private;

		if (!list_empty(&unlock_obj->wait_list)) {
			lock_next =
				(struct drm_kgsl_gem_object_wait_list_entry *)
					unlock_obj->wait_list.prev;

			/* Unblock the pid */
			lock_next->pid = 0;

			/* Delete it from the list */
			list_del((struct list_head *)&lock_next->list);

			unlock_obj->lockpid = 0;
			wake_up_interruptible(&lock_next->process_wait_q);

		} else {
			/* List is empty so set pid to 0 */
			unlock_obj->lockpid = 0;
		}
	}
	fence->fence_id = ENTRY_NEEDS_CLEANUP;  /* Mark it as needing cleanup */
}

static int kgsl_ts_notifier_cb(struct notifier_block *blk,
			       unsigned long code, void *_param)
{
	struct drm_kgsl_gem_object_fence *fence;
	struct kgsl_device *device = kgsl_get_device(code);
	int i;

	/* loop through the fences to see what things can be processed */

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		fence = &gem_buf_fence[i];
		if (!fence->ts_valid || fence->ts_device != code)
			continue;

		if (kgsl_check_timestamp(device, fence->timestamp))
			wakeup_fence_entries(fence);
	}

	return 0;
}

int
kgsl_gem_lock_handle_ioctl(struct drm_device *dev, void *data,
						   struct drm_file *file_priv)
{
	/* The purpose of this function is to lock a given set of handles. */
	/* The driver will maintain a list of locked handles. */
	/* If a request comes in for a handle that's locked the thread will */
	/* block until it's no longer in use. */

	struct drm_kgsl_gem_lock_handles *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	struct drm_kgsl_gem_object_fence_list_entry *this_fence_entry = NULL;
	struct drm_kgsl_gem_object_fence *fence;
	struct drm_kgsl_gem_object_wait_list_entry *lock_item;
	int i, j;
	int result = 0;
	uint32_t *lock_list;
	uint32_t *work_list = NULL;
	int32_t fence_index;

	/* copy in the data from user space */
	lock_list = kzalloc(sizeof(uint32_t) * args->num_handles, GFP_KERNEL);
	if (!lock_list) {
		result = -ENOMEM;
		goto error;
	}

	if (copy_from_user(lock_list, args->handle_list,
			   sizeof(uint32_t) * args->num_handles)) {
		result = -EFAULT;
		goto free_handle_list;
	}


	work_list = lock_list;
	mutex_lock(&dev->struct_mutex);

	fence_index = find_empty_fence();
	if (fence_index == ENTRY_EMPTY) {
		args->lock_id = 0xDEADBEEF;
		result = -EFAULT;
		goto out_unlock;
	}

	fence = &gem_buf_fence[fence_index];
	gem_buf_fence[fence_index].num_buffers = args->num_handles;
	args->lock_id = gem_buf_fence[fence_index].fence_id;

	for (j = args->num_handles; j > 0; j--, lock_list++) {
		obj = drm_gem_object_lookup(dev, file_priv, *lock_list);

		if (obj == NULL) {
			result = -EBADF;
			goto out_unlock;
		}

		priv = obj->driver_private;
		this_fence_entry = NULL;

		/* get a fence entry to hook into the fence */
		for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
			if (!priv->fence_entries[i].in_use) {
				this_fence_entry = &priv->fence_entries[i];
				this_fence_entry->in_use = 1;
				break;
			}
		}

		if (this_fence_entry == NULL) {
			fence->num_buffers = 0;
			fence->fence_id = ENTRY_EMPTY;
			args->lock_id = 0xDEADBEAD;
			result = -EFAULT;
			drm_gem_object_unreference(obj);
			goto out_unlock;
		}

		/* We're trying to lock - add to a fence */
		list_add((struct list_head *)this_fence_entry,
				 &gem_buf_fence[fence_index].buffers_in_fence);
		if (priv->lockpid) {

			if (priv->lockpid == args->pid) {
				/* now that things are running async this  */
				/* happens when an op isn't done */
				/* so it's already locked by the calling pid */
					continue;
			}


			/* if a pid already had it locked */
			/* create and add to wait list */
			for (i = 0; i < DRM_KGSL_HANDLE_WAIT_ENTRIES; i++) {
				if (priv->wait_entries[i].in_use == 0) {
					/* this one is empty */
					lock_item = &priv->wait_entries[i];
				    lock_item->in_use = 1;
					lock_item->pid = args->pid;
					INIT_LIST_HEAD((struct list_head *)
						&priv->wait_entries[i]);
					break;
				}
			}

			if (i == DRM_KGSL_HANDLE_WAIT_ENTRIES) {

				result =  -EFAULT;
				drm_gem_object_unreference(obj);
				goto out_unlock;
			}

			list_add_tail((struct list_head *)&lock_item->list,
							&priv->wait_list);
			mutex_unlock(&dev->struct_mutex);
			/* here we need to block */
			wait_event_interruptible_timeout(
					priv->wait_entries[i].process_wait_q,
					(priv->lockpid == 0),
					msecs_to_jiffies(64));
			mutex_lock(&dev->struct_mutex);
			lock_item->in_use = 0;
		}

		/* Getting here means no one currently holds the lock */
		priv->lockpid = args->pid;

		args->lock_id = gem_buf_fence[fence_index].fence_id;
	}
	fence->lockpid = args->pid;

out_unlock:
	mutex_unlock(&dev->struct_mutex);

free_handle_list:
	kfree(work_list);

error:
	return result;
}

int
kgsl_gem_unlock_handle_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	struct drm_kgsl_gem_unlock_handles *args = data;
	int result = 0;
	int32_t fence_index;

	mutex_lock(&dev->struct_mutex);
	fence_index = find_fence(args->lock_id);
	if (fence_index == ENTRY_EMPTY) {
		result = -EFAULT;
		goto out_unlock;
	}

	cleanup_fence(&gem_buf_fence[fence_index], 1);

out_unlock:
	mutex_unlock(&dev->struct_mutex);

	return result;
}


int
kgsl_gem_unlock_on_ts_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_unlock_on_ts *args = data;
	int result = 0;
	int ts_done = 0;
	int32_t fence_index, ts_device;
	struct drm_kgsl_gem_object_fence *fence;
	struct kgsl_device *device;

	if (args->type == DRM_KGSL_GEM_TS_3D)
		ts_device = KGSL_DEVICE_YAMATO;
	else if (args->type == DRM_KGSL_GEM_TS_2D)
		ts_device = KGSL_DEVICE_2D0;
	else {
		result = -EINVAL;
		goto error;
	}

	device = kgsl_get_device(ts_device);
	ts_done = kgsl_check_timestamp(device, args->timestamp);

	mutex_lock(&dev->struct_mutex);

	fence_index = find_fence(args->lock_id);
	if (fence_index == ENTRY_EMPTY) {
		result = -EFAULT;
		goto out_unlock;
	}

	fence = &gem_buf_fence[fence_index];
	fence->ts_device = ts_device;

	if (!ts_done)
		fence->ts_valid = 1;
	else
		cleanup_fence(fence, 1);


out_unlock:
	mutex_unlock(&dev->struct_mutex);

error:
	return result;
}

struct drm_ioctl_desc kgsl_drm_ioctls[] = {
	DRM_IOCTL_DEF(DRM_KGSL_GEM_CREATE, kgsl_gem_create_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_PREP, kgsl_gem_prep_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_SETMEMTYPE, kgsl_gem_setmemtype_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_GETMEMTYPE, kgsl_gem_getmemtype_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_BIND_GPU, kgsl_gem_bind_gpu_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_UNBIND_GPU, kgsl_gem_unbind_gpu_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_ALLOC, kgsl_gem_alloc_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_MMAP, kgsl_gem_mmap_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_GET_BUFINFO, kgsl_gem_get_bufinfo_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_SET_BUFCOUNT,
		      kgsl_gem_set_bufcount_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_SET_ACTIVE, kgsl_gem_set_active_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_LOCK_HANDLE,
				  kgsl_gem_lock_handle_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_UNLOCK_HANDLE,
				  kgsl_gem_unlock_handle_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_UNLOCK_ON_TS,
				  kgsl_gem_unlock_on_ts_ioctl, 0),
	DRM_IOCTL_DEF(DRM_KGSL_GEM_CREATE_FD, kgsl_gem_create_fd_ioctl,
		      DRM_MASTER),
};

static struct drm_driver driver = {
	.driver_features = DRIVER_USE_PLATFORM_DEVICE | DRIVER_GEM,
	.load = kgsl_drm_load,
	.unload = kgsl_drm_unload,
	.firstopen = kgsl_drm_firstopen,
	.lastclose = kgsl_drm_lastclose,
	.preclose = kgsl_drm_preclose,
	.suspend = kgsl_drm_suspend,
	.resume = kgsl_drm_resume,
	.reclaim_buffers = drm_core_reclaim_buffers,
	.get_map_ofs = drm_core_get_map_ofs,
	.get_reg_ofs = drm_core_get_reg_ofs,
	.gem_init_object = kgsl_gem_init_object,
	.gem_free_object = kgsl_gem_free_object,
	.ioctls = kgsl_drm_ioctls,

	.fops = {
		 .owner = THIS_MODULE,
		 .open = drm_open,
		 .release = drm_release,
		 .ioctl = drm_ioctl,
		 .mmap = msm_drm_gem_mmap,
		 .poll = drm_poll,
		 .fasync = drm_fasync,
		 },

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

int kgsl_drm_init(struct platform_device *dev)
{
	int i;

	driver.num_ioctls = DRM_ARRAY_SIZE(kgsl_drm_ioctls);
	driver.platform_device = dev;

	INIT_LIST_HEAD(&kgsl_mem_list);

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		gem_buf_fence[i].num_buffers = 0;
		gem_buf_fence[i].ts_valid = 0;
		gem_buf_fence[i].fence_id = ENTRY_EMPTY;
	}

	return drm_init(&driver);
}

void kgsl_drm_exit(void)
{
	drm_exit(&driver);
}
