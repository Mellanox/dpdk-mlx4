.. Copyright (c) <2012-2014>, 6WIND
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

Linux host with either a full installation of Mellanox OFED 2.3 (updated
kernel drivers and libraries), or only the following components:

- Mellanox OFA kernel drivers (*mlnx-ofa_kernel*)
- Generic verbs library and headers (*libibverbs*)
- Mellanox ConnectX InfiniBand library (*libmlx4*)

Note that the **mlx4_core.ko** kernel module must be loaded first and requires
the **log_num_mgm_entry_size=-1** parameter (now enabled by default),
otherwise the PMD won't work properly.

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

Make sure that kernel modules **mlx4_core**, **mlx4_en**, **mlx4_ib** and
**ib_uverbs** are loaded::

 root# modprobe -a mlx4_core mlx4_en mlx4_ib ib_uverbs

And that all interfaces are up::

 root# for i in `echo /sys/class/net/*/device/infiniband_verbs/uverbs* | \
  sed -rn 's,/([^/]+/){3}([^/]+)[^ ]*,\2,g;p'`; do echo ip link set $i up; done

Run *testpmd* interactively from the DPDK build tree (for more information
about command-line options, see the corresponding documentation)::

 root# ~/DPDK/build/app/test-pmd/testpmd -c 0xf000f000 -n 4 -d ./librte_pmd_mlx4.so -- -i
 EAL: Detected lcore 0 as core 0 on socket 0
 EAL: Detected lcore 1 as core 1 on socket 0
 EAL: Detected lcore 2 as core 2 on socket 0
 EAL: Detected lcore 3 as core 3 on socket 0
 EAL: Detected lcore 4 as core 4 on socket 0
 [...]
 EAL: Detected lcore 27 as core 3 on socket 1
 EAL: Detected lcore 28 as core 4 on socket 1
 EAL: Detected lcore 29 as core 5 on socket 1
 EAL: Detected lcore 30 as core 6 on socket 1
 EAL: Detected lcore 31 as core 7 on socket 1
 EAL: Support maximum 64 logical core(s) by configuration.
 EAL: Detected 32 lcore(s)
 EAL:   cannot open VFIO container, error 2 (No such file or directory)
 EAL: VFIO support could not be initialized
 EAL: Setting up memory...
 EAL: Ask a virtual area of 0x6400000 bytes
 EAL: Virtual area found at 0x7f15fd600000 (size = 0x6400000)
 EAL: Ask a virtual area of 0x200000 bytes
 [...]
 EAL: PCI device 0000:83:00.0 on NUMA socket 1
 EAL:   probe driver: 15b3:1007 librte_pmd_mlx4
 PMD: librte_pmd_mlx4: PCI information matches, using device "mlx4_0" (VF: false)
 PMD: librte_pmd_mlx4: 2 port(s) detected
 PMD: librte_pmd_mlx4: bad state for port 1: "down" (1)
 PMD: librte_pmd_mlx4: port 1 MAC address is 00:02:c9:b5:b7:50
 PMD: librte_pmd_mlx4: bad state for port 2: "down" (1)
 PMD: librte_pmd_mlx4: port 2 MAC address is 00:02:c9:b5:b7:51
 EAL: PCI device 0000:84:00.0 on NUMA socket 1
 EAL:   probe driver: 15b3:1007 librte_pmd_mlx4
 PMD: librte_pmd_mlx4: PCI information matches, using device "mlx4_1" (VF: false)
 PMD: librte_pmd_mlx4: 2 port(s) detected
 PMD: librte_pmd_mlx4: bad state for port 1: "down" (1)
 PMD: librte_pmd_mlx4: port 1 MAC address is 00:02:c9:b5:ba:b0
 PMD: librte_pmd_mlx4: bad state for port 2: "down" (1)
 PMD: librte_pmd_mlx4: port 2 MAC address is 00:02:c9:b5:ba:b1
 Interactive-mode selected
 Configuring Port 0 (socket 0)
 PMD: librte_pmd_mlx4: 0x7f35e0: TX queues number update: 0 -> 1
 PMD: librte_pmd_mlx4: 0x7f35e0: RX queues number update: 0 -> 1
 Port 0: 00:02:C9:B5:B7:50
 Configuring Port 1 (socket 0)
 PMD: librte_pmd_mlx4: 0x7f3620: TX queues number update: 0 -> 1
 PMD: librte_pmd_mlx4: 0x7f3620: RX queues number update: 0 -> 1
 Port 1: 00:02:C9:B5:B7:51
 Configuring Port 2 (socket 0)
 PMD: librte_pmd_mlx4: 0x7f3660: TX queues number update: 0 -> 1
 PMD: librte_pmd_mlx4: 0x7f3660: RX queues number update: 0 -> 1
 Port 2: 00:02:C9:B5:BA:B0
 Configuring Port 3 (socket 0)
 PMD: librte_pmd_mlx4: 0x7f36a0: TX queues number update: 0 -> 1
 PMD: librte_pmd_mlx4: 0x7f36a0: RX queues number update: 0 -> 1
 Port 3: 00:02:C9:B5:BA:B1
 Checking link statuses...
 Port 0 Link Up - speed 10000 Mbps - full-duplex
 Port 1 Link Up - speed 40000 Mbps - full-duplex
 Port 2 Link Up - speed 10000 Mbps - full-duplex
 Port 3 Link Up - speed 40000 Mbps - full-duplex
 Done
 testpmd>

The following commands are typed from the *testpmd* interactive prompt.

- Check ports status::

   testpmd> show port info all

   ********************* Infos for port 0  *********************
   MAC address: 00:02:C9:B5:B7:50
   Connect to socket: 0
   memory allocation on the socket: 0
   Link status: up
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

   ********************* Infos for port 1  *********************
   MAC address: 00:02:C9:B5:B7:51
   Connect to socket: 0
   memory allocation on the socket: 0
   Link status: up
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

   ********************* Infos for port 2  *********************
   MAC address: 00:02:C9:B5:BA:B0
   Connect to socket: 0
   memory allocation on the socket: 0
   Link status: up
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

   ********************* Infos for port 3  *********************
   MAC address: 00:02:C9:B5:BA:B1
   Connect to socket: 0
   memory allocation on the socket: 0
   Link status: up
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
   testpmd>

- Start IO forwarding between ports 1 and 3. The *tx_first* argument tells
  *testpmd* to send a single packet burst which will be forwarded forever by
  both ports::

   testpmd> set fwd io
   Set io packet forwarding mode
   testpmd> set portlist 1,3
   previous number of forwarding ports 4 - changed to number of configured ports 2
   testpmd> start tx_first
     io packet forwarding - CRC stripping disabled - packets/burst=32
     nb forwarding cores=1 - nb forwarding ports=2
     RX queues=1 - RX desc=128 - RX free threshold=0
     RX threshold registers: pthresh=8 hthresh=8 wthresh=0
     TX queues=1 - TX desc=512 - TX free threshold=0
     TX threshold registers: pthresh=32 hthresh=0 wthresh=0
     TX RS bit threshold=0 - TXQ flags=0x0
   testpmd>

- Display *testpmd* ports statistics::

   testpmd> show port stats all

     ######################## NIC statistics for port 0  ########################
     RX-packets: 0          RX-missed: 0          RX-bytes:  0
     RX-badcrc:  0          RX-badlen: 0          RX-errors: 0
     RX-nombuf:  0
     TX-packets: 0          TX-errors: 0          TX-bytes:  0
     ############################################################################

     ######################## NIC statistics for port 1  ########################
     RX-packets: 60800584   RX-missed: 0          RX-bytes:  3891239534
     RX-badcrc:  0          RX-badlen: 0          RX-errors: 0
     RX-nombuf:  0
     TX-packets: 61146609   TX-errors: 0          TX-bytes:  3913382976
     ############################################################################

     ######################## NIC statistics for port 2  ########################
     RX-packets: 0          RX-missed: 0          RX-bytes:  0
     RX-badcrc:  0          RX-badlen: 0          RX-errors: 0
     RX-nombuf:  0
     TX-packets: 0          TX-errors: 0          TX-bytes:  0
     ############################################################################

     ######################## NIC statistics for port 3  ########################
     RX-packets: 61146920   RX-missed: 0          RX-bytes:  3913402990
     RX-badcrc:  0          RX-badlen: 0          RX-errors: 0
     RX-nombuf:  0
     TX-packets: 60800953   TX-errors: 0          TX-bytes:  3891262080
     ############################################################################
   testpmd>

- Stop forwarding::

   testpmd> stop
   Telling cores to stop...
   Waiting for lcores to finish...

     ---------------------- Forward statistics for port 1  ----------------------
     RX-packets: 78238689       RX-dropped: 0             RX-total: 78238689
     TX-packets: 78681769       TX-dropped: 0             TX-total: 78681769
     ----------------------------------------------------------------------------

     ---------------------- Forward statistics for port 3  ----------------------
     RX-packets: 78681737       RX-dropped: 0             RX-total: 78681737
     TX-packets: 78238721       TX-dropped: 0             TX-total: 78238721
     ----------------------------------------------------------------------------

     +++++++++++++++ Accumulated forward statistics for all ports+++++++++++++++
     RX-packets: 156920426      RX-dropped: 0             RX-total: 156920426
     TX-packets: 156920490      TX-dropped: 0             TX-total: 156920490
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

 root# make clean
 rm -f librte_pmd_mlx4.so mlx4.o config.h
 root# make RTE_TARGET=x86_64-default-linuxapp-gcc RTE_SDK=~/DPDK
 rm -f config.h
 Looking for IBV_EXP_DEVICE_UD_RSS enum in infiniband/verbs.h. Defining RSS_SUPPORT.
 Looking for struct ibv_send_wr_raw type in infiniband/verbs.h. Defining SEND_RAW_WR_SUPPORT.
 Looking for struct rte_pktmbuf type in rte_mbuf.h. Defining HAVE_STRUCT_RTE_PKTMBUF.
 Looking for mtu_get_t type in rte_ethdev.h. Not defining HAVE_MTU_GET.
 Looking for mtu_set_t type in rte_ethdev.h. Defining HAVE_MTU_SET.
 Looking for struct rte_eth_fc_conf.autoneg field in rte_ethdev.h. Defining HAVE_FC_CONF_AUTONEG.
 Looking for struct eth_dev_ops.flow_ctrl_get field in rte_ethdev.h. Defining HAVE_FLOW_CTRL_GET.
 gcc -I[...]/DPDK/x86_64-default-linuxapp-gcc/include -I. -O3 -std=gnu99 -Wall -Wextra -fPIC -D_XOPEN_SOURCE=600 -g -DNDEBUG -UPEDANTIC   -c -o mlx4.o mlx4.c
 gcc -shared -o librte_pmd_mlx4.so mlx4.o -libverbs
 root#

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
