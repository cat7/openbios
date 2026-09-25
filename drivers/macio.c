/*
 *   derived from mol/mol.c,
 *   Copyright (C) 2003, 2004 Samuel Rydh (samuel@ibrium.se)
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   version 2
 *
 */

#include "config.h"
#include "arch/common/nvram.h"
#include "packages/nvram.h"
#include "libopenbios/bindings.h"
#include "libc/byteorder.h"
#include "libc/vsprintf.h"
#include "libc/string.h"

#include "drivers/drivers.h"
#include "macio.h"
#include "cuda.h"
#include "pmu.h"
#include "escc.h"
#include "drivers/pci.h"

#define OW_IO_NVRAM_SIZE   0x00020000
#define OW_IO_NVRAM_OFFSET 0x00060000
#define OW_IO_NVRAM_SHIFT  4

#define NW_IO_NVRAM_SIZE   0x00004000
#define NVRAM_BANK_SIZE    0x2000
#define NW_IO_NVRAM_OFFSET 0xfff04000

#define IO_OPENPIC_SIZE    0x00040000
#define IO_OPENPIC_OFFSET  0x00040000

static char *nvram;

static int macio_nvram_shift(void)
{
	int nvram_flat;

        if (is_oldworld())
                return OW_IO_NVRAM_SHIFT;

	nvram_flat = fw_cfg_read_i32(FW_CFG_PPC_NVRAM_FLAT);
	return nvram_flat ? 0 : 1;
}

int
macio_get_nvram_size(void)
{
	int shift = macio_nvram_shift();
        /* one 8 KB bank of the U3's two */
        if (is_u3())
                return NVRAM_BANK_SIZE;
        if (is_oldworld())
                return OW_IO_NVRAM_SIZE >> shift;
        else
                return NW_IO_NVRAM_SIZE >> shift;
}

static unsigned long macio_nvram_offset(void)
{
	unsigned long r;

	/* Hypervisor tells us where NVRAM lies */
	r = fw_cfg_read_i32(FW_CFG_PPC_NVRAM_ADDR);
	if (r)
		return r;

	/* Fall back to hardcoded addresses */
	if (is_oldworld())
		return OW_IO_NVRAM_OFFSET;

	return NW_IO_NVRAM_OFFSET;
}

static unsigned long macio_nvram_size(void)
{
	if (is_oldworld())
		return OW_IO_NVRAM_SIZE;
	else
		return NW_IO_NVRAM_SIZE;
}

void macio_nvram_init(const char *path, phys_addr_t addr)
{
	phandle_t chosen, aliases;
	phandle_t dnode;
	int props[2];
	char buf[64];
        unsigned long nvram_size, nvram_offset;

        nvram_offset = macio_nvram_offset();
        nvram_size = macio_nvram_size();

	nvram = (char*)addr + nvram_offset;
	nvconf_init();
	snprintf(buf, sizeof(buf), "%s", path);
	dnode = nvram_init(buf);
	set_int_property(dnode, "#bytes", arch_nvram_size() );
	if (is_u3()) {
		int reg[3];

		reg[0] = 0;
		reg[1] = __cpu_to_be32(nvram_offset);
		reg[2] = __cpu_to_be32(nvram_size);
		set_property(dnode, "reg", (char *)&reg, sizeof(reg));
	} else {
		props[0] = __cpu_to_be32(nvram_offset);
		props[1] = __cpu_to_be32(nvram_size);
		set_property(dnode, "reg", (char *)&props, sizeof(props));
	}
	set_property(dnode, "device_type", "nvram", 6);
	NEWWORLD(set_property(dnode, "compatible", "nvram,flash", 12));

	chosen = find_dev("/chosen");
	snprintf(buf, sizeof(buf), "%s", get_path_from_ph(dnode));
	push_str(buf);
	fword("open-dev");
	set_int_property(chosen, "nvram", POP());

	aliases = find_dev("/aliases");
	set_property(aliases, "nvram", buf, strlen(buf) + 1);
}

#ifdef DUMP_NVRAM
static void
dump_nvram(void)
{
  int i, j;
  for (i = 0; i < 10; i++)
    {
      for (j = 0; j < 16; j++)
      printk ("%02x ", nvram[(i*16+j)<<4]);
      printk (" ");
      for (j = 0; j < 16; j++)
        if (isprint(nvram[(i*16+j)<<4]))
            printk("%c", nvram[(i*16+j)<<4]);
        else
          printk(".");
      printk ("\n");
      }
}
#endif


/*
 * The U3's NVRAM is Intel-style flash: two 8 KB banks, each headed by a
 * 0x5a "nvram" partition holding the Adler-32 of the bank from byte 20
 * and a generation. The live bank is A if its generation is the higher
 * valid one, else B; a write goes to the other bank, one generation on,
 * as Apple's firmware and Mac OS X do it.
 */
static int nvram_bank;
static uint32_t nvram_generation;

static uint8_t
core99_chrp_checksum(const unsigned char *p)
{
	unsigned int i, sum = 0;

	for (i = 0; i < 16; i++) {
		if (i == 1) {
			continue;
		}
		sum += p[i];
		if (sum > 0xff) {
			sum = (sum & 0xff) + 1;
		}
	}
	return sum;
}

static uint32_t
core99_adler32(const unsigned char *p, int len)
{
	uint32_t lo = 1, hi = 0;
	int i;

	for (i = 0; i < len; i++) {
		lo = (lo + p[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	return (hi << 16) | lo;
}

static uint32_t
core99_bank_generation(const unsigned char *p)
{
	if (p[0] != 0x5a || p[1] != core99_chrp_checksum(p) ||
	    *(uint32_t *)(p + 16) != core99_adler32(p + 20, NVRAM_BANK_SIZE - 20)) {
		return 0;
	}
	return *(uint32_t *)(p + 20);
}

static void
macio_nvram_flash_put(char *buf)
{
	unsigned char *b = (unsigned char *)buf;
	volatile unsigned char *p;
	int i;

	if (b[0] == 0x5a) {
		*(uint32_t *)(b + 20) = nvram_generation + 1;
		b[1] = core99_chrp_checksum(b);
		*(uint32_t *)(b + 16) = core99_adler32(b + 20, NVRAM_BANK_SIZE - 20);
	}

	nvram_bank ^= 1;
	p = (volatile unsigned char *)nvram + nvram_bank * NVRAM_BANK_SIZE;
	p[0] = 0x20;
	p[0] = 0xd0;
	while (!(p[0] & 0x80)) {
	}
	p[0] = 0xff;
	for (i = 0; i < NVRAM_BANK_SIZE; i++) {
		if (b[i] == 0xff) {
			continue;
		}
		p[i] = 0x40;
		p[i] = b[i];
		while (!(p[i] & 0x80)) {
		}
	}
	p[0] = 0xff;
	nvram_generation++;
}

void
macio_nvram_put(char *buf)
{
	int i;
        unsigned int it_shift = macio_nvram_shift();

	if (is_u3()) {
		macio_nvram_flash_put(buf);
		return;
	}
	for (i=0; i < arch_nvram_size(); i++)
		nvram[i << it_shift] = buf[i];
#ifdef DUMP_NVRAM
	printk("new nvram:\n");
	dump_nvram();
#endif
}

void
macio_nvram_get(char *buf)
{
	int i;
        unsigned int it_shift = macio_nvram_shift();

	if (is_u3()) {
		uint32_t gen_a = core99_bank_generation((unsigned char *)nvram);
		uint32_t gen_b = core99_bank_generation((unsigned char *)nvram +
		                                        NVRAM_BANK_SIZE);

		nvram_bank = gen_a > gen_b ? 0 : 1;
		nvram_generation = gen_a > gen_b ? gen_a : gen_b;
		memcpy(buf, nvram + nvram_bank * NVRAM_BANK_SIZE, NVRAM_BANK_SIZE);
		return;
	}
	for (i=0; i< arch_nvram_size(); i++)
                buf[i] = nvram[i << it_shift];

#ifdef DUMP_NVRAM
	printk("current nvram:\n");
	dump_nvram();
#endif
}

static void
openpic_init(const char *path, phys_addr_t addr)
{
        phandle_t dnode;
        int props[2];
        char buf[128];

        fword("new-device");
        push_str("interrupt-controller");
        fword("device-name");

        snprintf(buf, sizeof(buf), "%s/interrupt-controller", path);
        dnode = find_dev(buf);
        set_property(dnode, "device_type", "open-pic", 9);
        set_property(dnode, "compatible", "chrp,open-pic", 14);
        set_property(dnode, "built-in", "", 0);
        props[0] = __cpu_to_be32(IO_OPENPIC_OFFSET);
        props[1] = __cpu_to_be32(IO_OPENPIC_SIZE);
        set_property(dnode, "reg", (char *)&props, sizeof(props));
        set_int_property(dnode, "#interrupt-cells", 2);
        set_int_property(dnode, "#address-cells", 0);
        set_property(dnode, "interrupt-controller", "", 0);
        set_int_property(dnode, "clock-frequency", 4166666);

        fword("finish-device");
}

DECLARE_UNNAMED_NODE(ob_macio, 0, sizeof(int));

/* ( str len -- addr ) */

static void
ob_macio_decode_unit(void *private)
{
	ucell addr;

	const char *arg = pop_fstr_copy();

	addr = strtol(arg, NULL, 16);

	free((char*)arg);

	PUSH(addr);
}

/*  ( addr -- str len ) */

static void
ob_macio_encode_unit(void *private)
{
	char buf[8];

	ucell addr = POP();

	snprintf(buf, sizeof(buf), "%x", addr);

	push_str(buf);
}

static void
ob_macio_dma_alloc(int *idx)
{
    call_parent_method("dma-alloc");
}

static void
ob_macio_dma_free(int *idx)
{
    call_parent_method("dma-free");
}

static void
ob_macio_dma_map_in(int *idx)
{
    call_parent_method("dma-map-in");
}

static void
ob_macio_dma_map_out(int *idx)
{
    call_parent_method("dma-map-out");
}

static void
ob_macio_dma_sync(int *idx)
{
    call_parent_method("dma-sync");
}

NODE_METHODS(ob_macio) = {
        { "decode-unit",	ob_macio_decode_unit	},
        { "encode-unit",	ob_macio_encode_unit	},
        { "dma-alloc",		ob_macio_dma_alloc	},
        { "dma-free",		ob_macio_dma_free		},
        { "dma-map-in",		ob_macio_dma_map_in	},
        { "dma-map-out",	ob_macio_dma_map_out	},
        { "dma-sync",		ob_macio_dma_sync		},
};

/*
 * The U3's own I2C bus, with the Pulsar clock chip Mac OS X uses to
 * freeze the timebase while it starts a second CPU.
 */
static void
ob_u3_i2c_init(phandle_t u3)
{
        phandle_t dnode;
        int props[2];

        set_int_property(u3, "#address-cells", 1);
        set_int_property(u3, "#size-cells", 1);

        fword("new-device");
        push_str("i2c");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "i2c", 4);
        set_property(dnode, "compatible", "keywest-i2c\0uni-n-i2c", 22);
        props[0] = __cpu_to_be32(0xf8001000);
        props[1] = __cpu_to_be32(0x1000);
        set_property(dnode, "reg", (char *)&props, sizeof(props));
        set_int_property(dnode, "AAPL,address", 0xf8001000);
        set_int_property(dnode, "AAPL,address-step", 0x10);
        set_int_property(dnode, "AAPL,i2c-rate", 100);
        set_property(dnode, "AAPL,driver-name", ".i2c-uni-n", 11);
        set_int_property(dnode, "#address-cells", 1);
        set_int_property(dnode, "#size-cells", 0);

        fword("new-device");
        push_str("i2c-hwclock");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "compatible", "pulsar-legacy-slewing", 22);
        set_int_property(dnode, "reg", 0xd2);
        fword("finish-device");

        fword("finish-device");
}

/* The U3's DMA address relocation table */
static void
ob_u3_dart_init(void)
{
        phandle_t dnode;
        int props[2];

        fword("new-device");
        push_str("dart");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "dart", 5);
        set_property(dnode, "compatible", "u3-dart\0dart", 13);
        props[0] = __cpu_to_be32(0xf8033000);
        props[1] = __cpu_to_be32(0x7000);
        set_property(dnode, "reg", (char *)&props, sizeof(props));
        fword("finish-device");
}

void
ob_unin_init(void)
{
        phandle_t dnode;
        int props[2];

        fword("new-device");
        push_str(is_u3() ? "u3" : "uni-n");
        fword("device-name");

        dnode = get_cur_dev();
        set_property(dnode, "device_type", "memory-controller", 18);
        if (is_u3()) {
                set_property(dnode, "compatible", "u3", 3);
        } else {
                set_property(dnode, "compatible", "uni-north", 10);
        }
        /* PowerMac7,3: U3 2.3 */
        set_int_property(dnode, "device-rev", is_u3() ? 0xb3 : 7);
        if (is_u3()) {
                int reg[3];

                reg[0] = 0;
                reg[1] = __cpu_to_be32(0xf8000000);
                reg[2] = __cpu_to_be32(0x1000000);
                set_property(dnode, "reg", (char *)&reg, sizeof(reg));
        } else {
                props[0] = __cpu_to_be32(0xf8000000);
                props[1] = __cpu_to_be32(0x1000000);
                set_property(dnode, "reg", (char *)&props, sizeof(props));
        }

        if (is_u3()) {
                ob_u3_i2c_init(dnode);
                ob_u3_dart_init();
        }

        fword("finish-device");
}

static void macio_gpio_init(const char *path)
{
    fword("new-device");

    push_str("gpio");
    fword("device-name");

    push_str("gpio");
    fword("device-type");

    PUSH(1);
    fword("encode-int");
    push_str("#address-cells");
    fword("property");

    PUSH(0);
    fword("encode-int");
    push_str("#size-cells");
    fword("property");

    push_str("mac-io-gpio");
    fword("encode-string");
    push_str("compatible");
    fword("property");

    PUSH(0x50);
    fword("encode-int");
    PUSH(0x30);
    fword("encode-int");
    fword("encode+");
    push_str("reg");
    fword("property");

    /* Build the extint-gpio1 for the PMU */
    fword("new-device");
    push_str("extint-gpio1");
    fword("device-name");
    PUSH(0x2f);
    fword("encode-int");
    PUSH(0x1);
    fword("encode-int");
    fword("encode+");
    push_str("interrupts");
    fword("property");
    PUSH(0x9);
    fword("encode-int");
    push_str("reg");
    fword("property");
    push_str("keywest-gpio1");
    fword("encode-string");
    push_str("gpio");
    fword("encode-string");
    fword("encode+");
    push_str("compatible");
    fword("property");
    fword("finish-device");

    /* Build the programmer-switch */
    fword("new-device");
    push_str("programmer-switch");
    fword("device-name");
    push_str("programmer-switch");
    fword("encode-string");
    push_str("device_type");
    fword("property");
    PUSH(0x37);
    fword("encode-int");
    PUSH(0x0);
    fword("encode-int");
    fword("encode+");
    push_str("interrupts");
    fword("property");
    fword("finish-device");

    fword("finish-device");
}

void
ob_macio_heathrow_init(const char *path, phys_addr_t addr)
{
    phandle_t aliases;

    BIND_NODE_METHODS(get_cur_dev(), ob_macio);

    cuda_init(path, addr);
    macio_nvram_init(path, addr);
    escc_init(path, addr);
    macio_ide_init(path, addr, 2);

    aliases = find_dev("/aliases");
    set_property(aliases, "mac-io", path, strlen(path) + 1);
}

/* The K2's I2C bus, with the TAS3004 sound equalizer */
static void
k2_i2c_init(phys_addr_t addr, phandle_t mpic)
{
        phandle_t dnode;
        int props[2];

        fword("new-device");
        push_str("i2c");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "i2c", 4);
        set_property(dnode, "compatible", "k2-i2c\0keywest-i2c", 19);
        set_property(dnode, "built-in", "", 0);
        props[0] = __cpu_to_be32(0x18000);
        props[1] = __cpu_to_be32(0x1000);
        set_property(dnode, "reg", (char *)&props, sizeof(props));
        props[0] = __cpu_to_be32(0x1a);
        props[1] = __cpu_to_be32(1);
        set_property(dnode, "interrupts", (char *)&props, sizeof(props));
        set_int_property(dnode, "interrupt-parent", mpic);
        set_int_property(dnode, "AAPL,address", addr + 0x18000);
        set_int_property(dnode, "AAPL,address-step", 0x10);
        set_int_property(dnode, "AAPL,i2c-rate", 100);
        set_property(dnode, "AAPL,driver-name", ".i2c-mac-io", 12);
        set_int_property(dnode, "#address-cells", 1);
        set_int_property(dnode, "#size-cells", 0);

        fword("new-device");
        push_str("deq");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "deq", 4);
        set_int_property(dnode, "reg", 0x6a);
        set_int_property(dnode, "i2c-address", 0x6a);
        fword("finish-device");

        fword("finish-device");
}

static void
set_be32_property(phandle_t ph, const char *name, const uint32_t *v, int n)
{
        uint32_t buf[40];
        int i;

        for (i = 0; i < n; i++) {
                buf[i] = __cpu_to_be32(v[i]);
        }
        set_property(ph, name, (char *)buf, n * sizeof(uint32_t));
}

/* FCR1 I2S0 functions of the K2, as ON_DEMAND platform-do properties */
static const struct {
        const char *name;
        uint32_t cmd[5];
        int n;
} k2_i2s_pfuncs[] = {
        { "platform-do-enable",           { 0x08000000, 3, 0x3c, 0x2000, 0x2000 }, 5 },
        { "platform-do-disable",          { 0x08000000, 3, 0x3c, 0, 0x2000 }, 5 },
        { "platform-do-clock-enable",     { 0x08800000, 3, 0x3c, 0x1000, 0x1000 }, 5 },
        { "platform-do-clock-disable",    { 0x08800000, 3, 0x3c, 0, 0x1000 }, 5 },
        { "platform-do-sw-reset",         { 0x08000000, 3, 0x3c, 0x800, 0x800 }, 5 },
        { "platform-do-clear-sw-reset",   { 0x08000000, 3, 0x3c, 0, 0x800 }, 5 },
        { "platform-do-cell-enable",      { 0x08000000, 3, 0x3c, 0x400, 0x400 }, 5 },
        { "platform-do-cell-disable",     { 0x08000000, 3, 0x3c, 0, 0x400 }, 5 },
};

static const struct {
        const char *name;
        uint32_t mask, shift;
} k2_i2s_getfuncs[] = {
        { "platform-do-get-enable",       0x2000, 0xd },
        { "platform-do-get-clock-enable", 0x1000, 0xc },
        { "platform-do-get-sw-reset",     0x800,  0xb },
        { "platform-do-get-cell-enable",  0x400,  0xa },
};

static const char *k2_i2s_fnames[] = {
        "enable", "disable", "clock-enable", "clock-disable", "sw-reset",
        "clear-sw-reset", "cell-enable", "cell-disable", "get-enable",
        "get-clock-enable", "get-sw-reset", "get-cell-enable",
};

/* The K2's I2S-a sound cell, with a TAS3004 on the K2 I2C bus */
static void
k2_i2s_init(phys_addr_t addr, phandle_t mpic)
{
        static const uint32_t clock_data[] = {
                0x02b11000, 0x3c, 0x3400, 0x44, 0x4000, 0x44, 0x400, 0x44,
                4, 0x69303439, 0x6e756c6c, 0,
                0x02ee0000, 0x3c, 0x3400, 0x44, 0x4000, 0x44, 0x200, 0x44,
                2, 0x69303435, 0x6e756c6c, 0,
                0x01194000, 0x3c, 0x3000, 0x3c, 0x400, 0x44, 0x4000, 0,
                0, 0x6e756c6c, 0x6e756c6c, 0,
        };
        static const uint32_t clock_id[] = {
                0x69327330, 0x69303435, 0x69303439, 0x69303138,
        };
        static const uint32_t prio[] = { 2, 4, 4 };
        uint32_t props[8];
        phandle_t macio, i2s_a, dnode;
        char buf[40];
        unsigned int i;

        macio = get_cur_dev();

        fword("new-device");
        push_str("i2s");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "i2s", 4);
        set_property(dnode, "built-in", "", 0);
        props[0] = 0x10000;
        props[1] = 0x1000;
        props[2] = 0x8000;
        props[3] = 0x400;
        set_be32_property(dnode, "reg", props, 4);
        set_int_property(dnode, "#address-cells", 1);
        set_property(dnode, "ranges", "", 0);
        set_int_property(dnode, "AAPL,address", addr + 0x10000);

        fword("new-device");
        push_str("i2s-a");
        fword("device-name");
        i2s_a = get_cur_dev();
        set_property(i2s_a, "device_type", "soundbus", 9);
        set_property(i2s_a, "compatible", "i2sbus", 7);
        set_property(i2s_a, "built-in", "", 0);
        props[0] = 0;
        props[1] = 0x1000;
        props[2] = 0;
        props[3] = 0x100;
        props[4] = 0x100;
        props[5] = 0x100;
        set_be32_property(i2s_a, "reg", props, 6);
        props[0] = 0x1e;
        props[1] = 1;
        props[2] = 1;
        props[3] = 0;
        props[4] = 2;
        props[5] = 0;
        set_be32_property(i2s_a, "interrupts", props, 6);
        set_int_property(i2s_a, "interrupt-parent", mpic);
        set_be32_property(i2s_a, "AAPL,requested-priorities", prio, 3);
        set_be32_property(i2s_a, "AAPL,clock-id", clock_id, 4);
        set_be32_property(i2s_a, "AAPL,clock-data", clock_data,
                          sizeof(clock_data) / sizeof(clock_data[0]));
        for (i = 0; i < sizeof(k2_i2s_fnames) / sizeof(k2_i2s_fnames[0]); i++) {
                snprintf(buf, sizeof(buf), "platform-%s", k2_i2s_fnames[i]);
                set_int_property(i2s_a, buf, macio);
        }

        fword("new-device");
        push_str("sound");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "soundchip", 10);
        set_property(dnode, "compatible", "AOAK2", 6);
        set_property(dnode, "built-in", "", 0);
        set_int_property(dnode, "layout-id", 36);
        set_int_property(dnode, "object-model-version", 2);
        set_int_property(dnode, "vendor-id", 0x106b);
        fword("finish-device");

        fword("finish-device");
        fword("finish-device");

        for (i = 0; i < sizeof(k2_i2s_pfuncs) / sizeof(k2_i2s_pfuncs[0]); i++) {
                props[0] = i2s_a;
                memcpy(&props[1], k2_i2s_pfuncs[i].cmd,
                       k2_i2s_pfuncs[i].n * sizeof(uint32_t));
                set_be32_property(macio, k2_i2s_pfuncs[i].name, props,
                                  k2_i2s_pfuncs[i].n + 1);
        }
        for (i = 0; i < sizeof(k2_i2s_getfuncs) / sizeof(k2_i2s_getfuncs[0]); i++) {
                props[0] = i2s_a;
                props[1] = 0x08000000;
                props[2] = 0x1a;
                props[3] = 0x3c;
                props[4] = k2_i2s_getfuncs[i].mask;
                props[5] = k2_i2s_getfuncs[i].shift;
                props[6] = 1;
                set_be32_property(macio, k2_i2s_getfuncs[i].name, props, 7);
        }
}

void
ob_macio_keylargo_init(const char *path, phys_addr_t addr)
{
    phandle_t aliases;

    BIND_NODE_METHODS(get_cur_dev(), ob_macio);

    if (has_pmu()) {
        macio_gpio_init(path);
        pmu_init(path, addr);
    } else {
        cuda_init(path, addr);
    }

    escc_init(path, addr);
    /* the K2's ATA is a separate PCI function */
    if (!is_u3()) {
        macio_ide_init(path, addr, 2);
    }
    openpic_init(path, addr);
    if (is_u3()) {
        char buf[128];
        phandle_t mpic;

        snprintf(buf, sizeof(buf), "%s/interrupt-controller", path);
        mpic = find_dev(buf);
        k2_i2s_init(addr, mpic);
        k2_i2c_init(addr, mpic);
    }

    aliases = find_dev("/aliases");
    set_property(aliases, "mac-io", path, strlen(path) + 1);
}
