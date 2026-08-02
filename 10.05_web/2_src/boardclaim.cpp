/* boardclaim.cpp: one program drives the board at a time

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <atomic>
#include <thread>

#include "qunibus.h"		// QUNILATOR_CLI_NAME (the interactive program)
#include "weblog.hpp"
#include "webevents.hpp"
#include "boardclaim.hpp"

// What a page reads while the interactive program has the hardware. It names
// the program and what ends the wait, because an operator reading it is either
// the person who started it or someone looking for whoever did - "the board is
// busy" tells neither of them anything they can act on.
static const char *held_reason =
		QUNILATOR_CLI_NAME " is running; the web interface is unavailable "
		"until it is exited";

// The claim handshake, one line each way. A menu that reads "granted" has the
// hardware; anything else names what stopped it.
static const char *msg_claim = "claim\n";
static const char *msg_granted = "granted\n";

// How long a menu waits for the service to put the machine down. A teardown
// stops the device workers and releases the PRU, which is seconds, not minutes;
// past this the service is not answering and saying so beats waiting.
static const int yield_timeout_ms = 60000;

static std::string path_from_env(const char *var, const char *fallback) {
	const char *v = getenv(var);
	return std::string(v != nullptr && *v ? v : fallback);
}

std::string boardclaim_socket_path(void) {
	return path_from_env("QUNILATOR_BOARD_SOCKET", "/run/qunilator-board.sock");
}

std::string boardclaim_lock_path(void) {
	return path_from_env("QUNILATOR_BOARD_LOCK", "/run/qunilator-board.lock");
}

// Fill a sockaddr_un, refusing a path the address cannot hold rather than
// truncating it into a socket nobody finds.
static bool fill_addr(const std::string &path, struct sockaddr_un *addr,
		std::string *error) {
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	if (path.size() >= sizeof(addr->sun_path)) {
		if (error != nullptr)
			*error = "socket path \"" + path + "\" is too long";
		return false;
	}
	memcpy(addr->sun_path, path.c_str(), path.size());
	return true;
}

// read() until the newline, or the deadline. Returns the line without it.
static bool read_line(int fd, int timeout_ms, std::string *line) {
	line->clear();
	while (line->size() < 256) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int r = poll(&pfd, 1, timeout_ms);
		if (r <= 0)
			return false;
		char c;
		ssize_t n = read(fd, &c, 1);
		if (n != 1)
			return false;
		if (c == '\n')
			return true;
		line->push_back(c);
	}
	return false;
}

// ---- the service side ----------------------------------------------------

static std::atomic<bool> serving(false);
static std::thread serve_thread;
static int listen_fd = -1;

// One claim, from the connection arriving to the connection closing. The board
// is held for the whole of it: yielded before the menu is told it may start,
// taken back once the socket says the menu is gone.
static void run_claim(int fd, const boardclaim_handlers_c &handlers) {
	WEB_INFO("board: the interactive menu asked for the hardware");
	webevents_hold_board(held_reason);
	if (handlers.yield)
		handlers.yield();

	// Told only after the hardware is actually down: a menu that started on the
	// service's PRU would be driving the bus against it.
	if (write(fd, msg_granted, strlen(msg_granted)) < 0)
		WEB_WARNING("board: the menu went away before it was told it could start");
	else
		WEB_INFO("board: yielded to the interactive menu");

	// The connection is the claim. Nothing else is read from it: the menu's
	// exit, kill or crash all close it, and that is the signal to take over.
	for (;;) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		if (poll(&pfd, 1, 250) > 0) {
			char scratch[64];
			ssize_t n = read(fd, scratch, sizeof scratch);
			if (n <= 0)
				break;      // closed, or the menu is gone
		}
		if (!serving)
			break;
	}
	close(fd);

	WEB_INFO("board: the interactive menu let go, taking the hardware back");
	if (handlers.resume)
		handlers.resume();
	webevents_release_board();
	WEB_INFO("board: the machine is back");
}

static void serve_loop(boardclaim_handlers_c handlers) {
	while (serving) {
		struct pollfd pfd = { listen_fd, POLLIN, 0 };
		if (poll(&pfd, 1, 250) <= 0)
			continue;
		int fd = accept(listen_fd, nullptr, nullptr);
		if (fd < 0)
			continue;
		std::string line;
		if (!read_line(fd, 5000, &line) || line != "claim") {
			close(fd);
			continue;
		}
		run_claim(fd, handlers);
	}
}

bool boardclaim_serve(const boardclaim_handlers_c &handlers, std::string *error) {
	std::string path = boardclaim_socket_path();
	struct sockaddr_un addr;
	if (!fill_addr(path, &addr, error))
		return false;

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		if (error != nullptr)
			*error = std::string("socket: ") + strerror(errno);
		return false;
	}
	// A path left behind by a service that did not shut down cleanly is not a
	// listener; bind would refuse it and no menu could ever ask for the board.
	unlink(path.c_str());
	if (bind(listen_fd, (struct sockaddr *) &addr, sizeof addr) < 0
			|| listen(listen_fd, 1) < 0) {
		if (error != nullptr)
			*error = "cannot listen on " + path + ": " + strerror(errno);
		close(listen_fd);
		listen_fd = -1;
		return false;
	}
	// Handing over the board is root's business, like the hardware it hands
	// over.
	chmod(path.c_str(), 0600);

	serving = true;
	serve_thread = std::thread(serve_loop, handlers);
	WEB_INFO("board: listening on %s for the interactive menu", path.c_str());
	return true;
}

void boardclaim_stop_serving(void) {
	if (!serving)
		return;
	serving = false;
	if (serve_thread.joinable())
		serve_thread.join();
	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}
	unlink(boardclaim_socket_path().c_str());
}

// ---- the menu side -------------------------------------------------------

// Both held for the process lifetime: the lock says no second menu runs, and
// the socket says the service has stood aside for this one.
static int lock_fd = -1;
static int claim_fd = -1;

// The lock file carries the holder's pid, so a refusal can name it rather than
// leaving an operator to find out which session is in the way.
static bool take_lock(std::string *error) {
	std::string path = boardclaim_lock_path();
	lock_fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
	if (lock_fd < 0) {
		if (error != nullptr)
			*error = "cannot open " + path + ": " + strerror(errno);
		return false;
	}
	if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0) {
		char pid[32];
		int n = snprintf(pid, sizeof pid, "%ld\n", (long) getpid());
		if (ftruncate(lock_fd, 0) == 0 && n > 0)
			(void) !write(lock_fd, pid, (size_t) n);
		return true;
	}
	if (errno != EWOULDBLOCK) {
		if (error != nullptr)
			*error = "cannot lock " + path + ": " + strerror(errno);
		close(lock_fd);
		lock_fd = -1;
		return false;
	}
	char buf[32] = { 0 };
	ssize_t n = pread(lock_fd, buf, sizeof buf - 1, 0);
	long other = n > 0 ? strtol(buf, nullptr, 10) : 0;
	if (error != nullptr) {
		char msg[160];
		if (other > 0)
			snprintf(msg, sizeof msg,
					"another interactive session holds the board (pid %ld)", other);
		else
			snprintf(msg, sizeof msg, "another interactive session holds the board");
		*error = msg;
	}
	close(lock_fd);
	lock_fd = -1;
	return false;
}

bool boardclaim_take(std::string *error) {
	if (!take_lock(error))
		return false;

	std::string path = boardclaim_socket_path();
	struct sockaddr_un addr;
	if (!fill_addr(path, &addr, error)) {
		boardclaim_release();
		return false;
	}
	claim_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (claim_fd < 0) {
		if (error != nullptr)
			*error = std::string("socket: ") + strerror(errno);
		boardclaim_release();
		return false;
	}
	if (connect(claim_fd, (struct sockaddr *) &addr, sizeof addr) < 0) {
		// Nothing is listening: no service holds the board, so it is already
		// this program's. The lock still stands against a second menu.
		close(claim_fd);
		claim_fd = -1;
		return true;
	}

	printf("The service holds the board; waiting for it to put the machine down.\n");
	fflush(stdout);
	if (write(claim_fd, msg_claim, strlen(msg_claim)) < 0) {
		if (error != nullptr)
			*error = "the service closed the connection before the claim was sent";
		boardclaim_release();
		return false;
	}
	std::string line;
	if (!read_line(claim_fd, yield_timeout_ms, &line)) {
		if (error != nullptr)
			*error = "the service did not give the board up";
		boardclaim_release();
		return false;
	}
	if (line + "\n" != msg_granted) {
		if (error != nullptr)
			*error = line.empty() ? "the service refused the board" : line;
		boardclaim_release();
		return false;
	}
	printf("The board is this session's; the web interface is locked until it ends.\n");
	fflush(stdout);
	return true;
}

void boardclaim_release(void) {
	if (claim_fd >= 0) {
		close(claim_fd);
		claim_fd = -1;
	}
	if (lock_fd >= 0) {
		flock(lock_fd, LOCK_UN);
		close(lock_fd);
		lock_fd = -1;
	}
}
