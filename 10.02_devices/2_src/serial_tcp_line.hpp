/* serial_tcp_line.hpp: one serial line's bytes carried over a TCP connection

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   A serial_tcp_line_c carries a single emulated serial line's byte stream over
   a TCP socket that speaks Telnet with the RFC2217 COM-PORT-CONTROL option. It
   sits behind the rs232adapter seam: the emulated device (DL11 SLU, DZV11 or
   DHV11 mux line) pushes transmitted bytes with send() and pulls received bytes
   with poll_rcv(), exactly the character granularity the devices already use.

   Roles:
     - ROLE_LISTEN  — bind a TCP port and accept one client (a host program
                      dials in to use this line).
     - ROLE_CONNECT — dial a configured host:port and reconnect on drop.

   The line is point-to-point: while one client holds it a second connection is
   accepted and immediately closed (refused), protecting the session in
   progress. A configurable idle timeout drops a silent client, and disconnect()
   drops it on demand, so a hung client never locks the line permanently.

   RFC2217 line parameters (baud, data bits, parity, stop bits) are negotiated,
   stored and reported, but bytes are always delivered full speed: the TCP link
   has no UART timing, so the negotiated baud is cosmetic.

   All socket I/O runs on one background thread. The device-facing methods
   (send/poll_rcv) and the dashboard-facing methods (client_connected,
   peer_address, disconnect, inject_rx) are thread-safe. The class has no PRU or
   qunibus dependency, so it links into the host test harness unchanged.
*/
#ifndef _SERIAL_TCP_LINE_HPP_
#define _SERIAL_TCP_LINE_HPP_

#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class serial_tcp_line_c {
public:
	enum role_t { ROLE_LISTEN, ROLE_CONNECT };

	// RFC2217 line parameters, stored and reported but not acted upon.
	struct line_params_t {
		uint32_t baud = 9600;
		uint8_t datasize = 8;   // 5..8
		uint8_t parity = 1;     // RFC2217: 1=none 2=odd 3=even 4=mark 5=space
		uint8_t stopsize = 1;   // RFC2217: 1=1 2=2 3=1.5
	};

	serial_tcp_line_c();
	~serial_tcp_line_c();

	serial_tcp_line_c(const serial_tcp_line_c &) = delete;
	serial_tcp_line_c &operator=(const serial_tcp_line_c &) = delete;

	/*** configuration — set before open() ***/
	role_t role = ROLE_LISTEN;
	std::string host;             // ROLE_CONNECT target host
	uint16_t port = 0;            // listen port / connect port
	unsigned idle_timeout_sec = 0;  // 0 = never drop on idle
	unsigned connect_retry_sec = 5; // ROLE_CONNECT reconnect delay
	std::string log_label = "tcpline";
	bool verbose = false;           // print connect/disconnect events to stderr

	// data tap: invoked for every data byte crossing the seam, under no lock.
	// from_pdp true  = byte the emulated device sent toward the client;
	// from_pdp false = byte received from the client toward the device.
	// May be empty. Used to mirror the line into a web console channel.
	std::function<void(const uint8_t *data, size_t len, bool from_pdp)> data_tap;

	/*** lifecycle ***/
	// Start the background I/O thread. Returns false only on a hard config
	// error (a listen port that cannot be bound); a connect-out that cannot
	// reach its peer yet still opens and keeps retrying.
	bool open(void);
	void close(void);
	bool is_open(void) { return running_; }

	/*** device seam (byte granularity) ***/
	// Non-blocking: deliver one received byte, false if none pending.
	bool poll_rcv(uint8_t *c);
	// Queue one byte toward the client. Dropped if no client is connected.
	void send(uint8_t c);

	/*** dashboard / control ***/
	bool client_connected(void);
	// Port actually bound in ROLE_LISTEN (equals `port`, or the kernel-assigned
	// value when `port` was 0). 0 until the listen socket is bound.
	uint16_t local_port(void);
	std::string peer_address(void);   // "ip:port" or "" if none
	void disconnect(void);            // drop the current client, if any
	// Inject dashboard keystrokes into the device's receive path. Allowed only
	// when no TCP client holds the line; returns false (and injects nothing)
	// while a client is connected.
	bool inject_rx(const uint8_t *data, size_t len);

	line_params_t reported_params(void);
	uint64_t rx_bytes(void);
	uint64_t tx_bytes(void);

private:
	/*** background thread ***/
	std::thread io_thread_;
	volatile bool running_ = false;
	int wake_pipe_[2] = { -1, -1 }; // self-pipe: [0] read, [1] write
	int listen_fd_ = -1;
	int client_fd_ = -1;

	void io_run(void);
	bool setup_listen(void);
	int try_connect(void);            // ROLE_CONNECT: returns connected fd or -1
	void accept_client(void);         // ROLE_LISTEN: accept or refuse
	void service_client(void);        // read/write loop for the live client
	void close_client(void);
	void wake(void);
	void drain_wake(void);

	/*** Telnet / RFC2217 framing ***/
	void on_new_connection(void);     // send initial negotiation
	void feed_inbound(const uint8_t *data, size_t len); // de-Telnet into rx_data_
	void queue_raw_out(const uint8_t *data, size_t len); // protocol bytes, verbatim
	void handle_subneg(void);         // process a complete IAC SB ... SE
	void handle_com_port_subneg(const uint8_t *body, size_t len);
	void reply_com_port(uint8_t cmd, uint32_t value, unsigned nbytes);

	// Telnet option negotiation, sending a change only when our state moves.
	// will_state_/do_state_: 0 = unsent, 1 = WILL/DO active, 2 = WONT/DONT.
	void send_will(uint8_t opt);
	void send_wont(uint8_t opt);
	void send_do(uint8_t opt);
	void send_dont(uint8_t opt);
	uint8_t will_state_[256] = { 0 };
	uint8_t do_state_[256] = { 0 };

	// Telnet parser state
	enum tstate_t { T_DATA, T_IAC, T_WILL, T_WONT, T_DO, T_DONT, T_SB, T_SB_IAC };
	tstate_t tstate_ = T_DATA;
	std::deque<uint8_t> sb_buf_;      // accumulates SB ... payload

	/*** shared state ***/
	std::mutex mutex_;
	std::deque<uint8_t> rx_data_;     // client -> device
	std::deque<uint8_t> tx_raw_;      // device/protocol -> socket (already framed)
	bool connected_ = false;
	bool drop_requested_ = false;     // disconnect() asks the io thread to drop
	uint16_t bound_port_ = 0;         // actual listen port after bind
	std::string peer_addr_;
	line_params_t params_;
	uint64_t rx_bytes_ = 0;
	uint64_t tx_bytes_ = 0;
	int64_t last_rx_ms_ = 0;          // for idle timeout
};

#endif // _SERIAL_TCP_LINE_HPP_
