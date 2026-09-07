#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <string>

#include "util/BleCatalog.h"

class GfxRenderer;
class MappedInputManager;

// The Store: a live browser over the phone app's Calibre library.
//
// THE PROBLEM. BLE GATT is client-driven. The phone is the central and the
// reader is the peripheral, so the reader cannot call out -- it can only answer
// what the phone asks. But a store is the other way round: the device knows
// which six books are on screen and when the user turned the page, and the phone
// is the only thing that can reach Calibre. Polling from the app would mean
// either a slow store or a battery-eating one.
//
// THE MECHANISM. The `status` characteristic is already notify-capable and the
// app already subscribes to it, so it is turned into a request channel. When the
// device wants something it puts a `pending` block in the status JSON and
// notifies:
//
//   {"state":"connected", ... ,"pending":{"op":"catalog_page","req":7,"offset":12,
//                                          "limit":6,"thumb_w":72,"thumb_h":108}}
//
// The app sees the notification, fetches from Calibre, and answers with an
// ordinary upload -- `start_put` with kind `catalog_page` and the SAME `req`.
// Everything below that is unchanged: the hello/HMAC gate, the framed writes on
// `data-in`, the credit flow control, the SHA-256 over the whole payload, the
// commit. The store adds a question, not a transport.
//
// CORRELATION. `req` is a session-monotonic counter starting at 1. The device
// has at most one question outstanding, and an answer naming any other `req` is
// refused as `stale request` and changes nothing on screen. That is what stops a
// slow reply, arriving after the user has already paged on, from repainting the
// screen with the page they left.
//
// SIZE. The full status document is ~570 bytes and a notification carries at
// most ATT_MTU-3 -- 20 bytes on a peer that never exchanges MTUs. So the
// notified document is built separately and kept under 180 bytes, shedding whole
// fields until it fits rather than ever being truncated; the GATT read returns
// the whole thing. See buildNotifyJson() in BleTransferActivity.cpp. `pending`
// is near the bottom of that shed order, so a request reaches the app intact.
//
// RETRY. A GATT notification is unacknowledged -- there is no ATT-level
// confirmation that the app received it -- so the pending block is re-notified
// every REPUBLISH_MS while it is outstanding, up to MAX_PUBLISHES times. The
// republish carries the same `req`, so an app that saw the first copy simply
// answers once and the later copies are ignored.
//
// TIMEOUT. If nothing has arrived by the deadline the request fails visibly:
// the screen says the phone did not answer and offers Retry. It never hangs,
// and it never silently shows stale content -- which is also why there is no
// cached catalogue at all. Without the app connected the Store has nothing to
// show and says so.
class BleStoreController {
 public:
  // What the activity must do on the controller's behalf. The controller owns
  // no BLE and no activity stack.
  class Host {
   public:
    virtual ~Host() = default;
    // The pending block changed: republish `status` (and repaint).
    virtual void storePublishStatus() = 0;
    // Screen content changed but the protocol state did not.
    virtual void storeRepaint() = 0;
    // Ask the app for this book through the ordinary `book` upload kind. The
    // activity is what knows how to arm the upload gate for it.
    virtual void storeArmBookFetch(const std::string& filename) = 0;
    // Leave the Store.
    virtual void storeFinish() = 0;
    // Open a book that is now on the card.
    virtual void storeOpenBook(const std::string& path) = 0;
  };

  enum class Screen {
    WAITING_APP,  // no app connected, or connected but not through the gate yet
    LOADING,      // a catalog_page / catalog_detail request is outstanding
    LIST,
    DETAIL,
    FETCHING,  // the book itself is coming over the wire
    SAVED,
    FAILED
  };

  enum class PendingOp { NONE, PAGE, DETAIL, FETCH };

  BleStoreController(GfxRenderer& renderer, MappedInputManager& mappedInput, Host& host);

  // --- lifecycle -------------------------------------------------------------
  void begin();
  void end();

  // --- protocol side ---------------------------------------------------------
  // The app is through the hello gate. Asks for the first page.
  void onAppReady();
  // The link dropped. Everything on screen is invalidated: the Store is live or
  // it is nothing.
  void onAppGone();

  bool hasPending() const { return pending_ != PendingOp::NONE; }
  PendingOp pendingOp() const { return pending_; }
  uint32_t pendingReq() const { return pendingReq_; }
  // True when `req` is the answer the device is actually waiting for.
  bool acceptsResponse(uint32_t req, PendingOp op) const { return pending_ == op && req == pendingReq_ && req != 0; }
  // Writes the `pending` object into the status document, when there is one.
  // `terse` keeps only what an answer must quote back -- `req` and `op` -- for a
  // notification too small to carry the geometry and the deadline as well. Those
  // are never lost: a GATT read of `status` always returns the full block.
  void describePending(JsonDocument& doc, bool terse = false) const;

  // A committed `catalog_page` / `catalog_detail` upload is staged at `path`.
  void onCatalogCommitted(const char* path, bool detail);
  // The `book` upload the fetch asked for has started / is progressing / landed.
  void onFetchStarted();
  void onFetchProgress(size_t received, size_t total);
  void onFetchSaved(const std::string& savedPath);
  // The app cannot answer (Calibre unreachable, book gone). `req` must match.
  void onAppError(uint32_t req, const std::string& message);
  // Any other protocol failure while the Store was waiting.
  void onTransferError(const std::string& message);

  // Deadlines and republishes. Called once per activity loop.
  void tick();

  // --- UI --------------------------------------------------------------------
  // Returns true when the frame was consumed.
  bool handleInput();
  void render(const std::string& sessionCode) const;

 private:
  void issue(PendingOp op, uint32_t offset, const std::string& id);
  void cancelPending();
  void fail(const std::string& message);

  int listRowHeight() const;
  int listTop() const;
  void renderWaiting(const std::string& sessionCode) const;
  void renderLoading() const;
  void renderList() const;
  void renderDetail() const;
  void renderFetching() const;
  void renderSaved() const;
  void renderFailed() const;
  void drawCover(const std::string& path, int x, int y, int maxWidth, int maxHeight) const;

  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  Host& host;

  Screen screen_ = Screen::WAITING_APP;
  BleCatalog::Page page_;
  BleCatalog::Page detail_;
  int selected_ = 0;
  uint32_t pageOffset_ = 0;

  PendingOp pending_ = PendingOp::NONE;
  uint32_t pendingReq_ = 0;
  uint32_t nextReq_ = 1;
  uint32_t pendingOffset_ = 0;
  std::string pendingId_;
  unsigned long pendingIssuedAt_ = 0;
  unsigned long pendingPublishedAt_ = 0;
  uint8_t pendingPublishes_ = 0;

  // The book currently being pulled down.
  std::string fetchTitle_;
  std::string fetchFilename_;
  std::string savedPath_;
  size_t fetchReceived_ = 0;
  size_t fetchTotal_ = 0;

  std::string errorMessage_;
};
