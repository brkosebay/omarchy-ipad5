// SPDX-License-Identifier: GPL-2.0-only
/* Read-only evdev capability/axis audit for the owner-run J71t test. */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int bit(const unsigned char *bits, unsigned int n)
{
	return !!(bits[n / 8] & (1u << (n % 8)));
}

int main(int argc, char **argv)
{
	unsigned char props[(INPUT_PROP_MAX + 8) / 8] = {0};
	unsigned char keys[(KEY_MAX + 8) / 8] = {0};
	unsigned char axes[(ABS_MAX + 8) / 8] = {0};
	char name[256] = {0};
	const struct { unsigned int code; const char *name; int max, res; } checks[] = {
		{ABS_X, "ABS_X", 14620, 100},
		{ABS_Y, "ABS_Y", 19538, 100},
		{ABS_MT_POSITION_X, "ABS_MT_POSITION_X", 14620, 100},
		{ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y", 19538, 100},
		{ABS_MT_SLOT, "ABS_MT_SLOT", 31, 0},
	};
	int fd, failed = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/input/eventN\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
	    ioctl(fd, EVIOCGPROP(sizeof(props)), props) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(axes)), axes) < 0) {
		perror("evdev capability ioctl");
		close(fd);
		return 1;
	}
	name[sizeof(name) - 1] = 0;
	printf("%s: %s\nINPUT_PROP_DIRECT=%d INPUT_PROP_POINTER=%d BTN_TOUCH=%d\n",
	       argv[1], name, bit(props, INPUT_PROP_DIRECT),
	       bit(props, INPUT_PROP_POINTER), bit(keys, BTN_TOUCH));
	failed |= strcmp(name, "Apple N1 Touchscreen") != 0 ||
		!bit(props, INPUT_PROP_DIRECT) || bit(props, INPUT_PROP_POINTER) ||
		!bit(keys, BTN_TOUCH) || !bit(axes, ABS_MT_TRACKING_ID);
	for (unsigned int i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		struct input_absinfo info;

		if (!bit(axes, checks[i].code) ||
		    ioctl(fd, EVIOCGABS(checks[i].code), &info) < 0) {
			fprintf(stderr, "missing/failed %s\n", checks[i].name);
			failed = 1;
			continue;
		}
		printf("%s min=%d max=%d resolution=%d fuzz=%d flat=%d\n",
		       checks[i].name, info.minimum, info.maximum, info.resolution,
		       info.fuzz, info.flat);
		failed |= info.minimum != 0 || info.maximum != checks[i].max ||
			info.resolution != checks[i].res;
	}
	close(fd);
	printf("%s: J71t direct-touch capabilities/ranges/resolution\n", failed ? "FAIL" : "PASS");
	return !!failed;
}
