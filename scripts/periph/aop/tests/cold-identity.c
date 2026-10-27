/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual identity loader and tag writer, with private-firmware IO faked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "a9-aop-afk.h"
#include "a9-aop-memory.h"

#define AOP_ECID_FW "apple/aop/boot-ecid.bin"
struct firmware { const u8 *data; size_t size; };
struct a9_aop { int dev; };
static bool force_cold;
static unsigned int cold_test, requests, releases, erased;
static int request_error;
static u8 ecid[8] = { 0x23, 0x56, 0x89 }, generated[8];
static struct firmware identity = { ecid, sizeof(ecid) };

static int request_firmware_direct(const struct firmware **fw, const char *name, int dev)
{
	(void)dev;
	assert(!strcmp(name, AOP_ECID_FW));
	requests++;
	*fw = &identity;
	return request_error;
}

static void release_firmware(const struct firmware *fw)
{
	assert(fw == &identity);
	releases++;
}

static void get_random_bytes(u8 *data, size_t size)
{
	assert(size == 8);
	for (size_t i = 0; i < size; i++)
		data[i] = generated[i] = 0xa0 + i;
}

static void memzero_explicit(u8 *data, size_t size)
{
	memset(data, 0, size);
	erased++;
}

#include "identity-functions.h"

int main(int argc, char **argv)
{
	struct a9_aop aop = {};
	size_t size = 342336, table, length, ecid_at, nonce_at;
	u32 field_size;
	u8 *original = malloc(size), *image = malloc(size);
	FILE *file;

	assert(argc == 3 && original && image);
	file = fopen(argv[1], "rb");
	assert(file && fread(original, 1, size, file) == size);
	fclose(file);
	assert(!a9_fw_table(original, size, &table, &length));
	assert(!a9_fw_tag(original, table, length, A9_FW_TAG('E', 'C', 'I', 'D'), &ecid_at, &field_size));
	assert(!a9_fw_tag(original, table, length, A9_FW_TAG('N', 'O', 'N', 'C'), &nonce_at, &field_size));
	for (cold_test = 0; cold_test < 16; cold_test++) {
		if (!a9_aop_cold_test_valid(cold_test))
			continue;
		for (unsigned int forced = 0; forced < 2; forced++) {
			bool selected = forced && (cold_test & A9_COLD_ECID);

			force_cold = forced;
			memcpy(image, original, size);
			requests = releases = erased = 0;
			assert(!a9_aop_prepare_identity(&aop, image, size));
			assert(requests == selected && releases == selected && erased == selected);
			for (size_t i = 0; i < size; i++) {
				u8 expected = original[i];

				if (selected && i >= ecid_at && i < ecid_at + 8)
					expected = ecid[i - ecid_at];
				if (selected && (cold_test & A9_COLD_NONCE) && i >= nonce_at && i < nonce_at + 8)
					expected = generated[i - nonce_at];
				assert(image[i] == expected);
			}
		}
	}
	force_cold = true;
	cold_test = 13;
	for (unsigned int fault = 0; fault < 6; fault++) {
		memcpy(image, original, size);
		requests = releases = erased = 0;
		request_error = fault == 0 ? -ENOENT : 0;
		identity.size = fault == 1 ? 7 : fault == 2 ? 9 : 8;
		ecid[0] = 0x23;
		if (fault == 3)
			memset(ecid, 0, 8);
		if (fault == 4)
			put_unaligned_le32(A9_FW_TAG('N', 'O', 'P', 'E'), image + nonce_at - 8);
		if (fault == 5)
			image[0x20] ^= 1;
		u8 *before = malloc(size);
		assert(before);
		memcpy(before, image, size);
		assert(a9_aop_prepare_identity(&aop, image, size) < 0);
		assert(!memcmp(before, image, size));
		assert(requests == 1 && releases == (fault != 0) && erased == (fault != 0));
		free(before);
	}
	size = 37024;
	file = fopen(argv[2], "rb");
	assert(file && fread(original, 1, size, file) == size && fgetc(file) == EOF);
	fclose(file);
	memcpy(image, original, size);
	assert(!a9_aop_cold_ext_fence(image, size));
	for (size_t i = 0; i < size; i++) {
		u8 expected = original[i];

		if (i == A9_COLD_EXT_FENCE_OFFSET)
			expected = 0xfe;
		if (i == A9_COLD_EXT_FENCE_OFFSET + 1)
			expected = 0xe7;
		assert(image[i] == expected);
	}
	assert(a9_aop_cold_ext_fence(image, size) == -EILSEQ);
	assert(a9_aop_cold_ext_fence(original, A9_COLD_EXT_FENCE_OFFSET + 1) == -EILSEQ);
	assert(!memcmp(image, original, A9_COLD_EXT_FENCE_OFFSET));
	free(image);
	free(original);
	puts("PASS identity: actual loader/helper, exact per-tag bytes, NONC isolation, no synthesized ID, malformed/missing input refuses without patching");
	puts("PASS real pinned ext: actual fence helper changes exactly two bytes; short image and duplicate patch refused");
	return 0;
}
