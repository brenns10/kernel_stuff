/*
 * drgn_vmalloc_test.c: Do a vm_map_ram and see what we get
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/gfp.h>
#include <linux/vmalloc.h>

#define NPAGE 4
void *addr;
struct page *pages[NPAGE];

static void cleanup(void)
{
	int i;
	if (addr)
		vm_unmap_ram(addr, NPAGE);

	for (i = 0; i < NPAGE; i++)
		if (pages[i])
			__free_pages(pages[i], 0);
}

static int __init init_dentryref(void)
{
	int i;
	u32 val = 0;
	for (i = 0; i < NPAGE; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i])
			goto err;
	}

	/*
	 * d4efd79a81ab ("mm: remove the prot argument from vm_map_ram")
	 * Since 5.8.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
	addr = vm_map_ram(pages, NPAGE, -1, PAGE_KERNEL);
#else
	addr = vm_map_ram(pages, NPAGE, -1);
#endif
	if (!addr)
		goto err;

	/* put a distinctive memory pattern in */
	for (i = 0; i < NPAGE * PAGE_SIZE / 4; i++)
		((uint32_t *)addr)[i] = val++;

	printk(KERN_INFO "drgn_vmalloc_test: 0x%lx\n", (unsigned long)addr);
	return 0;
err:
	cleanup();
	return -1;
}

static void __exit exit_dentryref(void)
{
	cleanup();
}

module_init(init_dentryref);
module_exit(exit_dentryref);

MODULE_AUTHOR("Stephen Brennan <stephen.s.brennan@oracle.com>");
MODULE_DESCRIPTION("Allocate with vm_map_ram to test drgn memory reader");
MODULE_LICENSE("GPL");
