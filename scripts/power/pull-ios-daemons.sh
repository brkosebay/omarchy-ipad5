#!/usr/bin/env bash
# HOST script, run by the orchestrator only while jailbroken iOS is running.
# Pull bounded, readable files; no sudo, device execution of binaries or DMG.
set -euo pipefail
cd "$(dirname "$0")/../.."
targets() {
    cat <<'PATHS'
/usr/libexec/powerd
/System/Library/CoreServices/powerd.bundle/powerd
/usr/libexec/thermalmonitord
/usr/libexec/powerdatad
/usr/libexec/PowerUIAgent
/System/Library/CoreServices/PowerUIAgent
/usr/libexec/perfdiagsselfenabled
/usr/libexec/PerfPowerServices
/usr/libexec/PerfPowerServicesExtended
/usr/libexec/perfpowerservicesd
/usr/libexec/PerfPowerTelemetryClientRegistrationService
/System/Library/PrivateFrameworks/PowerLog.framework/XPCServices/PerfPowerTelemetryClientRegistrationService.xpc/PerfPowerTelemetryClientRegistrationService
/System/Library/PrivateFrameworks/PowerLog.framework/XPCServices/PerfPowerTelemetryClientRegistrationService.xpc/Info.plist
/usr/bin/powermetrics
/usr/bin/ioreport
/usr/local/bin/powermetrics
/usr/local/bin/ioreport
/usr/share/sandbox/powerd.sb
/usr/share/sandbox/thermalmonitord.sb
/usr/share/sandbox/PerfPowerServices.sb
/usr/share/sandbox/PerfPowerServicesExtended.sb
/usr/share/sandbox/perfpowerservicesd.sb
/usr/share/sandbox/temporary-sandbox.sb
/System/Library/LaunchDaemons/com.apple.powerd.plist
/System/Library/LaunchDaemons/com.apple.thermalmonitord.plist
/System/Library/LaunchDaemons/com.apple.powerdatad.plist
/System/Library/LaunchDaemons/com.apple.PowerUIAgent.plist
/System/Library/LaunchDaemons/com.apple.perfdiagsselfenabled.plist
/System/Library/LaunchDaemons/com.apple.PerfPowerServices.plist
/System/Library/LaunchDaemons/com.apple.PerfPowerServicesExtended.plist
/System/Library/LaunchDaemons/com.apple.perfpowerservices.plist
/System/Library/LaunchDaemons/com.apple.perfpowerservicesd.plist
PATHS
}
if [ "${1:-}" = --list ] && [ "$#" -eq 1 ]; then targets; exit 0; fi
[ "$#" -le 1 ] || { echo "usage: $0 [new-output-directory | --list]" >&2; exit 2; }
out=${1:-build/power/ios-r3/daemons}
case "$out" in -*) echo 'Output path must not begin with -.' >&2; exit 2;; esac
command -v ldid >/dev/null || { echo 'Install host ldid to extract signatures.' >&2; exit 2; }
umask 077
mkdir -p "$(dirname "$out")"
mkdir "$out" # Never merge captures from different boots.
remote_script="$out/pull-remote.sh"
{
    cat <<'REMOTE'
set -eu
umask 077
stage=$(mktemp -d /tmp/a9-power-daemons.XXXXXX)
trap 'rm -rf "$stage"' 0
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
mkdir "$stage/payload"
(date -u; uname -a; id; sysctl kern.osversion || :) > "$stage/context.txt" 2>&1
printf 'status\tdevice-path\n' > "$stage/manifest.tsv"
while IFS= read -r path; do
    if [ ! -f "$path" ]; then status=absent
    elif [ ! -r "$path" ]; then status=unreadable
    else
        mkdir -p "$stage/payload${path%/*}"
        if cp "$path" "$stage/payload$path"; then status=copied; else status=copy-failed; fi
    fi
    printf '%s\t%s\n' "$status" "$path" >> "$stage/manifest.tsv"
done <<'PATHS'
REMOTE
    targets
    cat <<'REMOTE'
PATHS
tar -C "$stage" -cf - context.txt manifest.tsv payload
REMOTE
} > "$remote_script"
scripts/ipad-ssh.sh 'sh -s' < "$remote_script" > "$out/files.tar"
tar -xf "$out/files.tar" -C "$out"
rm "$out/files.tar" # Only our successfully extracted transfer archive.
python3 - "$out" <<'PY'
import hashlib, json, plistlib, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1])
rows = []
hashes = []
for binary in sorted((root / 'payload').rglob('*')):
    if not binary.is_file(): continue
    raw = binary.read_bytes()
    rel = binary.relative_to(root / 'payload')
    digest = hashlib.sha256(raw).hexdigest()
    hashes.append(f'{digest}  payload/{rel}\n')
    if raw[:4] not in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        continue
    proc = subprocess.run(['ldid', '-e', str(binary)], capture_output=True)
    ent_path = root / 'entitlements' / (str(rel) + '.plist')
    ent_path.parent.mkdir(parents=True, exist_ok=True)
    ent_path.write_bytes(proc.stdout)
    row = {'path': '/' + str(rel), 'sha256': digest, 'ldid_exit': proc.returncode,
           'entitlements': str(ent_path.relative_to(root))}
    try:
        ent = plistlib.loads(proc.stdout)
        row['access_keys'] = {k:v for k,v in ent.items() if any(s in k.lower() for s in
            ('ioreport', 'iokit', 'sandbox', 'container', 'platform', 'perfpow', 'thermal'))}
    except (ValueError, plistlib.InvalidFileException):
        row['error'] = 'No parseable XML entitlement blob; inspect original signature'
    if proc.stderr:
        row['ldid_stderr'] = proc.stderr.decode(errors='replace')
    rows.append(row)
(root / 'SHA256SUMS').write_text(''.join(hashes))
(root / 'entitlement-summary.json').write_text(json.dumps(rows, indent=2) + '\n')
if not rows: raise SystemExit('No Mach-O binaries pulled; inspect manifest.tsv')
print(f'Pulled {len(rows)} binaries; inspect {root}/entitlement-summary.json and manifest.tsv')
PY
