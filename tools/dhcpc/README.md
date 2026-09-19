# dhcpc

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](../../LICENSE)

dhcpc DMOD application module.

## Description

dhcpc starts a [dmdhcp](../../README.md) lease on one network interface and
exits - `dmdhcp_start()` does the rest (retransmission, renewal, rebinding,
applying the lease to the interface) with no thread of its own to keep
running afterward. One instance per interface, the same shape
[dmnet's `networkd`](https://github.com/choco-technologies/dmnet/tree/main/services/networkd)
uses for its own per-interface unit - and, like `networkd`, auto-started for
every interface `dmnetif` registers via a device rule
([`configs/dhcp.rules`](configs/dhcp.rules)) reacting to the same `netif`
class `networkd.rules` does. See [`../../docs/service.md`](../../docs/service.md)
for the full setup.

dhcpc links `dmdhcp_if` directly, so loading it - by any means - loads and
enables `dmdhcp` itself as its own module dependency. There is no separate
"load dmdhcp" unit anywhere in this repo.

## Building

Built automatically alongside `dmdhcp` (see the root
[CMakeLists.txt](../../CMakeLists.txt)) - no separate build step.

## Usage

dhcpc takes the interface to acquire a lease on as its only argument, and
exits as soon as `dmdhcp_start()` has been called - it does not wait for the
lease itself:

```bash
dmod_loader /path/to/dhcpc.dmf eth0
```

### Starting at boot

Nothing needs to run dhcpc by hand. Install [`configs/dhcp@.ini`](configs/dhcp@.ini)
into the directory scanned by `libsystemd_scan()` and
[`configs/dhcp.rules`](configs/dhcp.rules) into the one passed to
`libsystemd_load_rules()` (see
[dmsystem's configuration.md](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#device-rules)
for both formats), alongside dmnet's own `networkd@.ini`/`networkd.rules`:

```bash
cp /opt/dmdhcp/dhcpc/configs/dhcp@.ini /etc/dmsystem/units/dhcp@.ini
cp /opt/dmdhcp/dhcpc/configs/dhcp.rules /etc/dmsystem/rules/dhcp.rules
dmod_loader systemd.dmf --args "/etc/dmsystem/units /etc/dmsystem/rules"
```

From then on every interface `dmnetif` registers starts its own
`dhcp@<interface>` (and `networkd@<interface>`, from dmnet's own rule -
both fire from the same device event, see `../../docs/service.md`).

Note that `dhcp@.ini` is a *template*: on its own it starts nothing, so an
installation that omits `dhcp.rules` gets no automatic DHCP at all.
Conversely `service start dhcp@eth0` works without the rules file, since
`libsystemd` instantiates a template on demand.

## Project Structure

```
tools/dhcpc/
├── configs/
│   ├── dhcp@.ini      # libsystemd unit template (starts dhcpc at boot)
│   └── dhcp.rules     # device rule - one dhcp@<iface> instance per interface
├── src/
│   └── dhcpc.c
├── tests/
│   ├── CMakeLists.txt
│   └── dhcpc_test.c
├── CMakeLists.txt
├── README.md
└── dhcpc.dmr
```

## Author

Patryk Kubiak

## License

MIT
