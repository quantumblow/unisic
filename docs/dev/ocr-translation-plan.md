# OCR and translation: remaining work and refactor plan

Status: plan, written 2026-11-13. Baseline `dev` = `908c5e2` (PR #2 and PR #3 merged).
This file lists what is left after the PP-OCRv6 engine, the result window and
line-by-line translation landed. Read it before starting a round, and keep the
"done" markers current: a round that finishes a task ticks it here in the same
commit.

Placeholders are impossible: this project has no optional dependencies, every
user-facing string ships in eight catalogs, every new path gets an F8 smoke step
and a Settings Developer button, and every count this document mentions is
corrected in the same commit that changes it. The rules are in `AGENTS.md` and
`CLAUDE.md`; the subsystem notes are in `docs/dev/architecture.md`
(`src/ocr/`) and `docs/dev/packaging.md`.

## Where things stand

Working today:

- PP-OCRv6 det+rec on the shared `dlopen()`ed ONNX Runtime, tiers tiny / small /
  medium, downloaded on request into `<AppDataLocation>/paddleocr/<tier>/`,
  pinned by ModelScope URL + size + SHA-256. `auto` routes to PP-OCRv6 for
  Chinese (both scripts), Japanese and 48 Latin-script languages and keeps
  Tesseract for Korean, Arabic, Cyrillic, Thai, Indic and Greek.
- Result window: capture at 100% (scaled down only when it cannot fit), selectable
  text pane, clickable line boxes, Copy all, Original/Translation, target-language
  picker, translation laid over the picture line by line with colours sampled from
  each line, Cancel, error line, Esc to close. Since M2: Save image / Copy image /
  Edit in editor on the shown layer at the capture's own resolution, zoom
  (buttons, Ctrl+wheel, fit, 100%) and drag-to-pan with exact hit testing, a
  remembered split divider, Ctrl+wheel text size, an in-window status line, and
  overflow/RTL handling on the covers. Since M3: a pane selection translates only
  the lines it touches and the button says which it will do, answers stream in line
  by line with Cancel mid-stream, a line too long for one request is split at a
  sentence boundary and rejoined into its one cover, a failed chunk gets one retry
  and then leaves the finished lines with the failed range named, and the button
  plus one dot per chunk show "Translating… 2/3". Since M4.5: the line boxes
  take Ctrl+click (toggle) and Shift+click (range) into a controller-owned set,
  Copy selection takes those lines in document order, Translate selection sends
  them through the numbered path, and a word or part of a line (a pane
  double-click, or a selection across part of two lines) translates that exact
  text with its line as context into the pane only, with no cover over the whole
  line. The chosen lines are tinted under the pane text, the boxes report their
  selected state, Ctrl+A picks all lines and Esc clears the set first.
- Translation: any OpenAI-compatible `/chat/completions`, numbered-line prompts,
  SSE streaming with a buffered fallback, a request token budget (and long lines
  split at sentence boundaries), tolerant parsing, whole-text fallback,
  unchanged-answer detection, one retry per chunk, partial results that keep the
  finished chunks and name the failed lines, progress per chunk. Since M4:
  Settings has a Test connection probe (10 s cap, round trip and the model's
  answer or the service's error verbatim), the first Translate to a non-local
  host asks once and remembers the answer per host, and settings export leaves
  upload credentials and the translation key out unless explicitly included.
- Verified: 28/28 `ctest` (M4 and M4.5 grew `TranslatorTest` to 24 cases,
  `OcrResultTest` to 23, and `UploadUrlTest` with the export-secrets case). F8 baseline from M2:
  `ocr result` / `ocr result image` / `translation` / `translation overlay` /
  `paddleocr` all PASS with models present; M3 adds `translation stream`; M4 adds
  `translation connection`, `translation privacy`, `settings secrets` and the
  secrets half of `settings round-trip`; M4.5 adds `translation selection`. All
  of them PASS in an offscreen dev build; none has been run on a desktop yet.
  Tiers measured (tiny 0.09-0.13 s and ~0.4 GB, small 0.20-0.27 s and ~0.6 GB,
  medium 0.52-0.68 s and ~1.2 GB per 1600x1000 page), tier switch reloads without
  a restart.

Environment, so a new session does not rebuild its own:

- Build and tests: container `unisic-zh-build` (Fedora 43), see
  `notes/ocr-paddle-translation.md` section 3.
- AppImage: `unisic-appimage-env:latest` + `~/.cache/unisic-appimage`, about 6 min.
- The tier benchmark harness lives in `paddle-tier-bench.cpp` (`notes/`), re-add it
  as a temporary CMake target when timing work needs it.

## M0. Un-red the Nix CI (0.5 day, do first) - DONE 2026-11-13

The `Nix flake` job fails on every PR, `dev` included, so a green PR is impossible
today. The failure is not OCR: `unisic_bash_completion_test` sources
`resources/completions/unisic.bash`, which calls `compgen`, and the bash in the Nix
check sandbox has no `compgen` (26/27 other tests pass there).

**T0.1 (done)** `tests/bash-completion.sh` gets a `compgen` shim next to the existing
`compopt` shim: it only needs `-W <words> -- <prefix>`, so a 10-line prefix filter
covers it and the shipped completion script stays untouched.

- Acceptance: the script passes with the builtin disabled
  (`bash -c 'enable -n compgen; source tests/bash-completion.sh ...'` in the
  container), and the Nix job goes green on the PR that carries it.
- Files: `tests/bash-completion.sh` only.
- Done: the shim is installed only when `type compgen` fails, so the real builtin stays under test everywhere it exists. Verified in `unisic-zh-build` with `enable -n compgen` (all 11 cases pass) and with the builtin present; the Nix job itself is the last check and needs a push.

## M1. Out of the box: bundle the tiny model (2-3 days) - DONE 2026-11-13

The chosen direction: ship PP-OCRv6 **tiny** inside the binary so OCR works with no
download, and keep small / medium as explicit downloads for better accuracy. tiny
is 6.3 MB (det 1.8 MB, rec 4.5 MB, dictionary 27 KB) and cannot read Japanese, so
the routing has to know which tier is in use.

**D1 settled (2026-11-13): the bytes live in git and are embedded through the
qrc.** Every channel (AppImage, deb, rpm, Arch, Flatpak, Nix, tarball) then gets the
files for free, offline, with no manifest change; the cost is ~6 MB in the
repository and ~6 MB in every binary. Fetching them at build time was rejected: it
adds a network step to configure (a fixed-output derivation for the Nix sandbox,
one URL and hash per recipe) and breaks offline tarball builds.

**T1.1 (done)** Add the three pinned files (plus the Apache-2.0 notice and PaddleOCR's
copyright) and verify them at configure time with `file(SHA256 ...)`, failing
configure on a mismatch the way a missing dependency does.

- Files: `third_party/paddleocr/tiny/`, `CMakeLists.txt`, a notice file.

**T1.2 (done)** Embed through a qrc (`:/paddleocr/tiny/...`) and teach the loader to read
the bundled copy before the data directory. Sessions are already created from
memory (`CreateSessionFromArray`), so no ORT change is needed.

- Files: `third_party/paddleocr/tiny/` embedded by `CMakeLists.txt`'s `qt_add_resources` (the plan named a
  `resources/paddleocr.qrc`; the tree has no `.qrc` files and every other resource goes in the same way), `src/ocr/PaddleOcr.{h,cpp}`.
- Acceptance: with `<AppDataLocation>/paddleocr` deleted, `recognize()` still
  returns text, and `hasDownloads()` still says "nothing downloaded".

**T1.3 (done)** One "effective tier" rule, in one place: the selected tier when it is
installed, otherwise the bundled tiny, otherwise no PP-OCRv6 at all. D3 is settled:
`auto` does use the bundled tiny for Latin and Chinese, so an install with no
download starts on PP-OCRv6 instead of Tesseract.

- The Settings picker keeps showing the user's choice; the status line says which
  tier actually runs (for example "Tiny (included) is in use; download Small for
  better accuracy").
- `deleteDownloads()` removes only downloaded copies; the bundled tier has no
  Delete button and no Download button.
- Files: `src/ocr/PaddleOcr.{h,cpp}`, `qml/pages/SettingsPage.qml`, `src/Settings.h`.

**T1.4 (done)** Language routing has to follow the tier: tiny does not read Japanese, so
`supportsLanguages()` must say no to `jpn` when tiny is the effective tier, which
puts Japanese back on Tesseract until small or medium is installed. Pin this with
unit tests (`supportsLanguages` is testable without models).

- Files: `src/ocr/PaddleOcr.cpp`, `tests/` (new or existing OCR test).

**T1.5 (done)** Settings > General > Text recognition: show Included / Downloaded per
tier, gate the Download and Delete buttons on that, and keep the existing download
progress and Cancel.

**T1.6 (done)** Packaging: because the model is in the binary, no manifest changes are
expected. Confirm each channel (AppImage, deb, rpm, Arch, Flatpak, Nix) still
builds and reports the same gates, and record the new binary size.

**T1.7 (done)** Docs and strings: `README.md` (OCR works out of the box; Small is worth
downloading), `docs/dev/packaging.md` (bundled tier, sizes, license, why not
bundled on Windows), `docs/dev/architecture.md`, `resources/CHANGELOG.md` (EN/PL,
current beta heading), all eight catalogs, and the F8 step text, which currently
says "SKIP (models not downloaded)" and will now PASS everywhere.

- Acceptance: F8 `paddleocr` PASS with an empty app data dir; `ctest` green;
  `unisic --version`-style CLI smoke unaffected; AppImage rebuilt and run under
  Xvfb with a Chinese capture.

**Done in this round (2026-11-13):**

- Bytes: `third_party/paddleocr/tiny/` plus `LICENSE` (Apache-2.0) and `NOTICE`; sizes and SHA-256 checked by
  `CMakeLists.txt` at configure time, embedded for Linux through `qt_add_resources` and not for Windows.
- `PaddleOcr::effectiveTier()` is the one rule (selected when installed, else bundled tiny, else none). Routing, the
  `MemAvailable` guard and the loader follow it; `detPath`/`recPath`/`dictPath` read the qrc copy first.
- `supportsLanguages()` is now `effectiveTier()` + `tierSupportsLanguages()`, a pure per-tier table: tiny says no to
  `jpn`/`jpn_vert` (also inside a mixed spec) and auto script mode keeps Tesseract when Japanese is among the installed
  packs. Small and Medium still say yes. Pinned by `tests/PaddleOcrTest.cpp` (6 cases, 28/28 `ctest`).
- Settings lists Tiny/Small/Medium with `Included`/`Downloaded`/`Not downloaded`, marks the tier in use, hides Download
  for a bundled tier and hides Delete for it too; the status lines explain the fallback and say Small is worth
  downloading. `architecture.md` has the `modelInstalled`/`ready`/`tierStates` contract.
- Verified in `unisic-zh-build`: `ctest` 28/28, and F8 with `<AppDataLocation>/paddleocr` moved away reports
  `paddleocr: PASS (8 glyphs; script map PASS; tiny in use)` and moves the step out of SKIP. That run ended at 100 PASS,
  6 FAIL, 32 SKIP; the FAILs are container gaps (no WebP encoder, no portal twice, no ffmpeg twice, and
  `edit from history` failing to save its source), not OCR. With the models installed again the same step reports
  `small in use` on the final tree, so the tier in the line follows `effectiveTier()` as designed.
- A real Chinese capture (a text block cropped from the previous round's screenshot) reads back correctly through the
  embedded tiny pair with the model directory gone, and the Settings pane was screenshotted in English and Chinese in
  every state: bundled-in-use, selected-but-not-downloaded, and no ONNX Runtime (where "in use" is correctly not claimed
  and Download shows only the 11 MB runtime).
- T1.6: only the AppImage was rebuilt here (164,977,144 bytes, 159.3 MB -> 165.0 MB, 10 gates ON, model names and new
  strings found in the shipped binary, starts under Xvfb with an isolated profile; hash and the full verification log in
  `notes/ocr-paddle-translation.md`). The `unisic-website` repo is cloned beside this one now (`unisic/unisic-website`,
  checked 2026-11-13), but it is pinned to the released version (`src/lib/release.ts` `VERSION`, bumped by
  `.github/workflows/follow-release.yml`) and its OCR text describes Tesseract only, so the PaddleOCR/tiny wording
  belongs to the 0.8.10 release rather than this PR. T6.7 has the file list.

**Follow-up (2026-11-13, user's decision): the AppImage and the tarball carry ONNX Runtime.** D1 decided where
the model bytes live, but the runtime was still a Settings download on this channel, so "works out of the box"
was only true in the Flatpak. `scripts/build-appimage.sh` and the `appimage` workflow now unpack the same pinned
1.31.1 tarball into `usr/lib`, create the `libonnxruntime.so.1` link `QLibrary` asks for, ship its LICENSE and
ThirdPartyNotices, and assert all three after linuxdeploy (which cannot discover a `dlopen()`ed library, so this
is done by hand; linuxdeploy's own dependency pass then patches the file it finds in the AppDir). Measured:
175,106,552 bytes (159.3 MB before the OCR work, 165.0 MB with the models only), sha256
`87254d2a0723c89ba80af92eca4117cee37d37d796180d6ec7486d9af462bc5e`; the extracted bundle's log says
`paddleocr: ONNX Runtime 1.31.1 from libonnxruntime.so.1`, Settings shows `Tiny (6 MB): Included, in use` with no
Download button and only Small/Medium offered, and background removal asks for its model only. deb, rpm, Arch
and Nix keep the system package or the 11 MB download. The manual pass at the end of this document checks it on
each channel.

**Risk worth writing down:** tiny misreads Han glyphs that small gets right
(素材库 came back as 索材库 in the measurements), so "works out of the box" must
not read as "best result". The changelog and the Settings help both say Small is
the one to download.

## M2. Result window: save, edit, zoom (3-4 days) - DONE 2026-11-13

**T2.1 (done)** Move the composite to C++: `OcrResultController::compositedImage(bool translated)`
draws the covers and the fitted text at source resolution. The font fitting moved out of QML
(`Text.Fit`, 5 px floor, wrap) into `fittedPixelSize()`: the largest integer pixel size at which
the text wrapped at the box width still fits its height, stored per cover as `fontSize` in IMAGE
pixels, so QML scales that one number by its render scale instead of running a second fit. The
saved picture and the screen therefore cannot pick different sizes.

- Acceptance met: `OcrResultTest` pins it - a 4-line synthetic capture where every cover's size
  is the largest that still fits (checked against its own QTextLayout), the composite is exactly
  the capture's size (including a 3840x2160 one), nothing is drawn outside a cover, and the ink
  inside each cover is the sampled foreground. Verified live: the saved PNG of the synthetic
  result is 900x420 while the window was zoomed to 156%.

**T2.2 (done)** Save image… and Copy image for whichever layer is shown (Original or Translation).
Both go through `compositedImage()`, so they are at the capture's own resolution. Save opens the
native file dialog (D4) in the save folder + date subfolder with the capture filename template and
`.png` by default; the saved file gets a History tile. The window confirms in place ("Saved …",
"Image copied") because the app's toasts live in the MAIN window, which the result window can be
covering.

**T2.3 (done)** Edit in editor hands the composited image to a new `EditorSession` window (no
overwrite path and no history id: the editor's Save writes a new capture). Verified live: the
editor opened the 900x420 translated picture and showed `900 x 420 px`.

**T2.4 (done)** Zoom and pan in the picture pane: zoom out / 100% (the percentage button) / zoom
in / fit, Ctrl+wheel anchored at the cursor, plain wheel and drag to pan. Line boxes, covers and
their text are laid out in device pixels from `renderScale`, so the text is re-rendered (not
raster-scaled) and `lineAt(imagePoint, translated)` answers from the controller in image
coordinates - exact at any zoom. Verified live at 156%: a click on a zoomed cover selected the
right line in the pane; Ctrl+wheel at a corner kept that point under the cursor.

**T2.5 (done)** Split ratio: a draggable divider between the panes (works side by side and
stacked), stored in `ocr/resultSplit` (0 = auto, derived from the picture at 100%, so a fresh
window opens exactly as before), reset by double-click, Backspace or the focus ring's arrow keys;
Ctrl+wheel over the text pane changes its font size (`ocr/resultTextSize`, 0 = theme). Verified
live across a restart: `resultSplit=0.76` and `resultTextSize=20` came back with the window.

**T2.6 (done)** Overlay edges: the horizontal 30% expansion is capped at half the gutter to a
neighbour box on the same row, so a multi-column page stops grazing the next column; a
translation that cannot fit at the 5 px floor is clipped there, marked with a corner dot in the
window AND the saved picture, and the window says the full text is in the pane; an RTL target
(first strong character Arabic/Hebrew) is right-aligned in its cover by both the window and the
composite. Verified live on Arabic and Hebrew covers, and in the saved PNG.

Also in this round: F8 `ocr result image` step + `devTestOcrResultImage`/`devTestOcrResultWindow`
buttons (the second opens a synthetic result AND translation, so every new path can be tried with
no model and no endpoint), `OcrResultTest` grew five cases, and the window's chrome got its own
row for the picture actions and the zoom controls.

## M3. Translation: selection, streaming, protection, retries (3-5 days) - DONE 2026-11-13

**T3.1 (done)** Translate only the selection: when the text pane has a selection,
Translate works on those lines and the overlay covers exactly those lines; with no
selection it keeps translating the whole text. The button label says which one it
will do.

- `OcrResultController::translateRange(start, length)` widens the character
  selection to every line box it touches (`selectedLineIndexes()`, pure and
  pinned), sends only those lines, and maps covers only onto them. A selection
  made on the Translation layer maps back through `sourceStart`/`sourceLength`
  on each cover. The window shows "Translate selection" or "Translate all" and a
  line saying only the selected lines were translated.
- Acceptance met: `OcrResultTest::selectionMapsToWholeLines` pins the widening,
  the newline-only selection (no lines) and the reverse map.

**T3.2 (done)** Streaming: `stream: true` against OpenAI-compatible endpoints,
parse the SSE `data:` frames, append into the pane as they arrive, keep Cancel
working mid-stream, and fall back to the buffered request when an endpoint ignores
or rejects streaming. Extend the mock endpoint with an SSE mode for the tests.

- `Translator::translateStreaming()`/`takeSseDeltas()`/`parseStreamDelta()` parse
  OpenAI `data:` frames (NDJSON tolerated). A 400/404/405/415/422 to
  `stream: true` is retried buffered with nothing shown; a 200 JSON body from an
  endpoint that ignored `stream` is parsed as one answer. `translateStreamingTo()`
  is the explicit-endpoint seam the F8 and Developer checks use, so they never
  read or write the user's translation settings. The controller shows the
  arrived part through `parseNumberedLinesPartial()`, throttled to 60 ms, and
  builds covers only when a chunk finishes so a cover cannot point at a stale
  range. Cancel aborts the reply and drops the partial answer.
- Acceptance met: `tests/MockTranslationEndpoint.h` serves SSE, a JSON body, a
  stream-rejecting path and a slow stream; `TranslatorTest` covers the parser,
  the fallback, a JSON answer and Cancel mid-stream; `OcrResultTest` covers the
  live pane and Cancel through the controller.

**T3.3 (done)** Context protection: estimate tokens (about 4 characters per token
for Latin, about 1 per Han character), cap a request at a safe budget, split a very
long line at sentence boundaries, and tell the user when a line was split instead
of silently truncating.

- `Translator::estimateTokens()`, `splitLongLine()` (sentence, then word, then
  character) and `chunkTexts()`; `kRequestTokenBudget = 2000` and
  `kItemTokenBudget = 700`. A split line's parts are separate numbered items that
  are joined back into the one cover they belong to; `translationSplitLines`
  drives a window note.
- Acceptance met: `TranslatorTest` pins the estimate, the split without text
  loss, and the budget; `OcrResultTest::longLineIsSplitAndRejoined` proves a
  160-sentence line is split and one cover carries both translated parts.

**T3.4 (done)** Chunk retry and partial results: one retry with a short backoff per
chunk on a transport error or 5xx; when a chunk still fails, keep the finished
chunks, show what did translate, and name the part that did not, instead of
dropping the whole translation.

- `Done` carries `retryable` (transport, 5xx, 408/429); the controller retries a
  chunk once after 700 ms, then records its items as failed and continues.
  Finished chunks keep their covers and `translationError` names the failed
  1-based line ranges ("Lines 41-45 did not translate: ...", or "Selected lines
  ..." for a selection). The whole-text fallback retries once the same way.
- Acceptance met: `OcrResultTest::retriesAChunkOnceThenSucceeds` (first request
  500, second succeeds) and `partialTranslationNamesFailedLines` (40 lines shown,
  lines 41-45 named, three requests).

**T3.5 (done)** Progress: "Translating… 2/3" while chunks are in flight, and
per-chunk state in the pane when streaming and chunks are combined.

- `translationChunkCount`/`translationChunkIndex`/`translationChunksDone` drive
  the button label and one dot per chunk in the text pane (done, in flight,
  pending). A retry keeps the dot on the chunk being retried.

**Done in this round (2026-11-13):**

- `ctest` 28/28 in `unisic-zh-build`: `TranslatorTest` 19 cases, `OcrResultTest`
  19 cases.
- F8 `translation stream` step and the Settings Developer `Translation stream`
  button: selection widening, sentence split, token budget, SSE frames, the
  buffered fallback and a 5xx flagged retryable against a local `QTcpServer`, with
  no configured endpoint and no network. The step itself was run first in an
  offscreen dev build and reports `PASS (selection, sentence split, token budget,
  SSE frames, buffered fallback, retryable 5xx)`; the full F8 pass was not run
  because its fullscreen capture step needs a compositor and hangs on the Xvfb
  fallback, so the desktop row below stays open.
- All 14 new strings are translated in the eight catalogs; CHANGELOG EN/PL has
  the M3 entry.
- AppImage rebuilt from the merged `dev` for the real-machine pass: 175,172,088
  bytes, sha256
  `87a9b1634ea0ff2fd751cd3d842b087ccb1b8c2913cc128a58c3c3312ef4a45d`, 10 gates ON,
  the new strings are in the binary, and it starts under Xvfb with an isolated
  profile.

## M4. Settings, privacy, secrets (1-2 days) - DONE 2026-11-13

**T4.1 (done)** Test connection in Settings > General > Translation: one small
request with the configured endpoint, key, model and target, a 10 s timeout, then
success with the round-trip time and the model's answer, or the endpoint's error
verbatim.

- `Translator::testConnectionTo()` sends one buffered chat request with a hard
  QTimer cap (10 s for the button, overridable so the F8 check can use 300 ms) on
  top of `setTransferTimeout`, measures the round trip with QElapsedTimer, and
  reports `parseResponse`'s service message untouched (HTTP status plus body
  only when the body carries no error object). `testConnection()` reads Settings;
  AppContext owns a separate `Translator` probe, so the button can never cancel a
  translation in the result window. The Settings row shows the answer or the
  error under the button, and the running state disables it.
- Acceptance met: `TranslatorTest::testConnectionReportsTheAnswer`,
  `testConnectionReturnsTheServiceErrorVerbatim` and `testConnectionTimesOut`
  (a `Silent` mock path) pin the three outcomes. F8 `translation connection` and
  the Developer `Test connection` button run the same success/error/timeout
  chain against a local mock.

**T4.2 (done)** Settings export: secrets (upload credentials and the translation
key) are out by default, with an explicit include option in the export dialog;
importing a file without them must not wipe the ones already configured.

- `exportSettings(file, includeSecrets = false)` skips `translationApiKey` and
  writes the marker `"secrets": false`; `UploadManager::redactSecrets()` removes
  destination `headers`, `user`, `data` and credential-looking `arguments`
  entries (`isCredentialArgument`), keeping non-secret ones such as reqtype/time
  so an imported built-in still uploads. The Settings header Export button opens
  a themed dialog with an "Include upload credentials and the translation API
  key" switch (off every time it opens); the CLI has `--include-secrets`.
  `importSettings` merges with `UploadManager::mergeSecrets()` only when the file
  says it left secrets out: every credential field the file lacks is restored
  from the destination with the same name, while a full export stays
  authoritative (a deliberately cleared key stays cleared). Old files without
  the marker are treated as complete, so behaviour is unchanged.
- Acceptance met: `UploadUrlTest::settingsExportRedactsSecrets` pins the
  redaction, the preserved non-secret arguments, the merge and the
  authoritative-file case; F8 `settings round-trip` now checks the marker, the
  omitted key, the explicit include and that a secrets-free import keeps the
  configured key; `settings secrets` and the Developer button cover the pure
  helpers.

**T4.3 (done)** First-send privacy notice: the first time Translate would send
text to a non-localhost endpoint, say once which host the text goes to and
remember the answer. Local addresses (127.0.0.1, localhost, ::1) never ask.

- `Translator::isLocalEndpoint()` is the one definition of local (case-insensitive
  127.0.0.1, localhost, ::1). `OcrResultController::startTranslation()` holds the
  request before anything is sent when the host is not local and differs from
  `Settings::translationPrivacyHost` ("translation/privacyHost"); it exposes
  `privacyNoticeHost` and `confirmPrivacyNotice()`/`cancelPrivacyNotice()`. The
  result window's modal notice names the host, says the screenshot itself is not
  sent, and is asked once per host; a changed endpoint asks again. `Cancel`, Esc
  and closing the window drop the held request.
- Acceptance met: `TranslatorTest::localEndpointsStayOnThisMachine` and
  `OcrResultTest::privacyNoticeAsksOncePerRemoteHost` (no request before the
  answer, remembered host, a second host asks again, a local mock never asks).
  F8 `translation privacy` and the Developer `Translation privacy` button check
  the classification and the host the notice names.

**T4.4, D2 settled (2026-11-13): no keychain.** The key stays in the owner-only
config file, documented as such; T4.2 is what keeps it out of exports. A keychain
would mean libsecret as a new mandatory dependency in six packaging recipes, which
the no-optional-dependencies rule does not allow for one string.

**Done in this round (2026-11-13):**

- `ctest` 28/28 in `unisic-zh-build` (`TranslatorTest` 23 cases, `OcrResultTest`
  20, `UploadUrlTest` one more case).
- F8 in an offscreen dev build: `translation connection`, `translation privacy`,
  `settings round-trip` and `settings secrets` all PASS; the run's four FAILs are
  the known container gaps (no notification host, no webp encoder, no portal, no
  screen), so the desktop row below stays open.
- All 24 new strings are translated in the eight catalogs; CHANGELOG EN/PL has
  the M4 entry.

## M4.5. Result window: multi-select and word translation (2-3 days, after M4) - DONE 2026-11-13

Deferred from the 2026-11-13 Ubuntu X11 pass. Both gaps came from the same
design as M3, not from a defect in it: the text pane is one native `TextEdit`
selection, and `translateRange()` deliberately widened a character selection to
the recognized lines so the numbered request could put a translation on each
cover. This round added the two missing interactions without changing the M3
line-overlay contract.

**T4.5.1 (done) Multi-select for Copy and Translate**

- Ctrl+click a line box on the picture toggles that line in the selection set;
  Shift+click extends from the anchor; a plain click resets to one line. The
  pane draws the union of the chosen ranges as tinted bands while `TextEdit`
  keeps its one native selection, so the set is a controller/QML model, not a
  QTextEdit feature.
- Copy selection copies the chosen ranges in document order, one per line; Copy
  all is unchanged. Translate selection sends those lines, and when every chosen
  range is a whole line the existing numbered request and covers are used.
- Keyboard: Ctrl+A selects all lines, Esc clears the set; the picture boxes and
  the pane bands share one state. Accessibility: each box reports selected or
  not selected.
- Acceptance met: `OcrResultTest::multiSelectionOrdersAndMerges` pins replace,
  toggle, Shift range, order/`selectionText`, Ctrl+A and clear;
  `wholeLineMultiSelectionPaintsCovers` proves two non-adjacent lines go out
  alone and paint exactly their two covers. The set lives in the controller, so
  zoom, pan and Original/Translation switching cannot reset it; a pane drag
  clears it because that is the partial-text path.
- Implementation: `OcrResultController::selectedLines/toggleLine/extendLineSelection/
  selectAllLines/clearLineSelection/selectionText/translateSelectedLines` plus
  `selectionChanged`; the window tints bands from `positionToRectangle`, maps a
  translated cover back to its source line through `sourceStart`, and reports
  `Accessible.selected` on both delegate kinds.

**T4.5.2 (done) Word and partial-text translation**

- Classify the selection before sending. Whole lines keep the numbered path and
  the picture covers. Anything shorter (a double-clicked word, part of a line)
  sends exactly that text, with the source line as context in the prompt, and
  shows the answer in the pane only. A partial line must not paint a cover: a
  word's translation would otherwise replace the whole line and misrepresent
  the page.
- The window says which mode ran ("Translated the selected text" for the pane,
  the existing overlay note for whole lines).
- Acceptance met: `partialTextStaysInThePane` sends the word "two" with
  "Context: One two three" and "Selected text: two", asserts no cover, and
  repeats for a range crossing a line boundary without expanding either line;
  `TranslatorTest::partialTextPromptKeepsTheContext` pins the prompt/request
  shape. `OcrResultController::rangeCoversWholeLines` is the one classifier.
- Implementation: `Translator::partialPrompt()`/`partialRequest()`,
  `OcrResultController::startPartialTranslation()`/`requestPartialText()` with
  the same one-retry rule, `translationPartialText` for the window note.

**T4.5.3 (done) Tests, F8, strings**

- `OcrResultTest`: range classification (whole line vs partial), multi-range
  order and merge, and the exact-text request/prompt.
- F8 `translation selection` step + `devTestTranslationSelection` button, on top
  of the existing synthetic result.
- Eight catalogs and a CHANGELOG EN/PL entry under the current beta heading.

Two choices settled at the start of the round as proposed: Ctrl+click works on
the picture's line boxes (the pane keeps its native selection, so no custom
hit-testing inside the text), and a word request carries its line as context.

**Done in this round (2026-11-13):**

- `ctest` 28/28 in `unisic-zh-build` (`TranslatorTest` 24 cases, `OcrResultTest`
  23, `UploadUrlTest` unchanged).
- F8 in an offscreen dev build: `translation selection` PASS, and the run's four
  FAILs are still the known container gaps; the desktop row below stays open.
- All 8 new strings are translated in the eight catalogs; CHANGELOG EN/PL has
  the M4.5 entry.
- AppImage rebuilt from `feat/ocr-m4` for the real-machine pass: 175,229,432
  bytes, sha256
  `15fd9669cd8735712c18dccd944dbc1ff7a18ab6360d0f2a2eb1eb4db91f45e4`, 10 gates
  ON, the new source strings are in the binary, `--version` answers 0.8.9, and
  it starts under an isolated offscreen profile and exits cleanly on SIGTERM.

## M4.6. Word and partial-line translation on the picture (hybrid) - done 2026-11-13

Status: implemented 2026-11-13 on `feat/ocr-m4.6`, merged into `dev` at
`9a107e7`, then refined at `48a0204` (one-line partial note, CJK cover 10%
below the fit) and `601c022` (glyph-level gutter cap). The controller half
(glyphs, fragments, cover/callout decision, screen-only composite) and the QML
callout are in; `ctest` is green, and the AppImage was rebuilt from the refined
tip. The desktop acceptance lines below are still open and are not claimed as
done.

Goal: after M4.5 a double-clicked word (or any partial-line selection) translates
into the pane only. The answer should also appear over the picture: replace the
selected glyphs in place when the translation is readable there, and otherwise
show a themed callout anchored to the selection. Whole-line selections keep the
M3 numbered path and covers unchanged.

### Settled choices

- **Hybrid presentation.** In-place glyph cover when the fitted text is readable
  at the selection; anchored callout otherwise.
- **Screen-only.** Partial covers and callouts never enter Save image or Copy
  image; the saved composite keeps whole-line covers only. The window says so.
- **Covers are single-line only.** A selection touching more than one OCR line
  uses one callout for the whole answer. Splitting a model answer across line
  fragments is guesswork and is out of scope.
- **Streaming shows the callout immediately**, then at Done the answer becomes a
  cover if it fits; otherwise the callout stays with the final text. No
  per-delta font resizing.
- **Still not a page translation.** `translationPartialText` stays true, and the
  "service did not translate line by line" note must not appear for a partial
  translation (it currently does; fix it in this round).

### Data model

- `OcrResultController::setResult()` keeps a per-glyph list built in the same
  pass as `m_text`: `{start, length, rect, line, spaceBefore}` indexing `m_text`.
  Both engines already emit one `OcrWord` per glyph, so no OCR change is needed.
- `m_partialRange` is the pane selection; `m_partialFragments` is one entry per
  OCR line the selection touches, `{start, length, rect}`, with `rect` the union
  of the selected glyphs on that line, padded and overshot like a line cover and
  capped at the neighbour gutters with the M3 logic.
- Presentation state: `partialPresentation` (`pane` | `cover` | `callout`), plus
  `partialCallout` `{source, translation, x, y, width, height}` in IMAGE pixels
  for QML to place. `translatedLines` carries the cover in cover mode.

### Presentation decision

At Done, and again when the target changes:

1. No fragment: `pane`, no overlay.
2. More than one fragment: `callout`, anchored at the first fragment.
3. One fragment: run `translatedLineMap({fragment}, {answer}, image, &text)` and
   read the fitted size from its entry. Use `cover` when the size is at least
   `kMinPartialCoverPixelSize` (10 image px) and the entry is not `overflow`;
   otherwise `callout`.
4. While the request streams: `callout` with the partial answer, so the user sees
   it immediately; the decision above runs at the end. A very fast answer may
   flash the callout before the cover; acceptable, and the delay can be raised
   if it looks bad in practice.

### Rendering

- **Cover:** the normal `translatedLines` delegate already draws it. The entry
  carries `screenOnly: true`, and `composeOverlay()` skips entries with that
  flag, so Save/Copy image cannot bake a word cover into the page.
- **Callout:** an Item inside `imagePane`, above the picture and boxes, placed at
  `holderX + rect.x * renderScale` / `holderY + rect.y * renderScale`, clamped to
  the pane. Below the fragment, flipped above when there is no room; width at
  most 320 screen px, wraps, themed surface and border, translation in the
  primary colour and the source fragment in the secondary colour. Hidden on
  Original, on a new result, on Cancel and while the privacy notice is open.
  It follows zoom and pan because it is positioned from `renderScale` and
  `holderX`/`holderY`.
- **Hit testing:** the cover is a normal entry with `sourceStart`/`sourceLength`,
  so `lineAt(translated=true)`, pane mapping and selection stay exact. The
  callout is read-only and does not take clicks.

### Window notes and strings

Replace the M4.5 pane-only note with `translationPartialText` plus the
presentation:

- cover: "Translated the selected text on the picture."
- callout: "The selection is too small for the translation to fit, so it is shown in a callout."
- both: "Word and partial-line translations are shown on screen only. Save image keeps the page."

Exact wording at implementation, translated in all eight catalogs.

### Tests, F8 and Developer

- `OcrResultTest`: glyph ranges index `m_text`; a whole-word selection makes one
  `screenOnly` cover when the answer fits; a long answer makes a callout instead;
  a multi-line partial selection makes one callout; `composeOverlay()` skips
  `screenOnly`; streaming updates the callout text.
- F8 `translation partial overlay` step and `devTestTranslationPartialOverlay`
  button, on top of the synthetic result, with a preview seam that applies a
  fixed answer without a network. Check the cover and callout decisions.
- Developer button opens the window with a short answer (cover) and a long answer
  (callout), so the two presentations can be checked by eye.
- Eight catalogs, CHANGELOG EN/PL under 0.8.10, and the `src/ocr/` architecture
  note.

### Files

`src/ocr/OcrResultController.{h,cpp}` (glyphs, fragments, decision, preview
seam), `qml/OcrResultWindow.qml` (callout, notes, `screenOnly`), `src/AppContext.h`
(new dev action), `src/diag/SmokeTests.cpp` (F8 and dev), `tests/OcrResultTest.cpp`,
`i18n/unisic_*.ts`, `resources/CHANGELOG.md`, `docs/dev/architecture.md`, this plan.

### Acceptance

- Double-click a word and Translate: a readable translation appears over that
  word; a long one appears in a callout anchored to it, both live while streaming.
- A multi-word selection on one line takes one cover or one callout; a selection
  crossing a line boundary takes one callout.
- Whole-line selections and the M4.5 multi-select still use numbered covers.
- Save image and Copy image contain no word cover or callout; whole-line covers
  are still saved.
- Zoom, pan, Original/Translation and a new result leave no stale overlay.

### Estimate

3-4 days: 0.5-1 day glyph list plus tests, 1 day fragment/cover map and the fit
decision plus the composite flag, 1 day callout QML and streaming, 0.5-1 day
F8/Developer/strings/docs.

### Done in this round (2026-11-13)

- `setResult()` builds `m_glyphs` in the same pass as the text; the glyph range
  test pins every glyph against `m_text`, its line and its `spaceBefore`.
- `startPartialTranslation(start, length)` + `rebuildPartialFragments()` produce
  one padded, neighbour-capped fragment per touched line; `applyPartialAnswer()`
  is the one decision (`pane` / `cover` / `callout`), the cover is `screenOnly`
  and `composeOverlay()` skips it. Cancel, failure, a new result and a
  whole-line translation clear the state.
- `qml/OcrResultWindow.qml` draws the callout in `imagePane` from `renderScale`
  and the holder offset, and reports the cover/callout and screen-only notes;
  the "service did not translate line by line" note no longer fires for a
  partial answer.
- `ctest` 28/28 binaries; `OcrResultTest` 32 cases (9 new M4.6 cases), F8
  `translation partial overlay` and `devTestTranslationPartialOverlay` wired,
  eight catalogs filled, CHANGELOG EN/PL under 0.8.10 and the `src/ocr/`
  architecture note updated.
- Acceptance refinements after the desktop pass: the partial note is ONE
  elided line (two stacked notes under the controls were easy to miss); a CJK
  cover is set 10% below the largest fitted size because those glyphs fill the
  em box; and a cover is capped at the glyphs just outside the covered range,
  not only at the next line box. Pinned by `cjkCoverSetsItselfSlightlySmaller`,
  `partialCoverStopsAtThePreviousWord` and `lineCoverStopsAtALabelGlyph`.
- AppImage rebuilt from `dev` (tip `601c022`, with the one-line note, the CJK
  cover size and the glyph-level gutter cap) for the real-machine pass:
  175,241,720 bytes, sha256
  `2c9e1ed121171e4baf38dec3dc2d125ec3d2dc309a9654a5df40d22555f85674`, release
  flavor (`UNISIC_BUILD_NUMBER=20261113`, no Developer pane or F8), 10 gates
  ON, the new M4.6 strings are in the binary, `--version` answers 0.8.9, and it
  starts under an isolated offscreen profile and exits cleanly on SIGTERM.
  `dist/SHA256SUMS` regenerated; the file is gitignored.
- Still open for the release pass: F8 on a desktop and the manual acceptance
  lines below.

### Start here (historical)

Branch `feat/ocr-m4.6` from local `dev` at the design commit. Read
`OcrResultController::setResult()` (the glyph rebuild),
`startPartialTranslation()` and `translatedLineMap()` first. First commit:
`m_glyphs` plus a test that every glyph range indexes `m_text`; then the fragment
map and the fit decision, then the QML callout.

## M5. The other scripts' recognizers (5-8 days, D5 settled: own round)

Korean, Arabic, Cyrillic, Thai, Indic and Greek currently fall back to Tesseract.
They cannot move to PP-OCRv6, because a recognizer can only output the classes its
dictionary defines, and PP-OCRv6's dictionary has none of them: the shipped
`ppocrv6_dict.txt` carries 15,565 Han, 491 Latin, 180 kana and 76 Greek letters
(math, not a Greek pack), and **zero** Hangul, Cyrillic, Arabic, Thai or Devanagari
entries. Those scripts exist as the PP-OCRv5 multilingual recognizers
(`korean_PP-OCRv5_mobile_rec`, `cyrillic_...`, `arabic_...`, `devanagari_...`, and
the 106-language set behind them), each with its own dictionary.

So M5 is not "downgrade to v5": it is "add the per-script recognizers that only v5
publishes". The detection half stays PP-OCRv6's (DB detection is script-agnostic),
which keeps one pipeline, one runtime and the same boxes. Work: pin the per-script
rec+dict files (URL, size, SHA-256), extend the model download UI with a scripts
section, route in `supportsLanguages()`, and cover it in F8 and the Developer pane.
The SnowShotOCR mirror used for the v6 tiers has no multilingual files (it carries
only ch_PP-OCRv5 det/rec), so M5 needs its own pinned sources, most likely the
official PaddleOCR release assets.

Large enough to deserve its own round; do it after M2 and M3 so the result window
and translation paths are settled.

## M6. Packaging and quality (3-4 days, spread over the rounds)

**T6.1** Flatpak: download the models inside a real sandbox and run OCR there
(`--share=network` is already in the finish-args; the code path is the one the
container test covers).

**T6.2 (done 2026-11-13)** Debian: rebuild the full package on `debian:trixie` (the last
`unisic-deb-build` attempt exited 1 and was never re-run). The `Debian build check` job runs
exactly that on every PR now, and it passed on #5 (7m35s) and #6 (7m47s).

**T6.3** Windows: one real-machine pass (Tesseract path, translation window,
`PaddleOcr::available()` false). CI compiles it; nobody has run it.

**T6.4** Download resume: HTTP Range plus a `.part` file, so an interrupted small
or medium download does not start over.

**T6.5** Mirror: a second pinned base (or a documented mirror procedure) so a
ModelScope outage does not disable downloads; base URL, sizes and hashes change
together.

**T6.6 (partly done)** Tests: pull the tier memory table, the effective-tier rule and
`MemAvailable` parsing into pure functions and cover them in QtTest; extend F8 and
the Developer pane for every new path (save image, edit in editor, zoom, selection
translation, streaming, retry, test connection). The routing half is done
(`tierSupportsLanguages()`/`tierReadsJapanese()` are pure and pinned by
`tests/PaddleOcrTest.cpp`); `runMiB`/`availableMiB` are still file-statics, and the
"selected tier installed" branch of the effective-tier rule needs a fixture model. M2
wired its own paths: F8 `ocr result image`, `devTestOcrResultImage` and
`devTestOcrResultWindow`, plus five `OcrResultTest` cases. M3 wired selection
translation, streaming, retry and partial results: F8 `translation stream`,
`devTestTranslationStream`, and the new `TranslatorTest`/`OcrResultTest` cases. M4
wired Test connection, the first-send privacy notice and the export-secrets rule:
F8 `translation connection`, `translation privacy`, `settings secrets` and the
Developer equivalents. M4.5 wired the line-box set and the exact-text path: F8
`translation selection` and `devTestTranslationSelection`. M4.6 wired the
word/partial-line overlay: F8 `translation partial overlay`,
`devTestTranslationPartialOverlay` and six `OcrResultTest` cases.

**T6.7** i18n and website: native review of the Chinese and Polish strings added in
this feature, and the `unisic-website` sync. The site repo is cloned beside this tree and
its layout is confirmed; it is release-scoped on purpose (`src/lib/release.ts` holds
`VERSION` and `RELEASED`, `.github/workflows/follow-release.yml` bumps them, and its last
commit before this round was "describe only what v0.8.9 ships"), so the OCR/PP-OCRv6 text
ships WITH 0.8.10 as one edit, version bump included. The files that need it then:

- `src/lib/i18n/*.ts` (nine catalogs: en, pl, de, es, fr, it, nl, zh-Hans, zh-Hant): the
  comparison row `{ feature: 'OCR', notes: ['Tesseract', ...] }` and the OCR lede name
  Tesseract only, and both need PP-OCRv6 (tiny included, small/medium downloads) plus the
  translation feature.
- `content/docs/capture.md` (the OCR section), `content/docs/dependencies.md` ("OCR language
  packs": the tiers, the ONNX Runtime, which scripts stay on Tesseract), `content/docs/editor.md`
  (the result window and Translate), `content/docs/installation.md` ("bundles ... OCR data").
- `src/lib/components/Read.svelte` and the hero/feature copy if either names the OCR engine.

## Order and dependencies

1. M0 first: it is the only thing standing between every PR and a green build.
2. M1 next: it changes what OCR does out of the box and touches routing, so land it
   before the window work.
3. M2 and M3 can run in parallel; M2 touches `OcrResultWindow.qml` and the
   controller, M3 touches `Translator` and the controller's chunking, so agree on
   the controller's API before starting both.
4. M4 after M3 (the Test connection button uses the same request path as Translate).
5. M4.5 after M4: it touches only the result window and the controller, and the
   X11 pass showed both gaps, so do not fold it into M4's settings/privacy work.
6. M4.6 (landed 2026-11-13): the hybrid word/partial-line overlay, designed
   above. It built on the same result-window/controller boundary; the AppImage
   is rebuilt, so only the desktop F8 pass and the accepted manual lines are
   still open.
7. T6.x release chores (T6.1, T6.3, T6.4, T6.5, T6.6, T6.7) after M4.6 and before
   0.8.10; T6.2 is already done.
8. M5 last, on its own.

Every milestone ends with: build, `ctest` (27 + new tests), F8 on a desktop, a
rebuilt AppImage, a CHANGELOG entry under the current beta heading (EN/PL), and the
counts/lists that changed corrected in the same commit.

## Decisions (settled 2026-11-13)

- **D1** tiny bytes go into git and are embedded through the qrc; no per-recipe
  download. See M1.
- **D2** no keychain. The key stays in the owner-only config file and T4.2 keeps
  it out of exports.
- **D3** yes: with tiny bundled, `ocrEngine=auto` uses PP-OCRv6 for Latin and
  Chinese on every install. Japanese stays on Tesseract until small or medium is
  downloaded, because tiny cannot read it.
- **D4** the saved composite is PNG, in the same directory and with the same
  filename template as a normal capture save.
- **D5** the other scripts' recognizers get their own round (M5), not this cycle.

## Verification commands

```sh
# build and unit tests (container, see notes/ocr-paddle-translation.md section 3)
docker exec -u 1000:1000 -e HOME=/tmp -w /src unisic-zh-build cmake --build build --parallel 20
docker exec -u 1000:1000 -e HOME=/tmp -e QT_QPA_PLATFORM=offscreen -w /src unisic-zh-build \
  ctest --test-dir build --output-on-failure

# AppImage (snapshot image, ~6 min)
docker run --rm --network host --security-opt label=disable \
  -v /home/lyon/coding/QuantumBlow/unisic:/src:ro \
  -v /home/lyon/.cache/unisic-appimage:/cache \
  -e UNISIC_SRC=/src -e UNISIC_CACHE=/cache \
  -e QT_VERSION=6.8.3 -e UNISIC_BUILD_NUMBER=20261113 \
  -e http_proxy=http://127.0.0.1:7897 -e https_proxy=http://127.0.0.1:7897 \
  -w /cache unisic-appimage-env:latest bash /src/scripts/build-appimage.sh
```

F8 baseline with models present: the four OCR/translation steps PASS; with M1 the
`paddleocr` step moves from SKIP to PASS on a clean profile, so the SKIP total
drops by one. Treat the other FAILs as the known container gaps
(`notes/ocr-paddle-translation.md` section 8) unless they change.

## Manual verification pass (before the 0.8.10 release)

Everything below needs a real desktop or the real machine; the container covers the
logic and the windows only. Run it once M2/M3/M4/M4.5 have landed, and add their
acceptance lines to the Desktop block as each milestone closes. Tick a line only with
the compositor or OS named next to it: "verified in a container" is not a tick here.

### Desktop, Wayland (KDE Plasma first, one GNOME pass)

- [ ] F8 in a dev build (`unisic --hotkey smoke-test` or F8): `paddleocr`,
      `ocr result`, `ocr result image`, `translation` and `translation overlay`
      PASS, no new FAIL.
- [ ] Region OCR (`Meta+Shift+T`) on a Chinese page and an English page: the result
      window opens with the untouched capture at 100%, clickable lines, Copy all.
- [ ] Translate against a local Ollama: line-by-line overlay, colours sampled from a
      dark banner, Cancel mid-request, Original/Translation switch, Esc to close.
- [ ] Settings > General > Text recognition on the AppImage/Flatpak: `Tiny (6 MB):
      Included, in use` with no runtime download. Download Small, OCR a Japanese page
      (Small in use) and a Chinese page.
- [ ] Remove background in the editor: the runtime comes from the bundle on
      AppImage/Flatpak and from the 11 MB download on deb/rpm/Arch; one run, one undo.
- [ ] 100%, 125% and 150% display scale, plus a 1060x700 window: nothing clipped in the
      OCR rows, the result window or the translation overlay.
- [ ] M2 (landed): Save image and Copy image on both layers (the PNG at the capture's own
      resolution while the window shows it scaled), Edit in editor, zoom to 200% on a 4K capture
      with a click hitting the right line, Ctrl+wheel zoom/text size, drag to pan, the split
      ratio surviving a restart, and an RTL target rendering right to left in its covers.
- [x] M3 (landed, verified 2026-11-13 on Ubuntu X11; a Wayland run is still open):
      select lines in the pane and Translate selection translates only them (the button
      says so), the answer streams in line by line and Cancel stops it mid-stream, a long
      line split at a sentence boundary still lands in its one cover, a failing chunk
      leaves the finished lines with the failed range named, and "Translating… 2/3" with
      one dot per chunk is visible while it runs.
- [x] M4 (landed, verified 2026-11-13 on Ubuntu X11 GNOME; a Wayland run is still
      open): Test connection against a real endpoint, an export without secrets, the
      first-send host notice.
- [x] M4.5 (landed, verified 2026-11-13 on Ubuntu X11 GNOME; a Wayland run is still
      open): Ctrl+click two non-adjacent lines and Copy selection takes both,
      double-click a word and Translate sends only that word (shown in the pane, no cover
      over its line), a whole-line selection still paints covers, and Esc/Ctrl+A clear or
      extend the set.
- [ ] M4.6 (landed 2026-11-13, desktop pass still open): double-click a word and
      Translate paints a readable answer over those glyphs and puts a long one in a
      callout anchored to them; a selection crossing a line boundary uses one callout;
      Save image and Copy image contain no word cover or callout while whole-line covers
      are still saved; the partial note is one line, a Chinese cover reads at the
      page's size, and a cover stops before the word or label to its left; zoom, pan,
      Original/Translation and a new result leave no stale overlay.

### Desktop, X11 session (best-effort second target)

- [x] Region OCR and the translation overlay behave as on Wayland (verified
      2026-11-13 on Ubuntu X11, including the M3 selection/streaming flow).
- [ ] No OCR row is greyed out that was not greyed out on Wayland.

### Windows 11, real machine (T6.3, nobody has run it yet)

- [ ] Build with MSYS2 UCRT64 (commands in PR #6); `ctest` runs the 27 tests.
- [ ] Screenshots, editor, OCR result window and translation work on Tesseract.
- [ ] Settings > General > Text recognition: the OCR rows are greyed out with the
      Linux-only explanation (`PaddleOcr::available()` false).
- [ ] `bin\unisic.exe` did not grow by 6.3 MB and `strings` finds no
      `PP-OCRv6_det_tiny` (Windows embeds neither the model nor ONNX Runtime).
- [ ] Remove background is absent from the editor's More menu.

### Flatpak sandbox (T6.1)

- [ ] Install the bundle; OCR works with no download at all (runtime bundled, tiny
      embedded). Settings says `Tiny (6 MB): Included, in use`, no Download button.
- [ ] Download Small inside the sandbox (the finish-args allow network) and OCR Japanese.
- [ ] Remove background inside the sandbox (the 181 MB model download plus a run).

### AppImage and portable tarball

- [ ] On a desktop with no ONNX Runtime installed: Settings shows `ONNX Runtime 1.31.1`
      and `Tiny (6 MB): Included, in use`, and there is no Download button.
- [ ] Remove background asks only for the 181 MB model, never the runtime.
- [ ] Tarball: unpack and run `./AppRun`, same result.

### Native packages (deb, rpm, Arch)

- [ ] OCR works out of the box on Tesseract, and PaddleOCR offers Download (11 MB).
- [ ] After that download Tiny is in use; with Small downloaded, Japanese uses Paddle.
- [ ] rpm/Arch with the distro's own `onnxruntime` package: no download is offered.

### Release mechanics

- [ ] `UNISIC_VERSION_STRING` bumped, the CHANGELOG heading matches it, the metainfo
      `<release>` entry added, `follow-release.yml` bumps the site's `VERSION`, and the
      site's OCR copy is updated (T6.7) in the same release.
- [ ] `dist/SHA256SUMS` regenerated from the files the release actually publishes.
