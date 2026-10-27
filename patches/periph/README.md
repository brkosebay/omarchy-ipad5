# Peripherals mail series

> Historical design/test notes. Current support: [status](../../docs/release/STATUS.md); examples may need local firmware/captures.


Apply `series` in lexical order to HoolockLinux `hoolock` baseline
`6831bc701`. **Verified:** 55 patches replay exactly to the tree of
`a9/periph` head `64221b189e77eb7de61f5c38d30de2fd53927de5`.
Implementation/build/risk report: [docs/periph/README.md](../../docs/periph/README.md).

Groups:

- **0001–0009:** serdev stop bits, UART HDQ bindings/transport, bq27xxx
  read-only cached telemetry and optional mux support. Imported from the
  Pauli1Go template commits in the brief, with prerequisites.
- **0010–0018:** D2365 GPIO, Samsung T8010 baud programming, Broadcom C1
  identification/reset helper, advertising quirk and J172 HCI lifecycle.
- **0019–0022:** template Bellatrix bindings/read-only driver and D2365
  charger/status driver. Bellatrix is a template dependency only: it is
  **not enabled or described on J71t**. The J172 charger policy remains
  board-gated and is never selected by the J71t DT.
- **0023–0031:** ALS sample/integration fixes, calibrated CT821 prerequisite,
  shared match data, CT819 support and calibration arithmetic KUnit source.
- **0032:** experimental J71t HCI match/A9 baud opt-in and C0 ROM default
  identity handling. Silicon revision is not asserted by the DT.
- **0033:** experimental J71t bq27540-to-template-map match, periodic
  **read-only** D2365 refresh/status corroboration without USB-online claims.
- **0034:** optional raw private LSCI firmware input, NVMEM preferred.
- **0035:** self-contained `s8003-j71t-periph.dtsi` plus one board include.
  Contains all J71t peripheral wiring; touches no shared A9 or PCIe file.
- **0036:** remove redundant register-list constraints exposed by dt-schema.
- **0037:** LSCI uses direct firmware loading; missing calibration cannot enter
  sysfs fallback and stall boot.
- **0038:** J71t HCD candidates also use direct loading, fixing a second
  observed 60-second helper wait. Retains and documents the already-active
  transmit-power quirk; its core warning is expected.
- **0039:** hide cache-only design charge when no valid static value exists;
  the power-supply core still supports monitored-battery data.
- **0040:** J71t Tristar source-presence binding and required charger supplier.
- **0041:** dedicated J71t policy with private CBAT, explicit measured-source
  2400-mA request, capped 2500-mA battery current, battery/VBUS guards,
  checked writes and rollback. Read-only CBTL1610 supplier uses verified
  STOP-separated selector/read transactions. J172 remains separate.
- **0042:** J71t-only Tristar node at I2C0 0x1a and charger supplier reference.

- **0043:** PMIC/NVMEM bindings permit address-only register ranges with a
  separate explicit NVMEM size; legacy layouts remain valid.
- **0044:** NVMEM provider accepts the new format and retains legacy support.
- **0045:** Tristar supplier phandles and opt-in USB host-budget binding.
- **0046:** board-gated Apple host current requests, bounded signed allowance,
  notifier/query API, persistent bus-session state and lifecycle revocation.
- **0047:** J71t boot identity from DT or private BMac, programmed in controller
  RAM and read back before default-address validation.
- **0048:** TX-power workaround diagnostic moves to debug; command skip stays.
- **0049:** automatic source-aware charging, private-CBAT budget calibration,
  bounded Tristar/ADC signature detection, retained guards, checked rollback,
  latched hardware faults and desktop source telemetry.
- **0050:** self-contained J71t PMIC address-cell fix and source-budget wiring;
  all child offsets/lengths preserved, NVMEM read-only.

- **0051:** nominal 500-mA fallback now permits a 400-mA battery ceiling.
  Positive input-limit writes again explicitly authorize the owner's measured
  source above its detected allowance; all battery/VBUS/transaction guards
  remain, with override revocation and checked rollback.
- **0052:** unavailable optional cached charge-now samples return ENODATA,
  allowing other battery uevent fields to publish; valid/recovered samples,
  raw cache diagnostics and mandatory errors remain unchanged.

- **0053:** cache-only first-sample readiness, ENODATA while presence/safety
  measurements are unknown, one-second warmup retry and preserved runtime
  errors/normal refresh after readiness.
- **0054:** unavailable power-supply temperature maps to quiet thermal EAGAIN
  retries without fabricated temperature or suppression of genuine errors.
- **0055:** startup battery/provider no-data defers rather than latches the
  J71t policy; bounded decision logs and existing one-second reevaluation.

**Round 6 verified offline:** clean kernel/104 DTBs, exact 55-patch replay,
strict checkpatch for patches 53–55, actual charger/cache/uevent/thermal
ASan/UBSan tests including 65 no-data policy ticks and unassisted activation
once a valid sample arrives. Real PMIC/readback/timeout faults remain guarded.
**Verified on integ10:** owner 2400-mA charging around +1.16 A and valid
charge-now; **pending device verification:** automatic 475000 / 400000 limits
and quiet startup. Report: [Round 6](../../docs/periph/round6.md).

**Round 5 verified offline:** full kernel/104 DTBs, exact 52-patch replay,
clean strict checkpatch for patches 51/52, actual-function charger and battery
ASan/UBSan regressions. **Pending device verification:** 400-mA fallback
charging, successful owner override and quiet/recoverable charge-now reporting.
Report: [Round 5](../../docs/periph/round5.md). Round 4 device evidence confirms
real BT identity from boot and absence of the PMIC bad-cell-count warnings.

**Verified device baseline (Round 3 integration):** manually selecting the
2400-mA input budget produced about +1.17 A charging; FearA USI build 1312
loaded and LE discovery worked. **Verified offline for Round 4:** full kernel
and 104 DTBs build, all 50 patches replay exactly, scoped schemas/DTs pass,
and patches 43–50 have zero strict checkpatch errors/warnings/checks.
Actual-C-function sanitizer tests cover charging guards/calibration/rollback,
160 USB assertions, 114 Bluetooth assertions, all 4096 ADC raw values and
13 mux fault paths. See [Round 4 report](../../docs/periph/round4.md).

Automatic Mac high current still requires an observed host grant; no passive
host-identification shortcut was verified. Round 5 restores explicit owner
sysfs authorization for the known 2400-mA Mac source. The ordinary fallback
remains nominal 500 mA (calibrated input 475000 µA on this unit), with a nonzero
400-mA battery ceiling. Actual net current depends on system load. Historical
patches 40/41 retain the generic new-file MAINTAINERS reminder.

Original authorship is retained in imported mail patches. CT821 source was
adapted to the newer Hoolock baseline; the original template's runtime-PM
helper is retained. The source of template patch 0027 is explicitly recorded
in its commit message. Local A9 additions are experimental, not hardware-
validated upstream support.

In a clean integration tree, the orchestrator can apply with:

```sh
git am /work/patches/periph/0*.patch
```

Then merge `scripts/periph/kernel.config` on top of the existing 16K config;
the host helper `scripts/periph/configure-kernel.sh` does this for the
isolated `periph` worktree. Always build in Docker.

`scripts/periph/export-patches.sh` re-exports and replays the series using
an alternate Git index. It refuses a dirty kernel worktree and does not
commit, reset, or check out the host repo or any other kernel worktree.
