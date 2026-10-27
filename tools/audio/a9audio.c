// SPDX-License-Identifier: MIT
/*
 * a9audio: tiny static ALSA player/mixer for the HoolockRD shell (no
 * alsa-lib, no alsa-utils). Raw PCM and control ioctls only.
 *
 *   a9audio info  [-c CARD]                      list PCM + controls
 *   a9audio ctl   [-c CARD] NAME VALUE            set an integer/bool control
 *   a9audio play  [-c CARD] [-d DEV] [-p FRAMES] [-n PERIODS] FILE.wav
 *   a9audio tone  [-c CARD] [-d DEV] [-r RATE] [-f HZ] [-s SECS] [-a DBFS] [-b 16|32] [-L|-R]
 *   a9audio rec   [-c CARD] [-d DEV] [-r RATE] [-s SECS] [-b 16|32] FILE.wav
 *                 (stereo capture; prints per-channel RMS/peak/DC and the
 *                 measured rate, so a test passes without copying the file)
 *
 * Every blocking step has a timeout (poll) so a dead data path cannot hang
 * the shell; xruns are counted and recovered.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/spi/spidev.h>
#include <sound/asound.h>

static int card;
static int device;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- hw params helpers (struct snd_pcm_hw_params masks/intervals) ---- */

static struct snd_mask *hp_mask(struct snd_pcm_hw_params *p, int n)
{
	return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *hp_iv(struct snd_pcm_hw_params *p, int n)
{
	return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void hp_any(struct snd_pcm_hw_params *p)
{
	int n;

	memset(p, 0, sizeof(*p));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
		memset(hp_mask(p, n), 0xff, sizeof(struct snd_mask));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
		hp_iv(p, n)->min = 0;
		hp_iv(p, n)->max = ~0u;
	}
	p->rmask = ~0u;
	p->info = ~0u;
}

static void hp_set_mask(struct snd_pcm_hw_params *p, int n, unsigned int bit)
{
	struct snd_mask *m = hp_mask(p, n);

	memset(m, 0, sizeof(*m));
	m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void hp_set_int(struct snd_pcm_hw_params *p, int n, unsigned int v)
{
	struct snd_interval *i = hp_iv(p, n);

	i->min = i->max = v;
	i->integer = 1;
}

static unsigned int hp_get(struct snd_pcm_hw_params *p, int n)
{
	return hp_iv(p, n)->min;
}

/* ---- PCM ---- */

struct pcm {
	int fd;
	int capture;
	unsigned int rate, channels, bits, period, periods;
	unsigned long xruns;
};

static int pcm_open(struct pcm *pcm, unsigned int rate, unsigned int bits,
		    unsigned int period, unsigned int periods)
{
	struct snd_pcm_hw_params hp;
	struct snd_pcm_sw_params sp;
	char path[64];

	snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%d%c", card, device,
		 pcm->capture ? 'c' : 'p');
	pcm->fd = open(path, O_RDWR | O_NONBLOCK);
	if (pcm->fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}

	hp_any(&hp);
	hp_set_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	hp_set_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT,
		    bits == 32 ? SNDRV_PCM_FORMAT_S32_LE : SNDRV_PCM_FORMAT_S16_LE);
	hp_set_mask(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	hp_set_int(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	hp_set_int(&hp, SNDRV_PCM_HW_PARAM_RATE, rate);
	hp_set_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
	hp_set_int(&hp, SNDRV_PCM_HW_PARAM_PERIODS, periods);

	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
		fprintf(stderr, "HW_PARAMS (rate %u, %u bit, period %u x %u): %s\n",
			rate, bits, period, periods, strerror(errno));
		return -1;
	}
	pcm->rate = hp_get(&hp, SNDRV_PCM_HW_PARAM_RATE);
	pcm->channels = 2;
	pcm->bits = bits;
	pcm->period = hp_get(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	pcm->periods = hp_get(&hp, SNDRV_PCM_HW_PARAM_PERIODS);

	memset(&sp, 0, sizeof(sp));
	sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sp.period_step = 1;
	sp.avail_min = pcm->period;
	/* capture is started explicitly after PREPARE */
	sp.start_threshold = pcm->capture ? 1 : pcm->period * (pcm->periods - 1);
	sp.stop_threshold = pcm->period * pcm->periods;
	sp.boundary = pcm->period * pcm->periods;
	while (sp.boundary * 2 <= 0x7fffffffUL - pcm->period * pcm->periods)
		sp.boundary *= 2;
	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) < 0) {
		fprintf(stderr, "SW_PARAMS: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		fprintf(stderr, "PREPARE: %s\n", strerror(errno));
		return -1;
	}
	printf("pcmC%dD%d%c: %u Hz, %u ch, S%u_LE, period %u x %u\n", card,
	       device, pcm->capture ? 'c' : 'p', pcm->rate, pcm->channels, bits,
	       pcm->period, pcm->periods);
	return 0;
}

/* read exactly @frames; poll timeout guards against a stalled pointer */
static int pcm_read(struct pcm *pcm, void *buf, unsigned long frames)
{
	unsigned int fb = pcm->channels * pcm->bits / 8;
	char *p = buf;
	int timeout_ms = 2000 + 2000 * pcm->period * pcm->periods / pcm->rate;

	while (frames) {
		struct snd_xferi x = { .buf = p, .frames = frames };
		struct pollfd pfd = { .fd = pcm->fd, .events = POLLIN };

		if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) == 0) {
			p += x.result * fb;
			frames -= x.result;
			continue;
		}
		if (errno == EPIPE) {
			pcm->xruns++;
			ioctl(pcm->fd, SNDRV_PCM_IOCTL_PREPARE);
			ioctl(pcm->fd, SNDRV_PCM_IOCTL_START);
			continue;
		}
		if (errno != EAGAIN) {
			fprintf(stderr, "READI: %s\n", strerror(errno));
			return -1;
		}
		if (poll(&pfd, 1, timeout_ms) == 0) {
			fprintf(stderr, "stalled: no data for %d ms (no RX completions?)\n",
				timeout_ms);
			return -1;
		}
	}
	return 0;
}

/* write all frames; poll timeout guards against a stalled pointer */
static int pcm_write(struct pcm *pcm, const void *buf, unsigned long frames)
{
	unsigned int fb = pcm->channels * pcm->bits / 8;
	const char *p = buf;
	int timeout_ms = 2000 + 2000 * pcm->period * pcm->periods / pcm->rate;

	while (frames) {
		struct snd_xferi x = { .buf = (void *)p, .frames = frames };
		struct pollfd pfd = { .fd = pcm->fd, .events = POLLOUT };

		if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) {
			p += x.result * fb;
			frames -= x.result;
			continue;
		}
		if (errno == EPIPE) {
			pcm->xruns++;
			ioctl(pcm->fd, SNDRV_PCM_IOCTL_PREPARE);
			continue;
		}
		if (errno != EAGAIN) {
			fprintf(stderr, "WRITEI: %s\n", strerror(errno));
			return -1;
		}
		if (poll(&pfd, 1, timeout_ms) == 0) {
			fprintf(stderr, "stalled: no space for %d ms (pointer not moving?)\n",
				timeout_ms);
			return -1;
		}
	}
	return 0;
}

static void pcm_close(struct pcm *pcm, double t0, unsigned long total)
{
	struct snd_pcm_status st;
	int fl = fcntl(pcm->fd, F_GETFL);

	fcntl(pcm->fd, F_SETFL, fl & ~O_NONBLOCK);
	ioctl(pcm->fd, SNDRV_PCM_IOCTL_DRAIN);
	memset(&st, 0, sizeof(st));
	ioctl(pcm->fd, SNDRV_PCM_IOCTL_STATUS, &st);
	printf("played %lu frames in %.2f s (expected %.2f s), xruns %lu, state %d\n",
	       total, now() - t0, (double)total / pcm->rate, pcm->xruns, st.state);
	close(pcm->fd);
}

/* ---- WAV ---- */

static int play_wav(const char *path, unsigned int period, unsigned int periods)
{
	FILE *f = fopen(path, "rb");
	unsigned char hdr[12], ck[8];
	unsigned int rate = 0, ch = 0, bits = 0;
	unsigned long total = 0;
	struct pcm pcm = {};
	double t0;

	if (!f || fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) ||
	    memcmp(hdr + 8, "WAVE", 4)) {
		fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
		return 1;
	}
	for (;;) {
		uint32_t len;

		if (fread(ck, 1, 8, f) != 8) {
			fprintf(stderr, "no data chunk\n");
			return 1;
		}
		len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if (!memcmp(ck, "fmt ", 4)) {
			unsigned char fmt[40];

			if (len > sizeof(fmt) || fread(fmt, 1, len, f) != len)
				return 1;
			ch = fmt[2] | fmt[3] << 8;
			rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (unsigned)fmt[7] << 24;
			bits = fmt[14] | fmt[15] << 8;
			if ((fmt[0] | fmt[1] << 8) != 1 || ch != 2 || (bits != 16 && bits != 32)) {
				fprintf(stderr, "need PCM stereo 16/32-bit (got ch %u bits %u)\n",
					ch, bits);
				return 1;
			}
		} else if (!memcmp(ck, "data", 4)) {
			break;
		} else {
			fseek(f, len + (len & 1), SEEK_CUR);
		}
	}

	if (pcm_open(&pcm, rate, bits, period, periods))
		return 1;

	t0 = now();
	for (;;) {
		static char buf[65536];
		size_t fb = ch * bits / 8, n = fread(buf, fb, sizeof(buf) / fb, f);

		if (!n)
			break;
		if (pcm_write(&pcm, buf, n))
			break;
		total += n;
	}
	pcm_close(&pcm, t0, total);
	fclose(f);
	return 0;
}

static int play_tone(unsigned int rate, unsigned int bits, double hz, double secs,
		     double dbfs, int lr, unsigned int period, unsigned int periods)
{
	struct pcm pcm = {};
	unsigned long total = (unsigned long)(rate * secs), i, done = 0;
	double amp = pow(10.0, dbfs / 20.0), t0;
	static int32_t buf32[4096 * 2];
	static int16_t buf16[4096 * 2];

	if (pcm_open(&pcm, rate, bits, period, periods))
		return 1;

	t0 = now();
	while (done < total) {
		unsigned long n = total - done > 4096 ? 4096 : total - done;

		for (i = 0; i < n; i++) {
			double t = (double)(done + i) / rate;
			double fade = 1.0, s;

			if (t < 0.05)
				fade = t / 0.05;
			if (secs - t < 0.05)
				fade = (secs - t) / 0.05;
			s = amp * fade * sin(2 * M_PI * hz * t);
			if (bits == 32) {
				int32_t v = (int32_t)(s * 2147483647.0);

				buf32[2 * i] = lr == 'R' ? 0 : v;
				buf32[2 * i + 1] = lr == 'L' ? 0 : v;
			} else {
				int16_t v = (int16_t)(s * 32767.0);

				buf16[2 * i] = lr == 'R' ? 0 : v;
				buf16[2 * i + 1] = lr == 'L' ? 0 : v;
			}
		}
		if (pcm_write(&pcm, bits == 32 ? (void *)buf32 : (void *)buf16, n))
			break;
		done += n;
	}
	pcm_close(&pcm, t0, done);
	return 0;
}

static void wav_header(unsigned char *h, unsigned int rate, unsigned int bits,
		       uint32_t data_bytes)
{
	uint32_t v[] = { 36 + data_bytes, 16, 1 | 2 << 16, rate,
			 rate * 2 * bits / 8, (2 * bits / 8) | bits << 16,
			 data_bytes };

	memcpy(h, "RIFF", 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	memcpy(h + 36, "data", 4);
	/* little-endian host (arm64) */
	memcpy(h + 4, &v[0], 4);
	memcpy(h + 16, &v[1], 4);
	memcpy(h + 20, &v[2], 4);
	memcpy(h + 24, &v[3], 4);
	memcpy(h + 28, &v[4], 4);
	memcpy(h + 32, &v[5], 4);
	memcpy(h + 40, &v[6], 4);
}

static double db(double x)
{
	return x > 0 ? 20 * log10(x) : -999;
}

/*
 * Stereo capture to a WAV file. Prints per channel RMS, peak and DC
 * (dBFS / % of full scale) and the frame rate measured from the wall
 * clock: a silent or DC-only channel shows up without listening.
 */
static int rec_wav(const char *path, unsigned int rate, unsigned int bits,
		   double secs, unsigned int period, unsigned int periods)
{
	struct pcm pcm = { .capture = 1 };
	unsigned long total = (unsigned long)(rate * secs), done = 0, i;
	double sum[2] = {}, sq[2] = {}, peak[2] = {}, t0, t;
	unsigned char hdr[44] = {};
	static int32_t buf32[4096 * 2];
	static int16_t buf16[4096 * 2];
	unsigned long zero = 0;
	FILE *f;
	int c;

	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}
	fwrite(hdr, 1, sizeof(hdr), f);

	if (pcm_open(&pcm, rate, bits, period, periods)) {
		fclose(f);
		return 1;
	}
	if (ioctl(pcm.fd, SNDRV_PCM_IOCTL_START) < 0) {
		fprintf(stderr, "START: %s\n", strerror(errno));
		fclose(f);
		return 1;
	}

	t0 = now();
	while (done < total) {
		unsigned long n = total - done > 4096 ? 4096 : total - done;
		void *buf = bits == 32 ? (void *)buf32 : (void *)buf16;

		if (pcm_read(&pcm, buf, n))
			break;
		for (i = 0; i < 2 * n; i++) {
			double s = bits == 32 ? buf32[i] / 2147483648.0 :
						buf16[i] / 32768.0;

			c = i & 1;
			sum[c] += s;
			sq[c] += s * s;
			if (fabs(s) > peak[c])
				peak[c] = fabs(s);
			zero += s == 0;
		}
		fwrite(buf, pcm.channels * bits / 8, n, f);
		done += n;
	}
	t = now() - t0;

	ioctl(pcm.fd, SNDRV_PCM_IOCTL_DROP);
	close(pcm.fd);
	wav_header(hdr, pcm.rate, bits, done * 2 * bits / 8);
	fseek(f, 0, SEEK_SET);
	fwrite(hdr, 1, sizeof(hdr), f);
	fclose(f);

	printf("recorded %lu frames in %.2f s (measured %.0f Hz), xruns %lu -> %s\n",
	       done, t, t > 0 ? done / t : 0, pcm.xruns, path);
	for (c = 0; done && c < 2; c++) {
		double mean = sum[c] / done, rms = sqrt(sq[c] / done);

		printf("ch%d: rms %.1f dBFS, peak %.1f dBFS, dc %+.3f%%%s\n", c,
		       db(rms), db(peak[c]), 100 * mean,
		       !peak[c] ? "  SILENT (all zero)" :
		       rms - fabs(mean) < 1e-5 ? "  DC only" : "");
	}
	if (done)
		printf("zero samples: %lu of %lu\n", zero, 2 * done);
	return done == total ? 0 : 1;
}

/* ---- controls ---- */

static int ctl_open(void)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
	fd = open(path, O_RDWR);
	if (fd < 0)
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
	return fd;
}

static int ctl_info(void)
{
	struct snd_ctl_card_info ci = {};
	struct snd_ctl_elem_list list = {};
	struct snd_ctl_elem_id *ids;
	unsigned int i;
	int fd = ctl_open();

	if (fd < 0)
		return 1;
	ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &ci);
	printf("card %d: %s (%s) driver %s\n", card, ci.id, ci.name, ci.driver);

	ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list);
	ids = calloc(list.count, sizeof(*ids));
	list.space = list.count;
	list.pids = ids;
	ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list);
	for (i = 0; i < list.used; i++) {
		struct snd_ctl_elem_info inf = { .id = ids[i] };
		struct snd_ctl_elem_value v = { .id = ids[i] };

		ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &inf);
		ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v);
		printf("  ctl '%s' type %d", ids[i].name, inf.type);
		if (inf.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
			printf(" [%ld..%ld]", inf.value.integer.min, inf.value.integer.max);
		printf(" = %ld\n", v.value.integer.value[0]);
	}
	close(fd);
	return 0;
}

static int ctl_set(const char *name, long val)
{
	struct snd_ctl_elem_value v = {};
	struct snd_ctl_elem_info inf = {};
	unsigned int i;
	int fd = ctl_open();

	if (fd < 0)
		return 1;
	v.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	snprintf((char *)v.id.name, sizeof(v.id.name), "%s", name);
	inf.id = v.id;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &inf) < 0) {
		fprintf(stderr, "no control '%s': %s\n", name, strerror(errno));
		return 1;
	}
	for (i = 0; i < inf.count && i < 128; i++)
		v.value.integer.value[i] = val;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0) {
		fprintf(stderr, "write '%s' = %ld: %s\n", name, val, strerror(errno));
		return 1;
	}
	printf("'%s' = %ld\n", name, val);
	close(fd);
	return 0;
}

/* ---- /dev/mem helpers: pad scan and register peek (read-only) ---- */

/*
 * An MMIO read of a power-gated Apple block can hang the SoC, so /dev/mem
 * reads are limited to always-on blocks: the AP GPIO pads and the PMGR
 * power-state registers (which the Linux PMGR driver reads all the time).
 */
#define GPIO_BASE	0x20f100000ULL
#define GPIO_PINS	208
static const struct { uint64_t base, len; } safe_mmio[] = {
	{ GPIO_BASE, GPIO_PINS * 4 },	/* AP GPIO pads */
	{ 0x20e080000ULL, 0x8020 },	/* PMGR ps registers 0x80000..0x88010 */
};

static int mmio_safe(uint64_t base, uint64_t len)
{
	unsigned int i;

	for (i = 0; i < sizeof(safe_mmio) / sizeof(safe_mmio[0]); i++)
		if (base >= safe_mmio[i].base && len <= safe_mmio[i].len &&
		    base - safe_mmio[i].base <= safe_mmio[i].len - len)
			return 1;
	fprintf(stderr, "refusing %#llx+%#llx: only GPIO pads and PMGR ps "
		"registers are always powered\n", (unsigned long long)base,
		(unsigned long long)len);
	return 0;
}

static volatile uint32_t *map_phys(uint64_t base, size_t len, void **map,
				   size_t *map_len)
{
	long pg = sysconf(_SC_PAGESIZE);
	uint64_t start = base & ~(uint64_t)(pg - 1);
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);

	if (fd < 0) {
		fprintf(stderr, "/dev/mem: %s\n", strerror(errno));
		return NULL;
	}
	*map_len = (base - start + len + pg - 1) & ~(pg - 1);
	*map = mmap(NULL, *map_len, PROT_READ, MAP_SHARED, fd, start);
	close(fd);
	if (*map == MAP_FAILED) {
		fprintf(stderr, "mmap %#llx: %s\n", (unsigned long long)start,
			strerror(errno));
		return NULL;
	}
	return (volatile uint32_t *)((char *)*map + (base - start));
}

/*
 * Apple GPIO pad registers (4 bytes per pin): bit 0 data / live level when
 * input-enabled, [3:1] mode, 4 (S8000: cleared for peripheral routing),
 * [6:5] periph, [8:7] pull, 9 input enable, [11:10] drive.
 * Samples every pin n times and flags pins whose level changes: clocks.
 */
static int pads_scan(unsigned int n)
{
	static unsigned int ones[GPIO_PINS], trans[GPIO_PINS];
	static uint32_t cfg[GPIO_PINS], last[GPIO_PINS];
	const uint64_t base = GPIO_BASE;
	const unsigned int count = GPIO_PINS;
	size_t map_len;
	void *map;
	volatile uint32_t *r = map_phys(base, count * 4, &map, &map_len);
	unsigned int i, p;

	if (!r)
		return 1;

	for (p = 0; p < count; p++)
		last[p] = cfg[p] = r[p];
	for (i = 0; i < n; i++) {
		for (p = 0; p < count; p++) {
			uint32_t v = r[p];

			ones[p] += v & 1;
			trans[p] += (v ^ last[p]) & 1;
			last[p] = v;
		}
	}

	printf("pads @%#llx, %u samples each\n", (unsigned long long)base, n);
	for (p = 0; p < count; p++) {
		uint32_t c = cfg[p];

		printf("pin %3u cfg %08x mode %u b4 %u periph %u pull %u ie %u ds %u"
		       " | high %u/%u edges %u%s\n", p, c, (c >> 1) & 7,
		       (c >> 4) & 1, (c >> 5) & 3, (c >> 7) & 3, (c >> 9) & 1,
		       (c >> 10) & 3, ones[p], n, trans[p],
		       trans[p] ? "  TOGGLING" :
		       !((c >> 9) & 1) ? "  (ie 0: level not visible)" : "");
	}
	munmap(map, map_len);
	return 0;
}

static int peek(uint64_t base, unsigned int words)
{
	size_t map_len;
	void *map;
	volatile uint32_t *r;
	unsigned int i;

	if (!words || !mmio_safe(base, (uint64_t)words * 4))
		return 1;
	r = map_phys(base, words * 4, &map, &map_len);
	if (!r)
		return 1;
	for (i = 0; i < words; i++)
		printf("%#llx = %08x\n", (unsigned long long)(base + 4 * i), r[i]);
	munmap(map, map_len);
	return 0;
}

/* ---- CS42L81 over spidev: 6D page reg 00 xx -> value in byte 4 ---- */

static int spi_read_regs(const char *dev, unsigned int mode, unsigned int hz,
			 char **regs, int n)
{
	uint8_t m = mode;
	uint32_t speed = hz;
	int fd = open(dev, O_RDWR), i;

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (ioctl(fd, SPI_IOC_WR_MODE, &m) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		fprintf(stderr, "spidev setup: %s\n", strerror(errno));
		return 1;
	}
	for (i = 0; i < n; i++) {
		unsigned int reg = strtoul(regs[i], NULL, 0);
		uint8_t tx[5] = { 0x6d, reg >> 8, reg & 0x7f, 0, 0 }, rx[5] = { 0 };
		struct spi_ioc_transfer t = {
			.tx_buf = (unsigned long)tx,
			.rx_buf = (unsigned long)rx,
			.len = 5,
			.speed_hz = speed,
			.bits_per_word = 8,
		};

		if (ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0) {
			fprintf(stderr, "transfer: %s\n", strerror(errno));
			return 1;
		}
		printf("mode %u reg 0x%04x = 0x%02x  (rx %02x %02x %02x %02x %02x)\n",
		       mode, reg, rx[4], rx[0], rx[1], rx[2], rx[3], rx[4]);
	}
	close(fd);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: a9audio info|ctl|play|tone|rec [opts]\n"
		"  common: -c CARD -d DEVICE -p PERIOD_FRAMES -n PERIODS\n"
		"  ctl NAME VALUE\n"
		"  play FILE.wav\n"
		"  tone [-r RATE] [-b 16|32] [-f HZ] [-s SECS] [-a DBFS] [-L|-R]\n"
		"  rec [-r RATE] [-b 16|32] [-s SECS] FILE.wav  (stereo capture + levels)\n"
		"  spi /dev/spidevB.C [-m MODE] [-r HZ] REG... (CS42L81 reads)\n"
		"  pads [-n SAMPLES]   (AP GPIO 0x20f100000, 208 pads, always on)\n"
		"  peek BASE WORDS     (read-only; GPIO pads and PMGR ps registers only)\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *cmd;
	unsigned int rate = 48000, bits = 16, period = 1024, periods = 4, mode = 0;
	double hz = 1000, secs = 3, dbfs = -12;
	int lr = 0, i, rate_set = 0;
	char *args[16];
	int na = 0;

	if (argc < 2)
		usage();
	cmd = argv[1];
	for (i = 2; i < argc; i++) {
		char *a = argv[i];

		if (a[0] == '-' && a[1] && !a[2] && strchr("cdpnrbfsam", a[1]) && i + 1 < argc) {
			char *v = argv[++i];

			switch (a[1]) {
			case 'c': card = atoi(v); break;
			case 'd': device = atoi(v); break;
			case 'p': period = atoi(v); break;
			case 'n': periods = atoi(v); break;
			case 'r': rate = atoi(v); rate_set = 1; break;
			case 'm': mode = atoi(v); break;
			case 'b': bits = atoi(v); break;
			case 'f': hz = atof(v); break;
			case 's': secs = atof(v); break;
			case 'a': dbfs = atof(v); break;
			}
		} else if (!strcmp(a, "-L") || !strcmp(a, "-R")) {
			lr = a[1];
		} else if (na < 16) {
			args[na++] = a;
		} else {
			usage();
		}
	}

	if (!strcmp(cmd, "info"))
		return ctl_info();
	if (!strcmp(cmd, "ctl") && na == 2)
		return ctl_set(args[0], strtol(args[1], NULL, 0));
	if (!strcmp(cmd, "play") && na == 1)
		return play_wav(args[0], period, periods);
	if (!strcmp(cmd, "tone"))
		return play_tone(rate, bits, hz, secs, dbfs, lr, period, periods);
	if (!strcmp(cmd, "rec") && na == 1)
		return rec_wav(args[0], rate, bits, secs, period, periods);
	if (!strcmp(cmd, "pads") && na == 0)
		return pads_scan(periods != 4 ? periods : 4000);	/* -n */
	if (!strcmp(cmd, "peek") && na == 2)
		return peek(strtoull(args[0], NULL, 0), strtoul(args[1], NULL, 0));
	if (!strcmp(cmd, "spi") && na >= 2)
		return spi_read_regs(args[0], mode, rate_set ? rate : 1000000,
				     args + 1, na - 1);
	usage();
	return 2;
}
