#!/usr/bin/env python3
"""Host test of actual N1 remove, contact release and power-off ordering."""
from pathlib import Path
import runpy
import subprocess
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/touch/tests'
extract = runpy.run_path(str(ROOT / 'scripts/touch/test-protocol.py'))['function']
source = subprocess.run([str(ROOT / 'scripts/build-kernel.sh'), '--run', 'touch',
                         'cat drivers/input/touchscreen/apple_n1.c'],
                        check=True, capture_output=True, text=True).stdout
header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define N1_SLOTS 32
#define MT_TOOL_FINGER 0
struct device {int unused;};
struct apple_n1;
struct spi_device {struct device dev;int irq;struct apple_n1 *data;};
struct apple_n1 {struct spi_device *spi;void *input,*reset,*ldo,*pins,*sync_off,*clk,*vdd;bool powered,irq_requested;};
static int order,irq_frees,releases,syncs,stops;
static bool simulated_masked_irq;
static void devm_free_irq(struct device *dev,int irq,struct apple_n1 *n1) {
  (void)dev;assert(irq==62 && n1->powered && !order);
  // Model synchronization of either a running thread or an already masked IRQ.
  (void)simulated_masked_irq;order=1;irq_frees++;
}
static void input_mt_slot(void *input,int i) {(void)input;assert(order>=1 && i>=0 && i<32);}
static void input_mt_report_slot_state(void *input,int tool,bool active) {
  (void)input;assert(order>=1 && tool==0 && !active);releases++;
}
static void input_mt_sync_frame(void *input) {(void)input;syncs++;}
static void input_sync(void *input) {(void)input;}
static void gpiod_set_value_cansleep(void *gpio,int v) {
  if(gpio==(void *)1){assert(v==1 && order==1 && releases==32);order=2;}
  else {assert(gpio==(void *)2 && v==0 && order==4);order=5;}
}
static int pinctrl_select_state(void *pins,void *state) {(void)pins;(void)state;assert(order==2);order=3;return 0;}
static void clk_disable_unprepare(void *clk) {(void)clk;assert(order==3);order=4;}
static void regulator_disable(void *vdd) {(void)vdd;assert(order==5);order=6;stops++;}
static struct apple_n1 *spi_get_drvdata(struct spi_device *spi) {return spi->data;}
#define dev_warn(dev,...) ((void)(dev))
#define dev_info(dev,...) ((void)(dev))
'''
functions = '\n'.join(extract(source, n) for n in ('n1_release_contacts', 'n1_power_off', 'n1_remove'))
tests = r'''
int main(void) {
  for(int masked=0;masked<2;masked++) {
    struct spi_device spi={.irq=62};struct apple_n1 n1={.spi=&spi,.input=(void *)3,
      .reset=(void *)1,.ldo=(void *)2,.powered=true,.irq_requested=true};spi.data=&n1;
    order=irq_frees=releases=syncs=stops=0;simulated_masked_irq=masked;
    n1_remove(&spi);assert(order==6 && irq_frees==1 && releases==32 && syncs==1 && stops==1);
    assert(!n1.powered && !n1.irq_requested);
    n1_remove(&spi);assert(irq_frees==1 && stops==1); // shutdown/stop is idempotent
  }
  struct spi_device spi={0};struct apple_n1 diagnostic={.spi=&spi};spi.data=&diagnostic;
  order=irq_frees=releases=stops=0;n1_remove(&spi);
  assert(!order && !irq_frees && !releases && !stops);
  puts("PASS: actual C cleanup frees/synchronizes IRQ before releasing 32 contacts and reset/clock/LDO/analog power, including masked IRQ and repeated stop (ASan/UBSan)");
}
'''
OUT.mkdir(parents=True, exist_ok=True)
path = OUT / 'lifecycle.c'
path.write_text(header + functions + tests)
subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', str(path), '-o', str(OUT / 'lifecycle')], check=True)
subprocess.run([str(OUT / 'lifecycle')], check=True)
