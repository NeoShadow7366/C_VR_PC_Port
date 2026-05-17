@echo off
REM ---------------------------------------------------------------------------
REM citra_vr launcher - edit the variables in the CONFIG block, then either
REM   - double-click this file, or
REM   - run it from any cmd / PowerShell window.
REM
REM Behaviour:
REM   - Confirms the exe exists.
REM   - Reminds you to start SteamVR first (does NOT auto-launch it - leaving
REM     that to the user keeps the runtime selection explicit).
REM   - Forwards the ROM path as the first positional argument.
REM   - Pauses on exit so you can read any error MessageBox / log tail.
REM
REM Logs are written to:  %APPDATA%\Citra\log\citra_log.txt
REM ---------------------------------------------------------------------------

setlocal EnableDelayedExpansion

REM =========================== CONFIG (edit me) ==============================

REM Build configuration to run (Release | Debug | RelWithDebInfo).
set "BUILD_CONFIG=Release"

REM Build directory (defaults to the sibling build-vr next to this script).
set "BUILD_DIR=%~dp0build-vr"

REM Full path to the ROM you want to boot. Leave empty to auto-resume your
REM last game, or scan CITRA_VR_ROM_DIR and open the in-VR ROM picker.
REM Quote it if it contains spaces.
set "ROM_PATH=G:\Emulations\Roms\3ds\The Legend of Zelda Ocarina of Time.3ds"

REM Extra args (leave empty for now). Example: --version
set "EXTRA_ARGS="

REM Set to 1 to dump the last 40 lines of the Citra log after the run.
set "TAIL_LOG_ON_EXIT=1"

REM ---- VR optional env vars ----------------------------------------------
REM CITRA_VR_VK_DEBUG=1    -> enable Vulkan validation layers (may crash with
REM                            SteamVR's D3D11 interop layer).
set "CITRA_VR_VK_DEBUG="

REM CITRA_VR_HMD          -> force HMD profile (resolution factor, refresh
REM                          cap). SteamVR's OpenXR runtime hides the actual
REM                          headset model, so set this to your headset:
REM                          beyond2 | index | vive | generic
set "CITRA_VR_HMD=beyond2"

REM CITRA_VR_ROM_DIR      -> root directory scanned by the in-VR ROM browser
REM                          (recursive, depth 4, capped at 500 entries).
REM                          If empty, the browser falls back to the folder
REM                          of the ROM passed on the command line.
set "CITRA_VR_ROM_DIR=G:\Emulations\Roms\3ds"

REM ==========================================================================

set "EXE=%BUILD_DIR%\bin\%BUILD_CONFIG%\citra_vr.exe"

if not exist "%EXE%" (
    echo [run_citra_vr] ERROR: citra_vr.exe not found at:
    echo     %EXE%
    echo.
    echo Build it first:
    echo     cmake --build "%BUILD_DIR%" --config %BUILD_CONFIG% --target citra_vr
    pause
    exit /b 1
)

REM ROM_PATH is optional. When empty the exe shows a file-picker dialog.
if defined ROM_PATH if not exist "%ROM_PATH%" (
    echo [run_citra_vr] WARNING: ROM not found: %ROM_PATH%
    echo Clearing ROM_PATH - the file-picker dialog will open instead.
    set "ROM_PATH="
)

echo [run_citra_vr] Make sure SteamVR is running and your headset is awake.
echo [run_citra_vr] Launching: %EXE%
if defined ROM_PATH (
    echo [run_citra_vr] ROM:       %ROM_PATH%
) else (
    echo [run_citra_vr] ROM:       ^(none - will auto-resume last game or open in-VR picker^)
)
echo.

REM Path of the in-VR-browser sentinels. citra_vr writes these when the
REM user picks a ROM or a save-state Load slot from the in-VR menu, then
REM exits cleanly. We pick them up below and relaunch with that ROM and
REM (optionally) --loadslot N.
set "ROM_SENTINEL=%APPDATA%\Citra\next_rom.txt"
set "SLOT_SENTINEL=%APPDATA%\Citra\next_load_slot.txt"
if exist "%ROM_SENTINEL%"  del "%ROM_SENTINEL%"  >nul 2>&1
if exist "%SLOT_SENTINEL%" del "%SLOT_SENTINEL%" >nul 2>&1

set "LOAD_SLOT_ARG="

:run_loop
if defined ROM_PATH (
    "%EXE%" %EXTRA_ARGS% %LOAD_SLOT_ARG% "%ROM_PATH%"
) else (
    "%EXE%" %EXTRA_ARGS%
)
set "RC=%ERRORLEVEL%"
echo.
echo [run_citra_vr] citra_vr.exe exited with code %RC%.

REM Reset slot arg for next iteration; only set again if the sentinel exists.
set "LOAD_SLOT_ARG="

if exist "%ROM_SENTINEL%" (
    REM Read first line of the sentinel as the next ROM path.
    set /p NEXT_ROM=<"%ROM_SENTINEL%"
    del "%ROM_SENTINEL%" >nul 2>&1
    if defined NEXT_ROM if exist "!NEXT_ROM!" (
        echo [run_citra_vr] In-VR menu requested next ROM: !NEXT_ROM!
        set "ROM_PATH=!NEXT_ROM!"
        if exist "%SLOT_SENTINEL%" (
            set /p NEXT_SLOT=<"%SLOT_SENTINEL%"
            del "%SLOT_SENTINEL%" >nul 2>&1
            if defined NEXT_SLOT (
                echo [run_citra_vr] In-VR menu requested LoadState slot: !NEXT_SLOT!
                set "LOAD_SLOT_ARG=--loadslot !NEXT_SLOT!"
            )
        )
        goto run_loop
    ) else (
        echo [run_citra_vr] Sentinel ROM not found, not relaunching: !NEXT_ROM!
    )
)

if "%TAIL_LOG_ON_EXIT%"=="1" (
    set "LOG=%APPDATA%\Citra\log\citra_log.txt"
    if exist "!LOG!" (
        echo.
        echo [run_citra_vr] ---- last 40 lines of !LOG! ----
        powershell -NoProfile -Command "Get-Content -LiteralPath '!LOG!' -Tail 40"
        echo [run_citra_vr] -------------------------------------------------------------
    ) else (
        echo [run_citra_vr] No log file at !LOG! yet.
    )
)

echo.
pause
endlocal
exit /b %RC%
