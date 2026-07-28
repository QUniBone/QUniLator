/* webstorage.hpp: /api/images — disk image files of the web interface

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/
#ifndef _WEBSTORAGE_HPP_
#define _WEBSTORAGE_HPP_

#include <string>

struct mg_context;

// register /api/images; images live in $QUNILATOR_DIR/images
void webstorage_register(struct mg_context *ctx);

// the one directory tree the web interface keeps images in
const std::string &webstorage_images_dir();

// The value stored in a drive's "image" parameter and in a saved config:
// relative to $QUNILATOR_DIR ("images/du/foo.dsk") so it is portable and the
// drive opens it through the working directory. Accepts a bare subpath, the
// stored form, a legacy "./images/..", or an absolute path inside the images
// tree; an unmanaged absolute path (outside it) is returned unchanged.
std::string webstorage_image_path(const std::string &value);

// The canonical images-root-relative key ("du/foo.dsk") for any of those forms
// — used to compare two references to the same image regardless of how each was
// written. An unmanaged absolute path is returned unchanged.
std::string webstorage_image_subpath(const std::string &value);

// An enabled drive other than "except" already holding this image file, or
// empty. Two drives on one image both write it, so the second attachment is
// refused rather than left to corrupt the file.
std::string webstorage_image_held_by(const std::string &path, const std::string &except);

// Make images attached to a running machine read-only over the SMB/FTP/SFTP
// shares (the file's write bits are cleared) and restore write when they detach
// or the machine halts, so a share write cannot corrupt an image the emulator
// is using. Only files this toggles are touched. Call on attach/detach and on a
// run-state change; `machine_running` is powered and not halted.
void webstorage_refresh_readonly(bool machine_running);

#endif // _WEBSTORAGE_HPP_
