/* cow_image_test.cpp: host test of the copy-on-write disk image (storageimage.cpp)

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   storageimage_cow_c is plain file I/O on a base image, its sparse ".ovl"
   overlay and the ".ovl.map" bitmap sidecar, so it runs on the development
   host. The test drives it the way a drive does and checks what the disk reads
   back after each of the ways an image is left: closed, crashed with the
   overlay open, written by an overlay that filled only single blocks, and
   probed while no drive holds it.

   The overlay's allocated regions stand in for the bitmap after a crash, so
   the result depends on the filesystem reporting holes; run it on Linux.

   Build & run: 10.05_web/tools/run_config_test.sh
*/

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logger.hpp"
#include "storageimage.hpp"

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

static const unsigned BLOCK = 512;
static const unsigned BLOCKS = 64;

static bool exists(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// The content the disk should read: block b of the base is all (b + 1).
static std::vector<uint8_t> base_content()
{
	std::vector<uint8_t> v(BLOCKS * BLOCK);
	for (unsigned b = 0; b < BLOCKS; b++)
		memset(&v[b * BLOCK], (int) (b + 1), BLOCK);
	return v;
}

static void write_file(const std::string &path, const std::vector<uint8_t> &data)
{
	FILE *f = fopen(path.c_str(), "wb");
	fwrite(data.data(), 1, data.size(), f);
	fclose(f);
}

static bool reads_as(storageimage_cow_c &cow, const std::vector<uint8_t> &expect)
{
	std::vector<uint8_t> got(expect.size());
	cow.read(got.data(), 0, (unsigned) got.size());
	return got == expect;
}

static void write_block(storageimage_cow_c &cow, std::vector<uint8_t> &expect,
		unsigned block, unsigned offset, unsigned len, uint8_t value)
{
	std::vector<uint8_t> data(len, value);
	cow.write(data.data(), block * BLOCK + offset, len);
	memset(&expect[block * BLOCK + offset], value, len);
}

int main()
{
	logger = new logger_c();

	char dir_template[] = "/tmp/cow_image_testXXXXXX";
	std::string dir = mkdtemp(dir_template);
	std::string base = dir + "/disk.dsk";
	std::string ovl = base + ".ovl", map = base + ".ovl.map";
	std::vector<uint8_t> expect = base_content();
	write_file(base, expect);

	uint64_t dirty = 0, bytes = 0;
	check(!storageimage_cow_c::probe(base, &dirty, &bytes),
			"an image with no overlay probes as having none");

	// Writes land in the overlay and read back; the map is absent while open.
	{
		storageimage_cow_c cow(base);
		check(cow.open(nullptr, false), "opens");
		write_block(cow, expect, 3, 0, BLOCK, 0xaa);
		write_block(cow, expect, 20, 100, 10, 0xbb);
		check(reads_as(cow, expect), "written blocks read back over the base");
		check(!exists(map), "no map while the image is open");
		check(cow.dirty_block_count() >= 2, "the written blocks count as dirty");

		// A crash: a second view of the files with the first never closed has
		// only the overlay's allocated regions to go by.
		storageimage_cow_c crashed(base);
		check(crashed.open(nullptr, false), "reopens with no map");
		check(reads_as(crashed, expect),
				"after a crash the disk reads what was written and the base around it");
		crashed.close();
		unlink(map.c_str());
	}
	check(exists(map), "closing writes the map");

	// A map left by a clean close is not trusted past the next open: a write
	// after it survives a crash.
	{
		storageimage_cow_c cow(base);
		check(cow.open(nullptr, false), "opens over a map");
		write_block(cow, expect, 40, 0, BLOCK, 0xcc);
		storageimage_cow_c crashed(base);
		check(crashed.open(nullptr, false), "reopens after a crash");
		check(reads_as(crashed, expect), "a write after the last close survives a crash");
		crashed.close();
		unlink(map.c_str());
	}

	// An overlay holding a single written block of a page, with a map naming
	// just that block, reads right and has the page completed on open.
	{
		int fd = open(ovl.c_str(), O_WRONLY);
		std::vector<uint8_t> one(BLOCK, 0xdd);
		check(pwrite(fd, one.data(), BLOCK, 50 * BLOCK) == (ssize_t) BLOCK, "plant a lone block");
		close(fd);
		memset(&expect[50 * BLOCK], 0xdd, BLOCK);
		// the map: 24 header bytes, then one bit per block
		fd = open(map.c_str(), O_RDWR);
		uint8_t bits = 0;
		check(pread(fd, &bits, 1, 24 + 50 / 8) == 1, "read the map");
		bits |= 1 << (50 % 8);
		check(pwrite(fd, &bits, 1, 24 + 50 / 8) == 1, "add the lone block to the map");
		close(fd);

		storageimage_cow_c cow(base);
		check(cow.open(nullptr, false), "opens a partly written page");
		check(reads_as(cow, expect), "the lone block reads over a base around it");
		storageimage_cow_c crashed(base);
		check(crashed.open(nullptr, false), "reopens with no map");
		check(reads_as(crashed, expect), "the completed page rebuilds faithfully");
		crashed.close();
		unlink(map.c_str());
	}

	// Probed while closed: the count the open image reports.
	uint64_t open_dirty;
	{
		storageimage_cow_c cow(base);
		cow.open(nullptr, false);
		open_dirty = cow.dirty_block_count();
		cow.close();
	}
	check(storageimage_cow_c::probe(base, &dirty, &bytes), "a closed overlay probes");
	check(dirty == open_dirty, "the probe counts what the open image counts");
	check(bytes > 0, "the probe reports the overlay's footprint");
	check(exists(map), "probing leaves the map in place");

	// Export, commit and discard on an image no drive holds.
	{
		storageimage_cow_c cow(base);
		cow.open(nullptr, false);
		std::string flat = dir + "/flat.dsk";
		cow.export_to(flat);
		storageimage_cow_c flat_image(flat);
		flat_image.open(nullptr, false);
		check(reads_as(flat_image, expect), "the export is base plus overlay");
		flat_image.close();

		check(cow.commit(), "commits");
		check(cow.dirty_block_count() == 0, "a commit leaves nothing dirty");
		check(reads_as(cow, expect), "the committed disk reads the same");
		cow.close();
	}
	{
		storageimage_cow_c cow(base);
		cow.open(nullptr, false);
		write_block(cow, expect, 7, 0, BLOCK, 0xee);
		std::vector<uint8_t> committed = expect;
		memset(&committed[7 * BLOCK], 8, BLOCK);
		cow.discard();
		check(reads_as(cow, committed), "a discard returns to the base");
		cow.close();
	}

	std::string cleanup = "rm -rf '" + dir + "'";
	(void) !system(cleanup.c_str());

	printf("cow_image_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
