// SPDX-License-Identifier: GPL-2.0-only
/*
 * sep-bio-test: exercise the /dev/sep-bio ABI without fprintd.
 *
 * Usage: sep-bio-test info|list|enrol|verify
 * Build: make -C tools/sep
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../drivers/sep/uapi/apple-sep.h"

static const char *state_name(unsigned int s)
{
	switch (s) {
	case SEP_BIO_STATE_IDLE: return "IDLE";
	case SEP_BIO_STATE_PENDING: return "PENDING";
	case SEP_BIO_STATE_PROGRESS: return "PROGRESS";
	case SEP_BIO_STATE_DONE: return "DONE";
	case SEP_BIO_STATE_FAILED: return "FAILED";
	default: return "?";
	}
}

static int do_info(int fd)
{
	struct sep_bio_info info = { 0 };

	if (ioctl(fd, SEP_BIO_GET_INFO, &info)) {
		perror("SEP_BIO_GET_INFO");
		return 1;
	}
	printf("version=%u sensor_present=%u enrolled=%u capacity=%u stages=%u\n",
	       info.version, info.sensor_present, info.enrolled, info.capacity,
	       info.enroll_stages);
	return 0;
}

static int do_list(int fd)
{
	struct sep_bio_list list = { 0 };
	unsigned int i;

	if (ioctl(fd, SEP_BIO_LIST, &list)) {
		perror("SEP_BIO_LIST");
		return 1;
	}
	printf("identities=%u\n", list.count);
	for (i = 0; i < list.count && i < SEP_BIO_MAX_IDENTITIES; i++)
		printf("  %02x%02x%02x%02x  %s\n", list.id[i].uuid[0],
		       list.id[i].uuid[1], list.id[i].uuid[2],
		       list.id[i].uuid[3], list.id[i].label);
	return 0;
}

static int do_enrol(int fd)
{
	struct sep_bio_enrol_start start = { 0 };
	struct sep_bio_enrol_poll poll = { 0 };
	int i;

	strncpy((char *)start.label, "linux enrol", sizeof(start.label) - 1);

	if (ioctl(fd, SEP_BIO_ENROL_START, &start)) {
		perror("SEP_BIO_ENROL_START");
		return 1;
	}

	for (i = 0; i < 60; i++) {
		if (ioctl(fd, SEP_BIO_ENROL_POLL, &poll)) {
			perror("SEP_BIO_ENROL_POLL");
			return 1;
		}
		printf("state=%s stage=%u/%u status=0x%x guidance=%u progress=%u%%\n",
		       state_name(poll.state), poll.stage, poll.stages_total,
		       poll.status, poll.guidance, poll.progress_percent);
		if (poll.state == SEP_BIO_STATE_DONE ||
		    poll.state == SEP_BIO_STATE_FAILED)
			break;
		usleep(200000);
	}
	return poll.state == SEP_BIO_STATE_DONE ? 0 : 1;
}

static int do_verify(int fd)
{
	struct sep_bio_verify_start start = { 0 };
	struct sep_bio_verify_poll poll = { 0 };
	int i;

	if (ioctl(fd, SEP_BIO_VERIFY_START, &start)) {
		perror("SEP_BIO_VERIFY_START");
		return 1;
	}

	for (i = 0; i < 100; i++) {
		if (ioctl(fd, SEP_BIO_VERIFY_POLL, &poll)) {
			perror("SEP_BIO_VERIFY_POLL");
			return 1;
		}
		if (poll.state == SEP_BIO_STATE_DONE ||
		    poll.state == SEP_BIO_STATE_FAILED) {
			printf("state=%s result=%u status=0x%x guidance=%u\n",
			       state_name(poll.state), poll.result, poll.status,
			       poll.guidance);
			return poll.result == SEP_BIO_MATCH ? 0 : 1;
		}
		usleep(100000);
	}
	printf("timeout\n");
	return 1;
}

int main(int argc, char **argv)
{
	int fd, ret;

	if (argc != 2) {
		fprintf(stderr, "usage: %s info|list|enrol|verify\n", argv[0]);
		return 2;
	}

	fd = open("/dev/sep-bio", O_RDWR);
	if (fd < 0) {
		perror("/dev/sep-bio");
		return 1;
	}

	if (!strcmp(argv[1], "info"))
		ret = do_info(fd);
	else if (!strcmp(argv[1], "list"))
		ret = do_list(fd);
	else if (!strcmp(argv[1], "enrol"))
		ret = do_enrol(fd);
	else if (!strcmp(argv[1], "verify"))
		ret = do_verify(fd);
	else {
		fprintf(stderr, "unknown command %s\n", argv[1]);
		ret = 2;
	}

	close(fd);
	return ret;
}
