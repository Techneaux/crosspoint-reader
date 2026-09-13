#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const bool wantsTouchLongPress)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer), wantsTouchLongPress(wantsTouchLongPress) {}

void UiListActivity::onEnter() {
  Activity::onEnter();
  activeNav().reset();
  pendingSteps = 0;
  resetUi();
  app.on(ACTION_ROW, &UiListActivity::rowActionTrampoline, this);
  app.setScreen(&UiListActivity::screenTrampoline, this);
  requestUpdate();
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value < 0 || event.value >= self->listCount()) return;
  self->onRowAction(event);
}

void UiListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value;
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

bool UiListActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected >= 0 && selected < listCount()) activateIndex(selected);
    return true;
  }
  return false;
}

bool UiListActivity::routeListTouch() {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let the action trampoline dispatch.
  const auto route = UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  // No pressed-state repaint: the render it triggers would drop a slow tap's
  // release inside the uiReady window (tap-to-activate needed two taps), and
  // it costs a second e-ink refresh per tap.
  if (route.routed && app.invalidated()) requestUpdate();
  return static_cast<bool>(route);  // dispatched to the action handler
}

void UiListActivity::stepSelection(const int delta) {
  pendingSteps = static_cast<int8_t>(std::clamp(pendingSteps + delta, -MAX_PENDING_STEPS, MAX_PENDING_STEPS));
  flushPendingSelection(false);
}

void UiListActivity::flushPendingSelection(const bool wait) {
  if (pendingSteps == 0) return;
  // The render task reads nav mid-build (syncToProps, layout feedback); a step
  // landing during a render would tear selection/viewport.
  RenderLock lock(wait ? portMAX_DELAY : 0);
  if (!lock.locked()) return;  // render in flight: keep the steps queued
  auto& n = activeNav();
  const int count = listCount();
  // Replay one row at a time so wrap-around matches the individual taps.
  for (; pendingSteps > 0; --pendingSteps) n.selected = ButtonNavigator::nextIndex(n.selected, count);
  for (; pendingSteps < 0; ++pendingSteps) n.selected = ButtonNavigator::previousIndex(n.selected, count);
  n.follow(count);
  requestUpdate();
}

void UiListActivity::pageSelection(const int direction) {
  // Taps still queued means the render was busy at this pass's flush; skip the
  // hold (it repeats) rather than let it overtake the older taps if the render
  // finished in between.
  if (pendingSteps != 0) return;
  RenderLock lock(0);
  if (!lock.locked()) return;  // render in flight: the hold repeats after it
  auto& n = activeNav();
  const int count = listCount();
  // Page by the rows the last build actually drew (pageRows), not the
  // fixed-height visibleRows estimate: with wrapped labels the estimate
  // overshoots and rows between pages would never be shown.
  n.selected = direction > 0 ? ButtonNavigator::nextPageIndex(n.selected, count, n.pageRows())
                             : ButtonNavigator::previousPageIndex(n.selected, count, n.pageRows());
  n.follow(count);
  requestUpdate();
}

bool UiListActivity::hasActionRelease() const {
  using Button = MappedInputManager::Button;
  return mappedInput.wasReleased(Button::Confirm) || mappedInput.wasReleased(Button::Back) ||
         mappedInput.wasReleased(Button::Power) || mappedInput.wasScreenTouchReleased();
}

void UiListActivity::loop() {
  // Land queued steps before any handler runs. A frame carrying an action
  // release (Confirm, Back, Power, touch) waits for the render lock so the
  // handlers below — including subclass ones that read nav.selected directly —
  // act on the row the user navigated to. Any other frame never waits, so the
  // button poll keeps running through the render.
  flushPendingSelection(hasActionRelease());

  if (handleCustomInput()) return;
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    bool moved = false;
    {
      // Same nav-vs-render race as flushPendingSelection: the render task writes
      // pageRows/top mid-build, so read and mutate under one lock.
      RenderLock lock(*this);
      auto& n = activeNav();
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.pageRows() : -n.pageRows();
      moved = n.scrollBy(delta, listCount());
    }
    if (moved) requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() {
  buttonNavigator.onNextRelease([this] { stepSelection(1); });
  buttonNavigator.onPreviousRelease([this] { stepSelection(-1); });
  buttonNavigator.onNextContinuous([this] { pageSelection(1); });
  buttonNavigator.onPreviousContinuous([this] { pageSelection(-1); });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser per-theme row
    // height instead of FreeInkUI's touch-target-sized default, so lists fit
    // as many rows per screen as they did before the FreeInkUI migration.
    // props.rowHeight must be set explicitly: screen.list() otherwise falls
    // back to the (touch-friendly) theme token, not this local value.
    // A label that must wrap (labelText.maxLines > 1) grows only its own row:
    // list() sizes wrapped items per-row, so the dense height stays.
    const auto& metrics = UITheme::getInstance().getMetrics();
    rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  activeNav().syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, listCount(), props);
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
}

void UiListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UiListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  drawChrome();
  renderUi();
  // Wrapped labels grow rows, so fewer rows can fit than the fixed-height
  // estimate ListNav plans with. list() reports the real layout back
  // (ListNav::onListRendered); when the selection landed past the drawn rows
  // the nav advanced the viewport and asked for another build. Bounded: top
  // strictly advances toward the selection each pass.
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
  }
  drawFooter();
  renderer.displayBuffer();
}
