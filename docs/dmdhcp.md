# dmdhcp architecture

dmdhcp is a DHCP client (RFC 2131/2132), not a server. This document
covers the parts that aren't obvious from `include/dmdhcp.h`'s doc
comments alone: why it needs no thread of its own, the two-context
locking rule, why broadcast sends need a dedicated code path, and the
full state machine.

## No thread of its own

`dmudp_bind(DMDHCP_CLIENT_PORT, ...)` delivers every inbound DHCP message
inline, on whatever thread is already pumping the interface it arrived on
(owned by `networkd`, not dmdhcp). This is the same mechanism `dmicmp`,
`dmudp`'s own callers, and `dmtcp` all rely on - no protocol module in
this ecosystem spins up its own OS thread; only genuine *services*
(`dmnet/services/networkd`) do, because they own a blocking pump loop that
has to run somewhere.

The one thing dmdhcp needs that has no inbound message to piggyback on is
time-driven behavior: retransmission backoff, the RFC 2131 §4.4.1 ARP
conflict probe, and T1/T2/lease-expiry. `dmtcp` already solves exactly
this shape of problem for its own retransmission timer, using
`dmosi_timer_t` instead of a thread - dmdhcp does the same, with two
timers per lease (see "Two timers, two roles" below).

## Two lock contexts, one per-lease mutex

`lease->lock` is taken from two different threads:

1. The rx-thread callback (`dmdhcp_handle_datagram()`, `dmdhcp_input.c`) -
   whenever a message arrives.
2. The timer-service thread (`retransmit_timer_callback()` /
   `lease_timer_callback()`) - whenever a deadline elapses.

A user callback (`dmdhcp_callbacks_t` member) is **never** invoked while
`lease->lock` is held - every call site snapshots what it needs, unlocks,
calls out, and re-locks only if it still needs to touch the lease
afterward. This is the same rule `dmtcp` documents for its own
`conn->lock`.

## Two timers, two roles

- `retransmit_timer` is reused across three roles depending on
  `lease->state`, since a lease is in at most one of them at a time:
  backoff resend (SELECTING/REQUESTING/REBOOTING/RENEWING/REBINDING), the
  deferred ARP conflict probe hand-off (CHECKING_OFFER/CHECKING_ACK), or
  the RFC 2131 §4.4.1 post-DECLINE pause (DECLINING).
- `lease_timer` is reused sequentially through a lease's life: armed for
  T1 on entering BOUND, rearmed for T2 on entering RENEWING, rearmed for
  the full lease expiry on entering REBINDING.

**A timer callback must never destroy its own timer** -
`dmosi_timer_destroy()` joins the timer's own worker thread, and doing
that from inside that same thread's callback is a self-join. Every
teardown path threads a `dmdhcp_teardown_context_t` through to
`dmdhcp_lease_table_destroy_with_context()`, which stops (but does not
destroy) whichever timer is "self" for that teardown - a small, bounded,
documented leak (one `dmosi_timer_t`) instead of a hang or
use-after-free. This mirrors `dmtcp_teardown_context_t` exactly.

## Why the ARP probe can't run inline on the rx thread

`dmarp_resolve()` on a cache miss blocks waiting for a *future*
`dmarp_note_frame()` call to signal a reply - and that call is made by the
same per-interface rx pump thread that would be running dmdhcp's own
`dmudp_bind()` handler. Calling `dmarp_resolve()` synchronously from
inside that handler would block the one thread that could ever satisfy
the wait it's blocking on - a guaranteed self-timeout, not just a slow
path.

The fix: when `dmdhcp_handle_datagram()` accepts an OFFER or ACK, it does
the minimal state update (parse options, `state = checking_offer` /
`checking_ack`) and arms `retransmit_timer` for `DMDHCP_PROBE_HANDOFF_MS`
(1 ms) - just enough to hand the next step to the timer-service thread,
a genuinely different thread. `retransmit_timer_callback()`'s
`run_conflict_probe()` is where the real, potentially-blocking
`dmarp_resolve()` call happens.

This does mean a timer callback can block for up to
`DMDHCP_ARP_PROBE_TIMEOUT_MS`, which violates `dmosi_timer_callback_t`'s
own "keep the callback short" guidance - on a backend where all timers
share one worker (e.g. FreeRTOS's timer-service task), this stalls every
other timer in the system for that long. It happens at most twice per
initial DORA cycle (once for the OFFER probe, once for the ACK probe) and
never again afterward (RENEWING/REBINDING skip the probe entirely, see
below) - a deliberate, bounded, documented tradeoff, not an oversight.

## Why broadcast sends need dmudp_send_on_iface(), not dmudp_send()

DISCOVER, and REQUEST during SELECTING/INIT-REBOOT/REBINDING, and
DECLINE, are all broadcast to `255.255.255.255`. The routed
`dmudp_send()` (and `dmnetbridge_send()` underneath it) picks its egress
interface via `dmroute_lookup()` - which fails outright before any lease
exists (no route yet), and even once a lease exists, `resolve_egress()`
substitutes a matched route's gateway as the ARP next-hop for *any*
destination that route covers, including a broadcast one. A routed
broadcast send would therefore resolve the gateway's MAC instead of
transmitting an on-link broadcast frame.

`dmudp_send_on_iface()`/`dmip_v4_send_on_iface()`/
`dmnetbridge_send_on_iface()` (added upstream alongside this module)
bypass routing entirely: the caller names the interface directly, and the
destination is treated as on-link. `dmarp_resolve()` itself also learned
to short-circuit any broadcast target (the limited broadcast address, or
the interface's own configured subnet broadcast) straight to
`FF:FF:FF:FF:FF:FF`, with no real ARP exchange - a general correctness
fix, not dmdhcp-specific.

Only RENEWING's REQUEST and DHCPRELEASE are unicast to a known server
address, through the ordinary routed `dmudp_send()` - by then a route
exists and there's no broadcast-vs-gateway ambiguity.

## State machine

```
init --DISCOVER--> selecting --OFFER--> checking_offer --(no conflict)--> requesting
                       ^                      |                              |
                       |                 (conflict: DECLINE)                ACK
                       |                      v                              v
                    declining <----- (retries exhausted: failed)      checking_ack --(no conflict)--> bound
                                                                              |                           |
                                                                       (conflict: DECLINE)                T1
                                                                                                            v
                                                                                                        renewing --ACK--> bound
                                                                                                            |
                                                                                                            T2
                                                                                                            v
                                                                                                       rebinding --ACK--> bound
                                                                                                            |
                                                                                                    (NAK, or full expiry)
                                                                                                            v
                                                                                                  unapply + on_expired_or_lost + init

init_reboot --REQUEST--> rebooting --ACK--> checking_ack (same probe as above)
                             |
                        (NAK, or retries exhausted)
                             v
                           init (normal DISCOVER cycle, remembered address dropped)
```

Notes not obvious from the diagram:

- **SELECTING retries forever** (capped exponential backoff, RFC 2131
  §4.1) - there is no fallback for "nobody answered a DISCOVER" on an
  embedded device with no other network to fall back to.
- **REQUESTING/REBOOTING give up after `DMDHCP_MAX_REQUEST_RETRANSMITS`**
  and fall back to a fresh DISCOVER cycle, silently (no `on_error` - a
  NAK or timeout here just means someone else got the address first,
  ordinary DHCP behavior, not a failure worth surfacing).
- **RENEWING/REBINDING never re-probe for a conflict** - dmdhcp already
  owns the address uninterrupted, so an ACK here is applied directly,
  inline on the rx thread (`dmnetif_set_*()`/`dmroute_add()` are pure
  bookkeeping, no wire I/O - see `dmnetif.h`).
- **`on_error` only fires from the DECLINE path**, after
  `DMDHCP_MAX_DECLINE_RETRIES` consecutive address conflicts
  (`-EADDRINUSE`) - the one case where retrying automatically stopped
  being the right thing to do.
- **`on_expired_or_lost` fires with the interface already de-configured**,
  for both a NAK during RENEWING/REBINDING and a full expiry with no ACK
  - dmdhcp always restarts DISCOVER on its own afterward.

## Known gaps

- IPv4 only - DHCPv6 is out of scope, matching `dmip`/`dmudp`/`dmicmp`'s
  own IPv6 send gap.
- Option overload (RFC 2132 §9.3, reusing the `sname`/`file` fields for
  more options) is not parsed - `dmdhcp_parse_message()` never inspects
  those fields.
- No `dmdhcp` server-side (`DHCPOFFER`/`DHCPACK` generation) - this module
  is client-only.
- DNS server storage is dmdhcp's own - no other module in this ecosystem
  claims ownership of resolver configuration, so `dmdhcp_get_dns_server()`
  is the only place that information is currently exposed.
