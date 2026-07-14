# Phase 3 — Assessment Doc (wire TARGET_COLORSPACE into the render hook)

**Status:** ✅ COMPLETE and gated, both rigs. `MPV_RENDER_PARAM_TARGET_COLORSPACE`
now drives the render target's colorspace: a host can hand mpv a BT.2020/PQ (or
any) target per frame, and it flows through the same precedence machinery the
windowed VO uses for its swapchain-negotiated colorspace. The pl-opengl path
(which never passes the param) is byte-unchanged.

**Branch / commit:** `gpu-next-render-api-hdr` tip **`d0bd56d`** (P3.1), one
commit on `c0a347f` (P2.2). Diff: **+107 / −9, 1 file**
([libmpv_gpu_next.c](video/out/gpu_next/libmpv_gpu_next.c)).

## §1. What landed

P1.3 had read the param into a local and discarded it (`(void)target_cs;`).
P3.1 replaces that scaffold with real wiring:

1. **Enum translation (explicit switch, not a cast).** Two static helpers
   `map_primaries` / `map_transfer` map the frozen mpv-ABI enums
   (`mpv_color_primaries` / `mpv_color_transfer`, P1.1) to libplacebo's
   `pl_color_primaries` / `pl_color_transfer`. The mpv values currently align
   1:1 with libplacebo's, but the switch makes the independence real — a
   libplacebo enum reshuffle cannot silently change what a host's value means
   (the P1.1 design promise). `map_target_colorspace` builds the
   `pl_color_space`, mapping `mpv_hdr_metadata` field-for-field to
   `pl_hdr_metadata` (min/max_luma, max_cll, max_fall) and forwarding
   mastering-display primaries only when the host supplied a full chromaticity
   set (else libplacebo derives them from the primaries enum).

2. **Precedence wiring in `render()`.**
   - `target_known = target_cs && (primaries != AUTO || transfer != AUTO)` —
     a host pinning either is negotiating a real target; NULL or an all-AUTO
     struct (documented as "same as omitted") is *not*.
   - `target.color = target_known ? map_target_colorspace(target_cs) :
     pl_color_space_srgb`.
   - `gpu_next_core_apply_target_options(..., hint = target_known, depth)` —
     `hint=true` makes user `--target-prim/-trc/-peak` override only the
     fields the host left unset (AUTO/0). Verified against the core's
     condition `opts->target_X && (!target->color.X || !hint)`
     ([core.c:2355](video/out/gpu_next/core.c)).
   - `gpu_next_core_finalize_target_csp(..., &target_csp, !target_known)` with
     `target_csp = target_known ? host_csp : {0}` — feeds the Windows
     sRGB-as-PQ path (`treat_srgb_as_power22 & 4 && target_pq`,
     [core.c:2441](video/out/gpu_next/core.c)), which keys off
     `!target_unknown && target_csp->transfer == PL_COLOR_TRC_PQ`.

   When `target_known` is false the three lines reduce to *exactly* the
   pre-P3.1 values (`pl_color_space_srgb`, `hint=false`, `target_unknown=true`),
   so pl-opengl is byte-unchanged by construction.

## §2. Precedence semantics (the §5.2 Phase-0 hazard)

Phase-0 flagged "user has `--target-trc=pq` set *and* host supplies
TARGET_COLORSPACE — which wins?" Resolved exactly as the windowed swapchain-hint
path: the host value is the swapchain-equivalent **baseline**; a user
`--target-*` opt overrides it **only for fields the host left at AUTO/0**. With
`hint=true`, a host that pinned `max_luma=1000` keeps it even if the user passes
`--target-peak=600` (the host "pinned" it); a host that left `primaries=AUTO`
lets `--target-prim` through. This is the same `(!set_by_host || !hint)` gate
the windowed VO has always used — no new policy, no special-casing.

## §3. Verification

### Windows — D3D11 WARP self-baseline (the new HDR-relevant signal)

The harness (`_golden/render_api/rapi_harness_d3d11.c`) gained an optional
`[none|srgb|pq2020]` arg selecting the per-frame TARGET_COLORSPACE. Matrix
re-baselined (4 cases), three invariants, all green and byte-stable run-to-run:

```
  OK   sdr_t0   csp=none    f861c22d7992d3ce   ← byte-identical to the P2.2 baseline
  OK   srgb_t0  csp=srgb    f861c22d7992d3ce   ← all-AUTO struct == omitted
  OK   pq_t0    csp=pq2020  6328aa45fb40a136   ← BT.2020/PQ: different, deterministic
  PASS srgb==none : TARGET_COLORSPACE sRGB is a no-op vs omitted
  PASS pq!=none   : BT.2020/PQ TARGET_COLORSPACE changes the render
```

- **No-op proof:** `srgb_t0` (all-AUTO struct passed) is byte-identical to
  `sdr_t0` (param omitted) — the documented equivalence holds, and the wiring
  is provably inert unless the host pins something.
- **Effect proof:** `pq_t0` differs (`6328aa45…`), is real content (0%
  error-fill, 13 135 distinct colors, mean RGB shifted (123,168,129) →
  (138,153,140) vs `sdr_t0`) — TARGET_COLORSPACE=BT.2020/PQ genuinely reaches
  libplacebo and re-targets the render. On the SDR R8G8B8A8 WARP readback the
  PQ output is washed/clamped, exactly as the plan predicts; true HDR fidelity
  is the Phase-4 NVIDIA gate.
- **No-param pixels unchanged across the P3.1 change:** `sdr_t0`/`sdr_t2`
  hashes are identical to the pre-P3.1 P2.2 baseline (`f861c22d…`/`9c974824…`).

### WSL — no-regression oracle (pl-opengl + windowed untouched)

At `d0bd56d`, first run, all green:

```
windowed lavapipe 12-case golden : 12/12 PASS
pl-opengl render-API harness      : 4/4  PASS (byte-stable)
teardown (9 lifecycle cases)      : 9/9  CLEAN
```

pl-opengl never passes TARGET_COLORSPACE, so `target_known` is always false
there → the hook is byte-identical to pre-P3.1. Confirmed empirically (the
render-API harness hashes match the self-baseline).

## §4. Merge-worthiness

+107/−9, one file, no public-API change (the surface shipped in P1.1). The diff
is two translation helpers + a 6-line precedence wiring change; comments explain
*why* (the independence-via-switch rationale, the hint precedence) without
narrating the code. No churn elsewhere. Phase 4 extends the harness, not this
hook.

## §5. Phase 4 preview — the real go/no-go

Per [plan-hdr-render-api.md §"Phase 4"](plan-hdr-render-api.md): prove the
render-API HDR path is pixel-faithful to the windowed `--vo=gpu-next` path on
the real NVIDIA D3D11 HDR display.

- **Phase 4a (autonomous, deterministic):** extend the D3D11 harness with a
  true HDR target — `DXGI_FORMAT_R10G10B10A2_UNORM` (HDR10) and/or
  `R16G16B16A16_FLOAT` (scRGB) render target + BT.2020/PQ TARGET_COLORSPACE,
  headless WARP readback, self-baselined. Proves the HDR render *path* (PQ
  encoding into a ≥10-bit target) is correct and stable without needing the
  display. The 8-bit R8G8B8A8 readback cannot represent PQ headroom; a 10/16-bit
  target can.
- **Phase 4b (real display, user-run):** `run-hdrval-rapi.ps1` — the 3-run
  base1/base2/ref dance on the NVIDIA rig with the display in HDR mode,
  reference = the windowed `vo_gpu_next` render on the same display. PNG hashes
  + negotiated HDR metadata must match within GPU noise. **If not faithful,
  abandon to the `--wid=HWND` fallback** (the documented kill criterion — a
  *successful* time-box outcome).
