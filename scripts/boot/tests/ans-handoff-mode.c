#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
enum apple_ans_boot_mode { APPLE_ANS_ROUND7, APPLE_ANS_HANDOFF, APPLE_ANS_FULL_CYCLE };
static const char *ans_boot_modes[]={"round7","handoff","full-cycle"};
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
struct kernel_param {void *arg;};
static bool sysfs_streq(const char *a,const char *b) {
    unsigned n=strcspn(a,"\n");return strlen(b)==n&&!strncmp(a,b,n);
}
#define sysfs_emit sprintf
#include "production.h"
int main(void) {
    enum apple_ans_boot_mode mode=APPLE_ANS_HANDOFF;
    struct kernel_param p={&mode};char buf[32];
    for(unsigned i=0;i<ARRAY_SIZE(ans_boot_modes);i++) {
        assert(!ans_boot_mode_set(ans_boot_modes[i],&p));assert(mode==i);
        ans_boot_mode_get(buf,&p);assert(!strncmp(buf,ans_boot_modes[i],strlen(ans_boot_modes[i])));
    }
    assert(ans_boot_mode_set("typo",&p)==-EINVAL && mode==APPLE_ANS_FULL_CYCLE);
    assert(!ans_boot_mode_set("handoff\n",&p) && mode==APPLE_ANS_HANDOFF);
    puts("PASS mode parser: handoff/full-cycle/round7 and invalid-mode rejection");
}
