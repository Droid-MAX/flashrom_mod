/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2012 The Chromium OS Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of Google or the names of contributors or
 * licensors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * GOOGLE INC AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * GOOGLE OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF GOOGLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 *
 * gec_lpc_commands.h: LPC command constants for Chrome EC
 */


#ifndef __CROS_EC_LPC_COMMANDS_H
#define __CROS_EC_LPC_COMMANDS_H

#include <stdint.h>


/* I/O addresses for LPC commands */
#define EC_LPC_ADDR_KERNEL_DATA   0x62
#define EC_LPC_ADDR_KERNEL_CMD    0x66
#define EC_LPC_ADDR_KERNEL_PARAM 0x800
#define EC_LPC_ADDR_USER_DATA    0x200
#define EC_LPC_ADDR_USER_CMD     0x204
#define EC_LPC_ADDR_USER_PARAM   0x880
#define EC_LPC_PARAM_SIZE          128  /* Size of param areas in bytes */

/* LPC command status byte masks */
/* EC has written a byte in the data register and host hasn't read it yet */
#define EC_LPC_STATUS_TO_HOST     0x01
/* Host has written a command/data byte and the EC hasn't read it yet */
#define EC_LPC_STATUS_FROM_HOST   0x02
/* EC is processing a command */
#define EC_LPC_STATUS_PROCESSING  0x04
/* Last write to EC was a command, not data */
#define EC_LPC_STATUS_LAST_CMD    0x08
/* EC is in burst mode.  Chrome EC doesn't support this, so this bit is never
 * set. */
#define EC_LPC_STATUS_BURST_MODE  0x10
/* SCI event is pending (requesting SCI query) */
#define EC_LPC_STATUS_SCI_PENDING 0x20
/* SMI event is pending (requesting SMI query) */
#define EC_LPC_STATUS_SMI_PENDING 0x40
/* (reserved) */
#define EC_LPC_STATUS_RESERVED    0x80

/* EC is busy.  This covers both the EC processing a command, and the host has
 * written a new command but the EC hasn't picked it up yet. */
#define EC_LPC_STATUS_BUSY_MASK \
      (EC_LPC_STATUS_FROM_HOST | EC_LPC_STATUS_PROCESSING)

/* LPC command response codes */
enum lpc_status {
	EC_LPC_RESULT_SUCCESS = 0,
	EC_LPC_RESULT_INVALID_COMMAND = 1,
	EC_LPC_RESULT_ERROR = 2,
	EC_LPC_RESULT_INVALID_PARAM = 3,
	EC_LPC_RESULT_ACCESS_DENIED = 4,
};


/* Notes on commands:
 *
 * Each command is an 8-byte command value.  Commands which take
 * params or return response data specify structs for that data.  If
 * no struct is specified, the command does not input or output data,
 * respectively. */

/* Reboot.  This command will work even when the EC LPC interface is
 * busy, because the reboot command is processed at interrupt
 * level.  Note that when the EC reboots, the host will reboot too, so
 * there is no response to this command. */
#define EC_LPC_COMMAND_REBOOT 0xD1  /* Think "die" */


/* Hello.  This is a simple command to test the EC is responsive to
 * commands. */
#define EC_LPC_COMMAND_HELLO 0x01
struct lpc_params_hello {
	uint32_t in_data;  /* Pass anything here */
} __attribute__ ((packed));
struct lpc_response_hello {
	uint32_t out_data;  /* Output will be in_data + 0x01020304 */
} __attribute__ ((packed));


/* Get version number */
#define EC_LPC_COMMAND_GET_VERSION 0x02
enum lpc_current_image {
	EC_LPC_IMAGE_UNKNOWN = 0,
	EC_LPC_IMAGE_RO,
	EC_LPC_IMAGE_RW,
};
struct lpc_response_get_version {
	/* Null-terminated version strings for RO, RW-A, RW-B */
	char version_string_ro[32];
	char version_string_rw_a[32];
	char version_string_rw_b[32];
	uint32_t current_image;  /* One of lpc_current_image */
} __attribute__ ((packed));


/* Read test */
#define EC_LPC_COMMAND_READ_TEST 0x03
struct lpc_params_read_test {
	uint32_t offset;   /* Starting value for read buffer */
	uint32_t size;     /* Size to read in bytes */
} __attribute__ ((packed));
struct lpc_response_read_test {
	uint32_t data[32];
} __attribute__ ((packed));

/*****************************************************************************/
/* Flash commands */

/* Maximum bytes that can be read/written in a single command */
#define EC_LPC_FLASH_SIZE_MAX 64

/* Get flash info */
#define EC_LPC_COMMAND_FLASH_INFO 0x10
struct lpc_response_flash_info {
	/* Usable flash size, in bytes */
	uint32_t flash_size;
	/* Write block size.  Write offset and size must be a multiple
	 * of this. */
	uint32_t write_block_size;
	/* Erase block size.  Erase offset and size must be a multiple
	 * of this. */
	uint32_t erase_block_size;
	/* Protection block size.  Protection offset and size must be a
	 * multiple of this. */
	uint32_t protect_block_size;
} __attribute__ ((packed));


/* Read flash */
#define EC_LPC_COMMAND_FLASH_READ 0x11
struct lpc_params_flash_read {
	uint32_t offset;   /* Byte offset to read */
	uint32_t size;     /* Size to read in bytes */
} __attribute__ ((packed));
struct lpc_response_flash_read {
	uint8_t data[EC_LPC_PARAM_SIZE];
} __attribute__ ((packed));


/* Write flash */
#define EC_LPC_COMMAND_FLASH_WRITE 0x12
struct lpc_params_flash_write {
	uint32_t offset;   /* Byte offset to erase */
	uint32_t size;     /* Size to erase in bytes */
	uint8_t data[64];
} __attribute__ ((packed));


/* Erase flash */
#define EC_LPC_COMMAND_FLASH_ERASE 0x13
struct lpc_params_flash_erase {
	uint32_t offset;   /* Byte offset to erase */
	uint32_t size;     /* Size to erase in bytes */
} __attribute__ ((packed));

#define EC_LPC_COMMAND_REBOOT_EC 0xd2
#define EC_LPC_COMMAND_REBOOT_BIT_RECOVERY (1 << 0)
struct lpc_params_reboot_ec {
	uint8_t target;  /* enum lpc_current_image */
	uint8_t reboot_flags;
} __attribute__ ((packed));



#endif  /* __CROS_EC_LPC_COMMANDS_H */
