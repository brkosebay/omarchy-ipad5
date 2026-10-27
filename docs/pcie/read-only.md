# How the pcie kernel keeps the iPad's NVMe read-only

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


The internal NVMe disk holds the user's iOS install and there is no backup.
The pcie kernel (branch `a9/pcie`) must not be able to change it. Read-only is
enforced in the kernel itself, in three layers, plus (since round 6) a fourth
that permanently protects the system namespaces (layer 4 below). None of them can be turned
off at runtime: there is no module parameter, sysfs knob or kernel
command-line switch.

All three come from patch `nvme-pci: Add a read-only mode for Apple iDevice
bring-up` and are compiled in by `CONFIG_NVME_PCI_READ_ONLY=y` (Kconfig
default `y` on `ARCH_APPLE`, also pinned in `patches/pcie/pcie.config`).

## Layer 1 (the real gate): command allowlist before submission

`drivers/nvme/host/pci.c`, `nvme_pci_cmd_allowed()`, is called from
`nvme_prep_rq()` right after `nvme_setup_cmd()` has built the 64-byte NVMe
command. If the command is not on the allowlist, the request fails with
`BLK_STS_IOERR` (userspace sees `EIO`) and nothing is written to the
submission queue. The kernel logs (rate-limited):

    nvme nvme0: read-only: refused opcode 0x1 on I/O queue

Every command nvme-pci sends goes through `nvme_prep_rq()`: filesystem and
block-device I/O, discard/trim, write-zeroes, internal admin commands from
nvme-core, and user passthrough (`NVME_IOCTL_ADMIN_CMD`, `NVME_IOCTL_IO_CMD`,
io_uring passthrough, `nvme-cli`). The batched path (`nvme_queue_rqs`) also
calls `nvme_prep_rq()`.

Allowed on **I/O queues**: `Read (0x02)`, `Flush (0x00)`. Nothing else, so
Write, Write Zeroes, Write Uncorrectable, Dataset Management (trim), Copy,
Verify, Compare, reservations, zone commands and vendor-specific opcodes are
refused.

Allowed on the **admin queue**: Delete/Create I/O SQ and CQ, Get Log Page,
Identify, Abort, Get Features, Asynchronous Event Request, Keep Alive, and
Set Features only for the volatile feature IDs nvme-core uses: Power
Management (0x02), Temperature Threshold (0x04), Number of Queues (0x07),
Interrupt Coalescing (0x08), Async Event Config (0x0b), APST (0x0c),
Timestamp (0x0e), Keep Alive Timer (0x0f), Host Behavior (0x16). Set Features
with the Save bit (CDW10 bit 31) set is refused. Everything else is refused:
Format NVM, Sanitize, Firmware Image Download/Commit, Namespace
Management/Attachment, Security Send, Directive Send, Device Self-test,
Doorbell Buffer Config, Virtualization Management and all vendor-specific
admin opcodes (0xc0 and up).

## Layer 2 (backstop): the submission-queue copy

`nvme_sq_copy_cmd()` is the only function that copies commands into the
submission queue memory the controller reads. It runs the same allowlist
check again. If a refused command ever got this far (meaning layer 1 has a
bug, or a new code path skipped `nvme_prep_rq()`), the command is replaced
by a harmless one (Flush on I/O queues, Get Features/Number of Queues on the
admin queue) before the copy, and the kernel prints one
`WARN: nvme: read-only: neutralized opcode ...` with a backtrace. The only
caller that skips `nvme_prep_rq()` today is the Asynchronous Event Request,
which is allowed.

## Layer 3 (advisory): read-only gendisks

`nvme_ns_is_readonly()` in `drivers/nvme/host/core.c` returns true for every
namespace of a controller with `NVME_CTRL_FORCE_RO`, which nvme-pci sets on
every controller when the option is on. nvme-core re-evaluates this on every
namespace revalidation, so it can't flip back to read-write. The effect:

- `/sys/block/nvme0n*/ro` reads `1`, and `blockdev --getro` reports 1.
- Opening a namespace or partition for writing fails with `EACCES`
  (`block/bdev.c`).
- `blockdev --setrw` only clears the partition flag. `bdev_read_only()`
  still sees the disk flag.

The block layer alone isn't enough: `bio_check_ro()` only warns about
writes to a read-only bdev and lets them through. Layer 1 is what actually
stops them.

## Layer 4 (round 6): the system namespaces are write-protected

NVMe works since round 5. The j71t S3E exposes seven namespaces, read and
sha256-verified on the device (`docs/test-logs/integ10/`):

| NSID | contents | |
|---|---|---|
| 1 | iOS disk0, the APFS container (32 GB) | data |
| 2 | LLB/iBoot (IMG4), 8 MiB | boot firmware |
| 3 | syscfg (`gfCS`), factory calibration | must never be written |
| 4 | iPCB | must never be written |
| 5 | NVRAM | must never be written |
| 6 | effaceable storage (`ecaF`) | must never be written |
| 7 | unused (zeros) | |

Patch `nvme-pci: apple-ans: Write-protect the system namespaces` adds a gate
that is **independent of `CONFIG_NVME_PCI_READ_ONLY`**, so it stays when
media writes to NSID 1 are enabled one day:

- `dma_ops->cmd_allowed()` (`nvme_pci_apple_ans_cmd_allowed()`) runs in the
  same two places as layer 1/2 (`nvme_prep_rq()` and `nvme_sq_copy_cmd()`):
  - **I/O queues:** anything for NSID 1; for any other NSID only Read and
    Flush.
  - **Admin queue:** queue management, Identify, Get Log Page, Get
    Features, Abort, AER and Keep Alive.
  - **Set Features:** only controller-wide (NSID 0) or NSID 1.
  - **Format NVM:** only NSID 1.
  - **Refused:** Sanitize, firmware download/commit, namespace
    management/attachment, security send/receive, directives, self-test and
    every vendor-specific opcode (Apple's NAND tunnel), because they can
    reach every namespace.
  - Refusals log `Apple ANS: refused opcode 0x.. for nsid N on qid Q:
    system namespaces are protected`.
- `nvme_ctrl_ops->ns_is_readonly()` makes nvme-core mark every namespace
  but NSID 1 read-only (`set_disk_ro`) whenever it evaluates a namespace,
  as it does for `NVME_CTRL_FORCE_RO`.
- Lift (dangerous, never needed for normal use):
  `nvme.apple_ans_unlock_system_namespaces=1`, boot-time only (0444). The
  boot log then says `Apple ANS: system namespaces UNLOCKED`; otherwise
  `Apple ANS: namespaces other than NSID 1 are read-only and only safe
  admin commands are sent`.

So today two independent gates stand in front of NSIDs 2–7:
- the compile-time read-only allowlist, which has no knob;
- the system-namespace guard, whose knob is a boot parameter.

### Nothing writes NSID 1 unless userspace asks

Verified by code review of this tree, and by the device logs so far:
- With `CONFIG_NVME_PCI_READ_ONLY=y` (pinned), no media write can reach
  the controller at all.
- What the kernel issues on its own: Identify, Get Log Page, Get Features,
  queue management, AER, and Set Features for Number of Queues, Async Event
  Config, Timestamp, Host Behavior and APST (all NSID 0, all volatile
  controller state). Plus reads: partition scan, and whatever userspace
  reads.
- nvme-core and the block layer write media only for userspace: write(2)
  to a block device opened for writing, or a mounted filesystem. HoolockRD
  mounts nothing from NVMe.
- A Flush is sent only on behalf of such a writer (fsync, REQ_PREFLUSH).
  It commits the controller's volatile cache; it doesn't write data.
- Device evidence: the integ10 boot log has no `refused opcode` line. Any
  kernel attempt would have been refused and logged.

## Round 2: background bring-up

From round 2 on, the Apple ANS controller is enabled from nvme-pci's
`reset_work` instead of inside `nvme_probe()`, so a hung bring-up can't
block `/init`. That only moves *where* the init runs. Every command still
goes through `nvme_prep_rq()` and `nvme_sq_copy_cmd()`, and
`NVME_CTRL_FORCE_RO` is set in `nvme_pci_alloc_dev()` before anything else.
All three layers are unchanged.

## What still touches the controller (and why that's acceptable)

These are volatile controller or link state, not medium contents:

- PCIe config writes from the Apple ANS glue: latency timer, LTR max latency,
  L1 PM substates control (values from the ADT `pci-max-latency` and
  `pci-l1pm-control`).
- BAR writes: `CC` (enable/disable/shutdown notification), admin queue
  base/size, and the ANS HMB base/size registers (0x1810–0x1818).
- NVMMU programming in the host bridge (TCB tables, SART window).
- On shutdown, nvme-core sends a normal NVMe shutdown notification
  (`CC.SHN`), which iOS and iBoot also do on every boot.

The ANS firmware manages its own FTL and NAND internally (wear levelling,
read-disturb handling). We can't control that and it's the same thing that
happens when iBoot or iOS reads the disk.

## How to check it on the device

See `test-plan.md`, step 7 ("read-only proof"): the boot log line,
`/sys/block/*/ro`, and `EACCES` when opening for write. Block-device write
paths (dd, blkdiscard) stop at that open. NVMe passthrough ioctls are
different: root (`CAP_SYS_ADMIN`) may send any command through `/dev/nvme0`,
`/dev/ng0n1` or a read-only open of `/dev/nvme0n1`, so layer 3 doesn't cover
them and layer 1 is what stops a Format or Write sent that way. HoolockRD's
busybox has no tool that issues those ioctls, so on-device testing of layer 1
waits for a rootfs with `nvme-cli`. Until then, layer 1 is covered by code
review and by the absence of any `refused opcode` line during a normal boot
(it would mean the kernel itself tried to write). Never test with a real
write to a real sector.
