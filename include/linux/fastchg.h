/*
 * based on work from:
 *	Chad Froebel <chadfroebel@gmail.com> &
 *	Jean-Pierre Rasquin <yank555.lu@gmail.com>
 * for backwards compatibility
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _LINUX_FASTCHG_H
#define _LINUX_FASTCHG_H

extern int force_fast_charge;
extern int fast_charge_level;
extern int fake_charge_ac;

#define FAST_CHARGE_DISABLED		0	/* default */
#define FAST_CHARGE_FORCE_AC		1
#define FAST_CHARGE_FORCE_CUSTOM_MA	2

#define FAST_CHARGE_500		500
#define FAST_CHARGE_900		900
#define FAST_CHARGE_1200	1200		/* default for FAST_CHARGE_FORCE_AC */
#define FAST_CHARGE_1500	1500
#define FAST_CHARGE_1800	1800
#define FAST_CHARGE_2000	2000

#define FAST_CHARGE_LEVELS	"500 900 1200 1500 1800 2000"

#define FAKE_CHARGE_AC_DISABLE	0		/* default */
#define FAKE_CHARGE_AC_ENABLE	1

#endif
