#!/usr/bin/env bash
# Upload + load + warm reboot + wait for a NEW boot_id + health gate.
# Usage: kexec-host.sh <m1n1-payload|Image|Image.gz> [dtb] [cmdline]
# INITRD=path overrides initrd (otherwise payload's or running entry's).
# ENTRY=name overrides the unique test name; WAIT_SECONDS=240 sets the deadline.
# Leaves default unchanged unless SET_DEFAULT=1 (after a successful boot).
# HEALTH_GATE=0 skips kexec-health-gate.py (NVMe crawl check, ~2 min);
# HEALTH_GATE_ARGS adds its options. Exit 3 = booted but unhealthy.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
[ $# -ge 1 ] && [ $# -le 3 ] || { sed -n '2,8p' "$0" >&2; exit 2; }
WAIT_SECONDS=${WAIT_SECONDS:-240}
[[ "$WAIT_SECONDS" =~ ^[0-9]+$ ]] || exit 2
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
old=$("$SSH" 'cat /proc/sys/kernel/random/boot_id')
[[ "$old" =~ ^[0-9a-f-]{36}$ ]] || { echo "bad initial boot_id: $old" >&2; exit 1; }
board=$("$SSH" "tr '\\0' '\\n' </proc/device-tree/compatible | head -1")
args=("$1" "$stage" --board "$board")
[ -z "${2:-}" ] || args+=(--dtb "$2")
[ $# -lt 3 ] || args+=(--cmdline "$3")
[ -z "${INITRD:-}" ] || args+=(--initrd "$INITRD")
python3 -I "$ROOT/scripts/boot/prepare-entry.py" "${args[@]}"
digest=$(cd "$stage" && shasum -a 256 Image.gz | cut -c1-12)
NAME=${ENTRY:-test-$digest-$(date +%s)}
[[ "$NAME" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo 'invalid ENTRY' >&2; exit 2; }
"$ROOT/scripts/boot/upload-entry.sh" "$NAME" "$stage"
# Fail here, while SSH works, if load/DT/SMP checks fail.
"$SSH" "sudo -n /usr/local/bin/linux-a9-kexec load $NAME"
# Detached: closing SSH cannot cancel the shutdown request.
"$SSH" 'sudo -n systemd-run --unit=linux-a9-host-kexec --on-active=2s /usr/bin/systemctl kexec'
echo "waiting up to ${WAIT_SECONDS}s for $NAME (old boot_id $old)" >&2
deadline=$((SECONDS + WAIT_SECONDS))
while [ "$SECONDS" -lt "$deadline" ]; do
    result=$("$SSH" 'printf "boot_id="; cat /proc/sys/kernel/random/boot_id; uname -a; cat /proc/cmdline' 2>/dev/null) || result=
    new=$(printf '%s\n' "$result" | sed -n 's/^boot_id=//p')
    if [[ "$new" =~ ^[0-9a-f-]{36}$ ]] && [ "$new" != "$old" ]; then
        printf '%s\n' "$result"
        case " $result " in
        *"linux_a9.entry=$NAME "*) ;;
        *) echo "a different entry booted; expected $NAME" >&2; exit 1 ;;
        esac
        if [ "${HEALTH_GATE:-1}" != 0 ]; then
            # shellcheck disable=SC2086 # HEALTH_GATE_ARGS is a list of options
            python3 -I "$ROOT/scripts/boot/kexec-health-gate.py" \
                --expect-boot-id "$new" ${HEALTH_GATE_ARGS:-} || {
                echo "UNHEALTHY BOOT $NAME ($new): do not run tests on it." >&2
                echo 'See docs/boot/ds-kexec-nvme-crawl.md; kexec to a known-good entry or recover via DFU.' >&2
                exit 3
            }
        fi
        if [ "${SET_DEFAULT:-0}" = 1 ]; then
            "$SSH" "sudo -n /usr/local/bin/linux-a9-kexec set-default $NAME"
        fi
        exit 0
    fi
    sleep 2
done
echo 'timeout: no new SSH boot_id; inspect console/netconsole. Recover with buttons -> DFU -> scripts/boot.sh.' >&2
exit 1
