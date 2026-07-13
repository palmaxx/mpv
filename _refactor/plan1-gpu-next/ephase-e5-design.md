# E-phase step E5 — locked design: `frame_priv` + `map`/`unmap`/`discard` → core

Self-contained design doc for executing **E5** in a fresh session. Resume
context is `HANDOFF.md`; this file is the **locked architecture** for the
hardest E-phase slice. Decided and verified 2026-05-21.

---

## 1. Where things stand (E1–E4 done)

The `vo_gpu_next` → render-API refactor's E-phase extracts `draw_frame`'s
orchestration into the shared `gpu_next_core`. Done so far (branch
`gpu-next-render-api`, canonical tree WSL `~/mpv-fork`, HEAD after E4 =
`f0470ae`):

- **E1 `62d80a0`** — pl_queue lifecycle state → core.
- **E2 `a5bf618`** — draw-path `pl_queue_update` → core.
- **E3 `5c01bdc`** — SW-upload perf timer → core.
- **E4 `f0470ae`** — render-perf subsystem (`info_callback`, `perf_fresh/redraw`,
  `copy_frame_info_to_mp`) → core.

Each was golden-gated: 12-case windowed `--vo=gpu-next` matrix byte-identical
to pristine `35ae76d` + `teardown.sh` clean. **E5 must clear the same gate
per sub-commit.**

E5 is the hard slice: the `pl_source_frame` callbacks (`map_frame`,
`unmap_frame`, `discard_frame`) and `struct frame_priv`. `map_frame` is the
~110-line per-frame mapping logic and is currently coupled to the VO through
`frame_priv.vo`.

---

## 2. Verified assumptions (checked against source before locking)

1. **`m_config_cache->opts` is a stable pointer.** `options/m_config_core.c:592`
   sets `cache->opts = gdata[0].udata` once at allocation; `m_config_cache_update`
   copies new option values *into* that buffer (`m_option_copy` to
   `udata + offset`) and never reassigns the pointer. ⇒ the core may hold a
   `const struct gl_video_opts *` for its whole lifetime; updates are seen
   in-place. (`gl_video_opts` is already a core-visible type — `core.c`
   includes `video/out/gpu/video.h`.)
2. **The granular `stats_ctx` timing is vo_gpu_next-specific.** `vo_gpu.c`,
   `gpu/video.c` (`gl_video`) and `libmpv_gpu.c` use **no** `stats_ctx` /
   `stats_time_*`. The maintainer-referenced shared core (`gl_video`) does not
   carry this instrumentation. ⇒ the shared `gpu_next_core` must **not** own
   `stats_ctx`; the `"swdec-upload"` / `"hwdec-map"` / `"render"` timing is a
   front-end concern and stays driven by the front-end.

Consequence: a bare "stash a `gl_video_opts*` in the core" (the simple option)
is **insufficient** — it covers neither `ra`, nor the VO-private `image_lut`
(in `struct gl_next_opts`), nor stats. The locked design below is the correct,
maintainer-aligned answer.

---

## 3. Locked architecture

**The core owns `map_frame` / `unmap_frame` / `discard_frame` and
`struct frame_priv`.** This is required by the refactor's purpose — a single
`gpu_next_core` frame-ingest path with **zero callback duplication** between
the windowed VO and `render_backend_gpu_next` (the user's standing decision:
"extract the orchestration into the core, zero duplication").

The genuinely front-end-specific things `map_frame` needs are reached through
a **front-end interface the core holds** — the same pattern mpv already uses
for its shared-core/two-front-end splits (`render_backend_fns`,
`libmpv_gpu_context_fns`). This is *not* gratuitous complexity; it is the
maintainer-blessed shape.

### 3.1 `struct gpu_next_core_frontend`

A small interface the front-end fills once after `gpu_next_core_create` and
installs via `gpu_next_core_set_frontend(core, &fe)`. Lives in `core.h`.

```c
struct gpu_next_core_frontend {
    // Resolved option structs. opts is the stable m_config_cache pointer
    // (see §2.1); the core reads opts->hdr_reference_white,
    // opts->treat_srgb_as_power22, etc. live at map time.
    const struct gl_video_opts *opts;

    // RA handle, for the SW-upload timer and hwdec mapper/timer.
    struct ra *ra;

    // Optional instrumentation hooks (NULL on the libmpv backend, which
    // has no stats_ctx). The core wraps its per-frame work in
    // timer_start(timer_ctx, name) / timer_end(timer_ctx, name).
    void *timer_ctx;
    void (*timer_start)(void *ctx, const char *name);
    void (*timer_end)(void *ctx, const char *name);

    // Hwdec registry lookup (deferred-rig). The core calls this from
    // map_frame at map time, exactly mirroring today's
    // ra_hwdec_get(&p->hwdec_ctx, imgfmt). NULL ⇒ no hwdec (SW only).
    struct ra_hwdec *(*hwdec_get)(void *ctx, int imgfmt);
    void *hwdec_ctx;   // opaque; the VO passes &p->hwdec_ctx
};
```

Notes / rationale:
- **`opts` is a pointer, not a snapshot** — verified stable (§2.1), and a
  live read at map time preserves today's exact "map-time opts" semantics
  (today `map_frame` reads `p->opts_cache->opts` at map time). Capturing
  into `frame_priv` at *push* time was rejected: a frame can sit in the
  queue across a runtime `hdr-reference-white` / `treat-srgb-as-power22`
  change and would then map with stale values — a real (if tiny) regression.
- **`image_lut` is NOT in this struct.** It lives in `struct gl_next_opts`,
  which is vo_gpu_next-private and should stay private (its other fields are
  already passed to the core individually, e.g. `gpu_next_core_target_hint`).
  Instead the core holds the *resolved* image LUT as a value — see §3.3.
- **`hwdec_get` as a hook** (not a value captured at push) keeps hwdec
  resolution at *map* time, byte-identical to today. hwdec is deferred-rig
  (lavapipe cannot verify it); the hook is the clean seam.
- **`timer_*` hooks** carry the `"swdec-upload"` / `"hwdec-map"` stats. The
  VO supplies thin wrappers over `stats_time_start/_end`; the libmpv backend
  supplies NULL and the core skips them. This is the resolution to §2.2 —
  the core never touches `stats_ctx`.

### 3.2 `struct frame_priv` → `core.h`

```c
struct frame_priv {
    struct gpu_next_core *core;   // back-ref, set by the core at push
    struct ra_hwdec *hwdec;       // resolved in map_frame via fe.hwdec_get
    struct osd_state subs;        // blended-subs cache (front-end OSD; E6)
    uint64_t osd_sync;            // OSD sync counter (front-end OSD; E6)
};
```

`frame_priv` moves to `core.h` so both the core (its callbacks) and the VO
(the per-frame overlay loop, still VO-side until **E6**) can see it. `core.h`
gains an `#include` (or forward-decl) sufficient for `struct osd_state`
(`sub/osd_state.h`). The `vo` back-pointer is **gone** at the end of E5 — the
core reaches everything front-end-specific through `core->fe` (§3.1).

`subs` / `osd_sync` stay in `frame_priv` (the core's `unmap_frame` pushes the
`subs` overlay textures back to the core's `sub_tex` pool). They are written
by the per-frame overlay loop, which stays VO-side until E6.

### 3.3 image LUT

`map_frame` ends with `update_lut(p, &p->next_opts->image_lut)` then
`frame->lut = image_lut.lut; frame->lut_type = image_lut.type`. `update_lut`
is idempotent and path-cached, and **an `--image-lut` change already forces a
queue reset** (E1's `gpu_next_core_queue_request_reset` on image-LUT change),
so no in-flight frame can straddle an image-LUT change.

⇒ The core holds the resolved image LUT as two values
(`struct pl_custom_lut *image_lut; enum pl_lut_type image_lut_type;`), set by
the VO from `update_options` (where it already runs `update_lut` for the other
LUTs). `map_frame` reads `core->image_lut`. No staleness window exists.
Provide `gpu_next_core_set_image_lut(core, lut, type)`.

---

## 4. E5 sub-commits (each golden-gated; merge only if obviously natural)

**E5.1 — front-end interface, pure addition.**
Add `struct gpu_next_core_frontend`, `gpu_next_core_set_frontend()`, and
`gpu_next_core_set_image_lut()` to `core.{c,h}`; the core stores them. The VO
builds the struct after `gpu_next_core_create` (timer hooks = small wrappers
over `stats_time_start/_end` bound to `p->stats`; `opts = p->opts_cache->opts`;
`ra = p->ra_ctx->ra`; `hwdec_get` wraps `ra_hwdec_get`, `hwdec_ctx = &p->hwdec_ctx`)
and calls `set_frontend`; `update_options` calls `set_image_lut`. Nothing
consumes them yet. Golden: behaviour unchanged (like wiring step W1).

**E5.2 — `frame_priv` → core; move `discard_frame` + `unmap_frame`.**
`struct frame_priv` moves to `core.h` (gains `core`, keeps `subs`/`osd_sync`;
add `hwdec`). `discard_frame`/`unmap_frame` become `gpu_next_core_*` functions;
the VO push loop sets `.discard`/`.unmap` to them. Keep a **transitional**
`struct vo *vo` field in `frame_priv` for the still-VO `map_frame` and the
hwdec acquire/release shims — removed in E5.4.

**E5.3 — `map_frame` → core.**
`gpu_next_core_map_frame()` becomes the registered `.map` callback. It does
the whole body using `core->fe` (opts tweaks, `fe.ra`, `fe.hwdec_get`,
`fe.timer_*` around the SW upload) and `core->image_lut`. The hwdec branch
sets `frame->acquire`/`release`; for this step they may still point at the
VO shims reached transitionally (or move them in the same step — see E5.4).

**E5.4 — fold hwdec `acquire`/`release` into the core; drop transitional `vo`.**
The hwdec acquire/release shims (`97146f2` kept them VO-side only for the
stats wrap + `ra`; both are now in `core->fe`) become core functions using
`core->fe.timer_*`. Remove the transitional `frame_priv.vo`. `frame_priv` is
now `{ core, hwdec, subs, osd_sync }` — fully front-end-agnostic.

After E5: **E6** lifts the per-frame source-crop / overlay loop (it reads
`fp->subs`/`osd_sync`; `update_overlays` itself stays VO-side — it needs
`vo->osd`).

---

## 5. Gotchas / invariants to preserve

- **hwdec is deferred-rig.** lavapipe cannot verify it; golden only covers
  the SW path. Keep hwdec changes mechanical and re-validate on the Windows
  d3d11 rig (`run-hdrval.ps1`) when an HDR/hwdec-touching sub-step lands —
  E5.3/E5.4 touch the hwdec branch, so re-run `run-hdrval.ps1 -Ref <sha> -Hwdec`.
- **`map_frame` failure contract:** on SW-upload failure it must `talloc_free`
  `mpi` (+ any async-callback ref) and return false — already handled inside
  `gpu_next_core_upload_sw_planes`; the core `map_frame` just propagates.
- **The screenshot path** (`video_screenshot`) drives `pl_queue_update` over
  the *same* queued frames, so it uses the same `map`/`unmap` callbacks — no
  separate handling; moving them to the core covers it automatically.
- **`unmap_frame` ordering at teardown:** `gpu_next_core_destroy` already
  destroys the queue first so in-flight `unmap` runs before the `sub_tex`
  pool / hwdec mapper are freed — unchanged, but keep it that way.
- Per-commit gate (WSL `~/mpv-fork`):
  `bash _golden/capture.sh ./build/mpv _golden/cand && bash _golden/verify.sh _golden/baseline _golden/cand`
  then `bash _golden/teardown.sh ./build/mpv`. All 12 cases byte-identical,
  teardown all clean.
