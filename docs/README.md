# 3DS Macapad docs

- [What it shows](#what-it-shows)
- [Requirements](#requirements)
- [Install](#install)
- [Using it](#using-it)
- [Use as a controller](#use-as-a-controller)
- [Type on your Mac](#type-on-your-mac)
- [Keep Mac awake and Mouse jiggler](#keep-mac-awake-and-mouse-jiggler)
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
  keyboard shortcut or run a Shortcut. App keys show the app's icon; website keys show the site's icon when the Mac's
  Macapad is new enough to fetch them (it looks for the icons the site's page declares, then its favicon). Keys that
  were on the pad before get theirs too.

The song is polled every 1.5 seconds and is always current. Everything else is allowed to lag a little: the clock
offset, the weather and the Mac's numbers are kept on the SD card, so the dashboard is filled in from the first frame
and stays filled while the Mac is away (the song card says "Connecting…" until the Mac answers).

## Requirements

- [Macapad](https://ad-archer.github.io/Macapad/) 0.4 or later on your Mac (it needs `GET /api/status`). The 3DS's
  "Find your Mac" screen shows a QR code for that page too. The controller, keys that repeat while held, and the Keep
  Mac awake and Mouse jiggler switches need 1.0.
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

   Picking what a key does leads to **How it looks**: the key as it will show, with its name, its icon (automatic,
   or any of the drawn glyphs) and its color (automatic, or one of nine). The same screen opens from **This key** when
   you hold a key that's already set; **What it does** on it goes back to the first question.

   The search field at the top of "New key" (or the magnifier in the title bar, or **Y**) searches Mac actions, apps
   and Shortcuts at once. Each picker has the same field for searching just itself.

   The pickers open from a cache on the SD card, so there's no waiting after the first time. A thread keeps it fresh
   in the background: actions every 5 minutes, Shortcuts every 10, apps every 30. The last row of each picker (or
   **X**) asks the Mac again right away; **Refresh from your Mac** in the START menu does that for all three and
   fetches the pad's icons again too. Icons are also cached on the SD card and fetched again when a week old.

Taps go to the Mac in order. Any made while one is still on its way are sent together in the next request, so a quick
run of taps doesn't wait a round trip each. If the Mac's address changes, the 3DS looks for the same Mac on the network
again after a few failed polls.

## Use as a controller

**START → Use as a controller** turns the 3DS into a gamepad, mouse and keyboard for the Mac. Pick a preset, then
**Start**:

| Preset | Buttons | Sticks |
|---|---|---|
| **Mouse & keys** | A left click, B right click, X Esc, Y Return, L/R back/forward (Cmd+[ / Cmd+]), ZL/ZR previous/next tab, START Spotlight, SELECT Cmd+Tab, +Control Pad arrow keys | Circle Pad moves the pointer, C-Stick scrolls |
| **Retro games** | RetroArch's default keys: +Control Pad arrows, A X, B Z, X S, Y A, L Q, R W, START Return, SELECT Right Shift, ZL F1 (menu), ZR Space (fast-forward) | Circle Pad arrows |
| **PC games** | A Space (jump), B C (crouch), X R (reload), Y E (use), L right click (aim), R left click (shoot), ZL Shift (run), ZR Q, START Esc, SELECT Tab, +Control Pad 1–4 | Circle Pad W A S D, C-Stick mouse look |

In every preset the touch screen is a trackpad: drag to move the pointer (faster swipes go further), tap to click.
Drag up and down the strip at its right to scroll, and the two buttons under it are a left and right click (hold
them down, or hold a button that clicks, like A in Mouse & keys, while you drag to drag things).

The top screen lists what every button does, lights each one while it's held, and says whether it's getting through
to the Mac. **To stop, press START and SELECT together** (every other button is in use); tapping **‹ Done** works
too. START and SELECT wait a moment before they count, so pressing both to stop doesn't press either on the Mac.

**Change a button** sets any button to a key, keys held together (like Cmd+Tab), a left, right or middle click, or
nothing; a stick can move the pointer, scroll, or be arrow keys or W A S D. Changes are kept per preset; **Back to
preset** undoes them. ZL, ZR and the C-Stick only show on a New 3DS.

It works live: the 3DS asks the Mac for a session (`POST /api/input/session`) and then sends everything it holds over
UDP, the whole state each time, about 20 times a second while anything is held and twice a second otherwise. A lost
packet can't leave a key stuck, and the Mac lets go of everything if it stops hearing from the 3DS. The 3DS lets go
itself when you stop, close the lid or press HOME. If Macapad is paused or outside its hours, the screen says so and
tries again every few seconds.

## Type on your Mac

**START → Type on your Mac** opens the 3DS keyboard (**Type something…**) and types what you write on the Mac. Under
it are Esc, Tab, Delete, Return, Space and the arrows, which repeat while held like a real keyboard (so do the
+Control Pad's arrows). Tap Command, Shift, Option or Control first to add it to the next key, like Command then
Left. With a Mac older than 1.0 the keys still work, one press at a time.

## Keep Mac awake and Mouse jiggler

Two switches at the top of the START menu. **Keep Mac awake** stops the Mac's screen from sleeping; **Mouse jiggler**
nudges the pointer when the Mac has been idle for a while, so Slack and Teams never show you as away. They show what
the Mac says right now (and until when, if it was turned on for a while), so they stay right when they're changed on
the Mac or from another device. Keys for them, and for the new window sizes, Siri, brightness, accessibility and
clicks, are in "Pick a Mac action"; the Keep Mac awake and Mouse jiggler keys get a green dot while they're on.

## Buttons

| Button | Does |
|---|---|
| L / R | previous / next page (or swipe, or tap ‹ ›) |
| X | play / pause (in a picker or the menu: refresh from the Mac) |
| D-pad ← → | previous / next track |
| D-pad ↑ ↓ | volume |
| Y | mute |
| SELECT | edit mode (tap any key to change it) |
| START | menu: use as a controller, type on your Mac, keep Mac awake, mouse jiggler, pages (jump to, add, rename, remove), edit keys, 12/24-hour clock, forget this Mac, refresh, quit |

## Files on the SD card

Everything lives in `sd:/3ds/macapad/`:

| File | Holds |
|---|---|
| `config.json` | the Mac's address, this 3DS's device token, the clock setting |
| `pads.json` | your pages and keys |
| `controller.json` | the controller's preset, and the buttons you changed in each |
| `status.json` | the last clock offset, weather and stats, for the dashboard before the Mac answers (per Mac) |
| `catalog-actions.json`, `catalog-apps.json`, `catalog-shortcuts.json` | the pickers' lists, with when they were fetched (per Mac) |
| `icons/<bundleId>.png` | cached app icons |
| `icons/web/<host>.png` | cached website icons |

Forgetting a Mac removes its `status.json` and catalogs; the icons stay.

Each key in `pads.json` is `{kind, label, ref, category?, icon?}`:

| `kind` | `ref` is |
|---|---|
| `action` | a Macapad action id |
| `app` | a bundle id |
| `web` | a URL |
| `keys` | a key combo, like `cmd+shift+t` |
| `shortcut` | a Shortcut's name |

`icon` is the Material Symbols name the Mac suggests for an action. The 3DS draws it when it has a matching glyph;
built-in actions are drawn by id. Two more fields come from **How it looks**, both optional: `glyph`, the icon chosen
for the key (a name like `play_arrow` or `lock`; it wins over the automatic one), and `color` (`blue`, `green`,
`orange`, `red`, `purple`, `yellow`, `teal`, `pink` or `gray`).

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
| `source/main.c` | screens: find, pair, dashboard + pad (with page swipes), key editor, grouped lists, menu, controller, typing |
| `source/input.c` | live input: the session, and a thread that sends what's held and moved over UDP and reads the Mac's answers |
| `source/macapad.c` | config, pads and the controller presets on the SD card, status / press / icon / catalog threads (presses batched, connections kept open), the status snapshot, scanning, pairing, the catalog cache |
| `source/http.c` | a small HTTP/1.1 client on the 3DS socket API, with keep-alive and Content-Length framing, plus the subnet scan |
| `source/gfx.c` | rounded rects, circles, outlines, text that fits, the icon set, PNG → texture, camera preview |
| `source/qrscan.c` | the outer camera at 640×480: a capture thread and a QR-decoding thread, for pairing |
| `source/quirc/` | [quirc](https://github.com/dlbeer/quirc) v1.2, the QR decoder (ISC license, see its `LICENSE`) |

Circles are drawn as triangle fans rather than with citro2d's ellipse calls, which Azahar doesn't draw.

The worker threads run on the 3DS's second core when the system grants a share of it (`APT_SetAppCpuTimeLimit`), so
parsing answers and decoding images never cost the screen a frame; otherwise they run beside the main thread while it
waits for the display.

## Emulator

[Azahar](https://github.com/azahar-emu/azahar) runs it (`Azahar.app/Contents/MacOS/azahar macapad.3dsx`, or
`just run`), and its network calls reach the Mac's localhost. Its stand-in system font spaces letters widely, which is
why the screenshot in the README looks that way; a real 3DS draws text normally.
