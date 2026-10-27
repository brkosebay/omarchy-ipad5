# Test plan: first real SEP traffic (sepOS booted)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Everything below assumes the `a9/sep` kernel (driver + DT node) and a boot where
sepOS is actually running. That is the `SEP_BOOT=1` path added to
`scripts/boot/autoboot.sh`, with the patched `Pongo-a9sep.bin`.

Until sepOS boots, every check here degrades cleanly: the mailbox is idle, no
endpoint is allocated, and `/dev/sep-bio` reports `sensor_present = 0`.

## 0. Boot

```
SEP_BOOT=1 scripts/boot/autoboot.sh --once     # DFU already held by the owner
```

Expected in `build/boot/autoboot.log`:

```
SEP_BOOT=1: staging sepOS from .../build/sep/sep.j71t.img4
pongo-sep: sepOS booted
sepOS booted; the SEP stays alive across bootm
m1n1 running, waiting for Linux
Linux is up (USB gadget present)
```

If it says `SEPROM panic` the image was rejected (candidate A vs B); the Linux
boot still proceeds.

## 1. Bring the mailbox up

On the iPad (`ssh root@172.16.42.1` or the USB console):

```
mount -t debugfs none /sys/kernel/debug
cat /sys/kernel/debug/sep-a9/status        # running=0, "mailbox untouched"
echo 1 > /sys/kernel/debug/sep-a9/start    # arm
cat /sys/kernel/debug/sep-a9/status        # running=1
cat /sys/kernel/debug/sep-a9/log           # endpoint advertisements, if any
```

The first real signal is messages in `log`. The SEP announces endpoints at
boot; the `sbio` endpoint number is in that list (until now it was unknown).

## 2. Select the sbio endpoint and publish its OOL buffers

```
echo <N> > /sys/kernel/debug/sep-a9/sbio    # N from the advertisement
echo "<N> 16384" > /sys/kernel/debug/sep-a9/ool
```

`ool` allocates one send and one receive 16 KiB buffer and publishes them with
the control opcodes `0x04/0x02` and `0x05/0x03`.

## 3. SBIO handshake

```
echo "<N> 73 01000000" > /sys/kernel/debug/sep-a9/xfer
cat /sys/kernel/debug/sep-a9/xfer           # out_len=..., hex
```

Success is `out_len=0` and no timeout. A timeout means the SEP did not answer;
`-EAGAIN` means the mailbox is not armed or the endpoint is not allocated.

Once this succeeds, `/dev/sep-bio` starts reporting a reader:

```
sudo tools/sep/sep-bio-test info
version=4 sensor_present=1 enrolled=0 capacity=0 stages=0
```

## 4. Enrol / verify (provisional payloads)

```
sudo tools/sep/sep-bio-test enrol
sudo tools/sep/sep-bio-test verify
sudo tools/sep/sep-bio-test list
```

Build on the iPad: `make -C tools/sep && sudo tools/sep/sep-bio-test info`.

**Important:** the 48-byte EnrollMode and 68-byte MatchMode payloads are
serialized by `biometrickitd` in userspace and are not recovered yet. The
driver sends zeroed buffers with the caller's flags, so these commands prove
the transport and the ioctl state machine but not enrolment semantics. Do not
expect a usable fingerprint from this step; do expect the SEP to accept the
command (or a clean error via the tag `0xff` status word).

## 5. libfprint / fprintd (unchanged ABI)

The device speaks Aurora Silicon's `/dev/sep-bio` interface v4, so their
libfprint `aurora` driver, stock fprintd and Omarchy's fingerprint UI are the
intended consumers. On Arch ARM:

1. build libfprint with
   `libfprint-1.94.100-apple-sep.patch` from `aurora-silicon/aurora-sep-userspace`;
2. install the fprintd drop-in (`DeviceAllow=/dev/sep-bio rw`);
3. `fprintd-enroll` / `omarchy-setup-security-fingerprint`.

No fprintd work is needed for the steps above; `sep-bio-test` talks to the
kernel directly.

## 6. Failure modes

| symptom | meaning |
|---|---|
| `log` stays empty after arming | sepOS not running (SEP_BOOT failed) |
| `xfer` returns `-ETIMEDOUT` | command reached the mailbox, no reply |
| `xfer` returns `-EAGAIN` | mailbox unarmed or endpoint not allocated |
| `GET_INFO` shows `sensor_present=0` | SBIO handshake not done/failed |
| `SEPROM panic` at boot | sepOS image rejected (try candidate B) |
| whole device hangs on arm | SEP block unpowered; reboot, do not arm before sepOS is known good |
