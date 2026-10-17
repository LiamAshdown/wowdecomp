# Browser port plan: wowdecomp → WebAssembly + AzerothCore

Goal: run the 3.3.5a (12340) client in a desktop browser against a locally run AzerothCore server.
Account creation and management happen in the client's login screen, with no separate web app.

This plan is put together from five read-only investigations covering platform, graphics, networking, accounts, and build/data/audio.
Sizes are rough: S is days, M is about a week, L is weeks, XL is a month or more.

## Architecture

```
 Browser tab (COOP/COEP → SharedArrayBuffer)
 ┌──────────────────────────────────────────────────────────────┐
 │ main thread: DOM input, pointer lock, WebSocket objects       │
 │ game pthread (PROXY_TO_PTHREAD): Wow.wasm, existing loop,     │
 │   WebGL2 via OffscreenCanvas, sync XHR file reads             │
 │ worker pthreads: net, disk queue, M2 cache, FMOD mixer, ...   │
 └──────────────┬───────────────────────────────┬───────────────┘
        HTTP GET + Range                     WebSocket
                │                               │
 ┌──────────────▼───────────────────────────────▼───────────────┐
 │ bridge (one small Node process, tools/bridge/)               │
 │  /            static wasm/js + COOP/COEP headers             │
 │  /data/...    client Data/ with Range, case-insensitive,     │
 │               PUT overlay for WTF/Cache/Logs/Screenshots     │
 │  /tcp/ip/port WS↔TCP relay, allowlisted to AzerothCore only  │
 │  /account     register / change-password / info (JSON)       │
 └──────┬──────────────────────┬──────────────────┬─────────────┘
     TCP 3724               TCP 8085        MySQL (least-priv user)
   authserver             worldserver         acore_auth.account
        (AzerothCore via its docker compose)
```

Why one bridge and not websockify: the account endpoints need a server-side component anyway. The bridge also serves the data files and sends the COOP/COEP headers.
Doing it all in one process replaces three separate tools.
Realm-list addresses need no rewriting. The client gets `127.0.0.1:8085` from `realmlist`, and the web `WowConnection` connects to `ws://<page-host>/tcp/127.0.0.1/8085`.

## Key decisions

| Topic | Decision | Rejected |
|---|---|---|
| Platform seam | Add a `Web/` directory next to each `Win32/`/`Win64/` (33 modules). Start each file as a clone of its Win64 version. A web-only stand-in `windows.h` (`Engine/Source/Os/Web/compat/`) covers the shared headers that leak `<windows.h>`, so they need no edits. A force-included `msvc_crt.h` covers MSVC CRT names. | A Win32-API shim layer, because the API surface is too large. |
| Threads | pthreads with `-sPROXY_TO_PTHREAD`. The existing blocking loop, sync waits and sync file reads stay valid on the game pthread. | Single-thread with ASYNCIFY/JSPI, which bloats the binary and means reworking about 10 threads. |
| Window/input | `emscripten/html5.h` directly: DOM key/mouse → `OsQueuePut`, Pointer Lock for relative mouse. | SDL2, because we already have a native debug build (Windows) and SDL plus OffscreenCanvas plus pthreads is fiddly. |
| Renderer | New `CGxDeviceGles3` (WebGL2) forked from `CGxDeviceOpenGl`, using the free `EGxApi` slot. Bring-up uses `fixedFunction`, which makes every ShaderCreate a no-op, plus one uber-shader pair that emulates fixed-function. | LEGACY_GL_EMULATION (too slow, incomplete) and WebGPU (deferred). |
| Shaders (later) | Runtime ARB asm (`arbvp1`/`arbfp1` profiles in the MPQ `.bls` files) → GLSL ES 3.00 transpiler, wired into the unused "glsl" profile slot. | Translating DXBC, or hand-writing about 40 shaders and their permutations. |
| Game data | New `Blizzard::File` stack backend doing sync XHR + `Range` against the bridge, with an LRU block cache of about 256 MB. No copy or import of the roughly 17 GB of MPQs. | Preloading, an OPFS import (copies 17 GB), Emscripten lazy FS. |
| Sockets | Web `WowConnection` + `WowConnectionNet` on `emscripten/websocket.h` (about 15 Winsock call sites; `PlatformRun` becomes a callback/poll pump). | Emscripten BSD socket emulation (partial-read and SO_ERROR quirks) and PROXY_POSIX_SOCKETS. |
| Audio | Keep FMOD Ex (full source in tree). Add an `fmodex/emscripten` platform with an AudioWorklet output, so DSP and reverb stay identical. Use NOSOUND until it lands. | Rewriting SoundEngine (3.9k lines) on OpenAL/miniaudio. |
| Memory | wasm32 (ILP32 matches the original, so the Win32 layouts carry over). `-sINITIAL_MEMORY=1GB -sMAXIMUM_MEMORY=4GB -sALLOW_MEMORY_GROWTH`. | MEMORY64 (slower, uneven support). Revisit only if we hit the 4 GB ceiling. |
| Accounts | The bridge `/account` endpoint. The client computes the SRP6 salt and verifier, so the plaintext password never leaves the client. The bridge does a parameterized INSERT/UPDATE on `account` as a least-privilege DB user. | SOAP from the client (needs GM creds), a direct DB connection from the client, and a custom authserver opcode (forks AzerothCore). |
| Custom GlueXML | Loose `Interface\GlueXML` files under the web build. Skip `DeleteInterfaceFiles()` (`UIUtil/AddOns.cpp:151`, called at `CGlueMgr.cpp:725`) and build with `WOWDECOMP_DISABLE_SIGNATURE_CHECKS`. Fall back to a `patch-4.MPQ` if Storm won't prefer loose files. | Shipping an MPQ builder unless needed. |

## Milestones and work items

### M0: Server proven (S) — done 2026-10-17
Status: done. AzerothCore runs in Docker from `../azerothcore-wotlk`; its `.env` puts the DB on port 3307 and `docker-compose.override.yml` sets `AC_WARDEN_ENABLED=0`. `Tools/AzerothCore/create_account.py` writes SRP6 salt/verifier rows. `Tools/AzerothCore/login_probe.py` passes logon → realm list → world `AUTH_OK`. The native GUI login is not yet tested; the client's realmlist now points at 127.0.0.1, with `*.chromiecraft.bak` backups.

1. Clone azerothcore-wotlk `master` and run `docker compose up -d --build`. `ac-client-data-init` fetches the pre-extracted dbc/maps/vmaps/mmaps.
2. Set `Warden.Enabled = 0`. Without it the client gets kicked, because the client's Warden (`Game/GameClient/Win64/WardenClient.cpp`) never replies. Leave `StrictVersionCheck` off.
3. Set the `realmlist` table's `address` and `localAddress` to `127.0.0.1`, port 8085. Use IPs only; the client hand-parses the dotted quad.
4. Log in with the existing **native Windows** build (`SET realmList "127.0.0.1"`). This separates server config from port bugs.

### M1: It compiles and links for wasm (L–XL) — done 2026-10-17
Status: done. `emcmake cmake -S . -B build/web -G Ninja` builds `bin/Web/Wow.js` + `Wow.wasm`. A Node smoke variant (`-sNODERAWFS`, run from `bin/Win`) mounts all MPQs, then stops at graphics device creation, which is M3's job. `Web/` backends are mostly ports of the Win64 ones; input, audio output, sockets, Warden, LCD and movies are stubs for now.

1. **CMake (M):** replace the root `FATAL_ERROR "Windows only"` with a `WOWDECOMP_PLATFORM_WEB` path.
   - Under that path, filter `/Win(32|64)/` out of the `GLOB_RECURSE`s and do **not** define `WOWDECOMP_ARCH_32`, because its whole-file guards would pull in the x87 and asm sources.
   - Drop `.rc`, `/ENTRY:start`, `d3d9`, `ws2_32`, `wininet`, `dinput8`, `imm32` and `nvapi`.
   - Stub out lgLcd, SteelseriesMouse, DivxDecoder/Xvid, Battlenet, ComSat, ScanDLL and Warden.
2. **Third-party (S):** zlib, bzip2, lzma, expat, pcre, freetype (keep the in-tree version), lua and jpeg all build as-is. Replace the `__asm fld` at `lua-5.1/src/luaconf.h:562` with the portable `lua_number2int`.
3. **WinCompat.h + header leaks (S):** `SCritSect.h`, `Lock.h`, `Thread.h`, `GameUI.h`, `NetInternal.h`, etc.
4. **Core Web backends (L):** Storm (SThread, SLock, SCritSect, SReg → key/value file), BlizzardCore System (Thread, Lock, Time, Debug, Process, Hardware, Log, Unicode, Version), Os (Time, Tls, Path, SecureRandom, System).
   - Stub SErr/MachineState (minidumps) and Gui (commctrl dialogs).
   - `OsGuiMessageBox` → `console.error` plus a DOM overlay.
5. **Startup (S):** drop the 8 MB fiber in `WinMain.cpp` (`-sSTACK_SIZE` instead). Replace the about 250 ms busy-spin in `TimeManager::Calibrate` with `emscripten_get_now`.
6. **Verify ABI (S–M):** wasm `wchar_t` is 4 bytes (about 34 uses), `time_t` is 64-bit, and Itanium bitfield/padding rules apply. Add `static_assert(sizeof(...))` for the M2/WMO/ADT/DBC structs that overlay file bytes. There are none today; the Win32 build is the only reference.

Done when `Wow.wasm` reaches `EventDoMessageLoop()` with console logging and no window.

### M2: Game data over HTTP (L)
Status: done. `node Tools/Bridge/bridge.mjs` serves the page, a `game-manifest.txt` and the game files with Range support. `BlizzardCore/.../File/Web/WebVfs.cpp` reads them with synchronous XHR through a 256 MB block cache; writes go to MEMFS, and files under `WTF/`, `Cache/` and `Screenshots/` are uploaded to the bridge's user-data overlay on close (`PUT`/`DELETE /userdata/...`), so settings and keybindings survive a reload.
1. Bridge static and data serving: Range, case-insensitive lookup, `\`→`/`, PUT overlay for writes, COOP/COEP/CORP headers (S–M).
2. HTTP-range backend in `BlizzardCore/.../System/File/Web/Filestack.cpp`. It must honor the 4K-aligned unbuffered reads `IOFileUnit` issues (M).
3. Fixed `Data/` root in `Client.cpp` (`s_dataDirectories` around :208, registry install-path code around :603–842). Stub `Packages/Streaming` and the wininet patch download (S).

Done when the MPQs mount, DBCs load, and `Config.wtf` round-trips.

### M3: Login screen renders and takes input (XL)
Status: done, with a different design: there is no new device. `CGxDeviceOpenGl.cpp` compiles on web with its Win32/WGL parts guarded and replaced by `CGxDeviceOpenGL/Web/CGxDeviceOpenGlWeb.cpp`. `CGxDeviceOpenGL/Web/GlShim.cpp` emulates the fixed-function GL 1.x subset over WebGL2 with per-state generated GLSL. The scheduler runs one pass per animation frame (`EvtSched.cpp`). DOM input lives in `Os/Web/Input.cpp`. Login, character create, entering the world and moving all work, at about 144 fps in Shadowglen. The canvas drives `gxResolution`/`widescreen`, because the UI and glue scenes take their aspect ratio from those CVars. A decompilation bug in `CGxDeviceOpenGl::IXformSetProjection` (it tested bit 31 where the D3D devices test `GxMasterEnable_NormalProjection`) was shrinking every 3D scene to 20%. It is fixed, for native GL as well. F11 logs one frame of draws (`GlShimSetDump`).
1. Add `GxApi_Gles3` to `EGxApi` (`Gx/Types.h:13`), the factory (`Gx.cpp:179`), and `GxApiSupported`/`GxDefaultApi`. Force it, plus `fixedFunction`, on web (S).
2. `CGxDeviceGles3`: fork `CGxDeviceOpenGl.cpp` (L).
   - Remove `GlExt.cpp`, WGL, pbuffers, the gamma ramp, client-side arrays and the ARB/NV program paths.
   - Keep `DsSet` state caching, the VBO broker, FBO, query and blend tables.
   - Use buffer staging plus `glBufferSubData`, because WebGL has no map.
   - Context via `emscripten_webgl_create_context` on an OffscreenCanvas, with explicit swap.
3. Fixed-function uber shaders (L):
   - Lighting: 4 lights plus material.
   - Linear fog.
   - Texgen: 7 modes plus texture matrices.
   - Texenv ops: the 6-entry `s_texEnvOps` table across 8 stages.
   - Alpha test, and clip planes as `discard`.
   - Point sprites.
4. Textures (M):
   - Swizzle BGRA formats.
   - DXT via `WEBGL_compressed_texture_s3tc`, with a CPU fallback through `Blit.cpp` DxtMakeTable.
   - Float render targets need `EXT_color_buffer_float`.
   - Stub depth readback.
5. Os/Web input (L): DOM key/mouse/wheel/focus → OSINPUT, Pointer Lock for `OS_MOUSE_MODE_RELATIVE`, clipboard cached from paste events, cursor via CSS. Stub IME and Joystick.

Done when the GlueXML login screen draws and you can type into the account box.

### M4: Log in and play (M)
Status: done. The `WowConnection` WebSocket transport goes through the bridge `/tcp/<ip>/<port>` relay, which only allows the auth and world addresses.
1. Web `WowConnection` (`WowServices/Web/WowConnection.cpp`): connect/send/recv/close over `emscripten/websocket.h`. `host:port` maps to `ws://<page-host>/tcp/<ip>/<port>`. IPv4 `uint32_t` addressing stays (M).
2. Web `WowConnectionNet::PlatformRun`, replacing the `WaitForMultipleObjects`/`WSAEventSelect` loop (M).
3. Bridge `/tcp` relay, allowlisted to the authserver and worldserver ports from config (S).
4. SRP6, ARC4 header crypt and the auth proof-of-work are portable C++ and need no change.

Done when the flow goes auth → realm list → char select → create a character → enter world.

### M5: Account management in the client (M–L)
Status: done, with a different design from the one planned below: no GlueXML files are overridden. `WoW/Source/Glue/Web/WebAccount.cpp` runs after `GLUE_EVENT_FRAMES_LOADED`. It adds a "Create Account" button, makes "Manage Account" open a change-password dialog, and relabels the Battle.net prompt, all from embedded Lua. Its `WebCreateAccount` and `WebChangePassword` functions compute the SRP6 salt and verifier, and the old-password proof via `SRP6_Client`. They post to the bridge (`Tools/Bridge/account.mjs`: `/account/register`, `/account/challenge`, `/account/password`). The bridge writes `acore_auth.account` as the least-privilege `wowbridge` MySQL user, whose login is in the gitignored `Tools/Bridge/bridge.env`, and needs `npm install` in `Tools/Bridge`. Checks: `node Tools/Bridge/account.test.mjs`, and `Tools/AzerothCore/login_probe.py` against accounts created in the client.
1. **C++ glue API (M):** in `Glue/GlueScriptEvents_Scripts.cpp`, add rows to `s_scriptFunctions[]` (around :1600):
   - `CreateAccount(user, pass, email)`
   - `ChangeAccountPassword(old, new)`
   - `RequestAccountInfo()`
   - `CancelAccountRequest()`

   Verifier math reuses `WowServices/SRP6_Client.cpp`: `x = SHA1(salt‖SHA1(U:P))`, `v = g^x mod N`, with a 32-byte CSPRNG salt.
   Add the glue events `ACCOUNT_CREATE_RESULT`, `ACCOUNT_PASSWORD_RESULT` and `ACCOUNT_INFO` to `g_glueScriptEvents` (`CGlueMgr.cpp` around :712), plus an idle state for Escape/Cancel.
2. **Bridge `/account` (M):**
   - `register`: validates the username (`[A-Z0-9]{3,16}`, no `@`/`#`), the email, and that the verifier is 32 bytes. Then it runs `INSERT` into `account` (match the columns used by AzerothCore's `AccountMgr::CreateAccount`). The DB unique key enforces uniqueness.
   - `chpass`: the bridge re-verifies the old password itself. It runs an SRP6 server handshake with the client against the stored salt and verifier, then `UPDATE`s salt and verifier. A bare username is never enough.
   - `info`: email, join date, last login, expansion and ban status, after the same SRP proof.
   - Per-IP rate limits, and a DB user with grants only on `acore_auth.account`.
3. **GlueXML (M):** copy the stock `AccountLogin.lua/xml` and `GlueParent.lua` out of the MPQ into `Web/Interface/GlueXML/`.
   - Add a "Create Account" button to the login screen and a new Lua screen, `accountcreate` (no C++ screen change needed).
   - On charselect, add Change Password and Account Info. Errors go through `OPEN_STATUS_DIALOG`.
4. Skip delete account for now, since the characters DB cleanup makes it risky.

### M6: Fidelity and polish (L–XL, any order)
Status: partly done. The account screens also show account details and change the email (`/account/login`, `/account/info`, `/account/email`, behind a five-minute token from an SRP6 proof). FMOD Ex has a web output (`SoundEngine/fmodex/emscripten`, AudioWorklet over a SharedArrayBuffer ring). `GlShim` translates the ARB vertex and fragment programs to GLSL, enough for water. Cinematics and performance work are still open.

- ARB→GLSL ES transpiler for water, shadows, FFX glow and terrain pixel shaders. Lazy compile plus `KHR_parallel_shader_compile`, because terrain has 128 vertex permutations (L).
- FMOD emscripten platform: `os_misc` on pthreads, stub net, AudioWorklet output (L).
- Cinematics: build xvidcore with `--disable-assembly` to static wasm and link `XvidDecoder.cpp` directly (M).
- Performance: draw-call batching and GL call overhead (M).

## Hosting
`Tools/Deploy` runs a realm for friends: the prebuilt acore images plus the bridge (`Tools/Bridge/Dockerfile`), behind a reverse proxy (`Caddyfile`; Cloudflare in front in SSL mode "Full"). No AzerothCore port is published. The bridge maps the client's `127.0.0.1` addresses to the containers (`--allow 127.0.0.1:3724=ac-authserver:3724,...`). With `WOW_BRIDGE_SECRET` set, the page gives each browser a signed cookie, and each cookie gets its own user-data overlay (128 MB at most). The realm is open to anyone who visits. One-time DB setup is in `setup.sql`. The client files (`Data/`, `Interface/`, `WTF/Config.wtf` from `Tools/Deploy/Config.wtf`) and the build output are copied to `DATA_DIR`. The bridge indexes game files at startup, so restart it after changing them. Game data under `Data/` is fetched as immutable 1 MB blocks (`/blocks/<path>/<size>-<index>.bin`), which the browser and Cloudflare cache, and the page files carry ETags, so a repeat visit mostly loads from cache. The page's loading screen uses the client's own loading art: `node Tools/Bridge/extract-art.mjs --out <DATA_DIR>/web` pulls it out of the game data (it is never committed). The login title is a texture rendered from the game's Morpheus font: `python Tools/Bridge/make-logo.py MORPHEUS.TTF <game dir>/Interface/Glues/LasthavensLogo.tga` (restart the bridge so it is indexed).

## Top risks
1. **Renderer scope.** The fixed-function emulation and ARB transpiler have to be checked against real MPQ data, and `IVertexShaderPatch` rewrites the ARB text.
2. **Hidden ABI drift.** With no layout asserts, bitfield/padding/`wchar_t` differences surface only as runtime corruption. Mitigation: M1.6.
3. **Threads and hosting.** COOP/COEP is mandatory, every WebSocket call is proxied to the main thread, and busy-waits like `WowConnectionNet::Start`'s `OsSleep(100)` spin need auditing.
4. **Sync XHR latency** on scattered MPQ reads. Mitigation: the block cache, with the bridge on localhost.
5. **4 GB ceiling.** The original was a 2–3 GB large-address-aware process. Watch memory in Dalaran/Wintergrasp.
6. **Account endpoint is a public write path** if the bridge is ever exposed beyond localhost. Add TLS and an invite code or CAPTCHA at that point.
7. **Licensing.** FMOD Ex source is free for non-commercial use only, with a credit line. Keep this a personal/local build.

## Browser target
Desktop Chrome/Edge and Firefox. Safari works but has no MEMORY64 (not needed). S3TC is missing on iOS and Android, which means CPU DXT decode at about 4× memory. Mobile is out of scope.
