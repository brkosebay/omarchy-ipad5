#!/usr/bin/env python3
"""Read firmware over SSH, preserve sources, and stage explicit J71t profiles.

No remote writes, raw block devices, NVRAM tools, or device boot actions.
--dry-run never connects. --source supports a local rootfs-shaped directory.
BlueTool selection follows Project Sandcastle hcdpack's BRCMcfg records;
no J172 chip revision, module manufacturer, or NVRAM profile is assumed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
WIFI_ROOTS = (
    "/usr/share/firmware/wifi",
    "/System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware",
)
TOUCH_ROOTS = (
    "/usr/share/firmware/multitouch",
    "/System/Library/PrivateFrameworks/MultitouchSupport.framework",
    "/System/Library/Extensions/AppleMultitouchSPI.kext",
)
CALIBRATIONS = {
    "alsCalibration": "LSCI", "multi-touch-calibration": "MtCl",
    "prox-calibration": "PxCl", "bluetooth-tx-calibration": "BTTx",
    "bluetooth-rx-calibration": "BTRx", "bluetooth-taurus-calibration": "BCAL",
    "wifi-calibration-msf": "WCAL", "rx-calibration-2.4": "W24R",
    "rx-calibration-5.0": "W50R", "rx-calibration-temp": "WRxT",
    "freq-group-2g-calibration": "FG2G", "usb-input-limit-calibration": "CBAT",
}


def ioreg_string(value):
    """Decode ioreg's quoted Latin-1 byte rendering, without UTF-8 loss."""
    def unescape(match):
        token = match[0][1:]
        if token in ('\\', '"'):
            return token
        if re.fullmatch(r"[0-7]{1,3}", token):
            return chr(int(token, 8))
        if re.fullmatch(r"x[0-9a-fA-F]{2}", token):
            return chr(int(token[1:], 16))
        if token in ('n', 'r', 't'):
            return {'n': '\n', 'r': '\r', 't': '\t'}[token]
        raise ValueError("unsupported escape in ioreg byte string")
    return re.sub(r'\\(?:x[0-9a-fA-F]{2}|[0-7]{1,3}|.)', unescape, value).encode('latin1')


def parse_ioreg(data):
    """Read XML/binary plist or text `ioreg -l -p ... -w0` as flat nodes.

    Text records retain node paths and source line numbers for private evidence.
    Only top-level node properties are parsed; nested diagnostic dictionaries
    are irrelevant to calibration. Split on LF, not str.splitlines(): bytes
    such as 0x85 can legitimately occur in quoted Latin-1 NSData.
    """
    if data.lstrip().startswith((b'<?xml', b'<plist', b'bplist')):
        return plistlib.loads(data)
    nodes, stack = [], []
    node = None
    for number, line in enumerate(data.decode('latin1').split('\n'), 1):
        header = re.match(r'^([ |]*)\+-o (.*?)\s+<class ', line)
        if header:
            depth = len(header[1])
            while stack and stack[-1][0] >= depth:
                stack.pop()
            path = '/'.join([item[1] for item in stack] + [header[2]])
            node = {'IORegistryEntryName': header[2], '_path': path,
                    '_line': number, '_property_lines': {}}
            nodes.append(node)
            stack.append((depth, header[2]))
            continue
        prop = re.match(r'^[ |]*"([^"\n]+)" = (.*)\r?$', line)
        if not prop or node is None:
            continue
        key, value = prop.groups()
        if value.startswith('<') and value.endswith('>'):
            body = value[1:-1]
            if re.fullmatch(r'[0-9a-fA-F\s]*', body):
                try:
                    value = bytes.fromhex(body)
                except ValueError as error:
                    raise ValueError(f'invalid hex data at ioreg line {number}') from error
            else:
                strings = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
                if not strings or re.sub(r'"(?:[^"\\]|\\.)*"|[\s,]', '', body):
                    raise ValueError(f'unsupported byte data at ioreg line {number}')
                # IOData renders multiple C strings as comma-separated strings.
                # A single printable NSData (e.g. a MAC) has no synthetic NUL.
                value = b'\0'.join(ioreg_string(s) for s in strings)
        elif re.fullmatch(r'"(?:[^"\\]|\\.)*"', value):
            value = ioreg_string(value[1:-1]).decode('latin1')
        elif re.fullmatch(r'-?[0-9]+', value):
            value = int(value)
        else:
            continue
        if key in node and node[key] != value:
            raise ValueError(f'conflicting ioreg property {key} at line {number}')
        node[key] = value
        node['_property_lines'][key] = number
    if not nodes:
        raise ValueError('not a supported text ioreg dump or plist')
    return nodes


def live_radio_profile(data):
    """Cross-check the IOService HWIdentifiers and actually requested files."""
    fields, requests = [], []
    evidence = {}
    for number, line in enumerate(data.decode('latin1').split('\n'), 1):
        if re.search(r'"HWIdentifiers" = ', line):
            pairs = re.findall(r'"([CMmVsP])"=(?:"([A-Za-z0-9.]+)"|([0-9]+))', line)
            item = {k: quoted or integer for k, quoted, integer in pairs}
            if item.get('C') == '4355' and item.get('P') == 'feara':
                fields.append(item)
                evidence['HWIdentifiers_line'] = number
        if re.search(r'"RequestedFiles" = ', line) and 'P-feara_' in line:
            requests.append(dict(re.findall(r'"(Firmware|TxCap|Regulatory|NVRAM)"="([A-Za-z0-9_./-]+)"', line)))
            evidence['RequestedFiles_line'] = number
    if not fields or any(item != fields[0] for item in fields) or not requests:
        raise ValueError('missing or conflicting Feara HWIdentifiers/RequestedFiles')
    item = fields[0]
    if set(item) != set('CMmVsP') or not re.fullmatch(r'[A-Z][0-9]', item['s']):
        raise ValueError('incomplete live Feara module identity')
    chip = f"C-4355__s-{item['s']}"
    nvram = f"P-feara_M-{item['M']}_V-{item['V']}__m-{item['m']}.txt"
    expected = {k: chip + '/' + name for k, name in
                [('Firmware', 'feara.trx'), ('TxCap', 'feara.txcb'),
                 ('Regulatory', 'feara.clmb'), ('NVRAM', nvram)]}
    if any(request != expected for request in requests):
        raise ValueError('live RequestedFiles disagree with HWIdentifiers')
    vendor = {'m': 'MUR', 'u': 'USI', 't': 'TDK'}.get(item['V'])
    if not vendor:
        raise ValueError('unknown live module manufacturer')
    return chip, nvram, f"BCM4355{item['s']} FearA {vendor} MCC", evidence


def wifi_prefix(chip):
    # Verified in this kernel's brcmf_pcie_fwnames: C0 uses 4355, NOT 4355c0.
    rev = chip.rsplit('-', 1)[1].lower()
    return 'brcmfmac4355' + ('' if rev == 'c0' else rev) + '-pcie'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe_relative(path):
    q = PurePosixPath(path)
    if not q.is_absolute() or ".." in q.parts or any(c in path for c in "\n\r\0"):
        raise ValueError(f"unsafe source path: {path!r}")
    return Path(*q.parts[1:])


def hcd_images(data):
    """Return complete, named BRCMcfgS/D HCD streams; reject truncation."""
    marker = b"BRCMcfgS"
    cursor = 0
    while True:
        marker_at = data.find(marker, cursor)
        if marker_at < 0:
            return
        start = marker_at - 7
        cursor = marker_at + len(marker)
        if start < 0 or data[start:start + 2] != b"\x4c\xfc":
            continue
        pos = start
        name = None
        count = 0
        while pos + 3 <= len(data):
            op = data[pos:pos + 2]
            length = data[pos + 2]
            end = pos + 3 + length
            if op not in (b"\x4c\xfc", b"\x4e\xfc") or end > len(data):
                break
            payload = data[pos + 3:end]
            if count and payload[4:12] == marker:
                break
            if payload[4:12] == b"BRCMcfgD" and len(payload) >= 23:
                n = payload[22]
                if 23 + n <= len(payload):
                    name = payload[23:23 + n].rstrip(b"\0").decode("ascii", "strict")
            count += 1
            pos = end
            if op == b"\x4e\xfc":
                if name and count >= 2:
                    yield {"name": name, "offset": start, "data": data[start:pos]}
                cursor = pos
                break


class Source:
    def __init__(self, args):
        self.local = args.source.resolve() if args.source else None
        self.ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                    "-p", str(args.port), args.host] if args.host else None

    def command(self, command):
        return subprocess.check_output(self.ssh + [command], timeout=90)

    def read(self, path):
        rel = safe_relative(path)
        if self.local:
            p = (self.local / rel).resolve()
            p.relative_to(self.local)
            return p.read_bytes()
        return self.command("cat " + shlex.quote(path))

    def files(self):
        roots = WIFI_ROOTS + TOUCH_ROOTS
        if self.local:
            result = []
            for root in roots:
                base = self.local / safe_relative(root)
                if base.is_dir():
                    result += ["/" + str(p.relative_to(self.local))
                               for p in base.rglob("*") if p.is_file()]
            return sorted(set(result))
        # Missing optional roots are expected on some iOS versions. Failure of
        # every root is reported after discovery, not hidden by an empty manifest.
        command = "\n".join(
            "if test -d {0}; then find {0} -type f; fi".format(shlex.quote(r))
            for r in roots)
        return sorted(set(self.command(command).decode().splitlines()))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    source = ap.add_mutually_exclusive_group()
    source.add_argument("--host", help="root@iPad address (SSH reads only)")
    source.add_argument("--source", type=Path, help="offline rootfs-shaped source")
    ap.add_argument("--port", type=int, default=22)
    ap.add_argument("--dry-run", action="store_true", help="print plan without SSH")
    ap.add_argument("--output", type=Path, default=ROOT / "build/firmware")
    ap.add_argument("--chip", help="confirmed OTP chip tag, e.g. C-4355__s-C0")
    ap.add_argument("--nvram", help="exact confirmed OTP profile, e.g. P-feara_M-...txt")
    ap.add_argument("--bt-name", help="exact BlueTool candidate name to select")
    ap.add_argument("--ioreg", type=Path, help="local live IODeviceTree text (Latin-1) or XML plist")
    ap.add_argument("--ioservice", type=Path, help="local text IOService evidence; selects verified Wi-Fi profile and infers BT vendor")
    ap.add_argument("--read-ioreg", action="store_true", help="also run read-only ioreg on iOS")
    args = ap.parse_args()
    if args.dry_run:
        print(json.dumps({"roots": WIFI_ROOTS + TOUCH_ROOTS,
                          "read": ["/usr/sbin/BlueTool"],
                          "destination": str(args.output),
                          "profile_selection": "requires --chip, --nvram, --bt-name",
                          "remote_writes": False}, indent=2))
        return
    if not args.host and not args.source:
        ap.error("supply --host or --source; --dry-run needs neither")
    if args.host and (args.host.startswith("-") or not re.fullmatch(r"[A-Za-z0-9_.@:-]+", args.host)):
        ap.error("invalid SSH target")
    if not 1 <= args.port <= 65535:
        ap.error("invalid SSH port")
    radio_evidence = None
    if args.ioservice:
        chip, nvram, bt_name, radio_evidence = live_radio_profile(args.ioservice.read_bytes())
        for attr, selected in [('chip', chip), ('nvram', nvram), ('bt_name', bt_name)]:
            if getattr(args, attr) not in (None, selected):
                ap.error(f'--{attr.replace("_", "-")} disagrees with live IOService')
            setattr(args, attr, selected)
    if args.chip and not re.fullmatch(r"C-4355__s-[A-Z][0-9]", args.chip):
        ap.error("--chip must be an exact BCM4355 OTP tag")
    if args.nvram and (Path(args.nvram).name != args.nvram or not re.fullmatch(r"P-feara_[A-Za-z0-9_.-]+\.txt", args.nvram)):
        ap.error("--nvram must be a basename for the J71t feara module")
    if bool(args.chip) != bool(args.nvram):
        ap.error("Wi-Fi staging needs both --chip and --nvram")
    if args.bt_name and not args.nvram:
        ap.error("Bluetooth staging requires the same confirmed --chip and --nvram profile")
    out = args.output.resolve()
    out.relative_to(ROOT / "build/firmware")
    os.umask(0o077)
    out.mkdir(parents=True, exist_ok=True)
    records = []

    def write(rel, data, origin):
        p = out / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if p.exists() and p.read_bytes() != data and rel != 'discovered-paths.txt':
            raise ValueError(f"refusing to overwrite different firmware: {p}")
        p.write_bytes(data)
        p.chmod(0o600)
        records.append({"path": rel, "source": origin, "size": len(data), "sha256": sha(data)})

    src = Source(args)
    paths = src.files()
    selected = [p for p in paths if
                ("feara" in Path(p).name.lower() and any(p.startswith(r + "/") for r in WIFI_ROOTS))
                or (any(p.startswith(r + "/") for r in TOUCH_ROOTS)
                    and re.search(r"c1f15|j71|j72|multitouch.*\.(plist|im4p|mtfw|bin)$", p, re.I))]
    found = {}
    for path in selected:
        data = src.read(path)
        write("sources/" + str(safe_relative(path)), data, path)
        found[path] = data
    bluetool = src.read("/usr/sbin/BlueTool")
    write("sources/usr/sbin/BlueTool", bluetool, "/usr/sbin/BlueTool")
    candidates = list(hcd_images(bluetool))
    write("bluetooth-candidates.json", (json.dumps([
        {"name": c["name"], "offset": c["offset"], "size": len(c["data"]), "sha256": sha(c["data"])}
        for c in candidates if "BCM4355" in c["name"]], indent=2) + "\n").encode(), "/usr/sbin/BlueTool")

    if args.bt_name:
        if not re.match(r"BCM4355[A-Z][0-9] (?:.* )?FearA (MUR|USI|TDK)(?: |$)", args.bt_name, re.I):
            raise ValueError("--bt-name must identify a BCM4355 Feara image and its manufacturer")
        matching = [c for c in candidates if c["name"] == args.bt_name]
        if not matching or len({sha(c["data"]) for c in matching}) != 1:
            raise ValueError("Bluetooth image missing or ambiguous; inspect bluetooth-candidates.json")
        revision = re.match(r"BCM4355([A-Z][0-9])", args.bt_name)[1]
        if not args.chip or not args.chip.endswith("s-" + revision):
            raise ValueError("Bluetooth revision must match --chip confirmed from OTP/iOS")
        vendor = re.search(r"_V-([mut])(?:_|$)", args.nvram)
        expected = {"m": "MUR", "u": "USI", "t": "TDK"}
        if not vendor or not re.search(r" FearA " + expected[vendor[1]] + r"(?: |$)", args.bt_name, re.I):
            raise ValueError("Bluetooth manufacturer must match the confirmed Wi-Fi OTP profile")
        write("lib/firmware/brcm/BCM.apple,j71t.hcd", matching[0]["data"],
              "/usr/sbin/BlueTool: " + args.bt_name)

    if bool(args.chip) != bool(args.nvram):
        raise ValueError("Wi-Fi staging needs both --chip and --nvram")
    if args.chip:
        rev = args.chip.rsplit("-", 1)[1]
        chip_dirs = (args.chip, "4355" + rev)
        profile = [p for p in found if any(c in p.split("/") for c in chip_dirs)
                   and Path(p).name == args.nvram]
        if len(profile) != 1:
            raise ValueError("exact NVRAM profile not found uniquely in discovered iOS firmware roots")
        directory = str(PurePosixPath(profile[0]).parent)
        prefix = wifi_prefix(args.chip)
        tags = re.fullmatch(r"P-(feara)_M-([^_]+)_V-([^_]+)__m-(.+)\.txt", args.nvram)
        if not tags:
            raise ValueError("unrecognized OTP profile layout; preserve source and inspect before staging")
        board = "apple," + "-".join(tags.groups())
        # Exact OTP profile aliases only; no unqualified/global fallback NVRAM.
        for suffix, name in (("bin", "feara.trx"), ("clm_blob", "feara.clmb"),
                             ("txcap_blob", "feara.txcb"), ("txt", args.nvram)):
            path = directory + "/" + name
            if path not in found:
                raise ValueError(f"missing {path}; no complete Wi-Fi set staged")
        for suffix, name in (("bin", "feara.trx"), ("clm_blob", "feara.clmb"),
                             ("txcap_blob", "feara.txcb"), ("txt", args.nvram)):
            path = directory + "/" + name
            for country in ("", "-XX"):
                write(f"lib/firmware/brcm/{prefix}.{board}{country}.{suffix}", found[path], path)

    ioreg = args.ioreg.read_bytes() if args.ioreg else None
    if args.read_ioreg:
        if args.source:
            raise ValueError("use --ioreg for an offline source")
        ioreg = src.command("ioreg -l -p IODeviceTree -w0")
    if ioreg:
        tree = parse_ioreg(ioreg)
        ext = 'plist' if ioreg.lstrip().startswith((b'<?xml', b'<plist', b'bplist')) else 'txt'
        write("private/IODeviceTree." + ext, ioreg, "live iOS IODeviceTree (read-only)")
        calibrations = {}
        bt_addresses = []

        def walk(node):
            if isinstance(node, dict):
                for k, v in node.items():
                    if k in CALIBRATIONS and isinstance(v, bytes) and v:
                        if v.startswith((b"syscfg/", b"macaddr/")):
                            continue
                        key = CALIBRATIONS[k]
                        if key in calibrations and calibrations[key] != v:
                            raise ValueError(f"conflicting live calibration values for {key}")
                        calibrations[key] = v
                    if k == 'local-mac-address' and isinstance(v, bytes) and len(v) == 6:
                        name = node.get('IORegistryEntryName', '').split('@')[0]
                        if name in ('wlan', 'bluetooth'):
                            write('private/' + ('WMac' if name == 'wlan' else 'BMac') + '.bin', v,
                                  'IORegistry:' + node.get('_path', name) + ':' + k)
                            if name == 'bluetooth':
                                bt_addresses.append((v, 'IORegistry:' + node.get('_path', name) + ':' + k))
                    if k in ("mac-address-wifi0", "mac-address-bluetooth0") and isinstance(v, bytes) and len(v) == 6:
                        write("private/" + ("WMac" if "wifi" in k else "BMac") + ".bin", v, "IORegistry:" + k)
                        if k == 'mac-address-bluetooth0':
                            bt_addresses.append((v, 'IORegistry:' + k))
                    walk(v)
            elif isinstance(node, list):
                for v in node:
                    walk(v)
        walk(tree)
        if bt_addresses:
            if len({v for v, _ in bt_addresses}) != 1:
                raise ValueError('conflicting live Bluetooth addresses')
            address, origin = bt_addresses[0]
            if not any(address) or address[0] & 1:
                raise ValueError('Bluetooth address must be a nonzero unicast address')
            write('lib/firmware/apple/j71t-BMac.bin', address, origin + ' (display byte order)')
        for key, data in calibrations.items():
            write("private/" + key + ".bin", data, "IORegistry:" + key)
        if "LSCI" in calibrations:
            data = calibrations["LSCI"]
            if len(data) < 0x48 or len(data) % 2 or data[1:4] != b"\x02\x01\x08" or int.from_bytes(data[4:6], "little") != len(data):
                raise ValueError("unsupported LSCI header; raw copy preserved but ALS file not staged")
            if sum(int.from_bytes(data[i:i+2], "little") for i in range(0, len(data), 2)) & 0xffff != 0xffff:
                raise ValueError("LSCI checksum invalid; ALS file not staged")
            write("lib/firmware/apple/j71t-LSCI.bin", data, "IORegistry:alsCalibration (syscfg/LSCI)")
        if "CBAT" in calibrations:
            data = calibrations["CBAT"]
            if len(data) != 24 or int.from_bytes(data[20:24], "little") != 1:
                raise ValueError("unsupported J71t CBAT record; raw private copy preserved")
            write("lib/firmware/apple/j71t-CBAT.bin", data,
                  "IORegistry:usb-input-limit-calibration (syscfg/CBAT)")

    if radio_evidence is not None:
        radio_evidence.update({
            'ioservice_sha256': sha(args.ioservice.read_bytes()),
            'chip': args.chip, 'nvram': args.nvram, 'bt_name': args.bt_name,
            'wifi_claim': 'verified: HWIdentifiers agree with RequestedFiles on this unit',
            'bluetooth_claim': 'inferred: same FearA combo-module vendor; confirm HCI patch revision on device',
            'wifi_prefix': wifi_prefix(args.chip),
        })
        write('selection.json', (json.dumps(radio_evidence, indent=2) + '\n').encode(),
              'live IOService + shared-module Bluetooth inference')

    write("discovered-paths.txt", ("\n".join(paths) + "\n").encode(), "read-only source discovery")
    (out / "manifest.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"Copied/staged {len(records)} files under {out}")
    if not args.chip or not args.bt_name:
        print("Inventory pass: inspect candidates and confirm OTP/module before supplying --chip, --nvram and --bt-name.")
    if not ioreg:
        print("Calibration not pulled: supply --ioreg or --read-ioreg for the live device's resolved records.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
