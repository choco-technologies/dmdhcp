#ifndef DMDHCP_H
#define DMDHCP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmnetif.h"
#include "dmroute.h"
#include "dmdhcp_defs.h"

/**
 * @file dmdhcp.h
 * @brief DMOD DHCP - Public API
 *
 * dmdhcp is a DHCP *client* (RFC 2131/2132) - it does not implement a DHCP
 * server. One dmdhcp_lease_t represents the whole lease lifecycle (DORA,
 * renewal, rebinding, release) for one dmnetif_iface_t. On BOUND, dmdhcp
 * itself applies the lease to the interface (dmnetif_set_netmask()/
 * _set_ip_address()/_set_broadcast(), plus a default route via
 * dmroute_add()) and reverts it on release/expiry - a caller does not need
 * to apply the lease itself.
 *
 * Like dmtcp/dmudp/dmicmp, dmdhcp registers with dmudp_bind(DMDHCP_CLIENT_PORT,
 * ...) in dmod_init() and processes inbound DHCP messages inline, on
 * whatever thread is pumping the interface they arrived on - no thread or
 * queue of its own. The one structural exception is time-driven behavior
 * with no inbound message to piggyback on - retransmission backoff, the
 * RFC 2131 §4.4.1 ARP conflict probe, and T1/T2/lease-expiry - which runs
 * on a dmosi_timer_t instead, in timer/interrupt context (a different
 * thread than the rx path). A dmosi_mutex_t per lease guards every field
 * touched from both contexts, the same "two lock contexts" shape dmtcp
 * uses for its own retransmission timer - see docs/dmdhcp.md.
 *
 * Broadcast sends (DISCOVER; REQUEST during SELECTING/INIT-REBOOT/
 * REBINDING; DECLINE) go through dmudp_send_on_iface() rather than the
 * routed dmudp_send() - a DHCP client has no route to rely on until it has
 * a lease. RENEWING/RELEASE are unicast to the known server through the
 * ordinary routed dmudp_send(), since a route exists by then. XIDs and
 * retransmit jitter come from dmosi_rand32() (dmosi.h) - dmdhcp holds no
 * PRNG of its own.
 *
 * dmdhcp depends on dmudp (send/bind), dmnetif (dmnetif_iface_t, applying
 * the lease), dmroute (dmroute_addr_t, the default route), dmarp
 * (dmarp_resolve() as the RFC 2131 §4.4.1 conflict probe), dmlist (the
 * lease table and DNS server list) and dmosi (mutexes, dmosi_timer_t,
 * dmosi_rand32()). dmip is linked only because dmudp.h itself needs
 * dmip.h (dmip_addr_t) - dmdhcp never calls a dmip_* function directly,
 * see CMakeLists.txt.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *                    Wire ports and message header (RFC 2131 §2)
 * ========================================================================== */

#define DMDHCP_SERVER_PORT 67u
#define DMDHCP_CLIENT_PORT 68u

/** @brief RFC 2132 §9.1 - marks the start of the options area. */
#define DMDHCP_MAGIC_COOKIE 0x63825363u

/**
 * @brief Fixed BOOTP/DHCP header length (op..chaddr, RFC 2131 §2), before
 *        the 4-byte magic cookie
 *
 * sname/file are not modeled - always zero-filled on send, ignored on
 * receive (option overload, RFC 2132 §9.3, is out of scope).
 */
#define DMDHCP_FIXED_HEADER_LEN 236u

#define DMDHCP_CHADDR_LEN 16u

/**
 * @brief BROADCAST bit of the `flags` field (RFC 2131 §2)
 *
 * dmdhcp always sets this on every message it sends - it can't receive a
 * unicast reply before it has an address.
 */
#define DMDHCP_FLAG_BROADCAST 0x8000u

typedef enum
{
    dmdhcp_op_bootrequest = 1,
    dmdhcp_op_bootreply   = 2,
} dmdhcp_op_t;

/**
 * @brief Parsed DHCP/BOOTP fixed header fields (RFC 2131 §2)
 *
 * ciaddr/yiaddr/siaddr/giaddr are dmroute_addr_t with family
 * dmroute_family_v4 (DHCP is IPv4-only) or dmroute_family_none for a field
 * that's legitimately all-zero (e.g. ciaddr before a lease exists).
 */
typedef struct
{
    dmdhcp_op_t    op;
    uint8_t        htype;
    uint8_t        hlen;
    uint8_t        hops;
    uint32_t       xid;
    uint16_t       secs;
    uint16_t       flags;
    dmroute_addr_t ciaddr;
    dmroute_addr_t yiaddr;
    dmroute_addr_t siaddr;
    dmroute_addr_t giaddr;
    uint8_t        chaddr[DMDHCP_CHADDR_LEN];
} dmdhcp_message_t;

/**
 * @brief Build a DHCP/BOOTP fixed header + magic cookie into `buffer`
 *
 * Writes exactly DMDHCP_FIXED_HEADER_LEN + 4 bytes. Caller appends options
 * (dmdhcp_options_write()) immediately after.
 *
 * @param buffer     Output buffer
 * @param buffer_len Capacity of `buffer`
 * @param message    Header fields to write
 * @return 0 on success, negative errno on failure (buffer too small,
 *         `message` NULL, an address field with an unsupported family)
 */
dmod_dmdhcp_api(1.0, int, _build_message, ( uint8_t* buffer, size_t buffer_len, const dmdhcp_message_t* message ));

/**
 * @brief Parse a DHCP/BOOTP fixed header, validating the magic cookie
 *
 * @param buffer         Raw message bytes
 * @param length         Length of `buffer`
 * @param message        Output - parsed fixed header fields
 * @param options_offset Output - byte offset into `buffer` where the
 *                        options area begins (right after the cookie)
 * @return 0 on success, negative errno on failure (too short, bad magic
 *         cookie)
 */
dmod_dmdhcp_api(1.0, int, _parse_message, ( const uint8_t* buffer, size_t length, dmdhcp_message_t* message, size_t* options_offset ));

/* ============================================================================
 *                       Options codec (RFC 2132 TLV options)
 * ========================================================================== */

/**
 * @brief One parsed option
 *
 * `data` borrows storage from the buffer dmdhcp_options_find() was called
 * on - copy it out if it must outlive that buffer.
 */
typedef struct
{
    uint8_t        code;
    uint8_t        length;
    const uint8_t* data;
} dmdhcp_option_t;

#define DMDHCP_OPT_PAD           0u
#define DMDHCP_OPT_SUBNET_MASK   1u
#define DMDHCP_OPT_ROUTER        3u
#define DMDHCP_OPT_DNS_SERVER    6u
#define DMDHCP_OPT_HOSTNAME      12u
#define DMDHCP_OPT_REQUESTED_IP  50u
#define DMDHCP_OPT_LEASE_TIME    51u
#define DMDHCP_OPT_MSG_TYPE      53u
#define DMDHCP_OPT_SERVER_ID     54u
#define DMDHCP_OPT_RENEWAL_T1    58u
#define DMDHCP_OPT_REBINDING_T2  59u
#define DMDHCP_OPT_END           255u

typedef enum
{
    dmdhcp_msg_discover = 1,
    dmdhcp_msg_offer    = 2,
    dmdhcp_msg_request  = 3,
    dmdhcp_msg_decline  = 4,
    dmdhcp_msg_ack      = 5,
    dmdhcp_msg_nak      = 6,
    dmdhcp_msg_release  = 7,
    dmdhcp_msg_inform   = 8,
} dmdhcp_msg_type_t;

/**
 * @brief Encode a list of options into `buffer`, terminated with DMDHCP_OPT_END
 *
 * @param buffer       Output buffer (options area only - after the fixed
 *                      header + magic cookie written by dmdhcp_build_message())
 * @param buffer_len   Capacity of `buffer`
 * @param options      Options to encode, in order
 * @param option_count Number of entries in `options`
 * @param out_len      Output - total bytes written, including the END marker
 * @return 0 on success, negative errno on failure (buffer too small, an
 *         option with length > 255)
 */
dmod_dmdhcp_api(1.0, int, _options_write, ( uint8_t* buffer, size_t buffer_len, const dmdhcp_option_t* options, size_t option_count, size_t* out_len ));

/**
 * @brief Scan an options area for one option code, honoring PAD/END
 *
 * @param options     Options area (as reported by dmdhcp_parse_message()'s
 *                     options_offset)
 * @param options_len Length of the options area
 * @param code        Option code to find (a DMDHCP_OPT_* constant)
 * @param out         Output - the found option (data still points into
 *                     `options`)
 * @return 0 if found, -ENOENT if not present, negative errno on a
 *         malformed options area (truncated length byte)
 */
dmod_dmdhcp_api(1.0, int, _options_find, ( const uint8_t* options, size_t options_len, uint8_t code, dmdhcp_option_t* out ));

/** @brief Decode a 4-byte big-endian option (e.g. lease time, T1, T2). Returns -EINVAL if opt->length != 4. */
dmod_dmdhcp_api(1.0, int, _option_get_u32, ( const dmdhcp_option_t* opt, uint32_t* out ));

/** @brief Decode a single 4-byte IPv4 address option (e.g. subnet mask, requested IP, server ID). Returns -EINVAL if opt->length != 4. */
dmod_dmdhcp_api(1.0, int, _option_get_addr, ( const dmdhcp_option_t* opt, dmroute_addr_t* out ));

/** @brief How many 4-byte addresses a multi-address option (router, DNS server) carries - opt->length / 4. */
dmod_dmdhcp_api(1.0, size_t, _option_addr_count, ( const dmdhcp_option_t* opt ));

/** @brief Decode the Nth address (0-based) of a multi-address option. Returns -EINVAL if index >= dmdhcp_option_addr_count(opt). */
dmod_dmdhcp_api(1.0, int, _option_get_addr_at, ( const dmdhcp_option_t* opt, size_t index, dmroute_addr_t* out ));

/* ============================================================================
 *                              Lease handle and state
 * ========================================================================== */

/* Opaque handle - the real struct is defined in src/dmdhcp_internal.h */
typedef struct dmdhcp_lease* dmdhcp_lease_t;

/**
 * @brief RFC 2131 §4.4 client state machine, plus dmdhcp-specific states
 *        for the RFC 2131 §4.4.1 ARP conflict probe and DECLINE backoff
 */
typedef enum
{
    dmdhcp_state_init = 0,
    dmdhcp_state_selecting,       /**< DISCOVER sent/retried, awaiting OFFER */
    dmdhcp_state_checking_offer,  /**< OFFER accepted - ARP conflict probe in flight, on the timer thread */
    dmdhcp_state_requesting,      /**< REQUEST (SELECTING path) sent/retried, awaiting ACK/NAK */
    dmdhcp_state_checking_ack,    /**< ACK accepted - final ARP conflict probe in flight, pre-BOUND */
    dmdhcp_state_bound,
    dmdhcp_state_renewing,        /**< T1 elapsed - unicast REQUEST retries to the known server */
    dmdhcp_state_rebinding,       /**< T2 elapsed - broadcast REQUEST retries to any server */
    dmdhcp_state_init_reboot,     /**< dmdhcp_options_t.requested_ip was given - about to verify a remembered lease */
    dmdhcp_state_rebooting,       /**< REQUEST sent from INIT-REBOOT, awaiting ACK/NAK */
    dmdhcp_state_declining,       /**< DHCPDECLINE just sent - RFC 2131 §4.4.1's >=10s pause before restart */
    dmdhcp_state_failed,          /**< Terminal - repeated address conflicts, or a fatal internal error */
} dmdhcp_state_t;

/* ============================================================================
 *                                  Callbacks
 * ========================================================================== */

/**
 * @brief Fired when a lease is (re-)acquired - state has just entered BOUND
 *
 * Called after dmdhcp has already applied the lease to the interface
 * (netmask/address/broadcast/default route). Read the lease's fields with
 * dmdhcp_get_lease_info()/_get_dns_server() from inside this callback.
 *
 * @param lease      The lease
 * @param is_renewal false the first time a lease is acquired; true on every
 *                    subsequent successful renewal/rebinding
 * @param user_data   As passed to dmdhcp_start()/_set_callbacks()
 */
typedef void (*dmdhcp_bound_handler_t)( dmdhcp_lease_t lease, bool is_renewal, void* user_data );

/**
 * @brief Fired when a lease is lost - NAK'd, or the lease timer expired
 *        with no successful renewal
 *
 * The interface has already been de-configured (address/netmask/broadcast/
 * default route removed) by the time this fires. dmdhcp automatically
 * restarts from dmdhcp_state_init afterward - no action is required to
 * keep trying, only to be notified.
 *
 * @param lease     The lease
 * @param user_data As passed to dmdhcp_start()/_set_callbacks()
 */
typedef void (*dmdhcp_expired_handler_t)( dmdhcp_lease_t lease, void* user_data );

/**
 * @brief Fired on a terminal failure - state has just entered dmdhcp_state_failed
 *
 * Currently only raised after DMDHCP_MAX_DECLINE_RETRIES consecutive
 * address conflicts. The lease stays in dmdhcp_state_failed until
 * dmdhcp_stop() is called - dmdhcp does not retry on its own from this state.
 *
 * @param lease     The lease
 * @param error     A negative errno describing the failure (e.g. -EADDRINUSE)
 * @param user_data As passed to dmdhcp_start()/_set_callbacks()
 */
typedef void (*dmdhcp_error_handler_t)( dmdhcp_lease_t lease, int error, void* user_data );

typedef struct
{
    dmdhcp_bound_handler_t   on_bound;
    dmdhcp_expired_handler_t on_expired_or_lost;
    dmdhcp_error_handler_t   on_error;
} dmdhcp_callbacks_t;

/* ============================================================================
 *                             Starting a lease
 * ========================================================================== */

typedef struct
{
    /**
     * Option 12 to send with every DISCOVER/REQUEST, or NULL to omit.
     * Copied internally (Dmod_StrDup) - the pointer need not outlive this call.
     */
    const char* hostname;

    /**
     * family dmroute_family_v4 -> start in INIT-REBOOT (RFC 2131 §4.3.2),
     * verifying this remembered address instead of a fresh DISCOVER.
     * family dmroute_family_none -> normal INIT/SELECTING.
     */
    dmroute_addr_t requested_ip;
} dmdhcp_options_t;

/**
 * @brief Begin acquiring a lease on `iface`
 *
 * Starts the RFC 2131 state machine (DISCOVER/OFFER/REQUEST/ACK, or
 * INIT-REBOOT if options->requested_ip is set) and keeps it running -
 * retransmission, renewal, rebinding, and restart-on-NAK/expiry all
 * happen automatically until dmdhcp_stop()/_release() is called. The
 * first DISCOVER's own send result is not treated as fatal (DHCP is
 * inherently retry-based over an unreliable broadcast) - failures are only
 * reported through the callbacks below.
 *
 * @param iface     Interface to acquire a lease on. Must not already have
 *                   an active lease (see dmdhcp_stop()/_release() first).
 * @param callbacks Event callbacks (may be NULL to receive none - use
 *                   dmdhcp_set_callbacks() later, or just poll dmdhcp_get_state())
 * @param user_data Opaque pointer passed back on every callback
 * @param options   May be NULL for default behavior (no hostname, normal
 *                   INIT/SELECTING)
 * @return A valid handle on success, or NULL on allocation failure or if
 *         `iface` already has an active lease
 */
dmod_dmdhcp_api(1.0, dmdhcp_lease_t, _start, ( dmnetif_iface_t iface, const dmdhcp_callbacks_t* callbacks, void* user_data, const dmdhcp_options_t* options ));

/** @brief Replace a lease's callbacks/user_data. Safe to call at any time, including from inside a callback. */
dmod_dmdhcp_api(1.0, int, _set_callbacks, ( dmdhcp_lease_t lease, const dmdhcp_callbacks_t* callbacks, void* user_data ));

/**
 * @brief Force an immediate renewal attempt
 *
 * Only meaningful in dmdhcp_state_bound (moves to dmdhcp_state_renewing
 * immediately, ignoring the T1 timer) - a no-op returning 0 in any other
 * state.
 */
dmod_dmdhcp_api(1.0, int, _renew, ( dmdhcp_lease_t lease ));

/**
 * @brief Release the lease: send DHCPRELEASE (best-effort, unicast to the
 *        server - failure is not reported), de-configure the interface,
 *        and free `lease`
 *
 * `lease` must not be used after this call returns.
 */
dmod_dmdhcp_api(1.0, int, _release, ( dmdhcp_lease_t lease ));

/**
 * @brief Stop and free `lease` immediately, WITHOUT sending DHCPRELEASE
 *
 * Use this for a hard shutdown (e.g. the interface itself is going away);
 * prefer dmdhcp_release() for a clean, cooperative release. Safe to call
 * with NULL. `lease` must not be used after this call returns.
 */
dmod_dmdhcp_api(1.0, void, _stop, ( dmdhcp_lease_t lease ));

/* ============================================================================
 *                                  Accessors
 * ========================================================================== */

dmod_dmdhcp_api(1.0, dmdhcp_state_t,  _get_state,     ( dmdhcp_lease_t lease ));
dmod_dmdhcp_api(1.0, dmnetif_iface_t, _get_iface,     ( dmdhcp_lease_t lease ));
dmod_dmdhcp_api(1.0, void*,           _get_user_data, ( dmdhcp_lease_t lease ));

/** @brief The lease's current transaction ID - mainly a diagnostics/testing hook, stable for the lifetime of one DORA/renewal attempt. */
dmod_dmdhcp_api(1.0, uint32_t, _get_xid, ( dmdhcp_lease_t lease ));

/**
 * @brief Snapshot of the currently active lease's parameters
 *
 * A single struct rather than several accessors so a caller reading it
 * mid-renewal never observes a torn update (some fields from the old
 * lease, some from the new one).
 */
typedef struct
{
    dmroute_addr_t ip_address;
    dmroute_addr_t netmask;
    dmroute_addr_t gateway;             /**< family dmroute_family_none if the server offered no router (option 3) */
    dmroute_addr_t server_id;
    uint32_t       lease_time_sec;
    uint32_t       renewal_time_sec;    /**< T1 */
    uint32_t       rebinding_time_sec;  /**< T2 */
} dmdhcp_lease_info_t;

/** @brief Get the current lease's parameters. Returns -EINVAL unless dmdhcp_get_state() is one of BOUND/RENEWING/REBINDING. */
dmod_dmdhcp_api(1.0, int, _get_lease_info, ( dmdhcp_lease_t lease, dmdhcp_lease_info_t* out ));

/** @brief Number of DNS servers offered (option 6). dmdhcp owns this storage - no other module in this ecosystem tracks DNS configuration. */
dmod_dmdhcp_api(1.0, size_t, _get_dns_server_count, ( dmdhcp_lease_t lease ));

/** @brief Get the Nth (0-based) offered DNS server address. Returns -EINVAL if index >= dmdhcp_get_dns_server_count(lease). */
dmod_dmdhcp_api(1.0, int, _get_dns_server, ( dmdhcp_lease_t lease, size_t index, dmroute_addr_t* out ));

#ifdef __cplusplus
}
#endif

#endif // DMDHCP_H
