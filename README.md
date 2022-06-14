Chrome EC Bus Driver

* NOTE: This driver does NOT expose services to userspace directly
* To use this driver, either ACPI must expose child devices, or you must modify this driver to enumerate child devices for other drivers to attach to

Known ACPI IDs:

	ACPI\GOOG0004: Chrome EC Bus (This driver)
	ACPI\GOOG0012: Chrome EC I2C Passthrough
	ACPI\GOOG0013: Chrome EC Audio Codec (Usually DMIC over I2S)

Protocols Implemented:
* LPC v2
* LPC v3 (Most Chromebooks)
* MEC LPC (Braswell/Skylake Chromebooks, Framework laptop)

Note: Framework laptop does not implement GOOG0004 ACPI device. Override DSDT/SSDT with testsigning or with OpenCore to add it. (See https://github.com/coreboot/coreboot/blob/master/src/ec/google/chromeec/acpi/cros_ec.asl for an example)

Tested on HP Chromebook 14b (Ryzen 3 3250C)