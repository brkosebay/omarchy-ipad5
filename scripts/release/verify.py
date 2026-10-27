#!/usr/bin/env python3
"""Read-only, offline public-tree audit. Values are never printed on failure.

Usage: python3 -B scripts/release/verify.py SNAPSHOT [--private-source WORKSPACE]
Use --existing-repo for an already published clone: remotes and multiple
commits are accepted, while reachable history is also checked offline.
The optional private-source adds the workspace scrubber's exact denylist.
Git metadata is checked separately: the initial commit intentionally uses the
owner's public GitHub noreply identity. Three published VXD upstream credits
are retained; other real email addresses are rejected.
"""
import argparse
import ipaddress
import json
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote

PUBLIC_EMAIL = 'brkosebay@users.noreply.github.com'
PUBLIC_NAME = 'Berkay Kösebay'
# Keep the scanner's vocabulary split so its own source is audited normally.
# Case-insensitive substring checks catch new filenames, exports and hooks.
GRAPHICS_TERMS = re.compile(r'g[p]u|g[5]p|a[g]x|g[f]x|p[o]wervr|r[o]gue|(?<![a-z])u[s]c(?![a-z])|t[i]ler|m[e]tal', re.I)
GRAPHICS_STATUS = 'G' 'PU: research ongoing privately, not included; software rendering only.'
GRAPHICS_README_ROW = '| G' 'PU acceleration | ❌ | Software rendering; work not included. |'
GRAPHICS_STATUS_FILES = {'README.md', 'docs/release/STATUS.md'}


def graphics_path(rel):
    """VXD is a video decoder, including its upstream branding in filenames."""
    path = str(rel)
    if path.startswith(('patches/vdec/', 'tools/ds-video/', 'scripts/vdec/')):
        path = re.sub(r'p[o]wervr[- _]+m[s]vdx', 'video-decoder', path, flags=re.I)
    return bool(GRAPHICS_TERMS.search(path))


def graphics_findings(rel, text):
    """Fail closed on unreviewed graphics material, including mixed files.

    Ordinary Linux DRM display paths and VXD's MSVDX brand are not graphics
    research. The status sentence is permitted only as an exact complete line
    in the two entry points. Touch ID's sensor name is deliberately unrelated.
    """
    findings = set()
    if graphics_path(rel):
        findings.add('excluded-graphics-path')
    for line in text.splitlines():
        if str(rel) in GRAPHICS_STATUS_FILES and line == GRAPHICS_STATUS:
            continue
        if str(rel)=='README.md' and line==GRAPHICS_README_ROW:
            continue
        # Linux places display-only KMS drivers in this standard namespace.
        scan = line.replace('drivers/g' 'pu/drm/', 'drivers/display/')
        if str(rel).startswith(('patches/vdec/', 'tools/ds-video/', 'scripts/vdec/')):
            scan = re.sub(r'p[o]wervr(?:[- _]+m[s]vdx| video decoder)', 'video-decoder', scan, flags=re.I)
        if GRAPHICS_TERMS.search(scan):
            findings.add('graphics-content')
    return findings
# Published upstream credits in the pinned VXD GPL/MIT sources. Preserve
# these exact notices: redacting them invalidates the source SHA-256 pins.
PUBLIC_UPSTREAM_EMAILS = frozenset({
    'angelabaker@ti.com', 'waldo.bastian@intel.com', 'li.zeng@intel.com',
})
EMAIL = re.compile(r'(?i)(?<![\w.+-])[\w.+-]+@[a-z0-9.-]+\.[a-z]{2,}(?![\w.-])')
# IORegistry accessory/cable keys do not use the device-serial prefixes.
ID_PREFIX = r'(?i)((?<![\w])(?:[\w-]*serial[ _-]?(?:number|string)|USB Serial Number|ecid|udid|imei|(?-i:SERIAL))[\s"\x27]*(?:[:=]|\b(?:is|was)\b)[\s"\x27]*)'
IDENTIFIER = re.compile(ID_PREFIX + r'(<[0-9a-f ]{12,}>|0x[0-9a-f]{8,}|[0-9a-f]{12,}|(?-i:[A-Z0-9][A-Z0-9-]{5,}))(?![\w-])')
CAL_KEY = r'(?:wifi[-_]calibration(?:[-_]msf)?|multi[-_]touch[-_]calibration|accel(?:erometer)?[-_]?calibration|alsCalibration|usb-input-limit-calibration|MtCl|CBAT|LSCI|WCAL)'
CALIBRATION = re.compile(r'(?i)(["\x27]?' + CAL_KEY + r'["\x27]?\s*[:=]\s*)(<[0-9a-f\s]{8,}>|<"(?!syscfg/)(?:[^"\\]|\\.)+">|["\x27](?:[0-9a-f]{16,}|[A-Za-z0-9+/]{24,}={0,2})["\x27])')
PLIST_PRIVATE = re.compile(r'(?is)(<key>[^<]*(?:serial|ecid|udid|password|passwd|psk|ssid|token|ApImg4Ticket|APTicket|BBTicket|' + CAL_KEY + r')[^<]*</key>\s*<(?:data|string|integer)>)(.*?)(</(?:data|string|integer)>)')
SECRET_KEY = r'\b(?:(?:[a-z][a-z0-9]*[_-])*(?:password|passwd|passphrase)|psk|ssid|(?:api|access|auth|bearer|refresh)[_-]?token|(?-i:TOKEN|SECRET|API_KEY|ACCESS_KEY|AWS_SECRET_ACCESS_KEY|WIFI_PASS|DEVICE_PASS|IOS_PASS|SSH_PASS))\b'
SECRET_EQUAL_KEY = r'(?:' + SECRET_KEY + r'|\b(?-i:PASS)\b)'
SECRET_ASSIGN = re.compile(r"(?im)((?:(?<![\w${])" + SECRET_EQUAL_KEY + r"[\"']?[\t ]*=(?!=)[\t ]*|(?<![\w${])" + SECRET_KEY + r"[\"'][\t ]*:[\t ]*|^[\t ]*" + SECRET_KEY + r"[\t ]*:[\t ]*))(?:([\"'])([^\n]*?)\2|([A-Za-z0-9_+/=-]{4,})(?![\w.(]))")
SECRET_CLI = re.compile(r'(?:sshpass\s+-p\s+|--(?:password|passwd|psk|token)(?:=|\s+))(["\x27]?)([A-Za-z0-9_+/!@#%=-]{4,})\1')
TOKENS = re.compile(r'(?i)\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|xox[baprs]-[A-Za-z0-9-]{12,}|sk-[A-Za-z0-9_-]{20,}|AKIA[A-Z0-9]{16})\b')
BASIC_AUTH = re.compile(r'(?i)(?:Authorization:\s*(?:Bearer|Basic)\s+)[A-Za-z0-9+/_.=-]{8,}')
URL_AUTH = re.compile(r'([a-z]+://[^/\s:@]+:)[^/\s@]+(@)')
MOBILE_SECRET = re.compile(r'/var/mobile/(?:[^\s"\x27`<>]*?(?:/\.ssh|/Keychains|/Accounts|/BiometricKit|/BioProtect|/com\.apple\.wifi|/[^/\s]*(?:password|secret|token))|\.ssh)(?:/[^\s"\x27`<>]*)?', re.I)
WIFI_COMMAND = re.compile(r'(\bnmcli\s+(?:dev(?:ice)?\s+)?wifi\s+connect\s+)(?:(["\x27])([^\n]*?)\2|([^\s;]+))')
WIFI_PASSWORD = re.compile(r'(\bpassword\s+)(?:(["\x27])([^\n]*?)\2|([^\s;]+))')
SHADOW = re.compile(r'\$(?:[156y]|2[aby])\$[A-Za-z0-9./$=,-]{12,}')
CHPASSWD = re.compile(r'(["\x27][\w-]+:)([^\s"\x27]+)(["\x27]\s*\|\s*(?:sudo\s+)?chpasswd)')


def placeholder(value):
    return (not value or value.upper().startswith(('REDACTED', 'YOUR_', 'EXAMPLE', 'PLACEHOLDER')) or
            value in ('None', 'True', 'False', 'null', 'none') or value.startswith(('$', '<', '{', '"', "'", '$( ')))


def real_email(value):
    return value.lower() not in PUBLIC_UPSTREAM_EMAILS | {PUBLIC_EMAIL} and not value.lower().endswith(('.invalid', '.test'))

MAC = re.compile(r'(?i)(?<![\w:])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![\w:])')
MAC_DASH = re.compile(r'(?i)(?<![\w-])(?:[0-9a-f]{2}-){5}[0-9a-f]{2}(?![\w-])')
IP = re.compile(r'(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])')
PATTERNS = {
    'absolute-home': re.compile(r'(?:/Users/[^/\s]+|/home/(?:berkay|brkosebay)\b)'),
    'private-key': re.compile(r'-----BEGIN [A-Z ]*PRIVATE KEY-----'),
    'owner-username': re.compile(r'\bberkay\b'),
    'modern-udid': re.compile(r'(?i)\b[0-9a-f]{8}-[0-9a-f]{16}\b'),
    'imei-like': re.compile(r'(?<![\w.])\d{15}(?![\w.])'),
    'apple-serial': re.compile(r'\b(?=[A-Z0-9]{10,12}\b)(?=[A-Z0-9]*[0-9])(?:DM|DL|F9|C0|F4|DMP)[A-Z0-9]{8,10}\b'),
}
FORBIDDEN_PARTS = {'build', 'third_party', '__pycache__', '.vdec-r3', 'node_modules'}
FORBIDDEN_SUFFIXES = {'.im4m','.shsh','.shsh2','.ipsw','.img','.dmg','.zip','.tar','.tgz','.pyc','.bin','.macho','.im4p','.img4','.m' 'etallib','.hex','.dis','.gz','.jpg','.png','.jpeg','.dylib','.o','.so'}

def forbidden_artifact(rel):
    p=Path(rel)
    return (any(x in FORBIDDEN_PARTS or x.startswith('.vdec') for x in p.parts) or
            p.suffix.lower() in FORBIDDEN_SUFFIXES or str(p).startswith('docs/adt/') or
            p.name in {'-rescored.json', 'snapshot-audit.json', 'snapshot-scrub.json',
                       'changed-files-audit.json', 'snapshot-verification.json',
                       'snapshot-validation.json', 'release-checklist.md'} or
            (p.suffix.lower() in ('.txt','.json','.plist','.dat','.dump') and
             bool(re.search(r'(?i)(?:syscfg|nvram|boot-ecid|kernelcache|calibration|(?:^|[-_.])(?:MtCl|CBAT|WCAL)(?:[-_.]|$))',p.name))))


def privacy_findings(text):
    findings={label for label,pat in PATTERNS.items() if pat.search(text)}
    if any(real_email(m[0]) for m in EMAIL.finditer(text)):findings.add('non-public-email')
    if any(not placeholder(m[2]) or m[2].startswith('<') for m in IDENTIFIER.finditer(text)):findings.add('labelled-identifier')
    if CALIBRATION.search(text):findings.add('calibration-blob')
    if any(not placeholder(m[2].strip()) for m in PLIST_PRIVATE.finditer(text)):findings.add('plist-private-data')
    if any(not placeholder(m[3] if m[2] else m[4]) for m in SECRET_ASSIGN.finditer(text)):findings.add('literal-credential')
    if any(not placeholder(m[2]) for m in SECRET_CLI.finditer(text)):findings.add('cli-credential')
    for label,pat in (('credential-token',TOKENS),('authorization-secret',BASIC_AUTH),('mobile-secret-path',MOBILE_SECRET)):
        if pat.search(text):findings.add(label)
    if SHADOW.search(text):findings.add('password-hash')
    if any(not placeholder(m[2]) for m in CHPASSWD.finditer(text)):findings.add('chpasswd-secret')
    if any(not placeholder(m[3] if m[2] else m[4]) for m in WIFI_COMMAND.finditer(text)):findings.add('wifi-ssid-command')
    if WIFI_COMMAND.search(text) and any(not placeholder(m[3] if m[2] else m[4]) for m in WIFI_PASSWORD.finditer(text)):findings.add('wifi-password-command')
    if any(not placeholder(m[0].split(':',2)[-1].rsplit('@',1)[0]) for m in URL_AUTH.finditer(text)):findings.add('url-credential')
    return findings


def content_errors(rel, data, denylist=()):
    """The same content rules apply to working files and historical blobs."""
    errors=[]
    if forbidden_artifact(rel):
        errors.append((str(rel),'excluded-artifact'))
    try: t=data.decode('utf-8')
    except UnicodeError:
        return errors+[(str(rel),'non-text')]
    if '\0' in t: errors.append((str(rel),'nul-byte'))
    for label in graphics_findings(rel,t):errors.append((str(rel),label))
    scan=str(rel)+'\n'+t
    if any(v.lower() in scan.lower() for v in denylist):errors.append((str(rel),'private-denylist'))
    # Only two definition lines contain their own home/name patterns.
    # All other checker content is scanned, including credentials/tokens.
    is_checker=str(rel)=='scripts/release/verify.py'
    if is_checker:
        scan='\n'.join(line for line in scan.splitlines() if not line.lstrip().startswith(("'absolute-home': re.compile(", "'owner-username': re.compile(")))
    for label in privacy_findings(scan):errors.append((str(rel),label))
    for pat in (MAC,MAC_DASH):
        for m in pat.finditer(scan):
            if m[0].lower() not in ('00:00:00:00:00:00','00-00-00-00-00-00','ff:ff:ff:ff:ff:ff','aa:aa:aa:aa:aa:aa','43:55:c0:00:1f:ac','43:55:c1:00:76:93'):
                errors.append((str(rel),'mac'));break
    for m in IP.finditer(scan):
        if is_checker and m[0] in ('10.0.0.0','192.168.0.0','169.254.0.0','172.16.0.0'):continue  # subnet definitions
        try: ip=ipaddress.ip_address(m[0])
        except ValueError:continue
        if (ip in ipaddress.ip_network('10.0.0.0/8') or ip in ipaddress.ip_network('192.168.0.0/16') or ip in ipaddress.ip_network('169.254.0.0/16') or (ip in ipaddress.ip_network('172.16.0.0/12') and ip not in ipaddress.ip_network('172.16.42.0/24'))):
            errors.append((str(rel),'lan-ip'));break
    return errors


def audit(root, denylist=()):
    errors=[]; n=0; size=0
    for p in sorted(root.rglob('*')):
        rel=p.relative_to(root)
        if '.git' in rel.parts:
            continue
        if p.is_symlink():
            errors.append((str(rel),'symlink')); continue
        if not p.is_file():
            continue
        n+=1; data=p.read_bytes(); size+=len(data)
        errors.extend(content_errors(rel,data,denylist))
        try:t=data.decode('utf-8')
        except UnicodeError:continue
        if p.suffix=='.md':
            for target in re.findall(r'\[[^\]\n]*\]\(([^)\n]+)\)',t):
                target=target.split('#',1)[0].strip('<>')
                if not target or re.match(r'[a-z]+:',target):continue
                if not (p.parent/unquote(target)).exists(): errors.append((str(rel),'broken-link:'+target))
    return {'files':n,'bytes':size,'errors':[{'file':f,'rule':r} for f,r in sorted(set(errors))]}


def audit_git(root, denylist=(), existing_repo=False):
    """No remote is contacted. Existing repositories retain all content gates."""
    errors=[]
    def add(rule, path='.git'):errors.append({'file':path,'rule':rule})
    def git(*a, raw=False):
        data=subprocess.check_output(['git','--no-replace-objects','-C',str(root),*a])
        return data if raw else data.decode().strip()
    if not (root/'.git').exists():
        if existing_repo:add('existing-repo-required')
        return errors
    if git('remote') and not existing_repo:add('remote-configured','.git/config')
    if git('rev-parse','--is-shallow-repository')=='true':add('shallow-history')
    if Path(git('rev-parse','--path-format=absolute','--git-path','info/grafts')).exists():add('grafted-history')
    head=subprocess.run(['git','-C',str(root),'rev-parse','--verify','HEAD'],capture_output=True)
    if head.returncode:
        if existing_repo:add('existing-head-required')
        return errors
    if not existing_repo and git('rev-list','--count','--all','HEAD')!='1':add('not-single-initial-commit')
    for value in git('log','--all','HEAD','--format=%ae%n%ce').splitlines():
        if value.lower()!=PUBLIC_EMAIL:add('non-public-git-identity')
    if any(value!=PUBLIC_NAME for value in git('log','--all','HEAD','--format=%an%n%cn').splitlines()):add('wrong-public-git-name')
    if git('status','--porcelain','--untracked-files=all'):add('dirty-tree')
    if existing_repo:
        seen=set()
        # Tree paths, modes and blobs are checked even after later deletion.
        with subprocess.Popen(['git','--no-replace-objects','-C',str(root),'cat-file','--batch'],
                              stdin=subprocess.PIPE,stdout=subprocess.PIPE) as blobs:
            try:
                for tree in sorted(set(git('log','--all','HEAD','--format=%T').splitlines())):
                    for row in git('ls-tree','-rz',tree,raw=True).split(b'\0'):
                        if not row:continue
                        meta,path=row.split(b'\t',1);mode,kind,oid=meta.decode().split()
                        rel=path.decode();entry=(rel,mode,oid)
                        if entry in seen:continue
                        seen.add(entry)
                        if mode=='120000' or kind!='blob':
                            add('history-non-regular-file','history:'+rel);continue
                        blobs.stdin.write((oid+'\n').encode());blobs.stdin.flush()
                        header=blobs.stdout.readline().split();data=blobs.stdout.read(int(header[2]))
                        if blobs.stdout.read(1)!=b'\n':raise RuntimeError('invalid blob stream')
                        errors.extend({'file':'history:'+f,'rule':r} for f,r in content_errors(rel,data,denylist))
            finally:
                blobs.stdin.close()
    return [dict(file=f,rule=r) for f,r in sorted({(e['file'],e['rule']) for e in errors})]


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('snapshot',type=Path);ap.add_argument('--private-source',type=Path)
    ap.add_argument('--existing-repo',action='store_true',help='accept remotes/multiple commits; audit reachable history too (offline)')
    args=ap.parse_args()
    root=args.snapshot.resolve();deny=()
    if not root.is_dir():ap.error('snapshot directory does not exist')
    if args.private_source:
        import importlib.util
        spec=importlib.util.spec_from_file_location('private_scrub',args.private_source/'scripts/release/scrub.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);deny=mod.private_values(args.private_source.resolve())
    result=audit(root,deny)
    result['errors'].extend(audit_git(root,deny,args.existing_repo))
    print(json.dumps(result,indent=2));return bool(result['errors'])

if __name__=='__main__':sys.exit(main())
