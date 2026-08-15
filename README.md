# if_pair

A FreeBSD kernel module that gives you a simple point-to-point IP link
between a vnet jail and its host, or between two vnet jails - like
`epair(4)`, but without pretending to be an Ethernet cable.

> **Status**: early development. It builds cleanly on FreeBSD 15.0,
> but has not yet seen wide testing. Try it on a test machine first.

## The trade: a faster link, if the host becomes a router

The usual way to network a vnet jail is **bridged**: create an
`epair(4)`, put one end in the jail, and add the other to an
`if_bridge(4)` alongside the host's real NIC. The jail then behaves
like one more machine on your LAN - it can use DHCP, and LAN
neighbors can talk to it directly. The price is paid on every packet:
it gets dressed up as an Ethernet frame, address resolution (ARP or
IPv6 neighbor discovery) has to run, and the frame crosses the bridge
- all to move data between two interfaces inside the same kernel.

`if_pair` is the faster alternative, and the deal is simple: **you
let the host do what a router does**. Jails no longer sit on your
LAN; each one hangs off its own tiny point-to-point link to the host,
and the host forwards IP packets between those links and the real
network. Because such a link only ever carries IP between two known
endpoints, if_pair can strip away everything else:

- No Ethernet headers, no MAC addresses, no ARP or neighbor
  discovery - an address on each end and a route are the whole story.
- Checksums for traffic between the two ends are skipped entirely
  (the packets never leave the machine, so nothing can corrupt them).
- A big default MTU (16384), because there is no Ethernet on the
  "wire" to limit packet size.

## Trading the bridged network for a routed one

If your jails are bridged today, here is what changes, piece by
piece:

| Bridged (epair + if_bridge) | Routed (if_pair) |
|---|---|
| Jail has an address on the LAN | Jail has its own point-to-point address pair with the host - two host addresses (`/32`, `/128`), no subnet needed |
| Jail gets its address from LAN DHCP | Static addresses on the pair; the jail's default route points at the host |
| LAN neighbors reach the jail via the bridge | The host forwards: `sysctl net.inet.ip.forwarding=1`, plus **either** NAT on the host (private jail addresses) **or** a route for the jail's addresses via the host, added on your upstream router |
| Broadcast and discovery protocols (DHCP, mDNS, SSDP) reach the jail | They don't cross a router - that's inherent to routing, not a quirk of if_pair |

**Stay bridged** when you can't take that deal: the jail must appear
as a first-class citizen of the Ethernet segment, get its address
from an external DHCP server, be found by discovery protocols, or
speak anything that is not IP. Routed and bridged jails also coexist
fine on one host - use each where it fits.

## Quick start

You need FreeBSD 15.0 or newer, the kernel sources installed at
`/usr/src`, and root.

Build and load the module:

```sh
make
kldload ./if_pair.ko
```

Create a pair and a vnet jail, and give the jail one end:

```sh
ifconfig pair create                 # creates pair0a and pair0b
jail -c name=demo vnet persist
ifconfig pair0b vnet demo            # move the b side into the jail
```

Address both ends. On a point-to-point interface each side gets its
own address plus the peer's address as the destination - no shared
prefix needed, a host mask (`/32`) is fine:

```sh
ifconfig pair0a inet 192.0.2.1/32 192.0.2.2
jexec demo ifconfig pair0b inet 192.0.2.2/32 192.0.2.1 up
```

That's it - no MACs, no ARP, nothing else to configure:

```sh
ping -c 3 192.0.2.2                  # host -> jail
jexec demo ping -c 3 192.0.2.1       # jail -> host
```

IPv6 works the same way:

```sh
ifconfig pair0a inet6 2001:db8::1/128 2001:db8::2
jexec demo ifconfig pair0b inet6 2001:db8::2/128 2001:db8::1
jexec demo ping -6 -c 3 2001:db8::1
```

To give the jail a way out to the world, point its default route at
the host and let the host forward (and NAT, if the jail's addresses
are private):

```sh
jexec demo route add default 192.0.2.1
sysctl net.inet.ip.forwarding=1
# ...plus your usual pf or ipfw NAT rule on the host's uplink.
```

Watch it work - `tcpdump` runs on either end, and shows clean IP
packets with no Ethernet clutter:

```sh
tcpdump -n -i pair0a
```

Cleanup is one command - destroying either side removes both ends:

```sh
ifconfig pair0a destroy
jail -r demo
```

## Where to go from here

- **if_pair(4)** - the manual page (`man ./if_pair.4` before
  installing): all interface behavior, and an MTU CONFIGURATION
  section worth reading if your jail's traffic continues out through
  a normal 1500-byte network link.
- **NOTES.md** - the developer documentation: how it works inside,
  design decisions, and one known interaction between checksum
  offloading and in-kernel NAT (`ipfw nat`/`ng_nat`) with its
  one-line workaround. natd(8) and pf NAT are unaffected.
- To load the module at boot, add `if_pair_load="YES"` to
  `/boot/loader.conf`.
