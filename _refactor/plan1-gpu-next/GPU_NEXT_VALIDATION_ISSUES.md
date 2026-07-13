# gpu-next validation issues

> **Resolution status (2026-06-01, post-upstream-rebase).** Branch tips are
> now `gpu-next-render-api` = `82a1428`, `gpu-next-render-api-hdr` = `98fbe55`.
> - **#5 (dead `gpu_next_core_renderer`) — FIXED** (`1dc4802`): decl+def removed.
> - **#2 (`VO_CAP_FILM_GRAIN`) — RESOLVED as a no-op cleanup** (`82a1428`).
>   Investigation: `render_backend.driver_caps` is **never propagated to
>   `vo->driver->caps`** (the caps the decoder reads) — dead in mainline too
>   (`libmpv_gpu.c` sets it, nothing reads it). So the flag had no effect and
>   film grain was **not** broken: auto mode → decoder applies it on CPU;
>   `--vd-lavc-film-grain=gpu` → `gpu_next_core` applies it via `pl_film_grain`
>   regardless of the cap. Fix = match `render_backend_gpu` (drop the no-op
>   flag). *Actually negotiating GPU film grain over the render API in auto
>   mode* needs cross-cutting VO-caps plumbing (the decoder reads a const
>   static driver caps shared by gpu/gpu_next/**sw**) + AV1-FGS test content —
>   tracked as a separate optional enhancement, NOT a Plan-1 item.
> - **#1 (native-resource forwarding) — OPEN, deferred** (Linux GL hwdec only;
>   Windows D3D11/EGL+CUDA unaffected — not on the user's path).
> - **#3 (render-API hwdec functional-on-HW) — OPEN**, rides Plan 2's D3D11 HW harness.
> - **#4 (commit hygiene) — OPEN**, only matters for an upstream PR.

Scope: canonical WSL repo at `\\wsl.localhost\Ubuntu\home\maxde\mpv-fork`, branch
`gpu-next-render-api`, reviewed at `286b788` after the three new hwdec commits:

- `cbcd48c` `vo_gpu_next: add ra fields to libmpv_pl_context (F1.1)`
- `44bcecf` `vo_gpu_next: build ra_ctx alongside pl_opengl in libmpv_pl_context_gl (F1.2)`
- `286b788` `vo_gpu_next: wire hwdec on the libmpv render API (F1.3)`

No source code was edited during this pass.

## Verification performed

- `git status --short --branch`: clean on `gpu-next-render-api`.
- `ninja -C build`: passed in WSL.
- `./build/mpv --version`: ran and reported `mpv v0.41.0-dev-g286b7883f`.
- `git diff --check HEAD~3..HEAD`: clean.
- `meson test -C build --print-errorlogs`: `No tests defined.`
- `_golden/verify.sh _golden/baseline _golden/cand`: 12/12 windowed gpu-next cases passed,
  pixels and metadata identical.
- `_golden/render_api/build.sh` plus `_golden/render_api/rapi_run.sh`: render-API harness rebuilt
  and passed 4/4 self-baselined cases, pixels and metadata identical.

## What the three new commits fix

The previous PL_OPENGL/gpu-next render path was effectively software-decode only. These commits now
give the PL_OPENGL wrapper an `ra_ctx`/`ra`, create `ctx->hwdec_devs`, initialize `ra_hwdec_ctx`, and
pass `.ra`, `.hwdec_get`, and `.hwdec_ctx` into the shared gpu-next core. That addresses the biggest
capability mismatch in the earlier validation note for the OpenGL path.

## Remaining issues

1. Native render resources are still not forwarded to the new PL_OPENGL RA.

   The old `video/out/gpu/libmpv_gpu.c` render backend copies native render params into the RA before
   initializing hwdec:

   - `MPV_RENDER_PARAM_X11_DISPLAY` -> `"x11"`
   - `MPV_RENDER_PARAM_WL_DISPLAY` -> `"wl"`
   - `MPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE` -> `"drm_draw_surface_size"`
   - `MPV_RENDER_PARAM_DRM_DISPLAY_V2` -> `"drm_params_v2"`

   The new gpu-next path creates `p->context->ra_ctx` and initializes `ra_hwdec_ctx`, but does not
   mirror that `ra_add_native_resource()` loop. Several GL hwdec interops depend on those resources,
   including VAAPI, VDPAU, DRM PRIME, and DRM PRIME overlay. Windows D3D11/EGL and CUDA-style paths
   may still work, but Linux GL render-API hwdec parity is likely incomplete.

2. `VO_CAP_FILM_GRAIN` is still advertised by the gpu-next render backend but is not effective for
   `vo_libmpv`.

   - `video/out/gpu_next/libmpv_gpu_next.c` sets `ctx->driver_caps =
     VO_CAP_ROTATE90 | VO_CAP_FILM_GRAIN | VO_CAP_VFLIP`.
   - `video/out/vo_libmpv.c` still exposes hardcoded `.caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP`.
   - Decode-side film-grain handling checks `ctx->vo->driver->caps`, not the backend-local
     `render_backend.driver_caps`.

   Result: AV1 film grain side data likely remains disabled on the new libmpv PL_OPENGL path despite
   the backend advertising support internally.

3. The harnesses pass, but they do not prove real hardware hwdec engagement.

   The WSL render-API harness is a deterministic software/llvmpipe-style byte-stability gate. It
   proves the new RA/hwdec plumbing did not perturb the rendered output in that environment, but it
   does not show that D3D11VA, VAAPI, VDPAU, CUDA, or DRM PRIME actually load and map frames through
   PL_OPENGL. The commit message correctly calls out a separate real-hardware gate; that still needs
   to be treated as mandatory before calling hwdec done.

4. Review-hygiene concerns remain for upstreamability.

   The new commits are tightly scoped, but they still add fairly explanatory comments and each commit
   contains a `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>` trailer plus long harness
   notes. For an mpv PR, squash/reword and prune comments to non-obvious invariants; the related
   upstream review on mpv-player/mpv#16818 explicitly objected to excessive AI-looking commentary.

5. Minor cleanup: `gpu_next_core_renderer()` is still declared/defined but unused.

   This is not a behavior bug, but it looks like transitional API surface and should be removed or
   used before a maintainer review.

## Bottom line

The three new commits are a meaningful improvement: the earlier PL_OPENGL software-only objection is
no longer accurate for the canonical WSL branch, and both WSL golden gates pass. I would still not
call this merge-ready until native-resource forwarding is mirrored from `libmpv_gpu.c`, the film-grain
capability mismatch is resolved, and at least one real hardware hwdec path is shown to engage through
`MPV_RENDER_API_TYPE_PL_OPENGL`.
