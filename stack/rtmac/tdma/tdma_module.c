/***
 *
 *  rtmac/tdma/tdma_module.c
 *
 *  RTmac - real-time networking media access control subsystem
 *  Copyright (C) 2002      Marc Kleine-Budde <kleine-budde@gmx.de>,
 *                2003-2005 Jan Kiszka <Jan.Kiszka@web.de>
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

#include <asm/div64.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include <rtnet_sys.h>
#include <rtmac/rtmac_vnic.h>
#include <rtmac/tdma/tdma.h>
#include <rtmac/tdma/tdma_dev.h>
#include <rtmac/tdma/tdma_ioctl.h>
#include <rtmac/tdma/tdma_proto.h>
#include <rtmac/tdma/tdma_worker.h>


/* RTAI-specific: start scheduling timer */
#ifdef CONFIG_RTOS_STARTSTOP_TIMER
static int start_timer = 0;

module_param(start_timer, int, 0444);
MODULE_PARM_DESC(start_timer, "set to non-zero to start RTAI timer");
#endif

#ifdef CONFIG_PROC_FS
int tdma_proc_read(char *buf, char **start, off_t offset, int count,
                    int *eof, void *data)
{
    struct rtnet_device *rtdev = NULL;
    struct tdma_priv    *tdma;
    const char          *state;
    int                 d;
#ifdef CONFIG_RTNET_TDMA_MASTER
    u64                 cycle;
#endif
    RTNET_PROC_PRINT_VARS(80);


    if (!RTNET_PROC_PRINT("Interface       API Device      Operation Mode  "
                          "Cycle   State\n"))
        goto done;

    for (d = 1; d <= MAX_RT_DEVICES; d++) {
        rtdev = rtdev_get_by_index(d);
        if (!rtdev)
            continue;

        if (mutex_lock_interruptible(&rtdev->nrt_lock)) {
            rtdev_dereference(rtdev);
            rtdev = NULL;
            break;
        }

        if (!rtdev->mac_priv)
            goto unlock_dev;
        tdma = (struct tdma_priv *)rtdev->mac_priv->disc_priv;

        if (!RTNET_PROC_PRINT("%-15s %-15s ", rtdev->name,
                              tdma->api_device.device_name))
            break;
        if (test_bit(TDMA_FLAG_CALIBRATED, &tdma->flags)) {
#ifdef CONFIG_RTNET_TDMA_MASTER
            if (test_bit(TDMA_FLAG_BACKUP_MASTER, &tdma->flags) &&
                !test_bit(TDMA_FLAG_BACKUP_ACTIVE, &tdma->flags))
                state = "stand-by";
            else
#endif /* CONFIG_RTNET_TDMA_MASTER */
                state = "active";
        } else
            state = "init";
#ifdef CONFIG_RTNET_TDMA_MASTER
        if (test_bit(TDMA_FLAG_MASTER, &tdma->flags)) {
            cycle = tdma->cycle_period + 500;
            do_div(cycle, 1000);
            if (test_bit(TDMA_FLAG_BACKUP_MASTER, &tdma->flags)) {
                if (!RTNET_PROC_PRINT("Backup Master   %-7ld %s\n",
                                      (unsigned long)cycle, state))
                    break;
            } else {
                if (!RTNET_PROC_PRINT("Master          %-7ld %s\n",
                                      (unsigned long)cycle, state))
                    break;
            }
        } else
#endif /* CONFIG_RTNET_TDMA_MASTER */
            if (!RTNET_PROC_PRINT("Slave           -       %s\n", state))
                break;
unlock_dev:
        mutex_unlock(&rtdev->nrt_lock);
        rtdev_dereference(rtdev);
        rtdev = NULL;
    }

done:
    if (rtdev) {
        mutex_unlock(&rtdev->nrt_lock);
        rtdev_dereference(rtdev);
    }
    RTNET_PROC_PRINT_DONE;
}



int tdma_slots_proc_read(char *buf, char **start, off_t offset, int count,
                         int *eof, void *data)
{
    struct rtnet_device *rtdev = NULL;
    struct tdma_priv    *tdma;
    struct tdma_slot    *slot;
    int                 d, i;
    int                 jnt_id;
    u64                 slot_offset;
    RTNET_PROC_PRINT_VARS(80);


    if (!RTNET_PROC_PRINT("Interface       "
                          "Slots (id[->joint]:offset:phasing/period:size)\n"))
        goto done;

    for (d = 1; d <= MAX_RT_DEVICES; d++) {
        rtdev = rtdev_get_by_index(d);
        if (!rtdev)
            continue;

        if (mutex_lock_interruptible(&rtdev->nrt_lock)) {
            rtdev_dereference(rtdev);
            rtdev = NULL;
            break;
        }

        if (!rtdev->mac_priv)
            goto unlock_dev;
        tdma = (struct tdma_priv *)rtdev->mac_priv->disc_priv;

        if (!RTNET_PROC_PRINT("%-15s ", rtdev->name))
            break;

#ifdef CONFIG_RTNET_TDMA_MASTER
        if (test_bit(TDMA_FLAG_BACKUP_MASTER, &tdma->flags)) {
            slot_offset = tdma->backup_sync_inc - tdma->cycle_period + 500;
            do_div(slot_offset, 1000);
            if (!RTNET_PROC_PRINT("bak:%ld  ", (unsigned long)slot_offset))
                break;
        }
#endif /* CONFIG_RTNET_TDMA_MASTER */

        if (tdma->slot_table)
            for (i = 0; i <= tdma->max_slot_id; i++) {
                slot = tdma->slot_table[i];
                if (!slot ||
                    ((i == DEFAULT_NRT_SLOT) &&
                     (tdma->slot_table[DEFAULT_SLOT] == slot)))
                    continue;

                if (slot->queue == &slot->local_queue) {
                    if (!RTNET_PROC_PRINT("%d", i))
                        goto done;
                } else
                    for (jnt_id = 0; jnt_id <= tdma->max_slot_id; jnt_id++)
                        if (&tdma->slot_table[jnt_id]->local_queue ==
                            slot->queue) {
                            if (!RTNET_PROC_PRINT("%d->%d", i, jnt_id))
                                goto done;
                            break;
                        }

                slot_offset = slot->offset + 500;
                do_div(slot_offset, 1000);
                if (!RTNET_PROC_PRINT(":%ld:%d/%d:%d  ",
                        (unsigned long)slot_offset, slot->phasing + 1,
                        slot->period, slot->mtu))
                    goto done;
            }

        if (!RTNET_PROC_PRINT("\n"))
            break;

unlock_dev:
        mutex_unlock(&rtdev->nrt_lock);
        rtdev_dereference(rtdev);
        rtdev = NULL;
    }

done:
    if (rtdev) {
        mutex_unlock(&rtdev->nrt_lock);
        rtdev_dereference(rtdev);
    }
    RTNET_PROC_PRINT_DONE;
}
#endif /* CONFIG_PROC_FS */



int tdma_attach(struct rtnet_device *rtdev, void *priv)
{
    struct tdma_priv   *tdma = (struct tdma_priv *)priv;
    int                 ret;


    memset(tdma, 0, sizeof(struct tdma_priv));

    tdma->magic        = TDMA_MAGIC;
    tdma->rtdev        = rtdev;

    rtdm_lock_init(&tdma->lock);

    rtdm_event_init(&tdma->worker_wakeup, 0);
    rtdm_event_init(&tdma->xmit_event, 0);
    rtdm_event_init(&tdma->sync_event, 0);

    ret = tdma_dev_init(rtdev, tdma);
    if (ret < 0)
        goto err_out1;

    ret = rtdm_task_init(&tdma->worker_task, "rtnet-tdma", tdma_worker, tdma,
                         DEF_WORKER_PRIO, 0);
    if (ret != 0)
        goto err_out2;

    RTNET_MOD_INC_USE_COUNT;

    return 0;


  err_out2:
    tdma_dev_release(tdma);

  err_out1:
    rtdm_event_destroy(&tdma->sync_event);
    rtdm_event_destroy(&tdma->xmit_event);
    rtdm_event_destroy(&tdma->worker_wakeup);

    return ret;
}



int tdma_detach(struct rtnet_device *rtdev, void *priv)
{
    struct tdma_priv    *tdma = (struct tdma_priv *)priv;
    struct tdma_job     *job, *tmp;
    int                 err;


    set_bit(TDMA_FLAG_SHUTDOWN, &tdma->flags);

    rtdm_event_destroy(&tdma->sync_event);
    rtdm_event_destroy(&tdma->xmit_event);
    rtdm_event_destroy(&tdma->worker_wakeup);

    err = tdma_dev_release(tdma);
    if (err < 0)
        return err;

    rtdm_task_join_nrt(&tdma->worker_task, 100);

    list_for_each_entry_safe(job, tmp, &tdma->first_job->entry, entry) {
        if (job->id >= 0)
            tdma_cleanup_slot(tdma, SLOT_JOB(job));
        else if (job->id == XMIT_RPL_CAL) {
            __list_del(job->entry.prev, job->entry.next);
            kfree_rtskb(REPLY_CAL_JOB(job)->reply_rtskb);
        }
    }

    if (tdma->slot_table)
        kfree(tdma->slot_table);

#ifdef CONFIG_RTNET_TDMA_MASTER
    if (test_bit(TDMA_FLAG_MASTER, &tdma->flags))
        rtskb_pool_release(&tdma->cal_rtskb_pool);
#endif

    RTNET_MOD_DEC_USE_COUNT;

    return 0;
}



#ifdef CONFIG_PROC_FS
struct rtmac_proc_entry tdma_proc_entries[] = {
    { name: "tdma", handler: tdma_proc_read },
    { name: "tdma_slots", handler: tdma_slots_proc_read },
    { name: NULL, handler: NULL }
};
#endif /* CONFIG_PROC_FS */

struct rtmac_disc tdma_disc = {
    name:           "TDMA",
    priv_size:      sizeof(struct tdma_priv),
    disc_type:      __constant_htons(RTMAC_TYPE_TDMA),

    packet_rx:      tdma_packet_rx,
    rt_packet_tx:   tdma_rt_packet_tx,
    nrt_packet_tx:  tdma_nrt_packet_tx,

    get_mtu:        tdma_get_mtu,

    vnic_xmit:      RTMAC_DEFAULT_VNIC,

    attach:         tdma_attach,
    detach:         tdma_detach,

    ioctls:         {
        service_name:   "RTmac/TDMA",
        ioctl_type:     RTNET_IOC_TYPE_RTMAC_TDMA,
        handler:        tdma_ioctl
    },

#ifdef CONFIG_PROC_FS
    proc_entries:   tdma_proc_entries
#endif /* CONFIG_PROC_FS */
};



int __init tdma_init(void)
{
    int ret;


    printk("RTmac/TDMA: init time division multiple access control "
           "mechanism\n");

    ret = rtmac_disc_register(&tdma_disc);
    if (ret < 0)
        return ret;

#ifdef CONFIG_RTOS_STARTSTOP_TIMER
    if (start_timer)
        rtos_timer_start();
#endif

    return 0;
}



void tdma_release(void)
{
    rtmac_disc_deregister(&tdma_disc);

#ifdef CONFIG_RTOS_STARTSTOP_TIMER
    if (start_timer)
        rtos_timer_stop();
#endif

    printk("RTmac/TDMA: unloaded\n");
}



module_init(tdma_init);
module_exit(tdma_release);

MODULE_AUTHOR("Jan Kiszka");
MODULE_LICENSE("GPL");
