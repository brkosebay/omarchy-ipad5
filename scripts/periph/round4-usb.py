#!/usr/bin/env python3
"""Apply opt-in J71t USB host current grants and a gadget budget notifier.

Run via scripts/build-kernel.sh --run periph; no commits or device access.
"""
from pathlib import Path

CORE = r'''
/* Authorization from USB configuration and the opt-in Apple host protocol. */
static ATOMIC_NOTIFIER_HEAD(gadget_current_notifier);

static void gadget_current_snapshot(struct usb_gadget *gadget,
				    struct usb_gadget_current_budget *budget)
{
	int base = gadget->current_base_ma;
	int extra = gadget->current_extra_ma;
	int budget_ma = base;

	budget->of_node = dev_of_node(gadget->dev.parent);
	budget->configured = gadget->current_configured;
	budget->connected = gadget->current_bus_connected;
	budget->suspended = gadget->current_suspended ||
			    gadget->state == USB_STATE_SUSPENDED;
	budget->host_grant = gadget->current_host_grant;
	if (budget->host_grant) {
		/* IOUSBDeviceFamily active-bus policy: signed extra is wIndex. */
		budget_ma = base && extra ? 500 + extra : base + extra;
		budget_ma = clamp(budget_ma, 0, 2400);
	}
	budget->current_ma = budget->connected && !budget->suspended ? budget_ma : 0;
}

static void gadget_current_notify(struct usb_gadget *gadget)
{
	struct usb_gadget_current_budget budget;
	unsigned long flags;

	if (!gadget->current_budget_enabled)
		return;
	spin_lock_irqsave(&gadget->state_lock, flags);
	gadget_current_snapshot(gadget, &budget);
	spin_unlock_irqrestore(&gadget->state_lock, flags);
	atomic_notifier_call_chain(&gadget_current_notifier, 0, &budget);
}

/**
 * usb_gadget_register_current_notifier - subscribe to gadget budget changes
 * @nb: notifier whose callback must not sleep or query the budget
 *
 * Callbacks receive a temporary struct usb_gadget_current_budget pointer.
 * A consumer should schedule work, then query the current snapshot by its
 * controller node. Register before the initial query to avoid lost events.
 */
int usb_gadget_register_current_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&gadget_current_notifier, nb);
}
EXPORT_SYMBOL_GPL(usb_gadget_register_current_notifier);

int usb_gadget_unregister_current_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&gadget_current_notifier, nb);
}
EXPORT_SYMBOL_GPL(usb_gadget_unregister_current_notifier);

/**
 * usb_gadget_get_current_budget - query an opted-in controller's authorization
 * @node: controller node held by the caller
 * @budget: returned snapshot; valid only on success
 *
 * Sleepable. A device-tree phandle identifies the supplying controller.
 * The notifier callback must not call this function: the UDC lock may be held.
 */
int usb_gadget_get_current_budget(const struct device_node *node,
				  struct usb_gadget_current_budget *budget)
{
	struct usb_udc *udc;
	unsigned long flags;
	int ret = -ENODEV;

	if (!node || !budget)
		return -EINVAL;
	mutex_lock(&udc_lock);
	list_for_each_entry(udc, &udc_list, list) {
		struct usb_gadget *gadget = udc->gadget;

		if (!gadget->current_budget_enabled ||
		    dev_of_node(gadget->dev.parent) != node)
			continue;
		spin_lock_irqsave(&gadget->state_lock, flags);
		gadget_current_snapshot(gadget, budget);
		spin_unlock_irqrestore(&gadget->state_lock, flags);
		ret = 0;
		break;
	}
	mutex_unlock(&udc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(usb_gadget_get_current_budget);

static void gadget_current_set_base(struct usb_gadget *gadget, unsigned int ma)
{
	unsigned long flags;

	if (!gadget->current_budget_enabled)
		return;
	spin_lock_irqsave(&gadget->state_lock, flags);
	gadget->current_base_ma = min(ma, 500U);
	if (!ma) {
		gadget->current_extra_ma = 0;
		gadget->current_host_grant = false;
	}
	spin_unlock_irqrestore(&gadget->state_lock, flags);
	gadget_current_notify(gadget);
}

/* Also used to revoke a grant when its ep0 status transfer fails. */
void usb_gadget_revoke_current_grant(struct usb_gadget *gadget)
{
	unsigned long flags;

	if (!gadget->current_budget_enabled)
		return;
	spin_lock_irqsave(&gadget->state_lock, flags);
	gadget->current_extra_ma = 0;
	gadget->current_host_grant = false;
	spin_unlock_irqrestore(&gadget->state_lock, flags);
	gadget_current_notify(gadget);
}
EXPORT_SYMBOL_GPL(usb_gadget_revoke_current_grant);

void usb_gadget_current_budget_suspend(struct usb_gadget *gadget, bool suspended)
{
	unsigned long flags;

	if (!gadget->current_budget_enabled)
		return;
	spin_lock_irqsave(&gadget->state_lock, flags);
	gadget->current_suspended = suspended;
	/* Resume requires a fresh grant; do not reuse an earlier high budget. */
	gadget->current_extra_ma = 0;
	gadget->current_host_grant = false;
	spin_unlock_irqrestore(&gadget->state_lock, flags);
	gadget_current_notify(gadget);
}
EXPORT_SYMBOL_GPL(usb_gadget_current_budget_suspend);

int usb_gadget_handle_current_grant(struct usb_gadget *gadget,
				    const struct usb_ctrlrequest *ctrl)
{
	unsigned long flags;
	s16 extra = le16_to_cpu(ctrl->wIndex);
	s16 suspend = le16_to_cpu(ctrl->wValue);
	bool newly_connected = false;

	if (!gadget->current_budget_enabled)
		return -EOPNOTSUPP;
	/* Some UDCs omit DEFAULT/ADDRESS notifications during enumeration. */
	spin_lock_irqsave(&gadget->state_lock, flags);
	if (!gadget->current_bus_connected) {
		gadget->current_bus_connected = true;
		if (!gadget->current_base_ma)
			gadget->current_base_ma = 100;
		newly_connected = true;
	}
	spin_unlock_irqrestore(&gadget->state_lock, flags);
	if (newly_connected)
		gadget_current_notify(gadget);
	if (ctrl->bRequest != 0x40 ||
	    (ctrl->bRequestType != 0x40 && ctrl->bRequestType != 0xc0))
		return -EOPNOTSUPP;
	/* Only the exact no-data device vendor request is reserved on this board. */
	if (ctrl->wLength || extra < -2400 || extra > 2400 ||
	    suspend < 0 || suspend > 2400) {
		usb_gadget_revoke_current_grant(gadget);
		return -EINVAL;
	}
	spin_lock_irqsave(&gadget->state_lock, flags);
	if (gadget->current_suspended) {
		spin_unlock_irqrestore(&gadget->state_lock, flags);
		usb_gadget_revoke_current_grant(gadget);
		return -ESHUTDOWN;
	}
	gadget->current_extra_ma = extra;
	gadget->current_host_grant = true;
	spin_unlock_irqrestore(&gadget->state_lock, flags);
	dev_info_ratelimited(&gadget->dev,
			     "Apple host current grant: extra %d mA, suspend %d mA\n",
			     extra, suspend);
	gadget_current_notify(gadget);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_gadget_handle_current_grant);
'''

HEADER = r'''
struct device_node;
struct notifier_block;

/**
 * struct usb_gadget_current_budget - instantaneous USB power authorization
 * @of_node: controller node, valid for the synchronous callback or caller query
 * @current_ma: active authorization in mA, zero when disconnected or suspended
 * @host_grant: an explicit host vendor request supplied the additional budget
 * @configured: a configuration was selected in this bus session, even if suspended
 * @connected: actual host bus activity was observed, preserved across suspend
 * @suspended: the bus is suspended; an earlier host grant has been revoked
 */
struct usb_gadget_current_budget {
	const struct device_node *of_node;
	unsigned int current_ma;
	bool host_grant;
	bool configured;
	bool connected;
	bool suspended;
};
'''

PROTOTYPES = r'''
int usb_gadget_register_current_notifier(struct notifier_block *nb);
int usb_gadget_unregister_current_notifier(struct notifier_block *nb);
int usb_gadget_get_current_budget(const struct device_node *node,
				  struct usb_gadget_current_budget *budget);
int usb_gadget_handle_current_grant(struct usb_gadget *gadget,
				    const struct usb_ctrlrequest *ctrl);
void usb_gadget_revoke_current_grant(struct usb_gadget *gadget);
void usb_gadget_current_budget_suspend(struct usb_gadget *gadget, bool suspended);
'''

STUBS = r'''
static inline int usb_gadget_register_current_notifier(struct notifier_block *nb)
{ return -ENODEV; }
static inline int usb_gadget_unregister_current_notifier(struct notifier_block *nb)
{ return 0; }
static inline int usb_gadget_get_current_budget(const struct device_node *node,
						struct usb_gadget_current_budget *budget)
{ return -ENODEV; }
static inline int usb_gadget_handle_current_grant(struct usb_gadget *gadget,
						  const struct usb_ctrlrequest *ctrl)
{ return -EOPNOTSUPP; }
static inline void usb_gadget_revoke_current_grant(struct usb_gadget *gadget) { }
static inline void usb_gadget_current_budget_suspend(struct usb_gadget *gadget,
						     bool suspended) { }
'''

COMPLETE = r'''
static void composite_current_grant_complete(struct usb_ep *ep,
					     struct usb_request *req)
{
	struct usb_composite_dev *cdev = req->context;

	if (req->status && cdev)
		usb_gadget_revoke_current_grant(cdev->gadget);
	composite_setup_complete(ep, req);
}
'''


def replace(path, old, new):
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise RuntimeError(f'expected one match: {path}: {old[:70]}')
    path.write_text(text.replace(old, new, 1))


def main():
    if Path.cwd() != Path('/src/wt/periph'):
        raise RuntimeError('run only in /src/wt/periph')
    header = Path('include/linux/usb/gadget.h')
    replace(header, '/**\n * struct usb_gadget -', HEADER.lstrip('\n') + '\n/**\n * struct usb_gadget -')
    replace(header, ' * @mA: last set mA value\n',
            ' * @mA: last set mA value\n'
            ' * @current_base_ma: standard configuration current authorization\n'
            ' * @current_extra_ma: signed extra current from an opt-in host grant\n'
            ' * @current_host_grant: an explicit host grant is active\n'
            ' * @current_suspended: composite bus suspend revokes active authorization\n'
            ' * @current_configured: a configuration was selected in this bus session\n'
            ' * @current_bus_connected: a host reset/address/configuration or SETUP was seen\n'
            ' * @current_budget_enabled: board opted into USB current authorization\n')
    replace(header, '\tunsigned\t\t\tmA;\n',
            '\tunsigned\t\t\tmA;\n'
            '\t/* Opt-in current authorization, protected by state_lock. */\n'
            '\tunsigned int\t\t\tcurrent_base_ma;\n'
            '\ts16\t\t\t\tcurrent_extra_ma;\n'
            '\tbool\t\t\t\tcurrent_host_grant;\n'
            '\tbool\t\t\t\tcurrent_suspended;\n'
            '\tbool\t\t\t\tcurrent_configured;\n'
            '\tbool\t\t\t\tcurrent_bus_connected;\n'
            '\tbool\t\t\t\tcurrent_budget_enabled;\n')
    replace(header, '#if IS_ENABLED(CONFIG_USB_GADGET)\nint usb_gadget_frame_number',
            '#if IS_ENABLED(CONFIG_USB_GADGET)\n' + PROTOTYPES + '\nint usb_gadget_frame_number')
    replace(header, '#else\nstatic inline int usb_gadget_frame_number',
            '#else\n' + STUBS + '\nstatic inline int usb_gadget_frame_number')
    core = Path('drivers/usb/gadget/udc/core.c')
    replace(core, '#include <linux/workqueue.h>\n',
            '#include <linux/workqueue.h>\n#include <linux/notifier.h>\n#include <linux/property.h>\n')
    replace(core, 'static DEFINE_MUTEX(udc_lock);\n', 'static DEFINE_MUTEX(udc_lock);\n' + CORE)
    replace(core, 'out:\n\ttrace_usb_gadget_vbus_draw(gadget, ret);',
            'out:\n\t/* Authorization is independent of optional PHY power programming. */\n'
            '\tgadget_current_set_base(gadget, mA);\n'
            '\ttrace_usb_gadget_vbus_draw(gadget, ret);')
    replace(core, '\tgadget->state = state;\n',
            '\tgadget->state = state;\n'
            '\tif (state <= USB_STATE_DEFAULT) {\n'
            '\t\tgadget->current_host_grant = false;\n'
            '\t\tgadget->current_extra_ma = 0;\n'
            '\t\tgadget->current_suspended = false;\n'
            '\t\tgadget->current_configured = false;\n'
            '\t\tgadget->current_bus_connected = state == USB_STATE_DEFAULT;\n'
            '\t\tgadget->current_base_ma = state == USB_STATE_DEFAULT ? 100 : 0;\n'
            '\t} else if (state == USB_STATE_SUSPENDED) {\n'
            '\t\tgadget->current_host_grant = false;\n'
            '\t\tgadget->current_extra_ma = 0;\n'
            '\t\tgadget->current_suspended = true;\n'
            '\t} else if (state == USB_STATE_ADDRESS || state == USB_STATE_CONFIGURED) {\n'
            '\t\tgadget->current_bus_connected = true;\n'
            '\t\tif (state == USB_STATE_CONFIGURED)\n'
            '\t\t\tgadget->current_configured = true;\n'
            '\t\tgadget->current_suspended = false;\n'
            '\t}\n')
    replace(core, '\ttrace_usb_gadget_set_state(gadget, 0);\n',
            '\ttrace_usb_gadget_set_state(gadget, 0);\n\tgadget_current_notify(gadget);\n')
    replace(core, '\tgadget->dev.parent = parent;\n',
            '\tgadget->dev.parent = parent;\n'
            '\tgadget->current_base_ma = 0;\n'
            '\tgadget->current_extra_ma = 0;\n'
            '\tgadget->current_host_grant = false;\n'
            '\tgadget->current_suspended = false;\n'
            '\tgadget->current_configured = false;\n'
            '\tgadget->current_bus_connected = false;\n'
            '\tgadget->current_budget_enabled =\n'
            '\t\tdevice_property_read_bool(parent, "apple,host-current-budget");\n')
    replace(core, '\tdev_vdbg(gadget->dev.parent, "unregistering gadget\\n");\n',
            '\tdev_vdbg(gadget->dev.parent, "unregistering gadget\\n");\n'
            '\tgadget_current_set_base(gadget, 0);\n')
    composite = Path('drivers/usb/gadget/composite.c')
    replace(composite, '\nstatic int count_ext_compat(', COMPLETE + '\nstatic int count_ext_compat(')
    replace(composite, '\tif (w_length > USB_COMP_EP0_BUFSIZ) {\n',
            '\tvalue = usb_gadget_handle_current_grant(gadget, ctrl);\n'
            '\tif (value != -EOPNOTSUPP) {\n'
            '\t\treq->zero = 0;\n'
            '\t\treq->context = cdev;\n'
            '\t\treq->complete = composite_current_grant_complete;\n'
            '\t\treq->length = 0;\n'
            '\t\tgadget->ep0->driver_data = cdev;\n'
            '\t\tgoto check_value;\n\t}\n\n'
            '\tif (w_length > USB_COMP_EP0_BUFSIZ) {\n')
    replace(composite, '\tDBG(cdev, "suspend\\n");\n',
            '\tDBG(cdev, "suspend\\n");\n'
            '\tusb_gadget_current_budget_suspend(gadget, true);\n')
    replace(composite, '\tDBG(cdev, "resume\\n");\n',
            '\tDBG(cdev, "resume\\n");\n'
            '\tusb_gadget_current_budget_suspend(gadget, false);\n')
    replace(composite, '\t\t\tDBG(cdev, "ep_queue --> %d\\n", value);\n'
            '\t\t\treq->status = 0;\n'
            '\t\t\tcomposite_setup_complete(gadget->ep0, req);\n',
            '\t\t\tDBG(cdev, "ep_queue --> %d\\n", value);\n'
            '\t\t\tif (req->complete == composite_current_grant_complete)\n'
            '\t\t\t\tusb_gadget_revoke_current_grant(gadget);\n'
            '\t\t\treq->status = 0;\n'
            '\t\t\tcomposite_setup_complete(gadget->ep0, req);\n')


if __name__ == '__main__':
    main()
