/***
 *
 *  rtcfg/rtcfg_ioctl.c
 *
 *  Real-Time Configuration Distribution Protocol
 *
 *  Copyright (C) 2003, 2004 Jan Kiszka <jan.kiszka@web.de>
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

#include <asm/uaccess.h>
#include <linux/file.h>
#include <linux/vmalloc.h>

#include <rtcfg_chrdev.h>
#include <rtnet_rtpc.h>
#include <rtcfg/rtcfg_conn_event.h>
#include <rtcfg/rtcfg_event.h>
#include <rtcfg/rtcfg_frame.h>
#include <rtcfg/rtcfg_proc.h>


int rtcfg_event_handler(struct rt_proc_call *call)
{
    struct rtcfg_cmd *cmd_event;


    cmd_event = rtpc_get_priv(call, struct rtcfg_cmd);
    return rtcfg_do_main_event(cmd_event->ifindex, cmd_event->event_id, call);
}



void keep_cmd_add(struct rt_proc_call *call, void *priv_data)
{
    struct rtcfg_cmd *cmd;
    int              result = rtpc_get_result(call);


    /* do nothing on error (<0), or if file already present (=0) */
    if (result <= 0)
        return;

    cmd = rtpc_get_priv(call, struct rtcfg_cmd);

    /* Don't cleanup any buffers, we are going to recycle them! */
    rtpc_set_cleanup_handler(call, NULL);
}



void cleanup_cmd_add(struct rt_proc_call *call)
{
    struct rtcfg_cmd *cmd;
    void             *buf;


    cmd = rtpc_get_priv(call, struct rtcfg_cmd);

    /* unlock proc and update directory structure */
    rtcfg_unlockwr_proc(cmd->ifindex);

    buf = cmd->args.add.conn_buf;
    if (buf != NULL)
        kfree(buf);

    buf = cmd->args.add.stage1_data;
    if (buf != NULL)
        kfree(buf);

    if (cmd->args.add.stage2_file != NULL) {
        buf = cmd->args.add.stage2_file->buffer;
        if (buf != NULL)
            vfree(buf);
        kfree(cmd->args.add.stage2_file);
    }
}



void copy_stage_1_data(struct rt_proc_call *call, void *priv_data)
{
    struct rtcfg_cmd *cmd;
    int              result = rtpc_get_result(call);


    if (result <= 0)
        return;

    cmd = rtpc_get_priv(call, struct rtcfg_cmd);

    if (cmd->args.client.buffer_size < (size_t)result)
        rtpc_set_result(call, -ENOSPC);
    else if (copy_to_user(cmd->args.client.buffer,
                          cmd->args.client.rtskb->data, result) != 0)
        rtpc_set_result(call, -EFAULT);
}



void cleanup_cmd_client(struct rt_proc_call *call)
{
    struct rtcfg_cmd *cmd;
    void             *station_buf;
    struct rtskb     *rtskb;


    cmd = rtpc_get_priv(call, struct rtcfg_cmd);

    station_buf = cmd->args.client.station_buf;
    if (station_buf != NULL)
        kfree(station_buf);

    rtskb = cmd->args.client.rtskb;
    if (rtskb != NULL)
        kfree_rtskb(rtskb);
}



void copy_stage_2_data(struct rt_proc_call *call, void *priv_data)
{
    struct rtcfg_cmd *cmd;
    int              result = rtpc_get_result(call);
    struct rtskb     *rtskb;


    if (result <= 0)
        return;

    cmd = rtpc_get_priv(call, struct rtcfg_cmd);

    if (cmd->args.announce.buffer_size < (size_t)result)
        rtpc_set_result(call, -ENOSPC);
    else {
        rtskb = cmd->args.announce.rtskb;
        do {
            if (copy_to_user(cmd->args.announce.buffer,
                             rtskb->data, rtskb->len) != 0) {
                rtpc_set_result(call, -EFAULT);
                break;
            }
            cmd->args.announce.buffer += rtskb->len;
            rtskb = rtskb->next;
        } while (rtskb != NULL);
    }
}



void cleanup_cmd_announce(struct rt_proc_call *call)
{
    struct rtcfg_cmd *cmd;
    struct rtskb     *rtskb;


    cmd = rtpc_get_priv(call, struct rtcfg_cmd);

    rtskb = cmd->args.announce.rtskb;
    if (rtskb != NULL)
        kfree_rtskb(rtskb);
}



int rtcfg_ioctl_add(struct rtnet_device *rtdev, struct rtcfg_cmd *cmd)
{
    struct rtcfg_connection *conn_buf;
    struct rtcfg_file       *file = NULL;
    void                    *data_buf;
    size_t                  size;
    int                     ret;


    conn_buf = kmalloc(sizeof(struct rtcfg_connection), GFP_KERNEL);
    if (conn_buf == NULL)
        return -ENOMEM;
    cmd->args.add.conn_buf = conn_buf;

    data_buf = NULL;
    size = cmd->args.add.stage1_size;
    if (size > 0) {
        /* check stage 1 data size */
        if (sizeof(struct rtcfg_frm_stage_1_cfg) +
            2*RTCFG_ADDRSIZE_IP + size > rtdev->mtu) {
            ret = -ESTAGE1SIZE;
            goto err;
        }

        data_buf = kmalloc(size, GFP_KERNEL);
        if (data_buf == NULL) {
            ret = -ENOMEM;
            goto err;
        }

        ret = copy_from_user(data_buf, cmd->args.add.stage1_data, size);
        if (ret != 0) {
            ret = -EFAULT;
            goto err;
        }
    }
    cmd->args.add.stage1_data = data_buf;

    if (cmd->args.add.stage2_filename != NULL) {
        size = strnlen_user(cmd->args.add.stage2_filename, PATH_MAX);

        file = kmalloc(sizeof(struct rtcfg_file) + size, GFP_KERNEL);
        if (file == NULL) {
            ret = -ENOMEM;
            goto err;
        }

        file->name   = ((char *)file) + sizeof(struct rtcfg_file);
        file->buffer = NULL;

        ret = copy_from_user((char *)file->name,
                             (char *)cmd->args.add.stage2_filename,
                             size);
        if (ret != 0) {
            ret = -EFAULT;
            goto err;
        }
    }
    cmd->args.add.stage2_file = file;

    /* lock proc structure for modification */
    rtcfg_lockwr_proc(cmd->ifindex);

    ret = rtpc_dispatch_call(rtcfg_event_handler, 0, cmd,
                             sizeof(*cmd), keep_cmd_add,
                             cleanup_cmd_add);

    /* load file if missing */
    if (ret > 0) {
        struct file  *filp;
        mm_segment_t oldfs;


        filp = filp_open(file->name, O_RDONLY, 0);
        if (IS_ERR(filp)) {
            rtcfg_unlockwr_proc(cmd->ifindex);
            ret = PTR_ERR(filp);
            goto err;
        }

        file->size = filp->f_dentry->d_inode->i_size;

        file->buffer = vmalloc(file->size);
        if (file->buffer == NULL) {
            rtcfg_unlockwr_proc(cmd->ifindex);
            fput(filp);
            ret = -ENOMEM;
            goto err;
        }

        oldfs = get_fs();
        set_fs(KERNEL_DS);
        filp->f_pos = 0;

        ret = filp->f_op->read(filp, file->buffer, file->size,
                               &filp->f_pos);

        set_fs(oldfs);
        fput(filp);

        if (ret != (int)file->size) {
            rtcfg_unlockwr_proc(cmd->ifindex);
            ret = -EIO;
            goto err;
        }

        /* dispatch again, this time with new file attached */
        ret = rtpc_dispatch_call(rtcfg_event_handler, 0, cmd,
                                 sizeof(*cmd), NULL, cleanup_cmd_add);
    }

    return ret;

  err:
    kfree(conn_buf);
    if (data_buf != NULL)
        kfree(data_buf);
    if (file != NULL) {
        if (file->buffer != NULL)
            vfree(file->buffer);
        kfree(data_buf);
    }
    return ret;
}



int rtcfg_ioctl(struct rtnet_device *rtdev, unsigned int request, unsigned long arg)
{
    struct rtcfg_cmd        cmd;
    struct rtcfg_station    *station_buf;
    int                     ret;


    ret = copy_from_user(&cmd, (void *)arg, sizeof(cmd));
    if (ret != 0)
        return -EFAULT;

    cmd.ifindex  = rtdev->ifindex;
    cmd.event_id = _IOC_NR(request);

    switch (request) {
        case RTCFG_IOC_SERVER:
            ret = rtpc_dispatch_call(rtcfg_event_handler, 0, &cmd,
                                     sizeof(cmd), NULL, NULL);
            break;

        case RTCFG_IOC_ADD_IP:
        case RTCFG_IOC_ADD_MAC:
        case RTCFG_IOC_ADD_IP_MAC:
            ret = rtcfg_ioctl_add(rtdev, &cmd);
            break;

        case RTCFG_IOC_WAIT:
            ret = rtpc_dispatch_call(rtcfg_event_handler,
                                     cmd.args.wait.timeout, &cmd,
                                     sizeof(cmd), NULL, NULL);
            break;

        case RTCFG_IOC_CLIENT:
            station_buf = kmalloc(sizeof(struct rtcfg_station) *
                                  cmd.args.client.max_stations, GFP_KERNEL);
            if (station_buf == NULL)
                return -ENOMEM;
            cmd.args.client.station_buf = station_buf;
            cmd.args.client.rtskb       = NULL;

            ret = rtpc_dispatch_call(rtcfg_event_handler,
                                     cmd.args.client.timeout, &cmd,
                                     sizeof(cmd), copy_stage_1_data,
                                     cleanup_cmd_client);
            break;

        case RTCFG_IOC_ANNOUNCE:
            cmd.args.announce.rtskb = NULL;

            ret = rtpc_dispatch_call(rtcfg_event_handler,
                                     cmd.args.announce.timeout, &cmd,
                                     sizeof(cmd), copy_stage_2_data,
                                     cleanup_cmd_announce);
            break;

        case RTCFG_IOC_READY:
            ret = rtpc_dispatch_call(rtcfg_event_handler,
                                     cmd.args.ready.timeout, &cmd,
                                     sizeof(cmd), NULL, NULL);
            break;

        default:
            ret = -ENOTTY;
    }

    return ret;
}



struct rtnet_ioctls rtcfg_ioctls = {
    service_name:   "RTcfg",
    ioctl_type:     RTNET_IOC_TYPE_RTCFG,
    handler:        rtcfg_ioctl
};
