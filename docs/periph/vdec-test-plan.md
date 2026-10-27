# A9 VXD video decoder — device test plan (branch `a9/vdec`)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-27 provenance follow-up: `a9/vdec-gpl` (`a8a70e4a04`) replaces
the table's extraction input with pinned Imagination/TI GPL source, preserving
all 544 values and all ten driver objects. Refreshed rounds 5/6/7 use the GPL
header from its introduction. Host comparison passes 76/76 pictures; the
[source, object and constant audit](../release/vdec-gpl-provenance.md) records
the new offline evidence. Six QEMU boots pass; default compliance is 49/49,
and the pre-existing optional cached-mode compliance limitation is recorded
in the audit. No device test was run for this provenance change.

Round 7 comes first, then round 6 (passed on 2026-10-24,
`docs/test-logs/nextint6-2026-10-24/`) and round 5, both unchanged.

# Round 7: everyday apps (mpv, Chromium, GStreamer), display cost, HEVC

All device commands are for the **orchestrator/owner**. The implementation
agent ran none of them, touched no device and made no workspace commits;
its one kernel commit is on `a9/vdec`.

Branch `a9/vdec`, tip **`9a897a8fd2`**, one commit on round 6's
`3001513b95`: `capture_cached` (0644, **default off**: with it off the
queues are set up exactly as in round 6). Series `patches/vdec/round7/`
(21 = round 6's 20 + this one), checkpatch `--strict` clean apart from the
`Co-Authored-By:` trailer form, W=1 (LLVM, arm64) clean, `git merge-tree`
with `a9/nextint` (`7e753d3036`) clean. **T13-T17 need no new kernel**:
they run on the round-6 kernel already on the device. Only T18 needs the
new tip.

Workspace files (not committed): `scripts/vdec/ffmpeg-v4l2request/`
(PKGBUILD + 13 rebased patches), `scripts/vdec/build-ffmpeg-pkg.sh`,
`scripts/vdec/stage-r7-media.sh`, `scripts/vdec/media-bundle.sh`,
`scripts/vdec/r7-media/` (installer, device tests, mpv.conf, environment.d
file, Chromium test page), the `media` boot in `scripts/vdec/qemu-smoke.sh`
/ `qemu-init.sh`, `docs/periph/vdec-r7-display-path.md`,
`docs/periph/vdec-r7-hevc-assessment.md`. Infrastructure: the smoke kernel
is now built from a slimmed `git archive` (other architectures' and big
unused subsystems' sources left out, every Kconfig kept: 0.7 instead of
1.7 GB on the shared Docker disk; `SMOKE_JOBS` caps its `make -j`); its
cache `/src/tmp-vdec-qemu` was deleted and rebuilt once for that.

## R7.0 What was chosen, and why

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

## R7.1 Offline evidence

**The package** (`build/vdec-r7/pkgs/abi-check.txt`):
`ffmpeg-2:9.0.2-2.1-aarch64.pkg.tar.xz`, 12 MB, sha256 `cacdedc4…8f0f`,
built against the image's own library versions (every dependency checked
"same" against the image's pacman cache; tarball signature checked). Against
the stock package from that cache:

```text
libavcodec.so.63 ... libswscale.so.10: same sonames; NEEDED unchanged except libavutil +libudev.so.1;
  no exported symbol removed or added
-decoders -encoders -filters -muxers -demuxers -protocols -bsfs: identical
-hwaccels added: v4l2request
h264 decoder: Supported hardware devices: vaapi vdpau vulkan v4l2request
ABI: PASS
```

The check paid for itself on the way: without `glslang` in the build
environment FFmpeg 9 silently drops every `*_vulkan` filter, encoder and
hwaccel (the first build differed exactly there); the PKGBUILD now names it.

**Install dry run** against the image's installed-package database
(debugfs, read-only): `pacman -U --print` of the ffmpeg package together
with the 30 staged GStreamer packages (gst-plugins-base/good/bad 1.28.8-1
and their libraries, matching the image's gstreamer 1.28.8-1): 0 errors,
0 conflicts.

**QEMU** (`scripts/vdec/qemu-smoke.sh build && run`, kernel `9a897a8fd2`,
KASAN + lockdep, **PASS**, six boots, no splat): the five round-6 boots
unchanged (v4l2-compliance 49/49; fake MTX, 11 streams; GStreamer, 5
streams and the 300-frame clip), plus the new `media` boot: the fake
firmware under this ffmpeg package and Arch's stock mpv 0.41.0-6 (a
loader-isolated bundle, `scripts/vdec/media-bundle.sh`):

```text
ffmpeg hwaccels: vdpau vaapi drm opencl vulkan amf v4l2request
FFMPEG hp176: rc=0 bytes 266112 frames 7 stamped 7 completions +7      (wb176, hp176q, hp176m the same)
FFMPEG fhd1080: rc=0 bytes 18662400 frames 6 stamped 6 completions +6
FFMPEG clip720 drm_prime rc=0 completions +300
FFMPEG clip720 framemd5 rc=0 frames 300 completions +300
MPV clip720 rc=0 completions +300            (config file only: "Using hardware decoding (v4l2request-copy).")
MPV clip720 --hwdec rc=0 completions +69 Using hardware decoding (v4l2request-copy)
MPV clip720 --hwdec=no rc=0 completions +0
FFMPEG hp176 cached: ... stamped 7 completions +7; fhd1080 cached: ... stamped 6 completions +6
stats ... completions 1016 ... faults 0 ... panics 0 unknown 0 malformed 0
fake-mtx: decode slices 1016 pictures 1016 bad chunks 0
```

What that shows: Kwiboo's device probe finds the decoder through udev and
the media topology; every picture was submitted to the "hardware" and came
back through `av_hwframe_transfer_data` (the download mpv's copy mode
uses) carrying the fake firmware's `VXDFAKE!` stamp, so there was no
software fallback anywhere; FFmpeg's control conventions (it always sets
`SCALING_MATRIX_PRESENT` with the effective lists) pass the driver's checks
on the scaling-matrix streams; 1080p goes through FFmpeg's DRM descriptor
(stride 2048, chroma at 2048 × 1088); mpv picks `v4l2request-copy` from
the config file alone. `capture_cached=1` allocates and decodes the same
way; v4l2-compliance then reports 48/49, and the one failure
(`v4l2-test-buffers.cpp:843`: a coherent CREATE_BUFS on a queue holding
non-coherent buffers must fail) is the parameter's definition: it forces
non-coherent memory whatever the application asks. Off by default, with it
off 49/49. Fidelity as round 6: QEMU is cache-coherent and the fake decodes
nothing, so this is plumbing; bit-exactness and caches are T13 and T18.

> Private graphics-dependent excerpt omitted from this source snapshot.

**Kernel commit**: W=1 (LLVM, arm64) clean, checkpatch `--strict` clean but
for the trailer form, `git merge-tree` with `a9/nextint` clean.

## R7.2 Build, stage, install (host, then the iPad)

On the Mac (the package build takes 10 to 45 minutes, depending on what else the Docker VM runs; the stage below is already built in `build/vdec-r7/stage/`):

```sh
scripts/vdec/build-ffmpeg-pkg.sh   # -> build/vdec-r7/pkgs/, abi-check.txt must end "ABI: PASS"
scripts/vdec/stage-r7-media.sh     # -> build/vdec-r7/stage/ (+ gst/ from the image's mirror)
tar cf - -C build/vdec-r7 stage | scripts/omarchy/ssh.sh 'rm -rf /var/tmp/r7-media && mkdir -p /var/tmp/r7-media && tar xf - -C /var/tmp/r7-media --strip-components=1'
scripts/omarchy/ssh.sh 'ls -l /var/tmp/vdec/clip1080.h264 /var/tmp/vdec/clip1080.mp4'   # round 6 R6.3 pushes them if missing
```

On the iPad (installs the package with pacman -U after sha256 checks,
GStreamer's packages, the mpv block and the environment.d file, and puts the login
user into `video` for SSH sessions):

```sh
scripts/omarchy/ssh.sh 'sudo /var/tmp/r7-media/install-media.sh'
```

Expect `ffmpeg: ffmpeg 2:9.0.2-2.1`, `hwaccels: ... v4l2request ...`,
`gstreamer: gstreamer 1.28.8-1 gst-plugins-bad 1.28.8-1`,
`mpv: /home/linux/.config/mpv/mpv.conf: hwdec=v4l2request-copy hwdec-codecs=h264`. A new SSH login
picks up the `video` group (the tests read debugfs with sudo either way).
Then log out of the Hyprland session once (or reboot) so it gets the
environment.d file. **CMA**: Chromium allocates 17 bitstream buffers
(2 MiB each at 1080p) plus references + 2 pictures; if `CmaTotal` in the
test headers is 128 MiB, stage the next kernel with `cma=256M` (round 6
R6.3's staging line with the option added:
`TREE=nextint scripts/boot/stage-kernel.sh "nextint-$C" --cmdline "$base cma=256M"`).

The device tests are one script; every log goes to `/var/tmp/r7-logs/`
and the one-line results to `/var/tmp/r7-logs/summary.txt`.

## R7.3 T13: FFmpeg CLI (bit-exact, throughput, hardware proof)

```sh
scripts/omarchy/ssh.sh '/var/tmp/r7-media/r7-tests.sh ffmpeg'
scripts/omarchy/ssh.sh 'cat /var/tmp/r7-logs/clip1080.ffmpeg.framemd5' > build/vdec-r7/clip1080.ffmpeg.framemd5
python3 -I scripts/vdec/compare-framemd5.py build/vdec-r7/clip1080.ffmpeg.framemd5 build/vdec-r6/streams/clip1080.framemd5
```

PASS: `hwaccels:` lists `v4l2request`; `T13 framemd5: rc=0 frames 300
completions +300`; `framemd5: PASS (bit-exact)` on the Mac (the same 300
frames GStreamer matched in round 6, now through FFmpeg's hwaccel and the
dmabuf download). Throughput lines: `drm_prime` (zero copy, compare with
round 6's 52.8 fps), `copy` (what mpv gets), `sw` (two-core software
decode). `completions +300` on both hardware lines and `+0` on `sw`.

## R7.4 T14: mpv

```sh
scripts/omarchy/ssh.sh '/var/tmp/r7-media/r7-tests.sh mpv'
```

> Private graphics-dependent excerpt omitted from this source snapshot.

## R7.5 T15: GStreamer playbin system-wide

```sh
scripts/omarchy/ssh.sh '/var/tmp/r7-media/r7-tests.sh gst'
```

As the login user, no sudo, no `GST_PLUGIN_FEATURE_RANK`: `rank:
Rank primary + 1 (257)`, `T15 playbin: rc=0 completions +300 decoders:
v4l2slh264dec0`, same for `playbin3`, then gst-play on screen with its
completion count, CPU shares and `gst-play.png`. If `decoders:` shows
`avdec_h264` or nothing and the completions do not move: the user cannot
open the nodes (check `ls -l /dev/video0 /dev/media0`, `getfacl`), or the
registry was built before the install (`rm -rf ~/.cache/gstreamer-1.0`).

## R7.6 T16: Chromium

Needs the Hyprland session on the screen (the tests export
`WAYLAND_DISPLAY=wayland-1` and find Hyprland's instance).

```sh
scripts/omarchy/ssh.sh '/var/tmp/r7-media/r7-tests.sh chromium A'
```

It starts Chromium with Omarchy's flags plus the three switches (the
launcher puts `~/.config/chromium-flags.conf` before the command line, so
the test passes the whole `--enable-features=` list, Omarchy's two
features plus `AcceleratedVideoDecoder`, and it wins either way), a
throwaway profile and verbose media logging on `vxd-video.html` (the
1080p clip, muted, looping; the page writes its counters into the window
title), waits 30 s, reads the title through `hyprctl`, samples CPU and
takes `chromium-A.png`, then closes it. Results:

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

## R7.7 T17: where the CPU goes on screen

```sh
scripts/omarchy/ssh.sh '/var/tmp/r7-media/r7-tests.sh cost'
```

Four GStreamer runs of the same clip (method:
`docs/periph/vdec-r7-display-path.md` §2): A decoder only (round 6: 52.8
fps), B + `videoconvert` to BGRx on the CPU, C + CPU scaling to
1536x864, D on screen through waylandsink in the session (`rendered`,
`dropped`, Hyprland's CPU share). B ≈ A means the conversion is cheap and
the compositor is the cost; B ≪ A means the conversion itself is the first
thing to remove (the NV12 overlay plane described in the display study).

## R7.8 T18: cached CAPTURE buffers (round-7 kernel only)

Stage and boot the round-7 tip as round 6 R6.3 did (`git merge a9/vdec`
into `nextint`, `scripts/build-kernel.sh --tree nextint`, stage, warm
kexec; no DT change), then:

```sh
scripts/omarchy/ssh.sh '/var/tmp/r7-media/r7-tests.sh cached'
for c in 0 1; do scripts/omarchy/ssh.sh "cat /var/tmp/r7-logs/clip1080.cached$c.framemd5" > build/vdec-r7/clip1080.cached$c.framemd5; python3 -I scripts/vdec/compare-framemd5.py build/vdec-r7/clip1080.cached$c.framemd5 build/vdec-r6/streams/clip1080.framemd5; done
```

PASS: both `framemd5: PASS (bit-exact)` (with 1 the CPU caches are in the
loop: a stale line would show as a mismatch), and the two throughput
lines with `capture_cached=1` against 0. If 1 is bit-exact and faster,
the next round makes it the default.

## R7.9 Revert

```sh
scripts/omarchy/ssh.sh 'sudo /var/tmp/r7-media/install-media.sh --revert'
```

Reinstalls the stock `ffmpeg 2:9.0.2-2` (from `rollback/`, the image's own
package), removes the marked block from `~/.config/mpv/mpv.conf`, removes the
environment.d file. The GStreamer packages stay (stock, harmless). Nothing
here touches the kernel, the firmware or any persistent storage outside
the root image.

## R7.10 Failure ladder

| Symptom | Meaning / next step |
| --- | --- |
| `install-media.sh`: sha256 mismatch | transfer damaged: re-copy the stage |
| pacman: `ffmpeg ... conflicting dependencies` or a library missing at `ffmpeg -version` | the image moved past 9.0.2-2's libraries; bring back `pacman -Q x265 libplacebo dav1d libvpx`; revert |
| T13 `hwaccels` lacks v4l2request | the stock package is still installed (`pacman -Q ffmpeg`) |
| T13 framemd5 `completions +0` and real pictures | the hwaccel fell back to software: `ffmpeg-framemd5.log` (device probe: `Failed to ... media device`, `missing support`) and `ls -l /dev/media0 /dev/video0` |
| T13 framemd5 mismatch but round 6's GStreamer clip was exact | FFmpeg's control conventions differ (it always sets `SCALING_MATRIX_PRESENT` with flat lists): first differing frame + `ffmpeg -loglevel debug` of the first 10 frames |
| T13 `drm_prime` fps well under round 6's 52.8 | FFmpeg waits for each request before queueing the next (one picture in flight, as the driver allows); report, it is the driver's pipelining item |
| T14 `Using hardware decoding` missing | `mpv-null.log`: `Could not create device` (as T13) or `not on whitelist` (the config was not read: `~/.config/mpv/mpv.conf`, which the test reads as the login user) |
| T15 decoder `avdec_h264` or none | access or registry (R7.5) |
| any `decode job timed out`, `faults` > 0 in `mbox` | as round 6 R6.9: bring back `mbox`, `regs`, dmesg |
| iPad freezes | force reboot; the NBD root keeps the logs written so far |

## R7.11 What to bring back

1. `/var/tmp/r7-logs/` (all of it: `summary.txt`, logs, `.grep`, `.png`,
   `.cpu`, framemd5 files).
2. The `compare-framemd5.py` outputs of T13 and T18.
3. `pacman -Q ffmpeg gstreamer gst-plugins-bad mpv chromium` and
   `grep -i cma /proc/meminfo`.
4. The Chromium verdict (A row of the T16 table) and, for T14, which VO
   played with fewer drops.

## R7.12 Known limits

> Private graphics-dependent excerpt omitted from this source snapshot.

# Round 6 (done): High profile, B slices, weighted prediction, 1080p, GStreamer

All device commands are for the **orchestrator/owner**. The implementation
agent ran none of them, touched no device and made no workspace commits;
its commits are on `a9/vdec` only.

Branch `a9/vdec`, tip **`3001513b95`**, six commits on round 5's
`fe91c4ef73`:

| Commit | What |
| --- | --- |
| `1b388c0843` | slice parser keeps `pred_weight_table()` (frame-based mode has no PRED_WEIGHTS control); 128 slices per picture |
| `c15a7092d7` | submit more than 8 slices per picture: all fences armed up to 8 packets, only the last one beyond; Apple-sized command buffers (8 × 256 KiB, 32 KiB packet lists) |
| `a5a0399160` | **the recipe**: scaling lists, 8x8 transform, B slices (POC block, co-located picture, inverse L0, L1), explicit and implicit weighted prediction |
| `5821d9e880` | OUTPUT `sizeimage` ≥ one byte per pixel (2 MiB at 1080p; GStreamer leaves it to the driver) |
| `7b9c6ad981` | debugfs `mbox` copies only the printed fence fields (the KASAN+lockdep build went over the 2 KiB frame limit) |
| `3001513b95` | module parameter `scaling_order` (0 raster = the kext, default; 1 zig-zag; 2 transposed) to settle a scaling-list mismatch on the device without a rebuild |

**No DT change** (no `arch/` or binding commit since round 1), so a warm
kexec with the running DTB works as in round 5. `git merge-tree` of
`a9/nextint` (`fc52fc4e2b`, contains round 5) with this tip is clean.
Series: `patches/vdec/round6/` (20 patches = round 5's 14 + these 6),
checkpatch `--strict` clean apart from the `Co-Authored-By:` trailer form,
W=1 (LLVM, arm64) clean, the series reproduces the tree.

## R6.0 What is supported now

| Feature | Status |
| --- | --- |
| Profiles | Baseline, Main, **High** (8-bit 4:2:0, progressive) |
| Entropy | CAVLC, CABAC (CABAC bit-exact on the device since round 5) |
| Slice types | I, P, **B** (spatial and temporal direct) |
| Transform / quantisation | **8x8 transform**, **scaling matrices** (SPS and PPS, V4L2 SCALING_MATRIX control) |
| Weighted prediction | **explicit P and B** (table parsed in the kernel), **implicit B** |
| Slices per picture | **up to 128** (round 5: 8); ASO/FMO refused |
| Picture size | 16x16 up to **1920x1088** (level 4.2 in the DT); NV12 stride rounded to 256 |
| References | 16 DPB slots, short and long term, list modifications, MMCO (userspace's DPB) |
| Userspace | V4L2 stateless request API, frame based, Annex-B; **GStreamer `v4l2slh264dec`** (QEMU: plays a 300-frame b-pyramid/MMCO clip) |
| Refused per picture | interlace (field pictures, MBAFF), 4:2:2/4:4:4, > 8 bit, lossless transform bypass, FMO/ASO, SP/SI, data partitioning, scaling matrix flagged without the control |

Sizing at 1920x1088 (checked against the kext's allocations): the GART
identity-maps the low 32 bits of every DMA address over 512 page tables
(2 GiB, all of the iPad's DRAM), so it puts no limit on buffers; the
IntraBuffer (128 KiB) and StreamStateBuffer (8 KiB) are per context and
independent of the picture size, as in DecodeCreateH264VLD (`M<<16`,
`M<<12`); per CAPTURE buffer the stored-MB buffer is
`(W*H*0x80 + 0x41ff) & ~0xfff` = 1.0 MiB at 120x68 MBs; NV12 stride 2048,
chroma at 2048 × 1088 = 0x220000 (4 KiB aligned, as every MSVDX base
register needs); the register fields (`PIC0` W×H−1 in 14 bits, SLICE1 MB
x/y in 8 bits, CMDS sizes in 12 bits, the header's 16-bit strides) all
fit; command buffer: about 300 words per 1-slice B picture with weights,
1196 words for fhd1080s4's 4-slice B pictures (256 KiB per buffer).

**Not available for this decoder: mpv/ffmpeg.** Arch Linux ARM's ffmpeg
(9.0.2 today) has no V4L2 request API hwaccel (that is an out-of-tree
patch set; its V4L2 support is the stateful m2m wrappers only), so mpv's
`--hwdec` cannot use a stateless decoder. Playback goes through GStreamer
(gst-plugins-bad's v4l2codecs: `v4l2slh264dec`, version 1.28.8 in Arch
Linux ARM now).

## R6.1 The recipe additions and their evidence

All from `0x62c66b0` (the RENDEC emitter of DecodeExecuteH264VLD) and its
producers in `build/video/d5500-decomp.txt`; field layouts cross-checked
with psb_video's `msvdx_vec_h264_reg_io2.h`/`msvdx_cmds_io2.h`. The slice
record `SL` (`param_2`) is DXVA's `DXVA_Slice_H264_Long` shifted by 6
bytes (`+0x16` slice_type, `+0x19/+0x1a` num_ref_idx_l0/l1_active_minus1,
`+0x1e`/`+0x3e` RefPicList[0]/[1], `+0x5e` Weights[2][32][3][2],
`+0x361..0x363` direct/cabac_init/deblock); the picture record `PIC`
(`D+0x63e8`) is `DXVA_PicParams_H264` without StatusReportFeedbackNumber
(`+6` bit fields, `+0xc` RefFrameList, `+0x1c` CurrFieldOrderCnt,
`+0x24` FieldOrderCntList).

| Block | Words | Evidence |
| --- | --- | --- |
| `R5[e000]` scaling lists, 56 words | 8x8 Inter Y, 8x8 Intra Y, then 4x4 lists 0, 3, 4, 1, 5, 2; **raster order**; flat 16 when neither SPS nor PPS has a matrix; 8x8 flat without `transform_8x8_mode_flag` | emitter L39756-39775 [V]; list resolution `0x62be538` L33392-33534 [V]; the parser `0x629c918` (L12199-12245) stores `list[zigzag[j]]` and the kext's defaults at `0xfffffff005596170` equal Table 7-3/7-4 converted to raster, byte for byte (checked this round) [V]; hardware [U] |
| `R5[6250]` B only | cur top, cur bottom POC, then top/bottom per hardware DPB slot up to the high-water mark; 17 bits, sign in bit 16 | L39776-39797 [V]; unused slots inside the high-water mark: **0** [I] (Apple's RefFrameList is compact; psb writes 0) |
| `R5[62f4]` B only | `COL_PIC0` (bit 0 bottom field by POC distance, bit 2 MBAFF, bit 4 not-frame), co-located stored-MB VA (RefPicList1[0]'s), 8 words `LIST0_INVERSE` (first L0 position per slot), `(l1+4)>>2` words of L1 bytes | L39798-39887 [V]; psb `psb__H264_build_B_slice_chunk` identical [V] |
| `R5[633c]` | L0 bytes for P **and B** | L39888-39893 [V] |
| `R5[7100]` | reference surfaces now marked used from L0 and L1 (reset per slice) | L39894-40084, memset at L39296 [V] |
| `R5[7200]`, `R5[7300]` | 32 factor words `(Cr&0x1ff)<<18 \| (Cb&0x1ff)<<9 \| Y&0x1ff`, 32 offset words `Cr<<16 \| Cb<<8 \| Y` (8 bit); list 1 for B; for (weighted_pred && P) or (bipred_idc ≠ 0 && B) | L40086-40134 [V]; values: `0x62be288` copies the parser's table (zero for 0xff refs) [V] |
| `R5[7024]` | slice's `chroma<<4 \| luma` log2 denominators for explicit tables, `0x55` otherwise | `0x62c5fb8` L39467-39476, `0x62be348` L33194-33202 [V] |
| implicit bi-prediction | tables emitted but **zero**: the kext only refreshes them for explicit slices (stale or zero), so the core takes implicit weights from the POCs | `0x62b17xx` L25758-25763, `0x62be348` [V] for the kext; zero vs stale [I] |
| header word 1 | `MODE_CONFIG = weighted_pred \| bipred_idc<<1` at bit 17 | L40260 [V] |
| SLICE1 | P slices keep the PPS's `num_ref_idx_l1_default_active_minus1` (only I zeroes both) | `0x62c5fb8` L39477-39483, parser `0x62a0058` [V]; all test streams have 0 there |
| more than 8 slices | Apple's tag `(seq<<4) \| (packet*2 & 0xe)` repeats after 8; intermediate packets are `0x2000` (no response) | round 5 device: "1 of 4 packets answered" [V]; intermediate tags unarmed [I] |

## R6.2 Offline evidence

**Host, word for word** (`scripts/vdec/host-cmdstream.sh`): the driver's
parser + builder (from the tree) against `tools/ds-video/cmdstream.py
--gen`, which this round gained the scaling, B and weight chunks
(transcribed from the decompiled lines above, not from the C code):

```text
v4l2-h264 helper: 56 pictures, P/B0/B1 lists identical to the harness's
v4l2-h264 helper: 20 pictures, P/B0/B1 lists identical to the harness's
host harness: 76 pictures from 3001513b95
compare: 76/76 pictures identical -> PASS
```

76 pictures: the 11 embedded streams (56) plus hd720, fhd1080, fhd1080s4
(20), every I, P and B picture, 1/4/9-slice pictures, jvt and custom
scaling lists, explicit and implicit weights, 1080p (`min_bipred8` set,
stride 2048). `build/vdec-r6/host/compare.txt` has every line. The first
line pair also links the kernel's `v4l2-h264.c` into the harness and
checks its P/B0/B1 lists against the harness's own clause-8.2.4 builder.

**Independence caveat.** The generator's new chunks and the driver's were
written by the same agent from the same reading of the dump, so the
compare rules out transcription slips, not a misreading of the kext. Two
third sources: ffmpeg's `trace_headers` agrees with the driver's parsed
`pred_weight_table` on all 8 explicit-weight slices (denominators,
weights, offsets, inferred defaults), and the kernel helper agrees with
the harness's list builder (above). Whether the recipe is right only the
device can say.

**QEMU smoke** (`scripts/vdec/qemu-smoke.sh`, KASAN + lockdep +
DEBUG_ATOMIC_SLEEP, five boots, no splat):

- default: both self-tests, **v4l2-compliance 49/49, 0 warnings**, the
  no-firmware refusal as before;
- fakemtx: all 11 embedded streams decode-complete (`H264 DONE-MISMATCH
  (11 streams)`, mismatch expected: the fake writes `VXDFAKE!`, no
  pixels), `fake-mtx: decode slices 97 pictures 56 bad chunks 0`, the
  9-slice pictures complete with `1 of 9 packets answered` and the
  intermediate pushes logged `(not armed)`;
- **gst** (new): GStreamer 1.28.8 from Arch Linux ARM
  (`scripts/vdec/gst-bundle.sh`) against the fake firmware.
  `v4l2slh264dec` registers on the driver's media node; hp176 and wb176
  (B frames, weights), hp176q and hp176m (scaling matrices: GStreamer
  queues the SCALING_MATRIX control, nothing refused) and fhd1080
  (1080p, cropped to 1080) decode to files with **every frame stamped by
  the fake** (7, 7, 7, 7 and 6 frames); `clip720` (300 frames, x264
  preset medium defaults: b-pyramid with MMCO, weightp/weightb, 8x8,
  CABAC) runs through `checksumsink` (300 lines, `0:00:00.000000000
  <md5>` per frame) and `fpsdisplaysink` with **0 pictures refused, 0 bad
  chunks, 0 faults** (634 completions). The bundle also has playbin,
  qtdemux and waylandsink for T12 option B.

Fidelity: 4 KiB pages, DRAM below 4 GiB, RAM registers, a fake that
checks framing and addresses and decodes nothing. It proves the V4L2,
request, GStreamer negotiation, buffer and mailbox plumbing; it says
nothing about the core's reaction to the new register blocks.

## R6.3 Build, integrate, stage, upload (host)

As round 5 §3, with the new tip:

```sh
scripts/build-kernel.sh --run nextint 'git merge --no-edit a9/vdec && git log --oneline -1'
scripts/build-kernel.sh --tree nextint
scripts/build-kernel.sh --run nextint 'git merge-base --is-ancestor 3001513b95 HEAD && echo round6-included'
shasum -a 256 build/kernel-nextint/Image.gz build/kernel-nextint/dtbs/s8003-j71t.dtb
C=$(cut -c1-10 build/kernel-nextint/COMMIT)
base=$(scripts/omarchy/ssh.sh 'cat /proc/cmdline' | tr ' ' '\n' | grep -v '^apple_vxd\.' | tr '\n' ' ')
TREE=nextint scripts/boot/stage-kernel.sh "nextint-$C" --cmdline "$base"
```

Test program, streams and clips (on the Mac; the test binary is built by
the smoke's build step, the streams by the generator):

```sh
scripts/vdec/qemu-smoke.sh build          # -> build/vdec-qemu/vdec-h264-test-static
scripts/vdec/make-h264-streams.sh --clips # -> build/vdec-r6/streams/ (idempotent)
shasum -a 256 build/vdec-qemu/vdec-h264-test-static
scripts/omarchy/ssh.sh 'cat > /tmp/vdec-h264-test && chmod +x /tmp/vdec-h264-test' < build/vdec-qemu/vdec-h264-test-static
S=build/vdec-r6/streams
tar cf - -C $S hd720.h264 hd720.crc fhd1080.h264 fhd1080.crc fhd1080s4.h264 fhd1080s4.crc \
	clip1080.h264 clip1080.mp4 clip720.h264 | scripts/omarchy/ssh.sh 'mkdir -p /var/tmp/vdec && tar xf - -C /var/tmp/vdec'
```

(`/var/tmp` is on the NBD root, not RAM: the 1080p clip is 8 MB.) Boot it
with `scripts/omarchy/ssh.sh "sudo linux-a9-kexec reboot nextint-$C" ||
true`, netconsole attached, as round 5.

Order: **T1/T3 → T9 (regression) → T10 (new syntax, embedded) → T11
(720p/1080p files) → T12 (GStreamer) → T7.** Stop at the first failure
of a step, bring back §R6.9, and continue with the next step only where
the ladder says so (T10's streams are independent of each other).

## R6.4 T1, T3, T9: boot and the round-5 streams (regression)

```sh
scripts/omarchy/ssh.sh 'uname -a; echo 1 | sudo tee /sys/kernel/debug/apple-vxd/boot >/dev/null; echo boot rc=$?; sudo dmesg | grep -E "apple-vxd.*(command buffers|DEVA firmware ready|self-test)"; for s in grey64 ts176 ts176s4 ts176cb; do sudo /tmp/vdec-h264-test -s $s -t 3000 | grep -E "^(STREAM|FAIL)"; done'
```

Expect `command buffers: 8 x 256 KiB …`, `DEVA firmware ready`, and four
`STREAM … -> PASS` lines (CRCs as round 5: grey64 `c45562bc`; ts176
`6c1fb604 550cbab8 01220617`; ts176s4 `53953c0e 8763dd29 1b432e44`;
ts176cb `127127a7 56978d6c 73f851e0`). The FRAME lines gained the display
index: `FRAME grey64 0 I (display 0): DONE, crc c45562bc, ffmpeg c45562bc
MATCH, …`. Any regression here: stop, it is shared code (§R6.9 row 1).

## R6.5 T10: the new syntax, one stream at a time

Smallest feature first; each stream is 176x144, 7 pictures (sl176: 4),
decode order on the left, ffmpeg CRC of the same display position on the
right of each FRAME line:

```sh
scripts/omarchy/ssh.sh 'echo 1 | sudo tee /sys/module/apple_vxd/parameters/step_ioctls >/dev/null; for s in hp176 hp176t hp176q hp176m wp176 wb176 sl176; do sudo /tmp/vdec-h264-test -s $s -t 3000; echo "$s rc=$?"; done; sudo dmesg | grep -E "decode: (completion|TIMEOUT|picture refused)|timed out|PANIC" | tail -12; sudo cat /sys/kernel/debug/apple-vxd/mbox | grep stats'
```

| Stream | Exercises | Decode order | ffmpeg CRCs, display order |
| --- | --- | --- | --- |
| hp176 | High, CABAC, 8x8 transform, B (spatial direct), 2 refs | I P B B P B B | `e6eee1f1 114a1fa1 bece6560 713b9cad 80729469 13419606 400b2542` |
| hp176t | High, **CAVLC**, 8x8, B **temporal direct** (co-located motion) | I P B B P B B | `fbe20cc9 c434ff8c ba9b2577 c6d87205 6d978902 71d6b6e8 04d6b68b` |
| hp176q | `cqm=jvt`: PPS scaling lists "use default" | I P B B P B B | `c438c741 5f6453af e0c979f8 af60a77a 4d66fe0b cbc1d85d 2bab4b85` |
| hp176m | asymmetric custom matrix (cqmfile): order/transposition | I P B B P B B | `8031f265 c6fcd72b 5a19d01d ed28c157 ac87c53d 670d518a 87bf5b22` |
| wp176 | Main, fade, **explicit weighted P** (large weights such as 10/1 and 63/32, duplicate refs via list modification), 3 refs | I P P P P P P | `c4519db4 99e31941 72321fe3 5a2e0b42 209b11ee 4f22c92e 505ca5a2` |
| wb176 | High, fade, explicit P + **implicit B** (x264's default combination) | I P B B P B B | `c4519db4 3f52032c 3e7bd104 e276cb38 fb49c1bb 8c3574d6 103acbd6` |
| sl176 | Main, CAVLC, **9 slices per picture** (9 0x81 messages), I P B P | I P B P | `f039eb4b 878f24f2 26a1488f 3bd2c283` |

Pass per stream: `STREAM <s>: N/N DONE, N/N bit-exact -> PASS`, `rc=0`.
dmesg: B pictures show `first B NAL` in the `decode: src …` STEP line;
sl176's completions say `1 of 9 packets answered` and its pushes
`(not armed)` for packets 0-7. `mbox` stats: `faults 0`, `panics 0`.

If hp176q or hp176m mismatch (and hp176 matches), run the scaling-order
check before anything else and report which value is bit-exact:

```sh
scripts/omarchy/ssh.sh 'for o in 1 2 0; do echo $o | sudo tee /sys/module/apple_vxd/parameters/scaling_order >/dev/null; sudo /tmp/vdec-h264-test -s hp176m -t 3000 | grep STREAM; done'
```

(0 = raster, what the kext uploads; 1 = zig-zag, psb_video's VA-API
input as ffmpeg fills it; 2 = transposed. hp176q alone cannot tell
raster from transposed: the default lists are symmetric.)

Frames to the Mac, as round 5 (now 7 per stream, decode order in the
file name; `-video_size 176x144`).

## R6.6 T11: 720p and 1080p, and the decode time

```sh
scripts/omarchy/ssh.sh 'cd /var/tmp/vdec && for f in hd720 fhd1080 fhd1080s4; do sudo /tmp/vdec-h264-test -t 3000 -o /var/tmp/vdec -f $f.h264; echo "$f rc=$?"; done; sudo /tmp/vdec-h264-test -n -t 3000 -f fhd1080.h264 | grep TIMING; grep -i cma /proc/meminfo'
```

Expected (`stream fhd1080: 1920x1080 (coded 1920x1088), 6 pictures, …`,
`capture: 1920x1088 NV12, bytesperline 2048, sizeimage 3342336; output
sizeimage 2097152`, i.e. 1920 × 1088 rounded up to the device's 16 KiB
pages):

| File | Decode order | ffmpeg CRCs, display order (`.crc` next to it) |
| --- | --- | --- |
| hd720 | I P B B P B B P | `13c22127 a44d9fbe 7224ac9c ac0ad3fd 93d84015 d0937311 25aa15ea 7cd4ff61` |
| fhd1080 | I P B B P B | `b559fd41 26b43f57 5dcac51d 3a24e627 4618a980 9fdd7b71` |
| fhd1080s4 | 4 slices each: I P B B P B | `4c03f862 b265815f 5d71e76d 7325234e 6d6e0a4a 6dee0fe9` |

`TIMING fhd1080: 6 pictures, … ms QBUF->DQBUF in total, … ms each, …
pictures/s with one picture in flight` is the number to bring back (the
test waits for each picture, so it is latency, not throughput). The CRCs
cover the cropped 1920x1080; the 8 bottom coded rows are not compared.

## R6.7 T12: GStreamer (the playback path)

**Install**, option A (the image's own packages; the iPad has no
internet, so on the Mac):

```sh
scripts/omarchy/fetch-live-pkgs.sh --image build/omarchy/rootfs.img --out build/vdec-r6/live-pkgs gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad
scp -r build/vdec-r6/live-pkgs linux@172.16.42.1:/var/tmp/
scripts/omarchy/ssh.sh 'cd /var/tmp/live-pkgs && sha256sum -c --quiet SHA256SUMS && sudo pacman -U --needed --noconfirm $(cat list.txt)'
```

The script reads the image's pacman database read-only (debugfs) and
stops on a partial upgrade: if the image already has an older gstreamer
(gst-plugin-pipewire pulls it in), it will say so; then either upgrade
the gst family together (add `gst-plugin-pipewire` and
`gst-plugins-base-libs` to the list and accept the upgrade, your call) or
use option B.

Option B, no pacman: the QEMU-tested bundle (GStreamer 1.28.8, its own
glibc 2.43 loader, 26 MB):

```sh
scripts/vdec/gst-bundle.sh      # only if build/vdec-r6/gst is missing
tar cf - -C build/vdec-r6 gst | scripts/omarchy/ssh.sh 'tar xf - -C /var/tmp'
G='env GST_PLUGIN_SYSTEM_PATH_1_0=/var/tmp/gst/plugins GST_REGISTRY_1_0=/var/tmp/gst/registry.bin GST_REGISTRY_FORK=no /var/tmp/gst/lib/ld-linux-aarch64.so.1 --library-path /var/tmp/gst/lib /var/tmp/gst/bin/'
```

and prefix every `gst-*-1.0` below with `$G` (e.g. `${G}gst-launch-1.0`;
define `G` inside the ssh command).

**Element** (as root; for the user's session add the user to `video` or
`sudo setfacl -m u:linux:rw /dev/video0 /dev/media0`):

```sh
scripts/omarchy/ssh.sh 'sudo gst-inspect-1.0 v4l2codecs | grep -E "v4l2sl|Version"; sudo gst-inspect-1.0 v4l2slh264dec | grep -E "Long-name|alignment|stream-format"'
```

Expect `v4l2slh264dec: V4L2 Stateless H.264 Video Decoder`, `alignment: au`.

**Bit-exact, every frame** (300 frames of x264 preset-medium 1080p:
b-pyramid with MMCO, weightp + weightb, 8x8, CABAC):

```sh
scripts/omarchy/ssh.sh 'cd /var/tmp/vdec && sudo GST_DEBUG=v4l2codecs*:3 gst-launch-1.0 -q filesrc location=clip1080.h264 ! h264parse ! v4l2slh264dec ! checksumsink hash=md5 > clip1080.gst.sum; echo rc=$?; wc -l clip1080.gst.sum; sudo dmesg | grep -cE "picture refused|timed out"' 
scripts/omarchy/ssh.sh 'cat /var/tmp/vdec/clip1080.gst.sum' > build/vdec-r6/clip1080.gst.sum
python3 -I scripts/vdec/compare-framemd5.py build/vdec-r6/clip1080.gst.sum build/vdec-r6/streams/clip1080.framemd5
```

PASS = `rc=0`, 300 lines, 0 refused/timed out, and `framemd5: PASS
(bit-exact)`. (GStreamer copies each frame into tight NV12 for a sink
without video meta, which is what ffmpeg's framemd5 hashes; the
`GstVideoMeta support required, copying frames` warning is expected.)

**Throughput** (no copy: `fakevideosink` takes video meta):

```sh
scripts/omarchy/ssh.sh 'cd /var/tmp/vdec && sudo gst-launch-1.0 -v filesrc location=clip1080.h264 ! h264parse ! v4l2slh264dec ! fpsdisplaysink video-sink=fakevideosink text-overlay=false sync=false 2>&1 | grep -o "rendered: [0-9]*, dropped: [0-9]*, current: [0-9.]*, average: [0-9.]*" | tail -1; grep -E "apple-vxd|vxd" /proc/interrupts; sudo cat /sys/kernel/debug/apple-vxd/mbox | grep stats'
```

Bring back the `average:` fps (the last line is printed before EOS, so
`rendered` stops short of 300; that is fine). The vxd IRQ count and
`completions` must have grown by about 300 since the previous read: that
is the proof the hardware path decoded, not a software fallback (there is
none in this pipeline: no avdec/ffmpeg element is in it).

**On the screen** (Hyprland session; MP4 through playbin):

```sh
scripts/omarchy/ssh.sh 'export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1; cd /var/tmp/vdec; GST_PLUGIN_FEATURE_RANK=v4l2slh264dec:MAX timeout 15 gst-play-1.0 --videosink=waylandsink clip1080.mp4 & sleep 6; grim /tmp/vdec-play.png; wait'
scripts/omarchy/ssh.sh 'cat /tmp/vdec-play.png' > build/vdec-r6/vdec-play.png
```

> Private graphics-dependent excerpt omitted from this source snapshot.

## R6.8 T7: unbind/bind, after everything

Unchanged from round 5 (close all fds first). Expect the clean ladder,
`command buffers: 8 x 256 KiB` again after the rebind and boot.

## R6.9 Failure ladder (round 6)

| Symptom | Meaning / next step |
| --- | --- |
| A round-5 stream regresses (T9) | Shared path changed (SLICE1 now carries the PPS l1 default for P: 0 in these streams; command buffers 256 KiB; fence arming). Bring back the `decode:` STEP block of that stream (`step_ioctls=1`) and stop |
| `decode: picture refused (-22): …` on a test stream | Host-side refusal, nothing sent: the reason is in the line; a refusal on these streams is a driver bug, report it |
| hp176 I bit-exact, its P pictures differ | 8x8 transform (PPS0 bit 0) with CABAC: compare hp176t (CAVLC + 8x8): P exact there → a CABAC 8x8 context issue; P wrong in both → the 8x8 transform path itself |
| P exact, B pictures differ | the B chunk. hp176 (spatial) vs hp176t (temporal): only temporal wrong → co-located buffer/POC distances (`R5[6250]`, `COL_PIC0` bottom bit); both wrong → L1 bytes, inverse L0, or the 0x62f4 block layout. Bring back the `.nv12` of the first wrong B picture and its STEP lines |
| B picture times out (`decode job timed out`), P fine | the core rejected the B program: bring back `mbox`, `regs`, the STEP block, then unbind/bind before more B tests (each timeout quarantines a buffer) |
| hp176q/hp176m differ, hp176 exact | scaling-list upload: run the `scaling_order` loop of §R6.5 and report which value is exact |
| wp176 P pictures differ (I exact) | explicit weights: table layout (Y/Cb/Cr, 9-bit weights, 8-bit offsets) or the 0x7024 denominators; note which P pictures differ first |
| wb176 P exact, B differ | implicit bi-prediction: zero tables + `MODE_CONFIG` 5 (weighted_pred 1, bipred_idc 2). Report; the kext's stale-table behaviour (the last explicit P slice's L0 table) is the next thing to emulate |
| sl176 times out / only some slices decoded | more than 8 packets: check `1 of 9 packets answered` and `TO_MTX rd == wr`; bring back the push lines |
| hd720 exact, fhd1080 differs | width > 80 MBs: SPS0 `min_bipred8` (bit 11) or the 2048 stride; bottom 8 rows are cropped and not compared |
| `vb2 … GART map … failed` / `cma_alloc failed` in dmesg | CMA (128 MiB) exhausted: add `cma=256M` to the staged cmdline. Budget at 1080p: CAPTURE 3.3 MiB + stored-MB 1.0 MiB each, OUTPUT 2 MiB each, command buffers 2 MiB: about 55 MiB for 10 CAPTURE + 4 OUTPUT buffers |
| `gst-inspect-1.0 v4l2codecs` lists no element | the plugin could not open `/dev/media0` (permissions: run with sudo) or found no decoder entity: `media-ctl -p -d /dev/media0` |
| gst `not-negotiated` | bring back `GST_DEBUG=v4l2codecs*:6,GST_CAPS:4` output (first 200 lines) |
| gst checksums differ but vdec-h264-test is bit-exact on the same content | GStreamer's NV12 plane offsets (it computes chroma at bytesperline × 1088): compare the first wrong frame on the Mac (`ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080`) |
| iPad freezes | force reboot; report the last STEP line (camera); nothing persistent is touched |

## R6.10 What to bring back

1. `docs/periph/logs/vdec-round6-dmesg.txt`: full `dmesg` after T11.
2. Every `STREAM`/`FRAME`/`TIMING` line and rc of T9-T11.
3. On any mismatch: the `.nv12` files of that stream and the STEP lines
   of the first wrong picture; the `scaling_order` result if it applies.
4. T12: the `compare-framemd5.py` output, the fps line, `/proc/interrupts`
   (vxd) and `mbox` stats before/after, the screenshot.
5. Option A or B for GStreamer, and the installed `pacman -Q gstreamer
   gst-plugins-bad` versions if A.

## R6.11 Known limits

- Interlace (PAFF/MBAFF), 4:2:2/4:4:4, high bit depth: refused.
- Implicit bi-prediction sends zero tables (the kext sends whatever its
  arrays last held); unused DPB slots get POC 0 in the B block. Both
  [I], settled by wb176 and hp176.
- One picture in flight: the next request waits for the previous 0xc0;
  the decoder's own pipelining (several pictures queued) is round 7.
- No second deblock pass / alternate output (§16.9 item 3).
- `scaling_order` is a debug parameter; drop it once hp176m is bit-exact.
- MMCO, POC types 0/1/2, gaps: handled by userspace's DPB (GStreamer);
  `vdec-h264-test`'s own parser refuses MMCO (its streams have none).

# Round 5 (done)

All device commands are for the **orchestrator/owner**. The implementation
agent ran none of them, made no workspace commits and touched no device.
Studies: `docs/periph/ds-video-decoder.md` (block, boot ladder),
`docs/periph/ds-vxd-decode-protocol.md` (mailbox), `docs/periph/ds-vxd-cmdstream.md`
(command stream; **§16 is the H.264 one, read §0 below**),
`docs/periph/ds-vxd-deva-firmware.md` (what the DEVA firmware does with a
0x81). Device logs of round 4: `docs/test-logs/nextint3-2026-10-24/`.

Branch: `a9/vdec` on `hoolock` (6831bc701a), MAINTAINERS commit still
dropped. Tip: **`fe91c4ef73`**, five commits on round 4's `49734c4db2`:

| Commit | What |
| --- | --- |
| `ae89f28f11` | encoders split out of the pool (`apple-vxd-cmd.c`), host-buildable |
| `73cb18537d` | **debugfs `consume_check`**: is TO_MTX_RD really firmware-driven (§4) |
| `ed6bfe99cf` | in-driver H.264 slice finder / slice-header parser (frame-based mode) |
| `a30d47ea76` | **the AVC command-stream builder (R3c)** |
| `fe91c4ef73` | **device_run() decodes**: context buffers, DPB → device VAs, submit, fences, completion, watchdog |

Series: `patches/vdec/round5/` (14 patches = rounds 1–4 + these five).
checkpatch `--strict` clean apart from the `Co-Authored-By:` trailer form;
the series reproduces the tree exactly; `W=1` (LLVM, arm64) clean.
**The DT is unchanged** (no binding or dts commit since round 1), so a
warm kexec into this kernel with the running nextint DTB works.

Local tree build (`scripts/build-kernel.sh --tree vdec`, DIRTY empty):

```text
e54194aa4d6be006e7f76556855f96c2b651623a5aecf454be0fe4fda876390e  build/kernel-vdec/Image.gz
f0a1c0312f1b2d05c61a825f9d4bd121fb3b9e8aee92e88bc4d60e7f19d800f0  build/kernel-vdec/dtbs/s8003-j71t.dtb   (unchanged since round 4)
445132b857044f9be446be2e72fb5f9de8cce24d07ef3d77bf8098ebe0330b66  build/vdec-qemu/vdec-h264-test-static
```

Export command (the script is not executable, and its default checkpatch
ignore list stops at the first `Co-Authored-By:` trailer):
`CHECKPATCH_IGNORE=FILE_PATH_CHANGES,BAD_SIGN_OFF TREE=vdec ROUND=round5 bash scripts/vdec/export-round1.sh`.

## 0. Read first

### 0.1 The study's H.264 recipe was the HEVC program

`ds-vxd-cmdstream.md` §1–§15 decode `0xfffffff0062960a0` and the
`ctx+0x13cxxx` generators. That function is reached from the NAL
dispatcher for types **0x20/0x21/0x22** (HEVC VPS/SPS/PPS), and its
`parseSPS` checks `sps_max_sub_layers_minus1`: it is the **HEVC** picture
path (§16.0, found by the Codex RE agent this round, confirmed here). A
driver built from §1–§15 would have sent HEVC register programs for H.264
streams. The real AVC path is

```
0x62a15f0 HOST_H264DecodeFrame -> 0x62a10ac DecodePicture
  -> 0x62a0c00 DecodePictureInternal -> 0x62c4c98 DecodeExecuteH264VLD
     (0x62c5fb8 slice values, 0x62c6564 front end, 0x62c66b0 rendec blocks)
  -> 0x62beb30 vaEndFrame -> 0x62c4a00 -> shared submit 0x628a8d4
```

and is decoded in `ds-vxd-cmdstream.md` **§16.3–§16.9**. It is
Imagination's DXVA control-allocation format, which is **open source** in
Intel's psb_video (libva driver for the same MSVDX family; cloned to
`third_party/psb_video`, MIT): `0x9` CTRL_ALLOC_HEADER, `0xe` DMA (the CAVLC
table), `0x1` REGVALPAIR, `0xb` SR_SETUP, `0xa` BITSTREAM_DMA, `0x5`
RENDEC_BLOCK, `0x6` COMPLETION. Apple's `0x6000`/`0x7000` register banks
line up with psb's MSVDX VEC/CMDS offsets field by field (FE_SPS0 `0x200`,
BE_SPS0 `0x2d8`, BE_BASE_ADDR_CUR_PIC `0x2ec`, VLC_TABLE_ADDR0 `0xec`, luma/
chroma recon base `0xc`/`0x10`, intra buffer `0x18`, …). Apple's 544-entry
CAVLC blob is psb's 520-entry `ui16H264VLCTableData` plus 24 4:2:2 entries.

**For the orchestrator:** §1–§15 of `ds-vxd-cmdstream.md` should be
re-titled "HEVC" (the implementation agent did not edit that file outside
§16, which the RE agent owns). `vdec-test-plan.md` §9 of round 4 and the
protocol doc's address index name the HEVC functions as H.264.

### 0.2 What the AVC submission is

One command chunk and one kind-1 packet (**mailbox 0x81**) **per slice**, the
chunks back to back with no pad, and **no 0x88 packet**. Per chunk (§16.4):

```
CTRL_ALLOC_HEADER (10 words: slice params, strides, first/last MB, chroma)
{0xe0000440, CAVLC table VA}                         CMD_DMA, type VLC
REGVALPAIR 0x60ec x23, 0x61c0 x3                     constant VLC registers
{0xb0000001, B & 31, NAL bytes - 4*(B/32)}           SR_SETUP, RBDU on
{0xa0000000 | len, OUTPUT VA + NAL offset + 4*(B/32)}  bitstream DMA
REGVALPAIR 0x600c, 0x6200..0x6210, 0x6218           FE control/SPS0/PPS0/PIC0/SLICE0/1/2
RENDEC 0x6010; 0x62d8 x7 (BE copy + stored-MB VA + long-term mask)
RENDEC 0xe000 x56 (flat scaling lists, always)
RENDEC 0x633c (P: RefPicList0 as DPB slots)
RENDEC 0x7100 flag 0x08000000 (reference Y/UV VAs per DPB slot)
RENDEC 0x7000 x5 (sizes, operating mode 0x10211000, output Y, UV),
       0x7018 intra, 0x7024 0x55, 0x701c 0x013c000c, 0x7040 stride, 0x71b4 1
0x60000000
```

B = EBSP bits from the NAL header byte to slice_data (`8 + header bits`,
emulation-prevention bytes counted; the core strips them itself).
Message: `w1 = (chunk bytes / 4) << 16 | flags | 0x4004` (last) or
`0x2000` (others), `w2` = chunk device VA, `w3` = MMU context word,
`w4 = 0x10211000`, `w5` = StreamStateBuffer VA.

From the firmware study (DeepSeek, `ds-vxd-deva-firmware.md`):
**`0x2000` = NO_RESPONSE**: only the `0x4004` packet gets a 0xc0, and only it
raises the interrupt; `0x80` alone is enough before the first `0x81`; the
firmware hands the chunk to the silicon and never parses it (so the chunk
must end in `0x60000000`, which it does); **the reader polls `0x2030`, the
doorbell is only a wake-up**; `0x200c` is the firmware PC and `0x0` a
reader scratch word, so the live `0x0 = 0x01029821` means nothing either
way.

## 1. What round 5 adds

**`consume_check`** (debugfs, §4): one benign `0x83` in three stages — words
written but the write index not published (rd must not move, 20 ms; the
ring window must read back), write index published without the doorbell
(does rd move?), doorbell (rd must reach wr, 100 ms). Logs `0x0`,
`0x200c`, `0x2010` before/after. The dumps now print `0x0`/`0x200c` by
offset, not as "consumed word"/"fw pc".

**Decode path** (`apple-vxd-h264.c`, `-h264-parse.c`, `-h264-cmd.c`):

- `V4L2_PIX_FMT_H264_SLICE`, **frame-based, Annex-B**: the OUTPUT buffer is
  one access unit; the driver finds and parses the slices itself
  (SLICE_PARAMS is unused in frame-based mode). SPS/PPS/DECODE_PARAMS
  come from the request.
- Refused per picture, with a `decode: picture refused (-22): <why>` line:
  interlace, 4:2:2/4:4:4, >8 bit, FMO, scaling matrices, 8x8 transform,
  weighted prediction, B/SP/SI slices, data partitioning, ASO, more than 8
  slices. **CABAC** is built from the same recipe (PPS0 bit 2,
  cabac_init_idc, byte-aligned header) but is **unvalidated**.
- Context buffers at STREAMON: packed CAVLC table (4 KiB, CRC-checked),
  IntraBuffer 128 KiB, StreamStateBuffer 8 KiB; per CAPTURE buffer a
  stored-macroblock buffer `(W*H*0x80 + 0x41ff) & ~0xfff` (64x64: 16 KiB,
  176x144: 28 KiB, 1080p: ~1 MiB). All zeroed, 4 KiB aligned, GART-mapped.
- **NV12 bytesperline = width rounded up to 256** (176 → 256): every MSVDX
  base register drops the low 12 bits, and this keeps the chroma plane of a
  single-plane NV12 buffer 4 KiB aligned.
- DPB: hardware slot = V4L2 `dpb[]` index; each valid entry resolved to its
  CAPTURE buffer by `reference_ts`. RefPicList0 = v4l2-h264 P list +
  the slice's modifications.
- Submit: one fence per packet (Apple's `(seq << 4) | packet*2`), buffer
  bound to the last. The firmware answers only the `0x4004` packet
  (`0x2000` = no response); arming every packet just tolerates either
  policy, and the job completes on the last packet's fence; **0x1008 = 0xf00 before a submit after any GART
  change** (Apple's rule; MMU_CONTROL1 directory-cache invalidate in
  Imagination's layout); messages pushed in order.
- Completion: the `0x4004` packet's 0xc0 finishes the job in a work item
  (DONE, or ERROR if the 0xc0 has fault bits `0x18000000`). Watchdog
  **1 s**: ERROR + `decode job timed out: no completion for fence …` with
  the ring indices, `0x200c` and the IRQ status; that command buffer stays
  quarantined until the firmware is stopped (8 buffers → after 8 timeouts
  every job fails with "no free command buffer").

**Test assets** (workspace, uncommitted, for the orchestrator):

| File | What |
| --- | --- |
| `scripts/vdec/make-h264-streams.sh` | ffmpeg/libx264 → `build/vdec-r5/streams/*.h264` + ffmpeg NV12 decodes, and `scripts/vdec/h264-streams.h` |
| `scripts/vdec/h264-streams.h` | generated: `grey64` (64x64 grey IDR, 60 bytes), `ts176` (176x144 testsrc2, IDR + 2 P, 4.7 KB), `ts176s4` (same, 4 slices/picture), `ts176cb` (same, Main/CABAC, 1 slice), CRC-32 of every ffmpeg frame |
| `scripts/vdec/vdec-h264-parse.h` | independent userspace SPS/PPS/slice-header parser, POC 0/2, sliding-window DPB → DECODE_PARAMS |
| `scripts/vdec/vdec-h264-test.c` | the request-API decoder test; writes `/tmp/vdec-<stream>-<n>.nv12`, compares CRCs; exit 0 bit-exact / 2 DONE but different / 1 error |
| `scripts/vdec/host-cmdstream.sh`, `host/` | host build of the driver's builder + word diff against `tools/ds-video/cmdstream.py --gen` |
| `scripts/vdec/vxd-fake-mtx.c` | QEMU fake firmware now walks 0x81 chunks (framing, CAVLC table, bitstream address, output VA) |

## 2. Offline evidence

### 2.1 Host: builder vs. the independent generator

`scripts/vdec/host-cmdstream.sh` compiles `apple-vxd-cmd.c`,
`apple-vxd-h264-parse.c` and `apple-vxd-h264-cmd.c` from the tree against
shims, runs every picture of the three streams through the same path as
device_run() (DECODE_PARAMS from the independent userspace parser), and
diffs each against `cmdstream.py --gen` (written by the RE agent from the
dump, without seeing the driver):

```text
host harness: 10 pictures from fe91c4ef73
grey64 pic 0 (I, 1 slice(s)): 139 words, 1 packets: cmd MATCH, packets MATCH, generator complete
ts176 pic 0 (I, 1 slice(s)): 139 words, 1 packets: cmd MATCH, packets MATCH, generator complete
ts176 pic 1 (P, 1 slice(s)): 144 words, 1 packets: cmd MATCH, packets MATCH, generator complete
ts176 pic 2 (P, 1 slice(s)): 144 words, 1 packets: cmd MATCH, packets MATCH, generator complete
ts176s4 pic 0 (I, 4 slice(s)): 556 words, 4 packets: cmd MATCH, packets MATCH, generator complete
ts176s4 pic 1 (P, 4 slice(s)): 576 words, 4 packets: cmd MATCH, packets MATCH, generator complete
ts176s4 pic 2 (P, 4 slice(s)): 576 words, 4 packets: cmd MATCH, packets MATCH, generator complete
ts176cb pic 0 (I, 1 slice(s)): 139 words, 1 packets: cmd MATCH, packets MATCH, unresolved: pps.flags.ENTROPY_CODING_MODE
ts176cb pic 1 (P, 1 slice(s)): 144 words, 1 packets: cmd MATCH, packets MATCH, unresolved: pps.flags.ENTROPY_CODING_MODE
ts176cb pic 2 (P, 1 slice(s)): 144 words, 1 packets: cmd MATCH, packets MATCH, unresolved: pps.flags.ENTROPY_CODING_MODE
compare: 10/10 pictures identical -> PASS
```

(`unresolved: …ENTROPY_CODING_MODE` is the generator flagging CABAC as
unvalidated, not a missing value.) Two more independent checks:

- The slice-header lengths the driver's parser reports (the SR_SETUP bit
  offset) equal ffmpeg's `trace_headers` positions for every slice of
  ts176/ts176s4/ts176cb (IDR 32, P 26; the four slices of ts176s4 32/40/42/44
  and 26/34/36/38; CABAC P 27 + 5 alignment bits = 32).
- A perturbed input (header bits + 1) makes the compare report the exact
  word that differs (SR_SETUP word 1), so the comparison is not vacuous.

This shows the driver agrees word for word with a second implementation
for I, P, 4-slice and CABAC pictures. **It is not two independent
readings of the dump**: both implement the RE agent's §16.4, so the
compare rules out transcription errors in the driver, not a wrong reading
of the kext. The only third source is ffmpeg (below), and it covers only
the slice-header lengths. Whether the recipe is right only the device can
say.

"Diff against cmdstream.py" in the round brief: the old `cmdstream.py`
was a decompilation reader, not a generator; `--gen` was added this round
(by the RE agent) and is what the diff uses.

### 2.2 QEMU smoke

`scripts/vdec/qemu-smoke.sh` (KASAN + lockdep + DEBUG_ATOMIC_SLEEP, QEMU
virt, RAM-backed stand-in node), on the exported tip:
**`qemu-smoke: PASS (fe91c4ef73d6a977a1cd5550ef178078c5f5ab90)`**, no splat
in any of the four boots.

- **default** (no firmware): both self-tests at probe and rebind,
  `SMOKE PASS` twice, **v4l2-compliance 49/49, 0 warnings**, and the real
  stream refused cleanly: `FRAME grey64 0 I: ERROR … Y[0..7] 5a 5a …`,
  `vdec-h264-test (no firmware) rc=1`.
- **nov4l2**, **fwboot**: unchanged from round 4.
- **fakemtx** (2 vCPUs, `scripts/vdec/vxd-fake-mtx.c` playing the
  firmware):
  - `consume_check: PASS` (stage 2 "not taken": the fake is
    doorbell-driven, the real reader polls 0x2030);
  - all four streams decode-complete: 10 pictures, 19 slice packets,
    `fake-mtx: decode slices 19 pictures 10 bad chunks 0` (the fake walked
    every chunk: framing, `0x90000000 … 0x60000000`, the CAVLC table head
    at the 0xe DMA address, a readable bitstream address, an output
    address in CMDS 0x700c);
  - every `FRAME … DONE` with `Y[0..7] 56 58 44 46 41 4b 45 21`: the fake
    wrote `VXDFAKE!` at the luma VA it found in the stream, and the test
    read it back from the right CAPTURE buffer (CRC "MISMATCH" is
    expected: nothing is decoded);
  - multi-slice: `decode: completion: fence 0x216 (seq 0x21), 1 of 4
    packets answered, status 0x0 … -> DONE` (the fake answers only the
    0x4004 packet, like the real firmware);
  - `H264 DONE-MISMATCH (4 streams)`, `vdec-h264-test rc=2`; afterwards all
    eight command buffers `free`; then the panic test and unbind/bind with
    the IRQ armed as in round 4; fake summary `… completions 16 panics 1
    errors 0`.

**Fidelity:** 4 KiB pages, DRAM below 4 GiB, RAM registers, a
doorbell-driven fake that checks framing and addresses but decodes
nothing. A PASS shows the V4L2 → parse → build → submit → IRQ → work →
DONE path runs with no memory, lock or sleep-context error and moves the
right buffers; it says nothing about the core's reaction to the stream.

## 3. Build, integrate, stage (host)

Merge the tip into the nextint stack (a read-only `git merge-tree` of
`a9/nextint` 92b11076f3 + `a9/vdec` fe91c4ef73 is clean; nextint
already contains round 4), build, and stage a warm-kexec entry:

```sh
scripts/build-kernel.sh --run nextint 'git merge --no-edit a9/vdec && git log --oneline -1'
scripts/build-kernel.sh --tree nextint
scripts/build-kernel.sh --run nextint 'git merge-base --is-ancestor fe91c4ef73 HEAD && echo round5-included'
grep -E '^CONFIG_(VIDEO_APPLE_VXD|DEBUG_FS|CRC32)=y' build/kernel-nextint/config
shasum -a 256 build/kernel-nextint/Image.gz build/kernel-nextint/dtbs/s8003-j71t.dtb
C=$(cut -c1-10 build/kernel-nextint/COMMIT)
base=$(scripts/omarchy/ssh.sh 'cat /proc/cmdline' | tr ' ' '\n' | grep -v '^apple_vxd\.' | tr '\n' ' ')
TREE=nextint scripts/boot/stage-kernel.sh "nextint-$C" --cmdline "$base"
```

The DTB hash must equal the running one (no DT change): if it differs,
something else in nextint changed it, and the warm kexec hangs today — cold
boot that payload instead.

Upload the test program (static aarch64, built by `scripts/vdec/qemu-smoke.sh
build`) and the ffmpeg references (for looking at frames):

```sh
scripts/omarchy/ssh.sh 'cat > /tmp/vdec-h264-test && chmod +x /tmp/vdec-h264-test' < build/vdec-qemu/vdec-h264-test-static
shasum -a 256 build/vdec-qemu/vdec-h264-test-static
```

Boot it: `scripts/omarchy/ssh.sh "sudo linux-a9-kexec reboot nextint-$C" || true`.
Keep netconsole attached (`scripts/omarchy/netconsole.py`) and a camera on
the screen. Recovery is unchanged (`panic=0`, then the crash kernel or
buttons/DFU). No NVMe writes beyond the existing root write window; the
decoder only touches its GART-mapped buffers.

Order: **T1 → T3 (boot) → T8 (consume_check) → T9 grey64 → T9 ts176 →
T9 ts176s4 → T7.** Stop at the first failure and bring back §8.

## 4. T1 + T3 — probe, firmware boot (as round 4)

```sh
scripts/omarchy/ssh.sh 'uname -a; sudo dmesg | grep -iE "apple-vxd" | grep -E "self-test|GART|registered" ; echo 1 | sudo tee /sys/kernel/debug/apple-vxd/boot >/dev/null; echo boot rc=$?; sudo dmesg | grep -iE "apple-vxd|DEVA" | tail -25; sudo cat /sys/kernel/debug/apple-vxd/mbox | head -16'
```

Expect round 4's ladder unchanged (dir[0] `0x08…`, three self-tests ok,
`DEVA firmware ready: mailbox up`, rc=0).

### 4.1 T8 — `consume_check` (the round-4 "0 us" question)

```sh
scripts/omarchy/ssh.sh 'echo 1 | sudo tee /sys/kernel/debug/apple-vxd/consume_check >/dev/null; echo rc=$?; sudo dmesg | grep consume_check | tail -8'
```

PASS = `rc=0` and:

```text
consume_check: before: TO_MTX rd N wr N, 0x0 …, 0x200c …, 0x2010 …
STEP consume_check: stage 1: <w0> <w1> written at TO_MTX [N..N+2), read back <same>, write index NOT published; wait 20 ms
consume_check: stage 1: rd N after 20 ms (unchanged: ok), wr register N, ring readback matches
consume_check: stage 2: write index N+2 published, no doorbell: rd …, taken (the reader polls 0x2030) after … us
consume_check: stage 3: doorbell rung: rd N+2, taken after … us
consume_check: PASS: rd firmware-driven yes, reader polls the write index, …
```

The firmware study predicts **stage 2 "taken"** (the reader polls 0x2030;
the doorbell is a wake-up). "not taken (the reader waits for the doorbell)"
is a finding, not a failure. **FAIL** = stage 1 `MOVED with nothing
published` (`rc=-EIO`): rd is not the firmware's read index, stop and
report; or `NOT taken` at stage 3 (`rc=-ETIMEDOUT`). Also report whether
`0x0`/`0x200c`/`0x2010` changed.

## 5. T9 — the first real decode

One stream at a time, smallest first. Each run prints a `FRAME` line per
picture and a `STREAM` verdict, and writes `/tmp/vdec-<stream>-<n>.nv12`.

```sh
scripts/omarchy/ssh.sh 'echo 1 | sudo tee /sys/module/apple_vxd/parameters/step_ioctls >/dev/null; sudo /tmp/vdec-h264-test -s grey64 -t 3000; echo rc=$?; sudo dmesg | grep -E "decode:|h264:|timed out|completion|PANIC" | tail -20; sudo cat /sys/kernel/debug/apple-vxd/mbox | head -16; sudo cat /sys/kernel/debug/apple-vxd/cmdbufs'
```

Expected on success (`rc=0`):

```text
stream grey64: 64x64, 1 pictures, profile 66 level 10, poc type 2, refs 1
capture: 64x64 NV12, bytesperline 256, sizeimage 24576
FRAME grey64 0 I: DONE, crc c45562bc, ffmpeg c45562bc MATCH, Y[0..7] 80 80 80 80 80 80 80 80 -> /tmp/vdec-grey64-0.nv12
STREAM grey64: 1/1 DONE, 1/1 bit-exact -> PASS
```

and in dmesg (`step_ioctls=1`): `STEP decode: src 0 va … (60 bytes, 1
slices, first NAL 0x25+23, header 38 bits), dst 0 Y … UV … stride 256, 556
command bytes in buf 0 …`, one `STEP decode: push 0x81 fence 0x…0 {0x…8106
0x008b4015 <chunk VA> <MMU word> 0x10211000 <stream-state VA>}` (139 words;
first slice 0x1 | first picture 0x10 | last 0x4004) and
`STEP decode: completion: fence …, 1 of 1 packets answered, status …, … us
-> DONE`. `mbox` stats: `completions 1 … faults 0`, `cmdbufs` all `free`.

Then the P pictures and multi-slice:

```sh
scripts/omarchy/ssh.sh 'sudo /tmp/vdec-h264-test -s ts176 -t 3000; echo rc=$?; sudo /tmp/vdec-h264-test -s ts176s4 -t 3000; echo rc=$?; sudo dmesg | grep -E "decode: (completion|TIMEOUT|picture)|timed out" | tail -12'
```

For ts176s4 expect `1 of 4 packets answered` (0x2000 = no response; only
the last packet answers). Last, and optional (CABAC is built from the
study but unvalidated, §7):

```sh
scripts/omarchy/ssh.sh 'sudo /tmp/vdec-h264-test -s ts176cb -t 3000; echo rc=$?'
```

The reference CRCs (ffmpeg): grey64 `c45562bc`; ts176 `6c1fb604 550cbab8
01220617`; ts176s4 `53953c0e 8763dd29 1b432e44`; ts176cb `127127a7
56978d6c 73f851e0`. The test prints them next to the measured ones.

Look at the frames on the Mac:

```sh
mkdir -p build/vdec-r5/device && scripts/omarchy/ssh.sh 'cd /tmp && tar cf - vdec-*.nv12' | tar xf - -C build/vdec-r5/device
ffmpeg -f rawvideo -pixel_format nv12 -video_size 176x144 -i build/vdec-r5/device/vdec-ts176-0.nv12 -frames:v 1 build/vdec-r5/device/ts176-0.png
ffmpeg -f rawvideo -pixel_format nv12 -video_size 176x144 -i build/vdec-r5/streams/ts176.nv12 -frames:v 1 build/vdec-r5/device/ts176-0-ffmpeg.png
```

### 5.1 Failure ladder (stop at the first that applies)

| Symptom | Meaning / next step |
| --- | --- |
| `decode: picture refused (-22): …` | Host-side refusal, nothing was sent. The reason is in the line; a test-stream refusal is a driver bug, report it |
| `decode job timed out: no completion for fence …` (FRAME … ERROR after ~1 s) | The firmware took the message (check `TO_MTX rd == wr` in the line) but never answered. Bring back `mbox`, `regs`, `cmdbufs` and the whole `decode:` STEP block. Then, once: `sudo cat /sys/kernel/debug/apple-vxd/regs` again 5 s later (does `0x200c` move?). Do **not** retry more than twice on this firmware instance: each timeout quarantines one of the 8 command buffers. To reset: close the test, `unbind`/`bind` (§6; stops the firmware, reclaims every buffer), then `echo 1 > …/boot` again. A wedged core needs a reboot |
| TO_MTX rd != wr in the timeout line | The reader did not take the 0x81 at all: report `mbox` (the TO_MTX words of the message are dumped there) |
| `completion fence … with fault: status 0x…` / FRAME ERROR quickly | The core reported an error: the status word is the finding (§3.4 of the protocol doc: `0x18000000` fault class, bit 27 after reset, bit 31 REATTEMPT). Bring back the exact line |
| `FW_DEVA_PANIC` | Firmware panic: line + `mbox`. Reboot before anything else |
| FRAME DONE, MISMATCH, Y bytes `5a 5a …` | The core completed but wrote nothing to the CAPTURE buffer (the test pre-fills 0x5a): output VA / register bank wrong. Bring back the dmesg `decode:` line (Y/UV VAs) |
| FRAME DONE, MISMATCH, other bytes | Pixels were written: pull the `.nv12` files (above) and compare with the ffmpeg PNG. Luma right but chroma shifted/garbled → the chroma base or stride (this driver uses stride 256 and a 4 KiB-aligned chroma plane; the RE agent's example used 192, both are 64-byte multiples, so the device test does not tell them apart); block noise only at MB edges → try the deblock pass (study §16.9 item 3, not implemented); garbage → field packing |
| iPad freezes | Possible if the core hangs the bus. Force reboot; no persistent state is touched. Report the last STEP line (camera) |

## 6. T7 — unbind/bind after decoding

Unchanged from round 4: close every fd first, then unbind/bind; expect a
clean remove ladder and probe. Run it last.

## 7. What is assumed, and how the device settles it

| Assumption | Mark | Settled by |
| --- | --- | --- |
| AVC register program (§16.4), incl. forced profile 3 / FE+BE control 0x11, flat 0xe000 block on baseline | [V] host recipe, [U] hardware | T9 grey64 bit-exact |
| B = EBSP header bits incl. NAL header byte; DMA from NAL header + whole header words | [V] kext, [U] hardware | T9 grey64 (IDR B=32, so the DMA skips 4 bytes) |
| Hardware DPB slot = V4L2 dpb index; 0xdeadbeef for unused slots | [I] driver policy | T9 ts176 P frames bit-exact |
| Stored-MB buffer per CAPTURE, size formula | [V] kext | T9 ts176 (P pictures) |
| IntraBuffer/StreamStateBuffer sizes (M = 2 assumed) | [V] formula, [U] M | T9; a too-small buffer shows as corruption at the picture bottom or a fault |
| NV12 stride 256-aligned / 4 KiB chroma | [I] (psb masks) | T9 (stride 256 for both test sizes) |
| 0x1008 = 0xf00 after GART changes | [V] kext, [I] meaning | every T9 submit does it (buffers were just mapped) |
| Only the 0x4004 packet answers | [V] firmware study | T9 ts176s4: `1 of 4 packets answered` |
| No 0x87 deblock pass needed | [I] | T9 edges bit-exact |
| CABAC (aligned header, PPS0 bit 2, cabac_init_idc; no context upload) | [V] kext fields, [I] init by the silicon | T9 ts176cb (optional, last) |
| `consume_check` stage 2 | [V] firmware study says the reader polls 0x2030 | T8 |

## 8. What to bring back

1. `docs/periph/logs/vdec-round5-dmesg.txt`: full `dmesg` after T9.
2. T8's eight `consume_check` lines and the rc.
3. Every `FRAME`/`STREAM` line and the rc of each T9 run.
4. On any failure: `mbox`, `regs`, `cmdbufs`, the `decode:` STEP lines
   (with `step_ioctls=1`), and the `.nv12` files if frames were DONE.
5. `/proc/interrupts` line of the vxd IRQ after T9 (it must have counted).

## 9. Known limits

- B slices, weighted prediction, interlace, scaling matrices, 8x8
  transform, 4:2:2, 10-bit: refused (recipe not decoded or not validated).
- At most 8 slices per picture (fence packet index has 3 bits; 64 parsed).
- The second deblock pass (kind 3 / 0x87) and alternate output are not
  implemented (§16.9 items 3).
- `APPLE_VXD_JOB_TIMEOUT_MS` 1 s; a timed-out picture's command buffer is
  quarantined until the firmware is stopped.
- GART identity map: two vb2 buffers importing the same dmabuf would
  share entries (MMAP buffers are fine). Unbind with open fds is still the
  known UAF (round 4 T7).
- MTX halt: not implemented (the kext has none).
