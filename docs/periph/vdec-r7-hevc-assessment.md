# HEVC on the A9 VXD: effort assessment (round 7)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24, video decoder lane. **Decision: not started this round.** The
emitter is decoded bit for bit, but too little of it is tied to HEVC
syntax, and unlike H.264 there is no open-source register reference to
cross-check against. The next step is a reverse-engineering round (routing:
Codex), then a driver round with the same host word-for-word method as
rounds 5 and 6. Everything userspace needs is already in place (§4).

## 1. What exists

- `ds-vxd-cmdstream.md` §1–§15 decode Apple's **HEVC** picture path
  (`0xfffffff0062960a0`, NAL types 32/33/34; §16.0 proved the codec). The
  word grammar, the per-slice framing (`0x9`/`0xb`/`0xa` commands, 64-byte
  pad), the CMD (`0x30xx`/`0x34xx`) and ENT (`0x50xx`/`0x54xx`/`0x58xx`)
  register groups, the 0x5800 scaling-list encoding (§15.5) and the
  render/window program `0x62b7ab0` (§16.1: `0x70a0` render config,
  `0x70a4` tile counts, `0x70b0`/`0x70b4` sizes, `0x71b4` chroma format and
  bit depths, `0x7100+8k` reference surfaces, `0x700c`/`0x7010` output) are
  **[V]**: 114 [V], 38 [I], 25 [U] marks across those sections.
- §16.2 maps the HEVC slice structure (slice type B=0/P=1/I=2,
  num_ref_idx, SliceQpY, the two reference-index lists, weights) **[V]**.
- The mailbox, firmware boot, GART, command buffers, fences and the V4L2
  stateless plumbing are shared with H.264 and work on the device.
- A third source checked this round: Imagination/TI's 2021 vxd-dec staging
  series ("img,d5500-vxd", the same D5500 family). It does **not** help with
  registers: its host hands parsed headers (`hevcfw_*` structures,
  `hevcfw_data.h`) to IMG's closed firmware, which writes the PVDEC
  registers itself; its register headers name only the buffer bases
  (`PVDEC_ENTROPY_CR_GENC_BUFFER_{SIZE,BASE_ADDRESS}` `0x100`/`0x110`,
  `..._GENC_FRAGMENT_BASE_ADDRESS` `0x98`, `PVDEC_VEC_BE_CR_GENC_*`
  `0x30`/`0x40`/`0x50`, `..._ABOVE_PARAM_BASE_ADDRESS` `0xc0`). Two uses:
  the buffer names agree with Apple's `CHEVCDecoder::AllocatePictureMemory`
  (temporal MV, above parameters, slice parameters, fragment pointers), and
  `hevcfw_data.h` is a checklist of every syntax element IMG's program
  consumes (`lkml.iu.edu/hypermail/linux/kernel/2108.2/03492.html`, patch 14).

## 2. What is missing (all host-side RE, no device needed)

| # | Item | Where | Why it blocks |
| --- | --- | --- | --- |
| 1 | SPS syntax → parser state → `ctx+0x13c3xx` copies | parseSPS `0x62a7680` | every picture word reads C[] fields whose syntax meaning is [U] (e.g. `C[13c364]` bit depths/chroma are [V] only by shape) |
| 2 | PPS syntax → C[] | parsePPS `0x62aa954` | tiles, WPP, sign hiding, transquant bypass, deblocking/SAO overrides, cu_qp_delta depth, lists modification flags |
| 3 | slice header → `P[]` beyond §16.2 | `0x62abc88`, `0x62b2620` | SAO/deblock flags (§16.2 word 0 "[U] per-flag labels"), collocated_from_l0/ref_idx, five_minus_max_num_merge_cand, entry points |
| 4 | RPS → hardware DPB slot order (`C.u8[13c3e4+k]`, `Q.u32[0x7c]`) | `0x62b7ab0` L29130–29149 | which reference lands in `0x7100+8k`, long-term handling |
| 5 | HEVC buffer set and sizes | `0x62b87dc` (AllocatePictureMemory), `0x62b859c`, `C[13cf48]`, `C[13cff8]` | temporal-MV per picture, above-param, slice-param, GENC buffer + fragment base: none is allocated by the driver today |
| 6 | the HEVC packet: kind-4 payload (`[0x4c/4]=4`, `C[13c358]`, `C[13d0a8]`) and the per-slice chunk message | `0x62b7ab0` tail, `0x62b37d4`, `0x62b8cd8` | the DEVA firmware's kind-4 handling was never traced (`ds-vxd-deva-firmware.md` covers 0x81 only) |
| 7 | `0x70a0` `type2`, `C[13c380]` bit 18, `0x703c`/`0x71ec` policy bits | `0x62b38d0`, `0x62b8b0c` | marked [U] in §16.1 |

Estimate: items 1–4 and 7 are one RE round of the size of round 5's
`ds-vxd-cmdstream.md` §16 (parser traces with line numbers, as before);
items 5–6 add the DEVA firmware's kind-4 path (`ds-vxd-deva-firmware.md`
style). Then a driver round: an independent generator transcribed from the
dump (`tools/ds-video/cmdstream.py --gen` gains an HEVC mode), the driver's
builder, the host word-for-word compare over x265 test streams, and the
QEMU fake-firmware smoke; then device bring-up (I, P, B, tiles, WPP, 10-bit
refused). Three rounds in all, with more first-decode risk than H.264 had,
because there is no psb_video to name the fields.

## 3. Value

- **Browser: small.** Whether ALARM's Chromium 153 has its V4L2 H.265
  delegate cannot be told from the binary (the class name is absent, but
  class names need not be emitted); Linux Chromium also gates HEVC behind
  `PlatformHEVCDecoderSupport`. Web video is H.264/VP9/AV1 anyway.
- **mpv / local files: real.** iPhone and many camera recordings are HEVC
  (8-bit Main at 1080p is what the A9 decodes in iOS); without the hardware
  the iPad's two cores will not play 1080p HEVC in real time.

## 4. Already in place for when the driver exists

- FFmpeg's `hevc_v4l2request` hwaccel is built into the round-7 package
  (`scripts/vdec/ffmpeg-v4l2request/`), and stock mpv picks it up as
  `v4l2request-copy` like H.264.
- GStreamer's `v4l2slh265dec` (gst-plugins-bad) registers automatically
  once the driver exposes `V4L2_PIX_FMT_HEVC_SLICE`.
- Test streams: ALARM's x265 4.3 is in the image's package cache for a
  generator like `scripts/vdec/make-h264-streams.sh`.
