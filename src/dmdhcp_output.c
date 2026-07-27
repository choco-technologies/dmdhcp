/**
 * @file dmdhcp_output.c
 * @brief Message senders (DISCOVER/REQUEST variants/DECLINE/RELEASE), and
 *        the retransmit_timer: its creation, its callback (backoff resend
 *        OR the deferred RFC 2131 §4.4.1 ARP conflict probe, branching on
 *        lease->state)
 *
 * Broadcast sends (DISCOVER; REQUEST during SELECTING/INIT-REBOOT/
 * REBINDING; DECLINE) always go through dmudp_send_on_iface() - never the
 * routed dmudp_send(). This isn't only about the pre-lease bootstrap case
 * (no route yet): even once a lease exists and a default route is
 * installed, dmnetbridge's resolve_egress() substitutes that route's
 * gateway as the ARP next-hop for ANY destination the route matches -
 * including a broadcast one - so a routed send would resolve the
 * gateway's MAC instead of transmitting an on-link broadcast frame. Only
 * RENEWING's unicast REQUEST and DHCPRELEASE go through the routed
 * dmudp_send(), since those are addressed directly to a known server.
 */
#include "dmod.h"
#include "dmdhcp_internal.h"
#include "dmudp.h"
#include "dmarp.h"
#include <string.h>
#include <errno.h>

static dmroute_addr_t limited_broadcast(void)
{
    dmroute_addr_t addr = { 0 };
    addr.family = dmroute_family_v4;
    addr.addr.v4[0] = 255;
    addr.addr.v4[1] = 255;
    addr.addr.v4[2] = 255;
    addr.addr.v4[3] = 255;
    return addr;
}

/**
 * @brief Build one DHCP message (fixed header + magic cookie + options)
 *        and send it
 *
 * @param ciaddr           NULL to leave dmroute_family_none (client
 *                          doesn't have the address yet), otherwise the
 *                          address to put in the fixed header's ciaddr
 * @param requested_ip     NULL to omit option 50, otherwise its value
 * @param server_id_option NULL to omit option 54, otherwise its value
 * @param dst              Destination IP to hand to dmudp
 * @param via_iface        true -> dmudp_send_on_iface() (broadcast sends,
 *                          see this file's top comment), false ->
 *                          dmudp_send() (unicast to a known server)
 */
static int send_message(struct dmdhcp_lease* lease, dmdhcp_msg_type_t msg_type, const dmroute_addr_t* ciaddr,
                         const dmroute_addr_t* requested_ip, const dmroute_addr_t* server_id_option,
                         const dmroute_addr_t* dst, bool via_iface)
{
    dmnetif_mac_addr_t mac = { 0 };
    dmnetif_get_mac_address(lease->iface, &mac);

    dmdhcp_message_t header = { 0 };
    header.op = dmdhcp_op_bootrequest;
    header.htype = 1; /* ARP hardware type 1 = Ethernet, RFC 1700 */
    header.hlen = DMNETIF_MAC_ADDR_LEN;
    header.xid = lease->xid;
    header.flags = DMDHCP_FLAG_BROADCAST; /* dmdhcp can't receive a unicast reply before it has an address */
    header.ciaddr = (ciaddr != NULL) ? *ciaddr : (dmroute_addr_t){ .family = dmroute_family_none };
    memcpy(header.chaddr, mac.addr, DMNETIF_MAC_ADDR_LEN);

    uint8_t buffer[DMDHCP_MAX_MESSAGE_LEN];
    int result = dmdhcp_build_message(buffer, sizeof(buffer), &header);
    if (result != 0)
        return result;

    uint8_t msg_type_byte = (uint8_t)msg_type;
    dmdhcp_option_t options[4];
    size_t option_count = 0;
    options[option_count++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_MSG_TYPE, .length = 1, .data = &msg_type_byte };
    if (requested_ip != NULL)
    {
        options[option_count++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_REQUESTED_IP, .length = (uint8_t)DMROUTE_IPV4_ADDR_LEN, .data = requested_ip->addr.v4 };
    }
    if (server_id_option != NULL)
    {
        options[option_count++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_SERVER_ID, .length = (uint8_t)DMROUTE_IPV4_ADDR_LEN, .data = server_id_option->addr.v4 };
    }
    if (lease->hostname != NULL)
    {
        size_t len = strlen(lease->hostname);
        uint8_t hostname_len = (uint8_t)((len > 255u) ? 255u : len);
        options[option_count++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_HOSTNAME, .length = hostname_len, .data = (const uint8_t*)lease->hostname };
    }

    size_t options_offset = DMDHCP_FIXED_HEADER_LEN + 4u;
    size_t options_len = 0;
    result = dmdhcp_options_write(&buffer[options_offset], sizeof(buffer) - options_offset, options, option_count, &options_len);
    if (result != 0)
        return result;

    size_t total_len = options_offset + options_len;
    return via_iface
        ? dmudp_send_on_iface(lease->iface, dst, DMDHCP_CLIENT_PORT, DMDHCP_SERVER_PORT, buffer, total_len, DMDHCP_DEFAULT_ARP_TIMEOUT_MS)
        : dmudp_send(dst, DMDHCP_CLIENT_PORT, DMDHCP_SERVER_PORT, buffer, total_len, DMDHCP_DEFAULT_ARP_TIMEOUT_MS);
}

static int send_discover(struct dmdhcp_lease* lease)
{
    dmroute_addr_t broadcast = limited_broadcast();
    return send_message(lease, dmdhcp_msg_discover, NULL, NULL, NULL, &broadcast, true);
}

static int send_request_selecting(struct dmdhcp_lease* lease)
{
    dmroute_addr_t broadcast = limited_broadcast();
    return send_message(lease, dmdhcp_msg_request, NULL, &lease->offered_ip, &lease->server_id, &broadcast, true);
}

static int send_request_reboot(struct dmdhcp_lease* lease)
{
    dmroute_addr_t broadcast = limited_broadcast();
    return send_message(lease, dmdhcp_msg_request, NULL, &lease->offered_ip, NULL, &broadcast, true);
}

static int send_request_renew(struct dmdhcp_lease* lease)
{
    return send_message(lease, dmdhcp_msg_request, &lease->offered_ip, NULL, NULL, &lease->server_id, false);
}

static int send_request_rebind(struct dmdhcp_lease* lease)
{
    dmroute_addr_t broadcast = limited_broadcast();
    return send_message(lease, dmdhcp_msg_request, &lease->offered_ip, NULL, NULL, &broadcast, true);
}

static int send_decline(struct dmdhcp_lease* lease)
{
    dmroute_addr_t broadcast = limited_broadcast();
    return send_message(lease, dmdhcp_msg_decline, NULL, &lease->offered_ip, &lease->server_id, &broadcast, true);
}

int dmdhcp_output_send_release(struct dmdhcp_lease* lease)
{
    return send_message(lease, dmdhcp_msg_release, &lease->offered_ip, NULL, &lease->server_id, &lease->server_id, false);
}

static uint32_t next_backoff(uint32_t current_ms)
{
    uint32_t next = current_ms * 2u;
    return (next > DMDHCP_RETRANSMIT_MAX_MS) ? DMDHCP_RETRANSMIT_MAX_MS : next;
}

static void rearm_backoff(struct dmdhcp_lease* lease)
{
    lease->retransmit_interval_ms = next_backoff(lease->retransmit_interval_ms);
    dmosi_timer_set_period(lease->retransmit_timer, lease->retransmit_interval_ms);
    dmosi_timer_start(lease->retransmit_timer);
}

void dmdhcp_output_start_discovery(struct dmdhcp_lease* lease)
{
    lease->xid = dmosi_rand32();
    lease->offered_ip = (dmroute_addr_t){ .family = dmroute_family_none };
    lease->server_id  = (dmroute_addr_t){ .family = dmroute_family_none };
    lease->gateway    = (dmroute_addr_t){ .family = dmroute_family_none };
    lease->netmask    = (dmroute_addr_t){ .family = dmroute_family_none };
    dmdhcp_lease_table_clear_dns_servers(lease);
    lease->retransmit_count = 0;
    lease->retransmit_interval_ms = DMDHCP_RETRANSMIT_INITIAL_MS;

    send_discover(lease);

    lease->state = dmdhcp_state_selecting;
    dmosi_timer_set_period(lease->retransmit_timer, lease->retransmit_interval_ms);
    dmosi_timer_start(lease->retransmit_timer);
}

void dmdhcp_output_start_reboot(struct dmdhcp_lease* lease)
{
    /* lease->offered_ip is already set - dmdhcp_start() pre-seeds it from
     * dmdhcp_options_t.requested_ip before calling this. */
    lease->xid = dmosi_rand32();
    lease->retransmit_count = 0;
    lease->retransmit_interval_ms = DMDHCP_RETRANSMIT_INITIAL_MS;

    send_request_reboot(lease);

    lease->state = dmdhcp_state_rebooting;
    dmosi_timer_set_period(lease->retransmit_timer, lease->retransmit_interval_ms);
    dmosi_timer_start(lease->retransmit_timer);
}

void dmdhcp_output_start_renew(struct dmdhcp_lease* lease)
{
    lease->xid = dmosi_rand32();
    lease->retransmit_count = 0;
    lease->retransmit_interval_ms = DMDHCP_RETRANSMIT_INITIAL_MS;

    send_request_renew(lease);

    lease->state = dmdhcp_state_renewing;
    dmosi_timer_set_period(lease->retransmit_timer, lease->retransmit_interval_ms);
    dmosi_timer_start(lease->retransmit_timer);
}

void dmdhcp_output_start_rebind(struct dmdhcp_lease* lease)
{
    lease->xid = dmosi_rand32();
    lease->retransmit_count = 0;
    lease->retransmit_interval_ms = DMDHCP_RETRANSMIT_INITIAL_MS;

    send_request_rebind(lease);

    lease->state = dmdhcp_state_rebinding;
    dmosi_timer_set_period(lease->retransmit_timer, lease->retransmit_interval_ms);
    dmosi_timer_start(lease->retransmit_timer);
}

/**
 * @brief A confirmed address conflict: send DHCPDECLINE and either pause
 *        before restarting, or give up entirely
 *
 * Assumes lease->lock is held; releases it around the on_error callback
 * (if the retry limit is exceeded) and re-acquires it before returning -
 * see this file's top comment / dmdhcp_internal.h for why a callback is
 * never invoked while the lock is held.
 */
static void handle_conflict(struct dmdhcp_lease* lease)
{
    send_decline(lease);
    lease->decline_retry_count++;

    if (lease->decline_retry_count > DMDHCP_MAX_DECLINE_RETRIES)
    {
        lease->state = dmdhcp_state_failed;
        dmdhcp_error_handler_t on_error = lease->callbacks.on_error;
        void* user_data = lease->user_data;

        dmosi_mutex_unlock(lease->lock);
        if (on_error != NULL)
        {
            on_error((dmdhcp_lease_t)lease, -EADDRINUSE, user_data);
        }
        dmosi_mutex_lock(lease->lock);
        return;
    }

    lease->state = dmdhcp_state_declining;
    dmosi_timer_set_period(lease->retransmit_timer, DMDHCP_DECLINE_PAUSE_MS);
    dmosi_timer_start(lease->retransmit_timer);
}

/**
 * @brief Run the RFC 2131 §4.4.1 ARP conflict probe for lease->offered_ip
 *        and act on the result
 *
 * Runs on the timer thread (see dmdhcp_internal.h's top comment for why
 * this can't happen inline on the rx thread that received the OFFER/ACK).
 * Assumes lease->lock is held; may release/re-acquire it around a
 * callback (dmdhcp_lifecycle_apply()'s on_bound, or handle_conflict()'s
 * on_error).
 */
static void run_conflict_probe(struct dmdhcp_lease* lease)
{
    dmnetif_mac_addr_t mac = { 0 };
    bool conflict = dmarp_resolve(lease->iface, &lease->offered_ip, &mac, DMDHCP_ARP_PROBE_TIMEOUT_MS) == 0;

    if (conflict)
    {
        handle_conflict(lease);
        return;
    }

    if (lease->state == dmdhcp_state_checking_offer)
    {
        send_request_selecting(lease);
        lease->state = dmdhcp_state_requesting;
        lease->retransmit_count = 0;
        lease->retransmit_interval_ms = DMDHCP_RETRANSMIT_INITIAL_MS;
        dmosi_timer_set_period(lease->retransmit_timer, lease->retransmit_interval_ms);
        dmosi_timer_start(lease->retransmit_timer);
        return;
    }

    /* dmdhcp_state_checking_ack: probe came back clean - commit the lease */
    dmdhcp_lifecycle_apply(lease);
    lease->state = dmdhcp_state_bound;
    lease->decline_retry_count = 0;
    dmdhcp_lifecycle_arm_t1(lease);

    dmdhcp_bound_handler_t on_bound = lease->callbacks.on_bound;
    void* user_data = lease->user_data;

    dmosi_mutex_unlock(lease->lock);
    if (on_bound != NULL)
    {
        on_bound((dmdhcp_lease_t)lease, false, user_data);
    }
    dmosi_mutex_lock(lease->lock);
}

/**
 * @brief dmosi_timer_callback_t for lease->retransmit_timer
 *
 * Runs in timer/interrupt context (per dmosi_timer_callback_t's own doc
 * comment) - a different context than the rx-thread callback that drives
 * everything else in this module. See dmdhcp_internal.h's top comment.
 */
static void retransmit_timer_callback(void* arg)
{
    struct dmdhcp_lease* lease = (struct dmdhcp_lease*)arg;
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return;

    dmosi_mutex_lock(lease->lock);

    switch (lease->state)
    {
        case dmdhcp_state_selecting:
            send_discover(lease);
            rearm_backoff(lease);
            break;

        case dmdhcp_state_requesting:
        case dmdhcp_state_rebooting:
            lease->retransmit_count++;
            if (lease->retransmit_count > DMDHCP_MAX_REQUEST_RETRANSMITS)
            {
                /* No ACK/NAK after several tries - the offer/remembered
                 * lease is presumably stale. Fall back to a fresh DISCOVER
                 * cycle rather than retrying forever (unlike SELECTING,
                 * which has no such fallback to fall back to). */
                dmdhcp_output_start_discovery(lease);
            }
            else
            {
                if (lease->state == dmdhcp_state_requesting)
                {
                    send_request_selecting(lease);
                }
                else
                {
                    send_request_reboot(lease);
                }
                rearm_backoff(lease);
            }
            break;

        case dmdhcp_state_renewing:
            send_request_renew(lease);
            rearm_backoff(lease);
            break;

        case dmdhcp_state_rebinding:
            send_request_rebind(lease);
            rearm_backoff(lease);
            break;

        case dmdhcp_state_checking_offer:
        case dmdhcp_state_checking_ack:
            run_conflict_probe(lease);
            break;

        case dmdhcp_state_declining:
            dmdhcp_output_start_discovery(lease);
            break;

        default:
            break; /* stale fire (init/init_reboot/bound/failed) - nothing to do */
    }

    dmosi_mutex_unlock(lease->lock);
}

dmosi_timer_t dmdhcp_output_create_retransmit_timer(struct dmdhcp_lease* lease)
{
    return dmosi_timer_create(retransmit_timer_callback, lease, DMDHCP_RETRANSMIT_INITIAL_MS, false);
}
