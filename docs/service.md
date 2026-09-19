# DHCP as a dmsystem service

## Why this exists

Like [dmicmp](https://github.com/choco-technologies/dmicmp), dmdhcp is a
**Library**-type DMOD module (see `CMakeLists.txt`) - it has no `main()`, so
nothing can spawn it as a process, and something still needs to load and
enable it before `dmdhcp_start()` is callable at all.

Unlike dmicmp, though, dmdhcp answers nothing on its own the moment it's
loaded - its `dmod_init()` only sets up its lease table and binds the DHCP
client UDP port (see `src/dmdhcp.c`); it does **not** start acquiring a
lease on any interface. That only happens once something calls
`dmdhcp_start(iface, ...)`, which needs to name a specific
`dmnetif_iface_t` - there is no "just turn DHCP on" call with no interface
to hand it, because a device may have several interfaces and not all of
them should get an address by DHCP (a statically-addressed uplink, a bridge
member, a loopback...).

[`tools/dhcpc`](../tools/dhcpc/README.md) is that "something": a small
Application-type module that takes one interface name and calls
`dmdhcp_start()` on it. It links `dmdhcp_if` directly (see
[`tools/dhcpc/CMakeLists.txt`](../tools/dhcpc/CMakeLists.txt)), so loading
`dhcpc` - by any means - loads and enables `dmdhcp` as its own module
dependency automatically, the same way `tools/ping` in dmicmp pulls in
`dmicmp` itself with no separate unit needed for it. **There is no
`dhcp.ini` unit in this repo** - unlike a Library module with no
Application front-end of its own (like dmicmp), dmdhcp already has one in
`dhcpc`, so a bare "load dmdhcp" unit would just be a redundant second way
to reach the same load+enable dmdhcp already gets as a side effect of
`dhcpc` starting.

## Starting a lease per interface: `tools/dhcpc`

`dmdhcp_start()` keeps a lease running with no thread of its own once
called - retransmission, renewal, rebinding and re-applying the interface
config on renewal/loss all happen on `dmdhcp`'s own `dmudp_bind()` callback
and `dmosi_timer_t`s (see [dmdhcp.md](dmdhcp.md)), not on whatever process
happened to call `dmdhcp_start()`. So the process that calls it only needs
to do so once and exit - `dhcpc` is `type=oneshot` (it is expected to run
to completion and exit, not keep running - see dmsystem's
[Service type](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#service-type)
docs).

`dhcpc` needs one instance per interface, the same per-interface-instance
shape [dmnet's `networkd`](https://github.com/choco-technologies/dmnet/tree/main/services/networkd)
uses for its own RX pump:

```ini
; dhcp@.ini
description=Acquire a DHCP lease for %i
exec=dhcpc
args=%i
type=oneshot
```

## Auto-starting per interface: `dhcp.rules`

`dmnetif` reports every interface it registers as a `netif`-class device -
that's what [dmnet's `networkd.rules`](https://github.com/choco-technologies/dmnet/blob/main/services/networkd/configs/networkd.rules)
already reacts to for the RX pump, via `libsystemd_notify_device_added()`.
[`tools/dhcpc/configs/dhcp.rules`](../tools/dhcpc/configs/dhcp.rules) reacts
to the exact same event, independently:

```ini
; dhcp.rules
[class=netif]
start=dhcp@%name
```

Both `networkd.rules` and `dhcp.rules` can declare their own `[class=netif]`
section and both fire from one device-added event - `libsystemd` runs
*every* rule matching a class, not just whichever rules file was scanned
first (see dmsystem's
[Device rules](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#device-rules)
docs). So install both files in the same rules directory, and every
interface `dmnetif` reports gets both its RX pump (`networkd@<iface>`) and
a DHCP lease attempt (`dhcp@<iface>`) with no static per-interface file to
maintain.

There is no ordering dependency between the two: `dhcpc` only needs to call
`dmdhcp_start()`, which arms its own retransmission timer and keeps retrying
DISCOVER on its own - it doesn't need `networkd@<iface>`'s RX pump to
already be running, only for it to show up *eventually* so an OFFER/ACK can
be received.

If an interface should not get DHCP (a statically-addressed uplink, a
bridge member, a loopback...), don't install `dhcp.rules` for it - there is
currently no per-interface opt-out below the granularity of "this rules
directory reacts to `netif` events" (a future addition, not something
`dmdhcp` implements today).

## Files

| File | Does |
|------|------|
| [`tools/dhcpc/configs/dhcp@.ini`](../tools/dhcpc/configs/dhcp@.ini) | The per-interface template: `exec=dhcpc`, `args=%i`, `type=oneshot`. |
| [`tools/dhcpc/configs/dhcp.rules`](../tools/dhcpc/configs/dhcp.rules) | `[class=netif] start=dhcp@%name` - instantiates the template above for every interface `dmnetif` reports. |

## Enabling at boot

Install `dmdhcp` with its `.dmr` (see the root [README.md](../README.md)),
then drop `dhcp@.ini` into `libsystemd`'s units directory and `dhcp.rules`
into its rules directory, alongside
[dmnet's own `networkd@.ini`/`networkd.rules`](https://github.com/choco-technologies/dmnet/tree/main/services/networkd#starting-at-boot):

```bash
cp /opt/dmdhcp/dhcpc/configs/dhcp@.ini /etc/dmsystem/units/dhcp@.ini
cp /opt/dmdhcp/dhcpc/configs/dhcp.rules /etc/dmsystem/rules/dhcp.rules
dmod_loader systemd.dmf --args "/etc/dmsystem/units /etc/dmsystem/rules"
```

From then on every interface `dmnetif` registers - at boot or later -
starts its own `networkd@<interface>` and `dhcp@<interface>`, no static
per-interface file needed for either.

`service status dhcp@eth0` reports whether the one-shot lease-acquisition
step for `eth0` has run - like any other `oneshot` unit it reports "not
running" again as soon as that step exits cleanly, so check the lease
itself (`dmdhcp_get_state()`) rather than `service status` to know whether
`eth0` actually has an address.
