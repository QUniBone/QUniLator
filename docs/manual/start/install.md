---
title: Installing the software
description: Write the card image, fit the card, power on, and find QUniLator on your network.
sidebar:
  order: 4
---

The supported path is the ready-made card image. It carries Debian 13, the
emulator, the cape overlay and the boot settings, and configures itself on first
boot. What you do is: write the image, fit the card, power on, open the web
interface — in that order.

## Get the image for your bus

These links always give the newest release:

- **[qbone-dist.img.xz](https://github.com/QUniBone/QUniLator/releases/latest/download/qbone-dist.img.xz)** — QBUS
- **[unibone-dist.img.xz](https://github.com/QUniBone/QUniLator/releases/latest/download/unibone-dist.img.xz)** — UNIBUS

Release notes are on the [latest release page](https://github.com/QUniBone/QUniLator/releases/latest).

## Write it to a microSD card

8 GB or larger. The image is shrunk to fit the release and grows to fill the
card on first boot.

```sh
xz -dc qbone-dist.img.xz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
```

Replace `/dev/sdX` with the card — on macOS `/dev/rdiskN`. Check it twice;
`dd` will not ask. Raspberry Pi Imager, balenaEtcher and `bmaptool` all take
the `.xz` directly if you would rather not use `dd`.

## Fit the card and power on

The image runs from the microSD. The cape occupies the eMMC data lines, so the
overlay disables the eMMC and it is unusable while the card is fitted. If the
bone comes up on something else, hold the **S2** boot button while applying power
to force SD boot.

## Wait out the first boot

**It takes 2–3 minutes and includes a reboot of its own.** Setup runs in two
passes: the first applies boot settings and moves `eth0` onto a bridgeable
driver, then reboots; the second finds everything in place and starts the
emulator. Do not pull power in between — watch the LEDs instead.

## Find it on the network

Try these in order:

1. **`http://qbone.local/`** — the image runs an mDNS responder. Your client
   needs to speak mDNS too: macOS does, Linux wants `libnss-mdns`, Windows
   wants Bonjour.
2. **A service browser** — the interface advertises over DNS-SD as
   *\<hostname\> (QBone ddeeff)*, so it shows up in Safari's Bonjour
   bookmarks and in `avahi-browse -rt _http._tcp`.
3. **A USB cable** — the BeagleBone appears as a network interface at a fixed
   **192.168.7.2** and hands your machine an address on the same subnet. No
   LAN needed: `http://192.168.7.2/`.
4. **Your router's lease table** — the uplink MAC is pinned, so the lease is
   stable across reboots.
5. **A 3.3 V USB-serial adapter** on the J1 header, 115200 8N1. The address
   is printed above the login prompt, so you need not log in.

## Open it and create your identity

The web interface binds port 80 and asks you to create the operator identity on
first use — a name and a password, and it will not go further without both. That
one pair serves **both** the web login and the Linux account behind it, so the
same credentials get you into the file shares and `ssh` in later.

A card can be prepared with that identity already in it, in which case no dialog
appears; see `packaging/personalize-image.sh` in the source tree.

## Reading the LEDs

For the first few seconds the LEDs mean nothing in particular — the bootloader
and then the kernel drive them with their own defaults. The indicators below
start once userspace takes them over, and the whole sequence runs a second time
after the first-boot reboot.

| LEDs | Blinking | Meaning |
|---|---|---|
| `X··` | 0.5 s | booting |
| `XX·` | 0.5 s | configuring |
| `XXX` | 0.5 s | starting |
| `X··` `·X·` `··X` `·X·` … | ~150 ms sweep | **ready** |

The bouncing sweep is what you are waiting for: the emulator service is running.
Anything else still blinking after about five minutes means setup did not finish,
and the serial console is how to find out why. `usr3` is SD-card activity.

## The machine comes up dark

A QUniLator that has just started serves a machine that is **switched off**. The
service loads the configuration the DIP switches name, but puts none of it on
the bus: no card is installed, no register window answers, no emulated processor
takes the bus over.

That is deliberate. A QUniLator is fitted to a machine and configured afterwards, and
what it carries may describe a backplane it is no longer in. Switching the
machine on is an explicit act in the web interface. A configuration marked
**autostart** switches itself on at startup instead, and says so afterwards in the
standing notice.

## More than one QUniLator on the network

Addresses never collide: each QUniLator's DHCP lease is keyed to its own uplink MAC,
and each emulated Ethernet controller derives its station address from that MAC
as well.

Names would. Each QUniLator carries an identifier taken from its uplink MAC and
advertises as **`qbone (QBone ddeeff)`**, so two QUniLators are told apart in a
service browser out of the box. The hostname is still shared, so a second QUniLator
finds `qbone` taken and mDNS renames it `qbone-2.local`. Which QUniLator gets which
suffix follows boot order and can change — give each its own name instead:

```sh
sudo qunilator-rename pdp11-front
```

The name then follows everywhere by itself: `pdp11-front.local`, the DNS-SD
advertisement, and the DHCP lease.

## Keeping it current

The image ships with an apt source configured, and the interface can check for
and install updates itself. An update returns QUniLator to its DIP-selected
configuration, so a configuration applied by hand needs re-applying afterwards.

> **UNIBUS · UniBone**
>
> The UNIBUS image comes off the same build script as the QBUS one, but has not
> been exercised on a UniBone in a live backplane. Treat a first installation as
> bring-up, and please report what you find.

## Next

Run the [acceptance test](acceptance-test.md) before trusting the card in a
machine you care about, then pick a machine from the
[configuration catalogue](https://qunilator.com/configurations/).
