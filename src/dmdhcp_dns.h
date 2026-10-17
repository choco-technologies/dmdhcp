#ifndef DMDHCP_DNS_H
#define DMDHCP_DNS_H

/**
 * @file dmdhcp_dns.h
 * @brief Private bridge between the lease table and the
 *        dmdns_provide_servers DIF implementation
 *
 * Deliberately separate from dmdhcp_internal.h: the DIF implementation
 * lives in dmdhcp_registrations.c (a DIF implementation only registers in
 * a translation unit with DMOD_ENABLE_REGISTRATION set), and that file must
 * not pull in dmdhcp_internal.h's dmosi/dmlist headers - see its own top
 * comment. This header only needs the address type.
 */
#include "dmroute.h"

/**
 * @brief Called once per DNS server of a currently valid lease
 *
 * Same shape as dmdns_server_sink_t (dmip_addr_t is dmroute_addr_t), so
 * dmdns's sink can be passed straight through.
 */
typedef int (*dmdhcp_dns_visitor_t)( void* ctx, const dmroute_addr_t* server );

/**
 * @brief Report the DNS servers (option 6) of every lease that is currently
 *        BOUND, RENEWING or REBINDING
 *
 * Leases still being negotiated, or lost, report nothing - their servers
 * are either not confirmed yet or no longer valid. Takes the lease table
 * mutex, then each lease's own lock (the same order as everywhere else:
 * the table lock is never taken while a lease lock is held), so a lease
 * cannot be torn down while it is being visited. `visit` must not call
 * back into dmdhcp.
 */
void dmdhcp_lease_table_visit_dns_servers(dmdhcp_dns_visitor_t visit, void* ctx);

#endif // DMDHCP_DNS_H
