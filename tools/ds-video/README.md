# tools/ds-video — reading the A9 scaler/JPEG code out of the kernelcache

> Historical design/test notes. Current support: [status](../../docs/release/STATUS.md); examples may need local firmware/captures.


Helper scripts for [`docs/periph/ds-scaler-jpeg.md`](../../docs/periph/ds-scaler-jpeg.md).
They exist because the two kexts of interest (`AppleM2ScalerCSCDriver`,
`AppleJPEGDriver`) have **no symbols** in the kernelcache and Ghidra does not
recover all of their C++ virtual methods, so the addresses and register
offsets in that document have to be re-derived from the bytes.

> Private graphics-dependent excerpt omitted from this source snapshot.

Everything works on the saved artifacts only: the shared kernelcache and
`build/periph/kexts/prelink.json`. No device, no Ghidra required (Ghidra is
used for decompilation when it is up, but none of these tools need it).

```text
kernelcache  build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12
sha256       c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a
```

## kc-kexts.py — which kext owns an address, and read it

The j71t kernelcache is a plain `MH_EXECUTE` whose kext Mach-O headers sit
inside `__PRELINK_TEXT` and whose kext code sits in `__PLK_TEXT_EXEC`, which
is why `ipsw kernel extract` cannot split it. Each kext header still carries
its own segment table, so the addresses are recoverable.

```bash
tools/ds-video/kc-kexts.py list --filter JPEG      # bundles + code ranges
tools/ds-video/kc-kexts.py which 0xfffffff005fc5a54
tools/ds-video/kc-kexts.py segs AppleJPEGDriver
tools/ds-video/kc-kexts.py dis  0xfffffff005fd1924 --count 40
tools/ds-video/kc-kexts.py str  0xfffffff0054c9fb5
tools/ds-video/kc-kexts.py strings AppleJPEGDriver --filter subsampl
tools/ds-video/kc-kexts.py dump 0xfffffff007a13170 --length 0x41a0 --out /tmp/t.bin
```

`dis` runs `llvm-objdump -D` on the kernelcache in place (`-D`, not `-d`: the
kext text is a bare `__text` section, so `-d` prints nothing).

`vm → file offset` is computed from the **top-level** segment table only. A
kext header's own segment fileoffs are not usable: the prelinker packs kext
segments with different padding than the vm layout (measured: off by `0x5000`
for the JPEG `__DATA_CONST`), while the top-level table always matches
Ghidra's `read_memory`.

## catalog.py — string → method map

Apple compiles `__PRETTY_FUNCTION__`-style signature strings into every kext
and each is referenced from the method it belongs to, so walking
string → xref recovers a method map for a kext with no symbol table. This
talks to the shared Ghidra MCP server (`scripts/ghidra.py`'s server).

```bash
tools/ds-video/catalog.py scaler                    # all strings, with sites
tools/ds-video/catalog.py jpeg --mode sig           # signatures only
tools/ds-video/catalog.py jpeg --mode msg --filter Decode
tools/ds-video/catalog.py scaler --table --json /tmp/scaler.json
```

Output is markdown so it can be pasted into a document. Rows where the site
column shows `-` are strings Ghidra found no reference for (assertion tables,
or code Ghidra did not turn into a function).

## xrefs.py — find the code that references a literal

Ghidra's xref database is incomplete for these kexts, so this decodes the
kext's `__TEXT_EXEC` directly and resolves the two forms Apple's compiler
uses for every literal reference (`adrp` page + `add #imm12`).

```bash
tools/ds-video/xrefs.py 0xfffffff0054c9983          # one address
tools/ds-video/xrefs.py 0xfffffff0054c9fb5 0xfffffff0054c9a50
```

Only chained `adrp`+`add` pairs count as exact hits; a bare page reference is
not reported, so a hit means "this instruction materialises that exact
pointer".

## regprog.py — recover a register program

HAL methods are mostly straight-line `mov w8,#imm` / `str w8,[x19,#off]`
sequences, so an ordered `(offset, value)` list can be lifted from the
disassembly. This is a reading aid, not a decompiler.

```bash
tools/ds-video/regprog.py --start 0xfffffff005fce830 --end 0xfffffff005fce998 --only-str
tools/ds-video/regprog.py --start 0xfffffff005fd1924 --end 0xfffffff005fd23a4 --md
```

Values that arrive via `ldr`/`movk` chains print as `<reg>`; helper calls are
shown so the shape of the sequence stays visible. **Confirm every pair
against the surrounding code before quoting it as a register offset** — the
base register is not identified, so a `str` to an object field looks the same
as a `str` to MMIO.

## Generated artifacts

The shipped `vxd_h264_tables.h` is generated offline from the pinned
Imagination/TI GPL-2.0 table, with its copyright notices. It no longer needs
a kernelcache and contains only the VLC table (the unused historical HEVC
helpers were removed). `vlc_tables.py --header` uses the same public-source
generator; its extraction/default/register analysis modes still use local
Apple input. See [the provenance audit](../../docs/release/vdec-gpl-provenance.md).

```sh
python3 -B scripts/vdec/fetch-table-sources.py
python3 -B scripts/vdec/h264-tables.py --workspace --write tools/ds-video/vxd_h264_tables.h
python3 -B scripts/vdec/h264-tables.py --compare tools/ds-video/vxd_h264_tables.h --psb scripts/vdec/upstream/pnw_H264.c
```

`kc-kexts.py strings` and `regprog.py` outputs used while writing the study
are kept under `build/ds-video/` (gitignored):

| file | what |
| --- | --- |
| `scaler-strings.txt` | every printable run in the scaler kext's `__TEXT`, with addresses |
| `jpeg-strings.txt` | same for the JPEG kext |
| `jpeg-sig.json`, `jpeg-startdec-base.c` | method map and the decompiled base-class decoder |
| `jpeg-dec-regs.txt` | the decoder register program from `regprog.py` |
| `scaler-props-table.bin` | the 30-entry `{soc, hwVersion, extents}` table at `0xfffffff007a13170` |
