/* rtmac_module.c
 *
 * rtmac - real-time networking media access control subsystem
 * Copyright (C) 2002 Marc Kleine-Budde <kleine-budde@gmx.de>,
 *               2003 Jan Kiszka <Jan.Kiszka@web.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <rtmac/rtmac_disc.h>
#include <rtmac/rtmac_proc.h>
#include <rtmac/rtmac_proto.h>
#include <rtmac/rtmac_vnic.h>


int rtmac_init(void)
{
    int ret = 0;

    rt_printk("RTmac: init realtime media access control\n");

    rtmac_proto_init();

#ifdef CONFIG_PROC_FS
    ret = rtmac_proc_register();
    if (ret)
        goto error1;
#endif

    ret = rtmac_vnic_module_init();
    if (ret)
        goto error2;

    return 0;

error2:
#ifdef CONFIG_PROC_FS
    rtmac_proc_release();
#endif

error1:
    rtmac_proto_release();
    return ret;
}



void rtmac_release(void)
{
    printk("RTmac: unloaded\n");

    rtmac_proto_release();
#ifdef CONFIG_PROC_FS
    rtmac_proc_release();
#endif
    rtmac_vnic_module_cleanup();
}



module_init(rtmac_init);
module_exit(rtmac_release);

MODULE_AUTHOR("Marc Kleine-Budde, Jan Kiszka");
MODULE_LICENSE("GPL");
