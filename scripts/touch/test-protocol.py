#!/usr/bin/env python3
"""Exercise actual N1 C helpers with private firmware and malformed reports.

The kernel source is read from the isolated touch tree into build/touch/.
This compiles the real functions with input stubs and runs
ASan/UBSan on the host; no device, Apple binary output, or kernel is executed.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/touch/tests'


def function(source, name):
    point = source.index(name + '(')
    start = source.rfind('\nstatic ', 0, point)
    end = source.index('\n}\n', point) + 3
    return source[start:end]


def main():
    source = subprocess.run([str(ROOT / 'scripts/build-kernel.sh'), '--run', 'touch',
                             'cat drivers/input/touchscreen/apple_n1.c'],
                            check=True, capture_output=True, text=True).stdout
    source_path = ROOT / 'build/touch/apple_n1.c'
    source_path.parent.mkdir(parents=True, exist_ok=True)
    source_path.write_text(source)
    touchscreen = subprocess.run([str(ROOT / 'scripts/build-kernel.sh'), '--run', 'touch',
                                  'cat drivers/input/touchscreen.c'],
                                 check=True, capture_output=True, text=True).stdout
    start = touchscreen.index('\nvoid touchscreen_report_pos(')
    end = touchscreen.index('\n}\n', start) + 3
    # Exercise the pinned kernel's real coordinate transform, not a stub.
    transform = function(touchscreen, 'touchscreen_apply_prop_to_x_y') + touchscreen[start:end]
    functions = '\n'.join(function(source, name) for name in (
        'n1_sum', 'n1_put32', 'n1_get32', 'n1_hbpp_word',
        'n1_validate_firmware', 'n1_upload_calibration', 'n1_parse_contacts', 'n1_irq_frame'))
    header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
#define N1_SLOTS 32
#define N1_CAL_SIZE 1024
#define MT_TOOL_FINGER 0
#define ABS_MT_TOUCH_MAJOR 0
#define ABS_MT_TOUCH_MINOR 1
#define ABS_MT_ORIENTATION 2
#define ABS_MT_PRESSURE 3
#define ABS_MT_POSITION_X 4
#define ABS_MT_POSITION_Y 5
#define ABS_X 6
#define ABS_Y 7
#define swap(a,b) do {unsigned int tmp=(a);(a)=(b);(b)=tmp;} while(0)
#define DECLARE_BITMAP(name, bits) unsigned long name[(bits) / (8 * sizeof(unsigned long))]
#define clamp(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
static u16 get_unaligned_be16(const void *p) {const u8 *b=p;return (u16)b[0]<<8|b[1];}
static u16 get_unaligned_le16(const void *p) {const u8 *b=p;return (u16)b[1]<<8|b[0];}
static void put_unaligned_le16(u16 n, void *p) {u8 *b=p;b[0]=n;b[1]=n>>8;}
static void put_unaligned_be16(u16 n, void *p) {u8 *b=p;b[0]=n>>8;b[1]=n;}
static void bitmap_zero(unsigned long *p, int bits) {memset(p,0,bits/8);}
static int test_and_set_bit(int bit, unsigned long *p) {
  unsigned int word=bit/(8*sizeof(*p)), shift=bit%(8*sizeof(*p));
  int set=!!(p[word] & (1UL<<shift)); p[word]|=1UL<<shift;return set;
}
struct firmware {size_t size; const u8 *data;};
struct fake_input {int changes, syncs, active, x, y, axes[4];};
#define input_dev fake_input
struct touchscreen_properties {unsigned int max_x,max_y;bool invert_x,invert_y,swap_x_y;};
struct apple_n1 {struct fake_input *input;struct touchscreen_properties props;s16 x_min,x_max,y_min,y_max;u8 *tx;void *atn;};
static int n1_send_blob(struct apple_n1 *, size_t);
static int n1_read_frame(struct apple_n1 *, bool);
static int gpiod_get_value_cansleep(void *);
static void usleep_range(int a, int b) {(void)a;(void)b;}
static int input_mt_get_slot_by_key(struct fake_input *i, int key) {(void)i;return key%N1_SLOTS;}
static void input_mt_slot(struct fake_input *i, int slot) {(void)slot;i->changes++;}
static bool input_mt_report_slot_state(struct fake_input *i, int tool, bool active) {(void)tool;i->active=active;return active;}
static void input_report_abs(struct fake_input *i, int code, int value) {
  if(code==ABS_MT_POSITION_X || code==ABS_X)i->x=value;
  else if(code==ABS_MT_POSITION_Y || code==ABS_Y)i->y=value;
  else {assert(code<4);i->axes[code]=value;}
}
static void input_mt_sync_frame(struct fake_input *i) {i->syncs++;}
static void input_sync(struct fake_input *i) {(void)i;}
'''
    tests = r'''
static int read_calls, read_status[3], atn_asserted;
static int n1_read_frame(struct apple_n1 *n1, bool events) {
  (void)n1;assert(events);assert(read_calls<3);return read_status[read_calls++];
}
static int gpiod_get_value_cansleep(void *gpio) {(void)gpio;return atn_asserted;}
static size_t calibration_offset;
static int calibration_packets, fail_packet=-1;
static u8 calibration_fixture[N1_CAL_SIZE];
static int n1_send_blob(struct apple_n1 *n1, size_t size) {
  size_t payload=size-16;
  assert(payload==(calibration_offset==0?1008:16));
  assert(!memcmp(n1->tx,"\x18\xe1\x30\x01",4));
  assert(get_unaligned_be16(n1->tx+4)==payload/4);
  assert(n1_get32(n1->tx+6)==0x10009000+calibration_offset);
  assert(get_unaligned_be16(n1->tx+10)==n1_sum(n1->tx+4,6));
  for(size_t i=0;i<payload;i+=2){
    assert(n1->tx[12+i]==calibration_fixture[calibration_offset+i+1]);
    assert(n1->tx[13+i]==calibration_fixture[calibration_offset+i]);
  }
  assert(n1_get32(n1->tx+12+payload)==n1_sum(calibration_fixture+calibration_offset,payload));
  calibration_offset+=payload;
  return calibration_packets++==fail_packet?-EIO:0;
}
static void le16(u8 *p, unsigned int n) {p[0]=n;p[1]=n>>8;}
int main(int argc, char **argv) {
  assert(argc==2);
  FILE *file=fopen(argv[1],"rb");assert(file);
  u8 *asset=malloc(57016);assert(asset);assert(fread(asset,1,57016,file)==57016);fclose(file);
  struct firmware firmware={57016,asset};
  assert(n1_validate_firmware(&firmware)==0);
  asset[12]^=1;assert(n1_validate_firmware(&firmware)==-EBADMSG);asset[12]^=1;
  asset[10]^=1;assert(n1_validate_firmware(&firmware)==-EBADMSG);asset[10]^=1;
  firmware.size=2;assert(n1_validate_firmware(&firmware)==-EINVAL);free(asset);
  u8 word[4];n1_put32(word,0x12345678);assert(!memcmp(word,"\x56\x78\x12\x34",4));
  assert(n1_get32(word)==0x12345678);assert(n1_hbpp_word(0x4bc1));assert(!n1_hbpp_word(0));
  struct fake_input input={0};struct apple_n1 n1={.input=&input,.x_min=-112,.x_max=14508,.y_min=-112,.y_max=19426};
  read_status[0]=read_status[1]=read_status[2]=-EIO;
  assert(n1_irq_frame(&n1)==-EIO && read_calls==3);
  read_calls=0;read_status[1]=1;
  assert(n1_irq_frame(&n1)==1 && read_calls==2);
  read_calls=0;read_status[0]=read_status[1]=read_status[2]=0;atn_asserted=0;
  assert(n1_irq_frame(&n1)==0 && read_calls==1);
  read_calls=0;atn_asserted=1;
  assert(n1_irq_frame(&n1)==-EPROTO && read_calls==3);
  n1.tx=malloc(65536);assert(n1.tx);
  for(int i=0;i<N1_CAL_SIZE;i++)calibration_fixture[i]=i;
  assert(n1_upload_calibration(&n1,calibration_fixture)==0);
  assert(calibration_packets==2 && calibration_offset==1024);
  calibration_packets=0;calibration_offset=0;fail_packet=0;
  assert(n1_upload_calibration(&n1,calibration_fixture)==-EIO);
  assert(calibration_packets==1 && calibration_offset==1008);free(n1.tx);
  u8 packet[128]={0};packet[0]=0x44;packet[2]=24;packet[16]=1;packet[17]=20;
  u8 *finger=packet+24;finger[0]=7;finger[1]=3;le16(finger+4,100);le16(finger+6,200);
  le16(finger+12,321);le16(finger+14,123);le16(finger+16,0xff00);le16(finger+18,777);
  assert(n1_parse_contacts(&n1,packet,44)==0);assert(input.active);
  assert(input.x==212 && input.y==312);assert(input.axes[ABS_MT_PRESSURE]==0);
  assert(input.axes[ABS_MT_TOUCH_MAJOR]==321 && input.axes[ABS_MT_ORIENTATION]==-256);
  n1.props=(struct touchscreen_properties){.max_x=14620,.max_y=19538,.invert_y=true};
  assert(n1_parse_contacts(&n1,packet,44)==0 && input.x==212 && input.y==19226);
  // Confirm all four portrait corners after the actual generic DT transform.
  const int raw[4][2]={{-112,19426},{14508,19426},{-112,-112},{14508,-112}};
  const int expected[4][2]={{0,0},{14620,0},{0,19538},{14620,19538}};
  for(int c=0;c<4;c++) {
    le16(finger+4,raw[c][0]);le16(finger+6,raw[c][1]);
    assert(n1_parse_contacts(&n1,packet,44)==0);
    assert(input.x==expected[c][0] && input.y==expected[c][1]);
  }
  n1.props=(struct touchscreen_properties){0};le16(finger+4,100);le16(finger+6,200);
  input=(struct fake_input){0};packet[16]=2;memcpy(packet+44,packet+24,20);
  assert(n1_parse_contacts(&n1,packet,64)==-EPROTO);assert(!input.changes);
  packet[16]=1;assert(n1_parse_contacts(&n1,packet,43)==-EPROTO);assert(!input.changes);
  packet[16]=0;packet[17]=0;assert(n1_parse_contacts(&n1,packet,24)==0);assert(input.syncs==1);
  packet[16]=1;packet[17]=32;le16(packet+14,4);memmove(packet+28,packet+24,20);
  le16(packet+28+26,888);
  assert(n1_parse_contacts(&n1,packet,60)==0);assert(input.axes[ABS_MT_PRESSURE]==888);
  // Deterministic malformed packet exploration with allocations of exact size.
  uint32_t rng=0x31415926;
  for (size_t size=0;size<512;size++) for(int trial=0;trial<100;trial++) {
    u8 *data=malloc(size?size:1);assert(data);
    for(size_t k=0;k<size;k++){rng=rng*1664525+1013904223;data[k]=rng>>24;}
    if(size) data[0]=0x43+(trial%3);
    n1_parse_contacts(&n1,data,size);free(data);
  }
  puts("PASS: actual C firmware/calibration helpers, bounded falling-edge IRQ retries, generic portrait Y transform/four corners, 20/32-byte contacts, duplicate/truncated/empty frames, 51200 malformed packets (ASan/UBSan)");
  return 0;
}
'''
    OUT.mkdir(parents=True, exist_ok=True)
    harness = OUT / 'protocol.c'
    binary = OUT / 'protocol'
    harness.write_text(header + transform + functions + tests)
    subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(harness), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(ROOT / 'build/firmware/lib/firmware/apple/j71t-n1.bin')], check=True)


if __name__ == '__main__':
    main()
