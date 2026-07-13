# Phase 1 — Assessment Doc

**Status:** ✅ COMPLETE — incl. the residual Windows check. All three
commits landed and bit-faithfully gated on WSL. **Windows MSYS2
compile-validation DONE 2026-06-01** against `gpu-next-render-api-hdr`
tip `4ca0f77` (P2.1, which fills in the `libmpv_pl_d3d11.c` body, not
just the P1.2 stub): UCRT64 build with `d3d11:YES` + `pl_has_d3d11=1`,
`libmpv_pl_d3d11.c` compiles clean (114 KB obj), `libmpv-2.dll` + `mpv.exe`
link clean WITH the `context_backends[]` registration of
`libmpv_pl_context_d3d11` resolving (no undefined refs), `mpv --version`
runs, and `strings libmpv-2.dll` shows both `pl-opengl` and the new
`pl-d3d11` API type. `MPV_CLIENT_API_VERSION` = 2.6 in the build (P1.1
bump present). Worktree `c:\DEV\ai-dev\projects\mpv-wt-4ca0f77\bld`. The file's
functional/runtime behaviour (does `pl_d3d11_create`/`pl_d3d11_wrap`
actually render?) is still unproven — that's Phase 2's D3D11 SDR
self-baseline harness (`rapi_harness_d3d11.c` WARP host), not yet written.

**Branch / range:** `gpu-next-render-api-hdr`, three commits on top of
`c8e9298` (W6b tip):

| SHA | Commit | What | Diff |
|---|---|---|---|
| `e0d2f67` | P1.1 | public C-API surface | +334 / −2, 5 files |
| `f15815c` | P1.2 | `libmpv_pl_d3d11.c` stub | +74 / 0, 2 files |
| `8903175` | P1.3 | TARGET_COLORSPACE read | +3 / 0, 1 file |

Cumulative Phase 1: **+411 / −2 across 6 files**, all pure-additive, zero
behaviour change on the WSL build. The renderer is byte-identical to the
W6b baseline that signed off on real NVIDIA D3D11 HDR hardware.

## §1. Design intent (recap from [plan](plan-hdr-render-api.md) + [phase0 doc](hdr-phase0-feasibility.md))

Phase 1 was scoped as **scaffolding only**: lay down the C-API surface
and the stub file that Phase 2's actual D3D11 code will fill in. Every
commit is gated on three things:

1. The public API additions match the [hdr-phase0-feasibility.md](hdr-phase0-feasibility.md)
   §4 design verbatim (no leaked libplacebo types in `mpv/*.h`, mpv
   enums as a complete superset of libplacebo's, host-supplied target
   colorspace via a per-frame render param, not a per-context create
   param).
2. The new C-source file (P1.2) is the minimal mirror of
   [libmpv_pl_gl.c](video/out/opengl/libmpv_pl_gl.c) — same function
   signatures, same const struct shape — so Phase 2 fills in bodies
   without further structural churn.
3. No code path on the WSL build actually runs D3D11. WSL has
   `features['d3d11'] = NO` and `pl_has_d3d11 = 0`; both gates fail
   closed, the file is excluded from compilation, no symbols emit, no
   behaviour changes. Confirmed by `meson configure` output:
   `d3d11 : NO`.

## §2. Per-commit decisions + sanity checks

### P1.1 — public C-API surface (`e0d2f67`)

**Decisions made (cross-reference [hdr-phase0-feasibility.md §4](hdr-phase0-feasibility.md#4-mpv_render_param_target_colorspace-design)):**

- `MPV_RENDER_PARAM_TARGET_COLORSPACE` lands at enum value **21** — first
  free slot after `MPV_RENDER_PARAM_SW_POINTER = 20`. Same for
  `MPV_RENDER_PARAM_D3D11_INIT_PARAMS = 22` and `_D3D11_TEX = 23`. Per
  [hdr-phase0-feasibility.md §2a](hdr-phase0-feasibility.md#2a-adding-the-new-constants-is-abi-safe),
  adding enumerators at the end is ABI-safe; only the `0` terminator's
  value is guaranteed.
- `MPV_RENDER_API_TYPE_PL_D3D11` is the string `"pl-d3d11"`, mirroring
  `MPV_RENDER_API_TYPE_PL_OPENGL = "pl-opengl"`.
- `mpv_color_primaries` / `mpv_color_transfer` are **complete supersets**
  of libplacebo's `pl_color_primaries` / `pl_color_transfer` —
  cross-verified against
  [libplacebo/colorspace.h:200-256](/home/maxde/libplacebo/src/include/libplacebo/colorspace.h#L200);
  every libplacebo enumerator has a corresponding mpv enumerator. The
  mpv numeric values are **independent** of libplacebo's — Phase 3 uses
  a static switch translation, libplacebo bumps cannot break the mpv
  ABI.
- `mpv_hdr_metadata` field names match libplacebo's `pl_hdr_metadata`
  ([colorspace.h:412](/home/maxde/libplacebo/src/include/libplacebo/colorspace.h#L412))
  so Phase 3's translation is field-for-field.
- `pl_color_repr` (system / levels / bits / alpha) is **NOT** in the
  TARGET_COLORSPACE struct — derives from
  `MPV_RENDER_PARAM_DEPTH` (already plumbed) + the wrapped texture's
  DXGI format. Avoids a duplicate source-of-truth hazard.
- `mpv_d3d11_init_params::device` and `mpv_d3d11_tex::tex` are typed as
  `void *` (with documented cast intent) so the header compiles on
  non-Windows. Mirrors how
  [render_gl.h](include/mpv/render_gl.h)'s `mpv_opengl_init_params`
  uses generic `void*` callbacks instead of GL types.
- **API version bump** `2.5 → 2.6` (`include/mpv/client.h:251`). The
  same 2.6 row in `DOCS/client-api-changes.rst` retroactively documents
  W4's `MPV_RENDER_API_TYPE_PL_OPENGL` addition (it shipped without a
  bump — procedural gap caught by [hdr-phase0-feasibility.md §2b](hdr-phase0-feasibility.md#2b)).
  After reconfigure, `libmpv.so.2.6.0` is the new soname; confirmed.

**Sanity checks at commit time:**

- All three gates green (lavapipe 12/12, harness v2 4/4, teardown 9/9
  clean) — recorded in the commit message.
- `git diff --stat`: 334 lines added, 2 modified, no deletions. Minimal
  by Dudemanguy bar (no comment churn, no unrelated header rearrange).
- `git show e0d2f67 -- include/mpv/render.h` confirms only the three
  new enum entries + the three new structs + the new
  `MPV_RENDER_API_TYPE_PL_D3D11` define are added; no existing
  declarations modified.

### P1.2 — `libmpv_pl_d3d11.c` stub (`f15815c`)

**Decisions made:**

- File location: `video/out/d3d11/libmpv_pl_d3d11.c`, next to mpv's
  existing D3D11 unit (`ra_d3d11.c`, `context.c`). Parallels the
  GL backend at `video/out/opengl/libmpv_pl_gl.c`.
- All four `libmpv_pl_context_fns` hooks are stubbed:
  `init` / `wrap_fbo` return `MPV_ERROR_NOT_IMPLEMENTED`, `done_frame`
  is a no-op (no swapchain), `destroy` is a no-op (nothing allocated
  yet). Phase 2 fills the bodies.
- The const struct `libmpv_pl_context_d3d11` is declared but **NOT
  registered** in
  [libmpv_gpu_next.c::context_backends[]](video/out/gpu_next/libmpv_gpu_next.c#L39).
  Phase 2 adds the `extern` declaration + the `&libmpv_pl_context_d3d11`
  entry in the same commit as the init body. Until then, the symbol is
  unreachable from any C path.
- Meson dual-gates: `features['d3d11']` AND
  `libplacebo.get_variable('pl_has_d3d11', default_value: '0') == '1'`,
  nested inside the existing `features['d3d11']` block. Mirrors the
  exact pattern used for `libmpv_pl_gl.c`
  ([meson.build:1305-1314](meson.build#L1305)).
- The file `#include`s `<libplacebo/d3d11.h>` unconditionally —
  libplacebo's D3D11 header transitively includes `<windows.h>` and
  `<d3d11.h>`. On WSL the file is not compiled (meson gate); on the
  Windows MSYS2 rig the Windows headers are present. Mirrors how
  `libmpv_pl_gl.c` includes `<libplacebo/opengl.h>` unconditionally.

**Sanity checks at commit time:**

- `meson setup --reconfigure` output explicitly shows `d3d11 : NO` on
  WSL → the new source file is NOT in the build manifest. Confirmed by
  the rebuild touching only one .o file (`libmpv_gpu_next.c.o` was
  unaffected by P1.2; the build is incrementally the rebuild after
  meson reconfigure).
- All three WSL gates green.
- The file compiles to **zero bytes** of mpv runtime cost on WSL. The
  Windows compile validation cost ships on the next MSYS2 rig run.

**Residual risk (acknowledged):** the file body has not been
compile-validated. The Windows MSYS2 build at Phase 2 will exercise
the compile path for the first time; any pure-syntax errors will
surface there. Mitigation: the file is a near-verbatim copy of the
proven GL backend's structure; the only file-private symbols are the
four function bodies, all of which are trivial stubs. If the compile
fails, the diagnosis surface is tiny.

### P1.3 — TARGET_COLORSPACE param read (`8903175`)

**Decisions made:**

- Three lines added to
  [libmpv_gpu_next.c::render](video/out/gpu_next/libmpv_gpu_next.c#L241),
  immediately after the existing reads of `MPV_RENDER_PARAM_DEPTH` and
  `MPV_RENDER_PARAM_FLIP_Y`. Same lexical location → Phase 3's
  data-flow wiring is a one-line replacement of the `(void)target_cs;`
  cast with the actual use, no further reshuffling.
- The `(void)target_cs;` cast suppresses unused-variable warnings
  without using `__attribute__((unused))` (which would require a more
  invasive declaration shape). Standard idiom.
- No comments in the C source — the intent (read-without-use as a
  pre-wiring scaffold) lives in the commit message and in this
  assessment doc, NOT in code that would rot once Phase 3 lands. Per
  CLAUDE.md style.

**Sanity checks at commit time:**

- The render hook compiles clean (one file rebuilt).
- All three WSL gates green: pixels+meta identical to pristine — the
  param read is genuinely inert. No silent capture-side regression
  (the `_golden/baseline/*.json` sidecars include the rendered
  `target.color` representation; if the read had a side-effect, the
  HDR cases would differ).

**Residual risk:** zero. The read returns NULL (no host supplies the
param) and the result is cast to `void`. The renderer's `target.color
= pl_color_space_srgb` default at line 291 is untouched.

## §3. Cumulative Phase 1 verification

After all three commits, replayed every gate end-to-end against
pristine `35ae76d` baseline:

```
$ rm -rf _golden/cand && bash _golden/capture.sh ./build/mpv _golden/cand
  OK   sdr_t0  ... 12 cases ALL OK
$ bash _golden/verify.sh _golden/baseline _golden/cand
  PASS sdr_t0 : pixels+meta identical
  ... 12 cases ALL PASS
  --- 12 cases; ALL PASS (no regression) ---
$ bash _golden/render_api/rapi_run.sh
  --- ALL PASS (render-API path byte-stable) ---
$ bash _golden/teardown.sh ./build/mpv
  --- teardown: ALL CLEAN ---
```

Triple-green on `8903175` (Phase 1 tip), confirming the entire scaffold
is bit-faithful and the WSL build still proves it.

## §4. What Phase 1 did NOT do (and why)

By design, Phase 1 deliberately stops short of all of these:

- **No `context_backends[]` registration of D3D11.** The const struct
  exists; the entry doesn't. Reason: if registered with stub bodies, a
  host calling `mpv_render_context_create("pl-d3d11")` would succeed
  in `init` returning `MPV_ERROR_NOT_IMPLEMENTED`, then immediately
  fail — but the failure path involves a half-initialized
  `render_backend_gpu_next` which is fragile (W4 found out the hard
  way that `check_format` is called unconditionally — see HANDOFF row
  48). Phase 2 lands the registration ALONGSIDE the working init body,
  so the failure surface is "init returns error and is destroyed
  cleanly", not "init half-succeeded with a non-fatal but uninitialised
  pl_d3d11". This matches W4's actual landing pattern.
- **No use of `target_cs`.** Phase 3 wires it.
- **No `MPV_RENDER_PARAM_TARGET_COLORSPACE` write site in any test
  host.** The harness doesn't pass it; the windowed VO doesn't see
  it. Phase 3 adds the test cases (PL_OPENGL no-TARGET_COLORSPACE
  byte-identical; PL_OPENGL with TARGET_COLORSPACE=sRGB byte-identical
  to no-param baseline). Phase 4 adds the HDR cases.
- **No Windows-side compile validation.** Phase 2 owns this. The user
  builds on the MSYS2 rig with `pl_has_d3d11=1` and confirms
  `libmpv_pl_d3d11.c` compiles clean before merging Phase 2.

## §5. Stop gate — review checklist for the user

Before proceeding to Phase 2, confirm:

1. **API surface matches expectation.** Read
   [include/mpv/render.h](include/mpv/render.h) (the new enum entries,
   the new structs at lines 487-602) and
   [include/mpv/render_d3d11.h](include/mpv/render_d3d11.h) (the new
   header). Disagree with any field name, type, doc-comment? Now is
   the cheap time to push back — Phase 2's stub bodies + Phase 3's
   wiring all wash against this surface.
2. **API version bump strategy OK.** `2.5 → 2.6` is one bump covering
   W4's missed `PL_OPENGL` addition + the new `PL_D3D11` /
   `TARGET_COLORSPACE`. Alternative would have been 2.6 = W4 retroactive
   only + 2.7 = new HDR additions. Single bump is cleaner for an
   upstream submission later (no follow-up bump needed).
3. **No-comment-in-code style on P1.3 OK.** If you want a one-line
   `// applied in Phase 3` comment in the source instead of letting the
   commit message carry it, easy to amend. The CLAUDE.md style argues
   for terse code + documented commits.
4. **Windows compile validation gate scheduling.** Phase 2's first task
   is `git fetch` to the MSYS2 worktree + `meson setup bld --buildtype=
   release && ninja -C bld` to confirm `libmpv_pl_d3d11.c` compiles
   clean on the rig at `f15815c`. If it doesn't, Phase 2.0 is the fix
   commit before any new D3D11 code lands.

## §6. Phase 2 preview — what changes next

Per [plan-hdr-render-api.md §"Phase 2"](plan-hdr-render-api.md), Phase 2
is the first phase where actual D3D11 code runs:

- Fill `libmpv_pl_d3d11.c::init` body: read
  `MPV_RENDER_PARAM_D3D11_INIT_PARAMS` for the host's `ID3D11Device*`;
  verify feature level ≥ 11_0; call `pl_d3d11_create(pllog,
  &pl_d3d11_params{.device = host_device})`; populate `ctx->gpu`.
- Fill `wrap_fbo` body: read `MPV_RENDER_PARAM_D3D11_TEX`; call
  `pl_d3d11_wrap(ctx->gpu, &pl_d3d11_wrap_params{.tex = host_tex,
  .array_slice = 0})`; destroy the previous wrap on every call.
- Fill `destroy` body: tear down the wrap, then `pl_d3d11_destroy`, then
  `pl_log_destroy`.
- Register `&libmpv_pl_context_d3d11` in
  [libmpv_gpu_next.c::context_backends[]](video/out/gpu_next/libmpv_gpu_next.c#L39)
  behind `#if HAVE_D3D11 && defined(PL_HAVE_D3D11)`. mpv now matches
  `"pl-d3d11"` as a valid API type.
- **Phase 2 explicitly punts on hwdec.** Per
  [hdr-phase0-feasibility.md §5.3](hdr-phase0-feasibility.md#53-hwdec-interop-on-d3d11),
  the host's D3D11 device may differ from mpv's decoder D3D11 device.
  For the SDR self-baseline gate, Phase 2 ships with d3d11va hwdec
  disabled when API is `pl-d3d11` (documented limitation); Phase 4
  teaches the hwdec layer to consume the render-API device.
- New harness mode: `_golden/render_api/rapi_harness_d3d11.c` (or
  `--d3d11` flag on the existing harness) — drives `MPV_RENDER_API_TYPE_
  PL_D3D11` via WARP (`D3D_DRIVER_TYPE_WARP` is software D3D11,
  deterministic run-to-run) on the Windows MSYS2 rig. Self-baselines
  on first run; gates subsequent commits against the baseline.

Phase 2's gate is **two** check matrices:
- WSL: existing lavapipe 12-case golden + harness v2 self-baseline +
  teardown all green (no regression).
- Windows MSYS2: new D3D11 SDR harness self-baselines on Phase 2 tip
  → re-runs byte-identical.

**After Phase 2 → mandatory hard stop at the Reassessment Gate**
([plan-hdr-render-api.md](plan-hdr-render-api.md#-reassessment-gate-mandatory-hard-stop)).
Phase 3+ does not start without an explicit user re-decision.
