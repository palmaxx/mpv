# Phase 0 — AI Feasibility Report: `vo_gpu_next.c` → render-API extraction

Read-only static analysis. Scope: assess feasibility of extracting the libplacebo
rendering core out of `video/out/vo_gpu_next.c` into a shared unit consumed by
BOTH the windowed `vo_gpu_next` VO and a new `render_backend_gpu_next`, mirroring
the existing `render_backend_fns` + `libmpv_gpu_context_fns` two-layer model
(reference impl: `vo_gpu.c` + `gl_video` core + `libmpv_gpu.c`/`libmpv_gl.c`).

## Tooling caveat (affects §2 only)

`Bash`/`PowerShell` were fully denied this session, so `gh pr diff 16818` and
`git log/show` could not be run, and `WebFetch` of `github.com/.../16818.diff`
was denied. The PR #16818 salvage audit (§2) is therefore reconstructed from:
the maintainer review quotes in the task brief, the target-architecture contract,
the mainline code that #16818 must integrate against (fully read), and the
**structural fingerprint #16818 left in this very working tree**. This is
explicitly weaker evidence than the raw diff for exact %; the *architectural*
verdict (why it violates the model) is robust regardless. Treat the numeric
estimate in §2 as bounded, not measured. Everything in §1, §3, §4, §5 is from
full first-hand reads of mainline source and is firm.

### Provenance correction (important, changes how the plan reads)

`video/out/gpu_next/context.{c,h}` **already exists in mainline mpv** and is the
*existing* windowed-VO swapchain bring-up layer (`struct gpu_ctx`,
`gpu_ctx_create`/`gpu_ctx_resize`/`gpu_ctx_destroy`). `vo_gpu_next.c:52`
`#include "gpu_next/context.h"` and `preinit` (line 2242) calls `gpu_ctx_create`.
It is GL/D3D11/Vulkan swapchain *creation* glue — NOT render-API code, NOT a
#16818 artifact. The maintainer objection "GL code shouldn't be all over
gpu_next/context.c" means #16818 *added libmpv render-API GL paths into this
file*; it does not mean the directory itself is the parallel tree. The
refactor's job is the opposite: keep `gpu_ctx` as the windowed shell's private
swapchain helper, and add a separate libplacebo `libmpv_gpu_context_fns` for the
render-API surface-wrap — do not touch `gpu_next/context.c` for render-API code.

---

## §1. Extraction blast-radius map

Legend:
**(A)** cleanly extractable into shared libplacebo-core unit (no `vo`, no
`ra_ctx`, no swapchain);
**(B)** windowed-VO / `gpu_ctx` / swapchain / `vo_driver`-specific → stays in
the thin VO shell (the render backend supplies its own equivalent);
**(C)** entangled / ambiguous → the actual risk; needs an explicit seam.

Quantitative coupling (mechanical, `vo_gpu_next.c`): 102 references to
`vo->*` / `p->ra_ctx` / `p->context` / `p->sw` / `sw->fns` / `p->gpu` /
`p->pllog` / queue-param / vo-event helpers; 48 references to `struct vo *` /
`vo->priv` / `fp->vo`. The `vo` pointer is threaded into the **render hot path**
via `frame_priv.vo` (line 465) and recovered inside libplacebo map/acquire
callbacks (`map_frame` 701, `hwdec_acquire` 637, `unmap_frame` 854,
`info_callback` 871). This callback re-entrancy is the central extraction
problem: libplacebo calls back into mpv code that currently dereferences `vo`.

### 1a. `struct priv` (vo_gpu_next.c:107–170), field by field

| Field (line) | Class | Rationale |
|---|---|---|
| `log` 108 | A | Pass into core ctx; not vo-specific. |
| `global` 109 | A | Same. |
| `stats` 110 | A | `stats_ctx`; core-owned, keyed by name string. |
| `ra_ctx` 111 | **B** | Swapchain/context. Shell-owned. Core must not see it (note §1c hazards: 4 core fns reach through it today). |
| `context` (`gpu_ctx`) 112 | **B** | `gpu_ctx_create`. Pure windowed swapchain bring-up. Shell-only. |
| `hwdec_ctx` 113 | C | `ra_hwdec_ctx` built from `ra_ctx` (preinit 2251). hwdec interop is core-needed (map path) but constructed from shell `ra_ctx`. Ownership must move to core with `ra` injected by shell/backend. |
| `hwdec_mapper` 114 | C | Created in `hwdec_reconfig` (581) from `p->ra_ctx->ra` (line 586). Core logic, shell dependency. |
| `hwdec_timer` 115, `hwdec_perf` 116 | C | `timer_pool_create(p->ra_ctx->ra)` (586). Core perf, shell `ra`. |
| `sw_upload_timer` 117, `sw_upload_perf` 118 | C | `timer_pool_create(p->ra_ctx->ra)` (771). Same pattern. |
| `dr_lock`,`dr_buffers`,`num_dr_buffers` 121–123 | A | DR is pure `pl_buf` on `p->gpu`. No vo/swapchain. Cleanly core. |
| `pllog` 125 | A* | libplacebo log. Core-owned conceptually, but *value* is produced by `gpu_ctx`/backend ctx → injected, not created by core. |
| `gpu` 126 | A* | `pl_gpu`. The core's primary handle. Value injected by shell/backend (windowed: `gpu_ctx.gpu`; render: pl-wrapped user GL). |
| `rr` 127 | A | `pl_renderer_create`. Pure core. |
| `queue` 128 | A | `pl_queue`. Pure core (interpolation). |
| `sw` (`pl_swapchain`) 129 | **B** | libplacebo swapchain. Windowed-only. The render backend has **no `pl_swapchain`** — it renders into a wrapped user FBO. This field's use inside `draw_frame` is the #1 NO-GO risk (§3.1). |
| `osd_fmt` 130 | A | `pl_find_named_fmt` on `p->gpu`. Core. |
| `sub_tex`,`num_sub_tex` 131–132 | A | OSD texture pool on `p->gpu`. Core. |
| `src`,`dst` 134 | C | `mp_rect`. Set by `resize()` from `vo_get_src_dst_rects(vo,...)` (1551). Geometry is core-consumed (`apply_crop`), source is vo. Must become an input struct the shell/backend fills (render backend gets it via `render_backend_fns.resize`). |
| `osd_res` 135 | C | Same as src/dst — vo-derived, core-consumed. |
| `osd_state` 136 | A | `struct osd_state` (pl textures). Core. (OSD *content* via `vo->osd` is the seam, see funcs.) |
| `last_id`,`osd_sync`,`last_pts`,`is_interpolated`,`want_reset`,`flush_cache`,`frame_pending`,`paused` 138–145 | A/C | Pure render-state machine ⇒ A, EXCEPT `frame_pending` 144 (consumed only by windowed `flip_page` 1516 → B-adjacent) and `is_interpolated` 141 (read by windowed `control` VOCTRL_PAUSE 1892; render backend has no flip/pause-redraw → behaviour differs harmlessly but must be a documented seam). |
| `pars` (`pl_options`) 147 | A | Core render params. |
| `opts_cache`,`next_opts_cache`,`next_opts` 148–150 | A | `gl_video_conf`/`gl_next_conf` caches. Core (mirror `gl_video` which owns its opts). |
| `shader_cache`,`icc_cache` 151 | A | `pl_cache` on `p->gpu`+`pllog`. Core. |
| `video_eq` 152 | A | `mp_csp_equalizer`. Core (consumed in `update_options` 903). |
| `scalers` 153 | A | `map_scaler` scratch. Core. |
| `hooks` 154 | A | `pl_hook*` storage. Core. |
| `output_levels` 155 | C | Written in `update_options` 909; **read in `set_colorspace_hint` 1032 from the swapchain path** and in `apply_target_options` 954. Couples options→swapchain-hint. |
| `icc_params`,`icc_path`,`icc_profile` 157–159 | A* | Core libplacebo ICC. BUT auto-profile ingest is **B** (queries `p->ra_ctx->fns->control(VOCTRL_GET_ICC_PROFILE)` in `update_auto_profile` 1605). Profile bytes arrive shell-side; ICC *application* is core. |
| `user_hooks`,`num_user_hooks` 162–163 | A | Compiled user shaders. Core. |
| `perf_fresh`,`perf_redraw` 166–167 | A | `pl_dispatch_info`. Core; surfaced via `VOCTRL_PERFORMANCE_DATA` (shell) and would map to `render_backend_fns.perfdata`. |
| `target_params` 169 | C | Written in `draw_frame` 1481 under `vo->params_mutex`, published as `vo->target_params` (1490). Core computes it; publishing is vo-API. Mirror `gl_video_get_target_params_ptr` (vo_gpu.c:88) — core owns the struct, shell publishes the pointer. |

`struct gl_next_opts` (175–190) and `gl_next_conf` (201–239): **A**. Pure option
schema; moves with the core (analogue: `gl_video`'s `gl_video_conf`).

### 1b. Major functions

| Function (line) | Class | Notes |
|---|---|---|
| `get_dr_buf` 241 / `free_dr_buf` 257 / `get_image` 274 | A | Pure `pl_buf` on `p->gpu`. `get_image` signature is `(vo,...)` but body only uses `p->gpu`+`dr_lock`. Trivially reshaped to `(core,...)`. Maps to `render_backend_fns.get_image` AND `vo_driver.get_image_ts`. **Top extraction candidate (§4).** |
| `update_overlays` 315 | C | Core rendering of OSD into `pl_overlay`, but pulls `osd_render(vo->osd,...)` (326) and reads `p->next_opts`. `vo->osd` is the seam (render backend already sets OSD via `update_external`, mirroring `gl_video_set_osd_source`). Body otherwise pure pl. Becomes `core_update_overlays(core, osd, ...)`. |
| `plane_data_from_imgfmt` 471 | A | Pure format math. Already static, no `p`. |
| `hwdec_reconfig` 564 | C | Logic core; touches `p->ra_ctx->ra` (586) for timer pool. Need `ra` handle in core ctx. |
| `hwdec_get_tex` 592 | C | Core (wraps mapper tex into `pl_tex`), but branches on `ra_is_gl`/`ra_is_d3d11` of `p->hwdec_mapper->ra`. The `ra` comes from hwdec interop, not the swapchain — so this is API-of-the-hwdec-device specific, *not* presentation-API specific. Extractable if `ra` travels in core ctx. |
| `hwdec_acquire` 633 / `hwdec_release` 665 | C | libplacebo frame callbacks; recover `p` via `frame->user_data→mpi->priv→fp->vo->priv`. The `vo` hop (637, 669) must be replaced by a core-ctx pointer stored in `frame_priv`. |
| `format_supported` 678 | A | `(vo,...)` but uses only `p->gpu`. Reshape to core. |
| `map_frame` 695 | C | The big one. Mostly pure pl upload, BUT: recovers `p` via `fp->vo` (702), calls `hwdec_reconfig`, reads `p->opts_cache`/`p->next_opts`, `update_lut`. Pure once `vo` hop replaced and opts live in core. No swapchain/`ra_ctx` use directly. Extractable but invasive. |
| `unmap_frame` 849 / `discard_frame` 863 | C/A | `unmap_frame` uses `fp->vo→priv` (854) only to reach `p->sub_tex` pool — replace with core ptr. `discard_frame` pure. |
| `info_callback` 869 | C | `priv = vo` (871) → `p->perf_*`. Replace cookie with core ptr. |
| `update_options` 887 / `update_render_options` 2557 | A* | Pure options→`pl_options`/`pl_renderer` mapping. One leak: `update_render_options` calls `vo_set_queue_params(vo, ...)` (2603) and `update_icc_opts`→auto-profile. Split: pure pl mapping = A (core); the `vo_set_queue_params` call = a value the core returns and the shell/backend applies (render backend has no decoder queue knob — it's a no-op there, behaviour seam). |
| `apply_target_contrast` 915 / `apply_target_options` 944 | C | Core color math, BUT `apply_target_options` reads `p->ra_ctx->swapchain` for `color_depth` fallback (972–973). Dither-depth-from-swapchain must be passed in as a parameter (render backend supplies depth from `MPV_RENDER_PARAM_DEPTH`, exactly like `libmpv_gl.c`/`gl_video_set_fb_depth`). |
| `apply_crop` 1001 | A | Pure geometry. |
| `set_colorspace_hint` 1024 | **B** | Entirely swapchain: `p->ra_ctx->swapchain`, `sw->fns->set_color`, `pl_swapchain_colorspace_hint(p->sw,...)`. No analogue in render API. Stays in shell; the core must accept the resolved `target.color` as an input and must NOT call this. |
| `update_tm_viz` 1047 | A | Pure. |
| `draw_frame` 1071 | **C — the crux** | See §1c. Interleaves: pure core (queue push/update, `pl_render_image_mix`, peak metadata) with hard swapchain (`sw->fns->target_csp` 1162, `set_colorspace_hint` 1282/1286, `sw->fns->start_frame` 1290, `pl_swapchain_start_frame` 1291, `pl_frame_from_swapchain` 1310, error clear on `swframe.fbo` 1504, `pl_gpu_flush`+`frame_pending` 1506). Splitting this behaviour-preservingly is the entire risk of the project. |
| `flip_page` 1511 / `get_vsync` 1525 | B | Pure swapchain/`sw->fns`. Shell-only (render API has no flip — host presents). |
| `query_format` 1533 | A | Uses `p->hwdec_ctx`+`format_supported`. Maps to `render_backend_fns.check_format` + `vo_driver.query_format`. |
| `resize` 1546 | B/C | Calls `gpu_ctx_resize` (1553, swapchain ⇒ B) then stores `src/dst/osd_res` (C, core-consumed). Split accordingly: backend's `resize()` fills the geometry; `gpu_ctx_resize` stays shell. |
| `reconfig` 1568 | B | `p->ra_ctx->fns->reconfig` + clears `vo->target_params`. Shell. (Render backend's `reconfig` does the analogous thing without `ra_ctx`.) |
| `update_icc` 1582 | A | Pure `pl_icc_update`. |
| `update_auto_profile` 1597 | B | `p->ra_ctx->fns->control(VOCTRL_GET_ICC_PROFILE)`. Shell. Feeds bytes into core `update_icc`. |
| `video_screenshot` 1621 | C | Mostly pure pl (creates own FBO, `pl_render_image`), BUT reads `p->ra_ctx->opts.want_alpha` (1701), `p->src/dst/osd_res`, `p->last_pts`, `update_overlays(vo,...)`. Maps to `render_backend_fns.screenshot` + `VOCTRL_SCREENSHOT`. Extractable; `want_alpha` must be a core option. |
| `copy_frame_info_to_mp` 1825 | A | Pure perf marshalling. |
| `update_ra_ctx_options` 1870 | B | Computes `ra_ctx_opts.want_alpha`. Shell (render backend has no `ra_ctx`). The alpha *intent* is also needed core-side (§3) — duplicate the small predicate, don't share the `ra_ctx_opts`. |
| `control` 1883 | B | `vo_driver` dispatch; nearly every case is shell (`p->ra_ctx->fns->control`, resize, pause). Render backend exposes the relevant subset via discrete `render_backend_fns` (`reset`,`screenshot`,`perfdata`,`set_parameter`). Not shared. |
| `wakeup` 1963 / `wait_events` 1970 | B | `p->ra_ctx->fns->*`. Shell. |
| `cache_*` 1980–2168 | A | `pl_cache` file I/O on `p->global`/`p->pllog`. Core. |
| `uninit` 2170 | B+A | Mixed teardown: core objects (queue/rr/icc/caches/opts/perf) = core `destroy`; `gpu_ctx_destroy` (2220) + `ra_ctx`/`sw` nulling = shell. Split cleanly. |
| `preinit` 2228 | B+A | `gpu_ctx_create` + handle wiring (2242–2250) = shell; everything after (hwdec ctx, caches, `pl_renderer_create`, `pl_queue_create`, `osd_fmt`, `pl_options_alloc`, `update_render_options`) = core `init(core, pl_gpu, pllog, ra_for_hwdec)`. Mirrors `libmpv_gpu.c:init` calling `gl_video_init`. |
| `map_scaler` 2283 / `load_hook` 2372 / `update_icc_opts` 2398 / `update_lut` 2437 / `update_hook_opts*` 2467–2555 | A | All pure core (pl filters, user shaders, ICC params, LUT parse). `load_hook`/`update_lut` use `p->global` for file I/O — fine in core. |

### 1c. The entanglement core: `draw_frame` anatomy (1071–1509)

Sequential breakdown with class per block:

1. `update_options(vo)` 1076 — **A** (pure, modulo `vo_set_queue_params` leak).
2. Frame-pacing decisions (`will_redraw`/`cache_frame`/`can_interpolate`/
   `pts_offset`) 1080–1090 — **A**, but derived from `vo_frame` fields
   (`display_synced`, `num_vsyncs`, `still`, `ideal_frame_vsync`). `vo_frame`
   is already the render-API currency (`render_backend_fns.render` takes
   `struct vo_frame *`), so this is **A** — the backend passes the same struct.
3. vflip distort 1092–1098 — **A** (reads `frame->current->params.vflip`).
4. Non-monotonic-PTS reset guard + queue push loop 1109–1156 — **A** (pure
   `pl_queue`). `frame_priv.vo` set at 1144 — must become `frame_priv.core`.
5. `sw->fns->target_csp(sw)` 1162 + container/min-luma fixups 1164–1173 —
   **B**. Swapchain-reported target colorspace. **Render API has no swapchain
   `target_csp`**; equivalent must come from `MPV_RENDER_PARAM`/defaults.
6. Target-hint computation 1175–1287 (huge HDR block) — **A in math, B in
   sink**. All the `pl_color_space` inference is pure; it terminates in
   `set_colorspace_hint(p, &hint)` 1282/1286 which is **pure swapchain (B)**.
7. `sw->fns->start_frame(sw, NULL)` 1290 + `pl_swapchain_start_frame(p->sw,
   &swframe)` 1291; on failure, advance queue & `return VO_FALSE` 1300–1302 —
   **B**. This is where the render backend instead wraps the user FBO
   (`libmpv_gpu_context_fns.wrap_fbo` analogue) — no `pl_swapchain`.
8. `pl_frame_from_swapchain(&target,&swframe)` 1310 — **B** (swapchain). Render
   path builds `target` from the wrapped FBO instead.
9. `apply_target_options` 1314 / gamut clip 1315–1323 / sRGB-power22 / Win PQ
   1324–1362 — **C** (color math is A; `apply_target_options` reaches the
   swapchain for dither depth 972; Win-PQ branch reads `target_csp` from #5).
10. `update_overlays(...&target...)` 1364 — **C** (needs `vo->osd`).
11. `apply_crop(&target,p->dst,swframe.fbo->...)` 1369 — **C** (`p->dst` is
    vo-derived geometry; `swframe.fbo` dims are swapchain → render path uses
    wrapped-FBO dims).
12. `update_tm_viz` 1370 — **A**.
13. `pl_queue_update` + per-frame crop/OSD/signature/dynamic-hooks 1372–1466 —
    **A** (pure pl; `vo->params->w/h` 1420/1433 are image params, available
    via `reconfig` params — A).
14. `pl_render_image_mix(p->rr,&mix,&target,&params)` 1470 — **A**. The actual
    render. Identical call both paths.
15. `pl_frames_infer_mix` 1478 + publish `p->target_params`/`vo->target_params`
    1480–1490 + `pl_renderer_get_hdr_metadata(...&vo->params->color.hdr)`
    1492–1495 — **C**. Core computes; publishing into `vo->params`/
    `vo->target_params` under `vo->params_mutex` is vo-API. Mirror
    `gl_video_get_target_params_ptr` (core owns, shell publishes).
16. Error clear `pl_tex_clear(gpu,swframe.fbo,...)` 1504 — **C** (purple-clear
    is core behaviour but targets the swapchain FBO; render path clears the
    wrapped FBO).
17. `pl_gpu_flush(gpu)` 1506 + `p->frame_pending=true` 1507 — **B** (paired
    with windowed `flip_page`; render path's "submit" is the
    `done_frame`/host-present analogue).

**Verdict on the seam:** the natural, behaviour-preserving cut is a core entry
`core_render_frame(core, struct vo_frame *frame, struct pl_frame *target,
struct pl_color_space target_csp_hint, ...output...)` where **the caller
supplies the target `pl_frame` + the target colorspace facts and consumes the
computed target_params/hdr-metadata**, and the core never calls
`pl_swapchain_*` / `sw->fns->*` / `set_colorspace_hint`. This is *exactly* the
shape of `vo_gpu.c::draw_frame` → `gl_video_render_frame(renderer, frame,
&fbo, ...)` (vo_gpu.c:82), where `gl_video` takes a `ra_fbo` and never sees the
swapchain. The model is proven to work in mpv for the old renderer. The
libplacebo core can follow it because `pl_render_image_mix` (the actual render,
step 14) is already swapchain-agnostic — it takes a `pl_frame` target. **All
swapchain coupling in `draw_frame` is in target *acquisition* (5–8,11) and
*presentation* (16–17), not in rendering.** That is the single most important
finding: the render/acquire boundary is real and falls in a clean place.

The hard part is not "can it be cut" but **steps 6+9+15**: the HDR
hint/`target.color` computation is interleaved with the swapchain (it both
*reads* `sw->fns->target_csp` and *writes* `set_colorspace_hint`, then reads the
hinted colorspace back from `swframe`). Behaviour preservation for the windowed
path requires the shell to do: get target_csp → run hint math → push hint to
swapchain → start swapchain frame → read back swframe.color → call core with the
finalized target. So the hint math (6) cannot live wholly in the core OR wholly
in the shell — it must be a **third pure helper** (input: source color +
target_csp + opts + icc; output: hint `pl_color_space`) that the shell drives
between its swapchain calls, and that the render backend drives with
target_csp synthesized from render params. This is feasible (the math is
already side-effect-free except its two swapchain touchpoints) but it is the
delicate part and must be its own commit, golden-frame gated.

---

## §2. PR #16818 salvage audit

**Evidence basis:** see tooling caveat. Reconstructed from the architecture
contract + maintainer quotes + mainline integration surface. Numbers are
reasoned bounds, not a measured diff.

### Why its structure violates the `render_backend`/`libmpv_gpu_context_fns` model

The two maintainer quotes pin the two independent violations precisely:

1. **na-na-hi: "basically a reimplementation of vo_gpu_next.c with significant
   code duplication … reimplement vo_gpu_next.c with the RA like vo_gpu.c
   does."** → #16818 created a *parallel* libplacebo renderer for the render
   path instead of *extracting* the existing one. Concretely that means a
   second copy of the §1c hot path (`draw_frame`'s queue push/update +
   `pl_render_image_mix`), `map_frame`/`hwdec_acquire` (the 130-line frame
   upload+hwdec mapper), `update_render_options`/`update_options` (the ~300-line
   options→`pl_options` mapping), `update_overlays`, ICC/LUT/hooks. Any line
   that is **class A or C in §1** and was *copied* rather than *shared* is, by
   the maintainer's own bar, "must-rewrite-as-extraction." That is the bulk of
   `vo_gpu_next.c`'s ~1700 substantive lines.

2. **Dudemanguy: "design is pretty much a mess … GL code shouldn't be all over
   gpu_next/context.c."** → #16818 put render-API GL surface handling (the
   `wrap_fbo` equivalent: importing the user's GL FBO into a `pl_tex` /
   `pl_opengl`) into the *windowed* swapchain file `gpu_next/context.c`,
   instead of into a separate libplacebo `libmpv_gpu_context_fns` GL backend
   mirroring `video/out/opengl/libmpv_gl.c` (the existing
   `libmpv_gpu_context_gl` is 110 lines and is the exact template). Mixing the
   windowed `gpu_ctx` swapchain bring-up with render-API FBO-wrap in one file
   conflates two lifecycles (mpv-owned swapchain vs. caller-owned surface) and
   is precisely the layering the mainline model separates via
   `libmpv_gpu_context_fns`.

3. Implied third violation: not registering through `render_backends[]`
   (`vo_libmpv.c:114`) + `render_backend_fns` as a peer of
   `render_backend_gpu`/`render_backend_sw`, i.e. bypassing the generic backend
   contract (`video/out/libmpv.h:41`). If #16818 wired gpu-next into the GL
   backend or a side path rather than adding a clean `render_backend_gpu_next`,
   that is a structural rejection independent of duplication.

### Reusable vs must-rewrite (bounded estimate)

- **Conceptually reusable / directly informative (~25–35%):** the *proof that
  libplacebo can render into an externally-provided, caller-owned surface*
  (the `pl_opengl_wrap`/import-FBO mechanism), the public API plumbing
  (`MPV_RENDER_API_TYPE_*` string + `include/mpv/render.h` addition + docs),
  the `render_backend_fns` vtable wiring boilerplate, and the GL
  surface-import code *as a reference for what calls to make* (not where to put
  them). These are small, mechanical, and mostly correct in any approach.
- **Must-rewrite as extraction (~65–75%):** everything that is a *second copy*
  of class-A/C logic from `vo_gpu_next.c` (render loop, frame map, hwdec,
  options, OSD, ICC, hooks, peak/perf). Under the maintainer bar this is not
  "fix #16818's code" — it is "delete the copy, extract the original, call it
  from both." So most of #16818's renderer body is negative work (to be
  removed), not salvage.

**Conclusion:** #16818 is usable as a *requirements/reference* artifact
(it demonstrates the GL FBO-import is possible and shows the API surface), but
**not** as a code base to build on — the target architecture mandates
extraction-not-duplication, which inverts #16818's central design choice. The
GL-in-`gpu_next/context.c` placement must be discarded entirely in favor of a
new libplacebo `libmpv_gpu_context_fns`. Plan §"don't add #16818's parallel
tree" is correct in spirit; see provenance correction in the caveat for the
precise meaning.

---

## §3. Top-5 no-regression hazards (windowed path calling the extracted core)

Each: mechanism → why risky → detection.

### 3.1 Target acquisition / colorspace-hint round-trip (HDR) — HIGHEST
**Mechanism:** windowed `draw_frame` does target_csp ← `sw->fns->target_csp`
(1162) → HDR hint math (1175–1281) → `set_colorspace_hint` pushes hint to the
swapchain (1282) → `pl_swapchain_start_frame` (1291) → `pl_frame_from_swapchain`
reads the *negotiated* colorspace back (1310) → `apply_target_options` (1314).
The final `target.color` depends on what the swapchain *accepted*. If extraction
moves any of this math relative to the swapchain calls, the windowed target
colorspace changes.
**Why risky:** this is the exact code the whole project exists to share, it is
the most intricate block in the file (~110 lines of `pl_color_space` inference
with platform `#ifdef`s), and it is bidirectionally coupled to the swapchain
(reads target_csp, writes hint, reads back). Getting the *ordering* wrong
silently shifts HDR tone-mapping/peak on every HDR display.
**Detection:** golden-frame on a 4K HDR10 clip at fixed PTS, plus
`vo->params->color.hdr` (max_luma/min_luma/max_cll) and `target_params` dumped
and diffed bit-exact vs. baseline; also SDR clip (the 1000:1 contrast default
path, line 1168/1265) to catch the SDR min_luma regression.

### 3.2 Frame interpolation / `pl_queue` state machine
**Mechanism:** `p->queue`, `want_reset`/`flush_cache`/`last_id`/`last_pts`,
the non-monotonic-PTS guard (1109–1119), `pl_queue_push` with
`can_interpolate?frame->approx_duration:0` (1148), `pl_queue_update` with
`vsync_duration`/`interpolation_threshold` (1375–1396), and the
`preserve_mixing_cache`/`skip_caching_single_frame` flags (1087–1088).
**Why risky:** `pl_queue` is stateful across frames and sensitive to PTS
monotonicity; an extraction that changes *when* reset/flush happen, or that
double-pushes/duplicates frame IDs across the new call boundary, or reorders
`update_options` (which sets `flush_cache`, line 2703) vs. queue push, causes
judder, frame drops, or interpolation artifacts that are invisible on a single
golden frame.
**Detection:** seek/interpolation clip with `--video-sync=display-resample`;
compare `--frame-drop` stats and a multi-frame golden sequence across a seek
(not just one frame); verify `is_interpolated` and per-frame PTS log
(MP_VERBOSE lines 1114/1390) match baseline.

### 3.3 hwdec mapping re-entrancy (`vo` recovered in pl callbacks)
**Mechanism:** `hwdec_acquire`/`hwdec_release`/`map_frame` recover state via
`frame->user_data → mpi->priv → fp->vo → vo->priv` (637, 669, 702), and
`hwdec_get_tex` (592) branches on `ra_is_gl`/`ra_is_d3d11`/`ra_pl_get` of the
hwdec mapper's `ra`, building `pl_opengl_wrap`/`pl_d3d11_wrap`. Extraction must
replace the `vo` hop with a core-ctx pointer in `frame_priv` and thread the
hwdec `ra` into the core.
**Why risky:** these run inside libplacebo's render callback during
`pl_render_image_mix`; a wrong/stale pointer or a lifetime mismatch (the core
ctx must outlive in-flight queued frames — note `uninit` destroys the queue
*first*, line 2178, precisely for this) is a crash or silent wrong-texture, and
hwdec paths are platform-specific so a Windows D3D11/`pl_d3d11_wrap` regression
won't show on a dev machine using vaapi/GL.
**Detection:** golden-frame on a hwdec-decoded clip on the *target* platform
(Windows D3D11 per the no-regression bar), plus ASan/leak run across
play→seek→quit to catch the queue/ctx lifetime ordering; verify `hwdec_perf`
counter still populates (`VOCTRL_PERFORMANCE_DATA`, 1930).

### 3.4 ICC + dither-depth-from-swapchain
**Mechanism:** `apply_target_options` pulls dither depth from
`p->ra_ctx->swapchain` `color_depth` (972–973) when `dither_depth==0`;
auto-ICC bytes come from `p->ra_ctx->fns->control(VOCTRL_GET_ICC_PROFILE)`
(1605). Both are shell facts the core consumes.
**Why risky:** if the extracted core no longer sees swapchain color depth (it
must not), the shell has to pass the identical value in; an off-by-default
(e.g. core sees depth 0 → no dithering, or wrong bit depth) changes banding on
every windowed frame. ICC auto-profile timing (re-query on
`VOCTRL_UPDATE_RENDER_OPTS`, 1918) must be preserved.
**Detection:** golden-frame with `--dither-depth=auto` on a gradient/banding
test pattern and with an ICC profile loaded (`--icc-profile`); diff for banding
and color shift; verify `target.repr.bits.sample_depth` in `target_params`.

### 3.5 Options plumbing + `vo_set_queue_params` + alpha intent
**Mechanism:** `update_render_options` (2557) maps ~entire `gl_video_opts`/
`gl_next_opts` into `pl_options`, and as a side effect calls
`vo_set_queue_params(vo, 0, req_frames)` (2603) sizing the *decoder* lookahead
from the frame-mixer radius, and the alpha intent is computed in
`update_ra_ctx_options` (1870) feeding both `ra_ctx_opts.want_alpha` (shell) and
implicitly the render `background_transparency` (2565).
**Why risky:** if extraction relocates `update_render_options` into the core
but drops/duplicates the `vo_set_queue_params` call or changes *when* it runs
relative to option-cache update, the decoder requests the wrong number of
frames → interpolation silently degrades (too few frames) with no visual error
on a static frame. Splitting `want_alpha` (must be duplicated, not shared, since
render backend has no `ra_ctx`) risks the windowed and render paths disagreeing
on premultiplied alpha → wrong compositing on transparent backgrounds.
**Detection:** with `--interpolation` + a high frame-mixer radius, assert the
requested-frames value (instrument or compare `--frame-drop`/interpolation
behaviour over a seek); golden-frame with `--background=none`/transparent on a
clip with alpha vs. baseline.

(Additional, lower: OSD blend-subs signature mutation `mix.signatures` 1460 and
`osd_sync` accounting 1423–1450 — subtitle/OSD timing across the new boundary;
detect via golden-frame with `--blend-subtitles` and moving subs. Perf timers
`stats_time_*`/`info_callback` — `VOCTRL_PERFORMANCE_DATA` output should match;
detect via `--display-tags`/stats compare.)

---

## §4. Best first tiny behaviour-preserving extraction commit

**Extract the direct-rendering buffer pool + `get_image`** (and only that):

- Functions: `get_dr_buf` (241–255), `free_dr_buf` (257–272), `get_image`
  (274–313).
- Fields: `dr_lock`, `dr_buffers`, `num_dr_buffers` (121–123).

**Why this one (most isolated, lowest risk):**
- Zero swapchain, zero `ra_ctx`, zero `vo` use. `get_image`'s only dependency is
  `p->gpu` and the DR mutex/array; its `(struct vo *vo, ...)` signature is
  cosmetic — the body never touches `vo` beyond `vo->priv`.
- Self-contained lifetime: buffers allocated/freed via `pl_buf` on `p->gpu`,
  asserted empty at `uninit` (2196). No cross-talk with queue/render/OSD.
- It is **not on the per-frame render path** (it's the decoder's DR allocator,
  `vo_driver.get_image_ts`), so a mistake cannot shift a golden frame's pixels;
  worst case is DR disabled (falls back to normal alloc) — easy to detect, not a
  silent color regression.
- It maps 1:1 onto BOTH targets immediately: `render_backend_fns.get_image`
  (libmpv.h:60) and `vo_driver.get_image_ts` (2717) — i.e. it exercises the
  whole "shared core called by two front-ends" pattern end-to-end on a trivial,
  verifiable unit. Exactly mirrors `gl_video_get_image` being called from both
  `vo_gpu.c` and `libmpv_gpu.c:get_image` (libmpv_gpu.c:196).

First commit shape: new shared unit (e.g. `video/out/placebo/`-adjacent core
file) holding the DR pool + `core_get_image(core, imgfmt,w,h,align,flags)`;
`vo_gpu_next.c` keeps a 1-line `get_image` shim delegating to it. `git diff
--stat` ≈ one new small file + ~40 moved lines + a shim — minimal, comment-clean
(Dudemanguy bar). Golden frames must be byte-identical (they will be: pixel path
untouched).

(Second-best, if a non-render-path option unit is preferred: the
`gl_next_conf`/`struct gl_next_opts` schema (175–239) + `cache_*` 1980–2168 —
also pure, but larger and touches file I/O; DR is the cleaner true-minimum.)

---

## §5. GO / NO-GO

### GO — confidence: MEDIUM

**Rationale for GO:** The decisive question — "can the libplacebo core be
extracted behaviour-preserving without changing windowed `draw_frame`
semantics?" — resolves to **yes, conditionally**, because:
1. The actual render (`pl_render_image_mix`, draw_frame:1470) is already
   swapchain-agnostic — it consumes a `pl_frame` target. All swapchain coupling
   is in target *acquisition* (steps 5–8,11 in §1c) and *presentation*
   (16–17), which is a clean, identifiable boundary, not smeared through the
   render itself.
2. mpv **already proves this exact two-layer split works** for the old renderer:
   `vo_gpu.c::draw_frame` (82) calls `gl_video_render_frame(renderer, frame,
   &fbo, …)` with `gl_video` never touching the swapchain, and the *same*
   `gl_video` core is driven by `render_backend_gpu` via `libmpv_gpu.c`. The
   target architecture is to reproduce a structure that demonstrably exists.
3. A safe, truly-isolated first extraction (§4) exists to validate the harness
   and the "shared core, two front-ends" mechanic at near-zero risk before
   touching the render path.

**Why MEDIUM, not HIGH:** the colorspace-hint round-trip (§3.1) is genuinely
hard — the HDR hint math both reads `sw->fns->target_csp` and writes
`set_colorspace_hint` then reads the result back, so behaviour preservation
requires carving it into a *third* pure helper the shell drives *between* its
swapchain calls. This is feasible (the math is side-effect-free apart from those
two swapchain touchpoints) but it is intricate platform-`#ifdef`-laden code
that, if reordered, silently shifts HDR on every HDR display. It is not a
NO-GO (it does not require changing windowed *render* semantics — only
re-expressing where the swapchain touchpoints sit relative to pure math, which
is structurally the same as what the code already does sequentially), but it
caps confidence below high until a golden-frame harness on real HDR hardware
proves it.

**Why not LOW / not NO-GO:** NO-GO is defined as "core cannot be extracted
behaviour-preserving without changing windowed `draw_frame` semantics." The
analysis shows the opposite: the render call is identical both paths, and the
windowed path keeps *all* its swapchain semantics in the shell (it just calls a
pure core for the part that was always pure). No windowed render-semantic change
is required. The duplication objection that sank #16818 is avoided by
construction (extract, don't copy). Hence not NO-GO; the residual risk is
execution difficulty on one block, not architectural impossibility.

### Single biggest unknown

**Whether the HDR colorspace-hint round-trip (draw_frame 1162→1281→1310,
`sw->fns->target_csp` → hint math → `set_colorspace_hint` → read-back from
`swframe`) can be re-expressed as a pure shell-driven helper that yields a
bit-identical `target.color` on real HDR hardware across the platform
`#ifdef`s (notably the Windows PQ branch 1346–1361 and the SDR 1000:1 default
1168/1265).** This cannot be settled by static reading — it requires the Phase-1
golden-frame + `vo->params->color.hdr` diff on an actual 4K HDR10 path
(and the Windows libmpv path the user runs). Recommend the §4 DR extraction as
commit 1, then make THIS block its own dedicated, golden-gated commit early —
if it cannot be made bit-identical, that is the project's true kill point.

Secondary unknown (evidence, not architecture): exact #16818 salvage % is
bounded not measured here, because `gh pr diff 16818` / `git` / `WebFetch` were
all denied this session. It does not affect the GO decision (the plan mandates
extraction regardless of #16818), but the §2 percentages should be re-derived
from the real diff before relying on them for effort estimation.
