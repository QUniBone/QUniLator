# Console scripts

Step files for `qcon run` (see the package README for the schema). The ones
here drive the boards; a script is written against a **recording** of the
dialog, not against guesses:

1. `qcon record out.cast --host qbone --console ext`, drive the dialog once
   (by hand, or restart the machine and let it boot), stop with `^C`.
2. Read the cast: the exact prompt texts, their case, and their terminators
   are all there.
3. Write the step file from what the guest actually prints.

## What the DRS supervisors actually print (XXDP)

Learned from recorded runs on qbone; every from-scratch script used to
rediscover these one failure at a time:

- **Prompt case varies by supervisor.** DRSXM (`DRSXM-C0`, .BIN
  diagnostics) prints `Change HW (L)  ?`; DRSSM (`DRSSM-G2`, .BIC) prints
  `CHANGE HW (L)  ?`. Match prompts with `/…/i`, never with a fixed-case
  literal.
- **The prompt terminator differs.** DRSXM ends a prompt `? ^D` (EOT, 0x04);
  DRSSM ends it `? ` with no terminator byte. A catch-all "answer the
  default" case that works for both is a trailing question mark at the end
  of the window: `/\?[\x00-\x20]*$/`. Named cases start earlier in the line
  and therefore always win over the catch-all where both apply.
- **`EOP n` is not a pass.** The end-of-pass line prints on failing runs
  too, followed by the error count. A pass is `0 CUMULATIVE ERRORS` (or the
  older `END PASS`); a failure is `DVC FTL` / `FTL ERR` / a nonzero
  cumulative count — and the fatal error prints before EOP, so listing both
  as cases fails fast on the error.
- **The XXDP monitor banner** ends with `RESTART ADDRESS: …` and a `.`
  prompt; XXDP V2.5 on this pack asks no date. Give the monitor a moment
  (`wait: 2s`) after the banner before typing — the echo-paced sender
  recovers a dropped first character, but not from a monitor that is still
  seconds away from reading.
- **A logical prompt has no default.** `CHANGE SW (L)  ?` and friends answer
  a bare CR with `NO DEFAULT` and ask again — forever. Answer every `(L)`
  prompt explicitly (`Y`/`N`); only the octal and decimal prompts (`(O)`,
  `(D)`) take a bare CR for their shown default.
- **`DR>` inside a run means the diagnostic gave up** and returned to the
  supervisor. It is not a prompt to answer; it is the run ending early, and
  a script should carry it as a deviation.
- **Destructive tests ask first.** `NXT TST MAY ZERO LD UNIT. DOIT ANYWAY?`
  would zero the loaded pack — on drive 0 that is the XXDP system pack
  itself. Answer `N`, and run the drive tests against drive 1 with a scratch
  pack when they are the point of the run.
- The 2.11BSD boot block's `: ` prompt takes `ra(0,0,0)unix` — the canonical
  burst-corruption victim; echo pacing types it reliably.

## When a script hangs

It should not any more: a console that goes quiet at a prompt no pattern
matches fails the step within the stall window (15 s by default) and prints
the prompt — that is the line to add a case for. A step that legitimately
waits at a prompt sets `stall: 0`; a step whose guest is *working* silently
is unaffected, because a working guest leaves no prompt on the screen.

These patterns belong to the scripts for now; when the MCP server's
`run_xxdp_diagnostic` is rebuilt on this package, the DRS dialog handling
moves into that one place.

## Scripts

Each carries a `machine:` block, so one command applies the configuration,
starts the machine and drives the dialog:

- `qbone-211bsd-singleuser.yaml` — boot 2.11BSD to the single-user shell and
  prove it answers.
- `qbone-xxdp-zrlge0.yaml` — XXDP ZRLGE0 (RLV12 controller test) through the
  DRS dialog.
- `qbone-xxdp-ztkae0.yaml` — XXDP ZTKAE0 (TK50 functional). On this board the
  diagnostic reports a real device fault at init step 1.

The MCP server's `run_xxdp_diagnostic` drives the same dialog from the same
harness, so the DRS knowledge above lives in one place rather than in each
script — see `mcp-server/src/xxdp.ts`.

## What these replaced

`tools/console_send.py`, `tools/vax-console.mjs` and `tools/odt.py` each solved
part of this and were removed when the harness took it over: echo-driven send
and expect/send step files (console_send.py), a password mode for a prompt that
does not echo (vax-console.mjs), and paced ODT command lines (odt.py). The
equivalents are `qcon run` with a step file, `mode: no-echo`, and a step file
against the `ext` console.
