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


#define INITIAL_UDELAY 5     /* 5 us */
#define MAXIMUM_UDELAY 10000 /* 10 ms */

static ULONG CrosEcBusDebugLevel = 100;
static ULONG CrosEcBusDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

UINT32 ec_max_outsize, ec_max_insize;

lpc_driver_ops ec_lpc_ops = {0};

int (*ec_command_proto)(UINT16 command, UINT8 version,
	const void* outdata, int outsize,
	void* indata, int insize);
int (*ec_readmem)(int offset, int bytes, void* dest);

/*
 * Wait for the EC to be unbusy.  Returns 0 if unbusy, non-zero if
 * timeout.
 */
int wait_for_ec(int status_addr, int timeout_usec)
{
	LARGE_INTEGER StartTime;
	KeQuerySystemTimePrecise(&StartTime);

	/*
	* Delay first, in case we just sent out a command but the EC
	* hasn't raised the busy flag.  However, I think this doesn't
	* happen since the LPC commands are executed in order and the
	* busy flag is set by hardware.  Minor issue in any case,
	* since the initial delay is very short.
	*/

	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 200;
	KeDelayExecutionThread(KernelMode, false, &WaitInterval);

	while (true) {
		LARGE_INTEGER CurrentTime;
		KeQuerySystemTimePrecise(&CurrentTime);

		if (CurrentTime.QuadPart > StartTime.QuadPart + 10 * (LONGLONG)timeout_usec)
			break;

		if (!(inb(status_addr) & EC_LPC_STATUS_BUSY_MASK))
			return 0;

		WaitInterval.QuadPart = -10 * 100;
		KeDelayExecutionThread(KernelMode, false, &WaitInterval);
	}
	return -1;  /* Timeout */
}

static int ec_command_lpc(UINT16 command, UINT8 version,
	const void* outdata, int outsize,
	void* indata, int insize)
{
	struct ec_lpc_host_args args;
	const UINT8* d;
	UINT8* dout;
	int csum;
	int i;

	/* Fill in args */
	args.flags = EC_HOST_ARGS_FLAG_FROM_HOST;
	args.command_version = version;
	args.data_size = (UINT8)outsize;

	/* Initialize checksum */
	csum = command + args.flags + args.command_version + args.data_size;

	/* Write data and update checksum */
	for (i = 0, d = (UINT8*)outdata; i < outsize; i++, d++) {
		outb(*d, EC_LPC_ADDR_HOST_PARAM + i);
		csum += *d;
	}

	/* Finalize checksum and write args */
	args.checksum = (UINT8)csum;
	for (i = 0, d = (const UINT8*)&args; i < sizeof(args); i++, d++)
		outb(*d, EC_LPC_ADDR_HOST_ARGS + i);

	outb((UINT8)command, EC_LPC_ADDR_HOST_CMD);

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

	/* Read back args */
	for (i = 0, dout = (UINT8*)&args; i < sizeof(args); i++, dout++)
		*dout = inb(EC_LPC_ADDR_HOST_ARGS + i);

	/*
	 * If EC didn't modify args flags, then somehow we sent a new-style
	 * command to an old EC, which means it would have read its params
	 * from the wrong place.
	 */
	if (!(args.flags & EC_HOST_ARGS_FLAG_TO_HOST)) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC protocol mismatch\n");
		return -EC_RES_INVALID_RESPONSE;
	}

	if (args.data_size > insize) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC returned too much data\n");
		return -EC_RES_INVALID_RESPONSE;
	}

	/* Start calculating response checksum */
	csum = command + args.flags + args.command_version + args.data_size;

	/* Read response and update checksum */
	for (i = 0, dout = (UINT8*)indata; i < args.data_size;
		i++, dout++) {
		*dout = inb(EC_LPC_ADDR_HOST_PARAM + i);
		csum += *dout;
	}

	/* Verify checksum */
	if (args.checksum != (UINT8)csum) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC response has invalid checksum\n");
		return -EC_RES_INVALID_CHECKSUM;
	}

	/* Return actual amount of data received */
	return args.data_size;
}

int ec_lpc_read_bytes(unsigned int offset, unsigned int length, UINT8* dest) {
	int sum = 0;
	unsigned int i;
	for (i = 0; i < length; ++i) {
		dest[i] = inb(offset + i);
		sum += dest[i];
	}

	/* Return checksum of all bytes read */
	return sum;
}

int ec_lpc_write_bytes(unsigned int offset, unsigned int length, const UINT8* msg) {
	int sum = 0;
	unsigned int i;
	for (i = 0; i < length; ++i) {
		outb(msg[i], offset + i);
		sum += msg[i];
	}

	/* Return checksum of all bytes written */
	return sum;
}

static int ec_command_lpc_3(UINT16 command, UINT8 version,
	const void* outdata, int outsize,
	void* indata, int insize)
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
	rq.data_len = (UINT16)outsize;

	/* Copy data and update checksum */
	ec_lpc_ops.write(EC_LPC_ADDR_HOST_PACKET + sizeof(rq), outsize, outdata);
	for (i = 0, d = (const UINT8*)outdata; i < outsize; i++, d++) {
		csum += *d;
	}

	/* Finish checksum */
	for (i = 0, d = (const UINT8*)&rq; i < sizeof(rq); i++, d++)
		csum += *d;

	/* Write checksum field so the entire packet sums to 0 */
	rq.checksum = (UINT8)(-csum);

	/* Copy header */
	ec_lpc_ops.write(EC_LPC_ADDR_HOST_PACKET, sizeof(rq), (const UINT8 *)&rq);

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
	ec_lpc_ops.read(EC_LPC_ADDR_HOST_PACKET, sizeof(rs), (UINT8*)&rs);
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
	ec_lpc_ops.read(EC_LPC_ADDR_HOST_PACKET + sizeof(rs), rs.data_len, indata);
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

static int ec_readmem_lpc(int offset, int bytes, void* dest)
{
	int i = offset;
	UINT8* s = (UINT8*)(dest);
	int cnt = 0;

	if (offset >= EC_MEMMAP_SIZE - bytes)
		return -1;

	if (bytes) {				/* fixed length */
		ec_lpc_ops.read(EC_LPC_ADDR_MEMMAP + i, bytes, dest);
		cnt = bytes;
	}
	else {				/* string */
		for (; i < EC_MEMMAP_SIZE; i++, s++) {
			ec_lpc_ops.read(EC_LPC_ADDR_MEMMAP + i, 1, s);
			cnt++;
			if (!*s)
				break;
		}
	}

	return cnt;
}

int comm_init_lpc_mec(void);

NTSTATUS comm_init_lpc(void)
{
	int i;
	int byte = 0xff;

	/*
	 * Test if the I/O port has been configured for Chromium EC LPC
	 * interface.  Chromium EC guarantees that at least one status bit will
	 * be 0, so if the command and data bytes are both 0xff, very likely
	 * that Chromium EC is not present.  See crosbug.com/p/10963.
	 */
	byte &= inb(EC_LPC_ADDR_HOST_CMD);
	byte &= inb(EC_LPC_ADDR_HOST_DATA);
	if (byte == 0xff) {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_INIT,
			"Port 0x%x,0x%x are both 0xFF.\n",
			EC_LPC_ADDR_HOST_CMD, EC_LPC_ADDR_HOST_DATA);
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_INIT,
			"Very likely this board doesn't have a Chromium EC.\n");
		return STATUS_CONNECTION_INVALID;
	}

	/* All EC's supports reading mapped memory directly. */
	ec_readmem = ec_readmem_lpc;

	UINT8 signature[2];

	/* Check for a MEC first. */
	if (comm_init_lpc_mec && NT_SUCCESS(comm_init_lpc_mec())) {
		ec_lpc_ops.read(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_ID, 2, signature);
		if (signature[0] == 'E' && signature[1] == 'C') {
			ec_max_outsize = EC_LPC_HOST_PACKET_SIZE -
				sizeof(struct ec_host_request);
			ec_max_insize = EC_LPC_HOST_PACKET_SIZE -
				sizeof(struct ec_host_response);

			//All MEC EC's are Protocol V3
			ec_command_proto = ec_command_lpc_3;

			DbgPrint("MEC EC\n");
			return STATUS_SUCCESS;
		}
	}

	/*
	 * Test if LPC command args are supported.
	 *
	 * The cheapest way to do this is by looking for the memory-mapped
	 * flag.  This is faster than sending a new-style 'hello' command and
	 * seeing whether the EC sets the EC_HOST_ARGS_FLAG_FROM_HOST flag
	 * in args when it responds.
	 */

	ec_lpc_ops.read = ec_lpc_read_bytes;
	ec_lpc_ops.write = ec_lpc_write_bytes;
	ec_lpc_ops.read(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_ID, 2, signature);
	if (signature[0] != 'E' || signature[1] != 'C') {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_INIT,
			"Missing Chromium EC memory map.\n");
		return STATUS_NO_MEMORY;
	}

	/* Check which command version we'll use */
	i = inb(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_HOST_CMD_FLAGS);

	if (i & EC_HOST_CMD_FLAG_VERSION_3) {
		/* Protocol version 3 */
		ec_command_proto = ec_command_lpc_3;
		ec_max_outsize = EC_LPC_HOST_PACKET_SIZE -
			sizeof(struct ec_host_request);
		ec_max_insize = EC_LPC_HOST_PACKET_SIZE -
			sizeof(struct ec_host_response);

		DbgPrint("Ver 3\n");

	}
	else if (i & EC_HOST_CMD_FLAG_LPC_ARGS_SUPPORTED) {
		/* Protocol version 2 */
		ec_command_proto = ec_command_lpc;
		ec_max_outsize = ec_max_insize = EC_PROTO2_MAX_PARAM_SIZE;

		DbgPrint("Ver 2\n");
	}
	else {
		CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_INIT,
			"EC doesn't support protocols we need.\n");
		return STATUS_INVALID_DEVICE_STATE;
	}
	return STATUS_SUCCESS;
}
