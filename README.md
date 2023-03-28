Chrome EC Bus Driver

* NOTE: This driver does NOT expose services to userspace directly
* To use this driver, either ACPI must expose child devices, or you must modify this driver to enumerate child devices for other drivers to attach to

Known ACPI IDs:
* ACPI\GOOG0004: Chrome EC Bus (This driver)
* ACPI\GOOG0002: Chrome EC Keyboard Backlight (Technically could be controlled via ACPI, but crosecbus is more reliable) - https://github.com/coolstar/croskblight
* ACPI\GOOG0003: Chrome EC PD Notify (used for USB-C)
* ACPI\GOOG0006: Chrome EC Sensor Hub - https://github.com/coolstar/crossensors
* ACPI\GOOG0007: Chrome EC Vivaldi Keyboard Settings - https://github.com/coolstar/crosecvivaldi
* ACPI\GOOG000A: Chrome EC Keyboard - https://github.com/coolstar/croskeyboard4
* ACPI\GOOG000B: Pixel Slate Base (also used for Sensor Hub) - https://github.com/coolstar/crossensors
* ACPI\GOOG0012: Chrome EC I2C Passthrough - https://github.com/coolstar/croseci2c
* ACPI\GOOG0013: Chrome EC Audio Codec (Usually DMIC over I2S) - https://github.com/coolstar/croseccodec
* ACPI\GOOG0014: Chrome EC USB-C
* ACPI\GOOG0015: Chrome EC Trackpoint
* ACPI\GOOG0016: Chrome OS GPIOs

IDs not covered by crosec:
* ACPI\GOOG000C: Wilco EC (not this driver)
* ACPI\GOOG000D: Wilco EC Event (unused in Windows)
* ACPI\GOOG000E: Wilco EC UCSI (covered by in-box UCSI driver)

Protocols Implemented:
* LPC v2
* LPC v3 (Most Chromebooks)
* MEC LPC (Braswell/Skylake Chromebooks, Framework laptop)

Note: Framework laptop does not implement GOOG0004 ACPI device. Override DSDT/SSDT with testsigning or with OpenCore to add it. (See https://github.com/coreboot/coreboot/blob/master/src/ec/google/chromeec/acpi/cros_ec.asl for an example)

Tested on HP Chromebook 14b (Ryzen 3 3250C)
