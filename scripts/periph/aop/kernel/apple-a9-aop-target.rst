.. SPDX-License-Identifier: GPL-2.0-only

J71t AOP directed AFK buffer capture
==================================

Use the exact buffer PA and size read from AppleSPU's AFK 0x89 allocation
table in the last iOS session. The ring can start at an offset inside that
buffer. This default-off diagnostic does not reserve or preserve the source.
Bootloader and early kernel allocations may already have overwritten it.

Parameters and timing
---------------------

Set these tokens on the FIRST Linux boot after iOS -> DFU -> Linux, with no
intervening Linux boot, iOS reboot or AON power cycle. Substitute the actual
PA and size; angle brackets below are placeholders, not literal arguments::

  apple_a9_aop.debug=1 apple_a9_aop.ring_pa=<PA_FROM_IOS>
  apple_a9_aop.ring_pa_size=<SIZE_FROM_IOS> apple_a9_aop.ring_scan=0
  apple_a9_aop.active=0 apple_a9_aop.attach=0 apple_a9_aop.ring_iio=0

``ring_pa`` and ``ring_pa_size`` accept matching comma-separated lists of
one to four entries, in hex or decimal. Each size must be 1..65536 bytes;
zero PAs, overflowing ranges and unequal list lengths are refused. Use one
list per parameter; repeated occurrences replace rather than append a list.
Unaligned PAs and partial final pages are supported without reading outside
the supplied byte range.

With debug=1 the lists automatically trigger postcore_initcall_sync capture
on J71t. Neither ring_scan=1 nor ring_scan_early=1 is needed. The capture
precedes the optional broad scan and its allocations: all first snapshots,
one shared 1000 ms sleep, then all second snapshots. Each range reports its
actual start-to-start interval. Total source reads are bounded to 512 KiB.
Capture allocates no buffers: approximately 0.5 MiB of kernel BSS holds the
two snapshots of four ranges. Pages containing this storage and the current
kernel stack are excluded. Broad E1 also excludes the snapshot storage so
saved signatures cannot become false hits. Boot still follows allocator
initialization; historical allocation is unknown.

Either PA parameter excludes the active driver and observer wrapper accesses
even if the lists are malformed. Reading debugfs never retries a failed or
unavailable early capture. Omitting both lists leaves this mode disabled.

Read policy
-----------

All covering pages must be full memblock RAM, System RAM in the resource
tree, pfn_valid, pfn_is_map_memory and virt_addr_valid through phys_to_virt.
Reserved mapped RAM is admitted here without ring_scan_reserved=1. No-map
pages, MMIO, non-System RAM and invalid mappings are refused before source
bytes are read. The AOP firmware window at 0x210e00000 is not admitted.
Metadata for all covering pages is retained even when one page is refused.

Each exact byte range is copied in page-confined copy_from_kernel_nofault
chunks, rechecking eligibility before each chunk. A fault quarantines that
page across all ranges and both snapshots; other ranges continue. Partial
failed data is never displayed or compared. No mapping, source write, page
pin, reservation, cache maintenance, mailbox access or rptr acknowledgment
occurs. A read barrier does not establish inherited DMA cache coherency.

Frozen debugfs output
---------------------

Read the root-only file after boot::

  sudo cat /sys/kernel/debug/apple-a9-aop/ring_pa

Require phase=postcore-initcall, captured=1 and snapshot_end=1. Each requested
range reports its PA, size and errno. Each snapshot reports page counts,
System RAM/reserved/no-map membership (yes/no/mixed), linear-map coverage,
timestamp, refusal reason and fault PA. Reservation labels are current
metadata, not proof that Linux never wrote or owned the source.
Successful ranges containing unreserved RAM produce a critical kernel log
about possible inherited AOP DMA into Linux allocator pages, including free
pages. Nothing claims or repairs them on this boot.

Successful snapshots dump the first min(size,0x100) bytes and every non-zero
64-byte line, including a shorter final line. Zero lines outside the prefix
are omitted. These offsets are relative to the supplied PA. Signature checks
use the exact a9-aop-ring.h header at +0 and every relative 0x40-aligned offset
with 0x84 header bytes available. Matches report absolute PA, offset,
bufsize=0x3f40, marker=0x00070006, aligned bounded rptr/wptr and whether the
whole 16 KiB ring fits inside the supplied range. This does not parse samples.

The comparison reports changed=yes/no/unavailable, byte and 64-byte-line
change counts, and elapsed milliseconds. Changes establish only that the two
CPU-visible snapshots differ: writer=unknown, because Linux or another agent
could also write the bytes. Static data does not prove the AOP stopped. The
two snapshots are not atomic relative to DMA. Repeated debugfs reads only
format the saved bytes and must match exactly.
