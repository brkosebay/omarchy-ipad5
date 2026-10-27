# Video display path: CPU cost today, and an NV12 overlay for apple_adbe_drm

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Round 7 of the video decoder lane (2026-10-24). Analysis only: nothing on
`a9/display` was edited; the display driver work it describes is a separate
round. Marks: **[V]** verified in the kext (address given) or on the device
(log given), **[I]** inferred, **[U]** unresolved. Ghidra names were added to
the shared analysis for every function cited (`adp_*`).

## 1. Bottom line

> Private graphics-dependent excerpt omitted from this source snapshot.

## 2. Where the CPU goes today, and how to measure it

On-screen path in the Omarchy session (round 6 T12, `gst-play-1.0` →
playbin → waylandsink): decoder writes NV12 (hardware, 52.8 fps at 1080p
with no copy) → waylandsink offers the NV12 dmabuf, the compositor can't
import it (llvmpipe, no render node) → playsink inserts `videoconvert` →
**NV12→BGRx on the CPU in the player** (1920×1080, ~8 MB/frame written)
→ wl_shm buffer → **Hyprland uploads it into llvmpipe and composites it
scaled to 1536×864 on the CPU** → simpledrm-style dumb buffer → ADBE.
mpv `--hwdec=v4l2request-copy` adds a third copy (reading the dmabuf into
system memory) before its own conversion.

Every one of those CPU readers reads **uncached** memory today: the VXD
node is not `dma-coherent`, so dma-contig maps CAPTURE buffers Normal-NC.
Round 7's `capture_cached` parameter (`a9/vdec` `9a897a8fd2`, default off)
allocates them cacheable with vb2's own clean/invalidate at QBUF/DQBUF;
T18 measures the copy and conversion paths with it off and on and checks
bit-exactness with the caches in the loop.

Device measurements (all from SSH, `sudo`, the 300-frame `clip1080.h264`),
each changing one thing:

| Step | Pipeline | What it isolates |
| --- | --- | --- |
| A | `v4l2slh264dec ! fpsdisplaysink video-sink=fakevideosink sync=false` | decoder only (round 6: 52.8 fps) |
| B | `v4l2slh264dec ! videoconvert ! video/x-raw,format=BGRx ! fpsdisplaysink video-sink=fakesink sync=false` | + CPU conversion |
| C | `... ! videoconvertscale ! video/x-raw,format=BGRx,width=1536,height=864 ! ...` | + CPU scaling |
| D | `... ! fpsdisplaysink video-sink=waylandsink sync=true` in the session, `top -b -n 3 -d 2` alongside | + compositor (rendered/dropped, Hyprland's CPU %) |
| E | `systemctl stop sddm`, then `v4l2slh264dec ! videoconvert ! kmssink` | conversion without a compositor |

If B ≈ A, conversion is cheap and the compositor (D vs B) is the cost; if
B ≪ A, conversion is the first thing to remove (the overlay below removes
both B and D's composition work for full-screen video). The exact commands
are in `vdec-test-plan.md` §R7.7 (T17).

## 3. What the kext shows

### 3.1 Two layers, per-layer surfaces **[V]**

`adp_program_layers_gated` (`0xfffffff005d20d78`) loops `layer = 0, 1`.
For each it takes the swap's surface from slot `this+0x9af8+4*layer` (4 =
none) and calls `adp_program_layer_planes(this, pusher, layer, surface,
...)`, which writes **bank `0x4000 + 0x2000*layer`**. After both layers it
writes ADP control `0x1038` with **bit 0 = layer 0 on, bit 4 = layer 1 on**
(the live value is `0x00011001`: layer 0 only, `phaseA.txt`), and per-layer
blend words through `adp_program_layer_blend` (`0xfffffff005d22614`):
`0x2028 + 4*layer = 0xaaaffe8a` (no surface), `0xaaaffe9a` (YCbCr, or
blend flag), `0xaaaffeaa` (RGB), OR the layer index's low bit.

### 3.2 The plane loop (one layer) **[V]**

`adp_program_layer_planes` iterates `p = 0, 1` while
`p < adp_surface_plane_count(surface)` (IOSurface plane count, 1 if not
planar). Registers, all `+0x2000*layer`, all through the parameter FIFO
(`fifoCache`, `ds-display-kms.md` §13):

| Register | Value |
| --- | --- |
| `0x402c` | format word (below), once per layer |
| `0x4038 + 4p` | plane p bytes per row, `& ~0x3f \| 0x2a` (must be 64-aligned when linear, 4 KiB when tiled: `adp_check_stride_align`) |
| `0x4030 + 4p` | plane p device address, `& ~0x3f \| 0x2a` |
| `0x4040 + 4p`, `0x4048 + 4p` | plane p source rectangle (origin, size), already divided by the plane's subsampling: `0x80008000 \| …` |
| `0x4094 + 4p`, `0x40a4 + 4p` | plane p horizontal / vertical step, 16.16 |
| `0x409c + 4p`, `0x40ac + 4p`, `0x412c + 4p` | plane p scaler phase/increment words (`adp_program_plane_scaler` `0xfffffff005d2388c`) |
| `0x406c + 4i`, `0x407c + 4i` | destination position / size, `0x4068` enable mask (as for layer 0 today) |
| `0x4050`, `0x4054` | destination window |

Subsampling per plane: `adp_fmt_plane_hsub`/`_vsub` (`0x…35b74`,
`0x…35cf0`) return 2 for plane 1 of `'420v' '420f' 'x420' 'xf20'
'xw20' 'p420' 'pf20'` (both directions) and of the 4:2:2 formats
(horizontal only), 1 otherwise.

### 3.3 The format word `0x402c` **[V]** (layout) / **[I]** (bit names)

`0x402c = base | (local_fc & 0xfff00fff)` with `base = 0xc6000` for
`'RGBA'` and `0xe4000` for everything else, and

| Bits | Source | `'BGRA'` | `'420v'` |
| --- | --- | --- | --- |
| 0 | surface is linear (not tiled/compressed) | 1 | 1 |
| 1, 2 | x/y reflect (transform) | 0 | 0 |
| 20 | always set by iOS | (iBoot: 0) | 1 |
| 21 | `adp_fmt_is_rgb` | 1 | 0 |
| 22 | plane count == 2 | 0 | **1** |
| 25:24 | `adp_fmt_depth_class`: 0 = 8-bit YCbCr, 1 = 32-bit RGB and 16-bit-container YCbCr (`'x420'`), 2 = packed 10-bit (`'p420'`), 3 = 30-bit RGB | 1 | 0 |
| 27:26 | `adp_fmt_chroma_class`: 0 = 4:4:4, 1 = 4:2:2 and RGB, 2 = 4:2:0 | 1 | **2** |

Check against the device: the model gives `0x053e4001` for linear BGRA as
iOS writes it; iBoot's live value is **`0x052e4001`** (`phaseA.txt`), the
same except bit 20, which iBoot leaves clear. For NV12 (`'420v'`, linear)
the model gives **`0x085e4001`** (iOS) / `0x084e4001` (iBoot style).

### 3.4 Colour conversion **[V]**

`adp_csc_matrix_id` (`0xfffffff005d36750`) picks a matrix for the layer's
surface; `adp_program_csc` (`0xfffffff005d23c38`) writes it:
`0x40b8 + 4i` pre-offset (11-bit two's complement), `0x40c4 + 0xc*i + 4j`
coefficient (16-bit, Q12), `0x40e8 + 4i` post-offset, rows i = R, G, B;
then `0x40b4 = 0xaaaaaaaa` (CSC on) or `0xaaaaaaab` (bypass, RGB layers).
The table (`adp_csc_table`, pointers at `0xfffffff006dbbfe8`) read out of
the kext:

| id | When | Pre-offset (10-bit units) | Matrix (×4096) |
| --- | --- | --- | --- |
| 0x0c | BT.601, full range | 0, −512, −512 | `[4096 0 5743; 4096 −1410 −2925; 4096 7258 0]` |
| 0x0d | BT.601, video range | −64, −512, −512 | `[4769 0 6537; 4769 −1605 −3330; 4769 8263 0]` |
| 0x0e | BT.709, full range | 0, −512, −512 | `[4096 0 6450; 4096 −767 −1917; 4096 7601 0]` |
| 0x0f | BT.709, video range | −64, −512, −512 | `[4769 0 7343; 4769 −873 −2183; 4769 8652 0]` |

These are the textbook coefficients (1.164, 1.596/1.793, 2.017/2.112 …),
so the pipe converts in a 10-bit space with Q12 coefficients. Selection:
YCbCr matrix attribute 1 or 7 → BT.709 (0x0e/0x0f), otherwise BT.601
(0x0c/0x0d); video vs full range picks the odd/even id.

### 3.5 Scaling **[V]** (structure) / **[U]** (coefficient choice)

Non-1:1 scaling needs, per plane, the 16.16 steps, the three
phase-increment words of §3.2 and a polyphase coefficient set: constant
0x48c-byte tables at `0xfffffff0053ab5c0 + n*0x48c`, picked by ratio
(`0xfffffff005d23b48`) and merged by `adp_merge_scaler_lut`
(`0xfffffff005d23a68`) into a 0x120-word shadow (plane 0 in bits 15:0,
plane 1 in bits 31:16) uploaded through the `0x5000` (horizontal) and
`0x5800` (vertical) windows of the bank. A first overlay can run 1:1
(steps `0x10000`) and leave the coefficient upload for later.

## 4. What apple_adbe_drm would need (later display round)

1. **A second `drm_plane`, `DRM_PLANE_TYPE_OVERLAY`, zpos 1 (immutable)**,
   formats `NV12` (plus `XRGB8888`/`ARGB8888`, same bank), modifier
   `LINEAR` only. Properties: `COLOR_ENCODING` (BT.601/BT.709 → ids
   0x0d/0x0f or 0x0c/0x0e), `COLOR_RANGE` (limited/full), `zpos`.
2. **atomic_check**: base and every plane offset 64-byte aligned; pitch a
   multiple of 64; source width ≤ 2048; `src == dst` size until the scaler
   is done (reject scaling with -ERANGE); destination fully on screen (the
   pipe has no negative positions in the 14/15-bit fields).
3. **atomic_update**, appended to the same parameter-FIFO packet as the
   primary flip (`adbe_fifo_set_plane()` style, every value with the
   `0x2a` tag): `0x602c` (`0x085e4001` for NV12 video range), `0x6030` Y
   base, `0x6034` CbCr base, `0x6038`/`0x603c` pitches, `0x6040`/`0x6044`
   source origins (chroma origin halved), `0x6048`/`0x604c` source sizes
   (chroma halved), `0x6094`/`0x6098`/`0x60a4`/`0x60a8 = 0x10000`, the
   CSC block `0x60b4…0x60f0`, destination `0x606c`/`0x607c`, `0x6068`
   enable mask, `0x6050`/`0x6054` window, blend `0x202c = 0xaaaffe9b`,
   then `0x1038 |= 0x10`. Disable = clear bit 4 of `0x1038`.
4. **PRIME import**: `drm_gem_dma_prime_import_sg_table` accepts the
   decoder's dma-contig buffers (one contiguous segment) as they are; with
   the DART in bypass the scanout address is the physical address plus the
   bus offset `0x800000000`, as for the primary plane today.
5. **Lifetime**: keep the imported buffer referenced until the flip that
   replaces it has completed (the 0x400 completion the driver already
   counts), or the decoder may overwrite a frame on screen.
6. **Unknown, check on the device before relying on it**: whether layer 1
   needs the same one-time pipe init iBoot did for layer 0 (scaler LUT,
   `0x4020`–`0x4028`-style words: dump `0x6000…0x6200` at probe), and the
   blend order (layer 1 above layer 0 by default? the `0x2020` word).

> Private graphics-dependent excerpt omitted from this source snapshot.

