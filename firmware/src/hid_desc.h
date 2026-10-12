/* hid_desc.h: HID report descriptors and report IDs. MIT licence. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Input report IDs and the number of bytes after the ID. */
#define RPT_MOUSE    0x40
#define RPT_KEYBOARD 0x41
#define RPT_STATE    0x42
#define RPT_43       0x43
#define RPT_44       0x44
#define RPT_COMBINED 0x45
#define RPT_STATUS   0x79
#define RPT_QUALITY  0x7B
/* Feature report IDs. */
#define RPT_FEATURE_PASSTHROUGH 0x01
#define RPT_FEATURE_COMMAND     0x02
#define FEATURE_BODY_LEN        63   /* bytes after the ID */

/* Bytes after the ID of an input report, 0 for an ID that does not exist. */
uint8_t hid_input_len(uint8_t id);

/* Descriptor of a slot interface (mouse + keyboard + vendor collections) and of the dock. */
const uint8_t *hid_desc_slot(size_t *len);
const uint8_t *hid_desc_dock(size_t *len);
