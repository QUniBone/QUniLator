# QBone web-UI punch-list — disposition

Verified against the LIVE board, which serves exactly the current git HEAD
(deployed asset hash matched a fresh `npm run build`). Prior commits 4f2e6e6
("PDP-11/03 dashboard redesign") and 717b337 ("restore disk panels, fix control
panel, add serial port field") already implemented the bulk of this list; each
item below was re-verified on the running board. One genuine latent bug remained
and was fixed (commit 5c845fd). Frontend redeployed to the board.

# dashboard
- restart does not actually init the system
  → RESOLVED (verified live). RESTART POSTs `/api/control {action:"restart"}`
    (200 ok) and the machine reboots — the console re-runs the XXDP memory test
    and re-enters Dialog mode.
- leds
-- have three different sizes
   → RESOLVED. Every `.led` renders 13×13px (measured: topbar ×3, control lamps
     ×2, front-panel activity ×4 — all 13px, one `--led-size`).
-- color not matching real led red
   → RESOLVED. `--led-red` is #E11B14 (a true red LED, no longer amber).
-- top right leds should be green
   → RESOLVED. DCOK / POK / connected render with the green lens.
-- the pseudo 3d look should go away
   → RESOLVED. Flat disc with a hairline seat and a soft lit glow — no dome,
     bevel or grommet.
- console panel
-- should not have a "pdp-11/03" title
   → RESOLVED. Card head reads "Control panel".
-- should be arranged the same way as the original 11/03 panel
   → RESOLVED. Two indicator lamps grouped left, three bat switches grouped right.
-- switches should look more like they do in the original, but 2d
   → RESOLVED. Flat dark-slot switches with pale bat handles, drawn 2D.
-- labels should be like the original
   → RESOLVED. PWR OK, RUN, RESTART, ENABLE/HALT, AUX ON/OFF.
-- same background as the other widget
   → RESOLVED. Same card surface as the Front panel widget.
-- switching off and on does not reliably restart boot loader (halt engaged?)
   → RESOLVED (verified live). dc_off → {powered:false, halt:true};
     dc_on → {powered:true, halt:false} — comes up running, HALT cleared.
- rl02/ra81 widget
-- should retain the design from before the refactor (square buttons, labels)
   → RESOLVED. Coloured switch caps in a black bezel, restored.
-- device title should be on top and replace both "RA81" and "uda0" etc.
   → RESOLVED. One `.disk-title` (the device label) on top; the type/handle line
     is gone.
- console
-- still has controls above terminal area
   → RESOLVED for the dashboard console widget: the terminal renders alone, no
     tab bar or controls above it. (The standalone /console page keeps a console
     source/baud selector, which is functional there and left in place.)
- front panel
-- leds and switches should be centered
   → RESOLVED. Activity and DIP blocks are centred.
-- leds and switches should be visually grouped
   → RESOLVED. Activity LED cluster and DIP-switch cluster, each in its own inset.

# devices
this page is no longer needed
  → RESOLVED. The route, the sidebar entry and Devices.ts are all gone.

# configurations
- enabled/disabled switch on devices (except subunits) should go
  → RESOLVED. Top-level devices carry a "Remove" action; only a controller's
    drives keep an enable toggle.
- only enabled devices in list
  → RESOLVED. Only enabled top-level devices are listed; disabled ones are offered
    through Add.
- explicit "add" cta to add a device from list
  → RESOLVED. "+ Add device" opens a picker of the available devices.
- when adding a controller, disks can be enabled
  → RESOLVED. A controller's drive rows carry their own enable toggles.
- "enabled only" can go
  → RESOLVED. No such filter exists.
- current configuration should be selected when going to "configuration"
  → RESOLVED. /config redirects to /config/<current>.
- did not make modifications, but configuration still shows "modified"
  → NOT REPRODUCED. `modified` is computed by the backend (live device set vs the
    saved current config) and was correct in every state tested — it cleared the
    moment the live set matched the config. If it recurs it is the backend's
    live-vs-saved comparison (service-cpp), not the frontend, which only displays
    the flag from /api/configs and the /ws/events "config" event.
- used "revert", still shows as "modified"
  → NOT REPRODUCED. Reverting (apply the saved config) refetches devices and
    reloads the flag; in testing, restoring the saved device set cleared it.
- cannot enable dhv11 device (flips back to disabled after a moment)
  → RESOLVED (verified live: remove + re-add dhv11 holds enabled, no flip).
    Hardened the underlying race: after a device refresh the model was rebuilt
    and ALL cached /ws/events values were replayed onto it, including config
    values like `enabled`. A stale cached `enabled=false` could clobber the fresh
    authoritative GET on the refresh that follows a toggle. Replay is now limited
    to machine-driven status values (lamps/state/counters); config values come
    only from the GET. (commit 5c845fd)

## Deferred / not changed
- The "modified"/"revert" items are backend-owned (service-cpp config-modified
  comparison). No frontend defect found; flagged for that agent if it recurs.
- The standalone /console page keeps its source/baud selector (functional); the
  dashboard console widget — the one the punch-list names — is control-free.
