/* console_channel_test.cpp: host test of the console history channel

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any source file header for the full text.

   console_channel_c operates on raw bytes and a plain send callback, so it runs
   on the development host with no civetweb and no BeagleBone. The client sink is
   a synthetic mock: a struct that records what it received and can be scripted to
   report itself dead, exercising the three behaviours the console plan calls out:

     - a late-joining client receives the exact retained snapshot, then the live
       stream, in order and with no duplicated byte;
     - the ring drops the oldest bytes once it passes the 256 KB cap;
     - a client that reports dead on the snapshot is dropped, not inserted, and
       one that reports dead on a live append is dropped too.

   Build & run: 10.05_web/tools/run_config_test.sh (runs this alongside the
   configuration-model test).
*/

#include <cstddef>
#include <cstdio>
#include <string>

#include "webconsole_channel.hpp"

/*** a synthetic client sink ***/
struct mock_client {
	std::string got;       // everything the channel sent this client
	bool alive = true;     // when false, every send reports dead
	int sends = 0;         // successful send calls
};

// Matches console_channel_c::send_fn_t. The client handle is a mock_client.
static int mock_send(void *client, const char *data, size_t len) {
	mock_client *m = (mock_client *) client;
	if (!m->alive)
		return -1;
	m->got.append(data, len);
	m->sends++;
	return 1;
}

/*** test scaffolding ***/
static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what) {
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

int main(void) {
	/* 1. a late-joining client gets the retained snapshot then the live stream,
	      in order, with no gap and no duplicated byte */
	{
		console_channel_c ch(mock_send);
		ch.append("hello", 5);          // history before anyone connects

		mock_client late;
		ch.add_client(&late);           // replay: "hello"
		check(late.got == "hello", "late client replays the retained history");

		ch.append(" world", 6);         // live stream after joining
		check(late.got == "hello world", "late client then follows the live stream");
		check(late.sends == 2, "snapshot and live arrive as distinct sends");
		check(ch.client_count() == 1, "the live client stays registered");
	}

	/* 2. an empty ring replays nothing; the client still joins and streams */
	{
		console_channel_c ch(mock_send);
		mock_client c;
		ch.add_client(&c);
		check(c.got.empty() && c.sends == 0, "empty ring sends no snapshot");
		check(ch.client_count() == 1, "client joins even with an empty ring");
		ch.append("x", 1);
		check(c.got == "x", "client streams after an empty-ring join");
	}

	/* 3. the ring drops the oldest bytes past the 256 KB cap */
	{
		console_channel_c ch(mock_send);
		const size_t cap = console_channel_c::default_cap; // 256 KB
		std::string a(200 * 1024, 'A');
		std::string b(100 * 1024, 'B');
		ch.append(a.data(), a.size());
		ch.append(b.data(), b.size());  // 300 KB total, 44 KB over the cap
		check(ch.ring_size() == cap, "ring is capped at 256 KB");

		mock_client c;
		ch.add_client(&c);              // snapshot == the retained tail
		check(c.got.size() == cap, "snapshot is exactly the capped ring");
		// the tail is the last 156 KB of 'A' followed by all 100 KB of 'B'
		size_t kept_a = cap - b.size();
		check(c.got.compare(0, kept_a, std::string(kept_a, 'A')) == 0,
				"the surviving head is the youngest 'A' bytes");
		check(c.got.compare(kept_a, b.size(), b) == 0,
				"the whole youngest 'B' run survives");
		check(c.got[0] == 'A' && c.got[c.got.size() - 1] == 'B',
				"oldest bytes dropped, newest retained");
	}

	/* 4. a client that reports dead on the snapshot is dropped, not inserted */
	{
		console_channel_c ch(mock_send);
		ch.append("history", 7);

		mock_client dead;
		dead.alive = false;
		ch.add_client(&dead);
		check(ch.client_count() == 0, "dead-on-snapshot client is not inserted");
		check(dead.got.empty(), "dead-on-snapshot client received nothing");

		// a subsequent append must not touch the dropped client
		ch.append("more", 4);
		check(dead.got.empty() && dead.sends == 0,
				"dropped client gets no live bytes");
	}

	/* 5. a client that reports dead on a live append is dropped */
	{
		console_channel_c ch(mock_send);
		mock_client c;
		ch.add_client(&c);              // empty ring, joins cleanly
		check(ch.client_count() == 1, "client joined");
		c.alive = false;               // it dies before the next append
		ch.append("gone", 4);
		check(ch.client_count() == 0, "dead-on-append client is removed");
	}

	/* 6. a healthy client survives a dead sibling in the same append */
	{
		console_channel_c ch(mock_send);
		mock_client good, bad;
		ch.add_client(&good);
		ch.add_client(&bad);
		check(ch.client_count() == 2, "both clients joined");
		bad.alive = false;
		ch.append("ok", 2);
		check(good.got == "ok", "healthy client still receives the broadcast");
		check(ch.client_count() == 1, "only the dead client is dropped");
	}

	/* 7. remove_client and clear_clients forget clients but keep the ring */
	{
		console_channel_c ch(mock_send);
		ch.append("keep", 4);
		mock_client c;
		ch.add_client(&c);
		ch.remove_client(&c);
		check(ch.client_count() == 0, "remove_client forgets the client");
		check(ch.ring_size() == 4, "remove_client leaves the ring intact");

		mock_client d;
		ch.add_client(&d);
		ch.clear_clients();
		check(ch.client_count() == 0, "clear_clients forgets every client");
		check(ch.ring_size() == 4, "clear_clients leaves the ring intact");
		// a fresh client still replays the surviving history
		mock_client e;
		ch.add_client(&e);
		check(e.got == "keep", "the ring survives client churn");
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
