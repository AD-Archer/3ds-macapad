# 3DS Macapad docs

- [What it shows](#what-it-shows)
- [Requirements](#requirements)
- [Install](#install)
- [Using it](#using-it)
- [Buttons](#buttons)
- [Files on the SD card](#files-on-the-sd-card)
- [Building](#building)
- [Releases](#releases)
- [Code](#code)
- [Emulator](#emulator)

## What it shows

- **Top screen:** the time (from the Mac's clock), the weather where the Mac is, what's playing on the Mac (any app)
  with artwork and progress, and the Mac's CPU, memory, battery and disk.
- **Bottom screen:** a 4×3 pad of keys, in pages. A key can run a Mac action, open an app, open a website, press a
  keyboard shortcut or run a Shortcut.

## Requirements

- [Macapad](https://ad-archer.github.io/Macapad/) 0.4 or later on your Mac (it needs `GET /api/status`). The 3DS's
  "Find your Mac" screen shows a QR code for that page too.
- The 3DS and the Mac on the same Wi-Fi. A 3DS can't join a tailnet.

## Install

In FBI on the 3DS, choose **Remote Install → Scan QR Code** and scan this:

<img src="../cia/qr.png" width="160" alt="QR code for the newest macapad.cia">

It always installs the newest release (`releases/latest/download/macapad.cia`), so scan it again to update. FBI
downloads without signing in to GitHub, so the repository has to stay public for this to work.

You can also grab `macapad.cia` or `macapad.3dsx` from the [releases](https://github.com/AD-Archer/3ds-macapad/releases)
page: install the CIA with FBI for a HOME Menu icon, or put the 3DSX in `sd:/3ds/` and open it from the Homebrew
Launcher.

## Using it

1. **Find your Mac.** The 3DS knocks on port 7317 of every address on your Wi-Fi and lists the Macs that answer. If
   yours doesn't show, tap **Type address** and enter its IP (`192.168.1.20`, or `192.168.1.20:7399` for another port).
2. **Pair.** Your Mac shows an Allow / Don't Allow panel with a 4-digit code. Check it matches the 3DS and click
   Allow. The 3DS shows up in Macapad's Settings as "&lt;console name&gt;'s 3DS".

   **Or scan a code:** on the Mac, choose **Pair with QR Code…** from Macapad's menu bar icon. On the 3DS, tap
   **Scan code** and point the outer camera at it from about a foot away. It pairs right away with nothing to approve
   on the Mac, and works even when the scan can't find the Mac.
3. **Press keys.** Tap a key to run it. Swipe the pad left or right, or tap **‹** / **›** in its corners, to change
   pages; tap the page's name for the menu. Tap an empty slot, or hold any key, to change it.

   The search field at the top of "New key" (or the magnifier in the title bar, or **Y**) searches Mac actions, apps
   and Shortcuts at once. Each picker has the same field for searching just itself.

Taps go to the Mac in order. Any made while one is still on its way are sent together in the next request, so a quick
run of taps doesn't wait a round trip each. If the Mac's address changes, the 3DS looks for the same Mac on the network
again after a few failed polls.

## Buttons

| Button | Does |
|---|---|
| L / R | previous / next page (or swipe, or tap ‹ ›) |
| X | play / pause |
| D-pad ← → | previous / next track |
| D-pad ↑ ↓ | volume |
| Y | mute |
| SELECT | edit mode (tap any key to change it) |
| START | menu: pages (jump to, add, rename, remove), edit keys, 12/24-hour clock, forget this Mac, quit |

## Files on the SD card

Everything lives in `sd:/3ds/macapad/`:

| File | Holds |
|---|---|
| `config.json` | the Mac's address, this 3DS's device token, the clock setting |
| `pads.json` | your pages and keys |
| `icons/<bundleId>.png` | cached app icons |

Each key in `pads.json` is `{kind, label, ref, category?, icon?}`:

| `kind` | `ref` is |
|---|---|
| `action` | a Macapad action id |
| `app` | a bundle id |
| `web` | a URL |
| `keys` | a key combo, like `cmd+shift+t` |
| `shortcut` | a Shortcut's name |

`icon` is the Material Symbols name the Mac suggests for an action. The 3DS draws it when it has a matching glyph;
built-in actions are drawn by id.

## Building

The build runs in devkitPro's Docker image, so the only thing to install is Docker Desktop (keep it running). `just`
lists the tasks:

| Command | Does |
|---|---|
| `just build` | builds `macapad.3dsx`, always clean (the Docker VM's clock drifts, so make's timestamps can't be trusted) |
| `just run` | builds and opens it in the Azahar emulator |
| `just send 10.0.0.42` | builds and sends it to the 3DS over Wi-Fi (open the Homebrew Launcher and press **Y** first) |
| `just sd /Volumes/NINTENDO` | builds, copies it to `3ds/` on a mounted SD card and ejects it |
| `just cia` | builds `macapad.cia`, which installs to the HOME Menu with FBI |
| `just clean` | removes build output |

`cia/` holds what the CIA needs on top of the 3DSX build:

- `macapad.rsf`: title id `000400000F731700` and the services the app may use
- `banner.png` (256×128) and `Single-Click.wav`: the HOME Menu banner and its sound
- `build-cia.sh`: fetches pinned makerom and bannertool builds and packs `macapad.elf` + `macapad.smdh`

## Releases

`.github/workflows/build.yml` builds the 3DSX and CIA on every push and pull request; both are in each run's
**Artifacts**.

Every push to `main` is also released as the next version, which the workflow tags itself: `v1.0.0`, `v1.0.1`, and so
on. The CIA's version only has room for 0–15 in the last number, so after `v1.0.15` comes `v1.1.0`. Put `[minor]` or
`[major]` in a commit message to bump those instead.

The newest release is GitHub's "latest", which is what the install QR code points at. Each release also carries the
QR code as `macapad-qr.png`. The same image is kept at `cia/qr.png` for the README: the release job regenerates it
and commits it if it ever changes (say, after renaming the repo).

## Code

| File | What |
|---|---|
| `source/main.c` | screens: find, pair, dashboard + pad (with page swipes), key editor, grouped lists, menu |
| `source/macapad.c` | config and pads on the SD card, status / press / icon threads (presses batched, connections kept open), scanning, pairing, catalog |
| `source/http.c` | a small HTTP/1.1 client on the 3DS socket API, with keep-alive and Content-Length framing, plus the subnet scan |
| `source/gfx.c` | rounded rects, circles, outlines, text that fits, the icon set, PNG → texture, camera preview |
| `source/qrscan.c` | the outer camera at 640×480: a capture thread and a QR-decoding thread, for pairing |
| `source/quirc/` | [quirc](https://github.com/dlbeer/quirc) v1.2, the QR decoder (ISC license, see its `LICENSE`) |

Circles are drawn as triangle fans rather than with citro2d's ellipse calls, which Azahar doesn't draw.

## Emulator

[Azahar](https://github.com/azahar-emu/azahar) runs it (`Azahar.app/Contents/MacOS/azahar macapad.3dsx`, or
`just run`), and its network calls reach the Mac's localhost. Its stand-in system font spaces letters widely, which is
why the screenshot in the README looks that way; a real 3DS draws text normally.
