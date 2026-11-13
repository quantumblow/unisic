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
  each line, Cancel, error line, Esc to close.
- Translation: any OpenAI-compatible `/chat/completions`, numbered-line prompts,
  one request per 40 lines, tolerant parsing, whole-text fallback, unchanged-answer
  detection, per-tier `MemAvailable` guard.
- Verified: 27/27 `ctest`, F8 steps `ocr result` / `translation` /
  `translation overlay` / `paddleocr` all PASS with models present, tiers measured
  (tiny 0.09-0.13 s and ~0.4 GB, small 0.20-0.27 s and ~0.6 GB, medium 0.52-0.68 s
  and ~1.2 GB per 1600x1000 page), tier switch reloads without a restart.

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

## M2. Result window: save, edit, zoom (3-4 days)

**T2.1** Move the composite to C++: `OcrResultController::compositedImage(bool
translated)` draws the covers and the fitted text at source resolution. The font
fitting currently lives in QML (`Text.Fit`, 5 px floor, wrap); factor the fitted
pixel size into the C++ model and have QML use it, so the saved picture and the
screen cannot diverge.

- Acceptance: a golden-image test on a 4-line capture compares the composite with
  the QML overlay's geometry (`OcrResultTest`), and the saved PNG is at source
  resolution even when the window had to scale the capture down.

**T2.2** Save image… and Copy image, for whichever layer is shown (Original or
Translation). Follow the existing save-dialog pattern in `qml/pages/*`; PNG by
default, with the same filename template the capture save uses.

**T2.3** Edit in editor: hand the composited image to `EditorSession` and open the
editor, so a translated screenshot can be annotated, cropped and printed like any
capture. Reuses the existing open-with-image path.

**T2.4** Zoom and pan in the picture pane: zoom in / 100% / zoom out / fit
buttons, Ctrl+wheel, drag to pan while zoomed. Line boxes and the text overlay
transform with the picture, and hit testing stays exact at every zoom level.

- Acceptance: 100%, 125% and 150% display scale, a 4K capture, boxes aligned, and
  a click still selects the right line when zoomed to 200%.

**T2.5** Split ratio: a draggable divider between picture and text pane, with the
ratio remembered; Ctrl+wheel over the text pane changes its font size.

**T2.6** Overlay edges: cap the horizontal 30% expansion at the neighbour box so a
multi-column page stops grazing the next column; when a translation cannot fit at
the 5 px floor, say so (an ellipsis or a small marker) instead of clipping
silently; check an RTL target (Arabic/Hebrew) renders right to left in the cover.

## M3. Translation: selection, streaming, protection, retries (3-5 days)

**T3.1** Translate only the selection: when the text pane has a selection,
Translate works on those lines and the overlay covers exactly those lines; with no
selection it keeps translating the whole text. The button label says which one it
will do.

**T3.2** Streaming: `stream: true` against OpenAI-compatible endpoints, parse the
SSE `data:` frames, append into the pane as they arrive, keep Cancel working
mid-stream, and fall back to the buffered request when an endpoint ignores or
rejects streaming. Extend the mock endpoint with an SSE mode for the tests.

**T3.3** Context protection: estimate tokens (about 4 characters per token for
Latin, about 1 per Han character), cap a request at a safe budget, split a very
long line at sentence boundaries, and tell the user when a line was split instead
of silently truncating.

**T3.4** Chunk retry and partial results: one retry with a short backoff per chunk
on a transport error or 5xx; when a chunk still fails, keep the finished chunks,
show what did translate, and name the part that did not, instead of dropping the
whole translation.

**T3.5** Progress: "Translating… 2/3" while chunks are in flight, and per-chunk
state in the pane when streaming and chunks are combined.

## M4. Settings, privacy, secrets (1-2 days)

**T4.1** Test connection in Settings > General > Translation: one small request
with the configured endpoint, key, model and target, a 10 s timeout, then success
with the round-trip time and the model's answer, or the endpoint's error verbatim.

**T4.2** Settings export: secrets (upload credentials and the translation key) are
out by default, with an explicit include option in the export dialog; importing a
file without them must not wipe the ones already configured.

**T4.3** First-send privacy notice: the first time Translate would send text to a
non-localhost endpoint, say once which host the text goes to and remember the
answer. Local addresses (127.0.0.1, localhost, ::1) never ask.

**T4.4, D2 settled (2026-11-13): no keychain.** The key stays in the owner-only
config file, documented as such; T4.2 is what keeps it out of exports. A keychain
would mean libsecret as a new mandatory dependency in six packaging recipes, which
the no-optional-dependencies rule does not allow for one string.

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

**T6.2** Debian: rebuild the full package on `debian:trixie` (the last
`unisic-deb-build` attempt exited 1 and was never re-run).

**T6.3** Windows: one real-machine pass (Tesseract path, translation window,
`PaddleOcr::available()` false). CI compiles it; nobody has run it.

**T6.4** Download resume: HTTP Range plus a `.part` file, so an interrupted small
or medium download does not start over.

**T6.5** Mirror: a second pinned base (or a documented mirror procedure) so a
ModelScope outage does not disable downloads; base URL, sizes and hashes change
together.

**T6.6** Tests: pull the tier memory table, the effective-tier rule and
`MemAvailable` parsing into pure functions and cover them in QtTest; extend F8 and
the Developer pane for every new path (save image, edit in editor, zoom, selection
translation, streaming, retry, test connection).

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
5. M6 tasks ride along with whichever milestone they belong to; the packaging ones
   (T6.1, T6.2, T6.3) close a round.
6. M5 last, on its own.

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
