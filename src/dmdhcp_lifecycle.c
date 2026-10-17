/**
 * @file dmdhcp_lifecycle.c
 * @brief Applying/unapplying a lease to the interface, and the
 *        lease_timer: its creation, its callback (T1 -> T2 -> expiry,
 *        rearmed sequentially), and the "lease lost" path shared by a NAK
 *        (dmdhcp_input.c) and a full expiry (this file's own timer callback)
 */
#include "dmod.h"
#include "dmdhcp_internal.h"

int dmdhcp_lifecycle_apply(struct dmdhcp_lease* lease)
{
    int result = dmnetif_set_netmask(lease->iface, &lease->netmask);
    if (result != 0)
        return result;

    result = dmnetif_set_ip_address(lease->iface, &lease->offered_ip);
    if (result != 0)
        return result;

    if (lease->netmask.family == dmroute_family_v4)
    {
        dmroute_addr_t broadcast = { 0 };
        broadcast.family = dmroute_family_v4;
        for (int i = 0; i < DMROUTE_IPV4_ADDR_LEN; i++)
        {
            broadcast.addr.v4[i] = (uint8_t)(lease->offered_ip.addr.v4[i] | (uint8_t)(~lease->netmask.addr.v4[i]));
        }
        dmnetif_set_broadcast(lease->iface, &broadcast);
    }

    if (lease->gateway.family == dmroute_family_v4)
    {
        dmroute_addr_t any = { 0 };
        any.family = dmroute_family_v4; /* 0.0.0.0/0 - a default route */
        lease->default_route = dmroute_add(&any, &any, &lease->gateway, dmnetif_get_name(lease->iface), DMROUTE_DEFAULT_METRIC, dmroute_origin_static);
    }
    else
    {
        lease->default_route = NULL;
    }

    lease->iface_configured = true;
    return 0;
}

void dmdhcp_lifecycle_unapply(struct dmdhcp_lease* lease)
{
    if (!lease->iface_configured)
        return;

    if (lease->default_route != NULL)
    {
        dmroute_remove(lease->default_route);
        lease->default_route = NULL;
    }

    dmroute_addr_t none = { .family = dmroute_family_none };
    dmnetif_set_ip_address(lease->iface, &none);
    dmnetif_set_netmask(lease->iface, &none);
    dmnetif_set_broadcast(lease->iface, &none);

    lease->iface_configured = false;
}

void dmdhcp_lifecycle_arm_t1(struct dmdhcp_lease* lease)
{
    uint32_t period_ms = lease->t1_sec * 1000u;
    if (period_ms == 0u)
    {
        period_ms = 1u; /* avoid an ill-defined zero-period timer if the server offered T1 = 0 */
    }
    dmosi_timer_set_period(lease->lease_timer, period_ms);
    dmosi_timer_start(lease->lease_timer);
}

void dmdhcp_lifecycle_arm_t2(struct dmdhcp_lease* lease)
{
    uint32_t remaining_sec = (lease->t2_sec > lease->t1_sec) ? (lease->t2_sec - lease->t1_sec) : 1u;
    dmosi_timer_set_period(lease->lease_timer, remaining_sec * 1000u);
    dmosi_timer_start(lease->lease_timer);
}

void dmdhcp_lifecycle_arm_expiry(struct dmdhcp_lease* lease)
{
    uint32_t remaining_sec = (lease->lease_time_sec > lease->t2_sec) ? (lease->lease_time_sec - lease->t2_sec) : 1u;
    dmosi_timer_set_period(lease->lease_timer, remaining_sec * 1000u);
    dmosi_timer_start(lease->lease_timer);
}

void dmdhcp_lifecycle_handle_lost(struct dmdhcp_lease* lease)
{
    dmosi_mutex_lock(lease->lock);
    dmdhcp_lifecycle_unapply(lease);
    dmdhcp_expired_handler_t on_expired = lease->callbacks.on_expired_or_lost;
    void* user_data = lease->user_data;
    dmosi_mutex_unlock(lease->lock);

    if (on_expired != NULL)
    {
        on_expired((dmdhcp_lease_t)lease, user_data);
    }

    dmosi_mutex_lock(lease->lock);
    dmdhcp_output_start_discovery(lease);
    dmosi_mutex_unlock(lease->lock);
}

/**
 * @brief dmosi_timer_callback_t for lease->lease_timer
 *
 * Runs in timer/interrupt context, a different thread than the rx path -
 * see dmdhcp_internal.h's top comment. Reused sequentially for three
 * different deadlines depending on lease->state: T1 (while bound), T2
 * (while renewing), and full lease expiry (while rebinding).
 */
static void lease_timer_callback(void* arg)
{
    struct dmdhcp_lease* lease = (struct dmdhcp_lease*)arg;
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return;

    dmosi_mutex_lock(lease->lock);

    if (lease->state == dmdhcp_state_bound)
    {
        dmdhcp_output_start_renew(lease);
        dmdhcp_lifecycle_arm_t2(lease);
        dmosi_mutex_unlock(lease->lock);
        return;
    }

    if (lease->state == dmdhcp_state_renewing)
    {
        dmdhcp_output_start_rebind(lease);
        dmdhcp_lifecycle_arm_expiry(lease);
        dmosi_mutex_unlock(lease->lock);
        return;
    }

    if (lease->state == dmdhcp_state_rebinding)
    {
        /* Full lease expiry reached with no ACK - the lease is gone.
         * dmdhcp_lifecycle_handle_lost() manages its own locking (it must
         * release lease->lock before calling the user's
         * on_expired_or_lost), so release it here first. */
        dmosi_mutex_unlock(lease->lock);
        dmdhcp_lifecycle_handle_lost(lease);
        return;
    }

    dmosi_mutex_unlock(lease->lock); /* stale fire in any other state */
}

dmosi_timer_t dmdhcp_lifecycle_create_lease_timer(struct dmdhcp_lease* lease)
{
    return dmosi_timer_create(lease_timer_callback, lease, DMDHCP_RETRANSMIT_INITIAL_MS, false);
}
