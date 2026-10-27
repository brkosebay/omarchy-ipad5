# Research roadmap — 2026-10-27

The tested board is iPad6,11 / j71t / S8003. Read the
[current status](docs/release/STATUS.md) and [evidence](docs/release/EVIDENCE.md)
for observed behavior and limits.

- Prepare the clean OrchardLinux kernel port on
  [a9/orchard-j71t](https://github.com/brkosebay/linux/tree/a9/orchard-j71t).
  Revalidate that exact series on hardware before transferring device claims.
- Fix USB-C wall-charger detection and policy; computer charging reached 2.4 A.
- Fix rotation blanking and AOP warm reload; cold auto-rotate is the proven path.
- Continue SEP/Touch ID transport work. A valid status reply and SBIO init
  are milestones, not fingerprint authentication.
- Harden suspend, USB host, display power management and recovery.
- Investigate other boards independently, including A9X j127/j128;
  [PORTING.md](PORTING.md) records the differences and evidence requirements.

This snapshot contains source and research inputs. Obtain local firmware and
calibration from your own unit; historical patch exports have separate bases.
