#!/usr/bin/env python3
"""Exercise the actual J71t address helper with synthetic firmware/HCI faults.

Run in the kernel build container via scripts/build-kernel.sh --run periph.
No device access; no real Bluetooth identity appears in the fixtures/output.
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TREE = Path('/src/wt/periph')
OUT = ROOT / 'build/periph/round4-bt'

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef struct { u8 b[6]; } bdaddr_t;
static const bdaddr_t zero;
#define BDADDR_ANY (&zero)
#define HCI_QUIRK_BDADDR_PROPERTY_BROKEN 0
#define HCI_QUIRK_INVALID_BDADDR 1
#define HCI_QUIRK_USE_BDADDR_PROPERTY 2
#define HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER 3
#define HCI_OP_READ_BD_ADDR 0x1009
#define HCI_OP_LE_READ_TRANSMIT_POWER 0x204b
#define HCI_INIT_TIMEOUT 1000
#define HCI_CMD_TIMEOUT 1000
#define IS_ERR(p) ((intptr_t)(p) < 0 && (intptr_t)(p) >= -4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define bt_dev_err(...) ((void)0)
#define bt_dev_info(...) ((void)0)
struct device { struct device *parent; void *of_node; };
struct hci_dev { struct device dev; bdaddr_t public_addr; unsigned int quirks; u8 commands[64]; };
struct firmware { const u8 *data; size_t size; };
struct fwnode_handle { int unused; };
struct hci_rp_read_bd_addr { u8 status; bdaddr_t bdaddr; };
struct sk_buff { size_t len; u8 *data; };
static struct hci_rp_read_bd_addr reply;
static struct sk_buff skb;
static struct firmware fw;
static bdaddr_t property, programmed;
static struct device parent;
static struct hci_dev dev;
static int compatible, property_count, property_error, fw_error, set_error;
static int read_error, read_status, read_length, read_mismatch;
static int fw_calls, release_calls, set_calls, read_calls, free_calls;
static int unsupported_calls;
static int assertions;
static int bacmp(const bdaddr_t *a, const bdaddr_t *b) { return memcmp(a, b, 6); }
static void bacpy(bdaddr_t *a, const bdaddr_t *b) { memcpy(a, b, 6); }
static void baswap(bdaddr_t *a, const bdaddr_t *b) {
    for (int i = 0; i < 6; i++) a->b[i] = b->b[5-i];
}
static int of_device_is_compatible(void *node, const char *name) {
    assert(!strcmp(name, "apple,j71t-bcm4355-bt")); return compatible;
}
static struct fwnode_handle *dev_fwnode(struct device *d) { return NULL; }
static int fwnode_property_count_u8(struct fwnode_handle *f, const char *n) {
    assert(!strcmp(n, "local-bd-address")); return property_count;
}
static int fwnode_property_read_u8_array(struct fwnode_handle *f, const char *n,
                                       u8 *data, size_t size) {
    assert(size == 6); memcpy(data, &property, size); return property_error;
}
static bool hci_test_quirk(struct hci_dev *d, int bit) { return d->quirks & (1U << bit); }
static void hci_clear_quirk(struct hci_dev *d, int bit) { d->quirks &= ~(1U << bit); }
static int request_firmware_direct(const struct firmware **result, const char *name,
                                   struct device *d) {
    assert(!strcmp(name, "apple/j71t-BMac.bin")); fw_calls++;
    *result = &fw; return fw_error;
}
static void release_firmware(const struct firmware *f) { release_calls++; }
static int btbcm_set_bdaddr(struct hci_dev *d, const bdaddr_t *a) {
    set_calls++; bacpy(&programmed, a); return set_error;
}
static struct sk_buff *__hci_cmd_sync(struct hci_dev *d, unsigned int op,
                                     int len, void *data, int timeout) {
    assert(op == HCI_OP_READ_BD_ADDR); read_calls++;
    if (read_error) return (void *)(intptr_t)read_error;
    reply.status = read_status; bacpy(&reply.bdaddr, &programmed);
    if (read_mismatch) reply.bdaddr.b[0] ^= 1;
    skb.len = read_length; skb.data = (u8 *)&reply; return &skb;
}
static void kfree_skb(struct sk_buff *s) { free_calls++; }
static int __hci_cmd_sync_status(struct hci_dev *d, unsigned int op,
                                int len, void *data, int timeout) {
    assert(op == HCI_OP_LE_READ_TRANSMIT_POWER); unsupported_calls++; return 0;
}
'''

TESTS = r'''
static const bdaddr_t wire = {{0x50, 0x40, 0x30, 0x20, 0x10, 0x02}};
static const bdaddr_t display = {{0x02, 0x10, 0x20, 0x30, 0x40, 0x50}};
#define check(v) do { assert(v); assertions++; } while (0)
static void reset(void) {
    memset(&dev, 0, sizeof(dev)); dev.dev.parent = &parent;
    dev.quirks = (1U << HCI_QUIRK_INVALID_BDADDR) | (1U << HCI_QUIRK_USE_BDADDR_PROPERTY);
    compatible=1; property_count=6; property_error=0; bacpy(&property,&wire);
    fw.data=(const u8 *)&display; fw.size=6; fw_error=0; set_error=0;
    read_error=0; read_status=0; read_length=sizeof(reply); read_mismatch=0;
    fw_calls=0; release_calls=0; set_calls=0; read_calls=0; free_calls=0;
}
static void success(bool fallback) {
    check(btbcm_j71t_set_bdaddr(&dev)==0);
    check(!bacmp(&programmed,&wire)); check(!bacmp(&dev.public_addr,&wire));
    check(set_calls==1 && read_calls==1 && free_calls==1);
    check(!hci_test_quirk(&dev,HCI_QUIRK_INVALID_BDADDR));
    check(!hci_test_quirk(&dev,HCI_QUIRK_USE_BDADDR_PROPERTY));
    check(fw_calls == fallback); check(release_calls == fallback);
}
int main(void) {
    reset(); success(false);
    reset(); compatible=0; check(!btbcm_j71t_set_bdaddr(&dev));
    check(!fw_calls && !set_calls && !read_calls);
    reset(); dev.dev.parent=NULL; check(!btbcm_j71t_set_bdaddr(&dev));
    check(!fw_calls && !set_calls && !read_calls);
    reset(); property_count=-ENOENT; success(true);
    reset(); property_count=5; success(true);
    reset(); property_count=7; success(true);
    reset(); property_error=-EINVAL; success(true);
    reset(); memset(&property,0,6); success(true);
    reset(); memset(&property,0xff,6); success(true);
    reset(); property.b[5]=3; success(true);
    reset(); bacpy(&property,&display);
    dev.quirks |= 1U << HCI_QUIRK_BDADDR_PROPERTY_BROKEN; success(false);
    reset(); property_count=-ENOENT; fw_error=-ENOENT;
    check(btbcm_j71t_set_bdaddr(&dev)==-ENOENT);
    check(fw_calls==1 && !release_calls && !set_calls && !read_calls);
    for (int size=0; size<=7; size++) {
        if (size==6) continue;
        reset(); property_count=-ENOENT; fw.size=size;
        check(btbcm_j71t_set_bdaddr(&dev)==-EINVAL);
        check(release_calls==1 && !set_calls && !read_calls);
    }
    reset(); property_count=-ENOENT; fw.data=(const u8 *)&zero;
    check(btbcm_j71t_set_bdaddr(&dev)==-EINVAL); check(!set_calls && release_calls==1);
    reset(); property_count=-ENOENT; bdaddr_t invalid=display; invalid.b[0]=3;
    fw.data=(const u8 *)&invalid;
    check(btbcm_j71t_set_bdaddr(&dev)==-EINVAL); check(!set_calls && release_calls==1);
    reset(); set_error=-EIO;
    check(btbcm_j71t_set_bdaddr(&dev)==-EIO); check(set_calls==1 && !read_calls);
    reset(); read_error=-ETIMEDOUT;
    check(btbcm_j71t_set_bdaddr(&dev)==-ETIMEDOUT); check(read_calls==1 && !free_calls);
    reset(); read_length=sizeof(reply)-1;
    check(btbcm_j71t_set_bdaddr(&dev)==-EIO); check(free_calls==1);
    reset(); read_status=0x0c;
    check(btbcm_j71t_set_bdaddr(&dev)==-EIO); check(free_calls==1);
    check(!bacmp(&dev.public_addr,BDADDR_ANY));
    reset(); read_mismatch=1;
    check(btbcm_j71t_set_bdaddr(&dev)==-EIO); check(free_calls==1);
    check(hci_test_quirk(&dev,HCI_QUIRK_INVALID_BDADDR));
    reset(); dev.commands[38]=0x80;
    dev.quirks |= 1U << HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER;
    check(!hci_le_read_tx_power_sync(&dev)); check(!unsupported_calls);
    hci_clear_quirk(&dev,HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER);
    check(!hci_le_read_tx_power_sync(&dev)); check(unsupported_calls==1);
    dev.commands[38]=0; check(!hci_le_read_tx_power_sync(&dev)); check(unsupported_calls==1);
    printf("PASS: %d assertions: DT preference/order, private fallback, errors/readback\n", assertions);
    return 0;
}
'''


def main():
    source = (TREE / 'drivers/bluetooth/btbcm.c').read_text()
    start = source.index('/* Public identities are in HCI byte order here;')
    end = source.index('\nint btbcm_finalize(', start)
    helper = source[start:end]
    finalize = source[end:source.index('EXPORT_SYMBOL_GPL(btbcm_finalize);', end)]
    assert finalize.index('btbcm_j71t_set_bdaddr(hdev)') < finalize.index('btbcm_check_bdaddr(hdev)')
    sync = (TREE / 'net/bluetooth/hci_sync.c').read_text()
    skip_start = sync.index('static int hci_le_read_tx_power_sync(')
    skip_end = sync.index('\n}', skip_start)
    assert 'HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER' in sync[skip_start:skip_end]
    assert 'but not supported.", true }' in sync
    assert 'bt_dev_dbg(hdev, "%s", hci_broken_table[i].desc)' in sync
    assert 'bt_dev_warn(hdev, "%s", hci_broken_table[i].desc)' in sync
    OUT.mkdir(parents=True, exist_ok=True)
    c_path = OUT / 'identity-test.c'
    binary = OUT / 'identity-test'
    c_path.write_text(STUBS + helper + sync[skip_start:skip_end + 2] + TESTS)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-Wno-unused-but-set-variable', '-g0', '-O1',
                    '-fsanitize=address,undefined', str(c_path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})


if __name__ == '__main__':
    main()
