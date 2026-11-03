// SPDX-License-Identifier: GPL-2.0
/*
 * DIAGNOSTIC BUILD ONLY - branch diag-logkit, never merged into main.
 *
 * The merged 5.10.273 tree stops at the bootloader splash and leaves no usable
 * evidence: this kernel has no display or serial console driver, pstore has no
 * reserved region on this device, and the softlockup, hung-task and RCU
 * detectors all stay silent. So boot the kernel that is suspected, snapshot its
 * own log buffer into the unused tail of the 'oops' partition at several points
 * during boot, and read it back from recovery with dd.
 *
 * The schedule is the point. One late snapshot only says "the log ended
 * somewhere before then". Snapshots at 2, 5, 10, 20 and 40 s turn the boot into
 * a heartbeat: the last record that arrived brackets the stall to a few
 * seconds, and the record after it says whether the write or the ability to
 * schedule work failed first.
 *
 * Layout of one record (block aligned, 4 KiB block size):
 *
 *	block 0 of the slot:  struct ubd_hdr, zero padded
 *	block 1.. of the slot: the log text, as many bytes as hdr.len says
 *
 * The slots sit at 12 MiB .. 14.5 MiB of the 16 MiB oops partition, past where
 * a single 2 MiB mtdoops record reaches.
 *
 * ---------------------------------------------------------------------------
 * Safety net: 'boot-recovery' in the misc partition
 *
 * A kernel that hangs before userspace cannot be asked to do anything, and the
 * only way back is the key combination - which is exactly what fails after a
 * dead boot, because there is no reboot to catch and a forced power cycle is a
 * timing game. So as soon as the storage is up (about 1.2 s), this build writes
 * the AOSP bootloader command "boot-recovery" into the misc partition. From
 * then on, any cold boot - a panic reset, or the user holding power to force
 * the phone off and pressing it again - lands in recovery instead of the dead
 * kernel, with no cable, no fastboot and no key combination.
 *
 * misc is found by its GPT label rather than a hard-coded partition number,
 * and only if exactly one partition is called "misc", its first 32 bytes look
 * like a bootloader message (printable ASCII or zero) and it is at most 16 MiB.
 * Anything else and this writes nothing at all.
 *
 * Recovery normally clears the command when it boots, so the flag does not
 * stick beyond one boot; if the phone does come up, its next reboot will be
 * into recovery once, which is harmless.
 *
 * No per-CPU backtraces: arm64 here has no arch_trigger_cpumask_backtrace(), so
 * trigger_all_cpu_backtrace() is a stub that returns false, and smp_call_function
 * would block forever on a CPU that is spinning with interrupts off - the very
 * case this is meant to observe. The heartbeat timing is the substitute.
 */
#define pr_fmt(fmt) "UBLOGD: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/buffer_head.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/utsname.h>
#include <linux/string.h>

#define UBD_MAGIC		"UBLOGD01"
#define UBD_BLOCK_SHIFT		12
#define UBD_BLOCK_SIZE		(1 << UBD_BLOCK_SHIFT)	/* 4096 */
#define UBD_SLOT_BLOCKS		128			/* 512 KiB per slot */
#define UBD_FIRST_BLOCK		((12 * 1024 * 1024) >> UBD_BLOCK_SHIFT)
#define UBD_BUF_SIZE		(160 * 1024)

#define UBD_GPT_SIG		"EFI PART"
#define UBD_GPT_MAX_ENT		128
#define UBD_GPT_ENT_SZ		128
#define UBD_GPT_NAME_OFF	56
#define UBD_MAX_MISC		(16 * 1024 * 1024)

struct ubd_hdr {
	char magic[8];
	u32 seq;
	u32 len;
	u64 jiffies;
	u32 hz;
	u32 reserved;
	char release[64];
};

/* snapshots at 2, 5, 10, 20 and 40 s of uptime */
static const unsigned int ubd_delay_s[] = { 2, 3, 5, 10, 20 };
#define UBD_RUNS ARRAY_SIZE(ubd_delay_s)

static char ubd_buf[UBD_BUF_SIZE];
static size_t ubd_len;
static struct kmsg_dumper ubd_dumper;
static unsigned int ubd_seq;

static u8 ubd_gpt_hdr[512];
static u8 ubd_gpt_ents[UBD_GPT_MAX_ENT * UBD_GPT_ENT_SZ];

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

static void ubd_dumper_fn(struct kmsg_dumper *dumper, enum kmsg_dump_reason reason)
{
	size_t len = 0;

	if (kmsg_dump_get_buffer(dumper, false, ubd_buf, sizeof(ubd_buf), &len))
		ubd_len = len;
}

/*
 * The oops partition is sda15 on this board. Its major is fixed (8) but the
 * disk name is checked too, so a phone with a different layout writes nothing
 * instead of writing into somebody else's partition.
 */
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

static int ubd_write(struct block_device *bdev, loff_t off, const void *buf,
		     size_t len)
{
	size_t done = 0;

	while (done < len) {
		sector_t blk = (off + done) >> UBD_BLOCK_SHIFT;
		size_t n = min_t(size_t, UBD_BLOCK_SIZE, len - done);
		struct buffer_head *bh;

		if ((off + done) & (UBD_BLOCK_SIZE - 1))
			return -EINVAL;

		bh = __bread(bdev, blk, UBD_BLOCK_SIZE);
		if (!bh)
			return -EIO;

		memcpy(bh->b_data, (const char *)buf + done, n);
		if (n < UBD_BLOCK_SIZE)
			memset(bh->b_data + n, 0, UBD_BLOCK_SIZE - n);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		done += n;
	}
	return 0;
}

/* read any byte range, block by block */
static int ubd_read(struct block_device *bdev, u64 off, void *dst, size_t len)
{
	size_t done = 0;

	while (done < len) {
		u64 pos = off + done;
		size_t skip = pos & (UBD_BLOCK_SIZE - 1);
		size_t n = min_t(size_t, UBD_BLOCK_SIZE - skip, len - done);
		struct buffer_head *bh;

		bh = __bread(bdev, pos >> UBD_BLOCK_SHIFT, UBD_BLOCK_SIZE);
		if (!bh)
			return -EIO;
		memcpy((char *)dst + done, bh->b_data + skip, n);
		brelse(bh);
		done += n;
	}
	return 0;
}

/*
 * Find the partition labelled "misc" in the GPT. Returns 0 and the 1-based
 * partition number only if exactly one partition matches and it is small
 * enough to be a misc partition; -ENOENT when there is none, -EINVAL when the
 * answer is ambiguous.
 */
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

		/* UTF-16LE "misc", case insensitive on the ASCII letters */
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

/*
 * Ask the bootloader to boot recovery next time, the way an A/B update does.
 * Only the 32-byte command field is touched; the rest of the message (status,
 * recovery, stage, and the A/B slot metadata at 2048) is left as it was.
 */
static int ubd_set_bcb(struct gendisk *disk, int *partno_out, u64 *bytes_out)
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

	if (i_size_read(misc->bd_inode) < UBD_BLOCK_SIZE) {
		bdput(misc);
		return -ENOSPC;
	}

	bh = __bread(misc, 0, UBD_BLOCK_SIZE);
	if (!bh) {
		bdput(misc);
		return -EIO;
	}

	/*
	 * Belt and braces: a bootloader message starts with a NUL-padded ASCII
	 * command. A filesystem or a kernel image does not, so if this does
	 * not look like one, refuse instead of clobbering it.
	 */
	for (i = 0; i < 32; i++) {
		u8 c = bh->b_data[i];

		if (c && (c < 0x20 || c > 0x7e)) {
			brelse(bh);
			bdput(misc);
			return -EMEDIUMTYPE;
		}
	}

	if (!memcmp(bh->b_data, "boot-recovery", 13) &&
	    !bh->b_data[13]) {
		brelse(bh);
		bdput(misc);
		*partno_out = partno;
		*bytes_out = bytes;
		return 1;			/* already set */
	}

	memcpy(bh->b_data, "boot-recovery", 13);
	memset(bh->b_data + 13, 0, 32 - 13);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	bdput(misc);

	*partno_out = partno;
	*bytes_out = bytes;
	return 0;
}

static void ubd_run_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(ubd_work, ubd_run_fn);

static void ubd_flag_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(ubd_flag_work, ubd_flag_fn);

static unsigned int ubd_flag_tries;
#define UBD_FLAG_TRIES	24		/* half a second apart: 12 s */

/*
 * Storage comes up around 1.2 s, so the first few attempts are expected to
 * fail. Keep trying until the flag is in place, then stop for good.
 */
static void ubd_flag_fn(struct work_struct *work)
{
	struct block_device *bdev;
	int partno = 0;
	u64 bytes = 0;
	int err;

	ubd_flag_tries++;

	bdev = ubd_bdev();
	if (!bdev) {
		err = -ENODEV;
	} else {
		err = ubd_set_bcb(bdev->bd_disk, &partno, &bytes);
		blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
	}

	if (!err || err == 1) {
		pr_info("misc is sda%d (%llu bytes), boot-recovery %s at %u.%u s\n",
			partno, bytes, err ? "was already set" : "written",
			ubd_flag_tries / 2, (ubd_flag_tries % 2) * 5);
		return;
	}

	if (ubd_flag_tries < UBD_FLAG_TRIES)
		schedule_delayed_work(&ubd_flag_work, HZ / 2);
	else
		pr_err("gave up on boot-recovery after %u tries (last error %d)\n",
		       ubd_flag_tries, err);
}

static void ubd_next(void)
{
	if (ubd_seq < UBD_RUNS)
		schedule_delayed_work(&ubd_work, ubd_delay_s[ubd_seq] * HZ);
}

static void ubd_run_fn(struct work_struct *work)
{
	static struct ubd_hdr hdr;
	struct block_device *bdev;
	loff_t off, size;
	size_t total;
	u64 ms;
	int err;

	ubd_seq++;
	ms = ktime_to_ms(ktime_get());
	pr_info("run %u/%u at %llu.%03llu s\n", ubd_seq,
		(unsigned int)UBD_RUNS, ms / 1000, ms % 1000);

	ubd_len = 0;
	kmsg_dump(KMSG_DUMP_OOPS);

	bdev = ubd_bdev();
	if (!bdev) {
		pr_err("run %u: sda15 is not there\n", ubd_seq);
		goto next;
	}

	size = i_size_read(bdev->bd_inode);
	off = (loff_t)UBD_FIRST_BLOCK * UBD_BLOCK_SIZE +
	      (loff_t)(ubd_seq - 1) * UBD_SLOT_BLOCKS * UBD_BLOCK_SIZE;

	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, UBD_MAGIC, 8);
	hdr.seq = ubd_seq;
	hdr.len = min_t(size_t, ubd_len, UBD_BUF_SIZE);
	hdr.jiffies = get_jiffies_64();
	hdr.hz = HZ;
	strscpy(hdr.release, init_uts_ns.name.release, sizeof(hdr.release));

	total = UBD_BLOCK_SIZE + hdr.len;
	if (off + total > size) {
		pr_err("run %u: %lld + %zu does not fit in %lld\n",
		       ubd_seq, off, total, size);
		blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
		goto next;
	}

	err = ubd_write(bdev, off, &hdr, sizeof(hdr));
	if (!err)
		err = ubd_write(bdev, off + UBD_BLOCK_SIZE, ubd_buf, hdr.len);
	blkdev_put(bdev, FMODE_READ | FMODE_WRITE);

	pr_info("run %u: %s %u bytes at %lld\n", ubd_seq,
		err ? "could not write" : "wrote", hdr.len, off);

next:
	ubd_next();
}

static int __init ubd_init(void)
{
	ubd_dumper.dump = ubd_dumper_fn;
	if (kmsg_dump_register(&ubd_dumper))
		pr_err("could not register the dumper\n");

	pr_info("snapshots at 2, 5, 10, 20 and 40 s\n");
	ubd_next();

	schedule_delayed_work(&ubd_flag_work, HZ / 2);
	return 0;
}
pure_initcall(ubd_init);
