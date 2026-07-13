-- Deterministic single-frame golden capture for vo=gpu-next.
--
-- On the first settled frame (playback-restart after --start, held by
-- --pause) this dumps, then quits:
--   $GOLDEN_OUT            PNG via screenshot-to-file "video"
--                          (gpu-next video_screenshot -> pl_render_image:
--                          exercises the extracted core path -- scaling,
--                          tone-map, hooks, ICC, target colorspace)
--   $GOLDEN_OUT + ".json"  curated, timing-free regression sidecar:
--                          video-params + video-out-params (colorspace /
--                          HDR mastering / sig-peak: the §3.1 signal) and
--                          the ordered libplacebo render-pass desc list
--                          (structural render regression; nanosecond
--                          timings deliberately dropped -> deterministic).
local utils = require "mp.utils"
local done = false
local function dump()
  if done then return end
  done = true
  local out = os.getenv("GOLDEN_OUT") or "golden.png"
  mp.commandv("screenshot-to-file", out, "video")
  local passes = {}
  local vp = mp.get_property_native("vo-passes")
  if vp and vp.fresh then
    for _, p in ipairs(vp.fresh) do passes[#passes + 1] = p.desc end
  end
  local rec = {
    video_params     = mp.get_property_native("video-params"),
    video_out_params = mp.get_property_native("video-out-params"),
    render_passes    = passes,
  }
  local f = io.open(out .. ".json", "w")
  f:write(utils.format_json(rec))
  f:close()
  mp.command("quit")
end
mp.register_event("playback-restart", dump)
