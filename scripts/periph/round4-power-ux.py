#!/usr/bin/env python3
"""Apply the J71t PMIC register-address DT fix, preserving legacy PMIC users.

Run inside the kernel build container; this changes only the named worktree
files and never commits or accesses hardware. Battery telemetry needs no
change for UPower's existing charge-unit backend.
"""
import argparse
from pathlib import Path


def replace_once(path, before, after):
    text = path.read_text()
    if after in text:
        return
    if text.count(before) != 1:
        raise SystemExit(f'{path}: expected exactly one source hunk')
    path.write_text(text.replace(before, after, 1))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, default=Path('/src/wt/periph'))
    args = ap.parse_args()
    root = args.kernel.resolve()
    if root != Path('/src/wt/periph'):
        raise SystemExit('only /src/wt/periph is authorized')

    replace_once(root / 'drivers/nvmem/simple-mfd-nvmem.c',
                 '\tint len;\n\tstruct nvmem_config nvmem_cfg = {',
                 '\tint len, ret;\n\tu32 size;\n\tstruct nvmem_config nvmem_cfg = {')
    replace_once(root / 'drivers/nvmem/simple-mfd-nvmem.c', '''
	if (len != 2 * sizeof(u32)) {
		dev_err(&pdev->dev, "Invalid reg property\\n");
		return -EINVAL;
	}

	nvmem->base = be32_to_cpup(&addr[0]);
	nvmem_cfg.size = be32_to_cpup(&addr[1]);
''', '''
	/* Address-only PMIC buses describe register count separately. */
	if (of_n_size_cells(pdev->dev.of_node) == 0 && len == sizeof(u32)) {
		ret = of_property_read_u32(pdev->dev.of_node, "size", &size);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "Missing NVMEM size\\n");
	} else if (of_n_size_cells(pdev->dev.of_node) == 1 &&
		   len == 2 * sizeof(u32)) {
		/* Preserve the original <base size> binding for other boards. */
		size = be32_to_cpup(&addr[1]);
	} else {
		return dev_err_probe(&pdev->dev, -EINVAL, "Invalid reg property\\n");
	}
	if (!size)
		return dev_err_probe(&pdev->dev, -EINVAL, "Empty NVMEM range\\n");

	nvmem->base = be32_to_cpup(&addr[0]);
	nvmem_cfg.size = size;
''')

    replace_once(root / 'Documentation/devicetree/bindings/mfd/apple,pmic.yaml',
                 '  "#size-cells":\n    const: 1\n',
                 '''  "#size-cells":
    enum: [0, 1]
    description: |
      Zero describes non-MMIO register addresses on the PMIC bus. NVMEM
      children then provide their register count with the size property.
      One preserves the legacy register-address and byte-count tuples.
''')
    replace_once(root / 'Documentation/devicetree/bindings/nvmem/apple,pmic-nvmem.yaml',
                 '  reg:\n    maxItems: 1\n',
                 '''  reg:
    maxItems: 1

  size:
    $ref: /schemas/types.yaml#/definitions/uint32
    minimum: 1
    description:
      Number of byte registers when the parent PMIC has zero size cells.
''')

    board = root / 'arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi'
    replace_once(board, '&{/soc/i2c@20a110000/pmic@3c} {\n', '''&{/soc/i2c@20a110000/pmic@3c} {
	/* Internal register offsets are not CPU MMIO resources. */
	#size-cells = <0>;

	rtc@500 {
		reg = <0x500>;
	};

	nvmem@5000 {
		reg = <0x5000>;
		size = <0x28>;
		read-only;
	};

''')
    replace_once(board, '\t\treg = <0x180 0x1e>, <0xa00 0x0d>;',
                 '\t\treg = <0x180>, <0xa00>;')
    print('Applied J71t address-only PMIC DT plus compatible NVMEM/binding support; no battery property changes')


if __name__ == '__main__':
    main()
