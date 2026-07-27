# dmdhcp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmdhcp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmdhcp/actions/workflows/ci.yml)

dmdhcp DMOD library module.

## Description

dmdhcp is a DHCP **client** (RFC 2131/2132) for the DMOD network stack. It
does not implement a DHCP server.

One `dmdhcp_lease_t` represents the whole lease lifecycle (DISCOVER/OFFER/
REQUEST/ACK, renewal, rebinding, release) for one interface. On acquiring
or renewing a lease, dmdhcp applies it to the interface itself
(address/netmask/broadcast plus a default route) and reverts that on
release/expiry - a caller does not need to configure the interface by
hand. Address-conflict detection (RFC 2131 §4.4.1, an ARP probe before
committing an address) is included.

dmdhcp keeps no thread of its own: inbound messages are processed inline
on whatever thread is already pumping the interface (via `dmudp_bind()`),
and time-driven behavior (retransmission backoff, the conflict probe,
lease timers) runs on `dmosi_timer_t` callbacks. See
[docs/dmdhcp.md](docs/dmdhcp.md) for the full architecture.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/`). Once built,
run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmdhcp-local.dmd -y
dmod_loader build/dmf/test_dmdhcp.dmf
```

## Usage

```c
#include "dmdhcp.h"

static void on_bound(dmdhcp_lease_t lease, bool is_renewal, void* user_data)
{
    dmdhcp_lease_info_t info;
    dmdhcp_get_lease_info(lease, &info);
    /* info.ip_address / .netmask / .gateway are already applied to the
     * interface at this point - nothing further to configure. */
}

static void on_lost(dmdhcp_lease_t lease, void* user_data)
{
    /* NAK'd or the lease fully expired - the interface has already been
     * de-configured. dmdhcp restarts DISCOVER on its own. */
}

dmdhcp_callbacks_t callbacks = { .on_bound = on_bound, .on_expired_or_lost = on_lost };
dmdhcp_lease_t lease = dmdhcp_start(iface, &callbacks, NULL, NULL);
```

## API

| Function | Description |
|----------|-------------|
| `dmdhcp_start()` | Begin acquiring a lease on an interface; keeps renewing/rebinding/restarting automatically. |
| `dmdhcp_set_callbacks()` | Replace a lease's callbacks/user_data. |
| `dmdhcp_renew()` | Force an immediate renewal attempt. |
| `dmdhcp_release()` | Send DHCPRELEASE, de-configure the interface, free the lease. |
| `dmdhcp_stop()` | De-configure and free the lease without sending DHCPRELEASE. |
| `dmdhcp_get_state()` / `_get_iface()` / `_get_user_data()` / `_get_xid()` | Simple accessors. |
| `dmdhcp_get_lease_info()` | Snapshot of the active lease's address/netmask/gateway/server/lease-time. |
| `dmdhcp_get_dns_server_count()` / `_get_dns_server()` | The DHCP-offered DNS server list. |
| `dmdhcp_build_message()` / `_parse_message()` | DHCP/BOOTP fixed header codec (RFC 2131 §2). |
| `dmdhcp_options_write()` / `_options_find()` / `_option_get_*()` | RFC 2132 TLV options codec. |

See [include/dmdhcp.h](include/dmdhcp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmdhcp`.
## Project Structure

```
dmdhcp/
├── docs/                      # Documentation (markdown format)
│   ├── README.md
│   ├── api-reference.md
│   └── dmdhcp.md              # Architecture: threading, locking, state machine
├── include/                   # Public headers
│   └── dmdhcp.h
├── src/
│   ├── dmdhcp_internal.h      # Private struct dmdhcp_lease, constants
│   ├── dmdhcp.c               # dmod_init()/_deinit(), _start()/_stop()/_release()/_renew()
│   ├── dmdhcp_registrations.c # DMOD_ENABLE_REGISTRATION (must stand alone)
│   ├── dmdhcp_lease_table.c   # Lease table CRUD, teardown, simple accessors
│   ├── dmdhcp_wire.c          # DHCP/BOOTP fixed header codec
│   ├── dmdhcp_options.c       # RFC 2132 TLV options codec
│   ├── dmdhcp_output.c        # Message senders + retransmit/probe timer
│   ├── dmdhcp_input.c         # dmudp_bind() registration + dispatch
│   └── dmdhcp_lifecycle.c     # Apply/unapply to interface + lease timer
├── tests/
│   ├── CMakeLists.txt
│   └── dmdhcp_test.c
├── CMakeLists.txt
├── Makefile
├── dmdhcp.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
