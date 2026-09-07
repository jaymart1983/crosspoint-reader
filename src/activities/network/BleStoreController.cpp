#include "BleStoreController.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {

constexpr const char* TAG = "STORE";
constexpr const char* BOOKS_ROOT = "/Books";
constexpr const char* STORE_ROOT = "/.crosspoint/store";
// Page covers and detail covers land in separate directories so that stepping
// into a book and back out again does not have to re-fetch the page: the list's
// six thumbnails are still on the card exactly where the list left them.
constexpr const char* PAGE_THUMB_DIR = "/.crosspoint/store";
constexpr const char* DETAIL_THUMB_DIR = "/.crosspoint/store/detail";

// --- Deadlines ---------------------------------------------------------------
// A page or a detail is a Calibre query plus a cover render on the phone, then
// the upload. 20 s is generous for that and still short enough that a user
// staring at "Asking your phone" knows something is wrong before they start
// pressing buttons.
constexpr unsigned long REQUEST_TIMEOUT_MS = 20000;
// A book can need a format conversion on the Calibre side before the app can
// even begin sending, which is the one step that is genuinely slow. The clock
// stops the moment the upload starts -- from there the ordinary transfer
// machinery reports progress and owns the failure.
constexpr unsigned long FETCH_TIMEOUT_MS = 45000;
// GATT notifications are unacknowledged, so a request that goes unanswered is
// re-notified rather than assumed lost. Same `req` every time: an app that got
// the first copy answers once and ignores the rest.
constexpr unsigned long REPUBLISH_MS = 4000;
constexpr uint8_t MAX_PUBLISHES = 4;

constexpr int LIST_ROW_PADDING = 4;
constexpr int LIST_SIDE_PADDING = 12;

}  // namespace

BleStoreController::BleStoreController(GfxRenderer& renderer, MappedInputManager& mappedInput, Host& host)
    : renderer(renderer), mappedInput(mappedInput), host(host) {}

void BleStoreController::begin() {
  Storage.ensureDirectoryExists(STORE_ROOT);
  BleCatalog::clearThumbnails(PAGE_THUMB_DIR);
  BleCatalog::clearThumbnails(DETAIL_THUMB_DIR);
  screen_ = Screen::WAITING_APP;
}

void BleStoreController::end() {
  // Everything the Store held was scratch for one session. Nothing is cached
  // across a close, by design: the catalogue is the app's, not the device's.
  BleCatalog::clearThumbnails(PAGE_THUMB_DIR);
  BleCatalog::clearThumbnails(DETAIL_THUMB_DIR);
}

void BleStoreController::onAppReady() {
  page_ = BleCatalog::Page{};
  detail_ = BleCatalog::Page{};
  selected_ = 0;
  pageOffset_ = 0;
  issue(PendingOp::PAGE, 0, {});
}

void BleStoreController::onAppGone() {
  cancelPending();
  page_ = BleCatalog::Page{};
  detail_ = BleCatalog::Page{};
  selected_ = 0;
  pageOffset_ = 0;
  // No offline browsing: what was on screen described a library this device
  // cannot reach any more, so it goes rather than sitting there looking live.
  BleCatalog::clearThumbnails(PAGE_THUMB_DIR);
  BleCatalog::clearThumbnails(DETAIL_THUMB_DIR);
  screen_ = Screen::WAITING_APP;
  host.storePublishStatus();
}

void BleStoreController::issue(const PendingOp op, const uint32_t offset, const std::string& id) {
  pending_ = op;
  pendingReq_ = nextReq_++;
  pendingOffset_ = offset;
  pendingId_ = id;
  pendingIssuedAt_ = millis();
  pendingPublishedAt_ = pendingIssuedAt_;
  pendingPublishes_ = 1;
  if (op != PendingOp::FETCH) screen_ = Screen::LOADING;
  LOG_DBG(TAG, "Request req=%u op=%d offset=%u id=%s", static_cast<unsigned>(pendingReq_), static_cast<int>(op),
          static_cast<unsigned>(offset), id.c_str());
  host.storePublishStatus();
}

void BleStoreController::cancelPending() {
  pending_ = PendingOp::NONE;
  pendingReq_ = 0;
  pendingId_.clear();
  pendingPublishes_ = 0;
}

void BleStoreController::fail(const std::string& message) {
  cancelPending();
  errorMessage_ = message;
  screen_ = Screen::FAILED;
  host.storePublishStatus();
}

void BleStoreController::describePending(JsonDocument& doc, const bool terse) const {
  if (pending_ == PendingOp::NONE) return;
  JsonObject out = doc["pending"].to<JsonObject>();
  out["req"] = pendingReq_;
  switch (pending_) {
    case PendingOp::PAGE:
      out["op"] = "catalog_page";
      out["offset"] = pendingOffset_;
      if (terse) break;
      out["limit"] = static_cast<uint32_t>(BleCatalog::PAGE_LIMIT);
      out["thumb_w"] = BleCatalog::THUMB_WIDTH;
      out["thumb_h"] = BleCatalog::THUMB_HEIGHT;
      out["desc_max"] = static_cast<uint32_t>(BleCatalog::MAX_LIST_DESCRIPTION_BYTES);
      break;
    case PendingOp::DETAIL:
      out["op"] = "catalog_detail";
      out["id"] = pendingId_.c_str();
      if (terse) break;
      out["thumb_w"] = BleCatalog::COVER_WIDTH;
      out["thumb_h"] = BleCatalog::COVER_HEIGHT;
      out["desc_max"] = static_cast<uint32_t>(BleCatalog::MAX_DETAIL_DESCRIPTION_BYTES);
      break;
    case PendingOp::FETCH:
      out["op"] = "catalog_fetch";
      out["id"] = pendingId_.c_str();
      if (terse) break;
      if (!fetchFilename_.empty()) out["name"] = fetchFilename_.c_str();
      break;
    case PendingOp::NONE:
      break;
  }
  if (terse) return;
  // Deliberately advertised: an app that knows the deadline can give up and send
  // catalog_error rather than let the device sit out the full window.
  out["timeout_ms"] = static_cast<uint32_t>(pending_ == PendingOp::FETCH ? FETCH_TIMEOUT_MS : REQUEST_TIMEOUT_MS);
}

void BleStoreController::onCatalogCommitted(const char* path, const bool detail) {
  const PendingOp expected = detail ? PendingOp::DETAIL : PendingOp::PAGE;
  if (pending_ != expected) {
    LOG_DBG(TAG, "Catalog arrived with nothing pending");
    return;
  }

  BleCatalog::Page parsed;
  std::string error;
  const char* thumbDir = detail ? DETAIL_THUMB_DIR : PAGE_THUMB_DIR;
  const bool ok = BleCatalog::parseContainer(path, thumbDir, pendingReq_, detail, BOOKS_ROOT, parsed, error);
  Storage.remove(path);
  if (!ok) {
    LOG_ERR(TAG, "Catalog rejected: %s", error.c_str());
    fail(error);
    return;
  }

  cancelPending();
  if (detail) {
    detail_ = std::move(parsed);
    screen_ = Screen::DETAIL;
  } else {
    page_ = std::move(parsed);
    pageOffset_ = page_.offset;
    selected_ = 0;
    screen_ = Screen::LIST;
  }
  host.storePublishStatus();
}

void BleStoreController::onFetchStarted() {
  // The app answered the fetch request; from here the ordinary upload path owns
  // the timeout, so the store's own deadline is done.
  cancelPending();
  fetchReceived_ = 0;
  fetchTotal_ = 0;
  screen_ = Screen::FETCHING;
  host.storePublishStatus();
}

void BleStoreController::onFetchProgress(const size_t received, const size_t total) {
  fetchReceived_ = received;
  fetchTotal_ = total;
  if (screen_ == Screen::FETCHING) host.storeRepaint();
}

void BleStoreController::onFetchSaved(const std::string& savedPath) {
  savedPath_ = savedPath;
  screen_ = Screen::SAVED;
  // The book is on the card now, so the detail view must stop offering to get it.
  if (!detail_.entries.empty()) detail_.entries[0].onDevice = true;
  for (auto& entry : page_.entries) {
    if (!entry.filename.empty() && entry.filename == fetchFilename_) entry.onDevice = true;
  }
  host.storePublishStatus();
}

void BleStoreController::onAppError(const uint32_t req, const std::string& message) {
  if (pending_ == PendingOp::NONE || req != pendingReq_) {
    LOG_DBG(TAG, "catalog_error for req %u, not the pending one", static_cast<unsigned>(req));
    return;
  }
  fail(message.empty() ? std::string("the app could not answer") : message);
}

void BleStoreController::onTransferError(const std::string& message) {
  if (screen_ == Screen::WAITING_APP) return;
  fail(message);
}

void BleStoreController::tick() {
  if (pending_ == PendingOp::NONE) return;

  const unsigned long now = millis();
  const unsigned long budget = pending_ == PendingOp::FETCH ? FETCH_TIMEOUT_MS : REQUEST_TIMEOUT_MS;
  if (now - pendingIssuedAt_ >= budget) {
    LOG_ERR(TAG, "Request %u timed out", static_cast<unsigned>(pendingReq_));
    // A question that is never answered has to surface as an error, not as a
    // spinner: the user gets Retry, and the stale `req` guarantees a late reply
    // cannot repaint the screen afterwards.
    fail("no answer from the app");
    return;
  }
  if (pendingPublishes_ < MAX_PUBLISHES && now - pendingPublishedAt_ >= REPUBLISH_MS) {
    pendingPublishedAt_ = now;
    pendingPublishes_++;
    LOG_DBG(TAG, "Re-notifying request %u (%u)", static_cast<unsigned>(pendingReq_),
            static_cast<unsigned>(pendingPublishes_));
    host.storePublishStatus();
  }
}

// --- input -------------------------------------------------------------------

bool BleStoreController::handleInput() {
  const bool back = mappedInput.wasPressed(MappedInputManager::Button::Back);
  const bool confirm = mappedInput.wasPressed(MappedInputManager::Button::Confirm);
  const auto swipe = mappedInput.wasSwipe();

  switch (screen_) {
    case Screen::WAITING_APP:
      if (back) {
        host.storeFinish();
        return true;
      }
      return false;

    case Screen::LOADING:
      // Back abandons the question. The request id still moves on, so the answer
      // that eventually arrives is refused rather than painted over whatever the
      // user did next.
      if (back) {
        cancelPending();
        if (page_.entries.empty()) {
          host.storeFinish();
        } else {
          screen_ = Screen::LIST;
          host.storePublishStatus();
        }
        return true;
      }
      return false;

    case Screen::LIST: {
      if (back) {
        host.storeFinish();
        return true;
      }
      const int count = static_cast<int>(page_.entries.size());
      const bool hasNext = pageOffset_ + static_cast<uint32_t>(count) < page_.total;
      const bool hasPrev = pageOffset_ > 0;

      const bool stepNext = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);
      const bool stepPrev = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
      if (stepNext || swipe == MappedInputManager::SwipeDir::Up) {
        // Falling off the bottom of the last row is the page turn: there is no
        // separate Next control to hunt for with the side keys.
        if (selected_ + 1 < count) {
          selected_++;
          host.storeRepaint();
        } else if (hasNext) {
          issue(PendingOp::PAGE, pageOffset_ + static_cast<uint32_t>(BleCatalog::PAGE_LIMIT), {});
        }
        return true;
      }
      if (stepPrev || swipe == MappedInputManager::SwipeDir::Down) {
        if (selected_ > 0) {
          selected_--;
          host.storeRepaint();
        } else if (hasPrev) {
          const uint32_t step = static_cast<uint32_t>(BleCatalog::PAGE_LIMIT);
          issue(PendingOp::PAGE, pageOffset_ >= step ? pageOffset_ - step : 0, {});
        }
        return true;
      }
      if (confirm && selected_ < count) {
        issue(PendingOp::DETAIL, 0, page_.entries[selected_].id);
        return true;
      }

      int row = -1;
      const auto touch = mappedInput.rowTouch(row, listTop(), listRowHeight(), count, 0, INT32_MAX, listRowHeight());
      if (touch == MappedInputManager::RowTouch::Down) {
        if (selected_ != row) {
          selected_ = row;
          host.storeRepaint();
        }
        return true;
      }
      if (touch == MappedInputManager::RowTouch::Tap && row < count) {
        selected_ = row;
        issue(PendingOp::DETAIL, 0, page_.entries[row].id);
        return true;
      }
      return false;
    }

    case Screen::DETAIL: {
      if (back) {
        screen_ = Screen::LIST;
        host.storeRepaint();
        return true;
      }
      if (confirm && !detail_.entries.empty()) {
        const auto& entry = detail_.entries[0];
        if (entry.onDevice) {
          // Already here: the button reads Read, and it opens the book rather
          // than asking the app for a copy the card already has.
          host.storeOpenBook(std::string(BOOKS_ROOT) + "/" + entry.filename);
          return true;
        }
        if (entry.filename.empty()) {
          fail("the app offered no file for this book");
          return true;
        }
        fetchTitle_ = entry.title;
        fetchFilename_ = entry.filename;
        host.storeArmBookFetch(entry.filename);
        issue(PendingOp::FETCH, 0, entry.id);
        return true;
      }
      return false;
    }

    case Screen::FETCHING:
      // Deliberately not cancellable from here: the upload is mid-flight and the
      // ordinary transfer path owns it. Back after it finishes.
      return false;

    case Screen::SAVED:
      if (confirm && !savedPath_.empty()) {
        host.storeOpenBook(savedPath_);
        return true;
      }
      if (back) {
        screen_ = Screen::LIST;
        host.storeRepaint();
        return true;
      }
      return false;

    case Screen::FAILED:
      if (confirm) {
        if (page_.entries.empty()) {
          issue(PendingOp::PAGE, pageOffset_, {});
        } else {
          screen_ = Screen::LIST;
          host.storeRepaint();
        }
        return true;
      }
      if (back) {
        host.storeFinish();
        return true;
      }
      return false;
  }
  return false;
}

// --- rendering ---------------------------------------------------------------

int BleStoreController::listTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + 2;
}

int BleStoreController::listRowHeight() const {
  // Six rows, the page footer and the button hints all have to fit whatever the
  // theme's header costs, so the row height is derived rather than fixed. A row
  // narrower than the nominal thumbnail simply scales the cover down --
  // drawBitmap1Bit fits it to the box -- which is better than six rows where the
  // last one runs off the panel.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int footer = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  const int available = renderer.getScreenHeight() - metrics.buttonHintsHeight - footer - listTop();
  const int nominal = BleCatalog::THUMB_HEIGHT + LIST_ROW_PADDING * 2;
  if (available <= 0) return nominal;
  return std::min(nominal, available / static_cast<int>(BleCatalog::PAGE_LIMIT));
}

void BleStoreController::drawCover(const std::string& path, const int x, const int y, const int maxWidth,
                                   const int maxHeight) const {
  // A missing or unreadable cover draws as an empty frame. It is one row of the
  // page, not the page.
  renderer.drawRect(x, y, maxWidth, maxHeight);
  if (path.empty()) return;
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) return;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    renderer.drawBitmap(bitmap, x, y, maxWidth, maxHeight);
  }
  file.close();
}

void BleStoreController::render(const std::string& sessionCode) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_STORE));

  switch (screen_) {
    case Screen::WAITING_APP:
      renderWaiting(sessionCode);
      break;
    case Screen::LOADING:
      renderLoading();
      break;
    case Screen::LIST:
      renderList();
      break;
    case Screen::DETAIL:
      renderDetail();
      break;
    case Screen::FETCHING:
      renderFetching();
      break;
    case Screen::SAVED:
      renderSaved();
      break;
    case Screen::FAILED:
      renderFailed();
      break;
  }
  renderer.displayBuffer();
}

void BleStoreController::renderWaiting(const std::string& sessionCode) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = listTop();

  // The Store has nothing of its own to show. Say that plainly rather than
  // implying a catalogue exists somewhere on the device.
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_STORE_NEEDS_APP), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_STORE_NEEDS_APP_HINT));
  y += lineHeight + metrics.verticalSpacing;

  const int qrSize = std::min({172, pageWidth - metrics.contentSidePadding * 2,
                               renderer.getScreenHeight() - y - lineHeight * 3 - metrics.buttonHintsHeight});
  if (qrSize > 0) {
    QrUtils::drawQrCode(renderer, Rect{(pageWidth - qrSize) / 2, y, qrSize, qrSize}, "https://ble.xteink.lol/");
    y += qrSize + metrics.verticalSpacing;
  }
  if (!sessionCode.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, (std::string(tr(STR_BLE_TRANSFER_CODE)) + sessionCode).c_str(), true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleStoreController::renderLoading() const {
  const int centerY = renderer.getScreenHeight() / 2 - 30;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_STORE_ASKING_APP), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_12_FONT_ID) + 8,
                            tr(STR_STORE_ASKING_APP_HINT));
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleStoreController::renderList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int rowHeight = listRowHeight();
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int textX = LIST_SIDE_PADDING * 2 + BleCatalog::THUMB_WIDTH;
  const int textWidth = pageWidth - textX - LIST_SIDE_PADDING;

  int y = listTop();
  for (size_t i = 0; i < page_.entries.size(); i++) {
    const auto& entry = page_.entries[i];
    if (static_cast<int>(i) == selected_) {
      renderer.fillRoundedRect(LIST_SIDE_PADDING / 2, y, pageWidth - LIST_SIDE_PADDING, rowHeight, 6, Color::LightGray);
    }
    drawCover(entry.thumbPath, LIST_SIDE_PADDING, y + LIST_ROW_PADDING, BleCatalog::THUMB_WIDTH,
              rowHeight - LIST_ROW_PADDING * 2);

    int textY = y + LIST_ROW_PADDING + 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY,
                      renderer.truncatedText(UI_10_FONT_ID, entry.title.c_str(), textWidth).c_str(), true,
                      EpdFontFamily::BOLD);
    textY += renderer.getLineHeight(UI_10_FONT_ID);
    if (!entry.author.empty()) {
      renderer.drawText(SMALL_FONT_ID, textX, textY,
                        renderer.truncatedText(SMALL_FONT_ID, entry.author.c_str(), textWidth).c_str());
    }
    textY += smallLine + 2;
    if (entry.onDevice) {
      renderer.drawText(SMALL_FONT_ID, textX, textY, tr(STR_STORE_ON_DEVICE));
      textY += smallLine;
    }
    // The blurb gets whatever rows are left. Two lines at this size is the
    // useful part of a 160-byte snippet; the rest is a tap away.
    const int blurbLines = std::max(0, (y + rowHeight - textY) / smallLine);
    if (blurbLines > 0 && !entry.description.empty()) {
      UITheme::drawCenteredWrappedText(renderer, Rect{textX, textY, textWidth, blurbLines * smallLine}, SMALL_FONT_ID,
                                       entry.description.c_str(), blurbLines, true, EpdFontFamily::REGULAR,
                                       UITheme::TextVerticalAlignment::TOP);
    }
    y += rowHeight;
  }

  // Where in the library this page sits. Without it, paging through a few
  // thousand books six at a time is navigation without a map.
  char footer[64];
  const unsigned first = page_.entries.empty() ? 0 : static_cast<unsigned>(pageOffset_) + 1;
  snprintf(footer, sizeof(footer), "%u-%u / %u", first,
           static_cast<unsigned>(pageOffset_ + page_.entries.size()), static_cast<unsigned>(page_.total));
  renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() - metrics.buttonHintsHeight - smallLine - 2,
                            footer);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleStoreController::renderDetail() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  if (detail_.entries.empty()) {
    renderLoading();
    return;
  }
  const auto& entry = detail_.entries[0];

  int y = listTop();
  drawCover(entry.thumbPath, (pageWidth - BleCatalog::COVER_WIDTH) / 2, y, BleCatalog::COVER_WIDTH,
            BleCatalog::COVER_HEIGHT);
  y += BleCatalog::COVER_HEIGHT + metrics.verticalSpacing;

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const Rect titleBounds{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, lineHeight * 2};
  UITheme::drawCenteredWrappedText(renderer, titleBounds, UI_10_FONT_ID, entry.title.c_str(), 2, true,
                                   EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::TOP);
  y += lineHeight * 2 + 4;
  if (!entry.author.empty()) {
    renderer.drawCenteredText(SMALL_FONT_ID, y,
                              renderer.truncatedText(SMALL_FONT_ID, entry.author.c_str(),
                                                     pageWidth - metrics.contentSidePadding * 2)
                                  .c_str());
    y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  }

  // The action line, then whatever height is left goes to the blurb.
  const int actionY = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight - 8;
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int blurbLines = std::max(0, (actionY - y - 8) / smallLine);
  if (blurbLines > 0 && !entry.description.empty()) {
    UITheme::drawCenteredWrappedText(
        renderer, Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, blurbLines * smallLine},
        SMALL_FONT_ID, entry.description.c_str(), blurbLines, true, EpdFontFamily::REGULAR,
        UITheme::TextVerticalAlignment::TOP);
  }

  const char* action = entry.onDevice ? tr(STR_STORE_ON_DEVICE) : tr(STR_STORE_GET_BOOK);
  renderer.drawCenteredText(UI_10_FONT_ID, actionY, action, true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), entry.onDevice ? tr(STR_OPEN) : tr(STR_STORE_GET_BOOK), "",
                                            "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleStoreController::renderFetching() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int centerY = renderer.getScreenHeight() / 2 - 60;

  renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_STORE_GETTING), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_12_FONT_ID) + 6,
                            renderer.truncatedText(UI_10_FONT_ID, fetchTitle_.c_str(),
                                                   pageWidth - metrics.contentSidePadding * 2)
                                .c_str());
  GUI.drawProgressBar(renderer,
                      Rect{metrics.contentSidePadding, centerY + 80, pageWidth - metrics.contentSidePadding * 2,
                           metrics.progressBarHeight},
                      fetchReceived_, fetchTotal_ > 0 ? fetchTotal_ : 1);

  char progress[48];
  snprintf(progress, sizeof(progress), "%u / %u KB", static_cast<unsigned>(fetchReceived_ / 1024),
           static_cast<unsigned>(fetchTotal_ / 1024));
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 80 + metrics.progressBarHeight + 10, progress);
  GUI.drawButtonHints(renderer, "", "", "", "");
}

void BleStoreController::renderSaved() const {
  const int centerY = renderer.getScreenHeight() / 2 - 30;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_STORE_ADDED), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_12_FONT_ID) + 8, fetchTitle_.c_str());
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleStoreController::renderFailed() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int centerY = renderer.getScreenHeight() / 2 - 40;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_STORE_NO_ANSWER), true, EpdFontFamily::BOLD);
  UITheme::drawCenteredWrappedText(
      renderer,
      Rect{metrics.contentSidePadding, centerY + renderer.getLineHeight(UI_12_FONT_ID) + 8,
           renderer.getScreenWidth() - metrics.contentSidePadding * 2, renderer.getLineHeight(SMALL_FONT_ID) * 3},
      SMALL_FONT_ID, errorMessage_.c_str(), 3, true, EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_STORE_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
