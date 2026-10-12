cmake_minimum_required(VERSION 3.20.0)

# The USB devicetree nodes (serial port + five HID interfaces) are the same on every board, so they
# live in one overlay that is added whatever the board.
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/usb.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(counterpuck LANGUAGES C)

target_include_directories(app PRIVATE src)
target_sources(app PRIVATE
	src/main.c
	src/identity.c
	src/usb.c
	src/hid_desc.c
	src/commands.c
	src/radio.c
	src/slot.c
	src/channels.c
	src/combiner.c
	src/bonds.c
	src/shell.c)
