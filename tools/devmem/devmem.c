// Minimal BusyBox-compatible devmem: devmem ADDRESS [WIDTH [VALUE]]
// WIDTH is 8/16/32/64 bits (default 32). Reads print 0x%0*X like BusyBox.
// Uses mmap of /dev/mem with properly sized volatile accesses (MMIO-safe),
// and the runtime page size (the iPad kernel uses 16K pages).
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 4) {
		fprintf(stderr, "usage: %s ADDRESS [WIDTH [VALUE]]\n", argv[0]);
		return 1;
	}
	uint64_t addr = strtoull(argv[1], NULL, 0);
	int width = argc > 2 ? atoi(argv[2]) : 32;
	if (width != 8 && width != 16 && width != 32 && width != 64) {
		fprintf(stderr, "bad width %d\n", width);
		return 1;
	}
	if (addr % (width / 8)) {
		fprintf(stderr, "unaligned address for width %d\n", width);
		return 1;
	}

	int fd = open("/dev/mem", argc > 3 ? O_RDWR | O_SYNC : O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	long page = sysconf(_SC_PAGESIZE);
	uint64_t base = addr & ~(uint64_t)(page - 1);
	void *map = mmap(NULL, page, argc > 3 ? PROT_READ | PROT_WRITE : PROT_READ,
			 MAP_SHARED, fd, (off_t)base);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	volatile void *p = (volatile char *)map + (addr - base);

	if (argc > 3) {
		uint64_t v = strtoull(argv[3], NULL, 0);
		switch (width) {
		case 8: *(volatile uint8_t *)p = v; break;
		case 16: *(volatile uint16_t *)p = v; break;
		case 32: *(volatile uint32_t *)p = v; break;
		case 64: *(volatile uint64_t *)p = v; break;
		}
	} else {
		uint64_t v = 0;
		switch (width) {
		case 8: v = *(volatile uint8_t *)p; break;
		case 16: v = *(volatile uint16_t *)p; break;
		case 32: v = *(volatile uint32_t *)p; break;
		case 64: v = *(volatile uint64_t *)p; break;
		}
		printf("0x%0*" PRIX64 "\n", width / 4, v);
	}
	munmap(map, page);
	close(fd);
	return 0;
}
