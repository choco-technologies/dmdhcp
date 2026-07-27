/**
 * @file dmdhcp.c
 * @brief DMOD lifecycle (dmod_init()/_deinit()) and dmdhcp_start()/_stop()/
 *        _release()/_renew()
 *
 * Everything else lives in the other dmdhcp_*.c files - see
 * dmdhcp_internal.h's top comment for the full file map.
 */
#include "dmod.h"
#include "dmdhcp_internal.h"
#include <errno.h>

dmod_dmdhcp_api_declaration(1.0, dmdhcp_lease_t, _start, ( dmnetif_iface_t iface, const dmdhcp_callbacks_t* callbacks, void* user_data, const dmdhcp_options_t* options ))
{
    if (iface == NULL)
        return NULL;

    struct dmdhcp_lease* lease = dmdhcp_lease_table_create(iface);
    if (lease == NULL)
        return NULL;

    if (callbacks != NULL)
    {
        lease->callbacks = *callbacks;
    }
    lease->user_data = user_data;
    if (options != NULL && options->hostname != NULL)
    {
        lease->hostname = Dmod_StrDup(options->hostname);
    }

    if (dmdhcp_lease_table_insert(lease) != 0)
    {
        /* `iface` already has an active lease */
        dmdhcp_lease_table_destroy(lease);
        return NULL;
    }

    dmosi_mutex_lock(lease->lock);
    if (options != NULL && options->requested_ip.family == dmroute_family_v4)
    {
        /* RFC 2131 §4.3.2 INIT-REBOOT - immediately followed by
         * dmdhcp_state_rebooting, no independently observable
         * "init_reboot" state (see dmdhcp_output_start_reboot()). */
        lease->offered_ip = options->requested_ip;
        dmdhcp_output_start_reboot(lease);
    }
    else
    {
        dmdhcp_output_start_discovery(lease);
    }
    dmosi_mutex_unlock(lease->lock);

    return (dmdhcp_lease_t)lease;
}

dmod_dmdhcp_api_declaration(1.0, int, _renew, ( dmdhcp_lease_t lease ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return -EINVAL;

    dmosi_mutex_lock(lease->lock);
    if (lease->state == dmdhcp_state_bound)
    {
        dmdhcp_output_start_renew(lease);
    }
    dmosi_mutex_unlock(lease->lock);
    return 0;
}

dmod_dmdhcp_api_declaration(1.0, int, _release, ( dmdhcp_lease_t lease ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return -EINVAL;

    /* Remove from the lease table first, so no new inbound datagram can
     * find (and start concurrently touching) this lease while it's being
     * torn down - same ordering dmtcp_close()/_abort() use. */
    dmdhcp_lease_table_remove(lease);

    dmosi_mutex_lock(lease->lock);
    if (lease->offered_ip.family == dmroute_family_v4 && lease->server_id.family == dmroute_family_v4)
    {
        dmdhcp_output_send_release(lease); /* best-effort - failure not propagated, see dmdhcp.h */
    }
    if (lease->iface_configured)
    {
        dmdhcp_lifecycle_unapply(lease);
    }
    dmosi_mutex_unlock(lease->lock);

    dmdhcp_lease_table_destroy(lease);
    return 0;
}

dmod_dmdhcp_api_declaration(1.0, void, _stop, ( dmdhcp_lease_t lease ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return;

    dmdhcp_lease_table_remove(lease);

    dmosi_mutex_lock(lease->lock);
    if (lease->iface_configured)
    {
        dmdhcp_lifecycle_unapply(lease);
    }
    dmosi_mutex_unlock(lease->lock);

    dmdhcp_lease_table_destroy(lease);
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    if (dmdhcp_lease_table_init() != 0)
    {
        DMOD_LOG_ERROR("Failed to allocate dmdhcp state\n");
        return -1;
    }

    int result = dmdhcp_input_register();
    if (result != 0)
    {
        DMOD_LOG_ERROR("dmdhcp: cannot bind UDP port %u (%d)\n", DMDHCP_CLIENT_PORT, result);
        return -1;
    }

    DMOD_LOG_INFO("DMDHCP initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    dmdhcp_input_unregister();
    dmdhcp_lease_table_deinit();

    DMOD_LOG_INFO("DMDHCP deinitialized\n");
    return 0;
}
