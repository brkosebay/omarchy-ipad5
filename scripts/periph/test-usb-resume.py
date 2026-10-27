#!/usr/bin/env python3
"""Exercise actual DWC2/ECM functions with a suspend-rejecting endpoint."""
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/periph/usb-resume'
spec = importlib.util.spec_from_file_location('charging', Path(__file__).with_name('test-charging.py'))
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
typedef int atomic_t;
#define DBG(cdev, ...) do { (void)(cdev); if (0) printf(__VA_ARGS__); } while (0)
#define str_true_false(v) ((v) ? "true" : "false")
#define dev_dbg(...) ((void)0)
#define cpu_to_le16(v) (v)
#define cpu_to_le32(v) (v)
#define GFP_ATOMIC 0
#define ECM_STATUS_BYTECOUNT 16
#define USB_CDC_NOTIFY_NETWORK_CONNECTION 0
#define USB_CDC_NOTIFY_SPEED_CHANGE 0x2a
#define PCGCTL 1
#define DCTL 2
#define PCGCTL_GATEHCLK 1
#define PCGCTL_STOPPCLK 2
#define DCTL_RMTWKUPSIG 4
enum { DWC2_L0, DWC2_L2 };
enum { ECM_NOTIFY_NONE, ECM_NOTIFY_CONNECT, ECM_NOTIFY_SPEED };
struct usb_gadget { int dummy; };
struct usb_composite_dev { struct usb_gadget *gadget; };
struct usb_configuration { struct usb_composite_dev *cdev; };
struct usb_function { struct usb_configuration *config; };
struct gether { struct usb_function func; };
struct usb_ep { int dummy; };
struct usb_request { void *buf, *context; int length, status; };
struct usb_cdc_notification {
    u8 bmRequestType, bNotificationType;
    uint16_t wValue, wIndex, wLength;
};
struct f_ecm {
    struct gether port;
    struct usb_ep *notify;
    struct usb_request *notify_req;
    u8 notify_state, ctrl_id;
    atomic_t notify_count;
    bool is_open;
};
struct dwc2_hsotg { int lx_state; bool bus_suspended; u32 regs[3]; };
static struct dwc2_hsotg hs;
static struct f_ecm ecm;
static int queued, resumes, net_resumes, forced_error;
static u8 last_type;
static int last_connected;
static struct f_ecm *func_to_ecm(struct usb_function *f)
{ return (void *)((char *)f - offsetof(struct f_ecm, port.func)); }
static int atomic_read(atomic_t *v) { return *v; }
static void atomic_inc(atomic_t *v) { ++*v; }
static void atomic_dec(atomic_t *v) { --*v; assert(*v >= 0); }
static void atomic_set(atomic_t *v, int n) { *v = n; }
static int gether_bitrate(struct usb_gadget *g) { (void)g; return 480000000; }
static void gether_resume(struct gether *g) { (void)g; ++net_resumes; }
static int usb_ep_queue(struct usb_ep *ep, struct usb_request *req, int flags)
{
    (void)ep; (void)flags;
    if (hs.lx_state != DWC2_L0) return -EAGAIN;
    if (forced_error) return forced_error;
    struct usb_cdc_notification *n = req->buf;
    last_type = n->bNotificationType;
    last_connected = n->wValue;
    ++queued;
    return 0;
}
static u32 dwc2_readl(struct dwc2_hsotg *h, int reg) { return h->regs[reg]; }
static void dwc2_writel(struct dwc2_hsotg *h, u32 value, int reg) { h->regs[reg] = value; }
static void udelay(int us) { assert(us == 5); }
static void resume_callback(struct dwc2_hsotg *h);
#define call_gadget(h, event) resume_callback(h)
'''

TESTS = r'''
static void resume_callback(struct dwc2_hsotg *h)
{
    ++resumes;
    assert(h->lx_state == DWC2_L0 && !h->bus_suspended);
    ecm_resume(&ecm.port.func);
}
static void reset(void)
{
    static struct usb_gadget gadget;
    static struct usb_composite_dev cdev = {.gadget = &gadget};
    static struct usb_configuration config = {.cdev = &cdev};
    static struct usb_ep ep;
    static _Alignas(4) u8 buf[16];
    static struct usb_request req = {.buf = buf};
    memset(&hs, 0, sizeof(hs));
    memset(&ecm, 0, sizeof(ecm));
    hs.lx_state = DWC2_L0;
    ecm.port.func.config = &config;
    ecm.notify_req = &req; req.context = &ecm; req.status = 0;
    ecm.notify = &ep; ecm.is_open = true;
    queued = resumes = net_resumes = forced_error = 0;
}
static void complete(void) { ecm_notify_complete(ecm.notify, ecm.notify_req); }
int main(int argc, char **argv)
{
    assert(argc == 2);
    reset();
    if (!strcmp(argv[1], "dwc2")) {
        hs.lx_state = DWC2_L2; hs.bus_suspended = true;
        hs.regs[PCGCTL] = PCGCTL_GATEHCLK | PCGCTL_STOPPCLK;
        dwc2_gadget_exit_clock_gating(&hs, 0);
        assert(resumes == 1 && net_resumes == 1 && queued == 1);
        assert(!hs.regs[PCGCTL]);
        assert(last_type == USB_CDC_NOTIFY_NETWORK_CONNECTION && last_connected);
        complete();
        assert(last_type == USB_CDC_NOTIFY_SPEED_CHANGE && queued == 2);
        complete();
        assert(ecm.notify_count == 0 && ecm.notify_state == ECM_NOTIFY_NONE);
    } else {
        /* The CONNECT that tells macOS carrier is up must survive -EAGAIN. */
        hs.lx_state = DWC2_L2;
        ecm_notify(&ecm);
        assert(!queued && !ecm.notify_count && ecm.notify_state == ECM_NOTIFY_CONNECT);
        hs.lx_state = DWC2_L0;
        ecm_resume(&ecm.port.func);
        assert(queued == 1 && last_type == USB_CDC_NOTIFY_NETWORK_CONNECTION && last_connected);
        complete(); complete();
        /* SPEED rejection also retains its place; resume refreshes CONNECT first. */
        ecm_notify(&ecm); forced_error = -EAGAIN; complete();
        assert(!ecm.notify_count && ecm.notify_state == ECM_NOTIFY_SPEED);
        forced_error = 0; ecm_resume(&ecm.port.func);
        assert(last_type == USB_CDC_NOTIFY_NETWORK_CONNECTION);
        /* Resume with an in-flight request cannot double-queue the shared request. */
        int before = queued;
        ecm_resume(&ecm.port.func);
        assert(queued == before && ecm.notify_count == 1);
        complete(); complete(); complete();
        assert(!ecm.notify_count && ecm.notify_state == ECM_NOTIFY_NONE);
        /* Refresh the closed state too; do not announce a fictitious connection. */
        ecm.is_open = false; ecm_resume(&ecm.port.func);
        assert(last_type == USB_CDC_NOTIFY_NETWORK_CONNECTION && !last_connected);
        ecm.notify_req->status = -ESHUTDOWN; complete();
        assert(!ecm.notify_count && ecm.notify_state == ECM_NOTIFY_NONE);
    }
    puts("PASS: actual USB resume/ECM notification functions");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', action='store_true', help='expect both tests to reject the original functions')
    args = p.parse_args()
    suffix = '-before' if args.baseline else ''
    source = (OUT / f'source/f_ecm{suffix}.c').read_text()
    functions = '\n\n'.join(helper.function_text(source, name) for name in
                              ('ecm_do_notify', 'ecm_notify', 'ecm_notify_complete', 'ecm_resume'))
    source = (OUT / f'source/dwc2-gadget{suffix}.c').read_text().replace(
        'void dwc2_gadget_exit_clock_gating(', 'static void dwc2_gadget_exit_clock_gating(')
    functions += '\n\n' + helper.function_text(source, 'dwc2_gadget_exit_clock_gating')
    c = OUT / f'actual-usb{suffix}.c'
    exe = c.with_suffix('')
    c.write_text(STUBS + functions + TESTS)
    cc = shutil.which('clang') or shutil.which('cc')
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-O1',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    for group in ('dwc2', 'ecm'):
        result = subprocess.run([str(exe), group], capture_output=True, text=True)
        print(group, result.stdout, result.stderr, end='')
        if args.baseline:
            assert result.returncode != 0 and 'Assertion' in result.stderr, group
        else:
            assert result.returncode == 0, group
    print('PASS: baseline rejected' if args.baseline else 'PASS: USB resume regressions')


if __name__ == '__main__':
    main()
