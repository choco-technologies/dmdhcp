/**
 * @file dmdhcp_lease_table.c
 * @brief The lease table (interface -> lease), lease lifecycle, and the
 *        simple per-lease accessors
 *
 * Two mutexes are at play in this file, deliberately different in scope:
 * g_leases_mutex guards LIST MEMBERSHIP only (which interfaces currently
 * have a tracked lease), never a lease's own fields - that's lease->lock,
 * taken separately by whichever file is actually processing that lease
 * (dmdhcp_input.c, dmdhcp_output.c, dmdhcp_lifecycle.c). See
 * dmdhcp_internal.h's struct dmdhcp_lease doc comment. Mirrors
 * dmtcp_conn_table.c's g_conn_mutex/conn->lock split exactly.
 */
#include "dmod.h"
#include "dmdhcp_internal.h"
#include <string.h>
#include <errno.h>

static dmlist_context_t* g_leases = NULL;
static dmosi_mutex_t     g_leases_mutex = NULL;

static int compare_by_iface(const void* data, const void* user_data)
{
    const struct dmdhcp_lease* entry = (const struct dmdhcp_lease*)data;
    return (entry->iface == (dmnetif_iface_t)user_data) ? 0 : -1;
}

struct dmdhcp_xid_key
{
    dmnetif_iface_t iface;
    uint32_t        xid;
};

static int compare_by_xid(const void* data, const void* user_data)
{
    const struct dmdhcp_lease* entry = (const struct dmdhcp_lease*)data;
    const struct dmdhcp_xid_key* key = (const struct dmdhcp_xid_key*)user_data;
    return (entry->iface == key->iface && entry->xid == key->xid) ? 0 : -1;
}

static int compare_pointer(const void* data, const void* user_data)
{
    return (data == user_data) ? 0 : -1;
}

int dmdhcp_lease_table_init(void)
{
    g_leases = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_leases_mutex = dmosi_mutex_create(false);
    return (g_leases != NULL && g_leases_mutex != NULL) ? 0 : -1;
}

void dmdhcp_lease_table_deinit(void)
{
    size_t count = dmlist_size(g_leases);
    for (size_t i = 0; i < count; i++)
    {
        struct dmdhcp_lease* lease = (struct dmdhcp_lease*)dmlist_pop_front(g_leases);

        dmosi_mutex_lock(lease->lock);
        if (lease->iface_configured)
        {
            dmdhcp_lifecycle_unapply(lease);
        }
        dmosi_mutex_unlock(lease->lock);

        dmdhcp_lease_table_destroy(lease);
    }
    dmlist_destroy(g_leases);
    g_leases = NULL;

    dmosi_mutex_destroy(g_leases_mutex);
    g_leases_mutex = NULL;
}

struct dmdhcp_lease* dmdhcp_lease_table_create(dmnetif_iface_t iface)
{
    struct dmdhcp_lease* lease = Dmod_Malloc(sizeof(*lease));
    if (lease == NULL)
        return NULL;

    memset(lease, 0, sizeof(*lease));
    lease->lock = dmosi_mutex_create(false);
    lease->dns_servers = dmlist_create(Dmod_GetCurrentAllocatorName());
    lease->retransmit_timer = dmdhcp_output_create_retransmit_timer(lease);
    lease->lease_timer = dmdhcp_lifecycle_create_lease_timer(lease);

    if (lease->lock == NULL || lease->dns_servers == NULL || lease->retransmit_timer == NULL || lease->lease_timer == NULL)
    {
        dmdhcp_lease_table_destroy(lease); /* magic is still 0 here - see destroy()'s own guard */
        return NULL;
    }

    lease->iface = iface;
    lease->magic = DMDHCP_LEASE_MAGIC;
    return lease;
}

void dmdhcp_lease_table_clear_dns_servers(struct dmdhcp_lease* lease)
{
    if (lease == NULL || lease->dns_servers == NULL)
        return;

    for (;;)
    {
        dmroute_addr_t* entry = (dmroute_addr_t*)dmlist_pop_front(lease->dns_servers);
        if (entry == NULL)
            break;
        Dmod_Free(entry);
    }
}

static void destroy_with_context(struct dmdhcp_lease* lease, dmdhcp_teardown_context_t context)
{
    /* dmdhcp_lease_table_create() calls this on a partially-constructed
     * lease (magic still 0) if a sub-allocation fails - every field below
     * tolerates being NULL, so no separate "was this ever set" tracking
     * is needed. */
    lease->magic = 0;

    if (lease->retransmit_timer != NULL)
    {
        dmosi_timer_stop(lease->retransmit_timer);
        if (context != dmdhcp_teardown_context_retransmit_timer)
        {
            dmosi_timer_destroy(lease->retransmit_timer);
        }
    }

    if (lease->lease_timer != NULL)
    {
        dmosi_timer_stop(lease->lease_timer);
        if (context != dmdhcp_teardown_context_lease_timer)
        {
            dmosi_timer_destroy(lease->lease_timer);
        }
    }

    /* Unapplying the interface (if still configured) is the caller's
     * responsibility, done under lease->lock before this function is ever
     * reached - see dmdhcp_stop()/_release() and
     * dmdhcp_lease_table_deinit(). destroy_with_context() itself only ever
     * frees this lease's own resources, mirroring dmtcp_conn_table.c's
     * identically-scoped destroy_with_context(). */

    dmdhcp_lease_table_clear_dns_servers(lease);
    if (lease->dns_servers != NULL)
    {
        dmlist_destroy(lease->dns_servers);
    }

    Dmod_Free(lease->hostname);

    if (lease->lock != NULL)
    {
        dmosi_mutex_destroy(lease->lock);
    }

    Dmod_Free(lease);
}

void dmdhcp_lease_table_destroy_with_context(struct dmdhcp_lease* lease, dmdhcp_teardown_context_t context)
{
    if (lease == NULL)
        return;
    destroy_with_context(lease, context);
}

void dmdhcp_lease_table_destroy(struct dmdhcp_lease* lease)
{
    dmdhcp_lease_table_destroy_with_context(lease, dmdhcp_teardown_context_normal);
}

struct dmdhcp_lease* dmdhcp_lease_table_find_by_iface(dmnetif_iface_t iface)
{
    dmosi_mutex_lock(g_leases_mutex);
    struct dmdhcp_lease* lease = (struct dmdhcp_lease*)dmlist_find(g_leases, iface, compare_by_iface);
    dmosi_mutex_unlock(g_leases_mutex);
    return lease;
}

struct dmdhcp_lease* dmdhcp_lease_table_find_by_xid(dmnetif_iface_t iface, uint32_t xid)
{
    struct dmdhcp_xid_key key = { .iface = iface, .xid = xid };

    dmosi_mutex_lock(g_leases_mutex);
    struct dmdhcp_lease* lease = (struct dmdhcp_lease*)dmlist_find(g_leases, &key, compare_by_xid);
    dmosi_mutex_unlock(g_leases_mutex);
    return lease;
}

int dmdhcp_lease_table_insert(struct dmdhcp_lease* lease)
{
    dmosi_mutex_lock(g_leases_mutex);
    int result;
    if (dmlist_find(g_leases, lease->iface, compare_by_iface) != NULL)
    {
        result = -EEXIST;
    }
    else
    {
        result = dmlist_push_back(g_leases, lease) ? 0 : -ENOMEM;
    }
    dmosi_mutex_unlock(g_leases_mutex);
    return result;
}

void dmdhcp_lease_table_remove(struct dmdhcp_lease* lease)
{
    dmosi_mutex_lock(g_leases_mutex);
    dmlist_remove(g_leases, lease, compare_pointer);
    dmosi_mutex_unlock(g_leases_mutex);
}

/* ============================================================================
 *                      Simple per-lease accessors
 * ========================================================================== */

dmod_dmdhcp_api_declaration(1.0, int, _set_callbacks, ( dmdhcp_lease_t lease, const dmdhcp_callbacks_t* callbacks, void* user_data ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return -EINVAL;

    dmosi_mutex_lock(lease->lock);
    lease->callbacks = (callbacks != NULL) ? *callbacks : (dmdhcp_callbacks_t){ 0 };
    lease->user_data = user_data;
    dmosi_mutex_unlock(lease->lock);
    return 0;
}

dmod_dmdhcp_api_declaration(1.0, dmdhcp_state_t, _get_state, ( dmdhcp_lease_t lease ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return dmdhcp_state_failed;

    dmosi_mutex_lock(lease->lock);
    dmdhcp_state_t state = lease->state;
    dmosi_mutex_unlock(lease->lock);
    return state;
}

dmod_dmdhcp_api_declaration(1.0, dmnetif_iface_t, _get_iface, ( dmdhcp_lease_t lease ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return NULL;
    return lease->iface; /* set once at creation, never mutated - safe without the lock */
}

dmod_dmdhcp_api_declaration(1.0, void*, _get_user_data, ( dmdhcp_lease_t lease ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return NULL;

    dmosi_mutex_lock(lease->lock);
    void* user_data = lease->user_data;
    dmosi_mutex_unlock(lease->lock);
    return user_data;
}

dmod_dmdhcp_api_declaration(1.0, uint32_t, _get_xid, ( dmdhcp_lease_t lease ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return 0;

    dmosi_mutex_lock(lease->lock);
    uint32_t xid = lease->xid;
    dmosi_mutex_unlock(lease->lock);
    return xid;
}

dmod_dmdhcp_api_declaration(1.0, int, _get_lease_info, ( dmdhcp_lease_t lease, dmdhcp_lease_info_t* out ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC || out == NULL)
        return -EINVAL;

    dmosi_mutex_lock(lease->lock);
    int result;
    if (lease->state != dmdhcp_state_bound && lease->state != dmdhcp_state_renewing && lease->state != dmdhcp_state_rebinding)
    {
        result = -EINVAL;
    }
    else
    {
        out->ip_address = lease->offered_ip;
        out->netmask = lease->netmask;
        out->gateway = lease->gateway;
        out->server_id = lease->server_id;
        out->lease_time_sec = lease->lease_time_sec;
        out->renewal_time_sec = lease->t1_sec;
        out->rebinding_time_sec = lease->t2_sec;
        result = 0;
    }
    dmosi_mutex_unlock(lease->lock);
    return result;
}

dmod_dmdhcp_api_declaration(1.0, size_t, _get_dns_server_count, ( dmdhcp_lease_t lease ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC)
        return 0;

    dmosi_mutex_lock(lease->lock);
    size_t count = dmlist_size(lease->dns_servers);
    dmosi_mutex_unlock(lease->lock);
    return count;
}

dmod_dmdhcp_api_declaration(1.0, int, _get_dns_server, ( dmdhcp_lease_t lease, size_t index, dmroute_addr_t* out ))
{
    if (lease == NULL || lease->magic != DMDHCP_LEASE_MAGIC || out == NULL)
        return -EINVAL;

    dmosi_mutex_lock(lease->lock);
    dmroute_addr_t* entry = (dmroute_addr_t*)dmlist_get(lease->dns_servers, index);
    int result;
    if (entry == NULL)
    {
        result = -EINVAL;
    }
    else
    {
        *out = *entry;
        result = 0;
    }
    dmosi_mutex_unlock(lease->lock);
    return result;
}
