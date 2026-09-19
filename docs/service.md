# DHCP as a dmsystem service

## Why this exists

Like [dmicmp](https://github.com/choco-technologies/dmicmp), dmdhcp is a
**Library**-type DMOD module (see `CMakeLists.txt`) - it has no `main()`, so
nothing can spawn it as a process. [`configs/dhcp.ini`](../configs/dhcp.ini)
gives [dmsystem](https://github.com/choco-technologies/dmsystem)'s
`libsystemd` a way to load and enable it automatically at boot, via
`type=library`, exactly the way dmicmp's own
[`configs/icmp.ini`](https://github.com/choco-technologies/dmicmp/blob/main/configs/icmp.ini)
does - see dmsystem's
[configuration docs](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#typelibrary-services-backed-by-a-library-module-not-a-process)
for the full `type=library` mechanism.

**The important difference from dmicmp:** dmicmp answers pings as soon as
it's loaded and enabled - it registers itself with `dmip` unconditionally
in `dmod_init()`. dmdhcp's `dmod_init()` only sets up its lease table and
binds the DHCP client UDP port (see `src/dmdhcp.c`) - it does **not** start
acquiring a lease on any interface. That only happens once something calls
`dmdhcp_start(iface, ...)`, which needs to name a specific
`dmnetif_iface_t` - there is no "just turn DHCP on" call with no interface
to hand it, because a device may have several interfaces and not all of
them should get an address by DHCP (a statically-addressed uplink, a bridge
member, a loopback...).

So loading `dmdhcp` via `configs/dhcp.ini` is necessary but not sufficient
on its own - it makes `dmdhcp_start()` callable, the same way loading any
other Library module does, but something still has to call it per
interface. That's what [`tools/dhcpc`](../tools/dhcpc/README.md) is for.

## `exec=dmdhcp`, `type=library`

Same mechanism as dmicmp's `icmp.ini`: a unit with `type=library` has its
`exec` loaded and enabled (`Dmod_LoadModuleByName` + `Dmod_EnableModule`)
when started, and disabled and unloaded (`Dmod_DisableModule` +
`Dmod_UnloadModule`) when stopped - no separate launcher script needed.

```ini
# dhcp.ini
description=Load the dmdhcp module so DHCP leases can be acquired
exec=dmdhcp
type=library
```

## Starting a lease per interface: `tools/dhcpc`

`dmdhcp_start()` keeps a lease running with no thread of its own once
called - retransmission, renewal, rebinding and re-applying the interface
config on renewal/loss all happen on `dmdhcp`'s own `dmudp_bind()` callback
and `dmosi_timer_t`s (see [dmdhcp.md](dmdhcp.md)), not on whatever process
happened to call `dmdhcp_start()`. So the process that calls it only needs
to do so once and exit - that process is
[`tools/dhcpc`](../tools/dhcpc/README.md), a small Application-type module
that takes an interface name and calls `dmdhcp_start()` on it, the same
per-interface-instance shape
[dmnet's `networkd`](https://github.com/choco-technologies/dmnet/tree/main/services/networkd)
uses for its own RX pump - except `dhcpc` is `type=oneshot` (it is expected
to run to completion and exit, not keep running - see dmsystem's
[Service type](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#service-type)
docs).

`dhcpc` is **not** auto-instantiated from a `netif` device rule the way
`networkd@.ini` is: `dmnet`'s
[`networkd.rules`](https://github.com/choco-technologies/dmnet/blob/main/services/networkd/configs/networkd.rules)
already maps device class `netif` to `networkd@%name`, and `libsystemd`
only honors the first rules file that defines a given class (see
dmsystem's
[Device rules](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#device-rules)
docs) - a second `[class=netif]` rule here would either silently lose to
`networkd`'s (leaving no interface ever getting DHCP) or silently replace
it (leaving no interface ever getting its RX pump, breaking every other
protocol on that device), depending on load order. Since not every
interface should run a DHCP client in the first place, that trade-off
isn't one worth making - `dhcp@.ini` is a template instead, instantiated
explicitly per interface:

```ini
# dhcp@.ini
description=Acquire a DHCP lease for %i
exec=dhcpc
args=%i
type=oneshot
after=dhcp,networkd@%i
requires=dhcp,networkd@%i
```

`after=`/`requires=` on both `dhcp` (so the `dmdhcp` module is loaded before
`dhcpc` tries to call into it) and `networkd@%i` (so this interface's RX
pump - and therefore any DHCPOFFER/DHCPACK reply - is already running)
means a plain `libsystemd_scan()` orders this correctly, as long as both a
`networkd@<iface>.ini` and a `dhcp@<iface>.ini` instance exist for that
interface.

## Files

| File | Does |
|------|------|
| [`configs/dhcp.ini`](../configs/dhcp.ini) | The dmsystem unit that loads+enables `dmdhcp`: `exec=dmdhcp`, `type=library`. |
| [`tools/dhcpc/configs/dhcp@.ini`](../tools/dhcpc/configs/dhcp@.ini) | The per-interface template: `exec=dhcpc`, `args=%i`, `type=oneshot`. |

## Enabling at boot

Install `dmdhcp` with its `.dmr` (see the root [README.md](../README.md)),
then drop `dhcp.ini` and one `dhcp@<interface>.ini` instance per interface
that should use DHCP into `libsystemd`'s units directory, alongside that
interface's own `networkd@<interface>.ini` (see
[dmnet's networkd docs](https://github.com/choco-technologies/dmnet/tree/main/services/networkd#starting-at-boot)):

```bash
cp /opt/dmdhcp/configs/dhcp.ini /etc/dmsystem/units/dhcp.ini
cp /opt/dmdhcp/dhcpc/configs/dhcp@.ini /etc/dmsystem/units/dhcp@.ini
cp /opt/dmdhcp/dhcpc/configs/dhcp@.ini /etc/dmsystem/units/dhcp@eth0.ini
dmod_loader systemd.dmf --args "/etc/dmsystem/units"
```

`service status dhcp` reports whether the `dmdhcp` module itself is loaded;
`service status dhcp@eth0` reports whether the one-shot lease-acquisition
step for `eth0` has run - like any other `oneshot` unit it reports "not
running" again as soon as that step exits cleanly, so check the lease
itself (`dmdhcp_get_state()`) rather than `service status` to know whether
`eth0` actually has an address.
