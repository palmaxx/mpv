#!/usr/bin/env python3
# Quick HDR-genuineness check for the Vulkan render-API harness outputs.
import struct, collections, sys

base = sys.argv[1] if len(sys.argv) > 1 else "vk_win"

def rd(p):
    with open(p, "rb") as f:
        return f.read()

# SDR rgba8: not error-fill (purple = 0x80,0x00,0xFF,0xFF) and has real spread.
sdr = rd(f"{base}/sdr.raw")
cnt = collections.Counter(sdr[i:i+4] for i in range(0, len(sdr), 4))
top, topn = cnt.most_common(1)[0]
print("SDR  rgba8 : distinct=%d, top=%s x%d (%.1f%%) %s"
      % (len(cnt), top.hex(), topn, 100.0*topn/(len(sdr)//4),
         "PURPLE ERROR-FILL!" if top.hex().lower().startswith("8000ff") else "real content"))

# pq10 A2B10G10R10_UNORM_PACK32 (LE uint32): R[9:0] G[19:10] B[29:20] A[31:30]
d = rd(f"{base}/pq10.raw")
mr = mg = mb = 0; nz = 0
for i in range(0, len(d), 4):
    v = struct.unpack_from("<I", d, i)[0]
    r = v & 1023; g = (v >> 10) & 1023; b = (v >> 20) & 1023
    mr = max(mr, r); mg = max(mg, g); mb = max(mb, b)
    if r or g or b: nz += 1
print("pq10 10bit : PQ code maxima R=%d G=%d B=%d /1023, nonzero=%.1f%% "
      "(high codes => genuine HDR PQ, >8bit range)" % (mr, mg, mb, 100.0*nz/(len(d)//4)))

# pq16f R16G16B16A16_SFLOAT: per-channel range (half float = struct 'e')
h = rd(f"{base}/pq16f.raw")
vmin = 1e9; vmax = -1e9; s = 0.0; n = 0
for i in range(0, len(h), 2):
    x = struct.unpack_from("<e", h, i)[0]
    vmin = min(vmin, x); vmax = max(vmax, x); s += x; n += 1
print("pq16f 16F  : value min=%.4f max=%.4f mean=%.4f (PQ-encoded into the float target)"
      % (vmin, vmax, s/n))
