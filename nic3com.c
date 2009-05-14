/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2009 Uwe Hermann <uwe@hermann-uwe.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include "flash.h"

#define BIOS_ROM_ADDR		0x04
#define BIOS_ROM_DATA		0x08
#define INT_STATUS		0x0e
#define SELECT_REG_WINDOW	0x800

#define PCI_IO_BASE_ADDRESS	0x10

#define PCI_VENDOR_ID_3COM	0x10b7

uint32_t io_base_addr;
struct pci_access *pacc;
struct pci_filter filter;

#if defined(__FreeBSD__) || defined(__DragonFly__)
int io_fd;
#endif

#define OK 0
#define NT 1	/* Not tested */

static struct nic_status {
	uint16_t device_id;
	int status;
	const char *device_name;
} nics[] = {
	/* 3C90xB */
	{0x9055, NT, "3C90xB: PCI 10/100 Mbps; shared 10BASE-T/100BASE-TX"},
	{0x9001, NT, "3C90xB: PCI 10/100 Mbps; shared 10BASE-T/100BASE-T4" },
	{0x9004, NT, "3C90xB: PCI 10BASE-T (TPO)" },
	{0x9005, NT, "3C90xB: PCI 10BASE-T/10BASE2/AUI (COMBO)" },
	{0x9006, NT, "3C90xB: PCI 10BASE-T/10BASE2 (TPC)" },
	{0x900a, NT, "3C90xB: PCI 10BASE-FL" },
	{0x905a, NT, "3C90xB: PCI 10BASE-FX" },

	/* 3C905C */
	{0x9200, OK, "3C905C: EtherLink 10/100 PCI (TX)" },

	/* 3C980C */
	{0x9805, NT, "3C980C: EtherLink Server 10/100 PCI (TX)" },

	{},
};

uint32_t nic3com_validate(struct pci_dev *dev)
{
	int i = 0;
	uint32_t addr = -1;

	for (i = 0; nics[i].device_name != NULL; i++) {
		if (dev->device_id != nics[i].device_id)
			continue;

		addr = pci_read_long(dev, PCI_IO_BASE_ADDRESS) & ~0x03;

		printf("Found NIC \"3COM %s\" (%04x:%04x), addr = 0x%x\n",
		       nics[i].device_name, PCI_VENDOR_ID_3COM,
		       nics[i].device_id, addr);

		if (nics[i].status == NT) {
			printf("===\nThis NIC is UNTESTED. Please email a "
			       "report including the 'flashrom -p nic3com'\n"
			       "output to flashrom@coreboot.org if it works "
			       "for you. Thank you for your help!\n===\n");
		}

		return addr;
	}

	return addr;
}

int nic3com_init(void)
{
	struct pci_dev *dev;
	char *msg = NULL;

#if defined (__sun) && (defined(__i386) || defined(__amd64))
	if (sysi86(SI86V86, V86SC_IOPL, PS_IOPL) != 0) {
#elif defined(__FreeBSD__) || defined (__DragonFly__)
	if ((io_fd = open("/dev/io", O_RDWR)) < 0) {
#else
	if (iopl(3) != 0) {
#endif
		fprintf(stderr, "ERROR: Could not get IO privileges (%s).\n"
			"You need to be root.\n", strerror(errno));
		exit(1);
	}

	pacc = pci_alloc();     /* Get the pci_access structure */
	pci_init(pacc);         /* Initialize the PCI library */
	pci_scan_bus(pacc);     /* We want to get the list of devices */

	if (nic_pcidev != NULL) {
		pci_filter_init(pacc, &filter);
		
		if ((msg = pci_filter_parse_slot(&filter, nic_pcidev))) {
			fprintf(stderr, "Error: %s\n", msg);
			exit(1);
		}
	}

	if (!filter.vendor && !filter.device) {
		pci_filter_init(pacc, &filter);
		filter.vendor = PCI_VENDOR_ID_3COM;
	}

	dev = pci_dev_find_filter(filter);

	if (dev && (dev->vendor_id == PCI_VENDOR_ID_3COM))
		io_base_addr = nic3com_validate(dev);
	else {
		fprintf(stderr, "Error: No supported 3COM NIC found.\n");
		exit(1);
	}

	/*
	 * The lowest 16 bytes of the I/O mapped register space of (most) 3COM
	 * cards form a 'register window' into one of multiple (usually 8)
	 * register banks. For 3C90xB/3C90xC we need register window/bank 0.
	 */
	OUTW(SELECT_REG_WINDOW + 0, io_base_addr + INT_STATUS);

	return 0;
}

int nic3com_shutdown(void)
{
	free(nic_pcidev);
	pci_cleanup(pacc);
	return 0;
}

void *nic3com_map(const char *descr, unsigned long phys_addr, size_t len)
{
	return 0;
}

void nic3com_unmap(void *virt_addr, size_t len)
{
}

void nic3com_chip_writeb(uint8_t val, volatile void *addr)
{
	OUTL((uint32_t)addr, io_base_addr + BIOS_ROM_ADDR);
	OUTB(val, io_base_addr + BIOS_ROM_DATA);
}

void nic3com_chip_writew(uint16_t val, volatile void *addr)
{
}

void nic3com_chip_writel(uint32_t val, volatile void *addr)
{
}

uint8_t nic3com_chip_readb(const volatile void *addr)
{
	uint8_t val;

	OUTL((uint32_t)addr, io_base_addr + BIOS_ROM_ADDR);
	val = INB(io_base_addr + BIOS_ROM_DATA);

	return val;
}

uint16_t nic3com_chip_readw(const volatile void *addr)
{
	return 0xffff;
}

uint32_t nic3com_chip_readl(const volatile void *addr)
{
	return 0xffffffff;
}
