// SPDX-License-Identifier: GPL-2.0
/*
 * DIAGNOSTIC BUILD ONLY - branch diag-panic, never merged into main.
 *
 * The 5.10.273 merge freezes at the POCO logo inside the early initcall
 * sequence, and every attempt to leave a note in RAM failed for the same
 * reason: we were writing into the wrong place. The device tree says what the
 * vendor's reserved region at 0xa7000000 is:
 *
 *   console-size = 0x200000   -> a 2 MiB printk ring at 0xa7000000
 *   pmsg-size    = 0x200000   -> a 2 MiB pmsg zone at 0xa7200000
 *   record-size  = absent     -> NO dmesg zone, so nothing is ever written on
 *                                panic and /sys/fs/pstore stays empty
 *
 * That explains a deliberate panic leaving pstore empty, and it explains the
 * earlier ring being clobbered: 0xa7200000 is the pmsg zone, which userspace
 * writes to on a running Android.
 *
 * What is left is the console ring: it is registered at 0.083 s, well before
 * the initcall sequence, and CONFIG_PSTORE_CONSOLE means every printk line is
 * copied into it synchronously. It is 2 MiB, reserved, and nothing else writes
 * there. So the cheapest possible trace is to print the initcall's name.
 *
 * pr_err, not pr_debug: console loglevel is 6 (loglevel=6 on the cmdline), so
 * KERN_DEBUG lines would never reach this console.
 *
 * The sequence number makes the ring's wrap-around irrelevant: a later boot
 * scans the ring for "UBDCALL <n> <symbol>" lines and the highest n is the
 * initcall that never returned.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/atomic.h>

static atomic_t ubd_seq = ATOMIC_INIT(0);

void __init ubd_ic(initcall_t fn)
{
	pr_err("UBDCALL %u %ps\n", (unsigned int)atomic_inc_return(&ubd_seq),
	       fn);
}
