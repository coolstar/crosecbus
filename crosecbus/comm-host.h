#ifndef __COMM_HOST_H__
#define __COMM_HOST_H__

#include "ec_commands.h"

/* ec_command return value for non-success result from EC */
#define EECRESULT 1000

typedef struct lpc_driver_ops {
	UINT8(*read)(unsigned int offset, unsigned int length, UINT8* dest);
	UINT8(*write)(unsigned int offset, unsigned int length, UINT8* dest);
} lpc_driver_ops;

extern int ec_max_outsize, ec_max_insize;

extern lpc_driver_ops ec_lpc_ops;

extern int (*ec_command_proto)(int command, int version,
	const void* outdata, int outsize, /* to EC */
	void* indata, int insize);        /* from EC */

/**
 * Return the content of the EC information area mapped as "memory".
 * The offsets are defined by the EC_MEMMAP_ constants. Returns the number
 * of bytes read, or negative on error. Specifying bytes=0 will read a
 * string (always including the trailing '\0').
 */
extern int (*ec_readmem)(int offset, int bytes, void* dest);

#endif