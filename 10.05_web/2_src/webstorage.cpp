/* webstorage.cpp: /api/images — disk/tape image files of the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Images live in a hierarchy under $QUNILATOR_DIR/images. Devices reference an
   image by a path relative to $QUNILATOR_DIR ("images/du/foo.dsk"), which the
   drive opens through the working directory — portable across boards. The API
   and the UI speak the images-root-relative "subpath" ("du/foo.dsk").

     GET    /api/images                 tree: folders and image files
     POST   /api/images                 multipart upload (multiple files + dir)
     GET    /api/images/<subpath>       download
     DELETE /api/images/<subpath>       delete a file
     POST   /api/move                   {from,to} rename/move a file or folder
     POST   /api/folders                {path} create a folder
     DELETE /api/folders/<subpath>      remove an empty folder
     GET    /api/roms                   the ROM listings the package ships
     POST   /api/roms                   {name,dir} copy one into the tree
     GET    /api/roms/labels?file=      the code labels a ROM listing defines

   Attaching an image to a drive is a parameter write on the drive
   (PUT /api/devices/<drive>/params/image with the subpath).
*/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "utils.hpp"
#include "device.hpp"
#include "storagedrive.hpp"
#include "storageimage.hpp"

#include "weblog.hpp"
#include "webstorage.hpp"
#include "webconfigs.hpp"
#include "webevents.hpp"

static std::string images_dir;

// -------------------------------------------------------------------------
// path model

// A relative path under the images root: one or more non-empty segments, no
// "." or ".." components, no leading dot on a segment (hidden files), no
// backslash, and not absolute. This is the single path-traversal guard.
static bool valid_subpath(const std::string &sub) {
	if (sub.empty() || sub[0] == '/' || sub.find('\\') != std::string::npos)
		return false;
	size_t start = 0;
	while (start <= sub.size()) {
		size_t slash = sub.find('/', start);
		std::string seg = sub.substr(start, slash == std::string::npos
				? std::string::npos : slash - start);
		if (seg.empty() || seg == "." || seg == ".." || seg[0] == '.')
			return false;
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return true;
}

// The canonical images-root-relative key for any stored/incoming image value.
// Accepts the stored form ("images/du/foo"), a legacy "./images/foo", an
// absolute path inside the images dir, or an already-bare subpath, and returns
// "du/foo". An unmanaged absolute path (outside the images dir) is returned
// unchanged so a drive attached to a file elsewhere reports its true path.
std::string webstorage_image_subpath(const std::string &value) {
	if (value.empty())
		return "";
	std::string v = value;
	std::string absp = images_dir + "/";
	if (v.compare(0, absp.size(), absp) == 0)
		return v.substr(absp.size());
	if (v.compare(0, 2, "./") == 0)
		v = v.substr(2);
	if (v.compare(0, 7, "images/") == 0)
		return v.substr(7);
	return v; // already a subpath, or an unmanaged absolute path
}

const std::string &webstorage_images_dir() {
	return images_dir;
}

bool webstorage_valid_subpath(const std::string &sub) {
	return valid_subpath(sub);
}

// The value stored in a drive's "image" parameter and a saved config: relative
// to $QUNILATOR_DIR so it is portable and the drive opens it via the working
// directory. Empty stays empty; an unmanaged absolute path is left untouched.
std::string webstorage_image_path(const std::string &value) {
	std::string sub = webstorage_image_subpath(value);
	if (sub.empty())
		return "";
	if (sub[0] == '/')
		return sub; // unmanaged absolute path
	return "images/" + sub;
}

// Absolute filesystem path for reading/writing the file.
static std::string image_abs(const std::string &value) {
	std::string sub = webstorage_image_subpath(value);
	if (sub.empty())
		return "";
	if (sub[0] == '/')
		return sub;
	return images_dir + "/" + sub;
}

// A drive opens a file it is given as its medium and makes an empty one when
// there is none, so a name with nothing behind it would come up as a blank
// disk. The web interface makes a new medium on purpose (POST /api/images
// with a JSON body), and puts in a drive only what is there: the file itself,
// or the compressed master a drive expands on first use. An empty value is an
// empty drive.
bool webstorage_image_exists(const std::string &path) {
	std::string abs = image_abs(path);
	if (abs.empty())
		return true;
	struct stat st;
	return stat(abs.c_str(), &st) == 0 || stat((abs + ".gz").c_str(), &st) == 0;
}

std::string webstorage_image_held_by(const std::string &path, const std::string &except) {
	std::string key = webstorage_image_subpath(path);
	if (key.empty())
		return "";
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		storagedrive_c *drive = dynamic_cast<storagedrive_c *>(dev);
		if (drive == nullptr || dev->name.value == except || !dev->enabled.value)
			continue;
		if (webstorage_image_subpath(drive->image_filepath.value) == key)
			return dev->name.value;
	}
	return "";
}

// make every directory along a subpath under the images root
static bool make_dirs(const std::string &sub) {
	std::string acc = images_dir;
	size_t start = 0;
	while (start < sub.size()) {
		size_t slash = sub.find('/', start);
		std::string seg = sub.substr(start, slash == std::string::npos
				? std::string::npos : slash - start);
		acc += "/" + seg;
		if (mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST)
			return false;
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return true;
}

// give a freshly written file to the qunilator user so the SMB/FTP/SFTP shares
// can manage it. Best effort: the user may not exist on a dev host.
static void own_by_qunilator(const std::string &path) {
	struct passwd *pw = getpwnam("qunilator");
	if (pw != nullptr)
		(void) !chown(path.c_str(), pw->pw_uid, pw->pw_gid);
}

bool webstorage_make_dirs(const std::string &dir_sub) {
	return dir_sub.empty() || make_dirs(dir_sub);
}

void webstorage_own_by_qunilator(const std::string &abs_path) {
	own_by_qunilator(abs_path);
}

// -------------------------------------------------------------------------
// JSON helpers

static void send_json(struct mg_connection *conn, int status, const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			status, status == 200 ? "OK" : "Error", (unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

static void send_error(struct mg_connection *conn, int status, const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, status, picojson::value(err));
}

static bool read_json_body(struct mg_connection *conn, picojson::value *out) {
	std::string body;
	char buf[4096];
	int n;
	while ((n = mg_read(conn, buf, sizeof buf)) > 0)
		body.append(buf, n);
	std::string err = picojson::parse(*out, body);
	return err.empty() && out->is<picojson::object>();
}

// drives an image file is attached to, matched on the whole subpath so a
// same-named file in another folder is not confused for this one.
static picojson::array attached_drives(const std::string &sub) {
	picojson::array result;
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		storagedrive_c *drive = dynamic_cast<storagedrive_c *>(dev);
		if (drive == nullptr)
			continue;
		if (webstorage_image_subpath(drive->image_filepath.value) == sub)
			result.push_back(picojson::value(dev->name.value));
	}
	return result;
}

// The switched-on drives naming an image, split by whether each holds the file
// open: an open drive offers it to the machine, and a drive whose open failed
// offers the machine no medium at all.
static void drive_states(const std::string &sub, picojson::array *mounted,
		picojson::array *unopened) {
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		storagedrive_c *drive = dynamic_cast<storagedrive_c *>(dev);
		if (drive == nullptr || !dev->enabled.value)
			continue;
		if (webstorage_image_subpath(drive->image_filepath.value) != sub)
			continue;
		bool open = drive->get_image() != nullptr && drive->get_image()->is_open();
		(open ? mounted : unopened)->push_back(picojson::value(dev->name.value));
	}
}

// Overlay status for an image. A drive holding the image open answers from its
// COW object; an image no drive holds open answers from its overlay files, so
// writes a drive left behind stay visible - and consolidatable - after the
// drive is switched off.
struct overlay_status_t {
	bool present = false;
	uint64_t dirty_blocks = 0;
	uint64_t overlay_bytes = 0;
};

// The files a copy-on-write overlay keeps beside its base image.
static const char *const overlay_suffixes[] = {".ovl", ".ovl.map"};

static bool same_file(const std::string &a, const std::string &b) {
	struct stat sa, sb;
	return stat(a.c_str(), &sa) == 0 && stat(b.c_str(), &sb) == 0
			&& sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// The drive holding an image open, or nullptr. *cow is its COW image when it
// runs one, else nullptr. The caller must hold device_c::mydevices_mutex.
//
// A drive is matched on the file it has open, so a drive whose "image" names
// the file by another path - absolute, through a symlink, expanded from a
// ".gz" - is still found, and its overlay is never worked on behind its back.
static storagedrive_c *find_open_drive(const std::string &sub, storageimage_cow_c **cow) {
	*cow = nullptr;
	std::string abs = image_abs(sub);
	for (device_c *dev : device_c::mydevices) {
		storagedrive_c *drive = dynamic_cast<storagedrive_c *>(dev);
		if (drive == nullptr || drive->get_image() == nullptr || !drive->get_image()->is_open())
			continue;
		storageimage_cow_c *drive_cow = dynamic_cast<storageimage_cow_c *>(drive->get_image());
		bool holds = drive_cow != nullptr
				? drive_cow->holds_base(abs)
				: (webstorage_image_subpath(drive->image_filepath.value) == sub
					|| same_file(drive->image_filepath.value, abs));
		if (!holds)
			continue;
		*cow = drive_cow;
		return drive;
	}
	return nullptr;
}

static overlay_status_t overlay_status(const std::string &sub) {
	overlay_status_t out;
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	storageimage_cow_c *cow;
	if (find_open_drive(sub, &cow) != nullptr) {
		if (cow != nullptr && cow->has_overlay()) {
			out.present = true;
			out.dirty_blocks = cow->dirty_block_count();
			out.overlay_bytes = cow->overlay_allocated_bytes();
		}
		return out;
	}
	out.present = storageimage_cow_c::probe(image_abs(sub), &out.dirty_blocks,
			&out.overlay_bytes);
	return out;
}

// -------------------------------------------------------------------------
// listing

// recurse the images tree, collecting folder subpaths and file entries
static void walk_images(const std::string &reldir, picojson::array &dirs,
		picojson::array &images, int depth) {
	if (depth > 16)
		return; // guard against a pathological tree / symlink loop
	std::string absdir = reldir.empty() ? images_dir : images_dir + "/" + reldir;
	DIR *dir = opendir(absdir.c_str());
	if (dir == nullptr)
		return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr) {
		std::string leaf = entry->d_name;
		if (leaf == "." || leaf == ".." || leaf[0] == '.')
			continue;
		// hide the copy-on-write overlay and its bitmap sidecar: they are
		// internal per-image artifacts, not images in their own right
		if (leaf.size() > 4 && leaf.compare(leaf.size() - 4, 4, ".ovl") == 0)
			continue;
		if (leaf.size() > 8 && leaf.compare(leaf.size() - 8, 8, ".ovl.map") == 0)
			continue;
		std::string sub = reldir.empty() ? leaf : reldir + "/" + leaf;
		std::string abs = images_dir + "/" + sub;
		struct stat st;
		if (lstat(abs.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			dirs.push_back(picojson::value(sub));
			walk_images(sub, dirs, images, depth + 1);
		} else if (S_ISREG(st.st_mode)) {
			picojson::object o;
			o["name"] = picojson::value(leaf);
			o["path"] = picojson::value(sub);      // images-root-relative
			o["dir"] = picojson::value(reldir);    // parent folder ("" = root)
			o["size"] = picojson::value((double) st.st_size);
			o["writable"] = picojson::value((st.st_mode & S_IWUSR) != 0);
			char mtime[32];
			strftime(mtime, sizeof mtime, "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
			o["mtime"] = picojson::value(mtime);
			o["attached"] = picojson::value(attached_drives(sub));
			picojson::array mounted, unopened;
			drive_states(sub, &mounted, &unopened);
			o["mounted"] = picojson::value(mounted);
			o["unopened"] = picojson::value(unopened);
			picojson::array uses;
			for (const config_image_use_t &u : webconfigs_image_uses(sub)) {
				picojson::object e;
				e["config"] = picojson::value(u.config);
				e["device"] = picojson::value(u.device);
				uses.push_back(picojson::value(e));
			}
			o["used"] = picojson::value(uses);
			overlay_status_t ov = overlay_status(sub);
			o["overlay"] = picojson::value(ov.present);
			if (ov.present) {
				o["overlay_dirty_blocks"] = picojson::value((double) ov.dirty_blocks);
				o["overlay_bytes"] = picojson::value((double) ov.overlay_bytes);
			}
			images.push_back(picojson::value(o));
		}
	}
	closedir(dir);
}

// GET /api/images -> { "dirs": [...subpaths], "images": [...entries] }
static void images_list(struct mg_connection *conn) {
	picojson::array dirs, images;
	walk_images("", dirs, images, 0);
	picojson::object out;
	out["dirs"] = picojson::value(dirs);
	out["images"] = picojson::value(images);
	send_json(conn, 200, picojson::value(out));
}

// -------------------------------------------------------------------------
// the ROM listings the package ships
//
// These are package content, not operator state: they live under /usr/share
// and every upgrade rewrites them, so nothing references them by path. They are
// offered as a *source* instead — the operator copies one into the images tree
// and the copy is theirs, to attach to a card and to edit. That keeps the two
// trees from being confused for one another: a device only ever names a file
// under images/.

// Where the packaging installs the M9312 console/diagnostic and boot PROM
// listings. Overridable so a build tree can be run without installing.
static std::string package_roms_dir() {
	const char *env = getenv("QUNILATOR_ROMS_DIR");
	return env != nullptr ? std::string(env) : "/usr/share/qunilator/roms";
}

// A MACRO-11 listing names itself on its ".title" line, which is what an
// operator can recognise a PROM by — the 23-nnnnn part number alone says
// nothing. Empty when the file carries no title.
static std::string listing_title(const std::string &abspath) {
	FILE *f = fopen(abspath.c_str(), "r");
	if (f == nullptr)
		return "";
	char line[512];
	std::string title;
	// the title is the first statement of the file; a handful of lines is
	// enough to find it without reading a 26 kB listing
	for (int n = 0; n < 20 && fgets(line, sizeof line, f) != nullptr; n++) {
		const char *p = strstr(line, ".title");
		if (p == nullptr)
			continue;
		p += strlen(".title");
		while (*p == ' ' || *p == '\t')
			p++;
		title = p;
		while (!title.empty() && (title.back() == '\n' || title.back() == '\r'
				|| title.back() == ' ' || title.back() == '\t'))
			title.pop_back();
		break;
	}
	fclose(f);
	return title;
}

// GET /api/roms — what the package offers to copy
static void package_roms_list(struct mg_connection *conn) {
	std::string dir = package_roms_dir();
	// by part number, which is the order the listing files are named in and the
	// one an operator looking for a known PROM expects
	std::vector<std::string> names;
	DIR *d = opendir(dir.c_str());
	if (d != nullptr) {
		struct dirent *e;
		while ((e = readdir(d)) != nullptr) {
			std::string name = e->d_name;
			if (name.empty() || name[0] == '.')
				continue;
			struct stat st;
			if (stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			names.push_back(name);
		}
		closedir(d);
	}
	std::sort(names.begin(), names.end());
	picojson::array roms;
	for (const std::string &name : names) {
		std::string abs = dir + "/" + name;
		struct stat st;
		if (stat(abs.c_str(), &st) != 0)
			continue;
		picojson::object o;
		o["name"] = picojson::value(name);
		o["size"] = picojson::value((double) st.st_size);
		o["title"] = picojson::value(listing_title(abs));
		roms.push_back(picojson::value(o));
	}
	picojson::object out;
	out["roms"] = picojson::value(roms);
	send_json(conn, 200, picojson::value(out));
}

// The code labels a MACRO-11 listing defines, by the rules the M9312 programs
// a socket with (memoryimage_c::load_macro11_listing): a numbered line names a
// label as its first word that is not an octal number, followed by ':', and the
// label takes the address of the line it stands on or of the next line that
// carries one. Reading only the names keeps this off the shared memory buffer
// the loader fills.
static std::vector<std::string> listing_labels(const std::string &abspath) {
	std::vector<std::string> labels;
	FILE *f = fopen(abspath.c_str(), "r");
	if (f == nullptr)
		return labels;
	char line[1024];
	int lineno_width = -1;
	std::string pending; // a label still waiting for an address
	while (fgets(line, sizeof line, f) != nullptr) {
		if (!strncasecmp(line, "Symbol table", 12))
			break;
		if (line[0] != ' ' && line[0] != '\t')
			continue; // a page header, not a numbered line
		const char *p = line;
		while (*p && isspace((unsigned char) *p))
			p++;
		if (*p == 0)
			continue;
		if (lineno_width < 0) {
			while (isdigit((unsigned char) *p))
				p++;
			lineno_width = (int) (p - line);
		}
		if ((int) strlen(line) <= lineno_width)
			continue;
		p = line + lineno_width;
		bool addressed = false; // the line carries an address
		for (;;) {
			while (*p && isspace((unsigned char) *p))
				p++;
			const char *q = p;
			while (*q >= '0' && *q <= '7')
				q++;
			if (q > p) {
				if (q - p != 3 && q - p != 6)
					break; // a local label such as "1$:", or anything else
				if (q - p == 6 && !addressed) {
					addressed = true;
					if (!pending.empty()) {
						labels.push_back(pending); // a label of an earlier line
						pending.clear();
					}
				}
				p = q;
				continue;
			}
			// the first word that is not a number ends the line, and a word
			// marked PC-relative (165004') ends it with nothing found
			while (*q && (isalnum((unsigned char) *q) || *q == '$' || *q == '.'))
				q++;
			if (q > p && *q == ':' && *p != '8' && *p != '9') {
				if (addressed)
					labels.push_back(std::string(p, q - p));
				else
					pending = std::string(p, q - p);
			}
			break;
		}
	}
	fclose(f);
	std::sort(labels.begin(), labels.end());
	labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
	return labels;
}

// Whether a listing named by a configuration may be read here: a file of the
// images tree, or one the package ships.
static bool rom_listing_readable(const std::string &abspath) {
	char *real = realpath(abspath.c_str(), nullptr);
	if (real == nullptr)
		return false;
	std::string r = real;
	free(real);
	for (const std::string &dir : {images_dir, package_roms_dir()}) {
		char *rd = realpath(dir.c_str(), nullptr);
		if (rd == nullptr)
			continue;
		std::string root = std::string(rd) + "/";
		free(rd);
		if (r.compare(0, root.size(), root) == 0)
			return true;
	}
	return false;
}

// GET /api/roms/labels?file=<value> — the code labels of the listing a ROM
// socket names, written the way a configuration writes it
static void rom_labels(struct mg_connection *conn) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	char file[1024] = "";
	if (ri->query_string == nullptr
			|| mg_get_var(ri->query_string, strlen(ri->query_string), "file",
					file, sizeof file) <= 0) {
		send_error(conn, 400, "file= names the listing");
		return;
	}
	std::string abs = image_abs(file);
	if (abs.empty() || !rom_listing_readable(abs)) {
		send_error(conn, 404, std::string("no ROM listing \"") + file + "\"");
		return;
	}
	picojson::array labels;
	for (const std::string &l : listing_labels(abs))
		labels.push_back(picojson::value(l));
	picojson::object out;
	out["file"] = picojson::value(std::string(file));
	out["title"] = picojson::value(listing_title(abs));
	out["labels"] = picojson::value(labels);
	send_json(conn, 200, picojson::value(out));
}

// POST /api/roms — {"name": .., "dir": ..} copy one into the images tree
static void package_rom_copy(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("name").is<std::string>()) {
		send_error(conn, 400, "body must be {\"name\":.., \"dir\":..}");
		return;
	}
	std::string name = req.get("name").get<std::string>();
	if (name.empty() || name[0] == '.' || name.find('/') != std::string::npos) {
		send_error(conn, 400, "\"" + name + "\" is not a ROM file name");
		return;
	}
	// the images-tree folder the copy lands in; the ROM folder by default, so
	// the copy sits where a machine's ROMs belong even if nothing was selected
	std::string dir = req.get("dir").is<std::string>()
			? webstorage_image_subpath(req.get("dir").get<std::string>()) : "";
	if (dir.empty())
		dir = "roms";
	std::string sub = dir + "/" + name;
	if (!valid_subpath(sub)) {
		send_error(conn, 400, "\"" + sub + "\" is not a valid image path");
		return;
	}

	std::string src = package_roms_dir() + "/" + name;
	int in = open(src.c_str(), O_RDONLY);
	if (in < 0) {
		send_error(conn, 404, "no ROM \"" + name + "\" is installed");
		return;
	}
	if (!make_dirs(dir)) {
		close(in);
		send_error(conn, 500, "cannot create folder \"" + dir + "\"");
		return;
	}
	std::string dest = images_dir + "/" + sub;
	// O_EXCL: a copy the operator has since edited is theirs, and a second copy
	// must not silently throw those edits away
	int out = open(dest.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
	if (out < 0) {
		int err = errno;
		close(in);
		if (err == EEXIST)
			send_error(conn, 409, "\"" + sub + "\" already exists — "
					"delete or rename it to copy a fresh one");
		else
			send_error(conn, 500, "cannot create \"" + sub + "\": " + strerror(err));
		return;
	}
	char buf[65536];
	ssize_t n;
	bool ok = true;
	uint64_t size = 0;
	while (ok && (n = read(in, buf, sizeof buf)) > 0) {
		ok = write(out, buf, (size_t) n) == n;
		size += (uint64_t) n;
	}
	if (n < 0)
		ok = false;
	close(in);
	close(out);
	if (!ok) {
		unlink(dest.c_str());
		send_error(conn, 500, "cannot write \"" + sub + "\"");
		return;
	}
	own_by_qunilator(dest);
	WEB_INFO("ROM %s copied to %s, %llu bytes", name.c_str(), sub.c_str(),
			(unsigned long long) size);
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["path"] = picojson::value(sub);
	res["size"] = picojson::value((double) size);
	send_json(conn, 200, picojson::value(res));
}

// -------------------------------------------------------------------------
// upload (multiple files into a target folder)

struct upload_state {
	std::string dir;            // target folder subpath ("" = root), from ?dir=
	std::vector<std::string> names; // file subpaths the request carries
	std::set<std::string> stored;   // absolute paths civetweb finished writing
	bool bad_name;
};

static int upload_field_found(const char *key, const char *filename,
		char *path, size_t pathlen, void *user_data) {
	(void) key;
	upload_state *state = (upload_state *) user_data;
	if (filename == nullptr)
		return MG_FORM_FIELD_STORAGE_SKIP; // ignore non-file fields
	if (!valid_subpath(filename)) {
		state->bad_name = true;
		return MG_FORM_FIELD_STORAGE_ABORT;
	}
	std::string sub = state->dir.empty() ? std::string(filename)
			: state->dir + "/" + filename;
	state->names.push_back(sub);
	snprintf(path, pathlen, "%s/%s", images_dir.c_str(), sub.c_str());
	return MG_FORM_FIELD_STORAGE_STORE;
}

static int upload_field_get(const char *, const char *, size_t, void *) {
	return MG_FORM_FIELD_HANDLE_NEXT;
}

// civetweb calls this only for a file it wrote to the end of its part and
// closed; a part cut short is deleted without it.
static int upload_field_stored(const char *path, long long file_size, void *user_data) {
	(void) file_size;
	upload_state *state = (upload_state *) user_data;
	state->stored.insert(path);
	own_by_qunilator(path);
	return MG_FORM_FIELD_HANDLE_NEXT;
}

// POST /api/images?dir=<folder> — multipart upload of one or more file fields
// into the target folder (query parameter, so its value is known before the
// body is parsed, independent of field order). Empty/absent dir = root.
static void images_upload(struct mg_connection *conn) {
	upload_state state;
	state.bad_name = false;
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (ri->query_string != nullptr) {
		char buf[512];
		int n = mg_get_var(ri->query_string, strlen(ri->query_string),
				"dir", buf, sizeof buf);
		if (n > 0)
			state.dir.assign(buf, n);
	}
	while (!state.dir.empty() && state.dir.back() == '/')
		state.dir.pop_back();
	if (!state.dir.empty() && !valid_subpath(state.dir)) {
		send_error(conn, 400, "target folder is not a valid path");
		return;
	}
	if (!state.dir.empty() && !make_dirs(state.dir)) {
		send_error(conn, 500, "cannot create the target folder");
		return;
	}
	struct mg_form_data_handler handler;
	handler.field_found = upload_field_found;
	handler.field_get = upload_field_get;
	handler.field_store = upload_field_stored;
	handler.user_data = &state;
	int fields = mg_handle_form_request(conn, &handler);
	if (state.bad_name) {
		send_error(conn, 400, "a file name is not a valid path segment");
		return;
	}
	if (state.names.empty()) {
		send_error(conn, 400, "no file in upload");
		return;
	}
	// A body that ends before its last part does - the client went away, or the
	// connection stalled past the request timeout - leaves that file unwritten,
	// and the upload is refused by name so nothing mistakes it for stored.
	for (const std::string &n : state.names) {
		if (fields < 0 || state.stored.count(images_dir + "/" + n) == 0) {
			WEB_WARNING("upload of image %s did not complete", n.c_str());
			send_error(conn, 400, "the upload of " + n + " did not complete");
			return;
		}
	}
	picojson::array stored;
	for (const std::string &n : state.names) {
		WEB_INFO("image %s uploaded", n.c_str());
		stored.push_back(picojson::value(n));
	}
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["names"] = picojson::value(stored);
	send_json(conn, 200, picojson::value(res));
}

// -------------------------------------------------------------------------
// per-file GET/DELETE and move

// POST /api/images with a JSON body — a blank medium to write on, rather than a
// file to upload: {name, dir, kind, size}.
//
// kind "disk" (the default) makes a file of `size` bytes. A drive that takes a
// fixed medium publishes exactly how big it is as its read-only "capacity"
// parameter, so a caller reads the size off the drive it means rather than from
// a table kept here; a controller that takes the size from the image instead
// (MSCP with useimagesize) accepts any size, which is why this is a byte count
// and not a menu. The file is sparse, so a scratch pack costs the blocks written
// to it rather than its capacity.
//
// kind "tape" makes a blank reel, which is not an empty file: a tape at the load
// point with nothing on it still carries a file mark, so the drive reads a mark
// and then end of medium rather than end of medium straight away. That is one
// SIMH marker of 0x00000000 and nothing else (see simh_tape.hpp).
static const uint64_t IMAGE_CREATE_MAX = 4ull * 1024 * 1024 * 1024;

static void image_create(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("name").is<std::string>()) {
		send_error(conn, 400, "body must be {\"name\":.., \"dir\":.., \"kind\":.., \"size\":..}");
		return;
	}
	std::string name = req.get("name").get<std::string>();
	std::string dir = req.get("dir").is<std::string>()
			? webstorage_image_subpath(req.get("dir").get<std::string>()) : "";
	std::string kind = req.get("kind").is<std::string>()
			? req.get("kind").get<std::string>() : "disk";
	double raw = req.get("size").is<double>() ? req.get("size").get<double>() : 0;
	if (kind != "disk" && kind != "tape") {
		send_error(conn, 400, "kind must be \"disk\" or \"tape\"");
		return;
	}
	if (name.empty() || name.find('/') != std::string::npos
			|| name == "." || name == "..") {
		send_error(conn, 400, "\"" + name + "\" is not a file name");
		return;
	}
	if (kind == "disk" && (raw < 512 || raw > (double) IMAGE_CREATE_MAX)) {
		send_error(conn, 400, "a disk image is between 512 bytes and 4 GiB");
		return;
	}
	uint64_t size = (uint64_t) raw;
	std::string sub = dir.empty() ? name : dir + "/" + name;
	if (!valid_subpath(sub)) {
		send_error(conn, 400, "\"" + sub + "\" is not a valid image path");
		return;
	}
	std::string abs = image_abs(sub);
	int fd = open(abs.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
	if (fd < 0) {
		if (errno == EEXIST)
			send_error(conn, 409, "\"" + sub + "\" already exists");
		else
			send_error(conn, 500, "cannot create \"" + sub + "\": " + strerror(errno));
		return;
	}
	bool written;
	if (kind == "tape") {
		const uint8_t tape_mark[4] = { 0, 0, 0, 0 }; // SIMH MTR_TMK
		written = write(fd, tape_mark, sizeof tape_mark) == (ssize_t) sizeof tape_mark;
		size = sizeof tape_mark;
	} else
		written = ftruncate(fd, (off_t) size) == 0;
	if (!written) {
		std::string reason = strerror(errno);
		close(fd);
		unlink(abs.c_str());
		send_error(conn, 500, "cannot write \"" + sub + "\": " + reason);
		return;
	}
	close(fd);
	own_by_qunilator(abs);
	WEB_INFO("blank %s %s created, %llu bytes", kind.c_str(), sub.c_str(),
			(unsigned long long) size);
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["name"] = picojson::value(sub);
	res["size"] = picojson::value((double) size);
	send_json(conn, 200, picojson::value(res));
}

static void image_delete(struct mg_connection *conn, const std::string &sub) {
	picojson::array attached = attached_drives(sub);
	if (!attached.empty()) {
		send_error(conn, 409, "image is attached to " + attached[0].get<std::string>());
		return;
	}
	std::string config = webconfigs_image_referenced(sub);
	if (!config.empty()) {
		send_error(conn, 409, "image is referenced by configuration \"" + config + "\"");
		return;
	}
	if (unlink(image_abs(sub).c_str()) != 0) {
		send_error(conn, 500, "cannot delete image \"" + sub + "\"");
		return;
	}
	// the overlay holds writes to this image only; left behind, it would lay
	// them over the next image given the same name
	for (const char *suffix : overlay_suffixes)
		(void) unlink((image_abs(sub) + suffix).c_str());
	WEB_INFO("image %s deleted", sub.c_str());
	picojson::object res;
	res["ok"] = picojson::value(true);
	send_json(conn, 200, picojson::value(res));
}

// POST /api/move — {from, to} rename/move a file or folder within the tree
static void image_move(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req)
			|| !req.get("from").is<std::string>() || !req.get("to").is<std::string>()) {
		send_error(conn, 400, "body must be {\"from\":..,\"to\":..}");
		return;
	}
	std::string from = webstorage_image_subpath(req.get("from").get<std::string>());
	std::string to = webstorage_image_subpath(req.get("to").get<std::string>());
	if (!valid_subpath(from) || !valid_subpath(to)) {
		send_error(conn, 400, "from/to must be valid image paths");
		return;
	}
	std::string from_abs = images_dir + "/" + from, to_abs = images_dir + "/" + to;
	struct stat st, st_from;
	if (lstat(from_abs.c_str(), &st_from) != 0) {
		send_error(conn, 404, "\"" + from + "\" does not exist");
		return;
	}
	if (lstat(to_abs.c_str(), &st) == 0) {
		send_error(conn, 409, "\"" + to + "\" already exists");
		return;
	}
	// moving a file that a drive or a config points at would break the link;
	// require it be detached first (same rule as delete)
	if (!attached_drives(from).empty()) {
		send_error(conn, 409, "image is attached to a drive");
		return;
	}
	if (!webconfigs_image_referenced(from).empty()) {
		send_error(conn, 409, "image is referenced by a saved configuration");
		return;
	}
	size_t slash = to.rfind('/');
	if (slash != std::string::npos && !make_dirs(to.substr(0, slash))) {
		send_error(conn, 500, "cannot create the target folder");
		return;
	}
	// an image's overlay moves with it, and an overlay already lying at the
	// destination would be laid over the moved image
	bool is_file = S_ISREG(st_from.st_mode);
	if (is_file)
		for (const char *suffix : overlay_suffixes)
			if (lstat((to_abs + suffix).c_str(), &st) == 0) {
				send_error(conn, 409, "\"" + to + suffix
						+ "\" is already there, an overlay left behind by an earlier image of that name");
				return;
			}
	if (rename(from_abs.c_str(), to_abs.c_str()) != 0) {
		send_error(conn, 500, "cannot move \"" + from + "\"");
		return;
	}
	if (is_file)
		for (const char *suffix : overlay_suffixes)
			if (lstat((from_abs + suffix).c_str(), &st) == 0
					&& rename((from_abs + suffix).c_str(), (to_abs + suffix).c_str()) != 0)
				WEB_ERROR("image %s moved, but its %s stayed behind", from.c_str(), suffix);
	WEB_INFO("image %s moved to %s", from.c_str(), to.c_str());
	picojson::object res;
	res["ok"] = picojson::value(true);
	send_json(conn, 200, picojson::value(res));
}

// -------------------------------------------------------------------------
// content introspection: run the Python decoders on one image, read-only

// Where the packaging installs the dec-disketten decoders + introspect.py.
static const char *INTROSPECT = "/usr/share/qunilator/decoders/introspect.py";

// Run introspect.py on an image and return its JSON. Executed with fork/exec,
// never a shell, so a file name carrying shell metacharacters is passed as one
// argument and cannot inject.
static std::string run_introspect(const std::string &abspath) {
	const char *argv[] = { "python3", INTROSPECT, abspath.c_str(), nullptr };
	std::string out;
	if (subprocess_run(argv, -1, &out) != 0)
		return "";
	return out;
}

// GET /api/images/<subpath>/contents
static void image_contents(struct mg_connection *conn, const std::string &sub) {
	std::string path = image_abs(sub);
	struct stat st;
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
		send_error(conn, 404, "unknown image \"" + sub + "\"");
		return;
	}
	std::string body = run_introspect(path);
	if (body.empty()) {
		send_error(conn, 500, "could not read the image contents");
		return;
	}
	// introspect.py already emits JSON; pass it through
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			(unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

// POST /api/images/<subpath>/overlay/<discard|commit|export>
//
// discard: throw the overlay away, restoring the pristine base
// commit:  fold the overlay into the base, then discard it
// export:  write a flattened plain image to {"dest": <subpath>}, base untouched
//
// An image a drive holds open is worked on through that drive's COW object.
// Mutating its overlay (or reading a consistent flattened export) while the
// guest is doing I/O would race live block writes, so that needs the machine
// not running (powered off or the CPU halted). An image no drive holds open -
// its drive switched off, say - is worked on through its files at any time.
static void image_overlay_op(struct mg_connection *conn, const std::string &base,
		const std::string &action) {
	bool running = webevents_is_powered() && !webevents_is_halted();

	// export needs its destination from the body before we take the device lock
	std::string dest_sub, dest_abs;
	if (action == "export") {
		picojson::value req;
		if (!read_json_body(conn, &req) || !req.get("dest").is<std::string>()) {
			send_error(conn, 400, "body must be {\"dest\":..}");
			return;
		}
		dest_sub = webstorage_image_subpath(req.get("dest").get<std::string>());
		if (!valid_subpath(dest_sub)) {
			send_error(conn, 400, "\"" + dest_sub + "\" is not a valid image path");
			return;
		}
		size_t slash = dest_sub.rfind('/');
		if (slash != std::string::npos && !make_dirs(dest_sub.substr(0, slash))) {
			send_error(conn, 500, "cannot create the target folder");
			return;
		}
		dest_abs = images_dir + "/" + dest_sub;
	}

	std::string drive_name = "none";
	bool ok = false;
	int fail_status = 500;
	std::string errmsg;
	{
		std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
		storageimage_cow_c *cow;
		storagedrive_c *drive = find_open_drive(base, &cow);
		// An image no drive holds open is worked on through its own files,
		// with the device list locked so no drive opens it meanwhile.
		std::unique_ptr<storageimage_cow_c> closed;
		if (drive == nullptr) {
			uint64_t dirty, bytes;
			if (storageimage_cow_c::probe(image_abs(base), &dirty, &bytes)) {
				closed.reset(new storageimage_cow_c(image_abs(base)));
				if (closed->open(nullptr, false))
					cow = closed.get();
			}
		} else if (running) {
			send_error(conn, 409,
					"halt the machine before changing an image overlay");
			return;
		} else
			drive_name = drive->name.value;
		if (cow == nullptr || !cow->has_overlay()) {
			send_error(conn, drive != nullptr ? 409 : 404,
					drive != nullptr
					? "drive " + drive->name.value + " writes \"" + base + "\" in place"
					: "\"" + base + "\" has no copy-on-write overlay");
			return;
		}
		if (action == "discard") {
			cow->discard();
			ok = true;
		} else if (action == "commit") {
			// consolidating writes into the base needs a writable base; a base
			// the user made read-only cannot be committed into (its permissions
			// are never touched), so fail with a clear, specific message
			if (medium_write_protected(image_abs(base))) {
				fail_status = 409;
				errmsg = "cannot consolidate into read-only base image \"" + base + "\"";
			} else {
				ok = cow->commit();
				if (!ok)
					errmsg = "commit failed (see the log)";
			}
		} else if (action == "export") {
			cow->export_to(dest_abs);
			ok = true;
		} else {
			errmsg = "unknown overlay action";
		}
	}
	if (!ok) {
		send_error(conn, fail_status, errmsg.empty() ? "overlay operation failed" : errmsg);
		return;
	}
	if (action == "export") {
		own_by_qunilator(dest_abs);
		WEB_INFO("image %s overlay exported to %s", base.c_str(), dest_sub.c_str());
	} else {
		WEB_INFO("image %s overlay %s (drive %s)", base.c_str(), action.c_str(),
				drive_name.c_str());
	}
	picojson::object res;
	res["ok"] = picojson::value(true);
	if (action == "export")
		res["dest"] = picojson::value(dest_sub);
	send_json(conn, 200, picojson::value(res));
}

// /api/images and /api/images/<subpath>
static int api_images_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/images"));

	if (rest.empty() || rest == "/") {
		if (strcmp(ri->request_method, "GET") == 0)
			images_list(conn);
		else if (strcmp(ri->request_method, "POST") == 0) {
			// Both add an image to the library, so both are a POST to it; what
			// the body carries says which. A multipart body is a file arriving
			// from the operator's machine, a JSON one is a blank medium to make.
			const char *ctype = mg_get_header(conn, "Content-Type");
			if (ctype != nullptr && strstr(ctype, "json") != nullptr)
				image_create(conn);
			else
				images_upload(conn);
		} else {
			send_error(conn, 405, "GET or POST required");
			return 405;
		}
		return 200;
	}

	std::string sub = rest.substr(1); // after the leading slash

	// GET /api/images/<subpath>/contents — list the files inside an image
	static const std::string CONT = "/contents";
	if (sub.size() > CONT.size()
			&& sub.compare(sub.size() - CONT.size(), CONT.size(), CONT) == 0) {
		std::string base = sub.substr(0, sub.size() - CONT.size());
		if (!valid_subpath(base)) {
			send_error(conn, 404, "unknown image");
			return 404;
		}
		if (strcmp(ri->request_method, "GET") != 0) {
			send_error(conn, 405, "GET required");
			return 405;
		}
		image_contents(conn, base);
		return 200;
	}

	// POST /api/images/<subpath>/overlay/<discard|commit|export>
	static const std::string OVL = "/overlay/";
	size_t ovl_at = sub.rfind(OVL);
	if (ovl_at != std::string::npos) {
		std::string overlay_base = sub.substr(0, ovl_at);
		std::string action = sub.substr(ovl_at + OVL.size());
		if (!valid_subpath(overlay_base)
				|| (action != "discard" && action != "commit" && action != "export")) {
			send_error(conn, 404, "unknown overlay action");
			return 404;
		}
		if (strcmp(ri->request_method, "POST") != 0) {
			send_error(conn, 405, "POST required");
			return 405;
		}
		image_overlay_op(conn, overlay_base, action);
		return 200;
	}

	if (!valid_subpath(sub)) {
		send_error(conn, 404, "unknown image");
		return 404;
	}
	std::string path = image_abs(sub);
	struct stat st;
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
		send_error(conn, 404, "unknown image \"" + sub + "\"");
		return 404;
	}
	if (strcmp(ri->request_method, "GET") == 0) {
		mg_send_mime_file(conn, path.c_str(), "application/octet-stream");
		return 200;
	}
	if (strcmp(ri->request_method, "DELETE") == 0) {
		image_delete(conn, sub);
		return 200;
	}
	send_error(conn, 405, "GET or DELETE required");
	return 405;
}

// POST /api/move
static int api_move_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "POST") != 0) {
		send_error(conn, 405, "POST required");
		return 405;
	}
	image_move(conn);
	return 200;
}

// GET /api/roms, POST /api/roms  and  GET /api/roms/labels
static int api_roms_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	if (uri == "/api/roms/labels") {
		if (strcmp(ri->request_method, "GET") != 0) {
			send_error(conn, 405, "GET required");
			return 405;
		}
		rom_labels(conn);
		return 200;
	}
	if (strcmp(ri->request_method, "GET") == 0)
		package_roms_list(conn);
	else if (strcmp(ri->request_method, "POST") == 0)
		package_rom_copy(conn);
	else {
		send_error(conn, 405, "GET or POST required");
		return 405;
	}
	return 200;
}

// POST /api/folders {path}  and  DELETE /api/folders/<subpath>
static int api_folders_handler(struct mg_connection *conn, void *) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/folders"));

	if (rest.empty() || rest == "/") {
		if (strcmp(ri->request_method, "POST") != 0) {
			send_error(conn, 405, "POST required");
			return 405;
		}
		picojson::value req;
		if (!read_json_body(conn, &req) || !req.get("path").is<std::string>()) {
			send_error(conn, 400, "body must be {\"path\":..}");
			return 400;
		}
		std::string sub = webstorage_image_subpath(req.get("path").get<std::string>());
		if (!valid_subpath(sub)) {
			send_error(conn, 400, "\"" + sub + "\" is not a valid folder path");
			return 400;
		}
		if (!make_dirs(sub)) {
			send_error(conn, 500, "cannot create folder \"" + sub + "\"");
			return 500;
		}
		own_by_qunilator(images_dir + "/" + sub);
		WEB_INFO("folder %s created", sub.c_str());
		picojson::object res;
		res["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(res));
		return 200;
	}

	// DELETE /api/folders/<subpath>
	std::string sub = rest.substr(1);
	if (!valid_subpath(sub)) {
		send_error(conn, 404, "unknown folder");
		return 404;
	}
	if (strcmp(ri->request_method, "DELETE") != 0) {
		send_error(conn, 405, "DELETE required");
		return 405;
	}
	std::string abs = images_dir + "/" + sub;
	struct stat st;
	if (lstat(abs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
		send_error(conn, 404, "unknown folder \"" + sub + "\"");
		return 404;
	}
	if (rmdir(abs.c_str()) != 0) {
		send_error(conn, 409, "folder \"" + sub + "\" is not empty");
		return 409;
	}
	WEB_INFO("folder %s removed", sub.c_str());
	picojson::object res;
	res["ok"] = picojson::value(true);
	send_json(conn, 200, picojson::value(res));
	return 200;
}

void webstorage_register(struct mg_context *ctx) {
	const char *base = getenv("QUNILATOR_DIR");
	if (base == nullptr)
		base = getenv("HOME");
	images_dir = std::string(base ? base : ".") + "/images";
	mkdir(images_dir.c_str(), 0755); // may already exist
	mg_set_request_handler(ctx, "/api/images", api_images_handler, nullptr);
	mg_set_request_handler(ctx, "/api/move", api_move_handler, nullptr);
	mg_set_request_handler(ctx, "/api/folders", api_folders_handler, nullptr);
	mg_set_request_handler(ctx, "/api/roms", api_roms_handler, nullptr);
}
