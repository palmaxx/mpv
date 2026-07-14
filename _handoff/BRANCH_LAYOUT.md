# Fork branch layout

- `main`: validated LGPL-capable release source at
  `024e4c45f974e7dc418a4a32e1dbebd3aada588f`; keep it unchanged until the
  rebased candidate passes the remaining release gates.
- `agent/rebase-upstream-20260714`: clean rebased source candidate at
  `4347588dea`, based on upstream `e5486b96d7`.
- `agent/rebase-upstream-20260714-handoff`: the rebased candidate plus
  historical design/audit notes, reusable render-API harness sources, and the
  current macOS handoff.
- `agent/macos-handoff`: retained pre-rebase handoff history.
- `master`: retained as the old upstream mirror/history; it is not the release
  or development starting point.

Generated builds, validation captures, Windows binaries, media samples, and
temporary integration clones do not belong in Git branches. Keep the validated
Windows ZIP and its SHA-256 sidecar in the private local release archive.
