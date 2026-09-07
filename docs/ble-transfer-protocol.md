# BLE Transfer Protocol

CrossPoint Reader exposes a small BLE transfer service while **File Transfer > Bluetooth Transfer** is open.

The browser companion is available at <https://ble.xteink.lol/>. Source and compatibility notes live at
<https://github.com/marginalia-os/ble-xteink>.

## Compatibility

- Protocol version: `1`
- Device name: `CrossPoint Transfer`
- Service UUID: `6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10`

Clients should discover the service by UUID. The user-visible name is not part of the compatibility contract.

## Characteristics

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `control` | `6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10` | client to reader | write with response |
| `data-in` | `6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10` | client to reader | write, write without response |
| `status` | `6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10` | reader to client | read, notify |
| `data-out` | `6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10` | reader to client | notify |

## Authentication

First use requires the six-digit code shown on the reader:

```json
{"op":"hello","version":1,"code":"123456"}
```

After code authentication, a client can include `pair_host_id`, `pair_host_name`, and `pair_secret` in the `hello`
command. The reader prompts to save the trusted host after a successful authenticated upload. Trusted auth signs this
message:

```text
{device_nonce}|{host_id}|1
```

using HMAC-SHA256 with the saved secret.

## Supported Operations

Uploads use `start_put`, binary frames on `data-in`, then `commit`.

Supported upload kinds:

- `book`: `.epub` saved under `/Books`
- `bmp`: `.bmp` saved under `/Pictures`
- `firmware`: `.bin` staged on SD card, validated, confirmed on device, then flashed through the OTA path
- `progress`: a batch of reading positions to apply to books already on the card (see below)

Downloads use `start_get`, notifications on `data-out`, and one `get_ack` per frame.

Supported download kinds:

- `crash_report`: reads `/crash_report.txt`
- `library`: the on-device book list with reading progress (see below)
- `progress_result`: the per-entry outcome of the last `progress` upload (see below)

There is one further control op, `set_time`, which is not a transfer.

### `library`

`{"op":"start_get","kind":"library"}` makes the reader walk `/Books`, stage a JSON document on the SD card, and then
send it through the ordinary frame/ack path. `status` reports `state: "preparing"` while the shelf is being walked
(seconds on a large library), then `state: "sending"` with `kind: "library"`, `size`, and `sent` as usual. Like every
other operation it is refused until `hello` has been accepted.

The payload is a JSON array, one object per book:

```json
[
  {"filename":"Dune.epub","size":1048576,"title":"Dune","author":"Frank Herbert","percent":0.4237,
   "location":"0300170023000000","timestamp":1725600000},
  {"filename":"Classics/Ulysses.epub","size":2097152,"title":"Ulysses","author":"James Joyce","percent":0}
]
```

| Field | Type | Source |
| --- | --- | --- |
| `filename` | string | Path relative to `/Books`, so a book in a sub-folder reads `Sub/Folder/Book.epub`. Always present. |
| `size` | number | Size of the book file in bytes. Always present. |
| `title` | string | The book's cached metadata title. Falls back to the bare filename when no metadata is available; never empty. |
| `author` | string | The book's cached metadata author. Empty string when unknown. |
| `percent` | number | Progress through the whole book, `0`–`1`, rounded to four decimals. `0` for a book that was never opened. **Display only** — never sync on this. Always present. |
| `location` | string | The saved position, exactly as the device stores it, hex-encoded. Absent for a book that was never opened. This is the field to sync on. |
| `timestamp` | number | UTC epoch seconds at which `location` was saved. **Absent means unknown**, not `0`. |
| `lastRead` | number | **Never emitted by this firmware.** Use `timestamp`. See below. |

Notes on the fields:

- Books are the formats the reader can open: `.epub`, `.xtc`/`.xtch`, `.txt`, `.md`. Sub-folders of `/Books` are walked
  to a depth of four; dot-entries are skipped. Entries are ordered per folder the way the file browser orders them.
- `title`/`author` come from the per-book metadata cache the reader writes the first time a book is opened, not from
  the recents list, so every book on the card is covered rather than only the ten most recent. A book that has never
  been opened has no EPUB metadata cached and lists under its filename with an empty author; the reader deliberately
  does not index books to answer this request. XTC books carry metadata in the file header, so they list correctly
  even before first open. `.txt`/`.md` have no embedded metadata at all and always list under their filename.
- `percent` is derived from the book's saved position, using the same mapping the reader and the KOReader sync path
  use. `.txt`/`.md` books always report `0`: their saved position is a page index whose page count depends on the
  current font, margins, and viewport, and none of that is recorded on disk, so no percentage can be recovered.
  `location` and `timestamp` are still exact for those books, which is why syncing on the stored position covers
  them and syncing on a percentage would not.
- `location` is the reader's own `progress.bin` byte for byte (see [Position encoding](#position-encoding)). Hand it
  straight back in a `progress` upload and the book reopens exactly where it was. Nothing anywhere converts a
  percentage into a position: that conversion is lossy — recovering a spine index and page from `0.4237` needs the
  book's pagination, which depends on the current font, margins and viewport — so it is not offered at all.
- `timestamp` is when that position was saved, from the device's own clock. It is omitted for a book whose position
  predates this firmware, and for one saved while the device did not know the time (see [Device clock](#device-clock)).
  Absent means unknown, and unknown loses every conflict.
- `lastRead` is specified as optional and this firmware still never emits it. It asked a vaguer question ("when was
  this book last read") than the sync path needs; `timestamp` answers the precise one and is what clients should use.

The document is streamed a book at a time, so it is never assembled in RAM. It is staged as
`/.crosspoint/ble-library.json` for the duration of the transfer and removed when the transfer screen is closed. A
`start_get` at `offset: 0` rebuilds the listing; a non-zero `offset` resumes the document that the preceding
`offset: 0` request staged, so a resumed download never straddles two different snapshots.

## Position encoding

`location` is the reader's saved position with nothing done to it. On disk that position lives in
`progress.bin` inside the book's cache directory; its layout is private to the reader activity that wrote it
(EPUB: spine index, page, chapter page count and optionally a visible-text offset, in 4, 6 or 10 bytes; XTC and
`.txt`/`.md`: a 4-byte page index). The protocol does not know or care what the bytes mean.

To survive JSON they are **lowercase hex, two characters per byte, no separators and no prefix**. `location` is
therefore an even-length string of 8, 12 or 20 characters. Hex rather than base64 because a position is at most ten
bytes, hex stays readable in a log or a bug report, and it needs no padding or alphabet caveats.

```text
progress.bin bytes  03 00 17 00 23 00 00 00
location            "0300170023000000"
```

The round trip is byte-for-byte: whatever `library` reports in `location`, handing that same string back in a
`progress` upload writes exactly those bytes back into `progress.bin`. Clients must not construct, truncate, pad or
otherwise edit a `location` — only store one and return it.

The device does check the *length* against the book's type before writing (EPUB accepts 4, 6 or 10 bytes; XTC, `.txt`
and `.md` accept 4), because for EPUB the byte count selects the format and a wrong length would resume the book
somewhere arbitrary. A length the book's reader does not write is rejected as `invalid`.

## Device clock

A build without the network stack has no NTP, so **the phone sets the device clock**:

```json
{"op":"set_time","epoch":1725600000}
```

`epoch` is UTC seconds. It is accepted only in `[1577836800, 4102444800)` — 2020 through 2099 — and refused outside
that as `invalid epoch`; the lower bound is what distinguishes a real wall clock from an RTC that has never been set,
and the upper bound stops a garbled value becoming a timestamp no later save can beat. Like every other op it requires
`hello` first. Success changes no state: the acknowledgement is the new `device_time` in the `status` the client is
already subscribed to.

`status` always carries `clock_supported`, and carries `device_time` **only when the device actually knows the time**:

```json
{"clock_supported": true, "device_time": 1725600000}
```

- `clock_supported: false` — this board has no RTC. `set_time` is refused with `no clock on this device`, saved
  positions are never timestamped, and this device always loses a conflict. Hide the control.
- `clock_supported: true` with no `device_time` — there is an RTC but it does not hold a plausible time: never set, or
  its backup power was lost. Send `set_time`.
- `device_time` present — compare it against your own clock and re-send `set_time` if it has drifted.

`device_time` is never `0`. An unknown time is an absent field, because `0` would read as a real instant in 1970.

**Set the clock before the first sync.** Until the device has a working clock its own saves carry no timestamp, and
an unstamped save loses every conflict — so a client that never calls `set_time` will happily overwrite reading the
user did on the device. The device cannot defend progress it could not date. A client should send `set_time` as soon
as `status` shows `clock_supported: true` with no `device_time`, and before uploading a `progress` batch.

The clock is a hardware RTC with full date registers (PCF8563 on this board, DS3231 and RX8130 on others), so this is
a genuine wall clock, not a since-boot counter — the firmware does not fall back to one, and never fabricates a date.
The RTC is read at most every 10 seconds and the reading is carried forward with the millisecond counter in between,
so two saves inside one poll window still order correctly.

## `progress`

`{"op":"start_put","kind":"progress","size":…,"sha256":…}` uploads reading positions from the app to the device. It
uses the ordinary upload machinery — binary frames on `data-in`, credit-based flow control via `ack_bytes`, SHA-256
over the whole document, `commit` — and the same `hello` gate. `name` is not used: the batch is scratch, staged at a
fixed path, and deleted once applied. The document must be at most 512 KB and 8192 entries.

The payload is a JSON array, one object per book:

```json
[
  {"filename":"Dune.epub","location":"0300170023000000","timestamp":1725600000},
  {"filename":"Classics/Ulysses.epub","location":"1a000400","timestamp":1725600123}
]
```

| Field | Type | Meaning |
| --- | --- | --- |
| `filename` | string | Path relative to `/Books`, exactly as `library` reported it. Required. |
| `location` | string | Hex-encoded saved position (see [Position encoding](#position-encoding)). Required. |
| `timestamp` | number | UTC epoch seconds at which the app believes that position was reached. Required. |

Nothing is written until the whole batch is on the card and its SHA-256 matches, so a transfer cut short cannot
half-apply.

### Conflict rule

An entry is applied **only if `timestamp` is strictly newer than the timestamp stored beside the device's own saved
position for that book.** Otherwise it is skipped and reported `skipped_older`.

- A device timestamp that is **unknown counts as the oldest possible**, so any valid incoming timestamp wins. Unknown
  means: the position was saved by firmware older than this feature, or saved while the device did not know the time.
- **Equal timestamps do not apply.** Echoing back what `library` just reported is a no-op, not a rewrite.
- An entry whose `timestamp` is outside the range `set_time` accepts is `invalid`, not applied. There is no path that
  writes a position without a usable timestamp.

### Per-entry results

One bad or unknown entry costs that entry only, never the batch: a book the phone knows about but this card does not
is the ordinary case. Every entry gets one of:

| Result | Meaning |
| --- | --- |
| `applied` | Written, and stamped with the incoming `timestamp`. |
| `skipped_older` | The device's own save is at least as new. |
| `not_found` | No such file under `/Books`. |
| `unsupported` | A file extension the reader cannot open, so it has no position format. |
| `invalid` | Malformed `filename`, `location` or `timestamp`, or a `location` length the book's reader does not write. |
| `write_failed` | The position could not be persisted (SD error). |

The outcomes are **not** in `status`: a notification carries at most `ATT_MTU - 3` bytes, so a shelf-sized array would
be truncated on the wire with no error. `status` reports only the headline after `commit` —

```json
{"state":"saved","kind":"progress","entries":42,"applied":37}
```

(Deliberately terse: the byte counts an upload normally reports are dropped for this kind so the whole status still
fits in one notification.)

— and the full list is fetched as a download:

```json
{"op":"start_get","kind":"progress_result"}
```

whose payload is a JSON array parallel to the upload, in the same order:

```json
[
  {"filename":"Dune.epub","result":"applied"},
  {"filename":"Classics/Ulysses.epub","result":"skipped_older"}
]
```

It is staged at `/.crosspoint/ble-progress-result.json` and removed when the transfer screen is closed, so fetch it
before disconnecting. A batch that fails as a whole (malformed JSON, over the entry cap, or an SD write error) reports
`state: "error"` and produces no result document.

### Currently-open book

There is none, by construction. Reaching the Bluetooth Transfer screen goes through
`ActivityManager::goToBluetoothTransfer()`, which calls `replaceActivity()` — that drops the current activity *and*
the whole activity stack. A reader has therefore already run `onExit()`, written its final `progress.bin` and been
destroyed before BLE advertising starts, and when the user next opens the book the reader loads its position from
disk. So there is no in-memory reader state for an incoming write to fight: positions are applied immediately, none
are deferred, and none are rejected on this account.

### Where the timestamp lives

Beside `progress.bin`, in a nine-byte sidecar `progress.time` (magic `CPPT`, a version byte, then the UTC epoch
little-endian), not inside `progress.bin` itself. Two reasons:

1. All three readers dispatch on the **exact byte length** of `progress.bin` — the EPUB reader accepts 4, 6 or 10 and
   treats 10 as "position + visible-text offset". Appending four timestamp bytes would turn a 6-byte position into the
   10-byte form and resume the book at a text offset that is really a clock reading. Length is load-bearing there; it
   cannot carry a trailer.
2. Firmware that predates this must keep opening books written by firmware that has it. An ignored extra file is
   compatible in both directions; a longer `progress.bin` is not.

Backwards compatibility follows from that: **every position saved before this existed has no sidecar, and reads as
"unknown"** — which the conflict rule treats as older than any real timestamp, never as epoch 0. A sidecar that is
short, has the wrong magic, or holds an implausible epoch also reads as unknown.

`progress.bin` keeps its crash-safe temp-and-rename write (issue #2275). The sidecar is written in place afterwards:
it is nine bytes in a single sector, and a torn write there fails the magic check and degrades to "unknown" — a
skipped sync, not a lost book. A save made while the device has no working clock **deletes** any existing sidecar
rather than leaving a stale one, because a stale stamp would claim a position the user reached just now was reached
much earlier, and let an incoming sync overwrite genuinely fresher reading.

## Explicit Non-goals

This protocol deliberately stays within CrossPoint Reader's project scope. CrossPoint does not support packages,
plugins, or package-state diagnostics, so the BLE service does not add package uploads or package-state downloads. It
also does not expose arbitrary SD-card browsing, arbitrary path reads, or arbitrary path writes.

Those operations belong in Marginalia or other forks that choose a package/plugin model. They are not reserved for a
later CrossPoint BLE iteration.

## Status Capabilities

Status JSON includes capability fields so clients can hide unsupported controls:

```json
{
  "protocol_version": 1,
  "firmware_name": "CrossPoint Reader",
  "upload_kinds": ["book", "bmp", "firmware", "progress"],
  "download_kinds": ["crash_report", "library", "progress_result"],
  "firmware_ota_supported": true,
  "clock_supported": true,
  "device_time": 1725600000,
  "browser_companion_url": "https://ble.xteink.lol/"
}
```

`device_time` is present only when the device knows the time; see [Device clock](#device-clock).

Clients should still handle `state: "error"` for rejected operations.
