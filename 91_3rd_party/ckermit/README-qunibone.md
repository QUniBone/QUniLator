# C-Kermit (build tooling)

Serial/network communications program used on the QBone to talk to the
emulated (and real) PDP-11 over a serial line and to move files.

- Source: https://kermitproject.org/ — C-Kermit **9.0.302** (20 Aug 2011, Open
  Source, BSD-style licence). The tarball (`cku302.tar.gz`) is fetched by
  `build.sh`; it is not vendored here.
- Runs on the QBone as `~/bin/kermit`.

## Building

```
./build.sh        # build the armhf binary into ./build/src/kermit
./build.sh -d     # build and deploy to $QUNILATOR_HOST:~/bin/kermit
```

`QUNILATOR_HOST` defaults to `hans@192.168.2.223`.

The build runs in Docker and produces a **dynamically linked** binary against
the device's glibc, so hostname resolution works. The QBone runs Debian 8
"jessie" (glibc 2.19), so `build.sh` compiles inside a qemu-emulated armhf
jessie container (`qunibone-ckermit-jessie`) whose gcc 4.9 / glibc 2.19 match
the device exactly. jessie is archived, so the image's apt sources point at
`archive.debian.org`.

The build uses the makefile's `linuxa` target with `-lcrypt -lresolv -lutil`
named explicitly, because the auto-detecting `linux` target probes only
`/usr/lib` and `/usr/lib64`, not the armhf multiarch path.

## Static alternative

A statically linked binary can instead be built with the bookworm cross
toolchain from `crossbuild.sh` (`gcc-arm-linux-gnueabihf`), which is how the
`demo` binary runs on the old glibc. Static linking is required there because
that toolchain targets a much newer glibc than the device has. It works but
disables NSS, so hostname/DNS resolution fails — the dynamic jessie build above
is preferred. The static recipe needs three source tweaks the jessie build does
not: neutralize the `ckcpro.c`/`wart` rule (wart cannot run when built for the
target), `-D_IO_file_flags` (the console-input path falls back to the removed
`_cnt` stdio field on modern glibc), and `-DNOLOGIN` (drops the IKSD login path,
whose `crypt` is absent from the cross sysroot).
