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

FAST_MUTEX MecAccessMutex;

int wait_for_ec(int status_addr, int timeout_usec);

// Thanks @DHowett!

typedef enum _ec_xfer_direction { EC_MEC_WRITE, EC_MEC_READ } ec_xfer_direction;

enum cros_ec_lpc_mec_emi_access_mode {
	/* 8-bit access */
	MEC_EC_BYTE_ACCESS = 0x0,
	/* 16-bit access */
	MEC_EC_WORD_ACCESS = 0x1,
	/* 32-bit access */
	MEC_EC_LONG_ACCESS = 0x2,
	/*
	 * 32-bit access, read or write of MEC_EMI_EC_DATA_B3 causes the
	 * EC data register to be incremented.
	 */
	 MEC_EC_LONG_ACCESS_AUTOINCREMENT = 0x3,
};

/* EMI registers are relative to base */
#define MEC_EMI_HOST_TO_EC(MEC_EMI_BASE)	((MEC_EMI_BASE) + 0)
#define MEC_EMI_EC_TO_HOST(MEC_EMI_BASE)	((MEC_EMI_BASE) + 1)
#define MEC_EMI_EC_ADDRESS_B0(MEC_EMI_BASE)	((MEC_EMI_BASE) + 2)
#define MEC_EMI_EC_ADDRESS_B1(MEC_EMI_BASE)	((MEC_EMI_BASE) + 3)
#define MEC_EMI_EC_DATA_B0(MEC_EMI_BASE)	((MEC_EMI_BASE) + 4)
#define MEC_EMI_EC_DATA_B1(MEC_EMI_BASE)	((MEC_EMI_BASE) + 5)
#define MEC_EMI_EC_DATA_B2(MEC_EMI_BASE)	((MEC_EMI_BASE) + 6)
#define MEC_EMI_EC_DATA_B3(MEC_EMI_BASE)	((MEC_EMI_BASE) + 7)

UINT16 mec_emi_base = 0, mec_emi_end = 0;

static void ec_mec_emi_write_access(UINT16 address, enum cros_ec_lpc_mec_emi_access_mode access_type) {
	outw((address & 0xFFFC) | (UINT16)access_type, MEC_EMI_EC_ADDRESS_B0(mec_emi_base));
}

static int ec_mec_xfer(ec_xfer_direction direction, UINT16 address,
	UINT8* data, UINT16 size)
{
	if (mec_emi_base == 0 || mec_emi_end == 0)
		return 0;

	ExAcquireFastMutex(&MecAccessMutex);

	/*
	 * There's a cleverer way to do this, but it's somewhat less clear what's happening.
	 * I prefer clarity over cleverness. :)
	 */
	int pos = 0;
	UINT16 temp[2];
	if (address % 4 > 0) {
		ec_mec_emi_write_access(address, MEC_EC_BYTE_ACCESS);
		/* Unaligned start address */
		for (int i = address % 4; i < 4; ++i) {
			UINT8* storage = &data[pos++];
			if (direction == EC_MEC_WRITE)
				outb(*storage, MEC_EMI_EC_DATA_B0(mec_emi_base) + i);
			else if (direction == EC_MEC_READ)
				*storage = inb(MEC_EMI_EC_DATA_B0(mec_emi_base) + i);
		}
		address = (address + 4) & 0xFFFC;
	}

	if (size - pos >= 4) {
		ec_mec_emi_write_access(address, MEC_EC_LONG_ACCESS_AUTOINCREMENT);
		while (size - pos >= 4) {
			if (direction == EC_MEC_WRITE) {
				memcpy(temp, &data[pos], sizeof(temp));
				outw(temp[0], MEC_EMI_EC_DATA_B0(mec_emi_base));
				outw(temp[1], MEC_EMI_EC_DATA_B2(mec_emi_base));
			}
			else if (direction == EC_MEC_READ) {
				temp[0] = inw(MEC_EMI_EC_DATA_B0(mec_emi_base));
				temp[1] = inw(MEC_EMI_EC_DATA_B2(mec_emi_base));
				memcpy(&data[pos], temp, sizeof(temp));
			}

			pos += 4;
			address += 4;
		}
	}

	if (size - pos > 0) {
		ec_mec_emi_write_access(address, MEC_EC_BYTE_ACCESS);
		for (int i = 0; i < (size - pos); ++i) {
			UINT8* storage = &data[pos + i];
			if (direction == EC_MEC_WRITE)
				outb(*storage, MEC_EMI_EC_DATA_B0(mec_emi_base) + i);
			else if (direction == EC_MEC_READ)
				*storage = inb(MEC_EMI_EC_DATA_B0(mec_emi_base) + i);
		}
	}

	ExReleaseFastMutex(&MecAccessMutex);

	return 0;
}

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

int ec_lpc_read_bytes(unsigned int offset, unsigned int length, UINT8* dest);
int ec_lpc_write_bytes(unsigned int offset, unsigned int length, const UINT8* msg);

static int ec_mec_lpc_read_bytes(unsigned int offset, unsigned int length, UINT8* dest) {
	int in_range = cros_ec_lpc_mec_in_range(offset, length);
	if (!in_range) {
		return ec_lpc_read_bytes(offset, length, dest);
	}

	int sum = 0;
	unsigned int i;
	ec_mec_xfer(EC_MEC_READ, (UINT16)offset - EC_HOST_CMD_REGION0, dest, (UINT16)length);
	for (i = 0; i < length; ++i) {
		sum += dest[i];
	}

	/* Return checksum of all bytes read */
	return sum;
}

static int ec_mec_lpc_write_bytes(unsigned int offset, unsigned int length, const UINT8* msg) {
	int in_range = cros_ec_lpc_mec_in_range(offset, length);
	if (!in_range) {
		return ec_lpc_write_bytes(offset, length, msg);
	}

	int sum = 0;
	unsigned int i;
	ec_mec_xfer(EC_MEC_WRITE, (UINT16)offset - EC_HOST_CMD_REGION0, (UINT8 *)msg, (UINT16)length);
	for (i = 0; i < length; ++i) {
		sum += msg[i];
	}

	/* Return checksum of all bytes written */
	return sum;
}

NTSTATUS comm_init_lpc_mec(void)
{
	/* This function assumes some setup was done by comm_init_lpc. */
	
	ExInitializeFastMutex(&MecAccessMutex);

	mec_emi_base = EC_HOST_CMD_REGION0;
	mec_emi_end = EC_LPC_ADDR_MEMMAP + EC_MEMMAP_SIZE;

	ec_lpc_ops.read = ec_mec_lpc_read_bytes;
	ec_lpc_ops.write = ec_mec_lpc_write_bytes;

	return STATUS_SUCCESS;
}
