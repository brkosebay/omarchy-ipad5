# ANS warm-boot forensics: what changes from boot to boot, before the failure

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Analysis-only sub-agent report. No device access, no kernel edits, no commits.
Sources are the logs already on this Mac; every claim is marked **verified**
(read directly out of a named log/line) or **inferred**.

Scope: the Apple NVMe controller ("ANS", `pci-apple-ans.c` + `pcie-apple-h9p.c`,
NVMMU/HMB/SART + S5L8960X DART) across warm boots inside one DFU epoch. The
question is what drifts between boots *before* the failure, and what the
earliest usable predictor is.

---

## 0. TL;DR

**Verified**

> Private graphics-dependent excerpt omitted from this source snapshot.

**Inferred (highest-value)**

8. The failing boot's DART TTBR bases **changed** between `11.66 s` and `50.95 s`;
   in both healthy boots the four TTBR values are byte-identical across all four
   dumps of the boot. A DART (IOMMU domain) teardown/re-create happened in the
   failing boot only — i.e. the driver ran a reset path and the controller did
   not come back.
9. The failure point is the **first ext4 read after the partition scan**, the same
   place round 6 failed (`p1 p2` printed, mount never returns), not link training
   and not `CC.EN`/`RDY`.

---

## 1. Method and data inventory

Epoch = one DFU cold boot; `N` = index of a warm boot since that cold boot.
`N=1` is the first kexec after the cold boot.

> Private graphics-dependent excerpt omitted from this source snapshot.

Consequence, and the single biggest gap in the evidence base: **only two healthy
boots in the entire corpus carry the full ANS host + queue dump.** Every table
below that compares register values across boots has n = 3 (two logged, one
photo-transcribed).

### 1.1 Reproduction caveat found in the round-7 chain script

`build/boot/warm-chain-r7-1/summary.txt` ends with `FAIL boot 1: 4 NVMe/DART/NVMMU
error lines`. That is a **harness artifact, not a failure**:

```
$ BAD='I/O tag .* timeout|completion polled|nvme.*reset|poisoned [1-9]|...'
$ grep -cE "$BAD" build/boot/warm-chain-r7-1/boot01/dmesg-after-read.txt
0
```

The current `scripts/boot/warm-chain.sh:20` pattern matches nothing in that file,
while the saved summary counted `bad_msgs=4` and listed the four `invalidate FIFO`
lines. The script's `BAD` regex at run time was broader than the committed one.
`b968dbcb` (r7-1 boot 1) was a healthy boot: root rw, 416 MB/s, no timeout lines.
Any future counting must not treat that `FAIL` as a boot failure.

---

## 2. Boot ledger: how many warm boots each epoch survived

> Private graphics-dependent excerpt omitted from this source snapshot.

Totals: **~50 recorded warm boots, 5 failures** (r6 `N=4`, r7 `N=3`, night `N=4`,
r28 `N=5`, 18:36 capture-death `N=5`) ⇒ ~10 % per-jump hazard.

The important negative: failures are **not** concentrated where N is small or
large. An epoch of 18 boots survived intact, and the r7 epoch died at N=3. Boot
index is a weak correlate of cumulative exposure, not a mechanism.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

No workload class is present in every failing epoch, and every workload class is
also present in long surviving epochs (r25-b ran 18 stalled-TA boots). **Verified:
workload is not a necessary condition.** Any causal role is unproven.

---

## 3. The round-7 three-boot series (the only register series)

Epoch: DFU → aopint `b25061d6f8` → kexec `round7-trial-4e9a87e9ef`.
`N=1` = `b968dbcb`, `N=2` = `c6980a14`, `N=3` = `c50e7cc8` (failed).

### 3.1 Values printed before/at `CC.EN`

| Value | N=1 `b968dbcb` (`warm-chain-r7-1/boot01/dmesg.txt`) | N=2 `c6980a14` (`-r7-2/boot01/dmesg.txt`) | N=3 `c50e7cc8` (photo) |
| --- | --- | --- | --- |
| inherited NVMMU at port restart | `SART 0x0 TCB 0x0:00000000` :4624 | `SART 0x0 TCB 0x0:00000000` :6439 | not visible (above the fold) |
| NVMMU ready | `SART 0xbc000000 -> 0x87ee00000 size 0x1200000, invalidate-fifo 1` :4661 | same :6517 | same (cut) |
| `core_mask / log_size / hmb_req` | `0x3 / 0xff / 0xb00000` :4709 | same | same |
| `CAP / VS / CC / CSTS / BFH` | `0x20f00103ff / 0x10001 / 0x0 / 0x0 / 0xbadadd` :4712 | same :6633 | same |
| HMB | `0x1200000@0x87ee00000 as DMA 0xbc000000` :4714 | same :6639 | same |
| stop / rebuild | `controller stopped (CSTS=0x0), rebuilding DMA` :4720, `NVMMU: stopped-controller cache/SART rebuilt` :4722 | same :6651/:6655 | same |
| `AQA / ASQ / ACQ` | `0x1f001f / 0xbbffe000 / 0xbbfff000` :4724 | same :6659 | same |
| MSI cap | `flags 0x87 addr 0x0:bffff000 data 0x0; irq 69` :4725 | same :6663 | same |
| **rc ctl / pwr / refclk / rdy / stat** | `0x1000011 / 0x101 / 0x1 / 0x1 / 0x11` :4727, :4755 | identical :6669, :6747 | identical (photo) |
| **port linksts / cdm** | `0x5 / 0x702200` | identical | identical |
| **port irq / mask** | `0x3000 / 0x2f0f` | identical | identical |
| **msi ctl / vecbase** | `0x31 / 0x0` | identical | identical |
| **port enable / pmgr** | `0x80704c4b / 0x0` | identical | identical |
| FIFO before `CC.EN` | `0x1 mode 1 poisoned 0 used 0,00000000` :4730 | identical :6679 | identical |
| FIFO after watch | `0x101 … used 0,00000000` :4756 | identical :6750 | identical |
| **NVMMU tcb** | `0x8:07d40000` :4732 | `0x8:0cf88000` :6685 | `0x8:0cf08000` |
| **NVMMU scratch** | `0x8:00180000` | `0x8:02180000` | `0x8:02180000` |
| NVMMU sart / err | `0x1 0xbc000000..0xbd100000 pa 0x87ee; err 0x0 ×7` | identical | identical |
| DART tcr / diag / reflector / fetch | `0x80 / 0x100 / 0x20fffc / 0xe0303` | identical | identical |
| **DART ttbr** | `0x80807d60 0x80807c2c 0x8080cc48 0x80807c3c` :4734 | `0x8080cebc 0x8080c8bc 0x80807cb4 0x8080c8ac` :6689 | `0x8000cebc 0x8000c8bc 0x80007cb4 0x8000c8ac` **(digits uncertain, see §3.4)** |
| DART err / addr | `0x100 / 0x1e0f000` | `0x100 / 0x1e0f000` | `0x100 / 0x1e0f000` |
| ASQ IOVA → PA | `0xbbffe000 → 0x80c4ec000` :4735 | `0xbbffe000 → 0x80cf24000` :6697 | not visible |
| ACQ IOVA → PA | `0xbbfff000 → 0x80cc94000` :4743 | `0xbbfff000 → 0x807dcc000` :6710 | not visible |
| TTBR0 walk ASQ | `STE[0x1df] 0x807ce0003 → PTE[0x1fe] 0x80c4ec003` | `STE[0x1df] 0x80cd84003 → PTE[0x1fe] 0x80cf24003` | not visible |
| `100 ms` watch | `enabling 6.536963` → `100ms after 6.640551` = **103.6 ms** | `11.544347` → `11.650727` = **106.4 ms** | `11.5397` → `11.646030` = **~106 ms** |
| queues ready | `1/0/0 default/read/poll queues` 6.671217 | 11.698537 | not visible |
| partitions | `p1 p2` 6.681994 | 11.730729 | `p1 p2` ~11.73 |

### 3.2 Values printed at rescue (only the failing boot reached rescue)

| Value | N=1 (healthy, `dmesg-after-read.txt:4939-4953`) | N=2 (healthy, `:7277-7290`) | N=3 (failed, photo 50.94 s) |
| --- | --- | --- | --- |
| `snapshot rescue` | `state 1 online 2 allocated 2 vectors 1 recovery_used 0` (×2) | identical (×2) | **`state 6 online 0 allocated 2 vectors 1 recovery_used 0`** |
| FIFO | `0xa01 … used 0,00000000` (both dumps) | `0xc01 … used 0,00010000`, then `0xc01 … used 0,00000000` | **`0xa01 … used 0,00004000`** |
| DART err / addr | `0x100 / 0x1e0f000`, then `0x100 / 0x3bffc540` | **`0x500 / 0x3bffd0b0`**, then `0x100 / 0x1e0f000` | **`0x500 / 0x3bfff0d0`** |
| DART ttbr | unchanged from boot | unchanged from boot | **changed from its own early dump** |
| q0 | `flags 0x1 depth 32 SQ tail 20/20 CQ head 20 … irq 69 count 6379/6607 SQ 0xbbffe000 CQ 0xbbfff000` | same shape, `count 7322/7988` | **`flags 0x0 … 14/14 … irq -1 count 0`**, same IOVAs |
| q1 | `flags 0x1 depth 36 6/6 → 23/23 … irq 69 SQ 0xbbffc000 CQ 0xbbffd000` | `12/12` then `0/0`, same IOVAs | **`flags 0x0 … 5/5 … irq -1 count 0` SQ 0xbbffa000 CQ 0xbbffb000** |
| `/proc/interrupts` | `36/37 AIC … apple-dart fault handler` counts **0 0**; `69 PCI-MSI … nvme0q0,nvme0q1` count 6001 | same, count 6858 | **no line 69 at all**; dart fault counts `0 0` |

### 3.3 What is genuinely constant (verified invariants)

These never vary across boots, so they cannot be the drifting cause:

- HMB physical address `0x87ee00000`, size `0x1200000`, DMA alias `0xbc000000`
  (fixed by the DT `nvme-hmb@0` `no-map` reservation, `cold5b-dmesg.txt:13`).
- SART VA window `0xbc000000..0xbd100000`, PA base `0x87ee…`, `sart 0x1`, mode 1.
- PCIe port 0 register block: `ctl 0x1000011 pwr 0x101 refclk 0x1 rdy 0x1 stat 0x11`,
  `linksts 0x5`, `cdm 0x702200`, `irq 0x3000/0x2f0f`, `msi 0x31/0x0`,
  `en 0x80704c4b`, `pmgr 0x0` — identical in all three boots *and* at rescue.
- NVMMU error registers (`err 0x0 …`) always zero, including in the failing boot.
- `invalidate-fifo 1` mode, DART `tcr 0x80`, `diag 0x100`, `reflector 0x20fffc`,
  `fetch 0xe0303`.
- Inherited NVMMU at port restart is `SART 0x0 TCB 0x0:00000000` in both boots that
  show the line: the outgoing kernel's teardown (`ports powered`, SART retire)
  worked every time. **No inherited SART/TCB leak.**
- Number of MSI vectors (`1`, `SINGLE_VECTOR` quirk) and admin/I/O queue depth
  (`32`/`36`).

### 3.4 The only values that move, and what they are

1. **TCB base** `0x8:07d40000` → `0x8:0cf88000` → `0x8:0cf08000`.
2. **Scratch** `0x8:00180000` → `0x8:02180000` → `0x8:02180000`.
3. **ASQ/ACQ backing pages** `0x80c4ec000/0x80cc94000` → `0x80cf24000/0x807dcc000`.
4. **DART TTBR bases** (4 per boot).

All four are buddy-allocator results in the DMA zone
(`0x800000000..0x87bbe3fff`, `cold5b-dmesg.txt:22`) and all four are
non-monotonic. They move because the cold-boot memory map is not bit-identical
between kexecs (kernel/initrd placement, CMA, ramoops), not because the ANS
driver accumulates anything. Treating them as "reused physical addresses that
collide with the previous kernel's memory" is **not supported**: TCB/ASQ/ACQ are
freshly allocated and zeroed each boot (`NVMMU: stopped-controller cache/SART
rebuilt`), and the HMB/SART regions are reserved and never reused by Linux.

The `TTBR` register value prints as 8 hex digits and its low nibble is non-zero
(e.g. `0x8080cebc`), so it cannot be a raw page-table PA. `value << 4` lands
inside the DMA zone for both healthy boots (`0x8_080CEBC0`, `0x8_0807D600`),
which is consistent with a `PA >> 4` encoding (**inferred**).

Two cautions on the N=3 TTBRs:

- The photo transcription reads `0x8000cebc 0x8000c8bc 0x80007cb4 0x8000c8ac`,
  one digit shorter than N=2's `0x8080…`. `<<4` would then give `0x8_000CEBC0`,
  i.e. **inside the 16 MiB CMA reservation at `0x800000000`** — a different
  allocator region than either healthy boot. That would be a real, interesting
  difference (DART tables from CMA), but it rests on one unreadable digit in a
  JPEG.
- The rescue dump's TTBRs (`0x…cb50 0x…cbcc 0x… 0x…cc80`) differ from the same
  boot's early dump. That part is visible enough to trust, and it is the one
  TTBR fact I will use (§6).

---

## 4. Corrections to the first reading in `2026-10-23-round7-warm3-rescue.md`

The rescue doc is right about the address arithmetic and right to be cautious,
but four of its observations are **not** failure-specific. All four are refuted
by the two healthy boots of the same entry:

| Rescue-doc reading | Correction (verified) |
| --- | --- |
| "DART `err` changed from `0x100` … to **`0x500`** … a likely DART fault on the controller's completion write" | `0x500` also occurs in the healthy N=2 boot at 33.770411, with an address in the *I/O* CQ (`0x3bffd0b0`), and clears to `0x100/0x1e0f000` 0.63 s later (:34.404327). `0x100` is the stale pre-Linux value documented in `docs/pcie/enable-freeze.md` ("error 0x100 … left over from before Linux attached"); the *address* latches even with `err 0x100` (N=1 shows `0x100 / 0x3bffc540`). A CQ-window address is therefore normal; **persistence** is the anomaly, and persistence cannot be judged from a single snapshot. |
| "Invalidate FIFO went from `0x101` to **`0xa01`**" | `0x022c` bit 0 is `EMPTY` (`patches/pcie/kernel/0013-*.patch`, `S8003_PORT_NVMMU_INV_FIFO_EMPTY BIT(0)`). `0xa01` and `0xc01` both have bit 0 set: the FIFO was empty. Healthy N=1 prints `0xa01` twice, N=2 prints `0xc01` twice. The low bits above bit 0 are undocumented status, not a drain indicator. |
| "used bitmap `0x4000` (bit 14) still set" | This is the only FIFO-adjacent datum that means something, and even it is not unique: healthy N=2 shows `used 0,00010000` (bit 16) at 33.770378 and `0` at 34.403584. A *set bit at a single instant* is a mapping in flight. What differs in N=3 is that bit 14 was set with **no I/O in progress and no completion ever arriving**. |
| "Both apple-dart fault IRQs show count 0, so the fault was never delivered" | `36/37` are `0 0` in **every** boot, healthy included (`warm-chain-r7-1/boot01/interrupts.txt:3-4`, `-r7-2/…:3-4`). The DART error register latches without an interrupt on this part; a zero count is not evidence. |

Net effect: `err 0x500`, `addr 0x3bffXXXX`, `FIFO 0xa0X`, and `irq count 0` are
all **background**. The rescue doc's conclusion — "the physical root cause remains
unconfirmed" — stands, and is strengthened.

---

## 5. What the failing boot actually shows (queue forensics)

From the photo, at 50.94 s, `state 6`, `online 0`:

```
Apple ANS: q0 flags 0x0 depth 32 SQ tail 14/14 CQ head 14 phase 1 next status 0x0
           cid 0x0 sqhead 0 vector 0 irq -1 count 0 SQ 0xbbffe000 CQ 0xbbfff000
Apple ANS: q1 flags 0x0 depth 36 SQ tail  5/5  CQ head  5 phase 1 next status 0x0
           cid 0x0 sqhead 0 vector 0 irq -1 count 0 SQ 0xbbffa000 CQ 0xbbffb000
```

Reading the print site (`patches/boot/kernel/0018-*`, `nvme_apple_ans_dump()`):

- `flags` bit 0 is `NVMEQ_ENABLED`; `irq` is computed only when it is set. So
  `flags 0x0 / irq -1` = **the queues were shut down** (`nvme_dev_disable()`),
  not "IRQ never delivered".
- `SQ tail == CQ head` on both queues ⇒ **no command was outstanding** at the
  snapshot. The admin queue completed 14 commands, the I/O queue 5.
- `state 6` is `NVME_CTRL_DEAD` in the upstream enum order (`NEW, LIVE, RESETTING,
  CONNECTING, DELETING, DELETING_NOIO, DEAD`) — **inferred from the enum, not
  from a log**; `online 0` and the disabled queues corroborate it.
- `recovery_used 0` (the `apple_ans_flags` bit-0 automatic-reset quota) means the
  "one automatic controller reset" path was **not** what ran.
- `num_vecs 1`, `queue_count 2` — the I/O queue *was* created and *did* run reads.

Combined with the boot log in the same photo (`p1 p2` at ~11.73 s), the sequence is:

1. `CC.EN` → ready in ~106 ms, exactly like the two healthy boots.
2. Partition scan of `nvme0n1` completes: 5 I/O commands, all with completions.
3. The first ext4 read (mount) never returns; after the 5 s `io_timeout` /
   10 s `admin_timeout` policy the driver enters a reset path.
4. The reset does not restore the controller; the driver disables it (DEAD) and
   PID1 eventually falls back to rescue at ~40 s.

Step 3 is the same failure point as round 6's fifth boot, where
`nvme0n1: p1 p2` existed but `mount` of `nvme0n1p2` never produced an EXT4
message (`docs/boot/ans-kexec.md` round 7 §"Evidence and limits"). Two independent
boot rounds, same signature: **enumeration works, the first filesystem read does
not.**

A detail worth recording, with low confidence: the failing boot's I/O queue landed
at `SQ 0xbbffa000 CQ 0xbbffb000`, i.e. **8 KiB lower** than in both healthy boots
(`0xbbffc000/0xbbffd000`). An IOVA allocator that had already handed out the
`…c000/…d000` pair to a previous mapping would produce exactly this shift, which
is weakly consistent with the leaked tag 14 in the same dump. It is equally
consistent with a different mapping order in the reset path. **Inferred, not
established.**

---

## 6. Drift analysis, by candidate

| Candidate drift | Verdict | Evidence |
| --- | --- | --- |
| HMB address / size / DMA alias | **no drift** | `0x1200000@0x87ee00000 as DMA 0xbc000000` in every round-6/7 boot |
| SART window / mode / inherited SART | **no drift**; teardown clean every boot | `ANS inherited NVMMU: SART 0x0` :4624, :6439 |
| PCIe link state (LTSSM, linksts, cdm, msi, en) | **no drift** | identical in all three boots and at rescue |
| NVMMU error registers | **no drift** (always 0) | `err 0x0 0x0 0x0 0x0:00000000 0x0 0x0` |
| DART err/addr | **no predictive drift**; `0x500`, CQ-window addresses and `0x100` all appear in healthy boots | §4 |
| FIFO status word | **no predictive drift**; bit 0 = EMPTY in all samples | patch 0013 |
| FIFO `used` bitmap | **transient in healthy boots**; a set bit at rest is abnormal but not unique (bit 16 in N=2) | §3.2 |
| NVMMU TCB / scratch PA | moves, non-monotonic, allocator-dependent | `0x07d40000`, `0x0cf88000`, `0x0cf08000` |
| DART TTBR bases | move per boot (allocation); **and change *within* the failing boot only** | §3.4, §6.1 |
| ASQ/ACQ backing PAs | move, non-monotonic | `0x80c4ec000/0x80cc94000` vs `0x80cf24000/0x807dcc000` |
| `CC.EN`→watch latency | **no drift**: 103.6 / 106.4 / ~106 ms | §3.1 |
| partition-scan latency | +0.146 s → +0.186 s (N=1→N=2) | 6.682→7.035 vs 11.731→12.831 |
| ext4 mount latency | **+0.353 s → +1.100 s** (3.1×) | `EXT4-fs … mounted` 7.050 vs 12.857 |
| 64 MiB direct read | **416 MB/s → 405 MB/s** (−2.6 %) | `summary.txt` in both chain dirs |
| bytes of dmesg / uptime at capture | 25.9 s → 32.9 s | `dmesg.txt` last timestamps |

### 6.1 The one within-boot change that is failure-specific

In both healthy boots the four DART TTBR values are **identical** in all four
dumps of the boot (early, after-watch, and both rescue snapshots):

```
N=1: 0x80807d60 0x80807c2c 0x8080cc48 0x80807c3c   (:4734, :4758, :4943, :4951)
N=2: 0x8080cebc 0x8080c8bc 0x80807cb4 0x8080c8ac   (:6689, :6753, :7281, :7288)
```

In the failing boot they differ between the early dump and the rescue dump
(`0x…cebc …` → `0x…cb50 0x…cbcc … 0x…cc80`). A DART table base only changes when
the IOMMU domain is torn down and re-attached — i.e. during a controller reset.
So the failing boot **did** run a reset/remap between 11.7 s and 50.9 s, and the
controller stayed dead afterwards. This is consistent with, and independent
evidence for, the sequence in §5. It is a *consequence* marker, not a precursor.

### 6.2 Mount latency and read throughput

The only quantitative "slowing down" signal in the corpus: first-mount latency
tripled and direct-read throughput fell 2.6 % between round-7 boots #1 and #2,
immediately before the #3 failure. Two caveats keep this at "suggestive":

- ext4 mount time includes journal replay; boot #2 mounts an fs dirtied by boot
  #1's kexec, so part of the 0.35 s → 1.10 s growth is software work, not the
  controller.
- Round 6 shows **no** such growth between its boots #2 and #3
  (`13.500→13.861` = 0.360 s, `22.225→22.580` = 0.355 s; partition scan
  +0.209 / +0.207 s).

So the trend exists in the one epoch that then failed, and is absent in an epoch
that also later failed. Treat as a hypothesis to measure, not a result.

---

## 7. Earliest predictor of failure

**Verified answer: none of the printed ANS/DART/NVMMU/PCIe values predicts it.**
For the failing boot, every value observable before `CC.EN` and 100 ms after it is
bit-identical to the previous, healthy boot. The earliest *observable* deviation
is the boot's own controller teardown (state 6, queues disabled, `irq -1`) — which
is already the failure, not a predictor.

What *is* available earlier, in order of usefulness:

1. **The first `Apple ANS: timeout tag … cid … qid … opcode …` line.** It is
   printed *before* the first CSTS read (`patches/boot/kernel/0018-*`,
   `nvme_timeout()`), so it survives even a wedged endpoint. It names the exact
   command that stopped completing. Its presence/absence and its `qid/opcode`
   separate "admin command after the reset" from "first data read". **This line
   is not in any capture we have** — it scrolled above the photo.
2. **The `RDY` deadline message** (`nvme.apple_ans_rdy_timeout=5`), which would
   prove the reset re-enable failed rather than the reset stop. Also not captured.
3. **A per-boot host snapshot on healthy boots** (`echo host >
   /sys/class/nvme/nvme0/apple_ans_diag`, cheap, host-only, no endpoint MMIO).
   With n=2 samples today, a drift test is impossible; 20 samples from the
   round-7 20-boot gate would make the `used`-bitmap / TTBR / mount-latency
   series statistically usable.
4. **Statistical exposure, not a value**: ~10 % per jump, independent of N
   (§2). For planning, budget ~1 failure per 10 warm jumps regardless of epoch
   length — an 18-boot epoch is possible (r25-b) and so is a death at N=3 (r7).

---

## 8. The single most informative next device capture

**Capture the failing boot's own kernel log from the rescue shell, before any
recovery or DFU.**

Concretely, on the first rescue of the next round-7 chain, over ACM or
`telnet 172.16.42.1` (both are up — the helper printed the prompt), run and save
off-device, in this order:

```sh
dmesg | grep -nE "Apple ANS|nvme|NVMMU|dart|CC.EN|CSTS|deadline" > /tmp/fail.txt
cat /tmp/fail.txt
cat /sys/fs/pstore/console-ramoops-0        # primary ramoops console, if readable
/linux-a9-ans-state                          # host mode; already in the Round 7 initramfs
```

then, only after those are off the device, `/linux-a9-ans-state --endpoint` once.

Why this and not something else: the photo proves the machine reached a working
shell and printed the *state* snapshot, but the two lines that identify the
mechanism — the timeout line (tag/CID/QID/opcode) and the reset/RDY outcome — are
above the visible region. Every other candidate capture (endpoint dump, DART
registers, interrupt counts) has already been shown in §3–§6 to be either constant
or ambiguous between healthy and failing boots. The retained `console-ramoops-0`
matters because the primary kernel's console is preserved and the round-7
initramfs already copies and prints it; the failing boot did have the 2 MiB
reservation (`j71t pstore: reserved`), so its own console *was* retained until the
DFU discarded it.

Second priority, cheap and additive: extend `scripts/boot/warm-chain.sh` to take
`echo host > /sys/class/nvme/nvme0/apple_ans_diag` **before each kexec** and once
after root, so every boot contributes a row to the `used`-bitmap / TTBR /
mount-latency series. Without that, the next analysis will again have n=2.

---

## 9. Reproduce (read-only, host only)

> Private graphics-dependent excerpt omitted from this source snapshot.


The failing-boot numbers in §3.1/§3.2 are transcribed from
`docs/boot/2026-10-23-round7-warm3-rescue.jpg`; I re-read the photo and OCR'd it,
which confirms `err 0x500`, `state 6 online 0`, `q0 … 14/14 … irq -1 count 0` and
`q1 … 5/5 … irq -1`, and leaves the TTBR digits ambiguous.

---

## 10. Confidence summary

> Private graphics-dependent excerpt omitted from this source snapshot.

