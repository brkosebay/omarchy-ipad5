# J71t Smart Cover suspend/wake round 1: implementation and test plan

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-24. **Offline implementation. No device access and no hardware
results yet.** Goal: closing a Smart Cover suspends Omarchy (s2idle) and
opening it wakes the iPad, as on iOS. Study: [ds-smart-cover.md](ds-smart-cover.md)
(read §3.1 with the correction below). Sleep/wake with Home/Power:
[s2idle-test-plan.md](s2idle-test-plan.md).

## What changed

**Kernel branch `a9/sensors`** owns `gpio-apple-d2365`. It carries the
driver's only post-merge fix (`26d38c9304`, never take over a PMIC function
line), the Smart Cover gpio-keys DT and the CT819 work. `a9/periph` and
`a9/power` only have the original driver. Round head **`60c6f2f07b`** on
`df595c3247`:

> Private graphics-dependent excerpt omitted from this source snapshot.

The two cherry-picks keep `a9/sensors` valid on its own. Git merges
identical hunks cleanly.

**Corrected register model** (AppleD2365PMU, kernelcache 20H350, vtable
`0xfffffff006fd37f8`). It replaces study §3.1 rows 1–4:

| Reg | Meaning | Evidence |
|---|---|---|
| `0x140`, 30 B | **events**, clear on read | `readEvents` (vtable `+0x618`, `0x6bba334`) reads 30 B at 0x140. With a NULL buffer it is the "clear events" call (`cannot clear Events`, `0x6bbda20`) |
| `0x180`, 30 B | live status | `0x6bba31c`, `0x6bb9cec` |
| `0x1c0`, 30 B | **interrupt mask, set = masked** | `mask`/`unmask` (`0x6bba5bc`/`0x6bba658`) set/clear a bit of the cache at `+0x178` and write `0x1c0+n/8`. Probe fills the cache with `0xff` and writes all 30 B (`0x6bb93f8`) |
| `0x1c6`..`0x1c8` | GPIO slice of that mask (events 0x30–0x47) | before sleep iOS writes the constant `ff ff fe ff ff f0 ff ff ff fc ff…` (`0xfffffff005915af4`) to 0x1c0. It then writes `ff ff ff` with the bit cleared for each line whose `GPIOCFG & 0xc1 == 1` to 0x1c6 (`0x6bbb51c`–`0x6bbb560`) |

So the study's "ack by writing 0x1c0" would rewrite masks. "Every input line
stays wake-enabled" is also backwards: only lines with config bit 0 set (and
bits 6–7 clear) are unmasked in iOS sleep. Step R0 prints the hall lines'
config, which says whether iOS arms them.

**Driver** (`drivers/gpio/gpio-apple-d2365.c`, only with
`apple,enable-hall-irqs`; the GPIO half is unchanged):

- At probe it logs the inherited mask and the latched events, writes
  all-ones to 0x1c0 as iOS does, creates a one-cell domain on the PMIC node
  (only events 0x30 and 0x3a map), clears stale events and requests AP GPIO
  76 as a threaded `IRQF_ONESHOT` IRQ named `d2365-events`.
- The thread reads 0x140 (which clears it and releases the level line) and
  runs the nested child IRQs (`d2365-hall` 48/58). If nothing of ours is
  latched, every enabled switch resyncs (gpio-keys re-reads the level).
- mask/unmask touch only the hall bits of 0x1c6/0x1c7. Every other event
  stays masked, because nothing else in the kernel consumes PMIC events.
- `irq_set_wake` counts. The wake reference on GPIO 76 is taken in
  `bus_sync_unlock`, outside the child descriptor lock (the regmap-irq
  pattern; `irq_chip_set_wake_parent()` needs a hierarchy this cascade
  doesn't have).
- Suspend writes the mask to "only wake-enabled halls". From `->suspend` to
  `->resume` the bus is never touched. An IRQ resumed before I2C is held
  with its event still latched, and released in `->resume`. One that races
  the transition calls `pm_wakeup_hard_event()` (only if a hall is
  wake-enabled).
- More than 64 IRQs/s, or read errors, hold the line with up to 5 backed-off
  retries; a good read restores the budget. A held line never refuses
  suspend; it warns, and the buttons still wake.
- It never touches the PMIC's wakeup source (the i2c core owns it via
  `wakeup-source`) or runtime PM. A failed event setup warns and keeps the
  GPIOs; only `-EPROBE_DEFER` propagates.

**DTB:** `s8003-j71t-smart-cover.dtb` = `s8003-j71t.dts` plus
`apple,enable-hall-irqs`. Both switches become IRQ `gpio-keys` (not
`-polled`): `lid-switch` (`SW_LID`, line 0) on event `0x30`, `cover-switch`
(`SW_MACHINE_COVER`, line 10) on `0x3a`, both with `debounce-interval = <14>`
(iOS default 14000 µs) and `wakeup-source`. Polarity is still
`J71T_COVER*_ACTIVE` (active low). **The DTB changes**, which is fine for
warm kexec. `s8003-j71t.dtb` stays the polled fallback, and its driver path
never reads 0x140 or writes 0x1c0.

**Earlier attempt.** The Codex thread (13:05–13:41 UTC) committed a first
version on `a9/sensors`, saved as local ref `backup/sensors-codex-smart-cover-r1`
(`8a29391e00`, not pushed: outside `a9/*`). Its register map and opt-in
structure were right. Three device-fatal bugs were invisible to its mock
tests, so the IRQ half was rewritten:
- `device_init_wakeup(pmic)` returns `-EEXIST`, because the i2c core already
  owns it. That failed the whole PMIC GPIO probe.
- `pm_runtime_resume_and_get(pmic)` returns `-EACCES`: runtime PM is never
  enabled there. Every event was held and nothing would ever be delivered.
- `-EBUSY` on suspend when the IRQ was held would leave a closed iPad awake.

Its integration build also died with OOM (`Error 137`), not a code error.

**a9/nextint pre-existing issue:** its `apple,pmic.yaml` has the `gpio:` key
twice, with the interrupt block between them (the periph×pcie merge).
Merging `a9/sensors` adds a third, textually clean copy of the block. The
test merge resolves this by taking a9/sensors' file, which is identical
except for that stray `gpio:`, so it also fixes nextint. Integration should
do the same.

## Offline: build, checks, results

```sh
scripts/sensors/build-smart-cover.sh owner   # a9/sensors checks + patches/sensors/smart-cover-r1/
scripts/sensors/build-smart-cover.sh merge   # detached a9/nextint + a9/sensors overlay
scripts/sensors/build-smart-cover.sh build   # build/kernel-sensors (KERNEL_LOCALVERSION=-a9-smart-cover)
scripts/sensors/build-smart-cover.sh check
python3 -B -I scripts/sensors/package-smart-cover.py   # build/power/smart-cover-r1/entry/
```

`check` runs W=1 on the driver and both j71t DTBs, `checkpatch --strict` on
the driver, `dt_binding_check` (d2365 gpio, apple,pmic, gpio-keys), and
dt-validate of both DTBs (the opt-in DTB may add **no** error over the
default). It also runs `check-smart-cover-dtb.py` (compiled DTB assertions)
and `test-smart-cover.py`: the real struct, constants and 11 IRQ/PM
functions compiled against a fault-injecting mock bus with the model above
(10 scenarios), plus a guard against the two probe bugs.

Results (2026-10-24): on a9/sensors and on the merge into a9/nextint
`cc0c824de6`, W=1 is clean apart from the existing `simple_bus_reg` warning
for `/soc/bus@20c000000`, file checkpatch is clean, `dt_binding_check`
passes, dt-validate shows no new error, and all DTB and mock checks pass.
Series checkpatch is 0/0/0 except one `MAINTAINERS` warning for the new
`.dts` (covered by the `arch/arm64/boot/dts/apple/` entry). Full LLVM arm64
build of the merge: `Image.gz` + 106 DTBs, release
`7.3.0-rc1-a9-smart-cover-gcc0c824de6d7-dirty`; `SOURCE.patch` equals the
overlay `integration.patch`. Entry `smart-cover-r1-60c6f2f07b` in
`build/power/smart-cover-r1/entry/` (checksums verified).
The merge is clean except the binding resolution above. Config is nextint's
plus `KEYBOARD_GPIO_POLLED=y` (for the fallback DTB).

## Staged userspace policy (workspace only, not in the rootfs builder)

`scripts/omarchy/rootfs/smart-cover/` (README there): logind
`HandleLidSwitch=suspend`, `…ExternalPower=suspend` (the USB cable used for
SSH counts as external power), `LidSwitchIgnoreInhibited=yes`,
`HoldoffTimeoutSec=5s`. Also `lid.conf` (`ipad-lid` keeps lock + display off
on close, never suspends itself) and `hypridle.conf` (DPMS off before every
sleep, on after). Installed in step D2. The shipped rootfs keeps `ignore`.

## Orchestrator: stage and warm kexec

**Everything from here on accesses the device.** Start from a healthy
NVMe-root boot of the current nextint entry, with Power/Home s2idle working.
Capture netconsole in a second Mac terminal:

```sh
scripts/omarchy/netconsole.py build/power/smart-cover-r1/device-netconsole.log
```

```sh
(cd build/power/smart-cover-r1/entry && shasum -a 256 -c SHA256SUMS)
ENTRY=$(cat build/power/smart-cover-r1/entry-name.txt)
scripts/boot/upload-entry.sh "$ENTRY" build/power/smart-cover-r1/entry
scripts/boot/warm-chain.sh "$ENTRY" 1 build/power/smart-cover-r1/device/warm-entry
scripts/omarchy/ssh.sh 'uname -r; tr "\0" "\n" </proc/device-tree/smart-cover/compatible; ls /proc/device-tree/soc/i2c@20a110000/pmic@3c/gpio/ | grep hall'
```

The entry's cmdline and initramfs come from the s2idle round-1 entry. If
that is no longer the right template, use
`package-smart-cover.py --template <current entry dir>`. Require warm-chain
PASS, a release containing `-a9-smart-cover`, `gpio-keys` (not
`gpio-keys-polled`), and `apple,enable-hall-irqs`.

Install the sleep helper (RAM only):

```sh
scripts/omarchy/ssh.sh 'cat > /tmp/smart-cover-sleep.sh; chmod 755 /tmp/smart-cover-sleep.sh' \
  < scripts/sensors/device/smart-cover-sleep.sh
mkdir -p build/power/smart-cover-r1/device
```

## R: read-only checks (no suspend, no register write from userspace)

**R0, boot evidence.** Stop at the first FAIL.

```sh
scripts/omarchy/ssh.sh 'sudo dmesg | grep -E "boot event mask|boot events|hall event|line (0|10) config|PMIC function|d2365|gpio-keys"' \
  | tee build/power/smart-cover-r1/device/r0-dmesg.txt
```

- PASS: exactly one each of `boot event mask <60 hex>`, `boot events <60 hex>`,
  `hall events 0x30/0x3a on IRQ <n>`, `line 0 config 0x..` and
  `line 10 config 0x..` (`input`, or `output, switching to input`).
- FAIL: `hall event IRQs unavailable: <err>` (event path not set up, GPIOs
  fine), or `belongs to a PMIC function` (gpio-keys cannot read the level).
  Report the line and stop.
- Record, don't judge: the mask (all `ff` would match iOS), the latched
  events (byte 6 = chars 13–14, bit 0 = line 0; byte 7 = chars 15–16, bit 2
  = line 10), and both config bytes. Bit 3 = input, bit 4 = function. Bit 0
  set with bits 6–7 clear means iOS keeps the line wake-enabled.

**R1, wiring.**

```sh
scripts/omarchy/ssh.sh 'grep -E "d2365|Smart Cover" /proc/interrupts; for d in smart-cover smart-cover-aux; do readlink /sys/devices/platform/$d/driver; cat /sys/devices/platform/$d/power/wakeup; done; cat /sys/bus/i2c/devices/0-003c/power/wakeup 2>/dev/null; journalctl -b -u systemd-logind | grep -i "watching system buttons"' \
  | tee build/power/smart-cover-r1/device/r1.txt
sleep 30; scripts/omarchy/ssh.sh 'grep d2365-events /proc/interrupts'
```

- PASS: one `d2365-events` line on the AP GPIO chip (hwirq 76). Two
  `d2365-hall` lines, hwirq `48` labelled `Smart Cover (PMIC GPIO 0)` and
  `58` labelled `Smart Cover (PMIC GPIO 10)`. Both drivers are `gpio-keys`; both
  `power/wakeup` read `enabled`. The `d2365-events` count is unchanged over
  the 30 s idle (no storm). logind logs `Watching system buttons on
  /dev/input/eventN (iPad Smart Cover)`. If the I2C device path differs,
  find the PMIC with `ls /sys/bus/i2c/devices/*/name`.
- If logind does not watch it: `udevadm info -n /dev/input/eventN | grep -E "ID_INPUT_SWITCH|TAGS"`
  must show `ID_INPUT_SWITCH=1` and `power-switch`. Report if not.

**R2, the owner moves the cover** (5 × close, wait 3 s, open, wait 3 s,
slowly):

```sh
scripts/omarchy/ssh.sh 'echo "file gpio-apple-d2365.c +p" | sudo tee /sys/kernel/debug/dynamic_debug/control >/dev/null; grep -E "d2365|Smart Cover" /proc/interrupts' \
  > build/power/smart-cover-r1/device/r2-irq-before.txt
scripts/omarchy/ssh.sh 'ipad-lid --watch 60' | tee build/power/smart-cover-r1/device/r2-watch.txt
scripts/omarchy/ssh.sh 'grep -E "d2365|Smart Cover" /proc/interrupts; sudo dmesg | grep -E "events [0-9a-f]{60}|holding the IRQ|storm"' \
  > build/power/smart-cover-r1/device/r2-irq-after.txt
```

- PASS: `--watch` shows `SW_LID` **1 on every close and 0 on every open**,
  5 + 5 events. `d2365-hall 48` rises by 10 and `d2365-events` by 10–20
  (both halls may move). Each `events` debug line has byte 6 bit 0 and/or
  byte 7 bit 2 set. No `holding the IRQ` or `storm`.
- Polarity wrong (1 when open): flip both switches with
  `scripts/sensors/cover-polarity.sh build/kernel-sensors/dtbs/s8003-j71t-smart-cover.dtb active-high both build/power/smart-cover-r1/entry/dtb`,
  re-run `upload-entry.sh` under a new entry name, and repeat from R0. logind
  reads the kernel value, so `ipad-lid invert` does not help it.
- Events on close only, or on open only: the inherited event edge config is
  one-sided, and wake on open cannot work. Stop and report the R0 config
  bytes; the GPIOCFG edge bits need their own study.
- `SW_LID` changes but `d2365-events` never rises: the PMIC does not signal
  these events. Stop and report R0.
- Count climbing with the cover still, or `holding the IRQ`: report the
  `events` lines and R0.

**R3, awake negative case** (from the study): 3 quick close/open pairs. PASS:
exactly 6 `SW_LID` changes, ≤ 4 `d2365-events` per move, no storm/hold line,
and the panel and touch stay normal (ipad-lid is still off by default).

## D: suspend and wake

Run one at a time and save each log. Keep netconsole running.

```sh
scripts/omarchy/ssh.sh 'sudo cat /run/smart-cover-r1/last.log' > build/power/smart-cover-r1/device/<step>.log
P=$(scripts/omarchy/ssh.sh "awk '/d2365-events/{sub(\":\",\"\",\$1); print \$1}' /proc/interrupts")
```

**D1, kernel wake path, logind still `ignore`.** Close the cover, then:

```sh
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=sc-r1-d1 /tmp/smart-cover-sleep.sh suspend d1-cover-wake'
# wait at least 20 s, then open the cover; touch nothing else
```

PASS, all of:
- `result slept elapsed=` ≥ 20 s.
- The kernel section has `PM: Triggering wakeup from IRQ $P` (and
  `pm_wakeup_irq=$P` if present).
- The after-snapshot shows `SW_LID=0 (open)` and `d2365-events` above the
  before-snapshot.
- No `holding the IRQ` / `storm` / `mask write failed`.
- The panel lights, touch works, and SSH and NVMe are as in s2idle-test-plan
  acceptance 3–6.

FAIL if it stays asleep after the open (wake with Home, save the log), or if
it wakes before the open.

**D2, logind policy.** Install it as in `scripts/omarchy/rootfs/smart-cover/README.md`,
keeping copies of the replaced files:

```sh
scripts/omarchy/ssh.sh 'sudo cp /etc/systemd/logind.conf.d/90-linux-a9-lid.conf /var/tmp/90-linux-a9-lid.conf.ignore; cat ~/.config/linux-a9/lid.conf > /var/tmp/lid.conf.orig 2>/dev/null; cp ~/.config/hypr/hypridle.conf /var/tmp/hypridle.conf.orig'
scripts/omarchy/ssh.sh 'sudo tee /etc/systemd/logind.conf.d/90-linux-a9-lid.conf >/dev/null' < scripts/omarchy/rootfs/smart-cover/90-linux-a9-lid.conf
scripts/omarchy/ssh.sh 'mkdir -p ~/.config/linux-a9; cat > ~/.config/linux-a9/lid.conf' < scripts/omarchy/rootfs/smart-cover/lid.conf
scripts/omarchy/ssh.sh 'cat > ~/.config/hypr/hypridle.conf' < scripts/omarchy/rootfs/smart-cover/hypridle.conf
scripts/omarchy/ssh.sh 'sudo systemctl reload systemd-logind; ipad-lid suspend off; ipad-lid on; systemctl --user restart hypridle ipad-lid; busctl get-property org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager HandleLidSwitch HandleLidSwitchExternalPower'
```

Require `s "suspend"` twice. If the reload is refused, warm-kexec the same
entry once and check again. Then, with the cover open:

```sh
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=sc-r1-d2 /tmp/smart-cover-sleep.sh wait d2-logind'
# within 2 min: close the cover; wait at least 20 s; open it
scripts/omarchy/ssh.sh 'journalctl -b -u systemd-logind --since -10min | grep -E "Lid (closed|opened)|suspend"'
```

PASS: logind logs `Lid closed.` and its suspend message within about 1 s of
the close; the D1 PASS list holds; logind logs `Lid opened.` after the wake.
The panel is off from the close on and back at its old brightness after the
open.

**D3, negative: cover open, nothing wakes it.**

```sh
scripts/omarchy/ssh.sh 'sudo systemd-run --unit=sc-r1-d3 /tmp/smart-cover-sleep.sh suspend d3-open-stays-asleep'
# cover stays open; wait 120 s; then press Home once
```

PASS: `elapsed` ≥ 120 s, the wake IRQ is Home's gpio-keys line (not `$P`),
and `d2365-events` is unchanged across the sleep.

**D4, stray wake with the cover closed.** Close the cover (logind
suspends), wait 20 s, press Home. PASS: it wakes, then logind suspends
again by itself within about 10 s (`HoldoffTimeoutSec=5s`):
`/sys/power/suspend_stats/success` +2 overall and two suspend messages from
logind. Opening the cover then wakes it as in D1.

**D5, reliability.** 10 close/open cycles with D2's checks, at least 10 s
asleep each, one tap between cycles. All must pass with no touch, USB, NVMe
or panel regression. After that, a two-magnet check: fold the cover behind
the iPad while it sleeps from Power. If folding wakes it, disable the second
hall's wake and repeat this check:
`echo disabled | sudo tee /sys/devices/platform/smart-cover-aux/power/wakeup`.
Record which way you went.

## Failure handling and rollback

Stop at the first failed PASS. Save the helper log, `dmesg`, netconsole and
`/proc/interrupts`. A sleep that does not wake on the cover still wakes with
Home/Power. If the event path misbehaves, boot the previous entry: its
default DTB never touches PMIC event registers. Policy rollback:

```sh
scripts/omarchy/ssh.sh 'sudo cp /var/tmp/90-linux-a9-lid.conf.ignore /etc/systemd/logind.conf.d/90-linux-a9-lid.conf; cp /var/tmp/hypridle.conf.orig ~/.config/hypr/hypridle.conf; [ -s /var/tmp/lid.conf.orig ] && cp /var/tmp/lid.conf.orig ~/.config/linux-a9/lid.conf || rm -f ~/.config/linux-a9/lid.conf; sudo systemctl reload systemd-logind; systemctl --user restart hypridle ipad-lid'
```

Leave the default entry unchanged. Open questions this round answers on
device: S1 polarity (R2), S3 events under Linux (R0/R2), S5 ack semantics
(R2: no storm), S6 cover wake (D1), S8 second hall (D5).
