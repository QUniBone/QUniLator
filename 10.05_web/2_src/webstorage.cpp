/* webstorage.cpp: /api/images — disk image files of the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Disk images live in $QUNILATOR_DIR/images (created on registration):

     GET  /api/images          list: name, size, mtime, attached drives, path
     POST /api/images          multipart upload into the images directory
     GET  /api/images/<name>   download

   Attaching an image to a drive is a parameter write on the drive
   (PUT /api/devices/<drive>/params/image with the listed path).
*/

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

#include "civetweb.h"
#include "picojson.h"

#include "logger.hpp"
#include "device.hpp"
#include "storagedrive.hpp"

#include "weblog.hpp"
#include "webstorage.hpp"
#include "webconfigs.hpp"

static std::string images_dir;

// image names are plain files in the images directory, nothing else
static bool valid_image_name(const std::string &name) {
	if (name.empty() || name[0] == '.')
		return false;
	return name.find('/') == std::string::npos
			&& name.find('\\') == std::string::npos;
}

const std::string &webstorage_images_dir() {
	return images_dir;
}

std::string webstorage_image_path(const std::string &name) {
	if (name.empty() || name.find('/') != std::string::npos)
		return name;
	return images_dir + "/" + name;
}

std::string webstorage_image_held_by(const std::string &path, const std::string &except) {
	if (path.empty())
		return "";
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		storagedrive_c *drive = dynamic_cast<storagedrive_c *>(dev);
		if (drive == nullptr || dev->name.value == except || !dev->enabled.value)
			continue;
		if (drive->image_filepath.value == path)
			return dev->name.value;
	}
	return "";
}

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

// drives an image file is attached to. The comparison is on the whole path,
// so a drive holding a same-named file from another directory is reported as
// what it is — not attached to the image listed here.
static picojson::array attached_drives(const std::string &name) {
	picojson::array result;
	std::string path = images_dir + "/" + name;
	std::lock_guard<std::mutex> lock(device_c::mydevices_mutex);
	for (device_c *dev : device_c::mydevices) {
		storagedrive_c *drive = dynamic_cast<storagedrive_c *>(dev);
		if (drive == nullptr)
			continue;
		if (webstorage_image_path(drive->image_filepath.value) == path)
			result.push_back(picojson::value(dev->name.value));
	}
	return result;
}

// GET /api/images
static void images_list(struct mg_connection *conn) {
	picojson::array images;
	DIR *dir = opendir(images_dir.c_str());
	if (dir != nullptr) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != nullptr) {
			std::string name = entry->d_name;
			if (!valid_image_name(name))
				continue;
			std::string path = images_dir + "/" + name;
			struct stat st;
			if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			picojson::object o;
			o["name"] = picojson::value(name);
			o["path"] = picojson::value(path);
			o["size"] = picojson::value((double) st.st_size);
			char mtime[32];
			strftime(mtime, sizeof(mtime), "%Y-%m-%d %H:%M",
					localtime(&st.st_mtime));
			o["mtime"] = picojson::value(mtime);
			o["attached"] = picojson::value(attached_drives(name));
			// where saved configurations put it, as configuration/drive pairs
			picojson::array uses;
			for (const config_image_use_t &u : webconfigs_image_uses(name)) {
				picojson::object e;
				e["config"] = picojson::value(u.config);
				e["device"] = picojson::value(u.device);
				uses.push_back(picojson::value(e));
			}
			o["used"] = picojson::value(uses);
			images.push_back(picojson::value(o));
		}
		closedir(dir);
	}
	send_json(conn, 200, picojson::value(images));
}

// multipart upload state
struct upload_state {
	std::string name; // file being received
	bool stored;
	bool bad_name;
};

static int upload_field_found(const char *key, const char *filename,
		char *path, size_t pathlen, void *user_data) {
	(void) key;
	upload_state *state = (upload_state *) user_data;
	if (filename == nullptr || !valid_image_name(filename)) {
		state->bad_name = true;
		return MG_FORM_FIELD_HANDLE_ABORT;
	}
	state->name = filename;
	snprintf(path, pathlen, "%s/%s", images_dir.c_str(), filename);
	return MG_FORM_FIELD_STORAGE_STORE;
}

static int upload_field_get(const char *, const char *, size_t, void *) {
	return MG_FORM_FIELD_HANDLE_NEXT; // no inline fields expected
}

static int upload_field_stored(const char *path, long long file_size, void *user_data) {
	(void) path;
	(void) file_size;
	((upload_state *) user_data)->stored = true;
	return MG_FORM_FIELD_HANDLE_NEXT;
}

// POST /api/images — multipart upload
static void images_upload(struct mg_connection *conn) {
	upload_state state;
	state.stored = false;
	state.bad_name = false;
	struct mg_form_data_handler handler;
	handler.field_found = upload_field_found;
	handler.field_get = upload_field_get;
	handler.field_store = upload_field_stored;
	handler.user_data = &state;
	mg_handle_form_request(conn, &handler);
	if (state.bad_name) {
		send_error(conn, 400, "image name must be a plain file name");
		return;
	}
	if (!state.stored) {
		send_error(conn, 400, "no file in upload");
		return;
	}
	WEB_INFO("image %s uploaded", state.name.c_str());
	picojson::object res;
	res["ok"] = picojson::value(true);
	res["name"] = picojson::value(state.name);
	send_json(conn, 200, picojson::value(res));
}

// /api/images and /api/images/<name>
static int api_images_handler(struct mg_connection *conn, void * /*cbdata*/) {
	const struct mg_request_info *ri = mg_get_request_info(conn);
	std::string uri = ri->local_uri ? ri->local_uri : "";
	std::string rest = uri.substr(strlen("/api/images"));

	if (rest.empty() || rest == "/") {
		if (strcmp(ri->request_method, "GET") == 0)
			images_list(conn);
		else if (strcmp(ri->request_method, "POST") == 0)
			images_upload(conn);
		else {
			send_error(conn, 405, "GET or POST required");
			return 405;
		}
		return 200;
	}

	// /api/images/<name>
	std::string name = rest.substr(1);
	if (!valid_image_name(name)) {
		send_error(conn, 404, "unknown image");
		return 404;
	}
	std::string path = images_dir + "/" + name;
	struct stat st;
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
		send_error(conn, 404, "unknown image \"" + name + "\"");
		return 404;
	}

	if (strcmp(ri->request_method, "GET") == 0) {
		mg_send_mime_file(conn, path.c_str(), "application/octet-stream");
		return 200;
	}
	if (strcmp(ri->request_method, "DELETE") == 0) {
		// an image in use stays: attached to a drive or part of a
		// saved configuration
		picojson::array attached = attached_drives(name);
		if (!attached.empty()) {
			send_error(conn, 409, "image is attached to "
					+ attached[0].get<std::string>());
			return 409;
		}
		std::string config = webconfigs_image_referenced(name);
		if (!config.empty()) {
			send_error(conn, 409,
					"image is referenced by configuration \"" + config + "\"");
			return 409;
		}
		if (unlink(path.c_str()) != 0) {
			send_error(conn, 500, "cannot delete image \"" + name + "\"");
			return 500;
		}
		WEB_INFO("image %s deleted", name.c_str());
		picojson::object res;
		res["ok"] = picojson::value(true);
		send_json(conn, 200, picojson::value(res));
		return 200;
	}
	send_error(conn, 405, "GET or DELETE required");
	return 405;
}

void webstorage_register(struct mg_context *ctx) {
	const char *base = getenv("QUNILATOR_DIR");
	if (base == nullptr)
		base = getenv("HOME");
	images_dir = std::string(base ? base : ".") + "/images";
	mkdir(images_dir.c_str(), 0755); // may already exist
	mg_set_request_handler(ctx, "/api/images", api_images_handler, nullptr);
}
