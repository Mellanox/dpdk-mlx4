.. Copyright (c) <2012-2013>, 6WIND
   All rights reserved.

============================
Mellanox ConnectX-3 DPDK PMD
============================

.. contents::
.. sectnum::

``librte_pmd_mlx4``
===================

DPDK Poll-Mode Driver (PMD) for Mellanox ConnectX-3 Ethernet adapters.

This driver is based on ``libibverbs`` and currently supports:

- Scattering/gathering RX/TX packets.
- Multiple RX (with RSS/RCA) and TX queues.
- Multiple MAC addresses.
- VLAN filtering.
- Link state information.
- Software counters/statistics.
- Start/stop/close operations.
- Multiple physical ports host adapter (requires a DPDK patch).
- DPDK 1.2.2 (6WIND or Intel).
- DPDK 1.3.0 (6WIND or Intel).

Unsupported features:

- Promiscuous mode.
- Broadcast frames (only partially supported).
- VLAN filtering doesn't work (not interpreted in kernel driver).
- Hardware counters.
- RSS hash key and options cannot be modified.

Installation
============

Requirements
------------

A Linux host with either a full installation of Mellanox OFED (updated
kernel drivers and libraries), or only following components:

- Mellanox OFA kernel drivers (``mlnx-ofa_kernel``).
- Generic verbs library and headers (``libibverbs``).
- Mellanox ConnectX InfiniBand library (``libmlx4``).

Some of the above components in their current version must be patched for RSS
support and compiled in the following order:

- ``mlnx-ofa_kernel-2.0`` requires a patch for RSS support.
- ``libibverbs-1.1.6mlnx1`` requires a patch for RSS support.
- ``libmlx4-1.0.4mlnx1`` doesn't require any patch but must be recompiled
  after patching ``libibverbs``.
- Other ``libibverbs`` dependencies (not currently used by
  ``librte_pmd_mlx4``) may also need recompilation, but this is out of the
  scope of this document.

``libibverbs`` is assumed to be installed in a standard system location and
should be found without specific linker flags (such as ``-L``). The same
applies to its headers location (``-I``).

Other requirements:

- A supported Intel (or 6WIND-provided) DPDK version.
- An up-to-date GCC-based toolchain.

Compilation (internal)
----------------------

In this mode, ``librte_pmd_mlx4`` is compiled at the same time as the DPDK
and internally linked with it.

A few Makefiles and source files in the DPDK must be patched first in order
to include the new driver. This patch is provided separately.

Other patches (also provided separately for DPDK 1.2.2 and DPDK 1.3.0) may be
necessary:

- One that fixes compilation warnings/errors when debugging is enabled.
- Another that enables the DPDK to manage more than a single physical port
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
 # cd lib
 # tar xzf ~/librte_pmd_mlx4-1.9.tar.gz
 # ln -s librte_pmd_mlx4-1.9 librte_pmd_mlx4
 # ls -ld librte_pmd_*
 drwxr-xr-x. 3 root root 4096 Dec 17 12:09 librte_pmd_e1000
 drwxr-xr-x. 3 root root 4096 Dec 17 12:09 librte_pmd_ixgbe
 lrwxrwxrwx. 1 root root   19 Apr 15 15:49 librte_pmd_mlx4 -> librte_pmd_mlx4-1.9
 drwxr-xr-x. 2 root root 4096 Apr 15 15:42 librte_pmd_mlx4-1.9

After this, the DPDK is ready to be configured/compiled and installed. Please
refer to its installation procedure. The configuration templates include
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

Compilation (external)
----------------------

In this mode, ``librte_pmd_mlx4`` is compiled independently as a shared
object. The DPDK source tree is only required for its headers.

**Note: this mode is only supported by 6WIND's DPDK.**

Once the DPDK is compiled, ``librte_pmd_mlx4`` can be unpacked elsewhere and
compiled::

 # tar xzf librte_pmd_mlx4-1.9.tar.gz
 # cd librte_pmd_mlx4-1.9
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

Quick testing
=============

Provided all software components have been successfully installed and at least
one ConnectX adapter is present in the host system, ``testpmd`` can be used to
test it.

Depending on how ``libpmd_rte_mlx4`` is compiled, the extra option ``-d
librte_pmd_mlx4.so`` may have to be passed to the DPDK if it's a shared
object.

These examples assume a dual port adapter with both ports linked to another
similar host.

Run ``testpmd`` interactively from the DPDK build tree (for more information
about its command-line options, please refer to its documentation)::

 # ~/DPDK/build/app/testpmd -c 0x6 -n 3 -- -i # internal
 # # or:
 # ~/DPDK/build/app/testpmd -d ~/librte_pmd_mlx4-1.9/librte_pmd_mlx4.so -c 0x6 -n 3 -- -i # external
 EAL: coremask set to 6
 EAL: Error reading numa node link for lcore 1 - using physical package id instead
 EAL: Detected lcore 1 as core 1 on socket 0
 EAL: Error reading numa node link for lcore 2 - using physical package id instead
 EAL: Detected lcore 2 as core 2 on socket 0
 EAL: Setting up hugepage memory...
 EAL: Ask a virtual area of 0xc00000 bytes
 EAL: Virtual area found at 0x7fb438e00000 (size = 0xc00000)
 [...]
 EAL: Requesting 1024 pages of size 2MB from socket 0
 EAL: Increasing open file limit
 EAL: WARNING: Cannot mmap /dev/hpet! The TSC will be used instead.
 EAL: Master core 1 is ready (tid=f7d38800)
 EAL: Core 2 is ready (tid=745f2700)
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 Interactive-mode selected
 Configuring Port 0
 Configuring Port 1
 Checking link statuses...
 Port 0 Link Up - speed 40000 Mbps - full-duplex
 Port 1 Link Up - speed 40000 Mbps - full-duplex
 Done
 testpmd>

The following commands are typed from the ``testpmd`` interactive prompt.

- Check port status with both ports connected::

   testpmd> show port info all

   ********************* Infos for port 0  *********************
   MAC address: 00:02:C9:F6:7D:70
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

   ********************* Infos for port 1  *********************
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

- Check port status after disconnecting one of them::

   testpmd> show port info all

   ********************* Infos for port 0  *********************
   MAC address: 00:02:C9:F6:7D:70
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

   ********************* Infos for port 1  *********************
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

- Plug it back and start basic forwarding between the two ports::

   testpmd> start
     io packet forwarding - CRC stripping disabled - packets/burst=16
     nb forwarding cores=1 - nb forwarding ports=2
     RX queues=1 - RX desc=128 - RX free threshold=0
     RX threshold registers: pthresh=8 hthresh=8 wthresh=4
     TX queues=1 - TX desc=512 - TX free threshold=0
     TX threshold registers: pthresh=36 hthresh=0 wthresh=0
     TX RS bit threshold=0 - TXQ flags=0x0
   testpmd>

- On the other host (under Linux), enable both interfaces, run ``tcpdump`` on
  one of them and send a ping through the other one::

   other# ifconfig eth4 up
   other# ifconfig eth5 up
   other# arp -s -i eth4 1.2.3.4 00:02:C9:F6:7D:71
   other# tpcdump -nvei eth5 &
   [1] 27404
   tcpdump: WARNING: eth5: no IPv4 address assigned
   tcpdump: listening on eth5, link-type EN10MB (Ethernet), capture size 65535 bytes
   other# ping -c1 -I eth4 1.2.3.4
   PING 1.2.3.4 (1.2.3.4) from 10.16.0.173 eth4: 56(84) bytes of data.
   17:42:06.611598 00:02:c9:f6:7d:31 > 00:02:c9:f6:7d:71, ethertype IPv4 (0x0800), length 98: (tos 0x0, ttl 64, id 0, offset 0, flags [DF], proto ICMP (1), length 84)
       10.16.0.173 > 1.2.3.4: ICMP echo request, id 17003, seq 1, length 64

   ^C
   --- 1.2.3.4 ping statistics ---
   1 packets transmitted, 0 received, 100% packet loss, time 2510ms

  The packet goes through unchanged.

- Display ports statistics::

   testpmd> show port stats all

     ######################## NIC statistics for port 0  ########################
     RX-packets: 0          RX-errors: 0         RX-bytes: 0
     TX-packets: 1          TX-errors: 0         TX-bytes: 98
     ############################################################################

     ######################## NIC statistics for port 1  ########################
     RX-packets: 1          RX-errors: 0         RX-bytes: 98
     TX-packets: 0          TX-errors: 0         TX-bytes: 0
     ############################################################################
   testpmd>

- Exit ``testpmd``::

   testpmd> quit
   Stopping port 0...done
   Stopping port 1...done
   bye...
   #
