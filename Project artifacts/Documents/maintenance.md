# Repository Maintenance Procedures

**Scope:** CitraVR SteamVR PC Port (`G:\Citra VR PC Port\`)
**Owners:** Build & CI Engineer (build), Doc Writer + History Cleaner (docs), Project Manager (cadence).
**Master plan:** [../Plans/CitraVR_SteamVR_Port_Plan.md](../Plans/CitraVR_SteamVR_Port_Plan.md)

---

## 1. Build (Windows / SteamVR)

### Prerequisites
- Visual Studio 2022 (Desktop C++ workload, Windows 10/11 SDK, MSVC v143).
- CMake ≥ 3.22.
- Vulkan SDK ≥ 1.3 (LunarG).
- SteamVR installed; OpenXR runtime set to **SteamVR** in `Settings → OpenXR`.
- Git with submodules (`git submodule update --init --recursive`).

### Configure & Build
```powershell
cd "G:\Citra VR PC Port"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DENABLE_OPENXR=ON -DENABLE_VULKAN=ON -DCITRA_USE_BUNDLED_QT=ON
cmake --build build --config RelWithDebInfo --parallel
```

### Run
```powershell
.\build\bin\RelWithDebInfo\citra-qt.exe
```
Confirm SteamVR is running before launch (or use steamvr_null driver for headless tests).

---

## 2. CI

- Workflow files live under `.github/workflows/` (owned by Build & CI Engineer).
- Required jobs (target state):
  1. `windows-msvc` — configure + build RelWithDebInfo.
  2. `format-check` — clang-format / lint.
  3. `license-scan` — License & Compliance Guardian gate.
  4. `unit-tests` — `src/tests/` runner.
- Smoke job for OpenXR loader presence (no headset required) using `steamvr_null` or mock runtime.

---

## 3. Documentation Maintenance

### Daily / Per-change
- All doc edits go through **Doc Writer** (see [docs-workflow.instructions.md](../../docs-workflow.instructions.md)).
- Backups land in `Project artifacts/Documents/dochistory/<area>/` via **History Cleaner** immediately after.

### Weekly
- Project Manager produces `Project artifacts/Documents/status/YYYY-MM-DD_status.md`.
- Verify no `*-vYYYYMMDD-*.md` files remain in doc roots (run cleanup check below).

### Cleanup check
```powershell
Get-ChildItem -Path "Project artifacts\Documents" -Filter "*-v*-*.md" -File |
  Where-Object { $_.Directory.Name -ne 'dochistory' }
```
Any output = History Cleaner must run.

---

## 4. Plans Maintenance

- Master plan is append-only via Amendment blocks.
- New plans: `Project artifacts/Plans/YYYY-MM-DD_<Topic>_v<N>.md`.
- Superseded plans move to `Project artifacts/Plans/planhistory/<topic>/`.

---

## 5. Submodule / Externals Hygiene

- Update submodules only via a tracked plan (Build & CI Engineer authors it).
- After any `externals/` change, License & Compliance Guardian re-runs the dependency ledger.
- Never commit binary blobs into `externals/` — use the upstream submodule.

---

## 6. Release Checklist (per milestone)

1. Master plan amendment posted.
2. CI green on `windows-msvc`, `format-check`, `license-scan`.
3. Testing & QA Lead signs off the hardware matrix (Index + Beyond 2 + ≥1 other HMD).
4. License & Compliance Guardian confirms `license.txt` and NOTICE up to date.
5. Code Reviewer signs off the diff.
6. Doc Writer freezes versioned docs snapshot.
7. Tag: `vr-port-<milestone>-YYYYMMDD`.

---

## 7. Backup & Recovery

- Repo is the source of truth; rely on Git history.
- For documentation, history is preserved via timestamped backups in `dochistory/` / `planhistory/`.
- **Never** force-push the main branch.

---

## 8. Incident Response

| Signal | Owner | Action |
|---|---|---|
| Build break | Build & CI Engineer | Revert + dated post-mortem in `Documents/build-ci/` |
| Crash on Beyond 2 only | Rendering & Performance Engineer | Capture RenderDoc + OpenXR logs, file dossier |
| Knuckles input regression | Input & Controller Specialist | Re-run action manifest tests |
| License finding | License & Compliance Guardian | Block release until resolved |

---

## 9. References

- [agent-guide.md](agent-guide.md)
- [docs-workflow.instructions.md](../../docs-workflow.instructions.md)
- [copilot-instructions.md](../../copilot-instructions.md)
