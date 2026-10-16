/* ether_filter_test.cpp: host test of an Ethernet controller's receive filter

   Copyright (c) 2026, Hans Huebner
   MIT license, see any source file header for the full text.

   The filter is classic BPF, which the Linux kernel runs on any socket: it is
   attached here to one end of a datagram socket pair, frames are sent from
   the other end, and what arrives is what a bridge socket would have queued.
   No interface and no privileges are needed.

   Build & run: 10.05_web/tools/run_config_test.sh
*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "ether_filter.hpp"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what) {
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static const ether_address_t prom = {{ 0x08, 0x00, 0x2b, 0x11, 0x22, 0x33 }};
static const ether_address_t decnet = {{ 0xaa, 0x00, 0x04, 0x00, 0x1e, 0x78 }};
static const ether_address_t other = {{ 0x08, 0x00, 0x2b, 0x44, 0x55, 0x66 }};
static const ether_address_t mop = {{ 0xab, 0x00, 0x00, 0x01, 0x00, 0x00 }};
static const ether_address_t broadcast = {{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }};

// Whether a frame addressed to dst passes the filter for addresses/promiscuous.
static bool passes(const std::vector<ether_address_t> &addresses, bool promiscuous,
		const ether_address_t &dst) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
		perror("socketpair");
		return false;
	}
	std::vector<struct sock_filter> code = ether_receive_filter(addresses, promiscuous);
	struct sock_fprog prog;
	prog.len = code.size();
	prog.filter = code.data();
	if (setsockopt(sv[1], SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog) < 0) {
		perror("SO_ATTACH_FILTER");
		close(sv[0]);
		close(sv[1]);
		return false;
	}
	uint8_t frame[60];
	memset(frame, 0, sizeof frame);
	memcpy(frame, dst.bytes, 6);
	memcpy(frame + 6, other.bytes, 6);
	send(sv[0], frame, sizeof frame, 0);
	fcntl(sv[1], F_SETFL, O_NONBLOCK);
	uint8_t got[60];
	bool arrived = recv(sv[1], got, sizeof got, 0) > 0;
	close(sv[0]);
	close(sv[1]);
	return arrived;
}

int main(void) {
	// what a controller answers to before any setup: its PROM address
	std::vector<ether_address_t> just_prom = { prom };
	check(passes(just_prom, false, prom), "the station address passes");
	check(!passes(just_prom, false, other), "another host's unicast is dropped");
	check(!passes(just_prom, false, decnet), "an address the controller was not given is dropped");
	check(passes(just_prom, false, broadcast), "broadcast passes");
	check(passes(just_prom, false, mop), "multicast passes");

	// a setup packet adds the DECnet address: frames to it reach the controller
	std::vector<ether_address_t> after_setup = { prom, decnet };
	check(passes(after_setup, false, decnet), "an address set up later passes");
	check(passes(after_setup, false, prom), "the station address still passes");
	check(!passes(after_setup, false, other), "another host's unicast is still dropped");

	// fourteen setup entries and the PROM address: the longest program
	std::vector<ether_address_t> full = { prom };
	for (uint8_t i = 0; i < 14; i++)
		full.push_back({{ 0xaa, 0x00, 0x04, 0x00, 0x10, i }});
	check(passes(full, false, full.back()), "the last of fifteen addresses passes");
	check(!passes(full, false, other), "fifteen addresses still drop another host");

	// promiscuous: everything
	check(passes(just_prom, true, other), "promiscuous passes another host's unicast");

	printf("ether_filter_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
