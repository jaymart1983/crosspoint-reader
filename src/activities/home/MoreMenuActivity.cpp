#include "MoreMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#if FREEINK_CAP_NETWORK
#include "OpdsServerStore.h"
#endif
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

MoreMenuActivity::MoreMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("MoreMenu", renderer, mappedInput) {}

void MoreMenuActivity::onEnter() {
  UiListActivity::onEnter();

  rows.clear();
  rows.push_back(MoreRow::FILES);
  rows.push_back(MoreRow::RECENTS);
#if FREEINK_CAP_NETWORK
  // Same rule the home screen used to apply: no configured OPDS server, no row.
  if (OPDS_STORE.hasServers()) rows.push_back(MoreRow::OPDS);
#endif
  rows.push_back(MoreRow::TRANSFER);
  rows.push_back(MoreRow::PREFERENCES);

  rowItems.clear();
  rowItems.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); i++) {
    fui::ListItem item;
    switch (rows[i]) {
      case MoreRow::FILES:
        item.label = tr(STR_BROWSE_FILES);
        item.icon = listIconFor(Folder);
        break;
      case MoreRow::RECENTS:
        item.label = tr(STR_MENU_RECENT_BOOKS);
        item.icon = listIconFor(Recent);
        break;
      case MoreRow::OPDS:
        item.label = tr(STR_OPDS_BROWSER);
        item.icon = listIconFor(Library);
        break;
      case MoreRow::TRANSFER:
        item.label = tr(STR_FILE_TRANSFER);
        item.icon = listIconFor(Transfer);
        break;
      case MoreRow::PREFERENCES:
        item.label = tr(STR_SETTINGS_TITLE);
        item.icon = listIconFor(Settings);
        break;
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

void MoreMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void MoreMenuActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  switch (rows[index]) {
    case MoreRow::FILES:
      activityManager.goToFileBrowser();
      break;
    case MoreRow::RECENTS:
      activityManager.goToRecentBooks();
      break;
    case MoreRow::OPDS:
#if FREEINK_CAP_NETWORK
      activityManager.goToBrowser();
#endif
      break;
    case MoreRow::TRANSFER:
      activityManager.goToFileTransfer();
      break;
    case MoreRow::PREFERENCES:
      activityManager.goToSettings();
      break;
  }
}

// Back goes home with the More row preselected, not out of the activity stack:
// this screen is always entered from home via replaceActivity().
void MoreMenuActivity::onBackButton() { onGoHome(HomeMenuItem::MORE); }
