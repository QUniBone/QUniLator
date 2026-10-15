# Hardware tests

`.github/workflows/build.yml` proves what a host can: the configuration model,
the upload handler, the emulated CPU cores against MAINDEC, the cross-build
and the package. What a board does with a build it cannot say.
`.github/workflows/hil.yml` does, on the UniBone, nightly and before every
release.

## Where it runs

On a self-hosted runner on netmbx, on the same LAN as the UniBone. The runner,
the board's web login, the VMS system disk and the board's deploy account are
set up from the `netzhansa-infra` repository (`docs/qunilator-ci.md` there);
none of them is in this repository or on GitHub.

The workflow triggers nightly, by hand (*Actions → hil → Run workflow*) and
from `release-deb.yml`, never on a pull request: the runner is inside a
private network, and code from a fork does not run there.

## What it does

- **vax-core**: builds the shimmed VAX core (`10.07_vax`) on the runner and
  boots VMS V4.7 on it with simh's own disk inside
  (`10.07_vax/3_tests/shim-boot-vms.sh`). It says whether the processor and
  the shim are sound, apart from the bus.
- **unibone**: `tools/ci/hil-unibone.sh` installs the UNIBUS package on the
  board, then:
  - checks that the board refuses an image that does not exist, and an upload
    cut off halfway;
  - uploads the VMS system disk through `POST /api/images`;
  - imports `tools/ci/vax780-vms47.qcfg.json`, applies it and switches the
    machine on;
  - boots VMS and logs in with `console-harness/scripts/unibone-ci-vms.yaml`;
  - restores the configuration the board had before.

  The run's artifacts hold the console recording (`vms-boot.cast`, readable
  with `qcon render`), the board's log and the deploy output.

A release runs the same test on the package it is about to publish, and
publishes nothing until it passes.

## Reading a failure

The job names what went wrong in its summary and as an annotation:

| Title | What to do |
|---|---|
| unibone unreachable | The board has no DNS name or does not answer on port 80: it is off, has no DHCP lease, or its service is down. |
| unibone login refused | The board's operator or password changed; update `qunilator_ci_board_*` in `netzhansa-infra` and re-run its `github-runner` role. |
| unibone deploy refused | The board no longer takes the runner's key, usually after a reflash: run `playbooks/qunilator-ci.yml` in `netzhansa-infra`. |
| unibone deploy failed | The package did not install; the message quotes the board's output. |
| Missing image accepted, Cut-off upload accepted | The web API has regressed on what it refuses. |
| VMS did not boot | The build boots no VMS on the board; start from `vms-boot.cast`. |

Jobs that stay *Queued* mean the runner is offline:
`systemctl status github-runner` on netmbx.
