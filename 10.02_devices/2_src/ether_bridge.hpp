/* ether_bridge.hpp: host Ethernet attachment for the emulated controllers

   Copyright (c) 2026, Hans Huebner

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   A raw AF_PACKET socket in promiscuous mode, shared with the host's own
   stack on the same interface: the emulated controller's station address is
   distinct from the host MAC, so both answer on one wire.

   The socket carries a kernel packet filter for the station address,
   broadcast and multicast. Promiscuous mode is what makes the station
   address visible at all, but it also delivers unicast for every other host
   on the LAN; on a busy segment that fills the receive buffer and the kernel
   drops the frames the emulation is waiting for. The controller still runs
   its own address filter over what arrives - the kernel filter only keeps
   the socket drainable on this single-core board.
*/
#ifndef _ETHER_BRIDGE_HPP_
#define _ETHER_BRIDGE_HPP_

#include <stdint.h>
#include <string>

#include "logsource.hpp"

// The station address an emulated Ethernet controller powers on with: DEC's
// OUI (08:00:2b) with the low three bytes taken from the board's own Ethernet
// address, so every board gets a distinct one. It is generated once and kept
// under QUNILATOR_DIR in a file named for the controller, so it stays the same
// across boots and two controllers on one board do not collide; a saved
// configuration's "mac" parameter overrides it, and the file can be edited.
// If the board's own address cannot be read the low bytes are random instead.
std::string ethernet_default_station_address(const char *controller);

class ether_bridge_c: public logsource_c {
public:
    // label prefixes this bridge's log messages, so two controllers in one
    // process stay apart in the log
    explicit ether_bridge_c(const char *label);
    ~ether_bridge_c();

    // Attach to ifname and answer to station_address. Logs the reason and
    // returns false if the interface is missing or the socket is refused.
    bool open(const std::string &ifname, const uint8_t station_address[6]);
    void close(void);

    // Re-aim the kernel filter, for a controller whose driver moves it off the
    // address it powered up with. Nothing to do while the socket is closed:
    // open() installs the filter for the address it is given.
    bool set_station_address(const uint8_t station_address[6]);

    bool is_open(void) const {
        return fd >= 0;
    }

    // Wait up to timeout_ms for a frame:
    //   > 0  frame length, with *outgoing set when this host sent it
    //     0  nothing arrived
    //    -1  the socket is gone; close() and attach again
    int receive(uint8_t *buffer, unsigned buffer_size, unsigned timeout_ms, bool *outgoing);

    // Put a frame on the wire, padded to the Ethernet minimum - the raw
    // socket does not pad. len covers the destination address through the
    // last data byte, without the CRC.
    bool send(const uint8_t *frame, unsigned len);

private:
    static const unsigned ETH_MIN_FRAME = 60;

    int fd;
    int ifindex;
    std::string interface_name;
};

#endif // _ETHER_BRIDGE_HPP_
