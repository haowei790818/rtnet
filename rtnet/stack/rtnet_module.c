/***
 *
 *  stack/rtnet_module.c - module framework, proc file system
 *
 *  Copyright (C) 2002      Ulrich Marx <marx@kammer.uni-hannover.de>
 *                2003-2005 Jan Kiszka <jan.kiszka@web.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <rtdev_mgr.h>
#include <rtnet_chrdev.h>
#include <rtnet_internal.h>
#include <rtnet_rtpc.h>
#include <stack_mgr.h>

MODULE_LICENSE("GPL");


struct rtnet_mgr STACK_manager;
struct rtnet_mgr RTDEV_manager;


/***
 *      proc filesystem section
 */
#ifdef CONFIG_PROC_FS

struct proc_dir_entry *rtnet_proc_root;


static int rtnet_read_proc_devices(char *buf, char **start, off_t offset,
                                   int count, int *eof, void *data)
{
    int i;
    int res;
    struct rtnet_device *rtdev;
    RTNET_PROC_PRINT_VARS(80);


    if (!RTNET_PROC_PRINT("Name\t\tFlags\n"))
        goto done;

    for (i = 1; i <= MAX_RT_DEVICES; i++) {
        rtdev = rtdev_get_by_index(i);
        if (rtdev != NULL) {
            res = RTNET_PROC_PRINT("%-15s %s%s%s%s\n",
                            rtdev->name,
                            (rtdev->flags & IFF_UP) ? "UP" : "DOWN",
                            (rtdev->flags & IFF_BROADCAST) ? " BROADCAST" : "",
                            (rtdev->flags & IFF_LOOPBACK) ? " LOOPBACK" : "",
                            (rtdev->flags & IFF_PROMISC) ? " PROMISC" : "");
            rtdev_dereference(rtdev);
            if (!res)
                break;
        }
    }

  done:
    RTNET_PROC_PRINT_DONE;
}



static int rtnet_read_proc_rtskb(char *buf, char **start, off_t offset, int count,
                                 int *eof, void *data)
{
    unsigned int rtskb_len;
    RTNET_PROC_PRINT_VARS(256);


    rtskb_len = ALIGN_RTSKB_STRUCT_LEN + SKB_DATA_ALIGN(RTSKB_SIZE);
    RTNET_PROC_PRINT("Statistics\t\tCurrent\tMaximum\n"
                     "rtskb pools\t\t%d\t%d\n"
                     "rtskbs\t\t\t%d\t%d\n"
                     "rtskb memory need\t%d\t%d\n",
                     rtskb_pools, rtskb_pools_max,
                     rtskb_amount, rtskb_amount_max,
                     rtskb_amount * rtskb_len, rtskb_amount_max * rtskb_len);

    RTNET_PROC_PRINT_DONE;
}



static int rtnet_read_proc_version(char *buf, char **start, off_t offset,
                                   int count, int *eof, void *data)
{
    const char verstr[] =
        "RTnet " RTNET_PACKAGE_VERSION " - built on " __DATE__ " " __TIME__ "\n"
        "RTcap:      "
#ifdef CONFIG_RTNET_RTCAP
            "yes\n"
#else
            "no\n"
#endif
        "rtnetproxy: "
#ifdef CONFIG_RTNET_PROXY
            "yes\n"
#else
            "no\n"
#endif
        "bug checks: "
#ifdef CONFIG_RTNET_CHECKED
            "yes\n";
#else
            "no\n";
#endif
    RTNET_PROC_PRINT_VARS(256);


    RTNET_PROC_PRINT(verstr);

    RTNET_PROC_PRINT_DONE;
}



static int rtnet_proc_register(void)
{
    struct proc_dir_entry *proc_entry;

    rtnet_proc_root = create_proc_entry("rtnet", S_IFDIR, 0);
    if (!rtnet_proc_root)
        goto error1;

    proc_entry = create_proc_entry("devices", S_IFREG | S_IRUGO | S_IWUSR,
                                   rtnet_proc_root);
    if (!proc_entry)
        goto error2;
    proc_entry->read_proc = rtnet_read_proc_devices;

    proc_entry = create_proc_entry("rtskb", S_IFREG | S_IRUGO | S_IWUSR,
                                   rtnet_proc_root);
    if (!proc_entry)
        goto error3;
    proc_entry->read_proc = rtnet_read_proc_rtskb;

    proc_entry = create_proc_entry("version", S_IFREG | S_IRUGO | S_IWUSR,
                                   rtnet_proc_root);
    if (!proc_entry)
        goto error4;
    proc_entry->read_proc = rtnet_read_proc_version;

    return 0;

  error4:
    remove_proc_entry("rtskb", rtnet_proc_root);

  error3:
    remove_proc_entry("devices", rtnet_proc_root);

  error2:
    remove_proc_entry("rtnet", 0);

  error1:
    /*ERRMSG*/printk("RTnet: unable to initialize /proc entries\n");
    return -1;
}



static void rtnet_proc_unregister(void)
{
    remove_proc_entry("devices", rtnet_proc_root);
    remove_proc_entry("rtskb", rtnet_proc_root);
    remove_proc_entry("version", rtnet_proc_root);
    remove_proc_entry("rtnet", 0);
}
#endif  /* CONFIG_PROC_FS */



/**
 *  rtnet_init()
 */
int __init rtnet_init(void)
{
    int err = 0;


    printk("\n*** RTnet " RTNET_PACKAGE_VERSION " - built on " __DATE__ " " __TIME__
           " ***\n\n");
    printk("RTnet: initialising real-time networking\n");

    if ((err = rtskb_pools_init()) != 0)
        goto err_out1;

#ifdef CONFIG_PROC_FS
    if ((err = rtnet_proc_register()) != 0)
        goto err_out2;
#endif

    /* initialize the Stack-Manager */
    if ((err = rt_stack_mgr_init(&STACK_manager)) != 0)
        goto err_out3;

    /* initialize the RTDEV-Manager */
    if ((err = rt_rtdev_mgr_init(&RTDEV_manager)) != 0)
        goto err_out4;

    rtnet_chrdev_init();

    if ((err = rtpc_init()) != 0)
        goto err_out5;

    return 0;


err_out5:
    rtnet_chrdev_release();
    rt_rtdev_mgr_delete(&RTDEV_manager);

err_out4:
    rt_stack_mgr_delete(&STACK_manager);

err_out3:
#ifdef CONFIG_PROC_FS
    rtnet_proc_unregister();
#endif

err_out2:
    rtskb_pools_release();

err_out1:
    return err;
}


/**
 *  rtnet_release()
 */
void rtnet_release(void)
{
    rtpc_cleanup();

    rtnet_chrdev_release();

    rt_stack_mgr_delete(&STACK_manager);
    rt_rtdev_mgr_delete(&RTDEV_manager);

    rtskb_pools_release();

#ifdef CONFIG_PROC_FS
    rtnet_proc_unregister();
#endif

    printk("RTnet: unloaded\n");
}


module_init(rtnet_init);
module_exit(rtnet_release);


EXPORT_SYMBOL(rtnet_proc_root);
EXPORT_SYMBOL(STACK_manager);
EXPORT_SYMBOL(RTDEV_manager);
