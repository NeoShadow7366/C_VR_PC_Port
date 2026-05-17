# NeoXR Citra — Windows Installer & Steam Integration

This directory builds a single-file Inno Setup installer that ships **both**
desktop entries:

| Entry | Binary | Description |
|---|---|---|
| **NeoXR Citra**    | `citra-qt.exe`            | Flat-screen Citra (3DS emulator), Qt UI. |
| **NeoXR Citra VR** | `citra_vr_launcher.exe`   | Tiny shim that starts SteamVR (Steam AppID 250820), waits for `vrserver.exe`, then launches `citra_vr.exe`. |

The installer optionally registers both entries as non-Steam shortcuts in the
user's Steam library and deploys grid art (capsule / hero / logo / icon / small
capsule) so they look identical to native Steam titles.

## Layout

```
dist/
├── installer/
│   ├── neoxr_citra.iss      Inno Setup script
│   ├── Build-Installer.ps1       One-shot build orchestrator (art + ISCC)
│   └── out/                      Output .exe lands here
├── steam_art/
│   ├── source/                   Raw concept art (PNG/JPG, any size)
│   ├── build_art.ps1             Crops/resizes to Steam grid dimensions
│   └── out/{qt,vr}/              Generated Steam-ready PNGs + icon.ico
└── steam_integration/
    ├── SteamShortcuts.psm1       Binary shortcuts.vdf reader/writer
    ├── Add-SteamShortcuts.ps1    Adds the two entries (idempotent)
    └── Remove-SteamShortcuts.ps1 Uninstall hook
```

## Build prerequisites

1. **Both** Citra builds compiled:
   ```powershell
   cmake --build --preset qt        # builds citra-qt.exe + DLLs into build\bin\Release\
   cmake --build --preset vr        # builds citra_vr.exe + citra_vr_launcher.exe into build-vr\bin\Release\
   ```
2. **Inno Setup 6** — https://jrsoftware.org/isinfo.php (free, ~3 MB).
   `ISCC.exe` must be on `PATH` or under
   `%PROGRAMFILES(X86)%\Inno Setup 6\`.

## Build the installer

```powershell
cmake --build build-vr --target neoxr_installer
```

…or directly:

```powershell
.\dist\installer\Build-Installer.ps1 -AppVersion 0.1.0
```

Output: `dist\installer\out\NeoXRCitra-Setup-<ver>.exe` (~50 MB).

## What end-users see

1. They run `NeoXRCitra-Setup-<ver>.exe`. Installs per-user to
   `%LOCALAPPDATA%\Programs\NeoXRCitra\` — **no admin needed**.
2. Wizard tasks page (Steam tasks only appear if Steam is detected):
   - `[✓]` Desktop shortcut for NeoXR Citra
   - `[✓]` Desktop shortcut for NeoXR Citra VR
   - `[✓]` Add **NeoXR Citra** to Steam library
   - `[✓]` Add **NeoXR Citra VR** to Steam library *(auto-launches SteamVR)*
3. After Finish + Steam restart, both entries appear in the user's Steam
   library with full grid art.
4. Clicking the VR entry: Steam launches `citra_vr_launcher.exe`, which:
   - Checks for `vrserver.exe`. If absent, fires `steam://run/250820`.
   - Polls up to 60 s for SteamVR to come online.
   - Spawns `citra_vr.exe` with all forwarded args.
5. Uninstall removes program files **and** Steam shortcuts/grid art.

## Regenerating art

If you swap a source image in `dist\steam_art\source\`, re-run:

```powershell
.\dist\steam_art\build_art.ps1
```

The script auto-fits titles, gradient-bands captions, generates a multi-res
`.ico`, and writes everything into `dist\steam_art\out\{qt,vr}\`.

Source-image role mapping is at the top of `build_art.ps1` (`$mapExplicit`);
edit there to re-assign which source feeds which slot.

## How Steam integration works under the hood

- **App ID** is derived per non-Steam-shortcut convention:
  `crc32(quoted_exe + appname) | 0x80000000`
  (matches Steam ROM Manager / SteamGridDB).
- **shortcuts.vdf** is rewritten in Steam's binary VDF format
  (types `0x00` nested map, `0x01` string, `0x02` int32 LE, `0x08` end).
  A `.bak` is created on first write.
- **Grid art** is dropped into `<Steam>\userdata\<id>\config\grid\` using the
  filenames Steam expects:
  - `<appid>.png`        vertical capsule (600×900)
  - `<appid>p.png`       small capsule    (462×174)
  - `<appid>_hero.png`   hero banner      (1920×620)
  - `<appid>_logo.png`   transparent logo (1280×720)
  - `<appid>_icon.png`   icon             (256×256)
- The **VR shortcut sets `OpenVR=1`** — this puts the VR badge in Steam Library
  and (combined with the launcher shim) is what makes Steam know to keep the
  SteamVR Status window relevant while the game is running.
- By default the installer writes to the **most recently used** Steam account;
  pass `-AllUsers` to `Add-SteamShortcuts.ps1` to register for every signed-in
  account.

## Known caveats

- **Steam must be restarted** after install for new shortcuts and art to show.
- If the user has set OpenXR runtime to something other than SteamVR
  (e.g. Oculus), the VR entry will still launch SteamVR via Steam — but the
  game itself loads whichever runtime is active. They should set runtime in
  *SteamVR Settings → Developer → Set SteamVR as OpenXR Runtime*.
- The launcher uses `steam://run/250820` which requires Steam to be running.
  If Steam is closed it will start Steam first, which can add ~10 s.
