#!/usr/bin/env python3
"""Run actual Tristar detection/lifecycle and role monitor code with scripted I/O."""
import importlib.util
import os
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
#include <pthread.h>
#include <stdatomic.h>
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
typedef uint8_t u8;
typedef _Atomic int atomic_t;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define clamp_t(t,x,lo,hi) ((t)(x)<(lo)?(lo):(t)(x)>(hi)?(hi):(t)(x))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,y) ((x)=(y))
#define dev_crit(...) ((void)0)
#define atomic_read(x) atomic_load(x)
#define atomic_inc(x) atomic_fetch_add(x,1)
#define time_before(a,b) ((long)((a)-(b))<0)
#define container_of(p,t,m) ((t *)((char *)(p)-__builtin_offsetof(t,m)))
#define dev_info_ratelimited(...) ((void)0)
static int detect_logs;
#define dev_warn_ratelimited(...) (++detect_logs)
#define IS_ERR_OR_NULL(x) (!(x))
#define lockdep_assert_held(x) ((void)0)
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
enum { POWER_SUPPLY_USB_TYPE_UNKNOWN, POWER_SUPPLY_USB_TYPE_SDP,
       POWER_SUPPLY_USB_TYPE_DCP, POWER_SUPPLY_USB_TYPE_CDP,
       POWER_SUPPLY_USB_TYPE_ACA, POWER_SUPPLY_USB_TYPE_C, POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID };
enum usb_role {USB_ROLE_NONE, USB_ROLE_HOST, USB_ROLE_DEVICE};
struct usb_role_switch {void *data;};
static int apple_s8000_usb_role_monitor(struct usb_role_switch *, int (*)(void *), void *);
struct device {int unused;};
struct i2c_client {struct device dev;};
struct mutex {pthread_mutex_t m;};
struct notifier_block {int unused;};
struct delayed_work {int unused;};
struct device_node {int unused;};
struct power_supply {int unused;};
struct usb_gadget_current_budget {
    struct device_node *of_node;
    bool connected, configured, suspended, host_grant;
    unsigned current_ma;
};
static unsigned long jiffies;
static unsigned auto_input_ua = 2100000;
static int detach_calls, notifications, scheduled;
static void *system_power_efficient_wq;
static unsigned long msecs_to_jiffies(unsigned ms) {return ms;}
static void mutex_lock(struct mutex *m) {assert(!pthread_mutex_lock(&m->m));}
static void mutex_unlock(struct mutex *m) {assert(!pthread_mutex_unlock(&m->m));}
static void mod_delayed_work(void *q, struct delayed_work *w, unsigned long ms)
{(void)q;(void)w;(void)ms;scheduled++;}
static void cancel_delayed_work_sync(struct delayed_work *w) {(void)w;}
static void usb_gadget_unregister_current_notifier(struct notifier_block *n) {(void)n;}
static void power_supply_changed(struct power_supply *p) {(void)p;notifications++;}
static void usleep_range(int a,int b) {(void)a;(void)b;}
static void msleep(int ms) {assert(ms==40 || ms==20);}
#include "apple_tristar_auto.h"
'''

MOCKS = r'''
static struct apple_tristar ts;
static struct i2c_client client;
static struct power_supply supply;
static struct usb_gadget_current_budget budget;
static int status, revision, budget_error, samples, cancel_sample, adc_values[8];
static int writes, dcp_writes, fail_restore, flip_sample;
static u8 pulses[8], routes[32];
static u8 route, pulse;
static int usb_gadget_get_current_budget(struct device_node *node,
                                          struct usb_gadget_current_budget *out)
{(void)node;*out=budget;return budget_error;}
static void usb_gadget_current_source_detached(struct device_node *node)
{(void)node;memset(&budget,0,sizeof(budget));detach_calls++;}
static enum usb_role usb_role_switch_get_role(struct usb_role_switch *sw)
{(void)sw;return USB_ROLE_DEVICE;}
static int apple_tristar_read(struct i2c_client *c,u8 reg)
{(void)c;if(reg==TRISTAR_REVISION)return revision;if(reg==TRISTAR_VBUS)return status;
 if(reg==TRISTAR_DX_CONTROL)return route;if(reg==TRISTAR_DCP_CONTROL)return pulse;
 assert(false);return -EINVAL;}
static int i2c_smbus_write_byte_data(struct i2c_client *c,u8 reg,u8 value)
{(void)c;writes++;
 if(reg==TRISTAR_DX_CONTROL){assert(writes<=32);routes[writes-1]=value;if(fail_restore && value==1)return -EIO;route=value;}
 else {assert(reg==TRISTAR_DCP_CONTROL);pulse=value;assert(dcp_writes<8);pulses[dcp_writes++]=value;}return 0;}
static int apple_tristar_adc_mv(struct apple_tristar *p)
{samples++;if(samples==cancel_sample)atomic_inc(&p->reset_generation);
 if(samples==flip_sample)status^=0x40;
 p->detect_stage="adc-result";assert(samples<=8);return adc_values[samples-1];}
'''

ROLE = r'''
static struct mutex apple_usb_role_instance_lock = {PTHREAD_MUTEX_INITIALIZER};
struct apple_usb_role {struct mutex lock;enum usb_role role;bool dying,monitor_fault;};
static struct apple_usb_role ur;
static struct apple_usb_role *apple_usb_role_instance;
static void *usb_role_switch_get_drvdata(struct usb_role_switch *sw) {return sw->data;}
static struct apple_usb_role *apple_usb_role_from_switch(struct usb_role_switch *sw)
{return sw && usb_role_switch_get_drvdata(sw)==apple_usb_role_instance ?
        apple_usb_role_instance:NULL;}
'''

TESTS = r'''
static struct usb_role_switch sw;
static void reset(void)
{
    memset(&ts,0,sizeof(ts));ts.lock.m=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    memset(&budget,0,sizeof(budget));ts.client=&client;ts.psy=&supply;
    status=0xa3;revision=0x8a;route=1;pulse=0;
    jiffies=10000;ts.online=1;ts.status=status;ts.source_present=true;
    samples=cancel_sample=writes=dcp_writes=fail_restore=budget_error=0;
    detach_calls=notifications=flip_sample=detect_logs=0;memset(pulses,0,sizeof(pulses));memset(routes,0,sizeof(routes));
    adc_values[0]=2500;adc_values[1]=2500;adc_values[2]=600;adc_values[3]=100;
    tristar_auto_init(&ts.autop);
    memset(&ur,0,sizeof(ur));ur.lock.m=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    ur.role=USB_ROLE_DEVICE;sw.data=&ur;apple_usb_role_instance=&ur;
    ts.role_sw=&sw;
}
static int monitor(void)
{return apple_s8000_usb_role_monitor(&sw,apple_tristar_monitor,&ts);}
static void classify_tests(void)
{
    const int mv[3]={2000,2500,3000};
    const int want[9]={500000,1000000,1000000,2100000,2400000,2400000,
                       1000000,1000000,1000000};
    for(int a=0;a<3;a++)for(int b=0;b<3;b++){
        reset();adc_values[0]=mv[a];adc_values[1]=mv[b];
        assert(monitor()==0);assert(ts.brick_sampled && ts.brick_ua==want[3*a+b]);
        assert(route==1 && pulse==0 && !ts.restore_pending && !ts.dcp_pending);
        assert(samples==2);apple_tristar_update(&ts);
        assert(ts.usb_type==POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID);
    }
    for(int c=1;c<=4;c++){
        reset();adc_values[0]=adc_values[1]=40;cancel_sample=c;
        assert(monitor()==0);assert(!ts.brick_sampled && !ts.brick_ua);
        assert(route==1 && pulse==0);
    }
    for(int kind=0;kind<3;kind++){
        reset();budget.connected=kind==0;budget.configured=kind==1;budget.host_grant=kind==2;
        assert(monitor()==0 && !samples && !writes);
    }
    reset();ur.role=USB_ROLE_HOST;
    assert(monitor()==-EBUSY && !samples && !writes);
    reset();ur.dying=true;assert(monitor()==-EBUSY && !writes);
    reset();fail_restore=1;assert(monitor()==-EUCLEAN && ur.monitor_fault);
    assert(ts.restore_pending && route!=1);
    fail_restore=0;assert(monitor()==0 && !ur.monitor_fault && route==1);
    /* BC1.2 ordinary USB primary D- 326..899, secondary D+ threshold 326. */
    for(int secondary=325;secondary<=326;secondary++){
        reset();adc_values[0]=adc_values[1]=40;adc_values[3]=secondary;
        assert(monitor()==0 && samples==4);
        assert(ts.brick_ua==1500000 && route==1 && pulse==0 && dcp_writes==4);
        assert(ts.analog_type==(secondary==325?POWER_SUPPLY_USB_TYPE_DCP:POWER_SUPPLY_USB_TYPE_CDP));
        apple_tristar_update(&ts);assert(ts.current_ua==1500000);
        budget.connected=true;apple_tristar_update(&ts);assert(ts.current_ua==1500000);
    }
    puts("PASS Tristar: Apple 3x3 signatures, BC1.2 threshold, connected/configured/grant exclusions, host/dying exclusions, every ADC cancellation, route recovery");
}
static void lifecycle_tests(void)
{
    reset();monitor();apple_tristar_update(&ts);assert(ts.brick_ua==2400000);
    for(int i=0;i<10;i++){
        budget.suspended=i%2;budget.of_node=ts.usb_node;
        apple_tristar_usb_event(&ts.usb_notifier,0,&budget);apple_tristar_update(&ts);
        assert(ts.brick_sampled && ts.brick_ua==2400000 && !ts.attach_id && !detach_calls);
    }
    budget_error=-ENODEV;apple_tristar_update(&ts);assert(ts.current_ua==2400000);
    budget_error=0;
    ts.autop.adapter=ts.autop.id_valid=ts.autop.ph_ok=true;ts.autop.ph_tries=2;
    ts.autop.phase=TRISTAR_AUTO_HOST;budget.host_grant=true;
    status=0;apple_tristar_update(&ts);
    assert(ts.attach_id==1 && !ts.brick_sampled && !ts.brick_ua && !ts.computer);
    assert(!ts.autop.adapter && !ts.autop.ph_ok && !ts.autop.id_valid && !ts.autop.ph_tries);
    assert(!budget.host_grant && !budget.connected && detach_calls==1 && ts.detach_role);
    status=0xa3;jiffies+=1000;samples=0;apple_tristar_update(&ts);monitor();apple_tristar_update(&ts);
    assert(ts.brick_sampled && ts.current_ua==2400000 && ts.attach_id==1);
    status=-EIO;apple_tristar_update(&ts);assert(ts.attach_id==1 && ts.brick_sampled);
    reset();budget.connected=true;budget.configured=true;budget.current_ma=500;
    apple_tristar_update(&ts);assert(ts.computer && ts.usb_type==POWER_SUPPLY_USB_TYPE_SDP);
    memset(&budget,0,sizeof(budget));apple_tristar_update(&ts);monitor();
    assert(ts.computer && ts.current_ua==500000 && !samples);
    puts("PASS Tristar: physical source epoch, fast attach, grant/ACA reset, suspend/reset and I/O-error retention, sticky computer classification");
}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static bool entered,release;
static int stall(void *data)
{
    (void)data;pthread_mutex_lock(&gate);entered=true;pthread_cond_signal(&ready);
    while(!release)pthread_cond_wait(&ready,&gate);
    pthread_mutex_unlock(&gate);return 0;
}
static void *sampling(void *data)
{(void)data;assert(!apple_s8000_usb_role_monitor(&sw,stall,NULL));return NULL;}
static void lock_test(void)
{
    reset();pthread_t thread;assert(!pthread_create(&thread,NULL,sampling,NULL));
    pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&ready,&gate);
    assert(pthread_mutex_trylock(&ur.lock.m)==EBUSY);
    release=true;pthread_cond_signal(&ready);pthread_mutex_unlock(&gate);
    assert(!pthread_join(thread,NULL));assert(!pthread_mutex_trylock(&ur.lock.m));
    pthread_mutex_unlock(&ur.lock.m);
    puts("PASS role monitor: real pthread interleaving blocks the role hardware mutex for the entire callback");
}

static void round8_tests(void)
{
    reset();route=0;
    assert(legacy_pin_mv(&ts,false,false)==-EOPNOTSUPP && !writes && !samples);
    puts("PASS charge7 regression reproduced: DX_CONTROL=0 fails before any route write/ADC");
    reset();ur.role=USB_ROLE_HOST;assert(!apple_tristar_quiesce(&ts) && !writes);
    reset();fail_restore=1;assert(monitor()==-EUCLEAN);
    fail_restore=0;assert(!apple_tristar_quiesce(&ts) && !ur.monitor_fault && route==1);
    /* Actual reg-0 state seen in host captures, in both orientations. */
    for(int hi=0;hi<2;hi++){
        reset();route=0;ts.status=status=hi?0xe3:0xa3;
        assert(!monitor() && ts.brick_ua==2400000 && route==0);
        assert(routes[0]==(hi?0xc0:0x0c));assert(routes[2]==(hi?0xd0:0x0d));
        reset();route=0;ts.status=status=hi?0xe3:0xa3;
        adc_values[0]=adc_values[1]=40;
        assert(!monitor() && ts.analog_type==POWER_SUPPLY_USB_TYPE_DCP);
        assert(pulses[0]==(hi?11:9) && pulses[1]==0);
        assert(pulses[2]==(hi?12:10) && pulses[3]==0 && route==0);
    }
    /* An unrelated accessory mux never becomes a USB monitor. */
    reset();route=0x22;int expected_ms=1000;
    for(int i=0;i<8;i++){
        assert(!monitor() && ts.detect_error==-EOPNOTSUPP && !samples && !writes);
        assert(!strcmp(ts.detect_stage,"route-select"));
        assert(ts.retry_ms==(unsigned)expected_ms && ts.detect_attempts==(unsigned)i+1);
        jiffies=ts.classify_after-1;assert(!monitor() && ts.detect_attempts==(unsigned)i+1);
        jiffies++;expected_ms=expected_ms<32000?expected_ms*2:60000;
    }
    assert(detect_logs==3); /* first warning, then at most one per minute */
    status=0;apple_tristar_update(&ts);
    assert(!ts.detect_attempts && !ts.detect_failures && !ts.retry_ms);
    reset();status=0xa1;assert(!monitor() && ts.detect_error==-ENOLINK && !writes);
    assert(!strcmp(ts.detect_stage,"vbus"));
    reset();flip_sample=1;assert(!monitor() && !ts.brick_sampled && route==1);
    assert(!strcmp(ts.detect_stage,"orientation"));
    reset();adc_values[0]=4001;assert(!monitor() && ts.detect_error==-ERANGE);
    assert(!strcmp(ts.detect_stage,"divider-range"));
    reset();ts.adc_poisoned=true;assert(!monitor() && !writes && !samples);
    status=0;apple_tristar_update(&ts);assert(ts.adc_poisoned);
    puts("PASS charge8: zero mux and BC pulses in both orientations, unsafe mux refusal, exponential retry, strict VBUS/orientation/range guards, poison survives detach");

    const u8 cable[6]={0x10,0x09,0x08,0,0,0};
    const int cc[]={0,659,660,1229,1230,2000,4001};
    const int ua[]={0,0,1500000,1500000,3000000,3000000,-ERANGE};
    for(unsigned i=0;i<ARRAY_SIZE(cc);i++)assert(apple_tristar_cc_current(cc[i])==ua[i]);
    for(int hi=0;hi<2;hi++){
        reset();ts.status=status=hi?0xe3:0xa3;route=hi?0x10:1;
        memcpy(ts.autop.id,cable,6);ts.autop.id_valid=true;
        adc_values[0]=adc_values[1]=1500;
        adc_values[2]=adc_values[3]=40;adc_values[4]=600;adc_values[5]=100;
        assert(!monitor() && ts.brick_ua==3000000 && ts.analog_type==POWER_SUPPLY_USB_TYPE_C);
        assert(samples==6 && dcp_writes==4 && routes[0]==(hi?0x1c:0xc1));
        assert(route==(hi?0x10:1));apple_tristar_update(&ts);
        assert(ts.current_ua==3000000 && ts.usb_type==POWER_SUPPLY_USB_TYPE_C);
        jiffies+=5000;samples=writes=dcp_writes=0;adc_values[0]=adc_values[1]=800;
        assert(!monitor() && ts.brick_ua==1500000);
        /* An unstable or interrupted update revokes an old CC allowance. */
        jiffies+=5000;samples=dcp_writes=0;adc_values[0]=800;adc_values[1]=1500;
        assert(!monitor() && !ts.brick_ua && ts.detect_error==-EAGAIN);
        assert(!strcmp(ts.detect_stage,"cc-debounce"));
    }
    reset();memcpy(ts.autop.id,cable,6);ts.autop.id_valid=true;
    adc_values[0]=adc_values[1]=1500;
    adc_values[2]=adc_values[3]=40;adc_values[4]=0;
    assert(!monitor() && ts.brick_ua==1500000 && ts.analog_type==POWER_SUPPLY_USB_TYPE_C);
    budget.connected=true;apple_tristar_update(&ts);
    assert(ts.computer && ts.usb_type==POWER_SUPPLY_USB_TYPE_SDP && !ts.brick_ua);
    int n=samples;assert(!monitor() && samples==n);
    reset();memcpy(ts.autop.id,cable,6);ts.autop.id_valid=true;
    ts.autop.id[2]=0;assert(!apple_tristar_typec_cable(&ts));
    ts.autop.id[2]=8;ts.autop.id[5]=1;assert(!apple_tristar_typec_cable(&ts));
    puts("PASS charge8: actual CC threshold edges, opposite pair with exact data nibble restore, two-sample debounce, 3A/1.5A refresh and revoke, Mac host precedence, unsupported IDs");
}

int main(void) {round8_tests();classify_tests();lifecycle_tests();lock_test();return 0;}
'''


def main():
    src = (ROOT / 'scripts/periph/kernel/apple_tristar_power.c').read_text()
    # Role support is a small patch against the pinned baseline; the built copy
    # is exported by check-charge7.sh so tests execute the actual implementation.
    role = Path(os.environ.get('CHARGE8_ROLE_SOURCE', str(ROOT / 'build/charge8/apple-s8000-usb-role.c'))).read_text()
    role = role.replace('int apple_s8000_usb_role_monitor(', 'static int apple_s8000_usb_role_monitor(')
    constants = '\n'.join(re.findall(r'^#define (?:TRISTAR_|APPLE_TRISTAR_).*$', src, re.M))
    structure = re.search(r'struct apple_tristar\s*\{.*?\n\};', src, re.S).group()
    funcs = ('apple_tristar_write_reg_checked', 'apple_tristar_write_checked',
             'apple_tristar_restore_route', 'apple_tristar_sample_valid', 'apple_tristar_sample_vbus',
             'apple_tristar_pin_mv', 'apple_tristar_typec_cable', 'apple_tristar_cc_current',
             'apple_tristar_brick_current', 'apple_tristar_detect_failed', 'apple_tristar_classify', 'apple_tristar_monitor',
             'apple_tristar_quiesce', 'apple_tristar_budget', 'apple_tristar_auto_cfg', 'apple_tristar_update',
             'apple_tristar_usb_event')
    body = '\n'.join(extract.function_text(src, f) for f in funcs)
    baseline = (ROOT / 'build/charge8/tristar-before.c').read_text()
    body += '\n' + extract.function_text(baseline, 'apple_tristar_pin_mv').replace(
        'apple_tristar_pin_mv', 'legacy_pin_mv')
    rbody = extract.function_text(role, 'apple_s8000_usb_role_monitor')
    out = ROOT / 'build/charge8/tests'
    out.mkdir(parents=True, exist_ok=True)
    c = out / 'tristar.c'
    c.write_text(PRE + constants + '\n' + structure + MOCKS + body + ROLE + rbody + TESTS)
    exe = out / 'tristar'
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-O1', '-fsanitize=address,undefined', '-pthread',
                    '-I', str(ROOT / 'scripts/periph/kernel'), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
