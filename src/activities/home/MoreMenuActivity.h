#pragma once
#include <I18n.h>

#include <vector>

#include "activities/UiListActivity.h"

// Where the home screen's old verb list went.
//
// Home is a shelf now, so Browse Files, Recent Books, File Transfer and Settings
// moved one level down rather than disappearing. Nothing here is the only way to
// reach any of them: the control centre (a power tap, from any screen) still has
// Settings and Home tiles, and the reader has its own menu.
class MoreMenuActivity final : public UiListActivity {
 public:
  MoreMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;

 private:
  // Rows are built once in onEnter() because the set only changes with a
  // capability flag, never at runtime. Enumerator names avoid SETTINGS and
  // TRANSFER-adjacent spellings that CrossPointSettings.h and friends define as
  // object-like macros -- an enumerator called SETTINGS would be macro-expanded
  // before the compiler ever saw it.
  enum class MoreRow { FILES, RECENTS, OPDS, TRANSFER, PREFERENCES };

  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_MORE); }
  void onBackButton() override;

  std::vector<MoreRow> rows;
  std::vector<freeink::ui::ListItem> rowItems;
};
