#!/usr/bin/env python3
"""Apply the J71t boot Bluetooth identity and known-quirk diagnostic fixes.

Run via scripts/build-kernel.sh --run periph. Does not commit or access hardware.
"""
from pathlib import Path


IDENTITY = r'''
/* Public identities are in HCI byte order here; the firmware file is BMac. */
static bool btbcm_j71t_bdaddr_valid(const bdaddr_t *addr)
{
	return bacmp(addr, BDADDR_ANY) && !(addr->b[5] & 1);
}

static int btbcm_j71t_set_bdaddr(struct hci_dev *hdev)
{
	struct fwnode_handle *fwnode;
	struct hci_rp_read_bd_addr *rp;
	const struct firmware *fw;
	struct sk_buff *skb;
	const char *source;
	bdaddr_t addr;
	int err;

	if (!hdev->dev.parent ||
	    !of_device_is_compatible(hdev->dev.parent->of_node,
				     "apple,j71t-bcm4355-bt"))
		return 0;

	fwnode = dev_fwnode(hdev->dev.parent);
	if (fwnode_property_count_u8(fwnode, "local-bd-address") == sizeof(addr) &&
	    !fwnode_property_read_u8_array(fwnode, "local-bd-address",
					   (u8 *)&addr, sizeof(addr))) {
		if (hci_test_quirk(hdev, HCI_QUIRK_BDADDR_PROPERTY_BROKEN)) {
			bdaddr_t swapped;

			baswap(&swapped, &addr);
			bacpy(&addr, &swapped);
		}
	} else {
		bacpy(&addr, BDADDR_ANY);
	}

	source = "device-tree";
	if (!btbcm_j71t_bdaddr_valid(&addr)) {
		/* Direct loading cannot stall a RAM boot in the sysfs fallback. */
		err = request_firmware_direct(&fw, "apple/j71t-BMac.bin",
					      &hdev->dev);
		if (err) {
			bt_dev_err(hdev, "BCM: No valid J71t device address (%d)", err);
			return err;
		}
		if (fw->size != sizeof(addr)) {
			release_firmware(fw);
			return -EINVAL;
		}
		/* Resolved iOS BMac is stored in display/network byte order. */
		baswap(&addr, (const bdaddr_t *)fw->data);
		release_firmware(fw);
		if (!btbcm_j71t_bdaddr_valid(&addr))
			return -EINVAL;
		source = "private BMac";
	}

	/* Program after the last Patchram reset, before the default-address check. */
	err = btbcm_set_bdaddr(hdev, &addr);
	if (err)
		return err;

	skb = __hci_cmd_sync(hdev, HCI_OP_READ_BD_ADDR, 0, NULL,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);
	if (skb->len != sizeof(*rp)) {
		kfree_skb(skb);
		return -EIO;
	}
	rp = (struct hci_rp_read_bd_addr *)skb->data;
	err = rp->status || bacmp(&rp->bdaddr, &addr) ? -EIO : 0;
	kfree_skb(skb);
	if (err) {
		bt_dev_err(hdev, "BCM: J71t device address readback failed");
		return err;
	}

	/* Keep MGMT reconfiguration consistent with the checked controller state. */
	bacpy(&hdev->public_addr, &addr);
	hci_clear_quirk(hdev, HCI_QUIRK_INVALID_BDADDR);
	hci_clear_quirk(hdev, HCI_QUIRK_USE_BDADDR_PROPERTY);
	bt_dev_info(hdev, "BCM: Using %s device address (%pMR)", source, &addr);
	return 0;
}
'''


def replace(path, old, new):
    source = path.read_text()
    if new in source:
        return
    if source.count(old) != 1:
        raise RuntimeError(f'expected one source match: {path}')
    path.write_text(source.replace(old, new, 1))


def main():
    if Path.cwd() != Path('/src/wt/periph'):
        raise RuntimeError('run only in /src/wt/periph')
    bt = Path('drivers/bluetooth/btbcm.c')
    replace(bt, '#include <linux/of.h>\n',
            '#include <linux/of.h>\n#include <linux/property.h>\n')
    replace(bt, '\nint btbcm_finalize(struct hci_dev *hdev,',
            IDENTITY + '\nint btbcm_finalize(struct hci_dev *hdev,')
    replace(bt, '\tbtbcm_check_bdaddr(hdev);\n\n\thci_set_quirk',
            '\terr = btbcm_j71t_set_bdaddr(hdev);\n'
            '\tif (err)\n\t\treturn err;\n\n'
            '\tbtbcm_check_bdaddr(hdev);\n\n\thci_set_quirk')
    bcm = Path('drivers/bluetooth/hci_bcm.c')
    replace(bcm, '/* Also required by J71t BCM4355C0; the core logs active quirks. */',
            '/* Also required by the J71t BCM4355C0 controller. */')
    sync = Path('net/bluetooth/hci_sync.c')
    replace(sync, '#define HCI_QUIRK_BROKEN(_quirk, _desc) { HCI_QUIRK_BROKEN_##_quirk, _desc }',
            '#define HCI_QUIRK_BROKEN(_quirk, _desc) \\\n'
            '\t{ HCI_QUIRK_BROKEN_##_quirk, _desc, false }')
    replace(sync, '\tconst char *desc;\n} hci_broken_table[]',
            '\tconst char *desc;\n\tbool quiet;\n} hci_broken_table[]')
    replace(sync,
            '\tHCI_QUIRK_BROKEN(READ_TRANSMIT_POWER,\n'
            '\t\t\t "HCI Read Transmit Power Level command is advertised, "\n'
            '\t\t\t "but not supported."),',
            '\t/* This established workaround still blocks the unsupported command. */\n'
            '\t{ HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER,\n'
            '\t  "HCI Read Transmit Power Level command is advertised, but not supported.", true },')
    replace(sync,
            '\t\tif (hci_test_quirk(hdev, hci_broken_table[i].quirk))\n'
            '\t\t\tbt_dev_warn(hdev, "%s", hci_broken_table[i].desc);',
            '\t\tif (!hci_test_quirk(hdev, hci_broken_table[i].quirk))\n'
            '\t\t\tcontinue;\n'
            '\t\tif (hci_broken_table[i].quiet)\n'
            '\t\t\tbt_dev_dbg(hdev, "%s", hci_broken_table[i].desc);\n'
            '\t\telse\n'
            '\t\t\tbt_dev_warn(hdev, "%s", hci_broken_table[i].desc);')


if __name__ == '__main__':
    main()
