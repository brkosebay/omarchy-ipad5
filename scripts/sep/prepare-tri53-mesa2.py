#!/usr/bin/env python3
"""Apply the bounded Mesa2 sources to the reused, clean Mesa1 worktree.

Run inside the builder, cwd=/src/wt/tri53-mesa1, repository mounted at /work.
Branch creation/commit are deliberately left to build-tri53-mesa2.sh.
"""
from pathlib import Path
import shutil
import subprocess


def replace_once(path, before, after):
    p = Path(path)
    text = p.read_text()
    assert text.count(before) == 1, (path, before)
    p.write_text(text.replace(before, after, 1))


def main():
    for source, target in (
        ('mesa-a9.c', 'drivers/mesa/mesa-a9.c'),
        ('spi-apple-s8000-phase.h', 'drivers/spi/spi-apple-s8000-phase.h'),
        ('s8003-j71t-mesa-status.dtsi', 'arch/arm64/boot/dts/apple/s8003-j71t-mesa-status.dtsi'),
    ):
        shutil.copyfile(Path('/work/drivers/mesa') / source, target)
    path = 'drivers/spi/spi-apple-s8000.c'
    # Reproducible refresh, including after a compiler-detected correction.
    Path(path).write_bytes(subprocess.check_output([
        'git', 'show', 'cc0b92c36c7917d3b11ee7ad845e27b266f9fc2c:' + path]))
    replace_once(path, '#include <linux/pm_runtime.h>\n',
                 '#include <linux/pm_runtime.h>\n#include <linux/property.h>\n')
    replace_once(path, '#define S8000_CTRL\t\t0x00',
                 '#include "spi-apple-s8000-phase.h"\n\n#define S8000_CTRL\t\t0x00')
    replace_once(path, 'u32 divisor, cfg, status, tx_level, rx_level;',
                 'u32 divisor, cfg, status, tx_level, rx_level, phase;')
    replace_once(path,
                 '\tif (device->mode & SPI_CPOL)\n\t\tcfg |= S8000_CFG_CPOL;\n',
                 '''\tif (device->mode & SPI_CPOL)
\t\tcfg |= S8000_CFG_CPOL;

\t/* Per-child opt-in: all other SPI clients retain their existing path. */
\tif (device_property_present(&device->dev, "apple,s8000-spi-clock-phase")) {
\t\tret = device_property_read_u32(&device->dev,
\t\t\t\t"apple,s8000-spi-clock-phase", &phase);
\t\tif (ret)
\t\t\treturn ret;
\t\tret = s8000_spi_apply_phase(device->mode, phase, &cfg,
\t\t\t\t\t    &word_delay_cycles);
\t\tif (ret)
\t\t\treturn ret;
\t}
''')
    replace_once(path,
                 '\t\tdev_info(&device->dev, "PIO begin %u len=%u speed=%u\\n",\n'
                 '\t\t\t spi->traced_xfers, transfer->len, transfer->effective_speed_hz);',
                 '\t\tdev_info(&device->dev, "PIO begin %u len=%u speed=%u cfg=%#x word_delay=%llu pin=%#x\\n",\n'
                 '\t\t\t spi->traced_xfers, transfer->len, transfer->effective_speed_hz,\n'
                 '\t\t\t (u32)(cfg | S8000_CFG_PIO), word_delay_cycles,\n'
                 '\t\t\t readl(spi->base + S8000_PIN));')


if __name__ == '__main__':
    main()
