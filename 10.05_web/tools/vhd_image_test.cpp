/* vhd_image_test.cpp: host test of the VHD disk image (storageimage_vhd.cpp)

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   storageimage_vhd_c is plain file I/O on a VHD file, so it runs on the
   development host. The test drives it the way a drive does - reads and writes
   of any length at any position, a sector split in two the way an RL02's
   256-byte sectors split one - against a model of the disk held in memory, and
   checks what the disk reads back after the image is closed and opened again.

   Where qemu-img is on the PATH it is the second opinion: it reads every VHD
   the test has written back to a raw image, which has to equal the model byte
   for byte, and it makes the fixed and dynamic VHDs another program writes. CI
   installs it; a run without it says so and skips those checks.

   A VHD SIMH wrote is read and written when VHD_TEST_SIMH names one; its raw
   twin, VHD_TEST_SIMH_RAW, is what it has to read as.

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
#include "storageimage_vhd.hpp"

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string &what)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what.c_str());
	}
}

static const unsigned SECTOR = 512;
static const uint32_t BLOCK = 2 * 1024 * 1024; // what create_dynamic and qemu-img write

static std::string dir;
static bool have_qemu = false;

static std::vector<uint8_t> read_file(const std::string &path)
{
	std::vector<uint8_t> v;
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return v;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		v.insert(v.end(), buf, buf + n);
	fclose(f);
	return v;
}

static void write_file(const std::string &path, const std::vector<uint8_t> &data)
{
	FILE *f = fopen(path.c_str(), "wb");
	fwrite(data.data(), 1, data.size(), f);
	fclose(f);
}

static uint64_t file_size(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 ? (uint64_t) st.st_size : 0;
}

static int run(const std::string &cmd)
{
	return system((cmd + " >/dev/null 2>&1").c_str());
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
	put_be32(p, (uint32_t) (v >> 32));
	put_be32(p + 4, (uint32_t) v);
}

static uint32_t checksum(const uint8_t *p, size_t len, size_t at)
{
	uint32_t sum = 0;
	for (size_t i = 0; i < len; i++)
		if (i < at || i >= at + 4)
			sum += p[i];
	return ~sum;
}

// Content that differs at every byte, so a misplaced or repeated buffer shows.
static std::vector<uint8_t> pattern(size_t len, unsigned seed)
{
	std::vector<uint8_t> v(len);
	for (size_t i = 0; i < len; i++)
		v[i] = (uint8_t) (i * 13 + i / 509 + seed * 101 + 1);
	return v;
}

// Write through the image and into the model alike.
static void put(storageimage_vhd_c &img, std::vector<uint8_t> &model, uint64_t pos,
		const std::vector<uint8_t> &data)
{
	std::vector<uint8_t> copy = data;
	img.write(copy.data(), pos, (unsigned) copy.size());
	memcpy(&model[pos], data.data(), data.size());
}

static bool reads_as(storageimage_vhd_c &img, const std::vector<uint8_t> &model)
{
	std::vector<uint8_t> got(model.size());
	// in uneven pieces, so a read crosses blocks and splits sectors
	const unsigned piece = 3 * SECTOR + 256;
	for (uint64_t pos = 0; pos < model.size(); pos += piece) {
		unsigned n = (unsigned) std::min<uint64_t>(piece, model.size() - pos);
		img.read(&got[pos], pos, n);
	}
	return got == model;
}

// What qemu-img reads the VHD as, the disk at the size its footer gives.
static bool qemu_reads_as(const std::string &vhd, const std::vector<uint8_t> &model)
{
	std::string raw = vhd + ".qemu.raw";
	unlink(raw.c_str());
	if (run("qemu-img convert --image-opts driver=vpc,force_size_calc=current_size,file.filename='"
			+ vhd + "' -O raw '" + raw + "'") != 0)
		return false;
	bool same = read_file(raw) == model;
	unlink(raw.c_str());
	return same;
}

// Writes of every shape a drive makes, against the model.
static void exercise(storageimage_vhd_c &img, std::vector<uint8_t> &model, const std::string &what)
{
	uint64_t size = model.size();
	put(img, model, 0, pattern(SECTOR, 1));                       // the first sector
	put(img, model, 5 * SECTOR + 256, pattern(256, 2));           // half a sector, an RL02 sector
	put(img, model, 9 * SECTOR, pattern(256, 3));                 // the other half of another one
	put(img, model, BLOCK - 700, pattern(1400, 4));               // across a block boundary
	put(img, model, 3ull * BLOCK + 4096, pattern(10 * SECTOR, 5)); // into a block of its own
	put(img, model, size - 300, pattern(300, 6));                 // the last bytes of the disk
	put(img, model, 5 * SECTOR, pattern(SECTOR * 2, 7));          // over a sector written before
	check(reads_as(img, model), what + ": reads back what was written");
}

static std::string make_raw(const std::string &name, const std::vector<uint8_t> &data)
{
	std::string path = dir + "/" + name;
	write_file(path, data);
	return path;
}

// A fixed VHD built here: the disk's bytes and a footer.
static std::string make_fixed(const std::string &name, const std::vector<uint8_t> &disk)
{
	std::vector<uint8_t> f(SECTOR, 0);
	memcpy(&f[0], "conectix", 8);
	put_be32(&f[8], 2);
	put_be32(&f[12], 0x00010000);
	put_be64(&f[16], UINT64_MAX);
	put_be64(&f[40], disk.size());
	put_be64(&f[48], disk.size());
	put_be32(&f[60], 2);
	put_be32(&f[64], checksum(f.data(), SECTOR, 64));
	std::vector<uint8_t> file = disk;
	file.insert(file.end(), f.begin(), f.end());
	return make_raw(name, file);
}

static void test_dynamic_created_here()
{
	// 5 blocks and a bit: the last block is only partly the disk's
	const uint64_t size = 4ull * BLOCK + 3 * SECTOR;
	std::string path = dir + "/new.vhd";
	std::string err;
	check(storageimage_vhd_c::create_dynamic(path, size, &err), "a dynamic VHD is created: " + err);
	check(!storageimage_vhd_c::create_dynamic(path, size, &err), "an existing file is not overwritten");
	check(!storageimage_vhd_c::create_dynamic(dir + "/odd.vhd", size + 1, &err),
			"a disk that is no whole number of sectors is refused");
	uint64_t disk_size = 0;
	check(storageimage_vhd_c::is_vhd_file(path, &disk_size) && disk_size == size,
			"a created VHD is recognised, at its size");
	uint64_t empty_size = file_size(path);
	check(empty_size < 8 * 1024, "a created VHD holds no block");
	if (have_qemu) {
		check(qemu_reads_as(path, std::vector<uint8_t>(size, 0)), "qemu-img reads a created VHD as zeros");
	}

	std::vector<uint8_t> model(size, 0);
	{
		storageimage_vhd_c img(path);
		check(img.size() == 0, "a VHD not open has no size");
		check(img.open(nullptr, false), "a created VHD opens");
		check(!img.is_readonly(), "a writable VHD opens for writing");
		check(img.size() == size, "size() is the disk's size, not the file's");
		check(reads_as(img, model), "a created VHD reads as zeros");
		exercise(img, model, "created dynamic VHD");
		// past the end: the part inside the disk lands, the rest is dropped
		std::vector<uint8_t> tail = pattern(1024, 8);
		img.write(tail.data(), size - 512, 1024);
		memcpy(&model[size - 512], tail.data(), 512);
		check(reads_as(img, model), "a write past the end keeps the part inside the disk");
		check(img.size() == size, "a write past the end leaves the disk's size");
		img.close();
	}
	check(file_size(path) > empty_size, "the file grew by the blocks written");
	{
		storageimage_vhd_c img(path);
		check(img.open(nullptr, false), "a written VHD opens again");
		check(reads_as(img, model), "a written VHD reads the same after reopening");
		// rewriting allocated blocks moves nothing
		uint64_t before = file_size(path);
		put(img, model, BLOCK - 700, pattern(1400, 9));
		check(file_size(path) == before, "a write into allocated blocks does not grow the file");
		img.close();
	}
	if (have_qemu) {
		check(qemu_reads_as(path, model), "qemu-img reads a written VHD as the model");
	}

	// truncate empties the disk and keeps its size
	{
		storageimage_vhd_c img(path);
		img.open(nullptr, false);
		check(img.truncate(), "a dynamic VHD truncates");
		check(img.size() == size, "truncating keeps the disk's size");
		std::fill(model.begin(), model.end(), 0);
		check(reads_as(img, model), "a truncated VHD reads as zeros");
		check(file_size(path) == empty_size, "a truncated VHD holds no block");
		put(img, model, 2ull * BLOCK, pattern(SECTOR, 10));
		img.close();
		img.open(nullptr, false);
		check(reads_as(img, model), "a truncated VHD takes writes again");
		img.close();
	}
	if (have_qemu)
		check(qemu_reads_as(path, model), "qemu-img reads a truncated and rewritten VHD as the model");
}

static void test_fixed()
{
	const uint64_t size = 4ull * BLOCK + 7 * SECTOR;
	std::vector<uint8_t> model = pattern(size, 11);
	std::string path = make_fixed("fixed.dsk", model);
	uint64_t fsize = file_size(path);
	{
		storageimage_vhd_c img(path);
		check(img.open(nullptr, false), "a fixed VHD opens");
		check(img.size() == size, "a fixed VHD's size is the disk's");
		check(reads_as(img, model), "a fixed VHD reads its disk");
		exercise(img, model, "fixed VHD");
		img.close();
	}
	check(file_size(path) == fsize, "writing a fixed VHD does not grow it");
	{
		storageimage_vhd_c img(path);
		img.open(nullptr, false);
		check(reads_as(img, model), "a fixed VHD reads the same after reopening");
		img.close();
	}
	if (have_qemu)
		check(qemu_reads_as(path, model), "qemu-img reads a written fixed VHD as the model");
}

// VHDs qemu-img wrote, fixed and dynamic, from a raw image with data and gaps.
static void test_qemu_written()
{
	if (!have_qemu)
		return;
	const uint64_t size = 6ull * BLOCK;
	std::vector<uint8_t> model(size, 0);
	std::vector<uint8_t> a = pattern(3 * SECTOR, 12), b = pattern(BLOCK, 13);
	memcpy(&model[SECTOR], a.data(), a.size());
	memcpy(&model[4ull * BLOCK], b.data(), b.size());
	std::string raw = make_raw("q.raw", model);
	for (const char *sub : {"dynamic", "fixed"}) {
		std::string what = std::string("qemu-img's ") + sub + " VHD";
		std::string vhd = dir + "/q-" + sub + ".vhd";
		check(run(std::string("qemu-img convert -f raw -O vpc -o subformat=") + sub
				+ ",force_size=on '" + raw + "' '" + vhd + "'") == 0, what + " is made");
		std::vector<uint8_t> m = model;
		storageimage_vhd_c img(vhd);
		check(img.open(nullptr, false), what + " opens");
		check(img.size() == size, what + " has the raw image's size");
		check(reads_as(img, m), what + " reads as the raw image");
		exercise(img, m, what);
		img.close();
		check(img.open(nullptr, false) && reads_as(img, m), what + " reads the same after reopening");
		img.close();
		check(qemu_reads_as(vhd, m), "qemu-img reads the written " + what + " as the model");
	}
}

// What another emulator keeps in the reserved areas stays where it was: SIMH
// writes its drive type into the footer's.
static void test_metadata_kept()
{
	const uint64_t size = 2ull * BLOCK;
	std::string path = dir + "/meta.vhd";
	std::string err;
	storageimage_vhd_c::create_dynamic(path, size, &err);
	std::vector<uint8_t> file = read_file(path);
	uint8_t *head = &file[0], *tail = &file[file.size() - SECTOR];
	const char tag[] = "RL02 metadata of another emulator";
	memcpy(head + 100, tag, sizeof tag);
	put_be32(head + 64, checksum(head, SECTOR, 64));
	memcpy(tail, head, SECTOR);
	write_file(path, file);
	std::vector<uint8_t> footer(head, head + SECTOR);
	std::vector<uint8_t> header(&file[SECTOR], &file[SECTOR] + 1024);

	std::vector<uint8_t> model(size, 0);
	storageimage_vhd_c img(path);
	check(img.open(nullptr, false), "a VHD with metadata opens");
	put(img, model, BLOCK + 5, pattern(700, 14));
	put(img, model, 0, pattern(SECTOR, 15));
	img.close();
	std::vector<uint8_t> after = read_file(path);
	check(std::vector<uint8_t>(after.end() - SECTOR, after.end()) == footer,
			"the moved footer is the footer as found");
	check(std::vector<uint8_t>(after.begin(), after.begin() + SECTOR) == footer,
			"the footer copy is untouched");
	check(std::vector<uint8_t>(after.begin() + SECTOR, after.begin() + SECTOR + 1024) == header,
			"the dynamic header is untouched");
	if (have_qemu)
		check(qemu_reads_as(path, model), "qemu-img reads a VHD with metadata as the model");
}

// A block whose bitmap marks only some of its sectors, as a writer that tracks
// sectors one by one leaves it: an unmarked sector reads as zeros whatever the
// file holds there, and a write into one makes it whole before marking it.
static void test_partial_bitmap()
{
	const uint64_t size = 2ull * BLOCK;
	std::string path = dir + "/partial.vhd";
	std::string err;
	storageimage_vhd_c::create_dynamic(path, size, &err);
	std::vector<uint8_t> model(size, 0);
	{
		storageimage_vhd_c img(path);
		img.open(nullptr, false);
		put(img, model, 0, pattern(16 * SECTOR, 19));
		img.close();
	}
	// block 0 starts right after the table: unmark sectors 0..7
	std::vector<uint8_t> file = read_file(path);
	uint64_t block_at = (uint64_t) ((uint32_t) file[3 * SECTOR] << 24 | (uint32_t) file[3 * SECTOR + 1] << 16
			| (uint32_t) file[3 * SECTOR + 2] << 8 | file[3 * SECTOR + 3]) * SECTOR;
	file[block_at] = 0x00;
	write_file(path, file);
	std::fill(model.begin(), model.begin() + 8 * SECTOR, 0);

	storageimage_vhd_c img(path);
	img.open(nullptr, false);
	check(reads_as(img, model), "an unmarked sector reads as zeros");
	put(img, model, 2 * SECTOR + 256, pattern(256, 20));
	check(reads_as(img, model), "half a sector written into an unmarked one reads with zeros beside it");
	img.close();
	file = read_file(path);
	check(file[block_at] == 0x20, "the sector written is marked, and only it");
	check(std::vector<uint8_t>(file.begin() + block_at + SECTOR + 2 * SECTOR,
			file.begin() + block_at + SECTOR + 2 * SECTOR + 256) == std::vector<uint8_t>(256, 0),
			"the part of a newly marked sector the write did not reach is zeros in the file");
	img.open(nullptr, false);
	check(reads_as(img, model), "a partly marked block reads the same after reopening");
	img.close();
}

static void test_simh_written()
{
	const char *simh = getenv("VHD_TEST_SIMH");
	if (simh == nullptr || file_size(simh) == 0) {
		printf("vhd_image_test: VHD_TEST_SIMH names no SIMH-written VHD, skipping it\n");
		return;
	}
	std::string path = dir + "/simh.vhd";
	write_file(path, read_file(simh));
	std::vector<uint8_t> original = read_file(path);
	std::vector<uint8_t> model;
	const char *raw = getenv("VHD_TEST_SIMH_RAW");
	storageimage_vhd_c img(path);
	check(img.open(nullptr, false), "SIMH's VHD opens");
	if (raw != nullptr) {
		model = read_file(raw);
		check(img.size() == model.size(), "SIMH's VHD has its raw twin's size");
	} else {
		model.resize(img.size());
		img.read(model.data(), 0, (unsigned) model.size());
	}
	check(reads_as(img, model), "SIMH's VHD reads as its raw twin");
	exercise(img, model, "SIMH's VHD");
	img.close();
	std::vector<uint8_t> after = read_file(path);
	check(after.size() >= original.size()
			&& std::equal(original.end() - SECTOR, original.end(), after.end() - SECTOR),
			"SIMH's footer stays as SIMH wrote it");
	check(std::equal(original.begin(), original.begin() + 3 * SECTOR, after.begin()),
			"SIMH's footer copy and dynamic header stay as SIMH wrote them");
	check(img.open(nullptr, false) && reads_as(img, model), "SIMH's VHD reads the same after reopening");
	img.close();
	if (have_qemu)
		check(qemu_reads_as(path, model), "qemu-img reads the written SIMH VHD as the model");
}

static void test_refused()
{
	std::string err;
	const uint64_t size = 2ull * BLOCK;

	// a differencing VHD needs its parent
	{
		std::string path = dir + "/diff.vhd";
		storageimage_vhd_c::create_dynamic(path, size, &err);
		std::vector<uint8_t> file = read_file(path);
		for (uint8_t *f : {&file[0], &file[file.size() - SECTOR]}) {
			put_be32(f + 60, 4);
			put_be32(f + 64, checksum(f, SECTOR, 64));
		}
		write_file(path, file);
		int fd = open(path.c_str(), O_RDONLY);
		storageimage_vhd_c::layout_t layout;
		check(!storageimage_vhd_c::parse(fd, &layout, &err)
				&& err.find("differencing") != std::string::npos,
				"a differencing VHD is refused, by name: " + err);
		close(fd);
		storageimage_vhd_c img(path);
		check(!img.open(nullptr, false) && !img.is_open(), "a drive does not open a differencing VHD");
	}

	// a footer whose checksum does not hold is no footer: the file is raw
	{
		std::string path = dir + "/badsum.dsk";
		storageimage_vhd_c::create_dynamic(path, size, &err);
		std::vector<uint8_t> file = read_file(path);
		file[0] ^= 0x20;
		file[file.size() - SECTOR + 30] ^= 1;
		write_file(path, file);
		check(!storageimage_vhd_c::is_vhd_file(path), "a footer with a wrong checksum is no VHD");
		check(!storageimage_vhd_c::takes(path), "a drive takes a file with a broken footer as raw");
	}

	// a damaged dynamic header
	{
		std::string path = dir + "/badhdr.vhd";
		storageimage_vhd_c::create_dynamic(path, size, &err);
		std::vector<uint8_t> file = read_file(path);
		file[SECTOR + 32] ^= 0x01; // the block size, under the header's checksum
		write_file(path, file);
		storageimage_vhd_c img(path);
		check(!img.open(nullptr, false), "a VHD with a damaged dynamic header is refused");
	}

	// a block table naming a block the file does not hold
	{
		std::string path = dir + "/short.vhd";
		storageimage_vhd_c::create_dynamic(path, size, &err);
		std::vector<uint8_t> file = read_file(path);
		put_be32(&file[3 * SECTOR + 4], 0x1000); // block 1 far past the end
		write_file(path, file);
		storageimage_vhd_c img(path);
		check(!img.open(nullptr, false), "a VHD shorter than its block table is refused");
	}

	// a dynamic VHD whose closing footer is lost is served read-only
	{
		std::string path = dir + "/nofooter.vhd";
		storageimage_vhd_c::create_dynamic(path, size, &err);
		std::vector<uint8_t> model(size, 0);
		{
			storageimage_vhd_c img(path);
			img.open(nullptr, false);
			put(img, model, 100, pattern(SECTOR, 16));
			img.close();
		}
		std::vector<uint8_t> file = read_file(path);
		file.resize(file.size() - SECTOR);
		write_file(path, file);
		check(storageimage_vhd_c::is_vhd_file(path), "a dynamic VHD is recognised by its leading copy");
		storageimage_vhd_c img(path);
		check(img.open(nullptr, false), "a dynamic VHD with its closing footer lost opens");
		check(img.is_readonly(), "a dynamic VHD with its closing footer lost is read-only");
		check(reads_as(img, model), "a dynamic VHD with its closing footer lost reads its blocks");
		img.close();
	}

	// a fixed VHD shorter than its disk
	{
		std::vector<uint8_t> disk = pattern(4 * SECTOR, 17);
		std::string path = make_fixed("shortfixed.vhd", disk);
		std::vector<uint8_t> file = read_file(path);
		put_be64(&file[file.size() - SECTOR + 48], 8 * SECTOR);
		put_be32(&file[file.size() - SECTOR + 64], checksum(&file[file.size() - SECTOR], SECTOR, 64));
		write_file(path, file);
		storageimage_vhd_c img(path);
		check(!img.open(nullptr, false), "a fixed VHD shorter than its disk is refused");
	}
}

// The content decides; a name decides only where there is no file to read yet.
static void test_recognition()
{
	std::string err;
	std::string raw = make_raw("raw.vhd", pattern(8 * SECTOR, 18));
	check(!storageimage_vhd_c::takes(raw), "a raw image named .vhd is taken as raw");
	std::string vhd = dir + "/vhd.dsk";
	storageimage_vhd_c::create_dynamic(vhd, 8 * SECTOR, &err);
	check(storageimage_vhd_c::takes(vhd), "a VHD named .dsk is taken as a VHD");
	check(storageimage_vhd_c::takes(dir + "/missing.VHD"), "a .vhd that is not there yet is a VHD");
	check(!storageimage_vhd_c::takes(dir + "/missing.dsk"), "a .dsk that is not there yet is raw");
	check(!storageimage_vhd_c::takes(""), "no image is no VHD");
	check(!storageimage_vhd_c::is_vhd_file(dir), "a folder is no VHD");

	// a VHD that is not there is not created by a drive
	storageimage_vhd_c img(dir + "/missing.vhd");
	check(!img.open(nullptr, true) && file_size(dir + "/missing.vhd") == 0,
			"a drive creates no VHD that is not there");

	// only its .gz is there: expanded on open
	std::string packed = dir + "/packed.vhd";
	storageimage_vhd_c::create_dynamic(packed, 4 * SECTOR, &err);
	check(run("gzip '" + packed + "'") == 0, "a VHD is compressed");
	check(storageimage_vhd_c::takes(packed), "a compressed VHD is taken by its name");
	storageimage_vhd_c gz(packed);
	check(gz.open(nullptr, false) && gz.size() == 4 * SECTOR, "a compressed VHD is expanded and opens");
	gz.close();

	// a write-protected VHD opens read-only
	std::string ro = dir + "/ro.vhd";
	storageimage_vhd_c::create_dynamic(ro, 4 * SECTOR, &err);
	chmod(ro.c_str(), 0444);
	storageimage_vhd_c rimg(ro);
	check(rimg.is_readonly(), "a write-protected VHD is read-only in the drive");
	check(rimg.open(nullptr, false) && rimg.is_readonly(), "a write-protected VHD opens read-only");
	rimg.close();
}

int main()
{
	logger = new logger_c();

	char dir_template[] = "/tmp/vhd_image_testXXXXXX";
	dir = mkdtemp(dir_template);
	have_qemu = run("qemu-img --version") == 0;
	if (!have_qemu)
		printf("vhd_image_test: no qemu-img, skipping the cross-checks against it\n");

	test_dynamic_created_here();
	test_fixed();
	test_qemu_written();
	test_metadata_kept();
	test_partial_bitmap();
	test_simh_written();
	test_refused();
	test_recognition();

	std::string cleanup = "rm -rf '" + dir + "'";
	(void) !system(cleanup.c_str());

	printf("vhd_image_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
