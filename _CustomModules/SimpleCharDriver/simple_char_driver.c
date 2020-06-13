#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define  CHAR_DEV_NAME	    "char_dev"
#define  CHAR_CLASS_NAME    "char_class"

MODULE_LICENSE		("GPL");
MODULE_AUTHOR		("AMEER HAMZA");
MODULE_DESCRIPTION	("Simple char driver");

static int major_num = 0;
static struct class *char_class = NULL;
static struct device *char_device = NULL;
static int dev_open_cnter = 0;
//static char buffer_drv_write[256];
//static char buffer_drv_read[256];
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
static int __init char_drv_init(void)
{
	printk(KERN_INFO "Hello to Char Driver\n");
	major_num = register_chrdev(0, CHAR_DEV_NAME, &fops);
	if (major_num < 0)
	{
		printk(KERN_INFO "Problem while register character device.\n");
		printk(KERN_INFO "Exiting.\n");
		return -1;
	}

	char_class = class_create(THIS_MODULE, CHAR_CLASS_NAME);

	if (IS_ERR(char_class))
	{
		unregister_chrdev(major_num, CHAR_DEV_NAME);
		printk(KERN_INFO "Error creating class.\n");
		return PTR_ERR(char_class);
	}

	char_device = device_create(char_class, NULL, MKDEV(major_num, 0), NULL, CHAR_DEV_NAME);

	if (IS_ERR(char_device))
	{
		class_destroy(char_class);
		unregister_chrdev(major_num, CHAR_DEV_NAME);
		printk(KERN_INFO "Error creating class.\n");
		return PTR_ERR(char_device);
	}

	printk(KERN_INFO "Device Register & Created successfully\n");
	return 0;
}
static void __exit char_drv_exit(void)
{
	device_destroy(char_class,MKDEV(major_num, 0));
	class_unregister(char_class);
	class_destroy(char_class);
	unregister_chrdev(major_num, CHAR_DEV_NAME);
	printk(KERN_INFO "Exiting from Char Driver\n");
	printk(KERN_INFO "Simple Char driver is unloaded\n");
}
static int dev_open(struct inode *inodep, struct file *filep)
{

	printk(KERN_INFO "Char device open - %d times\n", ++dev_open_cnter);
	return 0;
}
static int dev_release (struct inode *inodep, struct file *filep)
{
	printk(KERN_INFO "Char Device is closed\n");
	return 0;
}

static ssize_t dev_read (struct file *filep, char *buffer, size_t len, loff_t *offset)
{
	int ret = copy_to_user(buffer, buffer_drv_shared, shared_message_len);
	if (ret == 0)
	{
		printk(KERN_INFO "Driver has sent message of length %d\n", shared_message_len);
	}
	else
	{
		printk(KERN_INFO "Error Reading from device\n");
	}
	return 0;
}
static ssize_t dev_write (struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
	unsigned long ret = copy_from_user(buffer_drv_shared, buffer,len);
	shared_message_len = strlen(buffer_drv_shared);
	if (ret == 0)
	{
		printk(KERN_INFO "Driver has received message of length %d\n", shared_message_len );
	}
	else
	{
		printk(KERN_INFO "Error Writing to device\n");
	}
	return len;
}

module_init(char_drv_init);
module_exit(char_drv_exit);

