#!/usr/bin/env python3
"""Install staged offline touch transport changes into the touch kernel only.

Run with scripts/build-kernel.sh --run touch
  'python3 /work/scripts/touch/transport-install.py'
Does not configure, build, commit, or access the device.
"""
from pathlib import Path
import shutil


def main():
    tree = Path.cwd()
    if tree != Path('/src/wt/touch'):
        raise ValueError('run only in /src/wt/touch')
    source = Path('/work/build/touch/transport-re')
    files = {
        'spi-apple-s8000.c': 'drivers/spi/spi-apple-s8000.c',
        'clk-apple-s8000-touch.c': 'drivers/clk/clk-apple-s8000-touch.c',
        'apple-d2365-touch.c': 'drivers/regulator/apple-d2365-touch.c',
        's8003-j71t-touch.dtsi': 'arch/arm64/boot/dts/apple/s8003-j71t-touch.dtsi',
    }
    for name, relative in files.items():
        shutil.copyfile(source / name, tree / relative)
    configs = {
        'drivers/spi/Kconfig': ('config SPI_APPLE\n', '''config SPI_APPLE_S8000
\ttristate "Apple S8000 Samsung SPI v1 PIO controller"
\tdepends on ARCH_APPLE || COMPILE_TEST
\tdepends on OF && COMMON_CLK
\thelp
\t  Bounded byte-wide PIO support for the legacy SPI controller on A9.

'''),
        'drivers/clk/Kconfig': ('config COMMON_CLK_APPLE_NCO\n', '''config COMMON_CLK_APPLE_S8000_TOUCH
\ttristate "Apple S8000 touchscreen clock"
\tdepends on ARCH_APPLE || COMPILE_TEST
\tdepends on OF
\tselect MFD_SYSCON
\thelp
\t  S8000 PMGR TCLK gate and divider for the J71t N1 touchscreen.

'''),
        'drivers/regulator/Kconfig': ('config REGULATOR_88PG86X\n', '''config REGULATOR_APPLE_D2365_TOUCH
\ttristate "Apple D2365 touchscreen analog supply"
\tdepends on OF && REGMAP
\thelp
\t  Enable-only D2365 analog touch supply at register 0x031c bit zero.
\t  The parent apple,i2c-pmic must provide the register map.

'''),
    }
    for relative, (marker, content) in configs.items():
        path = tree / relative
        text = path.read_text()
        symbol = content.splitlines()[0] + '\n'
        if symbol not in text:
            if marker not in text:
                raise ValueError('missing Kconfig marker ' + marker)
            path.write_text(text.replace(marker, content + marker, 1))
    makefiles = {
        'drivers/spi/Makefile': 'obj-$(CONFIG_SPI_APPLE_S8000) += spi-apple-s8000.o\n',
        'drivers/clk/Makefile': 'obj-$(CONFIG_COMMON_CLK_APPLE_S8000_TOUCH) += clk-apple-s8000-touch.o\n',
        'drivers/regulator/Makefile': 'obj-$(CONFIG_REGULATOR_APPLE_D2365_TOUCH) += apple-d2365-touch.o\n',
    }
    for relative, line in makefiles.items():
        path = tree / relative
        text = path.read_text()
        if line not in text:
            path.write_text(text + line)
    path = tree / 'arch/arm64/boot/dts/apple/s8003-j71t.dts'
    line = '#include "s8003-j71t-touch.dtsi"\n'
    text = path.read_text()
    if line not in text:
        path.write_text(text + '\n' + line)
    print('Installed touch SPI, TCLK, analog supply and J71t DT; .config unchanged')


if __name__ == '__main__':
    main()
