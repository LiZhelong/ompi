/*
 * Copyright (c) 2013 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "opal/util/output.h"
#include "opal/class/opal_hotel.h"

#include "btl_usnic.h"
#include "btl_usnic_frag.h"
#include "btl_usnic_endpoint.h"
#include "btl_usnic_module.h"
#include "btl_usnic_ack.h"
#include "btl_usnic_util.h"
#include "btl_usnic_send.h"

/*
 * Force a retrans of a segment
 */
static void
ompi_btl_usnic_force_retrans(
    ompi_btl_usnic_endpoint_t *endpoint,
    ompi_btl_usnic_seq_t ack_seq)
{
    ompi_btl_usnic_send_segment_t *sseg;
    int is;

    is = WINDOW_SIZE_MOD(ack_seq+1);
    sseg = endpoint->endpoint_sent_segs[is];
    if (sseg == NULL || sseg->ss_hotel_room == -1) {
        return;
    }

    /* cancel retrans timer */
    opal_hotel_checkout(&endpoint->endpoint_hotel, sseg->ss_hotel_room);
    sseg->ss_hotel_room = -1;

    /* Queue up this segment to be resent */
    opal_list_append(&(endpoint->endpoint_module->pending_resend_segs),
                     &(sseg->ss_base.us_list.super));

    ++endpoint->endpoint_module->num_fast_retrans;
}


/*
 * We have received an ACK for a given sequence number (either standalone
 * or via piggy-back on a regular send)
 */
void
ompi_btl_usnic_handle_ack(
    ompi_btl_usnic_endpoint_t *endpoint,
    ompi_btl_usnic_seq_t ack_seq)
{
    ompi_btl_usnic_seq_t is;
    ompi_btl_usnic_send_segment_t *sseg;
    ompi_btl_usnic_send_frag_t *frag;
    ompi_btl_usnic_module_t *module;
    uint32_t bytes_acked;

    module = endpoint->endpoint_module;

    /* ignore if this is an old ACK */
    if (ack_seq < endpoint->endpoint_ack_seq_rcvd) {
#if MSGDEBUG
        opal_output(0, "Got OLD DUP ACK seq %d < %d\n",
                ack_seq, endpoint->endpoint_ack_seq_rcvd);
#endif
        ++module->num_old_dup_acks;
        return;

    /* A duplicate ACK means next seg was lost */
    } else if (ack_seq == endpoint->endpoint_ack_seq_rcvd) {
        ++module->num_dup_acks;

        ompi_btl_usnic_force_retrans(endpoint, ack_seq);
        return;
    }

    /* Does this ACK have a new sequence number that we haven't
       seen before? */
    for (is = endpoint->endpoint_ack_seq_rcvd + 1; is <= ack_seq; ++is) {
        sseg = endpoint->endpoint_sent_segs[WINDOW_SIZE_MOD(is)];

#if MSGDEBUG1
        opal_output(0, "  Checking ACK/sent_segs window %p, index %lu, seq %lu, occupied=%p, seg_room=%d",
            (void*) endpoint->endpoint_sent_segs,
            WINDOW_SIZE_MOD(is), is, (void*)sseg, (sseg?sseg->ss_hotel_room:-2));
#endif

        assert(sseg != NULL);
        assert(sseg->ss_base.us_btl_header->seq == is);
#if MSGDEBUG
        if (sseg->ss_hotel_room == -1) {
            opal_output(0, "=== ACKed frag in sent_frags array is not in hotel/enqueued, module %p, endpoint %p, seg %p, seq %" UDSEQ ", slot %lu",
                        (void*) module, (void*) endpoint,
                        (void*) sseg, is, WINDOW_SIZE_MOD(is));
        }
#endif

        /* Check the sending segment out from the hotel.  NOTE: The
           segment might not actually be in a hotel room if it has
           already been evicted and queued for resend.
           If it's not in the hotel, don't check it out! */
        if (OPAL_LIKELY(sseg->ss_hotel_room != -1)) {

            opal_hotel_checkout(&endpoint->endpoint_hotel, sseg->ss_hotel_room);
            sseg->ss_hotel_room = -1;

        /* hotel_room == -1 means queued for resend, remove it */
        } else {
            opal_list_remove_item((&module->pending_resend_segs),
                    &sseg->ss_base.us_list.super);
        }

        /* update the owning fragment */
        bytes_acked = sseg->ss_base.us_btl_header->payload_len;
        frag = sseg->ss_parent_frag;

        /* when no bytes left to ACK, fragment send is truly done */
        frag->sf_ack_bytes_left -= bytes_acked;
#if MSGDEBUG1
        opal_output(0, "   ACKED seg %p, frag %p, ack_bytes=%"PRIu32", left=%zd\n",
                (void*)sseg, (void*)frag, bytes_acked, frag->sf_ack_bytes_left);
#endif

        /* If all ACKs received, and this is a put or a regular send
         * that needs a callback, perform the callback now
         */
        if (frag->sf_ack_bytes_left == 0 &&
            ((frag->sf_base.uf_dst_seg[0].seg_addr.pval != NULL) ||
             (frag->sf_base.uf_base.des_flags &
              MCA_BTL_DES_SEND_ALWAYS_CALLBACK))) {
#if MSGDEBUG2
            opal_output(0, "completion callback for frag=%p, dest=%p\n",
                    (void*)frag, frag->sf_base.uf_dst_seg[0].seg_addr.pval);
#endif
            frag->sf_base.uf_base.des_cbfunc(&module->super,
                    frag->sf_endpoint, &frag->sf_base.uf_base,
                    OMPI_SUCCESS);
            frag->sf_base.uf_base.des_flags &=
                ~MCA_BTL_DES_SEND_ALWAYS_CALLBACK;
        }

        /* free this segment */
        sseg->ss_ack_pending = false;
        if (sseg->ss_send_posted == 0) {
            ompi_btl_usnic_release_send_segment(module, frag, sseg);
        }

        /* OK to return this fragment? */
        ompi_btl_usnic_send_frag_return_cond(module, frag);

        /* indicate this segment has been ACKed */
        endpoint->endpoint_sent_segs[WINDOW_SIZE_MOD(is)] = NULL;
    }

    /* update ACK received */
    endpoint->endpoint_ack_seq_rcvd = ack_seq;

    /* send window may have opened, possibly make endpoint ready-to-send */
    ompi_btl_usnic_check_rts(endpoint);
}

/*
 * Send an ACK
 */
void
ompi_btl_usnic_ack_send(
    ompi_btl_usnic_module_t *module,
    ompi_btl_usnic_endpoint_t *endpoint)
{
    ompi_btl_usnic_ack_segment_t *ack;
#if MSGDEBUG1
    uint8_t mac[6];
    char src_mac[32];
    char dest_mac[32];
#endif

    /* Get an ACK frag.  If we don't get one, just discard this ACK. */
    ack = ompi_btl_usnic_ack_segment_alloc(module);
    if (OPAL_UNLIKELY(NULL == ack)) {
        opal_output(0, "====================== No frag for sending the ACK -- skipped");
        abort();
    }

    /* send the seq of the lowest item in the window that
       we've received */
    ack->ss_base.us_btl_header->ack_seq =
        endpoint->endpoint_next_contig_seq_to_recv - 1;

    ack->ss_base.us_sg_entry[0].length = 
        sizeof(ompi_btl_usnic_btl_header_t);

#if MSGDEBUG1
    memset(src_mac, 0, sizeof(src_mac));
    memset(dest_mac, 0, sizeof(dest_mac));
    ompi_btl_usnic_sprintf_mac(src_mac, module->if_mac);
    ompi_btl_usnic_gid_to_mac(&endpoint->endpoint_remote_addr.gid, mac);
    ompi_btl_usnic_sprintf_mac(dest_mac, mac);

    opal_output(0, "--> Sending ACK, sg_entry length %d, seq %" UDSEQ " to %s, qp %u", 
                ack->ss_base.us_sg_entry[0].length,
                ack->ss_base.us_btl_header->ack_seq, dest_mac,
                endpoint->endpoint_remote_addr.qp_num[ack->ss_channel]);
#endif

    /* send the ACK */
    ompi_btl_usnic_post_segment(module, endpoint, ack);

    /* Stats */
    ++module->num_ack_sends;

    return;
}

/*
 * Sending an ACK has completed, return the segment to the free list
 */
void
ompi_btl_usnic_ack_complete(ompi_btl_usnic_module_t *module,
                                   ompi_btl_usnic_ack_segment_t *ack)
{
    ompi_btl_usnic_ack_segment_return(module, ack);
    ++module->mod_channels[ack->ss_channel].sd_wqe;
}

/*****************************************************************************/

/*
 * Callback for when a send times out without receiving a
 * corresponding ACK.
 */
void
ompi_btl_usnic_ack_timeout(
    opal_hotel_t *hotel,
    int room_num, 
    void *occupant)
{
    ompi_btl_usnic_send_segment_t *seg;
    ompi_btl_usnic_endpoint_t *endpoint;
    ompi_btl_usnic_module_t *module;

    seg = (ompi_btl_usnic_send_segment_t*) occupant;
    endpoint = seg->ss_parent_frag->sf_endpoint;
    module = endpoint->endpoint_module;

#if MSGDEBUG2
    {
        opal_output(0, "Send timeout!  seg %p, room %d, seq %" UDSEQ "\n",
                    (void*)seg, seg->ss_hotel_room,
                    seg->ss_base.us_btl_header->seq);
    }
#endif

    /* timeout checks us out, note this */
    seg->ss_hotel_room = -1;

    /* Queue up this frag to be resent */
    opal_list_append(&(module->pending_resend_segs), 
                     &(seg->ss_base.us_list.super));

    /* Stats */
    ++module->num_timeout_retrans;
}

