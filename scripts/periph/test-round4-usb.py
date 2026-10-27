#!/usr/bin/env python3
"""Offline fault tests for actual gadget-current authorization functions.

Run through scripts/build-kernel.sh --run periph. Synthetic nodes/requests;
no USB connection, hardware access, real identity, or PMIC writes.
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TREE = Path('/src/wt/periph')
OUT = ROOT / 'build/periph/round4-usb'

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef int16_t s16;
#define ENOTSUPP 524
#define le16_to_cpu(v) (v)
#define EXPORT_SYMBOL_GPL(v)
#define ATOMIC_NOTIFIER_HEAD(v) int v
#define min(a,b) ((a)<(b)?(a):(b))
#define clamp(x,lo,hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
#define dev_info_ratelimited(...) ((void)0)
#define trace_usb_gadget_vbus_draw(...) ((void)0)
#define trace_usb_gadget_set_state(...) ((void)0)
enum usb_device_state { USB_STATE_NOTATTACHED=0, USB_STATE_ATTACHED=1,
                        USB_STATE_POWERED=3, USB_STATE_DEFAULT=4,
                        USB_STATE_ADDRESS=6, USB_STATE_CONFIGURED=7, USB_STATE_SUSPENDED=9 };
struct device_node { int id; };
struct device { struct device *parent; struct device_node *of_node; };
struct notifier_block { int unused; };
struct usb_ctrlrequest { uint8_t bRequestType,bRequest; uint16_t wValue,wIndex,wLength; };
struct usb_gadget_current_budget {
    const struct device_node *of_node; unsigned int current_ma;
    bool host_grant,configured,connected,suspended;
};
struct usb_gadget;
struct usb_gadget_ops { int (*vbus_draw)(struct usb_gadget *, unsigned int); };
struct usb_gadget {
    struct device dev; enum usb_device_state state; int state_lock,work; bool teardown;
    unsigned int current_base_ma; s16 current_extra_ma;
    bool current_host_grant,current_suspended,current_configured,current_bus_connected,current_budget_enabled;
    const struct usb_gadget_ops *ops; unsigned int mA;
};
struct usb_udc { struct usb_gadget *gadget; };
static int udc_lock, udc_list, lock_depth, notifier_calls, work_calls, assertions;
static int normal_complete_calls, physical_error;
static struct usb_udc *registered_udc;
static struct usb_gadget_current_budget last_notice;
#define spin_lock_irqsave(lock,flags) do { (flags)=0; assert(!lock_depth); lock_depth++; } while(0)
#define spin_unlock_irqrestore(lock,flags) do { assert(lock_depth==1); lock_depth--; } while(0)
#define list_for_each_entry(pos,head,member) \
    for ((void)(head),pos=registered_udc; pos; pos=NULL)
static void mutex_lock(int *p) { assert(!*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }
static struct device_node *dev_of_node(struct device *dev) { return dev->of_node; }
static int atomic_notifier_chain_register(int *h, struct notifier_block *nb) { return 0; }
static int atomic_notifier_chain_unregister(int *h, struct notifier_block *nb) { return 0; }
static void atomic_notifier_call_chain(int *h, unsigned long event, void *data) {
    assert(!lock_depth); notifier_calls++; memcpy(&last_notice,data,sizeof(last_notice));
}
static void schedule_work(int *w) { work_calls++; }
struct usb_composite_dev { struct usb_gadget *gadget; };
struct usb_ep { int unused; };
struct usb_request { void *context; int status; };
static void composite_setup_complete(struct usb_ep *ep, struct usb_request *req) {
    normal_complete_calls++;
}
'''

TESTS = r'''
static struct device_node node={1}, other={2};
static struct device parent={.of_node=&node};
static struct usb_gadget gadget;
static struct usb_udc udc={.gadget=&gadget};
static struct usb_gadget_current_budget snap;
static struct usb_ctrlrequest request;
static int physical_draw(struct usb_gadget *g,unsigned int ma) { return physical_error; }
static const struct usb_gadget_ops ops={.vbus_draw=physical_draw}, no_ops;
#define check(v) do { assert(v); assertions++; } while (0)
static void reset(void) {
    memset(&gadget,0,sizeof(gadget)); gadget.dev.parent=&parent;
    gadget.current_budget_enabled=true; gadget.state=USB_STATE_CONFIGURED;
    gadget.current_configured=true;
    gadget.current_bus_connected=true;
    gadget.current_base_ma=500; gadget.ops=&ops; registered_udc=&udc;
    request=(struct usb_ctrlrequest){.bRequestType=0x40,.bRequest=0x40,.wValue=500,.wIndex=1900};
    physical_error=0; notifier_calls=0; normal_complete_calls=0;
}
static void snapshot(void) { check(!usb_gadget_get_current_budget(&node,&snap)); }
static void granted(int expected) {
    check(!usb_gadget_handle_current_grant(&gadget,&request)); snapshot();
    check(snap.host_grant && snap.current_ma==(unsigned int)expected);
}
int main(void) {
    reset(); snapshot(); check(snap.current_ma==500 && !snap.host_grant);
    check(snap.connected && snap.configured && !snap.suspended && snap.of_node==&node);
    granted(2400); check(notifier_calls==1 && last_notice.current_ma==2400);
    reset(); request.bRequestType=0xc0; granted(2400);
    reset(); request.wIndex=0; granted(500);
    reset(); request.wIndex=(uint16_t)-200; granted(300);
    reset(); request.wIndex=(uint16_t)-2400; granted(0);
    reset(); request.wIndex=2400; granted(2400);
    reset(); gadget.current_base_ma=0; granted(1900);
    reset(); gadget.state=USB_STATE_DEFAULT; gadget.current_base_ma=100; granted(2400);
    reset(); usb_gadget_set_state(&gadget,USB_STATE_NOTATTACHED); snapshot();
    check(!snap.connected && !snap.current_ma);
    reset(); gadget.current_budget_enabled=false;
    check(usb_gadget_handle_current_grant(&gadget,&request)==-EOPNOTSUPP);
    check(!notifier_calls && !gadget.current_host_grant);
    check(usb_gadget_get_current_budget(&node,&snap)==-ENODEV);
    reset(); request.bRequestType=0x41;
    check(usb_gadget_handle_current_grant(&gadget,&request)==-EOPNOTSUPP);
    reset(); request.bRequestType=0xc1;
    check(usb_gadget_handle_current_grant(&gadget,&request)==-EOPNOTSUPP);
    reset(); request.bRequest=0x41;
    check(usb_gadget_handle_current_grant(&gadget,&request)==-EOPNOTSUPP);
    reset(); granted(2400); request.wLength=1;
    check(usb_gadget_handle_current_grant(&gadget,&request)==-EINVAL);
    snapshot(); check(!snap.host_grant && snap.current_ma==500);
    reset(); granted(2400); request.wLength=65535;
    check(usb_gadget_handle_current_grant(&gadget,&request)==-EINVAL);
    snapshot(); check(!snap.host_grant && snap.current_ma==500);
    const int invalid[][2]={{2401,500},{-2401,500},{32767,0},{-32768,0},{0,2401},{0,-1}};
    for (unsigned int i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        reset(); granted(2400); request.wIndex=invalid[i][0]; request.wValue=invalid[i][1];
        check(usb_gadget_handle_current_grant(&gadget,&request)==-EINVAL);
        snapshot(); check(!snap.host_grant && snap.current_ma==500);
    }
    reset(); granted(2400); usb_gadget_current_budget_suspend(&gadget,true);
    snapshot(); check(snap.suspended && !snap.host_grant && !snap.current_ma);
    check(usb_gadget_handle_current_grant(&gadget,&request)==-ESHUTDOWN);
    usb_gadget_current_budget_suspend(&gadget,false); snapshot();
    check(!snap.host_grant && !snap.suspended && snap.current_ma==500);
    reset(); granted(2400); usb_gadget_set_state(&gadget,USB_STATE_SUSPENDED); snapshot();
    check(snap.configured && snap.suspended && !snap.host_grant && !snap.current_ma);
    usb_gadget_set_state(&gadget,USB_STATE_CONFIGURED); snapshot();
    check(snap.configured && !snap.suspended && !snap.host_grant && snap.current_ma==500);
    reset(); usb_gadget_set_state(&gadget,USB_STATE_DEFAULT);
    usb_gadget_current_budget_suspend(&gadget,true); snapshot();
    check(snap.connected && !snap.configured && snap.suspended && !snap.host_grant && !snap.current_ma);
    reset(); usb_gadget_set_state(&gadget,USB_STATE_POWERED);
    usb_gadget_set_state(&gadget,USB_STATE_SUSPENDED); snapshot();
    check(!snap.connected && !snap.configured && snap.suspended && !snap.current_ma);
    usb_gadget_current_budget_suspend(&gadget,false); snapshot(); check(!snap.connected);
    reset(); usb_gadget_set_state(&gadget,USB_STATE_DEFAULT);
    usb_gadget_set_state(&gadget,USB_STATE_SUSPENDED); snapshot();
    check(snap.connected && !snap.configured && snap.suspended && !snap.current_ma);
    reset(); usb_gadget_set_state(&gadget,USB_STATE_ATTACHED); snapshot(); check(!snap.connected);
    usb_gadget_set_state(&gadget,USB_STATE_ADDRESS); snapshot(); check(snap.connected && !snap.configured);
    reset(); usb_gadget_set_state(&gadget,USB_STATE_NOTATTACHED);
    request.bRequest=6;
    check(usb_gadget_handle_current_grant(&gadget,&request)==-EOPNOTSUPP);
    snapshot(); check(snap.connected && !snap.configured && snap.current_ma==100 && !snap.host_grant);
    reset(); granted(2400); usb_gadget_set_state(&gadget,USB_STATE_DEFAULT); snapshot();
    check(snap.connected && !snap.configured && !snap.host_grant && snap.current_ma==100);
    granted(2400); usb_gadget_set_state(&gadget,USB_STATE_NOTATTACHED); snapshot();
    check(!snap.connected && !snap.host_grant && !snap.current_ma);
    reset(); granted(2400); check(!usb_gadget_vbus_draw(&gadget,0)); snapshot();
    check(!snap.host_grant && !snap.current_ma);
    reset(); granted(2400); usb_gadget_revoke_current_grant(&gadget); snapshot();
    check(!snap.host_grant && snap.current_ma==500);
    reset(); granted(2400); struct usb_composite_dev cdev={.gadget=&gadget};
    struct usb_request req={.context=&cdev,.status=-ESHUTDOWN};
    composite_current_grant_complete(NULL,&req); snapshot();
    check(!snap.host_grant && snap.current_ma==500 && normal_complete_calls==1);
    reset(); granted(2400); req.status=0;
    composite_current_grant_complete(NULL,&req); snapshot(); check(snap.host_grant);
    reset(); req.context=NULL; req.status=-ESHUTDOWN;
    composite_current_grant_complete(NULL,&req); check(normal_complete_calls==1);
    reset(); gadget.current_base_ma=100; physical_error=-ENOTSUPP;
    check(usb_gadget_vbus_draw(&gadget,500)==-ENOTSUPP); snapshot(); check(snap.current_ma==500);
    reset(); gadget.current_base_ma=100; gadget.ops=&no_ops;
    check(usb_gadget_vbus_draw(&gadget,500)==-EOPNOTSUPP); snapshot(); check(snap.current_ma==500);
    reset(); check(!usb_gadget_vbus_draw(&gadget,5000)); snapshot(); check(snap.current_ma==500);
    check(usb_gadget_get_current_budget(&other,&snap)==-ENODEV);
    check(usb_gadget_get_current_budget(NULL,&snap)==-EINVAL);
    check(usb_gadget_get_current_budget(&node,NULL)==-EINVAL);
    registered_udc=NULL; check(usb_gadget_get_current_budget(&node,&snap)==-ENODEV);
    check(!lock_depth && !udc_lock);
    printf("PASS: %d assertions: signed grants, board gate, clamp, lifecycle revocation, notifier/query\n",assertions);
    return 0;
}
'''


def function(source, signature):
    start = source.index(signature)
    # All selected functions have an unindented closing brace.
    end = source.index('\n}', start) + 2
    return source[start:end] + '\n'


def verify_applier():
    """Replay the applier on file copies, without a second kernel worktree."""
    replay = OUT / 'applier-replay'
    paths = ('include/linux/usb/gadget.h', 'drivers/usb/gadget/udc/core.c',
             'drivers/usb/gadget/composite.c')
    for relative in paths:
        target = replay / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(subprocess.check_output(
            ['git', 'show', '681cceb6e8:' + relative], cwd=TREE))
    namespace = {'__name__': 'round4_usb_replay'}
    exec((ROOT / 'scripts/periph/round4-usb.py').read_text(), namespace)

    class ReplayPath:
        def __new__(cls, value):
            value = Path(value)
            return value if value.is_absolute() else replay / value

        @staticmethod
        def cwd():
            return TREE

    namespace['Path'] = ReplayPath
    namespace['main']()
    namespace['main']()  # Idempotence.
    for relative in paths:
        assert (replay / relative).read_bytes() == (TREE / relative).read_bytes(), relative
    print('PASS: fresh Round 3 file copies replay to exact USB sources; second apply unchanged')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    verify_applier()
    core = (TREE / 'drivers/usb/gadget/udc/core.c').read_text()
    composite = (TREE / 'drivers/usb/gadget/composite.c').read_text()
    start = core.index('/* Authorization from USB configuration')
    end = core.index('EXPORT_SYMBOL_GPL(usb_gadget_handle_current_grant);', start)
    helpers = core[start:end]
    state = function(core, 'void usb_gadget_set_state(')
    vbus = function(core, 'int usb_gadget_vbus_draw(')
    complete = function(composite, 'static void composite_current_grant_complete(')
    setup_start = composite.index('composite_setup(struct usb_gadget')
    setup = composite[setup_start:composite.index('\nstatic void __composite_disconnect', setup_start)]
    assert setup.index('usb_gadget_handle_current_grant') < setup.index('if (w_length > USB_COMP_EP0_BUFSIZ)')
    assert 'req->complete = composite_current_grant_complete' in setup
    assert 'composite_setup_complete(gadget->ep0, req);' in setup
    queue_failure = setup[setup.index('DBG(cdev, "ep_queue --> %d\\n", value);'):]
    assert queue_failure.index('usb_gadget_revoke_current_grant(gadget);') < queue_failure.index('req->status = 0;')
    assert 'usb_gadget_current_budget_suspend(gadget, true)' in composite
    assert 'usb_gadget_current_budget_suspend(gadget, false)' in composite
    c_path, binary = OUT / 'usb-current-test.c', OUT / 'usb-current-test'
    c_path.write_text(STUBS + helpers + state + vbus + complete + TESTS)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-Wno-unused-but-set-variable', '-g0', '-O1',
                    '-fsanitize=address,undefined', str(c_path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})


if __name__ == '__main__':
    main()
