# Mesa frame relay: AP-side path reconstruction (IRQ → SPI capture → SEP 0x06/0x65)

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Workstream research note, 2026-10-26. Scope: the *whole* AP-side path that turns a
Mesa interrupt into a 19,668-byte sensor frame and then into an SBIO
`0x23 → 0x06 → 0x65 → 0x07` relay on the SEP endpoint. Companion documents
(not repeated here): `mesa-driver-re.md` (hardware/ADT/power/command-table
survey), `mesa-driver-implementation.md` (the existing status-only Linux slice),
`sbio-runtime-re.md` (SEP mailbox, GenericTransfer framing, SBIO opcode map),
`driver-notes.md`, `sbio-commands.md`.

This pass adds the frame-relay layer that those documents left open, and
corrects four statements in them (see §9).

## 0. Evidence classes and method

Labels used throughout:

- **[V-trace]** observed on the wire: the iOS 16.7.10 os_log capture
  `build/sep/sbio-trace.log` (12,965 lines, 2026-10-24 first enrolment) and the
  curated excerpts. This is capture fact, not runtime evidence for a Linux boot.
- **[V-code]** read directly from the shipped kernelcache disassembly
  (`build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12`, image base
  `0xfffffff005298000`, Ghidra project `kernelcache.release.iPad6,11_12`).
  Addresses are static file addresses; kext code lives in `__PLK_TEXT_EXEC`
  (`0xfffffff005950000` + `0x137c000`) and `__PRELINK_TEXT`
  (`0xfffffff005298000` + `0x6b8000`).
- **[I]** inference from the two above. **[U]** unknown, with an anchor.

Method note: the shared Ghidra MCP server died at 22:55 (JVM gone, HTTP handler
wedged; last successful autosave 22:25) and came back later in the pass. Most of
the work was therefore done with a read-only Mach-O segment map + Capstone linear
disassembly (`llvm-objdump` cannot disassemble `__PLK_TEXT_EXEC`), resolving
`adrp`/`add`/`ldr` pairs and jump-island stubs mechanically; the two functions in
§4.5 and the `0x4f` fixup in §4.2 were decompiled with the restarted server
afterwards and are the only decompiler-sourced facts here. No Ghidra state was
written, no functions were created or renamed, no files outside this document
were touched. Call-graph claims are `bl`-based; vtable-dispatched edges are
called out explicitly.

Where a fact is only visible as a *log string next to a code path*, the string
address is given so it can be re-checked.

---

## 1. The path in one picture (all steps evidenced)

```
sensor asserts GPIO 44  ──►  AppleMesa::handleInterrupt            0x5efd20c   [V-code]
                               ├─ if X162Bypass (this+0x108): snapshot poll under IOLock(this+0x2d8)
                               └─ else: sendDeferredCommands 0x5efd41c
                                        getSensorStatusWithRetry 0x5efafc8
                                          └─ getSensorStatus 0x5ef3024   (7B → 23B)   [V-code]
                                             └─ isSensorStatusValid 0x5ef0c00
                                        └─ on failure: FIRE_ESD_RECOVERY
                             handleStateMachine 0x5efdc94                          [V-code]
                               └─ state > 3 && size != 0 && size <= 0x10000
                                    └─ readSensorData(size) 0x5efb0f8             [V-code]
                                         ├─ 11B  80 13 00 0b 00 00 00 <u32 size>   [V-trace]
                                         └─ 1B 00 + 19668B rx (one transport call) [V-trace]
                             ── signals the capture event source [I] ──
AppleMesaSEPDriver::handleCapture 0x66b67bc                                     [V-code]
  ├─ getCaptureBufferFromIOMD 0x66ae010   (type check: 19668 for type 1)        [V-code]
  ├─ handleCaptureData 0x66b6b6c          (type 1, source match, queue)         [V-code]
  └─ image-processing init builder 0x66a0d64                                    [V-code]
       ├─ sepTransact(0x23, NULL, 0, NULL, NULL)                               [V-trace]
       ├─ sepTransact(0x06, payload, 151|167, ...)                             [V-trace]
       ├─ sepTransact(0x65, frame_buf, 19668, ...)  → chunked fc/fd             [V-trace]
       └─ sepTransact(0x07, NULL, 0, out, 192)                                 [V-trace]
```

One-frame wall clock from the capture (12:10:12.194 – 12:10:12.279, 85 ms
total): poll 0.11 ms, capture write 0.07 ms, data read 20.8 ms, `0x23/0x06/0x65`
2.8 ms, `0x07` request → 192-byte reply 41.6 ms, `0x08` → 3224-byte reply
14.2 ms.

---

## 2. Sensor side: `AppleMesa` state machine (AppleBiometricSensor.kext)

### 2.1 Interrupt entry and IRQ gating — `handleInterrupt` `0xfffffff005efd20c`

**[V-code]** decoded control flow:

```
log "%s: entry: state=%d"                      ; this+0x150 = driver state  [0x5498214]
u8 deferred = 0                                ; [sp+0x1f]
if (this->0x108) {                             ; <-- X162Bypass (poll-only product mode)
    IOLockLock(this->0x2d8)
    if (this->0x2d0) {                         ; pending status snapshot owner
        getSensorStatus(this, this+0x159, 0x10, 1)
        [this->0x2d0 vtable+0x5d0](../../release/OMISSIONS.md)   ; copy 16B into that buffer
    }
    IOLockUnlock(this->0x2d8)
    → 0x5efd340: IOLockUnlock, AppleSPIBiometricSensor::enableInterrupts(this, 0), return
}
sendDeferredCommands(this, &deferred)           ; 0x5efd41c
r = getSensorStatusWithRetry(this, /*maxRetries=*/ (this->_lastCmd == 0x18), /*verbose=*/1)
if (r) {
    log "%s: FIRE_ESD_RECOVERY, reason=%d"     ; [0x5498228]
    this->0x2f2 = 1; this->0x318 = 1
    if (this->0x2f0) { [this->0x308 vtable+0xf0](../../release/OMISSIONS.md); [this->0x308 vtable+0xb0]() }
}
```

Facts that matter for Linux:

1. **The handler itself does one poll** (`getSensorStatusWithRetry`), i.e. the
   IRQ handler is where the 23-byte SPI exchange happens on iOS, not a deferred
   work item. There is no SPI read of frame data in the handler.
2. **`this+0x108` is the `X162Bypass` product flag**, not a per-frame gate. It is
   written once in `AppleMesa::start` (`0x5ef9680`) from a provider property
   whose value is logged as `"%s: X162Bypass= %u"` (`0x54969d8`); when it is
   non-zero, `enableInterrupts(this, 0)` is called at start, and every handler
   entry takes the snapshot branch: sample the 16-byte status into `this+0x159`
   under `IOLock(this+0x2d8)`, hand it to `this+0x2d0`, then **disable** the
   event source again (`0x5efd350` passes `w1 = 0`). In bypass mode the state
   machine is therefore driven by the event-source poll timer
   (`_setPollingInterval` `0x5eefb68`, period in `this+0xd4`), not by the GPIO.
   For j71t (no such property) the flag is 0: interrupts stay enabled and the
   handler runs the full path.
3. Interrupt enable/disable is *not* an MMIO write in the Mesa driver: it is
   `AppleSPIBiometricSensor::enableInterrupts(bool)` `0x5eefac4`, which logs
   `"%s: AppleSPIBiometricSensor::enableInterrupts true|false"` **[V-code,
   strings 0x5493d08/0x5493d3b]** and calls `[this->0xb0 vtable + 0xb8]`
   (enable) or `[+0xb0]` (disable) — index 23 / 22 of an
   `IOInterruptEventSource`. `this+0xb0` is the event source, `this+0xd4` the
   polling interval (`_setPollingInterval` at `0x5eefb68`).
4. `sendDeferredCommands` is called with the current pending command and only
   *before* the poll, so a deferred pause takes effect at the next IRQ
   boundary, never in the middle of a capture read.

### 2.2 Deferred commands (the "pause during imaging" mechanism) `0xfffffff005efd41c`

**[V-code]**:

```
log "%s: entry: _currentState=%d, _cmd=0x%x"                        ; [0x54983b4]
if (_currentState == 3) {                     ; 3 = imaging/capturing
    pendingCmd = (this->0x148) ? this->0x148->0x24 : 0xFFFF
    if (pendingCmd == this->_lastCmd) { *out = 0; return 0 }   ; already done
}
switch (pendingCmd) {
  case 0x0a:  log "%s: Put the sensor in Paused mode during imaging"  ; [0x549841e]
              send(this, "80 41 00 07 00 00 00", 7, verbose)          ; 0x07004180
              this->_lastCmd = pendingCmd; break
  case 0x01:  log "%s: Idle the sensor in imaging"                    ; [0x54983db]
              send(this, "80 30 00 07 00 00 00", 7, verbose)          ; 0x07003080
              this->_lastCmd = pendingCmd; break
  case 0x02:  (Sleep) → 0x5efd5f0
  default:    "Deferred idle command sent. Dropping image"            ; [0x549851e]
}
```

The two 7-byte frames are built as immediates into the stack
(`mov w8,#0x3080; movk w8,#0x700,lsl16` = `80 30 00 07`; `0x4180` = `80 41 00 07`),
i.e. **exactly** the `PauseCapture`/`Idle` frames of the kCMD table, sent
through the same `send()` path but *bypassing* `handleCMD`, and the last-command
byte is updated so `_currentState == 3` deferral terminates. This matches the
trace line `sendDeferredCommands: Put the sensor in Paused mode during imaging`
**[V-trace]**.

### 2.3 Status poll — `getSensorStatus` `0xfffffff005ef3024`

**[V-code]**:

```
w_tx   = 0x07001080                    ; bytes 80 10 00 07 (+3 zero bytes)
len    = min(arg_len, 16)
rxlen  = len + 7
vla    = alloca(rxlen, 16)             ; 16-byte aligned
memset(vla, 0, rxlen)
send(this, &w_tx, 7, vla, rxlen, verbose)               ; 0x5ef02fc
memcpy(arg_out, vla + 7, min(arg_len, 16))              ; 0x5ef31f0
if (verbose) log "%s: Status={%#02x, ...x16}"           ; [0x5496fad]
on error → [this vtable+0x470](../../release/OMISSIONS.md)                     ; IOReporter/log
```

So one SPI transaction of **23 bytes** when `len == 16`: the 7-byte frame is
clocked out and echoed back (the sensor does not drive MISO for those bytes; the
trace shows 7 zero bytes), then 16 status bytes come back. The status is copied
out of the VLA into the caller's buffer; the retry wrapper passes `this+0x159`,
so the field-16 cache is written on every poll, and the wrapper validates it
with `isSensorStatusValid` `0x5ef0c00` (`80 / 20 / 10` header, with the
`mesa-full-status` relaxation on byte 1). Both facts are already in
`mesa-driver-re.md` §5.2 and were re-confirmed here.

### 2.4 Retry policy — `getSensorStatusWithRetry` `0xfffffff005efafc8`

**[V-code]**:

```
n = 1
loop:
  err = getSensorStatus(this, this+0x159, 0x10, verbose)
  if (!err && isSensorStatusValid(this, this+0x159)) return 0
  if (n > maxRetries) return 0xE00002CA
  log "%s: Bad sensor status, Retrying..."            ; [0x5497063]
  n++
```

`handleInterrupt` calls it with `maxRetries = (lastCmd == 0x18)`, i.e. **0
retries normally, 1 retry only after the long-reset command**. The 3-try loop
seen at boot belongs to `AppleMesa::start`, not to this path. The Linux slice
must therefore not inherit a "retry 3 times per IRQ" behaviour.

### 2.5 Capture read — `readSensorData` `0xfffffff005efb0f8`

**[V-code]**, rebuilt from the disassembly rather than the earlier summary:

```
if (verbose) log "%s: entry"                                        ; "readSensorData"
u64 hdr = 0x0B001380                     ; bytes 80 13 00 0b | 00 00 00 (7)
u32 size;
u8  dummy = 0;
assert(size > 0 && size <= 65536)        ; [0x54929b6], error 0xE00002C2
buf_va = [this->0x298 vtable+0x150](../../release/OMISSIONS.md)   ; getBytesNoCopy(0,size)
log "%s: Put sensor in Read Data mode"                              ; [0x54970df]
err = send(this, &hdr, 11, NULL, 0, 1)   ; 0x5ef02fc → transferData
if (!err) send(this, /*tx=*/&dummy, /*txlen=*/1,
                     /*rxoff=*/0, /*rxmd=*/this->0x298, /*rxbuf=*/buf_va,
                     /*rxlen=*/size, /*flag=*/1)     ; 0x5eefe70
```

Confirmed constants: request header `80 13 00 0b` + three zero bytes + u32 LE
size (11 bytes total, `0x0B001380` is the first 8 bytes as one immediate); the
size gate is `0 < size <= 0x10000` with the literal assertion string
`size > 0 && size <= 65536`; the frame buffer is the driver's single
`IOBufferMemoryDescriptor` at `this+0x298`, and the RX pointer comes from
`getBytesNoCopy(0, size)` **before** the request is sent (so the mapping exists
across both transactions).

### 2.6 Transport — `send`/`transferData` `0x5ef02fc` / `0x5eefe70`

**[V-code]**:

```
transferData(this, tx_md, tx_buf, tx_len, rx_off, rx_md, rx_buf, rx_len, flag)
    if (this->0xb8 == 2) { x21 = 0; goto AOP_ONLY }        ; AOP-only product
    if (!this->0x90) return 0xE00002BD
    if (verbose) log "%s: transferData: txBufferLength=%u rxBufferLength=%u"  ; "transferData"
    ... buffer sanity for tx and rx (helper 0x5f08b34, kind 3) ...
    [this->0x90 vtable+0x558](../../release/OMISSIONS.md)
AOP_ONLY:
    if ((this->0xb8 - 1) <= 1) [this->0xc0 vtable+0x540](../../release/OMISSIONS.md)
    if (verbose) memcpy first 0xC0 bytes; log "Wrote: %5zuB | %s" / "Read: %5zuB | %s"
```

The `Wrote:`/`Read:` lines in the capture are emitted **after** the transport
call returns, which is what makes the timing in §5.1 readable. The transfer
object (`this+0x90`) is the SPI transport; `this+0xb8` is a "which transports
are live" mode where `2` means AOP-only and `1`/`0` mean SPI (+AOP mirror).

### 2.7 Transport configuration (frequency / phase / bit depth)

**[V-code]** `0xfffffff005ef9754` (inside `AppleMesa::start`'s property parse):

```
v = provider->getProperty("spi-freq")->value32();      ; [0x54927ed]
if (v) { this->0x248 = v; this->0x21c = 1000000000 / v;   ; ns per word
         [this->0x90 vtable+0x550]();
         this->setProperty("spi-freq", v, 32); }
v = provider->getProperty("spi-clock-phase")->value32();   ; [0x54927f6]
if (v) { this->0x221 = v; [this->0x90 vtable+0x550]();
         this->setProperty("spi-clock-phase", v, 32); }
this->0x245 = !!provider->getProperty("skipFingerDetect")->value32();
if (provider->getProperty("16BitMode")->value32()) {
     this->0x246 = 1; this->0x23c = this->0x124;  handleCMDSync(0x0b, 0x10);   /* 19668 */
} else {
     this->0x246 = 0; this->0x23c = this->0x120;  handleCMDSync(0x0b, 0x08);   /* 11924 */
}
this->0x247 = !!provider->getProperty("reportStatusMessages")->value32();
```

and the boot-arg overrides `mesa-spi-freq` (`0x5492857`, read at `0x5efa0ac`)
and `mesa-spi-freq`/`spi-clock-phase` re-read at `0x5efa13c`.

Consequences:

- **No per-transfer clock or mode change exists in the Mesa code.** Frequency,
  phase and bit depth are decided **once**, before the first command, from the
  provider properties (which for j71t come from the ADT `spi-frequency` =
  8,000,000) plus two boot-args. Everything afterwards — poll, capture request,
  19,668-byte read, pause — runs at that same setting. `vtable+0x550` is called
  immediately before each property is applied; it is the transport's
  "reprogram from my fields" hook (exact body not decoded, **[U]**).
- `this+0x23c` (expected data size) and `this+0x124`/`this+0x120` (max sizes at
  16-bit / 8-bit) are **bit-depth dependent**, so 19,668 is the *16-bit* frame
  size; a product without `16BitMode` would expect 11,924.
- The `0x0b` (`SetImageBitDepth`) command is issued here, at start, and is the
  only kCMD in this function: bit depth is not per-frame.

---

## 3. Frame handoff: from `AppleMesa` to `AppleMesaSEPDriver`

### 3.1 Where the frame leaves `AppleMesa`

**[V-code]** `handleStateMachine` `0xfffffff005efdc94` (reached only through two
vtable slots, `__PLK_DATA_CONST 0x6e21088` and `0x6e228d8`; no `bl` reaches it):

```
w1 = this->_lastCmd (this+0x154)
if (w1 <= 0x1f) { jump table 0x5eff6ec[w1]; w24 &= 0x70 }   ; kCMD completion switch
...
if (driverState > 3) {                                       ; 3 = pre-capture, 4 = imaging
    size = status.u32@+12
    if (size) {
        err = readSensorData(this, size)                     ; sole caller, 0x5efe060
        va  = [this->0x298 vtable+0x150](../../release/OMISSIONS.md)   ; getBytesNoCopy again
    } else { va = 0 }
} else if (size < this->0x124 && size < this->0x120)
    log "%s: Not enough data to read in capture mode(%u)"    ; [0x54984ea]
```

The data-available branch re-derives the buffer pointer (it is not cached from
`readSensorData`), then dispatches through the vtable slot `+0x710` with
`(this, x22, va, len)`; `x22` is the per-state context object. That slot is
**[I]** the `AppleMesa → client` callback implemented by the SEP-side
`AppleMesaShim` (the trace shows `AppleMesaSEPDriver` receiving an IOMD and
calling `getCaptureBufferFromIOMD` on it), i.e. the frame is handed over as
**(buffer VA, length)** in the same 64 KiB IOMD, not copied.

### 3.2 `getCaptureBufferFromIOMD` `0xfffffff0066ae010` — buffer ownership and type gate

**[V-code]**:

```
log "%s <- inMD:%p, outBuffer:%p"                          ; [0x56859e1]
err = [inMD vtable+0xd8](../../release/OMISSIONS.md)        ; prepare/map, direction 0
va  = [inMD vtable+0xe8](../../release/OMISSIONS.md)        ; getBytesNoCopy / mapping
obj = [va vtable+0x78]()                 ; wrapped handle (see below)
len = [inMD vtable+0x88](../../release/OMISSIONS.md)        ; getLength
switch (this->0x204) {                   ; sensor type
  case 2: case 3: assert len == 0x7200 (29184) "dataSize == (12 * sizeof(sensor96112_slice16_t))"
  case 1:         assert len == 0x4CD4 (19668) "dataSize == (11 * sizeof(navajo_slice16_t))"   [0x5682491]
  case 4:         len == 0x7068 ...
}
```

So the frame is **11 slices of 1,788 bytes** (`19668 = 11 × 1788`) for the Mesa
("navajo") sensor; the length is asserted, not negotiated. The captured IOMD is
what `AppleMesa` allocated once at start:

**[V-code]** `0x5ef13c8`: `this->0x298 = IOBufferMemoryDescriptorLike(3, 0x10000, 1)`
— one **65,536-byte** buffer, page-mapped, alignment argument **1**; length is
re-set per frame via `setLength`-style `vtable+0x150`. There is exactly one
frame buffer per driver instance, so a second frame can only be read after the
first has been consumed (that is what the "image consumed" ack and the queue
full check enforce, §4.4).

### 3.3 `handleCapture` `0xfffffff0066b67bc` and `handleCaptureData` `0xfffffff0066b6b6c`

**[V-code]** `handleCapture(inCapture)`:

```
log "handleCapture <- inCapture:%p (_sensorOpen:%d)"        ; [0x56865a6]
if (!this->0x1e8 /*_mesa*/)              return 0xE00002C9
if (!*(this+0x1c34) /*_sensorOpen*/)     return 0
if (!*(this+0x1c34+0xf7f) /*sensor idle + patch loaded*/) {
    [this userClientNotify](../../release/OMISSIONS.md)   ; "inCapture failed. Not in kSensorIdle or patch not loaded."  [0x549aeef]
    if (still not armed) → error path 0x66b6a90
}
err = getCaptureBufferFromIOMD(this, [inCapture], &buf)      ; §3.2
obj = wrap(this->0x1e8, buf)  ; 0x669f3c4
err = handleCaptureData(this, obj)                            ; 0x66b6b6c
release obj; release buf
```

`handleCaptureData(captureData)`:

```
log "%s <- captureData:%p (type=%u, source=%p)"              ; [0x5686629]
type = [captureData vtable+0x78]()
if (type != 1) → 0x66b7330                                   ; non-Mesa image
if (this->0x1c40 != [captureData vtable+0x80]()) return 0     ; source mismatch → drop
→ 0x66b6d1c ... [this->0x1c20 vtable+0xa8]() ...
  if (queue full) log "%s: *** Image queue full ***"          ; [0x56866d2], frame dropped
  asyncCaptureHandler(this->0x1c20, 0x9c4 /*init*/, 1)        ; 0x669f8a0 dispatcher
```

`captureData` is a separate object with a small interface: `+0x78` type,
`+0x80` source, `+0x88` variant, `+0x90`/`+0x98` source descriptors,
`+0x18` timestamp (u64), `+0x20` "charger on" byte, `+0x28` the underlying
IOMD. The class names are visible in the kext (`IOMesaCaptureData`,
`IOMesaCaptureDataLocal`, `IOMesaCaptureDataQueue` metaclass ctors at
`0x669f9e4`/`0x669fa1c`/`0x669fa44`).

---

## 4. The relay: `0x23 → 0x06 → 0x65 → 0x07` (per frame)

### 4.1 Ordering is code, not trace noise

**[V-code]** the builder at `0x66a0d64` performs, in this order, four
vtable-dispatched transactions through `[this vtable+0x6a0]` (the Mesa driver's
`sepTransact(cmd, in, inlen, out, outlen, lock, flags)`):

```
sepTransact(this, 0x23, NULL, 0, NULL, NULL, 0, 0)        ; 0x66a1110..0x66a112c
sepTransact(this, 0x06, payload, payloadLen, NULL, NULL)  ; 0x66a1144..0x66a115c
   ... if ([captureData vtable+0x78]() == 1) {
sepTransact(this, 0x65, [md vtable+0xd8](), [md vtable+0xa0](), NULL, NULL)
sepTransact(this, 0x07, NULL, 0, &out192, &outLen)        ; out buffer = 192 bytes
       }
```

`0x07`'s output buffer is a **192-byte stack buffer** (`add x8, sp, #0x88`), and
the reply in the capture is exactly `0: 192 = 0 + 192` **[V-trace]**. The `0x65`
payload pointer is `[md vtable+0xd8]()` on the capture's IOMD and the length
`[md vtable+0xa0]()` — i.e. **19,668 comes from the buffer, not from a
constant**; the only place the number is hard-coded is the assert in §3.2.

Between `0x65` and `0x07` the driver records `this->0x2a0 = now()` and
`this->0x2c0 = captureData+0x18`, then calls `0x66ad5b8(1)` (set "image in
flight"), and after `0x07` returns calls `0x66ad5b8(0)`. That in-flight flag is
what the next `handleCapture` (and `sendDeferredCommands` on the sensor side)
observes.

### 4.2 The `0x06` payload — 151 bytes, recovered field by field

**[V-code]** the same function builds it. `payloadLen = 0x97 (151)` plus
optional trailing blocks:

```
if ((flags & 0x12) && this->0x1d84)      payloadLen = 0x97 + this->0x1d84
if ((flags & 0x4000) && this->0x1da0 && this->0x1da8 > 3)
                                         payloadLen += this->0x1da8
buf = alloc(payloadLen)                                     ; 0x66bbfc8
```

Field stores, in program order (offsets are byte offsets into the payload; the
log line that follows them names the fields, which is how the mapping is
anchored — `"%s: kImageProcessingInit, chargerOn=%d, firstImage=%d,
lastImageEnroll=%d,lastImageMatch=%d, postponeMatchProcessing=%d,
matchFlags=%u"` at `0x549678b`, emitted at `0x66a10c8`):

| off | size | value (from code) | log field |
|---:|---:|---|---|
| `0x00` | u32 | `this+0x1d48` | `matchFlags` |
| `0x04` | u8 | 1 if (`this+0x117` \|\| `this+0x118`) && `(flags & 0x109) == 0` else 0 | `postponeMatchProcessing` |
| `0x05` | u8 | `captureData+0x20` | `chargerOn` |
| `0x07` | u8 | 1 if `[captureData vtable+0x88]() == 0` | `firstImage` |
| `0x08` | u8 | 1 if `[captureData vtable+0x88]() == 4` | `lastImageEnroll` |
| `0x09` | u8 | 1 if `[captureData vtable+0x88]() == 4` | `lastImageMatch` |
| `0x12` | u8 | 1 if `mode() == 2` (match) | — |
| `0x13` | u8 | 1 if `mode() == 3` (enrol) | — |
| `0x40` | u64 | `captureData+0x18` (frame timestamp) | — |
| `0x48` | u32 | `captureData->type` | — |
| `0x4c` | u8 | `[captureData vtable+0x98]()` | — |
| `0x4d` | u8 | `[captureData vtable+0x90]()` | — |
| `0x4f` | 24 | filled by `0x66b76dc(this, &buf[0x4f], captureData)` | — |
| `0x67` | u32 | `this+0x1d4c`, or **501** when that is `-1` | userID |
| `0x6b` | 16 | `this+0x1d50` (only when `flags & 2`) | — |
| `0x7b` | u32 | `this+0x1d60` (only when `flags & 2`) | — |
| `0x7f` | u32 | `this+0x1d98` (only when `flags & 0x10`) | — |
| `0x83` | 16 | `this+0x1d88` — **only when the count is 0** | — |
| `0x93` | u32 | count `this+0x1d84` (enumeration/template count) | — |
| `0x97` | var | `this+0x1d64`, count bytes — present when the count is non-zero | — |

`501` is the mobile user id seen in every SEP notification of the capture
(`info = 501`) **[V-trace]**, so field `0x67` is a user id **[I]**; the rest of
the named fields are exact because the log prints the same bytes it just wrote.

The 24 bytes at `0x4f..0x66` are now decoded (Ghidra decompile of `0x66b76dc`
after the server came back):

```
if (cd->type == 1)      *(u32*)&buf[0x4f] = 1;
else if (cd->type == 0) *(u32*)&buf[0x4f] = 0;
else                    return 0;                  /* leaves the field unset */
uuid_clear(&buf[0x53]);                            /* 16 zero bytes */
/* buf[0x63..0x66] is never written: 4 bytes of alignment gap before +0x67 */
```

so `+0x4f` is a "this is a Mesa/navajo-class image" flag, `+0x53..0x62` a cleared
UUID, and `+0x63..0x66` padding.

**Still not recovered:** the semantics of the `0x6b`/`0x83` blocks (`this+0x1d50`
16 B, `this+0x1d88` 16 B) and of the `+0x93` count/`+0x97` tail. A Linux relay
that sends a guessed `0x06` payload therefore remains unjustifiable; what is now
established is the **complete 151-byte layout** except those optional blocks,
including every field the log names.

After building, the driver **copies** the payload into a heap record
(`this+0x1db0`, length `this+0x1db8`) so the pointer handed to `0x06` stays
valid while the next frame arrives.

### 4.3 What `0x23` is

**[V-code]** `0x23` is sent with `in = NULL, inlen = 0, out = NULL` from the
same function that builds `0x06`, always first, and its result is checked
(`cbnz w0 → error path 0x66a1548`). It carries no payload and no reply payload
**[V-trace]**. It is a per-frame "begin" handshake on the SBIO endpoint, issued
by the *same* code path that knows the frame type — not a user-client command.
The older guess "SKS lock-state query" is therefore unlikely; the trace shows it
interleaved with every frame, including while the bag was unlocked.

### 4.4 Result handling and the "next action"

**[V-code / V-trace]**

- `0x07` reply (192 B) → `kImageProceessed` log at `0x66a18d4` with
  `err, imageForEnrolling, imageForMatching, assesment, feedback, purpose,
  contrast...` → `cacheCaptureData` → `handleEnrollment` (or match) → this is
  what emits the "image consumed" path and the 0x22 ack.
- `0x08` (0 in / 3224 out) is issued from `checkSensorState` →
  `sensorStateHandler` and is what carries the per-user unlock/status blob
  **[V-trace]**.
- `0x22` (0 in / 0 out) follows when the image is consumed **[V-trace]**; **[I]**
  it is the release for the single frame buffer of §3.2.
- Queue: `[this+0x1c20 vtable+0xa8]()` gates admission; overflow logs
  `*** Image queue full ***` and drops the frame instead of blocking the sensor.

### 4.5 The match-side variants (same primitives, different order)

**[V-code]** two more functions were decompiled with the restarted Ghidra server;
they are the match path and they reuse the enrolment-time `0x06` payload.

`retryMatchAfterHomeButtonPress` `0xfffffff0066b9944`:

```
dequeue image on [driver->0x1c20 vtable+0x90](../../release/OMISSIONS.md) until empty:
  if (cd->type == 1 && driver->0x204 != 1) {
      sepTransact(0x65, md_ptr, md_len)              ; replay the frame
      sepTransact(0x3e, NULL, 0, &out, &outLen)      ; outLen must be 6
      capture->0x34 = out[1]; capture->0x38 = *(float*)(out+2)
  }
  p = IOMalloc(driver->0x1db8); memcpy(p, driver->0x1db0, len)  ; cached 0x06 payload
  p->0x4d = cd->vtable[0x90](); p->0x63 = cd->vtable[0xa0]()
  if (driver->0x204 != 1 && previous out) p->0x4e = previous_out[0]
  p->0x07 = 1; p->0x09 = 1; p->0x40 = cd->+0x18; p->0x48 = cd->type
  sepTransact(0x06, p, len)
  if (cd->type == 1 && driver->0x204 == 1)
      sepTransact(0x65, md_ptr, md_len)              ; the j71t branch
  requestPowerAssertion(true); t0 = mach_absolute_time()
  sepTransact(0x07, NULL, 0, &out192, &outLen)
  t1 = mach_absolute_time(); requestPowerAssertion(false)
  ... sepTransact(0x09, NULL, 0, &out, &outLen)      ; 3234-byte match blob
sepTransact(0x0c, NULL, 0, NULL, NULL)               ; Cancel, always at the end
```

so for j71t the retry flow is `0x65 → 0x3e → 0x06 → 0x65 → 0x07 → 0x09 → 0x0c`,
not the enrolment order. Note it **reuses the stored 151-byte payload** and only
overwrites `+0x4d`, `+0x4e`, `+0x63`, `+0x07/0x09`, `+0x40`, `+0x48` — direct
evidence that the payload is a per-image record whose base fields come from the
last init and whose identity/timestamp fields are refreshed per frame.

`applyPostponedMatch` `0xfffffff0066b92f0`:

```
flags = driver->0x1d48 (matchFlags)
len = 0xc; if (flags & 8) len += driver->0x1d84
           if (flags & 0x4000) { require driver->0x1da0 && driver->0x1da8 >= 4;
                                 len += driver->0x1da8 }
p = IOMallocZero(len); p[0] = rev64(flags); [p+4] = count; memcpy template bytes
sepTransact(0x0a, p, len, &out, &outLen)          ; out must be 0x1c (28) bytes
     if out[0] == 1 → build the 0xc9c-byte match result and notify
                      [userClient 0xe3ff800b] with 0x1d bytes
```

Two things fall out of this: (a) the same flag word (`& 0x109`, bit 3, bit 14 =
`0x4000`) gates the envelope in three different commands — it is the driver's
"what is attached to this image" mask, not a per-command option; (b) `0x0a` here
is a *host-to-SEP payload* with a 28-byte reply, which is a second meaning for
the number `0x0a` (in the Mesa kCMD space `0x0a` is PauseCapture, which is a
sensor SPI frame and never a SEP opcode) — the same decimal/hex namespace trap
already documented in `sbio-runtime-re.md` §8.3.

These are state-dependent branches, not a fixed per-frame suffix. Across the
20 captured common prefixes, `0x08` appears 8 times, `0x09` 12 times, `0x1f`
9 times, and `0x22` 18 times. Linux must select the next action from the
decoded `0x07` result plus async state; it must not blindly send
`0x08 → 0x22` after every frame.

---

## 5. SPI transactions, byte for byte

### 5.1 The three transactions that matter per frame

**[V-trace]** (log timestamps from the first enrolment frame) and **[V-code]**
(the frames are built as immediates in the functions named):

| # | function | direction | len | bytes | duration |
|---:|---|---|---:|---|---|
| 1 | `getSensorStatus` `0x5ef3024` | tx | 7 | `80 10 00 07 00 00 00` | 0.11 ms |
| 1 | | rx | 23 | 7×`00`, then the 16-byte status starting `80 20 00 10 00 00 00` | |
| 2 | `readSensorData` `0x5efb0f8` | tx | 11 | `80 13 00 0b 00 00 00 <u32 size>` | 0.07 ms |
| 3 | `transferData` `0x5eefe70` | tx | 1 | `00` | **20.8 ms** total |
| 3 | | rx | 19668 | `ed fc 06 00 3a 80 ...` | |

Transaction 1 and 2 are separate transport calls, i.e. separate SPI messages
(CS re-arbitrated between them; **[I]** at the controller level).
Transaction 3 is **one** transport call with `tx_len = 1`, `rx_len = 19668` —
not a "1-byte write" followed by a separate read. The 20.8 ms gap in the log is
the wire time of 19,668 bytes at the programmed clock (**19668 × 8 / 7.69 MHz =
20.45 ms**), not a sensor fetch delay or an `IOSleep`; both `Wrote:` and `Read:`
are printed after the call returns, 87 µs apart.

Linux consequence: one `spi_message` with a single `spi_transfer` of
`len = 19668`, `tx_buf = NULL` (or a zeroed buffer) and `rx_buf = frame`,
`speed_hz = 8 MHz`, `cs_change = 0`. Do **not** split it into a 1-byte write
plus a read: that deasserts CS mid-frame.

MOSI content after the first byte is unconstrained by the protocol (the sensor
drives MISO); the trace cannot show it because only the first 0xC0 bytes are
hex-dumped. **[U]** but irrelevant for an RX-only Linux transfer.

### 5.2 Command frames used by the relay's surrounding logic

All built as immediates or as `u32 | (len << 32)` in `handleCMDSync`
`0x5ef9aa8`; the first four bytes are the "frame word", the u32 at `+7` is the
payload length when a command carries data:

| purpose | frame (first 4 B) | source |
|---|---|---|
| status poll | `80 10 00 07` | `0x5ef30a8` (`0x07001080`) |
| read data | `80 13 00 0b` + u32 len | `0x5efb160` (`0x0B001380`) |
| start capture | `80 40 00 07` | trace, `handleCMD` kCMD 6 |
| start capture (HV boost) | `80 42 00 07` | `handleCMD`, `this+0x37c == 1` |
| pause capture | `80 41 00 07` | `0x5efd5ac` (`0x07004180`) |
| idle (deferred, in imaging) | `80 30 00 07` | `0x5efd544` (`0x07003080`) |
| set image bit depth | `80 0b 00 <len+7>` | `handleCMDSync(0x0b, 8\|16)` |

### 5.3 Chip select, clock, mode

- **CS**: controller-internal (ADT `function-spi_cs0` = `"null"`), asserted for
  the whole transaction; the Mesa code never toggles a CS GPIO (there is no
  `function-mesa-cs`), so a Linux message with one transfer and
  `cs_change = 0` matches.
- **Clock**: 8,000,000 Hz from the ADT, programmed once (§2.7). The Samsung v1
  block divides the 61,538,461 Hz `spi2_nclk` by 8 → 7,692,307 Hz effective.
- **Mode**: `spi-clock-phase` is absent from j71t's ADT; the code only applies
  it *if present*, and otherwise leaves the provider's default (mode 0, the same
  framing the working audio and touch clients use). There is no runtime
  fallback to mode 3 in the Mesa code.

---

## 6. Async SEP notifications and what they trigger

**[V-code / V-trace]** — three distinct mechanisms, do not conflate:

1. **GenericTransfer completion** (`tag 0xfc/0xfd/0xfe/0xff`): completes the
   transfer object, wakes the waiter, `0x65` continuation credit is `0xfe`.
   This is what paces the two `0x65` chunks (16,356 then 3,312).
2. **Service notification** (`tag 1/2/3`, no payload in the mailbox):
   `AppleMesaSEPDriver::handleSEPMessage` logs
   `Got message from SEP, request = %u, length = %u, info = %u`; tag 3 →
   `bgOpsResultReadyHandler → checkBgOpsResult → sepTransact(0x21, 0 in,
   3214 out)`; tags 1/2 are keybag/template messages for user 501. Bit layout:
   `ep[7:0] tag[15:8] length[31:16] info[47:32]`.
3. **Local state machine** (`kCMD` completion + `sensorStateHandler`): the *next
   sensor* action is chosen here, not by the SEP. After the relay the SEP driver
   calls `checkSensorState` → `sensorStateHandler` → `pauseCapture` →
   `PauseCaptureGated` → `handleCMDSync(0x0a)`, which is what produces the
   sensor-side `80 41` (deferred if a frame is in flight, §2.2).

So the dependency runs **sensor → SEP → sensor**, with the SEP side acting as a
sink that answers late (41 ms for `0x07`, 14 ms for `0x08`): while the SEP is
processing, the sensor keeps streaming, and the driver's gates (in-flight flag,
image queue, the `X162Bypass` poll mode, deferred pause) are exactly what keep
the single 64 KiB frame buffer consistent.

---

## 7. Error and recovery paths (all evidenced)

| condition | detection | action |
|---|---|---|
| invalid status header | `isSensorStatusValid` `0x5ef0c00` | retry per §2.4, then `0xE00002CA` |
| status poll error | `getSensorStatus` ≠ 0 | `FIRE_ESD_RECOVERY, reason=%d`, set `+0x2f2`/`+0x318`, notify via `this+0x308` |
| capture size 0 or > 65536 | `readSensorData` assert | `0xE00002C2`, no transfer |
| data-ready but too small for the mode | `size < this+0x124 && size < this+0x120` | log `Not enough data to read in capture mode(%u)`, skip read |
| capture not armed / not idle / patch missing | `handleCapture` `this+0x1c34+0xf7f` | userClient notify `0xE3FF8001`, sub-code `0x47`/`0x48`, no relay |
| queue full | `[this+0x1c20 vtable+0xa8]()` | log `*** Image queue full ***`, drop frame |
| `0x06`/`0x65`/`0x07` error | each `cbnz w0` | abort this frame; `0x66ad5b8(0)` clears in-flight; unwinds the payload record |
| deferred pause while imaging | `sendDeferredCommands` | frame first, `80 41` at the next IRQ |
| SEP transfer timeout | GT layer | 10 s lock + 10 s completion (§`sbio-runtime-re.md`) |

Nothing in this path retries a *capture* automatically: a failed frame is
dropped and only the sensor's own streaming produces the next `state 7`.

---

## 8. Address index for this document (all **[V-code]**)

Sensor / `AppleBiometricSensor.kext` (`__PLK_TEXT_EXEC`):

| address | role |
|---|---|
| `0xfffffff005eefe70` | `transferData` (SPI/AOP split, `Wrote:`/`Read:` logs) |
| `0xfffffff005ef02fc` | `send()` wrapper → `transferData(tx, len, 0, 0, verbose)` |
| `0xfffffff005ef3024` | `getSensorStatus` (7B → 7+len) |
| `0xfffffff005ef0c00` | `isSensorStatusValid` |
| `0xfffffff005efafc8` | `getSensorStatusWithRetry` |
| `0xfffffff005efb0f8` | `readSensorData` (11B + 1B/len) |
| `0xfffffff005efd20c` | `handleInterrupt` |
| `0xfffffff005efd41c` | `sendDeferredCommands` |
| `0xfffffff005efdc94` | `handleStateMachine` (vtable-only, slots `0x6e21088`, `0x6e228d8`) |
| `0xfffffff005ef9aa8` | `handleCMDSync` (kCMD → frame table, `waitFor`) |
| `0xfffffff005eefac4` | `AppleSPIBiometricSensor::enableInterrupts` |
| `0xfffffff005efa924` | `waitFor(predicate, useDeadline)` (1 s `commandSleep`) |
| `0xfffffff005ef0a38` / `0x5ef0a58` | the two wait predicates (`+0x215/+0x213`, `+0x214`) |
| `0xfffffff005ef927c` | state-byte setter + `commandWakeup` |
| `0xfffffff005ef9754` | property parse (`spi-freq`, `spi-clock-phase`, `16BitMode`) |
| `0xfffffff005ef13c8` | 64 KiB frame IOMD allocation → `this+0x298` |
| `0xfffffff005ef0ec8` / `0x5ef3394` / `0x5ef37e4` | start / powerOnReset / setSensorPower |

SEP / `IOBiometricFamily.kext`:

| address | role |
|---|---|
| `0xfffffff0066b67bc` | `AppleMesaSEPDriver::handleCapture` |
| `0xfffffff0066ae010` | `getCaptureBufferFromIOMD` (type/length asserts) |
| `0xfffffff0066b6b6c` | `handleCaptureData` (type 1, source match, queue) |
| `0xfffffff00669f8a0` | `asyncCaptureHandler` dispatcher stub |
| `0xfffffff0066a0d64` | per-frame relay builder (`0x23`→`0x06`→`0x65`→`0x07`) and `0x06` payload |
| `0xfffffff0066a10c8` / `0x66a18d4` | `kImageProcessingInit` / `kImageProceessed` log sites |
| `0xfffffff0066b76dc` | fills payload `+0x4f..+0x66` |
| `0xfffffff0066bc0dc` | current command mode (2 match / 3 enrol) |
| `0xfffffff0066bb114` | `initSbioCommunication` (`0x73`, 4-byte 1) |
| `0xfffffff0066bc214` | low-level transact helper |
| `0xfffffff0066b61ac` | userClient notify (`0xE3FF8001`, …) |
| `0xfffffff0066bbfc8` / `0x66bbfb0` / `0x66bbf5c` / `0x66bc34c` | alloc / free / release / memcpy helpers |
| `0xfffffff0066b9944` | `retryMatchAfterHomeButtonPress` (`0x65`→`0x3e`→`0x06`→`0x65`→`0x07`→`0x09`→`0x0c`) |
| `0xfffffff0066b92f0` | `applyPostponedMatch` (`0x0a` envelope, 28-byte reply, notify `0xe3ff800b`) |
| `0xfffffff0066b76dc` | `0x06` payload `+0x4f` fixup (type flag + cleared UUID) |

Kept from earlier passes (unchanged, re-checked): `AppleSEPGenericTransfer::transfer`
`0xfffffff006690140`, `buildSendChunk` `0xfffffff00668e2b4`, `sendChunkMailbox`
`0xfffffff00668e094`, `handleChunk` `0xfffffff00668eaa4`.

**Bounded negative searches performed this pass** (read-only, whole
`__PLK_TEXT_EXEC` + `__PRELINK_TEXT`, 5.1 M instructions indexed):

- no delay call in `readSensorData` (whole function) or `transferData` (whole
  SPI branch): the only stubs the latter calls are log `0x5f08c6c`, memcpy
  `0x5f086b0`, buffer-check `0x5f08b34` and stack-check `0x5f08c60`. The 20.8 ms
  is wire time, and the timing math says the same;
- `this+0x298` is written exactly once (`0x5ef13d8`, at start) and only cleared
  in teardown (`0x5ef8c14`) — one frame buffer, never reallocated per frame;
- no per-transfer clock/phase setter: `spi-freq`/`spi-clock-phase` are read only
  in `0x5ef9754` and the boot-arg sites `0x5efa038/0x5efa0ac/0x5efa13c`;
- no `0x65` length constant anywhere in the Mesa kext; the only 19668
  occurrences in the relay path are the two asserts of §3.2;
- `0x65` is emitted from **two** functions, three call sites: the per-frame
  builder above (`0x66a11d8`, after `0x23`+`0x06`) and
  `retryMatchAfterHomeButtonPress` `0x66b9944` (`0x66b9ad4` before its `0x3e`
  probe, `0x66b9ef4` after its re-sent `0x06`). The enrolment capture on j71t
  used the first for all 20 frames; the retry flow is the match path (§4.5);
- `handleStateMachine` has no `bl` callers (vtable-only), so a Linux port must
  model it as "the poll completion callback", not as a called function.

---

## 9. Corrections to the earlier documents

1. **`mesa-driver-re.md` §5.3 / `sbio-commands.md` §5**: "…~21 ms
   sensor-internal delay… second write is a tx=1/rx=size transaction" is right
   about the transaction but wrong about the cause. The 20.8 ms is the SPI wire
   time of 19,668 bytes at 7.69 MHz; there is no delay and no second
   "dummy-write" transaction. The `Wrote: 1B | 00` line is the MOSI byte of the
   same full-duplex transfer.
2. **`mesa-driver-re.md` §12 item 1 (`U`)**: the IRQ-type question is unchanged,
   but the *enable/disable* mechanism is now known: `enableInterrupts()` is an
   `IOInterruptEventSource` enable/disable (`0x5eefac4`, vtable indices 22/23),
   not an MMIO mask, and it is toggled at start (by `X162Bypass`) and around
   power transitions, not per frame. `this+0x108` is a *product* flag
   (`X162Bypass`), not a per-frame gate — an earlier reading of this pass
   assumed a relay-protection gate and that was wrong.
3. **`mesa-driver-re.md` §5.2**: the status cache is a *driver field*
   (`this+0x159`, 16 bytes) written by every poll, including the snapshot path;
   `handleStateMachine` reads that cache, not a per-call buffer. Any Linux
   implementation that keeps the 16 bytes only on the stack cannot reproduce the
   "late state" behaviour seen in the trace (`Sensor was previously in Finger ON
   state`).
4. **`driver-notes.md` §7 note about `0x65`**: the two chunks are driven by the
   GT layer's `0xfe` credit, not by the Mesa driver; the Mesa side issues a
   single logical `0x65` with a 19,668-byte buffer.

---

## 10. Smallest read-only Linux slice after status hardware validation

The existing slice (`drivers/mesa/mesa-a9.c`, `mesa-driver-implementation.md`)
does exactly one 23-byte status transfer on demand. First validate that manual
status path with a board-local DT, PMU readback, one bounded power-on/poll/off,
and no IRQ property. Only after the iOS GPIO mode-3 specifier is translated to
one reviewed Linux trigger may the next **read-only** addition be
interrupt-driven status sampling with the iOS gating semantics. It touches
only the status opcode (`0x80 10 00 07`), i.e. the same opcode the current
driver already sends.

### 10.1 What to add

1. **IRQ → work item → one poll.** Register the threaded handler (already
   requested in probe with `IRQF_ONESHOT | IRQF_NO_AUTOEN`); the handler must do
   nothing but `disable_irq_nosync()` + `schedule_work()`. The work item takes
   `mesa->lock`, performs **one** 23-byte transfer, stores the 16 status bytes in
   `mesa->status` (a driver field, not a stack buffer — §9.3), re-enables the
   IRQ, and wakes any waiter. This mirrors `handleInterrupt` → one
   `getSensorStatusWithRetry` with `maxRetries = 0` (§2.4). *Deviation from iOS,
   deliberate:* iOS polls synchronously in the interrupt-event workloop; Linux
   must not run SPI in hard-IRQ context, so the poll moves to a work item. The
   observable property preserved is "exactly one poll per interrupt, no retry",
   which is what the trace shows.
2. **Status cache + decode.** Keep the 16 raw bytes plus decoded
   `state` (`+7`), `err` (`+8`), `flags` (`+9`), `event` (`+11`), `ctx`
   (`+12..15`, LE u32). Log transitions only; never log the raw frame tail
   (it is sensor state, and one day frame data).
3. **Poll-only mode (optional, mirrors `X162Bypass`).** Add
   `mesa->poll_only`, default 0. While set, the IRQ path samples and caches but
   does not drive any state transition, and the event source stays unarmed
   (debugfs `poll_only 0|1`). On j71t the ADT has no `X162Bypass`, so the flag
   must default to 0 and the driver must not invent the property.
4. **Bounded retry, exactly one rule.** Retry once **only** if the previous
   command byte was the long reset (`0x18`); otherwise report the raw status.
   Do not invent a 3-try loop; that belongs to `start`.
5. **No new opcodes.** The frame is built as the same 7-byte constant plus 16
   zero bytes of padding, one `spi_sync_transfer()`, `speed_hz = min(DT, 8 MHz)`,
   `cs_change = 0`.
6. **Counters, not actions.** `irq_count`, `poll_ok`, `poll_bad_header`,
   `poll_err`, `last_state`, `last_ctx`. No recovery, no power cycling, no
   reset frames in this slice: a bad status is reported raw, matching the
   existing "report it raw" boundary.

### 10.2 What must *not* be in it (and why)

| excluded | reason |
|---|---|
| `80 13 00 0b …` capture request | first write opcode in the frame path; belongs to the guarded S4 stage, and the frame cannot be interpreted without the sensor patch (§8 of `mesa-driver-re.md`) |
| `80 40`/`80 42` start, `80 41` pause, `80 30` idle, `80 31` sleep, `80 14` reset | sensor state changes; each needs its own stop-rule analysis |
| `0x0b` set-bit-depth, `0x08` host-int-depth, `0x16` encrypted params | parameter frames whose byte layout is not recovered (`mesa-driver-re.md` §13.7) |
| any patch load (`80 06` + upload) | the blob's provenance and persistence semantics are unresolved; guessed bytes are forbidden |
| provisioning / session key / lock-write / calibrate-NVM (`kCMD 0x0d–0x10`, `0x1b–0x1d`, `0x8b`, `0x8c`) | persist into sensor NVM, exactly as unrecoverable as syscfg |
| SEP relay (`0x23/0x06/0x65/0x07`) | needs `sbio`'s multi-chunk framing in `sep-a9` plus a real `0x06` payload; the 24 bytes at `+0x4f` are still unknown (§4.2) |
| any DMA/SIO path | status polls are 23 bytes; DMA is not needed and its endpoint numbering for `spi2` is still inferred |

### 10.3 Testable invariants for the slice (offline + one device run)

- built object has exactly one `spi_sync` symbol and contains the byte string
  `80 10 00 07` once;
- `grep` of the slice's C for `0x13`, `0x40`, `0x41`, `0x0d`…`0x1d`, `0x8b`,
  `0x8c` returns nothing (a mechanical proof that no write opcode slipped in);
- with `mesa.enable=0`: zero probes, zero IRQs, zero transfers;
- with `enable=1` and the sensor powered: N IRQs produce exactly N polls, each
  `23` bytes, each with a valid `80 20 ?? 10` header, and the cached
  `state`/`ctx` track the iOS trace values (idle → `ctx = 0x3350`, i.e. 13136);
- toggling `poll_only` changes the log pattern only — never the transfer count
  per IRQ.

Stop rule: the first status that fails the header check twice, or any state byte
outside the set already documented (`0x00 0x07 0x09 0x11 0x13 0x15 0x19`), ends
the run; the driver must leave the rails off and report raw.

---

## 11. Remaining unknowns (with exact anchors)

1. **`0x06` payload bytes `0x4f..0x66`** — produced by `0x66b76dc(this,
   &buf[0x4f], captureData)`. Decode that function to finish the 0x06 layout;
   nothing else in the payload is missing.
2. **`0x6b`/`0x83` blocks semantics** (`this+0x1d50` 16 B, `this+0x1d88` 16 B,
   `this+0x1d98`) — likely enrolment/match template metadata; anchor: the same
   function's `flags & 2` / `flags & 0x10` branches.
3. **`vtable+0x550` on the transport** (called before each property apply) —
   probably `setClockFrequency`/`setClockPhase` wrapper; body not decoded.
4. **`[md vtable+0xd8]` / `[md vtable+0xa0]` on `IOMesaCaptureData`** — the
   pointer/length pair handed to `0x65`; the pointer is almost certainly the
   same 64 KiB IOMD VA, but the exact call (getBytesNoCopy vs. getPhysicalSegment)
   is not proven, which matters only for DMA, not for the memcpy-based GT path.
5. **Object split between `AppleMesa` and `AppleSPIBiometricSensor`** — the
   metaclass ctor at `0x5eef384` gives `AppleSPIBiometricSensor` size `0xe8`,
   while the state machine fields reach `+0x318`; which methods live on which
   object needs one clean pass (the log prefix `AppleSPIBiometricSensor::` on
   `enableInterrupts` is not proof of the receiver).
6. **`0x23` semantics** — now known to be a per-frame, payload-less "begin"
   issued by the image-init builder, not a user-client command; the SEP-side
   handler is not identified.
7. Everything already listed in `mesa-driver-re.md` §13 that this pass did not
   touch (GPIO IRQ type, patch provenance, 32-byte child `reg`, SIO endpoints).
8. **SBIO `0x3e`** (emitted by `retryMatchAfterHomeButtonPress` after the first
   `0x65`, 0-in / 6-byte out, `out[1]` stored at capture+0x34, `*(float*)(out+2)`
   at capture+0x38): no name in the 88-entry Mesa table, no entry in the SEP
   opcode table of `sbio-runtime-re.md`; semantics open. Anchor:
   `0x66b9ad4`-`0x66b9b58`, reply check `local_70 == 6`.

---

## 12. What this document does not do

- No code, DT, patch or config change; no commit. Only this file was written.
- No device access, no `ipad-ssh.sh`, no SPI/SEP traffic, no Ghidra mutation.
- No guessed `0x06` payload, no guessed capture opcode, no NVM/provisioning
  write path — even where the surrounding logic is now fully mapped.
