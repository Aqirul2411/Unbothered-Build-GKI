// SPDX-License-Identifier: GPL-2.0
/*
 * DIAGNOSTIC BUILD ONLY - branch diag-panic, never merged into main.
 *
 * The 5.10.273 merge freezes at the POCO logo somewhere in the early initcall
 * sequence. Three attempts to leave a note in RAM for a later boot have all
 * come back empty, because a freeze ends with the person holding power until
 * the phone dies, and a cold boot is free to reuse or wipe every byte of it.
 * The ramoops region at 0xa7000000 does not help either: its dmesg zones are
 * only written when the kernel *panics* (that is what kmsg_dump is for), so a
 * hang leaves nothing behind, which is exactly what was observed.
 *
 * So this build stops trying to survive the freeze and instead refuses to
 * freeze. Right before the initcall sequence it arms a timer on every online
 * CPU: if the early initcalls have not finished within 25 seconds, one of
 * those timers fires and the kernel panics on purpose. A panic runs kmsg_dump,
 * pstore writes the whole printk buffer into the ramoops dmesg zones, and the
 * next boot can read it back from /sys/fs/pstore - the same path Android uses
 * for its own crash logs, and one that survives a reset because the region is
 * reserved and the reset is warm.
 *
 * Paired with initcall_debug (default on in this build), the printk buffer
 * ends with "calling <function>" for the initcall that never returned. That
 * name is the whole point of the exercise.
 *
 * A healthy boot disarms the timers as soon as do_initcalls() returns, about a
 * second later, so nothing is left running and no timer can fire later.
 *
 * Nothing else in this tree differs from 159d9aac6.
 */
#define pr_fmt(fmt) "UBDPANIC: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/cpumask.h>
#include <linux/smp.h>
#include <linux/nmi.h>

#define UBD_SECS	25

static struct timer_list ubd_wd[NR_CPUS];
static bool ubd_armed;

static void ubd_wd_fn(struct timer_list *t)
{
	pr_emerg("%d s without getting past the early initcalls (cpu%d) - "
		 "panicking so the log is not lost\n", UBD_SECS,
		 raw_smp_processor_id());
	trigger_all_cpu_backtrace();
	panic("UBDPANIC: stuck in the early initcall sequence");
}

void __init ubd_watchdog_arm(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		timer_setup(&ubd_wd[cpu], ubd_wd_fn, 0);
		ubd_wd[cpu].expires = jiffies + UBD_SECS * HZ;
		add_timer_on(&ubd_wd[cpu], cpu);
	}
	ubd_armed = true;
	pr_emerg("armed: %d s to get past early initcalls, or this panics "
		 "instead of freezing\n", UBD_SECS);
}

void __init ubd_watchdog_disarm(void)
{
	int cpu;

	if (!ubd_armed)
		return;
	for_each_online_cpu(cpu)
		del_timer_sync(&ubd_wd[cpu]);
	ubd_armed = false;
	pr_emerg("disarmed: early initcalls finished, booting normally\n");
}
