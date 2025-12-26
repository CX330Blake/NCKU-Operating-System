#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <asm/current.h>
#include <linux/string.h>

#define procfs_name "Mythread_info"
#define BUFSIZE 1024
char buf[BUFSIZE]; //kernel buffer
static struct proc_dir_entry *proc_entry;

static ssize_t Mywrite(struct file *fileptr, const char __user *ubuf,
		       size_t ubuffer_len, loff_t *offset)
{
	/*Your code here*/
	size_t buffer_len = strnlen(buf, BUFSIZE);
	size_t space_left;
	size_t copy_len;

	if (buffer_len >= BUFSIZE)
		return -ENOSPC;

	space_left = BUFSIZE - buffer_len - 1;
	copy_len = min(ubuffer_len, space_left);
	if (copy_len == 0)
		return -ENOSPC;

	if (copy_from_user(&buf[buffer_len], ubuf, copy_len))
		return -EFAULT;
	buffer_len += copy_len;
	buf[buffer_len] = '\0';

	buffer_len += scnprintf(&buf[buffer_len], BUFSIZE - buffer_len,
				"PID: %d, TID: %d, Time: %llu\n",
				current->tgid, current->pid,
				current->utime / 100 / 1000);

	return copy_len;
	/****************/
}

static ssize_t Myread(struct file *fileptr, char __user *ubuf,
		      size_t buffer_len, loff_t *offset)
{
	/*Your code here*/
	size_t data_len = strnlen(buf, BUFSIZE);

	if (*offset > 0)
		return 0;

	if (buffer_len < data_len)
		return -EINVAL;

	if (copy_to_user(ubuf, buf, data_len))
		return -EFAULT;

	*offset = data_len;
	memset(buf, 0, sizeof(buf));

	return *offset;
	/****************/
}

static struct proc_ops Myops = {
	.proc_read = Myread,
	.proc_write = Mywrite,
};

static int My_Kernel_Init(void)
{
	remove_proc_entry(procfs_name, NULL);
	proc_entry = proc_create(procfs_name, 0644, NULL, &Myops);
	if (!proc_entry)
		return -ENOMEM;

	pr_info("My kernel says Hi");
	return 0;
}

static void My_Kernel_Exit(void)
{
	proc_remove(proc_entry);
	pr_info("My kernel says GOODBYE");
}

module_init(My_Kernel_Init);
module_exit(My_Kernel_Exit);

MODULE_LICENSE("GPL");
