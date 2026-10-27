# Round 7 warm chain: rescue on the 3rd round-7 boot (2026-10-23 ~19:10)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Photo: [2026-10-23-round7-warm3-rescue.jpg](../release/OMISSIONS.md).
No serial/ECM capture: the USB console didn't come up in the rescue shell, so
the photo is the only record of the failing boot. Healthy boots of the same
entry are in build/boot/warm-chain-r7-1/boot01 and warm-chain-r7-2/boot01
(dmesg, cmdline, interrupts).

## Sequence

> Private graphics-dependent excerpt omitted from this source snapshot.

## Key lines, transcribed from the photo (incoming, failing boot)

Early (healthy-looking):
```
pcie-apple-h9p 610000000.pcie: port 1: link up (LTSSM 0x11)
nvme 0000:01:00.0: Apple ANS: 100ms after CC.EN, no host-side fault; polling CSTS
nvme 0000:01:00.0: host after watch: rc ctl 0x1000011 pwr 0x101 refclk 0x1 rdy 0x1 stat 0x11; port linksts 0x5 cdm 0x702200 irq 0x3000/0x2f0f msi 0x31/0x0 en 0x80704c4b pmgr 0x0
nvme 0000:01:00.0: host after watch: invalidate FIFO 0x101 mode 1 poisoned 0 used 0,00000000
nvme 0000:01:00.0: host after watch: nvmmu tcb 0x8:0cf08000 scratch 0x8:02180000 sart 0x1 0xbc000000..0xbd100000 pa 0x87ee: err 0x0 0x0 0x0 0x0:00000000 0x0 0x0
nvme 0000:01:00.0: host after watch: dart tcr 0x80 err 0x100 addr 0x1e0f000 diag 0x100 reflector 0x20fffc fetch 0xe0303 ttbr 0x8000cebc 0x8000c8bc 0x80007cb4 0x8000c8ac
nvme nvme0: passthrough uses implicit buffer lengths
nvme nvme0: write window: controller ONCS 0x4 VWC 0x1 DNRSL 0; trim and write zeroes not advertised
nvme nvme0: 1/0/0 default/read/poll queues
nvme0n1: p1 p2                                   (at ~11.73 s)
pci 0000:00:01.0: removing 2.5GT/s downstream link speed restriction
pci 0000:00:01.0: retraining failed
OF: /soc/pcie@610000000: no iommu-map translation for id 0x8 on (null)
pci 0000:00:01.0: bridge configuration invalid ([bus 00-00]), reconfiguring
pci 0000:02:00.0: [14e4:43dc] type 00 class 0x028000 PCIe Endpoint
```
Rescue (~40 s; snapshot printed ~50.9 s):
```
[omarchy-rd]: ######## RESCUE: no usable NVMe root ########
linux-a9: ANS snapshot mode=host boot_id=c50e7cc8-0055-4ccb-b9f2-05bd5c70ed08
nvme 0000:01:00.0: Apple ANS: snapshot rescue state 6 online 0 allocated 2 vectors 1 recovery_used 0
nvme 0000:01:00.0: host rescue: rc ctl 0x1000011 pwr 0x101 refclk 0x1 rdy 0x1 stat 0x11; port linksts 0x5 cdm 0x702200 irq 0x3000/0x2f0f msi 0x31/0x0 en 0x80704c4b pmgr 0x0
nvme 0000:01:00.0: host rescue: invalidate FIFO 0xa01 mode 1 poisoned 0 used 0,00004000
nvme 0000:01:00.0: host rescue: nvmmu tcb 0x8:0c954000 scratch 0x8:00180000 sart 0x1 0xbc000000..0xbd100000 pa 0x87ee: err 0x0 0x0 0x0 0x0:00000000 0x0 0x0
nvme 0000:01:00.0: host rescue: dart tcr 0x80 err 0x500 addr 0x3bfff0d0 diag 0x100 reflector 0x20fffc fetch 0xe0303 ttbr 0x8000cb50 0x8000cbcc 0x8000c... 0x8000cc800   (ttbr digits partly unreadable)
nvme 0000:01:00.0: Apple ANS: q0 flags 0x0 depth 32 SQ tail 14/14 CQ head 14 phase 1 next status 0x0 cid 0x0 sqhead 0 vector 0 irq -1 count 0 SQ 0x00000000bbffe000 CQ 0x00000000bbfff000
nvme 0000:01:00.0: Apple ANS: q1 flags 0x0 depth 36 SQ tail 5/5 CQ head 5 phase 1 next status 0x0 cid 0x0 sqhead 0 vector 0 irq -1 count 0 SQ 0x0000bbffa000 CQ 0x0000bbffb000
/proc/interrupts: 34 AIC-FIQ arch_timer 18047 37310; 36 AIC 0 0 65701 Level apple-dart fault handler; 37 AIC 0 0 65704 Level apple-dart fault handler; 38 arm-pmu
cmdline adds: nvme.apple_ans_debug=1 nvme.apple_ans_rdy_timeout=5 nvme_core.io_timeout=5 nvme_core.admin_timeout=10 nvme_core.max_retries=1 apple_boot_watchdog.timeout=120 softlockup_panic=1 nmi_watchdog=panic watchdog_thresh=30 crash_kexec_post_notifiers
```

## First reading (orchestrator, unverified)

- DART `err` changed from `0x100` (healthy, early) to **`0x500`** with
  **`addr 0x3bfff0d0`**. The admin CQ is IOVA `0xbbfff000` (DART-side
  `0x3bfff000`), so `0x3bfff0d0` is admin CQ entry 13 (16-byte entries): a
  likely DART fault on the controller's completion write. Both apple-dart fault
  IRQs show count 0, so the fault was never delivered.
- Invalidate FIFO went from `0x101` to `0xa01`, with used bitmap `0x4000`
  (bit 14) still set; nvmmu TCB and scratch addresses differ from the early
  line (different allocation on this boot is expected).
- Both queues show SQ tail == CQ head (q0 14/14, q1 5/5) with `irq -1 count 0`.
- No retained ramoops/pstore record was read (the device went to DFU next).
