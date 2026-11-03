// SPDX-License-Identifier: GPL-2.0
/*
 * DIAGNOSTIC BUILD ONLY - branch diag-scan, never merged into main.
 *
 * The 5.10.273 tree hangs inside the early initcall sequence. The hanging
 * build (branch diag-panic) prints the name of every initcall it is about to
 * run, into the ramoops console ring: 2 MiB at 0xa7000000, fed by every
 * printk through CONFIG_PSTORE_CONSOLE. That ring is the one part of the
 * diagnostic that is known to work: it is registered at 0.083 s and nothing
 * else on the device writes there.
 *
 * The open question is whether that memory is still there on the next boot.
 * A freeze ends with the person holding power until the phone dies, and this
 * device's dmesg zone does not exist (the device tree has console-size and
 * pmsg-size, but no record-size), so pstore has nothing to show. This build
 * answers that question directly: it is an otherwise unmodified 5.10.272 that
 * looks at the raw ring before the kernel's own ramoops probe runs, and prints
 * what it finds.
 *
 * Called from start_kernel() right after setup_arch(), which is when the
 * linear map exists (paging_init() ran) and before do_initcalls() - so before
 * postcore_initcall(ramoops_init) has a chance to see, and possibly zap, the
 * zone. 0xa7000000 is inside System RAM and the reserved-memory node has no
 * no-map property, so __va() is the right way in; no ioremap and no slab.
 *
 * Output, at KERN_ERR (console loglevel is 6, so pr_debug would never show):
 *
 *   UBDSCAN: ring 0xa7000000+2MiB nonzero=N sig=0x.. start=.. size=..
 *   UBDTAIL <seq> <symbol>          (one per kept line, ascending)
 *
 * The highest UBDTAIL sequence number is the initcall that never returned.
 * If there is no UBDTAIL line at all, the ring did not survive the reset and
 * the whole "leave a note in RAM" idea is finished for good.
 */
#define pr_fmt(fmt) "UBDSCAN: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <asm/memory.h>

#define UBD_PHYS	0x0a7000000UL
#define UBD_SIZE	0x00200000UL	/* 2 MiB console ring */
#define UBD_MAX		96		/* lines kept */
#define UBD_NAMELEN	72
#define UBD_MAXSEQ	100000000u	/* refuse to believe bigger */

struct ubd_hit {
	u32 seq;
	char name[UBD_NAMELEN];
};

static struct ubd_hit ubd_hits[UBD_MAX];
static unsigned int ubd_nhits;	/* kept */
static unsigned int ubd_nfound;	/* seen in the ring */

/* keep the UBD_MAX largest sequence numbers, ascending, min at [0] */
static void ubd_keep(u32 seq, const char *name)
{
	unsigned int i;
	struct ubd_hit t;

	if (ubd_nhits == UBD_MAX && seq <= ubd_hits[0].seq)
		return;

	if (ubd_nhits < UBD_MAX) {
		i = ubd_nhits++;
	} else {
		i = 0;			/* replace the smallest */
	}
	ubd_hits[i].seq = seq;
	strscpy(ubd_hits[i].name, name, UBD_NAMELEN);

	/* bubble up to its place */
	while (i > 0 && ubd_hits[i - 1].seq > ubd_hits[i].seq) {
		t = ubd_hits[i - 1];
		ubd_hits[i - 1] = ubd_hits[i];
		ubd_hits[i] = t;
		i--;
	}
}

static bool ubd_printable(u8 c)
{
	return c >= 0x20 && c < 0x7f;
}

void __init ubd_scan(void)
{
	const u8 *base = (const u8 *)__va(UBD_PHYS);
	const u8 *end = base + UBD_SIZE;
	const u8 *p;
	unsigned long nz = 0;
	u32 sig, start, size;
	char name[UBD_NAMELEN];
	unsigned int i;

	/*
	 * persistent_ram_buffer sits at the start of the zone: sig, start,
	 * size, then the text. Reading it tells us whether the kernel's own
	 * ramoops probe from the previous boot left a valid header behind.
	 */
	sig = ((const u32 *)base)[0];
	start = ((const u32 *)base)[1];
	size = ((const u32 *)base)[2];

	for (p = base; p < end; p++)
		if (*p)
			nz++;

	for (p = base; p + 9 < end; p++) {
		u32 seq = 0;
		unsigned int n = 0;
		bool digit = false;

		if (p[0] != 'U' || memcmp(p, "UBDCALL ", 8) != 0)
			continue;

		p += 8;
		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		while (p < end && *p >= '0' && *p <= '9') {
			digit = true;
			if (seq < UBD_MAXSEQ)
				seq = seq * 10 + (*p - '0');
			p++;
		}
		if (!digit || seq == 0)
			continue;

		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		while (p < end && n < UBD_NAMELEN - 1 && ubd_printable(*p) &&
		       *p != ' ') {
			name[n++] = *p++;
		}
		name[n] = '\0';

		ubd_nfound++;
		ubd_keep(seq, name);
		p--;			/* the loop's p++ moves past it */
	}

	pr_err("ring 0x%lx+%luKiB nonzero=%lu sig=0x%08x start=%u size=%u found=%u kept=%u\n",
	       (unsigned long)UBD_PHYS, (unsigned long)(UBD_SIZE >> 10), nz,
	       sig, start, size, ubd_nfound, ubd_nhits);

	if (!ubd_nfound) {
		pr_err("no UBDCALL lines - the ring did not survive the reset, "
		       "or that boot never wrote any\n");
		return;
	}

	if (ubd_nhits < ubd_nfound)
		pr_err("only the last %u of %u lines are shown, the ring wrapped\n",
		       ubd_nhits, ubd_nfound);

	for (unsigned int i = 0; i < ubd_nhits; i++)
		pr_err("UBDTAIL %u %s\n", ubd_hits[i].seq, ubd_hits[i].name);
}
