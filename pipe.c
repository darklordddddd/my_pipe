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
#include <linux/string.h>
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
	int eof_flag;
};
static struct user_buf uB;

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
			if  (f->f_mode & FMODE_WRITE)
				u->eof_flag = 0;
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
	u->eof_flag = 0;

	INIT_LIST_HEAD(&u->list);
	list_add(&u->list, &uB.list);
	//pr_info("id = %d\n", id.val);
	if (id.val == SU_ID) {
		pr_info("Superuser open\n");
		// подмена структуры файл. операций
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
		list_for_each_entry(u, &uB.list, list) {
			if (u->user_id.val == id.val) {
				u->eof_flag = 1;
				goto all;
			}
		}
	}
all:
	wake_up_interruptible(&rq);
	pr_info("My_pipe: device closed for this user\n");
	return 0;
}

static ssize_t pipe_read(struct file *f, char __user *bf,
size_t sz, loff_t *off) {

	kuid_t id = current_uid();
	struct user_buf *u;
	size_t bytes_read = 0;
	size_t delta;
	size_t i;
	unsigned long j;

	/* выделяем временный буфер для чтения
	 * данных с пользователя
	 */
	char *temp = kmalloc(sz, GFP_KERNEL);

	if (temp == NULL)
		return -1;

	//pr_info("size = %d\n", sz);

	// семафор = 0 (заблокирован)
	if (down_interruptible(&sem))
		return -ERESTARTSYS;

	// ищем нужного пользователя и его буфер
	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == id.val) {
			// если все прочитано (для eof)
			if ((u->eof_flag) &&
			 (u->r_index == u->w_index)) {
				u->eof_flag = 0;
				up(&sem);
				kfree(temp);
				return 0;
			}
			while (sz != bytes_read) {
				// нечего читать
				while (u->r_index == u->w_index) {
					up(&sem);
					//pr_info("Read sleep\n");
					if (wait_event_interruptible(rq,
					(u->r_index != u->w_index)
					| (u->eof_flag))) {
// если было прерывание
						return -ERESTARTSYS;
					}
					if (u->r_index == u->w_index) {
						copy_to_user
						(bf, temp, bytes_read);
						kfree(temp);
						return (ssize_t)bytes_read;
					}
					if (down_interruptible(&sem))
						return -ERESTARTSYS;
				}

				//что больше:
				//sz или доступное
				//доступное место
				delta = (size_t)((buf_size
				+ u->w_index - u->r_index) % buf_size);

				if (sz - bytes_read >= delta) {
// sz больше, читаем доступное
					for (i = 0, j = u->r_index; i < delta;
					i++, j = (j + 1) % buf_size)
						temp[bytes_read + i] =
						u->buf[j];

					bytes_read += delta;
					u->r_index = u->w_index;
				} else {
// sz меньше, читаем до конца
					delta = sz - bytes_read;
					for (i = 0, j = u->r_index; i < delta;
					i++, j = (j + 1) % buf_size)
						temp[bytes_read + i] =
						u->buf[j];

					u->r_index = (u->r_index +
					(unsigned long)delta) % buf_size;
					bytes_read = sz;
				}
				wake_up_interruptible(&wq);
			}
			goto r_rdy;
		}
	}
r_rdy:
	copy_to_user(bf, temp, bytes_read);
	kfree(temp);
	up(&sem);
	wake_up_interruptible(&wq);
	return (ssize_t)bytes_read;
}

static ssize_t pipe_write(struct file *f, const char __user *bf,
size_t sz, loff_t *off) {

	kuid_t id = current_uid();
	struct user_buf *u;
	size_t bytes_written = 0;
	size_t delta;
	size_t i;
	unsigned long j;

	/* выделяем временный буфер для чтения
	 * данных с пользователя
	 */
	char *temp = kmalloc(sz, GFP_KERNEL);

	if (temp == NULL)
		return -1;

	// копируем данные с пользователя
	copy_from_user(temp, bf, sz);

// семафор = 0 (заблокирован)
	if (down_interruptible(&sem))
		return -ERESTARTSYS;

// ищем нужного пользователя и его буфер
	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == id.val) {
			while (sz != bytes_written) {
				//некуда писать, ждем
				while ((u->w_index + 1)
				% buf_size == u->r_index) {
					up(&sem);
					//pr_info("Write sleep\n");
					wait_event_interruptible(wq,
					(u->w_index + 1) % buf_size
					!= u->r_index);
					if (down_interruptible(&sem))
						return -ERESTARTSYS;
				}

// что больше: sz или доступное
				delta = (size_t)((buf_size + u->r_index
				- u->w_index - 1) % buf_size);
				if (sz - bytes_written >= delta) {
// sz больше доступного, пишем в доступное
					for (i = 0, j = u->w_index; i < delta;
					i++, j = (j + 1) % buf_size)
						u->buf[j] =
						temp[bytes_written + i];

					bytes_written += delta;
					u->w_index = (u->w_index
					+ (unsigned long)delta) % buf_size;
				} else {
// sz меньше доступного, пишем, сколько есть
					delta = sz - bytes_written;
					for (i = 0, j = u->w_index;
					i < delta; i++, j = (j + 1) % buf_size)
						u->buf[j] =
						temp[bytes_written + i];

					u->w_index = (u->w_index
					+ (unsigned long)delta) % buf_size;
					bytes_written = sz;
				}
// "пробуждаем" очередь чтения
				wake_up_interruptible(&rq);
			}
			goto w_rdy;
		}
	}
w_rdy:
// освобождаем
	kfree(temp);
	up(&sem);
	wake_up_interruptible(&rq);
	return (ssize_t)bytes_written;
}

//su file operations

static int su_pipe_release(struct inode *i, struct file *f)
{
	struct user_buf *u;

	// writer
	if (f->f_mode & FMODE_WRITE) {
		list_for_each_entry(u, &uB.list, list) {
			if (u->user_id.val == SU_ID) {
				u->eof_flag = 1;
				goto all;
			}
		}
	}
all:
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
	size_t bytes_read = 0;
	size_t delta;
	size_t i;
	unsigned long j;

	/* выделяем временный буфер для чтения
	 * данных с пользователя
	 */
	char *temp = kmalloc(sz, GFP_KERNEL);

	if (temp == NULL)
		return -1;


// семафор = 0 (заблокирован)
	if (down_interruptible(&sem))
		return -ERESTARTSYS;

// ищем нужного пользователя и его буфер
	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == SU_ID) {
// если только все прочитано (для eof)
			if ((u->eof_flag) &&
			(u->r_index == u->w_index)) {
				u->eof_flag = 0;
				up(&sem);
				kfree(temp);
				return 0;
			}
			while (sz != bytes_read) {
				// нечего читать
				while (u->r_index == u->w_index) {
					up(&sem);
					//pr_info("Read sleep\n");
					if (wait_event_interruptible(rq,
					(u->r_index != u->w_index) |
					(u->eof_flag))) {
// если было прерывание
						return -ERESTARTSYS;
					}
					if (u->r_index == u->w_index) {
						copy_to_user
						(bf, temp, bytes_read);
						kfree(temp);
						return (ssize_t)bytes_read;
					}
					if (down_interruptible(&sem))
						return -ERESTARTSYS;
				}

//что больше: sz или доступное
//доступное место
				delta = (size_t)((buf_size +
				u->w_index - u->r_index) % buf_size);

				if (sz - bytes_read >= delta) {
// sz больше, читаем доступное
					for (i = 0, j = u->r_index; i < delta;
					i++, j = (j + 1) % buf_size)
						temp[bytes_read + i] =
						u->buf[j];

					bytes_read += delta;
					u->r_index = u->w_index;
				} else {
// sz меньше, читаем до конца
					delta = sz - bytes_read;
					for (i = 0, j = u->r_index; i < delta;
					i++, j = (j + 1) % buf_size)
						temp[bytes_read + i] =
						u->buf[j];

					u->r_index = (u->r_index +
					(unsigned long)delta) % buf_size;
					bytes_read = sz;
				}

				wake_up_interruptible(&wq);
			}
			goto r_rdy;
		}
	}
r_rdy:
	copy_to_user(bf, temp, bytes_read);
	kfree(temp);
	up(&sem);
	wake_up_interruptible(&wq);
	return (ssize_t)bytes_read;
}

static ssize_t su_pipe_write(struct file *f, const char __user *bf,
size_t sz, loff_t *off) {
	/* читаем пользователя
	 * и пишем именно в его буфер
	 */
	struct user_buf *u;
	size_t bytes_written = 0;
	size_t delta;
	size_t i;
	unsigned long j;

	/* выделяем временный буфер для чтения
	 * данных с пользователя
	 */
	char *temp = kmalloc(sz, GFP_KERNEL);

	if (temp == NULL)
		return -1;

// копируем данные с пользователя
	copy_from_user(temp, bf, sz);

// семафор = 0 (заблокирован)
	if (down_interruptible(&sem))
		return -ERESTARTSYS;

// ищем нужного пользователя и его буфер
	list_for_each_entry(u, &uB.list, list) {
		if (u->user_id.val == SU_ID) {
			while (sz != bytes_written) {
				//некуда писать, ждем
				while ((u->w_index + 1) %
					buf_size == u->r_index) {
					up(&sem);
					//pr_info("Write sleep\n");
					wait_event_interruptible(wq,
					(u->w_index + 1) % buf_size
					!= u->r_index);
					if (down_interruptible(&sem))
						return -ERESTARTSYS;
				}

//что больше: sz или доступное
				delta = (size_t)((buf_size +
				u->r_index - u->w_index - 1) % buf_size);
				if (sz - bytes_written >= delta) {
// sz больше доступного, пишем в доступное
					for (i = 0, j = u->w_index; i < delta;
					i++, j = (j + 1) % buf_size)
						u->buf[j] =
						temp[bytes_written + i];

					bytes_written += delta;
					u->w_index = (u->w_index +
					(unsigned long)delta) % buf_size;
				} else {
// sz меньше доступного, пишем, сколько есть
					delta = sz - bytes_written;
					for (i = 0, j = u->w_index; i < delta;
					i++, j = (j + 1) % buf_size)
						u->buf[j] =
						temp[bytes_written + i];

					u->w_index = (u->w_index +
					(unsigned long)delta) % buf_size;
					bytes_written = sz;
				}

// "пробуждаем" очередь чтения
				wake_up_interruptible(&rq);
			}
			goto w_rdy;
		}
	}
w_rdy:
// освобождаем
	kfree(temp);
	up(&sem);
	wake_up_interruptible(&rq);
	return (ssize_t)bytes_written;
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

