// SPDX-License-Identifier: GPL-2.0
/*
 * DIAGNOSTIC BUILD ONLY - branch diag-reader, never merged into main.
 *
 * The 5.10.273 boot dies early, so the diagnostic build (branch diag-logkit)
 * leaves its evidence in this device's reserved 4 MiB at 0xa7000000: two rings
 * of tagged records, one written per initcall, one per 50 ms of printk. The
 * phone then sits frozen, and the only way to look at that RAM is a kernel
 * that boots. fox recovery on this device has no CONFIG_DEVMEM and no
 * /proc/kcore - creating the node by hand gives ENXIO - so recovery cannot
 * read it, and nothing else in userspace can reach physical memory either.
 *
 * So: this is the good 272 kernel (5d78169f7) with one addition. It maps those
 * 2 MiB of reserved RAM and publishes them as a world-readable file:
 *
 *   adb exec-out cat /proc/ubdring > ring.bin
 *
 * No root, no dd, no partition written, nothing that can go wrong on the phone.
 * The mapping is reserved memory, so it cannot be swapped, freed or reused, and
 * mapping it changes nothing about the records already in it.
 *
 * If the rings are not there - an ordinary boot, or RAM that a full power-off
 * cleared - the file still exists and reads as zeros, and the kernel log says
 * "no rings", which is an answer too: it would mean the reset cleared RAM and
 * the next diagnostic has to panic instead of hanging.
 *
 * Layout, from kernel/ubdram.c: 1 MiB of 4 KiB log records at offset 0, then
 * 1 MiB of 64-byte initcall records. Parsed by ubdram.py.
 */
#define pr_fmt(fmt) "UBDREAD: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#define UBD_LOG_MAGIC	"UBDRAM01"
#define UBD_IC_MAGIC	0x43494255	/* "UBIC" */

#define UBD_SRC_PHYS	0x0a7200000ULL	/* both rings, 2 MiB */
#define UBD_SRC_SIZE	(2 * 1024 * 1024)
#define UBD_SLOT	4096
#define UBD_LOG_SLOTS	256
#define UBD_IC_OFF	(1024 * 1024)
#define UBD_IC_SLOT	64
#define UBD_IC_SLOTS	16384

static const u8 *ubd_src;

static ssize_t ubd_read(struct file *file, char __user *buf, size_t len,
			loff_t *ppos)
{
	size_t n;

	if (!ubd_src || *ppos >= UBD_SRC_SIZE)
		return 0;

	n = min_t(size_t, len, UBD_SRC_SIZE - *ppos);
	if (copy_to_user(buf, ubd_src + *ppos, n))
		return -EFAULT;
	*ppos += n;
	return n;
}

static const struct proc_ops ubd_proc_ops = {
	.proc_read	= ubd_read,
	.proc_lseek	= default_llseek,
};

static int __init ubd_reader_init(void)
{
	const u8 *p;
	int log_slots = 0, ic_recs = 0, i;
	u32 seq, newest = 0;

	p = memremap(UBD_SRC_PHYS, UBD_SRC_SIZE, MEMREMAP_WB);
	if (!p && pfn_valid(PHYS_PFN(UBD_SRC_PHYS)))
		p = __va((unsigned long)UBD_SRC_PHYS);
	if (!p) {
		pr_err("cannot map 0x%llx\n", UBD_SRC_PHYS);
		return 0;
	}
	ubd_src = p;

	for (i = 0; i < UBD_LOG_SLOTS; i++) {
		const u8 *r = p + (size_t)i * UBD_SLOT;

		if (memcmp(r, UBD_LOG_MAGIC, 8))
			continue;
		log_slots++;
		seq = *(const u32 *)(r + 8);
		if (seq > newest)
			newest = seq;
	}

	for (i = 0; i < UBD_IC_SLOTS; i++) {
		const u8 *r = p + UBD_IC_OFF + (size_t)i * UBD_IC_SLOT;

		if (*(const u32 *)r != UBD_IC_MAGIC)
			continue;
		ic_recs++;
		seq = *(const u32 *)(r + 4);
		if (seq > newest)
			newest = seq;
	}

	if (!proc_create("ubdring", 0444, NULL, &ubd_proc_ops)) {
		pr_err("cannot create /proc/ubdring\n");
		return 0;
	}

	if (log_slots || ic_recs)
		pr_info("%d log records, %d initcall records, newest seq %u; "
			"read /proc/ubdring\n", log_slots, ic_recs, newest);
	else
		pr_info("no rings at 0x%llx - RAM did not survive, or that boot "
			"never wrote any\n", UBD_SRC_PHYS);

	return 0;
}
late_initcall(ubd_reader_init);
