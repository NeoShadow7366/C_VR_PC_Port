# 2026-05-01 — Premium VR UX Plan (v1)

> Goal: take the CitraVR SteamVR build from "functional + polished" to "feels like a premium first-party VR app" on Bigscreen Beyond 2 (primary), Valve Index, and other PCVR HMDs.

## 1. Scope & Success Criteria

A "premium" PCVR experience for an emulator means:

1. **Frictionless onboarding** — first launch boots straight into a usable, beautiful state with zero log-reading.
2. **Comfortable viewing** — the 3DS screens feel like they sit in a deliberately designed virtual space, not floating in a void.
3. **Tactile, legible UI** — every interaction has visual + haptic feedback; text is sharp; affordances are obvious.
4. **Effortless input** — bindings discoverable in-headset, sensible defaults per controller, no manual config-file editing.
5. **Stable & quiet** — no validation spam, no sub-90 fps stutter on Beyond 2, graceful errors with on-screen messages instead of silent exits.
6. **Trustworthy lifecycle** — save, load, ROM-swap, and quit all work without restart sentinels visible to the user.

Success = a new user can plug in a Beyond 2, double-click a desktop shortcut, pick a ROM in-headset, play, save, reload, and quit without ever touching a config file or reading a log.

## 2. Current State (as of 2026-05-01)

### 2.1 What already feels premium

- **Visual chrome**: Steam-slate teal-accent ImGui theme, hero banner, colour-coded action buttons, `SeparatorText` section headers (VrApp.cpp ~L778+).
- **Cursor reticle**: per-hand tinted (teal/amber), procedural ring + dot, anti-halo outline, interactive scale-up + click pulse.
- **Wrist bar**: 3 rounded glyph buttons (☰ ▶ −) with hover halo, transparent corners, haptic ping on hover-enter.
- **Modal scrim**: DimLayer behind menu, alpha-synced to menu fade (160 ms ease).
- **Menu re-centre on every open**: yaw-only, world-anchored, 1.2 m forward.
- **Per-eye stereoscopy** for cursors when snapped to game quad.
- **Cursor smoothing**: smoothstep band 5 mm – 4 cm; 10 cm hard-snap safety.
- **Beyond 2 + Index profiles**: refresh-rate cap (90 / 120) via `XR_FB_display_refresh_rate`.
- **Haptic feedback**: hover-enter, trigger-press, menu-open, wrist taps.
- **Round-tripped settings** via Qt VR tab + `vr_config.txt`.
- **Bindings remap** in-headset, persisted.
- **Crash dumps** auto-written to `%APPDATA%\Citra\dumps\`.

### 2.2 Gaps vs "premium" bar

| # | Gap | Symptom | Severity |
|---|---|---|---|
| G1 | Game floats in pure black void | No sense of "place"; eye fatigue on Beyond 2 OLED at high brightness contrast | High |
| G2 | Single flat quad at fixed 1.5 m | No theatre/curved/large-screen feel; not adjustable in-headset | High |
| G3 | No first-launch onboarding | New user sees menu with no guidance; has to discover wrist bar / B-button | High |
| G4 | No visual binding overlay | User has to remember which button does what; bindings menu is text-only | Med |
| G5 | Save/Load requires sentinel restart | Visible exe relaunch breaks immersion; LoadState shows SteamVR splash | High |
| G6 | No audio spatialisation | 3DS audio is plain stereo into headphones, not anchored to the screen | Med |
| G7 | No comfort options | No vignette/snap-turn/IPD nudge/seated-vs-standing toggle | Med |
| G8 | No "now playing" / per-game persistence | Settings, bindings, save-state slot are global, not per-ROM | Med |
| G9 | Toast/error feedback missing | Save success, load failure, ROM-swap status only in log | Med |
| G10 | Wrist bar UX limits | Only 3 quick actions; no quick-access for slot save/load/screenshot/recenter | Low |
| G11 | Menu information density | Settings page is stepper-heavy; no presets ("Quality / Balanced / Performance") | Low |
| G12 | First-frame black flash | SteamVR overlay → app handover shows ~1 s of compositor splash | Low |
| G13 | Foveated rendering not used | Beyond 2 high-DPI render is full-resolution everywhere | Med |
| G14 | No screenshot / clip capture | No way to share a moment without OS-level tools | Low |
| G15 | Idle / AFK behaviour | App keeps running at full draw when HMD is off-head | Low |
| G16 | Log noise on startup | Validation messages and Vulkan warnings still verbose by default | Low |

## 3. Improvement Plan

Organised into four batches, each independently shippable. Each item lists Files, Effort (S/M/L), and acceptance criterion.

### Batch A — Sense of Place (the "wow" pass)

Highest ROI for premium feel. Most users notice these in the first 30 seconds.

| ID | Title | Files | Effort | Accept |
|---|---|---|---|---|
| A1 | **Virtual environment skybox / theatre** | NEW `layers/EnvironmentLayer.{h,cpp}`, VrApp layer chain | M | A subtle dim "lounge" or "void with floor grid" composition layer behind the game quad. 3 presets selectable in menu (Void / Lounge / Theater). Default = Lounge. |
| A2 | **Curved game quad option** | `layers/GameQuadLayer.{h,cpp}` (add `XrCompositionLayerCylinderKHR` path), VrSettings, menu toggle | M | Toggle Flat ↔ Curved (radius 1.5 m). Falls back to flat when `XR_KHR_composition_layer_cylinder` missing. |
| A3 | **Adjustable screen size + distance** | VrApp menu (Settings page), `GameQuadLayer` constants → members, vr_config.txt keys | S | Two sliders/steppers: distance 0.8–4.0 m, scale 0.6–3.0×. Persisted. Live preview. |
| A4 | **Soft drop shadow / floor reflection plane** | NEW small layer or quad pair under game quad | S | Optional shadow plate makes the screen feel anchored. Toggle. |
| A5 | **Configurable background brightness** | EnvironmentLayer, slider 0–100% | S | Lets user dial down OLED bloom on Beyond 2. |

### Batch B — Onboarding & Discoverability

| ID | Title | Files | Effort | Accept |
|---|---|---|---|---|
| B1 | **First-launch welcome card** | VrApp, `vr_config.txt` flag `seen_welcome=1` | S | One-time modal: "Press ☰ on your wrist to open the menu. Hold for Home." 3-step tutorial, dismiss button. |
| B2 | **In-headset controller diagram overlay** | NEW `layers/ControllerHintLayer.{h,cpp}` | M | When menu is open OR user holds System for 1 s, render a translucent SVG-like callout next to each controller showing current binding labels (A=A, B=B, Trigger=Touch, etc). |
| B3 | **Tooltips on hover** | ImGui `SetTooltip` + idle-hover timer | S | Every settings widget has a one-line description on 500 ms hover. |
| B4 | **Empty-state ROM browser** | `RomBrowser` UI | S | When library empty: friendly "Drop your `.3ds` files in `<dir>` then click Refresh" with a Browse-folder button. |
| B5 | **Quick-Start preset chooser** | VrApp menu | S | First-launch chooser: Quality / Balanced / Performance → sets internal-res, frame-limit, foveation. |

### Batch C — Comfort, Audio, Feedback

| ID | Title | Files | Effort | Accept |
|---|---|---|---|---|
| C1 | **Vignette on motion** (optional, off by default) | NEW small overlay layer or shader pass | M | Reduces FOV temporarily during headset translation > threshold. Setting: Off / Light / Strong. |
| C2 | **Spatial audio anchored to screen** | `audio_core` integration; OpenAL HRTF or Steam Audio (eval LGPL/GPL compat first) | L | 3DS stereo placed as a virtual stereo pair at the screen position. Toggle Off/HRTF. |
| C3 | **Haptic vocabulary expansion** | XrController + VrApp call sites | S | Distinct patterns: tap (10 ms / 200 Hz), thump (40 ms / 120 Hz), success-double, error-buzz. Used for save/load/menu/error. |
| C4 | **Toast layer** | NEW `layers/ToastLayer.{h,cpp}` | M | Bottom-of-FOV transient text (1.5 s ease-in/out). Used for "Saved to slot 3", "Load failed: …", "ROM loaded: <name>". Replaces silent log-only feedback. |
| C5 | **Recenter view** action | XrController (long-press right System), VrApp | S | Re-anchors player forward. Toast confirms. |
| C6 | **Auto-pause on HMD off-head** | `XR_FB_idle` or session state listener | S | When proximity sensor reports off-head: pause emulation, dim layers; resume on-head. |
| C7 | **Brightness / contrast post** | optional shader pass on game quad | M | Two sliders. Persists per-ROM (depends on D3). |

### Batch D — Lifecycle & Per-Game

| ID | Title | Files | Effort | Accept |
|---|---|---|---|---|
| D1 | **In-process LoadState / ROM swap** | `core/core.cpp`, `renderer_vulkan/renderer_vulkan.cpp` (~L81 dtor AV), VrApp `EndSessionForReload` + `ReinitSession` | L | No exe relaunch on Load or game change. Removes sentinel files. Tracks `/memories/repo/renderer_dtor_root_cause.md`. |
| D2 | **Persistent toast for sentinel-restart fallback** | citra_vr.cpp end-of-main, ToastLayer | S | Until D1 lands: show "Reloading…" splash on the SteamVR overlay so user knows what's happening. |
| D3 | **Per-game profile** | `Common::VRConfig` — namespaced keys e.g. `[gameid:000400000008F800]` | M | When ROM loads, layer per-game keys over global. Settings UI shows "(per game)" badge on overridden values. |
| D4 | **Quick-save / quick-load on wrist** | Wrist bar grows from 3 → 5 buttons OR add chord (Menu+Start = quicksave) | S | Single press triggers save to slot 0 with toast. Long press = load. |
| D5 | **Last-played resume** | vr_config.txt `last_rom` + `last_slot`; auto-load on launch unless overridden | S | Optional toggle "Auto-resume last session". |
| D6 | **Screenshot capture** | Read game-quad swapchain image, write PNG to `%APPDATA%\Citra\screenshots\` | M | Wrist-bar 4th button or chord. Toast on save. |

### Batch E — Performance & Quality Polish

| ID | Title | Files | Effort | Accept |
|---|---|---|---|---|
| E1 | **Foveated rendering** (Beyond 2: fixed; Index: dynamic if eye-tracked profile available) | `XR_FB_foveation_vulkan` ext path; `vk_swapchain` integration | M | 10–20% GPU saving at high render-scale, no visible quality loss in centre. Toggle in menu. |
| E2 | **Asynchronous space warp acceptance** | `XR_KHR_composition_layer_depth` for game quad | M | Reprojection looks correct on dropped frames. |
| E3 | **Validation noise gate** | Filter known-benign SteamVR validation strings in `vk_platform.cpp` debug callback | S | `--vk-debug` only shows real issues. |
| E4 | **Idle frame-rate throttle** when menu open | VrApp Frame() | S | When menu visible, run emulation at half rate (or pause, current behaviour) AND drop game-quad redraw to 30 Hz. |
| E5 | **First-frame splash hide** | Submit a dim solid colour layer on session begin until first real frame ready | S | Removes the SteamVR loading texture flash. |
| E6 | **Symbol uploads to debug-CI** | CI workflow already builds; add PDB artifact | S | Faster crash triage. |

## 4. Prioritisation & Sequencing

**Sprint Premium-1** (≈ 1 week, Batch A + B core):
- A1 (Lounge skybox), A3 (size/distance sliders), A4 (drop shadow)
- B1 (welcome card), B3 (tooltips), B5 (quality presets)
- C4 (toast layer) — unlocks user-facing feedback for everything below

**Sprint Premium-2** (≈ 1 week, depth + comfort):
- A2 (curved quad), C3 (haptic vocabulary), C5 (recenter action), C6 (off-head pause)
- D2 (reload toast), D4 (quick-save), D5 (last-played resume)
- E3, E4, E5 (cheap performance/quality wins)

**Sprint Premium-3** (≈ 2 weeks, the heavy hitters):
- D1 (in-process LoadState — removes sentinel restart, depends on `~RendererVulkan` fix per `/memories/repo/renderer_dtor_root_cause.md`)
- D3 (per-game profiles)
- E1 (foveated rendering), E2 (depth layer / ASW)
- B2 (controller diagram overlay)
- C2 (spatial audio — license vetting first)

**Backlog**: A5, C1, C7, D6, E6.

## 5. Risks & Constraints

- **D1 blocker**: `RendererVulkan` destructor AV on the LoadState path. Already root-caused (XrSession outlives VkDevice). Fix is ~3–5 day refactor (`EndSessionForReload` + `ReinitSession`) per existing memory note. Until then, all per-process lifecycle improvements are constrained.
- **C2 license risk**: Steam Audio is permissive but bundles `phonon.dll` — verify GPL-v3 compatibility before integration. OpenAL Soft HRTF (LGPL) is the safe fallback.
- **A2 extension support**: `XR_KHR_composition_layer_cylinder` is core in modern OpenXR but SteamVR's runtime should be probed; always keep flat-quad fallback.
- **E1 extension support**: `XR_FB_foveation` is Meta-only on standalone but the Vulkan variant works on PC runtimes that expose it; gate behind capability check.
- **Beyond 2 specifics**: high-DPI panels make text rendering quality + foveation more impactful here than on Index.

## 6. Acceptance / QA Hooks

- Extend `/memories/repo/qa_polish_2026_04_29.md` style checklist for each batch as it ships.
- CI smoke-test (`--selftest --frames 30`) must continue to pass after every batch.
- Manual in-headset pass on Beyond 2 + at least one Lighthouse HMD per sprint.

## 7. Cross-References

- Master plan: [`Project artifacts/Plans/CitraVR_SteamVR_Port_Plan.md`](../Plans/CitraVR_SteamVR_Port_Plan.md) — add an Amendment block referencing this v1 plan.
- Renderer destructor blocker: `/memories/repo/renderer_dtor_root_cause.md`
- Status snapshot: `/memories/repo/citra_vr_pcvr_status.md`
- Last polish QA: `/memories/repo/qa_polish_2026_04_29.md`
