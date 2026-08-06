# Web navigation and URL state

**Status:** Ready — implementation plan drafted in
[`web-navigation-plan.md`](../plans/web-navigation-plan.md).

The web UI's navigation state lives in the URL, so a page can be linked,
reloaded, and reached with the browser's back/forward buttons.

## Current state

The UI is a single-page app that switches between views (dashboard, devices,
storage, configurations, diagnostics) without reflecting the current view in the
URL. Reloading returns to the default view rather than the one being looked at,
and there is no deep link to a specific page. Switching pages tears down and
rebuilds views, which is how the console loses its connection on navigation
([console.md](console.md)).

## Requirements

- The current view is **tracked in the URL**, so a reload or a shared link lands
  on the same page, and browser back/forward navigate between views.
- URLs use **history API paths** (`/dashboard`, not `/#/dashboard`). civetweb
  serves the SPA shell for any unknown path so reloads and deep links resolve.
- The console is reachable at its **own standalone URL** — a bare full-screen
  console page, in addition to the console embedded in the dashboard
  ([console.md](console.md)).

## Decisions

- **History API paths**, with a civetweb SPA-shell fallback for unknown paths.
- **Standalone console URL** alongside the dashboard-embedded console.

## Open questions

- Which state belongs in the URL — just the top-level page, or also selections
  within it (selected configuration, selected device, active serial-mux tab)?
- Does putting the view in the URL change how views mount/unmount — enough to fix
  the console's navigation teardown, or is that a separate lifecycle fix?
- The SPA-shell fallback must not shadow the REST API and WebSocket routes — how
  is the fallback scoped (only non-`/api`, non-`/ws` paths)?
