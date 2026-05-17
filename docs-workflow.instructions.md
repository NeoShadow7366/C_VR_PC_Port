---
description: "Use when creating, updating, backing up, or reorganizing any documentation or plan in the CitraVR SteamVR port. Defines folder layout, timestamped naming, backup rotation, and Doc Writer / History Cleaner handoff."
applyTo: "Project artifacts/**/*.md, docs/**/*.md"
---

# Documentation Workflow

## 1. Folder Layout (canonical)

```
Project artifacts/
├── Plans/
│   ├── CitraVR_SteamVR_Port_Plan.md           # master, never date-prefixed
│   ├── 2026-04-25_<Topic>_v1.md               # all other plans: dated
│   └── planhistory/<topic>/                   # rotated backups
└── Documents/
    ├── agent-guide.md
    ├── maintenance.md
    ├── architecture/
    ├── input/
    ├── rendering/
    ├── build-ci/
    ├── testing/
    ├── licensing/
    └── dochistory/<area>/                     # rotated backups
```

## 2. Naming Rules

| File Type | Pattern | Example |
|---|---|---|
| Master plan | fixed name | `CitraVR_SteamVR_Port_Plan.md` |
| New plan | `YYYY-MM-DD_<Topic>_v<N>.md` | `2026-04-25_Agent_System_Setup_Plan.md` |
| Living doc | stable name | `agent-guide.md` |
| Versioned snapshot | `<name>-v<YYYYMMDD>-<HHMM>.md` | `agent-guide-v20260425-1430.md` |
| Backup folder | lowercase, no spaces | `dochistory/architecture/` |

## 3. Update Lifecycle

1. **Generator agent** (e.g. Codebase Documenter) drafts content in chat.
2. **Doc Writer** (only writer) saves it:
   - If target file exists, first **rename** the existing file to `<name>-v<YYYYMMDD>-<HHMM>.md` in the same folder.
   - Then write the new content under the canonical name.
3. **Doc Writer** invokes **History Cleaner** which moves the `*-v*` backup into the matching `dochistory/<area>/` (or `planhistory/<topic>/`) subfolder.
4. The doc root stays clean — only canonical/current files remain.

## 4. Master Plan Discipline

- `CitraVR_SteamVR_Port_Plan.md` is **append-only via amendments**.
- Each amendment block:
  ```markdown
  ## Amendment YYYY-MM-DD — <Title>
  - Source plan: Plans/YYYY-MM-DD_<Topic>_vN.md
  - Summary: <one paragraph>
  ```
- Major rewrites require a new dated companion plan; the master only records the pointer.

## 5. Cross-Linking

- Use Markdown links with workspace-relative paths and URL-encoded spaces: `Project%20artifacts/...`.
- Every new document must link **back** to the master plan and the agent guide.

## 6. Forbidden in Doc Folders

- Source code, binary assets, screenshots > 1 MB (use `assets/` instead).
- Untimestamped duplicates of evolving plans.
- Backup files at the doc root (must be in `dochistory/` / `planhistory/`).
