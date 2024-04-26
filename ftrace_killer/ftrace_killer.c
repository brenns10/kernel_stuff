/*
 * ftrace_killer.c: kernel module to call the (very much private) ftrace_kill()
 * function
 */
#include <linux/module.h>
#include <linux/printk.h>

void (*ftrace_kill)(void) = (void *) EXTERN_FTRACE_KILL;

static int __init init_ftrace_killer(void)
{
	pr_info("killing ftrace...\n");
	ftrace_kill();
	pr_info("ftrace killed!\n");
	return 0;
}

static void __exit exit_ftrace_killer(void) {}

module_init(init_ftrace_killer);
module_exit(exit_ftrace_killer);

MODULE_AUTHOR("Stephen Brennan <stephen.s.brennan@oracle.com>");
MODULE_DESCRIPTION("Kill ftrace savagely");
MODULE_LICENSE("GPL");
