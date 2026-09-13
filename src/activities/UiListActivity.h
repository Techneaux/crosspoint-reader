#pragma once

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Base for activities hosting a single FreeInkUI list screen. UiAppHost owns
// the app-hosting protocol (render target, FreeInkApp, uiReady handshake);
// this base layers the list protocol on top: the touch-routing / swipe-scroll
// / button-navigation loop (swipes scroll the viewport without moving the
// selection; buttons move the selection and pull the viewport along via
// fui::ListNav), and the render skeleton (chrome, app, footer). Subclasses
// supply the data: item count, screen content, and what activating a row does.
//
// Screens that are not a single list (sliders, tab layouts, state machines)
// should NOT derive from this — they use UiAppHost directly.
class UiListActivity : public Activity, protected UiAppHost {
 public:
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 protected:
  // Base-owned row action; subclass-registered actions start at ACTION_USER.
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_USER = 2;

  UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                 bool wantsTouchLongPress = false);

  // --- subclass contract -----------------------------------------------------
  // Current number of list rows (re-read every loop pass; may change).
  virtual int listCount() const = 0;
  // Build the screen: content margin, items, ListProps (call syncListViewport
  // right before screen.list). Runs on the render task via the base trampoline.
  virtual void buildScreen(UiScreen& screen) = 0;
  // Activate a row (touch tap or Confirm on the selection). Handlers that
  // leave this screen should call app.clearTapFlash() so a lingering flash
  // can't gray an unrelated element on the next render.
  virtual void activateIndex(int index) = 0;
  // Touch long-press on a row; only fires when the subclass opted in via the
  // wantsTouchLongPress constructor flag (rows must also carry InputLongPress).
  virtual void onRowLongPress(int index) {}
  // The selection/viewport state the loop, sync, and row dispatch operate on.
  // Default is the single `nav` member; UiTabListActivity redirects it to the
  // active tab's per-tab state.
  virtual freeink::ui::ListNav& activeNav() { return nav; }
  // Bounds-checked ACTION_ROW dispatch. Default: selection follows the tapped
  // row, then long-press/activate. UiTabListActivity remaps row -> ring.
  virtual void onRowAction(const freeink::ui::ActionEvent& event);
  // The button-navigation tail of loop(): release steps the selection, hold
  // jumps by page. UiTabListActivity replaces it with the ring walk.
  virtual void navigateButtons();
  // First hook in loop(); return true when the pass is consumed (popups, extra
  // buttons, gestures). Runs before the base button handling.
  virtual bool handleCustomInput() { return false; }
  // Back/Confirm handling; override wholesale for press/release or hold
  // variants. Return true when a button consumed the pass.
  virtual bool handleButtons();
  virtual void onBackButton() { finish(); }
  // Header band, drawn before the app renders. Default paints GUI.drawHeader
  // with headerTitle(); override either for custom chrome.
  virtual const char* headerTitle() const { return nullptr; }
  virtual void drawChrome();
  // Button hints, drawn after the app renders. Default: Back/Select/Up/Down.
  virtual void drawFooter();

  // --- helpers ---------------------------------------------------------------
  // Measure visibleRows for the screen band, apply follow-on-build, clamp the
  // viewport, and write selection/viewport into props. Call from buildScreen
  // right before screen.list(props).
  // hasSubtitle: rows carry a second (subtitle) text line, so on non-touch
  // hardware the denser override below uses the theme's *-with-subtitle row
  // height instead of its single-line one (see syncListViewport()).
  void syncListViewport(UiScreen& screen, freeink::ui::ListProps& props, bool hasSubtitle = false);
  // Step the selection by delta rows (a tap), wrapping like
  // ButtonNavigator::nextIndex, and pull the viewport to it. Never blocks: the
  // render task reads nav mid-build, so the step is applied under the render
  // lock only when the lock is free; otherwise it is counted and applied, with
  // any other queued steps and a single repaint, on the first loop pass after
  // the render. The main loop is the only button sampler, and waiting here for
  // an e-ink refresh would drop every tap that starts and ends inside it.
  void stepSelection(int delta);
  // Jump a page (a hold). Holds repeat every 500 ms, so one landing during a
  // render is skipped rather than queued.
  void pageSelection(int direction);

  // --- shared state ----------------------------------------------------------
  // Selection + viewport (selected/top/visibleRows/followOnBuild). Access via
  // activeNav() in shared code; `nav` is the single-list default storage.
  freeink::ui::ListNav nav;
  ButtonNavigator buttonNavigator;

 private:
  static void screenTrampoline(UiScreen& screen, void* user);
  static void rowActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // Apply the queued steps under the render lock and request a repaint.
  // wait=false leaves them queued while a render is in flight; wait=true
  // blocks — used before an action release so handlers (including subclass
  // ones reading nav.selected) act on the row the user reached.
  void flushPendingSelection(bool wait);
  // True when this input frame carries the release that list handlers act on:
  // Confirm, Back, Power, or a screen touch (row taps route on release).
  // Releases only: blocking on a press would stall until the render ends and
  // inflate getHeldTime(), turning a short Confirm into a long press.
  bool hasActionRelease() const;
  // Named apart from UiAppHost::routeTouch so the host overload stays visible
  // (not name-hidden) to subclasses with extra touch surfaces.
  bool routeListTouch();

  const bool wantsTouchLongPress;
  // Row steps not yet applied because the render task held the lock. Bounded:
  // holds are handled by continuous navigation, so a deeper queue only means
  // the user out-tapped a very slow render.
  static constexpr int MAX_PENDING_STEPS = 16;
  int8_t pendingSteps = 0;
};
