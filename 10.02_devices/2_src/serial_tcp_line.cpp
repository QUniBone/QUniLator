/* serial_tcp_line.cpp: one serial line's bytes carried over a TCP connection

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   See serial_tcp_line.hpp for the design. All socket I/O runs on io_thread_;
   the device-facing and dashboard-facing methods are thread-safe. The unit has
   no PRU or qunibus dependency so it links into the host test harness.
*/

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include "serial_tcp_line.hpp"

// Lifecycle events go to stderr (captured in the service journal) rather than
// through the device logger, so this unit stays free of the PRU/logger stack
// and links into the host test harness unchanged. INFO lines print only when
// the line is opened verbose; errors always print.
static void tcp_line_log(bool verbose, bool is_error, const char *label,
		const char *fmt, ...)
{
	if (!is_error && !verbose)
		return;
	fprintf(stderr, "[%s] ", label);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define LINE_INFO(...)  tcp_line_log(verbose, false, log_label.c_str(), __VA_ARGS__)
#define LINE_ERROR(...) tcp_line_log(verbose, true,  log_label.c_str(), __VA_ARGS__)

// Telnet framing
static const uint8_t T_IAC_B = 255;
static const uint8_t T_SE_B = 240;
static const uint8_t T_SB_B = 250;
static const uint8_t T_WILL_B = 251;
static const uint8_t T_WONT_B = 252;
static const uint8_t T_DO_B = 253;
static const uint8_t T_DONT_B = 254;

// Telnet options we support
static const uint8_t OPT_BINARY = 0;
static const uint8_t OPT_ECHO = 1;     // server echoes (client stops local echo)
static const uint8_t OPT_SGA = 3;      // suppress go ahead
static const uint8_t OPT_COMPORT = 44; // RFC2217 COM-PORT-CONTROL

// RFC2217 client->server command codes; server->client replies are +100
static const uint8_t CPC_SET_BAUDRATE = 1;
static const uint8_t CPC_SET_DATASIZE = 2;
static const uint8_t CPC_SET_PARITY = 3;
static const uint8_t CPC_SET_STOPSIZE = 4;

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

serial_tcp_line_c::serial_tcp_line_c()
{
}

serial_tcp_line_c::~serial_tcp_line_c()
{
	close();
}

bool serial_tcp_line_c::open(void)
{
	if (running_)
		return true;
	if (pipe(wake_pipe_) < 0)
		return false;
	// non-blocking read end so drain_wake never stalls
	int fl = fcntl(wake_pipe_[0], F_GETFL, 0);
	fcntl(wake_pipe_[0], F_SETFL, fl | O_NONBLOCK);

	if (role == ROLE_LISTEN && !setup_listen()) {
		::close(wake_pipe_[0]);
		::close(wake_pipe_[1]);
		wake_pipe_[0] = wake_pipe_[1] = -1;
		return false; // a listen port that cannot be bound is a hard error
	}

	running_ = true;
	io_thread_ = std::thread(&serial_tcp_line_c::io_run, this);
	return true;
}

void serial_tcp_line_c::close(void)
{
	if (!running_)
		return;
	running_ = false;
	wake();
	if (io_thread_.joinable())
		io_thread_.join();
	close_client();
	if (listen_fd_ >= 0) {
		::close(listen_fd_);
		listen_fd_ = -1;
	}
	if (wake_pipe_[0] >= 0)
		::close(wake_pipe_[0]);
	if (wake_pipe_[1] >= 0)
		::close(wake_pipe_[1]);
	wake_pipe_[0] = wake_pipe_[1] = -1;
}

void serial_tcp_line_c::wake(void)
{
	if (wake_pipe_[1] >= 0) {
		uint8_t b = 0;
		ssize_t n = ::write(wake_pipe_[1], &b, 1);
		(void) n;
	}
}

void serial_tcp_line_c::drain_wake(void)
{
	uint8_t buf[64];
	while (::read(wake_pipe_[0], buf, sizeof buf) > 0)
		;
}

bool serial_tcp_line_c::setup_listen(void)
{
	listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd_ < 0)
		return false;
	int one = 1;
	setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(listen_fd_, (struct sockaddr *) &addr, sizeof addr) < 0) {
		LINE_ERROR("cannot bind TCP port %u: %s", port, strerror(errno));
		::close(listen_fd_);
		listen_fd_ = -1;
		return false;
	}
	if (listen(listen_fd_, 1) < 0) {
		::close(listen_fd_);
		listen_fd_ = -1;
		return false;
	}
	// record the actual port (may have been kernel-assigned for port 0)
	struct sockaddr_in bound;
	socklen_t blen = sizeof bound;
	if (getsockname(listen_fd_, (struct sockaddr *) &bound, &blen) == 0) {
		std::lock_guard<std::mutex> lock(mutex_);
		bound_port_ = ntohs(bound.sin_port);
	}
	LINE_INFO("listening on TCP port %u", ntohs(bound.sin_port));
	return true;
}

// non-blocking connect with a bounded wait, so shutdown stays responsive
int serial_tcp_line_c::try_connect(void)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char portstr[16];
	snprintf(portstr, sizeof portstr, "%u", port);
	struct addrinfo *res = NULL;
	if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || res == NULL)
		return -1;

	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		freeaddrinfo(res);
		return -1;
	}
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	int rc = connect(fd, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if (rc == 0)
		return fd; // immediate connect
	if (errno != EINPROGRESS) {
		::close(fd);
		return -1;
	}

	fd_set wfds;
	FD_ZERO(&wfds);
	FD_SET(fd, &wfds);
	FD_SET(wake_pipe_[0], &wfds);
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(wake_pipe_[0], &rfds);
	int maxfd = fd > wake_pipe_[0] ? fd : wake_pipe_[0];
	struct timeval tv = { 5, 0 };
	if (select(maxfd + 1, &rfds, &wfds, NULL, &tv) <= 0) {
		::close(fd);
		return -1;
	}
	if (FD_ISSET(wake_pipe_[0], &rfds)) {
		drain_wake();
		::close(fd);
		return -1; // shutdown or reconfig
	}
	int err = 0;
	socklen_t elen = sizeof err;
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
		::close(fd);
		return -1;
	}
	return fd;
}

// listen role: wait for a client, accept it exactly once
void serial_tcp_line_c::accept_client(void)
{
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(listen_fd_, &rfds);
	FD_SET(wake_pipe_[0], &rfds);
	int maxfd = listen_fd_ > wake_pipe_[0] ? listen_fd_ : wake_pipe_[0];
	if (select(maxfd + 1, &rfds, NULL, NULL, NULL) <= 0)
		return;
	if (FD_ISSET(wake_pipe_[0], &rfds))
		drain_wake();
	if (!FD_ISSET(listen_fd_, &rfds))
		return;

	struct sockaddr_in peer;
	socklen_t plen = sizeof peer;
	int fd = accept(listen_fd_, (struct sockaddr *) &peer, &plen);
	if (fd < 0)
		return;

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	char ip[INET_ADDRSTRLEN] = { 0 };
	inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
	char addrbuf[64];
	snprintf(addrbuf, sizeof addrbuf, "%s:%u", ip, ntohs(peer.sin_port));

	client_fd_ = fd;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		connected_ = true;
		peer_addr_ = addrbuf;
		last_rx_ms_ = now_ms();
	}
	LINE_INFO("client %s connected", addrbuf);
}

void serial_tcp_line_c::close_client(void)
{
	if (client_fd_ >= 0) {
		::close(client_fd_);
		client_fd_ = -1;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (connected_)
		LINE_INFO("client %s disconnected", peer_addr_.c_str());
	connected_ = false;
	peer_addr_.clear();
	drop_requested_ = false;
	// bytes queued toward a now-absent client are discarded
	tx_raw_.clear();
}

void serial_tcp_line_c::io_run(void)
{
	while (running_) {
		if (role == ROLE_LISTEN) {
			if (listen_fd_ < 0 && !setup_listen()) {
				// transient bind failure: pause, then retry
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(wake_pipe_[0], &rfds);
				struct timeval tv = { 1, 0 };
				select(wake_pipe_[0] + 1, &rfds, NULL, NULL, &tv);
				drain_wake();
				continue;
			}
			accept_client();
			if (client_fd_ >= 0) {
				on_new_connection();
				service_client();
				close_client();
			}
		} else { // ROLE_CONNECT
			int fd = try_connect();
			if (fd < 0) {
				if (!running_)
					break;
				// wait connect_retry_sec, interruptible
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(wake_pipe_[0], &rfds);
				struct timeval tv = { (time_t) (connect_retry_sec ? connect_retry_sec : 1), 0 };
				select(wake_pipe_[0] + 1, &rfds, NULL, NULL, &tv);
				drain_wake();
				continue;
			}
			client_fd_ = fd;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				connected_ = true;
				peer_addr_ = host + ":" + std::to_string(port);
				last_rx_ms_ = now_ms();
			}
			LINE_INFO("connected to %s:%u", host.c_str(), port);
			on_new_connection();
			service_client();
			close_client();
		}
	}
}

void serial_tcp_line_c::service_client(void)
{
	while (running_) {
		bool have_tx;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			have_tx = !tx_raw_.empty();
			if (drop_requested_)
				return;
		}

		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(client_fd_, &rfds);
		FD_SET(wake_pipe_[0], &rfds);
		if (have_tx)
			FD_SET(client_fd_, &wfds);
		int maxfd = client_fd_ > wake_pipe_[0] ? client_fd_ : wake_pipe_[0];
		// second connection attempts arrive on the listen socket: watch it too so
		// we can refuse them promptly instead of leaving them in the backlog.
		if (role == ROLE_LISTEN && listen_fd_ >= 0) {
			FD_SET(listen_fd_, &rfds);
			if (listen_fd_ > maxfd)
				maxfd = listen_fd_;
		}

		struct timeval tv = { 1, 0 }; // 1 s tick drives the idle-timeout check
		int rc = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (!running_)
			return;

		if (FD_ISSET(wake_pipe_[0], &rfds))
			drain_wake();

		// refuse a second client
		if (role == ROLE_LISTEN && listen_fd_ >= 0 && FD_ISSET(listen_fd_, &rfds)) {
			int extra = accept(listen_fd_, NULL, NULL);
			if (extra >= 0) {
				LINE_INFO("refused a second connection (line in use)");
				::close(extra);
			}
		}

		if (FD_ISSET(client_fd_, &rfds)) {
			uint8_t buf[1024];
			ssize_t n = ::recv(client_fd_, buf, sizeof buf, 0);
			if (n == 0)
				return; // orderly close
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					; // spurious wakeup
				else
					return;
			} else {
				feed_inbound(buf, (size_t) n);
			}
		}

		if (have_tx && FD_ISSET(client_fd_, &wfds)) {
			uint8_t buf[1024];
			size_t n = 0;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				while (n < sizeof buf && !tx_raw_.empty()) {
					buf[n++] = tx_raw_.front();
					tx_raw_.pop_front();
				}
			}
			if (n > 0) {
				ssize_t w = ::send(client_fd_, buf, n, MSG_NOSIGNAL);
				if (w < 0) {
					if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
						return;
					// could not send: requeue at the front, preserving order
					std::lock_guard<std::mutex> lock(mutex_);
					for (size_t i = n; i > 0; i--)
						tx_raw_.push_front(buf[i - 1]);
				} else if ((size_t) w < n) {
					std::lock_guard<std::mutex> lock(mutex_);
					for (size_t i = n; i > (size_t) w; i--)
						tx_raw_.push_front(buf[i - 1]);
				}
			}
		}

		// idle timeout
		if (idle_timeout_sec > 0) {
			int64_t idle;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				idle = now_ms() - last_rx_ms_;
			}
			if (idle > (int64_t) idle_timeout_sec * 1000) {
				LINE_INFO("dropping idle client after %u s", idle_timeout_sec);
				return;
			}
		}
	}
}

// -------------------------------------------------------------------------
// Telnet / RFC2217 framing

void serial_tcp_line_c::queue_raw_out(const uint8_t *data, size_t len)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (size_t i = 0; i < len; i++)
		tx_raw_.push_back(data[i]);
}

void serial_tcp_line_c::on_new_connection(void)
{
	// reset the inbound parser and negotiation memory
	tstate_ = T_DATA;
	sb_buf_.clear();
	memset(will_state_, 0, sizeof will_state_);
	memset(do_state_, 0, sizeof do_state_);

	// offer 8-bit clean transport and advertise RFC2217. WILL ECHO tells the
	// client the guest echoes, so it stops local-echoing: input shows once, and
	// the guest can suppress echo for password entry. With SGA this is the
	// character-at-a-time remote-echo mode a Unix login expects.
	send_will(OPT_ECHO);
	send_will(OPT_BINARY);
	send_do(OPT_BINARY);
	send_will(OPT_SGA);
	send_do(OPT_SGA);
	send_will(OPT_COMPORT);
	wake();
}

void serial_tcp_line_c::send_will(uint8_t opt)
{
	if (will_state_[opt] == 1)
		return;
	will_state_[opt] = 1;
	uint8_t seq[3] = { T_IAC_B, T_WILL_B, opt };
	queue_raw_out(seq, 3);
}

void serial_tcp_line_c::send_wont(uint8_t opt)
{
	if (will_state_[opt] == 2)
		return;
	will_state_[opt] = 2;
	uint8_t seq[3] = { T_IAC_B, T_WONT_B, opt };
	queue_raw_out(seq, 3);
}

void serial_tcp_line_c::send_do(uint8_t opt)
{
	if (do_state_[opt] == 1)
		return;
	do_state_[opt] = 1;
	uint8_t seq[3] = { T_IAC_B, T_DO_B, opt };
	queue_raw_out(seq, 3);
}

void serial_tcp_line_c::send_dont(uint8_t opt)
{
	if (do_state_[opt] == 2)
		return;
	do_state_[opt] = 2;
	uint8_t seq[3] = { T_IAC_B, T_DONT_B, opt };
	queue_raw_out(seq, 3);
}

static bool option_supported(uint8_t opt)
{
	return opt == OPT_BINARY || opt == OPT_SGA || opt == OPT_COMPORT;
}

void serial_tcp_line_c::feed_inbound(const uint8_t *data, size_t len)
{
	uint8_t out[1024];
	size_t outn = 0;

	for (size_t i = 0; i < len; i++) {
		uint8_t b = data[i];
		switch (tstate_) {
		case T_DATA:
			if (b == T_IAC_B)
				tstate_ = T_IAC;
			else
				out[outn++] = b;
			break;
		case T_IAC:
			if (b == T_IAC_B) {
				out[outn++] = T_IAC_B; // escaped data 0xff
				tstate_ = T_DATA;
			} else if (b == T_WILL_B)
				tstate_ = T_WILL;
			else if (b == T_WONT_B)
				tstate_ = T_WONT;
			else if (b == T_DO_B)
				tstate_ = T_DO;
			else if (b == T_DONT_B)
				tstate_ = T_DONT;
			else if (b == T_SB_B) {
				sb_buf_.clear();
				tstate_ = T_SB;
			} else
				tstate_ = T_DATA; // ignore other 2-byte commands
			break;
		case T_WILL:
			option_supported(b) ? send_do(b) : send_dont(b);
			tstate_ = T_DATA;
			break;
		case T_WONT:
			send_dont(b);
			tstate_ = T_DATA;
			break;
		case T_DO:
			option_supported(b) ? send_will(b) : send_wont(b);
			tstate_ = T_DATA;
			break;
		case T_DONT:
			send_wont(b);
			tstate_ = T_DATA;
			break;
		case T_SB:
			if (b == T_IAC_B)
				tstate_ = T_SB_IAC;
			else
				sb_buf_.push_back(b);
			break;
		case T_SB_IAC:
			if (b == T_SE_B) {
				handle_subneg();
				tstate_ = T_DATA;
			} else {
				sb_buf_.push_back(b); // IAC IAC inside SB, or stray
				tstate_ = T_SB;
			}
			break;
		}
		if (outn == sizeof out) {
			// flush a full buffer of data bytes
			{
				std::lock_guard<std::mutex> lock(mutex_);
				for (size_t k = 0; k < outn; k++)
					rx_data_.push_back(out[k]);
				rx_bytes_ += outn;
				last_rx_ms_ = now_ms();
			}
			if (data_tap)
				data_tap(out, outn, false);
			outn = 0;
		}
	}

	if (outn > 0) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (size_t k = 0; k < outn; k++)
				rx_data_.push_back(out[k]);
			rx_bytes_ += outn;
			last_rx_ms_ = now_ms();
		}
		if (data_tap)
			data_tap(out, outn, false);
	} else {
		// even a pure-protocol burst counts as activity against the idle timer
		std::lock_guard<std::mutex> lock(mutex_);
		last_rx_ms_ = now_ms();
	}
	wake(); // protocol replies may be queued
}

void serial_tcp_line_c::handle_subneg(void)
{
	if (sb_buf_.empty())
		return;
	uint8_t opt = sb_buf_[0];
	if (opt != OPT_COMPORT)
		return;
	std::vector<uint8_t> body(sb_buf_.begin() + 1, sb_buf_.end());
	handle_com_port_subneg(body.data(), body.size());
}

// Build IAC SB COMPORT <cmd> <value big-endian> IAC SE, escaping any 0xff.
void serial_tcp_line_c::reply_com_port(uint8_t cmd, uint32_t value, unsigned nbytes)
{
	uint8_t seq[32];
	size_t n = 0;
	seq[n++] = T_IAC_B;
	seq[n++] = T_SB_B;
	seq[n++] = OPT_COMPORT;
	seq[n++] = cmd;
	for (unsigned i = 0; i < nbytes; i++) {
		uint8_t vb = (value >> (8 * (nbytes - 1 - i))) & 0xff;
		seq[n++] = vb;
		if (vb == T_IAC_B)
			seq[n++] = T_IAC_B;
	}
	seq[n++] = T_IAC_B;
	seq[n++] = T_SE_B;
	queue_raw_out(seq, n);
}

void serial_tcp_line_c::handle_com_port_subneg(const uint8_t *body, size_t len)
{
	if (len < 1)
		return;
	uint8_t cmd = body[0];
	switch (cmd) {
	case CPC_SET_BAUDRATE:
		if (len >= 5) {
			uint32_t b = ((uint32_t) body[1] << 24) | ((uint32_t) body[2] << 16)
					| ((uint32_t) body[3] << 8) | (uint32_t) body[4];
			std::lock_guard<std::mutex> lock(mutex_);
			if (b != 0)
				params_.baud = b;
		}
		reply_com_port(CPC_SET_BAUDRATE + 100, reported_params().baud, 4);
		break;
	case CPC_SET_DATASIZE:
		if (len >= 2) {
			std::lock_guard<std::mutex> lock(mutex_);
			if (body[1] != 0)
				params_.datasize = body[1];
		}
		reply_com_port(CPC_SET_DATASIZE + 100, reported_params().datasize, 1);
		break;
	case CPC_SET_PARITY:
		if (len >= 2) {
			std::lock_guard<std::mutex> lock(mutex_);
			if (body[1] != 0)
				params_.parity = body[1];
		}
		reply_com_port(CPC_SET_PARITY + 100, reported_params().parity, 1);
		break;
	case CPC_SET_STOPSIZE:
		if (len >= 2) {
			std::lock_guard<std::mutex> lock(mutex_);
			if (body[1] != 0)
				params_.stopsize = body[1];
		}
		reply_com_port(CPC_SET_STOPSIZE + 100, reported_params().stopsize, 1);
		break;
	default:
		break; // flow/control/purge and server-range echoes are ignored
	}
}

// -------------------------------------------------------------------------
// device seam

bool serial_tcp_line_c::poll_rcv(uint8_t *c)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (rx_data_.empty())
		return false;
	*c = rx_data_.front();
	rx_data_.pop_front();
	return true;
}

void serial_tcp_line_c::send(uint8_t c)
{
	bool connected;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		connected = connected_;
		if (connected) {
			tx_raw_.push_back(c);
			if (c == T_IAC_B)
				tx_raw_.push_back(T_IAC_B);
			tx_bytes_++;
		}
	}
	if (data_tap)
		data_tap(&c, 1, true); // the dashboard sees output even with no client
	if (connected)
		wake();
}

// -------------------------------------------------------------------------
// dashboard / control

bool serial_tcp_line_c::client_connected(void)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return connected_;
}

uint16_t serial_tcp_line_c::local_port(void)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return bound_port_;
}

std::string serial_tcp_line_c::peer_address(void)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return peer_addr_;
}

void serial_tcp_line_c::disconnect(void)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!connected_)
			return;
		drop_requested_ = true;
	}
	wake();
}

bool serial_tcp_line_c::inject_rx(const uint8_t *data, size_t len)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (connected_)
			return false; // a real client owns the writer slot
		for (size_t i = 0; i < len; i++)
			rx_data_.push_back(data[i]);
		rx_bytes_ += len;
	}
	if (data_tap)
		data_tap(data, len, false);
	return true;
}

serial_tcp_line_c::line_params_t serial_tcp_line_c::reported_params(void)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return params_;
}

uint64_t serial_tcp_line_c::rx_bytes(void)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return rx_bytes_;
}

uint64_t serial_tcp_line_c::tx_bytes(void)
{
	std::lock_guard<std::mutex> lock(mutex_);
	return tx_bytes_;
}
