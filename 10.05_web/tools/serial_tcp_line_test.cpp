/* serial_tcp_line_test.cpp: host loopback test of the TCP serial line backend

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   serial_tcp_line_c speaks to a real TCP socket and runs its own I/O thread, so
   this test drives it end to end on the development host with a raw client
   socket on the loopback interface — no BeagleBone, no PRU, no civetweb. It
   exercises the behaviours the serial-ports plan calls out:

     - listen accepts one client, and a second connection is refused;
     - data round-trips both directions across the Telnet framing;
     - RFC2217 SET-BAUDRATE is answered and the reported params reflect it;
     - the idle timeout drops a silent client;
     - disconnect() frees the line for a new client;
     - connect-out dials a configured host:port;
     - inject_rx works only while no client holds the line.

   Build & run: 10.05_web/tools/run_config_test.sh.
*/

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "serial_tcp_line.hpp"

/*** scaffolding ***/
static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

// Spin until pred() or the budget (ms) runs out; keeps the test quick yet robust
// against scheduling jitter in the line's background thread.
template<typename P> static bool wait_until(P pred, int budget_ms)
{
	for (int i = 0; i < budget_ms; i++) {
		if (pred())
			return true;
		usleep(1000);
	}
	return pred();
}

/*** a raw Telnet client on the loopback ***/
struct telnet_client {
	int fd = -1;
	// last COM-PORT reply captured (command, value)
	int cpc_cmd = -1;
	uint32_t cpc_val = 0;

	bool connect_to(uint16_t port)
	{
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return false;
		struct timeval tv = { 0, 200 * 1000 }; // 200 ms recv timeout
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof addr);
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		return ::connect(fd, (struct sockaddr *) &addr, sizeof addr) == 0;
	}
	void close_it()
	{
		if (fd >= 0)
			::close(fd);
		fd = -1;
	}
	void send_raw(const void *p, size_t n)
	{
		ssize_t w = ::send(fd, p, n, 0);
		(void) w;
	}
	void send_str(const char *s) { send_raw(s, strlen(s)); }

	// Read available bytes, strip Telnet framing, return the data bytes; also
	// records the most recent COM-PORT subnegotiation reply. Returns after one
	// recv (which blocks up to the 200 ms timeout).
	std::string pump()
	{
		uint8_t buf[2048];
		ssize_t n = ::recv(fd, buf, sizeof buf, 0);
		if (n <= 0)
			return std::string(); // timeout or closed
		return defframe(buf, (size_t) n);
	}
	// Returns true if the peer closed (recv == 0) within the timeout.
	bool closed()
	{
		uint8_t buf[256];
		ssize_t n = ::recv(fd, buf, sizeof buf, 0);
		return n == 0;
	}

private:
	enum { D, IAC, WILL, WONT, DO, DONT, SB, SB_IAC } st = D;
	std::vector<uint8_t> sb;
	std::string defframe(const uint8_t *data, size_t len)
	{
		std::string out;
		for (size_t i = 0; i < len; i++) {
			uint8_t b = data[i];
			switch (st) {
			case D: if (b == 255) st = IAC; else out.push_back((char) b); break;
			case IAC:
				if (b == 255) { out.push_back((char) 255); st = D; }
				else if (b == 251) st = WILL;
				else if (b == 252) st = WONT;
				else if (b == 253) st = DO;
				else if (b == 254) st = DONT;
				else if (b == 250) { sb.clear(); st = SB; }
				else st = D;
				break;
			case WILL: case WONT: case DO: case DONT: st = D; break;
			case SB: if (b == 255) st = SB_IAC; else sb.push_back(b); break;
			case SB_IAC:
				if (b == 240) { capture_sb(); st = D; }
				else { sb.push_back(b); st = SB; }
				break;
			}
		}
		return out;
	}
	void capture_sb()
	{
		if (sb.size() >= 2 && sb[0] == 44) { // COM-PORT-OPTION
			cpc_cmd = sb[1];
			cpc_val = 0;
			for (size_t i = 2; i < sb.size(); i++)
				cpc_val = (cpc_val << 8) | sb[i];
		}
	}
};

int main(void)
{
	/* 0. library defaults: role is LISTEN, so a line configured with only a
	   valid port binds and accepts — the device muxes rely on this to make an
	   enabled line usable from its default tcp_role/tcp_port alone. */
	{
		serial_tcp_line_c line;
		check(line.role == serial_tcp_line_c::ROLE_LISTEN,
				"default role is LISTEN");
		line.port = 0; // kernel-assigned, standing in for a device's default port
		check(line.open(), "line with default role opens on a valid port");
		telnet_client c;
		check(c.connect_to(line.local_port()),
				"a client connects to the default-role line");
		check(wait_until([&] { return line.client_connected(); }, 1000),
				"default-role line accepts the client");
		c.close_it();
		line.close();
	}

	/* 1. listen: one client, data both ways, second refused */
	{
		serial_tcp_line_c line;
		line.role = serial_tcp_line_c::ROLE_LISTEN;
		line.port = 0; // kernel-assigned
		check(line.open(), "listen line opens");
		uint16_t port = line.local_port();
		check(port != 0, "listen line reports a bound port");

		telnet_client c;
		check(c.connect_to(port), "client connects");
		check(wait_until([&] { return line.client_connected(); }, 1000),
				"line reports the client connected");
		check(!line.peer_address().empty(), "line reports the peer address");

		// device -> client
		line.send('H');
		line.send('i');
		std::string got;
		check(wait_until([&] { got += c.pump(); return got == "Hi"; }, 1000),
				"device output reaches the client");

		// client -> device
		c.send_str("yo");
		uint8_t ch = 0;
		std::string rx;
		wait_until([&] { if (line.poll_rcv(&ch)) rx.push_back((char) ch); return rx == "yo"; },
				1000);
		check(rx == "yo", "client input reaches the device");

		// second connection is refused
		telnet_client c2;
		check(c2.connect_to(port), "second TCP connect is accepted at kernel level");
		check(c2.closed(), "second client is refused (server closes it)");
		check(line.client_connected(), "the first client still holds the line");

		// first client still works after the refusal
		line.send('!');
		std::string more;
		check(wait_until([&] { more += c.pump(); return more.find('!') != std::string::npos; },
				1000), "first client keeps working after a refused second");

		c.close_it();
		line.close();
	}

	/* 2. RFC2217 SET-BAUDRATE is answered and reported */
	{
		serial_tcp_line_c line;
		line.role = serial_tcp_line_c::ROLE_LISTEN;
		line.port = 0;
		line.open();
		telnet_client c;
		c.connect_to(line.local_port());
		wait_until([&] { return line.client_connected(); }, 1000);
		c.pump(); // absorb the server's initial negotiation

		// IAC SB COM-PORT SET-BAUDRATE(1) 0x00002580 (9600) IAC SE
		uint8_t req[] = { 255, 250, 44, 1, 0x00, 0x00, 0x25, 0x80, 255, 240 };
		c.send_raw(req, sizeof req);
		check(wait_until([&] { c.pump(); return c.cpc_cmd == 101; }, 1000),
				"server answers SET-BAUDRATE with a 101 reply");
		check(c.cpc_val == 9600, "the reply echoes the requested baud");
		check(line.reported_params().baud == 9600, "the line stores the negotiated baud");

		c.close_it();
		line.close();
	}

	/* 3. idle timeout drops a silent client */
	{
		serial_tcp_line_c line;
		line.role = serial_tcp_line_c::ROLE_LISTEN;
		line.port = 0;
		line.idle_timeout_sec = 1;
		line.open();
		telnet_client c;
		c.connect_to(line.local_port());
		wait_until([&] { return line.client_connected(); }, 1000);
		// stay silent past the timeout; the server should drop us
		check(wait_until([&] { return !line.client_connected(); }, 3000),
				"idle client is dropped after the timeout");
		c.close_it();
		line.close();
	}

	/* 4. disconnect() frees the line for a new client */
	{
		serial_tcp_line_c line;
		line.role = serial_tcp_line_c::ROLE_LISTEN;
		line.port = 0;
		line.open();
		telnet_client c;
		c.connect_to(line.local_port());
		wait_until([&] { return line.client_connected(); }, 1000);
		line.disconnect();
		check(wait_until([&] { return !line.client_connected(); }, 1000),
				"disconnect() drops the client");
		c.close_it();

		telnet_client c2;
		check(c2.connect_to(line.local_port()), "a new client can connect after disconnect");
		check(wait_until([&] { return line.client_connected(); }, 1000),
				"the line accepts the fresh client");
		c2.close_it();
		line.close();
	}

	/* 5. inject_rx only when no client holds the line */
	{
		serial_tcp_line_c line;
		line.role = serial_tcp_line_c::ROLE_LISTEN;
		line.port = 0;
		line.open();
		// no client yet: dashboard input is accepted
		check(line.inject_rx((const uint8_t *) "ab", 2), "inject_rx accepted with no client");
		uint8_t ch = 0;
		std::string rx;
		wait_until([&] { if (line.poll_rcv(&ch)) rx.push_back((char) ch); return rx == "ab"; },
				500);
		check(rx == "ab", "injected bytes reach the device");

		telnet_client c;
		c.connect_to(line.local_port());
		wait_until([&] { return line.client_connected(); }, 1000);
		check(!line.inject_rx((const uint8_t *) "x", 1),
				"inject_rx refused while a client holds the line");
		c.close_it();
		line.close();
	}

	/* 6. connect-out dials a configured host:port */
	{
		// a plain listening socket stands in for the remote host
		int srv = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		struct sockaddr_in a;
		memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = 0;
		bind(srv, (struct sockaddr *) &a, sizeof a);
		listen(srv, 1);
		socklen_t alen = sizeof a;
		getsockname(srv, (struct sockaddr *) &a, &alen);
		uint16_t rport = ntohs(a.sin_port);

		serial_tcp_line_c line;
		line.role = serial_tcp_line_c::ROLE_CONNECT;
		line.host = "127.0.0.1";
		line.port = rport;
		check(line.open(), "connect-out line opens");

		struct timeval tv = { 2, 0 };
		setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		int cfd = accept(srv, NULL, NULL);
		check(cfd >= 0, "connect-out dials the configured host and is accepted");
		check(wait_until([&] { return line.client_connected(); }, 1000),
				"connect-out line reports connected");
		if (cfd >= 0)
			::close(cfd);
		::close(srv);
		line.close();
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
