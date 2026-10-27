/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal userspace H.264 parser for the apple-vxd R3c tests: Annex-B
 * splitting, SPS/PPS, the slice-header fields a frame-based V4L2 stateless
 * decoder needs (DECODE_PARAMS), POC types 0 and 2, and a sliding-window
 * DPB, the scaling lists after the fall-back rules (the SCALING_MATRIX
 * control, raster order), frame cropping, and each picture's POC so a
 * test can put B-frame streams back into display order.  Written
 * independently of the driver's in-kernel slice parser so the two can be
 * cross-checked.  Progressive streams without adaptive reference marking
 * (MMCO) only; anything else is reported, not guessed.
 *
 * Used by vdec-h264-test.c (on the device, through the request API) and
 * host/vdec-h264-host.c (command-stream dump against the reference
 * generator).  Header-only; needs <linux/videodev2.h>.
 */
#ifndef VDEC_H264_PARSE_H
#define VDEC_H264_PARSE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <linux/v4l2-controls.h>

#define VH_MAX_NALS	1024
#define VH_MAX_PICS	64

struct vh_nal {
	size_t sc;		/* start code */
	size_t off;		/* NAL header byte */
	size_t size;		/* header .. last non-zero byte */
	uint8_t type, ref_idc;
};

/* An RBSP bit reader over an unescaped copy. */
struct vh_bits {
	uint8_t buf[4096];
	size_t len, pos;	/* pos in bits */
	bool err;
	size_t epb_before[4096 * 8 / 8];	/* EPBs removed before rbsp byte i */
};

static void vh_unescape(struct vh_bits *b, const uint8_t *nal, size_t n)
{
	size_t i, zeros = 0, epb = 0;

	memset(b, 0, sizeof(*b));
	for (i = 0; i < n && b->len < sizeof(b->buf); i++) {
		if (zeros >= 2 && nal[i] == 3) {
			zeros = 0;
			epb++;
			continue;
		}
		zeros = nal[i] ? 0 : zeros + 1;
		b->epb_before[b->len] = epb;
		b->buf[b->len++] = nal[i];
	}
}

static unsigned int vh_u1(struct vh_bits *b)
{
	unsigned int v;

	if (b->pos >= b->len * 8) {
		b->err = true;
		return 0;
	}
	v = (b->buf[b->pos / 8] >> (7 - b->pos % 8)) & 1;
	b->pos++;
	return v;
}

static uint32_t vh_u(struct vh_bits *b, unsigned int n)
{
	uint32_t v = 0;

	while (n--)
		v = v << 1 | vh_u1(b);
	return v;
}

static uint32_t vh_ue(struct vh_bits *b)
{
	unsigned int z = 0;

	while (!vh_u1(b) && !b->err)
		if (++z > 31) {
			b->err = true;
			return 0;
		}
	return z ? ((1u << z) - 1) + vh_u(b, z) : 0;
}

static int32_t vh_se(struct vh_bits *b)
{
	uint32_t k = vh_ue(b);

	return k & 1 ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

static bool vh_more_rbsp_data(struct vh_bits *b)
{
	size_t last = b->len;

	while (last && !b->buf[last - 1])
		last--;
	if (!last)
		return false;
	/* the stop bit is the last 1 of the last non-zero byte */
	return b->pos < (last - 1) * 8 + (7 - __builtin_ctz(b->buf[last - 1]));
}

/* Split an Annex-B stream into NAL units. */
static unsigned int vh_split(const uint8_t *s, size_t n, struct vh_nal *nals,
			     unsigned int max)
{
	unsigned int cnt = 0;
	size_t i = 0, start = (size_t)-1, sc = 0;

	for (;;) {
		bool found = false;

		for (; i + 3 <= n; i++)
			if (!s[i] && !s[i + 1] && s[i + 2] == 1) {
				found = true;
				break;
			}
		if (start != (size_t)-1 && cnt < max) {
			size_t end = found ? i : n;

			while (end > start && !s[end - 1])
				end--;
			nals[cnt].sc = sc;
			nals[cnt].off = start;
			nals[cnt].size = end - start;
			nals[cnt].type = s[start] & 0x1f;
			nals[cnt].ref_idc = (s[start] >> 5) & 3;
			cnt++;
		}
		if (!found)
			break;
		sc = (i && !s[i - 1]) ? i - 1 : i;
		start = i + 3;
		i += 3;
	}
	return cnt;
}

/* Raster position of the i-th coefficient in (frame) zig-zag scan. */
static const uint8_t vh_zigzag4[16] = {
	0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15,
};

static const uint8_t vh_zigzag8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Table 7-3 / 7-4, in zig-zag order as the standard lists them. */
static const uint8_t vh_default4[2][16] = {
	{ 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42 },
	{ 10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34 },
};

static const uint8_t vh_default8[2][64] = {
	{ 6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
	  23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
	  27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	  31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42 },
	{ 9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
	  21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
	  24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	  27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35 },
};

/*
 * Scaling lists 0..11 of one parameter set, raster order (the V4L2
 * control's order): 0-5 the 4x4 lists, 6-11 the 8x8 lists (Intra Y,
 * Inter Y, Intra Cb, Inter Cb, Intra Cr, Inter Cr).
 */
struct vh_lists {
	uint8_t l[12][64];
};

static void vh_default_list(struct vh_lists *ls, unsigned int i)
{
	unsigned int j, n = i < 6 ? 16 : 64;

	for (j = 0; j < n; j++)
		ls->l[i][i < 6 ? vh_zigzag4[j] : vh_zigzag8[j]] =
			i < 6 ? vh_default4[i >= 3][j] :
				vh_default8[(i - 6) & 1][j];
}

/* 7.3.2.1.1.1; returns useDefaultScalingMatrixFlag. */
static bool vh_scaling_list(struct vh_bits *b, uint8_t *list, unsigned int size)
{
	const uint8_t *scan = size == 16 ? vh_zigzag4 : vh_zigzag8;
	int last = 8, next = 8;
	unsigned int j;

	for (j = 0; j < size; j++) {
		if (next) {
			next = (last + vh_se(b) + 256) % 256;
			if (!j && !next)
				return true;
		}
		list[scan[j]] = next ? next : last;
		last = list[scan[j]];
	}
	return false;
}

/*
 * The transmitted lists of an SPS or a PPS with the fall-back rules of
 * Table 7-2 applied: @fallback is NULL for rule A (the defaults) or the
 * SPS lists for rule B.
 */
static void vh_parse_lists(struct vh_bits *b, unsigned int count,
			   struct vh_lists *ls, const struct vh_lists *fallback)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		unsigned int size = i < 6 ? 16 : 64;

		if (vh_u1(b)) {			/* *_scaling_list_present_flag */
			if (vh_scaling_list(b, ls->l[i], size))
				vh_default_list(ls, i);
		} else if (i == 0 || i == 3 || i == 6 || i == 7) {
			if (fallback)
				memcpy(ls->l[i], fallback->l[i], size);
			else
				vh_default_list(ls, i);
		} else {
			/* Cb from Y, Cr from Cb: the list two (8x8) or one back */
			memcpy(ls->l[i], ls->l[i - (i < 6 ? 1 : 2)], size);
		}
	}
}

static void vh_flat_lists(struct vh_lists *ls)
{
	memset(ls, 16, sizeof(*ls));
}

/* Slice-header fields DECODE_PARAMS needs, from the first slice. */
struct vh_slice {
	uint32_t first_mb, slice_type, frame_num, idr_pic_id, poc_lsb;
	int32_t delta_poc_bottom, delta_poc[2];
	uint32_t poc_bits, marking_bits;	/* V4L2 *_bit_size fields */
	uint32_t header_bits;			/* RBSP bits after the NAL header */
	uint32_t header_bits_ebsp;		/* same, EPBs counted */
	bool idr, mmco;
};

static int vh_parse_slice(const uint8_t *nal, size_t n,
			  const struct v4l2_ctrl_h264_sps *sps,
			  const struct v4l2_ctrl_h264_pps *pps, struct vh_slice *s)
{
	unsigned int type = nal[0] & 0x1f, ref_idc = (nal[0] >> 5) & 3;
	uint32_t st, i, j, nref[2];
	struct vh_bits b;
	size_t start;

	vh_unescape(&b, nal + 1, n - 1);
	memset(s, 0, sizeof(*s));
	s->idr = type == 5;
	s->first_mb = vh_ue(&b);
	st = vh_ue(&b) % 5;
	s->slice_type = st;
	vh_ue(&b);			/* pic_parameter_set_id */
	s->frame_num = vh_u(&b, sps->log2_max_frame_num_minus4 + 4);
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
		return -1;		/* field_pic_flag: out of scope */
	if (s->idr)
		s->idr_pic_id = vh_ue(&b);
	start = b.pos;
	if (sps->pic_order_cnt_type == 0) {
		s->poc_lsb = vh_u(&b, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
			s->delta_poc_bottom = vh_se(&b);
	} else if (sps->pic_order_cnt_type == 1 &&
		   !(sps->flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO)) {
		s->delta_poc[0] = vh_se(&b);
		if (pps->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
			s->delta_poc[1] = vh_se(&b);
	}
	s->poc_bits = b.pos - start;
	if (pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT)
		vh_ue(&b);
	if (st == 1)
		vh_u1(&b);		/* direct_spatial_mv_pred_flag */
	nref[0] = pps->num_ref_idx_l0_default_active_minus1 + 1;
	nref[1] = pps->num_ref_idx_l1_default_active_minus1 + 1;
	if (st == 0 || st == 1 || st == 3) {
		if (vh_u1(&b)) {	/* num_ref_idx_active_override_flag */
			nref[0] = vh_ue(&b) + 1;
			if (st == 1)
				nref[1] = vh_ue(&b) + 1;
		}
	}
	if (nref[0] > 32 || nref[1] > 32)
		return -1;
	if (st != 2 && st != 4) {	/* ref_pic_list_modification */
		for (i = 0; i < (st == 1 ? 2u : 1u); i++)
			if (vh_u1(&b)) {
				uint32_t idc;

				do {
					idc = vh_ue(&b);
					if (idc < 3)
						vh_ue(&b);
				} while (idc != 3 && !b.err);
			}
	}
	if (((pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) && (st == 0 || st == 3)) ||
	    (pps->weighted_bipred_idc == 1 && st == 1)) {
		/* pred_weight_table(): skipped, the driver parses it itself */
		unsigned int chroma = sps->chroma_format_idc != 0;

		vh_ue(&b);
		if (chroma)
			vh_ue(&b);
		for (i = 0; i < (st == 1 ? 2u : 1u); i++)
			for (j = 0; j < nref[i] && !b.err; j++) {
				if (vh_u1(&b)) {
					vh_se(&b);
					vh_se(&b);
				}
				if (chroma && vh_u1(&b)) {
					vh_se(&b);
					vh_se(&b);
					vh_se(&b);
					vh_se(&b);
				}
			}
	}
	if (ref_idc) {
		start = b.pos;
		if (s->idr) {
			vh_u1(&b);
			vh_u1(&b);
		} else if (vh_u1(&b)) {
			uint32_t op;

			s->mmco = true;
			do {
				op = vh_ue(&b);
				if (op == 1 || op == 3)
					vh_ue(&b);
				if (op == 2)
					vh_ue(&b);
				if (op == 3 || op == 6)
					vh_ue(&b);
				if (op == 4)
					vh_ue(&b);
			} while (op && !b.err);
		}
		s->marking_bits = b.pos - start;
	}
	if ((pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) && st != 2 && st != 4)
		vh_ue(&b);
	vh_se(&b);			/* slice_qp_delta */
	if (st == 3 || st == 4) {
		if (st == 3)
			vh_u1(&b);
		vh_se(&b);
	}
	if (pps->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT)
		if (vh_ue(&b) != 1) {
			vh_se(&b);
			vh_se(&b);
		}
	s->header_bits = b.pos;
	s->header_bits_ebsp = b.pos + 8 * b.epb_before[b.pos / 8 < b.len ? b.pos / 8 : b.len - 1];
	return b.err ? -1 : 0;
}

/* One coded picture of a stream, with what V4L2 wants for it. */
struct vh_pic {
	size_t off, size;			/* access unit bytes in the stream */
	struct v4l2_ctrl_h264_decode_params dp;
	bool ref;
	char type;				/* 'I', 'P', 'B' */
	unsigned int nslices;
	unsigned int idr_count;			/* IDR pictures so far (incl.) */
	unsigned int display;			/* output order index */
};

struct vh_stream {
	struct v4l2_ctrl_h264_sps sps;
	struct v4l2_ctrl_h264_pps pps;
	/* sent with every picture when the PPS flags SCALING_MATRIX_PRESENT */
	struct v4l2_ctrl_h264_scaling_matrix scaling;
	struct vh_lists sps_lists;
	bool sps_scaling;
	unsigned int crop[4];			/* left, right, top, bottom */
	unsigned int width, height;		/* cropped (display) size */
	struct vh_pic pics[VH_MAX_PICS];
	unsigned int npics;
	const char *error;
};

/*
 * SPS: the V4L2 control, the frame cropping, and the sequence scaling
 * lists (fall-back rule A; flat if seq_scaling_matrix_present_flag is 0).
 */
static int vh_parse_sps(const uint8_t *nal, size_t n, struct vh_stream *vs)
{
	struct v4l2_ctrl_h264_sps *sps = &vs->sps;
	unsigned int i, cw, ch;
	struct vh_bits b;

	vh_unescape(&b, nal + 1, n - 1);
	memset(sps, 0, sizeof(*sps));
	vh_flat_lists(&vs->sps_lists);
	vs->sps_scaling = false;
	sps->profile_idc = vh_u(&b, 8);
	sps->constraint_set_flags = vh_u(&b, 8) >> 2;
	sps->level_idc = vh_u(&b, 8);
	sps->seq_parameter_set_id = vh_ue(&b);
	sps->chroma_format_idc = 1;
	if (V4L2_H264_SPS_HAS_CHROMA_FORMAT(sps)) {
		sps->chroma_format_idc = vh_ue(&b);
		if (sps->chroma_format_idc == 3 && vh_u1(&b))
			sps->flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
		sps->bit_depth_luma_minus8 = vh_ue(&b);
		sps->bit_depth_chroma_minus8 = vh_ue(&b);
		if (vh_u1(&b))
			sps->flags |= V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS;
		if (vh_u1(&b)) {	/* seq_scaling_matrix_present_flag */
			vs->sps_scaling = true;
			vh_parse_lists(&b, sps->chroma_format_idc != 3 ? 8 : 12,
				       &vs->sps_lists, NULL);
		}
	}
	sps->log2_max_frame_num_minus4 = vh_ue(&b);
	sps->pic_order_cnt_type = vh_ue(&b);
	if (sps->pic_order_cnt_type == 0) {
		sps->log2_max_pic_order_cnt_lsb_minus4 = vh_ue(&b);
	} else if (sps->pic_order_cnt_type == 1) {
		if (vh_u1(&b))
			sps->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
		sps->offset_for_non_ref_pic = vh_se(&b);
		sps->offset_for_top_to_bottom_field = vh_se(&b);
		sps->num_ref_frames_in_pic_order_cnt_cycle = vh_ue(&b);
		for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
			sps->offset_for_ref_frame[i] = vh_se(&b);
	}
	sps->max_num_ref_frames = vh_ue(&b);
	if (vh_u1(&b))
		sps->flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
	sps->pic_width_in_mbs_minus1 = vh_ue(&b);
	sps->pic_height_in_map_units_minus1 = vh_ue(&b);
	if (vh_u1(&b))
		sps->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	else if (vh_u1(&b))
		sps->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	if (vh_u1(&b))
		sps->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	/* frame cropping (4:2:0 frames: units of two luma samples); no VUI */
	memset(vs->crop, 0, sizeof(vs->crop));
	if (vh_u1(&b)) {
		cw = sps->chroma_format_idc == 3 ? 1 : 2;
		ch = (sps->chroma_format_idc == 1 ? 2 : 1) *
		     (sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY ? 1 : 2);
		vs->crop[0] = cw * vh_ue(&b);	/* left */
		vs->crop[1] = cw * vh_ue(&b);	/* right */
		vs->crop[2] = ch * vh_ue(&b);	/* top */
		vs->crop[3] = ch * vh_ue(&b);	/* bottom */
	}
	vs->width = 16 * (sps->pic_width_in_mbs_minus1 + 1) - vs->crop[0] - vs->crop[1];
	vs->height = 16 * (sps->pic_height_in_map_units_minus1 + 1) - vs->crop[2] - vs->crop[3];
	return b.err ? -1 : 0;
}

/* The V4L2 control: the lists in force for this PPS, raster order. */
static void vh_fill_scaling(struct vh_stream *vs, const struct vh_lists *ls)
{
	unsigned int i;

	for (i = 0; i < 6; i++)
		memcpy(vs->scaling.scaling_list_4x4[i], ls->l[i], 16);
	for (i = 0; i < 6; i++)
		memcpy(vs->scaling.scaling_list_8x8[i], ls->l[6 + i], 64);
}

static int vh_parse_pps(const uint8_t *nal, size_t n, struct vh_stream *vs)
{
	struct v4l2_ctrl_h264_pps *pps = &vs->pps;
	struct vh_lists pl;
	bool pic_scaling = false;
	struct vh_bits b;

	vh_unescape(&b, nal + 1, n - 1);
	memset(pps, 0, sizeof(*pps));
	pps->pic_parameter_set_id = vh_ue(&b);
	pps->seq_parameter_set_id = vh_ue(&b);
	if (vh_u1(&b))
		pps->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	if (vh_u1(&b))
		pps->flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
	pps->num_slice_groups_minus1 = vh_ue(&b);
	if (pps->num_slice_groups_minus1)
		return -1;	/* FMO: out of scope */
	pps->num_ref_idx_l0_default_active_minus1 = vh_ue(&b);
	pps->num_ref_idx_l1_default_active_minus1 = vh_ue(&b);
	if (vh_u1(&b))
		pps->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
	pps->weighted_bipred_idc = vh_u(&b, 2);
	pps->pic_init_qp_minus26 = vh_se(&b);
	pps->pic_init_qs_minus26 = vh_se(&b);
	pps->chroma_qp_index_offset = vh_se(&b);
	pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
	if (vh_u1(&b))
		pps->flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
	if (vh_u1(&b))
		pps->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (vh_u1(&b))
		pps->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
	pl = vs->sps_lists;
	if (vh_more_rbsp_data(&b)) {
		bool t8;

		if (vh_u1(&b))
			pps->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
		t8 = pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
		if (vh_u1(&b)) {	/* pic_scaling_matrix_present_flag */
			pic_scaling = true;
			/* rule A without sequence lists, rule B with them */
			vh_parse_lists(&b, 6 + (vs->sps.chroma_format_idc != 3 ? 2 : 6) * t8,
				       &pl, vs->sps_scaling ? &vs->sps_lists : NULL);
		}
		pps->second_chroma_qp_index_offset = vh_se(&b);
	}
	if (vs->sps_scaling || pic_scaling)
		pps->flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
	vh_fill_scaling(vs, &pl);
	return b.err ? -1 : 0;
}

/*
 * Split into pictures and compute DECODE_PARAMS, with a DPB that holds
 * the reference pictures by the CAPTURE timestamp each picture will be
 * queued with (ts(i) = (i + 1) * 1000 ns).  Sliding window only.
 */
static uint64_t vh_ts(unsigned int pic)
{
	return (uint64_t)(pic + 1) * 1000;
}

static int vh_analyse(const uint8_t *s, size_t n, struct vh_stream *vs)
{
	static struct vh_nal nals[VH_MAX_NALS];
	struct v4l2_h264_dpb_entry dpb[V4L2_H264_NUM_DPB_ENTRIES];
	unsigned int nn = vh_split(s, n, nals, VH_MAX_NALS), i, k;
	int32_t prev_poc_msb = 0, prev_poc_lsb = 0, prev_frame_num_offset = 0;
	uint32_t prev_frame_num = 0, max_fn, max_lsb;
	bool have_sps = false, have_pps = false;
	unsigned int idrs = 0;
	struct vh_pic *cur = NULL;
	size_t pending = (size_t)-1;	/* first non-VCL NAL before a picture */

	memset(vs, 0, sizeof(*vs));
	memset(dpb, 0, sizeof(dpb));
	for (i = 0; i < nn; i++) {
		const struct vh_nal *nl = &nals[i];
		const uint8_t *nal = s + nl->off;
		struct vh_slice sl;
		int32_t poc = 0;

		if (nl->type == 7) {
			if (vh_parse_sps(nal, nl->size, vs))
				return vs->error = "SPS", -1;
			have_sps = true;
		} else if (nl->type == 8) {
			if (vh_parse_pps(nal, nl->size, vs))
				return vs->error = "PPS (FMO?)", -1;
			have_pps = true;
		}
		if (nl->type != 1 && nl->type != 5) {
			if (pending == (size_t)-1)
				pending = nl->sc;
			if (cur)
				cur->size = nl->sc - cur->off;
			continue;
		}
		if (!have_sps || !have_pps)
			return vs->error = "slice before SPS/PPS", -1;
		if (vh_parse_slice(nal, nl->size, &vs->sps, &vs->pps, &sl))
			return vs->error = "slice header (interlace?)", -1;
		if (sl.first_mb) {			/* next slice of cur */
			if (!cur)
				return vs->error = "slice without a picture", -1;
			cur->nslices++;
			cur->size = nl->off + nl->size - cur->off;
			continue;
		}

		/* A new picture: retire the previous one into the DPB. */
		if (cur && cur->ref) {
			struct v4l2_h264_dpb_entry e = {
				.reference_ts = vh_ts(cur - vs->pics),
				.frame_num = cur->dp.frame_num,
				.fields = V4L2_H264_FRAME_REF,
				.top_field_order_cnt = cur->dp.top_field_order_cnt,
				.bottom_field_order_cnt = cur->dp.bottom_field_order_cnt,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
			};
			unsigned int used = 0, oldest = 0;

			for (k = 0; k < V4L2_H264_NUM_DPB_ENTRIES; k++)
				if (dpb[k].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE)
					used++;
			if (used >= (vs->sps.max_num_ref_frames ? vs->sps.max_num_ref_frames : 1)) {
				/* sliding window: drop the smallest FrameNumWrap */
				int32_t best = INT32_MAX;

				for (k = 0; k < V4L2_H264_NUM_DPB_ENTRIES; k++) {
					int32_t w = dpb[k].frame_num > cur->dp.frame_num ?
						(int32_t)dpb[k].frame_num - (1 << (vs->sps.log2_max_frame_num_minus4 + 4)) :
						(int32_t)dpb[k].frame_num;

					if ((dpb[k].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE) && w < best) {
						best = w;
						oldest = k;
					}
				}
				memset(&dpb[oldest], 0, sizeof(dpb[oldest]));
			}
			for (k = 0; k < V4L2_H264_NUM_DPB_ENTRIES; k++)
				if (!(dpb[k].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
					break;
			dpb[k] = e;
		}
		if (sl.mmco)
			return vs->error = "adaptive reference marking (MMCO)", -1;
		if (vs->npics == VH_MAX_PICS)
			return vs->error = "too many pictures", -1;

		cur = &vs->pics[vs->npics++];
		memset(cur, 0, sizeof(*cur));
		cur->off = pending != (size_t)-1 ? pending : nl->sc;
		cur->size = nl->off + nl->size - cur->off;
		cur->nslices = 1;
		cur->ref = nl->ref_idc != 0;
		cur->type = sl.slice_type == 2 || sl.slice_type == 4 ? 'I' :
			    sl.slice_type == 1 ? 'B' : 'P';
		pending = (size_t)-1;

		if (sl.idr)
			idrs++;
		cur->idr_count = idrs;
		if (sl.idr) {
			memset(dpb, 0, sizeof(dpb));
			prev_poc_msb = prev_poc_lsb = 0;
			prev_frame_num_offset = 0;
			prev_frame_num = 0;
		}
		max_fn = 1u << (vs->sps.log2_max_frame_num_minus4 + 4);
		max_lsb = 1u << (vs->sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (vs->sps.pic_order_cnt_type == 0) {
			int32_t msb = prev_poc_msb, lsb = sl.poc_lsb;

			if (lsb < prev_poc_lsb && prev_poc_lsb - lsb >= (int32_t)max_lsb / 2)
				msb += max_lsb;
			else if (lsb > prev_poc_lsb && lsb - prev_poc_lsb > (int32_t)max_lsb / 2)
				msb -= max_lsb;
			poc = msb + lsb;
			if (cur->ref) {
				prev_poc_msb = msb;
				prev_poc_lsb = lsb;
			}
		} else if (vs->sps.pic_order_cnt_type == 2) {
			int32_t off = sl.idr ? 0 : prev_frame_num_offset +
				(prev_frame_num > sl.frame_num ? (int32_t)max_fn : 0);

			poc = sl.idr ? 0 : 2 * (off + (int32_t)sl.frame_num) - !cur->ref;
			prev_frame_num_offset = off;
			prev_frame_num = sl.frame_num;
		} else {
			return vs->error = "POC type 1", -1;
		}

		memcpy(cur->dp.dpb, dpb, sizeof(dpb));
		for (k = 0; k < V4L2_H264_NUM_DPB_ENTRIES; k++)	/* PicNum */
			if (dpb[k].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE)
				cur->dp.dpb[k].pic_num = dpb[k].frame_num > sl.frame_num ?
					dpb[k].frame_num - max_fn : dpb[k].frame_num;
		cur->dp.nal_ref_idc = nl->ref_idc;
		cur->dp.frame_num = sl.frame_num;
		cur->dp.top_field_order_cnt = poc;
		cur->dp.bottom_field_order_cnt = poc + sl.delta_poc_bottom;
		cur->dp.idr_pic_id = sl.idr_pic_id;
		cur->dp.pic_order_cnt_lsb = sl.poc_lsb;
		cur->dp.delta_pic_order_cnt_bottom = sl.delta_poc_bottom;
		cur->dp.delta_pic_order_cnt0 = sl.delta_poc[0];
		cur->dp.delta_pic_order_cnt1 = sl.delta_poc[1];
		cur->dp.dec_ref_pic_marking_bit_size = sl.marking_bits;
		cur->dp.pic_order_cnt_bit_size = sl.poc_bits;
		cur->dp.flags = (sl.idr ? V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC : 0) |
				(cur->type == 'P' ? V4L2_H264_DECODE_PARAM_FLAG_PFRAME : 0) |
				(cur->type == 'B' ? V4L2_H264_DECODE_PARAM_FLAG_BFRAME : 0);
	}
	if (!vs->npics)
		return vs->error = "no pictures", -1;

	/* Output order: by IDR period, then by POC (no MMCO 5 here). */
	for (i = 0; i < vs->npics; i++) {
		const struct vh_pic *a = &vs->pics[i];
		int32_t pa = a->dp.top_field_order_cnt < a->dp.bottom_field_order_cnt ?
			     a->dp.top_field_order_cnt : a->dp.bottom_field_order_cnt;

		vs->pics[i].display = 0;
		for (k = 0; k < vs->npics; k++) {
			const struct vh_pic *c = &vs->pics[k];
			int32_t pc = c->dp.top_field_order_cnt < c->dp.bottom_field_order_cnt ?
				     c->dp.top_field_order_cnt : c->dp.bottom_field_order_cnt;

			if (c->idr_count < a->idr_count ||
			    (c->idr_count == a->idr_count && pc < pa))
				vs->pics[i].display++;
		}
	}
	return 0;
}

#endif
