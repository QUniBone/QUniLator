/* simh_tape_test.cpp: host test of the SIMH .tap tape image (simh_tape.cpp)

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   The .tap record layer is a pure file format with no PRU/qunibus dependency,
   so it runs on the development host. It exercises the read/write/space/rewind
   primitives and verifies the on-disk bytes match the SIMH format, so an image
   this emulator writes interchanges with SimH.

   Build & run: 10.05_web/tools/run_config_test.sh
*/

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "simh_tape.hpp"

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

// read a 32-bit little-endian marker from the raw file at an offset
static uint32_t raw_marker(const char *path, uint64_t at)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0xdeadbeef;
	fseek(f, (long) at, SEEK_SET);
	uint8_t b[4] = { 0, 0, 0, 0 };
	size_t n = fread(b, 1, 4, f);
	fclose(f);
	if (n != 4)
		return 0xdeadbeef;
	return (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16)
			| ((uint32_t) b[3] << 24);
}

int main(void)
{
	char tmpl[] = "/tmp/simhtape_XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) {
		perror("mkstemp");
		return 2;
	}
	close(fd);
	std::string path = tmpl;

	typedef simh_tape_c::result_t R;

	/* 1. write a tape: two records, a tape mark, one record, two tape marks */
	{
		simh_tape_c t;
		check(t.open(path, false), "open read/write");
		check(!t.is_readonly(), "mounted read/write");
		check(t.at_bot(), "starts at BOT");
		check(t.write_record((const uint8_t *) "HELLO", 5) == simh_tape_c::R_RECORD,
				"write record HELLO (odd length -> padded)");
		check(t.write_record((const uint8_t *) "WORLD!", 6) == simh_tape_c::R_RECORD,
				"write record WORLD!");
		check(t.write_tape_mark() == simh_tape_c::R_RECORD, "write tape mark");
		check(t.write_record((const uint8_t *) "END", 3) == simh_tape_c::R_RECORD,
				"write record END");
		check(t.write_tape_mark() == simh_tape_c::R_RECORD, "write tape mark 1 of 2");
		check(t.write_tape_mark() == simh_tape_c::R_RECORD, "write tape mark 2 of 2 (logical EOT)");
		t.close();
	}

	/* 2. the on-disk bytes are SIMH format: leading marker, padded data, trailing
	      marker; a 5-byte record occupies 4 + 6 + 4 = 14 bytes */
	check(raw_marker(path.c_str(), 0) == 5, "record 1 leading marker = length 5");
	check(raw_marker(path.c_str(), 10) == 5, "record 1 trailing marker = length 5 (data padded to 6)");
	check(raw_marker(path.c_str(), 14) == 6, "record 2 leading marker = length 6");
	check(raw_marker(path.c_str(), 24) == 6, "record 2 trailing marker = length 6");
	check(raw_marker(path.c_str(), 28) == simh_tape_c::MTR_TMK, "tape mark marker = 0");

	/* 3. read the tape back forward */
	{
		simh_tape_c t;
		check(t.open(path, false), "reopen");
		uint8_t buf[64];
		uint32_t len = 0;
		R r = t.read_forward(buf, sizeof buf, &len);
		check(r == simh_tape_c::R_RECORD && len == 5 && memcmp(buf, "HELLO", 5) == 0,
				"read record 1 = HELLO");
		r = t.read_forward(buf, sizeof buf, &len);
		check(r == simh_tape_c::R_RECORD && len == 6 && memcmp(buf, "WORLD!", 6) == 0,
				"read record 2 = WORLD!");
		r = t.read_forward(buf, sizeof buf, &len);
		check(r == simh_tape_c::R_TAPE_MARK, "read hits the tape mark");
		r = t.read_forward(buf, sizeof buf, &len);
		check(r == simh_tape_c::R_RECORD && len == 3 && memcmp(buf, "END", 3) == 0,
				"read record 3 = END");
		check(t.read_forward(buf, sizeof buf, &len) == simh_tape_c::R_TAPE_MARK, "tape mark");
		check(t.read_forward(buf, sizeof buf, &len) == simh_tape_c::R_TAPE_MARK, "tape mark (EOT)");
		check(t.read_forward(buf, sizeof buf, &len) == simh_tape_c::R_END_OF_MEDIUM,
				"past the last mark reads end of medium");
		t.close();
	}

	/* 4. space forward then reverse land on record boundaries */
	{
		simh_tape_c t;
		t.open(path, false);
		uint32_t len = 0;
		check(t.space_forward(&len) == simh_tape_c::R_RECORD && len == 5, "space over record 1 (len 5)");
		check(t.space_forward(&len) == simh_tape_c::R_RECORD && len == 6, "space over record 2 (len 6)");
		check(t.space_forward(&len) == simh_tape_c::R_TAPE_MARK, "space over the tape mark");
		// now positioned before END; reverse back over the mark and record 2
		check(t.space_reverse(&len) == simh_tape_c::R_TAPE_MARK, "reverse over the tape mark");
		check(t.space_reverse(&len) == simh_tape_c::R_RECORD && len == 6, "reverse over record 2");
		uint8_t buf[64];
		R r = t.read_forward(buf, sizeof buf, &len);
		check(r == simh_tape_c::R_RECORD && len == 6 && memcmp(buf, "WORLD!", 6) == 0,
				"read after reverse re-reads record 2");
		t.rewind();
		check(t.at_bot(), "rewind returns to BOT");
		check(t.space_reverse(&len) == simh_tape_c::R_BEGIN_OF_TAPE, "reverse at BOT reports begin of tape");
		t.close();
	}

	/* 5. a read-only mount refuses writes */
	{
		simh_tape_c t;
		check(t.open(path, true), "open read-only");
		check(t.is_readonly(), "mounted read-only");
		check(t.write_record((const uint8_t *) "x", 1) == simh_tape_c::R_ERROR,
				"write refused on a read-only tape");
		t.close();
	}

	/* 6. a write in the middle truncates the tape after it (real-hardware EOT) */
	{
		simh_tape_c t;
		t.open(path, false);
		uint32_t len = 0;
		t.space_forward(&len); // past record 1
		check(t.write_record((const uint8_t *) "NEW", 3) == simh_tape_c::R_RECORD,
				"overwrite from mid-tape");
		// file is now record1 (14 bytes) + record NEW (4+4+4 = 12) = 26 bytes
		check(t.file_size() == 26, "tape truncated to the new end after a mid-tape write");
		t.rewind();
		uint8_t buf[64];
		t.read_forward(buf, sizeof buf, &len); // record 1
		R r = t.read_forward(buf, sizeof buf, &len);
		check(r == simh_tape_c::R_RECORD && len == 3 && memcmp(buf, "NEW", 3) == 0,
				"the overwriting record reads back");
		check(t.read_forward(buf, sizeof buf, &len) == simh_tape_c::R_END_OF_MEDIUM,
				"nothing survives past the overwrite");
		t.close();
	}

	unlink(path.c_str());
	printf("simh_tape_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
