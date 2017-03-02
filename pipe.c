/* pipe.c */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#define DEVICE_NAME "my_pipe"

//здесь будет связанный список указателей на буферы (циклические) для каждого пользователя

struct task_struct;

struct user_buf {
	kuid_t user_id;
	unsigned char *buf;
	unsigned long w_index;
	unsigned long r_index;
	struct list_head list;
};
static struct user_buf uB;

//число пользователей
static int count = 0;
module_param(count, int, 0);

static int buf_size = 1;
module_param(buf_size, int, 0);

static dev_t pipe_dev;
static struct cdev my_cdev;

int pipe_open(struct inode *i, struct file *f) {
	//читаем пользователя и проверяем, есть ли он в базе
	kuid_t id = current_uid();
	struct list_head *node;
	struct user_buf *u;
	list_for_each(node, &uB.list) {
		u = list_entry(node, struct user_buf, list);
		//если в базе есть, то говорим, что уже открыто
		if (u->user_id.val == id.val) {
			printk(KERN_INFO "My_pipe: device has been opened for this user\n");
			return 0;
		}
	}
	
	//иначе увеличиваем количество пользователей на 1 и добавляем новый указатель на буфер, делаем кмаллок
	u = kmalloc(sizeof(*u), GFP_KERNEL);
	u->user_id = id;
	u->buf = kmalloc(buf_size*sizeof(char), GFP_KERNEL);
	u->w_index = 0;
	u->r_index = 0;
	INIT_LIST_HEAD(&u->list);
	list_add(&u->list, &uB.list);
	count++;
	printk(KERN_INFO "My_pipe: device has been opened for this user\n");
	return 0;
}

int pipe_release(struct inode *i, struct file *f) {
	
	/*	
	//очищаем буфер нужного пользователя, если count > 0, и вычитаем 1
	kuid_t id = current_uid();
	//struct list_head *node;
	struct user_buf *u, *tmp;
	list_for_each_entry_safe(u, tmp, &uB.list, list) {
		//u = list_entry(node, struct user_buf, list);
		//если в базе есть, то можно закрывать
		if (u->user_id.val == id.val) {
			count--;
			list_del(&u->list);
			kfree(u->buf);
			kfree(u);
			printk(KERN_ALERT "My_pipe: device has been closed for this user\n");
			return 0;
		}
	}
	
	//не было открыто
	printk(KERN_ALERT "My_pipe: nothing to close\n");
	return -1;
	*/
	printk(KERN_INFO "My_pipe: device has been closed for this user\n");
	return 0;
}

ssize_t pipe_read(struct file *f, char __user *bf, size_t sz, loff_t *off) {
	//читаем пользователя и читаем только из его буфера
	kuid_t id = current_uid();
	struct user_buf *u;
	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == id.val) {
			//читаем
			if (u->r_index == u->w_index) { return 0; } // нечего читать, здесь надо ждать
			else {
				copy_to_user(bf, &u->buf[u->r_index], 1);
				printk(KERN_INFO "My_pipe: read %c\n", u->buf[u->r_index]);
				u->r_index++;
				if (u->r_index == buf_size) u->r_index = 0;
				goto r_rdy;
			}
		}
	}
	r_rdy:
	return 1;
}

ssize_t pipe_write(struct file *f, const char __user *bf, size_t sz, loff_t *off) {
	//читаем пользователя и пишем именно в его буфер
	kuid_t id = current_uid();
	struct user_buf *u;
	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == id.val) {
			//пишем
			if (u->r_index - u->w_index == 1) {} // некуда писать, здесь надо ждать
			else {
				copy_from_user(&u->buf[u->w_index], bf, 1);
				printk(KERN_INFO "My_pipe: write %c\n", u->buf[u->w_index]);
				u->w_index++;
				if (u->w_index == buf_size) u->w_index = 0;
				goto w_rdy;
			}
		}
	}
	w_rdy:
	return 1;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = pipe_open,
	.read = pipe_read,
	.write = pipe_write,
	.release = pipe_release,
};

static int __init char_device_init(void) {
	
	int ret;
	
	// Dynamic major number
	ret = alloc_chrdev_region(&pipe_dev, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		printk(KERN_ALERT "My_pipe: Failed to get a major number\n");
		return -1;
	}
	printk(KERN_INFO "My_pipe: Registered correctly with major: %d and minor: %d number\n", MAJOR(pipe_dev), MINOR(pipe_dev));
	
	// cdev init
	cdev_init(&my_cdev, &fops);
	my_cdev.owner = THIS_MODULE;
	ret = cdev_add(&my_cdev, pipe_dev, 1);
	if (ret < 0) {
		printk(KERN_ALERT "My_pipe: Failed to init cdev struct\n");
		return -1;
	}
	
	// Check buffer size
	printk(KERN_INFO "My_pipe: Pipe init\n");
	if (buf_size <= 0) {
		printk(KERN_ALERT "My_pipe: Wrong buffer size!\n");
		return -1;	
	}
	printk(KERN_INFO "My_pipe: Buffer size is %d\n", buf_size);
	buf_size++; // для нормальной работы условий
	
	INIT_LIST_HEAD(&uB.list);
	
	printk(KERN_INFO "My_pipe: init completed\n");

	return 0;
}

static void __exit char_device_exit(void) {
	//очищаем буферы всех пользователей
	struct user_buf *u, *tmp;
	printk(KERN_INFO "My_pipe: cleaning lisked list...\n");
	list_for_each_entry_safe(u, tmp, &uB.list, list) {
		list_del(&u->list);
		kfree(u->buf);
		kfree(u);
	}
	cdev_del(&my_cdev);
	unregister_chrdev_region(pipe_dev, 1);
	printk(KERN_INFO "My_pipe: uninit completed\n");
}

module_init(char_device_init);
module_exit(char_device_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Character device module/My pipe");
MODULE_AUTHOR("Sergey Samokhvalov/Ilya Vedmanov");
