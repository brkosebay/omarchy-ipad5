#!/usr/bin/env python3
"""Build the one-shot tri53-sep7 arts/SBIO test from a pongoOS capture.

Usage:
  python3 -B -I scripts/sep/arts-manifest-plan.py PONGO_LOG --out DIR

Offline only. It verifies the fresh-boot capture, the fixed root-shmem
reservation, the arts/xarm advertisements and the local personalized IM4M. It
writes the isolated arts scripts, an XNU-ordered combined arts+xarm pair, a
guarded one-shot SBIO init probe, and the next fresh-boot read-only
GetProtectedConfig probe. The latter repeats the proven init and then sends
exactly one opcode 0x2b request.
The manifest itself is transferred separately as binary stdin; no generated
file embeds it.
"""

import hashlib
import re
import sys
from pathlib import Path


SHMEM_PA = 0x87ADB0000
SHMEM_PAGES = 1
PAGE = 0x1000
ARTS_NAME = 0x61727473
ARTS_BYTES = 0x4000
EXPECTED_NAMES = {
    0x00: 0x636E746C,  # cntl
    0x02: 0x61727473,  # arts
    0x10: 0x78617273,  # xars
    0x13: 0x7861726D,  # xarm
}
EXPECTED_OOL = {
    0x00: 0x00000000,
    0x02: 0x04040404,
    0x10: 0x08080808,
    0x13: 0x08080808,
}
MANIFEST = Path("build/sep/full/im4m.der")
MANIFEST_SIZE = 6157
MANIFEST_SHA256 = "fa3a1929f3fd9ef95dc5b02c202d76dfda217ba5b01944c5736b6d460a8c7f54"
KERNEL_ENTRY = "tri53-sep7-56c3395908"
SBIO_USER_ID = 501
SBIO_USER_PAYLOAD = SBIO_USER_ID.to_bytes(4, "little").hex()

ANSI = re.compile(r"\x1b\[[0-9;]*m")
MSG = re.compile(
    r"sep capture: msg ep=([0-9a-f]+) tag=([0-9a-f]+) op=([0-9a-f]+) "
    r"param=([0-9a-f]+) data=([0-9a-f]+) \(raw=([0-9a-f]+)\)"
)
SHMEM = re.compile(
    r"sep: -> root shmem ep=fe tag=([0-9a-f]+) pages=([0-9a-f]+) data=([0-9a-f]+)"
)
SUMMARY = re.compile(r"sep capture: total=(\d+) retained=(\d+) overwritten=(\d+)")


def stop(message):
    raise SystemExit(f"STOP: {message}")


def parse_capture(path):
    lines = [ANSI.sub("", line).rstrip("\r") for line in path.read_text(errors="replace").splitlines()]
    if sum("sep xnu: SEPOS BOOTED" in line for line in lines) != 1:
        stop("capture must contain exactly one SEPOS BOOTED line")

    shmem = [m for line in lines if (m := SHMEM.search(line))]
    if len(shmem) != 1:
        stop(f"expected one root-shmem line, found {len(shmem)}")
    pages = int(shmem[0].group(2), 16)
    pa = int(shmem[0].group(3), 16) << 12
    if (pa, pages) != (SHMEM_PA, SHMEM_PAGES):
        stop(
            f"root shmem is {pa:#x}/{pages}; tri53-sep7 reserves "
            f"{SHMEM_PA:#x}/{SHMEM_PAGES}"
        )

    summary = [m for line in lines if (m := SUMMARY.search(line))]
    if len(summary) != 1:
        stop("expected exactly one capture summary")
    total, retained, overwritten = map(int, summary[0].groups())
    if (total, retained, overwritten) != (16, 16, 0):
        stop(
            f"capture is total={total} retained={retained} "
            f"overwritten={overwritten}; expected 16/16/0"
        )

    messages = []
    for line in lines:
        match = MSG.search(line)
        if not match:
            continue
        fields = tuple(int(group, 16) for group in match.groups())
        ep, tag, op, param, data, raw = fields
        expected = data << 32 | param << 24 | op << 16 | tag << 8 | ep
        if raw != expected:
            stop(f"field/raw mismatch in {line}")
        messages.append(fields)
    if len(messages) != retained:
        stop(f"parsed {len(messages)} messages, retained={retained}")

    names, ool = {}, {}
    for ep, _tag, op, param, data, raw in messages:
        if ep != 0xFD:
            continue
        table = names if op == 0 else ool if op == 1 else None
        if table is None:
            stop(f"unknown discovery word {raw:016x}")
        if param in table and table[param] != data:
            stop(f"conflicting advertisement for endpoint {param:#x}")
        table[param] = data
    if set(names) != set(ool):
        stop("incomplete endpoint advertisement pairs")
    if names != EXPECTED_NAMES or ool != EXPECTED_OOL:
        stop(
            f"discovery table changed: names={names!r} ool={ool!r}; "
            "expected only cntl/arts/xars/xarm at the proven IDs and sizes"
        )

    xarm_requests = [
        fields for fields in messages
        if fields[0] == 0x13 and fields[2:5] == (0x0E, 0, 0)
    ]
    if len(xarm_requests) != 1:
        stop(f"expected one zero-length xarm opcode-0e request, found {len(xarm_requests)}")
    xarm_ep, xarm_tag, _op, _param, _data, xarm_raw = xarm_requests[0]
    if sum(fields[:5] == (0, 0, 0x10, 0, 0) for fields in messages) != 1:
        stop("expected one endpoint-0 opcode-10 power-state notification")

    arts = [endpoint for endpoint, name in names.items() if name == ARTS_NAME]
    if len(arts) != 1:
        stop(f"expected one arts endpoint, found {len(arts)}")
    arts_ep = arts[0]
    word = ool[arts_ep]
    limits = tuple(word >> shift & 0xFF for shift in (0, 8, 16, 24))
    if limits != (4, 4, 4, 4):
        stop(f"arts OOL is {limits}, expected exactly 4/4/4/4 pages")
    return pa, arts_ep, xarm_ep, xarm_tag, xarm_raw, names, ool


def verify_manifest():
    data = MANIFEST.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != MANIFEST_SIZE or digest != MANIFEST_SHA256:
        stop(
            f"{MANIFEST} is {len(data)} bytes sha256 {digest}; expected "
            f"{MANIFEST_SIZE} and {MANIFEST_SHA256}"
        )
    if data[:2] != b"\x30\x82" or int.from_bytes(data[2:4], "big") + 4 != len(data):
        stop("local IM4M has an invalid DER length")
    if data[4:10] != b"\x16\x04IM4M":
        stop("local manifest lacks the IM4M marker")
    return digest


def main(argv):
    if len(argv) != 3 or argv[1] != "--out":
        print(__doc__, file=sys.stderr)
        return 2

    capture = Path(argv[0])
    out = Path(argv[2])
    pa, arts_ep, xarm_ep, xarm_tag, xarm_raw, names, ool = parse_capture(capture)
    digest = verify_manifest()
    adverts = [f"raw {ep:02x} {names[ep]:08x} {ool[ep]:08x}" for ep in sorted(names)]

    out.mkdir(parents=True, exist_ok=True)
    (out / "plan.env").write_text(
        f"ARTS_EP={arts_ep:02x}\nXARM_EP={xarm_ep:02x}\n"
        f"XARM_TAG={xarm_tag:02x}\nXARM_REQUEST_RAW={xarm_raw:016x}\n"
        f"MANIFEST={MANIFEST}\n"
        f"MANIFEST_SIZE={MANIFEST_SIZE}\nMANIFEST_SHA256={digest}\n"
    )

    prep = [
        "# generated by scripts/sep/arts-manifest-plan.py; fresh tri53-sep7 boot only",
        "set -eu",
        "D=/sys/kernel/debug/sep-a9",
        f"grep -qw 'linux_a9.entry={KERNEL_ENTRY}' /proc/cmdline || {{ echo 'STOP: wrong tri53-sep7 payload'; exit 1; }}",
        "echo 1 >\"$D/start\"",
        "sleep 0.1",
        "echo '== fifo before'; cat \"$D/fifo\"",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO not empty'; exit 1; }",
        *[
            f'echo "{advert}" >"$D/advertise" || {{ echo "STOP: advertise {advert}"; exit 1; }}'
            for advert in adverts
        ],
        "echo '== endpoints declared'; cat \"$D/endpoints\"",
        f'echo "{arts_ep:02x} {ARTS_BYTES:x} {ARTS_BYTES:x}" >"$D/ool" || {{ echo "STOP: arts OOL publish failed; cold reset"; exit 1; }}',
        "sleep 0.2",
        "echo '== after arts OOL'; cat \"$D/fifo\"; cat \"$D/status\"; cat \"$D/log\"",
        "grep -q 'published=1' \"$D/status\" || { echo 'STOP: expected only arts published'; exit 1; }",
        "echo 'READY: upload the IM4M; no MANIFEST request has been sent'",
    ]
    (out / "remote-arts-prep.sh").write_text("\n".join(prep) + "\n")

    commit = [
        "# generated by scripts/sep/arts-manifest-plan.py; run once after binary upload",
        "set -u",
        "D=/sys/kernel/debug/sep-a9",
        "echo '== staged'; cat \"$D/arts-start\"",
        f"grep -q 'ran=0 .*manifest_len={MANIFEST_SIZE}' \"$D/arts-start\" || {{ echo 'STOP: IM4M was not staged exactly'; exit 1; }}",
        "echo '== send one arts MANIFEST (tag 1 op 6)'",
        "if ! echo 1 >\"$D/arts-start\"; then",
        "  echo 'STOP: MANIFEST failed or timed out; do not retry in this boot'",
        "  cat \"$D/arts-start\"; cat \"$D/fifo\"; dmesg | grep sep-a9 | tail -20; exit 1",
        "fi",
        "echo '== immediate'; cat \"$D/arts-start\"; cat \"$D/fifo\"",
        "sleep 2",
        "echo '== plus 2 s'; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/log\"",
        "sleep 8",
        "echo '== plus 10 s'; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/arts-start\"; cat \"$D/fifo\"",
        "dmesg | grep sep-a9 | tail -30",
    ]
    (out / "remote-arts-commit.sh").write_text("\n".join(commit) + "\n")

    combined_prep = [
        "# generated by scripts/sep/arts-manifest-plan.py; fresh tri53-sep7 boot only",
        "# XNU discovery order: initialize xarm OOL before arts OOL; no requests yet",
        "set -eu",
        "D=/sys/kernel/debug/sep-a9",
        f"grep -qw 'linux_a9.entry={KERNEL_ENTRY}' /proc/cmdline || {{ echo 'STOP: wrong tri53-sep7 payload'; exit 1; }}",
        "echo 1 >\"$D/start\"",
        "sleep 0.1",
        "echo '== fifo before'; cat \"$D/fifo\"",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO not empty'; exit 1; }",
        *[
            f'echo "{advert}" >"$D/advertise" || {{ echo "STOP: advertise {advert}"; exit 1; }}'
            for advert in adverts
        ],
        "echo '== endpoints declared'; cat \"$D/endpoints\"",
        f'echo "{xarm_ep:02x} 8000 8000" >"$D/ool" || {{ echo "STOP: xarm OOL publish failed; cold reset"; exit 1; }}',
        "sleep 0.2",
        f'echo "{arts_ep:02x} {ARTS_BYTES:x} {ARTS_BYTES:x}" >"$D/ool" || {{ echo "STOP: arts OOL publish failed; cold reset"; exit 1; }}',
        "sleep 0.2",
        "echo '== after xarm then arts OOL'; cat \"$D/fifo\"; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/log\"",
        "grep -q '^tx=8 rx=8 tx_busy=0 dropped=0 ' \"$D/status\" || { echo 'STOP: expected exactly eight OOL TX and eight ep0/op1 acknowledgements'; exit 1; }",
        "grep -q 'endpoints_complete=4 published=2' \"$D/status\" || { echo 'STOP: expected only xarm+arts published'; exit 1; }",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO not empty after OOL'; exit 1; }",
        "grep -q '^sep->ap recv_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: SEP->AP FIFO not empty after OOL'; exit 1; }",
        "echo 'READY: upload the IM4M; no MANIFEST or xarm reply has been sent'",
    ]
    (out / "remote-combined-prep.sh").write_text("\n".join(combined_prep) + "\n")

    combined_commit = [
        "# generated by scripts/sep/arts-manifest-plan.py; run once after binary upload",
        "# Commit order: successful arts MANIFEST, two-second observation, one capability-0 xarm reply",
        "set -eu",
        "D=/sys/kernel/debug/sep-a9",
        "echo '== staged/preflight'; cat \"$D/arts-start\"; cat \"$D/status\"; cat \"$D/fifo\"",
        f"grep -q 'ran=0 .*manifest_len={MANIFEST_SIZE}' \"$D/arts-start\" || {{ echo 'STOP: IM4M was not staged exactly or this script already ran'; exit 1; }}",
        "grep -q '^tx=8 rx=8 tx_busy=0 dropped=0 ' \"$D/status\" || { echo 'STOP: OOL counters changed before MANIFEST'; exit 1; }",
        "grep -q 'endpoints_complete=4 published=2' \"$D/status\" || { echo 'STOP: endpoint state changed before MANIFEST'; exit 1; }",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO busy before MANIFEST'; exit 1; }",
        "echo '== send one arts MANIFEST (tag 1 op 6)'",
        "if ! echo 1 >\"$D/arts-start\"; then",
        "  echo 'STOP: MANIFEST failed or timed out; do not retry in this boot'",
        "  cat \"$D/arts-start\"; cat \"$D/fifo\"; dmesg | grep sep-a9 | tail -30; exit 1",
        "fi",
        "sleep 2",
        "echo '== two seconds after MANIFEST, before xarm reply'",
        "cat \"$D/arts-start\"; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/log\"; cat \"$D/fifo\"",
        "grep -q 'ran=1 result=2 ret=0 ep=2 manifest_len=6157' \"$D/arts-start\" || { echo 'STOP: MANIFEST state is not successful'; exit 1; }",
        "grep -q 'reply tag=01 op=07 param=00 data=00000000 raw=0000000000070102' \"$D/arts-start\" || { echo 'STOP: MANIFEST reply differs; no xarm reply sent'; exit 1; }",
        "grep -q '^tx=9 rx=9 tx_busy=0 dropped=0 ' \"$D/status\" || { echo 'STOP: unexpected traffic after MANIFEST; no xarm reply sent'; exit 1; }",
        "grep -q 'endpoints_complete=4 published=2' \"$D/status\" || { echo 'STOP: discovery already changed after MANIFEST; no xarm reply sent'; exit 1; }",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO busy before xarm reply'; exit 1; }",
        "grep -q '^sep->ap recv_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: SEP->AP FIFO busy before xarm reply'; exit 1; }",
        f"echo '== send one captured-tag xarm capability-0 reply ep={xarm_ep:02x} tag={xarm_tag:02x}'",
        f'if ! echo "{xarm_ep:02x} {xarm_tag:02x} 00 00 00000000" >"$D/send"; then',
        "  echo 'STOP: xarm reply transport failed; do not retry in this boot'",
        "  cat \"$D/status\"; cat \"$D/fifo\"; dmesg | grep sep-a9 | tail -30; exit 1",
        "fi",
        "echo '== immediate after xarm'; cat \"$D/status\"; cat \"$D/fifo\"",
        "sleep 2",
        "echo '== plus 2 s'; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/log\"; cat \"$D/fifo\"",
        "sleep 8",
        "echo '== plus 10 s'; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/arts-start\"; cat \"$D/log\"; cat \"$D/fifo\"",
        "dmesg | grep sep-a9 | tail -50",
    ]
    (out / "remote-combined-commit.sh").write_text("\n".join(combined_commit) + "\n")

    sbio_init = [
        "# generated by scripts/sep/arts-manifest-plan.py; run once after a successful combined commit",
        "# Publishes only SBIO OOL and sends only init opcode 0x73 with u32 value 1",
        "set -eu",
        "D=/sys/kernel/debug/sep-a9",
        f"grep -qw 'linux_a9.entry={KERNEL_ENTRY}' /proc/cmdline || {{ echo 'STOP: wrong tri53-sep7 payload'; exit 1; }}",
        "echo '== preflight: successful combined gate, exactly eight endpoints'",
        "cat \"$D/arts-start\"; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/fifo\"",
        f"grep -q 'ran=1 result=2 ret=0 ep={arts_ep} manifest_len={MANIFEST_SIZE}' \"$D/arts-start\" || {{ echo 'STOP: MANIFEST was not successful'; exit 1; }}",
        "grep -q 'reply tag=01 op=07 param=00 data=00000000 raw=0000000000070102' \"$D/arts-start\" || { echo 'STOP: MANIFEST reply differs'; exit 1; }",
        "grep -q '^tx=10 rx=17 tx_busy=0 dropped=0 ' \"$D/status\" || { echo 'STOP: combined counters differ; do not publish SBIO'; exit 1; }",
        "grep -q 'endpoints_complete=8 published=2' \"$D/status\" || { echo 'STOP: expected eight endpoints and only xarm+arts published'; exit 1; }",
        "grep -q '^sbio_ep=-1 sbio_ready=0$' \"$D/status\" || { echo 'STOP: SBIO already selected or initialized'; exit 1; }",
        "grep -Eq '^0x08 sbio +mailbox 4-4 +4-4 +complete max_send=0x4000 max_recv=0x4000$' \"$D/endpoints\" || { echo 'STOP: sbio advertisement differs'; exit 1; }",
        "grep -Eq '^0x0a scrd +mailbox 4-4 +4-4 +complete max_send=0x4000 max_recv=0x4000$' \"$D/endpoints\" || { echo 'STOP: scrd advertisement differs'; exit 1; }",
        "grep -Eq '^0x0c sse +mailbox 4-4 +4-4 +complete max_send=0x4000 max_recv=0x4000$' \"$D/endpoints\" || { echo 'STOP: sse advertisement differs'; exit 1; }",
        "grep -Eq '^0x12 sks +mailbox 4-8 +4-8 +complete max_send=0x8000 max_recv=0x8000$' \"$D/endpoints\" || { echo 'STOP: sks advertisement differs'; exit 1; }",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO busy before SBIO'; exit 1; }",
        "grep -q '^sep->ap recv_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: SEP->AP FIFO busy before SBIO'; exit 1; }",
        "echo '== select advertised SBIO endpoint and publish one 16-KiB OOL pair'",
        "echo '08' >\"$D/sbio\" || { echo 'STOP: SBIO selection failed'; exit 1; }",
        "if ! echo '08 4000 4000' >\"$D/ool\"; then",
        "  echo 'STOP: SBIO OOL publication failed; do not retry in this boot'",
        "  cat \"$D/status\"; cat \"$D/fifo\"; dmesg | grep sep-a9 | tail -30; exit 1",
        "fi",
        "sleep 0.2",
        "echo '== after SBIO OOL'; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/log\"; cat \"$D/fifo\"",
        "grep -q '^tx=14 rx=21 tx_busy=0 dropped=0 ' \"$D/status\" || { echo 'STOP: expected four OOL TX and four acknowledgements; no init sent'; exit 1; }",
        "grep -q 'endpoints_complete=8 published=3' \"$D/status\" || { echo 'STOP: expected only xarm+arts+sbio published; no init sent'; exit 1; }",
        "grep -q '^sbio_ep=8 sbio_ready=0$' \"$D/status\" || { echo 'STOP: SBIO state changed before init'; exit 1; }",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO busy after SBIO OOL'; exit 1; }",
        "grep -q '^sep->ap recv_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: SEP->AP FIFO busy after SBIO OOL'; exit 1; }",
        "echo '== send exactly one framed SBIO init: opcode 0x73, input u32 1, no output'",
        "if ! echo '08 73 01000000' >\"$D/xfer\"; then",
        "  echo 'STOP: SBIO init failed or timed out; do not retry in this boot'",
        "  cat \"$D/xfer\"; cat \"$D/status\"; cat \"$D/fifo\"; dmesg | grep sep-a9 | tail -40; exit 1",
        "fi",
        "sleep 0.2",
        "echo '== final'; cat \"$D/xfer\"; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/log\"; cat \"$D/fifo\"",
        "grep -q '^out_len=0$' \"$D/xfer\" || { echo 'STOP: SBIO init returned data'; exit 1; }",
        "grep -q '^ep=0x08 cmd=0x73 ret=0 sep_status=0x0$' \"$D/xfer\" || { echo 'STOP: SBIO init completion differs'; exit 1; }",
        "grep -q '^gt_reply version=1 total=0 offset=0 flags=0x0 reserved=0x0 command=0x73 chunk=0 mailbox_data=0x0$' \"$D/xfer\" || { echo 'STOP: GenericTransfer reply header differs'; exit 1; }",
        "grep -q '^tx=15 rx=22 tx_busy=0 dropped=0 ' \"$D/status\" || { echo 'STOP: SBIO init counters differ'; exit 1; }",
        "grep -q 'endpoints_complete=8 published=3' \"$D/status\" || { echo 'STOP: endpoint state changed during init'; exit 1; }",
        "grep -q '^sbio_ep=8 sbio_ready=1$' \"$D/status\" || { echo 'STOP: driver did not mark SBIO ready'; exit 1; }",
        "test \"$(tail -n 1 \"$D/log\")\" = 'ep=08 tag=fc op=73 param=00 data=00000000' || { echo 'STOP: final mailbox word is not the expected SBIO completion'; exit 1; }",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO busy after init'; exit 1; }",
        "grep -q '^sep->ap recv_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: SEP->AP FIFO busy after init'; exit 1; }",
        "echo 'RESULT sbio-init=pass'",
        "echo 'STOP: collect logs and cold-reset; do not send GetProtectedConfig in this boot'",
    ]
    (out / "remote-sbio-init.sh").write_text("\n".join(sbio_init) + "\n")

    # A separate fresh-boot rung. Reuse every validated init gate above, but
    # replace the old stop with one read-only four-byte GetProtectedConfig
    # request. Generating it from the init script prevents the two procedures
    # from drifting on endpoint/counter/FIFO preconditions.
    sbio_config = list(sbio_init[:-2])
    sbio_config[0] = (
        "# generated by scripts/sep/arts-manifest-plan.py; run once after a "
        "successful combined commit in a fresh boot"
    )
    sbio_config[1] = (
        "# Publishes SBIO OOL, proves init 0x73, then sends exactly one "
        "read-only GetProtectedConfig 0x2b request"
    )
    sbio_config = [
        line.replace("echo '== final'", "echo '== after init'")
        for line in sbio_config
    ]
    sbio_config += [
        "echo 'GATE sbio-init=pass'",
        "echo '== preflight before one read-only GetProtectedConfig request'",
        "cat \"$D/status\"; cat \"$D/fifo\"",
        "grep -q '^tx=15 rx=22 tx_busy=0 dropped=0 ' \"$D/status\" || { echo 'STOP: counters changed after init'; exit 1; }",
        "grep -q '^sbio_ep=8 sbio_ready=1$' \"$D/status\" || { echo 'STOP: SBIO is not initialized'; exit 1; }",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO busy before GetProtectedConfig'; exit 1; }",
        "grep -q '^sep->ap recv_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: SEP->AP FIFO busy before GetProtectedConfig'; exit 1; }",
        f"echo '== send exactly one framed GetProtectedConfig: opcode 0x2b, user {SBIO_USER_ID}, expect 32 bytes'",
        f"if ! echo '08 2b {SBIO_USER_PAYLOAD}' >\"$D/xfer\"; then",
        "  echo 'STOP: GetProtectedConfig failed or timed out; do not retry in this boot'",
        "  cat \"$D/xfer\"; cat \"$D/status\"; cat \"$D/fifo\"; dmesg | grep sep-a9 | tail -50; exit 1",
        "fi",
        "sleep 0.2",
        "X=$(mktemp /tmp/sep-protected-config.XXXXXX)",
        "trap 'unlink \"$X\" 2>/dev/null || true' EXIT HUP INT TERM",
        "echo '== final'; cat \"$D/xfer\" | tee \"$X\"; cat \"$D/status\"; cat \"$D/endpoints\"; cat \"$D/log\"; cat \"$D/fifo\"",
        "grep -q '^out_len=32$' \"$X\" || { echo 'STOP: GetProtectedConfig reply length is not 32'; exit 1; }",
        "sed -n '2p' \"$X\" | grep -Eq '^[0-9a-f]{64}$' || { echo 'STOP: GetProtectedConfig payload is not exactly 32 bytes of hex'; exit 1; }",
        "grep -q '^ep=0x08 cmd=0x2b ret=0 sep_status=0x0$' \"$X\" || { echo 'STOP: GetProtectedConfig completion differs'; exit 1; }",
        "grep -q '^gt_reply version=1 total=32 offset=0 flags=0x0 reserved=0x0 command=0x2b chunk=32 mailbox_data=0x0$' \"$X\" || { echo 'STOP: GetProtectedConfig GenericTransfer header differs'; exit 1; }",
        "grep -q '^tx=16 rx=23 tx_busy=0 dropped=0 ' \"$D/status\" || { echo 'STOP: GetProtectedConfig counters differ'; exit 1; }",
        "grep -q 'endpoints_complete=8 published=3' \"$D/status\" || { echo 'STOP: endpoint state changed during GetProtectedConfig'; exit 1; }",
        "grep -q '^sbio_ep=8 sbio_ready=1$' \"$D/status\" || { echo 'STOP: SBIO readiness changed'; exit 1; }",
        "test \"$(tail -n 1 \"$D/log\")\" = 'ep=08 tag=fc op=2b param=00 data=00000000' || { echo 'STOP: final mailbox word is not the expected 0x2b completion'; exit 1; }",
        "grep -q '^ap->sep snd_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: AP->SEP FIFO busy after GetProtectedConfig'; exit 1; }",
        "grep -q '^sep->ap recv_sts=.* (empty)$' \"$D/fifo\" || { echo 'STOP: SEP->AP FIFO busy after GetProtectedConfig'; exit 1; }",
        "printf 'protected_config_hex='; sed -n '2p' \"$X\"",
        "echo 'RESULT sbio-protected-config=pass'",
        "echo 'STOP: collect logs and cold-reset; do not send another SBIO command in this boot'",
    ]
    (out / "remote-sbio-config.sh").write_text("\n".join(sbio_config) + "\n")

    print(
        f"GO: root shmem {pa:#x}; arts ep {arts_ep:#04x}; "
        f"xarm ep {xarm_ep:#04x} tag {xarm_tag:#04x} raw {xarm_raw:016x}; "
        f"IM4M {MANIFEST_SIZE} bytes {digest}"
    )
    print(f"wrote {out / 'plan.env'}")
    print(f"wrote {out / 'remote-arts-prep.sh'}")
    print(f"wrote {out / 'remote-arts-commit.sh'}")
    print(f"wrote {out / 'remote-combined-prep.sh'}")
    print(f"wrote {out / 'remote-combined-commit.sh'}")
    print(f"wrote {out / 'remote-sbio-init.sh'}")
    print(f"wrote {out / 'remote-sbio-config.sh'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
