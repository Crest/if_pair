# if_pair(4) -- A fast point-to-point IP link between vnets

This FreeBSD kernel module implements a simple point-to-point IP link
between a vnet jail and its host, or between two vnet jails - like
`if_epair(4)`, but without the complexity and overhead of emulating Ethernet.

> **Status**: Under active development.
> It has not yet seen wide testing. Try it on a test machine first.

## The two options for jail networking: bridged or routed

Previously the canonical way to connect vnet jails was either
`if_epair(4)` as a member of `if_bridge(4)` or SR-IOV VFs.
Both of these configurations bridge at the Ethernet layer whereas
`if_pair(4)` connects the vnet jails at the IP layer.

In a bridged deployment jails become nodes on the physical Ethernet network
the host is connected to. In a traditional Ethernet network each additional
jail increases the broadcast and multicast traffic
with its ARP, NDP, and DHCP traffic.

While it's already possible to use `if_epair(4)` without a bridge, its Ethernet
emulation overhead remains in a routed configuration.
Each `if_epair(4)` interface behaves like a single-queue network interface
served by a single kernel thread. All packets forwarded over an
`if_epair(4)` interface are processed by that single kernel thread,
limiting the achievable bandwidth per interface
to the packet rate a single CPU core can process times the configured MTU,
which defaults to 1500 bytes like a real Ethernet interface.

In a routed deployment using `if_pair(4)` the jails are connected to the host
(or each other) by point-to-point IP links with multi-hop connectivity
provided by routing instead of bridging.

In the simplest case the jail host is the default gateway for all jails
and the jail addresses are allocated from a prefix routed to the host.
In more complex setups each FreeBSD system may host multiple vnet jails
acting as routers for other jails, e.g. one router per tenant connected
to a single router that enforces tenant isolation.

If an encrypted overlay between jail hosts is needed in a routed setup,
WireGuard or IPsec can be used directly with neither the complexity nor
the additional per-packet overhead of Ethernet encapsulation protocols
like VXLAN or GENEVE.

## Quick start

You need FreeBSD 15.0 or newer, the kernel sources installed at
`/usr/src`, and root access to load the kernel module and manage jails.

Build and load the module:

```sh
make
kldload ./if_pair.ko
```

Create a pair and a vnet jail, and give the jail one side:

```sh
ifconfig pair0 create                    # creates both pair0a and pair0b
jail -c name=demo vnet persist           # create the demo jail
ifconfig pair0b vnet demo                # move the b side into the jail
ifconfig -j demo lo0 inet 127.0.0.1/8 up # bring up the jail's loopback
```

Configure the addresses on the host and inside the jail.
On a point-to-point interface both the local and remote addresses
have to be configured.

```sh
ifconfig pair0a inet 192.0.2.1/32 192.0.2.2 up         # host -> jail
ifconfig -j demo pair0b inet 192.0.2.2/32 192.0.2.1 up # jail -> host
```

That's it. The jail should now respond to pings.

```sh
ping -c 3 192.0.2.2            # host -> jail
jexec demo ping -c 3 192.0.2.1 # jail -> host
```

IPv6 works the same way:

```sh
ifconfig pair0a inet6 2001:db8::1/128 2001:db8::2 up         # host -> jail
ifconfig -j demo pair0b inet6 2001:db8::2/128 2001:db8::1 up # jail -> host
```

IPv6 ping:

```sh
ping6 -c 3 2001:db8::2            # host -> jail
jexec demo ping6 -c 3 2001:db8::1 # jail -> host
```

To give the jail a way out to the world, point its default route at
the host and let the host forward (and NAT) as needed.

```sh
jexec demo route add default 192.0.2.1
sysctl net.inet.ip.forwarding=1
# anything beyond this point depends on your network
```

Watch it work - `tcpdump` runs on either end, and shows clean IP
packets with no Ethernet clutter:

```sh
tcpdump -n -i pair0a
```

Remove the jail before the interface pair.
Destroying either side removes both ends of an `if_pair(4)`.

```sh
jail -r demo
ifconfig pair0a destroy
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
- To load the module at boot either add `if_pair_load="YES"` to
  `/boot/loader.conf` or run `sysrc kld_list+=if_pair` to
  have the rc.d scripts load it a bit later.
