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
#include <linux/semaphore.h>
#include <asm/uaccess.h>

#define DEVICE_NAME "my_pipe"
#define SU_ID 0

/* связанный список указателей на буферы
 * для каждого пользователя
 */
static DECLARE_WAIT_QUEUE_HEAD(wq);
static DECLARE_WAIT_QUEUE_HEAD(rq);
struct task_struct;
struct semaphore sem;

struct user_buf {
	kuid_t user_id;
	char *buf;
	unsigned long w_index;
	unsigned long r_index;
	struct list_head list;
};
static struct user_buf uB;

//число пользователей
static int count;

static int buf_size = 1;
module_param(buf_size, int, 0000);

static dev_t pipe_dev;
static struct cdev my_cdev;

//func
static int pipe_open(struct inode *i, struct file *f);
static int pipe_release(struct inode *i, struct file *f);
static ssize_t pipe_read(struct file *f, char __user *bf,
size_t sz, loff_t *off);
static ssize_t pipe_write(struct file *f, const char __user *bf,
size_t sz, loff_t *off);
static int su_pipe_release(struct inode *i, struct file *f);
static ssize_t su_pipe_read(struct file *f, char __user *bf,
size_t sz, loff_t *off);
static ssize_t su_pipe_write(struct file *f, const char __user *bf,
size_t sz, loff_t *off);

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = pipe_open,
	.read = pipe_read,
	.write = pipe_write,
	.release = pipe_release,
};

static const struct file_operations su_fops = {
	.owner = THIS_MODULE,
	.open = pipe_open,
	.read = su_pipe_read,
	.write = su_pipe_write,
	.release = su_pipe_release,
};

static int pipe_open(struct inode *i, struct file *f)
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
			//pr_info("id = %d\n", id.val);
			if (id.val == SU_ID) {
				pr_info("My_pipe: device has been opened for superuser\n");
				f->f_op = &su_fops;
			} else
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
	//pr_info("id = %d\n", id.val);
	if (id.val == SU_ID) {
		pr_info("Superuser open\n");
		f->f_op = &su_fops;
	} else
		pr_info("My_pipe: device has been opened for this user\n");
	return 0;
}

static int pipe_release(struct inode *i, struct file *f)
{
	kuid_t id = current_uid();
	struct user_buf *u;

	// writer
	if (f->f_mode & FMODE_WRITE) {
		if (down_interruptible(&sem))
		return -ERESTARTSYS;

		list_for_each_entry(u, &uB.list, list) {
			if (u->user_id.val == id.val) {
				//некуда писать
				while ((u->w_index + 1) % buf_size ==
				u->r_index) {
					up(&sem);
					//pr_info("Write sleep\n");
					wait_event_interruptible(wq,
					(u->w_index + 1) % buf_size
					!= u->r_index);
					if (down_interruptible(&sem))
						return -ERESTARTSYS;
					//return 0;
				}
				u->buf[u->w_index] = 0;
				//pr_info("My_pipe: write %c\n",
					//u->buf[u->w_index]);
				u->w_index++;
				if (u->w_index == buf_size)
					u->w_index = 0;
				//pr_info("w_index = %ld\n", u->w_index);
				//pr_info("r_index = %ld\n", u->r_index);

				goto w_rdy;
			}
		}
	}
w_rdy:
	up(&sem);
	wake_up_interruptible(&rq);
	pr_info("My_pipe: device closed for this user\n");
	return 0;
}

static ssize_t pipe_read(struct file *f, char __user *bf,
size_t sz, loff_t *off) {
	/* читаем пользователя
	 * и читаем только из его буфера
	 */

	kuid_t id = current_uid();
	struct user_buf *u;

	if (down_interruptible(&sem))
		return -ERESTARTSYS;

	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == id.val) {
			// нечего читать
			while (u->r_index == u->w_index) {
				up(&sem);
				//pr_info("Read sleep\n");
				if (wait_event_interruptible(rq,
				(u->r_index != u->w_index))) {
				// если было прерывание
					return -ERESTARTSYS;
				}
				if (down_interruptible(&sem))
					return -ERESTARTSYS;
			}
			if (u->buf[u->r_index] == 0) {
				//pr_info("EOF\n");
				u->r_index++;
				if (u->r_index == buf_size)
					u->r_index = 0;
				return 0;
			}
			copy_to_user(bf, &u->buf[u->r_index], 1);
			//pr_info("My_pipe: read %c\n", u->buf[u->r_index]);
			u->r_index++;
			if (u->r_index == buf_size)
				u->r_index = 0;
			//pr_info("w_index = %ld\n", u->w_index);
			//pr_info("r_index = %ld\n", u->r_index);

			goto r_rdy;
		}
	}
r_rdy:
	up(&sem);
	wake_up_interruptible(&wq);
	return 1;
}

static ssize_t pipe_write(struct file *f, const char __user *bf,
size_t sz, loff_t *off) {
	/* читаем пользователя
	 * и пишем именно в его буфер
	 */

	kuid_t id = current_uid();
	struct user_buf *u;

	if (down_interruptible(&sem))
		return -ERESTARTSYS;

	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == id.val) {
			//некуда писать
			while ((u->w_index + 1) % buf_size == u->r_index) {
				up(&sem);
				//pr_info("Write sleep\n");
				wait_event_interruptible(wq,
				(u->w_index + 1) % buf_size != u->r_index);
				if (down_interruptible(&sem))
					return -ERESTARTSYS;
				//return 0;
			}
			copy_from_user(&u->buf[u->w_index], bf, 1);
			//pr_info("My_pipe: write %c\n",
				//u->buf[u->w_index]);
			u->w_index++;
			if (u->w_index == buf_size)
				u->w_index = 0;
			//pr_info("w_index = %ld\n", u->w_index);
			//pr_info("r_index = %ld\n", u->r_index);

			goto w_rdy;
		}
	}
w_rdy:
	up(&sem);
	wake_up_interruptible(&rq);
	return 1;
}

//su file operations

static int su_pipe_release(struct inode *i, struct file *f)
{
	struct user_buf *u;

	// writer
	if (f->f_mode & FMODE_WRITE) {
		if (down_interruptible(&sem))
		return -ERESTARTSYS;

		list_for_each_entry(u, &uB.list, list) {
			if (u->user_id.val == SU_ID) {
				//некуда писать
				while ((u->w_index + 1) % buf_size ==
				u->r_index) {
					up(&sem);
					//pr_info("Write sleep\n");
					wait_event_interruptible(wq,
					(u->w_index + 1) % buf_size !=
					u->r_index);
					if (down_interruptible(&sem))
						return -ERESTARTSYS;
					//return 0;
				}
				u->buf[u->w_index] = 0;
				//pr_info("My_pipe: write %c\n",
					//u->buf[u->w_index]);
				u->w_index++;
				if (u->w_index == buf_size)
					u->w_index = 0;
				//pr_info("w_index = %ld\n", u->w_index);
				//pr_info("r_index = %ld\n", u->r_index);


				goto w_rdy;
			}
		}
	}
w_rdy:
	up(&sem);
	wake_up_interruptible(&rq);
	pr_info("My_pipe: device closed for superuser\n");
	return 0;
}

static ssize_t su_pipe_read(struct file *f, char __user *bf,
size_t sz, loff_t *off) {
	/* читаем пользователя
	 * и читаем только из его буфера
	 */
	struct user_buf *u;

	if (down_interruptible(&sem))
		return -ERESTARTSYS;

	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == SU_ID) {
			// нечего читать
			while (u->r_index == u->w_index) {
				up(&sem);
				//pr_info("Read sleep\n");
				if (wait_event_interruptible(rq,
				(u->r_index != u->w_index))) {
				// если было прерывание
					return -ERESTARTSYS;
				}
				if (down_interruptible(&sem))
					return -ERESTARTSYS;
			}
			if (u->buf[u->r_index] == 0) {
				//pr_info("EOF\n");
				u->r_index++;
				if (u->r_index == buf_size)
					u->r_index = 0;
				return 0;
			}
			copy_to_user(bf, &u->buf[u->r_index], 1);
			//pr_info("My_pipe: read %c\n", u->buf[u->r_index]);
			u->r_index++;
			if (u->r_index == buf_size)
				u->r_index = 0;
			//pr_info("w_index = %ld\n", u->w_index);
			//pr_info("r_index = %ld\n", u->r_index);

			goto r_rdy;
		}
	}
r_rdy:
	up(&sem);
	wake_up_interruptible(&wq);
	return 1;
}

static ssize_t su_pipe_write(struct file *f, const char __user *bf,
size_t sz, loff_t *off) {
	/* читаем пользователя
	 * и пишем именно в его буфер
	 */
	struct user_buf *u;

	if (down_interruptible(&sem))
		return -ERESTARTSYS;

	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == SU_ID) {
			//некуда писать
			while ((u->w_index + 1) % buf_size == u->r_index) {
				up(&sem);
				//pr_info("Write sleep\n");
				wait_event_interruptible(wq,
				(u->w_index + 1) % buf_size != u->r_index);
				if (down_interruptible(&sem))
					return -ERESTARTSYS;
				//return 0;
			}
			copy_from_user(&u->buf[u->w_index], bf, 1);
			//pr_info("My_pipe: write %c\n",
				//u->buf[u->w_index]);
			u->w_index++;
			if (u->w_index == buf_size)
				u->w_index = 0;
			//pr_info("w_index = %ld\n", u->w_index);
			//pr_info("r_index = %ld\n", u->r_index);

			goto w_rdy;
		}
	}
w_rdy:
	up(&sem);
	wake_up_interruptible(&rq);
	return 1;
}

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
	sema_init(&sem, 1);

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
