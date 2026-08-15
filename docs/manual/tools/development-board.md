---
title: Building on the card
description: Turn a flashed card into a development board with one command — the source tree in /root, buildable there, and every example machine a shortcut away.
---

A card flashed from the release image is a finished appliance: two programs, a
web root, a state directory. It carries no sources, and nothing on it needs any.

**`qunilator-devkit` puts them back.** It is the card's development kit in one
command, and what it leaves behind is what a QUniBone Classic card always looked
like — a checkout in `/root`, `./compile.sh` building it there, and an example
machine one `sudo ./rt11v5.5_dl_shared.sh` away.

```sh
sudo qunilator-devkit
```

You need this only to change the software or to run the example machines from
the command line. Operating a machine, swapping packs and saving configurations
is all the [web interface](../operating/web-interface.md), and none of it wants
a source tree.

## What the command does

1. **Installs what a build needs** — `git`, the C++ compiler and `make`,
   `libtirpc` (Sun RPC, which the blinkenlight panel client speaks and glibc no
   longer carries), the X11 headers for the VCB01 window, and the TI PRU code
   generation tools. The image usually already carries those last ones as
   `ti-pru-cgt-v2.3`; where a card's repositories have none, the pinned 2.3.1
   installer is fetched from TI. `--no-apt` skips the whole step.
2. **Fetches the repository into `/root`**, at the tag matching the version of
   the package installed — so the tree is the software the card is running, not
   whatever is newest. It fetches into a repository created in place rather than
   cloning, because `/root` already holds the shell's dotfiles, and it leaves
   everything it did not put there alone. `--branch` and `--url` name something
   else; `--dir` puts the tree elsewhere.
3. **Writes `qunibone-platform.env`** for the bus this card bridges. That file
   is the one thing bound to hardware rather than to software, which is why it
   is not in the repository.
4. **Personalizes the tree** by running `qunibone-platform.sh`: the examples of
   this card's bus merged into `10.03_app_demo/5_applications`, `4_deploy`
   pointed at this card's build directory, every `*.sh` made executable, and a
   shortcut in `/root` to each example.

Running it again fetches and personalizes again, which is how you move the tree
to another version.

> [!NOTE]
> **`git status` in that tree is not clean, and should not be**
>
> The merge deletes `5_applications_u` and `5_applications_q` and the `chmod +x`
> shows up as mode changes. That is what an installed card looks like; the
> `.git` is there to make the software's origin traceable and updatable, not to
> be a pristine worktree.

## Building

```sh
cd /root
./compile.sh              # ./compile.sh -a starts from clean
```

Expect the better part of an hour on a BeagleBone — `crossbuild.sh` in the same
tree does it in a minute on a desktop with Docker, and copies the result over.

The build writes two programs into `10.03_app_demo/4_deploy_q` (`4_deploy_u` on
a UniBone): `qbone-web`, the service, and `demo`, the same emulation driven from
a terminal menu. `compile.sh` then **installs both where the card runs them** —
`/usr/bin/qbone` and `/usr/bin/qbone-cli`, the latter set-user-id root and
executable by `qunilator-admin`, exactly as the package installs them — and
restarts the service.

> [!WARNING]
> **The restart takes the machine down**
>
> Restarting the service switches the emulated machine off and rebuilds it from
> the configuration. `./compile.sh -n` installs without restarting, so the
> restart happens when you say so; `-N` builds without installing at all.

## The example machines

`10.03_app_demo/5_applications` holds one command file per machine: a serial
line, memory, a boot loader, a drive with an image in it, and the message saying
what to type at the console. They are the Classic machines, and there is a
shortcut to every one of them in `/root`:

```sh
sudo /root/xxdp22-25.dlx.sh
```

Each file starts with `#!/root/10.03_app_demo/4_deploy/demo --verbose`, so it
runs as a program rather than as an argument to one. It runs as root because it
drives the PRUs, and the program in the tree is not the set-user-id one the
package installs.

Nothing has to be stopped first: the program asks the service for the board and
the service hands it over for the length of the session — see [Coming from
QUniBone Classic](../start/from-qunibone.md) for what that session is and what
it leaves behind.

### Where an example looks for its files

An example names its boot loaders **relative to itself** (`../bootloaders/dl.lst`),
and they are found there however the example was started — through the shortcut
in `/root`, or from another directory entirely. What a run *creates* lands in the
directory you started it from instead, so a memory dump or a new image never goes
back into the tree by accident.

Images are the exception: they are named in full, because they are not in the
tree at all. They live in the board's media tree, the same one the web interface
serves, sorted into a folder per medium:

```
p image /var/lib/qunilator/images/dl/xxdp25.rl02.dsk
```

### Getting the disk images

They are in neither the repository nor the package — large, and not all of them
ours to distribute — so the package ships the tool that fetches them instead:

```
sudo qunilator-fetch-images
```

That brings down the set for this board's bus from `files.retrocmp.com`, named
and filed the way the examples mount them. A repeat run fetches only what is
missing. The emulator expands an `<image>.gz` the first time something mounts
it, beside its own `.gz`, so it is found directly on the next run.

An example whose image is missing gets as far as mounting it and stops there.
That is also the quickest check that the interpreter, the options and the
relative lookup all work.
