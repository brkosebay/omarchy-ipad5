#!/usr/bin/env python3
"""Fetch small, pinned upstream idle/resume references; never access a device."""
import hashlib
import json
from pathlib import Path
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/idle/resume-sources'
SOURCES = {
    'xnu': ('apple-oss-distributions/xnu', 'xnu-8796.141.3', [
        'osfmk/arm64/cpu.c', 'osfmk/arm64/arm_init.c', 'osfmk/arm64/start.s', 'osfmk/arm64/cswitch.s',
        'osfmk/arm64/machine_routines_asm.s', 'osfmk/arm64/proc_reg.h',
        'pexpert/pexpert/arm64/apple_arm64_common.h', 'pexpert/pexpert/arm64/H7.h',
        'pexpert/pexpert/arm64/board_config.h', 'pexpert/pexpert/arm64/S8000.h',
        'pexpert/pexpert/arm64/S8003.h',
    ]),
    'm1n1': ('HoolockLinux/m1n1', 'd5a10ac52a', [
        'src/smp.c', 'src/start.S', 'src/utils_asm.S', 'src/utils.c', 'src/utils.h', 'src/cpu_regs.h',
        'src/chickens.c', 'src/chickens/twister.c', 'src/cpufreq.c', 'src/kboot.c',
    ]),
    'asahi': ('AsahiLinux/linux', '94fb23346d522edf53722357c426a3e58030beea', [
        'drivers/cpuidle/cpuidle-apple.c', 'arch/arm64/kernel/cpuidle.c',
        'arch/arm64/kernel/cpuidle-apple.S', 'arch/arm64/kernel/sleep.S',
        'arch/arm64/kernel/suspend.c', 'arch/arm64/include/asm/cpuidle.h',
    ]),
}


def get(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'linux-a9-idle-research'})
    with urllib.request.urlopen(request, timeout=45) as response:
        return response.read()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for name, (repo, ref, paths) in SOURCES.items():
        # Retain one exact commit even when the requested reference is a branch.
        identity = json.loads(get(f'https://api.github.com/repos/{repo}/commits/{ref}'))
        commit = identity['sha']
        files = []
        for path in paths:
            url = f'https://raw.githubusercontent.com/{repo}/{commit}/{path}'
            try:
                raw = get(url)
            except urllib.error.HTTPError as error:
                if error.code != 404:
                    raise
                files.append({'path': path, 'url': url, 'missing': True})
                continue
            destination = OUT / name / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(raw)
            files.append({'path': path, 'url': url, 'sha256': hashlib.sha256(raw).hexdigest()})
        manifest[name] = {'repo': repo, 'requested_ref': ref, 'commit': commit, 'files': files}
        print(name, commit, 'files', sum(not f.get('missing') for f in files), flush=True)
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
