# dmdhcp API Reference

See [include/dmdhcp.h](../include/dmdhcp.h) for the authoritative
declarations and doc comments - this page is a navigable summary. See
[dmdhcp.md](dmdhcp.md) for the architecture (threading, locking, the RFC
2131 state machine).

## Types

| Type | Description |
|------|-------------|
| `dmdhcp_lease_t` | Opaque handle to one lease's lifecycle on one interface. |
| `dmdhcp_state_t` | RFC 2131 §4.4 client state, plus dmdhcp's own conflict-probe/decline-backoff states. |
| `dmdhcp_callbacks_t` | `on_bound` / `on_expired_or_lost` / `on_error`. |
| `dmdhcp_options_t` | Optional hostname + INIT-REBOOT remembered address, passed to `dmdhcp_start()`. |
| `dmdhcp_lease_info_t` | Snapshot of the active lease's address/netmask/gateway/server/lease-time. |
| `dmdhcp_message_t` | Parsed DHCP/BOOTP fixed header fields (RFC 2131 §2). |
| `dmdhcp_option_t` | One parsed RFC 2132 TLV option. |
| `dmdhcp_msg_type_t` | DHCPDISCOVER..DHCPINFORM (option 53 values). |

## Functions

### Lease lifecycle

| Function | Description |
|----------|-------------|
| `dmdhcp_start()` | Begin acquiring a lease on an interface. |
| `dmdhcp_set_callbacks()` | Replace a lease's callbacks/user_data. |
| `dmdhcp_renew()` | Force `dmdhcp_state_bound` -> `_renewing` immediately. |
| `dmdhcp_release()` | Send DHCPRELEASE, de-configure the interface, free the lease. |
| `dmdhcp_stop()` | De-configure and free the lease without sending DHCPRELEASE. |

### Accessors

| Function | Description |
|----------|-------------|
| `dmdhcp_get_state()` | Current `dmdhcp_state_t`. |
| `dmdhcp_get_iface()` | The interface this lease was started on. |
| `dmdhcp_get_user_data()` | The `user_data` most recently set. |
| `dmdhcp_get_xid()` | Current transaction ID (diagnostics/testing). |
| `dmdhcp_get_lease_info()` | Snapshot of address/netmask/gateway/server/lease-time - `-EINVAL` unless BOUND/RENEWING/REBINDING. |
| `dmdhcp_get_dns_server_count()` / `dmdhcp_get_dns_server()` | The DHCP-offered DNS server list (option 6) - dmdhcp is the only owner of DNS configuration storage in this ecosystem. |

### Wire codec (usable independently of a lease)

| Function | Description |
|----------|-------------|
| `dmdhcp_build_message()` / `dmdhcp_parse_message()` | Fixed BOOTP/DHCP header + magic cookie. |
| `dmdhcp_options_write()` / `dmdhcp_options_find()` | RFC 2132 TLV options. |
| `dmdhcp_option_get_u32()` / `_get_addr()` / `_addr_count()` / `_get_addr_at()` | Decode a found option's payload. |

## Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `DMDHCP_SERVER_PORT` | 67 | |
| `DMDHCP_CLIENT_PORT` | 68 | |
| `DMDHCP_MAGIC_COOKIE` | `0x63825363` | RFC 2132 §9.1 |
| `DMDHCP_OPT_*` | - | Option codes dmdhcp reads/writes (subnet mask, router, DNS server, hostname, requested IP, lease time, msg type, server ID, T1, T2, end). |
