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

Downloads use `start_get`, notifications on `data-out`, and one `get_ack` per frame.

Supported download kinds:

- `crash_report`: reads `/crash_report.txt`
- `library`: the on-device book list with reading progress (see below)

### `library`

`{"op":"start_get","kind":"library"}` makes the reader walk `/Books`, stage a JSON document on the SD card, and then
send it through the ordinary frame/ack path. `status` reports `state: "preparing"` while the shelf is being walked
(seconds on a large library), then `state: "sending"` with `kind: "library"`, `size`, and `sent` as usual. Like every
other operation it is refused until `hello` has been accepted.

The payload is a JSON array, one object per book:

```json
[
  {"filename":"Dune.epub","size":1048576,"title":"Dune","author":"Frank Herbert","percent":0.4237},
  {"filename":"Classics/Ulysses.epub","size":2097152,"title":"Ulysses","author":"James Joyce","percent":0}
]
```

| Field | Type | Source |
| --- | --- | --- |
| `filename` | string | Path relative to `/Books`, so a book in a sub-folder reads `Sub/Folder/Book.epub`. Always present. |
| `size` | number | Size of the book file in bytes. Always present. |
| `title` | string | The book's cached metadata title. Falls back to the bare filename when no metadata is available; never empty. |
| `author` | string | The book's cached metadata author. Empty string when unknown. |
| `percent` | number | Progress through the whole book, `0`–`1`, rounded to four decimals. `0` for a book that was never opened. Always present. |
| `lastRead` | number | **Never emitted by this firmware.** See below. |

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
- `lastRead` is specified as optional and this firmware never emits it. CrossPoint stores no per-book timestamp:
  the saved position file holds a position and no time, SD file timestamps are not maintained on this device, and the
  RTC exposes only hour and minute. Clients must treat an absent `lastRead` as "unknown" rather than as epoch 0.

The document is streamed a book at a time, so it is never assembled in RAM. It is staged as
`/.crosspoint/ble-library.json` for the duration of the transfer and removed when the transfer screen is closed. A
`start_get` at `offset: 0` rebuilds the listing; a non-zero `offset` resumes the document that the preceding
`offset: 0` request staged, so a resumed download never straddles two different snapshots.

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
  "upload_kinds": ["book", "bmp", "firmware"],
  "download_kinds": ["crash_report", "library"],
  "firmware_ota_supported": true,
  "browser_companion_url": "https://ble.xteink.lol/"
}
```

Clients should still handle `state: "error"` for rejected operations.
