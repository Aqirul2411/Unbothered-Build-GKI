// SPDX-License-Identifier: GPL-2.0
/*
 * DIAGNOSTIC BUILD ONLY - branch diag-logkit, never merged into main.
 *
 * What was established before this file existed:
 *
 *   - a probe build that panicked from a pure_initcall did reset the phone, so
 *     start_kernel(), workqueue_init(), do_pre_smp_initcalls(), smp_init(),
 *     sched_init_smp() and the entry into do_initcalls() all complete;
 *   - a build that snapshotted the log into the oops partition at 2 s wrote
 *     nothing, and its "boot-recovery" flag was never set either, which needs
 *     the disk at about 1.2 s.
 *
 * The lesson is not only "the disk is too late". It is that nothing which
 * needs a scheduler is trustworthy this early: a workqueue item, a timer, a
 * threaded handler all need some other part of the kernel to still be
 * working. So the primary evidence here is written synchronously, from the
 * initcall path itself, into RAM that needs no disk, no allocator and no
 * workqueue.
 *
 * This device reserves 4 MiB for exactly this purpose - the devicetree node
 * ramoops@0xa7000000. Only the first 16 KiB of it are used by ramoops (the
 * console area at offset 0 and the pmsg area at 0x2000), and because the
 * region is reserved it never reaches the allocator, so nothing overwrites
 * the rest. Two rings live in the unused tail:
 *
 *   0xa7200000  1 MiB  log ring:      256 slots of 4 KiB
 *   0xa7300000  1 MiB  initcall ring: 16384 slots of 64 B
 *
 * 1. The initcall ring is the important one. do_one_initcall() records the
 *    address of the function it is about to call, and what it returned. That
 *    is a straight-line write with no locks, so it works even if interrupts
 *    are off and every scheduler in the kernel is wedged. The last record
 *    that has no matching "return" names the exact function the phone died
 *    in: the address in the dump maps to a symbol in System.map.
 *
 * 2. The log ring is the nice-to-have. A 50 ms timer takes the newest 3.9 KiB
 *    of the printk buffer. If the scheduler still works, this says what the
 *    named function was doing, in its own words. If it does not, the ring
 *    stays empty and the initcall ring carries the answer alone. A snapshot
 *    is also taken at every initcall level boundary, so even with no timer at
 *    all there is one copy of the log per level.
 *
 * Reading it, from recovery - the layout is fixed and every record carries a
 * magic, so a wrong address shows up as "nothing" rather than as noise:
 *
 *   dd if=/dev/mem bs=4096 skip=684544 count=512 of=/sdcard/ubdram.bin
 *
 * (0xa7200000 = 2803892224 bytes = block 684544 of 4096; 512 blocks covers
 * both rings. Each record is tagged, so dumps from different builds cannot be
 * confused either.)
 *
 * Kept from the previous build, because it costs nothing and might pay off if
 * the phone survives past the disk: as soon as storage exists, write the AOSP
 * "boot-recovery" command into misc, so a later cold boot comes up in recovery
 * on its own.
 */
#define pr_fmt(fmt) "UBDRAM: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/io.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/buffer_head.h>
#include <linux/workqueue.h>
#include <linux/mm.h>

#define UBD_LOG_MAGIC	"UBDRAM01"
#define UBD_IC_MAGIC	0x43494255	/* "UBIC" */
#define UBD_LEVEL_MAGIC	0x564c4255	/* "UBLV" */

#define UBD_LOG_PHYS	0x0a7200000ULL	/* 2 MiB into the reserved 4 MiB */
#define UBD_IC_PHYS	0x0a7300000ULL	/* 3 MiB into the reserved 4 MiB */
#define UBD_SLOT	4096
#define UBD_LOG_SLOTS	256		/* 1 MiB, wraps after ~12 s */
#define UBD_TEXT	3900
#define UBD_TICK_MS	50
#define UBD_END_MS	10000

#define UBD_IC_SLOT	64
#define UBD_IC_SLOTS	16384		/* 1 MiB; a boot makes ~6000 records */

#define UBD_GPT_SIG	"EFI PART"
#define UBD_GPT_MAX_ENT	128
#define UBD_GPT_ENT_SZ	128
#define UBD_GPT_NAME_OFF 56
#define UBD_MAX_MISC	(16 * 1024 * 1024)

enum { UBD_KIND_TICK = 0, UBD_KIND_MARK = 1 };
enum { UBD_IC_ENTER = 0, UBD_IC_EXIT = 1 };

/* a full 4 KiB slot: 48 byte header, then the log text */
struct ubd_rec {
	char magic[8];
	u32 seq;
	u32 len;
	u64 ms;
	u64 jiffies;
	u32 hz;
	u32 cpu;
	u32 kind;
	u32 cpus;
	char text[UBD_TEXT];
} __aligned(8);

/* one initcall, 64 byte slot */
struct ubd_ic {
	u32 magic;
	u32 seq;
	u64 fn;
	u64 ms;
	u16 kind;
	u16 level;
	u32 cpu;
	u32 cpus;
	s32 ret;
	u32 pad;
} __aligned(8);

static void *ubd_log_base;
static void *ubd_ic_base;
static struct ubd_rec ubd_rec;
static struct ubd_ic ubd_ic;
static char ubd_log[UBD_TEXT];
static size_t ubd_log_len;
static unsigned int ubd_seq;
static unsigned int ubd_ic_seq;
static struct timer_list ubd_timer;
static struct kmsg_dumper ubd_dumper;

static u8 ubd_gpt_hdr[512];
static u8 ubd_gpt_ents[UBD_GPT_MAX_ENT * UBD_GPT_ENT_SZ];

/* the level whose initcalls are running, as an index into
 * initcall_level_names[], -1 for the pre-smp "early" pass */
static int ubd_level = -1;

static u16 ubd_le16(const u8 *p)
{
	return (u16)p[0] | (u16)p[1] << 8;
}

static u32 ubd_le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 ubd_le64(const u8 *p)
{
	return (u64)ubd_le32(p) | (u64)ubd_le32(p + 4) << 32;
}

/* --- initcall ring: the record that has no answer names the culprit ----- */

void ubd_ic_trace(unsigned long fn, int kind, int ret)
{
	struct ubd_ic *r = &ubd_ic;

	if (!ubd_ic_base)
		return;

	memset(r, 0, sizeof(*r));
	r->magic = UBD_IC_MAGIC;
	r->kind = kind;
	r->level = ubd_level < 0 ? 0xffff : (u16)ubd_level;
	r->fn = fn;
	r->ms = ktime_to_ms(ktime_get());
	r->cpu = raw_smp_processor_id();
	r->cpus = num_online_cpus();
	r->ret = ret;

	/* the sequence number only marks the slot; it is written last so a
	 * dump taken between the two never looks like a newer entry */
	r->seq = ++ubd_ic_seq;
	memcpy(ubd_ic_base + (u64)(ubd_ic_seq % UBD_IC_SLOTS) * UBD_IC_SLOT,
		    r, sizeof(*r));
}

/* --- log ring: best effort, needs a working timer ---------------------- */

static void ubd_dump_fn(struct kmsg_dumper *dumper, enum kmsg_dump_reason reason)
{
	size_t len = 0;

	if (kmsg_dump_get_buffer(dumper, false, ubd_log, sizeof(ubd_log), &len))
		ubd_log_len = len;
}

static void ubd_snapshot(void)
{
	ubd_log_len = 0;
	kmsg_dump(KMSG_DUMP_OOPS);
}

static void ubd_store(u32 kind, const char *prefix)
{
	struct ubd_rec *r = &ubd_rec;
	size_t n = 0;

	if (!ubd_log_base)
		return;

	memset(r, 0, sizeof(*r));
	memcpy(r->magic, UBD_LOG_MAGIC, 8);
	r->ms = ktime_to_ms(ktime_get());
	r->jiffies = get_jiffies_64();
	r->hz = HZ;
	r->cpu = raw_smp_processor_id();
	r->kind = kind;
	r->cpus = num_online_cpus();

	if (prefix) {
		n = strlen(prefix);
		if (n > sizeof(r->text))
			n = sizeof(r->text);
		memcpy(r->text, prefix, n);
	}
	if (ubd_log_len && n < sizeof(r->text)) {
		size_t room = sizeof(r->text) - n;
		size_t take = min_t(size_t, room, ubd_log_len);

		memcpy(r->text + n, ubd_log, take);
		n += take;
	}
	r->len = n;

	r->seq = ++ubd_seq;
	memcpy(ubd_log_base + (u64)(ubd_seq % UBD_LOG_SLOTS) * UBD_SLOT,
		    r, sizeof(*r));
}

/*
 * Called from do_initcall_level() before the level's initcalls run. The level
 * name is one of the strings the kernel prints for
 * "calling  <name>+0x0/0x... @ 1", so a dump reads as a boot log even without
 * the symbol table.
 */
void ubd_mark(int level, const char *what)
{
	char prefix[64];

	ubd_level = level;
	snprintf(prefix, sizeof(prefix), "MARK level=%d name=%s\n", level,
		 what ? what : "?");
	ubd_snapshot();
	ubd_store(UBD_KIND_MARK, prefix);
}

static void ubd_tick(struct timer_list *timer)
{
	u64 ms;

	ubd_snapshot();
	ubd_store(UBD_KIND_TICK, NULL);

	ms = ktime_to_ms(ktime_get());
	if (ms + UBD_TICK_MS <= UBD_END_MS)
		mod_timer(timer, jiffies + msecs_to_jiffies(UBD_TICK_MS));
}

/* --- safety net: boot-recovery in misc, once storage shows up ----------- */

static struct block_device *ubd_bdev(void)
{
	struct block_device *bdev;

	bdev = blkdev_get_by_dev(MKDEV(8, 15), FMODE_READ | FMODE_WRITE, NULL);
	if (IS_ERR(bdev))
		return NULL;

	if (!bdev->bd_disk || bdev->bd_partno != 15 ||
	    strcmp(bdev->bd_disk->disk_name, "sda")) {
		blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
		return NULL;
	}
	return bdev;
}

static int ubd_read(struct block_device *bdev, u64 off, void *dst, size_t len)
{
	size_t done = 0;

	while (done < len) {
		u64 pos = off + done;
		size_t skip = pos & (UBD_SLOT - 1);
		size_t n = min_t(size_t, UBD_SLOT - skip, len - done);
		struct buffer_head *bh;

		bh = __bread(bdev, pos >> 12, UBD_SLOT);
		if (!bh)
			return -EIO;
		memcpy((char *)dst + done, bh->b_data + skip, n);
		brelse(bh);
		done += n;
	}
	return 0;
}

static int ubd_find_misc(struct gendisk *disk, int *partno, u64 *bytes)
{
	struct block_device *whole;
	u64 ent_off;
	u32 nent, esz, i;
	int matches = 0, ret = -ENOENT;

	whole = bdget_disk(disk, 0);
	if (!whole)
		return -ENODEV;

	if (ubd_read(whole, 512, ubd_gpt_hdr, sizeof(ubd_gpt_hdr)))
		goto out;
	if (memcmp(ubd_gpt_hdr, UBD_GPT_SIG, 8))
		goto out;

	ent_off = ubd_le64(ubd_gpt_hdr + 72) << 9;
	nent = ubd_le32(ubd_gpt_hdr + 80);
	esz = ubd_le32(ubd_gpt_hdr + 84);
	if (esz != UBD_GPT_ENT_SZ || !nent || nent > UBD_GPT_MAX_ENT)
		goto out;
	if (ubd_read(whole, ent_off, ubd_gpt_ents, nent * esz))
		goto out;

	for (i = 0; i < nent; i++) {
		const u8 *e = ubd_gpt_ents + i * esz;
		const u8 *name = e + UBD_GPT_NAME_OFF;
		u64 first = ubd_le64(e);
		u64 last = ubd_le64(e + 8);
		u64 size;

		if ((ubd_le16(name) | 0x20) != 'm' ||
		    (ubd_le16(name + 2) | 0x20) != 'i' ||
		    (ubd_le16(name + 4) | 0x20) != 's' ||
		    (ubd_le16(name + 6) | 0x20) != 'c' ||
		    ubd_le16(name + 8))
			continue;

		if (last < first)
			continue;
		size = (last - first + 1) << 9;
		if (size < 512 || size > UBD_MAX_MISC)
			continue;

		*partno = i + 1;
		*bytes = size;
		matches++;
	}

	if (matches == 1)
		ret = 0;
	else if (matches > 1)
		ret = -EINVAL;

out:
	bdput(whole);
	return ret;
}

static int ubd_set_bcb(struct gendisk *disk)
{
	struct block_device *misc;
	struct buffer_head *bh;
	u64 bytes = 0;
	int partno = 0, i, err;

	err = ubd_find_misc(disk, &partno, &bytes);
	if (err)
		return err;

	misc = bdget_disk(disk, partno);
	if (!misc)
		return -ENODEV;

	if (i_size_read(misc->bd_inode) < UBD_SLOT) {
		bdput(misc);
		return -ENOSPC;
	}

	bh = __bread(misc, 0, UBD_SLOT);
	if (!bh) {
		bdput(misc);
		return -EIO;
	}

	for (i = 0; i < 32; i++) {
		u8 c = bh->b_data[i];

		if (c && (c < 0x20 || c > 0x7e)) {
			brelse(bh);
			bdput(misc);
			return -EMEDIUMTYPE;
		}
	}

	if (!memcmp(bh->b_data, "boot-recovery", 13) && !bh->b_data[13]) {
		brelse(bh);
		bdput(misc);
		return 1;
	}

	memcpy(bh->b_data, "boot-recovery", 13);
	memset(bh->b_data + 13, 0, 32 - 13);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	bdput(misc);
	return 0;
}

static void ubd_flag_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(ubd_flag_work, ubd_flag_fn);
static unsigned int ubd_flag_tries;
#define UBD_FLAG_TRIES 40

static void ubd_flag_fn(struct work_struct *work)
{
	struct block_device *bdev;
	int err;

	ubd_flag_tries++;

	bdev = ubd_bdev();
	if (!bdev) {
		err = -ENODEV;
	} else {
		err = ubd_set_bcb(bdev->bd_disk);
		blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
	}

	if (!err || err == 1) {
		ubd_snapshot();
		ubd_store(UBD_KIND_MARK, err == 1 ?
			  "MARK misc flag was already set\n" :
			  "MARK misc flag written: boot-recovery\n");
		return;
	}

	if (ubd_flag_tries < UBD_FLAG_TRIES)
		schedule_delayed_work(&ubd_flag_work, HZ / 4);
}

/*
 * Reserved memory that is part of the linear map: memremap(MEMREMAP_WB) hands
 * back the direct-map address, and if it declines for any reason the direct map
 * is still there, so fall back to it rather than lose the ring.
 */
static void *ubd_ram_map(phys_addr_t phys, size_t size)
{
	void *p = memremap(phys, size, MEMREMAP_WB);

	if (p)
		return p;
	if (pfn_valid(PHYS_PFN(phys)))
		return __va((unsigned long)phys);
	return NULL;
}

static int __init ubd_init(void)
{
	/*
	 * Both rings are reserved memory, not MMIO: they are part of the linear
	 * map and survive a warm reset. memremap gives the normal cached mapping
	 * for that memory, which is what ramoops itself uses for the neighbouring
	 * console area - mapping the same RAM a second time as Device memory is
	 * the kind of aliasing the arm64 manual warns about.
	 */
	ubd_log_base = ubd_ram_map(UBD_LOG_PHYS,
				   (phys_addr_t)UBD_LOG_SLOTS * UBD_SLOT);
	ubd_ic_base = ubd_ram_map(UBD_IC_PHYS,
				  (phys_addr_t)UBD_IC_SLOTS * UBD_IC_SLOT);
	if (!ubd_ic_base)
		pr_err("no initcall trace, the boot cannot be explained\n");

	ubd_dumper.dump = ubd_dump_fn;
	ubd_dumper.max_reason = KMSG_DUMP_MAX;
	if (kmsg_dump_register(&ubd_dumper))
		pr_err("cannot register the dumper\n");

	timer_setup(&ubd_timer, ubd_tick, 0);
	mod_timer(&ubd_timer, jiffies + msecs_to_jiffies(UBD_TICK_MS));

	schedule_delayed_work(&ubd_flag_work, HZ / 2);

	ubd_snapshot();
	ubd_store(UBD_KIND_MARK, "MARK ubdram started\n");
	return 0;
}
early_initcall(ubd_init);
