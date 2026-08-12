/* scriptpath.hpp: resolve file names of a command script against its directory

 Copyright (c) 2026, Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 A command file names other files: disk images, MACRO-11 listings, a shared
 host directory. It means them relative to itself - the script and its data
 travel together - while the user starts it from wherever they happen to stand.

 So a file name from a script is looked up in the script's directory first, and
 that is all this does. It is deliberately NOT a chdir() into that directory:
 then everything the application *creates* would land next to the script too,
 while the natural place for new files is the directory the script was started
 from.

 The rule is therefore: an existing file is found next to the script, a new file
 is created in the current working directory. See scriptpath_resolve().
 */

#ifndef _SCRIPTPATH_HPP_
#define _SCRIPTPATH_HPP_

#include <string>

// Remember the directory of the command script under execution.
// <script_filepath> is the script itself, its directory part is extracted.
// Symbolic links are followed first: the example scripts are reached through
// shortcuts in the operator's home directory, and what the files they name sit
// next to is the script itself, not the link.
// A path without any "/" leaves the directory empty: the script was started
// from the current directory, so there is nothing to prepend.
void scriptpath_set(const std::string& script_filepath);

// Directory of the command script, "" if no script is running or it sits in the
// current directory. Only for messages: use scriptpath_resolve() to open files.
std::string scriptpath_dir(void);

// The path under which to open a file named by the script or the user:
//	- if no script directory is known, or <path> is absolute: <path> unchanged
//	- if <path> is relative and exists in the script directory: that one
//	- else <path> unchanged, so it refers to the current working directory
// Use this for every file which is *read*, and for one which is opened if it
// exists and created if it does not - a disk image. Do NOT use it for a file the
// application only ever writes (a memory dump, a trace, a log): those belong in
// the directory the user started the script from, which is what an unresolved
// relative path means.
std::string scriptpath_resolve(const std::string& path);

#endif
