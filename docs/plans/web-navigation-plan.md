# Web navigation and URL state — implementation plan

Puts the web UI's navigation state in the URL using History API paths, with a
civetweb SPA-shell fallback so reloads and deep links resolve, and a standalone
console URL. Implements the requirements in
[`web-navigation.md`](../planning/web-navigation.md). Built on the
Vite + Preact + TypeScript frontend (the cross-cutting decision for this round).

## 1. What exists today

The UI is one `index.html` with inline styles and a vendored Preact/xterm bundle.
It switches views in memory without reflecting the view in the URL, so a reload
returns to the default view and there is no deep link. civetweb serves static
files from `document_root` and registers explicit handlers for `/api/*` and the
`/ws/*` WebSockets (`webserver.cpp`); a request to `/dashboard` finds no such file
and 404s. Switching views tears down and rebuilds them, which is how the console
loses its WebSocket on navigation ([console.md](../planning/console.md)).

## 2. Route model

History API **paths**, no hash. Structure splits into two layers:

- **Path segments carry the structural selection** — the view and the one object
  it is focused on:

  ```
  /                       → redirect to /dashboard
  /dashboard
  /config                 configuration-management screen
  /config/<name>          a selected configuration
  /config/<name>/<device> a selected device within it
  /console                standalone console, primary channel
  /console/<channel>      a specific channel (ext, 0, 1, mux<n>/<port>)
  /storage                image library
  /diagnostics
  ```

- **Query parameters carry ephemeral view state** — the active tab, a filter, a
  scroll target — so a screen is fully reproducible from its URL without those
  details cluttering the path:

  ```
  /config/211bsd/uda0?tab=params&filter=enabled
  /console/mux0/2?scroll=bottom
  ```

  The router owns the path; components read and write their own query keys through
  a small `useQueryParam` helper, updating the URL with `replaceState` so ephemeral
  changes do not spam the history stack. Path changes use `pushState`, so
  back/forward step between meaningful screens, not every filter toggle.

## 3. Client router

A small path router in the Preact app (a focused dependency such as
`preact-iso`, or ~100 lines of `pushState`/`popState` handling — decided when the
frontend is scaffolded):

- Parses `location.pathname` into `{view, selection…}` and renders the matching
  top-level view.
- Listens for `popState` (back/forward) and re-renders from the URL.
- Intercepts in-app link clicks and calls `pushState` instead of a full load.
- Exposes `navigate(path)` for programmatic transitions (selecting a config row
  pushes `/config/<name>`).

Views mount and unmount on path change. The console is the one view that must
survive navigation; its keep-alive/reconnect handling is specified in the
[console plan](../planning/console.md) and uses the history replay there — the
router gives it a stable place to live (the standalone `/console` route and the
dashboard-embedded instance) and predictable mount/unmount, but does not itself
carry the console fix.

## 4. civetweb SPA-shell fallback

A deep link or reload of `/config/211bsd` must return the SPA shell so the client
router can resolve it. `begin_request_handler` (`webserver.cpp`) gains a
fallback, scoped so it never shadows the API or WebSockets:

- Requests to paths starting `/api/` or `/ws/` are left entirely to their
  registered handlers.
- A GET whose path resolves to an existing file under `document_root` (the built
  assets: `/assets/*`, favicons, the manifest) is served normally.
- Any other GET returns `index.html` (the SPA shell) with 200, so the client
  router takes over. Non-GET methods on unknown paths still 404.

This is a path-prefix test plus a file-exists check ahead of civetweb's default
static handling; the API and WS routes keep their exact behaviour.

## 5. Standalone console URL

`/console` renders a bare full-screen console page — the same terminal component
the dashboard embeds, without the surrounding dashboard chrome — and
`/console/<channel>` selects the channel. This is a route into the same
component, so history replay and reconnect behave identically whether the console
is viewed standalone or in the dashboard.

## 6. Build and serving

The Vite build emits `index.html` plus hashed `/assets/*`, written to the
`document_root` the package installs. Hashed asset names make them immutable
(long cache), while `index.html` itself must revalidate each deploy — the
existing ETag/`static_file_max_age: 0` handling already covers the shell. No new
board-side process; civetweb serves the built output as today.

## 7. Testability

- **Route parsing** unit-tested in the frontend: each path → the expected
  `{view, selection}`, and round-tripping `navigate()` → URL → parse.
- **Fallback** covered by an integration test against the host web build: `/api/*`
  and `/ws/*` untouched, an existing asset served, an unknown path returns the
  shell, a non-GET unknown path 404s. Rides the host harness the
  configuration-model plan introduces.

## 8. Decisions

Resolved:

- **History API paths** with a civetweb SPA-shell fallback scoped to non-`/api`,
  non-`/ws` GETs that do not match a static asset.
- **Structural selection in path segments** (config, device, console channel);
  **ephemeral view state in query parameters** (active tab, filter, scroll), so a
  screen reproduces fully from its URL.
- Path changes `pushState`; ephemeral query changes `replaceState`, keeping
  back/forward meaningful.
- **Standalone `/console[/<channel>]`** route rendering the same component the
  dashboard embeds.

Open:

- Router dependency vs. hand-rolled — settled when the Vite frontend is
  scaffolded; does not affect the route model above.
- Whether `/config` without a selection shows an empty detail pane or auto-selects
  the current configuration — a UI choice for the
  [config-management plan](../planning/web-config-management.md).
