# PinShot

**English** · [简体中文](README.zh-CN.md) · [Download](https://pin-shot.pages.dev) · [Releases](https://github.com/AFK-surf/pinshot/releases/latest)

Fast, PixPin-style screenshot, annotation, pinning, and screen recording for Windows and macOS. Written in [Pion](https://github.com/AFK-surf/pion) with [Invar](https://github.com/AFK-surf/invar): the UI and core logic are written once in `src/`, backed by a thin native layer in `native/` for Windows and macOS (zero local framework patches).

![PinShot](site/public/img/en-markup.jpg)

## Features

- **Instant capture**: smart window detection, area drag & edge/corner resize, pixel nudging, magnifier, and color picker (`C`); HDR-aware on Windows (WGC FP16 tone-mapped back to SDR on the GPU).
- **Markup & beautify**: shapes (rectangle, ellipse, focus spotlight), arrows & lines (`Shift` snaps to 0°/45°/90°), pen & eraser, redaction (pixel mosaic, brush mosaic, blur), numbered markers, text, and one-click **background beautify** (`B`, gradients / solid / desktop wallpaper with rounded corners & shadow).
- **Screen pins**: pin any capture or clipboard image (`Ctrl+2` / `⌃2`) as a floating window with zoom, opacity, rotation, flip, always-on-top, and OCR.
- **Offline Live Text OCR**: read text from any selection or pin (`Ctrl+O` / `⌘O`) on the device (Windows built-in OCR / Apple Vision), with in-place drag selection, double-click word, triple-click line, and `Ctrl+A/C` copy — no modal window.
- **Translate what OCR read**: press **Translate** in the pin's text bar (or the pin's right-click menu) and the translation is drawn over the original paragraphs, on a background taken from the picture; **Original** switches back and **Copy translation** copies it. English and other languages go to Chinese, Chinese to English. Services: Google's free web endpoint (no key, best effort) or your own key for DeepSeek or any OpenAI-compatible API (**Settings › Translate**). The text is sent only when you press Translate.
- **Scrolling screenshot**: capture tall pages or chats (`Ctrl+L` / `⌘L`) in either direction.
- **Area screen recording**: record any region (`Ctrl+R` / `⌘R`) at **30 / 60 / 90 / 120 fps** to hardware-accelerated **MP4 (H.265 / H.264, plus AV1 on Windows)** or **GIF**, with optional system audio, click/keycap overlay, and a compact post-recording Studio HUD for format, quality/FPS, and proportional scaling (`100% / 75% / 50%`).
- **Verified updates**: opt-in background updates on Windows signed with ECDSA P-256 and verified before download.

## Installation

### Windows

Requires Windows 10 (2004+) or Windows 11, x64.

- **Single-file self-installer**: download `pinshot.exe` from [pin-shot.pages.dev](https://pin-shot.pages.dev) (or [Releases](https://github.com/AFK-surf/pinshot/releases/latest)). Double-clicking it from **Downloads** automatically installs per-user into `%LOCALAPPDATA%\Programs\PinShot` (no administrator needed), adds a Start menu entry and a **Settings › Apps** uninstaller, and launches PinShot.
- **Portable**: move `pinshot.exe` to any folder of your choice (or pass `--portable`) to run in place without installing.

> The executable is not code-signed yet. If Windows SmartScreen warns *"Windows protected your PC"*, click **More info › Run anyway**.

Settings live in `%APPDATA%\PinShot\settings.json`; logs (including per-capture timings) live in `%APPDATA%\PinShot\pinshot.log`.

### macOS

Requires macOS 14+ on Apple silicon.

1. Download `PinShot-mac.dmg` from [pin-shot.pages.dev](https://pin-shot.pages.dev) (or [Releases](https://github.com/AFK-surf/pinshot/releases/latest)).
2. Open the DMG and drag `PinShot.app` to **Applications**, then open it.
3. On the first capture, grant permission in **System Settings › Privacy & Security › Screen & System Audio Recording**, then quit and reopen PinShot. If PinShot is not listed, click **+** and select `PinShot.app`.

Settings and logs live in `~/Library/Application Support/PinShot/`. Releases are signed with AFK AI's Developer ID and notarized by Apple, and update themselves like the Windows build (Settings › Automatic updates); an ad-hoc signed copy from before is replaced once by hand. Local builds are ad-hoc signed and do not update themselves; replacing the app with one may require re-granting screen recording permission.

## Usage & Shortcuts

### Capture & Markup

| Action | Windows | macOS |
| --- | --- | --- |
| Start capture | `Ctrl+Alt+A` (or click tray icon) | `⌃⌥A` (or click menu bar icon) |
| Pin clipboard image | `Ctrl+2` | `⌃2` |
| Select window / area | Click without moving / drag | Click without moving / drag |
| Nudge selection | Arrow keys (`1 px`), `Shift+Arrow` (`10 px`) | Arrow keys (`1 px`), `Shift+Arrow` (`10 px`) |
| Select whole screen | `Ctrl+A` | `⌘A` |
| Copy color under pointer | `C` | `C` |
| Toggle background beautify | `B` | `B` |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Y` (`Ctrl+Shift+Z`) | `⌘Z` / `⇧⌘Z` |
| Copy image | `Enter`, `Ctrl+C`, or double-click selection | `Enter`, `⌘C`, or double-click selection |
| Save (PNG / JPEG) | `Ctrl+S` | `⌘S` |
| Pin to screen | `Ctrl+T` | `⌘T` |
| Extract text (OCR) | `Ctrl+O` | `⌘O` |
| Scrolling screenshot | `Ctrl+L` | `⌘L` |
| Screen recording | `Ctrl+R` | `⌘R` |
| Clear selection / Quit | Right-click (twice to quit) or `Esc` | Right-click (twice to quit) or `Esc` |

### Pins

- **Move & resize**: drag to move; drag any edge or corner to resize keeping aspect ratio (`Shift` to stretch freely); scroll wheel to zoom.
- **Opacity**: `Ctrl+wheel` to adjust transparency.
- **Close**: press `Esc` or choose Close from the context menu.
- **Context menu**: copy, save as, extract text, always on top, rotate 90° left/right (`Ctrl+Shift+R` / `Ctrl+R`), flip horizontally/vertically, reset (`Ctrl+0`), opacity, close all.

### Tray / Menu Bar & Command Line

- **Tray / menu bar**: capture, pin clipboard, show/hide all pins, settings, open screenshots folder, check for updates, quit.
- **Windows CLI**:
  - `pinshot.exe --install` — install to `%LOCALAPPDATA%\Programs\PinShot` (optional: `--silent`, `--autostart`, `--desktop`, `--no-launch`)
  - `pinshot.exe --uninstall` — remove installed copy, Start menu shortcut, autostart, and Settings › Apps entry (optional: `--silent`)
  - `pinshot.exe --portable` — run in place without self-installing
  - `pinshot.exe --settings` — open Settings
  - `pinshot.exe --quit` — exit the running instance
  - `pinshot.exe --bench` — benchmark capture & window enumeration timings
  - `pinshot.exe --ocr <image>` — print OCR text from an image file (on macOS: `PinShot.app/Contents/MacOS/PinShot --ocr <image>`)

### Updates (Windows)

**Settings › About** provides **Check for updates** and an **Automatic updates** toggle (off by default). When enabled, PinShot checks [`latest.json`](https://pin-shot.pages.dev/dl/latest.json) once a day, verifies the **ECDSA P-256** release signature against the built-in public key, downloads and verifies SHA-256 in the background, and installs once idle (no capture, no open pins, 2 minutes idle). Failed updates leave the current version intact. The Microsoft Store build is updated exclusively by the Store.

## How It Works

- **Single resident process**: the frozen-screen overlay is Invar's main window (kept hidden between captures); pins, settings, and the recording HUD are child windows in the same process.
- **Low-latency capture**: on Windows, each monitor keeps a `Windows.Graphics.Capture` session holding only its latest frame, copied to a staging texture in a few milliseconds on hotkey press. Frames older than 5 s trigger a fresh session; sessions also rebuild on monitor wake, resume, and unlock. HDR monitors capture in FP16 and tone-map by the OS SDR white level on the GPU. GDI `BitBlt` is the automatic fallback (`PINSHOT_CAPTURE=gdi` forces GDI).
- **GPU-composed export**: annotations are stored in monitor pixels and rendered identically in the overlay and GPU export (`ui::take_picture`) at native resolution.
- **Upstream Invar host fast-paths**: uses Invar's native raw-pixel (`MBRAWPTR`) texture upload and `mb_render_set_atlas_size(1024)` / `mb_render_set_video_support(0)` (zero local patches).

## Build from Source

### Windows

Requires Visual Studio 2022 (C++ Desktop workload), `clang`/`clang++` (LLVM or llvm-mingw), [Pion](https://github.com/AFK-surf/pion) (`pionc` + `qbe`), and sibling checkouts of [Invar](https://github.com/AFK-surf/invar) (`../invar`) and [pion-packages](https://github.com/AFK-surf/pion-packages) (`../pion-packages`).

```powershell
scripts/build.ps1                # out/Release/pinshot.exe (self-installing single file)
scripts/build.ps1 -Test          # build + run Pion unit tests
scripts/install.ps1              # installs to %LOCALAPPDATA%\Programs\PinShot and launches it
packaging/pack-msix.ps1          # Microsoft Store MSIX package
```

### macOS

Requires Xcode Command Line Tools and [Pion](https://github.com/AFK-surf/pion) (`pionc`).

```bash
pionc build --release . -o out/Release/PinShot
```

A pushed `v<version>` tag makes the release (`.github/workflows/release.yml`): the **Windows package** and **macOS package** workflows build `pinshot.exe`, `PinShot-mac.dmg` (drag-to-Applications installer), and `PinShot-mac.zip` (in-app auto-update archive) from the tag and put them on its GitHub release; then, once approved in the `release` environment (which alone holds the update-signing key and the Cloudflare token), `packaging/publish.ps1` signs `latest.json`, puts the downloads in R2 and deploys the site. The **macOS package** workflow builds `PinShot-mac.dmg` and `PinShot-mac.zip`, signed with AFK AI's Developer ID and notarized (`scripts/sign-mac.sh`, from the `MACOS_DEVELOPER_ID_KEYCHAIN_B64_1` / `_2` and `KEYCHAIN_PASSWORD_AFK` secrets); both package workflows also run on demand.

## Sponsor

PinShot is free and open source. If you find it useful, you can support development on the [sponsor page](https://pin-shot.pages.dev/support) (WeChat Reward / Ko-fi), also linked under **Settings › About**.

## License

[MIT](LICENSE). The shutter sound (`assets/shutter.wav`) was generated with ElevenLabs.
