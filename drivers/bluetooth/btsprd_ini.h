/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026
 *
 * Parser for the Unisoc Bluetooth pskey / RF-config INI files. Packs the
 * self-describing text INI into the binary blob the CP2 firmware expects at
 * HCI-init time (VSC 0xfca0 pskey / 0xfca2 RF). See btsprd_ini.c for the
 * packing rule.
 */
#ifndef __BTSPRD_INI_H__
#define __BTSPRD_INI_H__

#include <linux/types.h>

/*
 * Pack the NUL-terminated INI text @ini (length @ini_len, terminator not
 * required) into @out (capacity @out_max). Returns the number of bytes
 * emitted, or a negative errno. The emitted length is asserted to equal the
 * "[Total Length=N]" declared in the INI header, so a malformed or truncated
 * file fails loud instead of yielding a short VSC.
 */
int btsprd_ini_pack(const char *ini, size_t ini_len, u8 *out, size_t out_max);

#endif /* __BTSPRD_INI_H__ */
