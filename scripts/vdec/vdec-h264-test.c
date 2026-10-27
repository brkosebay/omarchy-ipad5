// SPDX-License-Identifier: GPL-2.0
/*
 * apple-vxd: decode the embedded test streams (h264-streams.h), or Annex-B
 * files, through the V4L2 stateless request API, frame based, Annex-B, and
 * compare every NV12 frame with ffmpeg's decode (CRC-32 per frame).
 *
 *   vdec-h264-test [-d /dev/videoN] [-m /dev/mediaN] [-s stream] [-o dir]
 *                  [-t timeout_ms] [-n] [-f file.h264 ...]
 *
 * -f decodes files instead (the CRCs, one hex value per line in display
 * order, come from file.crc next to it: scripts/vdec/make-h264-streams.sh);
 * -n skips writing the .nv12 files (for the 1080p timing).
 *
 * Per picture: one OUTPUT buffer with the access unit, SPS + PPS +
 * DECODE_PARAMS (+ SCALING_MATRIX when the PPS says so) in a request, one
 * CAPTURE buffer (timestamp = DPB key).  Pictures are decoded in decode
 * order and compared with ffmpeg's frame of the same display position
 * (B-frame streams reorder).  The decoded frames are written to
 * <dir>/vdec-<stream>-<n>.nv12 (n = decode order; tight NV12 of the
 * cropped size), so they can be viewed with
 *   ffplay -f rawvideo -pixel_format nv12 -video_size WxH file
 *
 * Exit status: 0 every frame DONE and bit-exact, 2 every frame DONE but
 * some differ from ffmpeg (expected against the QEMU fake firmware, which
 * writes no pixels), 1 any error (ERROR buffer, timeout, ioctl failure).
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#include "h264-streams.h"
#include "vdec-h264-parse.h"

#define OUT	V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP	V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define MAX_CAP	(VH_MAX_PICS + 1)

static int vfd = -1, mfd = -1, timeout_ms = 2000;
static const char *outdir = "/tmp";
static bool write_frames = true;

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static int fail(const char *what)
{
	printf("FAIL %s: %s\n", what, strerror(errno));
	fflush(stdout);
	return -1;
}

static uint32_t crc32_zlib(const uint8_t *p, size_t n)
{
	uint32_t c = ~0u;
	unsigned int k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = c >> 1 ^ (0xedb88320u & -(c & 1));
	}
	return ~c;
}

struct mbuf {
	void *p;
	size_t len;
};

static int map_bufs(unsigned int type, unsigned int count, struct mbuf *m)
{
	struct v4l2_requestbuffers rb = { .count = count, .type = type,
					  .memory = V4L2_MEMORY_MMAP };
	unsigned int i;

	if (xioctl(vfd, VIDIOC_REQBUFS, &rb) || rb.count < count)
		return fail("REQBUFS");
	for (i = 0; i < count; i++) {
		struct v4l2_plane pl = { };
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
					 .index = i, .length = 1, .m.planes = &pl };

		if (xioctl(vfd, VIDIOC_QUERYBUF, &b))
			return fail("QUERYBUF");
		m[i].len = pl.length;
		m[i].p = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED,
			      vfd, pl.m.mem_offset);
		if (m[i].p == MAP_FAILED)
			return fail("mmap");
	}
	return 0;
}

static void free_bufs(unsigned int type, unsigned int count, struct mbuf *m)
{
	struct v4l2_requestbuffers rb = { .type = type, .memory = V4L2_MEMORY_MMAP };
	unsigned int i;

	for (i = 0; i < count; i++)
		if (m[i].p && m[i].p != MAP_FAILED)
			munmap(m[i].p, m[i].len);
	xioctl(vfd, VIDIOC_REQBUFS, &rb);
}

static int set_ctrls(int req, const struct vh_stream *vs, const struct vh_pic *pic)
{
	struct v4l2_ext_control c[4] = {
		{ .id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof(vs->sps),
		  .ptr = (void *)&vs->sps },
		{ .id = V4L2_CID_STATELESS_H264_PPS, .size = sizeof(vs->pps),
		  .ptr = (void *)&vs->pps },
		{ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
		  .size = sizeof(pic->dp), .ptr = (void *)&pic->dp },
		{ .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
		  .size = sizeof(vs->scaling), .ptr = (void *)&vs->scaling },
	};
	struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_REQUEST_VAL,
					.count = vs->pps.flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT ?
						 4 : 3,
					.request_fd = req, .controls = c };

	return xioctl(vfd, VIDIOC_S_EXT_CTRLS, &cs) ? fail("S_EXT_CTRLS") : 0;
}

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Returns 0 match, 2 mismatch, -1 error. */
static int run_stream(const struct vdec_h264_stream *st)
{
	static struct vh_stream vs;
	struct mbuf outb[1], capb[MAX_CAP];
	struct v4l2_format fo = { .type = OUT }, fc = { .type = CAP };
	unsigned int ncap, i, y, done = 0, match = 0, w, h, cw, ch, x0, y0;
	unsigned int bpl;
	int type, ret = -1;
	uint8_t *tight = NULL;
	double busy = 0, t0;

	memset(outb, 0, sizeof(outb));
	memset(capb, 0, sizeof(capb));
	if (vh_analyse(st->data, st->size, &vs)) {
		printf("FAIL %s: parser: %s\n", st->name, vs.error);
		return -1;
	}
	/* V4L2 formats take the coded size; the CRCs cover the cropped one. */
	cw = 16 * (vs.sps.pic_width_in_mbs_minus1 + 1);
	ch = 16 * (vs.sps.pic_height_in_map_units_minus1 + 1);
	w = vs.width;
	h = vs.height;
	x0 = vs.crop[0];
	y0 = vs.crop[2];
	if (st->width && (st->width != w || st->height != h)) {
		printf("FAIL %s: header says %ux%u, the SPS %ux%u\n", st->name,
		       st->width, st->height, w, h);
		return -1;
	}
	printf("stream %s: %ux%u (coded %ux%u), %u pictures, profile %u level %u, poc type %u, refs %u, %s%s%s%s\n",
	       st->name, w, h, cw, ch, vs.npics, vs.sps.profile_idc,
	       vs.sps.level_idc, vs.sps.pic_order_cnt_type,
	       vs.sps.max_num_ref_frames,
	       vs.pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE ? "CABAC" : "CAVLC",
	       vs.pps.flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE ? ", 8x8" : "",
	       vs.pps.flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT ? ", scaling lists" : "",
	       vs.pps.flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED ?
		       ", weighted P" : vs.pps.weighted_bipred_idc ? ", weighted B" : "");

	fo.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE;
	fo.fmt.pix_mp.width = cw;
	fo.fmt.pix_mp.height = ch;
	fo.fmt.pix_mp.num_planes = 1;
	fo.fmt.pix_mp.plane_fmt[0].sizeimage = 1 << 20;
	if (xioctl(vfd, VIDIOC_S_FMT, &fo))
		return fail("S_FMT output");
	/* The SPS goes in before the CAPTURE format is chosen. */
	{
		struct v4l2_ext_control c = { .id = V4L2_CID_STATELESS_H264_SPS,
					      .size = sizeof(vs.sps), .ptr = &vs.sps };
		struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL,
						.count = 1, .controls = &c };

		if (xioctl(vfd, VIDIOC_S_EXT_CTRLS, &cs))
			return fail("S_EXT_CTRLS sps");
	}
	fc.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fc.fmt.pix_mp.width = cw;
	fc.fmt.pix_mp.height = ch;
	if (xioctl(vfd, VIDIOC_S_FMT, &fc))
		return fail("S_FMT capture");
	bpl = fc.fmt.pix_mp.plane_fmt[0].bytesperline;
	printf("capture: %ux%u NV12, bytesperline %u, sizeimage %u; output sizeimage %u\n",
	       fc.fmt.pix_mp.width, fc.fmt.pix_mp.height, bpl,
	       fc.fmt.pix_mp.plane_fmt[0].sizeimage,
	       fo.fmt.pix_mp.plane_fmt[0].sizeimage);
	if (fc.fmt.pix_mp.width != cw || fc.fmt.pix_mp.height != ch || bpl < cw)
		return printf("FAIL capture format\n"), -1;

	ncap = vs.npics < MAX_CAP ? vs.npics : MAX_CAP;
	if (map_bufs(OUT, 1, outb) || map_bufs(CAP, ncap, capb))
		goto out;
	type = OUT;
	if (xioctl(vfd, VIDIOC_STREAMON, &type))
		goto out_fail_on;
	type = CAP;
	if (xioctl(vfd, VIDIOC_STREAMON, &type))
		goto out_fail_on;

	tight = malloc(w * h * 3 / 2);
	for (i = 0; i < vs.npics && i < ncap; i++) {
		const struct vh_pic *pic = &vs.pics[i];
		struct v4l2_plane op = { .bytesused = pic->size }, cp = { };
		struct v4l2_buffer ob = { .type = OUT, .memory = V4L2_MEMORY_MMAP,
					  .index = 0, .length = 1, .m.planes = &op,
					  .flags = V4L2_BUF_FLAG_REQUEST_FD };
		struct v4l2_buffer cb = { .type = CAP, .memory = V4L2_MEMORY_MMAP,
					  .index = i, .length = 1, .m.planes = &cp };
		uint32_t crc, ref = pic->display < st->frames ? st->ref_crc[pic->display] : 0;
		uint8_t *nv12 = capb[i].p;
		struct pollfd pf;
		char path[256];
		FILE *f;
		int req;

		if (pic->size > outb[0].len)
			return printf("FAIL picture %u too large (%zu > %zu bytes)\n", i,
				      pic->size, outb[0].len), -1;
		memcpy(outb[0].p, st->data + pic->off, pic->size);
		memset(nv12, 0x5a, capb[i].len);	/* untouched pixels show */

		if (xioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &req)) {
			fail("REQUEST_ALLOC");
			goto out_streamoff;
		}
		if (set_ctrls(req, &vs, pic))
			goto out_req;
		ob.request_fd = req;
		ob.timestamp.tv_sec = 0;
		ob.timestamp.tv_usec = vh_ts(i) / 1000;
		t0 = now_ms();
		if (xioctl(vfd, VIDIOC_QBUF, &cb)) {
			fail("QBUF capture");
			goto out_req;
		}
		if (xioctl(vfd, VIDIOC_QBUF, &ob)) {
			fail("QBUF output");
			goto out_req;
		}
		if (xioctl(req, MEDIA_REQUEST_IOC_QUEUE, NULL)) {
			fail("REQUEST_QUEUE");
			goto out_req;
		}
		pf.fd = req;
		pf.events = POLLPRI;
		if (poll(&pf, 1, timeout_ms) != 1) {
			printf("FAIL picture %u: no completion in %d ms\n", i, timeout_ms);
			goto out_req;
		}
		memset(&cp, 0, sizeof(cp));
		cb.m.planes = &cp;
		if (xioctl(vfd, VIDIOC_DQBUF, &cb)) {
			fail("DQBUF capture");
			goto out_req;
		}
		busy += now_ms() - t0;
		memset(&op, 0, sizeof(op));
		ob.m.planes = &op;
		ob.flags = 0;
		if (xioctl(vfd, VIDIOC_DQBUF, &ob)) {
			fail("DQBUF output");
			goto out_req;
		}
		close(req);

		for (y = 0; y < h; y++)
			memcpy(tight + y * w, nv12 + (y0 + y) * bpl + x0, w);
		for (y = 0; y < h / 2; y++)
			memcpy(tight + w * h + y * w,
			       nv12 + bpl * ch + (y0 / 2 + y) * bpl + x0, w);
		crc = crc32_zlib(tight, w * h * 3 / 2);
		snprintf(path, sizeof(path), "%s/vdec-%s-%u.nv12", outdir, st->name, i);
		if (write_frames) {
			f = fopen(path, "wb");
			if (f) {
				fwrite(tight, 1, w * h * 3 / 2, f);
				fclose(f);
			}
		}
		printf("FRAME %s %u %c%s (display %u): %s, crc %08x, ffmpeg %08x %s, Y[0..7] %02x %02x %02x %02x %02x %02x %02x %02x%s%s\n",
		       st->name, i, pic->type,
		       pic->nslices > 1 ? " (multi-slice)" : "", pic->display,
		       cb.flags & V4L2_BUF_FLAG_ERROR ? "ERROR" : "DONE", crc,
		       ref, crc == ref ? "MATCH" : "MISMATCH",
		       tight[0], tight[1], tight[2], tight[3], tight[4], tight[5],
		       tight[6], tight[7], write_frames ? " -> " : "",
		       write_frames ? path : "");
		fflush(stdout);
		if (!(cb.flags & V4L2_BUF_FLAG_ERROR))
			done++;
		if (crc == ref)
			match++;
		continue;
out_req:
		close(req);
		goto out_streamoff;
	}
	ret = done == i ? (match == i ? 0 : 2) : -1;
	if (i)
		printf("TIMING %s: %u pictures, %.1f ms QBUF->DQBUF in total, %.2f ms each, %.1f pictures/s with one picture in flight\n",
		       st->name, i, busy, busy / i, busy > 0 ? i * 1000.0 / busy : 0);

out_streamoff:
	type = CAP;
	xioctl(vfd, VIDIOC_STREAMOFF, &type);
	type = OUT;
	xioctl(vfd, VIDIOC_STREAMOFF, &type);
	goto out;
out_fail_on:
	fail("STREAMON");
out:
	free(tight);
	free_bufs(OUT, 1, outb);
	free_bufs(CAP, ncap, capb);
	printf("STREAM %s: %u/%u DONE, %u/%u bit-exact -> %s\n", st->name, done,
	       vs.npics, match, vs.npics,
	       ret == 0 ? "PASS" : ret == 2 ? "DONE-MISMATCH" : "FAIL");
	return ret;
}

/* FILE.h264 with FILE.crc (hex, one per line, display order). */
static int load_file(const char *path, struct vdec_h264_stream *st)
{
	static char name[64];
	static unsigned int crcs[VH_MAX_PICS];
	const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	char crcpath[512];
	unsigned char *data;
	unsigned int n = 0;
	long size;
	FILE *f;

	f = fopen(path, "rb");
	if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 ||
	    fseek(f, 0, SEEK_SET))
		return printf("FAIL open %s: %s\n", path, strerror(errno)), -1;
	data = malloc(size);
	if (!data || fread(data, 1, size, f) != (size_t)size)
		return printf("FAIL read %s\n", path), -1;
	fclose(f);
	snprintf(crcpath, sizeof(crcpath), "%.*s.crc",
		 (int)(strrchr(path, '.') ? strrchr(path, '.') - path : (long)strlen(path)),
		 path);
	f = fopen(crcpath, "r");
	while (f && n < VH_MAX_PICS && fscanf(f, "%x", &crcs[n]) == 1)
		n++;
	if (f)
		fclose(f);
	else
		printf("note: no %s, CRCs not compared\n", crcpath);
	snprintf(name, sizeof(name), "%.*s", (int)strcspn(base, "."), base);
	memset(st, 0, sizeof(*st));
	st->name = name;
	st->data = data;
	st->size = size;
	st->frames = n;
	st->ref_crc = crcs;
	return 0;
}

int main(int argc, char **argv)
{
	const char *video = "/dev/video0", *media = "/dev/media0", *only = NULL;
	int opt, worst = 0, files = 0;
	unsigned int i, ran = 0;

	while ((opt = getopt(argc, argv, "d:m:s:o:t:nf")) != -1) {
		switch (opt) {
		case 'd': video = optarg; break;
		case 'm': media = optarg; break;
		case 's': only = optarg; break;
		case 'o': outdir = optarg; break;
		case 't': timeout_ms = atoi(optarg); break;
		case 'n': write_frames = false; break;
		case 'f': files = 1; break;
		default:
			fprintf(stderr, "usage: %s [-d video] [-m media] [-s stream] [-o dir] [-t ms] [-n] [-f file.h264 ...]\n",
				argv[0]);
			return 1;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	vfd = open(video, O_RDWR | O_NONBLOCK);
	mfd = open(media, O_RDWR | O_NONBLOCK);
	if (vfd < 0 || mfd < 0) {
		printf("FAIL open %s / %s: %s\n", video, media, strerror(errno));
		return 1;
	}

	if (files) {
		for (; optind < argc; optind++) {
			struct vdec_h264_stream st;
			int r;

			if (load_file(argv[optind], &st))
				return 1;
			r = run_stream(&st);
			free((void *)st.data);
			ran++;
			if (r < 0)
				worst = 1;
			else if (r == 2 && worst == 0)
				worst = 2;
		}
	} else {
		for (i = 0; i < sizeof(vdec_h264_streams) / sizeof(vdec_h264_streams[0]); i++) {
			int r;

			if (only && strcmp(only, vdec_h264_streams[i].name))
				continue;
			r = run_stream(&vdec_h264_streams[i]);
			ran++;
			if (r < 0)
				worst = 1;
			else if (r == 2 && worst == 0)
				worst = 2;
		}
	}
	printf("H264 %s (%u streams)\n",
	       !ran ? "FAIL: no stream" : worst == 0 ? "PASS" :
	       worst == 2 ? "DONE-MISMATCH" : "FAIL", ran);
	return ran ? worst : 1;
}
