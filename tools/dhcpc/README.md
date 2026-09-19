# dhcpc

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](../../LICENSE)

dhcpc DMOD application module.

## Description

dhcpc starts a [dmdhcp](../../README.md) lease on one network interface and
exits - `dmdhcp_start()` does the rest (retransmission, renewal, rebinding,
applying the lease to the interface) with no thread of its own to keep
running afterward. One instance per interface, the same shape
[dmnet's `networkd`](https://github.com/choco-technologies/dmnet/tree/main/services/networkd)
uses for its own per-interface unit.

Unlike `networkd`, which pumps every interface `dmnetif` registers, not
every interface should run a DHCP client (a statically-addressed uplink, a
bridge member, a loopback...), so dhcpc is **not** wired to `dmnetif`'s
`netif` device class the way `networkd@.ini` is. Instead, drop a
`dhcp@<interface>.ini` instance file into `libsystemd`'s units directory
for each interface that should acquire an address by DHCP - see
[`configs/dhcp@.ini`](configs/dhcp@.ini) and
[`../../docs/service.md`](../../docs/service.md) for the full setup.

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

Nothing needs to run dhcpc by hand once its unit is in place. Install
[`../../configs/dhcp.ini`](../../configs/dhcp.ini) so the `dmdhcp` module
itself is loaded, then [`configs/dhcp@.ini`](configs/dhcp@.ini) as
`dhcp@<interface>.ini` for each interface that should use DHCP (an empty
file inherits everything from the template, the same way `getty@tty1.ini`
does for `getty@.ini` - see
[dmsystem's configuration.md](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#templates)):

```bash
cp /opt/dmdhcp/configs/dhcp.ini /etc/dmsystem/units/dhcp.ini
cp /opt/dmdhcp/dhcpc/configs/dhcp@.ini /etc/dmsystem/units/dhcp@.ini
cp /opt/dmdhcp/dhcpc/configs/dhcp@.ini /etc/dmsystem/units/dhcp@eth0.ini
dmod_loader systemd.dmf --args "/etc/dmsystem/units"
```

## Project Structure

```
tools/dhcpc/
├── configs/
│   └── dhcp@.ini      # libsystemd unit template - one instance per interface
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
