/***
 *
 *  rtmac/rtmac_proto.c
 *
 *  rtmac - real-time networking media access control subsystem
 *  Copyright (C) 2002       Marc Kleine-Budde <kleine-budde@gmx.de>,
 *                2003, 2004 Jan Kiszka <Jan.Kiszka@web.de>
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


#include <rtnet_sys.h>
#include <stack_mgr.h>
#include <rtmac/rtmac_disc.h>
#include <rtmac/rtmac_proto.h>
#include <rtmac/rtmac_vnic.h>



int rtmac_proto_rx(struct rtskb *skb, struct rtpacket_type *pt)
{
    struct rtmac_disc *disc = skb->rtdev->mac_disc;
    struct rtmac_hdr  *hdr;


    if (disc == NULL) {
#if 0 /* switch this warning off until we control error message output */
        /*ERROR*/rtos_print("RTmac: received RTmac packet on unattached "
                            "device %s\n", skb->rtdev->name);
#endif
        goto error;
    }

    hdr = (struct rtmac_hdr *)skb->data;
    rtskb_pull(skb, sizeof(struct rtmac_hdr));

    if (hdr->ver != RTMAC_VERSION) {
        rtos_print("RTmac: received unsupported RTmac protocol version on "
                   "device %s\n", skb->rtdev->name);
        goto error;
    }

    if (hdr->flags & RTMAC_FLAG_TUNNEL)
        return rtmac_vnic_rx(skb, hdr->type);
    else if (disc->disc_type == hdr->type)
        return disc->packet_rx(skb);

  error:
    kfree_rtskb(skb);
    return -1;
}



struct rtpacket_type rtmac_packet_type = {
    name:       "RTmac",
    type:       __constant_htons(ETH_RTMAC),
    handler:    rtmac_proto_rx
};



void rtmac_proto_release(void)
{
    while (rtdev_remove_pack(&rtmac_packet_type) == -EAGAIN) {
        rtos_print("RTmac: waiting for protocol unregistration\n");
        set_current_state(TASK_UNINTERRUPTIBLE);
        schedule_timeout(1*HZ); /* wait a second */
    }
}
