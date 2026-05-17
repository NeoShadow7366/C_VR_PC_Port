# QA Guide — VR Menu Polish Tiers 15–19
**Build date:** 2026-05-03  
**Binary:** `build-vr\bin\Release\citra_vr.exe`  
**Scope:** Changes made in this polish session. Covers sidebar tab badges, Display settings modified-dots + reset-all, toast redesign, and welcome modal redesign.

---

## How to use this guide
Work through each section in order. For each test item, mark **PASS**, **FAIL**, or **N/A**. Items marked with ⚠ are higher risk because they touch shared state (settings persistence, modal flow).

Use the hardware test configuration you have available; at minimum you need one SteamVR headset with two 6DOF controllers.

---

## Tier 15 — Sidebar tab badges

### 15-A  Library badge — populated library
1. Set `CITRA_VR_ROM_DIR` to a folder with at least 3 ROM files.
2. Launch CitraVR and open the menu.
3. **Expected:** The **Library** sidebar button shows a small cyan pill (active tab) or slate pill (inactive) with the exact ROM count on the right edge of the button (e.g. `5`).
4. Scan with a fresh ROM dir containing a different count → reopen menu → badge should update.

### 15-B  Library badge — empty dir
1. Set `CITRA_VR_ROM_DIR` to an empty folder or unset it.
2. Open menu → Library tab.
3. **Expected:** No badge pill appears on the Library button (count 0 is suppressed).

### 15-C  Input badge — remapped bindings
1. Go to **Input** tab → remap 2 bindings (e.g. A button, L trigger).
2. Navigate away from Input tab.
3. **Expected:** Input sidebar button shows a slate/cyan pill with `2`.
4. Reset all bindings in Input → navigate away.
5. **Expected:** Input badge disappears (count = 0 suppressed).

### 15-D  Badge visual — active vs inactive state
1. Click each tab in sequence.
2. **Expected:** The badge pill on the **active** tab is filled cyan with dark text. On **inactive** tabs it is a muted slate fill with off-white text. Both are legible and don't overlap the tab label.

### 15-E  Display / Session / About — no badge
- **Expected:** These three tabs never show a badge pill regardless of settings state.

---

## Tier 16 — Display settings modified-dot indicators

### 16-A  Baseline — all defaults
1. Open Display tab. If any modified-dots are visible, use Reset all (see Tier 17) first.
2. **Expected:** No cyan dots visible anywhere in the label column.

### 16-B  Stepper row dot
1. Change **Internal res** from 1× to 2×.
2. **Expected:** A small cyan dot (≈ 8 px diameter) appears in the label gutter to the left of "Internal res". All other stepper rows should remain dot-free.
3. Restore to 1× → dot disappears immediately.

### 16-C  Segmented row dot (Immersive mode)
1. Change **Immersive mode** from Off to High.
2. **Expected:** Cyan dot appears left of "Immersive mode" text.
3. Restore to Off → dot disappears.

### 16-D  Checkbox row dot (Swap screens)
1. Enable **Swap top/bottom screens**.
2. **Expected:** Cyan dot appears left of the checkbox label.
3. Uncheck → dot disappears.

### 16-E  Multiple dots simultaneously
1. Change Internal res (2×), Frame limit (90%), Screen size (1.5×), Audio stretching (off).
2. **Expected:** All four rows show dots. Other rows remain clean.

### 16-F  Dots survive tab switch
1. Change Volume, then navigate to Library tab and back.
2. **Expected:** Volume dot is still present after returning.

### 16-G  Dot alignment
- **Expected:** All dots sit in the same horizontal gutter position regardless of which setting row they appear on. Dots should not clip into the label text.

---

## Tier 17 — Display "Reset all" with confirm modal

### 17-A  Banner hidden when no overrides
1. Open Display tab with all settings at defaults.
2. **Expected:** The "N settings changed from default" banner and **Reset all** button are not visible. Settings list starts immediately after the search bar.

### 17-B  Banner count accuracy
1. Change 3 distinct settings (e.g. resolution, volume, layout).
2. **Expected:** Banner reads **"3 settings changed from default"** in cyan caption font. Count updates immediately when you change or restore a setting.

### 17-C  Banner count — singular form
1. Change exactly one setting.
2. **Expected:** Banner reads **"1 setting changed from default"** (no spurious "s").

### 17-D  Reset all button placement ⚠
1. With overrides active, verify the **Reset all** button is right-aligned at the end of the banner row (not left- or centre-aligned).
2. **Expected:** Button is visually amber/gold coloured, right-aligned, and fits within the content area without clipping.

### 17-E  Confirm modal opens
1. Tap **Reset all**.
2. **Expected:** Confirm modal opens with: display-font headline "Reset Display Settings?", descriptive body text, **Cancel** and **Reset all** buttons. Background is dimmed.

### 17-F  Cancel does nothing ⚠
1. Open confirm modal → tap **Cancel**.
2. **Expected:** Modal closes, all settings remain unchanged, banner still shows original count.

### 17-G  Confirm resets everything ⚠
1. Change resolution (2×), frame limit (90%), screen scale (1.5×), immersive mode (High).
2. Tap Reset all → confirm.
3. **Expected:**
   - All Display rows return to compiled defaults (res=1×, fl=100%, vol=1.0, scale=1.0×, dist=1.5 m, eye=32.5 mm, immersive=Off, 3D depth=0, layout=Default, swap=off).
   - All modified dots disappear.
   - Banner disappears.
   - Toast appears: "Display settings reset to defaults" with amber/warning styling.

### 17-H  Persistence after reset ⚠
1. Reset all → close menu → reopen.
2. **Expected:** Settings remain at defaults (no dots, no banner).

---

## Tier 18 — Premium toast notifications

### 18-A  Toast appears and dismisses
1. Change any Display setting (e.g. Screen size).
2. **Expected:** A toast slides up from below the menu ≈ 56 px from the bottom, holds for ~1.5 s, then slides back down. No abrupt pop-in/out.

### 18-B  Slide-in animation
- **Expected:** Toast starts ≈ 36 px below its resting position and eases (soft deceleration) into place. On dismiss it slides back down. No horizontal drift.

### 18-C  Info kind (default — cyan stripe)
1. Trigger any screen-geometry toast (Screen size, Screen dist, Eye offset, Layout).
2. **Expected:** Left stripe is **cyan**, icon bubble shows `i`, background is dark slate. Text is legible white.

### 18-D  Success kind (green stripe)
1. Apply a **Quality / Balanced / Performance** preset.
2. **Expected:** Toast stripe and bubble are **green**, glyph is `OK`.

### 18-E  Warning kind (amber stripe)
1. Confirm **Reset all** display settings.
2. **Expected:** Toast stripe and bubble are **amber**, glyph is `!`.

### 18-F  Toast width and centering
- **Expected:** Toast is ≈ 62% of menu width, horizontally centred. Left stripe is exactly 8 px wide and hugs the rounded left edge. Icon bubble, glyph, and message text are vertically centred within the 96 px toast height.

### 18-G  Toast doesn't block interaction
1. Trigger a toast → immediately interact with the menu (scroll, tap buttons).
2. **Expected:** Menu responds normally. Toast is non-interactive (passes through pointer events).

### 18-H  Rapid succession toasts
1. Quickly tap +/- on Screen size several times.
2. **Expected:** Each tap resets the timer; toast stays visible and updates smoothly without flicker.

---

## Tier 19 — Welcome modal redesign

> **Note:** The welcome modal only auto-opens on first launch (when `seen_welcome` is not set in vr_config.txt). To re-test: open `vr_config.txt` (path shown in the About tab), remove or set `seen_welcome=false`, then relaunch.

### 19-A  Modal opens on first launch ⚠
1. Remove `seen_welcome` from vr_config.txt, relaunch.
2. **Expected:** Welcome modal appears automatically on menu open. Background is dimmed. Modal is centred with 18 px rounded corners.

### 19-B  Hero band
- **Expected:** Top 200 px is a cyan→indigo gradient (left=cyan-dark, right=purple-dark). A 2-px bright highlight sits along the very top edge. No text or hero content is clipped by the rounded corners.

### 19-C  Page 1 content (VR)
- **Expected:** Icon bubble (dark circle with soft outer ring) shows `VR` glyph in cyan. Step badge reads `STEP 1 OF 3` in caption font. Title reads `Welcome to CitraVR` in display font. Body describes the floating screen and menu button.

### 19-D  Page 2 content (::)
1. Tap **Next**.
2. **Expected:** Icon bubble glyph changes to `::`, step badge reads `STEP 2 OF 3`, title is `Your controllers`, body lists the key bindings. Back button is now active.

### 19-E  Page 3 content (OK)
1. Tap **Next** again.
2. **Expected:** Glyph = `OK`, badge = `STEP 3 OF 3`, title = `Make it yours`, body describes Display/Input/Library tabs and presets tip.

### 19-F  Page indicator — pill vs dots
- **Expected:** Active page indicator is a **24×10 cyan rounded pill**. Inactive pages are small circles. Indicator is horizontally centred above the nav buttons.

### 19-G  Back navigation ⚠
1. Advance to page 3, tap **Back** twice.
2. **Expected:** Returns to page 1. Back button is disabled (dimmed) on page 1.

### 19-H  Dismiss and persistence ⚠
1. On page 3, tap **Got it, let's play!**.
2. **Expected:** Modal closes. Relaunch → modal does **not** reappear. vr_config.txt now contains `seen_welcome=true`.

### 19-I  Cancel / close without completing
1. Remove `seen_welcome`, relaunch. Close the menu (wrist button) while welcome modal is open.
2. **Expected:** Modal state is preserved; reopen menu → modal resumes at the page you left it on. `seen_welcome` is NOT written until page 3 is confirmed.

---

## Regression checks (all tiers)

| Area | Check |
|---|---|
| Library grid | ROMs still load, thumbnail images show, tap launches game |
| Pin/star | Gold stars persist across relaunches; pinned ROMs sort first |
| Saves tab | Slot cards render; Save/Load work when game is running |
| Input tab | Binding rows show chips; Reset bindings confirm modal works |
| Session tab | "Now Playing" hero when game active; Recenter works |
| About tab | Stat grid populates (OpenXR runtime, GPU, refresh rate, config path) |
| Perf overlay | Toggle in About; overlay persists across tab switches |
| Sidebar pulse dot | Cyan dot pulses when game is running; IDLE text when not |
| Footer hint bar | Controller verb hints visible at bottom of every tab |
| Tab switch fade | Brief alpha cross-fade when switching tabs (no hard cut) |

---

## Known limitations / out of scope
- Saves badge count (filled slot count) on the sidebar is wired but always empty — `ListSaveStates` is disk-bound and not called each sidebar frame by design.
- Tier 18 icon glyphs (`i`, `OK`, `!`, `X`) are plain ASCII rendered with the Display font. A proper icon glyph font is a future enhancement.
- Welcome modal `19-I` (partial-close state) depends on ImGui popup stack behaviour which can vary if the host application suspends mid-frame.
