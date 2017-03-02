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

/* связанный список указателей на буферы
 * для каждого пользователя
 */

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
static int count;
module_param(count, int, 0000);

static int buf_size = 1;
module_param(buf_size, int, 0000);

static dev_t pipe_dev;
static struct cdev my_cdev;

int pipe_open(struct inode *i, struct file *f)
{
	/* читаем пользователя и проверяем,
	 * есть ли он в базе
	 */
	kuid_t id = current_uid();
	struct list_head *node;
	struct user_buf *u;

	list_for_each(node, &uB.list) {
		u = list_entry(node, struct user_buf, list);
		/* если в базе есть, то говорим,
		 * что уже открыто
		 */
		if (u->user_id.val == id.val) {
			pr_info("My_pipe: device has been opened for this user\n");
			return 0;
		}
	}

	/* иначе увеличиваем количество
	 * пользователей на 1 и добавляем
	 * новый указатель на буфер
	 */
	u = kmalloc(sizeof(*u), GFP_KERNEL);
	u->user_id = id;
	u->buf = kmalloc_array(buf_size, sizeof(char), GFP_KERNEL);
	u->w_index = 0;
	u->r_index = 0;
	INIT_LIST_HEAD(&u->list);
	list_add(&u->list, &uB.list);
	count++;
	pr_info("My_pipe: device has been opened for this user\n");
	return 0;
}

int pipe_release(struct inode *i, struct file *f)
{
	pr_info("My_pipe: device closed for this user\n");
	return 0;
}

ssize_t pipe_read(struct file *f, char __user *bf,
size_t sz, loff_t *off) {
	/* читаем пользователя
	 * и читаем только из его буфера
	 */
	kuid_t id = current_uid();
	struct user_buf *u;

	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == id.val) {
			// нечего читать
			if (u->r_index == u->w_index)
				return 0;

			copy_to_user(bf, &u->buf[u->r_index], 1);
			pr_info("My_pipe: read %c\n", u->buf[u->r_index]);
			u->r_index++;
			if (u->r_index == buf_size)
				u->r_index = 0;
			goto r_rdy;
		}
	}
r_rdy:
	return 1;
}

ssize_t pipe_write(struct file *f, const char __user *bf,
size_t sz, loff_t *off) {
	/* читаем пользователя
	 * и пишем именно в его буфер
	 */
	kuid_t id = current_uid();
	struct user_buf *u;

	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == id.val) {
			//некуда писать
			if (u->r_index - u->w_index == 1) {
			} else {
				copy_from_user(&u->buf[u->w_index], bf, 1);
				pr_info("My_pipe: write %c\n",
					u->buf[u->w_index]);
				u->w_index++;
				if (u->w_index == buf_size)
					u->w_index = 0;
				goto w_rdy;
			}
		}
	}
w_rdy:
	return 1;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = pipe_open,
	.read = pipe_read,
	.write = pipe_write,
	.release = pipe_release,
};

static int __init char_device_init(void)
{

	int ret;

	// Dynamic major number
	ret = alloc_chrdev_region(&pipe_dev, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		pr_alert("My_pipe: Failed to get a major number\n");
		return -1;
	}
	pr_info("My_pipe: Registered correctly with major: ");
	pr_info("%d and minor: %d\n", MAJOR(pipe_dev), MINOR(pipe_dev));

	// cdev init
	cdev_init(&my_cdev, &fops);
	my_cdev.owner = THIS_MODULE;
	ret = cdev_add(&my_cdev, pipe_dev, 1);
	if (ret < 0) {
		pr_alert("My_pipe: Failed to init cdev struct\n");
		return -1;
	}

	// Check buffer size
	pr_info("My_pipe: Pipe init\n");
	if (buf_size <= 0) {
		pr_alert("My_pipe: Wrong buffer size!\n");
		return -1;
	}
	pr_info("My_pipe: Buffer size is %d\n", buf_size);
	buf_size++; // для нормальной работы условий

	INIT_LIST_HEAD(&uB.list);

	pr_info("My_pipe: init completed\n");

	return 0;
}

static void __exit char_device_exit(void)
{
	//очищаем буферы всех пользователей
	struct user_buf *u, *tmp;

	pr_info("My_pipe: cleaning lisked list...\n");
	list_for_each_entry_safe(u, tmp, &uB.list, list) {
		list_del(&u->list);
		kfree(u->buf);
		kfree(u);
	}
	cdev_del(&my_cdev);
	unregister_chrdev_region(pipe_dev, 1);
	pr_info("My_pipe: uninit completed\n");
}

module_init(char_device_init);
module_exit(char_device_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Character device module/My pipe");
MODULE_AUTHOR("Sergey Samokhvalov/Ilya Vedmanov");
