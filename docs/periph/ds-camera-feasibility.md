# A9 cameras / ISP — first feasibility study

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. iPad 5, iPad6,11, S8003 / J71t, iPadOS 16.7.10 (20H350).
**No device access, no kernel edit, no commit.** Everything below comes from the
saved ADT (`docs/adt/j71t.json`, `j71t.txt`), the live private dumps
(`build/firmware/private/IODeviceTree.txt`, `IOService.txt`), the shared iOS 16
kernelcache, or public Linux source.

Evidence tags: **[V]** = pinned in an artifact quoted here (ADT/ioreg/kernelcache
bytes/disassembly/upstream source). **[I]** = inference that fits the evidence
but is not proven. Unproven items are called out as "unresolved".

Artifacts:

```text
kernelcache  build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12
             sha256 c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a
kext         com.apple.driver.AppleH6CameraInterface  (class AppleH6CamIn)
             __TEXT      fffffff005601b00 + 0x71d7
             __TEXT_EXEC fffffff006462000 + 0x15750
             extracted to build/periph/kexts/AppleH6CameraInterface.* (gitignored)
Asahi source drivers/media/platform/apple/isp/ @ AsahiLinux/linux branch "asahi"
             (isp-drv.c, isp-fw.c, isp-cmd.h, isp-regs.h, isp-ipc.h)
```

## 0. Answer in one table

> Private graphics-dependent excerpt omitted from this source snapshot.

## 1. Hardware

### 1.1 The ISP node

There is exactly one ISP instance on this SoC — no `isp1`/second node exists
anywhere in the ADT **[V]**.

`/arm-io/isp` (ADT `docs/adt/j71t.json`; live `IODeviceTree.txt:2088`,
`IOService.txt:8645`):

| Property | Value |
| --- | --- |
| `compatible` | `"isp,s8000"`, `"isp,s5l8960x"` **[V]** |
| `reg` | `0x05B00000` + `0x500000` (5 MiB) **[V]**; iOS reports it as `0x205B00000` because its `arm-io` window is offset by `0x2_00000000` **[V]** |
| `interrupts` / `interrupt-parent` | AIC **92, 93, 94, 95** **[V]** |
| `iommu-parent` | phandle `0x62` = `mapper-isp` (under `dart-isp`) **[V]** |
| `power-gates`, `clock-gates` | `24, 93, 94, 38` **[V]** → by `pmgr/devices` id: **ISP, ISP_SENS0, ISP_SENS1, ISP-FAST** **[V]** |
| `clock-ids` | `281, 282` **[V]**; PMGR also exposes `ApplePMGRFunctionISPRefClock` and `function-conf_isp_ref0/1_clk_freq` (ISP reference clock 0/1) **[V]** |
| reset | `function-device_reset` → PMGR `ARST` arg 24 **[V]** |
| `sensor-type` | **35** (0x23) **[V]** |
| camera presence | `camera-rear`, `camera-front`, `face-detection-support` **[V]** |
| `function-saca0/1/2` (+`b`,`c`) | `/sacm`, args `'cam0'…'cam5'` **[V]** |

`/sacm` (`sacm,1`, no `reg`) is matched on the live device by
`AppleARMSlowAdaptiveClockingManager` **[V, IOService.txt:13627-13646]**: SAC =
*slow adaptive clocking*, i.e. the MIPI-clock/"aggressor" throttling path, **not**
a sensor bus. `saca0/0b/0c` = channel 0 raw10/raw12/raw14 MIPI frequency tables
(the kext has matching `sacActionChannel0_Raw10/12/14` code and counters) **[V]**.

Driving nothing else from the ISP node is needed for a still capture, but SAC is
what changes MIPI clock at runtime — fixed-clock bring-up can ignore it **[I]**.

### 1.2 Sensors, flash, actuators

The ADT has no camera child nodes: `/arm-io` contains only `i2c0` (PMU, LP8559,
TriStar) and `i2c2` (ALS) **[V]**; the front/rear sensors are not I²C devices of
the AP. Board-level data instead comes from the live driver instance
(`IOService.txt:8689-8712`), which publishes per-unit module identity:

| Property | Value |
| --- | --- |
| `BackCameraSerialNumber` | `0000072e01386501` **[V]** |
| `FrontCameraSerialNumber` | `000000000501981c` **[V]** |
| `BackCameraModuleSerialNumString` | `F6D74612B3NHXG4A4` **[V]** |
| `FrontCameraModuleSerialNumString` | `DNM74272NCUG2DP5H` **[V]** |
| `BackCameraModuleFocusActuatorID` | **34** **[V]** |
| `BackCameraActive` / `FrontCameraActive` | `No` (dump taken with cameras closed) **[V]** |
| `FirmwareLoaded` | **Yes** **[V]** — firmware *has* been loaded in that boot (see §2.4) |

`/product/camera` **[V]**: `auto-focus=1`, `rear-hdr=1`, `rear-max-video-fps-1080p=30`,
`rear-max-slomo-video-fps-720p=120`, `front-flash-capability=1`,
`rear-max-video-zoom=3`, `video-cap=2`, `pipelined-stillimage-capability=1`. The
ISP node defines no torch/strobe function and there is no flash node, so the
front "flash" is the screen and the rear has none **[V]**.
Public specs put the rear module at 8 MP and the front at 1.2 MP; the ADT does not
carry resolution, so treat the numbers as **[I]**.

Sensor identity (vendor/part) is **unresolved** [I]. The kext's VCM tables name
Rohm 64290/64291/64293 and ADI 5816/5840 **[V]**, and the module reports actuator
id 34 **[V]**; which one that is, and which sensor sits behind it, needs the
on-device read in §5.

### 1.3 Power, clock, reset wiring

Everything the host must touch before the ISP can run is in the ADT **[V]**:

| Resource | Node/function |
| --- | --- |
| ISP core, sensor 0, sensor 1, "ISP-FAST" domains | `power-gates = 24, 93, 94, 38` (+ same ids as `clock-gates`) |
| ISP DART domain | `dart-isp` `power-gates = 32` ("ISP_DART") |
| Camera analog rails | `function-cam0_ldo` (PMGR `pmuL` 0x208), `function-cam_avdd_ldo` (0x20c), `function-focus_ldo` (0x30a) |
| Sensor 0/1 DVDD | `function-cam_dvdd_gpio` → GPIO 111 |
| Sensor 0/1 shutdown | `function-sensor_shutdown` GPIO 48, `…shutdown2` GPIO 52 |
| Sensor 0/1 ext clock | `function-sensor_extclk` GPIO 49, `…extclk2` GPIO 53 |
| ISP ref clocks | `function-conf_isp_ref0/1_clk_freq` (PMGR `ISPC`) |
| Device reset | `function-device_reset` (PMGR `ARST` 24) |

Mainline already has `ps_isp`, `ps_isp_sens0`, `ps_isp_sens1` power domains in
`s800-0-3-pmgr.dtsi` **[V]**; **no `ISP-FAST` domain label exists yet** **[V,
grep of the 7.3-rc1 tree]**. The `pmuL` LDO functions have no mainline regulator
driver **[I]** — this is new code, not configuration.

### 1.4 IOMMU

`/arm-io/dart-isp` **[V]**: `dart,s8000`/`dart,s5l8960x`, `0x05B22000`+`0x2000`
(TTBR) and `0x05B30000`+`0x4000` (TCR), IRQ **96**, `page-size = 4096`,
`stt-idx-width = 8`, `no-fault-clients = 8`, `manual-availability = 1`,
`error-reflector` at `0x20FFFC000`.

This is the **same S5L8960X DART family this project already drives** for PCIe and
Wi-Fi (`docs/pcie/HANDOFF.md`, base-relative windows verified on this unit) **[V]**,
so the IOMMU is a reuse, not a risk. Note it is 4 KiB-page, not 16 KiB.

### 1.5 Neighbours in the camera path

| Block | MMIO | IRQ | Role in capture | Status here |
| --- | --- | --- | --- | --- |
| `isp` | `0x05B00000`+5 MiB | 92–95 | CSI-2 RX, ISP pipeline, ISPCPU | this study |
| `dart-isp` | `0x05B22000`, `0x05B30000` | 96 | ISP IOMMU | reuse 4K S5L8960X |
| `scaler0` | `0x07900000`+0x4000 | 179 | preview/scale | studied, no driver (`ds-scaler-jpeg.md`) |
| `jpeg` | `0x07B00000`+0x2000 | 175 | still encode | studied, no driver (`ds-scaler-jpeg.md`) |
| `ave` | `0x07CD0000` | 159–166 | H.264/H.265 encode | out of scope |
| AOP `spherecontrol` | — | — | motion data → ISP (`function-motion_data_*`, `aop-isp-endpoint-cfg`, `AppleSPUSphereDriver`) | optional; §2.6 |

For a *raw still* nothing beyond `isp` + `dart-isp` is required; JPEG is only
needed to produce a normal photo file **[I]**.

### 1.6 Firmware references in the ADT

There are **none** for the ISP: no `firmware-name`, no carveout, no
`firmware` property **[V]**. Compare `/arm-io/aop/iop-aop-nub`, which *does* have
`firmware-name` **[V]**. `/chosen` carries a single camera-related knob,
`isp-horizon = 0` **[V]** (a board/tuning selector; its consumer is unresolved).

## 2. The iOS stack

### 2.1 Driver identity

`com.apple.driver.AppleH6CameraInterface`, class `AppleH6CamIn`, version 15.4.0,
copyright 2016–2017, built with the 16.7 SDK **[V, prelink Info.plist]**. One
personality: `IONameMatch = "isp,s5l8960x"`, `IOProviderClass = AppleARMIODevice`
**[V]** — i.e. the kext binds the A7-compatible naming, not `isp,s8000`. On the
live device it matched at boot and is `active` **[V, IOService.txt:8702-8708]**.

The same kext image also carries `AppleH9CamIn::…` log strings **[V]**; H9 is
presumably the A10/A11 generation of the same driver **[I]**.

Dependencies (`OSBundleLibraries`) are only `kpi.*`, `IOSurface`, `IOReportFamily`
and `AppleARMPlatform` **[V]** — **no RTKit/RTBuddy**, so the ISP is not an
RTKit coprocessor. It is the older style: raw MMIO + a bespoke shared-memory IPC.

### 2.2 The ISPCPU

The driver talks about an "ISPCPU" throughout and performs a classic
load-and-release boot **[V]**:

- `AppleH6CamIn::start` maps device-memory index 0 of its provider (the ISP
  window) and reads a version register at **`base + 0xA0000`**
  (`fISPVersion = value >> 16`); it then picks a firmware buffer
  size: **3 MiB default, 4 MiB if version == 5**, overridable by boot-arg
  `camFWBufSize` **[V, disassembly of `start` at `fffffff00646364c`, refs at
  `fffffff006465b5c` and `fffffff006465bac`]**. It then clears `FirmwareLoaded` **[V]**.
- `ISP_LoadFirmware_gated(void *buf, u32 len, u32 platform_id, task_t task)`
  builds a memory descriptor over `buf`, prepares and maps it, clears the driver's
  shared surface at `[heap+0x28]` (`fCamFWBufSize` bytes), `memcpy`s `len` bytes
  in, stores `platform_id` (used to index a 32-entry, `0x150`-stride platform
  table at `fffffff006ef60e0`), logs `fw len=%d`, validates the image
  (`"Illegal ISPCPU firmware"`), then sets `FirmwareLoaded = true` and prints the
  image's SHA-256 when debug logging is on **[V, `fffffff006471fa0`]**.
- `ISP_Init` then waits for a wake signal from the ISPCPU, reads
  `numIPCChannels` / `ipcQueueSize`, allocates the driver heap, receives a second
  interrupt announcing the **channel description table**, enables interrupts and
  sends `CISP_CMD_START` + `CISP_CMD_PRINT_ENABLE` + `CISP_CMD_CH_INFO_GET`
  **[V, os_log strings]**.

Same shape as Asahi's M1 (`isp-fw.c`: reset the coproc, wait for the magic, read
`num_ipc_chans`, allocate `extra_size` heap, parse the channel table) **[V]**.

### 2.3 Host↔ISP protocol

Pinned by the kext's own log strings and IOReport counters **[V]**:

- **Channels**: the kext names `TERMINAL`, `DEBUG`, `BUF_H2T`, `BUF_T2H`,
  `IO_T2H` and `SHAREDMALLOC`; Asahi's M1 set is the same plus a plain `IO`
  channel **[V]**. Channel table entries carry
  name/type/source/count/IOVA; types are command / reply / report **[I from
  Asahi's struct + A9's "channel description table" log]**.
- **Commands**: `CISP_CMD_*` opcodes, asynchronous with `ack` and `res`:
  `"ispCmdSend - command (opcode: 0x%04X) took %lld usec res=0x%08X ack=0x%08X"`
  plus hexdumps `"ISPCPU Command Packet Sent"` / `"ISPCPU Response Packet Received"`
  **[V]**. Named commands seen on this firmware: `CISP_CMD_START`,
  `CISP_CMD_POWER_DOWN`, `CISP_CMD_PRINT_ENABLE`, `CISP_CMD_CH_START/STOP`,
  `CISP_CMD_CH_SET_FILE_LOAD`, `CISP_CMD_CH_INFO_GET`,
  `CISP_CMD_CH_NVSTORAGE_INFO_GET`, `CISP_CMD_CH_COLOR_FULL_RES_LSC_ENABLE`,
  `CISP_CMD_CH_MC_AOP_ISP_INFO_SET` **[V]**.
- **Buffers**: H2T/T2H tagged buffers ("`h2tBuf: pool=%d, tag=%p, addr0=…, len0=…`"),
  returned on frame completion, with an `FFW_INTERPROC_*` layer on top
  (`FFW_INTERPROC_MSG_MSG`, `FFW_INTERPROC_BUFF_TOT`) **[V]**. The firmware can
  ask the host for memory (`"ISP Memory allocation request, %d bytes"`) and the
  host defers to `SHAREDMALLOC` — the FW heap is host-allocated **[V]**.
  The **byte layout** of the command/reply packets and of the buffer descriptors
  is *not* pinned in this round: the driver only hexdumps them. Recovering it is
  open item 2 in §6 **[V, gap]**.
- **Observability**: IOReport group `H6ISP` counts `ISPCPU Commands Sent`,
  `ISPCPU Responses Received`, `H2T Buffer Responses`, `T2H Buffer Notifications`,
  `T2H IO Commands`, `ISPCPU Log Messages`, `Allocated Shared Memory`,
  `Number SAC Action0..2`, `ISP Power`/`ISP Fast`/`Sensor0 Power`/`Sensor1 Power`
  **[V, IOService.txt:8711]**. These counters are a ready-made checklist for the
  Linux bring-up: each stage of the plan can be validated with the same names.
- **Interrupts**: 4 vectors, handler prints `"pending: 0x%08X"`; DART faults are
  decoded by the driver (`DARTErrorHandler`, `HwClass/HwError/IsWrite/AXIId`) **[V]**.
- **Sensor I²C**: `"ISP_I2CRead() - Timeout Waiting for SMBSYS_REG_SMSTA__XEN"`,
  with slave-error and bus-jam diagnostics **[V]**. So the ISP block contains an
  SMBus-style I²C master that the host can drive, *and* the firmware uses for
  sensor/NVM access **[I]**.

### 2.4 Where the firmware comes from — the important negative result

- The kext has **no `__firmware` section and no blob**: its sections are only
  `__TEXT,__const` (1 KiB), `__TEXT,__os_log` (22 KiB), `__TEXT,__cstring` (4 KiB),
  `__TEXT_EXEC,__text` (86 KiB), `__stubs`, `__DATA,__data` (392 B) and
  `__DATA_CONST,__const` (15 KiB of pointers) **[V]**.
  Across the whole kernelcache only `com.apple.driver.AppleS8000SmartIO` has a
  `__DATA,__firmware` section **[V]**.
- There is **no firmware file name, resource name or path string** in the kext
  **[V, full `__cstring` dump]**.
- The only loader takes a pointer + length + platform id + task from its caller
  (§2.2) **[V]**.

Therefore the ISP image is **outside the kernel** and is installed at runtime
**[I]**. Two candidates remain: the camera userspace stack hands it down through
`AppleH6CamInUserClient` (entitlement `com.apple.camera.iokit-user-access`,
same `(ptr,len,task)` pattern as `ISP_LoadSetfile_gated` and
`ISP_LoadOverrideNVM_gated`) **[I]**, or a bootloader carveout does. Nothing in
the ADT or the kext supports the carveout theory. The live ioreg reports
`FirmwareLoaded = Yes` while both cameras are inactive **[V]** — consistent with a
load that happened earlier in that boot, without telling us who triggered it.

**This is testable offline-ish and is step D1 of §5:** the driver itself prints
`"AppleH6CamIn: ISPCPU Firmware SHA256 : <hex>"` (gated by a debug flag) **[V]**.
Hash-matching that against candidate files in the iOS root filesystem identifies
the blob exactly.

### 2.5 Tuning data is also userspace

`ISP_LoadSetfile_gated(void*, u32, task_t, u32)` (`CISP_CMD_CH_SET_FILE_LOAD`),
`ISP_LoadDataFile_gated`, `ISP_LoadOverrideNVM_gated` and
`ISP_LoadFullResLensShadingTable_gated` all take a **user pointer + size + task**
**[V]**, and the driver logs `"No set-file loaded for camera channel %d"` when
they are absent **[V]**. So the ISP is useless without a per-sensor "set-file"
(tuning) blob and optional NVM/LSC data, all from iOS.

### 2.6 Optional pieces

- **AOP motion data**: the ISP consumes gyro data for OIS/EIS through
  `function-motion_data_enable/param_get/param_set` → AOP `spherecontrol`
  (`AppleSPUSphereDriver`), with DT `aop-isp-endpoint-cfg` and a
  ring-buffer + mapped write-notifier register **[V]**. Not needed for a still
  **[I]**; it does tie into the AOP driver that is still being brought up (P6).
- **Autofocus**: VCM drivers for Rohm 64290/64291/64293 and ADI 5816/5840 are in
  the kext **[V]**; manual focus goes through
  `ISP_DriverInitiatedManualFocus_gated(camChannel, focusPos)` **[V]**.
- **Face detection**: `face-detection-support = 1` and `CISP_CMD_CH_FACE_DETECTION_*`
  **[V]**; firmware-side, no host work needed.

## 3. Prior art and how close M1 is

**Asahi's driver** (`drivers/media/platform/apple/isp`, Eileen Yoon, AsahiLinux
`asahi` branch, GPL-2.0) **[V]**:

- Matches `apple,t8103-isp`, `t8112`, `t8122`, `t6000`, `t6020`, `t6030`,
  `t6031` — **M1-class and later only** **[V]**.
- Split: `isp-fw.c` (coproc reset/WFI, boot handshake, channel-table parse),
  `isp-cmd.c`/`isp-cmd.h` (CISP commands, buffer pools, output config),
  `isp-ipc.c` (channel send/handle), `isp-drv.c` (DT, power domains, IOMMU,
  presets), `isp-v4l2.c` (V4L2 node), `isp-cam.c` (sensor/"detect camera").
- **It contains no firmware loader**: `isp-fw.c` powers the domains, resets the
  coproc through the `coproc` register window (`ISP_COPROC_EDPRCR`, `RVBAR`
  defined in `isp-regs.h`), writes the `ISP_GPIO_*` mailbox, waits
  for magic `0x8042006` twice, reads `num_ipc_chans` and the bootargs/extra-heap
  sizes from the same GPIO window, then parses the channel table **[V]**.
- Channel names it requires: `TERMINAL`, `IO`, `DEBUG`, `BUF_H2T`, `BUF_T2H`,
  `SHAREDMALLOC`, `IO_T2H` **[V]** — the A9 firmware names the same set minus the
  plain `IO` channel (§2.3).
- Command namespace `CISP_CMD_*` with the same names the A9 driver logs
  (`START 0x0000`, `PRINT_ENABLE 0x0004`, `POWER_DOWN 0x0010`,
  `CH_SET_FILE_LOAD 0x0111`, `CH_INFO_GET 0x010d`, `CH_STOP 0x0101`, …) **[V]**.
- No A-series or `s5l8960x` support anywhere; `CONFIG_VIDEO_APPLE_ISP` depends on
  `ARCH_APPLE` **[V]**.
- Not in mainline as of 7.3-rc1 (the tree this project builds has no
  `drivers/media/platform/apple/` and no `drivers/staging/media/apple/`) **[V]**.

**So: same family, same protocol lineage, but a different (older) hardware
generation.** The A9 ISP is the S5L8960X-compatible block ("H6"), the M1 one is
T8103; the *shape* (release coproc → read channel table → CISP commands →
H2T/T2H buffers → FW mallocs from host) is the same, but register offsets,
channel counts, boot mailboxes (A9: interrupts; M1: GPIO window), command
numbering and the packet layouts are **not** proven equal and must be re-derived
**[I]**. Expect to port the *structure* of Asahi's driver and re-derive the
numbers, exactly like the VDEC work reused TI's MSVDX layout.

**Older-SoC work:** none. A7/A8 (`s5l8960x`) has a full mainline device tree but
no ISP node and no driver **[V]**; the A10 (T8010) has no camera work in the
HoolockLinux/Pauli1Go trees **[I]**. This would be the first pre-M1 Apple ISP
driver anywhere.

> Private graphics-dependent excerpt omitted from this source snapshot.

## 4. Verdict

**Feasible, and the protocol is not the hard part.** The hard parts, in order:

1. **Blob sourcing.** The ISP image and the tuning set-file exist only on iOS.
   AGENTS.md currently says iOS is expendable — for cameras that is no longer
   free: **pull the camera blobs before any iOS wipe** (step D1).
2. **Sensor access is behind the ISP.** Sensor register I/O is an I²C master
   inside the ISP block; you cannot identify or configure the sensor without
   either that SMBSYS path or a booted ISP firmware. That makes "identify the
   sensor" and "boot the ISP" mutually entangled, and pushes the first real
   milestone to firmware boot rather than a simple probe **[I]**.
3. **No register map.** The A9 sub-block layout inside the 5 MiB window, the
   command packet format, and the A9 boot mailbox are all still unknown; Asahi's
   M1 map is only a starting hypothesis.
4. **Power/clock plumbing that mainline lacks**: `ISP-FAST` domain, `pmuL` LDOs,
   sensor shutdown/extclk pins, `sacm` (SAC) for MIPI-clock changes.

Nothing here is architecturally blocked (no secure-world handshake, no
personalised firmware, no encrypted blob is implied by the driver) **[I]**.

## 5. Staged plan

| Stage | What it proves | Needs | Exit criterion | Risk |
| --- | --- | --- | --- | --- |
| **D1** device, cheap | blob identity | jailbroken iOS, Camera app, debug boot-arg (`camDebug`/`camLogging` are the kext's property names; the boot-arg spelling follows Apple's convention **[I]**), syslog | kernel log shows `ISP_InitialSensorDetection - found sensor on chan %d: 0x%04X` and `ISPCPU Firmware SHA256 : …`; hash-match them against files pulled from the iOS rootfs (search `/System/Library/**` for 1–4 MiB blobs, and the iPhoneOS16.7 IPSW's camera kext resources) | Needs a camera session; the SHA-256 line is debug-gated |
| **D2** device, cheap | block is alive | DT node + `ps_isp`, `ps_isp_sens*`, `ISP-FAST`; `dart-isp` (reuse); 5 MiB ioremap | `devmem`/module read of **`0x5BA0000`** returns a small non-zero `fISPVersion`; ISP interrupts can be cleared | PMGR ids ↔ mainline labels must be matched; `ISP-FAST` label missing upstream |
| **D3** | coproc boots | D1 blob; DART surface for the firmware heap (3/4 MiB) + IPC surface; reset/WFI handshake | Driver logs the wake signature, reads `numIPCChannels`, and prints the **channel table names** from our own code | Boot mailbox for A9 unknown (interrupt vs GPIO); WFI/reset sequence guessed from M1 |
| **D4** | host↔FW command path | D3; command/reply channel | `CISP_CMD_PRINT_ENABLE` + `CISP_CMD_START` ack with `res=0`; firmware log lines arrive on `DEBUG` | Opcode numbers/struct layout may differ from Asahi's header |
| **D5** | sensor identified | D4; ISP SMBSYS I²C (or FW sensor-detect results) | sensor ID read, module NVM/LSC read, `CISP_CMD_CH_INFO_GET` returns a channel | Sensor may need a vendor driver that does not exist in Linux |
| **D6** | **one raw frame** | D5; tuning set-file (D1); raw buffer pool + DART | a RAW10 buffer arrives via a T2H notification and is dumped to a file, non-black, correct size | Set-file/format mismatches; MIPI timing; DVFS (SAC) unhandled |
| **D7** | it is a camera | D6; V4L2 + libcamera softISP; JPEG for stills | `v4l2-ctl --stream-mmap` and a libcamera/PipeWire app both see a working camera | Userspace/ISP colour pipeline quality work |

Sequencing note: **D1 before any further iOS shrinking.** It is the only step
that can permanently lose data, and it is nearly free.

### What userspace needs

- **Kernel**: one V4L2 video node per camera channel (`VIDIOC_*`, `vb2` +
  `VIDEOBUF2_DMA_SG` like Asahi), streaming **RAW10** (and later NV12/YUYV) with
  `DMABUF` export so the same buffer can go to the scaler/JPEG without a copy
  **[I, mirroring `isp-v4l2.c`]**.
- **libcamera**: Asahi's path is the "simple pipeline handler" plus a **software
  ISP** in libcamera (CPU debayer + 3A) — no kernel ISP tuning needed. On a 2-core
  A9 at 1080p that will be slow; plan on a reduced preview resolution first,
  full-res stills through the firmware's own pipeline later **[I]**.
- **Desktop**: PipeWire + WirePlumber already ship in Omarchy; with `libcamera`
  built against the V4L2 node, browsers and `cheese`-class apps get the camera
  through the PipeWire camera portal. No Omarchy patch is expected **[I]**.
- **Bring-up tools**: `v4l2-ctl`, `yavta`/`v4l2-compliance`, `ffmpeg -f v4l2`,
  and a raw-dump tool for RAW10 → PGM **[I]**.

### Risks, condensed

> Private graphics-dependent excerpt omitted from this source snapshot.

## 6. Open items for the next round

1. **D1** (orchestrator, device): boot iOS with `camDebug=1` `camLogging=1`,
   open Camera, capture `dmesg`; pull the SHA-256 line and the sensor-detection
   lines. Then locate and hash-match the blob in the iOS rootfs / IPSW.
2. Decompile `AppleH6CamIn::ISP_Init` and `ispCmdSend` in the shared Ghidra
   project to extract: the A9 boot mailbox, the command packet header layout, and
   the **opcode numbers** actually used (compare with Asahi's `isp-cmd.h`).
3. Recover the SMBSYS I²C register block from `ISP_I2CRead/ISP_I2CWrite` so the
   sensor can be probed without firmware (would decouple risk 2 above).
4. Confirm the ADT `power-gates` id → mainline `ps_*` label mapping, and add an
   `ISP-FAST` power domain to `s800-0-3-pmgr.dtsi` if it is really separate.
5. Decide where the ISP image lives in the Linux firmware layout
   (`apple/j71t-isp-fw.bin`, `apple/j71t-isp-setfile-<sensor>.bin`) and add it to
   `scripts/periph/pull_firmware.py` + `firmware-inventory.md` — never to git.
