# Native MSYS2 real-HDR validation runbook

Source is already staged: branch **gpu-next-render-api** (5ad4900,
0d612ff, a12365c on 35ae76d) exists in the Windows clone
`c:\DEV\ai-dev\projects\mpv-src`. Compare a12365c vs 35ae76d, native
Windows mpv.exe, NVIDIA GPU, display in HDR mode.

## 0. BLOCKING PREREQUISITE (user)
Free C:\ to **>= ~28 GB** (MSYS2 ~3-5 GB + mingw deps incl.
ffmpeg/libplacebo/libass ~8-12 GB + 2 build trees ~3-4 GB + headroom).
Then tell the agent "disk freed". Nothing below starts until then --
do not run a multi-GB install on a near-full system drive.

## 1. Install MSYS2 (agent, scripted)
`winget install -e --id MSYS2.MSYS2` (winget is present). MSYS2 lands at
`C:\msys64`. All further commands run in the **UCRT64** shell, driven
non-interactively as:
`C:\msys64\usr\bin\bash.exe -lc "<cmd>"` with `MSYSTEM=UCRT64`.

## 2. Toolchain + deps (pacman, --noconfirm)
`pacman -Syu --noconfirm` (twice; first may close the shell), then:
```
pacman -S --noconfirm --needed \
  mingw-w64-ucrt-x86_64-{gcc,meson,ninja,pkgconf,lua51,ffmpeg,libass,\
  libplacebo,luajit,python} git
```
**libplacebo version check:** `pkgconf --modversion libplacebo` in
UCRT64 must be **>= 7.360.1** (mpv meson requirement). MSYS2 is rolling
and normally current; if it is older, build libplacebo from source into
/ucrt64 the same way it was done in WSL (tag v7.360.1, -Dvulkan=enabled
-Dshaderc=enabled -Dlcms=enabled -Dd3d11=enabled -Dopengl=enabled).
Note: enable **d3d11** here (real Windows swapchain; this is the path
under test) -- unlike the WSL/Vulkan-only build.

## 3. Build both commits (agent)
In UCRT64, repo = /c/DEV/ai-dev/projects/mpv-src:
```
git config --global --add safe.directory /c/DEV/ai-dev/projects/mpv-src
for C in 35ae76d a12365c; do
  git -C /c/DEV/ai-dev/projects/mpv-src worktree add ../mpv-$C $C
  meson setup /c/DEV/ai-dev/projects/mpv-build-$C ../mpv-$C \
      --buildtype=release -Dlua=enabled
  ninja -C /c/DEV/ai-dev/projects/mpv-build-$C mpv.exe
done
```
(Worktrees keep both builds side by side off the one shared .git.)
Confirm each `mpv.exe --version` prints libplacebo v7.360.x.

## 4. Harness on real HDR (USER runs; agent cannot observe HDR)
Copy `_golden/golden.lua` and the HDR clip from WSL to the Windows repo
(agent does this). Then, with the **display switched to HDR mode in
Windows Settings**, the user runs for B in {35ae76d, a12365c}:
```
set GOLDEN_OUT=%CD%\hdrval\%B%_hdr.png
mpv-build-%B%\mpv.exe --no-config --no-audio --ao=null --fullscreen ^
  --vo=gpu-next --gpu-api=d3d11 --pause --osc=no --interpolation=no ^
  --tone-mapping=bt.2390 --hdr-compute-peak=no --target-colorspace-hint=yes ^
  --start=2.0 --script=_golden\golden.lua _golden\clips\hdr10_4k.mp4
```
(`--gpu-api=d3d11` + real HDR swapchain is exactly the
target_csp/set_colorspace_hint path the refactor touches.)

## 5. Acceptance (agent diffs)
- **Primary signal (must be bit-equal):** the `.png.json` sidecar
  (video-params/video-out-params: max/min-luma, max-cll, max-fall,
  primaries, transfer, sig-peak; render-pass list) for a12365c vs
  35ae76d. This is the negotiated target.color / vo->params->color.hdr
  Phase 0 §3.1 names. **Any diff here = real-HDR kill signal.**
- **Pixel signal:** real GPU may not be bit-deterministic run-to-run.
  So also capture 35ae76d **twice** = the GPU-noise floor. a12365c vs
  35ae76d pixel delta must be within that floor (ideally identical).
- Verdict written to HANDOFF.md; if sidecar bit-equal => the kill-point
  is retired on real HW and the Phase 2 long tail may resume.
