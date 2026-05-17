---
description: "Use when invoking, selecting, chaining, or onboarding to the CitraVR SteamVR port agent system. Explains hierarchy, when to call which agent, handoff protocol, and the master plan reference."
---

# Agent Invocation Guide (Quick Rules)

> Authoritative roster: [Project artifacts/Documents/agent-guide.md](Project%20artifacts/Documents/agent-guide.md)
> Master plan: [Project artifacts/Plans/CitraVR_SteamVR_Port_Plan.md](Project%20artifacts/Plans/CitraVR_SteamVR_Port_Plan.md)

## 1. Decision Tree — Which Agent?

| User Need | Start With |
|---|---|
| "Explain the system / module to a non-engineer" | Architecture Overviewer |
| "How does feature X actually work end-to-end?" | Feature Investigator |
| "Walk me through this function / data flow" | Codebase Analyst |
| "Generate Markdown docs for this subsystem" | Codebase Documenter (then → Doc Writer) |
| "Save / update a documentation file" | **Doc Writer** (only writer) |
| "Tidy old backup files" | History Cleaner |
| "Plan / coordinate the port" | Porting Coordinator |
| "Fix CMake / CI / OpenXR loader" | Build & CI Engineer |
| "Knuckles / hand-tracking / action manifest" | Input & Controller Specialist |
| "Vulkan / swapchain / Beyond 2 / perf" | Rendering & Performance Engineer |
| "Test plan / regression / hardware matrix" | Testing & QA Lead |
| "License question / new dependency" | License & Compliance Guardian |
| "Review this PR / refactor" | Code Reviewer |
| "Status / milestones / risk" | Project Manager |

## 2. Hierarchy (top → bottom)

1. **Project Manager** + **Porting Coordinator** — orchestration tier
2. **Specialists** — Build/CI, Input, Rendering, Testing, License, Code Reviewer
3. **Investigators** — Architecture Overviewer, Feature Investigator, Codebase Analyst
4. **Documenters** — Codebase Documenter (generator) → **Doc Writer** (writer) → **History Cleaner** (janitor)

## 3. Handoff Protocol

- A read-only agent that needs to persist output **must** hand off to Doc Writer with:
  - Target filename (timestamped)
  - Target folder (`Project artifacts\Documents\<area>\`)
  - Backup policy (rename existing to `*-vYYYYMMDD-HHMM.md`)
- Doc Writer **must** invoke History Cleaner immediately after creating any backup.
- Any agent producing a plan **must** save under `Project artifacts\Plans\` with the date prefix.

## 4. Hard Rules

- A read-only agent that proposes a code change must produce a **diff or patch in Markdown**, never edit files directly.
- No agent edits `Project artifacts\Plans\CitraVR_SteamVR_Port_Plan.md` without an "Amendment" entry referencing a dated plan.
- Every agent response that touches the port plan must cite the master plan path.

## 5. Templates

All `.agent.md` files follow the body template defined in [agent-files.instructions.md](agent-files.instructions.md):
Purpose → Scope → Responsibilities → Constraints → Interaction Protocol → Output Format.
