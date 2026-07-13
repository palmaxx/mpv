# Phase 2 — Assessment Doc + ⛔ Reassessment Gate go/no-go

**Status:** ✅ COMPLETE and **GO** for Phase 3. The D3D11 surface backend
(`MPV_RENDER_API_TYPE_PL_D3D11`) now provably renders on a real D3D11 device:
`pl_d3d11_create` / `pl_d3d11_wrap` produce correct, deterministic, byte-stable
output through a WARP software-D3D11 self-baseline harness. The one runtime gap
P2.1's compile-validation could not catch (an `mp_assert(ctx->ra_ctx)` the
render backend tripped on a backend with no `ra_ctx`) is fixed in **P2.2**.

**Branch / range:** `gpu-next-render-api-hdr`, Phase-2 commits on top of the
Phase-1 tip `8903175` (pre-rebase) / `d73689b` (post-rebase):

| SHA | Commit | What | Diff |
|---|---|---|---|
| `98fbe55` | P2.1 | `libmpv_pl_context_d3d11` impl + `context_backends[]` registration | (full body, ~135 LOC) |
| `c0a347f` | P2.2 | gate render-backend hwdec registry on a surface `ra_ctx` | +20 / −13, 1 file |

The Phase-2 *deliverable verification* (the WARP harness + run script) lives in
the git-excluded `_golden/render_api/` on the Windows worktree
(`rapi_harness_d3d11.c`, `build_d3d11.sh`, `rapi_d3d11_run.sh`), parallel to the
WSL `pl-opengl` harness — test infrastructure, not committed source, exactly as
the GL harness is git-excluded.

## §1. What Phase 2 set out to do

Per [plan-hdr-render-api.md §"Phase 2"](plan-hdr-render-api.md): fill the
`libmpv_pl_context_d3d11.c` bodies (`init` → `pl_d3d11_create`, `wrap_fbo` →
`pl_d3d11_wrap`, `destroy`), register the backend, and **prove it actually
renders** via a deterministic software-D3D11 (WARP) self-baseline harness — the
one Plan-1/Phase-1 item the WSL software rig structurally cannot exercise
(`pl_has_d3d11=0` on WSL). P2.1 landed the code + compile-validation
(2026-06-01); this phase adds the runtime proof and the fix it surfaced.

## §2. P2.2 — the runtime gap P2.1's compile-validation could not catch

**Symptom.** The very first real `pl-d3d11` render context
(`rapi_harness_d3d11 --probe`) aborted in `mpv_render_context_create` with:

```
Assertion failed: ctx->ra_ctx, file ../video/out/gpu/hwdec.c, line 253
```

**Root cause.** `render_backend_gpu_next::init`
([libmpv_gpu_next.c](video/out/gpu_next/libmpv_gpu_next.c)) unconditionally
stood up the `ra_hwdec_ctx` registry via `ra_hwdec_ctx_init()`, whose first
statement is `mp_assert(ctx->ra_ctx)`. The hwdec interops are **ra-typed**
(`ra_hwdec_load_driver` consumes `ra_ctx->ra`), so the registry requires an
`ra`. The GL surface backend builds one alongside `pl_opengl` (F1.2), so the
assert held on `pl-opengl`. The D3D11 backend (P2.1) **by design** exposes only
a `pl_gpu` and no `ra_ctx` — render-API d3d11va interop is deferred to Plan-2
Phase 4 ([hdr-phase0-feasibility.md §5.3](hdr-phase0-feasibility.md)) — so the
assert fired on the first `pl-d3d11` create. WSL never saw this (the backend is
compiled out there); the Windows compile-validation never *ran* a context
(P2.1's gate was "compiles + links + `--version`", not "renders").

**Fix.** Gate the registry on `p->context->ra_ctx`:

```c
if (p->context->ra_ctx) {
    ctx->hwdec_devs = hwdec_devices_create();
    p->hwdec_ctx = (struct ra_hwdec_ctx){ ..., .ra_ctx = p->context->ra_ctx };
    ra_hwdec_ctx_init(&p->hwdec_ctx, ctx->hwdec_devs, gl_opts->hwdec_interop, true);
}
```

Surface backends that provide an `ra_ctx` (the GL backend) get hwdec **exactly
as before** — `pl-opengl` is byte-unchanged (proven, §3). A backend without one
runs **software-decode only**: the `talloc_zero`'d `hwdec_ctx` (num_hwdecs=0,
ra_ctx=NULL) makes the core's `core_hwdec_get → ra_hwdec_get` find no interop
(loops 0 times, never dereferences the NULL `ra_ctx`), and `ra_hwdec_ctx_uninit`
on the zero struct is a no-op. Verified by reading `ra_hwdec_get`
([hwdec.c:336](video/out/gpu/hwdec.c)) + confirming the core never calls
`ra_hwdec_ctx_load_fmt` (it only calls `fe.hwdec_get`, [core.c:1581](video/out/gpu_next/core.c)).
This is the correct shape regardless of Plan 2: hwdec interop *requires* an
`ra`, so a surface backend that has none must skip it.

**Merge-worthiness (Dudemanguy bar).** +20 / −13, one file, no API change, no
behaviour change on any existing path; the diff is the `if` wrapper +
indentation + an expanded comment documenting *why* (the ra-typed-interop
invariant), no comment churn elsewhere. Phase 4 replaces the `if` body's
condition, not its shape.

## §3. Verification — the full per-commit gate, both rigs

### Windows (the new signal): D3D11 SDR self-baseline

WARP (`D3D_DRIVER_TYPE_WARP`) is Microsoft's software rasterizer — a real
FL11_1 device (compute / UAV), deterministic run-to-run on one host — so the
D3D11 harness self-baselines exactly like the WSL `pl-opengl` one.

```
$ bash build_d3d11.sh
  built rapi_harness_d3d11.exe (links bld/libmpv-2.dll)
$ ./rapi_harness_d3d11.exe --probe
  mpv_render_context_create("pl-d3d11") -> 0 (success)   ← was: assert abort
  PROBE OK: pl-d3d11 render context created
$ bash rapi_d3d11_run.sh --baseline   # hdr10_4k.mp4 @0.5, @2.0 → SDR R8G8B8A8 1280x720
  OK sdr_t0  f861c22d7992d3ce
  OK sdr_t2  9c97482444b51104
$ bash rapi_d3d11_run.sh              # re-run ×2
  PASS sdr_t0 : pixels+meta identical
  PASS sdr_t2 : pixels+meta identical
  --- ALL PASS (D3D11 render-API path byte-stable) ---
```

**Content sanity (not just stability):** the @0.5 frame is real tone-mapped
video — 0.00% purple error-fill (so `valid==true`, the render genuinely
succeeded, not a cleared FBO), 18 132 distinct colors, mean RGBA ≈
(123,168,129,255). The HDR10 yuv420p10 source is decoded (SW), uploaded, and
tone-mapped to the SDR sRGB target through libplacebo-over-D3D11. This is the
first proof that `pl_d3d11_create`/`pl_d3d11_wrap` work in mpv, runtime, not
just at compile.

### WSL (the no-regression oracle): unchanged paths stay bit-identical

At `c0a347f`, fetched to `~/mpv-fork`, `ninja -C build`:

```
windowed lavapipe 12-case golden : 12/12 PASS (pixels+meta identical)
pl-opengl render-API harness      : 4/4  PASS (byte-stable self-baseline)
teardown (9 lifecycle cases)      : 9/9  CLEAN
```

The teardown's `interp motion_720p24` case failed exit=1 with **no** fatal
marker on the first run and passed clean on immediate re-run — the documented
WSLg-exhaustion flake (CLAUDE.md), on the windowed `--vo=gpu-next
--force-window` path that P2.2 does not touch (P2.2 is libmpv-render-backend
only). Not a regression.

## §4. ⛔ Reassessment Gate — the mandatory go/no-go

The plan requires a written answer to each before Phase 3.

1. **Did Phase 0 return ≥ medium confidence?** Yes — high, on the mpv-side
   work; the single biggest unknown (host HDR surface feasibility) is a Phase-5
   concern, not a Phase-3/4 blocker.
2. **Did Phase 1 land additively with no regression?** Yes — +411/−2,
   triple-green, byte-identical to the W6b baseline (hdr-phase1-assessment.md).
3. **Did the Phase-2 D3D11 SDR path produce visually-correct output AND
   self-baseline byte-stably?** Yes — §3: real tone-mapped video, 0% error
   fill, byte-identical across 3 runs on WARP.
4. **Is the TARGET_COLORSPACE design clear enough to lock Phase 3?** Yes — the
   param struct + mpv-side enum supersets shipped in P1.1 and were reviewed at
   the Phase-1 stop gate; the read site is already in `render()` (P1.3, the
   `(void)target_cs;` scaffold). Phase 3 is a localized replacement of that
   cast with the enum→`pl_color_space` translation + `apply_target_options`
   precedence, no surface churn.
5. **Effort for Phase 3+4+5 vs the fallback?** Phase 3 (wire the param) is
   small and self-baseline-gated. Phase 4 (HDR fidelity vs windowed on the
   NVIDIA rig) is the real cost and the real kill-point. The fallback
   (`--wid=HWND`) remains free and proven. Proceeding is cheap up to the Phase-4
   gate, which is where a genuine go/no-go on pixel-fidelity lands.

**Decision: GO.** All five green; no kill-point hit. Per the standing project
goal (develop Plan 2 commit-by-commit, golden-gated, defer only macOS
*testing*), proceed to Phase 3 (wire `TARGET_COLORSPACE` into the render hook)
rather than halting for a separate user re-decision. Phase 4 (HDR fidelity)
remains the hard kill-point; if it fails, abandon to `--wid=HWND`.

## §5. Phase 3 preview — what changes next

Per [plan-hdr-render-api.md §"Phase 3"](plan-hdr-render-api.md):

- In `render_backend_gpu_next::render`, replace the `(void)target_cs;` scaffold
  with: if `TARGET_COLORSPACE` present, map mpv enums → `pl_color_space`
  (static switch, no leaked libplacebo types) and write into `target.color`
  BEFORE `gpu_next_core_apply_target_options` runs; pass `hint = true` so user
  `--target-*` opts override only host-unpinned fields. When absent, keep the
  `pl_color_space_srgb` default (pl-opengl stays backwards-compatible).
- Plumb `target_csp` / `target_unknown` from the host data to
  `finalize_target_csp` (drives the Windows `target_pq` path).
- New harness cases: `pl-opengl` no-param byte-identical; `pl-d3d11` with
  `TARGET_COLORSPACE=sRGB` byte-identical to the P2.2 self-baseline (no-op
  default); `pl-d3d11` with `TARGET_COLORSPACE=BT.2020/PQ` renders to the
  requested target (manual check — washed/clamped on the SDR WARP readback is
  expected; true HDR fidelity is the Phase-4 NVIDIA gate).
