/* usb.c: the USB device: a serial port for the shell and five HID interfaces. MIT licence. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/class/usbd_hid.h>
#include "usb.h"
#include "hid_desc.h"
#include "identity.h"
#include "commands.h"
#include "slot.h"
#include "puck_config.h"

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

USBD_DEVICE_DEFINE(puck_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), PUCK_USB_VID, PUCK_USB_PID);
USBD_DESC_LANG_DEFINE(lang_desc);
USBD_DESC_MANUFACTURER_DEFINE(mfr_desc, PUCK_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(product_desc, PUCK_USB_PRODUCT);

/* The serial number is made at start-up (it may come from the chip), so its text lives in a buffer
 * of ours instead of a string literal. */
static uint8_t serial_text[13];
static struct usbd_desc_node serial_desc = {
	.str = { .utype = USBD_DUT_STRING_SERIAL_NUMBER, .ascii7 = true },
	.ptr = serial_text,
	.bLength = USB_STRING_DESCRIPTOR_LENGTH("0000000000000"),
	.bDescriptorType = USB_DESC_STRING,
};

/* Bus powered, remote wakeup allowed (a controller connecting can wake a sleeping PC), 500 mA. */
USBD_CONFIGURATION_DEFINE(fs_config, USB_SCD_REMOTE_WAKEUP, 250, NULL);

static const struct usb_bos_capability_lpm bos_lpm = {
	.bLength = sizeof(struct usb_bos_capability_lpm),
	.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
	.bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
	.bmAttributes = 0,
};
USBD_DESC_BOS_DEFINE(bos_desc, sizeof(bos_lpm), &bos_lpm);

static const struct device *const hid_dev[NUM_IFACES] = {
	DEVICE_DT_GET(DT_NODELABEL(hid_0)), DEVICE_DT_GET(DT_NODELABEL(hid_1)),
	DEVICE_DT_GET(DT_NODELABEL(hid_2)), DEVICE_DT_GET(DT_NODELABEL(hid_3)),
	DEVICE_DT_GET(DT_NODELABEL(hid_4)),
};

static bool iface_ready[NUM_IFACES];
static bool host_suspended_last[NUM_IFACES] = { true, true, true, true, true };
static bool bus_suspended;
static struct k_mutex submit_lock[NUM_IFACES];
/* Up to REPORT_RING input reports per interface can be on their way to the host at once (the class
 * sends straight from our buffers). The semaphore counts the free ones and is given as the host takes
 * each report, in the order they were queued. */
#define REPORT_RING 8
static struct k_sem report_free[NUM_IFACES];
static uint8_t report_ring[NUM_IFACES][REPORT_RING][64] __aligned(4);
static uint8_t report_head[NUM_IFACES];

static int iface_of(const struct device *dev)
{
	for (int i = 0; i < NUM_IFACES; i++) {
		if (hid_dev[i] == dev) {
			return i;
		}
	}
	return -1;
}

/* A slot treats the host as suspended while its interface is not configured or the bus sleeps. */
static void refresh_host_state(int i)
{
	bool suspended = !iface_ready[i] || bus_suspended;

	if (i < NUM_SLOTS && suspended != host_suspended_last[i]) {
		host_suspended_last[i] = suspended;
		slot_host_state(i, suspended);
	}
}

static void on_iface_ready(const struct device *dev, const bool ready)
{
	int i = iface_of(dev);

	if (i >= 0) {
		iface_ready[i] = ready;
		if (ready) {
			/* A report that was in flight when the bus went quiet may never be reported as
			 * done: start over with the buffer free. */
			k_sem_reset(&report_free[i]);
			for (int n = 0; n < REPORT_RING; n++) {
				k_sem_give(&report_free[i]);
			}
			report_head[i] = 0;
		}
		refresh_host_state(i);
	}
}

static void on_input_report_done(const struct device *dev)
{
	int i = iface_of(dev);

	if (i >= 0) {
		k_sem_give(&report_free[i]);
	}
}

static void on_usb_message(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	switch (msg->type) {
	case USBD_MSG_SUSPEND:
		bus_suspended = true;
		break;
	case USBD_MSG_RESUME:
		bus_suspended = false;
		break;
	default:
		return;
	}
	for (int i = 0; i < NUM_IFACES; i++) {
		refresh_host_state(i);
	}
}

/* Feature reports: ID 1 is passed through to the controller, ID 2 is the vendor command channel.
 * Both are 64 bytes on the wire: the ID, then 63 bytes. */
static int on_set_report(const struct device *dev, const uint8_t type, const uint8_t id,
			 const uint16_t len, const uint8_t *const buf)
{
	int i = iface_of(dev);

	if (i < 0 || type != HID_REPORT_TYPE_FEATURE) {
		return -ENOTSUP;
	}
	if (len != FEATURE_BODY_LEN + 1) {
		return -EINVAL;
	}
	if (id == RPT_FEATURE_COMMAND) {
		return commands_set(i, buf + 1);
	}
	if (id == RPT_FEATURE_PASSTHROUGH) {
		return i < NUM_SLOTS ? slot_feature_set(i, buf + 1) : -ENODEV;
	}
	return -ENOTSUP;
}

static int on_get_report(const struct device *dev, const uint8_t type, const uint8_t id,
			 const uint16_t len, uint8_t *const buf)
{
	int i = iface_of(dev);
	int err;

	if (i < 0 || type != HID_REPORT_TYPE_FEATURE) {
		return -ENOTSUP;
	}
	if (len < FEATURE_BODY_LEN + 1) {
		return -EINVAL;
	}
	if (id == RPT_FEATURE_COMMAND) {
		err = commands_get(i, buf + 1);
	} else if (id == RPT_FEATURE_PASSTHROUGH) {
		err = i < NUM_SLOTS ? slot_feature_get(i, buf + 1) : -ENODEV;
	} else {
		return -ENOTSUP;
	}
	if (err < 0) {
		return err;
	}
	buf[0] = id;
	return FEATURE_BODY_LEN + 1;
}

static void on_output_report(const struct device *dev, const uint16_t len, const uint8_t *const buf)
{
	int i = iface_of(dev);

	/* The dock has no use for output reports: they are accepted and dropped. */
	if (i >= 0 && i < NUM_SLOTS) {
		(void)slot_output_report(i, buf, len);
	}
}

/* Reports are sent without ever waiting for the host (input_report_done tells when one is gone). A slot
 * thread must never sit waiting for a PC that is slow or has stopped listening, for example one that
 * is asleep: the controller would lose its link. */
static const struct hid_device_ops hid_ops = {
	.iface_ready = on_iface_ready,
	.get_report = on_get_report,
	.set_report = on_set_report,
	.output_report = on_output_report,
	.input_report_done = on_input_report_done,
};

int usb_submit_input(int iface, const uint8_t *report, uint16_t len)
{
	/* The class needs a word-aligned buffer; copy so callers may pass anything. */
	int err;

	if (iface < 0 || iface >= NUM_IFACES || len == 0 || len > sizeof(report_ring[0][0])) {
		return -EINVAL;
	}
	k_mutex_lock(&submit_lock[iface], K_FOREVER);
	if (!iface_ready[iface] || bus_suspended) {
		err = -EACCES;
	} else if (k_sem_take(&report_free[iface], K_NO_WAIT) != 0) {
		err = -EBUSY;        /* all buffers are still waiting for the host: drop this report */
	} else {
		uint8_t *slot = report_ring[iface][report_head[iface]];

		memcpy(slot, report, len);
		err = hid_device_submit_report(hid_dev[iface], len, slot);
		if (err) {
			k_sem_give(&report_free[iface]);
		} else {
			report_head[iface] = (report_head[iface] + 1) % REPORT_RING;
		}
	}
	k_mutex_unlock(&submit_lock[iface]);
	return err;
}

void usb_remote_wakeup(void)
{
	int err = usbd_wakeup_request(&puck_usbd);

	if (err) {
		LOG_WRN("Remote wakeup refused (%d), the host may be awake", err);
	} else {
		LOG_INF("Remote wakeup requested");
	}
}

int usb_start(void)
{
	size_t len;
	const uint8_t *desc;
	int err;

	memcpy(serial_text, identity_serial(), sizeof(serial_text));
	for (int i = 0; i < NUM_IFACES; i++) {
		k_mutex_init(&submit_lock[i]);
		k_sem_init(&report_free[i], REPORT_RING, REPORT_RING);
		desc = i < NUM_SLOTS ? hid_desc_slot(&len) : hid_desc_dock(&len);
		err = hid_device_register(hid_dev[i], desc, len, &hid_ops);
		if (err) {
			LOG_ERR("Registering HID interface %d failed (%d)", i, err);
			return err;
		}
	}

	if ((err = usbd_add_descriptor(&puck_usbd, &lang_desc)) ||
	    (err = usbd_add_descriptor(&puck_usbd, &mfr_desc)) ||
	    (err = usbd_add_descriptor(&puck_usbd, &product_desc)) ||
	    (err = usbd_add_descriptor(&puck_usbd, &serial_desc))) {
		LOG_ERR("Adding string descriptors failed (%d)", err);
		return err;
	}
	if ((err = usbd_add_configuration(&puck_usbd, USBD_SPEED_FS, &fs_config)) ||
	    (err = usbd_register_all_classes(&puck_usbd, USBD_SPEED_FS, 1))) {
		LOG_ERR("Configuring the USB device failed (%d)", err);
		return err;
	}
	/* Interface association descriptors, USB 2.01 with a BOS descriptor, device release 2. */
	usbd_device_set_code_triple(&puck_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	usbd_device_set_bcd_usb(&puck_usbd, USBD_SPEED_FS, 0x0201);
	usbd_device_set_bcd_device(&puck_usbd, 0x0002);
	if ((err = usbd_add_descriptor(&puck_usbd, &bos_desc)) ||
	    (err = usbd_msg_register_cb(&puck_usbd, on_usb_message)) ||
	    (err = usbd_init(&puck_usbd))) {
		LOG_ERR("Starting the USB device failed (%d)", err);
		return err;
	}
	err = usbd_enable(&puck_usbd);
	if (err) {
		LOG_ERR("Enabling the USB device failed (%d)", err);
	}
	return err;
}
