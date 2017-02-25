/* pipe.c */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>

#define DEVICE_NAME "my_pipe"
#define CLASS_NAME "pipe"

static int buf_size;
static int *buf;
static int major;
static struct class* pipeClass  = NULL;
static struct device* pipeDevice = NULL; 
module_param(buf_size, int, 0);

static int __init char_device_init(void) {
	// Dynamic major number
	majorNumber = register_chrdev(0, DEVICE_NAME, &fops);
	if (majorNumber < 0) {
		printk(KERN_ALERT "My_pipe: Failed to register a major number\n");
		return -1;
	}
	printk(KERN_INFO "My_pipe: Registered correctly with major number %d\n", majorNumber);
 
	// Register the device class
	pipeClass = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(pipeClass)) {
		unregister_chrdev(majorNumber, DEVICE_NAME);
		printk(KERN_ALERT "My_pipe: Failed to register device class\n");
		return PTR_ERR(pipeClass);
	}
	printk(KERN_INFO "My_pipe: Device class registered correctly\n");
 
	// Register the device driver
	pipeDevice = device_create(pipeClass, NULL, MKDEV(majorNumber, 0), NULL, DEVICE_NAME);
	if (IS_ERR(pipeDevice)) {
		class_destroy(pipeClass);
		unregister_chrdev(majorNumber, DEVICE_NAME);
		printk(KERN_ALERT "My_pipe: Failed to create the device\n");
		return PTR_ERR(pipeDevice);
	}
	printk(KERN_INFO "My_pipe: device class created correctly\n");

	// Check buffer size
	printk(KERN_INFO "My_pipe: Pipe init\n");
	if (buf_size < 0) {
		printk(KERN_ALERT "My_pipe: Wrong buffer size!\n");
		return -1;	
	}
	printk(KERN_INFO "My_pipe: Buffer size is %d\n", buf_size);
	
	printk(KERN_INFO "My_pipe: init completed\n");

	return 0;
}

static void __exit char_device_exit(void) {
	device_destroy(pipeClass, MKDEV(majorNumber, 0));
	class_unregister(pipeClass);
	class_destroy(pipeClass);
	unregister_chrdev(majorNumber, DEVICE_NAME);
	printk(KERN_INFO "My_pipe: uninit completed\n");
}

module_init(char_device_init);
module_exit(char_device_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Character device module/My pipe");
MODULE_AUTHOR("Sergey Samokhvalov/Ilya Vedmanov");
