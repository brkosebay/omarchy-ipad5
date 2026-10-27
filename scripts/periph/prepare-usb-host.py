#!/usr/bin/env python3
"""Apply the morning host-mode integration fixes to our isolated kernel tree."""
from pathlib import Path
import sys

root = Path(sys.argv[1])

# Migrate the first local integration draft's continuation alignment.
tristar = root / 'drivers/power/supply/apple_tristar_power.c'
text = tristar.read_text()
for spaces in (5, 6):
    text = text.replace('\t' * 5 + ' ' * spaces + 'ts->role_sw);',
                        '\t' * 5 + ' ' * 7 + 'ts->role_sw);')
tristar.write_text(text)


def replace(name, old, new):
    path = root / name
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise SystemExit(f'{name}: expected exactly one replacement point')
    path.write_text(text.replace(old, new))


replace('drivers/phy/apple/ausb.c',
        'static const struct phy_ops ausb_pm_phy_ops = {\n\t.owner = THIS_MODULE,',
        'static const struct phy_ops ausb_pm_phy_ops = {\n'
        '\t.set_mode = ausb_phy_usb2_set_mode,\n\t.owner = THIS_MODULE,')

name = 'drivers/power/supply/apple_tristar_power.c'
replace(name, '#include <linux/usb/gadget.h>',
        '#include <linux/usb/gadget.h>\n#include <linux/usb/role.h>')
replace(name, '\tstruct device_node *usb_node;',
        '\tstruct device_node *usb_node;\n\tstruct usb_role_switch *role_sw;')
replace(name, '\tif (ts->brick_sampled || ts->adc_poisoned || !ts->online || ts->error ||',
        '\t/* The shared host PHY must never be diverted to the analog ADC.\n'
        '\t * Disable analog classification on this dual-controller board, even\n'
        '\t * in device mode, to close the race with a userspace role change.\n'
        '\t * Digital gadget current grants remain available in device mode.\n'
        '\t */\n'
        '\tif (ts->role_sw)\n\t\treturn;\n'
        '\tif (ts->brick_sampled || ts->adc_poisoned || !ts->online || ts->error ||')
replace(name, '\tbool changed;\n\n\tmutex_lock(&ts->lock);',
        '\tbool changed;\n'
        '\t/* Query before taking ts->lock: role changes unbind the gadget. */\n'
        '\tbool host = ts->role_sw &&\n'
        '\t\tusb_role_switch_get_role(ts->role_sw) == USB_ROLE_HOST;\n\n'
        '\tmutex_lock(&ts->lock);')
replace(name, '\tonline = !error && (value & 3) == 3;',
        '\t/* A powered adapter is not a grant to charge from our host port. */\n'
        '\tonline = !host && !error && (value & 3) == 3;')
replace(name, 'static int apple_tristar_probe(struct i2c_client *client)',
        'static void apple_tristar_put_role(void *data)\n{\n'
        '\tusb_role_switch_put(data);\n}\n\n'
        'static int apple_tristar_probe(struct i2c_client *client)')
replace(name, '\tts->client = client;\n\ti2c_set_clientdata(client, ts);',
        '\tts->client = client;\n\ti2c_set_clientdata(client, ts);\n'
        '\tnode = of_parse_phandle(client->dev.of_node, "apple,usb-role-switch", 0);\n'
        '\tif (node) {\n'
        '\t\tts->role_sw = usb_role_switch_find_by_fwnode(of_fwnode_handle(node));\n'
        '\t\tof_node_put(node);\n'
        '\t\tif (!ts->role_sw)\n\t\t\treturn -EPROBE_DEFER;\n'
        '\t\tret = devm_add_action_or_reset(&client->dev, apple_tristar_put_role,\n'
        '\t\t\t\t\t       ts->role_sw);\n'
        '\t\tif (ret)\n\t\t\treturn ret;\n\t}')

replace('arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi',
        '\tperiph_tristar: power-controller@1a {\n',
        '\tperiph_tristar: power-controller@1a {\n'
        '\t\tapple,usb-role-switch = <&usb_role>;\n')
replace('Documentation/devicetree/bindings/power/supply/apple,j71t-cbtl1610-power.yaml',
        '  apple,pmic:\n',
        '  apple,usb-role-switch:\n'
        '    $ref: /schemas/types.yaml#/definitions/phandle\n'
        '    description: |\n'
        '      Shared Lightning USB role switch. With this link, analog charger\n'
        '      classification is disabled to avoid rerouting host data pins, and\n'
        '      host mode reports no input current allowance.\n\n'
        '  apple,pmic:\n')
for controller in ('ehci', 'ohci'):
    name = f'drivers/usb/host/{controller}-platform.c'
    replace(name, '\thcd->tpl_support = of_usb_host_tpl_support(dev->dev.of_node);',
            '\t/* J71t ADT port-current-limit = 50 units of 2 mA.\n'
            '\t * Accessory LDO5 is not a general-purpose 5 V supply.\n'
            '\t */\n'
            '\tif (of_device_is_compatible(dev->dev.of_node, "apple,s8000-' + controller + '"))\n'
            '\t\thcd->power_budget = 100;\n\n'
            '\thcd->tpl_support = of_usb_host_tpl_support(dev->dev.of_node);')

fragment = Path(__file__).resolve().parents[2] / 'patches/usbhost/morning.config'
(root / 'arch/arm64/configs/apple_a9_usb_host.config').write_text(fragment.read_text())
print('USB host integration edits applied (idempotent)')
