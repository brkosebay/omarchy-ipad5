#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Re-export rounds 5/6/7 with GPL provenance at the table's introduction.

Run on the host, or --inside via build-kernel.sh. Writes no commits/refs.
Temporary Git indexes replace the table at every historical tree, so the
exports never add the kext-attributed header and later delete it. Round 7
reproduces a9/vdec-gpl exactly; earlier rounds use the same GPL table/tools.
Mail patches use a zero From hash because their content has been refreshed.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

D = "drivers/media/platform/apple/vxd/"
FILES = [D + name for name in ("apple-vxd-h264-tables.h", "h264-tables.py", "upstream/h264_vlc.h")]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inside", action="store_true")
    args = ap.parse_args()
    if not args.inside:
        root = Path(__file__).resolve().parents[2]
        subprocess.run([str(root / "scripts/build-kernel.sh"), "--run", "vdec-gpl",
                        "python3 -B /work/scripts/vdec/export-gpl.py --inside"], check=True)
        return
    root = Path("/work")

    def git(*args, env=None, data=None):
        return subprocess.check_output(["git", *args], env=env, input=data)

    assert git("branch", "--show-current").strip() == b"a9/vdec-gpl"
    assert not git("status", "--porcelain")
    baseline = git("rev-parse", "a9/vdec").decode().strip()
    assert baseline == "9a897a8fd21c456231c51559f4d898aec388b58f"
    commits = git("rev-list", "--reverse", "hoolock.." + baseline).decode().splitlines()
    assert len(commits) == 21
    names = subprocess.check_output(["git", "-C", str(root), "show",
                                     "HEAD:patches/vdec/round7/series"]).decode().splitlines()
    assert len(names) == 21
    tip = git("rev-parse", "HEAD").decode().strip()
    blobs = {p: git("rev-parse", f"{tip}:{p}").decode().strip() for p in FILES}
    with tempfile.TemporaryDirectory(prefix="vdec-gpl-export-") as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=tmp + "/index")

        def tree(ref):
            git("read-tree", ref, env=env)
            if git("ls-tree", ref, FILES[0]):
                for path, blob in blobs.items():
                    git("update-index", "--add", "--cacheinfo", f"100644,{blob},{path}", env=env)
            return git("write-tree", env=env).decode().strip()

        previous = tree("hoolock")
        patches = []
        for n, commit in enumerate(commits, 1):
            current = tree(commit)
            # Preserve original author/date/message; the diff is regenerated.
            mail = git("format-patch", "-1", "--stdout", "--zero-commit", "--no-signature", "--no-stat", commit)
            header = mail.split(b"\ndiff --git ", 1)[0]
            if header.endswith(b"\n---\n"):
                header = header[:-5]
            if n == 13:
                header = header.replace(
                    b"0xc/0x10, intra buffer 0x18, ...).  The CAVLC table is the 544-entry\nblob from the kext: psb_video's 520-entry ui16H264VLCTableData plus the\n24 4:2:2 chroma-DC entries.",
                    b"0xc/0x10, intra buffer 0x18, ...).\n\n"
                    b"Generate the 544-entry CAVLC table from Imagination/TI's GPL-2.0\n"
                    b"h264_vlc_table_data, including the 4:2:2 chroma-DC entries. Its first\n"
                    b"520 entries also match MIT psb_video's ui16H264VLCTableData.")
                header += (b"\n\nProvenance refresh: generate the unchanged 544-entry CAVLC table from\n"
                           b"Imagination/TI GPL-2.0 source; include the pinned source and generator.\n")
            header = re.sub(rb"Subject: \[PATCH\]", f"Subject: [PATCH {n:02d}/21]".encode(), header)
            stat = git("diff", "--stat", previous, current)
            diff = git("diff", "--binary", "--full-index", previous, current)
            patches.append((names[n - 1], header.rstrip() + b"\n---\n" + stat + b"\n" + diff))
            previous = current
        assert previous == git("rev-parse", "HEAD^{tree}").decode().strip()
        for round_name, count in (("round5", 14), ("round6", 20), ("round7", 21)):
            out = root / "patches/vdec" / round_name
            for p in out.glob("*.patch"):
                p.unlink()
            names = []
            sums = []
            git("read-tree", "hoolock", env=env)
            for name, content in patches[:count]:
                content = re.sub(rb"\[PATCH (\d+)/21\]",
                                 lambda match: b"[PATCH " + match[1] + f"/{count}]".encode(), content)
                (out / name).write_bytes(content)
                git("apply", "--cached", "--check", "-", env=env, data=content)
                git("apply", "--cached", "-", env=env, data=content)
                names.append(name)
                sums.append(f"{hashlib.sha256(content).hexdigest()}  ./{name}")
            actual = git("write-tree", env=env).decode().strip()
            expected = tree(commits[count - 1])
            assert actual == expected, round_name
            (out / "series").write_text("\n".join(names) + "\n")
            (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
            print(f"{round_name}: {count} patches, application/tree equality PASS ({actual})")


if __name__ == "__main__":
    main()
