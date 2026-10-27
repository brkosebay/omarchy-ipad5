#!/usr/bin/env python3
"""Apply the USB resume fix in a9/periph (run through build-kernel.sh)."""
from pathlib import Path


def replace_once(path, old, new):
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise SystemExit(f'{path}: expected exactly one source anchor')
    path.write_text(text.replace(old, new, 1))


def main():
    replace_once(Path('drivers/usb/dwc2/gadget.c'),
                 '\t/* Change to L0 state */\n'
                 '\tcall_gadget(hsotg, resume);\n'
                 '\thsotg->lx_state = DWC2_L0;\n'
                 '\thsotg->bus_suspended = false;\n',
                 '\t/* Resume callbacks may immediately queue endpoint requests. */\n'
                 '\thsotg->lx_state = DWC2_L0;\n'
                 '\thsotg->bus_suspended = false;\n'
                 '\tcall_gadget(hsotg, resume);\n')
    path = Path('drivers/usb/gadget/function/f_ecm.c')
    replace_once(path,
                 '\tint\t\t\t\tstatus;\n\n\t/* notification already in flight? */',
                 '\tint\t\t\t\tstatus;\n'
                 '\tu8\t\t\t\tstate = ecm->notify_state;\n\n'
                 '\t/* notification already in flight? */')
    replace_once(path,
                 '\tif (status < 0) {\n\t\tatomic_dec(&ecm->notify_count);\n'
                 '\t\tDBG(cdev, "notify --> %d\\n", status);\n',
                 '\tif (status < 0) {\n\t\tatomic_dec(&ecm->notify_count);\n'
                 '\t\t/* A suspended UDC can reject the request with -EAGAIN. */\n'
                 '\t\tecm->notify_state = state;\n'
                 '\t\tDBG(cdev, "notify --> %d\\n", status);\n')
    replace_once(path,
                 '\tgether_resume(&ecm->port);\n}',
                 '\tgether_resume(&ecm->port);\n'
                 '\t/* Refresh host carrier, including notifications lost at suspend. */\n'
                 '\tecm_notify(ecm);\n}')


if __name__ == '__main__':
    main()
