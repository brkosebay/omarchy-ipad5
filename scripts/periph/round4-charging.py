#!/usr/bin/env python3
"""Install Round 4 source-aware charging in the isolated periph kernel tree."""
from pathlib import Path
import shutil

if Path.cwd() != Path('/src/wt/periph'):
    raise ValueError('run only in /src/wt/periph')
for name in ('apple_j71t_charger.c', 'apple_tristar_power.c'):
    shutil.copyfile('/work/scripts/periph/kernel/' + name, 'drivers/power/supply/' + name)

p = Path('drivers/power/supply/Kconfig')
text = p.read_text()
start = text.index('config APPLE_D2365_CHARGER\n')
end = text.index('\nconfig ', start + 1)
block = text[start:end]
if '\tdepends on USB_GADGET || !USB_GADGET' not in block:
    block = block.replace('\tdepends on MFD_SIMPLE_MFD_I2C',
                          '\tdepends on MFD_SIMPLE_MFD_I2C\n\tdepends on USB_GADGET || !USB_GADGET')
block = block.replace('J71t also provides read-only CBTL1610 VBUS detection and a separately\n'
                      '\t  armed 2400 mA policy for an owner-verified USB source. It requires',
                      'J71t also provides CBTL1610 source detection and an automatic\n'
                      '\t  current policy using verified Apple USB host grants and resistor\n'
                      '\t  signatures, with a conservative ordinary USB fallback. It requires')
block = block.replace('handoff settings, and revokes the request on disconnect or error.',
                      'handoff settings, and restores them on disconnect, suspend or error.')
p.write_text(text[:start] + block + text[end:])

p = Path('arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi')
text = p.read_text().replace('/* Current policy is explicitly armed for the verified Mac source. */',
                           '/* Automatic policy uses source authorization and private CBAT. */')
if 'usb-controller = <&usbdev>;' not in text:
    text = text.replace('compatible = "apple,j71t-cbtl1610-power";\n',
                        'compatible = "apple,j71t-cbtl1610-power";\n'
                        '\t\tusb-controller = <&usbdev>;\n'
                        '\t\tapple,pmic = <&{/soc/i2c@20a110000/pmic@3c}>;\n')
if 'apple,host-current-budget;' not in text:
    text += '\n&usbdev {\n\tapple,host-current-budget;\n};\n'
p.write_text(text)

p = Path('Documentation/devicetree/bindings/power/supply/apple,j71t-cbtl1610-power.yaml')
text = p.read_text().replace('title: Apple J71t CBTL1610 USB source presence',
                           'title: Apple J71t CBTL1610 USB source detection')
text = text.replace('  Read-only VBUS presence for the CBTL1610 revision used by J71t. The driver\n'
                    '  does not configure accessory routing or determine a source current budget.',
                    '  VBUS presence, Apple USB host allowances and bounded analog charger\n'
                    '  signature detection for the CBTL1610 revision used by J71t. Temporary\n'
                    '  data-pin monitoring restores the saved route after each measurement.')
if '  usb-controller:' not in text:
    text = text.replace('required:\n',
                        '  usb-controller:\n    $ref: /schemas/types.yaml#/definitions/phandle\n'
                        '    description: USB device controller reporting host current allowances.\n\n'
                        '  apple,pmic:\n    $ref: /schemas/types.yaml#/definitions/phandle\n'
                        '    description: D2365 PMIC providing the brick-ID ADC input.\n\nrequired:\n')
    text = text.replace('  - reg\n', '  - reg\n  - usb-controller\n  - apple,pmic\n')
    text = text.replace('            reg = <0x1a>;',
                        '            reg = <0x1a>;\n'
                        '            usb-controller = <&usbdev>;\n'
                        '            apple,pmic = <&pmic>;')
p.write_text(text)

p = Path('Documentation/devicetree/bindings/usb/dwc2.yaml')
text = p.read_text()
if '  apple,host-current-budget:' not in text:
    text = text.replace('  reg:\n',
                        '  apple,host-current-budget:\n    type: boolean\n'
                        '    description: |\n'
                        '      Enable Apple device-mode current-limit requests and source-budget\n'
                        '      notifications, bounded to the J71t 2400 mA platform maximum.\n\n  reg:\n', 1)
p.write_text(text)
