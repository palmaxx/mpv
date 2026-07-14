# Fork branch layout

- `main`: clean audited LGPL-capable source at
  `024e4c45f974e7dc418a4a32e1dbebd3aada588f`.
- `agent/macos-handoff`: `main` plus historical design/audit notes, reusable
  render-API harness sources, and the current macOS agent handoff.
- `master`: retained as the old upstream mirror/history; it is not the release
  or development starting point.

Generated builds, validation captures, Windows binaries, media samples, and
temporary integration clones do not belong in Git branches. Keep the validated
Windows ZIP and its SHA-256 sidecar in the private local release archive.
