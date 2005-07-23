/***
 *
 *  rtcfg/rtcfg_frame.c
 *
 *  Real-Time Configuration Distribution Protocol
 *
 *  Copyright (C) 2003-2005 Jan Kiszka <jan.kiszka@web.de>
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/if_ether.h>

#include <stack_mgr.h>
#include <rtcfg/rtcfg.h>
#include <rtcfg/rtcfg_conn_event.h>
#include <rtcfg/rtcfg_frame.h>


static unsigned int num_rtskbs = 32;
MODULE_PARM(num_rtskbs, "i");
MODULE_PARM_DESC(num_rtskbs, "Number of realtime socket buffers used by RTcfg");

static struct rtskb_queue   rtcfg_pool;
static rtos_task_t          rx_task;
static rtos_event_t         rx_event;
static struct rtskb_queue   rx_queue;



static int rtcfg_rx_handler(struct rtskb *rtskb, struct rtpacket_type *pt)
{
    if (rtskb_acquire(rtskb, &rtcfg_pool) == 0) {
        rtdev_reference(rtskb->rtdev);
        rtskb_queue_tail(&rx_queue, rtskb);
        rtos_event_signal(&rx_event);
    } else
        kfree_rtskb(rtskb);

    return 0;
}



static void rtcfg_rx_task(void *arg)
{
    struct rtskb          *rtskb;
    struct rtcfg_frm_head *frm_head;
    struct rtnet_device   *rtdev;


    while (rtos_event_wait(&rx_event, 0) == 0)
        while ((rtskb = rtskb_dequeue(&rx_queue))) {
            rtdev = rtskb->rtdev;

            if (rtskb->pkt_type == PACKET_OTHERHOST) {
                rtdev_dereference(rtdev);
                kfree_rtskb(rtskb);
                continue;
            }

            if (rtskb->len < sizeof(struct rtcfg_frm_head)) {
                RTCFG_DEBUG(1, "RTcfg: %s() received an invalid frame\n",
                            __FUNCTION__);
                rtdev_dereference(rtdev);
                kfree_rtskb(rtskb);
                continue;
            }

            frm_head = (struct rtcfg_frm_head *)rtskb->data;

            if (rtcfg_do_main_event(rtskb->rtdev->ifindex,
                                    frm_head->id + RTCFG_FRM_STAGE_1_CFG,
                                    rtskb) < 0)
                kfree_rtskb(rtskb);

            rtdev_dereference(rtdev);
        }
}



int rtcfg_send_frame(struct rtskb *rtskb, struct rtnet_device *rtdev,
                     u8 *dest_addr)
{
    int ret;


    rtskb->rtdev    = rtdev;
    rtskb->priority = RTCFG_SKB_PRIO;

    if (rtdev->hard_header) {
        ret = rtdev->hard_header(rtskb, rtdev, ETH_RTCFG, dest_addr,
                                 rtdev->dev_addr, rtskb->len);
        if (ret < 0)
            goto err;
    }

    if ((rtdev->flags & IFF_UP) != 0) {
        ret = 0;
        if (rtdev_xmit(rtskb) != 0)
            ret = -EAGAIN;
    } else {
        ret = -ENETDOWN;
        goto err;
    }

    rtdev_dereference(rtdev);
    return ret;

  err:
    kfree_rtskb(rtskb);
    rtdev_dereference(rtdev);
    return ret;
}



int rtcfg_send_stage_1(struct rtcfg_connection *conn)
{
    struct rtnet_device          *rtdev;
    struct rtskb                 *rtskb;
    unsigned int                 rtskb_size;
    struct rtcfg_frm_stage_1_cfg *stage_1_frm;


    rtdev = rtdev_get_by_index(conn->ifindex);
    if (rtdev == NULL)
        return -ENODEV;

    rtskb_size = rtdev->hard_header_len +
        sizeof(struct rtcfg_frm_stage_1_cfg) + conn->stage1_size +
        (((conn->addr_type & RTCFG_ADDR_MASK) == RTCFG_ADDR_IP) ?
        2*RTCFG_ADDRSIZE_IP : 0);

    rtskb = alloc_rtskb(rtskb_size, &rtcfg_pool);
    if (rtskb == NULL) {
        rtdev_dereference(rtdev);
        return -ENOBUFS;
    }

    rtskb_reserve(rtskb, rtdev->hard_header_len);

    stage_1_frm = (struct rtcfg_frm_stage_1_cfg *)
        rtskb_put(rtskb, sizeof(struct rtcfg_frm_stage_1_cfg));

    stage_1_frm->head.id      = RTCFG_ID_STAGE_1_CFG;
    stage_1_frm->head.version = 0;
    stage_1_frm->addr_type    = conn->addr_type & RTCFG_ADDR_MASK;

    if (stage_1_frm->addr_type == RTCFG_ADDR_IP) {
        rtskb_put(rtskb, 2*RTCFG_ADDRSIZE_IP);

        *(u32*)stage_1_frm->client_addr = conn->addr.ip_addr;

        stage_1_frm = (struct rtcfg_frm_stage_1_cfg *)
            (((u8 *)stage_1_frm) + RTCFG_ADDRSIZE_IP);

        *(u32*)stage_1_frm->server_addr = rtdev->local_ip;

        stage_1_frm = (struct rtcfg_frm_stage_1_cfg *)
            (((u8 *)stage_1_frm) + RTCFG_ADDRSIZE_IP);
    }

    stage_1_frm->burstrate = device[conn->ifindex].burstrate;
    stage_1_frm->cfg_len   = htons(conn->stage1_size);

    memcpy(rtskb_put(rtskb, conn->stage1_size), conn->stage1_data,
           conn->stage1_size);

    return rtcfg_send_frame(rtskb, rtdev, conn->mac_addr);
}



int rtcfg_send_stage_2(struct rtcfg_connection *conn, int send_data)
{
    struct rtnet_device          *rtdev;
    struct rtcfg_device          *rtcfg_dev = &device[conn->ifindex];
    struct rtskb                 *rtskb;
    unsigned int                 rtskb_size;
    struct rtcfg_frm_stage_2_cfg *stage_2_frm;
    size_t                       total_size;
    size_t                       frag_size;


    rtdev = rtdev_get_by_index(conn->ifindex);
    if (rtdev == NULL)
        return -ENODEV;

    if (send_data) {
        total_size = conn->stage2_file->size;
        frag_size  = MIN(rtdev->get_mtu(rtdev, RTCFG_SKB_PRIO) -
                         sizeof(struct rtcfg_frm_stage_2_cfg), total_size);
    } else {
        total_size = 0;
        frag_size  = 0;
    }

    rtskb_size = rtdev->hard_header_len +
        sizeof(struct rtcfg_frm_stage_2_cfg) + frag_size;

    rtskb = alloc_rtskb(rtskb_size, &rtcfg_pool);
    if (rtskb == NULL) {
        rtdev_dereference(rtdev);
        return -ENOBUFS;
    }

    rtskb_reserve(rtskb, rtdev->hard_header_len);

    stage_2_frm = (struct rtcfg_frm_stage_2_cfg *)
        rtskb_put(rtskb, sizeof(struct rtcfg_frm_stage_2_cfg));

    stage_2_frm->head.id          = RTCFG_ID_STAGE_2_CFG;
    stage_2_frm->head.version     = 0;
    stage_2_frm->flags            = rtcfg_dev->flags;
    stage_2_frm->stations         = htonl(rtcfg_dev->other_stations);
    stage_2_frm->heartbeat_period = htons(rtcfg_dev->spec.srv.heartbeat);
    stage_2_frm->cfg_len          = htonl(total_size);

    if (send_data)
        memcpy(rtskb_put(rtskb, frag_size), conn->stage2_file->buffer,
               frag_size);
    conn->cfg_offs = frag_size;

    return rtcfg_send_frame(rtskb, rtdev, conn->mac_addr);
}



int rtcfg_send_stage_2_frag(struct rtcfg_connection *conn)
{
    struct rtnet_device               *rtdev;
    struct rtskb                      *rtskb;
    unsigned int                      rtskb_size;
    struct rtcfg_frm_stage_2_cfg_frag *stage_2_frm;
    size_t                            frag_size;


    rtdev = rtdev_get_by_index(conn->ifindex);
    if (rtdev == NULL)
        return -ENODEV;

    frag_size = MIN(rtdev->get_mtu(rtdev, RTCFG_SKB_PRIO) -
                    sizeof(struct rtcfg_frm_stage_2_cfg_frag),
                    conn->stage2_file->size - conn->cfg_offs);

    rtskb_size = rtdev->hard_header_len +
        sizeof(struct rtcfg_frm_stage_2_cfg_frag) + frag_size;

    rtskb = alloc_rtskb(rtskb_size, &rtcfg_pool);
    if (rtskb == NULL) {
        rtdev_dereference(rtdev);
        return -ENOBUFS;
    }

    rtskb_reserve(rtskb, rtdev->hard_header_len);

    stage_2_frm = (struct rtcfg_frm_stage_2_cfg_frag *)
        rtskb_put(rtskb, sizeof(struct rtcfg_frm_stage_2_cfg_frag));

    stage_2_frm->head.id      = RTCFG_ID_STAGE_2_CFG_FRAG;
    stage_2_frm->head.version = 0;
    stage_2_frm->frag_offs    = htonl(conn->cfg_offs);

    memcpy(rtskb_put(rtskb, frag_size),
           conn->stage2_file->buffer + conn->cfg_offs, frag_size);
    conn->cfg_offs += frag_size;

    return rtcfg_send_frame(rtskb, rtdev, conn->mac_addr);
}



int rtcfg_send_announce_new(int ifindex)
{
    struct rtcfg_device       *rtcfg_dev = &device[ifindex];
    struct rtnet_device       *rtdev;
    struct rtskb              *rtskb;
    unsigned int              rtskb_size;
    struct rtcfg_frm_announce *announce_new;


    rtdev = rtdev_get_by_index(ifindex);
    if (rtdev == NULL)
        return -ENODEV;

    rtskb_size = rtdev->hard_header_len + sizeof(struct rtcfg_frm_announce) +
        (((rtcfg_dev->spec.clt.addr_type & RTCFG_ADDR_MASK) == RTCFG_ADDR_IP) ?
        RTCFG_ADDRSIZE_IP : 0);

    rtskb = alloc_rtskb(rtskb_size, &rtcfg_pool);
    if (rtskb == NULL) {
        rtdev_dereference(rtdev);
        return -ENOBUFS;
    }

    rtskb_reserve(rtskb, rtdev->hard_header_len);

    announce_new = (struct rtcfg_frm_announce *)
        rtskb_put(rtskb, sizeof(struct rtcfg_frm_announce));

    announce_new->head.id      = RTCFG_ID_ANNOUNCE_NEW;
    announce_new->head.version = 0;
    announce_new->addr_type    = rtcfg_dev->spec.clt.addr_type;

    if (announce_new->addr_type == RTCFG_ADDR_IP) {
        rtskb_put(rtskb, RTCFG_ADDRSIZE_IP);

        *(u32*)announce_new->addr = rtdev->local_ip;

        announce_new = (struct rtcfg_frm_announce *)
            (((u8 *)announce_new) + RTCFG_ADDRSIZE_IP);
    }

    announce_new->flags     = rtcfg_dev->flags;
    announce_new->burstrate = rtcfg_dev->burstrate;

    return rtcfg_send_frame(rtskb, rtdev, rtdev->broadcast);
}



int rtcfg_send_announce_reply(int ifindex, u8 *dest_mac_addr)
{
    struct rtcfg_device       *rtcfg_dev = &device[ifindex];
    struct rtnet_device       *rtdev;
    struct rtskb              *rtskb;
    unsigned int              rtskb_size;
    struct rtcfg_frm_announce *announce_rpl;


    rtdev = rtdev_get_by_index(ifindex);
    if (rtdev == NULL)
        return -ENODEV;

    rtskb_size = rtdev->hard_header_len +
        sizeof(struct rtcfg_frm_announce) +
        ((rtcfg_dev->spec.clt.addr_type == RTCFG_ADDR_IP) ?
        RTCFG_ADDRSIZE_IP : 0);

    rtskb = alloc_rtskb(rtskb_size, &rtcfg_pool);
    if (rtskb == NULL) {
        rtdev_dereference(rtdev);
        return -ENOBUFS;
    }

    rtskb_reserve(rtskb, rtdev->hard_header_len);

    announce_rpl = (struct rtcfg_frm_announce *)
        rtskb_put(rtskb, sizeof(struct rtcfg_frm_announce));

    announce_rpl->head.id      = RTCFG_ID_ANNOUNCE_REPLY;
    announce_rpl->head.version = 0;
    announce_rpl->addr_type    = rtcfg_dev->spec.clt.addr_type;

    if (announce_rpl->addr_type == RTCFG_ADDR_IP) {
        rtskb_put(rtskb, RTCFG_ADDRSIZE_IP);

        *(u32*)announce_rpl->addr = rtdev->local_ip;

        announce_rpl = (struct rtcfg_frm_announce *)
            (((u8 *)announce_rpl) + RTCFG_ADDRSIZE_IP);
    }

    announce_rpl->flags     = rtcfg_dev->flags & RTCFG_FLAG_READY;
    announce_rpl->burstrate = 0; /* padding field */

    return rtcfg_send_frame(rtskb, rtdev, dest_mac_addr);
}



int rtcfg_send_ack(int ifindex)
{
    struct rtnet_device      *rtdev;
    struct rtskb             *rtskb;
    unsigned int             rtskb_size;
    struct rtcfg_frm_ack_cfg *ack_frm;


    rtdev = rtdev_get_by_index(ifindex);
    if (rtdev == NULL)
        return -ENODEV;

    rtskb_size = rtdev->hard_header_len + sizeof(struct rtcfg_frm_ack_cfg);

    rtskb = alloc_rtskb(rtskb_size, &rtcfg_pool);
    if (rtskb == NULL) {
        rtdev_dereference(rtdev);
        return -ENOBUFS;
    }

    rtskb_reserve(rtskb, rtdev->hard_header_len);

    ack_frm = (struct rtcfg_frm_ack_cfg *)
        rtskb_put(rtskb, sizeof(struct rtcfg_frm_ack_cfg));

    ack_frm->head.id      = RTCFG_ID_ACK_CFG;
    ack_frm->head.version = 0;
    ack_frm->ack_len      = htonl(device[ifindex].spec.clt.cfg_offs);

    return rtcfg_send_frame(rtskb, rtdev,
                            device[ifindex].spec.clt.srv_mac_addr);
}



int rtcfg_send_simple_frame(int ifindex, int frame_id, u8 *dest_addr)
{
    struct rtnet_device     *rtdev;
    struct rtskb            *rtskb;
    unsigned int            rtskb_size;
    struct rtcfg_frm_simple *simple_frm;


    rtdev = rtdev_get_by_index(ifindex);
    if (rtdev == NULL)
        return -ENODEV;

    rtskb_size = rtdev->hard_header_len + sizeof(struct rtcfg_frm_simple);

    rtskb = alloc_rtskb(rtskb_size, &rtcfg_pool);
    if (rtskb == NULL) {
        rtdev_dereference(rtdev);
        return -ENOBUFS;
    }

    rtskb_reserve(rtskb, rtdev->hard_header_len);

    simple_frm = (struct rtcfg_frm_simple *)
        rtskb_put(rtskb, sizeof(struct rtcfg_frm_simple));

    simple_frm->head.id      = frame_id;
    simple_frm->head.version = 0;

    return rtcfg_send_frame(rtskb, rtdev,
                            (dest_addr) ? dest_addr : rtdev->broadcast);
}



int rtcfg_send_dead_station(struct rtcfg_connection *conn)
{
    struct rtnet_device           *rtdev;
    struct rtskb                  *rtskb;
    unsigned int                  rtskb_size;
    struct rtcfg_frm_dead_station *dead_station_frm;


    rtdev = rtdev_get_by_index(conn->ifindex);
    if (rtdev == NULL)
        return -ENODEV;

    rtskb_size = rtdev->hard_header_len +
        sizeof(struct rtcfg_frm_dead_station) +
        (((conn->addr_type & RTCFG_ADDR_MASK) == RTCFG_ADDR_IP) ?
        RTCFG_ADDRSIZE_IP : 0);

    rtskb = alloc_rtskb(rtskb_size, &rtcfg_pool);
    if (rtskb == NULL) {
        rtdev_dereference(rtdev);
        return -ENOBUFS;
    }

    rtskb_reserve(rtskb, rtdev->hard_header_len);

    dead_station_frm = (struct rtcfg_frm_dead_station *)
        rtskb_put(rtskb, sizeof(struct rtcfg_frm_dead_station));

    dead_station_frm->head.id      = RTCFG_ID_DEAD_STATION;
    dead_station_frm->head.version = 0;
    dead_station_frm->addr_type    = conn->addr_type & RTCFG_ADDR_MASK;

    if (dead_station_frm->addr_type == RTCFG_ADDR_IP) {
        rtskb_put(rtskb, RTCFG_ADDRSIZE_IP);

        *(u32*)dead_station_frm->logical_addr = conn->addr.ip_addr;

        dead_station_frm = (struct rtcfg_frm_dead_station *)
            (((u8 *)dead_station_frm) + RTCFG_ADDRSIZE_IP);
    }

    /* Ethernet-specific! */
    memcpy(dead_station_frm->physical_addr, conn->mac_addr, ETH_ALEN);
    memset(&dead_station_frm->physical_addr[ETH_ALEN], 0,
        sizeof(dead_station_frm->physical_addr) - ETH_ALEN);

    return rtcfg_send_frame(rtskb, rtdev, rtdev->broadcast);
}



static struct rtpacket_type rtcfg_packet_type = {
    name:       "RTcfg",
    type:       __constant_htons(ETH_RTCFG),
    handler:    rtcfg_rx_handler
};



int __init rtcfg_init_frames(void)
{
    int ret;


    if (rtskb_pool_init(&rtcfg_pool, num_rtskbs) < num_rtskbs)
        return -ENOMEM;

    rtskb_queue_init(&rx_queue);
    rtos_event_init(&rx_event);

    ret = rtos_task_init(&rx_task, rtcfg_rx_task, 0,
                         RTOS_LOWEST_RT_PRIORITY);
    if (ret < 0) {
        rtos_event_delete(&rx_event);
        goto error1;
    }

    ret = rtdev_add_pack(&rtcfg_packet_type);
    if (ret < 0)
        goto error2;

    return 0;

  error2:
    rtos_event_delete(&rx_event);
    rtos_task_delete(&rx_task);

  error1:
    rtskb_pool_release(&rtcfg_pool);

    return ret;
}



void rtcfg_cleanup_frames(void)
{
    struct rtskb *rtskb;


    while (rtdev_remove_pack(&rtcfg_packet_type) == -EAGAIN) {
        RTCFG_DEBUG(3, "RTcfg: waiting for protocol unregistration\n");
        set_current_state(TASK_UNINTERRUPTIBLE);
        schedule_timeout(1*HZ); /* wait a second */
    }

    rtos_event_delete(&rx_event);
    rtos_task_delete(&rx_task);

    while ((rtskb = rtskb_dequeue(&rx_queue)) != NULL) {
        rtdev_dereference(rtskb->rtdev);
        kfree_rtskb(rtskb);
    }

    rtskb_pool_release(&rtcfg_pool);
}
