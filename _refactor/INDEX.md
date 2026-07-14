# `_refactor/` — index of the gpu-next render-API work

> **Current status (2026-06-20):** start with
> [`FINAL_PR_AUDIT.md`](FINAL_PR_AUDIT.md). The frozen fork-review snapshot is
> `a8c7cfe7812e3d828eb603397860500a1569c512` in the clean standalone clone
> `C:\DEV\ai-dev\projects\mpv-src\_upload\mpv`. CodeRabbit and the full
> engineering audit are closed; substantive GitHub Actions checks pass.
> Post-freeze ARCA validation found that the snapshot initializes D3D11VA but
> fails to advertise the hardware format, forcing a CPU download. Local additive
> commit `f728638bc1` fixes and validates zero-copy without changing the frozen
> GitHub branch. Vulkan exposes a
> generic libplacebo RA for compatible interops, but VideoToolbox-over-MoltenVK
> remains unvalidated and true FFmpeg Vulkan Video decode is deferred. WSL is
> currently unavailable because no distribution is installed.
>
> The branch maps, SHAs, WSL workflow, and phase status below are retained as a
> historical implementation/validation record. Where they conflict with the
> final audit, the final audit is authoritative; do not delete the older detail.

Master entry point for the refactor docs (all git-excluded, per-developer;
see `CLAUDE.md` at the repo root for the auto-loaded summary). Reorganized
2026-06-01 into **Plan 1 (done)** vs **Plan 2 (active)**.

> **For a merge/audit of both plans into upstream mpv, start with
> [`MERGE_AUDIT_HANDOFF.md`](MERGE_AUDIT_HANDOFF.md)** (2026-06-04): the full
> technical walkthrough, diffstat, per-backend detail, deviations, deferrals,
> PR-split guidance, and an auditor checklist.

## Two plans, in order

1. **`plan1-gpu-next/` — DONE.** Expose mpv's gpu-next (libplacebo) renderer
   through the libmpv render API, **GL context-fns only** (`pl-opengl`), as a
   merge-worthy personal fork with zero windowed-path regression. Extracted
   `vo_gpu_next.c`'s libplacebo core into a shared `gpu_next/core` consumed by
   both the windowed VO and the new `render_backend_gpu_next`. **Complete,
   including hwdec** (the F-phase — an undocumented gap in the original spec,
   now filled). See `plan1-gpu-next/plan.md` → "Completeness assessment".
2. **`plan2-hdr-render-api/` — ACTIVE (next).** HDR over the render API:
   per-GPU-API surface-wrap backends (D3D11 → Vulkan → Metal) + a public
   `MPV_RENDER_PARAM_TARGET_COLORSPACE` param + per-backend SW/HW harnesses.
   The GL render path is structurally HDR-incapable (8-bit host FBO, no
   swapchain to negotiate target colorspace); this plan adds the backends that
   can carry HDR. Scaffolding (P1.1–P1.3) + the D3D11 impl (P2.1) + the
   runtime-proven D3D11 path (P2.2 + WARP self-baseline harness) are in;
   **Phase 2 done, Reassessment Gate = GO** (`hdr-phase2-assessment.md`).
   **Phase 3 done** (P3.1: `TARGET_COLORSPACE` wired into the render hook —
   `hdr-phase3-assessment.md`). **Phase 4a done** (HDR render path proven on
   WARP: R10G10B10A2/PQ, genuine 10-bit HDR, deterministic —
   `hdr-phase4-assessment.md`); Phase 4b (on-display fidelity vs windowed) is
   user-run. **Phase 6 Vulkan backend done** (V1.1+V2.1: `pl_vulkan_import` +
   wrap + acquire/hold sync, **proven byte-stable on lavapipe** —
   `hdr-phase6-assessment.md`). **macOS = MoltenVK via the same pl-vulkan
   backend** (the goal's "molten vk for mac"); native Metal is Mac-required
   (libplacebo `metal.h` ships only on macOS) and deferred.

## Current branch map (WSL fork `~/mpv-fork`, rebased onto upstream 2026-06-01)

| Branch | Tip | What |
|---|---|---|
| `gpu-next-render-api` | `82a1428` | **Plan 1 deliverable** (W1–W6b + F1.1–F1.3 hwdec + review fixes #5 `1dc4802` / #2 `82a1428`). On upstream `f239c2dd`. Gated: 12/12 lavapipe golden + teardown + 4/4 render-API harness. |
| `gpu-next-render-api-hdr` | `65dc39e` | **Plan 2 base** = Plan 1 + F1.3 + P1.1–P3.1 + V1.1/V2.1. D3D11 done (renders + HDR PQ path proven, WARP; P2.2 hwdec fix) + **TARGET_COLORSPACE** wired (P3.1) + **Vulkan backend done** (V2.1: `pl_vulkan_import`/wrap + acquire/hold sync, **proven byte-stable on lavapipe**). On `82a1428`. WSL no-regression green; D3D11 on Windows rig, Vulkan on lavapipe. |
| `bak/gpu-next-render-api-20260601` | `286b788` | pre-upstream-rebase backup of Plan 1 |
| `bak/gpu-next-render-api-hdr-20260601` | `4ca0f77` | pre-rebase backup of the HDR branch |
| `gpu-next-render-api-pre-rebase` | `8a2df0e` | older (pre-2026-05-24-rebase) backup |

Remotes: `upstream` = real mpv (`mpv-player/mpv`); `origin` =
`/mnt/c/DEV/ai-dev/projects/mpv-src` (the Windows clone — bidirectional sync,
see workflow). Re-rebase onto `upstream/master` before any PR.

## Where to work (the dual-tree model)

Plan 2 is **Windows/D3D11-centric** — the D3D11 backend only compiles where
`pl_has_d3d11=1`, which is the **Windows MSYS2** rig, NOT WSL (WSL libplacebo
has d3d11 off, so `libmpv_pl_d3d11.c` is excluded from the WSL build entirely).
So:

- **WSL `~/mpv-fork`** = canonical git history + the **lavapipe no-regression
  oracle**. After any Plan-2 commit, fetch it here and run the golden gate to
  prove the windowed / `pl-opengl` paths didn't regress (the deterministic
  lavapipe gate is irreplaceable). Build: `ninja -C build`.
- **Windows clone `C:\DEV\ai-dev\projects\mpv-src`** (worktree per the Windows
  `_refactor/`) = the **Plan-2 dev + test surface**: write the D3D11 backend +
  harness, build on MSYS2 UCRT64, run the D3D11 SDR (WARP) + HDR (NVIDIA)
  harnesses + `run-hdrval`.
- **Sync**: Windows fetches WSL over UNC
  (`\\wsl.localhost\Ubuntu\home\maxde\mpv-fork`); WSL fetches Windows via
  `git fetch origin` (`/mnt/c/...`). Author wherever the build/test loop is
  cheapest (Plan-2 D3D11 work → Windows), then sync the other way for its gate.

## Validation gates

- **Windowed `--vo=gpu-next` no-regression (mandatory):** WSL lavapipe
  12-case golden (`_golden/capture.sh` + `verify.sh`) + `teardown.sh`.
- **Render-API `pl-opengl` no-regression:** `_golden/render_api/rapi_run.sh`
  (self-baselined, software EGL).
- **Real-HDR (windowed):** Windows `_refactor/scripts/run-hdrval.ps1`
  (NVIDIA D3D11, display in HDR mode) — SW + `-Hwdec`.
- **Plan 2 D3D11 (to build):** a `rapi_harness_d3d11` (WARP SDR self-baseline +
  NVIDIA HDR) — this is also where render-API **hwdec gets its functional
  sign-off** (the one Plan-1 item the software WSL rig can't prove).

## Doc map

- `plan1-gpu-next/plan.md` — Plan 1 spec + **Completeness assessment** (the
  authoritative "Plan 1 is done" statement).
- `plan1-gpu-next/HANDOFF.md` — full Plan-1 + F-phase implementation log
  (every commit). SHA citations are pre-rebase; map by commit subject.
- `plan1-gpu-next/GPU_NEXT_VALIDATION_ISSUES.md` — **prior independent
  review of F1.3.** Flags real items beyond the hwdec gap: (#1) missing
  native-resource forwarding for Linux GL hwdec (Windows unaffected), (#2)
  `VO_CAP_FILM_GRAIN` advertised but ineffective on the render API (worth
  fixing), (#5) dead `gpu_next_core_renderer()`, (#4) upstream comment/
  trailer hygiene. Folded into `plan.md` → "Known issues before upstream
  merge". None block the Windows/D3D11 endpoint.
- `plan1-gpu-next/phase0-feasibility.md`, `phase1-golden-harness.md`,
  `phase1-reassessment-gate.md`, `phase2-hdr-validation-plan.md`,
  `phase2-msys2-hdr-runbook.md`, `ephase-e5-design.md` — Plan-1 phase docs.
- `plan2-hdr-render-api/plan-hdr-render-api.md` — Plan 2 spec.
- `plan2-hdr-render-api/hdr-phase0-feasibility.md` — Plan 2 feasibility.
- `plan2-hdr-render-api/hdr-phase1-assessment.md` — Plan 2 Phase-1 assessment
  (incl. the Windows compile-validation Status, done 2026-06-01).
- `plan2-hdr-render-api/hdr-phase2-assessment.md` — Plan 2 Phase-2 assessment +
  the **Reassessment Gate go/no-go = GO** (WARP D3D11 self-baseline proof + the
  P2.2 hwdec-registry fix; done 2026-06-02).
- `plan2-hdr-render-api/hdr-phase3-assessment.md` — Plan 2 Phase-3 assessment
  (P3.1: `TARGET_COLORSPACE` wired into the render hook; sRGB-no-op +
  BT.2020/PQ gated on WARP; pl-opengl byte-unchanged; done 2026-06-02).
- `plan2-hdr-render-api/hdr-phase4-assessment.md` — Plan 2 Phase-4: **4a + 4b
  done/GREEN**. 4a: HDR render path on WARP (R10G10B10A2/PQ, genuine 10-bit,
  deterministic). 4b (NVIDIA HDR rig, 2026-06-03): windowed real-HDR
  no-regression byte-identical (SW+HW); render-API HDR target negotiation
  bit-identical to the windowed swapchain (`video-out-params`); actual NVIDIA
  render-API HDR render genuine PQ + deterministic. `--wid=HWND` not needed.
- `plan2-hdr-render-api/hdr-phase5-assessment.md` — Plan 2 Phase-5a: **bare-Win32
  HDR present host VISUALLY CONFIRMED** vs windowed on the NVIDIA HDR display
  (2026-06-04). Key host-integration finding: the HDR host MUST query the
  display peak (`IDXGIOutput6::GetDesc1().MaxLuminance`) and pass it in
  `TARGET_COLORSPACE` (windowed does it via `--target-colorspace-hint`); HDR
  screenshots (Snipping Tool / OS SDR grab) are not a valid comparison. The 5b
  shell port reference.
- `plan2-hdr-render-api/hdr-phase5b-assessment.md` — Plan 2 Phase-5b
  (2026-06-12): **first production shell exists (ARCA, `C:\DEV\ARCA`)**; four
  findings from real product paths. **The engine-side defect P5b.1 is FIXED
  + GATED same day (`008434c` code + `dc7b021` docs, §7)**: the backends
  held their host-texture wrap (D3D11: one COM ref) across render calls, so
  direct backbuffer hosts could never `ResizeBuffers` — wraps are now
  caller-owned and released by end-of-render in all three backends (Vulkan
  handshake on a non-owning pointer; failed-hold orphan recovery preserves
  the `304f611` never-destroy-released rule). Host-contract amendments
  (high-performance adapter on hybrid GPUs; HMONITOR-based display-peak
  query — `GetContainingOutput` fails cross-adapter) + the libplacebo
  `blit_dst` limitation (`--border-background=none` + host clears; the
  render hook's purple error clear is now gated) are in the public
  render_d3d11.h and demonstrated by the updated `rapi_hdr_present.c`.
  Gate set grew two permanent cases: WARP `--refcount` (host sole owner
  post-render; fails on `304f611`) and `rapi_hdr_present --resize-test`
  (6/6 mid-playback ResizeBuffers on the NVIDIA rig). All gates green:
  WARP 6/6, WSL golden 12/12 + teardown + pl-opengl 4/4 + lavapipe-vk 4/4,
  NVIDIA Vulkan 4/4 byte-identical pre/post, hdrval SW+HW `46CDAFFF…`.
  ARCA re-vendored at `dc7b021` (`hdr-verify` exit 0); its ADR-004
  intermediate-texture workaround is now retirable (ARCA-side follow-up).
- `plan2-hdr-render-api/hdr-phase6-assessment.md` — Plan 2 Phase-6: **Vulkan
  backend done** (V1.1+V2.1: `pl_vulkan_import`/wrap + the acquire/hold
  external-sync handshake, proven byte-stable on lavapipe — a stronger oracle
  than WARP); **macOS via MoltenVK** = the same pl-vulkan backend; native Metal
  deferred (Mac-required — libplacebo `metal.h` is macOS-only).
- `plan2-hdr-render-api/hdr-phase7-hwdec-plan.md` — Plan 2 Phase-7 **(PLAN,
  not started)**: hardware decoding on the render API, closing
  `MERGE_AUDIT_HANDOFF.md` §9.1. Thesis: the render hook's hwdec machinery is
  already generic and gated only on the backend exposing a `ra_ctx` (P2.2);
  the GL F-phase proves the core path. **7a (D3D11/`d3d11va`, primary):** have
  `libmpv_pl_d3d11.c::init` build a `ra_ctx` via `ra_d3d11_create` on the
  **host's** device (render device == decode device by construction, so §9.1's
  "thread the decode device through" dissolves for D3D11) — mirrors the GL
  backend, no render-hook change expected. The one real risk is the
  decoded-frame→`pl_tex` bridge (verify first; GL is the existence proof it's
  core-level/backend-agnostic). Functional sign-off is NVIDIA-only (WARP has no
  HW decoder) — cheapest harness is the consumer ARCA (`hdr-verify` reports
  `hwdec-current` + decoder drops; flip its ADR-003 `hwdec=no`). **7b (Vulkan):**
  same shape via `ra_vk` on the imported `VkDevice`, but the decode device
  threading is real there; deferred. Lands → retires ARCA ADR-003.
- `scripts/run-hdrval.ps1` — Windows HDR validation (reference copy; the
  canonical one the user runs lives in the Windows clone root).
