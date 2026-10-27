# Build the /dev/sep-bio test tool on the iPad (aarch64 Arch/Omarchy).
#
#   make -C tools/sep
#   ./tools/sep/sep-bio-test info
#
# No libfprint/fprintd needed: this talks to the kernel ABI directly.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

CONFORMANCE_CFLAGS = -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror \
	-Wformat=2 -Wshadow -Wpointer-arith -Wstrict-prototypes

sep-bio-test: sep-bio-test.c ../../drivers/sep/uapi/apple-sep.h
	$(CC) $(CFLAGS) -o $@ sep-bio-test.c

# Strict host build of the mock conformance harness:
#   docker run --rm --platform linux/arm64 -v "$PWD:/work" \
#       -w /work linux-a9-kbuild:latest make -C tools/sep sep-bio-conformance CC=clang
sep-bio-conformance: sep-bio-conformance.c ../../drivers/sep/uapi/apple-sep.h
	$(CC) $(CONFORMANCE_CFLAGS) $(CFLAGS) -o $@ sep-bio-conformance.c

clean:
	rm -f sep-bio-test sep-bio-conformance
