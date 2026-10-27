# 2026-10-24 kexec test: SEP driver on hardware

> Historical design/test notes. Current support: [status](../../release/STATUS.md); examples may need local firmware/captures.


## What was tested

Staged `build/kernel-sep` (`7.3.0-rc1-sep-gf7812eeb87ed`) as a kexec entry
`sep-f7812eeb87` with **no dtb in the entry**, so `upload-entry.sh` reused the
running entry's dtb and `linux-a9-kexec` reported `fdt: reuse`. Loaded with
`kexec_loaded=1`, then `systemctl kexec`.

The kernel booted. NVMe did not: `probe of 610000000.pcie with driver
pcie-apple-h9p returned -517` (supplier deferral), ending in
`RESCUE: no usable NVMe root`. That is the known kexec-after-NVMe handoff gap,
not a SEP/driver problem; the initramfs rescue shell came up with USB
networking and `systemd.debug_shell` on `ttyGS0`.

## Result: driver plumbing is validated

Read over the USB debug shell (`scripts/linux-run.sh`):

```
# uname -r
7.3.0-rc1-sep-gf7812eeb87ed

# cat /sys/module/sep_a9/parameters/enable
Y

# ls /sys/kernel/debug/sep-a9
log  ool  send  start  status  xfer

# echo 1 > /sys/kernel/debug/sep-a9/start
# cat /sys/kernel/debug/sep-a9/status
base=ffff800080460000 mbox=ffff800080464000 running=1
recv_sts=0x28801 snd_sts=0x27701 en_int=0x111
tx=0 rx=0 dropped=0 log=0

# ls -l /dev/sep-bio
crw------- 1 0 0 10, 257 /dev/sep-bio

# grep -i sep /proc/misc
257 sep-bio
```

Interpretation:

- `ioremap` of `0x20DA00000` works and the mailbox is reachable — reading it
  does not stall the AP, even though the SEP is not booted.
- arming (`start`) enables the mailbox the way pongoOS does; `en_int` reads back
  `0x111`, not the `0x1000` written, so the register is not a plain latch.
  Worth keeping in mind when interrupts replace polling.
- `recv_sts & 0x20000` is set, which pongoOS treats as "empty": **no messages**,
  exactly as expected because sepOS was never booted on this boot (that must
  happen in pongoOS/m1n1 before Linux).
- `/dev/sep-bio` (misc minor 257, 0600 root) is registered, so the Aurora ABI
  surface exists on real hardware.

## Not tested yet

- Any SEP message, endpoint advertisement or transfer — all require sepOS.
- `ool` / `xfer` were left untouched (they fail with `-EAGAIN` while the
  mailbox is unarmed, and there is no endpoint to talk to).

## Next

Boot path that can actually start sepOS: pongoOS with
`patches/sep/pongoos/pongoos-a9-sep.patch` (`sep adopt` + `sep sboot`), then
`bootm` the Linux payload with `sep_a9.enable=1`. Only then do the endpoint
advertisements appear in `log` and `ool`/`xfer` become meaningful.
