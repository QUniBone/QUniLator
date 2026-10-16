---
title: KA11 and KD11-EA — PDP-11/20 and PDP-11/34 processors
description: The emulated PDP-11/20 and PDP-11/34 processors, what they bring to a machine, and every parameter they take.
bus: [unibus]
---

QUniLator carries two PDP-11 processors of its own. The **KA11** is the
processor of a PDP-11/20, listed as `CPU20`, of type `PDP-11/20`. The **KD11-EA**
is the processor of a PDP-11/34 with its **KT11-D** memory management, listed as
`CPU34`, of type `PDP-11/34`. Either one makes the UniBone the machine's
processor: it fetches and executes the program, and every bus cycle it makes
goes out on the UNIBUS, where the peripherals answer it — real cards in the
backplane and the board's own emulated ones alike.

> **UNIBUS · UniBone**
>
> An emulated processor is UNIBUS-only. A QBone drives a real CPU board and ships
> no processor emulation.

## What the machine sees

| | |
|---|---|
| **Processor** | `CPU20`: the PDP-11/20 instruction set, with no memory management and no processor modes. `CPU34`: the PDP-11/34 instruction set, EIS and MFPS/MTPS included, with kernel and user mode |
| **Memory management** | `CPU34` only: the KT11-D, relocating to 18-bit addresses. Its registers sit inside the processor, as on a real 11/34 |
| **Memory** | Enabling the processor gives it everything below the I/O page that no card in the backplane answers, out of the board's own memory. A [memory card](memory.md) placed beforehand keeps its range. Disabling the processor gives that memory back |
| **Switch register** | `switch_reg`, which the program reads at 777570 |
| **Console** | None inside the processor. A DL11 on the bus is the console line, as on a real machine |
| **Boot** | An [M9312](m9312.md) on the bus starts the processor at a ROM entry at power-up. Without one it starts from the address in `PC` |

One processor can be enabled at a time; enabling a second one is refused while
the first is in.

An enabled processor makes the board the machine: it arbitrates the bus while it
runs. Enable one only in a backplane that has no processor of its own. Applying
a configuration that enables one says so in a warning.

## Using it

1. On the **Configurations** screen, enable `CPU20` or `CPU34` with the
   peripherals the machine needs: a DL11 as its console, a disk controller and
   drive with a system on it, and an `M9312` with the boot ROM for that
   controller.
2. Enabling the processor leaves it halted. A power cycle runs the power-up
   sequence: the M9312 hands the processor its boot address, and the machine
   boots with no console input.
3. Without an M9312, set `PC` to a start address while the processor is halted
   and press **START**.

Set `pmi` for a machine whose memory the board supplies: the processor then
reaches its memory without bus cycles and runs about five times as fast.

## What has been verified

- DEC's MAINDEC diagnostics run against both cores on every UNIBUS build, on the
  build machine. The thirteen PDP-11/20 instruction tests MAINDEC-11-DZKAA to
  DZKAM pass on both. On the KD11-EA, DFKAA (basic instructions), DFKAB (traps),
  DFKAC (EIS and MFPS/MTPS) and the KT11-D memory management tests DFKTA,
  DFKTB, DFKTC, DFKTD, DFKTF and DFKTH pass. DFKTG is skipped: it drives the
  console line as a device, which the test harness does not have. These
  validate the instructions and the memory management, not the bus.
- On a card running its [internal bus](../operating/machine.md#bus), with no backplane, the PDP-11/20 boots
  XXDP from an emulated RL02 through an emulated RL11, with the M9312's DL boot
  ROM and a DL11 as its console.
- The PDP-11/34 boots the RSX-11M V4.2 distribution kit from an emulated TS11,
  through the M9312's `MS` boot ROM 23-764A9.

## Parameters both processors have

<!-- device-parameters: cpu_base_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`pmi`](#pmi) |  | 0 or 1 | 0 | Private Memory Interconnect: CPU accesses memory internally, not over UNIBUS. |
| [`switch_reg`](#switch_reg) | `swr` | octal | 0 | Console switch register. |
| [`breakpoint`](#breakpoint) | `bp` | octal | 0 | Stop when CPU fetches opcode from octal address. 0 = disable |
| [`cycle_tracefilepath`](#cycle_tracefilepath) | `ctf` | text | *empty* | If set, CPU cycle trace is active and dumped to file on HALT. |

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`run_led`](#run_led-halt_switch-continue_switch-start_switch) | `r` | 0 or 1 | RUN LED: 1 = CPU running, 0 = halted. |
| [`halt_switch`](#run_led-halt_switch-continue_switch-start_switch) | `h` | 0 or 1 | HALT switch: 1 = CPU stopped, 0 = CPU may run. |
| [`continue_switch`](#run_led-halt_switch-continue_switch-start_switch) | `c` | 0 or 1 | CONT action switch: 1 = CPU restart after HALT. CONT+HALT = single step. |
| [`start_switch`](#run_led-halt_switch-continue_switch-start_switch) | `s` | 0 or 1 | START action switch: 1 = reset & start CPU from PC. START+HALT: reset. |
| [`PC`](#pc) | `pc` | octal | program counter helper register. |
| [`PSW`](#psw-bus_addr-bus_data) | `psw` | octal | Processor status word. |
| [`bus_addr`](#psw-bus_addr-bus_data) | `ba` | octal | Bus address register of the current data transfer. |
| [`bus_data`](#psw-bus_addr-bus_data) | `bd` | octal | Bus data register of the current data transfer. |
| [`cycle_count`](#cycle_count) | `cc` | number | CPU opcodes executed since last HALT |

It also carries the parameters under [parameters every card on the bus has](index.md#parameters-every-card-on-the-bus-has).

On this device, `slot` starts at 0; `emulation_speed` is read-only and starts at 0.1.

### In detail

#### `run_led`, `halt_switch`, `continue_switch`, `start_switch`

The processor's front panel, which the dashboard's RUN, HALT, START and CONT controls drive. `start_switch` and `continue_switch` act once and fall back to 0.

START resets the machine — INIT on the bus, the processor cleared — and runs from the address in `PC`; with HALT set it only resets. CONT resumes a halted processor where it stopped; with HALT set it executes one instruction and stops again. Setting HALT stops a running processor after the instruction it is executing. Enabling the processor leaves it halted.

#### `pmi`

The processor reads and writes everything below the I/O page straight out of the board's memory, without a bus cycle; only the I/O page goes out on the bus. The machine runs about five times as fast: `emulation_speed` reads 0.5 with it set and 0.1 without, against the original's 1. Memory on a real card in the backplane is out of the processor's reach while this is set, so it is for a machine whose memory the board supplies.

#### `PC`

Where the processor is. It follows the program while the processor runs; while it is halted it can be set, and START runs from there.

#### `switch_reg`

The console switch register, which a program reads at 777570. Diagnostics take their options from it. It is read on every instruction, so a change reaches a running program at once.

#### `PSW`, `bus_addr`, `bus_data`

What the processor is doing, as it runs: the processor status word, and the address and data of the bus transfer in progress. The address is the virtual one the program issued, before memory management relocates it. The status word is the model's own width: the KA11 keeps priority, T and the condition codes in the low byte, the KD11-EA adds the current and previous mode in the high byte.

#### `cycle_count`

Instructions executed since the processor was last started. The dashboard's instruction rate is taken from it.

#### `breakpoint`

Halts the processor when it is about to execute the instruction at this address, which is octal and virtual. 0 turns the breakpoint off.

#### `cycle_tracefilepath`

A file on the board. While a path is set the processor keeps its most recent bus transfers, up to 16384 — address, cycle, data and whether the bus timed out — and writes them to that file, as comma-separated lines, when it halts on a HALT instruction, the HALT switch or the breakpoint. Empty: no trace is kept.

<!-- /device-parameters -->

## PDP-11/20 parameters

<!-- device-parameters: cpu20_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Settings

What a configuration stores and an operator sets.

| Parameter | Short | Value | Default | What it is |
|---|---|---|---|---|
| [`swab_vbit`](#swab_vbit) | `swab` | 0 or 1 | 0 | SWAB instruction does not(=0) or does(=1) modify psw v-bit (=0 is standard 11/20 behavior) |

It also carries the parameters under [parameters both processors have](#parameters-both-processors-have).

### In detail

#### `swab_vbit`

What SWAB does to the V bit. A real 11/20 leaves it as it was, which is 0. Set to 1, SWAB clears it as every later PDP-11 does, which is what a program or diagnostic written for those models expects, among them ZQKC.

<!-- /device-parameters -->

## PDP-11/34 parameters

<!-- device-parameters: cpu34_c -->
<!-- Written by docs/site/tools/device-params.mjs from the C++ source. Edit the source, then run it. -->

### Status

What the device reports as it runs. A configuration does not store these.

| Parameter | Short | Value | What it is |
|---|---|---|---|
| [`MMR0`](#mmr0-mmr1-mmr2-mmu_enabled) | `mmr0` | octal | KT11-D status register 0. |
| [`MMR1`](#mmr0-mmr1-mmr2-mmu_enabled) | `mmr1` | octal | KT11-D status register 1: register changes of the aborted instruction. |
| [`MMR2`](#mmr0-mmr1-mmr2-mmu_enabled) | `mmr2` | octal | KT11-D status register 2: virtual address of the instruction. |
| [`mmu_enabled`](#mmr0-mmr1-mmr2-mmu_enabled) | `mmu` | 0 or 1 | Memory management: 1 = relocation on, addresses are 18 bit. |

It also carries the parameters under [parameters both processors have](#parameters-both-processors-have).

### In detail

#### `MMR0`, `MMR1`, `MMR2`, `mmu_enabled`

The KT11-D memory management registers, as the running program leaves them. MMR0 holds the enable bit and, once an abort has frozen it, what caused the abort; MMR1 the register changes of the aborted instruction; MMR2 the virtual address that instruction was fetched from. `mmu_enabled` is 1 while relocation is on and addresses are 18 bits wide. The registers sit inside the processor, as on a real 11/34, so another card on the bus cannot read them.

<!-- /device-parameters -->
