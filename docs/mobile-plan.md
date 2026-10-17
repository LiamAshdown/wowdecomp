# Mobile plan: the browser client on phones and tablets

Goal: the existing browser build (`docs/browser-port-plan.md`) plays on Android Chrome and iOS Safari with touch
controls that are part of the game's own UI. They use the client's own frames, textures, fonts and templates, so a
player sees WoW buttons, not a web overlay. The only DOM element added is a hidden text field for the on-screen
keyboard.

Sizes as in the port plan: S is days, M is about a week, L is weeks.

## Architecture

```
 main thread (DOM)                         game pthread
 ┌────────────────────────────┐            ┌──────────────────────────────────────────┐
 │ touchstart/move/end/cancel │            │ OsInputGet: gesture events → mouse       │
 │  ├ finger in a zone? ──────┼─ atomics ─▶│   (knows the frame under the cursor)     │
 │  │  stick / button zones   │            │                                          │
 │  └ else: gesture recognizer┼─ queue ───▶│ WebTouch.lua (embedded, secure)          │
 │ hidden <input> (keyboard) ─┼─ OS_CHAR ─▶│  stick knob, MoveForwardStart/Strafe..., │
 └────────────────────────────┘            │  action bar layout, zone rects → C++     │
              ▲  OsIMEEnable(1) focuses it  └──────────────────────────────────────────┘
```

Two rules hold this together:

1. **Lua owns the look, C++ owns the fingers.** The HUD is Lua that builds frames out of the stock templates
   (`ActionButtonTemplate`, the minimap and quickslot art) and reports each touch frame's screen rect to C++.
   C++ routes every finger: one that starts in a zone belongs to that zone, and any other finger drives the one game
   cursor.
2. **Everything that isn't the HUD stays a mouse UI.** Bags, quest dialogs, the spellbook, vendors and glue
   screens get taps and drags translated into mouse events. Nothing in FrameXML has to change for them to work.

## Key decisions

| Topic | Decision | Rejected |
|---|---|---|
| Controls | WoW frames built by embedded Lua, run from C++ right after `FrameXML_CreateFrames` (`Ui/GameUI.cpp:1771`), the same way `WebAccount.cpp` adds its glue UI. Code run from there is secure, so it can call protected movement functions (`MoveForwardStart`, `StrafeLeftStart`, `JumpOrAscendStart`, `ActionButtonN:Click()`). | A DOM/HTML overlay (looks foreign, can't show cooldowns or range). A downloadable addon (tainted, so it can't call the protected movement API). |
| Finger routing | Zones: Lua calls `WebTouchSetZone(id, kind, l, b, r, t)` whenever its layout changes. The main-thread touch handler hit-tests those rects and needs no game state. | Hit-testing the frame tree from the main thread, which isn't safe because the frame tree lives on the game thread. |
| Movement | 8-way stick mapped onto the existing digital bits (forward/back plus strafe). A small deflection means walk. Camera drags are right-button drags, so the character faces the camera and the stick stays relative to the camera. | Analog facing (`SetFacing` plus heartbeat packets): more code and a server-side risk. Revisit if 8-way feels bad. |
| Taps outside zones | Tap = left click. Hold for 450 ms = right click, with the tooltip visible during the hold (use bag items, loot, sell, cancel buffs). Drag on the world = right drag (camera). Drag on a frame = left drag (pick up, move windows), or wheel steps if the frame doesn't move. Pinch = wheel (camera zoom). | Separate on-screen "right-click mode" toggles. |
| Deciding a drag | The main thread only queues "drag started at x,y". `OsInputGet` on the game thread turns it into a left or right drag. By then the cursor-move to that point has been processed, so the mouse-focus frame says whether the finger is on `WorldFrame`. | Guessing from zone rects alone. |
| Keyboard | `OsIMEEnable(1/0)` (called from `CSimpleEditBox::OnFocusChange`, `Frame/CSimpleEditBox.cpp:264`) focuses or blurs a hidden `<input>`. Its `input`/`beforeinput` events become `OS_INPUT_CHAR`, `KEY_BACKSPACE` and `KEY_ENTER`. `OsInputEnableSecureMode` switches the field to `type=password`. | An in-game keyboard drawn with WoW art (slow to type on, no autocorrect or IME). |
| UI size | `uiScale` set so that one UI unit is about one CSS pixel (UI height = window CSS height, clamped to 384–768). Values above 1.0 already go through `CVarHandler_uiScale` (`Ui/GameUI_CVar.cpp:389`); the 0.64–1 limit is only the options slider. Large panels are scaled to fit the height when shown (hook `UpdateUIPanelPositions`). | A separate "mobile FrameXML". |
| Mode switch | CVar `webTouch` (default from `(pointer: coarse)`), plus a checkbox in Interface Options. The HUD only exists while it's 1. Touchscreen laptops can turn it off. | Auto-switching on every mouse move. |

## Milestones

### Testing on a phone on the local network
Phones only allow the shared memory wasm threads use on a secure origin, so plain `http://<PC address>` fails. The
bridge serves HTTPS with a self-signed certificate for the PC's address (`bridge-lan` in `.claude/launch.json`, port 8443).
The phone asks once to accept the certificate. Make the certificate (again whenever the PC's address changes) with:

    printf '[req]\ndistinguished_name=dn\n[dn]\n' > /tmp/req.cnf
    openssl req -config /tmp/req.cnf -x509 -newkey rsa:2048 -nodes -days 825 -subj "//CN=wowdecomp LAN" \
      -addext "subjectAltName=IP:<PC address>,IP:127.0.0.1,DNS:localhost" \
      -keyout Tools/Bridge/tls/lan.key -out Tools/Bridge/tls/lan.crt
    node Tools/Bridge/bridge.mjs --host 0.0.0.0 --port 8443 --tls-cert Tools/Bridge/tls/lan.crt --tls-key Tools/Bridge/tls/lan.key

`Tools/Bridge/tls/` is gitignored. Windows asks once whether node may accept connections on private networks.

### T0: Prove a phone can run it at all (S–M), do this first
Status: code done 2026-10-17, phone test pending. The heap starts at 384 MB and grows (553 MB in Coldridge). On a touch
screen the page caps it at 2 GB (pre.js halves a refused maximum), uses a 64 MB block cache, and renders at most 1.5
pixels per CSS pixel. `?mem=`, `?cache=` and `?scale=` override those, and `?stats` shows fps, heap and buffer size.
Nothing else matters if iOS can't allocate the memory. Test on one mid-range Android phone (Chrome) and one iPhone (Safari),
with a Bluetooth mouse and keyboard, so input isn't the variable.

1. Memory: `-sINITIAL_MEMORY=1GB -sMAXIMUM_MEMORY=4GB` (`WoW/Source/CMakeLists.txt:46`) is likely more than iOS gives
   one tab, and shared memory reserves address space for its maximum. Drop `INITIAL_MEMORY` to about 384 MB for every build,
   since growth already exists. Let the page pass a smaller `Module.wasmMemory` (maximum 2 GB) on mobile. Check that
   the generated `Wow.js` takes it under pthreads; if it doesn't, make a second link with a lower maximum.
2. Block cache: `kMaxBlocks = 256` (`WebVfs.cpp:119`) is 256 MB. Use 64 MB on mobile, set from the page.
3. Textures: phones have no `WEBGL_compressed_texture_s3tc`. The caps already report DXT as unsupported
   (`CGxDeviceOpenGl.cpp:657`), so the client's own uncompressed path runs at 4–8× the memory. Measure it before
   optimizing.
4. Render scale: the drawing buffer is CSS size × devicePixelRatio (`CGxDeviceOpenGlWeb.cpp:31`, and
   `ClientToCanvas` in `Os/Web/Input.cpp:156`). On a DPR-3 phone that's about 2500×1170. Add one render-scale factor
   used in both places (1.0–1.5 on mobile).

Done when both phones reach character select and stand in Shadowglen at 30 fps or more for 10 minutes without the tab
being killed. Write down peak memory and fps for each phone.

### T1: Touch as a mouse, plus the page shell (M)
1. `Os/Web/Input.cpp`: `emscripten_set_touch{start,move,end,cancel}_callback_on_thread` on `#canvas`, with the
   gesture recognizer from the table above. One "cursor finger" at a time. A new cursor finger during a camera drag
   ends the drag first.
2. The drag-start decision in `OsInputGet` (game thread). For the `WorldFrame` check, the Engine needs a small hook
   that WoW sets (a function pointer, like `OsInputSetEventPollProc`), because Os can't include WoW headers.
3. `Tools/Bridge/web/index.html`:
   - `touch-action: none`, `user-select: none` and `-webkit-touch-callout: none` on the canvas, and no `contextmenu`.
   - Viewport `viewport-fit=cover, user-scalable=no`, with the safe-area insets passed to the game (`WebTouchInsets()`) so
     the HUD avoids the notch.
   - A "rotate your device" card in portrait.
   - Fullscreen, then `screen.orientation.lock('landscape')` on Android.
   - `navigator.wakeLock` while in game.
4. PWA manifest (`display: fullscreen`, `orientation: landscape`). It's the only way to get a full screen on iPhone
   Safari (Add to Home Screen).

Done when, with a Bluetooth keyboard, you can play the glue screens and the game by touch alone: log in, create a
character, open bags, take a quest, loot, and drag a spell onto the bar.

### T2: On-screen keyboard (S–M)
1. `Os/Web/OsIME.cpp`: `OsIMEEnable` focuses or blurs the hidden input (`autocapitalize=off`, `autocomplete=off`,
   `enterkeyhint=send`). Feed text from `input` events, and Backspace and Enter from `beforeinput`/`keydown`.
2. iOS only shows the keyboard when focus happens inside a user gesture, and the edit box gains focus a frame later on
   the game thread. Fallback: when focus arrives outside a gesture, show a small WoW-styled "Tap to type" bar. Tapping it is
   a real gesture, so the keyboard opens. Upgrade path if the extra tap annoys people: the game thread publishes the rects of
   shown edit boxes, and `touchend` focuses the input on the spot.
3. When the keyboard opens, `visualViewport` shrinks. Shift the chat frame up instead of resizing the canvas, so there's no
   re-layout.

Done when you can log in, whisper, and type `/reload` with no hardware keyboard, on both phones.

### T3: Touch HUD (L)
Status: done 2026-10-17, checked in the browser pane with synthesized touches at a landscape size. Not yet tried on a
real phone. Works:
- `Os/Web/Input.cpp`: zones, the stick, tap = left click, a 450 ms hold = right click, drag = left or right drag (decided
  on the game thread from the mouse focus), and pinch, which zooms the camera directly.
- `Ui/Web/WebTouch.cpp`: the HUD (stick, action arc with the bonus bar, row, thumb buttons, page button, menu with the
  micro buttons and bags), `uiScale` from the CSS height, panels at the top fitted to the height, chat above the stick,
  keybinding labels and tutorials off.
- The `webTouch` CVar defaults to the page's `(pointer: coarse)`, and `?touch=1` overrides it. Turn it off with
  `/console webTouch 0` (there is no options checkbox yet).

Still open: the Interface Options panel, collapsed chat, hold feedback, and checking the stance and pet bars on a class
that has them.
New `WoW/Source/Ui/Web/WebTouch.cpp` with embedded `WebTouch.lua`, run after FrameXML loads. Every piece is built from stock
art and templates, except the stick and the icons the game has no equivalent for (jump, target cycle, interact, auto-run, menu,
bar page, sit, and the rotate card). `python Tools/Bridge/make-touch-art.py <game dir>/Interface/TouchUI` draws those as TGAs, the
same way `make-logo.py` makes the login title; restart the bridge after running it.

- **Stick** (bottom left): a ring from the minimap border art and a knob from the quickslot art. `OnUpdate` reads
  `WebTouchGetStick()` (x, y from C++ atomics), moves the knob, and starts or stops the movement bits only when the
  octant changes.
- **Action bar** (bottom right, in an arc around the thumb): the stock `ActionButton1`–`12`, re-anchored and scaled to
  about 44 CSS px. Cooldowns, range tint, macros, drag-and-drop and paging keep working because they're the real
  buttons. Each one is a button zone, so tapping a skill never fights a camera drag. A page button switches to
  `MultiBarBottomLeft` for 12 more.
- **Thumb buttons**: jump, target nearest enemy, interact with target, auto-run, sit or walk.
- **Menu button** (top): a grid of the micro-menu buttons (bags, character, spellbook, talents, quests, map, social, LFG,
  game menu) using the stock `MicroButton` art. Hide the stock `MainMenuBar` art in touch mode.
- **Chat**: collapsed to the last three lines and expanded by a tap. The edit box opens the keyboard (T2).
- **Scale**: `uiScale` from the window height (decision table), and panels fitted to the height on show.
- Re-send the zone rects to C++ on `OnSizeChanged`, `UI_SCALE_CHANGED` and `DISPLAY_SIZE_CHANGED`.
- Keep all state in locals so addons can't taint it.

Done when you can quest from level 1 to 10 and finish a dungeon run (Ragefire or Deadmines) on a phone with nothing plugged in.

### T4: Mobile defaults and performance (M)
1. On the first run in touch mode, write a mobile graphics preset:
   - `farclip` about 350 and `maxFPS 30`
   - low ground clutter, particles and shadows, no full-screen glow
   - `baseMip 1`, which cuts texture memory by 4× and offsets the missing DXT
2. If T0 shows textures are what runs out of memory, decode DXT to 16-bit (`DxtMakeTable<CArgb4444>`/`CRgb565`
   already exist in `Gx/Blit.h`) instead of 32-bit. That halves it again. A DXT→ETC2 transcode (ETC2 is always available in
   WebGL2 on mobile) keeps 4 bpp, but it's the expensive option, so leave it until 16-bit isn't enough.
3. `visibilitychange`: suspend audio. iOS drops the WebSocket after about 30 s in the background, so show the
   stock disconnect dialog and keep the account name for a quick re-login.

Done when the T0 phones hold 30 fps in Orgrimmar and Dalaran with no out-of-memory reload over 30 minutes.

### T5: Polish (S each, any order)
- Hold feedback: a cooldown swirl (`CooldownFrameTemplate`) under the finger, so a right-click hold is visible.
- `navigator.vibrate(10)` on skill taps (Android).
- Target cycling by swiping across the target frame.
- Size options for the stick and button arc, and left-handed mode, in Interface Options.
- Bluetooth gamepads: `Os/Web/Joystick.cpp` is a stub. The Gamepad API maps onto the same stick and button zones.

## Top risks
1. **iOS memory.** The tab gets killed with no error. T0 is a gate: if 1–1.5 GB doesn't hold even with `baseMip 1`,
   iPhone is out, and the realistic target is Android plus iPad.
2. **Thermals.** Phones throttle after 10–15 minutes at full GPU load. Hence `maxFPS 30` and the render scale.
3. **Taint.** The HUD calls protected functions, which is only safe while its code stays secure. No globals, and no
   hooks into addon-reachable tables.
4. **One cursor.** WoW has one mouse, so two cursor fingers can't both act. Zones cover the cases that matter in
   combat, and the rest are menus, where one finger is enough.

## Not doing
- A native app shell (Capacitor or a WebView wrapper): a mobile browser gets the same result. Revisit only for App Store
  distribution.
- Portrait layout: the UI and the 3D camera assume landscape.
