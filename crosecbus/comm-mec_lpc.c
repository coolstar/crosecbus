#include "driver.h"
#include "comm-host.h" 

static __inline void outb(unsigned char __val, unsigned int __port) {
	WRITE_PORT_UCHAR((PUCHAR)__port, __val);
}

static __inline void outw(unsigned short __val, unsigned int __port) {
	WRITE_PORT_USHORT((PUSHORT)__port, __val);
}

static __inline unsigned char inb(unsigned int __port) {
	return READ_PORT_UCHAR((PUCHAR)__port);
}

static __inline unsigned short inw(unsigned int __port) {
	return READ_PORT_USHORT((PUSHORT)__port);
}


static ULONG CrosEcBusDebugLevel = 100;
static ULONG CrosEcBusDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

int wait_for_ec(int status_addr, int timeout_usec);

// Thanks @DHowett!

typedef enum _ec_xfer_direction { EC_MEC_WRITE, EC_MEC_READ } ec_xfer_direction;

#define MEC_EC_BYTE_ACCESS               0x00
#define MEC_EC_LONG_ACCESS_AUTOINCREMENT 0x03

#define MEC_EC_ADDRESS_REGISTER0      0x0802
#define MEC_EC_ADDRESS_REGISTER1      0x0803
#define MEC_EC_DATA_REGISTER0         0x0804
#define MEC_EC_DATA_REGISTER2         0x0806
#define MEC_EC_MEMMAP_START            0x100

static int ec_mec_xfer(ec_xfer_direction direction, UINT16 address,
	char* data, UINT16 size)
{
	/*
	 * There's a cleverer way to do this, but it's somewhat less clear what's happening.
	 * I prefer clarity over cleverness. :)
	 */
	int pos = 0;
	UINT16 temp[2];
	if (address % 4 > 0) {
		outw((address & 0xFFFC) | MEC_EC_BYTE_ACCESS, MEC_EC_ADDRESS_REGISTER0);
		/* Unaligned start address */
		for (int i = address % 4; i < 4; ++i) {
			char* storage = &data[pos++];
			if (direction == EC_MEC_WRITE)
				outb(*storage, MEC_EC_DATA_REGISTER0 + i);
			else if (direction == EC_MEC_READ)
				*storage = inb(MEC_EC_DATA_REGISTER0 + i);
		}
		address = (address + 4) & 0xFFFC;
	}

	if (size - pos >= 4) {
		outw((address & 0xFFFC) | MEC_EC_LONG_ACCESS_AUTOINCREMENT, MEC_EC_ADDRESS_REGISTER0);
		while (size - pos >= 4) {
			if (direction == EC_MEC_WRITE) {
				memcpy(temp, &data[pos], sizeof(temp));
				outw(temp[0], MEC_EC_DATA_REGISTER0);
				outw(temp[1], MEC_EC_DATA_REGISTER2);
			}
			else if (direction == EC_MEC_READ) {
				temp[0] = inw(MEC_EC_DATA_REGISTER0);
				temp[1] = inw(MEC_EC_DATA_REGISTER2);
				memcpy(&data[pos], temp, sizeof(temp));
			}

			pos += 4;
			address += 4;
		}
	}

	if (size - pos > 0) {
		outw((address & 0xFFFC) | MEC_EC_BYTE_ACCESS, MEC_EC_ADDRESS_REGISTER0);
		for (int i = 0; i < (size - pos); ++i) {
			char* storage = &data[pos + i];
			if (direction == EC_MEC_WRITE)
				outb(*storage, MEC_EC_DATA_REGISTER0 + i);
			else if (direction == EC_MEC_READ)
				*storage = inb(MEC_EC_DATA_REGISTER0 + i);
		}
	}
	return 0;
}

UINT16 mec_emi_base = 0, mec_emi_end = 0;
static int cros_ec_lpc_mec_in_range(unsigned int offset, unsigned int length) {
	if (length == 0)
		return -1;

	if (mec_emi_base == 0 || mec_emi_end == 0)
		return -1;

	if (offset >= mec_emi_base && offset < mec_emi_end) {
		if (offset + length - 1 >= mec_emi_end)
			return -1;
		return 1;
	}

	if (offset + length > mec_emi_base && offset < mec_emi_end)
		return -1;

	return 0;
}

UINT8 ec_lpc_read_bytes(unsigned int offset, unsigned int length, UINT8* dest);
UINT8 ec_lpc_write_bytes(unsigned int offset, unsigned int length, UINT8* msg);

static UINT8 ec_mec_lpc_read_bytes(unsigned int offset, unsigned int length, UINT8* dest) {
	int in_range = cros_ec_lpc_mec_in_range(offset, length);
	if (!in_range) {
		return ec_lpc_read_bytes(offset, length, dest);
	}

	int sum = 0;
	int i;
	ec_mec_xfer(EC_MEC_READ, offset - EC_HOST_CMD_REGION0, dest, length);
	for (i = 0; i < length; ++i) {
		sum += dest[i];
	}

	/* Return checksum of all bytes read */
	return sum;
}

static UINT8 ec_mec_lpc_write_bytes(unsigned int offset, unsigned int length, UINT8* msg) {
	int in_range = cros_ec_lpc_mec_in_range(offset, length);
	if (!in_range) {
		return ec_lpc_write_bytes(offset, length, msg);
	}

	int sum = 0;
	int i;
	ec_mec_xfer(EC_MEC_WRITE, offset - EC_HOST_CMD_REGION0, msg, length);
	for (i = 0; i < length; ++i) {
		sum += msg[i];
	}

	/* Return checksum of all bytes written */
	return sum;
}

NTSTATUS comm_init_lpc_mec(void)
{
	/* This function assumes some setup was done by comm_init_lpc. */
	
	mec_emi_base = EC_HOST_CMD_REGION0;
	mec_emi_end = EC_LPC_ADDR_MEMMAP + EC_MEMMAP_SIZE;

	ec_lpc_ops.read = ec_mec_lpc_read_bytes;
	ec_lpc_ops.write = ec_mec_lpc_write_bytes;

	return STATUS_SUCCESS;
}
