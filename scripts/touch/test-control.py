#!/usr/bin/env python3
"""Run actual N1 control helpers against independent iOS-derived wire fixtures.

Offline only. Tests the execute/wake/discovery path, failure diagnostics and
bounded retries under ASan/UBSan. No Apple executable or device is run.
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
    functions = '\n'.join(function(source, name) for name in (
        'n1_sum', 'n1_put32', 'n1_dump_error', 'n1_control_xfer', 'n1_check_control',
        'n1_execute', 'n1_wake', 'n1_command16', 'n1_get_report_once', 'n1_get_report', 'n1_discover'))
    header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef int16_t s16; typedef uint32_t u32;
#define N1_RETRIES 5
#define N1_REPORT_MAX 512
#define N1_FRAME_MAX 4096
#define READ_ONCE(x) (x)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define time_before(a,b) ((long)((a)-(b))<0)
#define msecs_to_jiffies(x) (x)
static unsigned long jiffies;
static int delays25, sleeps2, sleeps1;
static void msleep(int ms) {jiffies+=ms;}
static void usleep_range(int lo,int hi) {(void)hi;jiffies+=lo/1000;sleeps2+=lo==2000;sleeps1+=lo==1000;}
static void udelay(int us) {assert(us==25);delays25++;}
struct device {int dummy;}; struct spi_device {struct device dev;};
struct apple_n1 {struct spi_device *spi;u8 *tx,*rx;unsigned int traced_controls;size_t control_length;
 unsigned long last_wake;bool wake_valid;u16 max_packet;s16 x_min,x_max,y_min,y_max;};
static bool trace;
static int infos, errors, dumps;
static char reason[32];
static u8 dumped[517]; static size_t dumped_size;
static void log_message(bool error,const char *fmt,...) {
  if(error) errors++; else infos++;
  va_list ap;va_start(ap,fmt);
  if(strstr(fmt,"%s mismatch:")) {
    (void)va_arg(ap,int);(void)va_arg(ap,int);
    snprintf(reason,sizeof(reason),"%s",va_arg(ap,const char *));
  }
  if(strstr(fmt,"rx[%zu/%zu]")) {
    (void)va_arg(ap,const char *);size_t offset=va_arg(ap,size_t);
    size_t len=va_arg(ap,size_t);int part=va_arg(ap,int);const u8 *p=va_arg(ap,const u8 *);
    assert(error && len<=sizeof(dumped) && part<=32 && offset+part<=len);
    memcpy(dumped+offset,p,part);dumped_size=len;dumps++;
  }
  va_end(ap);
}
#define dev_err(dev,...) ((void)(dev),log_message(true,__VA_ARGS__))
#define dev_info(dev,...) ((void)(dev),log_message(false,__VA_ARGS__))
static u16 get_unaligned_le16(const void *p) {const u8 *b=p;return (u16)b[1]<<8|b[0];}
static u16 get_unaligned_be16(const void *p) {const u8 *b=p;return (u16)b[0]<<8|b[1];}
static u32 get_unaligned_le32(const void *p) {const u8 *b=p;return (u32)b[3]<<24|(u32)b[2]<<16|(u32)b[1]<<8|b[0];}
static void put_unaligned_le16(u16 n,void *p) {u8 *b=p;b[0]=n;b[1]=n>>8;}
static void put_unaligned_be16(u16 n,void *p) {u8 *b=p;b[0]=n>>8;b[1]=n;}
static void put_unaligned_le32(u32 n,void *p) {u8 *b=p;b[0]=n;b[1]=n>>8;b[2]=n>>16;b[3]=n>>24;}
static int n1_xfer(struct apple_n1 *,size_t);
'''
    tests = r'''
struct exchange {u8 tx[517],rx[517];size_t len;int ret;};
static struct exchange exchanges[128];static int calls,queued;
static size_t unhex(const char *hex,u8 *p) {
  size_t n=strlen(hex)/2;assert(strlen(hex)%2==0 && n<=517);
  for(size_t i=0;i<n;i++){unsigned int b;assert(sscanf(hex+2*i,"%2x",&b)==1);p[i]=b;}
  return n;
}
static void add(const char *tx,const char *rx,int ret) {
  assert(queued<128);struct exchange *e=exchanges+queued++;memset(e,0,sizeof(*e));
  e->len=unhex(tx,e->tx);assert(e->len==unhex(rx,e->rx));e->ret=ret;
}
static int n1_xfer(struct apple_n1 *n1,size_t len) {
  assert(calls<queued);struct exchange *e=exchanges+calls++;
  if(e->len!=len || memcmp(e->tx,n1->tx,len)) {
    fprintf(stderr,"exchange %d: expected %zu bytes, got %zu\n",calls,e->len,len);
    for(size_t i=0;i<len;i++)fprintf(stderr,"%02x",n1->tx[i]);fputc('\n',stderr);
  }
  assert(e->len==len && !memcmp(e->tx,n1->tx,len));memcpy(n1->rx,e->rx,len);return e->ret;
}
static void reset(struct apple_n1 *n1) {
  calls=queued=infos=errors=dumps=delays25=sleeps2=sleeps1=0;jiffies=0;
  dumped_size=0;reason[0]=0;trace=false;n1->traced_controls=0;n1->wake_valid=false;
}
static void wake(void) {
  add("ee00000000000000000000000000ee00","00000000000000000000000000000000",0);
}
static void e2(void) {
  add("e200000000000000000000000000e200","00000000000000000000000000000000",0);
  add("e200000000000000000000000000e200","e2150196090000000000000000009701",0);
}
/* KC ea2b70/ea3110/ea3374: same E3 twice, then E6/E7 TX2 changes to1
 * while the original request checksum is retained. Live J71t descriptors.
 */
static void d1(void) {
  add("e3d1000000000000000000000000b401","00000000000000000000000000000000",0);
  add("e3d1000000000000000000000000b401","e3d1000100000000000000000000b501",0);
  add("e6d1000100000000000000000000b801","00000000000000000000000000000000",0);
  add("e6d1010100000000000000000000b801","e6d1001500000000000000000000cc01",0);
}
static void descriptors(void) {
  d1();
  add("e3d3000000000000000000000000b601","00000000000000000000000000000000",0);
  add("e3d3000000000000000000000000b601","e3d3000c0001281e021000281e96f702",0);
  add("e7d3000c00000000000000000000c601","00000000000000000000000000000000",0);
  add("e7d3010c0000000000000000000000c601","e7d30001281e021000281e96090000f802",0);
  add("e3d0000000000000000000000000b301","00000000000000000000000000000000",0);
  add("e3d0000000000000000000000000b301","e3d0000800000000000000000000bb01",0);
  add("e6d0000800000000000000000000be01","00000000000000000000000000000000",0);
  add("e6d0010800000000000000000000be01","e6d0000101002801001e00000000ff01",0);
  add("e3a10000000000000000000000008401","00000000000000000000000000000000",0);
  add("e3a10000000000000000000000008401","e3a10006000000000000000000008a01",0);
  add("e6a10006000000000000000000008d01","00000000000000000000000000000000",0);
  add("e6a10106000000000000000000008d01","e6a10000000500000200000000008e01",0);
  add("e3d9000000000000000000000000bc01","00000000000000000000000000000000",0);
  add("e3d9000000000000000000000000bc01","e3d9001000000000000000000000cc01",0);
  add("e7d9001000000000000000000000d001","00000000000000000000000000000000",0);
  add("e7d90110000000000000000000000000000000d001","e7d90099390000c94c000090ff90ffac38e24bd608",0);
}
static void corrupt_info(void) {
  add("e3d1000000000000000000000000b401","00000000000000000000000000000000",0);
  add("e3d1000000000000000000000000b401","e3d10001000000000000000000000000",0);
}
int main(void) {
  struct spi_device spi={0};u8 tx[65536],rx[65536],data[512];
  struct apple_n1 n1={.spi=&spi,.tx=tx,.rx=rx};
  reset(&n1);trace=true;
  add("1d5334001000000100000045","000000000000000000000000",0);
  assert(!n1_execute(&n1));assert(calls==queued && jiffies==42 && infos==2);
  reset(&n1);trace=true;
  add("1d5334001000000100000045","000000000000000000000000",-EIO);
  assert(n1_execute(&n1)==-EIO && !jiffies && errors==1);
  reset(&n1);trace=true;wake();e2();descriptors();
  assert(!n1_discover(&n1) && calls==queued && errors==0);
  assert(n1.max_packet==2454 && n1.x_min==-112 && n1.x_max==14508);
  assert(n1.y_min==-112 && n1.y_max==19426 && delays25==10 && sleeps2==6);
  assert(n1.traced_controls==23 && infos==47);
  /* Duplicate wake suppressed, but it is sent again after100ms. */
  assert(!n1_wake(&n1) && calls==queued);jiffies=101;wake();assert(!n1_wake(&n1) && calls==queued);
  /* Checksummed but semantically wrong descriptors identify the field and
   * dump the full original wire packet, not only its copied payload. */
  reset(&n1);wake();e2();exchanges[2].rx[2]=2;exchanges[2].rx[14]++;
  assert(n1_discover(&n1)==-EPROTO && calls==3 && dumped_size==16);
  reset(&n1);wake();e2();descriptors();exchanges[10].rx[3]=0;exchanges[10].rx[15]--;
  assert(n1_discover(&n1)==-EPROTO && calls==11 && dumped_size==17);
  assert(!memcmp(dumped,exchanges[10].rx,17));
  reset(&n1);wake();e2();descriptors();exchanges[22].rx[3]--;exchanges[22].rx[19]--;
  assert(n1_discover(&n1)==-EPROTO && calls==queued && dumped_size==21);
  assert(!memcmp(dumped,exchanges[22].rx,21));
  reset(&n1);wake();
  for(int i=0;i<4;i++) {
    add("e200000000000000000000000000e200","00000000000000000000000000000000",0);
    add("e200000000000000000000000000e200","00000000000000000000000000000000",0);
  }
  assert(n1_discover(&n1)==-EBADMSG && calls==queued && dumps==4);
  assert(!strcmp(reason,"echo") && dumped_size==16 && !memcmp(dumped,rx,16));
  /* A valid-echo bad checksum gets a different reason and raw bytes,
   * including a long packet that must be dumped in multiple lines. */
  memset(rx,0,517);rx[0]=0xe7;
  assert(n1_check_control(&n1,0xe7,0xd9,517)==-EBADMSG);
  assert(!strcmp(reason,"checksum") && dumped_size==517 && !memcmp(dumped,rx,517));
  reset(&n1);n1.wake_valid=true;n1.last_wake=0;
  for(int i=0;i<4;i++)corrupt_info();d1();
  assert(!n1_get_report(&n1,0xd1,data,1) && calls==queued && data[0]==21);
  assert(sleeps2==5 && sleeps1==4 && dumps==4);
  reset(&n1);n1.wake_valid=true;n1.last_wake=0;
  for(int i=0;i<5;i++)corrupt_info();
  assert(n1_get_report(&n1,0xd1,data,1)==-EBADMSG && calls==queued && dumps==5);
  /* Full GET corruption consumes the SAME budget as E3 corruption. */
  reset(&n1);n1.wake_valid=true;n1.last_wake=0;d1();exchanges[3].rx[14]^=1;d1();
  assert(!n1_get_report(&n1,0xd1,data,1) && calls==queued && dumps==1);
  /* Bad lengths/status and transport errors abort without multiplying timeouts. */
  reset(&n1);n1.wake_valid=true;n1.last_wake=0;
  add("e3d1000000000000000000000000b401","00000000000000000000000000000000",-ETIMEDOUT);
  assert(n1_get_report(&n1,0xd1,data,1)==-ETIMEDOUT && calls==queued && errors==1);
  reset(&n1);n1.wake_valid=true;n1.last_wake=0;
  add("e3d1000000000000000000000000b401","00000000000000000000000000000000",0);
  add("e3d1000000000000000000000000b401","e3d1000102000000000000000000b701",0);
  assert(n1_get_report(&n1,0xd1,data,1)==-EPROTO && calls==queued && dumps==1);
  /* Trace cap is independent of error dumps and keeps control logging bounded. */
  reset(&n1);trace=true;memset(tx,0,sizeof(tx));
  for(int i=0;i<65;i++)add("0000","0000",0);
  for(int i=0;i<65;i++)assert(!n1_control_xfer(&n1,"test",2));
  assert(n1.traced_controls==64 && infos==128 && calls==queued);
  puts("PASS: actual C execute/wake/E2/E3/E6/E7 discovery, iOS wire fixtures, 2ms waits, 100ms wake suppression, bounded retries, echo/checksum/raw-packet diagnostics and trace cap (ASan/UBSan)");
}
'''
    OUT.mkdir(parents=True, exist_ok=True)
    harness = OUT / 'control.c'
    binary = OUT / 'control'
    harness.write_text(header + functions + tests)
    subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(harness), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
