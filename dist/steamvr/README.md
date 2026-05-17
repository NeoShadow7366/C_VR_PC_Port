# SteamVR / Steam Link integration

These scripts let CitraVR show up natively in:

- The **SteamVR dashboard library** (inside the headset)
- **Steam Link** (the Steam app on Quest / standalone headsets) — once
  registered, you can launch CitraVR from the headset without removing
  it to alt-tab on the desktop.

## One-time setup

1. Build CitraVR: `cmake --build --preset vr` (or open the VS solution).
2. Make sure SteamVR has been launched at least once on this user
   account (so `%LOCALAPPDATA%\openvr\` exists).
3. From a normal (non-admin) PowerShell, run:

   ```powershell
   cd "<repo>\dist\steamvr"
   .\register_steamvr.ps1
   ```

   The script auto-finds `citra_vr.exe` in the usual build output
   folders. Override with `-CitraVrExe "C:\path\to\citra_vr.exe"`.

4. Start (or restart) SteamVR. **CitraVR (3DS)** now appears in the
   dashboard library.

## Steam Link usage

Steam Link streams whatever is showing in SteamVR. Once the app is
registered:

1. Start SteamVR on the PC.
2. Put on the headset and open Steam Link / the SteamVR dashboard.
3. Pick **CitraVR (3DS)** from the library. The ROM picker opens in VR.

## Uninstall

```powershell
.\unregister_steamvr.ps1
```

Removes the manifest entry from SteamVR's `appconfig.json`. The exe
itself is not touched.

## How it works

- `citra_vr.vrmanifest` is the template. `register_steamvr.ps1` writes
  a copy next to the actual exe with the absolute `binary_path_windows`
  filled in, then appends that path to `manifest_paths` in
  `%LOCALAPPDATA%\openvr\appconfig.json`. SteamVR reads that file at
  startup and picks up the new application.
- No admin rights, no Steam Workshop entry, no installer. The manifest
  is purely user-scoped.

## Troubleshooting

- **CitraVR not appearing in SteamVR library**: confirm
  `%LOCALAPPDATA%\openvr\appconfig.json` contains the manifest path,
  and that the path on disk still exists. Then restart SteamVR.
- **Steam Link can't see it**: in Steam Link, make sure you're in
  **VR mode** (not desktop streaming). The app is only visible
  through the SteamVR dashboard, not the regular Steam Big Picture
  library.
- **Path moved after registration**: just re-run `register_steamvr.ps1`
  — it overwrites the entry with the new absolute path.
