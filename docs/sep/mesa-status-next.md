# First bounded Mesa hardware-status probe

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**2026-10-27 follow-up:** mesa1 run 2 completed but returned 23 zero RX bytes.
Use [the Mesa2 procedure](mesa-status2-next.md) for the next run. Its offline
reconstruction corrects the mode-0 assumption: Apple uses CPHA plus Samsung's
extended phase bit. A healthy warm kexec is sufficient for that status-only
experiment; the fresh-DFU advice below describes the original mesa1 plan.

Date: 2026-10-27. This is the next Touch ID experiment after the successful
read-only SBIO `GetProtectedConfig` run in
`docs/sep/logs/tri53-sep7-protected-config-20261027-1059/`.

## Decision

Move to the AP-side Mesa sensor now. Do not spend the next boot on another
enrollment-state query.

The 10:59 run established a complete SBIO v1 request/reply with user 501:
32 output bytes, all eight little-endian words `0xffffffff`, status zero,
`tx=16 rx=23`, and clean FIFOs. Apple's AP code treats those values as unset.
That is a policy-state result, not a sensor-readiness result. An empty-template
query would be useful later for catalog semantics, but it cannot unblock the
missing power/SPI/frame path.

The next dependency is therefore one deliberately small hardware slice:

1. verify D2365 Mesa enable registers `0x312` and `0x319` start with bit zero
   clear;
2. enable 1V8, wait 1 ms, enable 3V0, wait 100 ms;
3. perform exactly one 23-byte full-duplex SPI status poll at 8 MHz, mode 0;
4. disable 3V0, wait 1 ms, disable 1V8, wait 1 second;
5. verify both PMIC bits returned to zero and permanently disarm the driver for
   that boot.

This does not boot sepOS, publish SEP OOL memory, send an SBIO command, request
a fingerprint image, upload sensor firmware, touch sensor NVM, enroll, match,
or write persistent storage. No finger is needed.

## Evidence boundary

Verified from j71t ADT/kernelcache and existing Linux providers:

- SPI2 is at `0x20a088000`, AIC IRQ 190, PMGR domain `ps_spi2`, input clock
  61,538,461 Hz, controller-internal chip select, sensor ceiling 8 MHz.
- Mesa's two D2365 functions resolve to LDO7 register `0x312` bit zero (3V0)
  and LDO15 register `0x319` bit zero (1V8).
- The power order and delays above match the reconstructed Apple driver.
- The status request is `80 10 00 07 00 00 00`, padded with zeros to the
  23-byte full-duplex reply length. A valid reply begins at RX offset seven
  with `80 20 ?? 10`.

Still a bounded hypothesis:

- The Linux SPI mode is mode 0. The ADT has no explicit phase property and the
  Apple-to-Linux phase mapping remains unresolved. This payload makes one
  mode-0 attempt only. An invalid reply is evidence for a later, separate
  cold-boot mode experiment; it is not permission to retry or flip modes in
  this boot.
- GPIO44's iOS mode-3 interrupt translation remains unresolved. The first DT
  deliberately omits the Mesa sensor IRQ, so no floating or mis-typed IRQ can
  affect this probe.

Full reconstruction is in `docs/sep/workstreams/mesa-driver-re.md`; the
bounded implementation contract is in
`docs/sep/workstreams/mesa-driver-implementation.md`.

## Artifact

| item | value |
|---|---|
| kernel branch | `a9/tri53-mesa1` |
| kernel commit | `cc0b92c36c7917d3b11ee7ad845e27b266f9fc2c` |
| parent | `a9/tri53-sep7` at `56c339590811a04a7e3eb94e33f75546692e1265` |
| entry | `tri53-mesa1-cc0b92c36c` |
| payload | `build/boot/m1n1-linux-tri53-mesa1-cc0b92c36c.bin` |
| payload SHA-256 | `4a7559c48ef411a0dfa6e9cf414a5ee4f1875e9fa07abca223554b3d6154825b` |
| device runner | `scripts/sep/remote-mesa-status1.sh` |

The kernel includes the validated tri53-sep7 NVMe shared-MSI fixes. Its
command line removes `sep_a9.enable=1` and adds only `mesa_a9.enable=1`.
The Mesa driver remains inert at probe; all rail and SPI activity requires the
one-way debugfs arm and `CAP_SYS_RAWIO`.

## Does it need a fresh DFU?

Yes, use a fresh DFU for this first sensor run. The reason is a clean PMIC,
SPI2, and sensor baseline and loading this new payload, not a SEP lifetime
requirement. Do not run `pongo-sep.sh`, `pongo-msg64-wake.sh`, arts/xarm, or
any SBIO setup. A normal cold DFU-to-pongoOS-to-Linux boot is sufficient.

## Exact procedure

### 1. Host preflight

```sh
PKG=build/sep/tri53-mesa1
P=build/boot/m1n1-linux-tri53-mesa1-cc0b92c36c.bin
RUN="$PKG/status-$(date +%Y%m%d-%H%M)"
mkdir -p "$RUN"

(cd "$PKG/tri53-mesa1-entry" && shasum -a 256 -c SHA256SUMS)
test "$(shasum -a 256 "$P" | awk '{print $1}')" = \
  4a7559c48ef411a0dfa6e9cf414a5ee4f1875e9fa07abca223554b3d6154825b
grep -qx \
  'cc0b92c36c7917d3b11ee7ad845e27b266f9fc2c arm64: apple: add bounded J71t Mesa status transport' \
  build/kernel-tri53-mesa1/COMMIT
grep -qw 'mesa_a9.enable=1' "$PKG/cmdline"
! grep -qw 'sep_a9.enable=1' "$PKG/cmdline"
```

Stop before DFU if any check fails.

### 2. Fresh DFU and direct Linux boot

```sh
touch build/boot/autoboot.disabled
PALERA1N_BYPASS_PASSCODE_CHECK=1 palera1n -lp \
  -k build/prebuilt/pongo/Pongo-a9sep.bin

scripts/boot.sh linux "$P"
scripts/boot/autoboot.sh --state
# Only if still in pongoOS after 15 seconds:
printf 'bootm\n' | build/bin/pongoterm
scripts/boot/autoboot.sh --state
```

Do not issue any `sep` command at the pongoOS prompt.

### 3. Read-only Linux preflight

```sh
scripts/omarchy/ssh.sh 'cat /proc/cmdline; uname -a; uptime; findmnt /; lsblk' \
  | tee "$RUN/1-preflight.txt"
scripts/omarchy/ssh.sh 'sudo -n dmesg' >"$RUN/dmesg-before.txt"
grep -E 'mesa|d2365|20a088000|nvme|I/O error|timeout' \
  "$RUN/dmesg-before.txt" | tee "$RUN/2-relevant-before.txt"
```

Require all of the following before running the device script:

- `linux_a9.entry=tri53-mesa1-cc0b92c36c` and `mesa_a9.enable=1` are present;
- `sep_a9.enable=1` is absent;
- root/NVMe is healthy and there is no timeout or I/O error;
- both `apple-d2365-mesa` supplies registered;
- Mesa reports `probe: inert (no rail, no transfer)`;
- there is no Mesa probe error or deferred-probe timeout.

The runner independently rechecks the entry, debugfs files, unarmed state,
zero attempt count, omitted sensor IRQ, and both physical PMIC enable bits.

### 4. Run exactly one status transaction

```sh
scripts/omarchy/ssh.sh 'sudo -n sh -s' \
  <scripts/sep/remote-mesa-status1.sh | tee "$RUN/3-mesa-status.txt"

scripts/omarchy/ssh.sh 'sudo -n dmesg' >"$RUN/dmesg-after.txt"
grep -E 'mesa|d2365|20a088000|spi|regulator' \
  "$RUN/dmesg-after.txt" | tee "$RUN/4-relevant-after.txt"
```

Do not run the script twice. `arm_used=1` makes a second arm fail even if it
is attempted accidentally.

## Pass criteria

A complete pass ends with:

```text
pmic_baseline reg312=0x.. reg319=0x..        # both bit zero clear
pmic_powered reg312=0x.. reg319=0x..         # both bit zero set
spi_error=0 valid=1 attempts=1
tx=80 10 00 07 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
status_attempts=1 last_status_err=0 last_status_valid=1
pmic_powered_off reg312=0x.. reg319=0x..      # both bit zero clear
armed=0 arm_used=1 powered=0 rail_1v8=0 rail_3v0=0
RESULT mesa-status=pass
```

Any raw sensor `state`, `error`, flags, or context value is acceptable when
the transport/header is valid. In particular, a nonzero Mesa error byte can
mean the unpatched sensor is not operational yet; record it verbatim and do
not reinterpret it as a fingerprint result.

## Stop rules

- Baseline PMIC bit set, wrong entry/mode/frequency, missing provider/debugfs,
  or nonzero prior attempt count: the runner stops before any write.
- Rail enable/readback mismatch: cleanup powers off and disarms; do not retry.
- SPI error, timeout, all-zero/all-`ff`, or invalid `80 20 ?? 10` header: the
  raw bytes are still evidence; cleanup powers off and disarms; do not retry
  or change SPI mode in this boot.
- Failure to restore either PMIC bit: stop immediately and cold-reset.
- After pass or failure, preserve the logs and cold-reset before another Mesa
  experiment. Do not proceed to capture, patch upload, enrollment, or match in
  this boot.

After a valid status reply, the next engineering rung is to recover and
validate the sensor patch source/upload path and resolve GPIO44's Linux IRQ
mode. Frame capture and the 19,668-byte SBIO relay remain out of scope until
both are proven.
