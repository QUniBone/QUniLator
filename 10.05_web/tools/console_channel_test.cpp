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
	std::string got;       // everything the channel sent this client (binary)
	std::string ctrl;      // control frames the channel sent this client (text)
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

// Matches console_channel_c::send_text_fn_t: records the control frame.
static int mock_send_text(void *client, const char *data, size_t len) {
	mock_client *m = (mock_client *) client;
	if (!m->alive)
		return -1;
	m->ctrl.append(data, len);
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

	/* 8. with a text callback the channel names exactly one answerer among the
	      clients and promotes another when the answerer leaves */
	{
		const std::string ANS = "{\"answerer\":true}";
		console_channel_c ch(mock_send, mock_send_text);
		mock_client a, b, c;
		ch.add_client(&a);
		check(a.ctrl == ANS, "first client is named the answerer");
		ch.add_client(&b);
		ch.add_client(&c);
		check(b.ctrl.empty() && c.ctrl.empty(), "later clients are viewers");

		ch.remove_client(&a);           // the answerer leaves
		int promoted = (b.ctrl == ANS) + (c.ctrl == ANS);
		check(promoted == 1, "exactly one viewer is promoted when the answerer leaves");

		// removing the other viewer must not re-designate the sitting answerer
		mock_client *ansr = (b.ctrl == ANS) ? &b : &c;
		mock_client *viewer = (ansr == &b) ? &c : &b;
		std::string before = ansr->ctrl;
		ch.remove_client(viewer);
		check(ansr->ctrl == before, "removing a viewer leaves the answerer unchanged");

		ch.remove_client(ansr);
		check(ch.client_count() == 0, "all clients gone");
		mock_client d;
		ch.add_client(&d);
		check(d.ctrl == ANS, "a fresh client is named answerer when none exists");
	}

	/* 9. a dead first client is not designated; the next live one is */
	{
		const std::string ANS = "{\"answerer\":true}";
		console_channel_c ch(mock_send, mock_send_text);
		ch.append("h", 1);              // non-empty ring so the dead client is detected
		mock_client dead, live;
		dead.alive = false;
		ch.add_client(&dead);           // send_text reports dead -> not designated, not inserted
		check(ch.client_count() == 0 && dead.ctrl.empty(), "dead client is not the answerer");
		ch.add_client(&live);
		check(live.ctrl == ANS, "the next live client becomes the answerer");
	}

	/* 11. the replay strips terminal-query escapes (DA/DECID/DSR) so a connecting
	      terminal never re-answers a query buried in the history; the live stream
	      is passed through untouched */
	{
		console_channel_c ch(mock_send, mock_send_text);
		// A <DA> B <DECID> C <DSR> D, escapes interleaved with printable bytes
		const char raw[] = { 'A', 0x1b, '[', 'c', 'B', 0x1b, 'Z', 'C',
				0x1b, '[', '6', 'n', 'D' };
		ch.append(raw, sizeof(raw));
		mock_client c;
		ch.add_client(&c);
		check(c.got == "ABCD", "replay strips DA, DECID and DSR query escapes");
		// a real cursor move (CSI H) is display, not a query — it must survive
		console_channel_c ch2(mock_send, mock_send_text);
		const char disp[] = { 'X', 0x1b, '[', 'H', 'Y' };
		ch2.append(disp, sizeof(disp));
		mock_client d;
		ch2.add_client(&d);
		check(d.got == std::string("X\x1b[HY", 5), "replay keeps non-query escapes");
		// the live stream is not stripped: the answerer must see live queries
		const char liveq[] = { 0x1b, '[', 'c' };
		ch.append(liveq, sizeof(liveq));
		check(c.got == std::string("ABCD\x1b[c", 7), "live query passes through unstripped");
	}

	/* 10. a null text callback leaves every client a plain mirror (DL11 taps) */
	{
		console_channel_c ch(mock_send);   // no text callback
		mock_client a, b;
		ch.add_client(&a);
		ch.add_client(&b);
		check(a.ctrl.empty() && b.ctrl.empty(),
				"no answerer designation without a text callback");
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
