/* ether_filter.cpp: the kernel receive filter of an emulated Ethernet controller

   Copyright (c) 2026, Hans Huebner
   MIT license, see ether_bridge.hpp for the full text.
*/

#include "ether_filter.hpp"

std::vector<struct sock_filter> ether_receive_filter(
        const std::vector<ether_address_t> &addresses, bool promiscuous)
{
    std::vector<struct sock_filter> code;

    if (promiscuous) {
        code.push_back({ BPF_RET | BPF_K, 0, 0, 0xffff });
        return code;
    }

    // Laid out as: the multicast test, four instructions per address, drop,
    // accept. A jump counts the instructions it skips, so each is computed
    // from the index of the accept at the end.
    const unsigned accept = 3 + 4 * addresses.size() + 1;
    auto to_accept = [&](unsigned from) { return (uint8_t) (accept - from - 1); };

    code.push_back({ BPF_LD | BPF_B | BPF_ABS, 0, 0, 0 });     // A = dst[0]
    code.push_back({ BPF_ALU | BPF_AND | BPF_K, 0, 0, 1 });    // A &= multicast bit
    code.push_back({ BPF_JMP | BPF_JEQ | BPF_K, 0, to_accept(2), 0 }); // set? -> accept
    for (const ether_address_t &a : addresses) {
        const uint8_t *sa = a.bytes;
        uint32_t hi = ((uint32_t) sa[0] << 24) | ((uint32_t) sa[1] << 16)
                | ((uint32_t) sa[2] << 8) | sa[3];
        uint32_t lo = ((uint32_t) sa[4] << 8) | sa[5];
        unsigned at = code.size();
        code.push_back({ BPF_LD | BPF_W | BPF_ABS, 0, 0, 0 });  // A = dst[0..3]
        code.push_back({ BPF_JMP | BPF_JEQ | BPF_K, 0, 2, hi }); // no -> next address
        code.push_back({ BPF_LD | BPF_H | BPF_ABS, 0, 0, 4 });  // A = dst[4..5]
        code.push_back({ BPF_JMP | BPF_JEQ | BPF_K, to_accept(at + 3), 0, lo });
    }
    code.push_back({ BPF_RET | BPF_K, 0, 0, 0 });               // drop
    code.push_back({ BPF_RET | BPF_K, 0, 0, 0xffff });          // accept
    return code;
}
