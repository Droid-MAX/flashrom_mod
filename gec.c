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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "flashchips.h"
#include "fmap.h"
#include "gec_lpc_commands.h"
#include "programmer.h"
#include "spi.h"
#include "writeprotect.h"

/* 1 if we want the flashrom to call erase_and_write_flash() again. */
static int need_2nd_pass = 0;

/* 1 if we want the flashrom to try jumping to new firmware after update. */
static int try_latest_firmware = 0;

/* The range of each firmware copy from the image file to update.
 * But re-define the .flags as the valid flag to indicate the firmware is
 * new or not (if flags = 1).
 */
static struct fmap_area fwcopy[4];  // [0] is not used.

/* The names of enum lpc_current_image to match in FMAP area names. */
static const char *sections[4] = {
	"UNKNOWN SECTION",  // EC_LPC_IMAGE_UNKNOWN -- never matches
	"RO_SECTION",       // EC_LPC_IMAGE_RO
	"RW_SECTION_A",     // EC_LPC_IMAGE_RW_A
	"RW_SECTION_B",     // EC_LPC_IMAGE_RW_B
};


#ifdef SUPPORT_CHECKSUM
static verify_checksum(uint8_t* expected,
                       unsigned int addr,
                       unsigned int count) {
	int rc;
	struct lpc_params_flash_checksum csp;
	struct lpc_response_flash_checksum csr;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	uint8_t cs;
	int j;

	csp.offset = addr;
	csp.size = count;

	rc = priv->ec_command(EC_LPC_COMMAND_FLASH_CHECKSUM,
			      &csp, sizeof(csp), &csr, sizeof(csr));
	if (rc) {
		msg_perr("GEC: verify_checksum() error.\n");
		return rc;
	}

	for (cs = 0, j = 0; j < count; ++j) {
		BYTE_IN(cs, expected[j]);
	}
	if (cs != csr.checksum) {
		msg_pdbg("GEC: checksum dismatch at 0x%02x "
		         "(ec: 0x%02x, local: 0x%02x). Retry.\n",
		         addr, csr.checksum, cs);
		msg_pdbg("GEC: ");
		for (j = 0; j < count; ++j) {
			msg_pdbg("%02x-", expected[j]);
			if ((j & 15) == 15) msg_pdbg("\nGEC: ");
		}
		programmer_delay(1000);
		return 1;
	}
	return 0;
}
#endif  /* SUPPORT_CHECKSUM */


/* Given the range not able to update, mark the corresponding
 * firmware as old.
 */
static void gec_invalidate_copy(unsigned int addr, unsigned int len)
{
	int i;

	for (i = EC_LPC_IMAGE_RO; i < ARRAY_SIZE(fwcopy); i++) {
		struct fmap_area *fw = &fwcopy[i];
		if ((addr >= fw->offset && (addr < fw->offset + fw->size)) ||
		    (fw->offset >= addr && (fw->offset < addr + len))) {
			msg_pdbg("Mark firmware [%s] as old.\n",
			         sections[i]);
			fw->flags = 0;  // mark as old
		}
	}
}


/* Asks EC to jump to a firmware copy. If target is EC_LPC_IMAGE_UNKNOWN,
 * then this functions picks a NEW firmware copy and jumps to it. Note that
 * RO is preferred, then A, finally B.
 *
 * Returns 0 for success.
 */
static int gec_jump_copy(enum lpc_current_image target) {
	struct lpc_params_reboot_ec p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;

	memset(&p, 0, sizeof(p));
	p.target = target != EC_LPC_IMAGE_UNKNOWN ? target :
	           fwcopy[EC_LPC_IMAGE_RO].flags ? EC_LPC_IMAGE_RO :
	           fwcopy[EC_LPC_IMAGE_RW_A].flags ? EC_LPC_IMAGE_RW_A :
	           fwcopy[EC_LPC_IMAGE_RW_B].flags ? EC_LPC_IMAGE_RW_B :
	           EC_LPC_IMAGE_UNKNOWN;
	msg_pdbg("GEC is jumping to [%s]\n", sections[p.target]);
	if (p.target == EC_LPC_IMAGE_UNKNOWN) return 1;

	rc = priv->ec_command(EC_LPC_COMMAND_REBOOT_EC,
			      &p, sizeof(p), NULL, 0);
	if (rc) {
		msg_perr("GEC cannot jump to [%s]\n", sections[p.target]);
	} else {
		msg_pdbg("GEC has jumped to [%s]\n", sections[p.target]);
	}

	/* Sleep 1 sec to wait the EC re-init. */
	usleep(1000000);

	return rc;
}


/* Given an image, this function parses FMAP and recognize the firmware
 * ranges.
 */
int gec_prepare(uint8_t *image, int size) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	struct fmap *fmap;
	int i, j;

	if (!(priv && priv->detected)) return 0;

	// Parse the fmap in the image file and cache the firmware ranges.
	fmap = fmap_find_in_memory(image, size);
	if (!fmap) return 0;

	// Lookup RO/A/B sections in FMAP.
	for (i = 0; i < fmap->nareas; i++) {
		struct fmap_area *fa = &fmap->areas[i];
		for (j = EC_LPC_IMAGE_RO; j < ARRAY_SIZE(sections); j++) {
			if (!strcmp(sections[j], (const char *)fa->name)) {
				msg_pdbg("Found '%s' in image.\n", fa->name);
				memcpy(&fwcopy[j], fa, sizeof(*fa));
				fwcopy[j].flags = 1;  // mark as new
			}
		}
	}

	/* Warning: before update, we jump the EC to RO copy. If you want to
	 *          change this behavior, please also check the gec_finish().
	 */
	return gec_jump_copy(EC_LPC_IMAGE_RO);
}


/* Returns >0 if we need 2nd pass of erase_and_write_flash().
 *         <0 if we cannot jump to any firmware copy.
 *        ==0 if no more pass is needed.
 *
 * This function also jumps to new-updated firmware copy before return >0.
 */
int gec_need_2nd_pass(void) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;

	if (!(priv && priv->detected)) return 0;

	if (need_2nd_pass) {
		if (gec_jump_copy(EC_LPC_IMAGE_UNKNOWN)) {
			return -1;
		}
	}

	return need_2nd_pass;
}


/* Returns 0 for success.
 *
 * Try latest firmware: B > A > RO
 *
 * This function assumes the EC jumps to RO at gec_prepare() so that
 * the fwcopy[RO].flags is old (0) and A/B are new. Please also refine
 * this code logic if you change the gec_prepare() behavior.
 */
int gec_finish(void) {
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;

	if (!(priv && priv->detected)) return 0;

	if (try_latest_firmware) {
		if (fwcopy[EC_LPC_IMAGE_RW_B].flags &&
		    gec_jump_copy(EC_LPC_IMAGE_RW_B) == 0) return 0;
		if (fwcopy[EC_LPC_IMAGE_RW_A].flags &&
		    gec_jump_copy(EC_LPC_IMAGE_RW_A) == 0) return 0;
		return gec_jump_copy(EC_LPC_IMAGE_RO);
	}

	return 0;
}


int gec_read(struct flashchip *flash, uint8_t *readarr,
             unsigned int blockaddr, unsigned int readcnt) {
	int i;
	int rc = 0;
	struct lpc_params_flash_read p;
	struct lpc_response_flash_read r;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int maxlen = opaque_programmer->max_data_read;

	for (i = 0; i < readcnt; i += maxlen) {
		p.offset = blockaddr + i;
		p.size = min(readcnt - i, maxlen);
		rc = priv->ec_command(EC_LPC_COMMAND_FLASH_READ,
				      &p, sizeof(p), &r, sizeof(r));
		if (rc) {
			msg_perr("GEC: Flash read error at offset 0x%x\n",
			         blockaddr + i);
			return rc;
		}

#ifdef SUPPORT_CHECKSUM
		if (verify_checksum(r.data, blockaddr + i,
			            min(readcnt - i, sizeof(r.data)))) {
			msg_pdbg("GEC: re-read...\n");
			i -= sizeof(r.data);
			continue;
		}
#endif
		memcpy(readarr + i, r.data, p.size);
	}

	return rc;
}


int gec_block_erase(struct flashchip *flash,
                           unsigned int blockaddr,
                           unsigned int len) {
	struct lpc_params_flash_erase erase;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;
#ifdef SUPPORT_CHECKSUM
	uint8_t *blank;
#endif

#ifdef SUPPORT_CHECKSUM
re_erase:
#endif
	erase.offset = blockaddr;
	erase.size = len;
	rc = priv->ec_command(EC_LPC_COMMAND_FLASH_ERASE,
			      &erase, sizeof(erase), NULL, 0);
	if (rc == EC_LPC_RESULT_ACCESS_DENIED) {
		// this is active image.
		gec_invalidate_copy(blockaddr, len);
		need_2nd_pass = 1;
		return ACCESS_DENIED;
	}
	if (rc) {
		msg_perr("GEC: Flash erase error at address 0x%x, rc=%d\n",
		         blockaddr, rc);
		return rc;
	}

#ifdef SUPPORT_CHECKSUM
	blank = malloc(len);
	memset(blank, 0xff, len);
	if (verify_checksum(blank, blockaddr, len)) {
		msg_pdbg("GEC: Re-erase...\n");
		goto re_erase;
	}
#endif

	try_latest_firmware = 1;
	return rc;
}


int gec_write(struct flashchip *flash, uint8_t *buf, unsigned int addr,
                    unsigned int nbytes) {
	int i, rc = 0;
	unsigned int written = 0;
	struct lpc_params_flash_write p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int maxlen = opaque_programmer->max_data_write;

	for (i = 0; i < nbytes; i += written) {
		written = min(nbytes - i, maxlen);
		p.offset = addr + i;
		p.size = written;
		memcpy(p.data, &buf[i], written);
		rc = priv->ec_command(EC_LPC_COMMAND_FLASH_WRITE,
				      &p, sizeof(p), NULL, 0);
		if (rc == EC_LPC_RESULT_ACCESS_DENIED) {
			// this is active image.
			gec_invalidate_copy(addr, nbytes);
			need_2nd_pass = 1;
			return ACCESS_DENIED;
		}

#ifdef SUPPORT_CHECKSUM
		if (verify_checksum(&buf[i], addr + i, written)) {
			msg_pdbg("GEC: re-write...\n");
			i -= written;
			continue;
		}
#endif

		if (rc) break;
	}

	try_latest_firmware = 1;
	return rc;
}


static int gec_list_ranges(const struct flashchip *flash) {
	msg_pinfo("You can specify any range:\n");
	msg_pinfo("  from: 0x%06x, to: 0x%06x\n", 0, flash->total_size * 1024);
	msg_pinfo("  unit: 0x%06x (%dKB)\n", 2048, 2048);
	return 0;
}


static int gec_set_range(const struct flashchip *flash,
                         unsigned int start, unsigned int len) {
	struct lpc_params_flash_wp_range p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;

	p.offset = start;
	p.size = len;
	rc = priv->ec_command(EC_LPC_COMMAND_FLASH_WP_SET_RANGE,
			      &p, sizeof(p), NULL, 0);
	if (rc) {
		msg_perr("GEC: wp_set_range error: rc=%d\n", rc);
		return rc;
	}

	return 0;
}


static int gec_enable_writeprotect(const struct flashchip *flash) {
	struct lpc_params_flash_wp_enable p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;

	p.enable_wp = 1;
	rc = priv->ec_command(EC_LPC_COMMAND_FLASH_WP_ENABLE,
			      &p, sizeof(p), NULL, 0);
	if (rc) {
		msg_perr("GEC: wp_enable_wp error: rc=%d\n", rc);
	}

	return rc;
}


static int gec_disable_writeprotect(const struct flashchip *flash) {
	struct lpc_params_flash_wp_enable p;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	int rc;

	p.enable_wp = 0;
	rc = priv->ec_command(EC_LPC_COMMAND_FLASH_WP_ENABLE,
			      &p, sizeof(p), NULL, 0);
	if (rc) {
		msg_perr("GEC: wp_disable_wp error: rc=%d\n", rc);
	} else {
		msg_pinfo("Disabled WP. Reboot EC and de-assert #WP.\n");
	}

	return rc;
}


static int gec_wp_status(const struct flashchip *flash) {
	int rc;
	struct lpc_response_flash_wp_range range;
	struct lpc_response_flash_wp_enable en;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	uint8_t value;

	rc = priv->ec_command(EC_LPC_COMMAND_FLASH_WP_GET_RANGE,
			      NULL, 0, &range, sizeof(range));
	if (rc) {
		msg_perr("GEC: wp_get_wp_range error: rc=%d\n", rc);
		return rc;
	}
	rc = priv->ec_command(EC_LPC_COMMAND_FLASH_WP_GET_STATE,
			      NULL, 0, &en, sizeof(en));
	if (rc) {
		msg_perr("GEC: wp_get_wp_state error: rc=%d\n", rc);
		return rc;
	}

	/* TODO: Fix scripts which rely on SPI-specific terminology. */
	value = (en.enable_wp << 7);
	msg_pinfo("WP: status: 0x%02x\n", value);
	msg_pinfo("WP: status.srp0: %x\n", en.enable_wp);
	msg_pinfo("WP: write protect is %s.\n",
	          en.enable_wp ? "enabled" : "disabled");
	msg_pinfo("WP: write protect range: start=0x%08x, len=0x%08x\n",
	          range.offset, range.size);

	return 0;
}


int gec_probe_size(struct flashchip *flash) {
	int rc;
	struct lpc_response_flash_info info;
	struct gec_priv *priv = (struct gec_priv *)opaque_programmer->data;
	struct block_eraser *eraser;
	static struct wp wp = {
		.list_ranges    = gec_list_ranges,
		.set_range      = gec_set_range,
		.enable         = gec_enable_writeprotect,
		.disable        = gec_disable_writeprotect,
		.wp_status      = gec_wp_status,
	};

	rc = priv->ec_command(EC_LPC_COMMAND_FLASH_INFO,
			      NULL, 0, &info, sizeof(info));
	if (rc) return 0;

	flash->total_size = info.flash_size / 1024;
	flash->page_size = 64;
	flash->tested = TEST_OK_PREW;
	eraser = &flash->block_erasers[0];
	eraser->eraseblocks[0].size = info.erase_block_size;
	eraser->eraseblocks[0].count = info.flash_size /
	                               eraser->eraseblocks[0].size;
	flash->wp = &wp;

	return 1;
};
