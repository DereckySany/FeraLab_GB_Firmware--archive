#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/kthread.h>
#define HIF_USE_DMA_BOUNCE_BUFFER 1
#include "hif_internal.h"
#include "AR6002/hw2.0/hw/mbox_host_reg.h"
#ifdef ANDROID_ENV
#include <linux/wakelock.h>
extern struct wake_lock ar6k_init_wake_lock;
#endif
/* ATHENV */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) && defined(CONFIG_PM)
#define dev_to_sdio_func(d)	container_of(d, struct sdio_func, dev)
#define to_sdio_driver(d)      container_of(d, struct sdio_driver, drv)
static int hifDeviceSuspend(struct device *dev);
static int hifDeviceResume(struct device *dev);
static int Func0_CMD52WriteByte(struct mmc_card *card, unsigned int address, unsigned char byte);
#endif /* CONFIG_PM */
static int hifDeviceInserted(struct sdio_func *func, const struct sdio_device_id *id);
static void hifDeviceRemoved(struct sdio_func *func);
static HIF_DEVICE *addHifDevice(struct sdio_func *func);
static HIF_DEVICE *getHifDevice(struct sdio_func *func);
static void delHifDevice(HIF_DEVICE * device);

static const struct sdio_device_id ar6k_id_table[] = {
    {  SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_AR6002_BASE | 0x0))  },
    {  SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_AR6002_BASE | 0x1))  },
    {  SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_AR6003_BASE | 0x0))  },
    { /* null */                                         },
};
MODULE_DEVICE_TABLE(sdio, ar6k_id_table);

static struct sdio_driver ar6k_driver = {
	.name = "ar6k_wlan",
	.id_table = ar6k_id_table,
	.probe = hifDeviceInserted,
	.remove = hifDeviceRemoved,
        };

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) && defined(CONFIG_PM)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,29)
static struct dev_pm_ops ar6k_device_pm_ops = {
#else
static struct pm_ops ar6k_device_pm_ops = {
#endif 
    .suspend = hifDeviceSuspend,
    .resume = hifDeviceResume,
};
#endif /* CONFIG_PM */

static int registered = 0;

OSDRV_CALLBACKS osdrvCallbacks;
extern A_UINT32 onebitmode;
extern A_UINT32 busspeedlow;
extern A_UINT32 debughif;

#define AR_DEBUG_PRINTF(lvl, args)
#define AR_DEBUG_ASSERT(test)

static BUS_REQUEST *hifAllocateBusRequest(HIF_DEVICE *device);
static void hifFreeBusRequest(HIF_DEVICE *device, BUS_REQUEST *busrequest);
static void ResetAllCards(void);

A_STATUS HIFInit(OSDRV_CALLBACKS *callbacks)
{
    int status;
    AR_DEBUG_ASSERT(callbacks != NULL);
    osdrvCallbacks = *callbacks;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: HIFInit registering\n"));
    registered = 1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) && defined(CONFIG_PM)
    if (callbacks->deviceSuspendHandler && callbacks->deviceResumeHandler) {
        ar6k_driver.drv.pm = &ar6k_device_pm_ops;
    }
#endif /* CONFIG_PM */
    status = sdio_register_driver(&ar6k_driver);
    AR_DEBUG_ASSERT(status==0);

    if (status != 0) {
        return A_ERROR;
    }

    return A_OK;

}

static A_STATUS
__HIFReadWrite(HIF_DEVICE *device,
             A_UINT32 address,
             A_UCHAR *buffer,
             A_UINT32 length,
             A_UINT32 request,
             void *context)
{
    A_UINT8 opcode;
    A_STATUS    status = A_OK;
    int     ret;
    A_UINT8 *tbuffer;

    AR_DEBUG_ASSERT(device != NULL);
    AR_DEBUG_ASSERT(device->func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Device: 0x%p, buffer:0x%p\n", device, buffer));

    do {
        if (request & HIF_EXTENDED_IO) {
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid command type: 0x%08x\n", request));
            status = A_EINVAL;
            break;
        }

        if (request & HIF_BLOCK_BASIS) {
            length = (length / HIF_MBOX_BLOCK_SIZE) * HIF_MBOX_BLOCK_SIZE;
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
                            ("AR6000: Block mode (BlockLen: %d)\n",
                            length));
        } else if (request & HIF_BYTE_BASIS) {
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
                            ("AR6000: Byte mode (BlockLen: %d)\n",
                            length));
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid data mode: 0x%08x\n", request));
            status = A_EINVAL;
            break;
        }

        if ((address >= HIF_MBOX_START_ADDR(0)) &&
            (address <= HIF_MBOX_END_ADDR(3)))
        {

            AR_DEBUG_ASSERT(length <= HIF_MBOX_WIDTH);
            address += (HIF_MBOX_WIDTH - length);
        }

        if (request & HIF_FIXED_ADDRESS) {
            opcode = CMD53_FIXED_ADDRESS;
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Address mode: Fixed 0x%X\n", address));
        } else if (request & HIF_INCREMENTAL_ADDRESS) {
            opcode = CMD53_INCR_ADDRESS;
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Address mode: Incremental 0x%X\n", address));
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid address mode: 0x%08x\n", request));
            status = A_EINVAL;
            break;
        }

        if (request & HIF_WRITE) {
#if HIF_USE_DMA_BOUNCE_BUFFER
	  AR_DEBUG_ASSERT(device->dma_buffer != NULL);
	  tbuffer = device->dma_buffer;
	  AR_DEBUG_ASSERT(length <= HIF_DMA_BUFFER_SIZE);
	  memcpy(tbuffer, buffer, length);
#else
	  tbuffer = buffer;
#endif
            if (opcode == CMD53_FIXED_ADDRESS) {
                ret = sdio_writesb(device->func, address, tbuffer, length);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: writesb ret=%d address: 0x%X, len: %d, 0x%X\n",
						  ret, address, length, *(int *)tbuffer));
            } else {
                ret = sdio_memcpy_toio(device->func, address, tbuffer, length);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: writeio ret=%d address: 0x%X, len: %d, 0x%X\n",
						  ret, address, length, *(int *)tbuffer));
            }
        } else if (request & HIF_READ) {
#if HIF_USE_DMA_BOUNCE_BUFFER
	  AR_DEBUG_ASSERT(device->dma_buffer != NULL);
	  AR_DEBUG_ASSERT(length <= HIF_DMA_BUFFER_SIZE);
	  tbuffer = device->dma_buffer;
#else
	  tbuffer = buffer;
#endif
            if (opcode == CMD53_FIXED_ADDRESS) {
                ret = sdio_readsb(device->func, tbuffer, address, length);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: readsb ret=%d address: 0x%X, len: %d, 0x%X\n",
						  ret, address, length, *(int *)tbuffer));
            } else {
                ret = sdio_memcpy_fromio(device->func, tbuffer, address, length);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: readio ret=%d address: 0x%X, len: %d, 0x%X\n",
						  ret, address, length, *(int *)tbuffer));
            }
#if HIF_USE_DMA_BOUNCE_BUFFER
	  /* copy the read data from the dma buffer */
	  memcpy(buffer, tbuffer, length);
#endif
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid direction: 0x%08x\n", request));
            status = A_EINVAL;
            break;
        }

        if (ret) {
            status = A_ERROR;
        }
    } while (FALSE);

    return status;
}

/* queue a read/write request */
A_STATUS
HIFReadWrite(HIF_DEVICE *device,
             A_UINT32 address,
             A_UCHAR *buffer,
             A_UINT32 length,
             A_UINT32 request,
             void *context)
{
    A_STATUS    status = A_OK;
    unsigned long flags;
    BUS_REQUEST *busrequest;
    BUS_REQUEST *async;
    BUS_REQUEST *active;

    AR_DEBUG_ASSERT(device != NULL);
    AR_DEBUG_ASSERT(device->func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Device: %p\n", device));

    do {
        if ((request & HIF_ASYNCHRONOUS) || (request & HIF_SYNCHRONOUS)){
            /* serialize all requests through the async thread */
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Execution mode: %s\n", (request & HIF_ASYNCHRONOUS)?"Async":"Synch"));
            busrequest = hifAllocateBusRequest(device);
            if (busrequest == NULL) {
               AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: no async bus requests available\n"));
                return A_ERROR;
            }
            spin_lock_irqsave(&device->asynclock, flags);
            busrequest->address = address;
            busrequest->buffer = buffer;
            busrequest->length = length;
            busrequest->request = request;
            busrequest->context = context;
            /* add to async list */
            active = device->asyncreq;
            if (active == NULL) {
                device->asyncreq = busrequest;
                device->asyncreq->inusenext = NULL;
            } else {
                for (async = device->asyncreq;
                     async != NULL;
                     async = async->inusenext) {
                     active =  async;
                }
                active->inusenext = busrequest;
                busrequest->inusenext = NULL;
            }
            spin_unlock_irqrestore(&device->asynclock, flags);
            if (request & HIF_SYNCHRONOUS) {
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: queued sync req: 0x%X\n", (unsigned int)busrequest));

                /* wait for completion */
                up(&device->sem_async);
                if (down_interruptible(&busrequest->sem_req) != 0) {
                    /* interrupted, exit */
                    return A_ERROR;
                } else {
                    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: sync return freeing 0x%X: 0x%X\n",
						      (unsigned int)busrequest, busrequest->status));
                    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: freeing req: 0x%X\n", (unsigned int)request));
                    hifFreeBusRequest(device, busrequest);
                    return busrequest->status;
                }
            } else {
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: queued async req: 0x%X\n", (unsigned int)busrequest));
                up(&device->sem_async);
                return A_PENDING;
            }
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid execution mode: 0x%08x\n", (unsigned int)request));
            status = A_EINVAL;
            break;
        }
    } while(0);

    return status;
}
/* thread to serialize all requests, both sync and async */
static int async_task(void *param)
 {
    HIF_DEVICE *device;
    BUS_REQUEST *request;
    A_STATUS status;
    unsigned long flags;

    device = (HIF_DEVICE *)param;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async task\n"));
    set_current_state(TASK_INTERRUPTIBLE);
    while(!device->async_shutdown) {
        /* wait for work */
        if (down_interruptible(&device->sem_async) != 0) {
            /* interrupted, exit */
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async task interrupted\n"));
            break;
        }
        if (device->async_shutdown) {
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async task stopping\n"));
            break;
        }
        /* we want to hold the host over multiple cmds if possible, but holding the host blocks card interrupts */
        sdio_claim_host(device->func);
        spin_lock_irqsave(&device->asynclock, flags);
        /* pull the request to work on */
        while (device->asyncreq != NULL) {
            request = device->asyncreq;
            if (request->inusenext != NULL) {
                device->asyncreq = request->inusenext;
            } else {
                device->asyncreq = NULL;
            }
            spin_unlock_irqrestore(&device->asynclock, flags);
            /* call HIFReadWrite in sync mode to do the work */
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async_task processing req: 0x%X\n", (unsigned int)request));
            status = __HIFReadWrite(device, request->address, request->buffer,
                                  request->length, request->request & ~HIF_SYNCHRONOUS, NULL);
            if (request->request & HIF_ASYNCHRONOUS) {
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async_task completion routine req: 0x%X\n", (unsigned int)request));
                device->htcCallbacks.rwCompletionHandler(request->context, status);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async_task freeing req: 0x%X\n", (unsigned int)request));
                hifFreeBusRequest(device, request);
            } else {
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async_task upping req: 0x%X\n", (unsigned int)request));
                request->status = status;
                up(&request->sem_req);
            }
            spin_lock_irqsave(&device->asynclock, flags);
        }
        spin_unlock_irqrestore(&device->asynclock, flags);
        sdio_release_host(device->func);
    }

    complete_and_exit(&device->async_completion, 0);
    return 0;
 }

A_STATUS
HIFConfigureDevice(HIF_DEVICE *device, HIF_DEVICE_CONFIG_OPCODE opcode,
                   void *config, A_UINT32 configLen)
{
    A_UINT32 count;

    switch(opcode) {
        case HIF_DEVICE_GET_MBOX_BLOCK_SIZE:
            ((A_UINT32 *)config)[0] = HIF_MBOX0_BLOCK_SIZE;
            ((A_UINT32 *)config)[1] = HIF_MBOX1_BLOCK_SIZE;
            ((A_UINT32 *)config)[2] = HIF_MBOX2_BLOCK_SIZE;
            ((A_UINT32 *)config)[3] = HIF_MBOX3_BLOCK_SIZE;
            break;

        case HIF_DEVICE_GET_MBOX_ADDR:
            for (count = 0; count < 4; count ++) {
                ((A_UINT32 *)config)[count] = HIF_MBOX_START_ADDR(count);
            }
            break;
        case HIF_DEVICE_GET_IRQ_PROC_MODE:
            *((HIF_DEVICE_IRQ_PROCESSING_MODE *)config) = HIF_DEVICE_IRQ_SYNC_ONLY;
            break;
        case HIF_DEVICE_GET_OS_DEVICE:
                /* pass back a pointer to the SDIO function's "dev" struct */
            ((HIF_DEVICE_OS_DEVICE_INFO *)config)->pOSDevice = &device->func->dev;
            break; 
        default:
            AR_DEBUG_PRINTF(ATH_DEBUG_WARN,
                            ("AR6000: Unsupported configuration opcode: %d\n", opcode));
            return A_ERROR;
    }

    return A_OK;
}

void
HIFShutDownDevice(HIF_DEVICE *device)
{
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: +HIFShutDownDevice\n"));
    if (device != NULL) {
        AR_DEBUG_ASSERT(device->func != NULL);
    } else {
        AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: HIFShutDownDevice, resetting\n"));
        ResetAllCards();
        if (registered) {
            registered = 0;
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
                            ("AR6000: Unregistering with the bus driver\n"));
            sdio_unregister_driver(&ar6k_driver);
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
                            ("AR6000: Unregistered\n"));
        }
    }
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: -HIFShutDownDevice\n"));
}

static void
hifIRQHandler(struct sdio_func *func)
{
    A_STATUS status;
    HIF_DEVICE *device;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: +hifIRQHandler\n"));

    device = getHifDevice(func);
    atomic_set(&device->irqHandling, 1);
    sdio_release_host(device->func);
    status = device->htcCallbacks.dsrHandler(device->htcCallbacks.context);
    sdio_claim_host(device->func);
    atomic_set(&device->irqHandling, 0);
    AR_DEBUG_ASSERT(status == A_OK || status == A_ECANCELED);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: -hifIRQHandler\n"));
}

static int startup_task(void *param)
{
    HIF_DEVICE *device;
    device = (HIF_DEVICE *)param;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: call HTC from startup_task\n"));
#ifdef ANDROID_ENV
    wake_lock(&ar6k_init_wake_lock);
#endif
    if ((osdrvCallbacks.deviceInsertedHandler(osdrvCallbacks.context,device)) != A_OK) {
        AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Device rejected\n"));
    }
#ifdef ANDROID_ENV
    wake_unlock(&ar6k_init_wake_lock);
#endif
/* ATHENV */
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) && defined(CONFIG_PM)
/* handle HTC startup via thread*/
static int resume_task(void *param)
{
    HIF_DEVICE *device;
    device = (HIF_DEVICE *)param;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: call HTC from resume_task\n"));
/* ATHENV */
#ifdef ANDROID_ENV
    wake_lock(&ar6k_init_wake_lock);
#endif
/* ATHENV */
        /* start  up inform DRV layer */
    if (device && device->claimedContext && osdrvCallbacks.deviceResumeHandler &&
        osdrvCallbacks.deviceResumeHandler(device->claimedContext) != A_OK) {
        AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Device rejected\n"));
    }

/* ATHENV */
#ifdef ANDROID_ENV
    wake_unlock(&ar6k_init_wake_lock);
#endif
/* ATHENV */
    return 0;
}
#endif /* CONFIG_PM */

static int hifDeviceInserted(struct sdio_func *func, const struct sdio_device_id *id)
{
    int ret;
    HIF_DEVICE * device;
    int count;
    struct task_struct* startup_task_struct;

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
		    ("AR6000: hifDeviceInserted, Function: 0x%X, Vendor ID: 0x%X, Device ID: 0x%X, block size: 0x%X/0x%X\n",
		     func->num, func->vendor, func->device, func->max_blksize, func->cur_blksize));

    addHifDevice(func);
    device = getHifDevice(func);

    spin_lock_init(&device->lock);

    spin_lock_init(&device->asynclock);

        /* enable the SDIO function */
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: claim\n"));
    sdio_claim_host(func);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: enable\n"));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
    /* give us some time to enable, in ms */
    func->enable_timeout = 100;
#endif

    ret = sdio_enable_func(func);
    if (ret) {

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
        AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), Unable to enable AR6K: 0x%X, timeout: %d\n",
					  __FUNCTION__, ret, func->enable_timeout));
#else

        AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), Unable to enable AR6K: 0x%X\n",
					  __FUNCTION__, ret));
#endif
        sdio_release_host(func);
        return ret;
    }
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: set block size 0x%X\n", HIF_MBOX_BLOCK_SIZE));
    ret = sdio_set_block_size(func, HIF_MBOX_BLOCK_SIZE);
    sdio_release_host(func);
    if (ret) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), Unable to set block size 0x%x  AR6K: 0x%X\n",
					  __FUNCTION__, HIF_MBOX_BLOCK_SIZE, ret));
        return ret;
    }
    /* Initialize the bus requests to be used later */
    A_MEMZERO(device->busRequest, sizeof(device->busRequest));
    for (count = 0; count < BUS_REQUEST_MAX_NUM; count ++) {
        sema_init(&device->busRequest[count].sem_req, 0);
        hifFreeBusRequest(device, &device->busRequest[count]);
    }

    /* create async I/O thread */
    device->async_shutdown = 0;
    device->async_task = kthread_create(async_task,
                                       (void *)device,
                                       "AR6K Async");
    if (IS_ERR(device->async_task)) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), to create async task\n", __FUNCTION__));
        return A_ERROR;
    }
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: start async task\n"));
    sema_init(&device->sem_async, 0);
    wake_up_process(device->async_task );

    /* create startup thread */
    startup_task_struct = kthread_create(startup_task,
                                  (void *)device,
                                  "AR6K startup");
    if (IS_ERR(startup_task_struct)) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), to create startup task\n", __FUNCTION__));
        return A_ERROR;
    }
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: start startup task\n"));
    wake_up_process(startup_task_struct);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: return %d\n", ret));
    return ret;
}


void
HIFAckInterrupt(HIF_DEVICE *device)
{
    AR_DEBUG_ASSERT(device != NULL);

    /* Acknowledge our function IRQ */
}

void
HIFUnMaskInterrupt(HIF_DEVICE *device)
{
    int ret;;

    AR_DEBUG_ASSERT(device != NULL);
    AR_DEBUG_ASSERT(device->func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: HIFUnMaskInterrupt\n"));

    /* Register the IRQ Handler */
    sdio_claim_host(device->func);
    ret = sdio_claim_irq(device->func, hifIRQHandler);
    sdio_release_host(device->func);
    AR_DEBUG_ASSERT(ret == 0);
}

void HIFMaskInterrupt(HIF_DEVICE *device)
{
    int ret;
    AR_DEBUG_ASSERT(device != NULL);
    AR_DEBUG_ASSERT(device->func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: HIFMaskInterrupt\n"));

    /* Mask our function IRQ */
    sdio_claim_host(device->func);
    while (atomic_read(&device->irqHandling)) {
        sdio_release_host(device->func);
        schedule_timeout(HZ/10);
        sdio_claim_host(device->func);
    }
    ret = sdio_release_irq(device->func);
    sdio_release_host(device->func);
    //AR_DEBUG_ASSERT(ret == 0);
}

static BUS_REQUEST *hifAllocateBusRequest(HIF_DEVICE *device)
{
    BUS_REQUEST *busrequest;
    unsigned long flag;

    /* Acquire lock */
    spin_lock_irqsave(&device->lock, flag);

    /* Remove first in list */
    if((busrequest = device->s_busRequestFreeQueue) != NULL)
    {
        device->s_busRequestFreeQueue = busrequest->next;
    }

    /* Release lock */
    spin_unlock_irqrestore(&device->lock, flag);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: hifAllocateBusRequest: 0x%p\n", busrequest));
    return busrequest;
}

static void
hifFreeBusRequest(HIF_DEVICE *device, BUS_REQUEST *busrequest)
{
    unsigned long flag;

    AR_DEBUG_ASSERT(busrequest != NULL);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: hifFreeBusRequest: 0x%p\n", busrequest));
    /* Acquire lock */
    spin_lock_irqsave(&device->lock, flag);


    /* Insert first in list */
    busrequest->next = device->s_busRequestFreeQueue;
    busrequest->inusenext = NULL;
    device->s_busRequestFreeQueue = busrequest;

    /* Release lock */
    spin_unlock_irqrestore(&device->lock, flag);
}

static void hifDeviceRemoved(struct sdio_func *func)
{
    A_STATUS status = A_OK;
    HIF_DEVICE *device;
    __attribute__((unused)) int ret;
    AR_DEBUG_ASSERT(func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: +hifDeviceRemoved\n"));
    device = getHifDevice(func);
    if (device->claimedContext != NULL) {
        status = osdrvCallbacks.deviceRemovedHandler(device->claimedContext, device);
    }

    if (device->is_suspend) {
        device->is_suspend = FALSE;
    }
    else {
        if (!IS_ERR(device->async_task)) {
            init_completion(&device->async_completion);
            device->async_shutdown = 1;
            up(&device->sem_async);
            wait_for_completion(&device->async_completion);
            device->async_task = NULL;
        }
        /* Disable the card */
        sdio_claim_host(device->func);
        ret = sdio_disable_func(device->func);
        sdio_release_host(device->func);
    }
    delHifDevice(device);
    //AR_DEBUG_ASSERT(status == A_OK);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: -hifDeviceRemoved\n"));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) && defined(CONFIG_PM)
static int hifDeviceSuspend(struct device *dev)
{
    int ret;
    struct sdio_func *func = dev_to_sdio_func(dev);
    A_STATUS status = A_OK;
    HIF_DEVICE *device;   
    device = getHifDevice(func);
    if (device && device->claimedContext && osdrvCallbacks.deviceSuspendHandler) {
        status = osdrvCallbacks.deviceSuspendHandler(device->claimedContext);
    }
    if (status == A_OK) {
        /* Waiting for all pending request */
        if (!IS_ERR(device->async_task)) {
            init_completion(&device->async_completion);
            device->async_shutdown = 1;
            up(&device->sem_async);
            wait_for_completion(&device->async_completion);
            device->async_task = NULL;
        }
        sdio_claim_host(device->func);
        status = sdio_disable_func(device->func);
        AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: reseting SDIO card back to uninitialized state \n"));
        ret = Func0_CMD52WriteByte(device->func->card, SDIO_CCCR_ABORT, (1 << 3));
        if (ret!=0) {
             AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: reset failed : %d \n",ret));
        }
        sdio_release_host(device->func);
        device->is_suspend = TRUE;
    } else if (status == A_EBUSY) {
        return -EBUSY; /* Return -1 if customer use all android patch of mmc stack provided by us */
    }
    return A_SUCCESS(status) ? 0 : status;
}

static int hifDeviceResume(struct device *dev)
{
    struct task_struct* pTask;
    const char *taskName;
    int (*taskFunc)(void *);
	struct sdio_func *func = dev_to_sdio_func(dev);
    A_STATUS ret = A_OK;
    HIF_DEVICE *device;   
    device = getHifDevice(func);

    if (device->is_suspend) {
       /* enable the SDIO function */
        sdio_claim_host(func);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
        func->enable_timeout = 69;
#endif
        ret = sdio_enable_func(func);
        if (ret) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), Unable to enable AR6K: 0x%X\n",
					  __FUNCTION__, ret));
            sdio_release_host(func);
            return ret;
    }
        ret = sdio_set_block_size(func, HIF_MBOX_BLOCK_SIZE);
        sdio_release_host(func);
        if (ret) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), Unable to set block size 0x%x  AR6K: 0x%X\n",
					  __FUNCTION__, HIF_MBOX_BLOCK_SIZE, ret));
            return ret;
        }
        device->is_suspend = FALSE;
        /* create async I/O thread */
        if (!device->async_task) {
            device->async_shutdown = 0;
            device->async_task = kthread_create(async_task,
                                           (void *)device,
                                           "AR6K Async");
           if (IS_ERR(device->async_task)) {
               AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), to create async task\n", __FUNCTION__));
                return A_ERROR;
           }
           AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: start async task\n"));
           wake_up_process(device->async_task );    
        }
    }

    if (!device->claimedContext) {
        printk("WARNING!!! No claimedContext during resume wlan\n"); 
        taskFunc = startup_task;
        taskName = "AR6K startup";
    } else {
        taskFunc = resume_task;
        taskName = "AR6K resume";
    }
    /* create resume thread */
    pTask = kthread_create(taskFunc, (void *)device, "%s", taskName);
    if (IS_ERR(pTask)) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), to create resume task\n", __FUNCTION__));
        return A_ERROR;
    }
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: start resume task\n"));
    wake_up_process(pTask);
    return A_SUCCESS(ret) ? 0 : ret;
}
#endif /* CONFIG_PM */

static HIF_DEVICE *
addHifDevice(struct sdio_func *func)
{
    HIF_DEVICE *hifdevice;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: addHifDevice\n"));
    AR_DEBUG_ASSERT(func != NULL);
    hifdevice = (HIF_DEVICE *)kzalloc(sizeof(HIF_DEVICE), GFP_KERNEL);
    AR_DEBUG_ASSERT(hifdevice != NULL);
#if HIF_USE_DMA_BOUNCE_BUFFER
    hifdevice->dma_buffer = kmalloc(HIF_DMA_BUFFER_SIZE, GFP_KERNEL);
    AR_DEBUG_ASSERT(hifdevice->dma_buffer != NULL);
#endif
    hifdevice->func = func;
    sdio_set_drvdata(func, hifdevice);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: addHifDevice; 0x%p\n", hifdevice));
    return hifdevice;
}

static HIF_DEVICE *
getHifDevice(struct sdio_func *func)
{
    AR_DEBUG_ASSERT(func != NULL);
    return (HIF_DEVICE *)sdio_get_drvdata(func);
}

static void
delHifDevice(HIF_DEVICE * device)
{
    AR_DEBUG_ASSERT(device!= NULL);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: delHifDevice; 0x%p\n", device));
    if (device->dma_buffer != NULL) {
        kfree(device->dma_buffer);
    }
    kfree(device);
}

static void ResetAllCards(void)
{
}

void HIFClaimDevice(HIF_DEVICE  *device, void *context)
{
    device->claimedContext = context;
}

void HIFReleaseDevice(HIF_DEVICE  *device)
{
    device->claimedContext = NULL;
}

A_STATUS HIFAttachHTC(HIF_DEVICE *device, HTC_CALLBACKS *callbacks)
{
    if (device->htcCallbacks.context != NULL) {
            /* already in use! */
        return A_ERROR;
    }
    device->htcCallbacks = *callbacks;
    return A_OK;
}

void HIFDetachHTC(HIF_DEVICE *device)
{
    A_MEMZERO(&device->htcCallbacks,sizeof(device->htcCallbacks));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) && defined(CONFIG_PM)
#define SDIO_SET_CMD52_ARG(arg,rw,func,raw,address,writedata) \
    (arg) = (((rw) & 1) << 31)           | \
            (((func) & 0x7) << 28)       | \
            (((raw) & 1) << 27)          | \
            (1 << 26)                    | \
            (((address) & 0x1FFFF) << 9) | \
            (1 << 8)                     | \
            ((writedata) & 0xFF)

#define SDIO_SET_CMD52_READ_ARG(arg,func,address) \
    SDIO_SET_CMD52_ARG(arg,0,(func),0,address,0x00)
#define SDIO_SET_CMD52_WRITE_ARG(arg,func,address,value) \
    SDIO_SET_CMD52_ARG(arg,1,(func),0,address,value)

static int Func0_CMD52WriteByte(struct mmc_card *card, unsigned int address, unsigned char byte)
{
    struct mmc_command ioCmd;
    unsigned long      arg;

    memset(&ioCmd,0,sizeof(ioCmd));
    SDIO_SET_CMD52_WRITE_ARG(arg,0,address,byte);
    ioCmd.opcode = SD_IO_RW_DIRECT;
    ioCmd.arg = arg;
    ioCmd.flags = MMC_RSP_R5 | MMC_CMD_AC;

    return mmc_wait_for_cmd(card->host, &ioCmd, 0);
}

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27) && defined(CONFIG_PM) */

