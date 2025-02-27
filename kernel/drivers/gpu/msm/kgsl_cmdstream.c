#include "kgsl.h"
#include "kgsl_cmdstream.h"
#include "kgsl_sharedmem.h"
#include "kgsl_yamato.h"

int kgsl_cmdstream_init(struct kgsl_device *device)
{
	return 0;
}

int kgsl_cmdstream_close(struct kgsl_device *device)
{
	struct kgsl_mem_entry *entry, *entry_tmp;

	BUG_ON(!mutex_is_locked(&device->mutex));

	list_for_each_entry_safe(entry, entry_tmp, &device->memqueue, list) {
		list_del(&entry->list);
		kgsl_destroy_mem_entry(entry);
	}
	return 0;
}

uint32_t
kgsl_cmdstream_readtimestamp(struct kgsl_device *device,
			     enum kgsl_timestamp_type type)
{
	uint32_t timestamp = 0;

	if (type == KGSL_TIMESTAMP_CONSUMED)
		KGSL_CMDSTREAM_GET_SOP_TIMESTAMP(device,
						 (unsigned int *)&timestamp);
	else if (type == KGSL_TIMESTAMP_RETIRED)
		KGSL_CMDSTREAM_GET_EOP_TIMESTAMP(device,
						 (unsigned int *)&timestamp);
	rmb();
	return timestamp;
}

void kgsl_cmdstream_memqueue_drain(struct kgsl_device *device)
{
	struct kgsl_mem_entry *entry, *entry_tmp;
	uint32_t ts_processed;

	BUG_ON(!mutex_is_locked(&device->mutex));

	/* get current EOP timestamp */
	ts_processed = device->ftbl.device_cmdstream_readtimestamp(
					device,
					KGSL_TIMESTAMP_RETIRED);

	list_for_each_entry_safe(entry, entry_tmp, &device->memqueue, list) {
		if (!timestamp_cmp(ts_processed, entry->free_timestamp))
			break;

		list_del(&entry->list);
		kgsl_destroy_mem_entry(entry);
	}
}

/* to be called when a process is destroyed, this walks the memqueue and
 * frees any entryies that belong to the dying process
 */
void kgsl_cmdstream_memqueue_cleanup(struct kgsl_device *device,
				     struct kgsl_process_private *private)
{
	struct kgsl_mem_entry *entry, *entry_tmp;

	BUG_ON(!mutex_is_locked(&device->mutex));

	list_for_each_entry_safe(entry, entry_tmp, &device->memqueue, list) {
		if (entry->priv == private) {
			list_del(&entry->list);
			kgsl_destroy_mem_entry(entry);
		}
	}
}

void
kgsl_cmdstream_freememontimestamp(struct kgsl_device *device,
				  struct kgsl_mem_entry *entry,
				  uint32_t timestamp,
				  enum kgsl_timestamp_type type)
{
	BUG_ON(!mutex_is_locked(&device->mutex));

	entry->free_timestamp = timestamp;

	list_add_tail(&entry->list, &device->memqueue);
}
