// SPDX-License-Identifier: GPL-2.0
/*
 * Host harness for the apple-vxd H.264 command-stream builder.
 *
 * Links the driver's pure files (apple-vxd-cmd.c, apple-vxd-h264-parse.c,
 * apple-vxd-h264-cmd.c) against small shims, runs every picture of the
 * embedded test streams through the same path device_run() takes (slice
 * parse, RefPicList0/1, build) with fixed fake device addresses, and
 * prints one JSON object per picture:
 *
 *   {"stream", "pic", "type", "input": <tools/ds-video/cmdstream.py --gen
 *    schema>, "cmd": [u32 ...], "packets": [{"kind", "payload": [30]}]}
 *
 * scripts/vdec/host/compare.py feeds "input" to the independent Python
 * generator and diffs both outputs word by word.  The DECODE_PARAMS come
 * from the userspace parser (vdec-h264-parse.h), not from the driver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/kernel.h>

#include <media/v4l2-h264.h>

#include "apple-vxd-h264-cmd.h"
#include "apple-vxd-h264-parse.h"

#include "../h264-streams.h"
#include "../vdec-h264-parse.h"

/* Fake device address map (4 KiB aligned like the real ones). */
#define VA_SRC		0x00400000u
#define VA_CAP(i)	(0x00800000u + (i) * 0x00100000u)
#define VA_COLO(i)	(0x04000000u + (i) * 0x00100000u)
#define VA_CAVLC	0x02000000u
#define VA_INTRA	0x02010000u
#define VA_STATE	0x02040000u
#define VA_CMD		0x03000000u

static u32 cmdmem[65536];		/* 256 KiB, APPLE_VXD_CMDBUF_SIZE */
static u8 pktmem[32768];		/* APPLE_VXD_PKTLIST_SIZE */
static struct apple_vxd_h264_slice slices[APPLE_VXD_H264_MAX_SLICES];
static u8 slots[APPLE_VXD_H264_MAX_SLICES][2][V4L2_H264_REF_LIST_LEN];

static void print_sps(const struct v4l2_ctrl_h264_sps *s)
{
	printf("\"sps\":{\"profile_idc\":%u,\"constraint_set_flags\":%u,\"level_idc\":%u,"
	       "\"seq_parameter_set_id\":%u,\"chroma_format_idc\":%u,"
	       "\"bit_depth_luma_minus8\":%u,\"bit_depth_chroma_minus8\":%u,"
	       "\"log2_max_frame_num_minus4\":%u,\"pic_order_cnt_type\":%u,"
	       "\"log2_max_pic_order_cnt_lsb_minus4\":%u,\"max_num_ref_frames\":%u,"
	       "\"num_ref_frames_in_pic_order_cnt_cycle\":%u,"
	       "\"offset_for_non_ref_pic\":%d,\"offset_for_top_to_bottom_field\":%d,"
	       "\"pic_width_in_mbs_minus1\":%u,\"pic_height_in_map_units_minus1\":%u,"
	       "\"flags\":%u}",
	       s->profile_idc, s->constraint_set_flags, s->level_idc,
	       s->seq_parameter_set_id, s->chroma_format_idc,
	       s->bit_depth_luma_minus8, s->bit_depth_chroma_minus8,
	       s->log2_max_frame_num_minus4, s->pic_order_cnt_type,
	       s->log2_max_pic_order_cnt_lsb_minus4, s->max_num_ref_frames,
	       s->num_ref_frames_in_pic_order_cnt_cycle,
	       s->offset_for_non_ref_pic, s->offset_for_top_to_bottom_field,
	       s->pic_width_in_mbs_minus1, s->pic_height_in_map_units_minus1,
	       s->flags);
}

static void print_pps(const struct v4l2_ctrl_h264_pps *p)
{
	printf("\"pps\":{\"pic_parameter_set_id\":%u,\"seq_parameter_set_id\":%u,"
	       "\"num_slice_groups_minus1\":%u,"
	       "\"num_ref_idx_l0_default_active_minus1\":%u,"
	       "\"num_ref_idx_l1_default_active_minus1\":%u,"
	       "\"weighted_bipred_idc\":%u,\"pic_init_qp_minus26\":%d,"
	       "\"pic_init_qs_minus26\":%d,\"chroma_qp_index_offset\":%d,"
	       "\"second_chroma_qp_index_offset\":%d,\"flags\":%u}",
	       p->pic_parameter_set_id, p->seq_parameter_set_id,
	       p->num_slice_groups_minus1, p->num_ref_idx_l0_default_active_minus1,
	       p->num_ref_idx_l1_default_active_minus1, p->weighted_bipred_idc,
	       p->pic_init_qp_minus26, p->pic_init_qs_minus26,
	       p->chroma_qp_index_offset, p->second_chroma_qp_index_offset,
	       p->flags);
}

static void print_dp(const struct v4l2_ctrl_h264_decode_params *d)
{
	unsigned int i;

	printf("\"decode_params\":{\"nal_ref_idc\":%u,\"frame_num\":%u,"
	       "\"top_field_order_cnt\":%d,\"bottom_field_order_cnt\":%d,"
	       "\"idr_pic_id\":%u,\"pic_order_cnt_lsb\":%u,"
	       "\"delta_pic_order_cnt_bottom\":%d,\"delta_pic_order_cnt0\":%d,"
	       "\"delta_pic_order_cnt1\":%d,\"dec_ref_pic_marking_bit_size\":%u,"
	       "\"pic_order_cnt_bit_size\":%u,\"slice_group_change_cycle\":%u,"
	       "\"flags\":%u,\"dpb\":[",
	       d->nal_ref_idc, d->frame_num, d->top_field_order_cnt,
	       d->bottom_field_order_cnt, d->idr_pic_id, d->pic_order_cnt_lsb,
	       d->delta_pic_order_cnt_bottom, d->delta_pic_order_cnt0,
	       d->delta_pic_order_cnt1, d->dec_ref_pic_marking_bit_size,
	       d->pic_order_cnt_bit_size, d->slice_group_change_cycle, d->flags);
	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry *e = &d->dpb[i];

		printf("%s{\"reference_ts\":%llu,\"pic_num\":%u,\"frame_num\":%u,"
		       "\"fields\":%u,\"top_field_order_cnt\":%d,"
		       "\"bottom_field_order_cnt\":%d,\"flags\":%u}",
		       i ? "," : "", (unsigned long long)e->reference_ts,
		       e->pic_num, e->frame_num, e->fields,
		       e->top_field_order_cnt, e->bottom_field_order_cnt, e->flags);
	}
	printf("]}");
}

/*
 * RefPicList0 for P slices, clause 8.2.4.2.1 for frames: short-term
 * references by descending PicNum, then long-term by ascending
 * LongTermPicNum.  (The driver uses the kernel's v4l2-h264 helper.)
 */
static unsigned int p_list(const struct v4l2_ctrl_h264_sps *sps,
			   const struct v4l2_ctrl_h264_decode_params *dp,
			   struct v4l2_h264_reference *list)
{
	s32 key[V4L2_H264_NUM_DPB_ENTRIES];
	u32 maxfn = 1u << (sps->log2_max_frame_num_minus4 + 4);
	unsigned int n = 0, i, j;

	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry *e = &dp->dpb[i];
		bool lt = e->flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;

		if (!(e->flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) ||
		    !(e->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;
		key[n] = lt ? 0x10000 + (s32)e->pic_num :
			      -(e->frame_num > dp->frame_num ?
				(s32)e->frame_num - (s32)maxfn : (s32)e->frame_num);
		list[n].index = i;
		list[n].fields = V4L2_H264_FRAME_REF;
		n++;
	}
	for (i = 1; i < n; i++)
		for (j = i; j && key[j - 1] > key[j]; j--) {
			s32 k = key[j];
			struct v4l2_h264_reference r = list[j];

			key[j] = key[j - 1];
			list[j] = list[j - 1];
			key[j - 1] = k;
			list[j - 1] = r;
		}
	return n;
}

/*
 * RefPicList0/1 for B slices, clause 8.2.4.2.3 for frames: short-term
 * references before the current picture by descending POC then after it
 * by ascending POC (L1: the other way round), long-term by ascending
 * LongTermPicNum; L1 equal to L0 with more than one entry swaps its first
 * two.  Independent of the kernel's v4l2-h264 helper the driver uses.
 */
static unsigned int b_lists(const struct v4l2_ctrl_h264_decode_params *dp,
			    struct v4l2_h264_reference *l0,
			    struct v4l2_h264_reference *l1)
{
	s32 cur = min(dp->top_field_order_cnt, dp->bottom_field_order_cnt);
	s64 key0[V4L2_H264_NUM_DPB_ENTRIES], key1[V4L2_H264_NUM_DPB_ENTRIES];
	unsigned int n = 0, i, j, l;

	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry *e = &dp->dpb[i];
		s32 poc = min(e->top_field_order_cnt, e->bottom_field_order_cnt);

		if (!(e->flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) ||
		    !(e->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;
		if (e->flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) {
			key0[n] = key1[n] = (1LL << 40) + e->pic_num;
		} else if (poc < cur) {
			key0[n] = -(s64)poc;		/* first, descending */
			key1[n] = (1LL << 33) - poc;	/* after the later ones */
		} else {
			key0[n] = (1LL << 33) + poc;
			key1[n] = poc;
		}
		l0[n].index = l1[n].index = i;
		l0[n].fields = l1[n].fields = V4L2_H264_FRAME_REF;
		n++;
	}
	for (l = 0; l < 2; l++) {
		s64 *key = l ? key1 : key0;
		struct v4l2_h264_reference *list = l ? l1 : l0;

		for (i = 1; i < n; i++)
			for (j = i; j && key[j - 1] > key[j]; j--) {
				s64 k = key[j];
				struct v4l2_h264_reference r = list[j];

				key[j] = key[j - 1];
				list[j] = list[j - 1];
				key[j - 1] = k;
				list[j - 1] = r;
			}
	}
	if (n > 1) {
		for (i = 0; i < n && l0[i].index == l1[i].index; i++)
			;
		if (i == n) {
			struct v4l2_h264_reference r = l1[0];

			l1[0] = l1[1];
			l1[1] = r;
		}
	}
	return n;
}

static void print_weights(const struct apple_vxd_h264_slice *sl)
{
	unsigned int l, i;

	printf(",\"pred_weights\":{\"luma_log2_weight_denom\":%u,"
	       "\"chroma_log2_weight_denom\":%u,\"weight_factors\":[",
	       sl->luma_log2_weight_denom, sl->chroma_log2_weight_denom);
	for (l = 0; l < 2; l++) {
		const struct apple_vxd_h264_weight *w = sl->weights[l];

		printf("%s{\"luma_weight\":[", l ? "," : "");
		for (i = 0; i < V4L2_H264_REF_LIST_LEN; i++)
			printf("%s%d", i ? "," : "", w[i].weight[0]);
		printf("],\"luma_offset\":[");
		for (i = 0; i < V4L2_H264_REF_LIST_LEN; i++)
			printf("%s%d", i ? "," : "", w[i].offset[0]);
		printf("],\"chroma_weight\":[");
		for (i = 0; i < V4L2_H264_REF_LIST_LEN; i++)
			printf("%s[%d,%d]", i ? "," : "", w[i].weight[1], w[i].weight[2]);
		printf("],\"chroma_offset\":[");
		for (i = 0; i < V4L2_H264_REF_LIST_LEN; i++)
			printf("%s[%d,%d]", i ? "," : "", w[i].offset[1], w[i].offset[2]);
		printf("]}");
	}
	printf("]}");
}

static void print_scaling(const struct v4l2_ctrl_h264_scaling_matrix *m)
{
	unsigned int i, j;

	printf("\"scaling_matrix\":{\"scaling_list_4x4\":[");
	for (i = 0; i < 6; i++) {
		printf("%s[", i ? "," : "");
		for (j = 0; j < 16; j++)
			printf("%s%u", j ? "," : "", m->scaling_list_4x4[i][j]);
		printf("]");
	}
	printf("],\"scaling_list_8x8\":[");
	for (i = 0; i < 6; i++) {
		printf("%s[", i ? "," : "");
		for (j = 0; j < 64; j++)
			printf("%s%u", j ? "," : "", m->scaling_list_8x8[i][j]);
		printf("]");
	}
	printf("]}");
}

/*
 * The driver builds its initial lists with the kernel's v4l2-h264 helper
 * (linked in from the tree): it must agree with p_list()/b_lists().
 */
static unsigned int helper_checked, helper_bad;

static void check_helper(const struct vdec_h264_stream *st, unsigned int pi,
			 const struct v4l2_ctrl_h264_sps *sps,
			 const struct v4l2_ctrl_h264_decode_params *dp,
			 const struct v4l2_h264_reference *p, unsigned int np,
			 struct v4l2_h264_reference (*b)[V4L2_H264_REF_LIST_LEN],
			 unsigned int nb)
{
	struct v4l2_h264_reference hp[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference hb[2][V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reflist_builder bld;
	unsigned int i, l;
	bool ok;

	v4l2_h264_init_reflist_builder(&bld, dp, sps, dp->dpb);
	v4l2_h264_build_p_ref_list(&bld, hp);
	v4l2_h264_build_b_ref_lists(&bld, hb[0], hb[1]);
	ok = bld.num_valid == np && np == nb;
	for (i = 0; ok && i < np; i++)
		ok = hp[i].index == p[i].index;
	for (l = 0; l < 2; l++)
		for (i = 0; ok && i < nb; i++)
			ok = hb[l][i].index == b[l][i].index;
	helper_checked++;
	if (!ok) {
		helper_bad++;
		fprintf(stderr, "%s pic %u: v4l2-h264 helper lists differ from the harness\n",
			st->name, pi);
	}
}

static int one_picture(const struct vdec_h264_stream *st, const struct vh_stream *vs,
		       unsigned int pi)
{
	const struct vh_pic *pic = &vs->pics[pi];
	const u8 *au = st->data + pic->off;
	u32 w = vs->sps.pic_width_in_mbs_minus1 + 1;
	u32 h = vs->sps.pic_height_in_map_units_minus1 + 1;
	u32 stride = (16 * w + 255) & ~255u;
	struct apple_vxd_cmdbuf cb = { };
	struct apple_vxd_h264_frame f = { };
	struct v4l2_h264_reference list[V4L2_H264_REF_LIST_LEN + 1];
	struct v4l2_h264_reference bl[2][V4L2_H264_REF_LIST_LEN];
	const char *why = NULL;
	unsigned int n, i, k, nv, nb, off, lst;

	if (apple_vxd_h264_find_slices(au, pic->size, &vs->sps, &vs->pps, slices,
				       APPLE_VXD_H264_MAX_SLICES, &n, &why)) {
		fprintf(stderr, "%s pic %u: find_slices: %s\n", st->name, pi, why);
		return -1;
	}
	nv = p_list(&vs->sps, &pic->dp, list);
	nb = b_lists(&pic->dp, bl[0], bl[1]);
	check_helper(st, pi, &vs->sps, &pic->dp, list, nv, bl, nb);
	for (i = 0; i < n; i++) {
		struct v4l2_h264_reference l[V4L2_H264_REF_LIST_LEN];
		bool bslice = slices[i].slice_type == V4L2_H264_SLICE_TYPE_B;

		memset(slots[i], APPLE_VXD_H264_NO_REF, sizeof(slots[i]));
		if (slices[i].slice_type != V4L2_H264_SLICE_TYPE_P && !bslice)
			continue;
		for (lst = 0; lst < (bslice ? 2u : 1u); lst++) {
			const struct v4l2_h264_reference *src = bslice ? bl[lst] : list;
			unsigned int cnt = bslice ? nb : nv;
			unsigned int len = 1 + (lst ? slices[i].num_ref_idx_l1_active_minus1 :
						      slices[i].num_ref_idx_l0_active_minus1);

			for (k = 0; k < V4L2_H264_REF_LIST_LEN; k++)
				l[k] = k < cnt ? src[k] :
				       (struct v4l2_h264_reference){ 3, APPLE_VXD_H264_NO_REF };
			if (apple_vxd_h264_modify_ref_list(&slices[i], lst, &vs->sps,
							   &pic->dp, l, len)) {
				fprintf(stderr, "%s pic %u: ref list modification failed\n",
					st->name, pi);
				return -1;
			}
			for (k = 0; k < len; k++)
				slots[i][lst][k] = l[k].index;
		}
	}

	f.sps = &vs->sps;
	f.pps = &vs->pps;
	f.dp = &pic->dp;
	f.scaling = &vs->scaling;
	f.slices = slices;
	f.nslices = n;
	f.ref_slots = (const u8 (*)[2][V4L2_H264_REF_LIST_LEN])slots;
	f.src_va = VA_SRC;
	f.dst_luma = VA_CAP(pi);
	f.dst_chroma = VA_CAP(pi) + stride * 16 * h;
	f.dst_stride = stride;
	f.colocated = VA_COLO(pi);
	for (k = 0; k < V4L2_H264_NUM_DPB_ENTRIES; k++) {
		const struct v4l2_h264_dpb_entry *e = &pic->dp.dpb[k];

		if (!(e->flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
			continue;
		f.dpb_luma[k] = VA_CAP(e->reference_ts / 1000 - 1);
		f.dpb_chroma[k] = f.dpb_luma[k] + stride * 16 * h;
		f.dpb_colocated[k] = VA_COLO(e->reference_ts / 1000 - 1);
	}
	f.bufs.cavlc = VA_CAVLC;
	f.bufs.intra = VA_INTRA;
	f.bufs.stream_state = VA_STATE;
	f.first_picture = pi == 0;

	cb.vaddr = cmdmem;
	cb.size = sizeof(cmdmem);
	cb.va = VA_CMD;
	cb.pkt = pktmem;
	cb.pkt_size = sizeof(pktmem);
	apple_vxd_cmdbuf_reset(&cb);
	if (apple_vxd_h264_build(&cb, &f, &why)) {
		fprintf(stderr, "%s pic %u: build: %s\n", st->name, pi, why);
		return -1;
	}

	printf("{\"stream\":\"%s\",\"pic\":%u,\"type\":\"%c\",\"nslices\":%u,\"input\":{",
	       st->name, pi, pic->type, n);
	print_sps(&vs->sps);
	printf(",");
	print_pps(&vs->pps);
	printf(",");
	print_dp(&pic->dp);
	printf(",");
	print_scaling(&vs->scaling);
	printf(",\"slices\":[");
	for (i = 0; i < n; i++) {
		const struct apple_vxd_h264_slice *sl = &slices[i];

		printf("%s{\"params\":{\"header_bit_size\":%u,\"first_mb_in_slice\":%u,"
		       "\"slice_type\":%u,\"colour_plane_id\":0,\"redundant_pic_cnt\":%u,"
		       "\"cabac_init_idc\":%u,\"slice_qp_delta\":%d,\"slice_qs_delta\":%d,"
		       "\"disable_deblocking_filter_idc\":%u,"
		       "\"slice_alpha_c0_offset_div2\":%d,\"slice_beta_offset_div2\":%d,"
		       "\"num_ref_idx_l0_active_minus1\":%u,"
		       "\"num_ref_idx_l1_active_minus1\":%u,\"flags\":%u,"
		       "\"ref_pic_list0\":[",
		       i ? "," : "",
		       /* to slice_data: CABAC includes cabac_alignment_one_bit */
		       (vs->pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) ?
		       (8 + sl->header_bits_raw + 7) & ~7u : 8 + sl->header_bits_raw,
		       sl->first_mb_in_slice,
		       sl->slice_type, sl->redundant_pic_cnt, sl->cabac_init_idc,
		       sl->slice_qp_delta, sl->slice_qs_delta,
		       sl->disable_deblocking_filter_idc,
		       sl->slice_alpha_c0_offset_div2, sl->slice_beta_offset_div2,
		       sl->num_ref_idx_l0_active_minus1,
		       sl->num_ref_idx_l1_active_minus1,
		       sl->direct_spatial_mv_pred ? 1 : 0);
		for (lst = 0; lst < 2; lst++) {
			printf("%s", lst ? "],\"ref_pic_list1\":[" : "");
			for (k = 0; k < V4L2_H264_REF_LIST_LEN; k++)
				printf("%s{\"fields\":3,\"index\":%u}", k ? "," : "",
				       slots[i][lst][k]);
		}
		printf("]}");
		print_weights(sl);
		printf(",\"offset\":%u,\"size\":%u}", sl->nal_offset, sl->nal_size);
	}
	printf("],\"first_picture\":%s,\"va\":{\"cmd\":%u,\"src\":%u,"
	       "\"dst_luma\":%u,\"dst_chroma\":%u,\"dst_stride\":%u,"
	       "\"cavlc\":%u,\"intra\":%u,\"stream_state\":%u,\"colocated\":%u,"
	       "\"dpb\":[",
	       f.first_picture ? "true" : "false", VA_CMD, f.src_va, f.dst_luma,
	       f.dst_chroma, f.dst_stride, VA_CAVLC, VA_INTRA, VA_STATE,
	       f.colocated);
	for (k = 0; k < V4L2_H264_NUM_DPB_ENTRIES; k++) {
		if (f.dpb_luma[k])
			printf("%s{\"luma\":%u,\"chroma\":%u,\"colocated\":%u}",
			       k ? "," : "", f.dpb_luma[k], f.dpb_chroma[k],
			       f.dpb_colocated[k]);
		else
			printf("%snull", k ? "," : "");
	}
	printf("]}},\"cmd\":[");
	for (k = 0; k < cb.used / 4; k++)
		printf("%s%u", k ? "," : "", cmdmem[k]);
	printf("],\"packets\":[");
	for (off = 0, k = 0; off < cb.pkt_used; k++) {
		const u32 *p = (const u32 *)(pktmem + off);

		printf("%s{\"kind\":%u,\"payload\":[", k ? "," : "",
		       p[3 + VXD_PKT_KIND / 4]);
		for (i = 0; i < VXD_PKT_H264_PAYLOAD / 4; i++)
			printf("%s%u", i ? "," : "", p[3 + i]);
		printf("]}");
		off += VXD_PKT_HDR_BYTES + p[1];
	}
	printf("]}\n");
	return 0;
}

/* -f FILE.h264 ...: Annex-B files instead of the embedded streams. */
static int load_file(const char *path, struct vdec_h264_stream *st)
{
	const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	static char names[16][64];
	static unsigned int nnames;
	unsigned char *data;
	FILE *f = fopen(path, "rb");
	long n;

	if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET))
		return -1;
	data = malloc(n);
	if (!data || fread(data, 1, n, f) != (size_t)n)
		return -1;
	fclose(f);
	snprintf(names[nnames % 16], sizeof(names[0]), "%.*s",
		 (int)(strcspn(base, ".")), base);
	memset(st, 0, sizeof(*st));
	st->name = names[nnames++ % 16];
	st->data = data;
	st->size = n;
	return 0;
}

int main(int argc, char **argv)
{
	static struct vdec_h264_stream files[16];
	static struct vh_stream vs;
	const struct vdec_h264_stream *list = vdec_h264_streams;
	unsigned int s, i, count = ARRAY_SIZE(vdec_h264_streams);
	int rc = 0;

	if (argc > 2 && !strcmp(argv[1], "-f")) {
		for (count = 0; count + 2 < (unsigned int)argc && count < 16; count++)
			if (load_file(argv[count + 2], &files[count])) {
				fprintf(stderr, "%s: cannot read\n", argv[count + 2]);
				return 1;
			}
		list = files;
		argc = 1;
	}
	for (s = 0; s < count; s++) {
		const struct vdec_h264_stream *st = &list[s];

		if (argc > 1 && strcmp(argv[1], st->name))
			continue;
		if (vh_analyse(st->data, st->size, &vs)) {
			fprintf(stderr, "%s: parser: %s\n", st->name, vs.error);
			rc = 1;
			continue;
		}
		for (i = 0; i < vs.npics; i++)
			if (one_picture(st, &vs, i))
				rc = 1;
	}
	fprintf(stderr, "v4l2-h264 helper: %u pictures, P/B0/B1 lists %s\n",
		helper_checked, helper_bad ? "DIFFER" : "identical to the harness's");
	return rc || helper_bad;
}
