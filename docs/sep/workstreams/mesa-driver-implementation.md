# Mesa AP-side driver slice: implementation note

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


Status: implemented in the shared checkout, **not committed**, **no device
run**. Validation is offline only (host parser tests, static safety checks,
kernel object build). Companion to
`docs/sep/workstreams/mesa-driver-re.md` (hardware reconstruction) and
`docs/sep/workstreams/sbio-runtime-re.md` (SEP boundary, untouched here).

This slice is deliberately separate from SEP/SBIO. It proves AP-side power
sequencing and one SPI status transport. It does not capture frames, relay
frames, upload a sensor patch, enroll, provision, store keys or claim Touch ID
readiness.

---

## 1. What was added

| path | role |
|---|---|
| `drivers/mesa/mesa-a9.c` | the whole driver, including the marker-delimited host-testable status core |
| `drivers/mesa/Kconfig` | `CONFIG_APPLE_MESA_A9`, depends on SPI, REGULATOR, `ARCH_APPLE \|\| COMPILE_TEST` |
| `drivers/mesa/Makefile` | `obj-$(CONFIG_APPLE_MESA_A9) += mesa-a9.o` |
| `scripts/sep/build-mesa-driver.sh` | copies the three files into the `mesa` kernel worktree, adds one idempotent line to `drivers/Kconfig` and `drivers/Makefile`, builds only `drivers/mesa/mesa-a9.o` with `W=1` |
| `tools/sep/mesa_status_test.c` | host tests for the status parser and the one request template |
| `tools/sep/mesa_test.py` | lifts the status core into a host build, runs those tests, then runs the static safety checks |
| `docs/sep/workstreams/mesa-driver-implementation.md` | this note |

> Private graphics-dependent excerpt omitted from this source snapshot.

---

## 2. Driver model

### 2.1 Inert by default

`mesa_a9.enable=1` is required even to register the SPI driver: with the
parameter absent, `module_init()` logs one line and returns without calling
`spi_register_driver()`. There is therefore no probe, no debugfs tree and no
device binding by default.

When enabled, probe acquires resources only:

- `vdd-1v8` then `vdd-3v0` through `devm_regulator_get()` (never
  `devm_regulator_get_enable()`, because the sequencing is the point);
- an optional IRQ (`spi->irq > 0`) with `IRQF_ONESHOT | IRQF_NO_AUTOEN`;
- debugfs nodes;
- the DT `spi-max-frequency` and optional `apple,power-off-delay-ms`.

Probe does **not** enable a rail, does **not** call `spi_setup()` and does
**not** issue a transfer. `spi_setup()` is deferred to the first status poll, so
even controller programming happens only after an explicit arm and power-on.
(The SPI core has already configured the device from firmware when it
registered the `spi_device`; this driver does not touch that configuration at
probe.)

### 2.2 Power sequencing

Power on (under `mesa->lock`):

```
regulator_enable(1v8)
usleep_range(1000, 1250)          /* 1 ms */
regulator_enable(3v0)
msleep(100)                       /* 100 ms */
```

If `regulator_enable(3v0)` fails, 1v8 is disabled again only when this same
power-on attempt enabled it; a pre-existing partial rail state is preserved.
Per-rail `vdd_1v8_on` / `vdd_3v0_on` flags keep
`regulator_disable()` balanced and make a partial state explicit.

Power off (reverse order, also under the lock):

```
regulator_disable(3v0)
usleep_range(1000, 1250)          /* 1 ms */
regulator_disable(1v8)
msleep(power_off_ms)              /* DT or 1000 ms default, clamped to 5000 */
```

A rail is only disabled if this driver enabled it, so disarm, removal and
shutdown all work from a partial state. Removal and shutdown both end with
both rails off; a failed `regulator_disable()` is retried exactly once at
teardown and never looped.

Delays and the 8 MHz transfer ceiling are static facts from
`AppleMesa_setSensorPower` / the ADT (`mesa-driver-re.md` sections 3.2 and
5), not values discovered at runtime.

### 2.3 The only sensor transaction

`mesa_a9_status_locked()` builds one `struct spi_transfer`:

```
tx[0..6]  = 80 10 00 07 00 00 00
tx[7..22] = 00 (padding to the 23-byte reply)
len       = 23
speed_hz  = min(DT spi-max-frequency, 8000000)
```

One `spi_sync_transfer()`, no loop, no retry, no fallback. It is the only SPI
call site in the driver; `nm -u` on the built object shows exactly one
`spi_sync` symbol. `spi_setup()` is called once before the first transfer,
not at probe and not again.

The reply is parsed only if `rx[7] == 0x80`, `rx[8] == 0x20` and
`rx[10] == 0x10` (`rx[9]` is the documented `??`). Offsets in the 16-byte
status are payload-relative, so the sensor state (`+7`) is `rx[14]` in the
full 23-byte buffer. The parser records:

| field | payload offset | full-buffer offset |
|---|---|---|
| raw 16-byte status | 0..15 | 7..22 |
| sensor state | +7 | 14 |
| status/error | +8 | 15 |
| flags | +9 and +11 | 16 and 18 |
| context (u32 LE) | +12..15 | 19..22 |

`error == 0x29` is stored and printed verbatim. It is **not** treated as
bootROM success, patch readiness or any other semantic success; the report's
0x29 reading is only an observed candidate.

### 2.4 IRQ

The optional IRQ is requested with the firmware/D T trigger. No
`IRQF_TRIGGER_*` flag is passed, and no `irq_set_irq_type()` call exists, so
the unknown iOS GPIO mode-3 translation stays a firmware/DT concern. The
threaded handler does three things:

```c
atomic_inc(&mesa->irq_count);
WRITE_ONCE(mesa->last_irq, jiffies);
return IRQ_HANDLED;
```

It never powers a rail, never talks to the sensor, never logs per interrupt
and never takes `mesa->lock`. `disable_irq()` under the lock therefore cannot
deadlock against it. The IRQ starts disabled (`IRQF_NO_AUTOEN`) and remains
disabled while the sensor is unpowered; it is enabled only after an explicit
power-on has settled both rails, and disabled before power-off, disarm, removal
or shutdown.

### 2.5 Locking and waits

`mesa->lock` serializes power transitions and the status transfer. It is the
only driver mutex. All waits are bounded by construction: two fixed
microsecond sleeps, one fixed 100 ms settle, one DT/default power-off delay
clamped to 5000 ms, plus the SPI and regulator framework's own timeouts.
There is no workqueue, timer, completion, state machine or retry loop.

---

## 3. debugfs ABI

Directory: `/sys/kernel/debug/mesa-a9/`

| node | mode | direction | gate | effect |
|---|---|---|---|---|
| `state` | 0444 | read | none | snapshot; no I/O |
| `irq-count` | 0444 | read | none | `%d\n`, cumulative since probe |
| `arm` | 0200 | write | `CAP_SYS_RAWIO` | `1` arms once per boot; `0` safe disarm |
| `power` | 0200 | write | `CAP_SYS_RAWIO` + armed | `on`/`off` (also accepts `1`/`0` via `kstrtobool`) |
| `status` | 0400 | read | `CAP_SYS_RAWIO` + armed + powered; mode checked before the transfer | exactly one 23-byte poll |

`arm=1` is **one-way per boot**. The first successful arm sets `arm_used`;
`arm=0` disables the IRQ, powers both rails off in reverse order and clears
`armed`, but a later `arm=1` returns `-EPERM` until reload/reboot. Writing
`arm=1` while already armed is a no-op. `arm=0` is always accepted (safe
disarm).

`status` performs its poll on the first read of an open file (offset 0) and
serves any later chunks of that read sequence from the formatted text, so a
short userspace buffer cannot turn one read into several transfers. A new
open starts a new poll. It returns text even when the transfer or validation
fails, so the raw bytes are visible; only a gate failure returns `-EPERM` (no
capability), `-EACCES` (not armed) or `-ENODEV` (not powered). An unsupported
SPI mode refuses the transfer without returning errno: the text shows
`spi_error=-95` (`-EOPNOTSUPP`) and zeroed bytes instead. Output fields:

```
mesa-a9 status: one full-duplex poll, transport only
spi_error=<errno or 0> valid=<0|1> attempts=<n>
tx=<23 bytes, hex>
rx=<23 bytes, hex>
raw=<16 bytes at rx[7..22], hex>
state[7]=0x.. error[8]=0x.. flags[9]=0x.. flags[11]=0x.. context[12..15]=0x........
```

This is a transport/parser reachability statement. It is not evidence that
the sensor is patched, that Touch ID is ready, or that the bootROM state is
acceptable.

`state` fields: `armed`, `arm_used`, `powered`, `rail_1v8`, `rail_3v0`,
`spi_mode`, `mode_supported`, `configured`, `bus_hz`, `xfer_hz`,
`power_off_ms`, `irq`, `irq_requested`, `irq_enabled`, `irq_count`,
`last_irq_jiffies`, `status_attempts`, `last_status_err`,
`last_status_valid`.

The ABI is documented here for review; this round intentionally contains no
device-run script. Device execution remains the orchestrator's guarded
stages (S1/S2 in `mesa-driver-re.md` section 12), one variable at a time.

---

## 4. Evidence, and the GLM corrections as code

| correction from the prior report | how this code reflects it |
|---|---|
| the sensor patch source is not proven to be SEP | no patch path, no patch constant, no claim in the header comment; the status poll cannot upload anything |
| `+8 == 0x29` is only a candidate | the parser stores and prints the byte; no branch, no success flag, no readiness string |
| iOS GPIO mode 3 mapping is unknown | no trigger flag and no `irq_set_irq_type()`; the IRQ is optional and firmware-owned |
| SPI mode 0 is expected, not established | mode comes from DT unchanged; modes 0 and 3 are accepted at the protocol level, anything else is refused; no fallback |
| LDO7/LDO15 register decoding is static only | the driver knows only named supplies; no PMIC register address or LDO number appears |
| a poll must not claim Touch ID readiness | every status output starts with "transport only"; docs repeat it |

Other facts used, all from `mesa-driver-re.md`:

- 1V8 -> 1 ms -> 3V0 -> 100 ms, reverse on power-off, ADT
  `power-off-delay` 1000 ms (regulator role is static analysis, hence named
  supplies and no register writes);
- 8 MHz `spi-frequency` and the 23-byte poll shape (`80 10 00 07` request,
  `80 20 ?? 10` response header, 16 status bytes);
- IRQ is AP GPIO 44 mode 3 on j71t, but the Linux trigger remains unresolved;
- no reset GPIO on j71t; this slice has no reset operation at all.

---

## 5. Device tree is still gated

No DT patch is included. The driver consumes only standard resources, so the
eventual node can be reviewed independently:

```dts
mesa: biosensor@0 {
	compatible = "apple,mesa";		/* ADT: "biosensor,mesa" */
	reg = <0>;
	spi-max-frequency = <8000000>;		/* ADT spi-frequency */
	interrupts = <44 ...>;			/* trigger still unknown */
	vdd-1v8-supply = <&mesa_1v8>;
	vdd-3v0-supply = <&mesa_3v0>;
	apple,power-off-delay-ms = <1000>;	/* optional; default 1000 */
};
```

Open items that keep this out of the DT round:

1. iOS GPIO specifier mode 3 has no confirmed Linux trigger; `IRQ_TYPE_EDGE_BOTH`
   and a level type are both still candidates.
2. The D2365 LDO7/LDO15 enable-register decode (`0x312` / `0x319`) is static
   analysis only, not a device read-back.
3. The GLM example regulator nodes hang off `&i2c0`, while the live PMU decode
   points at `i2c1` (`0x9c40000003c`). That must be settled before a DT patch.
4. The 32-byte SPI child `reg` blob beyond the unit address/CS bit is
   undecoded; both the touch and audio Linux nodes use `reg = <0>` and work,
   but that is precedent, not proof for Mesa.
5. `spi_setup()` is deferred until the first status poll, so a wrong DT mode
   will surface as `-EOPNOTSUPP` from `status`, not as a probe failure.

---

## 6. Offline validation

Run from the repo root:

```sh
python3 tools/sep/mesa_test.py
scripts/sep/build-mesa-driver.sh
git diff --check
```

Results in this round:

| check | result |
|---|---|
| `tools/sep/mesa_test.py` | 8/8 host parser tests pass, 17/17 static checks pass |
| `scripts/sep/build-mesa-driver.sh` | `CC [M] drivers/mesa/mesa-a9.o`, no warnings |
| forced `W=1` rebuild | no warnings or errors |
| `scripts/checkpatch.pl --no-tree --file drivers/mesa/mesa-a9.c` | 0 errors, 0 warnings |
| `nm -u drivers/mesa/mesa-a9.o` | one `spi_sync` symbol, `spi_setup`, `devm_regulator_get`, `regulator_enable`/`regulator_disable`; no other SPI entry point |
| `git diff --check` | clean (new files checked separately for whitespace) |

The host suite drives the shipping parser, not a copy: `mesa_test.py` lifts
the block between the `MESA_A9_STATUS_CORE_BEGIN` / `_END` markers, prepends
a typedef prelude and compiles `tools/sep/mesa_status_test.c` against it under
ASan/UBSan. The static half proves:

- exactly one `spi_sync_transfer()` call site, and no other SPI I/O API;
- the only `0x..` literals in code are `0x00 0x07 0x10 0x20 0x80`, i.e. the
  status request and header; every forbidden command byte would be outside
  that set;
- no capture/frame/patch/provisioning/session/unpair/calibration/NVM/TestCMD/
  HV-boost/key token in code;
- no frame-sized constant (`19668`, `11924`, `65536`, `16356`, ...) and one
  `devm_kzalloc(sizeof(*mesa))` allocation only;
- `arm`/`power`/`status` route through `capable(CAP_SYS_RAWIO)`; `power` and
  `status` additionally require armed and (for status) powered;
- the mutating debugfs nodes are 0200 write-only; state nodes are read-only;
- the IRQ handler has no SPI, regulator, mutex or sleep token, and only
  increments an atomic counter;
- probe contains no `regulator_enable`, `spi_setup`, `spi_sync`, `msleep` or
  `usleep`;
- 1V8 is acquired and enabled before 3V0, 3V0 before 1V8 on the way down, and
  a 3V0 enable failure disables 1V8;
- the status path has no loop and defers `spi_setup()`;
- no `devm_regulator_get_enable`, workqueue, timer, completion or wait-event;
- remove and shutdown power both rails off and stop the IRQ.

---

## 7. Residual risks and blockers

1. **No hardware validation.** Nothing in this note was run on the iPad; the
   driver has never been bound, armed or powered on a device. The first device
   run is the orchestrator's guarded S1/S2, not this workstream.
2. **No DT, no supplies, no bind.** Until the regulator nodes and the `mesa@0`
   child exist with correct `vdd-1v8`/`vdd-3v0` phandles, probe fails with
   `-ENODEV`/`-EPROBE_DEFER`. That is expected and is why the driver is
   `enable=0` by default.
3. **Mode 0 is an expectation.** Both mode 0 and mode 3 are accepted from DT,
   but no runtime check can tell whether the sensor actually sampled the poll
   correctly; only the reply header can, and a bad reply is reported, not
   retried.
4. **The header check is intentionally strict** (`rx[8] == 0x20`). The iOS
   `mesa-full-status` variant uses a mask that accepts more values; that
   variant is not implemented because it is not part of this slice.
5. **`0x29` remains uninterpreted.** The raw byte and the rest of the status
   are printed; the driver never turns them into "bootROM", "patched" or
   "ready".
6. **The IRQ trigger is untested.** The handler may count nothing if the DT
   trigger is wrong; that is a firmware/DT issue, deliberately not guessed in
   C.
7. **Status is one poll per open/read sequence.** Reopening the file for each
   read is a new explicit poll; the driver itself never retries, never waits
   for a state change and never interprets the sensor state machine.
8. **The power-off delay is clamped to 5000 ms.** The ADT value is 1000 ms;
   the clamp exists only to keep a malformed DT from creating an unbounded
   teardown wait.
9. **No module unload protection beyond `THIS_MODULE`.** Debugfs file
   operations hold the module reference while open, as usual; the device
   unbind/debugfs lifetime race is the standard one, not addressed here.
