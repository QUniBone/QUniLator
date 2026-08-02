/* recording_test.cpp: host test of the board-side console recorder.
 *
 * The recorder is the only place a session driven by hand can be captured:
 * output reaches every client, but each client's input goes straight to the
 * line, so no client sees what another typed. These checks are about the file
 * it leaves behind being a valid asciicast that carries both directions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "webrecording.hpp"
#include "webconsole_channel.hpp"

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what) {
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static std::string slurp(const std::string &path) {
	FILE *f = fopen(path.c_str(), "r");
	if (f == nullptr)
		return "";
	std::string out;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		out.append(buf, n);
	fclose(f);
	return out;
}

static std::vector<std::string> lines_of(const std::string &s) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start < s.size()) {
		size_t nl = s.find('\n', start);
		if (nl == std::string::npos)
			break;
		out.push_back(s.substr(start, nl - start));
		start = nl + 1;
	}
	return out;
}

// a sink that discards; the channel needs one
static int null_send(void *, const char *, size_t) {
	return 1;
}

int main(void) {
	char dir[] = "/tmp/qcon-rec-XXXXXX";
	if (mkdtemp(dir) == nullptr) {
		fprintf(stderr, "cannot make a temp dir\n");
		return 1;
	}
	const std::string path = std::string(dir) + "/session.cast";

	/* 1. a recording is a valid v3 cast carrying both directions in order */
	{
		console_recorder_c rec;
		check(!rec.recording(), "a fresh recorder is not recording");
		check(rec.start(path, "console ext").empty(), "start succeeds");
		check(rec.recording(), "and it is then recording");
		rec.output("login: ", 7);
		rec.input("root", 4);
		rec.output("root\r\n", 6);
		rec.stop();
		check(!rec.recording(), "stop ends the recording");

		std::vector<std::string> lines = lines_of(slurp(path));
		check(lines.size() == 5, "header, three events and the exit line");
		check(lines[0].find("\"version\":3") != std::string::npos,
				"the header declares asciicast v3");
		check(lines[0].find("\"title\":\"console ext\"") != std::string::npos,
				"and carries the title");
		check(lines[1].find(",\"o\",\"login: \"]") != std::string::npos,
				"output is an 'o' event");
		check(lines[2].find(",\"i\",\"root\"]") != std::string::npos,
				"input is an 'i' event -- the direction only the board sees");
		check(lines[4].find("\"x\"") != std::string::npos,
				"a finished recording ends with an exit event");
	}

	/* 2. control bytes and high bytes survive the round trip as escapes */
	{
		console_recorder_c rec;
		check(rec.start(path, "escapes").empty(), "start for the escape case");
		const char raw[] = { 'A', '\x1b', '[', '2', 'J', '\r', '\n', '\x7f', (char) 0xff };
		rec.output(raw, sizeof raw);
		rec.stop();
		std::vector<std::string> lines = lines_of(slurp(path));
		check(lines[1].find("\\u001b") != std::string::npos,
				"an escape byte is written as \\u001b");
		check(lines[1].find("\\r\\n") != std::string::npos,
				"CR and LF keep their short forms");
		check(lines[1].find("\\u00ff") != std::string::npos,
				"a byte above 0x7f survives as its latin1 code point");
	}

	/* 3. the size cap stops the recording rather than filling the disk */
	{
		console_recorder_c rec;
		check(rec.start(path, "capped", 200).empty(), "start with a small cap");
		std::string blob(64, 'x');
		for (int i = 0; i < 20 && rec.recording(); i++)
			rec.output(blob.data(), blob.size());
		check(!rec.recording(), "the recorder stops itself at the cap");
		std::string body = slurp(path);
		check(body.find("size cap") != std::string::npos,
				"and says so in the file");
	}

	/* 4. nothing is written when no recording was started */
	{
		console_recorder_c rec;
		rec.output("ignored", 7);
		rec.input("ignored", 7);
		check(!rec.recording(), "output and input are dropped when idle");
	}

	/* 5. a channel feeds its recorder the output it broadcasts */
	{
		console_channel_c ch(null_send);
		console_recorder_c rec;
		check(rec.start(path, "channel").empty(), "start for the channel case");
		ch.set_recorder(&rec);
		ch.append("hello", 5);
		ch.set_recorder(nullptr);
		ch.append("unrecorded", 10);
		rec.stop();
		std::string body = slurp(path);
		check(body.find("\"hello\"") != std::string::npos,
				"the channel's output reaches the recorder");
		check(body.find("unrecorded") == std::string::npos,
				"and stops when the recorder is detached");
	}

	unlink(path.c_str());
	rmdir(dir);
	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
