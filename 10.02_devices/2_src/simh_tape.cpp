/* simh_tape.cpp: a SIMH-format (.tap) magnetic tape image, record-oriented

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   See simh_tape.hpp for the on-disk format.
*/

#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "simh_tape.hpp"

bool simh_tape_c::open(const std::string &path, bool readonly)
{
	close();
	path_ = path;
	readonly_ = readonly;
	// A read/write mount creates the tape if absent; a read-only mount requires it.
	std::ios::openmode mode = std::ios::in | std::ios::out | std::ios::binary;
	f_.open(path.c_str(), mode);
	if (!f_.is_open() && !readonly) {
		// create it
		std::ofstream create(path.c_str(), std::ios::binary);
		create.close();
		f_.open(path.c_str(), mode);
	}
	if (!f_.is_open()) {
		// last resort: read-only mount
		f_.open(path.c_str(), std::ios::in | std::ios::binary);
		readonly_ = true;
	}
	if (!f_.is_open())
		return false;
	open_ = true;
	pos_ = 0;
	refresh_size();
	return true;
}

void simh_tape_c::close()
{
	if (f_.is_open()) {
		f_.flush();
		f_.close();
	}
	open_ = false;
	pos_ = 0;
	size_ = 0;
}

void simh_tape_c::refresh_size()
{
	struct stat st;
	size_ = (stat(path_.c_str(), &st) == 0) ? (uint64_t) st.st_size : 0;
}

void simh_tape_c::truncate_here()
{
	// a data record or tape mark just written is the new end of the medium;
	// discard anything that followed it on the file
	f_.flush();
	if (::truncate(path_.c_str(), (off_t) pos_) == 0)
		size_ = pos_;
	else
		refresh_size();
}

bool simh_tape_c::read_marker(uint64_t at, uint32_t *marker)
{
	if (at + 4 > size_)
		return false;
	f_.clear();
	f_.seekg((std::streamoff) at, std::ios::beg);
	uint8_t b[4];
	f_.read((char *) b, 4);
	if (f_.gcount() != 4)
		return false;
	*marker = (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16)
			| ((uint32_t) b[3] << 24);
	return true;
}

bool simh_tape_c::write_marker(uint64_t at, uint32_t marker)
{
	f_.clear();
	f_.seekp((std::streamoff) at, std::ios::beg);
	uint8_t b[4] = { (uint8_t) marker, (uint8_t) (marker >> 8), (uint8_t) (marker >> 16),
			(uint8_t) (marker >> 24) };
	f_.write((const char *) b, 4);
	return (bool) f_;
}

simh_tape_c::result_t simh_tape_c::read_forward(uint8_t *buf, uint32_t max_len,
		uint32_t *rec_len)
{
	if (rec_len != nullptr)
		*rec_len = 0;
	if (!open_)
		return R_ERROR;
	// Skip any erase gaps, then read the record they precede. Iterating (rather
	// than recursing) bounds the scan by the file size, so a long run of gap
	// markers cannot overflow the stack.
	uint32_t marker;
	for (;;) {
		// physical end of file reads as end of medium
		if (pos_ >= size_)
			return R_END_OF_MEDIUM;
		if (!read_marker(pos_, &marker))
			return R_END_OF_MEDIUM;
		if (marker != MTR_GAP)
			break;
		pos_ += 4; // erase gap: skip and keep scanning forward
	}
	if (marker == MTR_TMK) {
		pos_ += 4;
		objp_++;
		return R_TAPE_MARK;
	}
	if (marker == MTR_EOM)
		return R_END_OF_MEDIUM;
	uint32_t len = marker & MTR_MAXLEN;
	if (rec_len != nullptr)
		*rec_len = len;
	uint64_t data_at = pos_ + 4;
	uint32_t want = len < max_len ? len : max_len;
	if (buf != nullptr && want > 0) {
		f_.clear();
		f_.seekg((std::streamoff) data_at, std::ios::beg);
		f_.read((char *) buf, want);
		if ((uint32_t) f_.gcount() != want)
			return R_ERROR;
	}
	pos_ = data_at + padded(len) + 4; // data (padded) + trailing marker
	objp_++;
	return (marker & MTR_ERF) ? R_ERROR : R_RECORD;
}

simh_tape_c::result_t simh_tape_c::space_forward(uint32_t *rec_len)
{
	return read_forward(nullptr, 0, rec_len);
}

simh_tape_c::result_t simh_tape_c::space_reverse(uint32_t *rec_len)
{
	if (rec_len != nullptr)
		*rec_len = 0;
	if (!open_)
		return R_ERROR;
	// Skip any erase gaps behind us, then space over the record they follow.
	// Iterating bounds the scan by the current position, so a long run of gap
	// markers cannot overflow the stack.
	uint32_t marker;
	for (;;) {
		if (pos_ == 0)
			return R_BEGIN_OF_TAPE;
		if (pos_ < 4)
			return R_ERROR;
		// the record just behind us ends with a trailing marker copy
		if (!read_marker(pos_ - 4, &marker))
			return R_ERROR;
		if (marker != MTR_GAP)
			break;
		pos_ -= 4; // erase gap: skip and keep scanning backward
	}
	if (marker == MTR_TMK) {
		pos_ -= 4;
		if (objp_ > 0) objp_--;
		return R_TAPE_MARK;
	}
	uint32_t len = marker & MTR_MAXLEN;
	if (rec_len != nullptr)
		*rec_len = len;
	uint64_t span = (uint64_t) 4 + padded(len) + 4; // leading + data + trailing
	if (span > pos_)
		return R_ERROR;
	pos_ -= span;
	if (objp_ > 0) objp_--;
	return (marker & MTR_ERF) ? R_ERROR : R_RECORD;
}

simh_tape_c::result_t simh_tape_c::write_record(const uint8_t *buf, uint32_t len)
{
	if (!open_)
		return R_ERROR;
	if (readonly_)
		return R_ERROR;
	if (len == 0 || len > MTR_MAXLEN)
		return R_ERROR;
	if (!write_marker(pos_, len))
		return R_ERROR;
	f_.clear();
	f_.seekp((std::streamoff) (pos_ + 4), std::ios::beg);
	f_.write((const char *) buf, len);
	if (len & 1) {
		char pad = 0;
		f_.write(&pad, 1); // pad the data to an even byte count
	}
	uint64_t trailer_at = pos_ + 4 + padded(len);
	if (!write_marker(trailer_at, len))
		return R_ERROR;
	pos_ = trailer_at + 4;
	objp_++;
	f_.flush();
	// a write is the new logical end of tape: drop anything beyond
	truncate_here();
	return R_RECORD;
}

simh_tape_c::result_t simh_tape_c::write_tape_mark()
{
	if (!open_)
		return R_ERROR;
	if (readonly_)
		return R_ERROR;
	if (!write_marker(pos_, MTR_TMK))
		return R_ERROR;
	pos_ += 4;
	objp_++;
	f_.flush();
	truncate_here();
	return R_RECORD;
}
