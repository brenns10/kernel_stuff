#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A proc file which you can write into and retrieve from the core dump.");
MODULE_AUTHOR("Stephen Brennan <stephen@brennan.io>");


DEFINE_MUTEX(crashstash_lock);
int count;
struct proc_dir_entry *pde;

LIST_HEAD(crashstash);
u64 size;
u64 pages;

static void crashstash_free(void)
{
	struct page *cur, *next;

	BUG_ON(!mutex_is_locked(&crashstash_lock));
	list_for_each_entry_safe(cur, next, &crashstash, lru)
	{
		list_del(&cur->lru);
		__free_page(cur);
	}
	size = 0;
	pages = 0;
}

static int crashstash_open(struct inode *inode, struct file *filp)
{
	int rv = 0;
	int mode = filp->f_flags & O_ACCMODE;
	nonseekable_open(inode, filp);
	mutex_lock(&crashstash_lock);
	if (count) {
		rv = -EBUSY;
		goto out;
	}
	/* We won't support read/write mode: either read or write */
	if (mode == O_RDWR) {
		rv = -EINVAL;
		goto out;
	}

	/* Writers automatically clear the old contents and setup a new stash */
	if (mode == O_WRONLY) {
		crashstash_free();
		/*
		 * Put the address of the crashstash list head, and the size and
		 * pages variables, in the kernel log. This is necessary in
		 * order for later vmcore analysis to be able to find and
		 * interpret the crashstash.
		 *
		 * Writing to the log at open-time is best, because release() is
		 * not reliably called directly after the file is closed. These
		 * memory addresses don't change at runtime, but we want them to
		 * be in the log buffer when a vmcore is generated, so writing
		 * the message just at initialization time isn't good enough: it
		 * may be a long time between initialization and when a vmcore
		 * is generated.
		 *
		 * Of course, if the file is written to a lot, we could cause
		 * log spam, so use pr_info_ratelimited().
		 */
		pr_info_ratelimited("crashstash: STASH: %llx SIZE: %llx PAGES: %llx\n",
				    (u64)&crashstash, (u64)&size, (u64)&pages);
	}
	count++;

out:
	mutex_unlock(&crashstash_lock);
	return rv;
}

static int crashstash_release(struct inode *inode, struct file *filp)
{
	/* release() is called... at some point after the last close()
	 * but before any subsequent open(). Be sure to drop our reference
	 * to the file so it can be reopened. */
	mutex_lock(&crashstash_lock);
	BUG_ON(count <= 0);
	count--;
	mutex_unlock(&crashstash_lock);
	return 0;
}

static ssize_t crashstash_write(struct file *f, const char __user *data, size_t amt, loff_t *off) {
	size_t pgoff, chunk_amt;
	void *dst;
	ssize_t written = 0;
	struct page *pg;

	/* We'll only allow writing at the end of the file */
	mutex_lock(&crashstash_lock);
	if (*off != size) {
		mutex_unlock(&crashstash_lock);
		return -EINVAL;
	}

	while (amt) {
		pgoff = *off % PAGE_SIZE;
		if (pgoff) {
			BUG_ON(list_empty(&crashstash));
			pg = list_last_entry(&crashstash, struct page, lru);
		} else {
			pg = alloc_page(GFP_KERNEL);
			list_add_tail(&pg->lru, &crashstash);
			pages++;
		}
		dst = (void *)page_address(pg) + pgoff;
		chunk_amt = min(amt, PAGE_SIZE - pgoff);
		if (copy_from_user(dst, data, chunk_amt) != 0) {
			written = -EFAULT;
			break;
		}
		amt -= chunk_amt;
		*off += chunk_amt;
		size += chunk_amt;
		written += chunk_amt;
		data += chunk_amt;
	}
	proc_set_size(pde, size);
	mutex_unlock(&crashstash_lock);

	return written;
}

static ssize_t crashstash_read(struct file *f, char __user *data, size_t amt, loff_t *off)
{
	struct page *pg;
	size_t pgnum, pgoff, chunk_amt;
	ssize_t read = 0;
	void *src;

	/* Only read the amount we currently have */
	mutex_lock(&crashstash_lock);
	if (*off + amt > size)
		amt = size - *off;

	/* Seek to the correct page, don't bother with anything clever */
	pgnum = *off / PAGE_SIZE;
	pgoff = *off % PAGE_SIZE;
	list_for_each_entry(pg, &crashstash, lru)
	{
		if (!pgnum)
			break;
		pgnum--;
	}

	/* Now we can read pages */
	while (amt) {
		pgoff = *off % PAGE_SIZE;
		chunk_amt = min(amt, PAGE_SIZE - pgoff);
		src = (void *)page_address(pg) + pgoff;
		if (copy_to_user(data, src, chunk_amt) != 0) {
			read = -EFAULT;
			break;
		}
		pg = list_next_entry(pg, lru);
		*off += chunk_amt;
		amt -= chunk_amt;
		read += chunk_amt;
		data += chunk_amt;
	}

	mutex_unlock(&crashstash_lock);
	return read;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,6,0)
static const struct file_operations crashstash_fops = {
	.owner                  = THIS_MODULE,
	.read                   = crashstash_read,
	.write                  = crashstash_write,
	.open                   = crashstash_open,
	.release                = crashstash_release,
};
#else
static const struct proc_ops crashstash_fops = {
	.proc_read              = crashstash_read,
	.proc_write             = crashstash_write,
	.proc_open              = crashstash_open,
	.proc_release           = crashstash_release,
};
#endif

static int crashstash_init(void)
{
	pde = proc_create("crashstash", 0600, NULL, &crashstash_fops);
	if (!pde) {
		return -ENOENT;
	}
	pr_info("crashstash: successfully initialized\n");
	return 0;
}

static void crashstash_exit(void)
{
	proc_remove(pde);
	pde = NULL;
	mutex_lock(&crashstash_lock);
	crashstash_free();
	mutex_unlock(&crashstash_lock);
	pr_info("crashstash: exited\n");
}

module_init(crashstash_init);
module_exit(crashstash_exit);
