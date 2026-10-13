/* Firmware images compiled into the kext; see scripts/fetch-firmware.sh. */
#ifndef QWZ_FW_H
#define QWZ_FW_H

#include <stddef.h>

struct qwz_fw_entry {
	const char		*name;
	const unsigned char	*data;
	size_t			 size;
};

extern const struct qwz_fw_entry qwz_fw_table[];
extern const int qwz_fw_count;

#endif
