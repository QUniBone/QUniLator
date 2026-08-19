# A console that is a reproduction, and a console that is a readout — implementation plan

The dashboard's control panel is a reproduction of a real DEC front panel — a
PDP-11/23 bezel, layout and legends, drawn in CSS. It is faithful, and it is
liked, and it is wrong about what it drives: two of its five legends name things
the board does not have.

That was tolerable while nothing contradicted it. It stopped being tolerable
when `DCOK` and `POK` in the title bar began reporting the backplane's actual
power lines, because the two claims now sit on one screen and disagree — the bus
carries good DC power while the bezel's `PWR OK` lamp is dark, and both are
correct, because they are not about the same thing.

The correction shipped so far is hover text: every control and lamp says what it
really does, and the artwork is untouched. That is the right default and should
stay the default. This plan adds the other option for the operator who wants the
panel to read as an instrument rather than as an artefact — a setting that
swaps the legends for what they drive, keeping the bezel's shape, colours and
layout exactly.

## 1. What exists today

**The control panel** (`10.05_web/3_frontend/src/components/Dashboard.ts:114`)
draws two lamps and three switches on the bezel. Each is a `PanelSwitch` (`:41`)
or a shared `Led`, and each now carries an explanatory `title` from the `CP_INFO`
table (`:90`). The legends are literal: `PWR OK`, `RUN`, `RESTART`, `HALT`,
`AUX ON/OFF`.

**The front panel** (`:185`) draws the cape's own four activity LEDs and four DIP
switches, with no legends at all beyond the index under each cell; the hover text
comes from `fpLedInfo` (`:176`) and `fpDipInfo` (`:180`).

**What each element actually drives**, which is the whole reason this plan
exists:

| Element | Legend claims | Actually driven by | Honest? |
|---|---|---|---|
| Lamp 1 | DC supply is good | `hw.powered` — the emulation is installed on the bus | **no** |
| Lamp 2 | processor is running | the processor's run state | yes |
| Switch 1 | RESTART | power-cycle sequence + resume from the power-up vector | yes |
| Switch 2 | HALT | the processor's run state | yes |
| Switch 3 | auxiliary DC power | `dc_on`/`dc_off` — installs/removes the emulated cards | **no** |
| Front lamps | (unlabelled) | drive activity, `storagedrive.cpp:276-288` | n/a |
| Front switches | (unlabelled) | the board's DIP block, read once at backend start (`webconfigs.cpp:25`) | n/a |

The two dishonest rows are one axis: whether QUniLator's emulation is on the bus.
It now has an indicator of its own in the title bar — `bus active` / `bus dark`,
`Shell.ts:111` — which is where an operator who does not care about the bezel
should be reading it.

**The settings surface** is `GET`/`PUT /api/settings`, backed by
`10.05_web/2_src/websettings.cpp`: a small struct per group, serialised into
`settings.json` under the state directory (`:321`) and written by `save_settings()`
(`:134`) on every accepted change. The frontend page is
`components/Machine.ts`, one card per group, each row a name/control/description
triple in a `.set-grid`. A `{"t":"settings"}` event with no payload tells every
other page to re-read (`docs/api.md`, the events table), which is how a second tab
follows a change it did not make.

**The dashboard already has a per-card option mechanism**, but it does not reach
these two cards: `optionToggles()` (`Dashboard.ts:596`) returns `null` for any card
that is not a device (`:598`), and the panels are fixed cards (`:290`). Options
that do exist are stored per configuration in the dashboard layout, not in
`settings.json`.

## 2. What is being chosen

Not a different panel. The same bezel, the same geometry, the same lamps and
paddles and the same `digital` wordmark — only the silkscreen changes.

- **`faithful`** (default) — the panel as DEC printed it. `PWR OK`, `RUN`,
  `RESTART`, `HALT`, `AUX` `ON`/`OFF`. What each really drives is in the hover
  text, as now.
- **`functional`** — the same panel with legends naming what the board does with
  them. The two honest legends do not move; only the two that lie are replaced.

The front panel gains legends in `functional` that it does not have at all today,
since its rows are unlabelled either way and the hover text is the only thing
naming them.

Candidate legends for `functional`, to settle when it is built (§6):

| Element | `faithful` | `functional` |
|---|---|---|
| Lamp 1 | `PWR OK` | `ACTIVE` |
| Lamp 2 | `RUN` | `RUN` |
| Switch 3 | `AUX` + `ON`/`OFF` | `EMULATION` + `ON`/`OFF` |
| Front lamps | — | `DRIVE ACTIVITY` |
| Front switches | — | `CONFIG SELECT` |

`ACTIVE` is chosen over `EMULATION` for the lamp because the lamp cell is
56 px wide (`styles.css`, `.cp-lampcell`) and the legend must not widen the card;
the switch legend has the two-line `ON`/`OFF` marker beside it and more room.
Whether the bezel can carry `EMULATION` without reflowing is a measurement to
make, not a guess — the card is sized from what it draws
(`measureCards()`, `Dashboard.ts`), so a wider legend silently grows the card and
pushes the grid around.

**Out of scope.** This changes what the panel *says*, not what it *does*. The
switches keep their actions, the lamps keep their sources, and `dc_on`/`dc_off`
keep their names in the API. A mode that replaces the bezel with a plain control
strip, or hides it entirely, is a different plan.

## 3. The design

A machine setting, `panel_style`, one of `faithful` (default) or `functional`.

**Backend** — a string beside `address_width` in `websettings.cpp`: a static with
its default, into `settings_json()` and out of the `PUT` validator with a 422 for
anything else, `save_settings()` on accept, and the existing `{"t":"settings"}`
broadcast. It is a presentation setting and reaches no device, so unlike
`address_width` it has no bus precondition and applies at any time — worth saying
in its description, since every other row on that page carries one.

**Frontend** — the legends come out of `Dashboard.ts` into a small table keyed by
style, beside `CP_INFO`, and the two panels read `s.settings.panel_style`.
`CP_INFO` stays exactly as it is and is shown in both styles: `functional` makes
the legend honest, it does not make the explanation redundant — `ACTIVE` still
does not tell you that the cards leave the bus and the drives give up their media.

**Machine page** — a third row in the existing Bus card, or a card of its own if
the panel gains further settings later. Two radios, in the shape the external
console's `source` already uses (`Machine.ts:36-48`).

## 4. Where the setting belongs

`settings.json` rather than the dashboard layout, on the reasoning the user gave:
this is a property of the operator reading the board, not of the machine being
emulated. A configuration is a machine — cards, addresses, media — and carrying
a legend preference inside one would mean the panel changed wording when the DIP
switches selected a different configuration, which is nonsense.

The counter-argument is that the dashboard's other presentation choices — which
cards are shown, where they sit, which widget options are on — *are* per
configuration, so this one lands in a different place from its neighbours. That
is accepted: those choices are about a particular machine's cards, and this one is
not.

## 5. Order of work

1. `panel_style` in `websettings.cpp` — default, serialise, validate, save.
   `api.md` gains it in the settings section.
2. Legend table in `Dashboard.ts`, both panels reading the setting. No behaviour
   moves; the hover text is untouched.
3. The Machine-page row.
4. Measure the bezel in `functional` at the longest legend and confirm the card
   does not grow. If it does, the legend is wrong, not the layout.

## 6. What to check while building it

- Both styles in a browser, on the board — the bezel's geometry identical between
  them, the card the same number of grid cells wide in each.
- The hover text present and unchanged in both.
- A second tab following the change through `{"t":"settings"}` without a reload.
- The setting surviving a service restart, and an unknown value in a
  hand-edited `settings.json` falling back to `faithful` rather than to a blank
  panel.

## 7. Testability

`websettings.cpp` has no host test today — `run_config_test.sh` links
`webconfigs`, `webpower`, `webauth`, `webcontrol` and the rest, but not it — so
there is nothing to ride, and one setting does not justify standing a new test
binary up.

What is worth doing instead is what `webcontrol.cpp` already does: the accept /
reject decision is a pure function of the request body, so it can be one, next to
`control_decide` in shape if not in file, and tested from `status_power_test`
without linking the settings module or touching the filesystem. That leaves the
load/save round trip untested, which is the part every other setting is untested
for as well — a gap to close deliberately for all of them, not incidentally for
this one.

The legend table is data, so the frontend check is a type-level one: every
element named in one style is named in the other.

## 8. Open decisions

- The `functional` legends themselves, above — particularly whether the lamp reads
  `ACTIVE` or something that says *what* is active.
- Whether `functional` should also relabel `RESTART`, which is honest but
  understates itself: it rebuilds every card, which the legend does not suggest.
- Whether the front panel's `functional` legends are worth the vertical space they
  cost, given the same words are already one hover away.
