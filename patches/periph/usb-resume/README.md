# J71t USB resume / ECM notification fix

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.


Two incremental commits on `a9/periph`, after Round 6 (`64221b189e`). Apply
after the top-level periph series through `0055-*`, in `series` order.
`BASE`, `HEAD` and `manifest.json` record the exact commits and patch hashes.

See [diagnosis, tests and orchestrator commands](../../../docs/periph/usb-resume.md).
No device access or hardware validation was performed. The rootfs recovery
service is a separate, uncommitted workspace change under `scripts/omarchy/`.
