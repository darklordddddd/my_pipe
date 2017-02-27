/* pipe.c */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>

#define DEVICE_NAME "my_pipe"

static int buf_size = 1;
static int *buf;
static int pipe_major;
module_param(buf_size, int, 0);

static dev_t pipe_dev;

/*static struct file_operations fops = 
{
	.open = pipe_open,
	.read = pipe_read,
	.write = pipe_write,
	.release = pipe_release,
};*/

static int __init char_device_init(void) {
	
	int ret;
	
	// Dynamic major number
	ret = alloc_chrdev_region(&pipe_dev, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		printk(KERN_ALERT "My_pipe: Failed to get a major number\n");
		return -1;
	}
	printk(KERN_INFO "My_pipe: Registered correctly with major: %d and minor: %d number %d\n", MAJOR(pipe_dev), MINOR(pipe_dev));

	// Check buffer size
	printk(KERN_INFO "My_pipe: Pipe init\n");
	if (buf_size <= 0) {
		printk(KERN_ALERT "My_pipe: Wrong buffer size!\n");
		return -1;	
	}
	printk(KERN_INFO "My_pipe: Buffer size is %d\n", buf_size);
	
	printk(KERN_INFO "My_pipe: init completed\n");

	return 0;
}

static void __exit char_device_exit(void) {
	unregister_chrdev_region(pipe_dev, 1);
	printk(KERN_INFO "My_pipe: uninit completed\n");
}

module_init(char_device_init);
module_exit(char_device_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Character device module/My pipe");
MODULE_AUTHOR("Sergey Samokhvalov/Ilya Vedmanov");
