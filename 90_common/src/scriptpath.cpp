/* scriptpath.cpp: resolve file names of a command script against its directory

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

 See scriptpath.hpp for what this is for.
 */

#include <stdlib.h>
#include <sys/stat.h>

#include "scriptpath.hpp"

// Directory of the command script under execution, without trailing "/".
// Empty: no script, or it sits in the current directory anyway.
static std::string script_dir;

// The directory part of <path>, by text alone.
// "" if there is none, "/" for a file in the root directory.
static std::string dirname_of(const std::string& path)
{
	size_t pos = path.find_last_of('/');
	if (pos == std::string::npos)
		return "";	// no directory part: the current one
	if (pos == 0)
		return "/";	// file in the root directory
	return path.substr(0, pos);
}

void scriptpath_set(const std::string& script_filepath)
{
	// The script is regularly reached through a symbolic link: the shortcuts in
	// the operator's home directory point into 10.03_app_demo/5_applications,
	// and the images and listings a script names sit next to the script itself,
	// not next to the link. So resolve the link first and take the directory of
	// what it points at.
	char *real = realpath(script_filepath.c_str(), NULL);
	if (real) {
		script_dir = dirname_of(real);
		free(real);
	} else
		// Gone, or unreadable on the way: fall back to the name as given, which
		// is what the caller opened it under anyway.
		script_dir = dirname_of(script_filepath);
}

std::string scriptpath_dir(void)
{
	return script_dir;
}

std::string scriptpath_resolve(const std::string& path)
{
	if (script_dir.empty() || path.empty())
		return path;	// no script running, or nothing to resolve
	if (path[0] == '/')
		return path;	// absolute, means just itself

	// "dir" or "dir/", both may occur: script_dir is "/" for a script in the
	// root directory
	std::string candidate = script_dir;
	if (candidate[candidate.size() - 1] != '/')
		candidate += "/";
	candidate += path;

	struct stat statbuff;
	if (stat(candidate.c_str(), &statbuff) == 0)
		return candidate;	// there it is, next to the script

	// Not next to the script: leave the path alone, so it refers to the
	// directory the user started the script from. This is what makes a file the
	// application creates appear there, and not next to the script.
	return path;
}
