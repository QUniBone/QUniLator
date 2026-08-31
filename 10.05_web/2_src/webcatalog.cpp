/* webcatalog.cpp: /api/catalog — configurations offered by subscribed catalogues

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   A catalogue is a static JSON index at a URL, naming .qcfg.zip bundles — the
   same bundles the configuration export writes: one configuration document
   plus images/<subpath> entries for every image it names. The board fetches
   both itself (libcurl), verifies the zip against the index's sha256, streams
   the images into $QUNILATOR_DIR/images — skipping any that are already
   there, the same keep-existing rule the browser import applies — and imports
   the document through webconfigs_import(), the exact path a hand-fed import
   takes.

   One job runs at a time, on a thread of this service. Its status is an
   in-memory struct published as a {"t":"catalog"} frame on /ws/events
   whenever it changes, and replayed to every page that connects, so a tab
   opened mid-download shows the progress bar at once. A service restart
   aborts a running job; the fetched images it leaves behind are valid media,
   and a retried fetch skips them.

   The index cache ($QUNILATOR_DIR/catalog/sources.json) persists what each
   subscribed catalogue offered when it was last reachable, so one dead
   catalogue degrades to a stale listing rather than an empty one.
*/

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "civetweb.h"
#include "miniz.h"
#include "picojson.h"
#include "picosha2.h"

#include "weblog.hpp"
#include "webconfigs.hpp"
#include "webevents.hpp"
#include "websettings.hpp"
#include "webstorage.hpp"
#include "webversion.hpp"
#include "webcatalog.hpp"

#if defined(QBUS)
static const char *platform_bus = "qbus";
#elif defined(UNIBUS)
static const char *platform_bus = "unibus";
#else
static const char *platform_bus = "any"; // a host build carries no bus to mismatch
#endif

// an index is a listing, not a payload; anything larger is not one
static const size_t INDEX_MAX_BYTES = 1024 * 1024;
static const long INDEX_TIMEOUT_S = 15;
// headroom the disk-space precheck keeps free beyond the zip and its images
static const unsigned long long SPACE_MARGIN = 64ULL * 1024 * 1024;

static std::string state_dir(void) {
	return websettings_state_dir() + "/catalog";
}

static std::string sources_path(void) {
	return state_dir() + "/sources.json";
}

static std::string download_path(void) {
	return state_dir() + "/download.zip";
}

// ---- job status ----

struct job_status_c {
	std::string state = "idle"; // idle refreshing downloading verifying
	                            // extracting importing done failed cancelled
	std::string mode, source, entry, config, title;
	unsigned long long bytes_done = 0, bytes_total = 0;
	std::string file; // the image being extracted
	unsigned files_done = 0, files_total = 0;
	unsigned images_written = 0;
	std::vector<std::string> images_kept;
	std::string error, note, autostart_note;
};

// cat_mutex guards the status, the sources cache and the job bookkeeping.
// status_seq moves with every change; the events broadcast loop publishes a
// frame whenever it has.
static std::mutex cat_mutex;
static job_status_c status;
static uint64_t status_seq = 1;
static uint64_t published_seq = 0;
static bool job_active = false;
static std::atomic<bool> cancel_requested(false);
// what each subscribed catalogue offered when last reachable; the parsed
// contents of sources.json
static picojson::value sources_cache;

static std::string now_iso(void) {
	char buf[32];
	time_t t = time(nullptr);
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

// caller holds cat_mutex
static void bump_locked(void) {
	status_seq++;
}

static void set_state(const std::string &state) {
	std::lock_guard<std::mutex> lock(cat_mutex);
	status.state = state;
	bump_locked();
}

static void set_extract_file(const std::string &file, unsigned long long total) {
	std::lock_guard<std::mutex> lock(cat_mutex);
	status.file = file;
	status.bytes_done = 0;
	status.bytes_total = total;
	bump_locked();
}

// The job ends here, whatever happened. WEB_INFO/WEB_WARNING put the outcome
// in the journal, so a fetch can be read back from the board afterwards.
static void finish_job(const std::string &state, const std::string &error) {
	std::lock_guard<std::mutex> lock(cat_mutex);
	status.state = state;
	status.error = error;
	job_active = false;
	bump_locked();
	if (error.empty())
		WEB_INFO("catalog: %s %s: %s", status.mode.c_str(),
				status.entry.empty() ? "" : status.entry.c_str(), state.c_str());
	else
		WEB_WARNING("catalog: %s %s: %s (%s)", status.mode.c_str(),
				status.entry.empty() ? "" : status.entry.c_str(), state.c_str(),
				error.c_str());
}

// caller holds cat_mutex
static picojson::value status_json_locked(void) {
	picojson::object o;
	o["state"] = picojson::value(status.state);
	o["mode"] = picojson::value(status.mode);
	o["source"] = picojson::value(status.source);
	o["entry"] = picojson::value(status.entry);
	o["config"] = picojson::value(status.config);
	o["title"] = picojson::value(status.title);
	o["bytes_done"] = picojson::value((double) status.bytes_done);
	o["bytes_total"] = picojson::value((double) status.bytes_total);
	o["file"] = picojson::value(status.file);
	o["files_done"] = picojson::value((double) status.files_done);
	o["files_total"] = picojson::value((double) status.files_total);
	o["images_written"] = picojson::value((double) status.images_written);
	picojson::array kept;
	for (const std::string &k : status.images_kept)
		kept.push_back(picojson::value(k));
	o["images_kept"] = picojson::value(kept);
	o["error"] = picojson::value(status.error);
	o["note"] = picojson::value(status.note);
	o["autostart_note"] = picojson::value(status.autostart_note);
	return picojson::value(o);
}

std::string webcatalog_event_json(void) {
	std::lock_guard<std::mutex> lock(cat_mutex);
	picojson::value v = status_json_locked();
	picojson::object o = v.get<picojson::object>();
	o["t"] = picojson::value(std::string("catalog"));
	return picojson::value(o).serialize();
}

bool webcatalog_poll(void) {
	std::lock_guard<std::mutex> lock(cat_mutex);
	if (status_seq == published_seq)
		return false;
	published_seq = status_seq;
	return true;
}

// ---- HTTP helpers (module-local, the house idiom) ----

static void send_json(struct mg_connection *conn, int http, const picojson::value &val) {
	std::string body = val.serialize();
	mg_printf(conn,
			"HTTP/1.1 %d %s\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %u\r\n\r\n",
			http, http == 200 ? "OK" : (http == 202 ? "Accepted" : "Error"),
			(unsigned) body.size());
	mg_write(conn, body.c_str(), body.size());
}

static void send_error(struct mg_connection *conn, int http, const std::string &message) {
	picojson::object err;
	err["error"] = picojson::value(message);
	send_json(conn, http, picojson::value(err));
}

static bool read_json_body(struct mg_connection *conn, picojson::value *out) {
	std::string body;
	char buf[4096];
	int n;
	while ((n = mg_read(conn, buf, sizeof buf)) > 0) {
		body.append(buf, (size_t) n);
		if (body.size() > 64 * 1024)
			return false; // a request, not an upload
	}
	if (body.empty())
		return false;
	return picojson::parse(*out, body).empty() && out->is<picojson::object>();
}

// ---- fetching (libcurl) ----

static CURL *curl_begin(const std::string &url) {
	CURL *h = curl_easy_init();
	if (h == nullptr)
		return nullptr;
	static std::string agent = webversion_package() + "/" + webversion_version();
	curl_easy_setopt(h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(h, CURLOPT_USERAGENT, agent.c_str());
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); // whatever curl offers
	return h;
}

static size_t index_write_cb(char *data, size_t size, size_t nmemb, void *userp) {
	std::string *body = (std::string *) userp;
	size_t n = size * nmemb;
	if (body->size() + n > INDEX_MAX_BYTES)
		return 0; // over the cap: abort the transfer
	body->append(data, n);
	return n;
}

static bool fetch_index(const std::string &url, std::string *body, std::string *err) {
	CURL *h = curl_begin(url);
	if (h == nullptr) {
		*err = "curl could not be initialized";
		return false;
	}
	body->clear();
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, index_write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, body);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, INDEX_TIMEOUT_S);
	CURLcode rc = curl_easy_perform(h);
	long code = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(h);
	if (rc == CURLE_WRITE_ERROR) {
		*err = "the index is larger than a catalogue index can be";
		return false;
	}
	if (rc != CURLE_OK) {
		*err = curl_easy_strerror(rc);
		return false;
	}
	if (code != 200) {
		*err = "the server answered " + std::to_string(code);
		return false;
	}
	return true;
}

// The zip download: streamed to disk, hashed as it arrives so verification
// costs no second read, progress published through the job status, cancel
// honoured between chunks.
struct zip_download_c {
	FILE *out;
	picosha2::hash256_one_by_one hasher;
	unsigned long long received;
};

static size_t zip_write_cb(char *data, size_t size, size_t nmemb, void *userp) {
	zip_download_c *dl = (zip_download_c *) userp;
	size_t n = size * nmemb;
	if (cancel_requested)
		return 0;
	if (fwrite(data, 1, n, dl->out) != n)
		return 0;
	dl->hasher.process(data, data + n);
	dl->received += n;
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		status.bytes_done = dl->received;
		bump_locked();
	}
	return n;
}

static bool fetch_zip(const std::string &url, const std::string &path,
		std::string *sha_hex, std::string *err) {
	CURL *h = curl_begin(url);
	if (h == nullptr) {
		*err = "curl could not be initialized";
		return false;
	}
	zip_download_c dl;
	dl.out = fopen(path.c_str(), "wb");
	dl.received = 0;
	if (dl.out == nullptr) {
		curl_easy_cleanup(h);
		*err = "the download file could not be created";
		return false;
	}
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, zip_write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &dl);
	// No overall timeout - the bundle may be hundreds of MB on a slow line -
	// but a transfer that moves less than a byte a second for a minute is dead.
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
	CURLcode rc = curl_easy_perform(h);
	long code = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(h);
	bool write_ok = fclose(dl.out) == 0;
	if (cancel_requested) {
		*err = "cancelled";
		return false;
	}
	if (rc != CURLE_OK) {
		*err = curl_easy_strerror(rc);
		return false;
	}
	if (code != 200) {
		*err = "the server answered " + std::to_string(code);
		return false;
	}
	if (!write_ok) {
		*err = "the download could not be written";
		return false;
	}
	dl.hasher.finish();
	*sha_hex = picosha2::get_hash_hex_string(dl.hasher);
	return true;
}

// A bundle URL relative to the index that names it: "foo.qcfg.zip" beside the
// index, "/x/y.zip" on the index's host, or absolute. ".." is not resolved -
// a catalogue that needs it can write the URL absolute.
static std::string resolve_url(const std::string &base, const std::string &rel) {
	if (rel.compare(0, 7, "http://") == 0 || rel.compare(0, 8, "https://") == 0)
		return rel;
	size_t scheme = base.find("://");
	if (scheme == std::string::npos)
		return "";
	if (!rel.empty() && rel[0] == '/') {
		size_t slash = base.find('/', scheme + 3);
		return (slash == std::string::npos ? base : base.substr(0, slash)) + rel;
	}
	std::string r = rel;
	if (r.compare(0, 2, "./") == 0)
		r = r.substr(2);
	size_t last = base.rfind('/');
	if (last == std::string::npos || last < scheme + 3)
		return base + "/" + r;
	return base.substr(0, last + 1) + r;
}

// ---- the index cache ----

static bool read_sources_file(picojson::value *out) {
	std::ifstream f(sources_path().c_str());
	if (!f)
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	return picojson::parse(*out, ss.str()).empty() && out->is<picojson::object>();
}

// caller holds cat_mutex; writes the cache to disk, temp + rename
static void write_sources_locked(void) {
	std::string tmp = sources_path() + ".new";
	{
		std::ofstream f(tmp.c_str());
		if (!f || !(f << sources_cache.serialize()))
			return;
	}
	chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
	rename(tmp.c_str(), sources_path().c_str());
}

// the cached record for one subscribed URL, or null
static picojson::value cached_source_locked(const std::string &url) {
	if (!sources_cache.is<picojson::object>())
		return picojson::value();
	const picojson::value &arr = sources_cache.get("sources");
	if (!arr.is<picojson::array>())
		return picojson::value();
	for (const picojson::value &s : arr.get<picojson::array>())
		if (s.is<picojson::object>() && s.get("url").is<std::string>()
				&& s.get("url").get<std::string>() == url)
			return s;
	return picojson::value();
}

// A structurally valid catalogue index this reader understands: the
// "qunilator-catalog/1" schema the project site publishes at
// /catalog/v1/index.json, generated from docs/site/src/content/configurations.
static bool index_is_valid(const picojson::value &v, std::string *err) {
	if (!v.is<picojson::object>()) {
		*err = "the index is not a JSON object";
		return false;
	}
	if (!v.get("schema").is<std::string>()
			|| v.get("schema").get<std::string>() != "qunilator-catalog/1") {
		*err = "the index has no schema this reader understands";
		return false;
	}
	if (!v.get("configurations").is<picojson::array>()) {
		*err = "the index lists no configurations";
		return false;
	}
	return true;
}

// the entry of this id in a source's cached index, or null
static picojson::value cached_entry_locked(const std::string &source_url,
		const std::string &entry_id) {
	picojson::value src = cached_source_locked(source_url);
	if (!src.is<picojson::object>() || !src.get("index").is<picojson::object>())
		return picojson::value();
	const picojson::value &cfgs = src.get("index").get("configurations");
	if (!cfgs.is<picojson::array>())
		return picojson::value();
	for (const picojson::value &e : cfgs.get<picojson::array>())
		if (e.is<picojson::object>() && e.get("id").is<std::string>()
				&& e.get("id").get<std::string>() == entry_id)
			return e;
	return picojson::value();
}

// the download record {url, bytes, sha256} of an entry, or null
static picojson::value entry_download(const picojson::value &entry) {
	const picojson::value &dl = entry.get("download");
	return dl.is<picojson::object>() ? dl : picojson::value();
}

// ---- the jobs ----

static void refresh_job(void) {
	std::vector<std::string> urls = websettings_catalog_sources();
	picojson::array out;
	for (const std::string &url : urls) {
		if (cancel_requested)
			break;
		std::string body, err;
		picojson::object rec;
		rec["url"] = picojson::value(url);
		picojson::value idx;
		bool ok = fetch_index(url, &body, &err);
		if (ok) {
			std::string perr = picojson::parse(idx, body);
			if (!perr.empty()) {
				ok = false;
				err = "the index is not JSON: " + perr;
			} else {
				ok = index_is_valid(idx, &err);
			}
		}
		if (ok) {
			rec["ok"] = picojson::value(true);
			rec["error"] = picojson::value(std::string(""));
			rec["fetched_at"] = picojson::value(now_iso());
			rec["index"] = idx;
		} else {
			// unreachable or unreadable: keep what it offered last time, so
			// the listing goes stale rather than empty
			rec["ok"] = picojson::value(false);
			rec["error"] = picojson::value(err);
			std::lock_guard<std::mutex> lock(cat_mutex);
			picojson::value old = cached_source_locked(url);
			if (old.is<picojson::object>()) {
				if (old.get("index").is<picojson::object>())
					rec["index"] = old.get("index");
				if (old.get("fetched_at").is<std::string>())
					rec["fetched_at"] = old.get("fetched_at");
			}
		}
		out.push_back(picojson::value(rec));
	}
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		picojson::object root;
		root["refreshed_at"] = picojson::value(now_iso());
		root["sources"] = picojson::value(out);
		sources_cache = picojson::value(root);
		write_sources_locked();
	}
	finish_job("idle", "");
}

// bytes still to find on disk for this entry: the zip plus every image the
// index names that is not already present
static unsigned long long space_needed_locked(const picojson::value &entry) {
	unsigned long long need = SPACE_MARGIN;
	picojson::value dl = entry_download(entry);
	if (dl.is<picojson::object>() && dl.get("bytes").is<double>())
		need += (unsigned long long) dl.get("bytes").get<double>();
	if (entry.get("images").is<picojson::array>())
		for (const picojson::value &im : entry.get("images").get<picojson::array>()) {
			if (!im.is<picojson::object>() || !im.get("path").is<std::string>())
				continue;
			std::string sub = im.get("path").get<std::string>();
			struct stat st;
			if (stat((webstorage_images_dir() + "/" + sub).c_str(), &st) == 0)
				continue; // present: the fetch will keep it
			if (im.get("bytes").is<double>())
				need += (unsigned long long) im.get("bytes").get<double>();
		}
	return need;
}

static bool space_available(unsigned long long need) {
	struct statvfs vfs;
	if (statvfs(websettings_state_dir().c_str(), &vfs) != 0)
		return true; // no answer is no reason to refuse
	unsigned long long free_bytes =
			(unsigned long long) vfs.f_bavail * vfs.f_frsize;
	return free_bytes >= need;
}

// miniz hands extracted data to this; writing to a dot-leading temp file
// beside the final path, which the images listing cannot see and the API
// cannot reach, so a partial image never looks like a whole one.
struct extract_sink_c {
	FILE *out;
	unsigned long long written;
};

static size_t extract_write_cb(void *opaque, mz_uint64 /*ofs*/, const void *buf, size_t n) {
	extract_sink_c *sink = (extract_sink_c *) opaque;
	if (cancel_requested)
		return 0;
	if (fwrite(buf, 1, n, sink->out) != n)
		return 0;
	sink->written += n;
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		status.bytes_done = sink->written;
		bump_locked();
	}
	return n;
}

static void fetch_job(std::string source_url, std::string entry_name,
		std::string config_name) {
	picojson::value entry;
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		entry = cached_entry_locked(source_url, entry_name);
	}
	if (!entry.is<picojson::object>()) {
		finish_job("failed", "the entry is no longer in the catalogue");
		return;
	}
	picojson::value dl = entry_download(entry);
	std::string zip_url = dl.is<picojson::object>() && dl.get("url").is<std::string>()
			? resolve_url(source_url, dl.get("url").get<std::string>()) : "";
	if (zip_url.empty()) {
		finish_job("failed", "the entry names no bundle URL");
		return;
	}
	// the click-to-start window is long enough for the disk to fill; check
	// again here, where it counts
	bool have_space;
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		have_space = space_available(space_needed_locked(entry));
		if (have_space) {
			status.state = "downloading";
			status.bytes_done = 0;
			status.bytes_total = dl.get("bytes").is<double>()
					? (unsigned long long) dl.get("bytes").get<double>() : 0;
			bump_locked();
		}
	}
	if (!have_space) {
		finish_job("failed", "not enough disk space for this machine");
		return;
	}

	std::string zip_path = download_path();
	std::string sha_hex, err;
	if (!fetch_zip(zip_url, zip_path, &sha_hex, &err)) {
		unlink(zip_path.c_str());
		finish_job(cancel_requested ? "cancelled" : "failed",
				cancel_requested ? "" : "download failed: " + err);
		return;
	}
	set_state("verifying");
	if (dl.get("sha256").is<std::string>()) {
		std::string want = dl.get("sha256").get<std::string>();
		for (char &c : want)
			c = (char) tolower(c);
		if (want != sha_hex) {
			unlink(zip_path.c_str());
			finish_job("failed", "checksum mismatch: the download is not the "
					"bundle the catalogue describes");
			return;
		}
	}

	// walk the archive once: the configuration document and the image entries
	mz_zip_archive zip;
	memset(&zip, 0, sizeof zip);
	if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
		unlink(zip_path.c_str());
		finish_job("failed", "the bundle is not a zip archive");
		return;
	}
	int doc_index = -1;
	std::vector<mz_uint> image_indices;
	mz_uint n_entries = mz_zip_reader_get_num_files(&zip);
	for (mz_uint i = 0; i < n_entries; i++) {
		mz_zip_archive_file_stat st;
		if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory)
			continue;
		std::string name = st.m_filename;
		if (name.compare(0, 7, "images/") == 0)
			image_indices.push_back(i);
		else if (doc_index < 0 && name.find('/') == std::string::npos
				&& name.size() > 5
				&& name.compare(name.size() - 5, 5, ".json") == 0)
			doc_index = (int) i;
	}
	if (doc_index < 0) {
		mz_zip_reader_end(&zip);
		unlink(zip_path.c_str());
		finish_job("failed", "the bundle holds no configuration document");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		status.state = "extracting";
		status.files_total = (unsigned) image_indices.size();
		status.files_done = 0;
		bump_locked();
	}

	for (mz_uint i : image_indices) {
		if (cancel_requested)
			break;
		mz_zip_archive_file_stat st;
		if (!mz_zip_reader_file_stat(&zip, i, &st))
			continue;
		std::string sub = std::string(st.m_filename).substr(7);
		if (!webstorage_valid_subpath(sub)) {
			mz_zip_reader_end(&zip);
			unlink(zip_path.c_str());
			finish_job("failed", "the bundle names an image path it may not: "
					+ sub);
			return;
		}
		std::string abs = webstorage_images_dir() + "/" + sub;
		struct stat fst;
		if (stat(abs.c_str(), &fst) == 0) {
			// already here: kept, never overwritten - a drive may hold it
			std::lock_guard<std::mutex> lock(cat_mutex);
			status.images_kept.push_back(sub);
			status.files_done++;
			bump_locked();
			continue;
		}
		size_t slash = sub.rfind('/');
		std::string dir_sub = slash == std::string::npos ? "" : sub.substr(0, slash);
		std::string base = slash == std::string::npos ? sub : sub.substr(slash + 1);
		if (!webstorage_make_dirs(dir_sub)) {
			mz_zip_reader_end(&zip);
			unlink(zip_path.c_str());
			finish_job("failed", "the image folder could not be created: " + dir_sub);
			return;
		}
		std::string tmp = webstorage_images_dir() + "/"
				+ (dir_sub.empty() ? "" : dir_sub + "/")
				+ ".catalog-" + base + ".part";
		set_extract_file(sub, st.m_uncomp_size);
		extract_sink_c sink;
		sink.out = fopen(tmp.c_str(), "wb");
		sink.written = 0;
		if (sink.out == nullptr) {
			mz_zip_reader_end(&zip);
			unlink(zip_path.c_str());
			finish_job("failed", "the image could not be written: " + sub);
			return;
		}
		mz_bool ok = mz_zip_reader_extract_to_callback(&zip, i,
				extract_write_cb, &sink, 0);
		bool closed = fclose(sink.out) == 0;
		if (!ok || !closed || cancel_requested) {
			unlink(tmp.c_str());
			if (cancel_requested)
				break;
			mz_zip_reader_end(&zip);
			unlink(zip_path.c_str());
			finish_job("failed", "the bundle could not be unpacked at " + sub);
			return;
		}
		chmod(tmp.c_str(), 0664);
		if (rename(tmp.c_str(), abs.c_str()) != 0) {
			unlink(tmp.c_str());
			mz_zip_reader_end(&zip);
			unlink(zip_path.c_str());
			finish_job("failed", "the image could not be placed: " + sub);
			return;
		}
		webstorage_own_by_qunilator(abs);
		std::lock_guard<std::mutex> lock(cat_mutex);
		status.images_written++;
		status.files_done++;
		bump_locked();
	}
	if (cancel_requested) {
		mz_zip_reader_end(&zip);
		unlink(zip_path.c_str());
		finish_job("cancelled", "");
		return;
	}

	size_t doc_size = 0;
	char *doc_raw = (char *) mz_zip_reader_extract_to_heap(&zip,
			(mz_uint) doc_index, &doc_size, 0);
	mz_zip_reader_end(&zip);
	unlink(zip_path.c_str());
	if (doc_raw == nullptr) {
		finish_job("failed", "the configuration document could not be read");
		return;
	}
	picojson::value doc;
	std::string perr = picojson::parse(doc, std::string(doc_raw, doc_size));
	mz_free(doc_raw);
	if (!perr.empty()) {
		finish_job("failed", "the configuration document is not JSON: " + perr);
		return;
	}

	set_state("importing");
	std::string dip_note, autostart_note, error;
	int http = 422;
	if (!webconfigs_import(config_name, doc, &dip_note, &autostart_note,
			&error, &http)) {
		finish_job("failed", error);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		status.note = dip_note;
		status.autostart_note = autostart_note;
	}
	finish_job("done", "");
}

// Start a job thread. The thread is detached: nothing ever joins it, its
// completion is the job_active flag it clears, and the process going down
// takes it along like every other worker thread here.
template <typename F>
static bool launch_job(const job_status_c &initial, F body, std::string *err) {
	std::lock_guard<std::mutex> lock(cat_mutex);
	if (job_active) {
		*err = "a catalogue job is already running";
		return false;
	}
	cancel_requested = false;
	status = initial;
	job_active = true;
	bump_locked();
	std::thread(body).detach();
	return true;
}

// ---- endpoints ----

// GET /api/catalog: the subscribed sources in their configured order, each
// with what it offered when last reachable, decorated with what only this
// board knows - which entries are already imported, which of their images are
// already here, and whether the bus matches - plus the current job.
static void catalog_get(struct mg_connection *conn) {
	std::vector<std::string> urls = websettings_catalog_sources();
	std::lock_guard<std::mutex> lock(cat_mutex);
	picojson::object root;
	if (sources_cache.is<picojson::object>()
			&& sources_cache.get("refreshed_at").is<std::string>())
		root["refreshed_at"] = sources_cache.get("refreshed_at");
	else
		root["refreshed_at"] = picojson::value(std::string(""));
	root["bus"] = picojson::value(std::string(platform_bus));
	picojson::array sources;
	for (const std::string &url : urls) {
		picojson::value cached = cached_source_locked(url);
		picojson::object src;
		src["url"] = picojson::value(url);
		if (cached.is<picojson::object>()) {
			src["ok"] = cached.get("ok");
			src["error"] = cached.get("error");
			if (cached.get("fetched_at").is<std::string>())
				src["fetched_at"] = cached.get("fetched_at");
			if (cached.get("index").is<picojson::object>()) {
				picojson::object idx = cached.get("index").get<picojson::object>();
				picojson::array cfgs;
				if (idx["configurations"].is<picojson::array>())
					for (picojson::value e : idx["configurations"].get<picojson::array>()) {
						if (!e.is<picojson::object>())
							continue;
						picojson::object &eo = e.get<picojson::object>();
						std::string id = eo["id"].is<std::string>()
								? eo["id"].get<std::string>() : "";
						std::string bus = eo["bus"].is<std::string>()
								? eo["bus"].get<std::string>() : "any";
						eo["imported"] = picojson::value(webconfigs_exists(id));
						eo["bus_ok"] = picojson::value(bus == "any"
								|| strcmp(platform_bus, "any") == 0
								|| bus == platform_bus);
						unsigned present = 0, total = 0;
						if (eo["images"].is<picojson::array>())
							for (const picojson::value &im : eo["images"].get<picojson::array>()) {
								if (!im.is<picojson::object>()
										|| !im.get("path").is<std::string>())
									continue;
								total++;
								struct stat st;
								if (stat((webstorage_images_dir() + "/"
										+ im.get("path").get<std::string>()).c_str(),
										&st) == 0)
									present++;
							}
						eo["images_present"] = picojson::value((double) present);
						eo["images_total"] = picojson::value((double) total);
						cfgs.push_back(e);
					}
				idx["configurations"] = picojson::value(cfgs);
				src["index"] = picojson::value(idx);
			}
		} else {
			src["ok"] = picojson::value(false);
			src["error"] = picojson::value(std::string(""));
		}
		sources.push_back(picojson::value(src));
	}
	root["sources"] = picojson::value(sources);
	root["job"] = status_json_locked();
	send_json(conn, 200, picojson::value(root));
}

static void catalog_refresh(struct mg_connection *conn) {
	job_status_c initial;
	initial.state = "refreshing";
	initial.mode = "refresh";
	std::string err;
	if (!launch_job(initial, refresh_job, &err)) {
		send_error(conn, 409, err);
		return;
	}
	picojson::object o;
	o["ok"] = picojson::value(true);
	send_json(conn, 202, picojson::value(o));
}

static void catalog_fetch(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("source").is<std::string>()
			|| !req.get("entry").is<std::string>()
			|| !req.get("config").is<std::string>()) {
		send_error(conn, 400, "body must name \"source\", \"entry\" and \"config\"");
		return;
	}
	std::string source = req.get("source").get<std::string>();
	std::string entry_name = req.get("entry").get<std::string>();
	std::string config = req.get("config").get<std::string>();
	if (!webconfigs_valid_name(config)) {
		send_error(conn, 422, "\"" + config + "\" is not a configuration name");
		return;
	}
	if (webconfigs_exists(config)) {
		send_error(conn, 409, "a configuration named \"" + config
				+ "\" is already here; import under another name");
		return;
	}
	std::vector<std::string> urls = websettings_catalog_sources();
	bool subscribed = false;
	for (const std::string &u : urls)
		subscribed |= (u == source);
	if (!subscribed) {
		send_error(conn, 422, "the source is not a subscribed catalogue");
		return;
	}
	std::string title;
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		picojson::value entry = cached_entry_locked(source, entry_name);
		if (!entry.is<picojson::object>()) {
			send_error(conn, 422, "the catalogue offers no entry \"" + entry_name
					+ "\" — refresh and try again");
			return;
		}
		std::string bus = entry.get("bus").is<std::string>()
				? entry.get("bus").get<std::string>() : "any";
		if (bus != "any" && strcmp(platform_bus, "any") != 0
				&& bus != platform_bus) {
			send_error(conn, 422, "this machine needs a " + bus
					+ " backplane, which this board does not drive");
			return;
		}
		if (!space_available(space_needed_locked(entry))) {
			send_error(conn, 507, "not enough disk space for this machine");
			return;
		}
		if (entry.get("title").is<std::string>())
			title = entry.get("title").get<std::string>();
	}
	job_status_c initial;
	initial.state = "starting";
	initial.mode = "fetch";
	initial.source = source;
	initial.entry = entry_name;
	initial.config = config;
	initial.title = title;
	std::string err;
	if (!launch_job(initial,
			[source, entry_name, config] { fetch_job(source, entry_name, config); },
			&err)) {
		send_error(conn, 409, err);
		return;
	}
	WEB_INFO("catalog: fetching \"%s\" from %s as \"%s\"", entry_name.c_str(),
			source.c_str(), config.c_str());
	picojson::object o;
	o["ok"] = picojson::value(true);
	send_json(conn, 202, picojson::value(o));
}

static void catalog_cancel(struct mg_connection *conn) {
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		if (!job_active) {
			send_error(conn, 409, "no catalogue job is running");
			return;
		}
	}
	cancel_requested = true;
	picojson::object o;
	o["ok"] = picojson::value(true);
	send_json(conn, 202, picojson::value(o));
}

static bool valid_source_url(const std::string &url) {
	if (url.size() > 512)
		return false;
	if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)
		return false;
	for (char c : url)
		if (c <= ' ' || c == 0x7f)
			return false;
	return true;
}

static void catalog_sources_get(struct mg_connection *conn) {
	picojson::array arr;
	for (const std::string &u : websettings_catalog_sources())
		arr.push_back(picojson::value(u));
	picojson::object o;
	o["sources"] = picojson::value(arr);
	send_json(conn, 200, picojson::value(o));
}

static void catalog_sources_put(struct mg_connection *conn) {
	picojson::value req;
	if (!read_json_body(conn, &req) || !req.get("sources").is<picojson::array>()) {
		send_error(conn, 400, "body must be {\"sources\": [\"url\", ...]}");
		return;
	}
	const picojson::array &arr = req.get("sources").get<picojson::array>();
	if (arr.size() > 32) {
		send_error(conn, 422, "at most 32 catalogues");
		return;
	}
	std::vector<std::string> urls;
	for (const picojson::value &v : arr) {
		if (!v.is<std::string>() || !valid_source_url(v.get<std::string>())) {
			send_error(conn, 422, "a catalogue is an http(s) URL of at most "
					"512 characters");
			return;
		}
		urls.push_back(v.get<std::string>());
	}
	websettings_set_catalog_sources(urls);
	webevents_note_settings();
	// what the new list offers, without waiting to be asked; a running job
	// keeps its right of way
	job_status_c initial;
	initial.state = "refreshing";
	initial.mode = "refresh";
	std::string err;
	(void) launch_job(initial, refresh_job, &err);
	catalog_sources_get(conn);
}

static int api_catalog_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/catalog"));
	if (!rest.empty() && rest[rest.size() - 1] == '/')
		rest.erase(rest.size() - 1);
	bool is_get = strcmp(ri->request_method, "GET") == 0;
	bool is_post = strcmp(ri->request_method, "POST") == 0;
	bool is_put = strcmp(ri->request_method, "PUT") == 0;

	if (rest.empty()) {
		if (!is_get) {
			send_error(conn, 405, "GET required");
			return 405;
		}
		catalog_get(conn);
		return 200;
	}
	if (rest == "/sources") {
		if (is_get)
			catalog_sources_get(conn);
		else if (is_put)
			catalog_sources_put(conn);
		else {
			send_error(conn, 405, "GET or PUT required");
			return 405;
		}
		return 200;
	}
	if (!is_post) {
		send_error(conn, 405, "POST required");
		return 405;
	}
	if (rest == "/refresh")
		catalog_refresh(conn);
	else if (rest == "/fetch")
		catalog_fetch(conn);
	else if (rest == "/cancel")
		catalog_cancel(conn);
	else {
		send_error(conn, 404, "unknown path");
		return 404;
	}
	return 200;
}

// ---- startup ----

// remove .catalog-*.part leftovers a killed job left under the images tree
static void clean_partials(const std::string &dir, int depth) {
	if (depth > 16)
		return;
	DIR *d = opendir(dir.c_str());
	if (d == nullptr)
		return;
	struct dirent *de;
	while ((de = readdir(d)) != nullptr) {
		std::string name = de->d_name;
		if (name == "." || name == "..")
			continue;
		std::string path = dir + "/" + name;
		struct stat st;
		if (lstat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			clean_partials(path, depth + 1);
		else if (name.compare(0, 9, ".catalog-") == 0)
			unlink(path.c_str());
	}
	closedir(d);
}

void webcatalog_register(struct mg_context *ctx) {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	mkdir(state_dir().c_str(), 0700); // may already exist
	// a stale download and half-written images are what a killed job leaves
	unlink(download_path().c_str());
	clean_partials(webstorage_images_dir(), 0);
	{
		std::lock_guard<std::mutex> lock(cat_mutex);
		picojson::value v;
		if (read_sources_file(&v))
			sources_cache = v;
	}
	mg_set_request_handler(ctx, "/api/catalog", api_catalog_handler, nullptr);
}
