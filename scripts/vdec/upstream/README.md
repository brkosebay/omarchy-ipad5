# Pinned public source inputs

> Historical design/test notes. Current support: [status](../../../docs/release/STATUS.md); examples may need local firmware/captures.


These three files are unmodified upstream files, used for offline generation
and independent comparison. `sources.json` records commits, paths, download
URLs, decoded-file SHA-256 and licensing. Keep upstream notices intact.

`h264_vlc.h` and `h264_idx.h` lines 1–10 retain upstream's exact
`SPDX-License-Identifier: GPL-2.0`, Imagination Technologies Ltd. copyright,
2021 Texas Instruments Incorporated copyright, and Angela Stegmaier credit.
`pnw_H264.c` lines 1–28 retain the Intel/Imagination copyrights and complete
MIT permission/disclaimer; it has no SPDX line.

The independently downloaded TI kernel copy of `h264_vlc.h` is identical:
commit `6baa440211758789c37226e1b7ca154ce90f8be8`,
`drivers/media/platform/vxe-vxd/decoder/h264_vlc.h` at
<https://git.ti.com/cgit/ti-linux-kernel/ti-linux-kernel/plain/drivers/media/platform/vxe-vxd/decoder/h264_vlc.h?id=6baa440211758789c37226e1b7ca154ce90f8be8>.

Run `python3 -B scripts/vdec/fetch-table-sources.py` to check locally;
`--fetch` downloads and validates the pinned sources before replacing them.
The generator and comparator themselves never access the network or Apple files.
