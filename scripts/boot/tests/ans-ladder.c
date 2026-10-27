#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
struct apple_h9p_pcie {void *dev;bool ans_pre_enumeration;};
static int retrains,cycles,replays,win,local;
static int apple_s8003_retrain(struct apple_h9p_pcie *p,bool gen1) {
 assert(gen1==(retrains==1));retrains++;return win==retrains?0:-ENOLINK;
}
static int apple_s8003_deep_cycle(struct apple_h9p_pcie *p) {
 cycles++;return local?-EIO:win==2+cycles?0:-ETIMEDOUT;
}
static int apple_s8003_replay_phy(struct apple_h9p_pcie *p) {assert(p->ans_pre_enumeration);replays++;return 0;}
#include "production.h"
int main(void) {
 for(win=1;win<=6;win++) {
  struct apple_h9p_pcie p={.ans_pre_enumeration=true};retrains=cycles=replays=local=0;
  assert(!apple_s8003_deep_recover(&p));
  assert(retrains==(win<2?win:2) && cycles==(win<3?0:win-2) && replays==(win==6));
 }
 struct apple_h9p_pcie p={0};win=0;retrains=cycles=replays=0;
 assert(apple_s8003_deep_recover(&p)==-ETIMEDOUT && cycles==3 && !replays);
 retrains=cycles=replays=0;local=1;p.ans_pre_enumeration=true;
 assert(apple_s8003_deep_recover(&p)==-EIO && cycles==1 && !replays);
 puts("PASS retrain/Gen1/three cycles/pre-scan PHY order, first-success stop and post-probe/local exclusion");
}
