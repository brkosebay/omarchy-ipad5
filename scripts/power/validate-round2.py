#!/usr/bin/env python3
"""Host-only fault tests: compile exact kernel helpers against fake MMIO;
exercise ladder guards against temporary sysfs. Never connects to the iPad.
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('a9_ladder', HERE / 'ladder.py')
ladder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ladder)


def body(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    level = 1
    end = opening + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def kernel_helpers():
    guard = (HERE / 'kernel/apple-a9-tuning.h').read_text()
    thermal = (HERE / 'kernel/apple-s8003-thermal.c').read_text()
    shared = (HERE / 'kernel/s8003-power.h').read_text()
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
typedef uint16_t u16;
#define clamp(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
static bool j71t = true;
#define of_machine_is_compatible(s) (j71t)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define __iomem
#define __user
#define CAP_SYS_RAWIO 0
#define capable(c) true
#define copy_from_user(d,s,n) (memcpy(d,s,n),0)
#define strim(s) (s)
#define sysfs_streq(a,b) (!strcmp(a,b))
struct file { void *private_data; };
typedef long loff_t;
typedef long ssize_t;
#define BIT(n) (1U << (n))
#define GENMASK_ULL(h,l) ((UINT64_MAX << (l)) & (UINT64_MAX >> (63-(h))))
#define FIELD_GET(m,v) (((v) & (m)) / ((m) & -(m)))
#define FIELD_PREP(m,v) (((u64)(v) * ((m) & -(m))) & (m))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,x) ((v)=(x))
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define rcu_dereference(p) (p)
#define mod_delayed_work(w,q,t) ((void)0)
#define system_wq NULL
#define A9_POLL_MS 100
#define dev_err(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
#define time_after(a,b) ((long)((b) - (a)) < 0)
#define msecs_to_jiffies(v) (v)
#define raw_spin_lock_irqsave(l,f) ((f)=0)
#define raw_spin_unlock_irqrestore(l,f) ((void)(f))
#define mutex_lock(l) ((void)0)
#define mutex_unlock(l) ((void)0)
#define CPUFREQ_TABLE_END UINT_MAX
#define cpufreq_for_each_valid_entry(p,t) for ((p)=(t); (p)->frequency != CPUFREQ_TABLE_END; (p)++)
#define APPLE_DVFS_CMD 0x20
#define APPLE_DVFS_STATUS 0x50
#define APPLE_DVFS_CMD_PS1 15
#define A9_STOCK_KHZ 1800000U
#define A9_BOOST_KHZ 1848000U
#define A9_BOOST_STATE 15U
#define A9_HEARTBEAT_MS 2000
#define A9_SENSORS 6
#define A9_STALE_MS 500
struct mutex { int dummy; };
struct work_struct { int dummy; };
struct delayed_work { struct work_struct work; };
struct freq_qos_request { int dummy; };
struct cpufreq_frequency_table { unsigned int frequency, driver_data; };
struct cpufreq_policy { struct cpufreq_frequency_table *freq_table; };
struct a9_guard;
struct apple_cpu_priv { bool a9; void *reg_base; int lock; struct a9_guard *guard; };
static unsigned long jiffies;
static unsigned int stores;
static int request_error, sensor_error, temperature, qos_error;
static bool trip_in_temperature;
static u32 *voltage_race;
void apple_a9_thermal_revert(void);
static bool a9_tuning = true;
static unsigned int a9_passive_millic = 80000;
static const unsigned int a9_step_khz[] = {1896000,1944000,1992000,2040000,2088000,2160000};
static int a9_temperatures(int *hottest) {
    if (trip_in_temperature) { trip_in_temperature=false; apple_a9_thermal_revert(); }
    if (voltage_race) { *voltage_race=1; voltage_race=NULL; }
    *hottest=temperature; return sensor_error;
}
static int freq_qos_update_request(struct freq_qos_request *q, int value) { return qos_error; }
static u64 readq(void *address) { return *(u64 *)address; }
static u32 readl(void *address) { return *(u32 *)address; }
static void writeq(u64 value, void *address) { stores++; *(u64 *)address=value; }
static void writel(u32 value, void *address) { stores++; *(u32 *)address=value; }
static int sign_extend32(u32 value, int index) { return (int32_t)(value << (31-index)) >> (31-index); }
static int apple_soc_cpufreq_request(struct apple_cpu_priv *priv, unsigned int state) {
    if (request_error) return request_error;
    writeq(state | (state << 12),priv->reg_base+APPLE_DVFS_CMD);
    *(u32 *)(priv->reg_base+APPLE_DVFS_STATUS)=state|(state<<4);
    return 0;
}
'''
    ceiling = '\n'.join(line for line in guard.splitlines() if line.startswith((
        '#define A9_PLL_MAX_KHZ', '#define A9_OVERVOLT_LIMIT_UV', '#define A9_DAC_STEP_UV', '#define A9_CORE_BASE_UV', '#define A9_AUX_BASE_UV')))
    text = prefix + '\n' + ceiling + '\n'
    text += body(shared, 'struct apple_a9_thermal_profile {') + ';\n'
    text += body(shared, 'static inline struct apple_a9_thermal_profile apple_a9_stock_thermal_profile') + '\n'
    text += body(guard, 'struct apple_a9_thermal_profile apple_a9_get_thermal_profile') + '\n'
    text += body(guard, 'struct a9_guard {') + ';\n'
    text += 'static struct a9_guard *a9_active_guard;\n'
    for marker in ('static unsigned int a9_psinfo1_offset', 'static unsigned int a9_psinfo_khz',
                   'static unsigned int a9_step_state', 'static int a9_voltage_words', 'static bool a9_voltage_handoff_ok',
                   'static int a9_restore_locked', 'static void a9_revert', 'static int a9_check_stock',
                   'void apple_a9_thermal_revert', 'static int a9_arm',
                   'static int a9_configure', 'static bool a9_target_allowed',
                   'static const char *a9_guard_fault', 'static void a9_guard_work',
                   'static ssize_t a9_control_write'):
        text += body(guard, marker) + '\n'
    text += 'struct a9_thermal;\n' + body(thermal, 'struct a9_sensor {') + ';\n'
    text += body(thermal, 'struct a9_thermal {') + ';\n'
    text += '\n'.join(line for line in thermal.splitlines() if line.startswith((
        '#define A9_TRUSTED_MASK', 'static const u16 a9_offsets', 'static const u16 a9_clear_offsets'))) + '\n'
    for marker in ('static bool a9_sensor_trusted', 'static void __iomem *a9_sensor_base',
                   'static void a9_sample(', 'static int a9_sensor_temp'):
        text += body(thermal, marker) + '\n'
    # Pull the actual conversion expression directly from the driver.
    decode = next(line.strip() for line in thermal.splitlines() if 'q6 = sign_extend32' in line)
    convert = next(line.strip() for line in thermal.splitlines() if 's->temp = q6' in line)
    text += 'static int decode(u32 raw, bool tcc) { struct a9_sensor sensor, *s=&sensor; int q6; s->bk=raw; ' + decode + convert + ' return s->temp; }\n'
    text += r'''
int main(void) {
    static u64 mmio[0x1000/8];
    static u32 voltage[]={0,0x1782};
    struct apple_cpu_priv priv={.a9=true,.reg_base=mmio};
    struct cpufreq_frequency_table table[15];
    struct cpufreq_policy policy={.freq_table=table};
    struct a9_guard g={.priv=&priv,.policy=&policy,.ready=true,.armed=true,.limit=1848000,.voltage_base=voltage,.voltage_routes_boot=0x1782};
    priv.guard=&g;
    const u64 lo[]={0x3200000000000000ULL,0x6024b5ULL,0x1f06060300202423ULL,
        0x28060600002024b2ULL,0x3d060600002024c1ULL,0x5005050000202641ULL,
        0x68020200002023f0ULL,0x74000000002024b0ULL,0x74000000002024d0ULL};
    const unsigned int khz[]={0,300000,396000,600000,912000,1200000,1512000,1800000,1848000};
    for (int n=0;n<16;n++) {
        g.stock1[n]=n<=8?lo[n]:lo[2];
        g.stock2[n]=n==8?0x670002650d000000ULL:n==7?0x67000f650d000000ULL:
            n==6?0x56000f4d0d000000ULL:n==1?0x22000f007f000000ULL:0x22000f400d000000ULL;
        if (n) assert(a9_psinfo_khz(g.stock1[n])==(n<=8?khz[n]:396000));
        *(u64 *)(priv.reg_base+a9_psinfo1_offset(n))=g.stock1[n];
        *(u64 *)(priv.reg_base+0x868+n*8)=g.stock2[n];
    }
    for (int n=1;n<=14;n++) table[n-1]=(struct cpufreq_frequency_table){n<=8?khz[n]:a9_step_khz[n-9],n==8?15:n};
    table[14].frequency=CPUFREQ_TABLE_END;
    assert(a9_check_stock(&g)==0);
    table[7].driver_data=8;assert(a9_check_stock(&g)==-EINVAL);table[7].driver_data=15;
    g.stock1[15]^=1;assert(a9_check_stock(&g)==-EBUSY);g.stock1[15]^=1;
    g.stock1[9]^=1; assert(a9_check_stock(&g)==-EBUSY);g.stock1[9]^=1;
    g.stock2[8]^=1ULL<<40; assert(a9_check_stock(&g)==-EINVAL);g.stock2[8]^=1ULL<<40;
    assert(a9_psinfo1_offset(8)==0x368 && a9_psinfo1_offset(15)==0x3a0);
    unsigned int before=stores;
    assert(a9_configure(&g,1992000,50001)==-ERANGE && stores==before);
    assert(a9_configure(&g,1992000,1)==-EINVAL && stores==before);
    assert(a9_configure(&g,1992000,49999)==-EINVAL && stores==before);
    voltage[0]=1;assert(a9_configure(&g,1992000,6250)==-EOPNOTSUPP && stores==before);voltage[0]=0;
    voltage[1]=0;assert(a9_configure(&g,1992000,6250)==-EOPNOTSUPP && stores==before);voltage[1]=0x1782;
    g.armed=false;
    for (u32 bad=1;bad;bad<<=1) {
        voltage[0]=bad;
        assert(!a9_voltage_handoff_ok(&g));
        assert(a9_arm(&g)==-EOPNOTSUPP && !g.armed && stores==before);
    }
    voltage[0]=0;
    g.voltage_ctl_boot=1;assert(a9_arm(&g)==-EOPNOTSUPP);g.voltage_ctl_boot=0;
    g.voltage_routes_boot=0;assert(a9_arm(&g)==-EOPNOTSUPP);g.voltage_routes_boot=0x1782;
    assert(a9_arm(&g)==0 && g.armed && stores==before);
    g.armed=false;voltage_race=&voltage[0];
    assert(a9_arm(&g)==-EOPNOTSUPP && !g.armed && stores==before);voltage[0]=0;
    assert(a9_arm(&g)==0 && g.armed);
    u64 w1,w2;
    assert(a9_voltage_words(255ULL<<56,101ULL<<32,3125,&w1,&w2)==-ERANGE);
    assert(a9_voltage_words(116ULL<<56,255ULL<<32,0,&w1,&w2)==-ERANGE);
    assert(a9_psinfo_khz((300ULL<<4)|(2ULL<<13))==3600000);
    sensor_error=-EIO;assert(a9_configure(&g,1992000,0)==-EIO && stores==before);sensor_error=0;
    temperature=75000;assert(a9_configure(&g,1992000,0)==-EBUSY && stores==before);temperature=30000;
    jiffies=2001;assert(a9_configure(&g,1992000,0)==-EPERM && stores==before);jiffies=0;
    a9_tuning=false;assert(a9_configure(&g,1992000,0)==-EPERM && stores==before);a9_tuning=true;
    for (unsigned int i=0;i<ARRAY_SIZE(a9_step_khz);i++) {
      for (unsigned int delta=0;delta<=50000;delta+=3125) {
        assert(a9_configure(&g,a9_step_khz[i],delta)==0);
        unsigned int n=i+9;
        u64 value=readq(priv.reg_base+a9_psinfo1_offset(n));
        assert(a9_psinfo_khz(value)==a9_step_khz[i]);
        assert(((value^g.stock1[7])&~(GENMASK_ULL(12,4)|GENMASK_ULL(63,56)))==0);
        assert((value>>56)==116+delta/3125);
        u64 hi=readq(priv.reg_base+0x868+n*8);
        assert(((hi^g.stock2[7])&~GENMASK_ULL(39,32))==0);
        assert(FIELD_GET(GENMASK_ULL(39,32),hi)==101+delta/3125);
        assert(450000+(value>>56)*3125==812500+delta);
        assert(600000+FIELD_GET(GENMASK_ULL(39,32),hi)*3125==915625+delta);
        assert(g.delta_uv==delta);
        for (unsigned int s=0;s<=8;s++) {
            assert(readq(priv.reg_base+a9_psinfo1_offset(s))==g.stock1[s]);
            assert(readq(priv.reg_base+0x868+s*8)==g.stock2[s]);
        }
        assert(a9_target_allowed(&priv,n,a9_step_khz[i]));
        assert(!a9_target_allowed(&priv,n+1,2260000));
        *(u32 *)(priv.reg_base+APPLE_DVFS_STATUS)=n|(n<<4);
        assert(a9_restore_locked(&g)==0 && g.dirty==0 && g.configured==0);
        assert(readq(priv.reg_base+a9_psinfo1_offset(n))==g.stock1[n]);
        assert(g.delta_uv==0);
      }
    }
    /* Shared slot rejects aliases, overflow, non-rungs and non-stock dividers. */
    for (unsigned int khz=2208000;khz<=A9_PLL_MAX_KHZ;khz+=48000) {
      for (unsigned int delta=0;delta<=50000;delta+=3125) {
        assert(a9_configure(&g,khz,delta)==0 && g.configured==BIT(14));
        assert(a9_psinfo_khz(readq(priv.reg_base+a9_psinfo1_offset(14)))==khz);
        assert(a9_target_allowed(&priv,14,khz));
        assert(!a9_target_allowed(&priv,14,2160000));
        assert(!a9_target_allowed(&priv,14,khz-48000));
        assert(a9_restore_locked(&g)==0 && !g.dirty && !g.configured);
        assert(readq(priv.reg_base+a9_psinfo1_offset(14))==g.stock1[14]);
      }
    }
    assert(a9_configure(&g,A9_PLL_MAX_KHZ,50000)==0);
    assert(a9_psinfo_khz(readq(priv.reg_base+a9_psinfo1_offset(14)))==A9_PLL_MAX_KHZ);
    assert(a9_restore_locked(&g)==0);
    before=stores;
    assert(a9_configure(&g,A9_PLL_MAX_KHZ+24000,0)==-EINVAL && stores==before);
    assert(a9_configure(&g,2184000,0)==-EINVAL && stores==before);
    g.stock1[7]|=1;
    assert(a9_configure(&g,2208000,0)==-EINVAL && stores==before);
    g.stock1[7]&=~1ULL;
    g.stock1[7]|=2ULL<<13;
    assert(a9_configure(&g,2208000,0)==-EINVAL && stores==before);
    g.stock1[7]&=~(2ULL<<13);
    table[13].frequency=A9_PLL_MAX_KHZ;assert(a9_check_stock(&g)==0);
    table[13].driver_data=13;assert(a9_check_stock(&g)==-EINVAL);
    table[13].driver_data=14;table[13].frequency=2160000;
    /* All-load 1848 uses only dormant slot 15 at unchanged stock voltage. */
    before=stores;
    assert(a9_configure(&g,1848000,3125)==-EINVAL && stores==before);
    assert(a9_configure(&g,1848000,0)==0 && g.configured==BIT(15));
    assert(a9_psinfo_khz(readq(priv.reg_base+a9_psinfo1_offset(15)))==1848000);
    assert(readq(priv.reg_base+0x868+15*8)==g.stock2[7]);
    assert(readq(priv.reg_base+a9_psinfo1_offset(8))==g.stock1[8]);
    assert(readq(priv.reg_base+0x868+8*8)==g.stock2[8]);
    assert(a9_target_allowed(&priv,15,1848000));
    assert(a9_restore_locked(&g)==0 && !g.dirty && !g.configured);
    assert(readq(priv.reg_base+a9_psinfo1_offset(15))==g.stock1[15]);
    assert(!a9_target_allowed(&priv,15,1848000));
    g.armed=false;assert(!a9_target_allowed(&priv,8,1848000));g.armed=true;
    jiffies=2001;assert(!a9_target_allowed(&priv,8,1848000));jiffies=0;
    *(u32 *)(priv.reg_base+APPLE_DVFS_STATUS)=0x22;
    assert(a9_restore_locked(&g)==0 && readq(priv.reg_base+APPLE_DVFS_CMD)==0x2002);
    request_error=-ETIMEDOUT;g.dirty=BIT(9);before=stores;
    assert(a9_restore_locked(&g)==-ETIMEDOUT && stores==before && g.dirty==BIT(9));
    assert(decode(0x80000780,false)==30000 && decode(0x80000780,true)==30000);
    assert(decode(0x80007fc0,false)==-1000 && decode(0x8000ffc0,true)==-1000);
    assert(decode(0x80001900,false)==100000);
    assert(a9_guard_fault(0,79999,false)==NULL);
    assert(!strcmp(a9_guard_fault(-EIO,30000,false),"sensor-error"));
    assert(!strcmp(a9_guard_fault(0,80000,false),"thermal-passive"));
    assert(!strcmp(a9_guard_fault(0,95000,false),"thermal-hot"));
    assert(!strcmp(a9_guard_fault(0,100000,false),"thermal-critical"));
    assert(!strcmp(a9_guard_fault(0,30000,true),"heartbeat-timeout"));
    /* Effective profile, signed temperatures, clamp and arm/configure gates. */
    for (unsigned int requested=0;requested<=100000;requested+=1000) {
        a9_passive_millic=requested;
        struct apple_a9_thermal_profile p=apple_a9_get_thermal_profile();
        assert(p.passive==clamp(requested,70000U,92000U));
        assert(p.tuning && p.hot==95000 && p.critical==100000);
        assert(p.arm==p.passive-5000 && p.cooldown==p.passive-10000);
        assert(a9_guard_fault(0,-1000,false)==NULL);
        assert(a9_guard_fault(0,p.passive-1,false)==NULL);
        assert(!strcmp(a9_guard_fault(0,p.passive,false),"thermal-passive"));
    }
    a9_passive_millic=90000;
    temperature=84999;g.armed=false;g.dirty=0;request_error=0;
    assert(a9_arm(&g)==0);
    before=stores;temperature=85000;
    assert(a9_configure(&g,1992000,0)==-EBUSY && stores==before);
    g.armed=false;assert(a9_arm(&g)==-EBUSY);
    temperature=84999;assert(a9_arm(&g)==0);
    assert(a9_configure(&g,1992000,0)==0);
    temperature=89999;a9_guard_work(&g.work.work);assert(g.armed);
    temperature=90000;a9_guard_work(&g.work.work);assert(!g.armed && !g.dirty);
    g.latched=false;temperature=94999;a9_guard_work(&g.work.work);assert(!g.latched);
    temperature=95000;a9_guard_work(&g.work.work);assert(g.latched);
    a9_tuning=false;
    assert(apple_a9_get_thermal_profile().passive==80000);
    assert(apple_a9_get_thermal_profile().hot==90000);
    a9_tuning=true;j71t=false;assert(!apple_a9_get_thermal_profile().tuning);j71t=true;
    a9_passive_millic=80000;temperature=30000;
    struct a9_thermal thermal={0}; struct a9_sensor *s=&thermal.sensor[0];int value;
    s->parent=&thermal;s->error=-EAGAIN;assert(a9_sensor_temp(s,&value)==-EAGAIN);
    s->error=0;s->sampled=0;s->temp=30000;jiffies=501;assert(a9_sensor_temp(s,&value)==-ETIMEDOUT);
    jiffies=500;assert(a9_sensor_temp(s,&value)==0 && value==30000);
    static u64 thermal_soc[0x1000/8],thermal_tcc[0x1000/8];
    thermal.base[0]=thermal_soc;thermal.base[1]=thermal_tcc;
    *(u32 *)(thermal.base[1]+0x100)=0x3403f;
    *(u32 *)(thermal.base[1]+0x108)=0x5fc38434;
    *(u32 *)(thermal.base[1]+0x110)=0x9ec3dc2b;
    for (int i=0;i<6;i++) {
        struct a9_sensor *channel=&thermal.sensor[i];
        channel->parent=&thermal;channel->id=i;
        void *base=a9_sensor_base(channel);
        *(u32 *)base=9;
        *(u32 *)(base+(i>=3?0x20:0x18))=0x80000980;
        *(u32 *)(base+(i>=3?0x38:0x24))=42;
        a9_sample(channel);
        assert(!channel->error && channel->temp==38000);
        assert(a9_sensor_temp(channel,&value)==(i==0||i==1||i==5?0:-ENODATA));
    }
    assert(a9_offsets[4]==0x40 && a9_clear_offsets[4]==0x438);
    assert(*(u32 *)(thermal.base[1]+0x100)==0x3403f);
    assert(*(u32 *)(thermal.base[1]+0x108)==0x5fc38434);
    assert(*(u32 *)(thermal.base[1]+0x110)==0x9ec3dc2b);
    request_error=0;jiffies=0;g.armed=true;g.heartbeat=0;
    assert(a9_configure(&g,1992000,0)==0);
    *(u32 *)(priv.reg_base+APPLE_DVFS_STATUS)=0xbb;
    a9_active_guard=&g;
    unsigned int generation=g.trip_generation;
    apple_a9_thermal_revert();
    assert(!g.armed && g.latched && g.limit==1800000 && g.dirty==0 && g.revert_error==0);
    assert(g.thermal_qos_pending && g.trip_generation==generation+1);
    assert(!strcmp(g.reason,"thermal-trip"));
    g.armed=true;g.heartbeat=0;trip_in_temperature=true;
    assert(a9_configure(&g,1992000,0)==-EPERM && !g.armed && !g.dirty);
    trip_in_temperature=true;
    assert(a9_arm(&g)==-EAGAIN && !g.armed);
    assert(a9_arm(&g)==0);
    assert(a9_configure(&g,1992000,50000)==0);
    voltage[0]=1;
    assert(!a9_target_allowed(&priv,11,1992000));
    a9_guard_work(&g.work.work);
    assert(!g.armed && g.latched && !g.dirty && !g.delta_uv && !g.configured);
    assert(!strcmp(g.reason,"voltage-mode"));
    voltage[0]=0;assert(a9_arm(&g)==0);
    jiffies=2001;a9_guard_work(&g.work.work);
    assert(!g.armed && !g.dirty && !strcmp(g.reason,"heartbeat-timeout"));
    jiffies=0;assert(a9_arm(&g)==0);
    sensor_error=-EIO;a9_guard_work(&g.work.work);
    assert(!g.armed && !strcmp(g.reason,"sensor-error"));sensor_error=0;
    assert(a9_arm(&g)==0);temperature=80000;a9_guard_work(&g.work.work);
    assert(!g.armed && !strcmp(g.reason,"thermal-passive"));temperature=30000;
    struct file control={.private_data=&g};
    voltage[0]=1;before=stores;
    assert(a9_control_write(&control,"arm",3,NULL)==-EOPNOTSUPP);
    assert(g.control_error==-EOPNOTSUPP && !g.armed && stores==before);
    voltage[0]=0;
    assert(a9_control_write(&control,"arm",3,NULL)==3 && g.armed && !g.control_error);
    const char *configure="configure 1992000 6250";
    assert(a9_control_write(&control,configure,strlen(configure),NULL)==strlen(configure));
    assert(g.dirty && g.delta_uv==6250);
    configure="configure 1992000 50001";
    assert(a9_control_write(&control,configure,strlen(configure),NULL)==-ERANGE);
    assert(!g.armed && !g.dirty && !g.delta_uv && !g.configured && !g.revert_error);
    assert(g.control_error==-ERANGE && !strcmp(g.reason,"configure-error"));
    puts("kernel helpers: 102 bounded dual-rail encodes, ceiling/rounding, thermal/heartbeat/restore gates PASS");
}
'''
    out = ROOT / 'build/power/oc-r2/tests'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'kernel-helpers.c').write_text(text)
    subprocess.run(['clang', '-std=gnu11', '-O1', '-g0', '-fsanitize=undefined,address',
                    str(out / 'kernel-helpers.c'), '-o', str(out / 'kernel-helpers')], check=True)
    subprocess.run([str(out / 'kernel-helpers')], check=True)


class LadderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.output = io.StringIO()
        self.capture = contextlib.redirect_stdout(self.output)
        self.capture.__enter__()
        self.addCleanup(self.capture.__exit__, None, None, None)
        for name, value in [('voltage_readback', {}), ('boot_id', 'test-boot')]:
            patch = mock.patch.object(ladder, name, return_value=value)
            patch.start()
            self.addCleanup(patch.stop)
        for i, name in enumerate(sorted(ladder.NAMES)):
            zone = self.root / f'class/thermal/thermal_zone{i}'
            zone.mkdir(parents=True)
            for file, value in {'type': name, 'temp': '30000', 'mode': 'enabled',
                                'trip_point_0_type': 'passive', 'trip_point_0_temp': '80000',
                                'trip_point_1_type': 'hot', 'trip_point_1_temp': '90000',
                                'trip_point_2_type': 'critical', 'trip_point_2_temp': '100000'}.items():
                (zone / file).write_text(value)
        cool = self.root / 'class/thermal/cooling_device0'
        cool.mkdir()
        (cool / 'type').write_text('cpufreq-cpu0')
        (cool / 'max_state').write_text('13')
        for zone in (self.root / 'class/thermal').glob('thermal_zone*'):
            (zone / 'cdev0').symlink_to(cool)
        raw = self.root / 'kernel/debug/a9_thermal/snapshot'
        raw.parent.mkdir(parents=True)
        raw.write_text('format=3 trusted_mask=23 thermal_profile_format=1 profile_ready=1 '
                       'profile=stock profile_tuning=0 passive_millic=80000 hot_millic=90000 '
                       'critical_millic=100000 arm_millic=75000 cooldown_millic=70000\n' + '\n'.join(f'{n} bk=80000780 count=00000100 temp=30000 error=0 age_ms=0 trusted=1' for n in sorted(ladder.NAMES)))

    def test_valid_thermal_set(self):
        self.assertEqual(len(ladder.sensors(self.root)[0]), 3)

    def test_missing_zone(self):
        (self.root / 'class/thermal/thermal_zone0/type').write_text('battery')
        with self.assertRaisesRegex(RuntimeError, 'missing SoC'):
            ladder.sensors(self.root)

    def test_missing_cooling(self):
        (self.root / 'class/thermal/thermal_zone0/cdev0').unlink()
        with self.assertRaisesRegex(RuntimeError, 'not bound'):
            ladder.sensors(self.root)

    def test_raised_critical_trip(self):
        (self.root / 'class/thermal/thermal_zone0/trip_point_2_temp').write_text('120000')
        with self.assertRaisesRegex(RuntimeError, 'raised trips'):
            ladder.sensors(self.root)

    def test_stale_sensor(self):
        raw = self.root / 'kernel/debug/a9_thermal/snapshot'
        raw.write_text(raw.read_text().replace('age_ms=0', 'age_ms=501', 1))
        with self.assertRaisesRegex(RuntimeError, 'stale sensor'):
            ladder.sensors(self.root)

    def test_unvalidated_channels_do_not_block_or_trigger_policy(self):
        raw = self.root / 'kernel/debug/a9_thermal/snapshot'
        raw.write_text(raw.read_text() + '\napple-a9-soc2 bk=015e0988 count=0 error=-11 age_ms=117003\n'
                       'apple-a9-tcc0 bk=816202c4 count=29 error=0 age_ms=0\n'
                       'apple-a9-tcc1 bk=5fc38434 count=5fc38434 error=-11 age_ms=4294831968\n')
        for i, name in enumerate(sorted(ladder.ALL_NAMES - ladder.NAMES), 10):
            zone = self.root / f'class/thermal/thermal_zone{i}'
            zone.mkdir()
            (zone / 'type').write_text(name)
            # Missing/ENODATA temp and absent trips are tolerated only here.
        self.assertEqual(set(ladder.sensors(self.root)[0]), ladder.NAMES)

    def test_required_sensor_enodata_is_a_clear_abort(self):
        original = ladder.read
        def reader(path):
            if path == self.root / 'class/thermal/thermal_zone0/temp':
                raise OSError(61, 'No data available')
            return original(path)
        with mock.patch.object(ladder, 'read', side_effect=reader), self.assertRaisesRegex(RuntimeError, 'required sensor unavailable'):
            ladder.sensors(self.root)

    def test_overtemperature(self):
        (self.root / 'class/thermal/thermal_zone0/temp').write_text('80000')
        with self.assertRaisesRegex(RuntimeError, 'temperature range'):
            ladder.sensors(self.root)

    def test_checksum_failure_stops_child_and_reverts(self):
        policy = self.root / 'policy'
        policy.mkdir()
        boost = self.root / 'devices/system/cpu/cpufreq/boost'
        boost.parent.mkdir(parents=True)
        child = mock.Mock()
        child.poll.return_value = None
        watchdog = mock.Mock()
        watchdog.timeout = 10
        with mock.patch.object(ladder, 'ensure_stock', return_value={}), mock.patch.object(ladder, 'control') as control, \
                mock.patch.object(ladder, 'telemetry', return_value={'guard': {'armed': '1', 'latched': '0'}, 'actual_khz': 1848000}), \
                mock.patch.object(ladder.subprocess, 'Popen', return_value=child), \
                mock.patch.object(ladder, 'stop_child') as stop, \
                mock.patch.object(ladder.hashlib, 'sha256') as sha:
            sha.return_value.hexdigest.return_value = 'corrupt'
            with self.assertRaisesRegex(RuntimeError, 'correctness failure'):
                ladder.trial(self.root, policy, 1848000, 30, self.root, watchdog)
            self.assertEqual(control.call_args_list[-1].args[1], 'revert')
            stop.assert_called_once_with(child)
            watchdog.ping.assert_not_called()

    def test_failed_revert_still_stops_owned_workload(self):
        policy = self.root / 'policy'
        policy.mkdir()
        boost = self.root / 'devices/system/cpu/cpufreq/boost'
        boost.parent.mkdir(parents=True)
        child = mock.Mock()
        child.poll.return_value = 1

        def command(root, value):
            if value == 'revert':
                raise OSError('revert failed')
        with mock.patch.object(ladder, 'ensure_stock', return_value={}), mock.patch.object(ladder, 'control', side_effect=command), \
                mock.patch.object(ladder, 'telemetry', return_value={'guard': {'armed': '1', 'latched': '0'}, 'actual_khz': 1848000}), \
                mock.patch.object(ladder.subprocess, 'Popen', return_value=child), \
                mock.patch.object(ladder, 'stop_child') as stop:
            with self.assertRaisesRegex(OSError, 'revert failed'):
                ladder.trial(self.root, policy, 1848000, 30, self.root, mock.Mock())
            stop.assert_called_once_with(child)

    def test_undelivered_clock_is_not_a_pass(self):
        policy = self.root / 'policy'
        policy.mkdir()
        boost = self.root / 'devices/system/cpu/cpufreq/boost'
        boost.parent.mkdir(parents=True)
        child = mock.Mock(returncode=0)
        child.poll.side_effect = [None, 0]
        sample = {'guard': {'armed': '1', 'latched': '0'}, 'actual_khz': 1800000}
        with mock.patch.object(ladder, 'ensure_stock', return_value={}), \
                mock.patch.object(ladder, 'control'), \
                mock.patch.object(ladder.subprocess, 'Popen', return_value=child), \
                mock.patch.object(ladder, 'stop_child'), \
                mock.patch.object(ladder, 'telemetry', return_value=sample), \
                mock.patch.object(ladder.time, 'monotonic', side_effect=[0, 0, 30]), \
                mock.patch.object(ladder.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'never delivered'):
                ladder.trial(self.root, policy, 1896000, 30, self.root, mock.Mock(timeout=10))

    def test_failed_final_revert_keeps_watchdog_active(self):
        policy = self.root / 'devices/system/cpu/cpufreq/policy0'
        policy.mkdir(parents=True)
        for name, value in {'scaling_driver': 'apple-cpufreq', 'scaling_governor': 'schedutil',
                            'scaling_min_freq': '300000', 'scaling_max_freq': '1800000'}.items():
            (policy / name).write_text(value)
        args = types.SimpleNamespace(minutes=5, logdir=self.root / 'logs', sysfs=self.root,
                                     watchdog=Path('/dev/watchdog0'), mode='boost', max_delta_uv=50000,
                                     voltage_step_uv=6250, control_seconds=30, retry_failure=None)
        watchdog = mock.Mock()
        with mock.patch.object(ladder.shutil, 'which', return_value='/fake/stress-ng'), \
                mock.patch.object(ladder, 'status', return_value={'enable': '1'}), \
                mock.patch.object(ladder, 'voltage_handoff'), \
                mock.patch.object(ladder, 'Watchdog', return_value=watchdog), \
                mock.patch.object(ladder, 'trial', side_effect=RuntimeError('trial failed')), \
                mock.patch.object(ladder, 'control', side_effect=OSError('revert failed')):
            with self.assertRaisesRegex(OSError, 'revert failed'):
                ladder.run_trials(args)
            watchdog.abandon.assert_called_once()
            watchdog.close.assert_not_called()
            self.assertIsNone(json.loads((args.logdir / 'summary.json').read_text())['max_stable_khz_by_delta_uv']['0'])


if __name__ == '__main__':
    kernel_helpers()
    unittest.main()
