/* simh_tape.hpp: a SIMH-format (.tap) magnetic tape image, record-oriented

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   A SIMH .tap file is a stream of records. Each record is a 32-bit
   little-endian length marker, the data bytes (padded to an even count), then
   the same 32-bit marker again — the trailing copy lets a drive space in
   reverse. Special marker values carry structure rather than data:

     0x00000000  tape mark (file mark)          — one marker, no data, no trailer
     0xffffffff  end of medium (EOM)            — logical end; nothing follows
     0xfffffffe  erase gap                       — skipped when spacing
     bit 31 set   the record is flagged bad (error), low 24 bits = its length

   This is the on-disk format SimH, E11 and this emulator share, so images move
   between them unchanged. The class is a pure record layer over a file, with no
   PRU, qunibus or device dependency, so it links into the host test unchanged.
*/
#ifndef _SIMH_TAPE_HPP_
#define _SIMH_TAPE_HPP_

#include <cstdint>
#include <fstream>
#include <string>

class simh_tape_c {
public:
	// Outcome of a read or space operation.
	enum result_t {
		R_RECORD,             // a data record was read/spaced
		R_TAPE_MARK,      // a tape mark was crossed
		R_END_OF_MEDIUM,  // EOM marker or physical end of file
		R_BEGIN_OF_TAPE,  // reverse operation already at BOT
		R_ERROR           // malformed image or I/O error
	};

	simh_tape_c() {}
	~simh_tape_c() { close(); }
	simh_tape_c(const simh_tape_c &) = delete;
	simh_tape_c &operator=(const simh_tape_c &) = delete;

	// Open a .tap file. `readonly` mounts a write-protected tape; a read/write
	// mount that finds the file missing creates an empty tape. Returns false on
	// a file that cannot be opened.
	bool open(const std::string &path, bool readonly);
	void close();
	bool is_open() const { return open_; }
	bool is_readonly() const { return readonly_; }
	uint64_t file_size() const { return size_; }
	uint64_t position() const { return pos_; }

	// The object position: the count of objects (records and tape marks) passed
	// since BOT. This is what a TMSCP controller reports in the position field of
	// a read/write/access end packet, distinct from the byte offset above.
	uint64_t object_position() const { return objp_; }

	void rewind() { pos_ = 0; objp_ = 0; }  // to beginning of tape (BOT)
	bool at_bot() const { return pos_ == 0; }

	// Read the next record forward. On R_RECORD up to `max_len` bytes are copied to
	// `buf` and *rec_len is the record's true length (which may exceed max_len:
	// the caller learns the real length and buf holds the truncated prefix).
	result_t read_forward(uint8_t *buf, uint32_t max_len, uint32_t *rec_len);

	// Space over one record in each direction without transferring data.
	// *rec_len is the spanned record's length (0 for a tape mark).
	result_t space_forward(uint32_t *rec_len);
	result_t space_reverse(uint32_t *rec_len);

	// Write a data record at the current position, then truncate the tape after
	// it: a write destroys everything beyond, as on real hardware. Refused on a
	// read-only tape.
	result_t write_record(const uint8_t *buf, uint32_t len);
	result_t write_tape_mark();

	// SIMH marker constants (exposed for the controller and tests).
	static const uint32_t MTR_TMK = 0x00000000;
	static const uint32_t MTR_EOM = 0xffffffff;
	static const uint32_t MTR_GAP = 0xfffffffe;
	static const uint32_t MTR_ERF = 0x80000000; // error flag in a data marker
	static const uint32_t MTR_MAXLEN = 0x00ffffff;

private:
	std::fstream f_;
	std::string path_;
	bool open_ = false;
	bool readonly_ = false;
	uint64_t pos_ = 0;  // current byte offset in the file (the "tape position")
	uint64_t objp_ = 0; // object position: records + tape marks passed since BOT
	uint64_t size_ = 0; // file size in bytes

	static uint32_t padded(uint32_t len) { return (len + 1) & ~1u; } // even byte count
	bool read_marker(uint64_t at, uint32_t *marker);
	bool write_marker(uint64_t at, uint32_t marker);
	void refresh_size();
	void truncate_here(); // drop the file after the current position
};

#endif // _SIMH_TAPE_HPP_
