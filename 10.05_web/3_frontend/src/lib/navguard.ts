// A navigation guard for unsaved edits. A screen with pending changes registers
// a confirm callback; the navigation helper consults it before leaving, so the
// "discard changes?" decision lives in one place, independent of which control
// (sidebar, master list, …) triggers the move. Only one screen guards at a time,
// which matches the editor: at most one configuration is being edited.
type Guard = () => Promise<boolean>;

let guard: Guard | null = null;

export function setNavGuard(fn: Guard) {
  guard = fn;
}

// Clear the guard. Passing the same function that was set clears only if it is
// still the active one, so a later screen's guard is not dropped by an earlier
// screen's cleanup racing behind it.
export function clearNavGuard(fn?: Guard) {
  if (!fn || guard === fn) guard = null;
}

// True when navigation may proceed: no guard, or the guard's confirm resolves
// true. Callers await this before routing.
export async function mayLeave(): Promise<boolean> {
  return guard ? guard() : true;
}

// Route only if the guard allows it. `loc` is a preact-iso location.
export async function guardedRoute(
  loc: { route: (path: string, replace?: boolean) => void },
  path: string,
  replace?: boolean
) {
  if (await mayLeave()) loc.route(path, replace);
}
