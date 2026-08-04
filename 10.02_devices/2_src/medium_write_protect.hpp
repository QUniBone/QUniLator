/* medium_write_protect.hpp: an image file's mode is the medium's write ring

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see any device source header for the full text.

   Clearing an image file's write permission write-protects the medium it
   holds, the way pulling a tape's write ring or turning an RL02's WRITE PROT
   switch does. The emulator runs as root, and the kernel grants root a
   read/write open whatever a file's mode says, so a medium's protection is
   read out of the mode rather than left to open() to discover.

   The owner's write bit is the one that carries it: the image tree belongs to
   the qunilator account, an image in it is 0664, and "chmod 444" is what an
   operator writes to protect one.

   A path with no file behind it is not protected - a drive attaching a medium
   that does not exist yet creates it.
*/

#ifndef _MEDIUM_WRITE_PROTECT_HPP_
#define _MEDIUM_WRITE_PROTECT_HPP_

#include <sys/stat.h>
#include <unistd.h>

#include <string>

inline bool medium_write_protected(const std::string &path)
{
	struct stat st;
	if (path.empty() || stat(path.c_str(), &st) != 0)
		return false;
	if (!S_ISREG(st.st_mode))
		return false;
	if (geteuid() == 0)
		return (st.st_mode & S_IWUSR) == 0;
	// unprivileged, which is what a host test run is: the kernel's own answer
	// covers ownership, group membership and a read-only filesystem
	return access(path.c_str(), W_OK) != 0;
}

#endif
