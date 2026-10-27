/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual sysfs admission, console gate and checkpoint with concurrent writers. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
typedef unsigned int u32;
struct console {const char *name; int flags; struct console *next;};
#define CON_ENABLED 4
#define CON_SUSPENDED 16
static struct console ram={"ramoops",CON_ENABLED,NULL},efi={"efi",CON_ENABLED,NULL};
static struct console *consoles=&ram;
static _Thread_local int srcu_depth;
static int console_srcu_read_lock(void) {assert(!srcu_depth++);return 1;}
static void console_srcu_read_unlock(int n) {assert(n==1 && srcu_depth--==1);}
static int console_srcu_read_flags(struct console *c) {assert(srcu_depth);return c->flags;}
#define for_each_console_srcu(c) for(c=consoles;c;c=c->next)
static unsigned int cold_defer=1,cold_test=5;
static bool force_cold=true,flush_ok=true,handoff;
static unsigned int queued,printed,flushed,barriers;
struct a9_aop {pthread_mutex_t lock; bool stopping,cold_triggered; int boot_work,dev;
 unsigned int experiment_stage,cold_frames; uint64_t region_pa,ext_dma;
 struct {bool seen;} ho;};
struct device {struct a9_aop *data;};
struct device_attribute {int unused;};
#define dev_get_drvdata(d) ((d)->data)
#define mutex_lock(p) pthread_mutex_lock(p)
#define mutex_unlock(p) pthread_mutex_unlock(p)
static int system_long_wq;
static bool queue_work(int q,int *w) {(void)q;(void)w;assert(++queued==1);return true;}
static int kstrtouint(const char *s,int base,unsigned int *v) {
 char *end;unsigned long n=strtoul(s,&end,base);
 if(end==s || n>UINT32_MAX || (*end && strcmp(end,"\n"))) return -EINVAL;
 *v=n;return 0;
}
#define dev_info(...) do { (void)cold_test; ++printed; } while (0)
static bool pr_flush(int ms,bool reset) {assert(ms==1000 && !reset && printed);flushed++;return flush_ok;}
#define mb() (++barriers)
#include "trigger-functions.h"
static void *trigger(void *d) {
 intptr_t ret=cold_release_store(d,NULL,"1\n",2);
 return (void *)ret;
}
int main(void) {
 struct a9_aop a={.lock=PTHREAD_MUTEX_INITIALIZER};struct device d={&a};
 assert(a9_aop_ramoops_ready());
 assert(!a9_aop_cold_checkpoint(&a,"before-run") && printed==1 && flushed==1 && barriers==1);
 flush_ok=false;assert(a9_aop_cold_checkpoint(&a,"before-run")==-ETIMEDOUT && barriers==1);flush_ok=true;
 consoles=&efi;assert(!a9_aop_ramoops_ready());
 assert(a9_aop_cold_checkpoint(&a,"before-run")==-ENODEV);
 assert(cold_release_store(&d,NULL,"1",1)==-ENODEV && !queued && !a.cold_triggered);
 consoles=&ram;
 ram.flags=0;assert(!a9_aop_ramoops_ready());
 ram.flags=CON_ENABLED|CON_SUSPENDED;assert(!a9_aop_ramoops_ready());ram.flags=CON_ENABLED;
 const char *bad[]={"0","2","garbage","-1"};
 for(unsigned int i=0;i<4;i++) assert(cold_release_store(&d,NULL,bad[i],strlen(bad[i]))==-EINVAL);
 force_cold=false;assert(cold_release_store(&d,NULL,"1",1)==-EINVAL);force_cold=true;
 cold_defer=0;assert(cold_release_store(&d,NULL,"1",1)==-EINVAL);cold_defer=1;
 a.stopping=true;assert(cold_release_store(&d,NULL,"1",1)==-EBUSY && !queued);a.stopping=false;
 /* aop_handoff=sleep: any record at probe forbids a fallback cold reload. */
 handoff=true;a.ho.seen=true;
 assert(cold_release_store(&d,NULL,"1",1)==-EBUSY && !queued && !a.cold_triggered);
 a.ho.seen=false;handoff=false;
 /* The actual store holds the same mutex as shutdown and queues only once. */
 for(unsigned int i=0;i<20;i++) {
  pthread_t one,two;void *r1,*r2;queued=0;a.cold_triggered=false;
  /* Readiness SRCU stubs need no shared counter during simultaneous reads. */
  assert(!pthread_create(&one,NULL,trigger,&d));
  assert(!pthread_create(&two,NULL,trigger,&d));
  assert(!pthread_join(one,&r1) && !pthread_join(two,&r2));
  assert(((intptr_t)r1==2 && (intptr_t)r2==-EBUSY) || ((intptr_t)r2==2 && (intptr_t)r1==-EBUSY));
  assert(queued==1 && a.cold_triggered);
 }
 puts("PASS actual cold trigger/checkpoint: RAM backend only, disabled/suspended/EFI refused, flush failure blocks, invalid input/mode/shutdown refused, concurrent duplicate triggers queue once; handoff record blocks cold reload");
}
