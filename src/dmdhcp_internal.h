#ifndef DMDHCP_INTERNAL_H
#define DMDHCP_INTERNAL_H

#include "dmdhcp.h"
#include "dmosi.h"
#include "dmlist.h"

/**
 * @file dmdhcp_internal.h
 * @brief Private state and cross-file plumbing for dmdhcp
 *
 * The public opaque dmdhcp_lease_t (struct dmdhcp_lease*) is defined here,
 * not in include/dmdhcp.h - callers only ever get a pointer, never see the
 * layout (opaque-handle + magic-guard pattern, worked example in
 * dm_sw_ring; also see dmtcp_internal.h for the same shape applied to a
 * comparably stateful protocol).
 *
 * File map:
 *
 *  - dmdhcp_registrations.c  DMOD_ENABLE_REGISTRATION - must stand alone,
 *                            see that file's own comment
 *  - dmdhcp_wire.c           dmdhcp_build_message()/_parse_message() (public API)
 *  - dmdhcp_options.c        RFC 2132 TLV codec (public API)
 *  - dmdhcp_lease_table.c    g_leases CRUD, destroy_with_context(), and the
 *                            simple per-lease accessors (_get_state/_get_iface/
 *                            _get_user_data/_get_xid/_set_callbacks)
 *  - dmdhcp_output.c         Message senders (DISCOVER/REQUEST variants/
 *                            DECLINE/RELEASE), and the retransmit_timer:
 *                            its creation, its callback (backoff resend OR
 *                            the deferred RFC 2131 §4.4.1 ARP probe,
 *                            branching on lease->state)
 *  - dmdhcp_input.c          dmudp_bind(DMDHCP_CLIENT_PORT, ...) registration
 *                            and dmdhcp_handle_datagram(): xid/iface match,
 *                            option parsing, per-state dispatch
 *  - dmdhcp_lifecycle.c      apply/unapply the lease to the interface, and
 *                            the lease_timer: its creation, its callback
 *                            (T1 -> T2 -> expiry, rearmed sequentially)
 *  - dmdhcp.c                dmod_init()/_deinit() and dmdhcp_start()/_stop()/
 *                            _release()/_renew()
 *
 * Threading: dmdhcp keeps no thread of its own. Inbound messages are
 * processed inline, synchronously, on whatever thread is pumping the
 * interface they arrived on (dmudp_datagram_handler_t's documented
 * delivery context). Retransmission backoff, the ARP conflict probe, and
 * T1/T2/lease-expiry run on dmosi_timer_t callbacks instead - a different
 * thread than the rx path. `lock` is the one mutex guarding every field
 * touched from both contexts - the same "two lock contexts" shape dmtcp
 * uses for its own retransmission timer (see dmtcp/docs/dmtcp.md). A user
 * callback (dmdhcp_callbacks_t member) is never invoked while `lock` is
 * held - always snapshot what's needed, unlock, then call out.
 */

/**
 * @brief Magic value guarding struct dmdhcp_lease - "DHCP" in ASCII
 *
 * Checked at the top of every function that receives a dmdhcp_lease_t.
 */
#define DMDHCP_LEASE_MAGIC 0x44484350u

/** @brief RFC 2131 §4.1 initial retransmission interval */
#define DMDHCP_RETRANSMIT_INITIAL_MS 4000u

/** @brief RFC 2131 §4.1 retransmission interval cap (doubles up to this) */
#define DMDHCP_RETRANSMIT_MAX_MS 64000u

/**
 * @brief How many consecutive retransmits of a REQUEST (SELECTING or
 *        INIT-REBOOT path) are attempted before giving up and falling
 *        back to a fresh DISCOVER cycle
 *
 * Unlike dmdhcp_state_selecting (which retries forever - there is no
 * fallback for "nobody answered a DISCOVER"), a REQUEST that never gets an
 * ACK/NAK likely means the offer/remembered lease is no longer valid, so
 * dmdhcp gives up on it rather than retrying indefinitely.
 */
#define DMDHCP_MAX_REQUEST_RETRANSMITS 4u

/** @brief RFC 2131 §4.4.1 conflict probe (dmarp_resolve()) timeout */
#define DMDHCP_ARP_PROBE_TIMEOUT_MS 1000u

/**
 * @brief Period the retransmit_timer is armed for just to hand off from
 *        the rx thread to the timer thread before running the ARP probe
 *
 * See this file's top comment and dmdhcp_output.c: dmarp_resolve() must
 * not be called inline from the rx thread that would also need to observe
 * its own reply.
 */
#define DMDHCP_PROBE_HANDOFF_MS 1u

/** @brief Consecutive address conflicts (DHCPDECLINE cycles) tolerated before giving up (dmdhcp_state_failed) */
#define DMDHCP_MAX_DECLINE_RETRIES 3u

/** @brief RFC 2131 §4.4.1: "SHOULD wait a minimum of ten seconds" after a DHCPDECLINE before restarting */
#define DMDHCP_DECLINE_PAUSE_MS 10000u

/** @brief arp_timeout_ms forwarded to dmudp_send()/_send_on_iface() for the actual frame transmission (distinct from DMDHCP_ARP_PROBE_TIMEOUT_MS, the RFC 2131 §4.4.1 conflict probe) */
#define DMDHCP_DEFAULT_ARP_TIMEOUT_MS 1000u

/** @brief Largest DHCP message (fixed header + cookie + options) dmdhcp builds or accepts */
#define DMDHCP_MAX_MESSAGE_LEN 576u

/**
 * @brief Which (if any) of a lease's own timers is currently executing the
 *        callback that led to a given teardown
 *
 * dmosi_timer_destroy() joins the timer's own worker thread - calling it
 * on a timer from within that same timer's own callback is a self-join.
 * Teardown paths pass the timer (if any) that is "self" here so it is
 * stopped but not destroyed - a small, bounded, documented leak instead of
 * a hang/use-after-free. Mirrors dmtcp_teardown_context_t exactly (see
 * dmtcp_internal.h) generalized to dmdhcp's two timers.
 */
typedef enum
{
    dmdhcp_teardown_context_normal,             /**< rx thread, dmdhcp_stop()/_release() caller, ... */
    dmdhcp_teardown_context_retransmit_timer,   /**< running on lease->retransmit_timer's own callback */
    dmdhcp_teardown_context_lease_timer,        /**< running on lease->lease_timer's own callback */
} dmdhcp_teardown_context_t;

/**
 * @brief One DHCP lease - the real definition behind dmdhcp_lease_t
 *
 * `lock` guards every field below it (not `magic`/`lock` themselves) - see
 * this file's top comment for the two contexts it's taken from.
 */
struct dmdhcp_lease
{
    uint32_t        magic;
    dmosi_mutex_t   lock;
    dmnetif_iface_t iface;
    dmdhcp_state_t  state;

    uint32_t        xid;

    /* Offer/lease fields, populated as they're learned from OFFER/ACK
     * options (dmdhcp_input.c) or pre-seeded from dmdhcp_options_t on an
     * INIT-REBOOT start (offered_ip only). */
    dmroute_addr_t     offered_ip;
    dmroute_addr_t     netmask;
    dmroute_addr_t     gateway;      /* family dmroute_family_none if no router was offered */
    dmroute_addr_t     server_id;    /* family dmroute_family_none until the first OFFER/ACK - not sent on an INIT-REBOOT REQUEST */
    uint32_t           lease_time_sec;
    uint32_t           t1_sec;
    uint32_t           t2_sec;
    dmlist_context_t*  dns_servers;  /* dmlist of heap dmroute_addr_t* (Dmod_Malloc'd) */

    uint32_t        retransmit_interval_ms;
    uint32_t        retransmit_count;   /* consecutive resends in the current requesting/rebooting phase - reset on every state entry */
    uint32_t        decline_retry_count;

    /* Reused across roles depending on lease->state - see dmdhcp_output.c's
     * retransmit_timer_callback(): backoff resend (selecting/requesting/
     * rebooting/renewing/rebinding), the deferred ARP probe hand-off
     * (checking_offer/checking_ack), or the DECLINE backoff pause
     * (declining). */
    dmosi_timer_t   retransmit_timer;

    /* Reused sequentially: armed for T1 on entering bound, rearmed for T2
     * on entering renewing, rearmed for the full lease expiry on entering
     * rebinding - see dmdhcp_lifecycle.c. */
    dmosi_timer_t   lease_timer;

    dmroute_route_t default_route;   /* only valid while iface_configured */
    bool            iface_configured;

    char* hostname; /* Dmod_StrDup of dmdhcp_options_t.hostname, or NULL */

    dmdhcp_callbacks_t callbacks;
    void*              user_data;
};

/* ---- dmdhcp_lease_table.c ---- */

int  dmdhcp_lease_table_init(void);
void dmdhcp_lease_table_deinit(void);

/**
 * @brief Allocate and zero-initialize a lease (mutex + both timers
 *        included) - not yet inserted into the lease table
 *
 * @return The new lease, or NULL on allocation failure (nothing left
 *         partially allocated)
 */
struct dmdhcp_lease* dmdhcp_lease_table_create(dmnetif_iface_t iface);

/**
 * @brief Free a lease's own resources (mutex, both timers fully
 *        destroyed, DNS server list, hostname, the struct itself)
 *
 * The caller must have already removed `lease` from the lease table
 * (dmdhcp_lease_table_remove()) if it was ever inserted - this function
 * never touches the table. Equivalent to
 * dmdhcp_lease_table_destroy_with_context(lease, dmdhcp_teardown_context_normal).
 */
void dmdhcp_lease_table_destroy(struct dmdhcp_lease* lease);

/**
 * @brief Like dmdhcp_lease_table_destroy(), except the timer named by
 *        `context` (if any) is only stopped, never destroyed - see
 *        dmdhcp_teardown_context_t
 */
void dmdhcp_lease_table_destroy_with_context(struct dmdhcp_lease* lease, dmdhcp_teardown_context_t context);

/** @return The lease tracked for `iface`, or NULL if it has none */
struct dmdhcp_lease* dmdhcp_lease_table_find_by_iface(dmnetif_iface_t iface);

/** @return The lease tracked for `iface` whose current xid == `xid`, or NULL */
struct dmdhcp_lease* dmdhcp_lease_table_find_by_xid(dmnetif_iface_t iface, uint32_t xid);

/** @return 0 on success, -EEXIST if `iface` already has a tracked lease, -ENOMEM */
int dmdhcp_lease_table_insert(struct dmdhcp_lease* lease);

/** @brief Remove `lease` from the table (a no-op if it isn't in it). Does not free it. */
void dmdhcp_lease_table_remove(struct dmdhcp_lease* lease);

/**
 * @brief Free every entry currently in lease->dns_servers and empty the
 *        list (the list itself is not destroyed - only
 *        dmdhcp_lease_table_destroy_with_context() does that)
 *
 * Called before repopulating from a fresh OFFER/ACK's option 6
 * (dmdhcp_input.c) and once during final teardown.
 */
void dmdhcp_lease_table_clear_dns_servers(struct dmdhcp_lease* lease);

/* ---- dmdhcp_output.c ---- */

/**
 * @brief Create lease->retransmit_timer (not started)
 *
 * Defined here (not dmdhcp_lease_table.c) because it must be created by
 * the same translation unit that defines its dmosi_timer_callback_t - see
 * this file's top comment / dmtcp_internal.h's identical rule.
 */
dmosi_timer_t dmdhcp_output_create_retransmit_timer(struct dmdhcp_lease* lease);

/**
 * @brief (Re)start discovery: draw a fresh xid, send DHCPDISCOVER
 *        (broadcast), set state = dmdhcp_state_selecting, arm
 *        retransmit_timer at the initial backoff interval
 *
 * Assumes lease->lock is held. Used by dmdhcp_start() (normal path),
 * every "give up and start over" path, and the post-DECLINE-pause restart.
 */
void dmdhcp_output_start_discovery(struct dmdhcp_lease* lease);

/**
 * @brief Start an INIT-REBOOT attempt: draw a fresh xid, send a broadcast
 *        REQUEST for lease->offered_ip (already pre-seeded by
 *        dmdhcp_start()), set state = dmdhcp_state_rebooting, arm
 *        retransmit_timer
 *
 * Assumes lease->lock is held. Used only by dmdhcp_start() when
 * dmdhcp_options_t.requested_ip was given.
 */
void dmdhcp_output_start_reboot(struct dmdhcp_lease* lease);

/**
 * @brief Enter dmdhcp_state_renewing: send a unicast REQUEST to
 *        lease->server_id, arm retransmit_timer
 *
 * Assumes lease->lock is held. Used by the lease_timer's T1 fire and by
 * dmdhcp_renew().
 */
void dmdhcp_output_start_renew(struct dmdhcp_lease* lease);

/**
 * @brief Enter dmdhcp_state_rebinding: send a broadcast REQUEST, arm
 *        retransmit_timer
 *
 * Assumes lease->lock is held. Used by the lease_timer's T2 fire.
 */
void dmdhcp_output_start_rebind(struct dmdhcp_lease* lease);

/**
 * @brief Send a best-effort DHCPRELEASE (unicast to lease->server_id)
 *
 * Assumes lease->lock is held. Failure is not propagated - see
 * dmdhcp_release()'s own doc comment in dmdhcp.h.
 */
int dmdhcp_output_send_release(struct dmdhcp_lease* lease);

/* ---- dmdhcp_input.c ---- */

/**
 * @brief Register/unregister dmdhcp_handle_datagram() with dmudp, called
 *        from dmod_init()/_deinit() (see dmdhcp.c)
 *
 * Kept in dmdhcp_input.c, the same translation unit as
 * dmdhcp_handle_datagram() itself - required by the loader (see this
 * file's top comment).
 */
int  dmdhcp_input_register(void);
void dmdhcp_input_unregister(void);

/* ---- dmdhcp_lifecycle.c ---- */

/**
 * @brief Create lease->lease_timer (not started)
 *
 * Defined here for the same reason dmdhcp_output_create_retransmit_timer()
 * lives in dmdhcp_output.c - see this file's top comment.
 */
dmosi_timer_t dmdhcp_lifecycle_create_lease_timer(struct dmdhcp_lease* lease);

/**
 * @brief Apply the currently-held lease fields to lease->iface:
 *        dmnetif_set_netmask() -> _set_ip_address() -> _set_broadcast(),
 *        then install a default route via dmroute_add() if a gateway was
 *        offered
 *
 * Assumes lease->lock is held. Idempotent - safe to call again on a
 * renewal even if nothing actually changed.
 *
 * @return 0 on success, negative errno if any dmnetif_set_*()/dmroute_add()
 *         call failed (iface_configured left however far it got)
 */
int dmdhcp_lifecycle_apply(struct dmdhcp_lease* lease);

/**
 * @brief Undo dmdhcp_lifecycle_apply(): remove the default route (if any)
 *        and clear the interface's address/netmask/broadcast
 *
 * Assumes lease->lock is held. Safe to call even if iface_configured is
 * already false (a no-op).
 */
void dmdhcp_lifecycle_unapply(struct dmdhcp_lease* lease);

/**
 * @brief Arm lease_timer for T1 (lease->t1_sec from now) - called right
 *        after entering dmdhcp_state_bound
 *
 * Assumes lease->lock is held.
 */
void dmdhcp_lifecycle_arm_t1(struct dmdhcp_lease* lease);

/**
 * @brief Arm lease_timer for T2 (lease->t2_sec from the original BOUND
 *        entry) - called right after entering dmdhcp_state_renewing
 *
 * Assumes lease->lock is held.
 */
void dmdhcp_lifecycle_arm_t2(struct dmdhcp_lease* lease);

/**
 * @brief Arm lease_timer for the full lease expiry (lease->lease_time_sec
 *        from the original BOUND entry) - called right after entering
 *        dmdhcp_state_rebinding
 *
 * Assumes lease->lock is held.
 */
void dmdhcp_lifecycle_arm_expiry(struct dmdhcp_lease* lease);

/**
 * @brief Lease lost (NAK'd during renewing/rebinding, or the lease timer
 *        reached full expiry with no ACK): unapply the interface, fire
 *        on_expired_or_lost, then restart discovery
 *
 * Manages lease->lock itself (must be called with it NOT held) - it needs
 * to release the lock before calling the user's on_expired_or_lost, the
 * same "never call a callback while holding the lock" rule as everywhere
 * else in this module. Used by dmdhcp_input.c (NAK case) and by this
 * file's own lease_timer callback (expiry case).
 */
void dmdhcp_lifecycle_handle_lost(struct dmdhcp_lease* lease);

#endif // DMDHCP_INTERNAL_H
