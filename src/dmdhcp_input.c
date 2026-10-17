/**
 * @file dmdhcp_input.c
 * @brief dmdhcp_handle_datagram(): xid/iface match, option parsing, and
 *        the per-(state, message type) dispatch
 *
 * Registered with dmudp_bind(DMDHCP_CLIENT_PORT, ...) in dmod_init() (see
 * dmdhcp.c). Runs inline, synchronously, on whatever thread is pumping the
 * interface a datagram arrived on - see dmudp_datagram_handler_t in
 * dmudp.h - so everything here happens in that same call, with no queue or
 * worker thread of its own. `payload` is only valid for the duration of
 * this call (dmudp_datagram_handler_t's own contract) - nothing here
 * stores a pointer into it past return.
 */
#include "dmod.h"
#include "dmdhcp_internal.h"
#include "dmudp.h"

/**
 * @brief Populate lease->offered_ip/netmask/gateway/server_id/
 *        lease_time_sec/t1_sec/t2_sec/dns_servers from an OFFER or ACK's
 *        fixed header (yiaddr) and options
 *
 * Assumes lease->lock is held. T1/T2 fall back to RFC 2131 §4.4.1's
 * suggested defaults (0.5 / 0.875 of the lease time) if the server didn't
 * send options 58/59 explicitly.
 */
static void populate_lease_from_options(struct dmdhcp_lease* lease, const dmdhcp_message_t* msg, const uint8_t* options, size_t options_len)
{
    lease->offered_ip = msg->yiaddr;

    dmdhcp_option_t opt;
    if (dmdhcp_options_find(options, options_len, DMDHCP_OPT_SUBNET_MASK, &opt) == 0)
    {
        dmdhcp_option_get_addr(&opt, &lease->netmask);
    }

    if (dmdhcp_options_find(options, options_len, DMDHCP_OPT_ROUTER, &opt) == 0 && dmdhcp_option_addr_count(&opt) > 0)
    {
        dmdhcp_option_get_addr_at(&opt, 0, &lease->gateway);
    }
    else
    {
        lease->gateway = (dmroute_addr_t){ .family = dmroute_family_none };
    }

    if (dmdhcp_options_find(options, options_len, DMDHCP_OPT_SERVER_ID, &opt) == 0)
    {
        dmdhcp_option_get_addr(&opt, &lease->server_id);
    }

    uint32_t lease_time = 0;
    if (dmdhcp_options_find(options, options_len, DMDHCP_OPT_LEASE_TIME, &opt) == 0)
    {
        dmdhcp_option_get_u32(&opt, &lease_time);
    }
    lease->lease_time_sec = lease_time;

    uint32_t t1 = 0;
    if (dmdhcp_options_find(options, options_len, DMDHCP_OPT_RENEWAL_T1, &opt) == 0)
    {
        dmdhcp_option_get_u32(&opt, &t1);
    }
    else if (lease_time > 0)
    {
        t1 = lease_time / 2u;
    }
    lease->t1_sec = t1;

    uint32_t t2 = 0;
    if (dmdhcp_options_find(options, options_len, DMDHCP_OPT_REBINDING_T2, &opt) == 0)
    {
        dmdhcp_option_get_u32(&opt, &t2);
    }
    else if (lease_time > 0)
    {
        t2 = (lease_time * 7u) / 8u;
    }
    lease->t2_sec = t2;

    dmdhcp_lease_table_clear_dns_servers(lease);
    if (dmdhcp_options_find(options, options_len, DMDHCP_OPT_DNS_SERVER, &opt) == 0)
    {
        size_t count = dmdhcp_option_addr_count(&opt);
        for (size_t i = 0; i < count; i++)
        {
            dmroute_addr_t* entry = Dmod_Malloc(sizeof(*entry));
            if (entry == NULL)
                break;
            dmdhcp_option_get_addr_at(&opt, i, entry);
            if (!dmlist_push_back(lease->dns_servers, entry))
            {
                Dmod_Free(entry);
                break;
            }
        }
    }
}

static void dmdhcp_handle_datagram(const dmip_addr_t* src, uint16_t src_port, uint16_t dst_port, dmnetif_iface_t iface, const uint8_t* payload, size_t payload_len)
{
    (void)src;
    (void)src_port;
    (void)dst_port;

    dmdhcp_message_t msg;
    size_t options_offset = 0;
    if (dmdhcp_parse_message(payload, payload_len, &msg, &options_offset) != 0)
        return;
    if (msg.op != dmdhcp_op_bootreply)
        return; /* only ever expect server -> client messages here */

    struct dmdhcp_lease* lease = dmdhcp_lease_table_find_by_xid(iface, msg.xid);
    if (lease == NULL)
        return; /* no lease on this interface is waiting on this transaction */

    const uint8_t* options = payload + options_offset;
    size_t options_len = payload_len - options_offset;

    dmdhcp_option_t type_opt;
    if (dmdhcp_options_find(options, options_len, DMDHCP_OPT_MSG_TYPE, &type_opt) != 0 || type_opt.length != 1)
        return;
    uint8_t msg_type = type_opt.data[0];

    dmosi_mutex_lock(lease->lock);
    dmdhcp_state_t state = lease->state;

    if (msg_type == (uint8_t)dmdhcp_msg_offer && state == dmdhcp_state_selecting)
    {
        populate_lease_from_options(lease, &msg, options, options_len);
        lease->state = dmdhcp_state_checking_offer;
        /* Hand off to the timer thread for the RFC 2131 §4.4.1 conflict
         * probe - dmarp_resolve() must not be called inline here, see
         * dmdhcp_internal.h's top comment. */
        dmosi_timer_set_period(lease->retransmit_timer, DMDHCP_PROBE_HANDOFF_MS);
        dmosi_timer_start(lease->retransmit_timer);
        dmosi_mutex_unlock(lease->lock);
        return;
    }

    if (msg_type == (uint8_t)dmdhcp_msg_ack && (state == dmdhcp_state_requesting || state == dmdhcp_state_rebooting))
    {
        populate_lease_from_options(lease, &msg, options, options_len);
        lease->state = dmdhcp_state_checking_ack;
        dmosi_timer_set_period(lease->retransmit_timer, DMDHCP_PROBE_HANDOFF_MS);
        dmosi_timer_start(lease->retransmit_timer);
        dmosi_mutex_unlock(lease->lock);
        return;
    }

    if (msg_type == (uint8_t)dmdhcp_msg_nak && (state == dmdhcp_state_requesting || state == dmdhcp_state_rebooting))
    {
        /* Never had a confirmed lease this session (the offer/remembered
         * address was rejected) - restart silently, no on_error. This is
         * ordinary DHCP behavior (e.g. another client got the address
         * first), not a failure worth surfacing to the application. */
        dmdhcp_output_start_discovery(lease);
        dmosi_mutex_unlock(lease->lock);
        return;
    }

    if (msg_type == (uint8_t)dmdhcp_msg_ack && (state == dmdhcp_state_renewing || state == dmdhcp_state_rebinding))
    {
        /* dmdhcp already owns this address uninterrupted, so no conflict
         * probe is needed here (RFC 2131 only calls for probing before
         * *first* using an address) - apply directly, inline. */
        populate_lease_from_options(lease, &msg, options, options_len);
        dmdhcp_lifecycle_apply(lease);
        lease->state = dmdhcp_state_bound;
        lease->decline_retry_count = 0;
        dmdhcp_lifecycle_arm_t1(lease);

        dmdhcp_bound_handler_t on_bound = lease->callbacks.on_bound;
        void* user_data = lease->user_data;
        dmosi_mutex_unlock(lease->lock);
        if (on_bound != NULL)
        {
            on_bound((dmdhcp_lease_t)lease, true, user_data);
        }
        return;
    }

    if (msg_type == (uint8_t)dmdhcp_msg_nak && (state == dmdhcp_state_renewing || state == dmdhcp_state_rebinding))
    {
        dmosi_mutex_unlock(lease->lock);
        dmdhcp_lifecycle_handle_lost(lease); /* manages its own locking */
        return;
    }

    dmosi_mutex_unlock(lease->lock); /* unexpected message for the current state - ignore */
}

int dmdhcp_input_register(void)
{
    return dmudp_bind(DMDHCP_CLIENT_PORT, dmdhcp_handle_datagram);
}

void dmdhcp_input_unregister(void)
{
    dmudp_unbind(DMDHCP_CLIENT_PORT);
}
