#!/usr/bin/env python3
"""Run the actual guarded PMIC ADC code against a scripted, write-audited regmap."""
import importlib.util
from pathlib import Path
import re
import subprocess
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('extract', Path(__file__).with_name('test-charging.py'))
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
#define div_u64(a,b) ((a)/(b))
struct regmap {int unused;};
struct apple_tristar {
    struct regmap *pmic; bool adc_poisoned; const char *detect_stage; int adc_status;
    const char *adc_fault_stage; unsigned adc_fault_attach, attach_id;
};
static int seq[1100], nseq, cursor, starts, reads, write_error, bulk_error;
static u8 result[2];
/* Time advances virtually; the actual polling predicate and driver code run. */
#define read_poll_timeout(op,val,cond,delay,timeout,sleep,...) ({ \
    int rc=-ETIMEDOUT; \
    for(unsigned i=0;i<=(timeout)/(delay);i++){val=op(__VA_ARGS__);if(cond){rc=0;break;}} \
    rc; })
static int regmap_read(struct regmap *m,unsigned reg,unsigned *value)
{(void)m;assert(reg==0x156);reads++;int r=seq[cursor<nseq?cursor++:nseq-1];
 if(r<0)return r;*value=(unsigned)r;return 0;}
static int regmap_write(struct regmap *m,unsigned reg,unsigned value)
{(void)m;assert(reg==0x4000 && value==1);starts++;return write_error;}
static int regmap_bulk_read(struct regmap *m,unsigned reg,void *data,unsigned n)
{(void)m;assert(reg==0x4001 && n==2);if(bulk_error)return bulk_error;
 memcpy(data,result,2);return 0;}
'''
TESTS = r'''
static struct apple_tristar ts;
static void reset(void)
{memset(&ts,0,sizeof(ts));seq[0]=0;seq[1]=0;seq[2]=1;nseq=3;
 cursor=starts=reads=write_error=bulk_error=0;result[0]=0;result[1]=12;}
int main(void)
{
 reset();assert(apple_tristar_adc_mv(&ts)==274 && starts==1 && !ts.adc_poisoned);
 assert(ts.adc_status==1 && !strcmp(ts.detect_stage,"adc-result"));
 reset();seq[0]=1;nseq=1;
 assert(apple_tristar_adc_mv(&ts)==-ETIMEDOUT && !starts && !ts.adc_poisoned);
 assert(!strcmp(ts.detect_stage,"adc-idle"));
 reset();seq[0]=-EIO;nseq=1;
 assert(apple_tristar_adc_mv(&ts)==-EIO && !starts && !ts.adc_poisoned);
 reset();ts.attach_id=7;write_error=-EIO;
 assert(apple_tristar_adc_mv(&ts)==-EIO && starts==1 && ts.adc_poisoned);
 assert(!strcmp(ts.detect_stage,"adc-trigger") && ts.adc_fault_attach==7);
 assert(!strcmp(ts.adc_fault_stage,"adc-trigger"));
 reset();seq[1]=0;nseq=2;
 assert(apple_tristar_adc_mv(&ts)==-ETIMEDOUT && ts.adc_poisoned);
 assert(!strcmp(ts.detect_stage,"adc-done"));
 int r=reads;seq[1]=1; /* late completion cannot authorize another trigger */
 assert(apple_tristar_adc_mv(&ts)==-EIO && starts==1 && reads==r);
 reset();seq[1]=-EIO;nseq=2;
 assert(apple_tristar_adc_mv(&ts)==-EIO && ts.adc_poisoned);
 reset();bulk_error=-EIO;
 assert(apple_tristar_adc_mv(&ts)==-EIO && !ts.adc_poisoned);
 reset();result[0]=0x80;
 assert(apple_tristar_adc_mv(&ts)==-ENODATA && !ts.adc_poisoned);
 /* Fresh LOW then HIGH for both pins: no stale pin result reused. */
 reset();assert(apple_tristar_adc_mv(&ts)==274);
 cursor=0;result[1]=25;
 assert(apple_tristar_adc_mv(&ts)==528 && starts==2);
 puts("PASS charge8 actual ADC: low/high completion, idle timeout/I2C failure without writes, trigger/done faults poison, late completion rejected, invalid/result errors and two fresh conversions");
 return 0;
}
'''
def main():
    src = (ROOT / 'scripts/periph/kernel/apple_tristar_power.c').read_text()
    constants = '\n'.join(re.findall(r'^#define D2365_ADC_.*$', src, re.M))
    body = '\n'.join(extract.function_text(src, f) for f in
                     ('apple_tristar_pmic_read', 'apple_tristar_adc_mv'))
    out = ROOT / 'build/charge8/tests'
    out.mkdir(parents=True, exist_ok=True)
    c = out / 'adc.c'
    c.write_text(PRE + constants + '\n' + body + TESTS)
    exe = out / 'adc'
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
if __name__ == '__main__':
    main()
