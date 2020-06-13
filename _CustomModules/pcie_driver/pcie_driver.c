#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define  PCIE_DEV_NAME	    "custom_pcie"
#define  PCIE_CLASS_NAME    "pcie_class"

MODULE_LICENSE		("GPL");
MODULE_AUTHOR		("AMEER HAMZA");
MODULE_DESCRIPTION	("PCIE driver");

static int major_num = 0;
static struct class *pcie_class = NULL;
static struct device *pcie_device = NULL;
static int pcie_device_open_cnter = 0;
static char buffer_drv_shared[256];
static int shared_message_len = 0;

static int     dev_open(struct inode *, struct file *);
static int     dev_release (struct inode *, struct file *);
static ssize_t dev_read (struct file *, char *, size_t, loff_t *);
static ssize_t dev_write (struct file *, const char *, size_t, loff_t *);

static struct file_operations fops =
{
		.open = dev_open,
		.read = dev_read,
		.write = dev_write,
		.release = dev_release,
};
static int __init pcie_drv_init(void)
{
	printk(KERN_INFO "Hello to PCIE Driver\n");
	major_num = register_chrdev(0, PCIE_DEV_NAME, &fops);
	if (major_num < 0)
	{
		printk(KERN_INFO "Problem while register PCIE device.\n");
		printk(KERN_INFO "Exiting.\n");
		return -1;
	}

	pcie_class = class_create(THIS_MODULE, PCIE_CLASS_NAME);

	if (IS_ERR(pcie_class))
	{
		unregister_chrdev(major_num, PCIE_DEV_NAME);
		printk(KERN_INFO "Error creating class.\n");
		return PTR_ERR(pcie_class);
	}

	pcie_device = device_create(pcie_class, NULL, MKDEV(major_num, 0), NULL, PCIE_DEV_NAME);

	if (IS_ERR(pcie_device))
	{
		class_destroy(pcie_class);
		unregister_chrdev(major_num, PCIE_DEV_NAME);
		printk(KERN_INFO "Error creating class.\n");
		return PTR_ERR(pcie_device);
	}

	printk(KERN_INFO "Device Register & Created successfully\n");
	return 0;
}
static void __exit pcie_drv_exit(void)
{
	device_destroy(pcie_class,MKDEV(major_num, 0));
	class_unregister(pcie_class);
	class_destroy(pcie_class);
	unregister_chrdev(major_num, PCIE_DEV_NAME);
	printk(KERN_INFO "Exiting from PCIE Driver\n");
	printk(KERN_INFO "PCIE driver is unloaded\n");
}
static int dev_open(struct inode *inodep, struct file *filep)
{
	printk(KERN_INFO "PCIE device open - %d times\n", ++pcie_device_open_cnter);
	return 0;
}
static int dev_release (struct inode *inodep, struct file *filep)
{
	printk(KERN_INFO "PCIE Device is closed\n");
	return 0;
}

static ssize_t dev_read (struct file *filep, char *buffer, size_t len, loff_t *offset)
{
	copy_to_user(buffer, buffer_drv_shared, shared_message_len);
	return 0;
}
static ssize_t dev_write (struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
	copy_from_user(buffer_drv_shared, buffer,len);
	return len;
}

module_init(pcie_drv_init);
module_exit(pcie_drv_exit);

