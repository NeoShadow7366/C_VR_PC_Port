# Agent System Setup Plan — v1

**Date:** 2026-04-25
**Author:** Repository Governance Architect (Copilot)
**Status:** Established
**Master plan:** [CitraVR_SteamVR_Port_Plan.md](CitraVR_SteamVR_Port_Plan.md)

---

## 1. Objective

Establish the complete agent system, instruction files, and documentation scaffolding required to govern the long-term port of CitraVR to native Windows + SteamVR (OpenXR), with first-class support for Valve Knuckles and Bigscreen Beyond 2.

## 2. Scope

In scope:
- 14 named agents covering investigation, specialization, documentation, and orchestration.
- Root-level instruction files governing agent behavior, file rules, and docs workflow.
- Canonical folder structure under `Project artifacts/Documents/`.
- Mermaid hierarchy + interaction sequence.

Out of scope (this plan):
- Source-code changes to the Citra core or VR layer.
- CI workflow YAML (Build & CI Engineer to author separately).

## 3. Deliverables (this plan ships)

| # | Path | Purpose |
|---|---|---|
| 1 | `copilot-instructions.md` | Project-wide agent guidance |
| 2 | `agent-files.instructions.md` | Rules for `.agent.md` / `.instructions.md` |
| 3 | `agent-guide.instructions.md` | How to invoke agents |
| 4 | `docs-workflow.instructions.md` | Documentation lifecycle |
| 5 | `Project artifacts/Documents/agent-guide.md` | Authoritative agent roster + hierarchy |
| 6 | `Project artifacts/Documents/maintenance.md` | Repo maintenance procedures |
| 7 | `.github/agents/*.agent.md` | 14 individual agent definitions |
| 8 | `Project artifacts/Plans/2026-04-25_Agent_System_Setup_Plan.md` | This plan |

## 4. Agent Roster (final)

| Tier | Agent | Class |
|---|---|---|
| 1 | Project Manager | read-only |
| 1 | Porting Coordinator | read-only |
| 2 | Build & CI Engineer | scoped writer |
| 2 | Input & Controller Specialist | read-only |
| 2 | Rendering & Performance Engineer | read-only |
| 2 | Testing & QA Lead | read-only |
| 2 | License & Compliance Guardian | read-only |
| 2 | Code Reviewer | read-only |
| 3 | Architecture Overviewer | read-only |
| 3 | Feature Investigator | read-only |
| 3 | Codebase Analyst | read-only |
| 4 | Codebase Documenter | read-only (generator) |
| 4 | Doc Writer | sole docs writer |
| 4 | History Cleaner | scoped mover (backups only) |

## 5. Folder Structure Created

```
Project artifacts/
├── Plans/
│   ├── CitraVR_SteamVR_Port_Plan.md           (existing, master)
│   ├── 2026-04-25_Agent_System_Setup_Plan.md  (this file)
│   └── planhistory/                           (created on first rotation)
└── Documents/
    ├── agent-guide.md
    ├── maintenance.md
    ├── architecture/   (placeholder, populated by Codebase Documenter)
    ├── input/
    ├── rendering/
    ├── build-ci/
    ├── testing/
    ├── licensing/
    ├── status/
    └── dochistory/                            (created on first rotation)
```

## 6. Hard Rules Enforced

1. Read-only agents never write files; they propose to Doc Writer.
2. Doc Writer is the sole writer of documentation files.
3. History Cleaner runs after any documentation backup is produced.
4. All agents reference the master port plan when relevant.
5. Plans are timestamped (`YYYY-MM-DD_<Topic>_v<N>.md`); the master plan is append-only via Amendments.
6. Build & CI Engineer's write scope is restricted to `CMakeLists.txt`, `CMakeModules/`, `.ci/`, `.github/workflows/`.

## 7. Interaction Flow

See full Mermaid diagrams in [../Documents/agent-guide.md](../Documents/agent-guide.md). Canonical path:

```
User → Porting Coordinator → Specialist(s) → Codebase Documenter → Doc Writer → History Cleaner
```

## 8. Acceptance Criteria

- [x] All 4 root instruction files exist.
- [x] `agent-guide.md` and `maintenance.md` exist under `Project artifacts/Documents/`.
- [x] All 14 `.agent.md` files exist under `.github/agents/`.
- [x] Each agent file follows the standard body template.
- [x] Permissions matrix matches the agent guide.
- [x] This plan is dated and stored in `Project artifacts/Plans/`.

## 9. Next Actions (post-setup)

1. Porting Coordinator authors `2026-04-26_Port_Roadmap_v1.md` referencing the master plan.
2. Build & CI Engineer drafts `Project artifacts/Documents/build-ci/windows-toolchain.md` (via Codebase Documenter → Doc Writer).
3. Input & Controller Specialist drafts the Knuckles + hand-tracking action manifest plan.
4. Rendering & Performance Engineer drafts the Vulkan↔OpenXR swapchain plan with Beyond 2 specifics.
5. License & Compliance Guardian initializes the dependency ledger.

## 10. Amendments

_None yet._
