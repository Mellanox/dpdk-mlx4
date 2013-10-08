.. Copyright (c) <2012-2013>, 6WIND
   All rights reserved.

.. title:: Mellanox ConnectX-3 DPDK poll mode driver

``librte_pmd_mlx4``
===================

DPDK poll mode driver (PMD) for Mellanox ConnectX-3 Ethernet adapters.

This driver is based on *libibverbs* and currently supports:

- Scattering/gathering RX/TX packets
- Multiple RX (with RSS/RCA) and TX queues
- Multiple MAC addresses
- VLAN filtering
- Link state information
- Software counters/statistics
- Start/stop/close operations
- Multiple physical ports host adapter (requires a DPDK patch)
- DPDK 1.2.2 (6WIND or Intel)
- DPDK 1.3.0 (6WIND or Intel)

Known limitations
-----------------

- Multiple RX VLAN filters can be configured, but only the first one works
  properly.
- RSS hash key and options cannot be modified.
- Hardware counters are not implemented.

Installation
============

Requirements
------------

Linux host with either a full installation of Mellanox OFED 2.0 (updated
kernel drivers and libraries), or only the following components:

- Mellanox OFA kernel drivers (``mlnx-ofa_kernel``)
- Generic verbs library and headers (``libibverbs``)
- Mellanox ConnectX InfiniBand library (``libmlx4``)

Some of the above components in their current version must be patched for RSS
support and compiled in the following order:

- ``mlnx-ofa_kernel-2.0`` requires a patch for RSS support.
- ``libibverbs-1.1.6mlnx1`` requires a patch for RSS support.
- ``libmlx4-1.0.4mlnx1`` can be patched for better performance and must be
  recompiled anyway after patching ``libibverbs`` for RSS support.
- Other ``libibverbs`` dependencies (not currently used by
  ``librte_pmd_mlx4``) may also need recompilation, but this is out of the
  scope of this document.

``libibverbs`` is assumed to be installed in a standard system location and
should be found without specific linker flags (such as ``-L``). The same
applies to its headers location (``-I``).

Other requirements:

- A supported Intel (or 6WIND-provided) DPDK version
- An up-to-date GCC-based toolchain

Compilation (internal)
----------------------

In this mode, ``librte_pmd_mlx4`` is compiled at the same time as the DPDK
and internally linked with it.

A few Makefiles and source files in the DPDK must be patched first
to include the new driver. This patch is provided separately.

Other patches (also provided separately for DPDK 1.2.2 and DPDK 1.3.0) may be
necessary:

- a patch to fix compilation warnings/errors when debugging is enabled,
- a patch to allow the DPDK to manage more than one single physical port
  per adapter (the DPDK normally expects one PCI bus address per port).

The driver itself must be unpacked in the ``lib/`` subdirectory, alongside
IGB and IXGBE drivers (``librte_pmd_igb`` and ``librte_pmd_ixgbe``).

::

 # unzip 516836_DPDK.L.1.3.0_183.zip
 Archive:  516836_DPDK.L.1.3.0_183.zip
    creating: DPDK/
   inflating: DPDK/LICENSE.GPL
   inflating: DPDK/LICENSE.LGPL
   inflating: DPDK/Makefile
    creating: DPDK/app/
   inflating: DPDK/app/Makefile
 [...]
 # cd DPDK
 # patch -p2 < ~/0001-librte_pmd_mlx4-implement-driver-support.patch
 [...]
 # patch -p2 < ~/0002-lib-fix-non-C99-macros-definitions-in-exported-heade.patch
 [...]
 # patch -p2 < ~/0003-pci-allow-drivers-to-be-bound-several-times-to-the-s.patch
 [...]
 # patch -p2 < ~/0004-pci-fix-probing-blacklisted-device-with-RTE_PCI_DRV_.patch
 [...]
 # cd lib
 # tar xzf ~/librte_pmd_mlx4-1.10.tar.gz
 # ln -s librte_pmd_mlx4-1.10 librte_pmd_mlx4
 # ls -ld librte_pmd_*
 drwxr-xr-x 3 root root 4096 Dec 17 12:09 librte_pmd_e1000
 drwxr-xr-x 3 root root 4096 Dec 17 12:09 librte_pmd_ixgbe
 lrwxrwxrwx 1 root root   20 May 27 13:49 librte_pmd_mlx4 -> librte_pmd_mlx4-1.10
 drwxrwxr-x 2 root root 4096 May 23 11:48 librte_pmd_mlx4-1.10

The DPDK is now ready to be configured/compiled and installed. For more information, see the corresponding installation procedure. The configuration templates include
``librte_pmd_mlx4`` by default.

Configuration/compilation example::

 # cd DPDK
 # make config T=x86_64-default-linuxapp-gcc
 Configuration done
 # make
 [...]
 == Build lib/librte_pmd_mlx4
   CC mlx4.o
   AR librte_pmd_mlx4.a
   INSTALL-LIB librte_pmd_mlx4.a
 [...]
 Build complete

The following macros can be overridden in the configuration file or on the
command-line:

- ``CONFIG_RTE_LIBRTE_MLX4_DEBUG``: if ``y``, enable driver debugging.
- ``CONFIG_RTE_LIBRTE_MLX4_SGE_WR_N`` (default: ``4``): change the maximum
  number of scatter/gather elements per work request. The minimum value is
  1, which disables support for segmented packets and jumbo frames with a
  size greater than a single segment for both TX and RX.

Compilation (external)
----------------------

In this mode, ``librte_pmd_mlx4`` is compiled independently as a shared
object. The DPDK source tree is only required for its headers.

.. note::

   This mode is only supported by 6WIND DPDK.

Once DPDK is compiled, ``librte_pmd_mlx4`` can be unpacked elsewhere and
compiled::

 # tar xzf librte_pmd_mlx4-1.10.tar.gz
 # cd librte_pmd_mlx4-1.10
 # make clean
 rm -f librte_pmd_mlx4.so mlx4.o
 # make RTE_SDK=~/DPDK DPDK_6WIND=1
 warning: RTE_TARGET is not set.
 gcc -I/root/DPDK/build/include -O3 -std=gnu99 -Wall -Wextra -fPIC -D_XOPEN_SOURCE=600 -DNDEBUG -UPEDANTIC   -c -o mlx4.o mlx4.c
 gcc -shared -libverbs -o librte_pmd_mlx4.so mlx4.o
 #

The following macros can be overridden on the command-line:

- ``RTE_SDK`` (mandatory): DPDK source tree location.
- ``RTE_TARGET`` (default: ``build``): DPDK output directory for generated
  files.
- ``DEBUG``: if ``1``, enable driver debugging.
- ``DPDK_6WIND``: if ``1``, enable 6WIND DPDK extensions.
- ``MLX4_PMD_SGE_WR_N`` (default: ``4``): change the maximum number of
  scatter/gather elements per work request. The minimum value is 1, which
  disables support for segmented packets and jumbo frames with a size
  greater than a single segment for both TX and RX.

Testing
=======

Provided all software components have been successfully installed and at least
one ConnectX adapter is present in the host system, ``testpmd`` can be used to
test it.

If ``libpmd_rte_mlx4`` is compiled externally as a shared object, the extra
option ``-d librte_pmd_mlx4.so`` is necessary.

The following examples assume a machine configured with two dual-port
adapters (4 ports total), on which the second ports are connected to each
other using a crossover cable (40Gbps speed).

Run ``testpmd`` interactively from the DPDK build tree (for more information
about command-line options, see the corresponding documentation)::

 # ~/DPDK/build/app/testpmd -c 0x600 -n 4 -- -i # internal
 # # or:
 # ~/DPDK/build/app/testpmd -d ~/librte_pmd_mlx4-1.10/librte_pmd_mlx4.so -c 0x600 -n 4 -- -i # external
 EAL: coremask set to 600
 EAL: Detected lcore 9 as core 1 on socket 1
 EAL: Detected lcore 10 as core 2 on socket 1
 EAL: Setting up hugepage memory...
 EAL: Ask a virtual area of 0x76400000 bytes
 EAL: Virtual area found at 0x2aaa34600000 (size = 0x76400000)
 [...]
 EAL: Ask a virtual area of 0x200000 bytes
 EAL: Virtual area found at 0x7f48d8400000 (size = 0x200000)
 EAL: Requesting 1024 pages of size 2MB from socket 0
 EAL: Requesting 1024 pages of size 2MB from socket 1
 EAL: Increasing open file limit
 EAL: Master core 9 is ready (tid=6519b840)
 EAL: Core 10 is ready (tid=d73e6700)
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 Interactive-mode selected
 Configuring Port 0
 Configuring Port 1
 Configuring Port 2
 Configuring Port 3
 Checking link statuses...
 Port 0 Link Up - speed 10000 Mbps - full-duplex
 Port 1 Link Up - speed 40000 Mbps - full-duplex
 Port 2 Link Up - speed 10000 Mbps - full-duplex
 Port 3 Link Up - speed 40000 Mbps - full-duplex
 Done
 testpmd>

As previously described:

- DPDK port 0 is adapter 1 port 1, connected to another host at 10Gbps.
- DPDK port 1 is adapter 1 port 2, connected to DPDK port 3 at 40Gbps.
- DPDK port 2 is adapter 2 port 1, connected to another host at 10Gbps.
- DPDK port 3 is adapter 2 port 2, connected to DPDK port 1 at 40Gbps.

The following commands are typed from the ``testpmd`` interactive prompt.

- Check ports status::

   testpmd> show port info all

   ********************* Infos for port 0  *********************
   MAC address: 00:02:C9:F6:7D:30
   Link status: up
   Link speed: 10000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 1  *********************
   MAC address: 00:02:C9:F6:7D:31
   Link status: up
   Link speed: 40000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 2  *********************
   MAC address: 00:02:C9:F6:7D:70
   Link status: up
   Link speed: 10000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 3  *********************
   MAC address: 00:02:C9:F6:7D:71
   Link status: up
   Link speed: 40000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   VLAN offload:
     strip on
     filter on
     qinq(extend) off
   testpmd>

- Check ports status after disconnecting DPDK port 3 by manually removing
  its QSFP adapter::

   testpmd> show port info all

   ********************* Infos for port 0  *********************
   MAC address: 00:02:C9:F6:7D:30
   Link status: up
   Link speed: 10000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 1  *********************
   MAC address: 00:02:C9:F6:7D:31
   Link status: down
   Link speed: 40000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 2  *********************
   MAC address: 00:02:C9:F6:7D:70
   Link status: up
   Link speed: 10000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 3  *********************
   MAC address: 00:02:C9:F6:7D:71
   Link status: down
   Link speed: 10000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   VLAN offload:
     strip on
     filter on
     qinq(extend) off
   testpmd>

  DPDK port 1, which still has its QSFP adapter, shows a 40Gbps link speed
  with status "down", while DPDK port 3 only shows a 10Gbps link speed due
  to the missing QSFP adapter. DPDK ports 0 and 2 are obviously unaffected
  by this.

- Plug it back and start MAC forwarding between ports 1 and 3::

   testpmd> set fwd mac
   Set mac packet forwarding mode
   testpmd> set portlist 1,3
   previous number of forwarding ports 4 - changed to number of configured ports 2
   testpmd> start
     mac packet forwarding - CRC stripping disabled - packets/burst=16
     nb forwarding cores=1 - nb forwarding ports=2
     RX queues=1 - RX desc=128 - RX free threshold=0
     RX threshold registers: pthresh=8 hthresh=8 wthresh=4
     TX queues=1 - TX desc=512 - TX free threshold=0
     TX threshold registers: pthresh=36 hthresh=0 wthresh=0
     TX RS bit threshold=0 - TXQ flags=0x0
   testpmd>

- In the following examples, ``eth18`` and ``eth19`` are equivalent to DPDK
  ports 1 and 3, respectively. Commands are entered from another terminal
  while ``testpmd`` is still running::

   root# ifconfig eth18
   eth18     Link encap:Ethernet  HWaddr 00:02:c9:f6:7d:31
             inet6 addr: fe80::2:c900:1f6:7d31/64 Scope:Link
             UP BROADCAST RUNNING MULTICAST  MTU:8000  Metric:1
             RX packets:0 errors:0 dropped:0 overruns:0 frame:0
             TX packets:19 errors:0 dropped:0 overruns:0 carrier:0
             collisions:0 txqueuelen:1000
             RX bytes:0 (0.0 B)  TX bytes:24195 (23.6 KiB)

   root# ifconfig eth19
   eth19     Link encap:Ethernet  HWaddr 00:02:c9:f6:7d:71
             inet6 addr: fe80::2:c900:1f6:7d71/64 Scope:Link
             UP BROADCAST RUNNING MULTICAST  MTU:8000  Metric:1
             RX packets:0 errors:0 dropped:0 overruns:0 frame:0
             TX packets:6 errors:0 dropped:0 overruns:0 carrier:0
             collisions:0 txqueuelen:1000
             RX bytes:0 (0.0 B)  TX bytes:468 (468.0 B)

- Generate a single packet on ``eth18``::

   root# arp -s -i eth18 1.2.3.4 00:02:c9:f6:7d:71 # eth19's MAC address
   root# ping -I eth18 -c1 1.2.3.4
   PING 1.2.3.4 (1.2.3.4) from 10.16.0.116 eth18: 56(84) bytes of data.
   ^C
   --- 1.2.3.4 ping statistics ---
   1 packets transmitted, 0 received, 100% packet loss, time 0ms

- Display ``testpmd`` ports statistics::

   testpmd> show port stats all

     ######################## NIC statistics for port 0  ########################
     RX-packets: 0          RX-errors: 0         RX-bytes: 0
     TX-packets: 0          TX-errors: 0         TX-bytes: 0
     ############################################################################

     ######################## NIC statistics for port 1  ########################
     RX-packets: 0          RX-errors: 0         RX-bytes: 0
     TX-packets: 27202696   TX-errors: 0         TX-bytes: 2665864208
     ############################################################################

     ######################## NIC statistics for port 2  ########################
     RX-packets: 0          RX-errors: 0         RX-bytes: 0
     TX-packets: 0          TX-errors: 0         TX-bytes: 0
     ############################################################################

     ######################## NIC statistics for port 3  ########################
     RX-packets: 27202759   RX-errors: 0         RX-bytes: 2665870382
     TX-packets: 0          TX-errors: 0         TX-bytes: 0
     ############################################################################
   testpmd>

  The ping packet is being forwarded by ``testpmd`` between both ports
  through the crossover cable in a loop.

- Use ``tcpdump`` to dump this packet on ``eth19``::

   root# tcpdump -veni eth19 -c5
   tcpdump: WARNING: eth19: no IPv4 address assigned
   tcpdump: listening on eth19, link-type EN10MB (Ethernet), capture size 65535 bytes
   17:10:10.767264 00:02:c9:f6:7d:31 > 02:00:00:00:00:00, ethertype IPv4 (0x0800), length 98: (tos 0x0, ttl 64, id 0, offset 0, flags [DF], proto ICMP (1), length 84) 10.16.0.116 > 1.2.3.4: ICMP echo request, id 14217, seq 1, length 64
   17:10:10.767266 00:02:c9:f6:7d:31 > 02:00:00:00:00:00, ethertype IPv4 (0x0800), length 98: (tos 0x0, ttl 64, id 0, offset 0, flags [DF], proto ICMP (1), length 84) 10.16.0.116 > 1.2.3.4: ICMP echo request, id 14217, seq 1, length 64
   17:10:10.767266 00:02:c9:f6:7d:31 > 02:00:00:00:00:00, ethertype IPv4 (0x0800), length 98: (tos 0x0, ttl 64, id 0, offset 0, flags [DF], proto ICMP (1), length 84) 10.16.0.116 > 1.2.3.4: ICMP echo request, id 14217, seq 1, length 64
   17:10:10.767267 00:02:c9:f6:7d:31 > 02:00:00:00:00:00, ethertype IPv4 (0x0800), length 98: (tos 0x0, ttl 64, id 0, offset 0, flags [DF], proto ICMP (1), length 84) 10.16.0.116 > 1.2.3.4: ICMP echo request, id 14217, seq 1, length 64
   17:10:10.767268 00:02:c9:f6:7d:31 > 02:00:00:00:00:00, ethertype IPv4 (0x0800), length 98: (tos 0x0, ttl 64, id 0, offset 0, flags [DF], proto ICMP (1), length 84) 10.16.0.116 > 1.2.3.4: ICMP echo request, id 14217, seq 1, length 64
   5 packets captured
   442 packets received by filter
   406 packets dropped by kernel

- Stop forwarding and display ports statistics::

   testpmd> stop
   Telling cores to stop...
   Waiting for lcores to finish...

     ---------------------- Forward statistics for port 1  ----------------------
     RX-packets: 0              RX-dropped: 0             RX-total: 0
     TX-packets: 33029196       TX-dropped: 0             TX-total: 33029196
     ----------------------------------------------------------------------------

     ---------------------- Forward statistics for port 3  ----------------------
     RX-packets: 33029196       RX-dropped: 0             RX-total: 33029196
     TX-packets: 0              TX-dropped: 0             TX-total: 0
     ----------------------------------------------------------------------------

     +++++++++++++++ Accumulated forward statistics for all ports+++++++++++++++
     RX-packets: 33029196       RX-dropped: 0             RX-total: 33029196
     TX-packets: 33029196       TX-dropped: 0             TX-total: 33029196
     ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

   Done.
   testpmd> show port stats all

     ######################## NIC statistics for port 0  ########################
     RX-packets: 0          RX-errors: 0         RX-bytes: 0
     TX-packets: 0          TX-errors: 0         TX-bytes: 0
     ############################################################################

     ######################## NIC statistics for port 1  ########################
     RX-packets: 0          RX-errors: 0         RX-bytes: 0
     TX-packets: 33029196   TX-errors: 0         TX-bytes: 3236861208
     ############################################################################

     ######################## NIC statistics for port 2  ########################
     RX-packets: 0          RX-errors: 0         RX-bytes: 0
     TX-packets: 0          TX-errors: 0         TX-bytes: 0
     ############################################################################

     ######################## NIC statistics for port 3  ########################
     RX-packets: 33029196   RX-errors: 0         RX-bytes: 3236861208
     TX-packets: 0          TX-errors: 0         TX-bytes: 0
     ############################################################################
   testpmd>

- Exit ``testpmd``::

   testpmd> quit
   Stopping port 0...done
   Stopping port 1...done
   Stopping port 2...done
   Stopping port 3...done
   bye...
   root#
