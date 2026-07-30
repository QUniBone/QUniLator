/* ether_bridge.cpp: host Ethernet attachment for the emulated controllers

   Copyright (c) 2026, Hans Huebner
   MIT license, see ether_bridge.hpp for the full text.
*/

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <vector>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/filter.h>
#include <linux/if_ether.h>

#include "logger.hpp"
#include "ether_bridge.hpp"

#include <fstream>
#include <cstdlib>
#include <cstdio>

// The station address an emulated Ethernet controller powers on with. See
// ether_bridge.hpp for what it is and why it is kept.
std::string ethernet_default_station_address(const char *controller)
{
    const char *dir = getenv("QUNILATOR_DIR");
    std::string path = std::string(dir && *dir ? dir : "/var/lib/qunilator")
            + "/" + controller + ".mac";

    // a value written on a previous run wins, keeping the address stable
    {
        std::ifstream in(path);
        std::string line;
        if (in && std::getline(in, line) && line.size() >= 17)
            return line.substr(0, 17);
    }

    std::string address;
    // DEC OUI plus the three octets after the board address's third colon
    {
        std::ifstream in("/sys/class/net/eth0/address");
        std::string hw;
        if (in && std::getline(in, hw) && hw.size() >= 17) {
            size_t p = 0;
            int colons = 0;
            while (p < hw.size() && colons < 3) {
                if (hw[p] == ':')
                    colons++;
                p++;
            }
            if (colons == 3 && hw.size() - p >= 8)
                address = "08:00:2b:" + hw.substr(p, 8);
        }
    }
    // fallback: random low three bytes
    if (address.empty()) {
        uint8_t r[3] = { 0, 0, 0 };
        FILE *urandom = fopen("/dev/urandom", "rb");
        if (urandom) {
            size_t got = fread(r, 1, sizeof r, urandom);
            (void) got;
            fclose(urandom);
        }
        char buf[24];
        snprintf(buf, sizeof buf, "08:00:2b:%02x:%02x:%02x", r[0], r[1], r[2]);
        address = buf;
    }

    // persist so the next boot reads the same address
    {
        std::ofstream out(path);
        if (out)
            out << address << "\n";
    }
    return address;
}

ether_bridge_c::ether_bridge_c(const char *label) :
        fd(-1), ifindex(0)
{
    log_label = label;
}

ether_bridge_c::~ether_bridge_c()
{
    close();
}

bool ether_bridge_c::open(const std::string &ifname, const uint8_t station_address[6])
{
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        ERROR("bridge: cannot open raw socket: %s (root required)", strerror(errno));
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname.c_str(), sizeof(ifr.ifr_name) - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        ERROR("bridge: no such interface \"%s\"", ifname.c_str());
        ::close(sock);
        return false;
    }
    ifindex = ifr.ifr_ifindex;

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifindex;
    if (bind(sock, (struct sockaddr *) &sll, sizeof(sll)) < 0) {
        ERROR("bridge: cannot bind to \"%s\": %s", ifname.c_str(), strerror(errno));
        ::close(sock);
        return false;
    }

    // the controller's station address is not the host MAC: receive everything
    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = ifindex;
    mreq.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(sock, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ERROR("bridge: cannot enable promiscuous mode on \"%s\": %s", ifname.c_str(),
                strerror(errno));
        ::close(sock);
        return false;
    }

    fd = sock;
    interface_name = ifname;
    set_station_address(station_address);
    INFO("bridge: attached to \"%s\"", ifname.c_str());
    return true;
}

bool ether_bridge_c::set_station_address(const uint8_t station_address[6])
{
    if (fd < 0)
        return true;

    // queue only frames this controller could want: its station address,
    // broadcast and multicast
    const uint8_t *sa = station_address;
    uint32_t sa_hi = ((uint32_t) sa[0] << 24) | ((uint32_t) sa[1] << 16)
            | ((uint32_t) sa[2] << 8) | sa[3];
    uint32_t sa_lo = ((uint32_t) sa[4] << 8) | sa[5];
    struct sock_filter code[] = {
        { BPF_LD | BPF_B | BPF_ABS, 0, 0, 0 },      // A = dst[0]
        { BPF_ALU | BPF_AND | BPF_K, 0, 0, 1 },     // A &= multicast bit
        { BPF_JMP | BPF_JEQ | BPF_K, 0, 4, 0 },     // set? -> accept
        { BPF_LD | BPF_W | BPF_ABS, 0, 0, 0 },      // A = dst[0..3]
        { BPF_JMP | BPF_JEQ | BPF_K, 0, 3, sa_hi },
        { BPF_LD | BPF_H | BPF_ABS, 0, 0, 4 },      // A = dst[4..5]
        { BPF_JMP | BPF_JEQ | BPF_K, 0, 1, sa_lo },
        { BPF_RET | BPF_K, 0, 0, 0xffff },          // accept
        { BPF_RET | BPF_K, 0, 0, 0 },               // drop
    };
    struct sock_fprog prog;
    prog.len = sizeof(code) / sizeof(code[0]);
    prog.filter = code;
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) < 0) {
        WARNING("bridge: cannot attach address filter: %s", strerror(errno));
        return false;
    }
    return true;
}

void ether_bridge_c::close(void)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

int ether_bridge_c::receive(uint8_t *buffer, unsigned buffer_size, unsigned timeout_ms,
        bool *outgoing)
{
    *outgoing = false;
    if (fd < 0)
        return -1;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ready = poll(&pfd, 1, (int) timeout_ms);
    if (ready <= 0)
        return 0; // timeout or signal: the caller re-checks its terminate flag

    struct sockaddr_ll from;
    socklen_t fromlen = sizeof(from);
    ssize_t len = recvfrom(fd, buffer, buffer_size, MSG_DONTWAIT,
            (struct sockaddr *) &from, &fromlen);
    if (len < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        WARNING("bridge: receive error: %s", strerror(errno));
        return -1;
    }

    *outgoing = (from.sll_pkttype == PACKET_OUTGOING);
    return (int) len;
}

bool ether_bridge_c::send(const uint8_t *frame, unsigned len)
{
    if (fd < 0 || len < 14)
        return false;

    std::vector<uint8_t> padded(frame, frame + len);
    if (padded.size() < ETH_MIN_FRAME)
        padded.resize(ETH_MIN_FRAME, 0);

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifindex;
    sll.sll_halen = 6;
    memcpy(sll.sll_addr, padded.data(), 6);
    // the frame's ethertype becomes skb->protocol: local taps bound to a
    // specific protocol (a mopd on this host) only see the looped frame if
    // it matches
    sll.sll_protocol = htons((padded[12] << 8) | padded[13]);

    ssize_t sent = sendto(fd, padded.data(), padded.size(), 0, (struct sockaddr *) &sll,
            sizeof(sll));
    if (sent != (ssize_t) padded.size()) {
        WARNING("bridge: send failed: %s", strerror(errno));
        return false;
    }
    return true;
}
