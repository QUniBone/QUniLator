/* ether_filter.hpp: the kernel receive filter of an emulated Ethernet controller

   Copyright (c) 2026, Hans Huebner
   MIT license, see ether_bridge.hpp for the full text.

   The classic BPF program a bridge socket carries, so the kernel queues only
   the frames the controller could want: every physical address it answers
   to, broadcast and multicast - or everything, when the controller is
   promiscuous. It is a function of the address set alone, which is what lets
   a host test run it in the kernel without a controller or an interface.
*/
#ifndef _ETHER_FILTER_HPP_
#define _ETHER_FILTER_HPP_

#include <stdint.h>
#include <vector>
#include <linux/filter.h>

// A physical address a controller answers to.
struct ether_address_t {
    uint8_t bytes[6];
};

// The program accepting multicast and broadcast, and unicast to any of
// addresses; with promiscuous set it accepts every frame.
std::vector<struct sock_filter> ether_receive_filter(
        const std::vector<ether_address_t> &addresses, bool promiscuous);

#endif
