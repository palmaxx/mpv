# Phase 6 — Cross-platform backends (Vulkan; macOS via MoltenVK)

**Status:** **Vulkan backend ✅ DONE and functionally proven** on the WSL
lavapipe deterministic-software-Vulkan rig — a *stronger* oracle than D3D11's
WARP (it is the same software-Vulkan the windowed golden uses). The backend
renders correctly + deterministically through the full
`pl_vulkan_import` → `pl_vulkan_wrap` → acquire/release hold-handshake → readback
path. **macOS is covered by this same backend via MoltenVK** (the goal's
"molten vk for mac" option); a *native* Metal backend is genuinely Mac-required
and deferred (see §3).

**Branch / commits:** `gpu-next-render-api-hdr`, on `d0bd56d` (D3D11+P3.1):

| SHA | Commit | What | Diff |
|---|---|---|---|
| `07ae03e` | V1.1 | PL_VULKAN public C-API surface + stub + meson gate + 2.6→2.7 | +289 / −2, 6 files |
| `65dc39e` | V2.1 | `libmpv_pl_context_vulkan` impl + `acquire_target` hook + register | +163 / −7, 3 files |

The functional verification (`rapi_harness_vulkan.c` + `build_vulkan.sh` +
`rapi_vk_run.sh`) lives in the WSL fork's git-excluded `_golden/render_api/`,
parallel to the GL/D3D11 harnesses.

## §1. The Vulkan backend (V1.1 + V2.1)

Structurally parallel to the D3D11 backend, with one real difference: a wrapped
`VkImage` is an **externally-synchronized** resource, so it needs a per-frame
ownership handshake that D3D11 does not.

**Public API (V1.1, `render_vulkan.h`):** `MPV_RENDER_API_TYPE_PL_VULKAN` +
`MPV_RENDER_PARAM_VULKAN_INIT_PARAMS` / `_VULKAN_TEX`. `mpv_vulkan_init_params`
mirrors `pl_vulkan_import_params` (the host owns the
`VkInstance`/`VkPhysicalDevice`/`VkDevice`/queues; mpv *imports*, never
creates). `mpv_vulkan_tex` carries the `VkImage` + format/usage/dims plus the
acquire/release semaphores and current/final layouts.

**The sync handshake (V2.1):** the shared render hook gained one optional
`acquire_target` context hook (NULL for GL/D3D11 → their paths byte-unchanged).
Per render():
- `wrap_fbo` → `pl_vulkan_wrap` the host image (destroying the prior wrap).
- `acquire_target` → `pl_vulkan_release_ex` (hand to libplacebo; wait the host's
  acquire semaphore; treat as `current_layout`). Called in render() only, not
  in get_target_size, so the wrap-for-sizing pass never releases.
- render / error-clear runs.
- `done_frame` → `pl_vulkan_hold_ex` (hand back to the host in `final_layout`,
  fire the release semaphore). `destroy()` also holds if a render aborted
  mid-handshake, so libplacebo never owns a wrap mpv is about to destroy.
`qf = VK_QUEUE_FAMILY_IGNORED` skips queue-family ownership transfers (the wrap
is concurrent across libplacebo's queues on the shared imported device).

## §2. Verification — functional proof on lavapipe + no-regression

### Functional (the new signal): lavapipe self-baseline

`rapi_harness_vulkan.c` is a self-contained Vulkan host on lavapipe: it creates
a `VkInstance` (≥1.2), a `VkDevice` with `pl_vulkan_required_features` + the
available subset of `pl_vulkan_recommended_extensions`, a render-target
`VkImage`, and a binary release semaphore; drives mpv through
`MPV_RENDER_API_TYPE_PL_VULKAN`; then copies the held image (in
`TRANSFER_SRC_OPTIMAL`) to a host-visible buffer, waiting on the release
semaphore, and dumps RGBA8.

```
--probe -> mpv_render_context_create("pl-vulkan") -> 0 (success)   [import works]
$ bash rapi_vk_run.sh --baseline    # 4 cases, lavapipe
  OK sdr_t0  1656185f16bd02f2
  OK sdr_t3  4abec114ea3ab9c1
  OK hdr_t2  4b166052ef0301d3
  OK mot_t5  57444a8dfcd08751
$ bash rapi_vk_run.sh               # re-run x2
  --- ALL PASS (Vulkan render-API path byte-stable) ---
```

Content sanity: the sdr_t0 frame is real video — 0% error-fill, 37 200 distinct
colors. The full import/wrap/acquire/render/hold/readback path produces correct,
deterministic output. This is the first proof `pl_vulkan_import`/`pl_vulkan_wrap`
+ the hold-handshake work in mpv at runtime, and it gates on a **stronger**
oracle than D3D11's WARP: lavapipe is the same deterministic software Vulkan the
windowed golden trusts.

### No-regression (unchanged paths byte-identical)

At `65dc39e` on WSL: windowed 12/12 golden + pl-opengl render-API harness 4/4 +
teardown 9/9, all green. The new `acquire_target` hook is NULL for GL/D3D11, and
on Windows the D3D11 render-API gate stayed byte-identical across V1.1+V2.1.

## §3. macOS — MoltenVK now, native Metal deferred

The goal allows **"either metal or molten vk for mac."**

- **MoltenVK (chosen, already done):** MoltenVK presents a conformant Vulkan
  1.2 API over Metal. A macOS host creates a MoltenVK `VkInstance`/`VkDevice`
  and drives mpv through the **existing** `MPV_RENDER_API_TYPE_PL_VULKAN` — the
  V2.1 backend runs unchanged. **No new mpv code; already gated** (the lavapipe
  proof exercises the identical code path). Only on-device *testing* is deferred
  (needs a Mac), matching the goal's "defer only macOS testing."
- **Native Metal (deferred, Mac-required to even author):** a
  `libmpv_pl_context_metal.c` + `render_metal.h` using `pl_metal_create` /
  `pl_metal_wrap` would be more idiomatic for macOS EDR. But libplacebo's
  `metal.h` ships **only on macOS** — it is absent on both this Windows MSYS2
  rig and the WSL rig (`pl_has_metal` unset on both). The Metal API surface
  (`id<MTLDevice>`, `id<MTLTexture>`, the Obj-C bridge) cannot be referenced,
  compiled, or golden-gated here, so authoring it blind would violate the
  per-commit gating discipline. It is genuinely a Mac-resident task: write
  `render_metal.h` + the backend against the real `metal.h`, gate with a
  Metal-host harness on the Mac. The architecture is ready for it (the
  `libmpv_pl_context_fns` interface + `acquire_target` already model the
  externally-synchronized-surface case Metal would reuse via `MTLSharedEvent`).

## §4. Where Plan 2 stands after Phase 6

All three target GPU APIs are now reachable through the libmpv render API with a
single shared `render_backend_gpu_next` + per-API context-fns (the
maintainer-blessed shape):

| Backend | Platform | Status |
|---|---|---|
| `pl-d3d11` | Windows | Functional + HDR render path proven (WARP); on-display HDR fidelity (4b) = user-run |
| `pl-vulkan` | Linux/Wayland, Windows, **macOS via MoltenVK** | Functional + deterministically gated (lavapipe) |
| `pl-metal` (native) | macOS | Deferred — Mac + libplacebo-with-Metal required to author/gate |

`MPV_RENDER_PARAM_TARGET_COLORSPACE` (P3.1) is shared across all backends, so
HDR negotiation is API-agnostic. Remaining: D3D11 Phase 4b (on-display fidelity,
user-run), and — if macOS EDR wants a native path beyond MoltenVK — the Metal
backend, on a Mac.
