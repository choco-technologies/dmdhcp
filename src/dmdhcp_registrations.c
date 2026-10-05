/*
 * Registers dmdhcp's Built-in API in the .dmod.inputs section - the same
 * shape dmlist_registrations.c/dmosi_registrations.c/dmtcp_registrations.c
 * use for their own multi-file modules.
 *
 * This must live in its own translation unit, separate from every other
 * dmdhcp_*.c file: the registration struct array dmdhcp_defs.h generates
 * when DMOD_ENABLE_REGISTRATION is set covers every function declared in
 * dmdhcp.h, not just the ones defined in whichever file set the macro - so
 * defining it in more than one translation unit produces one duplicate
 * "multiple definition" linker error per public function. Only dmdhcp.h is
 * included here (not the full dmod.h) so this can't accidentally
 * re-register dmod's own kernel Built-in APIs too - see
 * dmtcp_registrations.c's own doc comment for the same reasoning.
 *
 * The dmdns_provide_servers DIF implementation lives here for the same
 * reason: a DIF implementation is only registered from a translation unit
 * with DMOD_ENABLE_REGISTRATION set. dmdns.h is needed only for the DIF's
 * declaration - dmdhcp calls no dmdns function, so dmdns is not a required
 * module of dmdhcp and a board without DNS loads nothing extra.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmdhcp.h"
#include "dmdns.h"
#include "dmdhcp_dns.h"

/**
 * @brief dmdns_provide_servers DIF (see dmdns.h): report the DNS servers
 *        (option 6) of every currently valid lease
 *
 * dmdns asks this on every lookup that goes to the network, so the servers
 * of a lease that expired, was NAK'd, released or stopped are simply never
 * reported again - nothing has to be removed from dmdns.
 */
dmod_dmdns_dif_api_declaration(1.0, dmdhcp, void, _provide_servers, ( dmdns_server_sink_t add, void* sink_ctx ))
{
    if (add != NULL)
        dmdhcp_lease_table_visit_dns_servers(add, sink_ctx);
}
