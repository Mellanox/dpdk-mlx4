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
- Multiple RX and TX queues.
- Multiple MAC addresses.
- VLAN filtering.
- Link state information.
- Software counters/statistics.
- Start/stop/close operations.
- Increasing the number of RX/TX queues at runtime.
- Multiple physical ports host adapter (requires a DPDK patch).
    
Unsupported features:

- Locking for live reconfiguration operations on RX/TX queues.
- Promiscuous mode.
- Broadcast frames (only partially supported).
- VLAN filtering doesn't work (not interpreted in kernel driver).
- RSS/RCA isn't implemented.
- Hardware counters.

It can be compiled either internally or externally to the DPDK.

Installation
============

Requirements
------------

A Linux host with either a full installation of Mellanox OFED (updated
kernel drivers and libraries), or only following components:

- Mellanox OFA kernel drivers (``mlnx-ofa_kernel-1.8.6``).
- Generic verbs library and headers (``libibverbs-1.1.6``).
- Mellanox ConnectX InfiniBand library (``libmlx4-1.0.4``).
- Userspace driver for Mellanox ConnectX InfiniBand (``libmverbs-0.1.0``).

``libibverbs`` is assumed to be installed in a standard system location and
can be found without specific linker flags (such as ``-L``). The same
applies to its headers location (``-I``).

Other requirements:

- Intel (or 6WIND-provided) DPDK version 1.2.2 source tree (other versions
  are currently not supported).
- An up-to-date GCC-based toolchain.

Compilation (internal)
----------------------

In this mode, ``librte_pmd_mlx4`` is compiled at the same time as the DPDK
and internally linked with it.

A few Makefiles and source files in the DPDK must be patched first in order
to include the new driver. This patch is provided separately.

Other patches (also provided separately) can be necessary:

- One that fixes compilation warnings/errors when enabling debugging.
- Another that enables the DPDK to manage more than a single physical port
  per adapter (the DPDK normally expects one PCI bus address per port).

The driver itself must be unpacked in the ``lib/`` subdirectory, alongside
IGB and IXGBE drivers (``librte_pmd_igb`` and ``librte_pmd_ixgbe``).

::

 # unzip 504977_DPDK.L.1.2.2_2.zip 
 Archive:  504977_DPDK.L.1.2.2_2.zip
   creating: DPDK/
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
 # tar xzf ~/librte_pmd_mlx4-1.0.tar.gz
 # ln -s librte_pmd_mlx4-1.0 librte-pmd_mlx4
 # ls -ld librte_pmd_*
 drwxr-xr-x. 1 foo users 4096 Jun 14  2012 librte_pmd_igb
 drwxr-xr-x. 1 foo users 4096 Jun 14  2012 librte_pmd_ixgbe
 lrwxrwxrwx. 1 foo users   19 Jan 23 15:24 librte_pmd_mlx4 -> librte_pmd_mlx4-1.0
 drwxr-xr-x. 1 foo users 4096 Jan 20 17:10 librte_pmd_mlx4-1.0

After this, the DPDK is ready to be configured/compiled and installed. Please
refer to its installation procedure. The default configuration templates
include ``librte_pmd_mlx4`` by default.

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

**While compiling like this is possible with Intel's DPDK, only 6WIND's
version is currently able to load and use the resulting library.**

As with internal compilation, the DPDK may require the following patches:

- One that fixes compilation warnings/errors when enabling debugging.
- Another that enables the DPDK to manage more than a single physical port
  per adapter (the DPDK normally expects one PCI bus address per port).

See previous section for how to apply them and configure/compile the DPDK.

Once the DPDK is compiled, ``librte_pmd_mlx4`` can be unpacked elsewhere and
compiled::

 # tar xzf librte_pmd_mlx4-1.0.tar.gz
 # cd librte_pmd_mlx4-1.0
 # make clean
 rm -f librte_pmd_mlx4.so mlx4.o
 # make RTE_SDK=~/DPDK
 warning: RTE_TARGET is not set.
 gcc -I/root/incoming/1.2.2/DPDK/build/include -O3 -std=gnu99 -Wall -Wextra -fPIC -D_XOPEN_SOURCE=600 -DNDEBUG -UPEDANTIC   -c -o mlx4.o mlx4.c
 gcc -shared -libverbs -o librte_pmd_mlx4.so mlx4.o

The following macros can be overridden on the command-line:

- ``RTE_SDK`` (mandatory): DPDK source tree location.
- ``RTE_TARGET`` (default: ``build``): DPDK output directory for generated
  files.
- ``DEBUG``: if ``1``, enable driver debugging.
- ``IBVERBS``: source tree location of a compiled ``libibverbs`` (if not
  installed system-wide).
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

 # ~/DPDK/build/app/testpmd -c 0x6 -n 1 -- -i # internal
 # # or:
 # ~/DPDK/build-app/testpmd -d ~/librte_pmd_mlx4-1.0/librte_pmd_mlx4.so -c 0x6 -n 1 -- -i # external
 EAL: coremask set to 6
 EAL: Detected lcore 0 on socket 0
 EAL: Detected lcore 1 on socket 0
 EAL: Detected lcore 2 on socket 0
 EAL: Detected lcore 3 on socket 0
 EAL: Detected lcore 4 on socket 0
 EAL: Detected lcore 5 on socket 0
 EAL: Detected lcore 6 on socket 0
 EAL: Detected lcore 7 on socket 0
 EAL: WARNING: Cannot mmap /dev/hpet! The TSC will be used instead.
 EAL: Master core 1 is ready (tid=66c12800)
 EAL: Core 2 is ready (tid=6220c700)
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: probe driver: 15b3:1003 rte_mlx4_pmd
 Interactive-mode selected
 Initializing port 0... done:  Link Up - speed 40000 Mbps - full-duplex
 Initializing port 1... done:  Link Up - speed 40000 Mbps - full-duplex
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

   ********************* Infos for port 1  *********************
   MAC address: 00:02:C9:F6:7D:71
   Link status: up
   Link speed: 40000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
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

   ********************* Infos for port 1  *********************
   MAC address: 00:02:C9:F6:7D:71
   Link status: up
   Link speed: 40000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   testpmd>

- Plug it back and start basic forwarding between the two ports::

   testpmd> start
     io packet forwarding - CRC stripping disabled - packets/burst=16
     nb forwarding cores=1 - nb forwarding ports=2
     RX queues=1 - RX desc=128 - RX free threshold=0
     RX threshold registers: pthresh=8 hthresh=8 wthresh=4
     TX queues=1 - TX desc=512 - TX free threshold=0
     TX threshold registers: pthresh=36 hthresh=0 wthresh=0
     TX RS bit threshold=0
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
