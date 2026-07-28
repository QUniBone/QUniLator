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

// the one directory the web interface keeps images in
const std::string &webstorage_images_dir();

// a bare image name resolved against that directory. A value carrying a
// directory of its own is returned unchanged: the menu can attach an image
// from anywhere, and the web interface reports such a drive by its full
// path rather than pretending the file is one it manages.
std::string webstorage_image_path(const std::string &name);

// An enabled drive other than "except" already holding this image file, or
// empty. Two drives on one image both write it, so the second attachment is
// refused rather than left to corrupt the file.
std::string webstorage_image_held_by(const std::string &path, const std::string &except);

#endif // _WEBSTORAGE_HPP_
