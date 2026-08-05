<!-- migrated from https://retrocmp.com/projects/unibone/325-unibone-host-file-sharing -->

## Why "host file sharing"?

Any vintage computer emulator uses one binary file per emulated disk to save the disk's data blocks. This file is called the "image" of a disk.\
UniBone/QBone uses disk images which are compatible to the SimH emulator.

When running, the emulated DEC computer puts a file system onto this disk image, creating a collection of hierarchically directories and files.\
Mostly we run pre-installed images though.

The vintage file system is only accessible through the emulated machine's user interface.

Exchanging files between PDP's and the modern world is a permanent challenge.\
Current solutions include

- Tools to externally manipulate disk images. There are a lot, well-known projects are DBIT's "PUTR", Don North's "xxdpdir" or Will's Work "DIAGDIR" .\
  Then there's the recent [FSX](https://github.com/kgober/FSX) (2019), quite complete. And RSX11M always had "FLX", the file transfer program.
- Creating and dumping files on the vintage machine, by "catching output" and "typing input" text via a terminal emulator.
- Light weight communication programs via serial line.\
  There are lots of special reader/dumpers, and of course [KERMIT](https://www.kermitproject.org/). Even [PDP11GUI](http://retrocmp.com/tools/pdp11gui) falls into this category.
- Host programs and perhaps special hardware to read vintage floppy disks.
- Setup of Ethernet networking on both vintage machine and modern computer:\
  For instance DECnet is an option (<http://retrocmp.com/decnet>), TCP/IP works with 2.11BSD UNIX or the [Billquist port on RSX-11M](http://mim.update.uu.se/tcpip) .\
  \

"Host File Sharing" now is another approach: QUniBone's emulated disk image is permanentely analyzed and kept in sync with an external "host" directory.\
The host directory is a regular place in the BeagleBone's Linux filesystem, and can be used by any Linux tool ... also remote accessed via sftp.\
\
So the files visible for the DEC operating system can be manipulated under Linux, changes under Linux are written back to the DEC disk.

![sharedfiles rt11 dir](images/sharedfiles-rt11-dir.jpg)

![sharedfiles linux dir](images/sharedfiles-linux-dir.jpg)

From DEC view, the DEC disk content changes "auto-magically" to reflect the current content on the Linux side.\
The DEC machine does not need to know anything about the "host file sharing" mechanism.

This is an attractive approach as:

- there's no need to learn a special workflow for file converting.
- it is very easy to operate: file share just with the "COPY/DIR/DELETE" commands of DEC and Linux operating systems.
- it needs no additional hardware or software resources on the DEC PDP-11 or Linux side.\
  So we can exchange data even with older and very limited PDP-11s.
- the PDP-11 can directly boot from a set of host files.
- implementations need only to deal with the DEC filesystem layout. The type of emulated disk does not matter.

QUniBones "file sharing" is a successor of the [tu58fs](/tools/tu58fs) emulator, with many enhancements.

Currently RT11 and XXDP filesystem can be used, hierarchical filesystems like Unix V6 or Files-11 are also possible.

## How to use

To publish a QUniBone disk as "shared directory", only two additional parameter lines are necessary:

- **shared_filesystem **specifies the type of DEC file system on the disk image.\
  Currently RT11 and XXDP (soon) are implemented.
- **shared_dir** is the location of the synchronized Linux host directory, relative to "demos" work path.

 In an actual "demo" application, these parameter where set for a RL02 drive \#0 like this:

    >>p image              RT11v53.rl02

    >>p shared_filesystem  RT11

    >>p shared_dir         shared_rl1

    >>p

    Parameters of device rl0:
    Name               Short Value        Info                                                                                                             
    ------------------ ----- ------------ -----------------------------------------------------------------------------
    name               name  rl0          Unique identifier of device                                                                                      
    type               type  RL02         Type                                                                                                             
    ...
    image              img   RT11v53.rl02 Path to binary image file. Empty to detach. ".gz" archive also searched.                                         
    shared_dir         shd   shared_rl0   Path to directory with shared files. Created on demand, empty to disable sharing.
    shared_filesystem  shfs  RT11         Encode shared dir in this file system (empty, RT11, XXDP).
    ...

** **

Generally, think of the Linux host directory as "user interface" to the DEC disk image.

This means:

- on startup, the host directory is re-initialized from the DEC side.
- if the DEC image is formatted/initilialized, all files on the host side get lost.
- After RESET of the emulated DEC machine all disk images are reopened and the host directories are initialized again.
- the host directory should be considered "volatile" ... don't keep important files there.

 Multiple drives can be "shared" in parallel. As a shared file system is hold in-memory, the BeagleBones may be a limit later.

### Creating images from host files

Normally the shared host directory is initialized from the DEC image.\
The reverse operation is: Building a new image from downloaded DEC files.\
This is quite straigth forward:

1.  Mount an filled, empty or non-existant image with the "p image \<filename\>"
2.  Clear all files in the host directory - the image gets synced and is now empty.
3.  Copy new files to the shared dir - the image gets synced and contains now a DEC file system with the desired files.

The image can now be used without attaching a Linux host directory.

### Synchronizing

Files changes can occur simultaneously on the DEC disk image and the host directory.\
Also file operations (like writing file content or directory entries) may take some time on the DEC side, as we have a slow CPU here.\
As one consequence, synchronisation takes only place if both sides haven't touched their respective files for a certain period (1 second at the moment).\
After that it's assumed file system operations are completed and data content is stable, so synchronization can begin.\
Bottom line: it make take some seconds until changes appear on the other side.

Keep in mind the synchronisation of host and DEC directory is memory, CPU and SDcard intensive.\
That's because for each change on each side, the complete DEC image is re-analyzed and possibly rewritten.

As the content of the shared DEC disk image changes auto-magically, the DEC OS must not cache the shared disk in PDP memory !\
This is no problem on non-caching operating systems like RT11 or XXDP.\
Maybe this will cause trouble on other filesystem types later, defintive for newer UNIXes.

## About RT-11

The [RT-11](https://en.wikipedia.org/wiki/RT-11) operating system was used over the whole PDP-11's lifespan. It's filesystem is also accessible by RSX-11, RSTS and VAX/VMS with the EXCHANGE utility. This makes it a primary candidate for "file sharing".

However the RT-11 filesystem is quite different from modern Linux standard, *a lot of mapping* is needed.

The defining document is [AA-PD6PA-TC](http://www.bitsavers.org/pdf/dec/pdp11/rt11/v5.6_Aug91/AA-PD6PA-TC_RT-11_Volume_and_File_Formats_Manual_Aug91.pdf) "RT-11 Volume and File Formats Manual" from August 1991.

**Directories:** RT-11 has no subdirectories. Linux subdirectories are ignored and erased.\
The directory can not grow indefinitely, it consists of max. 31 disk blocks of 512 bytes, with each block holding max. 72 file entries of each 7 words in size.\
So the absolute maximum of file count is 2232 files. (For "directory extensions" see below).

**Filenames:** Filenames are saved in "[Radix-50](https://en.wikipedia.org/wiki/DEC_RADIX_50)" compression: 3 letters can be saved in one 16 bit word.\
The naming pattern is "6.3", and possible filename letters are "space", "A"-"Z", "0"-"9", "\$" and "." .\
So a Linux filename is heavily truncated and modified to be RT-11 compatible. For example:\
"aaa_readme.txt.ba~" goes to "AAA RE.BA\$" ...\
The modified RT-11 name will also re-appear on the Linux side, as it is different.\
And there's a special problem: two Linux file may map to the same RT-11 filename, with confusing results.\
Best use only RT-11 compatible names under Linux, staying all UPPERCASE.

**File protection:** RT-11 files have a "protection bit" (E.PROT) for read-only status. Linux mapping is:\
PROT \<-\> chmod 444\
UNPROT \<-\> chmod 644\
There is an other protecion bit E.READ, which just inhibits file write, not file deletion. This is ignored under QUniBone.

**File time stamps:** Under RT-11 only the file creation date is saved, not the time. The first possible date is 1-Jan-72, the latest date is 31-Dec-99.\
if Linux date year is \> 1999, it is truncated to 1999.\
There is an "AGE" bit in the RT-11 directory entry for larger years, but this is ignored by all RT-11 monitors and also by QUniBone.

**File sizes:** RT-11 files have sizes in multiple of 512 byte blocks.\
This is preserved under Linux, files are patched with 00s where necessary.

Host files with a size of 0 are not ignored under RT-11.

**File allocation:** Files under RT-11 are always saved in consecutive disk blocks.\
This creates unused areas on the disk if files are deleted and the empty space can not be reused by smaller files.\
RT-11 compresses the file system manually with the SQUEEZE utility.\
As QUniBone rewrites the RT-11 file system on any Linux host dir change, SQUEEZE is never necessary.

**Special files:** A RT-11 disk has two special areas, which make a disk bootable and which are visible as Linux files:\
**\$BOOT.BLK**: Block 0 is the primary bootloader.\
**\$MONI.TOR:** Blocks 2..5 are a secondary bootloader.\
These entries are generated by RT-11 utility COPY/BOOT.\
You can make a RT-11 disk bootable by copying "good" \$BOOT.BLK and \$MONI.TOR to the image under Linux.\
**\$VOLUM.INF**: this Linux file contains information about the current RT-11 disk layout. It not present on the RT-11 disk, and re-created on any image change.

**User file system modifications:**\
The RT-11 file system can be modified in two ways:\
- You can have bigger directory entries: "extra bytes" in the directory entry. This area is accessible to application programs only, not by the RT-11 monitor itself. As experimental feature, QUniBone generates an extra Linux file for this directory extension, with suffix ".dirext".\
- Files can have "prefix blocks", with additional fixed space for each file. RT-11 itself provides no support for prefix blocks, you need to write special applications to use it. As experimental feature, QUniBone generates an extra Linux file for prefix blocks if present, with suffix ".prefix"

**Interleaving**

On some disks reading and writing disk data requires CPU support opposed to DMA operation. This is the case of RX01 and RX02 floppies. Time to transfer one sector would cause the next disk sector to pass the read/write head, requiring a full disk rotation to appear again under the head. This would be result in very slow overall transfer, so between two sectors with ascending number a gap filled with a sector with other number is placed. This is called "sector interleaving".

Normally the disk is low-level formatted with an interleaved sector pattern, so interleaving is transparent to the file system. In case of RX01 and RX02 however floppy sectors are preformatted in strict ascending order. To its up to the file system to use sectors in an itnerleaved way. Thus the interlaving pattern is visible in the binary disk image, giving a quite confusing data layout in the images hex dump.

Decoding/encoding of the disk image is done by an additional "partition layout module" in the hostfile sharing software.

 

## About XXDP

"XXDP" is the device-independend diagnostic package. It consists of a small operating system monitor and 100s of test programs.\
Classic distribution is on 10MB RL02 platters, which is large enough to hold the full set of programs.

The "XXDP" file system is a single-user version of the early DOS-BATCH-11 operating system.\
Only a single user directory is implemented, so like RT-11 above there are no subdirectories.

From user view its much like RT-11, regarding timestamps, 6.3 file names, file size in whole blocks, boot block and monitor.

 

## Internals

To sync a DEC disc image with a Linuxfile system, several conversion states are necessary.

The drawing show the overall signal and data flow:

   [![host file sharing architecture](images/host_file_sharing_architecture.jpg)](/images/stories/joerg/unibone/host_file_sharing_architecture.pdf)*Click to enlarge*

\
Synchronization between disk image and host file system takes place very often, on any change on any side. So optimization is a must-have.

- No multiple memory copies of the binary image and the cached or filesystem tree.\
  In-memory trees contain only structural information, file data content is fetched from the disk.\
  The decoded DEC filesystem is hold in-memory however.
- No full scan operations to determine file system state. An event based message system greatly reduces CPU load.

While synchronizing, the DEC disk controller may not write or read the image to avoid data inconsistencies. A global lock is used here.

Also synchronization is only performed when both DEC image and Linux host directory were idle for a certain period ... assuming all pending file activies were finished then.

 

### Known bugs:

\
- a file can not be "renamed" via sftp. It can be copied and deleted via sftp, but "renaming" produces wrong "inotify" events.
