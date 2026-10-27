#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef uint32_t u32;
#define BIT(n) (1UL<<(n))
#define GENMASK(h,l) ((~0U>>(31-(h)))&(~0U<<(l)))
#define FIELD_GET(m,v) (((v)&(m))>>__builtin_ctz(m))
#define __iomem
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define ilog2(x) __builtin_ctz(x)
#define EXPORT_SYMBOL_GPL(x)
#include "registers.h"
enum apple_ans_boot_mode { APPLE_ANS_ROUND7, APPLE_ANS_HANDOFF, APPLE_ANS_FULL_CYCLE };
static const char *ans_boot_modes[] = {"round7","handoff","full-cycle"};
static bool ans_keep_power;
static bool warm_boot = true;
static void *of_chosen;
static bool of_property_read_bool(void *node,const char *property) { return warm_boot; }
struct device { int unused; };
struct apple_h9p_pcie {
    struct {bool active,ready;void *base;} nvmmu;
    unsigned char *base_rc,*base_config,*base_port[4];
    void *perst[4]; struct device *dev;
    int late_work; unsigned long down_ports,enabled_ports;
    enum apple_ans_boot_mode ans_mode;
};
static unsigned char rc[0x8000], pb[0x4000], cfg[0x1000], nv[0x100];
static struct apple_h9p_pcie host;
static int pin, full, legacy, writes, gpio, cycles, inactive, event, pin_event, delay_event;
static int disabled_clock, disabled_power, legacy_ports;
static int gpio_error,stop_error,stuck_off,sart;
static jmp_buf jump;
#define panic(...) longjmp(jump,1)
static u32 readl(void *p) {
    if ((unsigned char*)p>=pb && (unsigned char*)p<pb+sizeof(pb))
        assert(*(u32*)(rc+S8003_RC_PORT_CTL(0))&1);
    return *(u32*)p;
}
static void writel(u32 v,void *p) { writes++; if(!stuck_off || p!=rc+S8003_RC_PORT_CTL(0))*(u32*)p=v; }
static void writew(unsigned short v,void *p) { *(unsigned short*)p=v; }
static void h9p_rmw(void *p,u32 c,u32 s) {
    if (p==rc+S8003_RC_PORT_REFCLK(0) && c==1) { assert(pin_event && inactive); disabled_clock=++event; }
    if (p==rc+S8003_RC_PORT_PWR(0) && c==1) { assert(delay_event>disabled_clock); disabled_power=++event; }
    if (p==rc+S8003_RC_PORT_CTL(0) && c==1) assert(disabled_power);
    writel((readl(p)&~c)|s,p);
}
static int gpiod_get_value_cansleep(void *p) { return pin; }
static int gpiod_direction_output(void *p,int v) { gpio++; pin=v; pin_event=++event; return gpio_error; }
static void udelay(int us) { assert(us==60); delay_event=++event; }
static void cancel_delayed_work_sync(void *p) {}
static int apple_s8003_setup_port_round7(struct apple_h9p_pcie *h,unsigned port) {legacy++;return 0;}
static int apple_s8003_setup_port_full(struct apple_h9p_pcie *h,unsigned port) {full++;cycles++;return 0;}
static int apple_s8003_setup_port_reset(struct apple_h9p_pcie *h,unsigned port,bool keep_power) {
    assert(keep_power);full++;return 0;
}
static void apple_s8003_port_stop(struct apple_h9p_pcie *h,unsigned port) {assert(pin);}
static int apple_s8003_port_wait(struct apple_h9p_pcie *h,unsigned p,void *a,u32 m,u32 v,unsigned t,const char *s) {
    assert(pin && inactive && !disabled_clock);return stop_error;
}
static void h9p_writel_flush(u32 v,void *p) {
    assert(pin && inactive && !disabled_clock && p==nv+H9P_NVMMU_SART_CTRL && !v);sart++;
}
static void apple_s8003_port_off(struct apple_h9p_pcie *h,unsigned port) {
    h9p_rmw(rc+S8003_RC_PORT_REFCLK(port),1,0);udelay(60);
    h9p_rmw(rc+S8003_RC_PORT_PWR(port),1,0);h9p_rmw(rc+S8003_RC_PORT_CTL(port),1,0);
}
static void apple_h9p_pcie_park_round7(struct apple_h9p_pcie *h,unsigned long ports) {legacy_ports=ports;}
static struct apple_h9p_pcie *apple_ans_host(struct device *d) {return &host;}
static int apple_ans_dart_set_active(struct device *d,bool active) {assert(!active);inactive++;return 0;}
#include "production.h"
static void init(void) {
    memset(rc,0,sizeof(rc));memset(pb,0,sizeof(pb));memset(cfg,0,sizeof(cfg));
    host=(struct apple_h9p_pcie){.base_rc=rc,.base_config=cfg,.ans_mode=APPLE_ANS_HANDOFF,.enabled_ports=3,
        .nvmmu={.active=true,.ready=true,.base=nv}};
    host.base_port[0]=pb; host.base_port[1]=pb;
    *(u32*)(rc+S8003_RC_PORT_CTL(0))=1;
    *(u32*)(rc+S8003_RC_PORT_PWR(0))=1;
    *(u32*)(rc+S8003_RC_PORT_REFCLK(0))=1;
    *(u32*)(rc+S8003_RC_PORT_READY(0))=1;
    *(u32*)(pb+S8003_PORT_LINKSTS)=1;
    *(u32*)(pb+S8003_PORT_LINKCDMSTS)=0x11<<9;
    pin=full=legacy=writes=gpio=cycles=inactive=event=0;
    gpio_error=stop_error=stuck_off=sart=disabled_clock=disabled_power=legacy_ports=0;
}
int main(void) {
    init(); assert(!apple_s8003_setup_port_once(&host,0));
    assert(full==1 && !writes && !gpio && !inactive);
    unsigned offsets[]={S8003_RC_PORT_CTL(0),S8003_RC_PORT_PWR(0),
        S8003_RC_PORT_REFCLK(0),S8003_RC_PORT_READY(0)};
    for(int i=0;i<4;i++) {
        init(); *(u32*)(rc+offsets[i])=0;
        assert(!apple_s8003_setup_port_once(&host,0));assert(full==1);
    }
    init(); pin=1; assert(!apple_s8003_setup_port_once(&host,0) && full==1);
    init(); *(u32*)(pb+S8003_PORT_LINKCDMSTS)=3<<9;
    assert(!apple_s8003_setup_port_once(&host,0) && full==1);
    init(); assert(!apple_ans_restart_port(NULL));
    assert(host.ans_mode==APPLE_ANS_FULL_CYCLE && full==1 && inactive==1);
    assert(!apple_ans_restart_port(NULL));
    assert(host.ans_mode==APPLE_ANS_HANDOFF && full==2 && inactive==2);
    init(); host.ans_mode=APPLE_ANS_ROUND7;
    assert(apple_ans_restart_port(NULL)==-EOPNOTSUPP);
    assert(!apple_s8003_setup_port_once(&host,0) && legacy==1);
    for(int m=APPLE_ANS_HANDOFF;m<=APPLE_ANS_FULL_CYCLE;m++) {
        init();host.ans_mode=m;inactive=1;apple_h9p_pcie_park(&host,3);
        assert(pin && disabled_clock<delay_event && delay_event<disabled_power && legacy_ports==2);
        assert(sart==1 && !host.nvmmu.ready && !host.nvmmu.active && host.enabled_ports==2 && (host.down_ports&1));
        assert(!(*(u32*)(rc+S8003_RC_PORT_REFCLK(0))&1));
        assert(!(*(u32*)(rc+S8003_RC_PORT_PWR(0))&1));
        assert(!(*(u32*)(rc+S8003_RC_PORT_CTL(0))&1));
        for(int fail=0;fail<3;fail++) {
            init();host.ans_mode=m;inactive=1;
            gpio_error=fail==0?-EIO:0;stop_error=fail==1?-ETIMEDOUT:0;stuck_off=fail==2;
            if(!setjmp(jump)){apple_h9p_pcie_park(&host,3);assert(0);}
            assert(host.enabled_ports==3);
        }
    }
    init();host.ans_mode=APPLE_ANS_ROUND7;apple_h9p_pcie_park(&host,3);assert(legacy_ports==3 && !gpio);
    puts("PASS both trial modes retire SART then fully down port 0; failed isolation refuses kexec; Round7/Wi-Fi unchanged");
}
