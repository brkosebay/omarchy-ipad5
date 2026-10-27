// SPDX-License-Identifier: GPL-2.0
/*
 * apple-vxd V4L2 smoke test (no firmware).  Replays what crashed round 2
 * (udev's v4l_id: open + VIDIOC_QUERYCAP + close) and then walks every
 * ioctl the round-3 audit covers, the media node, and one decode job that
 * must fail cleanly because the DEVA firmware is not running.
 *
 * Usage: vdec-smoke [/dev/videoN] [/dev/mediaN]
 * Prints "ok"/"FAIL" per check and "SMOKE PASS|FAIL (n failures)" last.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#define OUT	V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP	V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

static int fails;

static void check(int ok, const char *fmt, ...)
{
	int err = errno;
	va_list ap;

	printf(ok ? "ok   " : "FAIL ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (!ok) {
		printf(" (errno %d: %s)", err, strerror(err));
		fails++;
	}
	printf("\n");
	fflush(stdout);
}

static const char *fcc(unsigned int f)
{
	static char s[5];

	memcpy(s, &f, 4);
	s[4] = 0;
	return s;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static void v4l_id(const char *path, int round, struct v4l2_capability *cap)
{
	int fd = open(path, O_RDONLY);

	check(fd >= 0, "v4l_id #%d: open %s", round, path);
	if (fd < 0)
		return;
	memset(cap, 0, sizeof(*cap));
	check(xioctl(fd, VIDIOC_QUERYCAP, cap) == 0,
	      "v4l_id #%d: QUERYCAP driver=%s card=%s bus=%s caps=%#x device_caps=%#x",
	      round, cap->driver, cap->card, cap->bus_info, cap->capabilities,
	      cap->device_caps);
	close(fd);
}

static void enum_fmts(int fd, unsigned int type, unsigned int want)
{
	struct v4l2_fmtdesc d;
	unsigned int n = 0, first = 0;

	for (;;) {
		memset(&d, 0, sizeof(d));
		d.type = type;
		d.index = n;
		if (xioctl(fd, VIDIOC_ENUM_FMT, &d))
			break;
		printf("     ENUM_FMT %s[%u] = %s (%s)\n", type == OUT ? "out" : "cap",
		       n, fcc(d.pixelformat), d.description);
		if (!n)
			first = d.pixelformat;
		n++;
	}
	check(errno == EINVAL && n >= 1 && first == want,
	      "ENUM_FMT %s: %u format(s), first %s, terminated by EINVAL",
	      type == OUT ? "output" : "capture", n, fcc(first));
}

static int g_fmt(int fd, unsigned int type, struct v4l2_format *f)
{
	memset(f, 0, sizeof(*f));
	f->type = type;
	return xioctl(fd, VIDIOC_G_FMT, f);
}

static void decode_without_firmware(int fd, int mfd)
{
	struct v4l2_requestbuffers rb;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer b;
	struct pollfd pfd;
	int req_fd = -1, type, r;

	memset(&rb, 0, sizeof(rb));
	rb.type = OUT;
	rb.memory = V4L2_MEMORY_MMAP;
	rb.count = 1;
	check(xioctl(fd, VIDIOC_REQBUFS, &rb) == 0 && rb.count >= 1,
	      "REQBUFS output -> %u (caps %#x)", rb.count, rb.capabilities);
	check(rb.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS,
	      "output queue supports requests");
	memset(&rb, 0, sizeof(rb));
	rb.type = CAP;
	rb.memory = V4L2_MEMORY_MMAP;
	rb.count = 1;
	check(xioctl(fd, VIDIOC_REQBUFS, &rb) == 0 && rb.count >= 1,
	      "REQBUFS capture -> %u", rb.count);

	check(xioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &req_fd) == 0 && req_fd >= 0,
	      "MEDIA_IOC_REQUEST_ALLOC -> fd %d", req_fd);
	if (req_fd < 0)
		return;

	memset(&b, 0, sizeof(b));
	memset(planes, 0, sizeof(planes));
	b.type = OUT;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = planes;
	b.length = 1;
	planes[0].bytesused = 4096;
	b.flags = V4L2_BUF_FLAG_REQUEST_FD;
	b.request_fd = req_fd;
	check(xioctl(fd, VIDIOC_QBUF, &b) == 0, "QBUF output (in request)");
	check(xioctl(req_fd, MEDIA_REQUEST_IOC_QUEUE, NULL) == 0,
	      "MEDIA_REQUEST_IOC_QUEUE");

	memset(&b, 0, sizeof(b));
	memset(planes, 0, sizeof(planes));
	b.type = CAP;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = planes;
	b.length = 1;
	check(xioctl(fd, VIDIOC_QBUF, &b) == 0, "QBUF capture");

	type = OUT;
	check(xioctl(fd, VIDIOC_STREAMON, &type) == 0, "STREAMON output");
	type = CAP;
	check(xioctl(fd, VIDIOC_STREAMON, &type) == 0, "STREAMON capture");

	pfd.fd = req_fd;
	pfd.events = POLLPRI;
	r = poll(&pfd, 1, 5000);
	check(r == 1, "request completed (poll -> %d, revents %#x)", r, pfd.revents);

	memset(&b, 0, sizeof(b));
	memset(planes, 0, sizeof(planes));
	b.type = OUT;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = planes;
	b.length = 1;
	r = xioctl(fd, VIDIOC_DQBUF, &b);
	check(r == 0 && (b.flags & V4L2_BUF_FLAG_ERROR),
	      "DQBUF output: flags %#x (ERROR expected, no firmware)", b.flags);
	memset(&b, 0, sizeof(b));
	memset(planes, 0, sizeof(planes));
	b.type = CAP;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = planes;
	b.length = 1;
	r = xioctl(fd, VIDIOC_DQBUF, &b);
	check(r == 0 && (b.flags & V4L2_BUF_FLAG_ERROR),
	      "DQBUF capture: flags %#x (ERROR expected, no firmware)", b.flags);

	type = OUT;
	check(xioctl(fd, VIDIOC_STREAMOFF, &type) == 0, "STREAMOFF output");
	type = CAP;
	check(xioctl(fd, VIDIOC_STREAMOFF, &type) == 0, "STREAMOFF capture");
	memset(&rb, 0, sizeof(rb));
	rb.type = OUT;
	rb.memory = V4L2_MEMORY_MMAP;
	check(xioctl(fd, VIDIOC_REQBUFS, &rb) == 0, "REQBUFS output 0");
	rb.type = CAP;
	check(xioctl(fd, VIDIOC_REQBUFS, &rb) == 0, "REQBUFS capture 0");
	close(req_fd);
}

int main(int argc, char **argv)
{
	const char *vpath = argc > 1 ? argv[1] : "/dev/video0";
	const char *mpath = argc > 2 ? argv[2] : "/dev/media0";
	struct media_device_info mi;
	struct media_v2_topology topo;
	struct v4l2_query_ext_ctrl qc;
	struct v4l2_frmsizeenum fs;
	struct v4l2_capability cap;
	struct v4l2_format f;
	unsigned int nctrls = 0;
	int fd, mfd, i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	/* What udev did in round 2, a few times over. */
	for (i = 0; i < 3; i++)
		v4l_id(vpath, i, &cap);

	fd = open(vpath, O_RDWR | O_NONBLOCK);
	check(fd >= 0, "open %s O_RDWR", vpath);
	if (fd < 0)
		goto out;

	/* The core adds V4L2_CAP_EXT_PIX_FORMAT itself. */
	check((cap.device_caps & ~V4L2_CAP_EXT_PIX_FORMAT) ==
	      (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING),
	      "device_caps = M2M_MPLANE | STREAMING (| EXT_PIX_FORMAT): %#x",
	      cap.device_caps);
	check(!strcmp((char *)cap.driver, "apple-vxd"), "driver name apple-vxd");

	enum_fmts(fd, OUT, V4L2_PIX_FMT_H264_SLICE);
	enum_fmts(fd, CAP, V4L2_PIX_FMT_NV12);

	check(g_fmt(fd, OUT, &f) == 0 &&
	      f.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_H264_SLICE &&
	      f.fmt.pix_mp.num_planes == 1 &&
	      f.fmt.pix_mp.plane_fmt[0].sizeimage >= 1024 * 1024,
	      "G_FMT output: %s %ux%u planes %u sizeimage %u",
	      fcc(f.fmt.pix_mp.pixelformat), f.fmt.pix_mp.width,
	      f.fmt.pix_mp.height, f.fmt.pix_mp.num_planes,
	      f.fmt.pix_mp.plane_fmt[0].sizeimage);
	check(g_fmt(fd, CAP, &f) == 0 &&
	      f.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12 &&
	      f.fmt.pix_mp.num_planes == 1 && f.fmt.pix_mp.plane_fmt[0].sizeimage,
	      "G_FMT capture: %s %ux%u planes %u sizeimage %u",
	      fcc(f.fmt.pix_mp.pixelformat), f.fmt.pix_mp.width,
	      f.fmt.pix_mp.height, f.fmt.pix_mp.num_planes,
	      f.fmt.pix_mp.plane_fmt[0].sizeimage);

	memset(&f, 0, sizeof(f));
	f.type = OUT;
	f.fmt.pix_mp.pixelformat = 0x30313233;	/* bogus */
	f.fmt.pix_mp.width = 8192;
	f.fmt.pix_mp.height = 8192;
	check(xioctl(fd, VIDIOC_TRY_FMT, &f) == 0 &&
	      f.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_H264_SLICE &&
	      f.fmt.pix_mp.width <= 1920 && f.fmt.pix_mp.height <= 1088,
	      "TRY_FMT output bogus 8192x8192 -> %s %ux%u",
	      fcc(f.fmt.pix_mp.pixelformat), f.fmt.pix_mp.width,
	      f.fmt.pix_mp.height);

	memset(&f, 0, sizeof(f));
	f.type = OUT;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE;
	f.fmt.pix_mp.width = 1920;
	f.fmt.pix_mp.height = 1080;
	check(xioctl(fd, VIDIOC_S_FMT, &f) == 0, "S_FMT output H264_SLICE 1920x1080 -> %ux%u",
	      f.fmt.pix_mp.width, f.fmt.pix_mp.height);
	/* Round 5: NV12 strides are 256-byte aligned (4 KiB-aligned chroma). */
	check(g_fmt(fd, CAP, &f) == 0 && f.fmt.pix_mp.width == 1920 &&
	      f.fmt.pix_mp.height == 1088 &&
	      f.fmt.pix_mp.plane_fmt[0].bytesperline == 2048 &&
	      f.fmt.pix_mp.plane_fmt[0].sizeimage == 2048 * 1088 * 3 / 2,
	      "G_FMT capture follows: %s %ux%u bytesperline %u sizeimage %u",
	      fcc(f.fmt.pix_mp.pixelformat), f.fmt.pix_mp.width,
	      f.fmt.pix_mp.height, f.fmt.pix_mp.plane_fmt[0].bytesperline,
	      f.fmt.pix_mp.plane_fmt[0].sizeimage);
	check(xioctl(fd, VIDIOC_TRY_FMT, &f) == 0, "TRY_FMT capture");
	check(xioctl(fd, VIDIOC_S_FMT, &f) == 0, "S_FMT capture");

	memset(&fs, 0, sizeof(fs));
	fs.pixel_format = V4L2_PIX_FMT_H264_SLICE;
	check(xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0 &&
	      fs.stepwise.max_width == 1920 && fs.stepwise.max_height == 1088,
	      "ENUM_FRAMESIZES H264_SLICE: max %ux%u", fs.stepwise.max_width,
	      fs.stepwise.max_height);
	fs.index = 0;
	fs.pixel_format = V4L2_PIX_FMT_NV12;
	check(xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0 && errno == EINVAL,
	      "ENUM_FRAMESIZES NV12 -> EINVAL");

	memset(&qc, 0, sizeof(qc));
	qc.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
	while (!xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qc)) {
		printf("     ctrl %#x %s\n", qc.id, qc.name);
		nctrls++;
		qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
	}
	check(nctrls >= 10, "QUERY_EXT_CTRL: %u controls", nctrls);

	mfd = open(mpath, O_RDWR);
	check(mfd >= 0, "open %s", mpath);
	if (mfd >= 0) {
		memset(&mi, 0, sizeof(mi));
		check(xioctl(mfd, MEDIA_IOC_DEVICE_INFO, &mi) == 0 &&
		      !strcmp(mi.bus_info, (char *)cap.bus_info),
		      "MEDIA_IOC_DEVICE_INFO driver=%s model=%s bus=%s (matches QUERYCAP)",
		      mi.driver, mi.model, mi.bus_info);
		memset(&topo, 0, sizeof(topo));
		check(xioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &topo) == 0 &&
		      topo.num_entities >= 3,
		      "MEDIA_IOC_G_TOPOLOGY: %u entities, %u interfaces, %u pads, %u links",
		      topo.num_entities, topo.num_interfaces, topo.num_pads,
		      topo.num_links);
		decode_without_firmware(fd, mfd);
		close(mfd);
	}
	close(fd);

	/* And once more, after a full session: release must have been clean. */
	v4l_id(vpath, 3, &cap);
out:
	printf("SMOKE %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
