#!/usr/bin/env python3
"""Install the reviewed J71t charging experiment in /src/wt/periph only.

Run through scripts/build-kernel.sh --run periph. Does not commit anything.
"""
from pathlib import Path
import shutil


def replace(path, old, new):
    p = Path(path)
    text = p.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise ValueError('unexpected source: ' + path)
    p.write_text(text.replace(old, new, 1))


if Path.cwd() != Path('/src/wt/periph'):
    raise ValueError('run only in /src/wt/periph')
for name in ('apple_tristar_power.c', 'apple_j71t_charger.c'):
    shutil.copyfile('/work/scripts/periph/kernel/' + name, 'drivers/power/supply/' + name)
replace('drivers/power/supply/Makefile',
        'obj-$(CONFIG_APPLE_D2365_CHARGER) += apple_d2365_charger.o',
        'obj-$(CONFIG_APPLE_D2365_CHARGER) += apple_d2365_charger.o apple_j71t_charger.o apple_tristar_power.o')
replace('drivers/power/supply/apple_d2365_charger.c',
        '\t{ .compatible = "apple,j71t-d2365-charger" },\n',
        '\t/* J71t has a separate, explicitly armed charging driver. */\n')
replace('drivers/power/supply/Kconfig',
        '\t  This driver can also be built as a module. If so, the module will be\n\t  called apple_d2365_charger.',
        '\t  J71t also provides read-only CBTL1610 VBUS detection and a separately\n'
        '\t  armed 2400 mA policy for an owner-verified USB source. It requires\n'
        '\t  private CBAT calibration and safe battery telemetry, preserves the\n'
        '\t  handoff settings, and revokes the request on disconnect or error.\n\n'
        '\t  The modules are apple_d2365_charger, apple_j71t_charger and\n'
        '\t  apple_tristar_power.')
replace('arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi',
        '\t\t/* Status only: J172 input-current policy must never run on J71t. */\n',
        '\t\t/* Current policy is explicitly armed for the verified Mac source. */\n')
replace('arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi',
        '\t\treg-names = "status", "control";\n',
        '\t\treg-names = "status", "control";\n\t\tpower-supplies = <&periph_tristar>;\n')
replace('Documentation/devicetree/bindings/power/supply/apple,d2365-charger.yaml',
        '  The J172 variant uses a platform-calibrated, guarded input-current policy.',
        '  The J172 variant uses a platform-calibrated, guarded input-current policy.\n'
        '  J71t has an explicitly armed policy for a verified USB source, requiring\n'
        '  private input-current calibration and live Tristar source presence.')
binding = Path('Documentation/devicetree/bindings/power/supply/apple,d2365-charger.yaml')
text = binding.read_text()
old = '''  - if:
      properties:
        compatible:
          const: apple,j71t-d2365-charger
    then:
      properties:
        power-supplies: false
    else:
      required:
        - power-supplies'''
if old in text:
    binding.write_text(text.replace(old, '  - required:\n      - power-supplies'))
Path('Documentation/devicetree/bindings/power/supply/apple,j71t-cbtl1610-power.yaml').write_text('''# SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
%YAML 1.2
---
$id: http://devicetree.org/schemas/power/supply/apple,j71t-cbtl1610-power.yaml#
$schema: http://devicetree.org/meta-schemas/core.yaml#

title: Apple J71t CBTL1610 USB source presence

maintainers:
  - linux-a9 <linux-a9@localhost>

description: |
  Read-only VBUS presence for the CBTL1610 revision used by J71t. The driver
  does not configure accessory routing or determine a source current budget.

properties:
  compatible:
    const: apple,j71t-cbtl1610-power

  reg:
    maxItems: 1

required:
  - compatible
  - reg

additionalProperties: false

examples:
  - |
    i2c {
        #address-cells = <1>;
        #size-cells = <0>;
        power-controller@1a {
            compatible = "apple,j71t-cbtl1610-power";
            reg = <0x1a>;
        };
    };
''')
path = Path('arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi')
if 'periph_tristar: power-controller@1a' not in path.read_text():
    with path.open('a') as stream:
        stream.write('\n&i2c0 {\n\tperiph_tristar: power-controller@1a {\n'
                     '\t\tcompatible = "apple,j71t-cbtl1610-power";\n'
                     '\t\treg = <0x1a>;\n\t};\n};\n')
