---
description: "Use when creating, modifying, reviewing, or debugging agent definition files (.agent.md) and instruction files (.instructions.md) in the CitraVR SteamVR port repository. Covers frontmatter, tool restrictions, naming, and read-only enforcement."
applyTo: "**/*.agent.md, **/*.instructions.md"
---

# Agent & Instruction File Rules

## 1. File Locations (enforced)

| File Pattern | Required Location |
|---|---|
| `*.agent.md` | `.github/agents/` |
| `copilot-instructions.md` | Repository root |
| `agent-*.instructions.md`, `docs-*.instructions.md` | Repository root (per project convention) |
| Topic-scoped `*.instructions.md` | `.github/instructions/` |

## 2. Required Frontmatter — Agents

```yaml
---
description: "Use when ... <keyword-rich trigger phrases>"
name: "Agent Display Name"
tools: [read, search]              # minimal set — see permissions matrix
model: "Claude Sonnet 4.5"         # optional
user-invocable: true               # false for orchestration-only subagents
---
```

## 3. Permissions Matrix (CitraVR project)

| Agent Class | Allowed Tools |
|---|---|
| Read-only analyst | `[read, search]` |
| Read-only with web research | `[read, search, web]` |
| Documenter (proposes only) | `[read, search]` |
| **Doc Writer** | `[read, search, edit]` — scope-limited via body |
| **History Cleaner** | `[read, search, execute]` — file-move only |
| **Build & CI Engineer** | `[read, search, edit, execute]` — scope-limited to build files |
| Project Manager / Coordinator | `[read, search, todo, agent]` |

> **Never** grant `edit` to a read-only agent. **Never** grant `execute` outside Build/CI/History Cleaner.

## 4. Mandatory Body Sections

Every `.agent.md` MUST contain, in order:

1. **Purpose** — one sentence
2. **Scope** — what is in/out
3. **Responsibilities** — bulleted
4. **Constraints** — explicit DO NOTs (incl. read-only enforcement)
5. **Interaction Protocol** — who calls this agent and who it hands off to
6. **Output Format** — exactly what it returns

## 5. Required Frontmatter — Instructions

```yaml
---
description: "Use when ... <keyword-rich>"
applyTo: "<glob or omit for on-demand>"
---
```

- Avoid `applyTo: "**"` unless the rule truly applies to every file (it burns context).
- One concern per file; do not mix testing rules with build rules.

## 6. Anti-patterns (auto-reject in review)

- Agent with `tools: [edit]` but description says "read-only".
- `.agent.md` outside `.github/agents/`.
- Missing **Constraints** section.
- Vague description ("a helpful agent").
- Circular handoffs (A → B → A) without exit criteria.
- Hardcoded absolute paths other than the documented `G:\Citra VR PC Port\` root.

## 7. Reference

Full roster and hierarchy: [Project artifacts/Documents/agent-guide.md](Project%20artifacts/Documents/agent-guide.md)
