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

static UINT8 ec_checksum_buffer(char* data, size_t size)
{
	UINT8 sum = 0;
	for (int i = 0; i < size; ++i) {
		sum += data[i];
	}
	return sum;
};

static int ec_command_lpc_mec_3(int command, int version, const void* outdata,
	int outsize, void* indata, int insize)
{
	
	struct ec_host_request rq;
	struct ec_host_response rs;
	const UINT8* d;
	UINT8* dout;
	int csum = 0;
	int i;

	/* Fail if output size is too big */
	if (outsize + sizeof(rq) > EC_LPC_HOST_PACKET_SIZE)
		return -EC_RES_REQUEST_TRUNCATED;

	/* Fill in request packet */
	/* TODO(crosbug.com/p/23825): This should be common to all protocols */
	rq.struct_version = EC_HOST_REQUEST_VERSION;
	rq.checksum = 0;
	rq.command = command;
	rq.command_version = version;
	rq.reserved = 0;
	rq.data_len = outsize;

	/* Copy data and update checksum */
	ec_mec_xfer(EC_MEC_WRITE, sizeof(rq), outdata, outsize);
	for (i = 0, d = (const UINT8*)outdata; i < outsize; i++, d++) {
		csum += *d;
	}

	/* Finish checksum */
	for (i = 0, d = (const UINT8*)&rq; i < sizeof(rq); i++, d++)
		csum += *d;

	/* Write checksum field so the entire packet sums to 0 */
	rq.checksum = (UINT8)(-csum);

	/* Copy header */
	ec_mec_xfer(EC_MEC_WRITE, 0, &rq, sizeof(rq));

	/* Start the command */
	outb(EC_COMMAND_PROTOCOL_3, EC_LPC_ADDR_HOST_CMD);

	if (wait_for_ec(EC_LPC_ADDR_HOST_CMD, 1000000)) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"Timeout waiting for EC response\n");
		return -EC_RES_ERROR;
	}

	/* Check result */
	i = inb(EC_LPC_ADDR_HOST_DATA);
	if (i) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC returned error result code %d\n", i);
		return -EECRESULT - i;
	}

	/* Read back response header and start checksum */
	ec_mec_xfer(EC_MEC_READ, 0, &rs, sizeof(rs));
	csum = 0;
	for (i = 0, dout = (UINT8*)&rs; i < sizeof(rs); i++, dout++) {
		csum += *dout;
	}

	if (rs.struct_version != EC_HOST_RESPONSE_VERSION) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC response version mismatch\n");
		return -EC_RES_INVALID_RESPONSE;
	}

	if (rs.reserved) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC response reserved != 0\n");
		return -EC_RES_INVALID_RESPONSE;
	}

	if (rs.data_len > insize) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC returned too much data\n");
		return -EC_RES_RESPONSE_TOO_BIG;
	}

	/* Read back data and update checksum */
	ec_mec_xfer(EC_MEC_READ, sizeof(rs), indata, rs.data_len);
	for (i = 0, dout = (UINT8*)indata; i < rs.data_len; i++, dout++) {
		csum += *dout;
	}

	/* Verify checksum */
	if ((UINT8)csum) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC response has invalid checksum\n");
		return -EC_RES_INVALID_CHECKSUM;
	}

	/* Return actual amount of data received */
	return rs.data_len;
}

static int ec_readmem_lpc_mec(int offset, int bytes, void* dest)
{
	int i = offset;
	int cnt = 0;
	char* s = dest;

	if (offset >= EC_MEMMAP_SIZE - bytes)
		return -1;

	if (bytes) {
		ec_mec_xfer(EC_MEC_READ, MEC_EC_MEMMAP_START + i, dest, bytes);
		cnt = bytes;
	}
	else {
		/* Somewhat brute-force to set up a bunch of
		 * individual transfers, but clearer than copying the xfer code
		 * to add a stop condition.
		 */
		for (; i < EC_MEMMAP_SIZE; ++i, ++s) {
			ec_mec_xfer(EC_MEC_READ, MEC_EC_MEMMAP_START + i, s, 1);
			cnt++;
			if (!*s)
				break;
		}
	}
	return cnt;
}

NTSTATUS comm_init_lpc_mec(void)
{
	char signature[2];

	/* This function assumes some setup was done by comm_init_lpc. */

	ec_readmem_lpc_mec(EC_MEMMAP_ID, 2, &signature[0]);
	if (signature[0] != 'E' || signature[1] != 'C') {
		return STATUS_INVALID_DEVICE_STATE;
	}

	ec_command_proto = ec_command_lpc_mec_3;
	ec_readmem = ec_readmem_lpc_mec;

	return STATUS_SUCCESS;
}
