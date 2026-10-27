# PCIe + NVMe workstream (A9 `apcie`, read-only NVMe)

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


| File | What |
|---|---|
| `BRIEF.md` | the task brief |
| `a9-vs-a10.md` | register-by-register A9 (S8003) vs A10 (T8010) comparison and what the port changes |
| `kernelcache-notes.md` | round-1 static analysis of the iOS 16.7.10 iPad6,11 kernelcache (AppleS800xPCIe/S8000/S8003, AppleNVMeMMU) |
| `HANDOFF.md` | current state, device results, open hypotheses, next steps |
| `enable-freeze.md` | round 3: the SoC freeze at CC.EN (DART fault-handler panic), iOS NVMe/DART/NVMMU comparison, round-3 changes and knobs |
| `b1-hang.md` | round 2: why B1 hung, what iOS does instead (with addresses), the fixes, the build bug, Wi-Fi status |
| `live-adt-decode.txt` | decode of the live ADT (`/dev/mtd`): tunables, sart-region, invalidate-fifo-workaround |
| `read-only.md` | how the kernel enforces read-only NVMe, layer by layer |
| `test-plan.md` | round-7 on-device steps (Omarchy): MAC, regdb/country, association, iperf, soak, firmware reset, ASPM power run, NVMe soak |
| `suspend.md` | round 8: s2idle design (`pcie_apple_h9p.suspend` levels, iOS port-sleep sequence), patches 0036–0042, non-PCIe blockers, what's unverified |
| `suspend-test-plan.md` | round 8 on-device suspend stages A–G (pm_test first, then real s2idle with the Home button), stop conditions |
| `wifi.md` | BCM4355 on PCIe port 1: device status, round-7 fixes (MAC, regdb, country, P2P, ASPM, recovery, suspend), ADT facts, power sequence, firmware |
| `logs/` | device logs (2026-10-22 Omarchy Wi-Fi boot) |

## Reproduce

```sh
# kernel tree: /src/wt/pcie (branch a9/pcie) in the linux-a9-src volume
scripts/pcie/build.sh                  # merge patches/pcie/pcie.config, build, verify config
scripts/pcie/build-m1n1.sh             # m1n1 + HMB patch -> build/m1n1-pcie/m1n1.bin (needed for the main payload)
scripts/pcie/fetch-regdb.sh            # wireless-regdb -> build/firmware/regdb/regulatory.db{,.p7s}
scripts/pcie/mkinitramfs-wifi.sh       # Wi-Fi firmware + regdb -> build/kernel-pcie/initramfs-wifi{,-overlay}.gz
scripts/pcie/mkpayload-pcie.sh         # -> build/kernel-pcie/m1n1-linux-pcie-r7{,-aspm,-debug}.bin
python3 -I scripts/pcie/adt-pcie.py ADT.bin   # PCIe/NVMe props of a (live) ADT dump
scripts/pcie/export-patches.sh         # -> patches/pcie/kernel/*.patch
scripts/pcie/check-config.sh [config]  # does a config contain every fragment option?
scripts/pcie/config.sh                 # only to reset .config from scratch
```

Don't build the pcie tree with plain `scripts/build-kernel.sh --tree pcie`:
the tree-local `.config` can lose options (that's how round 1 shipped
without the driver). `build.sh` re-merges the fragment every time.

On a fresh tree, `git am patches/pcie/kernel/*.patch` on top of HoolockLinux
`hoolock` 6831bc70 recreates the branch. `patches/pcie/m1n1/` applies to
HoolockLinux/m1n1 `idevice` d5a10ac.

`scripts/pcie/ktree.sh pull|push PATH...` copies files between the
Docker-volume tree and `/tmp/pcie-edit` for editing on the host.
