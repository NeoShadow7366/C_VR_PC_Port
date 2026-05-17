# CitraVR SteamVR PC Port — Copilot / Agent Instructions

> **Project**: Native Windows + SteamVR (OpenXR) port of [CitraVR](https://github.com/amwatson/CitraVR).
> **Targets**: Valve Index Knuckles, Bigscreen Beyond 2, generic OpenXR PCVR HMDs.
> **Repository root**: `G:\Citra VR PC Port\`

---

## 1. Mandatory Folder Rules (STRICT)

| Asset Type | Required Location |
|---|---|
| Plans | `Project artifacts\Plans\` |
| Documentation | `Project artifacts\Documents\` |
| Agent definitions | `.github\agents\` |
| Instructions | Repository root (per project convention) |
| Doc backups | `Project artifacts\Documents\dochistory\` (subfolder per area) |
| Plan backups | `Project artifacts\Plans\planhistory\` |

- **Filenames must be timestamped** for plans/docs that evolve: `YYYY-MM-DD_Topic_vN.md` (e.g. `2026-04-25_SteamVR_Port_Plan_v1.md`).
- **Never** dump plans, docs, or backups in the repository root or in `src/`.
- **History Cleaner** is the only agent that moves backups to `dochistory/` / `planhistory/`.

## 2. Master Reference

All agents and contributors MUST treat as authoritative:
[Project artifacts/Plans/CitraVR_SteamVR_Port_Plan.md](Project%20artifacts/Plans/CitraVR_SteamVR_Port_Plan.md)

Any deviation requires a new dated plan and a note in the master plan's "Amendments" section.

## 3. Agent System (summary — see full guide)

Full roster, hierarchy, invocation rules, and Mermaid flow:
[Project artifacts/Documents/agent-guide.md](Project%20artifacts/Documents/agent-guide.md)

**Read-only agents** (analyze + propose only):
Architecture Overviewer, Feature Investigator, Codebase Analyst, Codebase Documenter,
Porting Coordinator, Input & Controller Specialist, Rendering & Performance Engineer,
Testing & QA Lead, License & Compliance Guardian, Code Reviewer, Project Manager.

**Write-permitted agents**:
- **Doc Writer** — sole agent allowed to create/modify files in `docs/` and `Project artifacts\Documents\`.
- **History Cleaner** — moves backup files only; never edits content.
- **Build & CI Engineer** — may modify `CMakeLists.txt`, `CMakeModules/`, `.ci/`, `.github/workflows/`.

All other source-code edits go through human review; no agent commits production C++ unsupervised.

## 4. Code Style & Conventions

- Match upstream Citra style (`.clang-format` if present; otherwise Citra's existing pattern: 4-space indent, snake_case for functions/locals, PascalCase for types).
- Platform guards: prefer `#ifdef _WIN32` / `#ifdef ENABLE_OPENXR` over scattered runtime checks.
- New VR code lives under `src/video_core/renderer_vulkan/vr/` and `src/input_common/openxr/` (or equivalent — confirm with Porting Coordinator).
- Never remove Android/Quest code paths without explicit approval — keep them under `#ifdef ANDROID`.

## 5. Build & Toolchain

- **Build system**: CMake (Visual Studio 2022 generator, MSVC, x64).
- **Graphics**: Vulkan (primary). OpenGL legacy path retained but not VR-targeted.
- **XR**: OpenXR loader + SteamVR runtime. No Oculus/Meta-specific extensions in the Windows build.
- **Submodules**: `externals/openxr-sdk-src/` is the source of truth for OpenXR headers.

Build commands and CI live in [Project artifacts/Documents/maintenance.md](Project%20artifacts/Documents/maintenance.md).

## 6. Licensing

- Project remains **GPLv3** (Citra inheritance).
- All third-party additions must be GPLv3-compatible. License & Compliance Guardian reviews every new dependency.

## 7. Agent Interaction Summary

```
User → Porting Coordinator → (specialist read-only agents) → Codebase Documenter
                                                                    ↓
                                                              Doc Writer → History Cleaner
```

See [agent-guide.md](Project%20artifacts/Documents/agent-guide.md) for the full Mermaid diagram and protocol.

## 8. Companion Instruction Files

- [agent-files.instructions.md](agent-files.instructions.md) — rules governing `.agent.md` and `.instructions.md` files.
- [agent-guide.instructions.md](agent-guide.instructions.md) — how to author and invoke agents.
- [docs-workflow.instructions.md](docs-workflow.instructions.md) — documentation lifecycle.
