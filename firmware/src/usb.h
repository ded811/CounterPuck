/* usb.h: the USB device. Interfaces 0 to 3 are the radio slots, 4 is the dock. MIT licence. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NUM_SLOTS 4
#define DOCK_IFACE NUM_SLOTS
#define NUM_IFACES (NUM_SLOTS + 1)

int usb_start(void);

/* Send an input report (ID in report[0], len <= 64). Returns -EACCES, quietly, while the interface
 * is not configured or the bus is suspended. The call returns once the host has taken the report. */
int usb_submit_input(int iface, const uint8_t *report, uint16_t len);

/* Wake a suspended host (USB remote wakeup). */
void usb_remote_wakeup(void);
