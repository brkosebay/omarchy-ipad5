# Round 1 B1 hang: analysis and round-2 fixes

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


## What happened (device, round 1)

The B1 payload (prebuilt m1n1, `nvme.apple_ans_dart_hmb=1`) brought the
link up and enumerated the S3E. It then printed, in order:

    Apple ANS: core_mask=0x3 log_size=0xff hmb_req=0xb00000 align=0x1000
    Apple ANS: HMB 0xb00000 at DART DMA 0x00000000bb000000
    Apple ANS: LTR cap at 0x178, L1SS cap at 0x180

and nothing after that. `/init` never ran, so there was no shell.

The round-1 code ran this sequence between the first and last lines:
1. Allocate the DART-mapped HMB.
2. "Reset DMA": write each of the 36 FlatDMA tags to NVMMU+0x4, back to back,
   reading NVMMU+0x4 back after every write.
3. Print the LTR/L1SS line, then write the LTR and L1SS registers in the S3E's
   config space.
4. Write CC=0 and poll CSTS (bounded at 2 s).
5. Write AQA/ASQ/ACQ, 0x1800 and the HMB registers, set CC.EN, and wait for
   CSTS.RDY for up to CAP.TO.

There was no "controller did not stop" message (that would have printed after
2 s). Round 1 had no log line between that point and the end of step 5, so two
readings fit the photo equally well. **Neither is verified:**

- **(a) A wedged CPU**: a non-posted access (the config writes, or a
  CSTS/BAR read) never completed.
- **(b) No wedge**: the controller never set CSTS.RDY, and Linux was still in
  `nvme_enable_ctrl()`'s CAP.TO wait (A10's CAP.TO = 0xf0, i.e. 120 s) when
  the photo was taken. Boot carried on on the other CPU (the interleaved PMIC
  `OF:` lines), but `/init` waited for the async probe either way.

How to tell them apart: the round-1 kernel had RCU stall warnings at 21 s,
so (a) should have printed `rcu: INFO: rcu_preempt detected stalls` within
about a minute. Under (b), `nvme nvme0: Device not ready; aborting
initialisation` would appear after about 2 minutes. **The follow-up photo of
the round-1 B1 boot, if one was taken after a few minutes, settles this.**
Round 2 settles it anyway: it logs `enabling controller (RDY timeout Ns)`
before the wait, and has hard-lockup, RCU, hung-task and workqueue
detectors.

## Why the shell never came up (verified)

`nvme_probe()` ran the whole controller enable synchronously. nvme-pci probes
asynchronously, and `kernel_init()` calls `async_synchronize_full()` before it
starts `/init`. So any hang in the bring-up, and even a long CAP.TO wait,
blocked the shell. The config also had no lockup or stall detectors.

## What iOS does differently (verified in the iOS 16.7.10 iPad6,11 kernelcache)

| | Round-1 Linux | iOS | Evidence |
|---|---|---|---|
| NVMMU invalidate, `invalidate-fifo-workaround = 1` | 36 invalidates back to back, each read back, including tags that were never mapped | under a lock, spin until **port0+0x22c bit 0 = 1 (FIFO empty)**, IODelay(1) per loop, panic after 20 ms; then write the tag to NVMMU+0x4; **no read-back**. The invalidate's only caller is the unmap path, so a tag is invalidated only after it was mapped | `fffffff005eebc9c`; its only xref is from unmap `fffffff005eeb9e8` (slot 171); accessors `fffffff005eec090` (read via `+0xe8` = reg[1], the port-0 block) and `fffffff005eebfe8` (write via `+0xd8`, NVMMU). The mode comes from the ADT in AppleNVMeMMU::start `fffffff005ee8bf0` |
| Is the workaround active on this iPad? | not handled | **yes.** The IPSW ADT lacks the property, but iBoot adds `invalidate-fifo-workaround = 1` to the live ADT | `docs/pcie/live-adt-decode.txt` (from `/dev/mtd`) |
| HMB ("scratch buffer") | DART-mapped kernel buffer at IOVA 0xbb000000 | `nvme-scratch-virt-region` = {0xbc000000, 0x1200000} (the NVMMU SART window over iBoot's `sart-region` 0x87ee00000/0x1200000). The DART fallback exists in code but isn't the normal path | live ADT; `fffffff006780098` (PreCCEnableHook: 0x1800 = 0, then 64-bit 0x1810 = base, 0x1818 = size); `fffffff00677f9b4` |
| ASPM | Linux turned **L1 on** by itself (`ASPM: default states L1`: DT platforms default to L0s/L1) | L1 together with L1SS, LTR and the CLKREQ# peripheral function, plus refclk gating in the RC | A0 dmesg; `drivers/pci/pcie/aspm.c` `pcie_aspm_override_default_link_state()`; ADT `pci-aspm-default` |
| RC/PHY/port tunables | none | `apcie-phy/common-tunables` at common init, `pcie-rc-tunables` and `apcie-config-tunables` per port | round-1 notes Q3; the values are in the live ADT |
| Controller enable | CC=0, wait RDY=0; AQA/ASQ/ACQ; 0x1800/0x1810/0x1818; CC = 0x460001; wait RDY=1 | **same order and same CC value**: IOSQES = 6 (`mov w0,#6` at vtable+0x6c0), IOCQES = 4, MPS/CSS/AMS = 0 | `fffffff0067a16bc` (enable), `fffffff0067a3a50` (admin queues), vtable `fffffff006f6dd70` + 0x6c0/0x6e8 |

## Root cause

**Inferred, most likely first:**

1. **NVMMU invalidate-FIFO overrun.** iOS treats a FIFO that doesn't drain
   within 20 ms as fatal, and iBoot turns the workaround on for this exact
   chip. The round-1 code fed it 36 invalidates without waiting, plus
   read-backs iOS never does. If the NVMMU wedges, it sits in port 0's
   inbound path, so completions for the very next endpoint access (the L1SS
   config write or the CSTS read) never return, and the CPU stalls. That
   matches the timing exactly: the last line before the hang is the one right
   after the invalidates.
2. **ASPM L1 turned on by Linux.** The link could enter L1 while the 11 MB HMB
   was being allocated. If L1 exit doesn't work in our configuration (no
   L1SS/LTR/CLKREQ# setup), the next endpoint access would stall the same way.
   A0 doesn't rule this out: there, the endpoint was barely touched after ASPM
   came on.
3. **DART-mapped HMB.** This is a supported but unusual path. On its own it
   would more likely end in a CSTS.RDY timeout or a DART fault than in a CPU
   stall.

Under reading (b), cause 3 or a missing tunable becomes the lead suspect: the
firmware never finishing its init with that HMB.

Round 2 fixes all three (plus the tunables) at once. **If payload 1 works, the
cause is still not isolated.** The first A/B is payload 2: the DART HMB with
all the other fixes. The second clue is the tunables' `N of M changed`
counts. The per-step log lines and the lockup detectors say which step any
remaining problem is in.

**Not the cause (verified):** the NVMe protocol side. The CC value, admin
queue setup, 0x1800 and the HMB register order all match iOS.

## Round-2 changes

- `PCI: apple-h9p`:
  - iOS-exact FIFO-gated invalidate: polls at most 20 ms, logs instead of
    panicking, no read-back, never-mapped slots skipped.
  - Live-ADT tunables applied where iOS applies them, logging how many
    registers changed.
  - A bounded wait for the LTSSM to reach L0.
  - `apple_ans_link_up()` exported.
- `nvme-pci: apple-ans`:
  - A log line per step: CAP/VS/CC/CSTS, stop, HMB, enable with the RDY
    timeout.
  - A link-up check before each controller access.
- `nvme-pci`: the first enable of an ANS controller runs from `reset_work`
  (`nvme_reset_wq`), so `/init` never waits for it. On failure the
  controller goes DEAD.
- DT (j71t/j72t):
  - Tunables copied from the j71t live ADT.
  - `apple,nvmmu-invalidate-fifo = <1>`.
  - `aspm-no-l0s`/`aspm-no-l1` on both ports.
  - Port 1 (Wi-Fi) disabled.
- Config fragment, enforced and verified on every build by
  `scripts/pcie/build.sh`:
  - Buddy hard-lockup detector (10 s).
  - Soft-lockup detector (20 s).
  - RCU stall warning at 20 s.
  - Hung-task detector (30 s).
  - Workqueue watchdog (30 s).
  - All warn-only, so the other CPU keeps the shell alive.
- Main payload: m1n1 with the HMB patch, so the HMB is iBoot's SART buffer,
  as on iOS.

## Residual hazards

- The glue's link checks cover only its own steps. After a failed enable,
  nvme-core and nvme-pci still touch the endpoint on their own failure paths
  (CSTS reads, `pci_device_is_present()`, CC.SHN in `nvme_dev_disable()`). If
  the link or the inbound path is dead by then, those can stall a CPU too. The
  lockup detectors will show it.
- `reset_work` holds `shutdown_lock` while it waits for CSTS.RDY. So a
  `reboot` issued during that wait (up to CAP.TO) can block until it ends.

## Build bug (round 1)

The round-1 kernel lacked `CONFIG_PCIE_APPLE_H9P` (verified). The cause: my
bisect loop ran `make` at older commits in the shared tree. Kconfig's
syncconfig dropped the symbol, which didn't exist yet at those commits, and
it came back at the tip as "not set". `NVME_PCI_READ_ONLY` survived only
because it defaults to `y`.

Now:
- `scripts/pcie/build.sh` merges `patches/pcie/pcie.config` before every
  build and checks the merged and the built config.
- `scripts/pcie/mkpayload-pcie.sh` refuses to package a kernel whose config
  lacks any fragment line (`scripts/pcie/check-config.sh`).

## Wi-Fi (port 1): not done this round

Port 1 trains to LTSSM 0x0 (no receiver), because the BCM4355 isn't powered.
On A10, Pauli1Go's `dev` branch needs all of these:
- a D2365 PMU GPIO driver (`apple,d2365-pmic-gpio`, WLAN VDD on PMU GPIO 14
  on iPad 7) behind a fixed regulator;
- a wlan-uart platform provider (`apple,t8010-wlan-uart-provider` on uart2);
- PCI pwrctrl core changes plus `pci-pwrctrl-apple-wifi` (device-wake GPIO,
  power sequencing);
- several H9P link options (`apple,link-retry-after-power-cycle`,
  `apple,endpoint-power-after-link-setup`, ...).

For A9, the PMU GPIO that powers the BCM4355 still has to be identified. The
iOS device tree shows a PMU `event_name-gpio8 = wlan` (probably host-wake),
and the wlan-uart node has `function-pcie_port_control`. This is a multi-patch
port, so it's left for its own round.
