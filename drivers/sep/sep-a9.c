// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple A9 (s8000/s8003) Secure Enclave mailbox driver.
 *
 * The SEP is an ASC coprocessor: registers at 0x20DA00000, a 32-bit mailbox
 * at +0x4000 and four GIC interrupts (ADT "interrupts" = 99 98 101 100).
 * The AP and the SEP exchange 64-bit little-endian messages:
 *
 *	bits  0..7	destination endpoint
 *	bits  8..15	tag
 *	bits 16..23	opcode
 *	bits 24..31	parameter
 *	bits 32..63	data
 *
 * sepOS announces its services on endpoint 0xfd: op 0 carries the endpoint's
 * 4CC, op 1 its OOL (out-of-line buffer) page-count ranges. The driver only
 * listens; it never asks for, guesses or probes an endpoint. A service is
 * usable once both halves of its advertisement are known, either from the
 * mailbox or declared by hand from a pongoOS capture.
 *
 * A usable endpoint gets a send and a receive OOL buffer, published with four
 * control messages on endpoint 0 ('cntl'). Commands then use
 * AppleSEPGenericTransfer: each OOL buffer holds a seven-u32 little-endian
 * header (version, total, offset, flags, reserved, command, chunk length) and
 * the payload at +0x1c, and a message is carried in as many chunks as the
 * endpoint's own buffer needs. Tags 0xfc/0xfd/0xfe/0xff are the chunk, credit
 * and error tags; exactly one such transaction may exist mailbox-wide, and it
 * has one absolute deadline. See docs/sep/workstreams/sbio-runtime-re.md.
 *
 * Words that arrive with a tag outside 0xfc..0xff on the selected async
 * endpoint ('echo EP > sbio') are recorded read-only in a fixed-size ring and
 * never answered: such a word carries a 16-bit value/length in bits 16..31 and
 * a 32-bit info word in bits 32..63, and no payload is read from anywhere.
 *
 * Nothing is ever sent automatically: arming the mailbox, publishing buffers
 * and every command are explicit debugfs writes. debugfs is the only interface
 * that sends: 'xfer' is one transfer, 'notify' is the passive capture.
 *
 * Arming activates the AppleA7IOPV2 wrapper the way AppleA7IOP::start does
 * (docs/sep/xart-handshake.md section 6), but only while the SEP PMGR word is
 * what pongoOS handed over (0x4f0000ff; AUTO_ENABLE is only warned about):
 * the driver never writes PMGR, CPU RUN, NMI or reset. Every AP->SEP message
 * is one 64-bit store to +0x4010, as in AppleA7IOPV2::postMailbox. Probe and
 * arm log the PMGR word and SEP +0x28/+0x44/+0xb88 read-only, for comparison
 * with pongoOS peeks. debugfs wake-test runs XNU's endpoint-0 nap wake once
 * (section 7).
 *
 * SEP->AP messages are popped with the matching single 64-bit load from
 * +0x4038, but only after recv_sts bit 17 says that the FIFO is nonempty.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/completion.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include <asm/cacheflush.h>

#include <uapi/linux/apple-sep.h>

#define SEP_MBOX_OFFSET		0x4000
#define SEP_MBOX_SIZE		0x1000

/*
 * Mailbox register layout (pongoOS struct mailbox_registers32), except that
 * the AP->SEP message is sent the way AppleA7IOPV2::postMailbox
 * (0xfffffff005b4e4a8) does: one 64-bit store to +0x4010, not pongoOS's two
 * 32-bit stores to +0x4010/+0x4014. AppleA7IOPV2's receive primitive
 * (0xfffffff005b4e498) uses one 64-bit load from +0x4038.
 */
#define SEP_MBOX_DIS_INT	0x00
#define SEP_MBOX_EN_INT		0x04
#define SEP_MBOX_SND_STS	0x08
#define SEP_MBOX_SND		0x10	/* 64-bit, written with writeq() only */
#define SEP_MBOX_RECV_STS	0x20
#define SEP_MBOX_RECV		0x38	/* 64-bit, read with readq() only */

/*
 * Status bits, as in every Apple mailbox variant: bit 16 full, bit 17 empty.
 * recv_sts bit 17 clear => a message is waiting (pongoOS sep_check_mailbox);
 * snd_sts bit 16 set => the AP->SEP FIFO has no room.
 */
#define SEP_MBOX_STS_FULL	0x10000
#define SEP_MBOX_STS_EMPTY	0x20000
/* pongoOS enables exactly this interrupt bit after probe */
#define SEP_MBOX_INT_ENABLE	0x1000
#define SEP_TX_TIMEOUT_US	100000
/* transport proof: queued AP->SEP words must be consumed within 1 s */
#define SEP_DRAIN_TIMEOUT_US	1000000

/*
 * AppleA7IOPV2 wrapper, offsets from the SEP base (not the mailbox). The
 * interrupt mask has a set/clear pair in two views, primary +0x4000/+0x4004
 * (= DIS_INT/EN_INT above) and mirror +0xc00/+0xc04; reads of either register
 * of a pair return the mask. +0x4008 and +0x4020 are snd_sts and recv_sts.
 */
#define SEP_WRAP_MASK_SET	0x4000
#define SEP_WRAP_MASK_CLR	0x4004
#define SEP_WRAP_MIRROR_SET	0x0c00
#define SEP_WRAP_MIRROR_CLR	0x0c04
#define SEP_WRAP_INBOX_CTL	0x4008
#define SEP_WRAP_OUTBOX_CTL	0x4020
#define SEP_WRAP_MASK_ALL	0x1111
#define SEP_WRAP_INBOX_IRQ	0x0001	/* bit 0: AP->SEP (inbox) notification */
#define SEP_WRAP_OUTBOX_IRQ	0x1000	/* bit 12: SEP->AP (outbox) notification */
#define SEP_WRAP_OUTBOX_EN	0x0001	/* AppleA7IOPV2_setOutboxEnable */
#define SEP_WRAP_NREGS		6

/* PMGR power-state word, as in drivers/pmdomain/apple/pmgr-pwrstate.c */
#define SEP_PS_RESET		BIT(31)
#define SEP_PS_AUTO_ENABLE	BIT(28)
#define SEP_PS_ACTUAL		GENMASK(7, 4)
#define SEP_PS_TARGET		GENMASK(3, 0)
#define SEP_PS_ACTIVE		0xf
/*
 * The word pongoOS leaves after booting sepOS ("PMGR sep inherited
 * reg=4f0000ff", 15:42). ps_sep carries apple,no-auto-pm so Linux keeps it.
 * AUTO_ENABLE (bit 28) is no longer suspected: at 16:18 a word stayed queued
 * under pongoOS with the word at 0x4f0000ff. Arm and wake-test therefore only
 * warn about bit 28, and refuse any other difference from this value.
 */
#define SEP_PS_HANDOFF		0x4f0000ff

/*
 * SEP block registers snapshotted read-only at probe and arm, in this order:
 * +0x28, +0x44 (CPU control, RUN), +0xb88, then snd_sts and recv_sts.
 */
#define SEP_CORE_NREGS		5

/* Endpoint-0 nap wake (docs/sep/xart-handshake.md section 7) */
#define SEP_OP_NAP_WAKE		0x0e	/* AP: raw 0x00000000000e0000, twice */
#define SEP_OP_NAP_AWAKE	0x0f	/* SEP: wake indication */
#define SEP_OP_OS_ACTIVE	0x15	/* SEP: wake indication, also OS active */
#define SEP_WAKE_WAIT_US	1000000

#define SEP_LOG_ENTRIES		256
/* Passive capture of unsolicited (non-transport) SBIO words; read-only. */
#define SEP_NOTIFY_ENTRIES	32

#define SEP_MSG_EP(ep)		((u64)(ep) & 0xff)
#define SEP_MSG_TAG(tag)	(((u64)(tag) & 0xff) << 8)
#define SEP_MSG_OP(op)		(((u64)(op) & 0xff) << 16)
#define SEP_MSG_PARAM(param)	(((u64)(param) & 0xff) << 24)
#define SEP_MSG_DATA(data)	(((u64)(data)) << 32)
#define SEP_MSG(ep, tag, op, param, data) \
	(SEP_MSG_EP(ep) | SEP_MSG_TAG(tag) | SEP_MSG_OP(op) | \
	 SEP_MSG_PARAM(param) | SEP_MSG_DATA(data))

#define SEP_MSG_GET_EP(msg)	((u8)((msg) & 0xff))
#define SEP_MSG_GET_TAG(msg)	((u8)(((msg) >> 8) & 0xff))
#define SEP_MSG_GET_OP(msg)	((u8)(((msg) >> 16) & 0xff))
#define SEP_MSG_GET_PARAM(msg)	((u8)(((msg) >> 24) & 0xff))
#define SEP_MSG_GET_DATA(msg)	((u32)((msg) >> 32))

/* The whole 8-bit endpoint namespace; 0xfd..0xff are transport endpoints. */
#define SEP_EP_COUNT		256
#define SEP_EP_DISCOVERY	0xfd
#define SEP_EP_CLIENT_MAX	0xfc

/* Endpoint 0xfd (AppleSEPDiscovery): param = endpoint ID. */
#define SEP_DISC_NAME		0x00	/* data = 4CC, 0x636e746c = 'cntl' */
#define SEP_DISC_OOL		0x01	/* data bytes LSB first: in min/max, out min/max */

/* OOL ranges are counted in 4 KiB SEP pages. "in" is AP->SEP (send). */
#define SEP_OOL_PAGE		0x1000

/* Control-endpoint opcodes that publish an endpoint OOL buffer. */
#define SEP_OP_SET_SEND_ADDR	0x02
#define SEP_OP_SET_RECV_ADDR	0x03
#define SEP_OP_SET_SEND_SIZE	0x04
#define SEP_OP_SET_RECV_SIZE	0x05

/*
 * ---------------------------------------------------------------------------
 * AppleSEPGenericTransfer: the host-testable core.
 *
 * Sources: docs/sep/workstreams/sbio-runtime-re.md §3 (frame), §4 (state
 * machine, validation codes), §4.5 (the transfer contract), §9 (non-GT tags).
 * Everything between the two markers below is plain C: no MMIO, no locks, no
 * allocation, no sleeping and no kernel helpers (the little-endian accessors
 * are local for that reason). tools/sep/test_gt_core.py lifts exactly this block
 * out of this file and compiles it on the host, so the offline tests drive the
 * code that ships instead of a model of it. Keep it free of kernel APIs.
 *
 * Besides the state machine the block holds two things the driver glue must not
 * re-implement: sep_gt_is_protocol_error(), the single classification that
 * decides what has to be quarantined, and sep_xfer_parse(), the strict parser
 * for the debugfs 'xfer' line.
 * ---------------------------------------------------------------------------
 */
/* >>> SEP_GT_CORE_BEGIN (host-testable: tools/sep/test_gt_core.py) */

/* Wire tags. 0xfc..0xff are the transport tags; everything else is a service
 * notification (tag = "request" in the kext log, §3.4, §9).
 */
#define SEP_GT_TAG_FIRST	0xfc	/* AP: opener.  SEP: first reply chunk */
#define SEP_GT_TAG_CONT		0xfd	/* continuation chunk, both directions */
#define SEP_GT_TAG_CREDIT	0xfe	/* SEP: send the next chunk.  seq 0 */
#define SEP_GT_TAG_ERR		0xff	/* SEP: error, status word in recv+0x10 */

/* The seven-u32 little-endian header every OOL buffer starts with. */
#define SEP_GT_VERSION		1
#define SEP_GT_HDR_LEN		0x1c
#define SEP_GT_VERSION_OFF	0x00	/* 1; anything else is rejected */
#define SEP_GT_TOTAL_OFF	0x04	/* payload bytes of the whole message */
#define SEP_GT_OFFSET_OFF	0x08	/* this chunk's offset in the message */
#define SEP_GT_FLAGS_OFF	0x0c	/* request 0; reply allocation selector */
#define SEP_GT_RESERVED_OFF	0x10	/* request 0; tag 0xff reply: status */
#define SEP_GT_COMMAND_OFF	0x14	/* SEP opcode, echoed in the reply */
#define SEP_GT_CHUNK_OFF	0x18	/* payload bytes in this buffer */
#define SEP_GT_PAYLOAD_OFF	SEP_GT_HDR_LEN

/* A reply with either allocation bit set cannot be honoured: both allocators
 * are stubs returning 0xe00002c7 in this build (§3.2).
 */
#define SEP_GT_FLAGS_ALLOC	0x6

/*
 * Bounds. 19,668 request bytes and 16,379 reply bytes are proven on the wire
 * (§3.3, §4.4); the request cap XNU uses is unknown, so 64 KiB is a
 * self-imposed ceiling (§12.4). Chunks are bounded in both directions, and one
 * absolute deadline covers a whole transaction: XNU restarts its 10 s clock at
 * every wait, which is exactly what we do not copy (§4.5).
 */
#define SEP_GT_MAX_TOTAL	0x10000
#define SEP_GT_MAX_CHUNKS	8
#define SEP_GT_TIMEOUT_MS	10000
#define SEP_GT_QUIET_MS		100	/* quarantine after a timeout/-EPROTO */

/* gt+0x30 in XNU: 0 idle, 2 AP owns the wire, 1 a reply is arriving. */
enum sep_gt_state {
	SEP_GT_IDLE = 0,
	SEP_GT_RECV = 1,
	SEP_GT_SEND = 2,
};

/* Who owns one received word. Pure, so the routing split of §6/§9 is testable:
 * a non-GT tag can never satisfy a transaction, and an ARTS reply (tag 1 on
 * the 'arts' endpoint) can never be filed as SBIO async traffic.
 */
enum sep_gt_rx_kind {
	SEP_GT_RX_OTHER = 0,	/* not ours at all */
	SEP_GT_RX_CHUNK,	/* 0xfc/0xfd for the active transaction */
	SEP_GT_RX_CREDIT,	/* 0xfe for the active transaction */
	SEP_GT_RX_ERROR,	/* 0xff for the active transaction */
	SEP_GT_RX_STALE,	/* a GT tag with no transaction to satisfy */
	SEP_GT_RX_NOTIFY,	/* non-GT tag on the selected async endpoint */
};

/* The header, unpacked. */
struct sep_gt_hdr {
	u32 version;
	u32 total;
	u32 offset;
	u32 flags;
	u32 reserved;
	u32 command;
	u32 chunk;
};

/* One GT word as it went over the wire, kept for the debugfs readback. */
struct sep_gt_word {
	u16 seq;
	u32 offset;
	u32 len;
	u8 tag;
};

/*
 * One transaction. Lives in struct sep_a9 (there is one mailbox), so detaching
 * it never frees anything and RX can be written without a lifetime rule.
 */
struct sep_gt_xact {
	const u8 *in;		/* request payload, caller's, read only */
	u8 *out;		/* reply payload, caller's */
	size_t out_cap;
	u32 in_total;
	u32 in_done;
	u32 out_total;
	u32 out_done;
	u32 cmd;
	u32 status;		/* tag 0xff status, raw and uninterpreted */
	u16 seq;		/* sequence of the next request chunk */
	u16 recv_seq;		/* last accepted reply sequence */
	u8 state;
	bool credit;		/* a 0xfe credit waits to be used */
	bool want_more;		/* reply incomplete: a 0xfe must go out */
	bool done;
	int err;		/* local protocol/size error, sticky */
	u32 n_sent;
	u32 n_recv;
	u32 credits;		/* 0xfe words accepted */
	u32 ignored;		/* GT words dropped without any effect */
	u32 violations;		/* rejected reply headers */
	struct sep_gt_hdr last;	/* last header seen, either direction */
	u32 last_msg_data;	/* bits 32..63 of the last GT word */
	u8 last_tag;
	u16 last_seq;
	struct sep_gt_word sent[SEP_GT_MAX_CHUNKS];
	struct sep_gt_word recv[SEP_GT_MAX_CHUNKS];
};

static u32 sep_gt_le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 |
	       (u32)p[3] << 24;
}

static void sep_gt_le32_put(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void sep_gt_hdr_pack(u8 *buf, const struct sep_gt_hdr *h)
{
	sep_gt_le32_put(buf + SEP_GT_VERSION_OFF, h->version);
	sep_gt_le32_put(buf + SEP_GT_TOTAL_OFF, h->total);
	sep_gt_le32_put(buf + SEP_GT_OFFSET_OFF, h->offset);
	sep_gt_le32_put(buf + SEP_GT_FLAGS_OFF, h->flags);
	sep_gt_le32_put(buf + SEP_GT_RESERVED_OFF, h->reserved);
	sep_gt_le32_put(buf + SEP_GT_COMMAND_OFF, h->command);
	sep_gt_le32_put(buf + SEP_GT_CHUNK_OFF, h->chunk);
}

static void sep_gt_hdr_unpack(const u8 *buf, struct sep_gt_hdr *h)
{
	h->version = sep_gt_le32(buf + SEP_GT_VERSION_OFF);
	h->total = sep_gt_le32(buf + SEP_GT_TOTAL_OFF);
	h->offset = sep_gt_le32(buf + SEP_GT_OFFSET_OFF);
	h->flags = sep_gt_le32(buf + SEP_GT_FLAGS_OFF);
	h->reserved = sep_gt_le32(buf + SEP_GT_RESERVED_OFF);
	h->command = sep_gt_le32(buf + SEP_GT_COMMAND_OFF);
	h->chunk = sep_gt_le32(buf + SEP_GT_CHUNK_OFF);
}

/*
 * The mailbox word of a GT chunk: endpoint in bits 0..7, tag in 8..15, the
 * whole 32-bit command in 16..47 and the chunk sequence in 48..63 (§3.1). The
 * endpoint byte is supplied by the provider in XNU, so a real endpoint there is
 * XNU-compatible; §12.2 retires the gt_ep=0 idea.
 */
static u64 sep_gt_word(u8 ep, u8 tag, u32 cmd, u16 seq)
{
	return (u64)ep | (u64)tag << 8 | (u64)cmd << 16 | (u64)seq << 48;
}

/* The credit word: no payload, command echoed, sequence 0, never counted as a
 * chunk (§4.4).
 */
static u64 sep_gt_credit_word(u8 ep, u32 cmd)
{
	return sep_gt_word(ep, SEP_GT_TAG_CREDIT, cmd, 0);
}

static u8 sep_gt_word_ep(u64 w)
{
	return (u8)w;
}

static u8 sep_gt_word_tag(u64 w)
{
	return (u8)(w >> 8);
}

static u32 sep_gt_word_cmd(u64 w)
{
	return (u32)(w >> 16);
}

static u16 sep_gt_word_seq(u64 w)
{
	return (u16)(w >> 48);
}

/* Payload capacity of a published OOL buffer: its own length minus 0x1c. Not
 * every endpoint is 0x4000 bytes, so this is the only correct source (§3.3).
 */
static u32 sep_gt_cap(size_t ool_len)
{
	if (ool_len <= SEP_GT_HDR_LEN)
		return 0;
	return (u32)(ool_len - SEP_GT_HDR_LEN);
}

/* How much of the request still fits in one chunk. */
static u32 sep_gt_chunk_bytes(u32 total, u32 done, u32 cap)
{
	if (done >= total)
		return 0;
	return total - done < cap ? total - done : cap;
}

/*
 * Which handler owns this word? `async_ep` is the selected unsolicited-tag
 * endpoint (-1 when none is selected).
 */
static enum sep_gt_rx_kind sep_gt_classify(u64 msg, bool active, u8 xact_ep,
					   int async_ep)
{
	u8 ep = sep_gt_word_ep(msg);
	u8 tag = sep_gt_word_tag(msg);

	if (tag != SEP_GT_TAG_FIRST && tag != SEP_GT_TAG_CONT &&
	    tag != SEP_GT_TAG_CREDIT && tag != SEP_GT_TAG_ERR)
		return (int)ep == async_ep ? SEP_GT_RX_NOTIFY : SEP_GT_RX_OTHER;

	if (!active || ep != xact_ep)
		return SEP_GT_RX_STALE;

	if (tag == SEP_GT_TAG_CREDIT)
		return SEP_GT_RX_CREDIT;
	if (tag == SEP_GT_TAG_ERR)
		return SEP_GT_RX_ERROR;
	return SEP_GT_RX_CHUNK;
}

/*
 * May a transfer start at all? Separate from the driver so the quarantine rule
 * (§7, I9) is testable: a refused start is what stops a late word from a
 * timed-out transaction being taken for a reply to the next one.
 */
static int sep_gt_gate(bool dead, bool active, bool fifo_empty,
		       s64 quiet_ms_left)
{
	if (dead)
		return -ENODEV;		/* poisoned for this boot */
	if (active)
		return -EBUSY;		/* one transaction per mailbox */
	if (!fifo_empty)
		return -EBUSY;		/* words still queued: they would race */
	if (quiet_ms_left > 0)
		return -EAGAIN;		/* quarantine window still open */
	return 0;
}

static int sep_gt_xact_init(struct sep_gt_xact *x, u32 cmd, u32 in_total,
			    const void *in, void *out, size_t out_cap)
{
	memset(x, 0, sizeof(*x));
	/*
	 * XNU rejects `(out != NULL) == (outlen == NULL)` with 0xe00002c2 before
	 * it sends anything. Both halves of the pair have to agree: a buffer with
	 * no capacity is as useless as a body with nowhere to go, and a command
	 * whose reply turns out to have bytes may not be started without one.
	 */
	if ((out == NULL) != (out_cap == 0)) {
		x->err = -EINVAL;
		return -EINVAL;
	}
	x->cmd = cmd;
	x->in = in;
	x->in_total = in_total;
	x->out = out;
	x->out_cap = out_cap;
	x->state = SEP_GT_SEND;
	return 0;
}

/*
 * Stage the next request chunk (header + payload) into the endpoint's send OOL
 * buffer and advance the per-transfer state. Returns the number of buffer bytes
 * to clean before the doorbell word, or 0 when nothing is left to send.
 *
 * The sequence is per transfer and counts chunks only: 0 for the 0xfc opener,
 * 1 for the first 0xfd, and the 0xfe credit never consumes a number (§4.4). A
 * zero-length request still sends exactly one 0xfc word with chunk = 0.
 *
 * Being asked for a chunk in a state that cannot send one is the core's own
 * error (`-EPROTO`), so a credit that arrives together with a reply opener
 * fails closed here instead of pushing a 0xfd the SEP is not waiting for. It
 * still returns 0 bytes, so the caller must check `x->err`.
 */
static u32 sep_gt_xact_stage(struct sep_gt_xact *x, u8 *ool, size_t ool_len,
			     u8 *tag, u16 *seq)
{
	struct sep_gt_hdr h;
	u32 cap = sep_gt_cap(ool_len);
	u32 chunk;

	if (x->err)
		return 0;
	if (x->state != SEP_GT_SEND) {
		x->err = -EPROTO;	/* no request chunk belongs on the wire */
		return 0;
	}
	if (x->n_sent && x->in_done >= x->in_total)
		return 0;		/* nothing left; not an error */
	if (x->n_sent >= SEP_GT_MAX_CHUNKS) {
		x->err = -EMSGSIZE;
		return 0;
	}

	chunk = sep_gt_chunk_bytes(x->in_total, x->in_done, cap);
	if (!chunk && x->in_done < x->in_total) {
		x->err = -EMSGSIZE;	/* no room for a payload byte */
		return 0;
	}

	memset(&h, 0, sizeof(h));
	h.version = SEP_GT_VERSION;
	h.total = x->in_total;
	h.offset = x->in_done;
	h.command = x->cmd;
	h.chunk = chunk;
	sep_gt_hdr_pack(ool, &h);
	if (chunk)
		memcpy(ool + SEP_GT_HDR_LEN, x->in + x->in_done, chunk);

	*tag = x->n_sent ? SEP_GT_TAG_CONT : SEP_GT_TAG_FIRST;
	*seq = x->seq++;
	x->sent[x->n_sent].tag = *tag;
	x->sent[x->n_sent].seq = *seq;
	x->sent[x->n_sent].offset = h.offset;
	x->sent[x->n_sent].len = chunk;
	x->n_sent++;
	x->in_done += chunk;
	x->last = h;
	x->last_tag = *tag;
	x->last_seq = *seq;
	x->last_msg_data = (u32)(x->cmd >> 16) | (u32)*seq << 16;
	return SEP_GT_HDR_LEN + chunk;
}

/*
 * Any word after the transaction ended: counted, dropped, no state changed.
 * `status`/`done` are written by the RX side under the driver lock, so this is
 * also the "is it too late to touch x" test below.
 */
static bool sep_gt_xact_closed(const struct sep_gt_xact *x)
{
	if (x->status || x->done)
		return true;
	return false;
}

static int sep_gt_fail(struct sep_gt_xact *x, int err)
{
	x->err = err;
	x->violations++;
	return err;
}

/*
 * A credit only means something while the AP still owns the wire and has bytes
 * left to send. XNU's handleChunk turns 0xfe into buildSendChunk, which only
 * works in state 2; anywhere else it fails 0x102 and aborts the transfer
 * (§4.2). So a credit in RECV, a credit after the request is complete, a
 * credit after the transaction ended, and a second credit that arrives before
 * the waiter consumed the first are all protocol errors here, not something to
 * count and drop: the SEP is out of step and the next word cannot be trusted.
 */
static int sep_gt_xact_rx_credit(struct sep_gt_xact *x)
{
	if (x->err)
		return x->err;
	if (sep_gt_xact_closed(x) || x->state != SEP_GT_SEND ||
	    x->in_done >= x->in_total || x->credit)
		return sep_gt_fail(x, -EPROTO);

	x->credits++;
	x->credit = true;
	return 0;
}

/*
 * Parse one reply chunk out of the receive OOL buffer and append its payload to
 * the caller's buffer. Every rule of §4.3 is checked here, and nothing is
 * copied before all of them pass. The payload is copied before the caller's
 * 0xfe goes out, which is the only way it survives: the SEP rewrites the buffer
 * from offset 0 on the next chunk (§6, C2).
 *
 * Two rules are ours, not XNU's, and both fail closed:
 *
 *  - the reply tag and sequence progression: the first reply must be 0xfc with
 *    sequence 0, and every continuation 0xfd with a strictly greater sequence
 *    (0x6c is the trace: fc seq 0, then fd seq 1). None of XNU's three reply
 *    parsers looks at the sequence, so this is defensive: it catches a
 *    duplicated or reordered continuation, and it tolerates gaps so that a
 *    sepOS with a different numbering is not refused over the exact step. The
 *    observed wire agrees with it; if a real reply is refused here, relax this
 *    check from the log, not pre-emptively.
 *  - a reply tag outside 0xfc/0xfd never reaches this function in the driver
 *    (sep_gt_classify() splits the tags), but the core rejects it too, so the
 *    host tests and any future caller get the same answer.
 */
static int sep_gt_xact_rx_chunk(struct sep_gt_xact *x, const u8 *ool,
				size_t ool_len, u8 tag, u16 seq)
{
	struct sep_gt_hdr h;
	u32 cap = sep_gt_cap(ool_len);

	if (x->err)
		return x->err;
	if (sep_gt_xact_closed(x)) {
		x->ignored++;
		return 0;
	}
	if (tag != SEP_GT_TAG_FIRST && tag != SEP_GT_TAG_CONT)
		return sep_gt_fail(x, -EPROTO);
	if (ool_len < SEP_GT_HDR_LEN)
		return sep_gt_fail(x, -EMSGSIZE);

	memset(&h, 0, sizeof(h));
	sep_gt_hdr_unpack(ool, &h);
	x->last = h;
	x->last_tag = tag;
	x->last_seq = seq;

	if (h.version != SEP_GT_VERSION)
		return sep_gt_fail(x, -EPROTO);
	if (h.flags & SEP_GT_FLAGS_ALLOC)
		return sep_gt_fail(x, -EPROTO);
	if (h.total > SEP_GT_MAX_TOTAL)
		return sep_gt_fail(x, -EMSGSIZE);
	if (h.chunk > h.total)
		return sep_gt_fail(x, -EPROTO);
	if (h.chunk > cap)
		return sep_gt_fail(x, -EMSGSIZE);
	/* +0x10 is the status field only in a tag 0xff envelope; a chunk reply
	 * that puts anything there is not a chunk reply we understand.
	 */
	if (h.reserved)
		return sep_gt_fail(x, -EPROTO);
	/* A body with nowhere to go. XNU asserts on this after the fact
	 * (`out == NULL && gt->outlen`); we refuse the frame before the copy and
	 * before any state says "complete".
	 */
	if (h.chunk && !x->out)
		return sep_gt_fail(x, -ENOBUFS);

	if (tag == SEP_GT_TAG_FIRST) {
		if (x->state != SEP_GT_SEND)
			return sep_gt_fail(x, -EPROTO);	/* duplicate opener */
		if (seq)
			return sep_gt_fail(x, -EPROTO);	/* fc is sequence 0 */
		if (h.offset)
			return sep_gt_fail(x, -EPROTO);
		if (h.command != x->cmd)
			return sep_gt_fail(x, -EPROTO);
		if (h.total > x->out_cap)
			return sep_gt_fail(x, -EMSGSIZE);
		x->out_total = h.total;
		x->state = SEP_GT_RECV;
	} else {
		if (x->state != SEP_GT_RECV)
			return sep_gt_fail(x, -EPROTO);	/* no opener yet */
		if (seq <= x->recv_seq)
			return sep_gt_fail(x, -EPROTO);	/* duplicate or reordered */
		if (h.offset != x->out_done)
			return sep_gt_fail(x, -EPROTO);
		if (h.command != x->cmd)
			return sep_gt_fail(x, -EPROTO);
		if (h.total != x->out_total)
			return sep_gt_fail(x, -EPROTO);
	}
	if (x->out_done > x->out_total)	/* cannot happen; keep the maths safe */
		return sep_gt_fail(x, -EPROTO);
	if (h.chunk > x->out_total - x->out_done)
		return sep_gt_fail(x, -EPROTO);	/* progress + chunk > total */
	if (x->n_recv >= SEP_GT_MAX_CHUNKS)
		return sep_gt_fail(x, -EPROTO);	/* chunk-count ceiling */

	if (h.chunk)
		memcpy(x->out + x->out_done, ool + SEP_GT_HDR_LEN, h.chunk);
	x->recv[x->n_recv].tag = tag;
	x->recv[x->n_recv].seq = seq;
	x->recv[x->n_recv].offset = h.offset;
	x->recv[x->n_recv].len = h.chunk;
	x->n_recv++;
	x->out_done += h.chunk;
	x->want_more = x->out_done < x->out_total;
	x->done = !x->want_more;
	x->recv_seq = seq;		/* the last accepted reply sequence */
	return 0;
}

/*
 * Parse a tag 0xff error envelope: the 32-bit status sits at +0x10, which is
 * the header's reserved field, and the command is not echoed (§3.2, §7.2). The
 * value is reported raw — 0x101 has no symbolic meaning in the kernelcache and
 * is never special-cased. Status 0 is XNU's "treat as complete".
 */
static int sep_gt_xact_rx_error(struct sep_gt_xact *x, const u8 *ool,
				size_t ool_len, u16 seq)
{
	struct sep_gt_hdr h;

	if (x->err)
		return x->err;
	if (sep_gt_xact_closed(x)) {
		x->ignored++;
		return 0;
	}
	if (ool_len < SEP_GT_HDR_LEN)
		return sep_gt_fail(x, -EMSGSIZE);

	memset(&h, 0, sizeof(h));
	sep_gt_hdr_unpack(ool, &h);
	x->last = h;
	x->last_tag = SEP_GT_TAG_ERR;
	x->last_seq = seq;
	x->status = h.reserved;
	if (x->n_recv < SEP_GT_MAX_CHUNKS) {
		x->recv[x->n_recv].tag = SEP_GT_TAG_ERR;
		x->recv[x->n_recv].seq = seq;
		x->recv[x->n_recv].offset = h.offset;
		x->recv[x->n_recv].len = 0;
	}
	x->n_recv++;
	if (!x->status) {
		x->want_more = false;
		x->done = true;
	}
	return 0;
}

/*
 * Does this failure mean "the SEP and this driver no longer agree about the
 * wire", i.e. does the next transaction have to wait for the quarantine?
 *
 * Every way a received frame can be rejected lands here (-EPROTO for a shape or
 * state violation, -EMSGSIZE for an impossible size, -ENODEV for a missing
 * receive buffer, -ENOBUFS for a body with nowhere to go), so the driver's
 * quarantine decision is one classification instead of a list of errnos that
 * silently misses the next one. -ETIMEDOUT has its own accounting, -EIO is the
 * AP->SEP transport (a local store failure, not a received frame) and -EINVAL
 * is a caller mistake before anything was sent; none of them mean a desynced
 * peer.
 */
static bool sep_gt_is_protocol_error(int err)
{
	switch (err) {
	case -EPROTO:
	case -EMSGSIZE:
	case -ENODEV:
	case -ENOBUFS:
		return true;
	default:
		return false;
	}
}

/*
 * Strict parser for the debugfs 'xfer' line, "EP CMD [HEXBYTES]".
 *
 * EP and CMD are hex without a 0x prefix or sign; the payload is an even number
 * of hex digits, byte by byte, with optional spaces/tabs between bytes and
 * optional trailing whitespace. An odd nibble, a non-hex character, junk after
 * the payload or a field above its limit is -EINVAL, and a payload longer than
 * `max` is -E2BIG; nothing is ever truncated into a "valid" shorter request,
 * which is what the old sscanf/kstrtouint version did.
 *
 * An empty payload (no third field at all) is the legal zero-length request:
 * *len = 0.
 */
static int sep_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool sep_is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* One hex field: at least one digit, <= max, and no junk. */
static int sep_hex_field(const char **pp, u32 max, u32 *out)
{
	const char *p = *pp;
	u32 v = 0;
	int d;

	while (sep_is_space(*p))
		p++;
	if (sep_hexval(*p) < 0)
		return -EINVAL;
	while ((d = sep_hexval(*p)) >= 0) {
		if (v > 0x0fffffff)
			return -E2BIG;	/* would not fit in a u32 */
		v = (v << 4) | (u32)d;
		p++;
	}
	if (v > max)
		return -EINVAL;
	*pp = p;
	*out = v;
	return 0;
}

static int sep_xfer_parse(const char *src, u32 max_ep, unsigned int *ep,
			  unsigned int *cmd, u8 *payload, size_t max,
			  size_t *len)
{
	const char *p = src;
	size_t n = 0;
	u32 v;
	int ret, hi, lo;

	*len = 0;
	ret = sep_hex_field(&p, max_ep, &v);
	if (ret)
		return ret;
	*ep = v;
	ret = sep_hex_field(&p, 0xff, &v);
	if (ret)
		return ret;
	*cmd = v;

	for (;;) {
		while (sep_is_space(*p))
			p++;
		if (!*p)
			break;
		hi = sep_hexval(*p++);
		if (hi < 0)
			return -EINVAL;
		lo = sep_hexval(*p);
		if (lo < 0)
			return -EINVAL;	/* odd nibble, or junk in the payload */
		p++;
		if (n >= max)
			return -E2BIG;
		payload[n++] = (u8)((hi << 4) | lo);
	}
	*len = n;
	return 0;
}

/* <<< SEP_GT_CORE_END */

#define SEP_CMD_SBIO_INIT	0x73

/* AppleSEPARTStorageService on the advertised 'arts' endpoint. */
#define SEP_NAME_ARTS		0x61727473	/* 'arts', big-endian 4CC */
#define SEP_ARTS_OOL_SIZE	0x4000		/* j71t advertises 4 pages */
#define SEP_ARTS_TAG		0x01		/* first XNU tag-pool entry + 1 */
#define SEP_ARTS_OP_MANIFEST	0x06		/* Opcode::MANIFEST (3) << 1 */
#define SEP_ARTS_OP_REPLY	0x07		/* request opcode | 1 */
#define SEP_ARTS_TIMEOUT_MS	10000
#define SEP_ARTS_DER_MIN	10

enum sep_arts_result {
	SEP_ARTS_NEVER,
	SEP_ARTS_WAITING,
	SEP_ARTS_REPLY,
	SEP_ARTS_TIMEOUT,
};

struct sep_ep {
	/* advertisement */
	u32 name;
	u8 in_min, in_max, out_min, out_max;	/* 4 KiB pages */
	bool has_name;
	bool has_ool;
	bool name_manual;	/* declared through debugfs advertise, */
	bool ool_manual;	/* not (yet) seen on the mailbox */
	bool conflict;		/* a later mailbox advertisement disagreed */

	/* OOL buffers, kept until the driver goes away once published */
	bool published;
	void *send;
	void *recv;
	phys_addr_t send_pa;
	phys_addr_t recv_pa;
	size_t send_len;
	size_t recv_len;
};

/* One unsolicited non-transport word, exactly as received (§9). */
struct sep_notify {
	u64 msg;
	ktime_t at;
	u64 seq;
	u32 data;		/* bits 32..63, the whole info/data field */
	u16 len;		/* bits 16..31, the 16-bit value/length field */
	u8 ep;
	u8 tag;
};

/* PMGR word and, only while it says on and out of reset, SEP core registers */
struct sep_core_snap {
	bool taken;
	int pmgr_ret;
	u32 pmgr;
	bool valid;			/* reg[] was read */
	u32 reg[SEP_CORE_NREGS];
};

struct sep_a9 {
	struct device *dev;
	void __iomem *base;
	void __iomem *mbox;

	struct mutex lock;		/* receive side: log, endpoint table */
	struct mutex tx_lock;		/* AP->SEP FIFO */
	struct mutex cfg_lock;		/* debugfs configuration and transfers */
	u64 log[SEP_LOG_ENTRIES];
	unsigned int log_head;
	unsigned int log_count;
	unsigned long last_rx;		/* jiffies */

	struct delayed_work poll;
	unsigned int poll_us;
	bool running;

	struct sep_ep *eps;		/* SEP_EP_COUNT entries */

	/*
	 * The one mailbox-wide GenericTransfer transaction (§11.3 I1).
	 *
	 * Ownership, one rule for everything RX can see: sep->lock. gt_active and
	 * gt_ep are read by RX; gt_quiet_until is restarted by every stale GT word
	 * on the RX path and is the quarantine the next transfer's gate looks at;
	 * gt_ok/gt_stale/gt_tmo/gt_proto/gt_dead are counted and read under the
	 * same lock (status, xfer readback, gate). cfg_lock only serialises whole
	 * transactions, it does not protect any field on its own.
	 *
	 * `gt` itself is written by RX only while gt_active is set (RX holds
	 * sep->lock throughout), by the waiter under sep->lock when it consumes
	 * an event, and by the holder of cfg_lock before/after the active window.
	 * Nothing RX touches is ever reachable after the atomic detach in
	 * sep_gt_transfer().
	 */
	struct sep_gt_xact gt;
	struct completion gt_ev;
	bool gt_active;			/* under sep->lock */
	u8 gt_ep;			/* under sep->lock */
	unsigned long gt_quiet_until;	/* under sep->lock */
	u32 gt_ok;			/* under sep->lock */
	u32 gt_stale;			/* under sep->lock */
	u32 gt_tmo;			/* under sep->lock */
	u32 gt_proto;			/* under sep->lock */
	bool gt_dead;			/* under sep->lock */
	bool gt_stale_logged;		/* under sep->lock */

	int sbio_ep;			/* -1 = unset, under sep->lock */
	bool sbio_ready;		/* 0x73 acknowledged on sbio_ep */

	/* passive capture of non-GT tags on sbio_ep, under sep->lock */
	struct sep_notify notify[SEP_NOTIFY_ENTRIES];
	unsigned int notify_head;
	unsigned int notify_count;
	unsigned int notify_dropped;
	u64 notify_total;

	/* last debugfs xfer, for reading back */
	int last_ep;
	u8 last_cmd;
	int last_ret;
	u32 last_status;
	size_t last_out_len;
	u8 *last_out;			/* SEP_GT_MAX_TOTAL bytes, devm */

	/* One-shot AppleSEPARTStorage MANIFEST request, under cfg_lock. */
	struct completion arts_done;
	enum sep_arts_result arts_result;
	bool arts_ran;
	bool arts_waiting;
	int arts_ep;
	size_t arts_manifest_len;
	u8 arts_reply_tag;
	u8 arts_reply_op;
	u8 arts_reply_param;
	u32 arts_reply_data;
	u64 arts_reply_raw;
	int arts_ret;
	s64 arts_reply_us;
	ktime_t arts_t0;

	/* last wrapper activation (debugfs start), under cfg_lock */
	bool armed_once;
	u32 pwrstate;
	u32 wrap_before[SEP_WRAP_NREGS];
	u32 wrap_after[SEP_WRAP_NREGS];
	u32 outbox_read;		/* +0x4020 before the RMW */

	/* read-only snapshots: probe, then around the last arm (cfg_lock) */
	struct sep_core_snap probe_snap;
	struct sep_core_snap arm_snap;
	struct sep_core_snap arm_after_snap;

	/* nap-wake diagnostic, once per boot */
	bool wake_waiting;		/* under lock: sep_wake_rx records */
	bool wake_seen;			/* under lock */
	u64 wake_msg;			/* first ep 0 op 0x0f/0x15 */
	ktime_t wake_at;
	unsigned int wake_ind;		/* indications seen */
	unsigned int wake_other;	/* any other message in the window */
	u64 wake_other_first;
	bool wake_ran;			/* the rest under cfg_lock */
	int wake_ret;
	ktime_t wake_t0;		/* first word posted */
	u32 wake_pmgr;
	u32 wake_pmgr_after;		/* re-read at the end */
	int wake_pmgr_after_ret;
	u32 wake_sts[3];		/* snd_sts: before, after word 1, at the end */
	bool wake_w1_drained;
	bool wake_w2_posted;
	bool wake_w2_drained;
	s64 wake_w1_us;
	s64 wake_w2_us;

	/* last AP->SEP drain check (ool, send), under cfg_lock */
	char drain_what[32];
	int drain_ret;
	u32 drain_sts_before;
	u32 drain_sts_after;
	s64 drain_us;

	struct dentry *dbg;
	u64 tx_count;
	u64 rx_count;
	u64 tx_busy;
	u64 dropped;
};

/* Wrapper registers snapshotted around arming, in this order. */
static const u16 sep_wrap_regs[SEP_WRAP_NREGS] = {
	SEP_WRAP_MASK_SET, SEP_WRAP_MASK_CLR, SEP_WRAP_MIRROR_SET,
	SEP_WRAP_MIRROR_CLR, SEP_WRAP_INBOX_CTL, SEP_WRAP_OUTBOX_CTL,
};

static void sep_wrap_snapshot(struct sep_a9 *sep, u32 *v)
{
	unsigned int i;

	for (i = 0; i < SEP_WRAP_NREGS; i++)
		v[i] = readl(sep->base + sep_wrap_regs[i]);
}

static int sep_wrap_print(char *buf, size_t size, const u32 *v)
{
	return scnprintf(buf, size,
			 "set4000=%08x clr4004=%08x setc00=%08x clrc04=%08x snd4008=%08x rcv4020=%08x",
			 v[0], v[1], v[2], v[3], v[4], v[5]);
}

static const u16 sep_core_regs[SEP_CORE_NREGS] = {
	0x28, 0x44, 0xb88, SEP_WRAP_INBOX_CTL, SEP_WRAP_OUTBOX_CTL,
};

static int sep_core_print(char *buf, size_t size, const struct sep_core_snap *s)
{
	if (!s->taken)
		return scnprintf(buf, size, "not taken");
	if (s->pmgr_ret)
		return scnprintf(buf, size, "pmgr=read error %d, SEP not read",
				 s->pmgr_ret);
	if (!s->valid)
		return scnprintf(buf, size, "pmgr=%08x (not on), SEP not read",
				 s->pmgr);
	return scnprintf(buf, size,
			 "pmgr=%08x sep28=%08x sep44=%08x sepb88=%08x snd4008=%08x rcv4020=%08x",
			 s->pmgr, s->reg[0], s->reg[1], s->reg[2], s->reg[3],
			 s->reg[4]);
}

/* snd_sts/recv_sts: bit 17 empty, bit 16 full */
static const char *sep_fifo_state(u32 sts)
{
	if (sts & SEP_MBOX_STS_EMPTY)
		return "empty";
	return sts & SEP_MBOX_STS_FULL ? "full" : "not empty";
}

/*
 * Pop exactly one SEP->AP word. The caller must first observe recv_sts bit 17
 * clear. noinline keeps this primitive visible for the packaged-vmlinux
 * instruction audit: one 64-bit load from mbox + 0x38 and no access to +0x34.
 */
static noinline u64 sep_read_msg(struct sep_a9 *sep)
{
	return readq(sep->mbox + SEP_MBOX_RECV);
}

static void sep_log_push(struct sep_a9 *sep, u64 msg)
{
	unsigned int slot = (sep->log_head + sep->log_count) % SEP_LOG_ENTRIES;

	sep->log[slot] = msg;
	if (sep->log_count < SEP_LOG_ENTRIES)
		sep->log_count++;
	else
		sep->log_head = (sep->log_head + 1) % SEP_LOG_ENTRIES;

	sep->rx_count++;
	sep->last_rx = jiffies;
}

/* The 4CC is stored most significant byte first; decode only printable names. */
static bool sep_fourcc(u32 word, char name[5])
{
	int i;

	for (i = 0; i < 4; i++) {
		name[i] = (char)(word >> (24 - 8 * i));
		if (name[i] < 0x20 || name[i] > 0x7e)
			return false;
	}
	name[4] = '\0';
	return true;
}

static void sep_ool_word(u32 word, u8 *in_min, u8 *in_max, u8 *out_min,
			 u8 *out_max)
{
	*in_min = word & 0xff;
	*in_max = (word >> 8) & 0xff;
	*out_min = (word >> 16) & 0xff;
	*out_max = word >> 24;
}

static bool sep_ep_complete(const struct sep_ep *e)
{
	return e->has_name && e->has_ool;
}

/* Record one 0xfd message. Called with sep->lock held. */
static void sep_discovery_rx(struct sep_a9 *sep, u64 msg)
{
	u8 id = SEP_MSG_GET_PARAM(msg);
	u32 data = SEP_MSG_GET_DATA(msg);
	struct sep_ep *e = &sep->eps[id];
	u8 a, b, c, d;
	char name[5];

	switch (SEP_MSG_GET_OP(msg)) {
	case SEP_DISC_NAME:
		if (!sep_fourcc(data, name))
			strscpy(name, "????");
		if (e->has_name && e->name != data) {
			e->conflict = true;
			dev_warn(sep->dev, "ep %#04x: name %08x conflicts with %08x\n",
				 id, data, e->name);
			return;
		}
		e->name = data;
		e->has_name = true;
		e->name_manual = false;
		dev_info(sep->dev, "ep %#04x: advertised '%s'\n", id, name);
		break;
	case SEP_DISC_OOL:
		sep_ool_word(data, &a, &b, &c, &d);
		if (e->has_ool && (e->in_min != a || e->in_max != b ||
				   e->out_min != c || e->out_max != d)) {
			e->conflict = true;
			dev_warn(sep->dev, "ep %#04x: OOL advertisement %08x conflicts\n",
				 id, data);
			return;
		}
		e->in_min = a;
		e->in_max = b;
		e->out_min = c;
		e->out_max = d;
		e->has_ool = true;
		e->ool_manual = false;
		dev_info(sep->dev, "ep %#04x: OOL in [%u,%u] out [%u,%u] pages%s\n",
			 id, a, b, c, d, e->has_name ? "" : " (no name yet)");
		break;
	default:
		dev_warn(sep->dev, "ep 0xfd: unknown op %#x\n", SEP_MSG_GET_OP(msg));
		break;
	}
}

/*
 * A transport word that no unfinished transaction is waiting for: either no
 * transaction at all, or one that has already completed (a full reply or a SEP
 * error). Count it, drop it, and restart the quarantine. Nothing here touches an
 * OOL buffer or a completion, which is what keeps a late word from an abandoned
 * transaction from ever satisfying a later one (§11.3 I8).
 *
 * Restarting gt_quiet_until on *every* stale word is what makes the window mean
 * "100 ms after the last stale GT word" instead of "100 ms after the error that
 * abandoned the transaction": a SEP that dribbles out late chunks 30 ms apart
 * would otherwise hand the next transfer a word inside its quiet period. Called
 * with sep->lock held, so the deadline is visible to the next gate test at once.
 *
 * Logged once per boot only; the counter and the deadline are the real signal,
 * a stale word per poll would otherwise flood dmesg.
 */
static void sep_gt_stale(struct sep_a9 *sep, u64 msg)
{
	lockdep_assert_held(&sep->lock);
	sep->gt_quiet_until = jiffies + msecs_to_jiffies(SEP_GT_QUIET_MS);
	sep->gt_stale++;
	if (sep->gt_stale_logged)
		return;
	sep->gt_stale_logged = true;
	dev_warn(sep->dev,
		 "gt: stale word with no unfinished transaction: raw=%016llx ep=%02x tag=%02x cmd=%#x seq=%u (dropped, no buffer touched, quarantine restarted)\n",
		 msg, sep_gt_word_ep(msg), sep_gt_word_tag(msg),
		 sep_gt_word_cmd(msg), sep_gt_word_seq(msg));
}

/*
 * Passive capture of one unsolicited non-transport word on the selected async
 * endpoint. Recorded raw with its two service fields (bits 16..31 the 16-bit
 * value/length, bits 32..63 the whole info/data word, §9) and never
 * interpreted: no reply is sent, no lock state is synthesised and no body is
 * read from anywhere.
 *
 * What the trace and handler show: tags 1, 2 and 3 are self-contained service
 * notifications. Later GenericTransfer traffic can be causally related to an
 * operation, but it is not a payload continuation owned by this mailbox word.
 * The driver therefore records every field and infers no body or lock state.
 * Called with sep->lock held; no allocation, no sleeping.
 */
static void sep_notify_push(struct sep_a9 *sep, u64 msg)
{
	unsigned int slot = (sep->notify_head + sep->notify_count) %
			    SEP_NOTIFY_ENTRIES;
	struct sep_notify *n = &sep->notify[slot];

	lockdep_assert_held(&sep->lock);
	n->msg = msg;
	n->at = ktime_get();
	n->seq = ++sep->notify_total;
	n->ep = sep_gt_word_ep(msg);
	n->tag = sep_gt_word_tag(msg);
	n->len = (u16)((msg >> 16) & 0xffff);
	n->data = (u32)(msg >> 32);

	if (sep->notify_count < SEP_NOTIFY_ENTRIES) {
		sep->notify_count++;
	} else {
		sep->notify_head = (sep->notify_head + 1) % SEP_NOTIFY_ENTRIES;
		sep->notify_dropped++;
	}
}

/*
 * One transport word for the active transaction. Called with sep->lock held and
 * inside the RX path, so: invalidate, parse, copy, complete — no allocation, no
 * sleeping, no mailbox write, and no lock other than the one already held. The
 * whole receive buffer is invalidated because the CPU never writes it between
 * publication and the end of a transfer (§6 rule C1); the payload is copied out
 * here, before the waiter can send the 0xfe that asks for the next chunk (C2).
 */
static void sep_gt_rx(struct sep_a9 *sep, enum sep_gt_rx_kind kind, u64 msg)
{
	struct sep_gt_xact *x = &sep->gt;
	struct sep_ep *e = &sep->eps[sep->gt_ep];
	u8 tag = sep_gt_word_tag(msg);
	u16 seq = sep_gt_word_seq(msg);
	int ret = 0;

	lockdep_assert_held(&sep->lock);
	x->last_msg_data = (u32)(msg >> 32);
	/*
	 * The transaction has already ended (a complete reply or a SEP error):
	 * this word cannot belong to it. Treat it as stale — the same path as a
	 * word with no transaction at all — so it restarts the quiet window and
	 * cannot be followed immediately by a transfer that would race it. The
	 * core's own "closed" branch is the identical rule for direct callers;
	 * the driver never reaches it.
	 */
	if (sep_gt_xact_closed(x)) {
		sep_gt_stale(sep, msg);
		goto out;
	}
	if (!e->recv || !e->recv_len) {
		/* No buffer to parse: a receive-side shape failure, so it is
		 * accounted like any other rejected frame, not silently fatal.
		 */
		ret = sep_gt_fail(x, -ENODEV);
		goto out;
	}

	if (kind == SEP_GT_RX_CREDIT) {
		ret = sep_gt_xact_rx_credit(x);
	} else {
		dcache_inval_poc((unsigned long)e->recv,
				 (unsigned long)e->recv + e->recv_len);
		if (kind == SEP_GT_RX_CHUNK)
			ret = sep_gt_xact_rx_chunk(x, e->recv, e->recv_len,
						   tag, seq);
		else
			ret = sep_gt_xact_rx_error(x, e->recv, e->recv_len, seq);
	}

	if (ret && x->violations == 1) {
		dev_warn(sep->dev,
			 "gt: ep %#04x cmd %#x: reply rejected (%d): tag=%02x seq=%u v=%u total=%u offset=%u flags=%#x reserved=%#x command=%#x chunk=%u\n",
			 sep->gt_ep, x->cmd, ret, tag, seq, x->last.version,
			 x->last.total, x->last.offset, x->last.flags,
			 x->last.reserved, x->last.command, x->last.chunk);
	}
out:
	complete(&sep->gt_ev);
}

/* Record the one expected arts MANIFEST response. Called with lock held. */
static void sep_arts_rx(struct sep_a9 *sep, u64 msg)
{
	if (!sep->arts_waiting || SEP_MSG_GET_EP(msg) != sep->arts_ep ||
	    SEP_MSG_GET_TAG(msg) != SEP_ARTS_TAG ||
	    SEP_MSG_GET_OP(msg) != SEP_ARTS_OP_REPLY)
		return;

	sep->arts_reply_tag = SEP_MSG_GET_TAG(msg);
	sep->arts_reply_op = SEP_MSG_GET_OP(msg);
	sep->arts_reply_param = SEP_MSG_GET_PARAM(msg);
	sep->arts_reply_data = SEP_MSG_GET_DATA(msg);
	sep->arts_reply_raw = msg;
	sep->arts_reply_us = ktime_us_delta(ktime_get(), sep->arts_t0);
	sep->arts_result = SEP_ARTS_REPLY;
	sep->arts_waiting = false;
	complete(&sep->arts_done);
}

/*
 * During a wake test: ep 0 op 0x0f or 0x15 is the indication the XNU mailbox
 * drain callback wakes its waiter for. Anything else is counted, first one
 * kept. Called with sep->lock held.
 */
static void sep_wake_rx(struct sep_a9 *sep, u64 msg)
{
	u8 op = SEP_MSG_GET_OP(msg);

	if (!sep->wake_waiting)
		return;

	if (SEP_MSG_GET_EP(msg) == 0 &&
	    (op == SEP_OP_NAP_AWAKE || op == SEP_OP_OS_ACTIVE)) {
		if (!sep->wake_ind++) {
			sep->wake_msg = msg;
			sep->wake_at = ktime_get();
			sep->wake_seen = true;
		}
		return;
	}
	if (!sep->wake_other++)
		sep->wake_other_first = msg;
}

/* Verdict of the last wake test. Called with cfg_lock held. */
static const char *sep_wake_result(const struct sep_a9 *sep)
{
	if (!sep->wake_ran)
		return "never";
	if (sep->wake_ret == -ETIMEDOUT)
		return "word1-stuck";
	if (sep->wake_seen)
		return "wake";
	if (sep->wake_ret == -ENODATA)
		return "no-wake";
	return "error";
}

/*
 * Drain everything the SEP queued. The status bit is the same handshake
 * pongoOS uses; there is no separate acknowledge on the AP side.
 *
 * This is the single RX entry point, and the only place a received word is
 * classified. Order matters:
 *
 *	raw ring -> wake indication -> discovery -> ARTS -> GenericTransfer ->
 *	passive async capture
 *
 * A word can be claimed by exactly one of the last four. Discovery is the
 * transport endpoint itself; ARTS is the named storage service (its replies use
 * tag 1, so endpoint filtering is what keeps them out of the SBIO async ring);
 * GenericTransfer only accepts 0xfc..0xff for the endpoint of the transaction in
 * flight; everything else on the selected async endpoint is captured read-only.
 */
static int sep_drain(struct sep_a9 *sep)
{
	unsigned int drained = 0;

	mutex_lock(&sep->lock);
	while (!(readl(sep->mbox + SEP_MBOX_RECV_STS) & SEP_MBOX_STS_EMPTY)) {
		u64 msg = sep_read_msg(sep);
		enum sep_gt_rx_kind kind;

		sep_log_push(sep, msg);
		sep_wake_rx(sep, msg);
		if (SEP_MSG_GET_EP(msg) == SEP_EP_DISCOVERY) {
			sep_discovery_rx(sep, msg);
		} else {
			sep_arts_rx(sep, msg);
			kind = sep_gt_classify(msg, sep->gt_active, sep->gt_ep,
					       sep->sbio_ep);
			switch (kind) {
			case SEP_GT_RX_CHUNK:
			case SEP_GT_RX_CREDIT:
			case SEP_GT_RX_ERROR:
				sep_gt_rx(sep, kind, msg);
				break;
			case SEP_GT_RX_STALE:
				sep_gt_stale(sep, msg);
				break;
			case SEP_GT_RX_NOTIFY:
				sep_notify_push(sep, msg);
				break;
			default:
				break;
			}
		}

		if (++drained > SEP_LOG_ENTRIES) {
			sep->dropped++;
			break;
		}
	}
	mutex_unlock(&sep->lock);

	return drained;
}

static void sep_poll_work(struct work_struct *work)
{
	struct sep_a9 *sep = container_of(to_delayed_work(work),
					  struct sep_a9, poll);

	if (!sep->running)
		return;

	sep_drain(sep);
	schedule_delayed_work(&sep->poll, usecs_to_jiffies(sep->poll_us));
}

/*
 * Send one already-built message once the AP->SEP FIFO has room. The mailbox
 * must be armed: nothing touches the SEP registers before that.
 *
 * This is the only AP->SEP write in the driver (cntl, GenericTransfer and the
 * wake test all come here): a single 64-bit writeq() of low | high << 32 to
 * +0x4010, then mb() (dsb sy, as pongoOS "sep msg64" does, so both probes issue
 * the same sequence). XNU's postMailbox is one 64-bit store; tri53-sep..sep3
 * and pongoOS wrote the two halves with two 32-bit stores, and on 2026-10-26
 * such a word was left unconsumed even under pongoOS (16:18). noinline keeps
 * one store site in the object code, so the instruction can be checked.
 */
static noinline int sep_send_word(struct sep_a9 *sep, u64 msg)
{
	u32 sts;
	int ret;

	if (!sep->running)
		return -ENXIO;

	mutex_lock(&sep->tx_lock);
	ret = readl_poll_timeout(sep->mbox + SEP_MBOX_SND_STS, sts,
				 !(sts & SEP_MBOX_STS_FULL), 100,
				 SEP_TX_TIMEOUT_US);
	if (ret) {
		sep->tx_busy++;
		dev_warn(sep->dev, "tx FIFO full (snd_sts=%#x), not sent: %016llx\n",
			 sts, msg);
	} else {
		writeq(msg, sep->mbox + SEP_MBOX_SND);
		/* dsb sy: the word is visible before the SEP can drop the FIFO */
		mb();
		sep->tx_count++;
	}
	mutex_unlock(&sep->tx_lock);

	return ret ? -EBUSY : 0;
}

/*
 * The control-plane word shape: opcode in bits 16..23, parameter in 24..31,
 * data in 32..63. A GenericTransfer chunk word is built by sep_gt_word()
 * instead, which keeps the full 32-bit command and adds the sequence.
 */
static inline int sep_send_raw(struct sep_a9 *sep, u8 ep, u8 tag, u8 op,
			       u8 param, u32 data)
{
	return sep_send_word(sep, SEP_MSG(ep, tag, op, param, data));
}

/*
 * Wait up to 1 s for the AP->SEP FIFO to empty (snd_sts bit 17): the transport
 * proof that sepOS consumed what was just queued. It only reports; nothing is
 * resent. 'before' is snd_sts read before queueing. Called with cfg_lock held
 * on an armed mailbox.
 */
static __printf(3, 4) int sep_tx_drain(struct sep_a9 *sep, u32 before,
				       const char *fmt, ...)
{
	ktime_t start = ktime_get();
	va_list args;
	u32 sts;
	int ret;

	ret = readl_poll_timeout(sep->mbox + SEP_MBOX_SND_STS, sts,
				 sts & SEP_MBOX_STS_EMPTY, 100,
				 SEP_DRAIN_TIMEOUT_US);

	va_start(args, fmt);
	vscnprintf(sep->drain_what, sizeof(sep->drain_what), fmt, args);
	va_end(args);
	sep->drain_ret = ret;
	sep->drain_sts_before = before;
	sep->drain_sts_after = sts;
	sep->drain_us = ktime_us_delta(ktime_get(), start);

	if (ret)
		dev_warn(sep->dev, "%s: AP->SEP FIFO not drained after %lld us (snd_sts %#x -> %#x): transport FAIL\n",
			 sep->drain_what, sep->drain_us, before, sts);
	else
		dev_info(sep->dev, "%s: AP->SEP FIFO drained in %lld us (snd_sts %#x -> %#x): transport pass\n",
			 sep->drain_what, sep->drain_us, before, sts);
	return ret;
}

/*
 * Publish an endpoint's OOL buffers through 'cntl' (endpoint 0, the target
 * endpoint in param): send size (0x04), send address (0x02), receive size
 * (0x05), receive address (0x03). Sizes in bytes, addresses as PA >> 12, as
 * AppleSEPManager::setSendOOLBuffer/setReceiveOOLBuffer do.
 */
static int sep_ool_publish(struct sep_a9 *sep, u8 ep, struct sep_ep *e)
{
	int ret;

	ret = sep_send_raw(sep, 0, 0, SEP_OP_SET_SEND_SIZE, ep, e->send_len);
	if (!ret)
		ret = sep_send_raw(sep, 0, 0, SEP_OP_SET_SEND_ADDR, ep,
				   e->send_pa >> 12);
	if (!ret)
		ret = sep_send_raw(sep, 0, 0, SEP_OP_SET_RECV_SIZE, ep,
				   e->recv_len);
	if (!ret)
		ret = sep_send_raw(sep, 0, 0, SEP_OP_SET_RECV_ADDR, ep,
				   e->recv_pa >> 12);
	return ret;
}

static bool sep_ool_size_ok(size_t len, u8 min, u8 max)
{
	return len && IS_ALIGNED(len, SEP_OOL_PAGE) &&
	       len >= (size_t)min * SEP_OOL_PAGE &&
	       len <= (size_t)max * SEP_OOL_PAGE;
}

/*
 * Allocate and publish an endpoint's send and receive buffers, sized within
 * its advertised ranges. Physically contiguous and page aligned: the SEP
 * reads AP DRAM directly (no DART), so the physical address is handed over
 * as-is. The AP->SEP FIFO must be empty before the four control words, and
 * must drain within 1 s after them (-ETIMEDOUT otherwise: stop, no retry).
 * Called with cfg_lock held.
 */
static int sep_ep_alloc(struct sep_a9 *sep, u8 ep, size_t send_len,
			size_t recv_len)
{
	struct sep_ep *e = &sep->eps[ep];
	struct sep_ep adv;
	void *send, *recv;
	u32 before;
	int ret;

	if (ep > SEP_EP_CLIENT_MAX)
		return -EINVAL;

	mutex_lock(&sep->lock);
	adv = *e;
	mutex_unlock(&sep->lock);

	if (adv.published)
		return -EBUSY;	/* sepOS keeps the first binding */
	if (!sep_ep_complete(&adv))
		return -ENOENT;	/* no complete advertisement for ep */
	if (!sep_ool_size_ok(send_len, adv.in_min, adv.in_max) ||
	    !sep_ool_size_ok(recv_len, adv.out_min, adv.out_max))
		return -ERANGE;

	/* Words still queued would only pile up behind the new ones. */
	before = readl(sep->mbox + SEP_MBOX_SND_STS);
	if (!(before & SEP_MBOX_STS_EMPTY)) {
		dev_warn(sep->dev, "ep %#04x: AP->SEP FIFO not empty (snd_sts=%#x), OOL not published, nothing sent\n",
			 ep, before);
		return -EBUSY;
	}

	send = alloc_pages_exact(PAGE_ALIGN(send_len), GFP_KERNEL | __GFP_ZERO);
	recv = alloc_pages_exact(PAGE_ALIGN(recv_len), GFP_KERNEL | __GFP_ZERO);
	if (!send || !recv) {
		ret = -ENOMEM;
		goto err_free;
	}

	/*
	 * Write the zeroes back and drop the lines: the SEP must not read
	 * stale data, and no dirty line may later overwrite a SEP write.
	 */
	dcache_clean_inval_poc((unsigned long)send,
			       (unsigned long)send + PAGE_ALIGN(send_len));
	dcache_clean_inval_poc((unsigned long)recv,
			       (unsigned long)recv + PAGE_ALIGN(recv_len));

	mutex_lock(&sep->lock);
	e->send = send;
	e->recv = recv;
	e->send_pa = virt_to_phys(send);
	e->recv_pa = virt_to_phys(recv);
	e->send_len = send_len;
	e->recv_len = recv_len;
	mutex_unlock(&sep->lock);

	ret = sep_ool_publish(sep, ep, e);
	/*
	 * Once any control message went out the SEP may hold these addresses,
	 * so the pages are never freed after a partial publish either.
	 */
	mutex_lock(&sep->lock);
	e->published = true;
	mutex_unlock(&sep->lock);

	dev_info(sep->dev, "ep %#04x: OOL send %pa len %#zx recv %pa len %#zx%s\n",
		 ep, &e->send_pa, send_len, &e->recv_pa, recv_len,
		 ret ? " (publish incomplete)" : "");
	if (!ret)
		ret = sep_tx_drain(sep, before, "ool ep %#04x", ep);
	return ret;

err_free:
	if (send)
		free_pages_exact(send, PAGE_ALIGN(send_len));
	if (recv)
		free_pages_exact(recv, PAGE_ALIGN(recv_len));
	return ret;
}

/*
 * GenericTransfer engine, driver half (docs/sep/workstreams/sbio-runtime-re.md
 * §3-§7, §11.3-§11.5).
 *
 * Lock order, and there is no other nesting anywhere in this file:
 *
 *	cfg_lock -> { tx_lock, sep->lock }
 *
 * cfg_lock is the mailbox-wide GenericTransfer mutex: every transfer entry point
 * holds it for the whole transaction, which is what makes "exactly one
 * transaction per mailbox" true (I1), and gt_active re-checks that invariant
 * from the RX side. tx_lock and sep->lock are never taken together, and no path
 * takes cfg_lock while holding either. RX takes sep->lock only: it never sleeps,
 * never allocates, never writes the mailbox, and the only thing it signals is
 * gt_ev.
 *
 * sep->lock, not cfg_lock, owns everything RX can see: gt_active/gt_ep, the
 * transaction fields RX writes, and the gate/quarantine state (gt_quiet_until,
 * gt_dead, gt_tmo, gt_proto, gt_ok, gt_stale). That is why sep_gt_stale() can
 * push the quiet deadline out from the poll path while a debugfs reader is
 * printing it: both hold sep->lock. cfg_lock only serialises whole transfers.
 */

/*
 * XNU wipes both OOL buffers after every transfer (§4.5): payloads must not
 * linger in a buffer the next command reuses. Clean, never invalidate, so no
 * line of ours is discarded (C1); only called once the transaction is detached
 * and the mailbox drained, so no RX path is reading the receive buffer. A
 * GenericTransfer on the 'arts' endpoint therefore discards a staged IM4M — the
 * two are not used together.
 */
static void sep_gt_wipe(struct sep_ep *e)
{
	if (e->send && e->send_len) {
		memset(e->send, 0, e->send_len);
		dcache_clean_poc((unsigned long)e->send,
				 (unsigned long)e->send + e->send_len);
	}
	if (e->recv && e->recv_len) {
		memset(e->recv, 0, e->recv_len);
		dcache_clean_poc((unsigned long)e->recv,
				 (unsigned long)e->recv + e->recv_len);
	}
}

/* Stage, clean and post one request chunk. Called with cfg_lock held. */
static int sep_gt_send_chunk(struct sep_a9 *sep, struct sep_ep *e)
{
	struct sep_gt_xact *x = &sep->gt;
	u8 tag = 0;
	u16 seq = 0;
	u32 staged;
	int ret;

	staged = sep_gt_xact_stage(x, e->send, e->send_len, &tag, &seq);
	if (!staged) {
		if (!x->err)
			x->err = -EPROTO;	/* asked for a chunk with none left */
		return x->err;
	}
	/* header and payload complete before the doorbell (I2) */
	dcache_clean_poc((unsigned long)e->send, (unsigned long)e->send + staged);

	ret = sep_send_word(sep, sep_gt_word(sep->gt_ep, tag, x->cmd, seq));
	if (ret)
		x->err = -EIO;
	return ret;
}

/*
 * The 0xfe that asks for the next reply chunk. No payload, sequence 0, command
 * echoed, and the previous send header is left in the OOL buffer exactly as XNU
 * leaves it (§3.4).
 */
static int sep_gt_send_credit(struct sep_a9 *sep)
{
	int ret = sep_send_word(sep, sep_gt_credit_word(sep->gt_ep, sep->gt.cmd));

	if (ret)
		sep->gt.err = -EIO;
	return ret;
}

/*
 * Wait for the next transport event, bounded by the transaction's one absolute
 * deadline. A 2 ms slice with one drain per slice keeps a multi-chunk transfer
 * from depending on the poll cadence; the deadline still ends it.
 */
static int sep_gt_wait(struct sep_a9 *sep, ktime_t deadline)
{
	for (;;) {
		s64 left_ms = ktime_ms_delta(deadline, ktime_get());
		unsigned long slice;

		if (left_ms <= 0)
			return -ETIMEDOUT;
		slice = msecs_to_jiffies(left_ms < 2 ? 1 : 2);
		if (wait_for_completion_timeout(&sep->gt_ev, slice))
			return 0;
		sep_drain(sep);
	}
}

/*
 * One GenericTransfer transaction: the request goes out in as many 0xfc/0xfd
 * chunks as the endpoint's own published send buffer needs, each one only after
 * the SEP asked for it with 0xfe, then the reply arrives the same way with our
 * 0xfe credits in between.
 *
 *	AP:  ep=E tag=0xfc cmd           opener, sequence 0, payload in the OOL
 *	SEP: ep=E tag=0xfe cmd           send the next chunk
 *	AP:  ep=E tag=0xfd cmd           continuation, sequence 1..
 *	SEP: ep=E tag=0xfc / 0xfd cmd    reply chunks, offset = bytes so far
 *	SEP: ep=E tag=0xff op=0          error, status at receive OOL +0x10
 *
 * One absolute deadline and no retries. Sequence is checked only for defensive
 * intra-reply progression; it is not a transaction correlation token. On the
 * wire a late word of an abandoned transaction is indistinguishable from one
 * of the next, which is why the gate and quarantine exist. Called with
 * cfg_lock held.
 *
 * Failure classes, and why they are not one class:
 *
 *	-ETIMEDOUT	nothing arrived in time; 2 in a row poison the mailbox
 *	-EIO		the AP->SEP store failed; local, no quiet window
 *	protocol	anything sep_gt_is_protocol_error() accepts, i.e. a frame
 *			we received and could not believe or a state it broke;
 *			quarantine, and 3 in a row poison the mailbox
 *
 * Everything the quarantine protects is decided inside one sep->lock section at
 * the detach below, so a stale word that arrives while this transaction is
 * still detached-or-active can never be classified against the next one.
 */
static int sep_gt_transfer(struct sep_a9 *sep, u8 ep, u8 cmd,
			   const void *in, size_t in_len,
			   void *out, size_t out_cap,
			   size_t *replylen, u32 *status)
{
	struct sep_gt_xact *x = &sep->gt;
	struct sep_ep *e = &sep->eps[ep];
	ktime_t deadline;
	u32 cap_send, cap_recv;
	bool fifo_empty, dead, active;
	bool proto, poisoned;
	s64 quiet_ms = 0;
	int ret;

	*replylen = 0;
	*status = 0;

	if (!sep->running)
		return -ENXIO;		/* unarmed: not one MMIO access either */
	if (ep > SEP_EP_CLIENT_MAX || !e->published || !e->send || !e->recv)
		return -ENODEV;
	cap_send = sep_gt_cap(e->send_len);
	cap_recv = sep_gt_cap(e->recv_len);
	if (!cap_send || !cap_recv)
		return -ENODEV;
	if (in_len > SEP_GT_MAX_TOTAL ||
	    in_len > (size_t)SEP_GT_MAX_CHUNKS * cap_send)
		return -EMSGSIZE;
	if (in_len && !in)
		return -EINVAL;

	/*
	 * The output contract, checked before a single byte goes out: a buffer
	 * with no capacity and a capacity with no buffer are both caller bugs,
	 * and a command that answers with a body we cannot take must never be
	 * started (XNU: `(out != NULL) == (outlen == NULL)` -> 0xe00002c2).
	 */
	if ((out == NULL) != (out_cap == 0))
		return -EINVAL;

	/*
	 * Flush what is already queued before the gate looks at it: those words
	 * belong to an earlier transaction (or to nobody) and must be counted as
	 * stale now, not mistaken for a reply to this one.
	 */
	sep_drain(sep);

	fifo_empty = !!(readl(sep->mbox + SEP_MBOX_SND_STS) & SEP_MBOX_STS_EMPTY);
	mutex_lock(&sep->lock);
	if (time_before(jiffies, sep->gt_quiet_until))
		quiet_ms = (s64)jiffies_to_msecs(sep->gt_quiet_until - jiffies);
	dead = sep->gt_dead;
	active = sep->gt_active;
	ret = sep_gt_gate(dead, active, fifo_empty, quiet_ms);
	mutex_unlock(&sep->lock);
	if (ret) {
		dev_warn(sep->dev,
			 "gt: ep %#04x cmd %#x refused: %d (dead=%d active=%d fifo_empty=%d quiet %lld ms)\n",
			 ep, cmd, ret, dead, active, fifo_empty, quiet_ms);
		return ret;
	}

	ret = sep_gt_xact_init(x, cmd, in_len, in, out, out_cap);
	if (ret)
		return ret;
	mutex_lock(&sep->lock);
	reinit_completion(&sep->gt_ev);
	sep->gt_ep = ep;
	sep->gt_active = true;
	mutex_unlock(&sep->lock);

	deadline = ktime_add_ms(ktime_get(), SEP_GT_TIMEOUT_MS);
	dev_info(sep->dev,
		 "gt: ep %#04x cmd %#x request %zu bytes, OOL send %#zx recv %#zx, one deadline of %d ms\n",
		 ep, cmd, in_len, e->send_len, e->recv_len, SEP_GT_TIMEOUT_MS);

	ret = sep_gt_send_chunk(sep, e);
	if (ret)
		goto out;

	for (;;) {
		bool stop, credit, more;

		ret = sep_gt_wait(sep, deadline);
		if (ret)
			break;

		/*
		 * Read and clear the event under sep->lock and re-arm the
		 * completion before dropping it: RX runs under the same lock, so
		 * no word that arrives between these two steps can be lost.
		 */
		mutex_lock(&sep->lock);
		stop = x->err || x->status || x->done;
		if (x->err)
			ret = x->err;
		else if (x->status)
			ret = -EIO;
		else
			ret = 0;
		credit = !stop && x->credit;
		more = !stop && !credit && x->want_more;
		x->credit = false;
		x->want_more = false;
		reinit_completion(&sep->gt_ev);
		mutex_unlock(&sep->lock);

		if (stop)
			break;
		if (credit)
			ret = sep_gt_send_chunk(sep, e);	/* next 0xfd */
		else if (more)
			ret = sep_gt_send_credit(sep);		/* 0xfe */
		else
			continue;				/* spurious wake */
		if (ret)
			break;
	}

out:
	/*
	 * Detach atomically: from here on no RX path can reach this transaction,
	 * so nothing that is still on the wire can complete it, and nothing that
	 * arrives later can be classified against the next one. Counters and the
	 * quarantine deadline are updated in the same sep->lock section, because
	 * sep_gt_stale() (RX) restarts the same deadline and the readers
	 * (status, xfer) take the same lock.
	 *
	 * `proto` is the one classification of a received-frame failure: a frame
	 * rejected by the core for its shape, its size or the state it implies is
	 * quarantined exactly like -EPROTO, so an -EMSGSIZE can no longer skip
	 * the quiet window just because its local errno is not -EPROTO.
	 */
	proto = sep_gt_is_protocol_error(ret) ||
		sep_gt_is_protocol_error(x->err);

	mutex_lock(&sep->lock);
	sep->gt_active = false;
	sep->gt_ep = 0xff;
	if (ret == -ETIMEDOUT) {
		sep->gt_quiet_until = jiffies +
				      msecs_to_jiffies(SEP_GT_QUIET_MS);
		if (++sep->gt_tmo >= 2)
			sep->gt_dead = true;
	} else if (proto) {
		sep->gt_quiet_until = jiffies +
				      msecs_to_jiffies(SEP_GT_QUIET_MS);
		if (++sep->gt_proto >= 3)
			sep->gt_dead = true;
	}
	if (!ret) {
		sep->gt_tmo = 0;
		sep->gt_proto = 0;
		sep->gt_ok++;
	}
	poisoned = sep->gt_dead;
	mutex_unlock(&sep->lock);

	if (!ret) {
		*replylen = x->out_done;
	} else {
		*status = x->status;	/* raw; 0x101 is never special-cased */
		if (proto)
			dev_warn(sep->dev,
				 "gt: ep %#04x cmd %#x: abandoned after a frame failure (%d), quarantine %d ms%s\n",
				 ep, cmd, x->err ? x->err : ret, SEP_GT_QUIET_MS,
				 poisoned ? ", mailbox poisoned" : "");
	}

	/* Swallow the words of the abandoned transaction before anything may be
	 * queued behind them, then wipe both buffers. A word that shows up here
	 * is a stale GT word, and sep_gt_stale() pushes the quiet deadline out
	 * again, so the window is 100 ms after the last one and not 100 ms after
	 * the failure.
	 */
	sep_drain(sep);
	sep_gt_wipe(e);
	return ret;
}

static ssize_t sep_dbg_status_read(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	unsigned int i, complete = 0, published = 0;
	u32 gt_ok, gt_stale;
	bool gt_dead;
	int sbio_ep, sbio_ready;
	char *buf;
	ssize_t ret;

	buf = kzalloc(1024, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&sep->lock);
	for (i = 0; i < SEP_EP_COUNT; i++) {
		complete += sep_ep_complete(&sep->eps[i]);
		published += sep->eps[i].published;
	}
	sbio_ep = sep->sbio_ep;
	sbio_ready = sep->sbio_ready;
	gt_ok = sep->gt_ok;
	gt_stale = sep->gt_stale;
	gt_dead = sep->gt_dead;
	mutex_unlock(&sep->lock);

	if (sep->running) {
		u32 rsts = readl(sep->mbox + SEP_MBOX_RECV_STS);
		u32 ssts = readl(sep->mbox + SEP_MBOX_SND_STS);

		ret = scnprintf(buf, 1024,
				"base=%px mbox=%px running=1\n"
				"recv_sts=%#x (%s) snd_sts=%#x (%s) en_int=%#x\n",
				sep->base, sep->mbox,
				rsts, rsts & SEP_MBOX_STS_EMPTY ? "empty" : "pending",
				ssts, sep_fifo_state(ssts),
				readl(sep->mbox + SEP_MBOX_EN_INT));
	} else {
		ret = scnprintf(buf, 1024,
				"base=%px mbox=%px running=0 (mailbox untouched)\n",
				sep->base, sep->mbox);
	}
	ret += scnprintf(buf + ret, 1024 - ret,
			 "tx=%llu rx=%llu tx_busy=%llu dropped=%llu log=%u\n"
			 "rx_idle_ms=%u endpoints_complete=%u published=%u\n"
			 "sbio_ep=%d sbio_ready=%d gt_ok=%u gt_stale=%u gt_dead=%d\n",
			 sep->tx_count, sep->rx_count, sep->tx_busy, sep->dropped,
			 sep->log_count,
			 sep->rx_count ? jiffies_to_msecs(jiffies - sep->last_rx) : 0,
			 complete, published, sbio_ep, sbio_ready, gt_ok,
			 gt_stale, gt_dead);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, ret);
	kfree(buf);
	return ret;
}

static const struct file_operations sep_dbg_status_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sep_dbg_status_read,
};

static ssize_t sep_dbg_log_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	size_t size = SEP_LOG_ENTRIES * 112;
	unsigned int i;
	char *buf;
	ssize_t len = 0;

	buf = kvzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&sep->lock);
	for (i = 0; i < sep->log_count; i++) {
		u64 msg = sep->log[(sep->log_head + i) % SEP_LOG_ENTRIES];
		u32 data = SEP_MSG_GET_DATA(msg);
		u8 a, b, c, d;
		char name[5];

		len += scnprintf(buf + len, size - len,
				 "ep=%02x tag=%02x op=%02x param=%02x data=%08x",
				 SEP_MSG_GET_EP(msg), SEP_MSG_GET_TAG(msg),
				 SEP_MSG_GET_OP(msg), SEP_MSG_GET_PARAM(msg),
				 data);
		if (SEP_MSG_GET_EP(msg) == SEP_EP_DISCOVERY &&
		    SEP_MSG_GET_OP(msg) == SEP_DISC_NAME &&
		    sep_fourcc(data, name)) {
			len += scnprintf(buf + len, size - len,
					 "  [ep %#04x '%s']",
					 SEP_MSG_GET_PARAM(msg), name);
		} else if (SEP_MSG_GET_EP(msg) == SEP_EP_DISCOVERY &&
			   SEP_MSG_GET_OP(msg) == SEP_DISC_OOL) {
			sep_ool_word(data, &a, &b, &c, &d);
			len += scnprintf(buf + len, size - len,
					 "  [ep %#04x OOL in %u-%u out %u-%u pages]",
					 SEP_MSG_GET_PARAM(msg), a, b, c, d);
		}
		len += scnprintf(buf + len, size - len, "\n");
	}
	mutex_unlock(&sep->lock);

	len = simple_read_from_buffer(ubuf, count, ppos, buf, len);
	kvfree(buf);
	return len;
}

static const struct file_operations sep_dbg_log_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sep_dbg_log_read,
};

static const char *sep_ep_source(const struct sep_ep *e)
{
	if (e->name_manual && e->ool_manual)
		return "manual";
	if (e->name_manual || e->ool_manual)
		return "mixed";
	return "mailbox";
}

static const char *sep_ep_state(const struct sep_ep *e)
{
	if (!e->has_ool)
		return "incomplete: no OOL message";
	if (!e->has_name)
		return "incomplete: no name message";
	if (e->published)
		return "published";
	if (!e->in_max && !e->out_max)
		return "complete, no OOL";
	return "complete";
}

/* Endpoint table, one line per endpoint the driver knows anything about. */
static ssize_t sep_dbg_endpoints_read(struct file *file, char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	size_t size = SEP_EP_COUNT * 192;
	unsigned int i;
	char *buf;
	ssize_t len = 0;

	buf = kvzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len += scnprintf(buf + len, size - len,
			 "# id   name  src     in_pages out_pages state\n");
	mutex_lock(&sep->lock);
	for (i = 0; i < SEP_EP_COUNT; i++) {
		struct sep_ep *e = &sep->eps[i];
		char name[5] = "-";
		char in[8] = "-", out[8] = "-";

		if (!e->has_name && !e->has_ool)
			continue;
		if (e->has_name && !sep_fourcc(e->name, name))
			snprintf(name, sizeof(name), "????");
		if (e->has_ool) {
			snprintf(in, sizeof(in), "%u-%u", e->in_min, e->in_max);
			snprintf(out, sizeof(out), "%u-%u", e->out_min, e->out_max);
		}
		len += scnprintf(buf + len, size - len,
				 "0x%02x %-5s %-7s %-8s %-9s %s",
				 i, name, sep_ep_source(e), in, out,
				 sep_ep_state(e));
		if (e->conflict)
			len += scnprintf(buf + len, size - len, " CONFLICT");
		if (sep_ep_complete(e) && !e->published && (e->in_max || e->out_max))
			len += scnprintf(buf + len, size - len,
					 " max_send=%#x max_recv=%#x",
					 e->in_max * SEP_OOL_PAGE,
					 e->out_max * SEP_OOL_PAGE);
		if (e->published)
			len += scnprintf(buf + len, size - len,
					 " send=%pa/%#zx recv=%pa/%#zx",
					 &e->send_pa, e->send_len,
					 &e->recv_pa, e->recv_len);
		len += scnprintf(buf + len, size - len, "\n");
	}
	mutex_unlock(&sep->lock);

	len = simple_read_from_buffer(ubuf, count, ppos, buf, len);
	kvfree(buf);
	return len;
}

static const struct file_operations sep_dbg_endpoints_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sep_dbg_endpoints_read,
};

/*
 * Declare an advertisement by hand, from a pongoOS log of the 0xfd messages
 * (all numbers hex):
 *
 *	echo "ID NAME IN_MIN IN_MAX OUT_MIN OUT_MAX" > advertise
 *	echo "raw ID NAME_WORD OOL_WORD" > advertise
 *
 * e.g. "10 xars 8 8 8 8" or, copying the two data words of
 * "ep=fd op=0 param=10 data=78617273" / "ep=fd op=1 param=10 data=8080808",
 * "raw 10 78617273 8080808". This only fills the table; nothing is sent.
 * A value the mailbox already delivered can't be overridden.
 */
static ssize_t sep_dbg_advertise_write(struct file *file,
				       const char __user *ubuf, size_t count,
				       loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	unsigned int id, word, ool, v[4];
	u8 a, b, c, d;
	char buf[96], str[8];
	struct sep_ep *e;
	int ret = 0, i;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sscanf(buf, "raw %x %x %x", &id, &word, &ool) == 3) {
		if (!sep_fourcc(word, str))
			return -EINVAL;
	} else if (sscanf(buf, "%x %7s %x %x %x %x", &id, str, &v[0], &v[1],
			  &v[2], &v[3]) == 6) {
		if (strlen(str) != 4)
			return -EINVAL;
		word = 0;
		for (i = 0; i < 4; i++) {
			if (str[i] < 0x20 || str[i] > 0x7e || v[i] > 0xff)
				return -EINVAL;
			word = word << 8 | (u8)str[i];
		}
		ool = v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24;
	} else {
		return -EINVAL;
	}
	if (id > SEP_EP_CLIENT_MAX)
		return -EINVAL;
	sep_ool_word(ool, &a, &b, &c, &d);

	mutex_lock(&sep->lock);
	e = &sep->eps[id];
	if (e->published) {
		ret = -EBUSY;
	} else if ((e->has_name && !e->name_manual && e->name != word) ||
		   (e->has_ool && !e->ool_manual &&
		    (e->in_min != a || e->in_max != b ||
		     e->out_min != c || e->out_max != d))) {
		ret = -EEXIST;	/* disagrees with what the mailbox delivered */
	} else {
		if (!e->has_name || e->name_manual) {
			e->name = word;
			e->has_name = true;
			e->name_manual = true;
		}
		if (!e->has_ool || e->ool_manual) {
			e->in_min = a;
			e->in_max = b;
			e->out_min = c;
			e->out_max = d;
			e->has_ool = true;
			e->ool_manual = true;
		}
	}
	mutex_unlock(&sep->lock);

	if (ret)
		return ret;
	dev_info(sep->dev, "ep %#04x: declared '%s' OOL in [%u,%u] out [%u,%u] pages\n",
		 id, str, a, b, c, d);
	return count;
}

static const struct file_operations sep_dbg_advertise_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sep_dbg_advertise_write,
};

/*
 * Raw bring-up interface:
 *	echo "ep tag op param data" > send	(all hex, one message)
 *	send a message to an endpoint without any framing assumptions
 * After a successful write the AP->SEP FIFO is watched for up to 1 s and the
 * result logged (dmesg, debugfs fifo); the write itself still succeeds, since
 * the word is queued either way.
 */
static ssize_t sep_dbg_send_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	unsigned int ep, tag, op, param, data;
	char buf[64];
	u32 before;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sscanf(buf, "%x %x %x %x %x", &ep, &tag, &op, &param, &data) != 5)
		return -EINVAL;

	mutex_lock(&sep->cfg_lock);
	if (!sep->running) {
		ret = -ENXIO;
	} else {
		before = readl(sep->mbox + SEP_MBOX_SND_STS);
		ret = sep_send_raw(sep, ep, tag, op, param, data);
		if (!ret)
			sep_tx_drain(sep, before, "send ep %#04x tag %#04x",
				     ep & 0xff, tag & 0xff);
	}
	mutex_unlock(&sep->cfg_lock);
	return ret ? ret : count;
}

static const struct file_operations sep_dbg_send_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sep_dbg_send_write,
};

/*
 * OOL buffer bring-up:
 *	echo "EP SEND_SIZE RECV_SIZE" > ool	(all hex, e.g. "10 8000 8000")
 * The endpoint needs a complete advertisement and each size must be a 4 KiB
 * multiple inside its advertised range ("in" = send, "out" = receive). The
 * buffers are published through 'cntl' and kept for the lifetime of the
 * driver; sepOS accepts one binding per endpoint. The write fails with
 * -ETIMEDOUT if the four control words are not consumed within 1 s.
 */
static ssize_t sep_dbg_ool_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	unsigned int ep, send_len, recv_len;
	char buf[64];
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sscanf(buf, "%x %x %x", &ep, &send_len, &recv_len) != 3 ||
	    ep > SEP_EP_CLIENT_MAX)
		return -EINVAL;

	mutex_lock(&sep->cfg_lock);
	ret = sep->running ? sep_ep_alloc(sep, ep, send_len, recv_len) : -ENXIO;
	mutex_unlock(&sep->cfg_lock);
	return ret ? ret : count;
}

static const struct file_operations sep_dbg_ool_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sep_dbg_ool_write,
};

static int sep_find_named_ep(struct sep_a9 *sep, u32 name)
{
	unsigned int i;
	int found = -1;

	mutex_lock(&sep->lock);
	for (i = 0; i <= SEP_EP_CLIENT_MAX; i++) {
		if (!sep_ep_complete(&sep->eps[i]) || sep->eps[i].name != name)
			continue;
		if (found >= 0) {
			found = -EEXIST;
			break;
		}
		found = i;
	}
	mutex_unlock(&sep->lock);
	return found;
}

/*
 * Binary write of the IM4M DER into the already-published arts AP->SEP OOL
 * buffer.  It only stages bytes; arts-start is the separate commit point.
 */
static ssize_t sep_dbg_arts_manifest_write(struct file *file,
					   const char __user *ubuf, size_t count,
					   loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	struct sep_ep *e;
	loff_t off = *ppos;
	int ep, ret = 0;

	if (!count)
		return 0;

	mutex_lock(&sep->cfg_lock);
	if (!sep->running) {
		ret = -ENXIO;
		goto out;
	}
	if (sep->arts_ran) {
		ret = -EALREADY;
		goto out;
	}
	ep = sep_find_named_ep(sep, SEP_NAME_ARTS);
	if (ep < 0) {
		ret = ep == -1 ? -ENOENT : ep;
		goto out;
	}
	e = &sep->eps[ep];
	if (!e->published || !e->send || e->send_len != SEP_ARTS_OOL_SIZE) {
		ret = -ENODEV;
		goto out;
	}
	if (off < 0 || off != sep->arts_manifest_len ||
	    count > e->send_len - off) {
		ret = -EINVAL;
		goto out;
	}
	if (copy_from_user(e->send + off, ubuf, count)) {
		ret = -EFAULT;
		goto out;
	}
	dcache_clean_poc((unsigned long)e->send + off,
			 (unsigned long)e->send + off + count);
	sep->arts_manifest_len += count;
	*ppos += count;
	dev_info(sep->dev, "arts ep %#04x: staged IM4M bytes %#zx/%#zx\n",
		 ep, sep->arts_manifest_len, e->send_len);
out:
	mutex_unlock(&sep->cfg_lock);
	return ret ? ret : count;
}

static const struct file_operations sep_dbg_arts_manifest_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sep_dbg_arts_manifest_write,
};

static int sep_arts_manifest_start(struct sep_a9 *sep)
{
	struct sep_ep *e;
	unsigned long left;
	u16 der_content;
	u32 before;
	int ep, ret;

	if (!sep->running)
		return -ENXIO;
	if (sep->arts_ran)
		return -EALREADY;
	ep = sep_find_named_ep(sep, SEP_NAME_ARTS);
	if (ep < 0)
		return ep == -1 ? -ENOENT : ep;
	e = &sep->eps[ep];
	if (!e->published || !e->send || e->send_len != SEP_ARTS_OOL_SIZE)
		return -ENODEV;
	if (sep->arts_manifest_len < SEP_ARTS_DER_MIN)
		return -ENODATA;
	if (((u8 *)e->send)[0] != 0x30 || ((u8 *)e->send)[1] != 0x82 ||
	    memcmp(e->send + 4, "\x16\x04IM4M", 6))
		return -EBADMSG;
	der_content = get_unaligned_be16(e->send + 2);
	if ((size_t)der_content + 4 != sep->arts_manifest_len)
		return -EMSGSIZE;

	before = readl(sep->mbox + SEP_MBOX_SND_STS);
	if (!(before & SEP_MBOX_STS_EMPTY))
		return -EBUSY;

	/* This request is one-shot even on timeout: never resend in this boot. */
	sep->arts_ran = true;
	sep->arts_ep = ep;
	sep->arts_reply_tag = 0;
	sep->arts_reply_op = 0;
	sep->arts_reply_param = 0xff;
	sep->arts_reply_data = 0;
	sep->arts_reply_raw = 0;
	sep->arts_reply_us = 0;
	sep->arts_ret = 0;
	sep->arts_result = SEP_ARTS_WAITING;
	reinit_completion(&sep->arts_done);
	mutex_lock(&sep->lock);
	sep->arts_waiting = true;
	mutex_unlock(&sep->lock);
	dcache_clean_poc((unsigned long)e->send,
			 (unsigned long)e->send + sep->arts_manifest_len);

	dev_info(sep->dev, "arts ep %#04x: sending MANIFEST tag=%#x op=%#x, IM4M %#zx bytes\n",
		 ep, SEP_ARTS_TAG, SEP_ARTS_OP_MANIFEST,
		 sep->arts_manifest_len);
	sep->arts_t0 = ktime_get();
	ret = sep_send_raw(sep, ep, SEP_ARTS_TAG, SEP_ARTS_OP_MANIFEST, 0, 0);
	if (!ret)
		ret = sep_tx_drain(sep, before, "arts MANIFEST");
	if (ret)
		goto cancel;

	left = wait_for_completion_timeout(&sep->arts_done,
					   msecs_to_jiffies(SEP_ARTS_TIMEOUT_MS));
	if (!left) {
		ret = -ETIMEDOUT;
		sep->arts_result = SEP_ARTS_TIMEOUT;
		goto cancel;
	}

	ret = sep->arts_reply_param ? -EIO : 0;
	sep->arts_ret = ret;
	dev_info(sep->dev, "arts ep %#04x: MANIFEST reply raw=%016llx param=%#x data=%#x after %lld us: %s\n",
		 ep, sep->arts_reply_raw, sep->arts_reply_param,
		 sep->arts_reply_data, sep->arts_reply_us,
		 ret ? "SEP rejected" : "success");
	return ret;

cancel:
	mutex_lock(&sep->lock);
	sep->arts_waiting = false;
	mutex_unlock(&sep->lock);
	sep->arts_ret = ret;
	dev_warn(sep->dev, "arts ep %#04x: MANIFEST stopped: %d (no retry in this boot)\n",
		 ep, ret);
	return ret;
}

static ssize_t sep_dbg_arts_start_write(struct file *file,
					const char __user *ubuf, size_t count,
					loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	char buf[8];
	unsigned int on;
	int ret;

	if (count >= sizeof(buf) || copy_from_user(buf, ubuf, count))
		return -EINVAL;
	buf[count] = '\0';
	if (kstrtouint(strim(buf), 0, &on) || on != 1)
		return -EINVAL;
	mutex_lock(&sep->cfg_lock);
	ret = sep_arts_manifest_start(sep);
	mutex_unlock(&sep->cfg_lock);
	return ret ? ret : count;
}

static ssize_t sep_dbg_arts_start_read(struct file *file, char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	char buf[320];
	ssize_t len;

	mutex_lock(&sep->cfg_lock);
	len = scnprintf(buf, sizeof(buf),
			"ran=%d result=%u ret=%d ep=%d manifest_len=%zu\n"
			"reply tag=%02x op=%02x param=%02x data=%08x raw=%016llx after_us=%lld\n",
			sep->arts_ran, sep->arts_result, sep->arts_ret,
			sep->arts_ep, sep->arts_manifest_len,
			sep->arts_reply_tag, sep->arts_reply_op,
			sep->arts_reply_param, sep->arts_reply_data,
			sep->arts_reply_raw, sep->arts_reply_us);
	mutex_unlock(&sep->cfg_lock);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations sep_dbg_arts_start_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sep_dbg_arts_start_read,
	.write = sep_dbg_arts_start_write,
};

static int sep_pwrstate_read(u32 *val);

/*
 * cat fifo: both FIFO status words decoded, the four mask views, the current
 * PMGR word, the last wrapper activation (PMGR word, registers before and
 * after), the last AP->SEP drain check, the read-only probe/arm snapshots and
 * the wake-test verdict. Unarmed, no SEP register is read: only the PMGR word
 * and the recorded values are shown.
 */
static ssize_t sep_dbg_fifo_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	const size_t size = 2048;
	u32 v[SEP_WRAP_NREGS], ps;
	ssize_t len = 0;
	char *buf;
	int ret;

	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&sep->cfg_lock);
	if (sep->running) {
		sep_wrap_snapshot(sep, v);
		len += scnprintf(buf + len, size - len,
				 "running=1\nap->sep snd_sts=%#x (%s)\nsep->ap recv_sts=%#x (%s)\nnow   ",
				 v[4], sep_fifo_state(v[4]),
				 v[5], sep_fifo_state(v[5]));
		len += sep_wrap_print(buf + len, size - len, v);
		len += scnprintf(buf + len, size - len, "\n");
	} else {
		len += scnprintf(buf + len, size - len,
				 "running=0 (mailbox untouched)\n");
	}

	ret = sep_pwrstate_read(&ps);
	if (ret)
		len += scnprintf(buf + len, size - len,
				 "pmgr now: read error %d\n", ret);
	else
		len += scnprintf(buf + len, size - len,
				 "pmgr now=%08x (%s %08x)\n", ps,
				 ps == SEP_PS_HANDOFF ? "handoff" :
				 (ps & ~SEP_PS_AUTO_ENABLE) == SEP_PS_HANDOFF ?
				 "handoff+AUTO" : "NOT handoff",
				 SEP_PS_HANDOFF);

	if (sep->armed_once) {
		len += scnprintf(buf + len, size - len,
				 "arm pwrstate=%08x actual=%lx target=%lx\nbefore ",
				 sep->pwrstate,
				 FIELD_GET(SEP_PS_ACTUAL, sep->pwrstate),
				 FIELD_GET(SEP_PS_TARGET, sep->pwrstate));
		len += sep_wrap_print(buf + len, size - len, sep->wrap_before);
		len += scnprintf(buf + len, size - len,
				 "\nrmw4020 read=%08x wrote=%08x\nafter  ",
				 sep->outbox_read,
				 sep->outbox_read | SEP_WRAP_OUTBOX_EN);
		len += sep_wrap_print(buf + len, size - len, sep->wrap_after);
		len += scnprintf(buf + len, size - len, "\n");
	} else {
		len += scnprintf(buf + len, size - len, "arm: never\n");
	}

	if (sep->drain_what[0])
		len += scnprintf(buf + len, size - len,
				 "drain %s: %s after %lld us, snd_sts %#x -> %#x\n",
				 sep->drain_what,
				 sep->drain_ret ? "FAIL (not drained)" : "pass",
				 sep->drain_us, sep->drain_sts_before,
				 sep->drain_sts_after);
	else
		len += scnprintf(buf + len, size - len, "drain: none yet\n");

	len += scnprintf(buf + len, size - len, "probe      ");
	len += sep_core_print(buf + len, size - len, &sep->probe_snap);
	len += scnprintf(buf + len, size - len, "\narm before ");
	len += sep_core_print(buf + len, size - len, &sep->arm_snap);
	len += scnprintf(buf + len, size - len, "\narm after  ");
	len += sep_core_print(buf + len, size - len, &sep->arm_after_snap);
	len += scnprintf(buf + len, size - len, "\nwake-test: %s\n",
			 sep_wake_result(sep));
	mutex_unlock(&sep->cfg_lock);

	len = simple_read_from_buffer(ubuf, count, ppos, buf, len);
	kfree(buf);
	return len;
}

static const struct file_operations sep_dbg_fifo_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sep_dbg_fifo_read,
};


/*
 * /dev/sep-bio: the ABI the Aurora Silicon libfprint driver speaks
 * (include/uapi/linux/apple-sep.h, interface version 4). Keeping the same
 * interface means their libfprint `aurora` driver, fprintd and Omarchy's
 * fingerprint UI work unchanged later. Until the SBIO state machine and the
 * real EnrollMode/MatchMode payloads exist, this interface never sends a
 * message: every SEP command goes through debugfs.
 */
static struct sep_a9 *sep_a9_instance;

static int sep_bio_open(struct inode *inode, struct file *file)
{
	file->private_data = sep_a9_instance;
	return 0;
}

static long sep_bio_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct sep_a9 *sep = file->private_data;
	struct sep_bio_info info = {
		.version = SEP_BIO_IFACE_VERSION,
		.sensor_present = 0,
		.enrolled = 0,
		.capacity = 0,
		.enroll_stages = 0,
	};
	struct sep_bio_list *list;

	if (!sep)
		return -ENODEV;

	switch (cmd) {
	case SEP_BIO_GET_INFO:
		/*
		 * sensor_present stays 0 until SBIO has acknowledged 0x73: the
		 * sensor only exists once sepOS has brought it up, and libfprint
		 * correctly declines to expose a reader without one.
		 */
		info.sensor_present = sep->sbio_ready;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	case SEP_BIO_LIST:
		/* too large for the stack, and empty until SBIO is implemented */
		list = kzalloc(sizeof(*list), GFP_KERNEL);
		if (!list)
			return -ENOMEM;
		if (copy_to_user((void __user *)arg, list, sizeof(*list))) {
			kfree(list);
			return -EFAULT;
		}
		kfree(list);
		return 0;
	case SEP_BIO_ENROL_POLL: {
		struct sep_bio_enrol_poll poll = {
			.state = SEP_BIO_STATE_IDLE,
		};

		if (copy_to_user((void __user *)arg, &poll, sizeof(poll)))
			return -EFAULT;
		return 0;
	}
	case SEP_BIO_VERIFY_POLL: {
		struct sep_bio_verify_poll poll = {
			.state = SEP_BIO_STATE_IDLE,
			.result = SEP_BIO_NOT_COMPARED,
		};

		if (copy_to_user((void __user *)arg, &poll, sizeof(poll)))
			return -EFAULT;
		return 0;
	}
	case SEP_BIO_ENROL_START:
	case SEP_BIO_VERIFY_START:
		/*
		 * EnrollMode (0x03) and MatchMode (0x25) need the real 68-byte
		 * payloads built by biometrickitd, plus sensor frames; zeroed
		 * payloads prove nothing (docs/sep/linux-bringup-next.md §4).
		 */
	case SEP_BIO_CANCEL:
	case SEP_BIO_DELETE:
	case SEP_BIO_DELETE_ALL:
	case SEP_BIO_ATTEST:
		return -ENOSYS;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations sep_bio_fops = {
	.owner = THIS_MODULE,
	.open = sep_bio_open,
	.unlocked_ioctl = sep_bio_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice sep_bio_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "sep-bio",
	.mode = 0600,
	.fops = &sep_bio_fops,
};


/*
 * Read, never write, the SEP's PMGR power-state word: the apple,pmgr child
 * labelled "sep" (ps_sep, apple,always-on), through the syscon regmap the
 * pmgr-pwrstate driver uses. This is the hardware state behind the "sep"
 * line of pm_genpd_summary.
 */
static int sep_pwrstate_read(u32 *val)
{
	struct device_node *pmgr, *np = NULL, *child;
	struct regmap *map;
	const char *label;
	u32 off;
	int ret;

	for_each_compatible_node(pmgr, NULL, "apple,pmgr") {
		for_each_child_of_node(pmgr, child) {
			if (of_device_is_compatible(child, "apple,pmgr-pwrstate") &&
			    !of_property_read_string(child, "label", &label) &&
			    !strcmp(label, "sep")) {
				np = child;
				break;
			}
		}
		if (np)
			break;
	}
	if (!np)
		return -ENODEV;

	ret = of_property_read_u32(np, "reg", &off);
	if (!ret) {
		map = syscon_node_to_regmap(pmgr);
		ret = IS_ERR(map) ? PTR_ERR(map) : regmap_read(map, off, val);
	}
	of_node_put(np);
	of_node_put(pmgr);
	return ret;
}

/*
 * Gate for arm and wake-test on a PMGR word just read: refuse (-EIO, nothing
 * touched) unless the domain is on and out of reset and the word equals the
 * handoff value apart from AUTO_ENABLE; AUTO_ENABLE alone is a warning and is
 * never repaired.
 */
static int sep_pwrstate_gate(struct sep_a9 *sep, u32 ps, const char *what)
{
	if (FIELD_GET(SEP_PS_ACTUAL, ps) != SEP_PS_ACTIVE || (ps & SEP_PS_RESET)) {
		dev_err(sep->dev, "%s refused: sep power domain not on (pwrstate=%08x actual=%lx target=%lx%s), SEP untouched\n",
			what, ps, FIELD_GET(SEP_PS_ACTUAL, ps),
			FIELD_GET(SEP_PS_TARGET, ps),
			ps & SEP_PS_RESET ? " reset" : "");
		return -EIO;
	}
	if ((ps & ~SEP_PS_AUTO_ENABLE) != SEP_PS_HANDOFF) {
		dev_err(sep->dev, "%s refused: pwrstate=%08x differs from the pongoOS handoff word %08x beyond AUTO_ENABLE, SEP untouched\n",
			what, ps, SEP_PS_HANDOFF);
		return -EIO;
	}
	if (ps & SEP_PS_AUTO_ENABLE)
		dev_warn(sep->dev, "%s: PMGR sep AUTO_ENABLE (bit 28) set (pwrstate=%08x), not repaired, continuing\n",
			 what, ps);
	return 0;
}

/*
 * Read-only snapshot, the counterpart of pongoOS "peek": the PMGR word, then
 * SEP +0x28, +0x44, +0xb88, +0x4008 and +0x4020, but only if the PMGR word
 * says actual on and no reset (a read of the powered-down block can stall).
 */
static void sep_core_snapshot(struct sep_a9 *sep, struct sep_core_snap *s)
{
	unsigned int i;

	memset(s, 0, sizeof(*s));
	s->taken = true;
	s->pmgr_ret = sep_pwrstate_read(&s->pmgr);
	if (s->pmgr_ret || FIELD_GET(SEP_PS_ACTUAL, s->pmgr) != SEP_PS_ACTIVE ||
	    (s->pmgr & SEP_PS_RESET))
		return;
	for (i = 0; i < SEP_CORE_NREGS; i++)
		s->reg[i] = readl(sep->base + sep_core_regs[i]);
	s->valid = true;
}

/*
 * Arm: AppleA7IOPV2 wrapper activation as AppleA7IOP::start performs it after
 * setActive(true) (docs/sep/xart-handshake.md section 6). Mask the four known
 * sources in both views, unmask bit 0 (inbox) then bit 12 (outbox) in both
 * views, set +0x4020 bit 0 by read-modify-write, read back to flush. The old
 * arm cleared only bit 12 in the primary view: a sepOS waiting in WFI then
 * never saw the AP->SEP words (14:00 run, five words left queued for 40 s).
 *
 * setActive(true) has no SEP-local MMIO equivalent, so the PMGR word must be
 * the pongoOS handoff value 0x4f0000ff (sep_pwrstate_gate): on and out of
 * reset. AUTO_ENABLE (bit 28) only draws a warning and is not repaired; any
 * other difference is refused and nothing is touched. No PMGR, CPU RUN
 * (+0x44), NMI, reset or auto-boot write; +0x4008 is never written and +0x4020
 * only gains bit 0. The core registers are snapshotted before and after so
 * that can be checked. Called with cfg_lock held.
 */
static int sep_arm(struct sep_a9 *sep)
{
	struct sep_core_snap *pre = &sep->arm_snap, *post = &sep->arm_after_snap;
	void __iomem *b = sep->base;
	u32 ps, v;
	char line[192];
	int ret;

	ret = sep_pwrstate_read(&ps);
	if (ret) {
		dev_err(sep->dev, "arm refused: no PMGR sep power state (%d), SEP untouched\n",
			ret);
		return ret;
	}
	ret = sep_pwrstate_gate(sep, ps, "arm");
	if (ret)
		return ret;
	sep->pwrstate = ps;
	dev_info(sep->dev, "arm: sep power domain on (pwrstate=%08x)\n", ps);

	sep_core_snapshot(sep, pre);
	sep_core_print(line, sizeof(line), pre);
	dev_info(sep->dev, "arm: core before %s\n", line);

	sep_wrap_snapshot(sep, sep->wrap_before);
	sep_wrap_print(line, sizeof(line), sep->wrap_before);
	dev_info(sep->dev, "arm: before %s\n", line);

	writel(SEP_WRAP_MASK_ALL, b + SEP_WRAP_MASK_SET);
	writel(SEP_WRAP_MASK_ALL, b + SEP_WRAP_MIRROR_SET);
	writel(SEP_WRAP_INBOX_IRQ, b + SEP_WRAP_MASK_CLR);
	writel(SEP_WRAP_INBOX_IRQ, b + SEP_WRAP_MIRROR_CLR);
	writel(SEP_WRAP_OUTBOX_IRQ, b + SEP_WRAP_MASK_CLR);
	writel(SEP_WRAP_OUTBOX_IRQ, b + SEP_WRAP_MIRROR_CLR);
	v = readl(b + SEP_WRAP_OUTBOX_CTL);
	writel(v | SEP_WRAP_OUTBOX_EN, b + SEP_WRAP_OUTBOX_CTL);
	/* flush the posted writes before any mailbox word can follow */
	readl(b + SEP_WRAP_OUTBOX_CTL);
	mb();

	sep->outbox_read = v;
	sep_wrap_snapshot(sep, sep->wrap_after);
	sep->armed_once = true;
	dev_info(sep->dev, "arm: wrote 4000=%x c00=%x 4004=%x c04=%x 4004=%x c04=%x 4020=%08x (read %08x)\n",
		 SEP_WRAP_MASK_ALL, SEP_WRAP_MASK_ALL, SEP_WRAP_INBOX_IRQ,
		 SEP_WRAP_INBOX_IRQ, SEP_WRAP_OUTBOX_IRQ, SEP_WRAP_OUTBOX_IRQ,
		 v | SEP_WRAP_OUTBOX_EN, v);
	sep_wrap_print(line, sizeof(line), sep->wrap_after);
	dev_info(sep->dev, "arm: after %s (AP->SEP FIFO %s)\n", line,
		 sep_fifo_state(sep->wrap_after[4]));

	sep_core_snapshot(sep, post);
	sep_core_print(line, sizeof(line), post);
	dev_info(sep->dev, "arm: core after %s\n", line);
	if (post->pmgr_ret || post->pmgr != ps)
		dev_warn(sep->dev, "arm: PMGR sep word changed during arm: %08x -> %08x\n",
			 ps, post->pmgr);
	if (pre->valid && post->valid) {
		if (pre->reg[1] != post->reg[1])
			dev_warn(sep->dev, "arm: CPU control +0x44 changed %08x -> %08x\n",
				 pre->reg[1], post->reg[1]);
		if ((pre->reg[3] ^ post->reg[3]) & BIT(0))
			dev_warn(sep->dev, "arm: +0x4008 bit 0 changed %08x -> %08x\n",
				 pre->reg[3], post->reg[3]);
		if (!(post->reg[4] & SEP_WRAP_OUTBOX_EN))
			dev_warn(sep->dev, "arm: +0x4020 bit 0 clear after arm (%08x)\n",
				 post->reg[4]);
	}
	return 0;
}

/*
 * Arming control. Nothing writes the SEP registers until the user writes 1
 * here, and before that they are only read once at probe, and only while the
 * PMGR word says the domain is on: the first read of a powered-down SEP block
 * can stall the AP, and on a checkm8 boot sepOS is usually not running. Arm
 * only after the pongoOS/m1n1 side has confirmed the SEP is up. Arming
 * activates the wrapper (sep_arm), then only drains and records; it sends
 * nothing.
 */
static ssize_t sep_dbg_start_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	unsigned int on;
	char buf[8];
	int ret = 0;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (kstrtouint(buf, 0, &on))
		return -EINVAL;

	mutex_lock(&sep->cfg_lock);
	if (on && !sep->running) {
		ret = sep_arm(sep);
		if (!ret) {
			sep->running = true;
			schedule_delayed_work(&sep->poll, 0);
		}
	} else if (!on && sep->running) {
		sep->running = false;
		cancel_delayed_work_sync(&sep->poll);
		writel(SEP_MBOX_INT_ENABLE, sep->mbox + SEP_MBOX_DIS_INT);
	}
	mutex_unlock(&sep->cfg_lock);

	return ret ? ret : count;
}

static const struct file_operations sep_dbg_start_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sep_dbg_start_write,
};

static void sep_wake_report(struct sep_a9 *sep)
{
	const char *res = sep_wake_result(sep);
	u64 m = sep->wake_msg;

	if (sep->wake_pmgr_after_ret)
		dev_warn(sep->dev, "wake-test: PMGR sep re-read failed (%d)\n",
			 sep->wake_pmgr_after_ret);
	else
		dev_info(sep->dev, "wake-test: pwrstate before=%08x after=%08x%s\n",
			 sep->wake_pmgr, sep->wake_pmgr_after,
			 sep->wake_pmgr_after == sep->wake_pmgr ? "" : " CHANGED");

	if (sep->wake_seen)
		dev_info(sep->dev, "wake-test: result=%s: ep=%02x tag=%02x op=%02x param=%02x data=%08x (raw %016llx) %lld us after word 1, indications=%u, word 2 %s, other=%u\n",
			 res, SEP_MSG_GET_EP(m), SEP_MSG_GET_TAG(m),
			 SEP_MSG_GET_OP(m), SEP_MSG_GET_PARAM(m),
			 SEP_MSG_GET_DATA(m), m,
			 ktime_us_delta(sep->wake_at, sep->wake_t0), sep->wake_ind,
			 sep->wake_w2_drained ? "drained" :
			 sep->wake_w2_posted ? "NOT drained" : "not posted",
			 sep->wake_other);
	else
		dev_warn(sep->dev, "wake-test: result=%s (ret %d): word 1 %s, word 2 %s, no ep 0 op 0x0f/0x15, other=%u first=%016llx, snd_sts %#x -> %#x -> %#x\n",
			 res, sep->wake_ret,
			 sep->wake_w1_drained ? "drained" : "NOT drained",
			 sep->wake_w2_drained ? "drained" :
			 sep->wake_w2_posted ? "NOT drained" : "not posted",
			 sep->wake_other, sep->wake_other_first,
			 sep->wake_sts[0], sep->wake_sts[1], sep->wake_sts[2]);
}

/*
 * XNU's endpoint-0 nap wake (AppleSEPManager_waitUntilCanSendMessage,
 * docs/sep/xart-handshake.md section 7), bounded and once per boot. Preconds:
 * armed, PMGR word still 0x4f0000ff (AUTO_ENABLE only warned about, see
 * sep_pwrstate_gate), no published OOL buffer, AP->SEP FIFO empty; otherwise
 * nothing is sent. Then: post raw 0x00000000000e0000 (ep 0 tag 0 op 0x0e
 * param 0 data 0) through sep_send_raw, one 64-bit store to +0x4010, wait
 * up to 1 s for the FIFO to drain, and post the same word a second time only
 * if it did; then wait up to 1 s for ep 0 op 0x0f or 0x15. Nothing is retried
 * and there is no 200-pair loop. Called with cfg_lock held.
 */
static int sep_wake_test(struct sep_a9 *sep)
{
	unsigned int i, published = 0;
	ktime_t t2, deadline;
	u32 ps, sts;
	int ret;

	if (!sep->running)
		return -ENXIO;
	if (sep->wake_ran)
		return -EALREADY;

	mutex_lock(&sep->lock);
	for (i = 0; i < SEP_EP_COUNT; i++)
		published += sep->eps[i].published;
	mutex_unlock(&sep->lock);
	if (published) {
		dev_warn(sep->dev, "wake-test refused: %u endpoint(s) already have OOL published, nothing sent\n",
			 published);
		return -EBUSY;
	}

	ret = sep_pwrstate_read(&ps);
	if (ret) {
		dev_err(sep->dev, "wake-test refused: no PMGR sep power state (%d), nothing sent\n",
			ret);
		return ret;
	}
	ret = sep_pwrstate_gate(sep, ps, "wake-test");
	if (ret)
		return ret;

	sts = readl(sep->mbox + SEP_MBOX_SND_STS);
	if (!(sts & SEP_MBOX_STS_EMPTY)) {
		dev_warn(sep->dev, "wake-test refused: AP->SEP FIFO not empty (snd_sts=%#x), nothing sent\n",
			 sts);
		return -EBUSY;
	}

	/* From here on, this boot has used its one wake test. */
	sep->wake_ran = true;
	sep->wake_pmgr = ps;
	sep->wake_sts[0] = sts;
	sep->wake_sts[1] = sep->wake_sts[2] = 0;
	sep->wake_w1_drained = sep->wake_w2_posted = sep->wake_w2_drained = false;
	sep->wake_w1_us = sep->wake_w2_us = 0;

	mutex_lock(&sep->lock);
	sep->wake_seen = false;
	sep->wake_msg = 0;
	sep->wake_ind = 0;
	sep->wake_other = 0;
	sep->wake_other_first = 0;
	sep->wake_waiting = true;
	mutex_unlock(&sep->lock);

	dev_info(sep->dev, "wake-test: pwrstate=%08x snd_sts=%#x, posting %016llx (64-bit store to +0x4010)\n",
		 ps, sts, SEP_MSG(0, 0, SEP_OP_NAP_WAKE, 0, 0));
	sep->wake_t0 = ktime_get();
	ret = sep_send_raw(sep, 0, 0, SEP_OP_NAP_WAKE, 0, 0);
	if (ret)
		goto out;
	ret = sep_tx_drain(sep, sts, "wake-test word 1");
	sep->wake_sts[1] = sep->drain_sts_after;
	sep->wake_w1_us = sep->drain_us;
	if (ret) {
		dev_warn(sep->dev, "wake-test: word 1 not consumed, word 2 not posted\n");
		ret = -ETIMEDOUT;
		goto out;
	}
	sep->wake_w1_drained = true;

	ret = sep_send_raw(sep, 0, 0, SEP_OP_NAP_WAKE, 0, 0);
	if (ret)
		goto out;
	sep->wake_w2_posted = true;
	t2 = ktime_get();
	deadline = ktime_add_us(t2, SEP_WAKE_WAIT_US);
	for (;;) {
		bool seen;

		sep_drain(sep);
		mutex_lock(&sep->lock);
		seen = sep->wake_seen;
		mutex_unlock(&sep->lock);

		sts = readl(sep->mbox + SEP_MBOX_SND_STS);
		if (!sep->wake_w2_drained && (sts & SEP_MBOX_STS_EMPTY)) {
			sep->wake_w2_drained = true;
			sep->wake_w2_us = ktime_us_delta(ktime_get(), t2);
		}
		if ((seen && sep->wake_w2_drained) ||
		    ktime_after(ktime_get(), deadline))
			break;
		usleep_range(500, 1000);
	}
	sep->wake_sts[2] = sts;

out:
	if (!sep->wake_w2_posted)
		sep->wake_sts[2] = readl(sep->mbox + SEP_MBOX_SND_STS);
	sep->wake_pmgr_after_ret = sep_pwrstate_read(&sep->wake_pmgr_after);
	mutex_lock(&sep->lock);
	sep->wake_waiting = false;
	if (!ret)
		ret = sep->wake_seen ? 0 : -ENODATA;
	mutex_unlock(&sep->lock);
	sep->wake_ret = ret;
	sep_wake_report(sep);
	return ret;
}

/*
 * echo 1 > wake-test: run sep_wake_test once. The write fails with
 * ENXIO (unarmed), EALREADY (already run this boot), EBUSY (OOL published or
 * FIFO not empty: nothing sent), EIO (PMGR word not 0x4f0000ff apart from
 * AUTO_ENABLE, or off: nothing sent),
 * ETIMEDOUT (word 1 not consumed: word 2 not sent) or ENODATA (no wake
 * indication). cat wake-test reads the recorded result; it reads no register.
 */
static ssize_t sep_dbg_wake_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	unsigned int on;
	char buf[8];
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (kstrtouint(buf, 0, &on) || on != 1)
		return -EINVAL;

	mutex_lock(&sep->cfg_lock);
	ret = sep_wake_test(sep);
	mutex_unlock(&sep->cfg_lock);
	return ret ? ret : count;
}

static ssize_t sep_dbg_wake_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	const size_t size = 1024;
	ssize_t len = 0;
	char *buf;
	u64 m;

	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&sep->cfg_lock);
	len += scnprintf(buf + len, size - len, "result=%s", sep_wake_result(sep));
	if (!sep->wake_ran) {
		len += scnprintf(buf + len, size - len, "\n");
		goto out;
	}
	m = sep->wake_msg;
	len += scnprintf(buf + len, size - len,
			 " ret=%d\npwrstate before=%08x after=%08x (read %d) word=%016llx tx=writeq+0x4010\n"
			 "word1 drained=%d after %lld us, snd_sts %#x -> %#x\n"
			 "word2 posted=%d drained=%d after %lld us, snd_sts at end %#x\n",
			 sep->wake_ret, sep->wake_pmgr, sep->wake_pmgr_after,
			 sep->wake_pmgr_after_ret,
			 SEP_MSG(0, 0, SEP_OP_NAP_WAKE, 0, 0),
			 sep->wake_w1_drained, sep->wake_w1_us,
			 sep->wake_sts[0], sep->wake_sts[1],
			 sep->wake_w2_posted, sep->wake_w2_drained,
			 sep->wake_w2_us, sep->wake_sts[2]);
	if (sep->wake_seen)
		len += scnprintf(buf + len, size - len,
				 "indication ep=%02x tag=%02x op=%02x param=%02x data=%08x raw=%016llx after %lld us (count %u)\n",
				 SEP_MSG_GET_EP(m), SEP_MSG_GET_TAG(m),
				 SEP_MSG_GET_OP(m), SEP_MSG_GET_PARAM(m),
				 SEP_MSG_GET_DATA(m), m,
				 ktime_us_delta(sep->wake_at, sep->wake_t0),
				 sep->wake_ind);
	else
		len += scnprintf(buf + len, size - len,
				 "indication none (ep 0 op 0x0f/0x15)\n");
	len += scnprintf(buf + len, size - len, "other=%u first=%016llx\n",
			 sep->wake_other, sep->wake_other_first);
out:
	mutex_unlock(&sep->cfg_lock);

	len = simple_read_from_buffer(ubuf, count, ppos, buf, len);
	kfree(buf);
	return len;
}

static const struct file_operations sep_dbg_wake_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sep_dbg_wake_read,
	.write = sep_dbg_wake_write,
};

/*
 * echo "EP CMD [HEXPAYLOAD]" > xfer
 *
 * Hex, payload as byte pairs ("01000000" or "01 00 00 00"). The line is parsed
 * by sep_xfer_parse() in the host-tested core, which rejects an odd nibble, a
 * non-hex character, junk after the payload and an oversized field instead of
 * quietly sending a shorter request than the caller wrote.
 *
 * One GenericTransfer transaction through the engine, result kept for read().
 * The syntax and the readback shape of the single-chunk callers are unchanged;
 * a payload larger than one OOL chunk is chunked instead of rejected.
 */
static ssize_t sep_dbg_xfer_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	unsigned int ep = 0, cmd = 0;
	size_t n = 0, replylen;
	u32 status;
	u8 *payload;
	char *buf;
	int ret;

	/* worst case "8 73 " + 65,536 space-separated bytes + newline */
	if (count > SEP_GT_MAX_TOTAL * 3 + 64)
		return -E2BIG;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	payload = kvmalloc(SEP_GT_MAX_TOTAL, GFP_KERNEL);
	if (!payload) {
		kfree(buf);
		return -ENOMEM;
	}

	ret = sep_xfer_parse(buf, SEP_EP_CLIENT_MAX, &ep, &cmd, payload,
			     SEP_GT_MAX_TOTAL, &n);
	if (ret) {
		kvfree(payload);
		kfree(buf);
		return ret;
	}

	mutex_lock(&sep->cfg_lock);
	ret = sep_gt_transfer(sep, ep, cmd, payload, n, sep->last_out,
			      SEP_GT_MAX_TOTAL, &replylen, &status);
	sep->last_ep = ep;
	sep->last_cmd = cmd;
	sep->last_ret = ret < 0 ? ret : 0;
	sep->last_status = status;
	sep->last_out_len = ret < 0 ? 0 : replylen;
	if (!ret && cmd == SEP_CMD_SBIO_INIT && (int)ep == sep->sbio_ep) {
		mutex_lock(&sep->lock);
		sep->sbio_ready = true;
		mutex_unlock(&sep->lock);
		dev_info(sep->dev, "sbio ep %#04x: init (0x73) acknowledged\n",
			 ep);
	}
	mutex_unlock(&sep->cfg_lock);

	kvfree(payload);
	kfree(buf);

	return ret < 0 ? ret : count;
}

/* Wire tag of one recorded chunk, for the readback below. */
static const char *sep_gt_tag_name(u8 tag)
{
	switch (tag) {
	case SEP_GT_TAG_FIRST:
		return "fc";
	case SEP_GT_TAG_CONT:
		return "fd";
	case SEP_GT_TAG_CREDIT:
		return "fe";
	case SEP_GT_TAG_ERR:
		return "ff";
	default:
		return "??";
	}
}

static int sep_gt_chunks_print(char *buf, size_t size, const char *what,
			       const struct sep_gt_word *w, u32 n)
{
	size_t len;
	u32 i;

	len = scnprintf(buf, size, "%s n=%u", what, n);
	for (i = 0; i < n && i < SEP_GT_MAX_CHUNKS && len + 1 < size; i++)
		len += scnprintf(buf + len, size - len, " %s:%u:%u:%u",
				 sep_gt_tag_name(w[i].tag), w[i].seq,
				 w[i].offset, w[i].len);
	if (len + 1 < size)
		len += scnprintf(buf + len, size - len, "\n");
	return min(len, size - 1);
}

static ssize_t sep_dbg_xfer_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	struct sep_gt_xact *x = &sep->gt;
	size_t size = SEP_GT_MAX_TOTAL * 2 + 1024;
	char *buf;
	ssize_t len = 0;
	size_t i;

	buf = kvzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&sep->cfg_lock);
	len += scnprintf(buf + len, size - len,
			 "out_len=%zu\n", sep->last_out_len);
	for (i = 0; i < sep->last_out_len; i++)
		len += scnprintf(buf + len, size - len, "%02x",
				 sep->last_out[i]);
	len += scnprintf(buf + len, size - len,
			 "\nep=%#04x cmd=%#04x ret=%d sep_status=%#x\n",
			 sep->last_ep, sep->last_cmd, sep->last_ret,
			 sep->last_status);
	len += scnprintf(buf + len, size - len,
			 "gt_reply version=%u total=%u offset=%u flags=%#x reserved=%#x command=%#x chunk=%u mailbox_data=%#x\n",
			 x->last.version, x->last.total, x->last.offset,
			 x->last.flags, x->last.reserved, x->last.command,
			 x->last.chunk, x->last_msg_data);
	len += scnprintf(buf + len, size - len,
			 "gt_xact state=%u sent=%u recv=%u credits=%u ignored=%u violations=%u in=%u/%u out=%u/%u\n",
			 x->state, x->n_sent, x->n_recv, x->credits, x->ignored,
			 x->violations, x->in_done, x->in_total, x->out_done,
			 x->out_total);
	len += sep_gt_chunks_print(buf + len, size - len, "gt_sent", x->sent,
				   x->n_sent);
	len += sep_gt_chunks_print(buf + len, size - len, "gt_recv", x->recv,
				   x->n_recv);
	/* the counters and the deadline are RX-writable: same lock as RX */
	mutex_lock(&sep->lock);
	len += scnprintf(buf + len, size - len,
			 "gt_counters ok=%u stale=%u timeouts=%u proto=%u dead=%d quarantine_left_ms=%lld\n",
			 sep->gt_ok, sep->gt_stale, sep->gt_tmo, sep->gt_proto,
			 sep->gt_dead,
			 time_before(jiffies, sep->gt_quiet_until) ?
			 (s64)jiffies_to_msecs(sep->gt_quiet_until - jiffies) : 0);
	mutex_unlock(&sep->lock);
	mutex_unlock(&sep->cfg_lock);

	len = simple_read_from_buffer(ubuf, count, ppos, buf, len);
	kvfree(buf);
	return len;
}

static const struct file_operations sep_dbg_xfer_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sep_dbg_xfer_read,
	.write = sep_dbg_xfer_write,
};


/*
 * echo EP > sbio : select the sbio endpoint (hex). Accepted only for an
 * endpoint whose complete advertisement names it 'sbio'.
 */
static ssize_t sep_dbg_sbio_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	unsigned int ep;
	char buf[8];
	bool ok;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (kstrtouint(strim(buf), 16, &ep) || ep > SEP_EP_CLIENT_MAX)
		return -EINVAL;

	mutex_lock(&sep->cfg_lock);		/* with sep->lock: see lock order */
	mutex_lock(&sep->lock);
	ok = sep_ep_complete(&sep->eps[ep]) &&
	     sep->eps[ep].name == 0x7362696f;	/* 'sbio' */
	if (ok) {
		sep->sbio_ep = ep;
		sep->sbio_ready = false;
	}
	mutex_unlock(&sep->lock);
	mutex_unlock(&sep->cfg_lock);
	if (!ok)
		return -ENOENT;

	return count;
}

static const struct file_operations sep_dbg_sbio_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sep_dbg_sbio_write,
};

/*
 * cat notify: the last SEP_NOTIFY_ENTRIES words that arrived on the selected
 * async endpoint with a non-transport tag, raw. Read-only by construction: this
 * path never replies, never reads a payload and never synthesises lock state
 * from any tag (§9). `len` is the 16-bit value/length field (bits 16..31) and
 * `data` the whole 32-bit info/data field (bits 32..63). Tags 1/2/3 are
 * self-contained service words; the readback states their fields without
 * interpreting them.
 */
static ssize_t sep_dbg_notify_read(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct sep_a9 *sep = file->private_data;
	size_t size = SEP_NOTIFY_ENTRIES * 160 + 512;
	unsigned int i;
	char *buf;
	ssize_t len = 0;

	buf = kvzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&sep->lock);
	len += scnprintf(buf + len, size - len,
			 "# non-transport words on the selected async endpoint (raw; nothing replied, no state inferred)\n"
			 "sbio_ep=%d entries=%u captured=%llu dropped=%u len_is_bits16_31 data_is_bits32_63\n",
			 sep->sbio_ep, sep->notify_count, sep->notify_total,
			 sep->notify_dropped);
	for (i = 0; i < sep->notify_count; i++) {
		const struct sep_notify *n =
			&sep->notify[(sep->notify_head + i) % SEP_NOTIFY_ENTRIES];

		len += scnprintf(buf + len, size - len,
				 "t_us=%lld ep=%02x tag=%02x len=%u data=%08x raw=%016llx\n",
				 ktime_to_us(n->at), n->ep, n->tag, n->len,
				 n->data, n->msg);
	}
	mutex_unlock(&sep->lock);

	len = simple_read_from_buffer(ubuf, count, ppos, buf, len);
	kvfree(buf);
	return len;
}

static const struct file_operations sep_dbg_notify_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sep_dbg_notify_read,
};

static void sep_debugfs_init(struct sep_a9 *sep)
{
	sep->dbg = debugfs_create_dir("sep-a9", NULL);
	if (IS_ERR_OR_NULL(sep->dbg)) {
		sep->dbg = NULL;
		return;
	}

	debugfs_create_file("status", 0444, sep->dbg, sep, &sep_dbg_status_fops);
	debugfs_create_file("log", 0444, sep->dbg, sep, &sep_dbg_log_fops);
	debugfs_create_file("endpoints", 0444, sep->dbg, sep,
			    &sep_dbg_endpoints_fops);
	debugfs_create_file("advertise", 0200, sep->dbg, sep,
			    &sep_dbg_advertise_fops);
	debugfs_create_file("send", 0200, sep->dbg, sep, &sep_dbg_send_fops);
	debugfs_create_file("ool", 0200, sep->dbg, sep, &sep_dbg_ool_fops);
	debugfs_create_file("arts-manifest", 0200, sep->dbg, sep,
			    &sep_dbg_arts_manifest_fops);
	debugfs_create_file("arts-start", 0600, sep->dbg, sep,
			    &sep_dbg_arts_start_fops);
	debugfs_create_file("start", 0200, sep->dbg, sep, &sep_dbg_start_fops);
	debugfs_create_file("fifo", 0444, sep->dbg, sep, &sep_dbg_fifo_fops);
	debugfs_create_file("xfer", 0600, sep->dbg, sep, &sep_dbg_xfer_fops);
	debugfs_create_file("sbio", 0200, sep->dbg, sep, &sep_dbg_sbio_fops);
	debugfs_create_file("notify", 0444, sep->dbg, sep, &sep_dbg_notify_fops);
	debugfs_create_file("wake-test", 0600, sep->dbg, sep, &sep_dbg_wake_fops);
}

static int sep_probe(struct platform_device *pdev)
{
	struct sep_a9 *sep;
	struct resource *res;
	char line[192];

	sep = devm_kzalloc(&pdev->dev, sizeof(*sep), GFP_KERNEL);
	if (!sep)
		return -ENOMEM;
	sep->eps = devm_kcalloc(&pdev->dev, SEP_EP_COUNT, sizeof(*sep->eps),
				GFP_KERNEL);
	if (!sep->eps)
		return -ENOMEM;

	sep->dev = &pdev->dev;
	mutex_init(&sep->lock);
	mutex_init(&sep->tx_lock);
	mutex_init(&sep->cfg_lock);
	init_completion(&sep->gt_ev);
	init_completion(&sep->arts_done);
	INIT_DELAYED_WORK(&sep->poll, sep_poll_work);
	sep->poll_us = 10000;
	sep->sbio_ep = -1;
	sep->last_ep = -1;
	sep->arts_ep = -1;
	sep->arts_result = SEP_ARTS_NEVER;
	sep->gt_ep = 0xff;

	/*
	 * The debugfs xfer readback buffer: one whole transfer's worth of reply,
	 * so the engine's own limit is what bounds a reply and not this buffer.
	 */
	sep->last_out = devm_kzalloc(&pdev->dev, SEP_GT_MAX_TOTAL, GFP_KERNEL);
	if (!sep->last_out)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	sep->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(sep->base))
		return PTR_ERR(sep->base);

	sep->mbox = sep->base + SEP_MBOX_OFFSET;

	/*
	 * No SEP write here; the mailbox is armed through debugfs start. The
	 * only access is this read-only snapshot, and its SEP reads happen only
	 * if the PMGR word says on. sep-a9 probes before pmgr-pwrstate, so this
	 * PMGR word is the one the bootloader chain left.
	 */
	sep_core_snapshot(sep, &sep->probe_snap);
	sep_core_print(line, sizeof(line), &sep->probe_snap);
	dev_info(&pdev->dev, "probe: %s\n", line);

	platform_set_drvdata(pdev, sep);
	sep_a9_instance = sep;

	if (misc_register(&sep_bio_dev))
		dev_warn(&pdev->dev, "could not register /dev/sep-bio\n");

	sep_debugfs_init(sep);

	dev_info(&pdev->dev, "mailbox at %px (sep %px), unarmed\n", sep->mbox,
		 sep->base);
	dev_info(&pdev->dev, "tx: one 64-bit store per message to +0x%x (writeq)\n",
		 SEP_MBOX_OFFSET + SEP_MBOX_SND);
	dev_info(&pdev->dev, "rx: one 64-bit load per message from +0x%x (readq, nonempty only)\n",
		 SEP_MBOX_OFFSET + SEP_MBOX_RECV);
	return 0;
}

static void sep_remove(struct platform_device *pdev)
{
	struct sep_a9 *sep = platform_get_drvdata(pdev);

	sep->running = false;
	cancel_delayed_work_sync(&sep->poll);
	misc_deregister(&sep_bio_dev);
	sep_a9_instance = NULL;
	writel(SEP_MBOX_INT_ENABLE, sep->mbox + SEP_MBOX_DIS_INT);
	debugfs_remove_recursive(sep->dbg);
	/* Published OOL buffers stay allocated: the SEP may still use them. */
}

static const struct of_device_id sep_of_match[] = {
	{ .compatible = "apple,s8000-sep" },
	{ .compatible = "apple,s8003-sep" },
	{ }
};
MODULE_DEVICE_TABLE(of, sep_of_match);

static struct platform_driver sep_driver = {
	.probe = sep_probe,
	.remove = sep_remove,
	.driver = {
		.name = "sep-a9",
		.of_match_table = sep_of_match,
	},
};

/*
 * Without a SEP node in the device tree, instantiate the device ourselves
 * with the j71t addresses from the ADT. Interrupts are not wired up in this
 * stage (see docs/sep/driver-notes.md); the mailbox is polled.
 */
#define SEP_A9_BASE	0x20da00000ULL
#define SEP_A9_SIZE	0x10000

static struct platform_device *sep_pdev;

/*
 * Off by default: reading the SEP block while its power domain is down can
 * stall the AP, and on a checkm8 boot sepOS is usually not running at all.
 * Enable explicitly with sep_a9.enable=1 on the kernel command line.
 */
static bool enable;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Probe the A9 SEP mailbox (default: off)");

static int __init sep_init(void)
{
	struct resource res[] = {
		DEFINE_RES_MEM(SEP_A9_BASE, SEP_A9_SIZE),
	};
	struct device_node *np;
	int ret;

	if (!enable) {
		pr_info("sep-a9: disabled; pass sep_a9.enable=1 to probe\n");
		return 0;
	}

	/*
	 * If the device tree describes the SEP (apple,s8003-sep), the platform
	 * driver instantiates itself from it; registering our own device would
	 * create a second instance.
	 */
	np = of_find_compatible_node(NULL, NULL, "apple,s8003-sep");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "apple,s8000-sep");
	if (np) {
		of_node_put(np);
		return platform_driver_register(&sep_driver);
	}

	sep_pdev = platform_device_register_simple("sep-a9", -1, res,
						   ARRAY_SIZE(res));
	if (IS_ERR(sep_pdev))
		return PTR_ERR(sep_pdev);

	ret = platform_driver_register(&sep_driver);
	if (ret) {
		platform_device_unregister(sep_pdev);
		return ret;
	}

	return 0;
}

static void __exit sep_exit(void)
{
	platform_driver_unregister(&sep_driver);
	if (sep_pdev)
		platform_device_unregister(sep_pdev);
}

module_init(sep_init);
module_exit(sep_exit);

MODULE_DESCRIPTION("Apple A9 Secure Enclave mailbox");
MODULE_LICENSE("GPL");
