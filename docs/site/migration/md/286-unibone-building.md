<!-- migrated from https://retrocmp.com/projects/unibone/286-unibone-building -->

Here are some hints how to build an UniBone and test it.

## Prepare the BeagleBone

1\. Download the SDcard image

2\. Make an SDcard from the image. On Ubuntu:

2.1 Plug a new sdcard of at least 16GB into your card reader. After recognition the desktop file explorer may pop-up some window.

2.2. Determine the Linux SDcard device:

    joerg@vmubuprog:~$ dmesg | tail
    [   45.200662] usbcore: registered new interface driver usb-storage
    [   45.204700] usbcore: registered new interface driver uas
    [   46.199822] scsi 33:0:0:0: Direct-Access     Generic  STORAGE DEVICE   0903 PQ: 0 ANSI: 6
    [   46.201107] sd 33:0:0:0: Attached scsi generic sg2 type 0
    [   46.482076] sd 33:0:0:0: [sdb] 62521344 512-byte logical blocks: (32.0 GB/29.8 GiB)
    [   46.488105] sd 33:0:0:0: [sdb] Write Protect is off
    [   46.488108] sd 33:0:0:0: [sdb] Mode Sense: 21 00 00 00
    [   46.494056] sd 33:0:0:0: [sdb] Write cache: disabled, read cache: enabled, doesn't support DPO or FUA
    [   46.524458]  sdb: sdb1 sdb2 sdb3 sdb4 < sdb5 >
    [   46.544925] sd 33:0:0:0: [sdb] Attached SCSI removable disk
    joerg@vmubuprog:~$

The output above means that the raw SDcard is "/dev/sdb"

2.3. To write the zipped image onto the card type something like:

    joerg@vmubuprog# sudo -s
    [sudo] password for joerg:
    root@vmubuprog# gunzip <unibone_sdcard_2018_11_21.dd.gz | time dd of=/dev/sdb bs=1M
    13+360271 Datensaetze ein
    13+360271 Datensaetze aus
    15931539456 Bytes (16 GB) kopiert, 828,463 s, 19,2 MB/s
    0.00user 9.66system 13:48.46elapsed 1%CPU (0avgtext+0avgdata 3232maxresident)k
    200inputs+31116288outputs (1major+350minor)pagefaults 0swaps

This make take some time.\
\
3. Optionally cut a trace on the BBB to disable the onboard eMMC. This is necessary to enable GPIO pin 13 of PRU1, which is normally not used.\
\
4. Put the SDcard into the BBB. If you have a 5V power supply, you can operate the BBB already independent of UniBone. Network name is "unibone", login with "root/root".

5\. Set the BeagleBones "hostname"

The initial network name of the BBB is "unibone". If you use multiple UniBones, each must get a different name. The hostname is found on the SDcard filesystem at /etc/hostname. You can edit this on the Ubuntu host: re-plugin the SDcard, navigate to /media/something/etc,  do a "sudo nano hostname".

## Populate the board

There's not much to say about it. First cut off the two small adapter boards, see below.

The populated board looks like this (click to enlarge):

[![board populated small](images/board-populated-small.jpg)](/images/stories/joerg/unibone/board-populated-big.jpg)

 Solder best in that order:

1.  all the SMDs. I use a standard iron, SMD flux and thin SMD tin-solder.

2.  flat parts: resistors, LEDs, capacitors.

3.  high parts: transistors, pin headers, IC sockets, switches and the relay.

4.  Plug ICs and resistor packs into sockets.

5.  clean the board.

6.  apply an unique serial number label.

Only a minimal subset of all the pinheaders is necessary for normal operation, the others are debugging.

As I never seen it before, an automatic SMD production run is fascinating to me:

 

 

## Mount BBB onto PCB

Two special parts are used to fit the BeagleBone vertically into a DEC card space:

- little spacer PCBs
- pinheaders in reduced high

 

![bbb mounted top](images/bbb-mounted-top.jpg)

 

![bbb mounted back](images/bbb-mounted-back.jpg)

### Soldering reduced pinheaders onto spacer PCBs

The pin headers have plastic strips, you get them pushed to one end. The trick is to use the BeagleBone itself as soldering template:

|  |  |
|----|----|
| ![bbb mount pinheaders 01](images/bbb-mount-pinheaders-01.jpg) | ![bbb mount pinheaders 02](images/bbb-mount-pinheaders-02.jpg) |
| ![bbb mount pinheaders 03](images/bbb-mount-pinheaders-03.jpg) | ![bbb mount pinheaders 04](images/bbb-mount-pinheaders-04.jpg) |
| ![bbb mount pinheaders 05](images/bbb-mount-pinheaders-05.jpg) | ![bbb mount pinheaders 06](images/bbb-mount-pinheaders-06.jpg) |
| ![bbb mount pinheaders 07](images/bbb-mount-pinheaders-07.jpg) | ![bbb mount pinheaders 08](images/bbb-mount-pinheaders-08.jpg) |

 

### Mounting the spacer PCBs

The little spacer PCBs sit pickyback on the main PCB. The trick here is to align them and solder through both PCBs at once. This is suprisingly easy, at least if you fill the metalized PCB holes with flux. Align the two stacked PCBs with the BBB and temporarily pinheaders.

|  |  |
|----|----|
| ![bbb mount spacer 01](images/bbb-mount-spacer-01.jpg) | ![bbb mount spacer 02](images/bbb-mount-spacer-02.jpg) |
| ![bbb mount spacer 03](images/bbb-mount-spacer-03.jpg) | ![bbb mount spacer 04](images/bbb-mount-spacer-04.jpg) |
| ![bbb mount spacer 05](images/bbb-mount-spacer-05.jpg) | ![bbb mount spacer 06](images/bbb-mount-spacer-06.jpg) |
| ![bbb mount spacer 07](images/bbb-mount-spacer-07.jpg) | ![bbb mount spacer 08](images/bbb-mount-spacer-08.jpg) |

 

Just remember to use double length of solder and heat more than the double time.

Sometimes your solder may not flow through both PCB layers, resulting in an open contact.

![bbb mount spacer failure](images/bbb-mount-spacer-failure.jpg)

\
When that happens: **do not resolder from the clean side .*\*
Always resolder from the side you worked on already**, suck off the old solder if necessary.\
If you try to close the bad junction from both sides

- you loose visible control of the junction
- you may get **two** solder blobs not contacting each other. Or worse: only contacting sometimes !

If you don't feel the need to use a circuit beeper, then you're doing right.

 \*\*\*

 

Now continue with tests ...
