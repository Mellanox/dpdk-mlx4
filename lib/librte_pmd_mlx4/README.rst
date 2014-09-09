.. Copyright (c) <2012-2013>, 6WIND
   All rights reserved.

.. title:: Mellanox ConnectX-3 DPDK poll-mode driver

About the Mellanox ConnectX-3 DPDK poll-mode driver
===================================================

DPDK PMD for Mellanox ConnectX-3 Ethernet adapters.

This driver is based on *libibverbs* and currently supports:

- Scattering/gathering RX/TX packets
- Multiple RX (with RSS/RCA) and TX queues
- Multiple MAC addresses
- VLAN filtering
- Link state information
- Software counters/statistics
- Start/stop/close operations
- Multiple physical ports host adapter
- DPDK 1.6.0 or above from `dpdk.org <http://www.dpdk.org/>`_.

Known limitations
-----------------

- Multiple RX VLAN filters can be configured, but only the first one works
  properly.
- RSS hash key and options cannot be modified.
- Hardware counters are not implemented.

Requirements
------------

Linux host with either a full installation of Mellanox OFED 2.1 (updated
kernel drivers and libraries), or only the following components:

- Mellanox OFA kernel drivers (*mlnx-ofa_kernel*)
- Generic verbs library and headers (*libibverbs*)
- Mellanox ConnectX InfiniBand library (*libmlx4*)

Note that the **mlx4_core.ko** kernel module must be loaded first and requires
the **log_num_mgm_entry_size=-1** parameter, otherwise the PMD won't
work.

*libibverbs* is assumed to be installed in a standard system location and
should be found without specific linker flags (such as *-L*). The same
applies to its headers location (*-I*).

Other requirements:

- A supported DPDK version
- An up-to-date GCC-based toolchain

Usage
=====

Provided all software components have been successfully installed and at least
one ConnectX adapter is present in the host system, *testpmd* can be used to
test it.

The following examples show a machine hosting two dual-port adapters (4 ports
total), linked together at 40Gbps by a crossover cable.

Run *testpmd* interactively from the DPDK build tree (for more information
about command-line options, see the corresponding documentation)::

 # ~/DPDK/build/app/testpmd -d ~/librte_pmd_mlx4-1.20/librte_pmd_mlx4.so -c 0x600 -n 3 -- -i
 EAL: Detected lcore 0 as core 0 on socket 0
 EAL: Detected lcore 1 as core 0 on socket 1
 [...]
 EAL: Detected lcore 23 as core 10 on socket 1
 EAL: Skip lcore 24 (not detected)
 [...]
 EAL: Skip lcore 63 (not detected)
 EAL: Setting up memory...
 EAL: Ask a virtual area of 0x1073741824 bytes
 EAL: Virtual area found at 0x7fb2e9800000 (size = 0x40000000)
 EAL: Ask a virtual area of 0x1069547520 bytes
 EAL: Virtual area found at 0x7fb2a9a00000 (size = 0x3fc00000)
 EAL: Ask a virtual area of 0x2097152 bytes
 EAL: Virtual area found at 0x7fb2a9600000 (size = 0x200000)
 EAL: Ask a virtual area of 0x2097152 bytes
 EAL: Virtual area found at 0x7fb2a9200000 (size = 0x200000)
 EAL: Requesting 512 pages of size 2MB from socket 0
 EAL: Requesting 512 pages of size 2MB from socket 1
 EAL: TSC frequency is ~3332637 KHz
 EAL: Master core 9 is ready (tid=2aadd820)
 EAL: Core 10 is ready (tid=a83a1700)
 EAL: PCI device 0000:02:00.0 on NUMA socket -1
 EAL:   probe driver: 15b3:1007 rte_mlx4_pmd
 EAL: PCI device 0000:02:00.0 on NUMA socket -1
 EAL:   probe driver: 15b3:1007 rte_mlx4_pmd
 EAL: PCI device 0000:02:00.0 on NUMA socket -1
 EAL:   probe driver: 15b3:1007 rte_mlx4_pmd
 EAL: PCI device 0000:07:00.0 on NUMA socket -1
 EAL:   probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: PCI device 0000:07:00.0 on NUMA socket -1
 EAL:   probe driver: 15b3:1003 rte_mlx4_pmd
 EAL: PCI device 0000:07:00.0 on NUMA socket -1
 EAL:   probe driver: 15b3:1003 rte_mlx4_pmd
 Interactive-mode selected
 Configuring Port 0 (socket -1)
 Port 0: 00:02:C9:B5:BA:B0
 Configuring Port 1 (socket -1)
 Port 1: 00:02:C9:B5:BA:B1
 Configuring Port 2 (socket -1)
 Port 2: 00:02:C9:F6:7D:C0
 Configuring Port 3 (socket -1)
 Port 3: 00:02:C9:F6:7D:C1
 Done
 testpmd>

The following commands are typed from the *testpmd* interactive prompt.

- Check ports status::

   testpmd> show port info all

   ********************* Infos for port 0  *********************
   MAC address: 00:02:C9:B5:BA:B0
   Connect to socket: -1
   memory allocation on the socket: 0
   Link status: down
   Link speed: 40000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   Maximum number of MAC addresses of hash filtering: 0
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 1  *********************
   MAC address: 00:02:C9:B5:BA:B1
   Connect to socket: -1
   memory allocation on the socket: 0
   Link status: down
   Link speed: 10000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   Maximum number of MAC addresses of hash filtering: 0
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 2  *********************
   MAC address: 00:02:C9:F6:7D:C0
   Connect to socket: -1
   memory allocation on the socket: 0
   Link status: down
   Link speed: 40000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   Maximum number of MAC addresses of hash filtering: 0
   VLAN offload:
     strip on
     filter on
     qinq(extend) off

   ********************* Infos for port 3  *********************
   MAC address: 00:02:C9:F6:7D:C1
   Connect to socket: -1
   memory allocation on the socket: 0
   Link status: down
   Link speed: 10000 Mbps
   Link duplex: full-duplex
   Promiscuous mode: enabled
   Allmulticast mode: disabled
   Maximum number of MAC addresses: 128
   Maximum number of MAC addresses of hash filtering: 0
   VLAN offload:
     strip on
     filter on
     qinq(extend) off
   testpmd>

- Start IO forwarding between ports 0 and 2. The *tx_first* argument tells
  *testpmd* to send a single packet burst which will be forwarded forever by
  both ports::

   testpmd> set fwd io
   Set io packet forwarding mode
   testpmd> set portlist 0,2
   previous number of forwarding ports 4 - changed to number of configured ports 2
   testpmd> start tx_first
     io packet forwarding - CRC stripping disabled - packets/burst=16
     nb forwarding cores=1 - nb forwarding ports=2
     RX queues=1 - RX desc=128 - RX free threshold=0
     RX threshold registers: pthresh=8 hthresh=8 wthresh=4
     TX queues=1 - TX desc=512 - TX free threshold=0
     TX threshold registers: pthresh=36 hthresh=0 wthresh=0
     TX RS bit threshold=0 - TXQ flags=0x0
   testpmd>

- Display *testpmd* ports statistics::

   testpmd> show port stats all

     ######################## NIC statistics for port 0  ########################
     RX-packets: 2347842    RX-missed: 0          RX-bytes: 150261888
     RX-badcrc:  0          RX-badlen: 0          RX-errors: 0
     RX-nombuf:  0
     TX-packets: 2907072    TX-errors: 0          TX-bytes: 186052608
     ############################################################################

     ######################## NIC statistics for port 1  ########################
     RX-packets: 0          RX-missed: 0          RX-bytes: 0
     RX-badcrc:  0          RX-badlen: 0          RX-errors: 0
     RX-nombuf:  0
     TX-packets: 0          TX-errors: 0          TX-bytes: 0
     ############################################################################

     ######################## NIC statistics for port 2  ########################
     RX-packets: 2907240    RX-missed: 0          RX-bytes: 186063360
     RX-badcrc:  0          RX-badlen: 0          RX-errors: 0
     RX-nombuf:  0
     TX-packets: 2347986    TX-errors: 0          TX-bytes: 150271104
     ############################################################################

     ######################## NIC statistics for port 3  ########################
     RX-packets: 0          RX-missed: 0          RX-bytes: 0
     RX-badcrc:  0          RX-badlen: 0          RX-errors: 0
     RX-nombuf:  0
     TX-packets: 0          TX-errors: 0          TX-bytes: 0
     ############################################################################
   testpmd>

- Stop forwarding::

   testpmd> stop
   Telling cores to stop...
   Waiting for lcores to finish...

     ---------------------- Forward statistics for port 0  ----------------------
     RX-packets: 103538490      RX-dropped: 0             RX-total: 103538490
     TX-packets: 128201016      TX-dropped: 0             TX-total: 128201016
     ----------------------------------------------------------------------------

     ---------------------- Forward statistics for port 2  ----------------------
     RX-packets: 128201000      RX-dropped: 0             RX-total: 128201000
     TX-packets: 103538506      TX-dropped: 0             TX-total: 103538506
     ----------------------------------------------------------------------------

     +++++++++++++++ Accumulated forward statistics for all ports+++++++++++++++
     RX-packets: 231739490      RX-dropped: 0             RX-total: 231739490
     TX-packets: 231739522      TX-dropped: 0             TX-total: 231739522
     ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

   Done.
   testpmd>

- Exit *testpmd*::

   testpmd> quit
   Stopping port 0...done
   Stopping port 1...done
   Stopping port 2...done
   Stopping port 3...done
   bye...
   root#

Compilation
===========

This driver is normally compiled independently as a shared object. The DPDK
source tree is only required for its headers, no patches required.

Once DPDK is compiled, *librte_pmd_mlx4* can be unpacked elsewhere and
compiled::

 # make clean
 rm -f librte_pmd_mlx4.so mlx4.o
 # make RTE_TARGET=x86_64-default-linuxapp-gcc RTE_SDK=~/DPDK
 gcc -I[...]/DPDK/x86_64-default-linuxapp-gcc/include -O3 -std=gnu99 -Wall -Wextra -fPIC -D_XOPEN_SOURCE=600 -g -DNDEBUG -UPEDANTIC   -c -o mlx4.o mlx4.c
 gcc -shared -libverbs -o librte_pmd_mlx4.so mlx4.o
 #

The following macros can be overridden on the command-line:

   RTE_SDK
      DPDK source tree location (mandatory).
   RTE_TARGET
      DPDK output directory for generated files (default: *build*).
   DEBUG
      If *1*, enable driver debugging.
   MLX4_PMD_SGE_WR_N
      Change the maximum number of scatter/gather elements per work
      request. The minimum value is 1, which disables support for segmented
      packets and jumbo frames with a size greater than a single segment for
      both TX and RX. Default: *4*).
