#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/of_dma.h>

#include "dma-proxy.h"

#undef INTERNAL_TEST

MODULE_LICENSE("GPL");

#define DRIVER_NAME 		"dma_proxy"
#define ERROR 			-1

/* The following module parameter controls if the internal test runs when the module is inserted.
 */
static unsigned internal_test = 1;
module_param(internal_test, int, S_IRUGO);

/* The following data structure represents a single channel of DMA, transmit or receive in the case
 * when using AXI DMA.  It contains all the data to be maintained for the channel.
 */
struct dma_proxy_channel {
	struct dma_proxy_channel_interface *interface_p;	/* user to kernel space interface */
	dma_addr_t interface_phys_addr;

	struct device *proxy_device_p;				/* character device support */
	struct device *dma_device_p;
	dev_t dev_node;
	struct cdev cdev;
	struct class *class_p;

	struct dma_chan *channel_p;				/* dma support */
	struct completion cmp;
	dma_cookie_t cookie;
	dma_addr_t dma_handle;
	u32 direction;						/* DMA_MEM_TO_DEV or DMA_DEV_TO_MEM */
	struct scatterlist sglist;
};

/* Handle a callback and indicate the DMA transfer is complete to another
 * thread of control
 */
static void sync_callback(void *completion)
{
	/* Indicate the DMA transaction completed to allow the other
	 * thread of control to finish processing
	 */
	complete(completion);
}

/* Prepare a DMA buffer to be used in a DMA transaction, submit it to the DMA engine
 * to ibe queued and return a cookie that can be used to track that status of the
 * transaction
 */
static void start_transfer(struct dma_proxy_channel *pchannel_p, u64 pl_addr, unsigned int sg_len)
{
	enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	struct dma_async_tx_descriptor *chan_desc;
	//struct dma_proxy_channel_interface *interface_p = pchannel_p->interface_p;
    struct dma_device *dma_device = pchannel_p->channel_p->device;

    u32 context[5] = {0x0};
    //u64 pl_addr = 0x80000000 + pl_addr_offset;
	
    /* For now use a single entry in a scatter gather list just for future
	 * flexibility for scatter gather.
	 */
	sg_init_table(&pchannel_p->sglist, 1);
	//sg_dma_len(&pchannel_p->sglist) = interface_p->length;
    sg_dma_len(&pchannel_p->sglist) = sg_len;

    if(pchannel_p->direction == DMA_MEM_TO_DEV){                   
        //如果从PS读,写PL,BD描述符缓冲区地址为DMA总线地址,APP字段为PL物理地址
        context[0] = 0x40800000 + (sg_len & 0x7FFFFF);   //APP0: datamover command, EOF=1,Type=1,BTT=interface_p->length(最多23位，即8MB), 
        context[1] = pl_addr & 0xFFFFFFFF;                                      //APP1: datamover command, 低32位
        context[2] = (pl_addr>>32) & 0xFFFFFFFF;
	    
        sg_dma_address(&pchannel_p->sglist) = pchannel_p->dma_handle;
    }
    else if(pchannel_p->direction == DMA_DEV_TO_MEM){              
        //如果从PL读,写PS,BD描述符缓冲区地址为PL物理地址,APP字段为PS DMA总线地址
        context[0] = 0x40800000 + (sg_len & 0x7FFFFF);              //APP0: datamover command, EOF=1,Type=1,BTT=interface_p->length, 
        context[1] = (pchannel_p->dma_handle) & 0xFFFFFFFF;         //APP1: datamover command, 低32位
        context[2] = ( (pchannel_p->dma_handle)>>32) & 0xFFFFFFFF;
        
        sg_dma_address(&pchannel_p->sglist) = pl_addr;
    }

	chan_desc = dma_device->device_prep_slave_sg(pchannel_p->channel_p, &pchannel_p->sglist, 1, 
						pchannel_p->direction, flags, (void*)context);

	/* Make sure the operation was completed successfully
	 */
	if (!chan_desc) {
		printk(KERN_ERR "dmaengine_prep*() error\n");
	} else {
		chan_desc->callback = sync_callback;
		chan_desc->callback_param = &pchannel_p->cmp;

		/* Initialize the completion for the transfer and before using it
		 * then submit the transaction to the DMA engine so that it's queued
		 * up to be processed later and get a cookie to track it's status
		 */
		init_completion(&pchannel_p->cmp);

		pchannel_p->cookie = dmaengine_submit(chan_desc);
		if (dma_submit_error(pchannel_p->cookie)) {
			printk("Submit error\n");
	 		return;
		}

		/* Start the DMA transaction which was previously queued up in the DMA engine
		 */
		dma_async_issue_pending(pchannel_p->channel_p);
	}
}

/* Wait for a DMA transfer that was previously submitted to the DMA engine
 */
static void wait_for_transfer(struct dma_proxy_channel *pchannel_p)
{
	unsigned long timeout = msecs_to_jiffies(3000);
	enum dma_status status;

	pchannel_p->interface_p->status = PROXY_BUSY;

	/* Wait for the transaction to complete, or timeout, or get an error
	 */
	timeout = wait_for_completion_timeout(&pchannel_p->cmp, timeout);
	status = dma_async_is_tx_complete(pchannel_p->channel_p, pchannel_p->cookie, NULL, NULL);

	if (timeout == 0)  {
		pchannel_p->interface_p->status  = PROXY_TIMEOUT;
		printk(KERN_ERR "DMA timed out\n");
	} else if (status != DMA_COMPLETE) {
		pchannel_p->interface_p->status = PROXY_ERROR;
		printk(KERN_ERR "DMA returned completion callback status of: %s\n",
			   status == DMA_ERROR ? "error" : "in progress");
	} else
		pchannel_p->interface_p->status = PROXY_NO_ERROR;
}

/* Perform the DMA transfer for the channel, starting it then blocking to wait for completion.
 */
static void transfer(struct dma_proxy_channel *pchannel_p)
{
    // 4 bytes align
    int max_transfer_size = 0x7FFF00; 
    u64 pl_addr = 0x80000000;
    int cnt = 0;
    const char dir[3][5] = {"h2h","h2c","c2h"};

	/* The physical address of the buffer in the interface is needed for the dma transfer
	 * as the buffer may not be the first data in the interface
	 */
    printk("pchannel_p->interface_phys_addr: %#llx\n",pchannel_p->interface_phys_addr);
	pchannel_p->dma_handle = (dma_addr_t)(pchannel_p->interface_phys_addr + 
					offsetof(struct dma_proxy_channel_interface, buffer));
    while(pchannel_p->interface_p->length > max_transfer_size){
        printk(KERN_INFO "%s %dst transfer: pl addr %#llx, ps physical addr %#llx, size %#x\n", dir[pchannel_p->direction], cnt, pl_addr, pchannel_p->dma_handle, max_transfer_size);
        start_transfer(pchannel_p,pl_addr,max_transfer_size);
	    wait_for_transfer(pchannel_p);
        //update PL DDR buffer start addr and ARM DDR buffer start addr
        pl_addr  += max_transfer_size;
        pchannel_p->dma_handle += max_transfer_size;
        //update the left bytes to transfer
        pchannel_p->interface_p->length -= max_transfer_size;
        ++cnt;
    }
    if(pchannel_p->interface_p->length > 0){
        printk(KERN_INFO "%s %dst transfer: pl addr %#llx, ps physical addr %#llx, size %#x\n\n", dir[pchannel_p->direction], cnt, pl_addr, pchannel_p->dma_handle, pchannel_p->interface_p->length);   
        //one possible is left to transfer is less than max_transfer_size, one possible is the origin to transfer is less than max_transfer_size
        start_transfer(pchannel_p, pl_addr, pchannel_p->interface_p->length);
	    wait_for_transfer(pchannel_p);
    }

}

/* The following functions are designed to test the driver from within the device
 * driver without any user space.
 */
static void tx_test( struct dma_proxy_channel *pchannel_p ,int test_size)
{
	pchannel_p->interface_p->length = test_size;
	transfer(pchannel_p);
}
static void test(  struct dma_proxy_channel *pchannel_p, u32 dir)
{
	int i;
	//int test_size = 0x800000;
    	int test_size = 0x1000;

	printk("Starting internal test\n");

	/* Initialize the buffers for the test
	 */
	if(dir == DMA_MEM_TO_DEV){
		for (i = 0; i < test_size; i++) 
			pchannel_p->interface_p->buffer[i] = i;
		tx_test(pchannel_p,test_size);
	}
	else if(dir == DMA_DEV_TO_MEM){
		for (i = 0; i < test_size; i++) 
			pchannel_p->interface_p->buffer[i] = 0;
		
		/* Receive the data that was just sent and looped back */
		pchannel_p->interface_p->length = test_size;
		transfer(pchannel_p);

		/* Verify the receiver buffer matches the transmit buffer to
		 * verify the transfer was good
	 	*/
		for (i = 0; i < test_size; i++)
			if (pchannel_p->interface_p->buffer[i] != (unsigned char)i) {
				printk("buffers not equal, first index = %d\n", i);
				break;
			}
		printk("Internal test complete\n");
		
	}
}

/* Map the memory for the channel interface into user space such that user space can
 * access it using coherent memory which will be non-cached for s/w coherent systems
 * such as Zynq 7K or the current default for Zynq MPSOC. MPSOC can be h/w coherent
 * when set up and then the memory will be cached.
 */
static int mmap(struct file *file_p, struct vm_area_struct *vma)
{
	struct dma_proxy_channel *pchannel_p = (struct dma_proxy_channel *)file_p->private_data;

	return dma_mmap_coherent(pchannel_p->dma_device_p, vma,
					   pchannel_p->interface_p, pchannel_p->interface_phys_addr,
					   vma->vm_end - vma->vm_start);
}

/* Open the device file and set up the data pointer to the proxy channel data for the
 * proxy channel such that the ioctl function can access the data structure later.
 */
static int local_open(struct inode *ino, struct file *file)
{
	file->private_data = container_of(ino->i_cdev, struct dma_proxy_channel, cdev);

	return 0;
}

/* Close the file and there's nothing to do for it
 */
static int release(struct inode *ino, struct file *file)
{
	struct dma_proxy_channel *pchannel_p = (struct dma_proxy_channel *)file->private_data;
	struct dma_device *dma_device = pchannel_p->channel_p->device;

	/* Stop all the activity when the channel is closed assuming this
	 * may help if the application is aborted without normal closure
	 */

	dma_device->device_terminate_all(pchannel_p->channel_p);
	return 0;
}

/* Perform I/O control to start a DMA transfer.
 */
static long ioctl(struct file *file, unsigned int unused , unsigned long arg)
{
	struct dma_proxy_channel *pchannel_p = (struct dma_proxy_channel *)file->private_data;

	/* Perform the DMA transfer on the specified channel blocking til it completes
	 */
	transfer(pchannel_p);

	return 0;
}
static struct file_operations dm_fops = {
	.owner    = THIS_MODULE,
	.open     = local_open,
	.release  = release,
	.unlocked_ioctl = ioctl,
	.mmap	= mmap
};


/* Initialize the driver to be a character device such that is responds to
 * file operations.
 */
static int cdevice_init(struct dma_proxy_channel *pchannel_p, char *name)
{
	int rc;
	char class_name[32] = "dma_proxy_";
	static struct class *local_class_p = NULL;

	/* Allocate a character device from the kernel for this driver.
	 */
	rc = alloc_chrdev_region(&pchannel_p->dev_node, 0, 1, "dma_proxy");

	if (rc) {
		dev_err(pchannel_p->dma_device_p, "unable to get a char device number\n");
		return rc;
	}

	/* Initialize the device data structure before registering the character 
	 * device with the kernel.
	 */
	cdev_init(&pchannel_p->cdev, &dm_fops);
	pchannel_p->cdev.owner = THIS_MODULE;
	rc = cdev_add(&pchannel_p->cdev, pchannel_p->dev_node, 1);

	if (rc) {
		dev_err(pchannel_p->dma_device_p, "unable to add char device\n");
		goto init_error1;
	}

	/* Only one class in sysfs is to be created for multiple channels,
	 * create the device in sysfs which will allow the device node
	 * in /dev to be created
	 */
	//strcat(class_name, name);
	if (!local_class_p) {
		//local_class_p = class_create(THIS_MODULE, class_name);
		local_class_p = class_create(THIS_MODULE, DRIVER_NAME);

		if (IS_ERR(pchannel_p->dma_device_p->class)) {
			dev_err(pchannel_p->dma_device_p, "unable to create class\n");
			rc = ERROR;
			goto init_error2;
		}
	}
	pchannel_p->class_p = local_class_p;

	/* Create the device node in /dev so the device is accessible
	 * as a character device
	 */
	pchannel_p->proxy_device_p = device_create(pchannel_p->class_p, NULL,
					  	 pchannel_p->dev_node, NULL, name);

	if (IS_ERR(pchannel_p->proxy_device_p)) {
		dev_err(pchannel_p->dma_device_p, "unable to create the device\n");
		goto init_error3;
	}

	return 0;

init_error3:
	class_destroy(pchannel_p->class_p);

init_error2:
	cdev_del(&pchannel_p->cdev);

init_error1:
	unregister_chrdev_region(pchannel_p->dev_node, 1);
	return rc;
}

/* Exit the character device by freeing up the resources that it created and
 * disconnecting itself from the kernel.
 */
static void cdevice_exit(struct dma_proxy_channel *pchannel_p)
{
	/* Take everything down in the reverse order
	 * from how it was created for the char device
	 */
	static int i = 0;
	if (pchannel_p->proxy_device_p) {
		device_destroy(pchannel_p->class_p, pchannel_p->dev_node);
		if(i==1)
			class_destroy(pchannel_p->class_p);

		cdev_del(&pchannel_p->cdev);
		unregister_chrdev_region(pchannel_p->dev_node, 1);
	}
	i++;
}

/* Create a DMA channel by getting a DMA channel from the DMA Engine and then setting
 * up the channel as a character device to allow user space control.
 */
static int create_channel(struct platform_device *pdev, struct dma_proxy_channel *pchannel_p, char *name, u32 direction)
{
	int rc;

	/* Request the DMA channel from the DMA engine and then use the device from
	 * the channel for the proxy channel also.
	 */
	pchannel_p->channel_p = dma_request_slave_channel(&pdev->dev, name);
	if (!pchannel_p->channel_p) {
		dev_err(pchannel_p->dma_device_p, "DMA channel request error\n");
		return ERROR;
	}
	/* Initialize the character device for the dma proxy channel
	 */
	rc = cdevice_init(pchannel_p, name);
	if (rc) 
		return rc;

	pchannel_p->direction = direction;

	/* Allocate DMA memory for the proxy channel interface.
	 */
	pchannel_p->interface_p = (struct dma_proxy_channel_interface *)
		dmam_alloc_coherent(pchannel_p->dma_device_p,
					sizeof(struct dma_proxy_channel_interface),
					&pchannel_p->interface_phys_addr, GFP_KERNEL);
		printk(KERN_INFO "Allocating uncached memory at virtual address %p, physical address %#llx\n", 
			pchannel_p->interface_p, pchannel_p->interface_phys_addr);

	if (!pchannel_p->interface_p) {
		dev_err(pchannel_p->dma_device_p, "DMA allocation error\n");
		return ERROR;
	}
	return 0;
}

/* Initialize the dma proxy device driver module.
 */
static int dma_proxy_probe(struct platform_device *pdev)
{
	int rc;
	struct dma_proxy_channel *chandev;

	const char *nodeName = NULL;
	u32 dir = 0;

	printk(KERN_INFO "dma_proxy module initialized\n");
	chandev = devm_kzalloc(&pdev->dev, sizeof(*chandev), GFP_KERNEL);
	if (!chandev)
                return -ENOMEM;
	chandev->dma_device_p = &pdev->dev; 
	platform_set_drvdata(pdev, chandev);
	
	of_property_read_string_index(pdev->dev.of_node, "dma-names", 0, &nodeName);

	if(!strcmp(nodeName,"h2c")){
		printk("h2c\n");
		dir = DMA_MEM_TO_DEV;
	}
	
	else if(!strcmp(nodeName,"c2h")){
		printk("c2h\n");
		dir = DMA_DEV_TO_MEM;
	}
	// Create the channel.
	rc = create_channel(pdev, chandev, nodeName, dir);

	if (rc) 
		return rc;

	if (internal_test)
		test(chandev, dir);

	return 0;
}
 
/* Exit the dma proxy device driver module.
 */
static int dma_proxy_remove(struct platform_device *pdev)
{

	printk(KERN_INFO "dma_proxy module exited\n");
	struct dma_proxy_channel *chandev  = platform_get_drvdata(pdev);

	if (chandev->proxy_device_p)
		cdevice_exit(chandev);
	
	if (chandev->channel_p) {
		chandev->channel_p->device->device_terminate_all(chandev->channel_p);
		dma_release_channel(chandev->channel_p);
	}
	return 0;
}

static const struct of_device_id dma_proxy_of_ids[] = {
	{ .compatible = "xlnx,dma_proxy",},
	{}
};

static struct platform_driver dma_proxy_driver = {
	.driver = {
		.name = "dma_proxy_driver",
		.owner = THIS_MODULE,
		.of_match_table = dma_proxy_of_ids,
	},
	.probe = dma_proxy_probe,
	.remove = dma_proxy_remove,
};

static int __init dma_proxy_init(void)
{
	return platform_driver_register(&dma_proxy_driver);

}

static void __exit dma_proxy_exit(void)
{
	platform_driver_unregister(&dma_proxy_driver);
}

module_init(dma_proxy_init)
module_exit(dma_proxy_exit)

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("DMA Proxy Prototype");
MODULE_LICENSE("GPL v2");
