/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#define LOG_TAG "DSI"

#include <linux/delay.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
/* #include "mach/irqs.h" */
#include "mt-plat/sync_write.h"
#include <debug.h>
#include "disp_drv_log.h"
#include "disp_drv_platform.h"
#include "mtkfb.h"
#include "ddp_drv.h"
#include "ddp_manager.h"
#include "ddp_dump.h"
#include "ddp_irq.h"
#include "ddp_dsi.h"
#include "ddp_log.h"
#include "ddp_mmp.h"
#include "disp_helper.h"
#include "ddp_reg.h"

#ifdef CONFIG_MTK_LEGACY
#include <mach/mt_gpio.h>
#include <cust_gpio_usage.h>
#else
#include "disp_dts_gpio.h"
#endif
#ifndef CONFIG_MTK_CLKMGR
#include "ddp_clkmgr.h"
#endif

#define DSI_OUTREG32(cmdq, addr, val) DISP_REG_SET(cmdq, addr, val)
#define DSI_BACKUPREG32(cmdq, hSlot, idx, addr) DISP_REG_BACKUP(cmdq, hSlot, idx, addr)
#define DSI_POLLREG32(cmdq, addr, mask, value) DISP_REG_CMDQ_POLLING(cmdq, addr, value, mask)
#define DSI_INREG32(type, addr) INREG32(addr)
#define DSI_READREG32(type, dst, src) mt_reg_sync_writel(INREG32(src), dst)

static int dsi_reg_op_debug;

#define BIT_TO_VALUE(TYPE, bit)  \
do { \
	TYPE r;\
	*(unsigned long *)(&r) = ((unsigned int)0x00000000);\
	r.bit = ~(r.bit);\
	r;\
} while (0)

#define DSI_MASKREG32(cmdq, REG, MASK, VALUE)	DISP_REG_MASK((cmdq), (REG), (VALUE), (MASK))

#define DSI_OUTREGBIT(cmdq, TYPE, REG, bit, value)  \
do {\
	TYPE r;\
	TYPE v;\
	if (cmdq) {\
		*(unsigned int *)(&r) = ((unsigned int)0x00000000); \
		r.bit = ~(r.bit);  \
		*(unsigned int *)(&v) = ((unsigned int)0x00000000); \
		v.bit = value; \
		DISP_REG_MASK(cmdq, &REG, AS_UINT32(&v), AS_UINT32(&r)); \
	} else { \
		mt_reg_sync_writel(INREG32(&REG), &r); \
		r.bit = (value); \
		DISP_REG_SET(cmdq, &REG, INREG32(&r)); \
	} \
} while (0)


#ifdef CONFIG_FPGA_EARLY_PORTING
#define MIPITX_Write60384(slave_addr, write_addr, write_data)			\
do {	\
	DDPMSG("MIPITX_Write60384:0x%x,0x%x,0x%x\n", slave_addr, write_addr, write_data);		\
	mt_reg_sync_writel(0x2, MIPITX_BASE+0x14);		\
	mt_reg_sync_writel(0x1, MIPITX_BASE+0x18);		\
	mt_reg_sync_writel(((unsigned int)slave_addr << 0x1), MIPITX_BASE+0x04);		\
	mt_reg_sync_writel(write_addr, MIPITX_BASE+0x0);		\
	mt_reg_sync_writel(write_data, MIPITX_BASE+0x0);		\
	mt_reg_sync_writel(0x1, MIPITX_BASE+0x24);		\
	while (INREG32(MIPITX_BASE+0xC)&0x1 != 0x1)\
		;		\
	mt_reg_sync_writel(0xFF, MIPITX_BASE+0xC);		\
	\
	mt_reg_sync_writel(0x1, MIPITX_BASE+0x14);		\
	mt_reg_sync_writel(0x1, MIPITX_BASE+0x18);		\
	mt_reg_sync_writel(((unsigned int)slave_addr << 0x1), MIPITX_BASE+0x04);		\
	mt_reg_sync_writel(write_addr, MIPITX_BASE+0x0);		\
	mt_reg_sync_writel(0x1, MIPITX_BASE+0x24);		\
	while (INREG32(MIPITX_BASE+0xC)&0x1 != 0x1)\
		;		\
	mt_reg_sync_writel(0xFF, MIPITX_BASE+0xC);		\
	\
	mt_reg_sync_writel(0x1, MIPITX_BASE+0x14);		\
	mt_reg_sync_writel(0x1, MIPITX_BASE+0x18);		\
	mt_reg_sync_writel(((unsigned int)slave_addr << 0x1)+1, MIPITX_BASE+0x04);		\
	mt_reg_sync_writel(0x1, MIPITX_BASE+0x24);		\
	while (INREG32(MIPITX_BASE+0xC)&0x1 != 0x1)\
		;		\
	mt_reg_sync_writel(0xFF, MIPITX_BASE+0xC);		\
	\
	DDPMSG("MIPI write data = 0x%x, read data = 0x%x\n", write_data, INREG32(MIPITX_BASE));		\
	if (INREG32(MIPITX_BASE) == write_data) \
		DDPMSG("MIPI write success\n");		\
	else \
		DDPMSG("MIPI write fail\n");		\
} while (0)

#define MIPITX_INREG32(addr) \
do { \
	unsigned int val = 0;\
	if (0)\
		val = INREG32(addr); \
	if (dsi_reg_op_debug) \
		DDPMSG("[mipitx/inreg]%p=0x%08x\n", (void *)addr, val); \
	val; \
} while (0)

#define MIPITX_OUTREG32(addr, val) \
do { \
	if (dsi_reg_op_debug) \
		DDPMSG("[mipitx/reg]%p=0x%08x\n", (void *)addr, val); \
	if (0) \
		mt_reg_sync_writel(val, addr); \
} while (0)

#define MIPITX_OUTREGBIT(TYPE, REG, bit, value) \
do { \
	TYPE r;\
	if (0) \
		mt_reg_sync_writel(INREG32(&REG), &r); \
	*(unsigned long *)(&r) = ((unsigned long)0x00000000);	  \
	r.bit = value;	  \
	MIPITX_OUTREG32(&REG, AS_UINT32(&r));	  \
} while (0)

#define MIPITX_MASKREG32(x, y, z)  MIPITX_OUTREG32(x, (MIPITX_INREG32(x)&~(y))|(z))
#else
#define MIPITX_INREG32(addr) \
do { \
	unsigned int val = 0; val = INREG32(addr);\
	if (dsi_reg_op_debug) \
		DDPMSG("[mipitx/inreg]%p=0x%08x\n", (void *)addr, val); \
	val; \
} while (0)

#define MIPITX_OUTREG32(addr, val) \
do {\
	if (dsi_reg_op_debug) {	\
		DDPMSG("[mipitx/reg]%p=0x%08x\n", (void *)addr, val);\
	} \
	mt_reg_sync_writel(val, addr);\
} while (0)

#define MIPITX_OUTREGBIT(TYPE, REG, bit, value)  \
do {	\
	TYPE r;\
	mt_reg_sync_writel(INREG32(&REG), &r);	  \
	r.bit = value;	  \
	MIPITX_OUTREG32(&REG, AS_UINT32(&r));	  \
} while (0)

#define MIPITX_MASKREG32(x, y, z)  MIPITX_OUTREG32(x, (MIPITX_INREG32(x)&~(y))|(z))
#endif

struct t_dsi_context {
	unsigned int lcm_width;
	unsigned int lcm_height;
	struct cmdqRecStruct **handle;
	bool enable;
	struct DSI_REGS regBackup;
	unsigned int cmdq_size;
	LCM_DSI_PARAMS dsi_params;
};

struct t_dsi_context _dsi_context[DSI_INTERFACE_NUM];

#define DSI_MODULE_BEGIN(x)		(x == DISP_MODULE_DSIDUAL ? 0 : DSI_MODULE_to_ID(x))
#define DSI_MODULE_END(x)		(x == DISP_MODULE_DSIDUAL ? 1 : DSI_MODULE_to_ID(x))
#define DSI_MODULE_to_ID(x)		(x == DISP_MODULE_DSI0 ? 0 : 1)
#define DIFF_CLK_LANE_LP (0x10)

struct DSI_REGS *DSI_REG[2];
struct DSI_PHY_REGS *DSI_PHY_REG[2];
struct DSI_CMDQ_REGS *DSI_CMDQ_REG[2];
struct DSI_VM_CMDQ_REGS *DSI_VM_CMD_REG[2];

static wait_queue_head_t _dsi_cmd_done_wait_queue[2];
static wait_queue_head_t _dsi_dcs_read_wait_queue[2];
static wait_queue_head_t _dsi_wait_bta_te[2];
static wait_queue_head_t _dsi_wait_ext_te[2];
static wait_queue_head_t _dsi_wait_vm_done_queue[2];
static wait_queue_head_t _dsi_wait_vm_cmd_done_queue[2];
static wait_queue_head_t _dsi_wait_sleep_out_done_queue[2];
static bool waitRDDone;
static bool wait_vm_cmd_done;
static bool wait_sleep_out_done;
static int s_isDsiPowerOn;
static int dsi_currect_mode;
static int dsi_force_config;
static int dsi0_te_enable = 1;
static const LCM_UTIL_FUNCS lcm_utils_dsi0;
static const LCM_UTIL_FUNCS lcm_utils_dsi1;
static const LCM_UTIL_FUNCS lcm_utils_dsidual;
static cmdqBackupSlotHandle _h_intstat;

unsigned int clock_lane[2] = { 0 }; /* MIPITX_DSI_CLOCK_LANE */
unsigned int data_lane0[2] = { 0 }; /* MIPITX_DSI_DATA_LANE0 */
unsigned int data_lane1[2] = { 0 }; /* MIPITX_DSI_DATA_LANE1 */
unsigned int data_lane2[2] = { 0 }; /* MIPITX_DSI_DATA_LANE2 */
unsigned int data_lane3[2] = { 0 }; /* MIPITX_DSI_DATA_LANE3 */

atomic_t PMaster_enable = ATOMIC_INIT(0);

static int _dsi_is_video_mode(enum DISP_MODULE_ENUM module);

static const char *_dsi_cmd_mode_parse_state(unsigned int state)
{
	switch (state) {
	case 0x0001:
		return "idle";
	case 0x0002:
		return "Reading command queue for header";
	case 0x0004:
		return "Sending type-0 command";
	case 0x0008:
		return "Waiting frame data from RDMA for type-1 command";
	case 0x0010:
		return "Sending type-1 command";
	case 0x0020:
		return "Sending type-2 command";
	case 0x0040:
		return "Reading command queue for type-2 data";
	case 0x0080:
		return "Sending type-3 command";
	case 0x0100:
		return "Sending BTA";
	case 0x0200:
		return "Waiting RX-read data";
	case 0x0400:
		return "Waiting SW RACK for RX-read data";
	case 0x0800:
		return "Waiting TE";
	case 0x1000:
		return "Get TE";
	case 0x2000:
		return "Waiting SW RACK for TE";
	case 0x4000:
		return "Waiting external TE";
	case 0x8000:
		return "Get external TE";
	default:
		return "unknown";
	}
	return "unknown";
}

static const char *_dsi_vdo_mode_parse_state(unsigned int state)
{
	switch (state) {
	case 0x0001:
		return "Video mode idle";
	case 0x0002:
		return "Sync start packet";
	case 0x0004:
		return "Hsync active";
	case 0x0008:
		return "Sync end packet";
	case 0x0010:
		return "Hsync back porch";
	case 0x0020:
		return "Video data period";
	case 0x0040:
		return "Hsync front porch";
	case 0x0080:
		return "BLLP";
	case 0x0100:
		return "--";
	case 0x0200:
		return "Mix mode using command mode transmission";
	case 0x0400:
		return "Command transmission in BLLP";
	default:
		return "unknown";
	}

	return "unknown";
}

enum DSI_STATUS DSI_DumpRegisters(enum DISP_MODULE_ENUM module, int level)
{
	u32 i = 0;
	u32 k = 0;

	if (disp_helper_get_option(DISP_OPT_REG_PARSER_RAW_DUMP)) {
		if (module == DISP_MODULE_DSI0 || module == DISP_MODULE_DSIDUAL) {
			DDPDUMP("== START: DISP DSI0 REGS ==\n");
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x000, INREG32(DDP_REG_BASE_DSI0 + 0x000),
				0x004, INREG32(DDP_REG_BASE_DSI0 + 0x004),
				0x008, INREG32(DDP_REG_BASE_DSI0 + 0x008),
				0x00C, INREG32(DDP_REG_BASE_DSI0 + 0x00C));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x010, INREG32(DDP_REG_BASE_DSI0 + 0x010),
				0x014, INREG32(DDP_REG_BASE_DSI0 + 0x014),
				0x018, INREG32(DDP_REG_BASE_DSI0 + 0x018),
				0x01C, INREG32(DDP_REG_BASE_DSI0 + 0x01C));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x020, INREG32(DDP_REG_BASE_DSI0 + 0x020),
				0x024, INREG32(DDP_REG_BASE_DSI0 + 0x024),
				0x028, INREG32(DDP_REG_BASE_DSI0 + 0x028),
				0x02C, INREG32(DDP_REG_BASE_DSI0 + 0x02C));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x030, INREG32(DDP_REG_BASE_DSI0 + 0x030),
				0x034, INREG32(DDP_REG_BASE_DSI0 + 0x034),
				0x038, INREG32(DDP_REG_BASE_DSI0 + 0x038),
				0x050, INREG32(DDP_REG_BASE_DSI0 + 0x050));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x054, INREG32(DDP_REG_BASE_DSI0 + 0x054),
				0x058, INREG32(DDP_REG_BASE_DSI0 + 0x058),
				0x05C, INREG32(DDP_REG_BASE_DSI0 + 0x05C),
				0x060, INREG32(DDP_REG_BASE_DSI0 + 0x060));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x064, INREG32(DDP_REG_BASE_DSI0 + 0x064),
				0x068, INREG32(DDP_REG_BASE_DSI0 + 0x068),
				0x074, INREG32(DDP_REG_BASE_DSI0 + 0x074),
				0x078, INREG32(DDP_REG_BASE_DSI0 + 0x078));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x07C, INREG32(DDP_REG_BASE_DSI0 + 0x07C),
				0x080, INREG32(DDP_REG_BASE_DSI0 + 0x080),
				0x084, INREG32(DDP_REG_BASE_DSI0 + 0x084),
				0x088, INREG32(DDP_REG_BASE_DSI0 + 0x088));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x090, INREG32(DDP_REG_BASE_DSI0 + 0x090),
				0x094, INREG32(DDP_REG_BASE_DSI0 + 0x094),
				0x098, INREG32(DDP_REG_BASE_DSI0 + 0x098),
				0x0A0, INREG32(DDP_REG_BASE_DSI0 + 0x0A0));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x0A4, INREG32(DDP_REG_BASE_DSI0 + 0x0A4),
				0x100, INREG32(DDP_REG_BASE_DSI0 + 0x100),
				0x104, INREG32(DDP_REG_BASE_DSI0 + 0x104),
				0x108, INREG32(DDP_REG_BASE_DSI0 + 0x108));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x10C, INREG32(DDP_REG_BASE_DSI0 + 0x10C),
				0x110, INREG32(DDP_REG_BASE_DSI0 + 0x110),
				0x114, INREG32(DDP_REG_BASE_DSI0 + 0x114),
				0x118, INREG32(DDP_REG_BASE_DSI0 + 0x118));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x11C, INREG32(DDP_REG_BASE_DSI0 + 0x11C),
				0x130, INREG32(DDP_REG_BASE_DSI0 + 0x130),
				0x134, INREG32(DDP_REG_BASE_DSI0 + 0x134),
				0x138, INREG32(DDP_REG_BASE_DSI0 + 0x138));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x13C, INREG32(DDP_REG_BASE_DSI0 + 0x13C),
				0x140, INREG32(DDP_REG_BASE_DSI0 + 0x140),
				0x144, INREG32(DDP_REG_BASE_DSI0 + 0x144),
				0x148, INREG32(DDP_REG_BASE_DSI0 + 0x148));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x14C, INREG32(DDP_REG_BASE_DSI0 + 0x14C),
				0x150, INREG32(DDP_REG_BASE_DSI0 + 0x150),
				0x154, INREG32(DDP_REG_BASE_DSI0 + 0x154),
				0x158, INREG32(DDP_REG_BASE_DSI0 + 0x158));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x15C, INREG32(DDP_REG_BASE_DSI0 + 0x15C),
				0x160, INREG32(DDP_REG_BASE_DSI0 + 0x160),
				0x164, INREG32(DDP_REG_BASE_DSI0 + 0x164),
				0x168, INREG32(DDP_REG_BASE_DSI0 + 0x168));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x16C, INREG32(DDP_REG_BASE_DSI0 + 0x16C),
				0x170, INREG32(DDP_REG_BASE_DSI0 + 0x170),
				0x174, INREG32(DDP_REG_BASE_DSI0 + 0x174),
				0x178, INREG32(DDP_REG_BASE_DSI0 + 0x178));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x17C, INREG32(DDP_REG_BASE_DSI0 + 0x17C),
				0x180, INREG32(DDP_REG_BASE_DSI0 + 0x180),
				0x184, INREG32(DDP_REG_BASE_DSI0 + 0x184),
				0x188, INREG32(DDP_REG_BASE_DSI0 + 0x188));
			DDPDUMP("DSI0: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x18C, INREG32(DDP_REG_BASE_DSI0 + 0x18C),
				0x190, INREG32(DDP_REG_BASE_DSI0 + 0x190),
				0x198, INREG32(DDP_REG_BASE_DSI0 + 0x198),
				0x19C, INREG32(DDP_REG_BASE_DSI0 + 0x19C));
			DDPDUMP("DSI0: 0x%04x=0x%08x\n",
				0x200, INREG32(DDP_REG_BASE_DSI0 + 0x200));
			DDPDUMP("-- END: DISP DSI0 REGS --\n");
		}

		if (module == DISP_MODULE_DSI1 || module == DISP_MODULE_DSIDUAL) {
			DDPDUMP("== START: DISP DSI1 REGS ==\n");
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x000, INREG32(DDP_REG_BASE_DSI1 + 0x000),
				0x004, INREG32(DDP_REG_BASE_DSI1 + 0x004),
				0x008, INREG32(DDP_REG_BASE_DSI1 + 0x008),
				0x00C, INREG32(DDP_REG_BASE_DSI1 + 0x00C));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x010, INREG32(DDP_REG_BASE_DSI1 + 0x010),
				0x014, INREG32(DDP_REG_BASE_DSI1 + 0x014),
				0x018, INREG32(DDP_REG_BASE_DSI1 + 0x018),
				0x01C, INREG32(DDP_REG_BASE_DSI1 + 0x01C));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x020, INREG32(DDP_REG_BASE_DSI1 + 0x020),
				0x024, INREG32(DDP_REG_BASE_DSI1 + 0x024),
				0x028, INREG32(DDP_REG_BASE_DSI1 + 0x028),
				0x02C, INREG32(DDP_REG_BASE_DSI1 + 0x02C));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x030, INREG32(DDP_REG_BASE_DSI1 + 0x030),
				0x034, INREG32(DDP_REG_BASE_DSI1 + 0x034),
				0x038, INREG32(DDP_REG_BASE_DSI1 + 0x038),
				0x050, INREG32(DDP_REG_BASE_DSI1 + 0x050));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x054, INREG32(DDP_REG_BASE_DSI1 + 0x054),
				0x058, INREG32(DDP_REG_BASE_DSI1 + 0x058),
				0x05C, INREG32(DDP_REG_BASE_DSI1 + 0x05C),
				0x060, INREG32(DDP_REG_BASE_DSI1 + 0x060));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x064, INREG32(DDP_REG_BASE_DSI1 + 0x064),
				0x068, INREG32(DDP_REG_BASE_DSI1 + 0x068),
				0x074, INREG32(DDP_REG_BASE_DSI1 + 0x074),
				0x078, INREG32(DDP_REG_BASE_DSI1 + 0x078));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x07C, INREG32(DDP_REG_BASE_DSI1 + 0x07C),
				0x080, INREG32(DDP_REG_BASE_DSI1 + 0x080),
				0x084, INREG32(DDP_REG_BASE_DSI1 + 0x084),
				0x088, INREG32(DDP_REG_BASE_DSI1 + 0x088));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x090, INREG32(DDP_REG_BASE_DSI1 + 0x090),
				0x094, INREG32(DDP_REG_BASE_DSI1 + 0x094),
				0x098, INREG32(DDP_REG_BASE_DSI1 + 0x098),
				0x0A0, INREG32(DDP_REG_BASE_DSI1 + 0x0A0));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x0A4, INREG32(DDP_REG_BASE_DSI1 + 0x0A4),
				0x100, INREG32(DDP_REG_BASE_DSI1 + 0x100),
				0x104, INREG32(DDP_REG_BASE_DSI1 + 0x104),
				0x108, INREG32(DDP_REG_BASE_DSI1 + 0x108));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x10C, INREG32(DDP_REG_BASE_DSI1 + 0x10C),
				0x110, INREG32(DDP_REG_BASE_DSI1 + 0x110),
				0x114, INREG32(DDP_REG_BASE_DSI1 + 0x114),
				0x118, INREG32(DDP_REG_BASE_DSI1 + 0x118));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x11C, INREG32(DDP_REG_BASE_DSI1 + 0x11C),
				0x130, INREG32(DDP_REG_BASE_DSI1 + 0x130),
				0x134, INREG32(DDP_REG_BASE_DSI1 + 0x134),
				0x138, INREG32(DDP_REG_BASE_DSI1 + 0x138));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x13C, INREG32(DDP_REG_BASE_DSI1 + 0x13C),
				0x140, INREG32(DDP_REG_BASE_DSI1 + 0x140),
				0x144, INREG32(DDP_REG_BASE_DSI1 + 0x144),
				0x148, INREG32(DDP_REG_BASE_DSI1 + 0x148));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x14C, INREG32(DDP_REG_BASE_DSI1 + 0x14C),
				0x150, INREG32(DDP_REG_BASE_DSI1 + 0x150),
				0x154, INREG32(DDP_REG_BASE_DSI1 + 0x154),
				0x158, INREG32(DDP_REG_BASE_DSI1 + 0x158));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x15C, INREG32(DDP_REG_BASE_DSI1 + 0x15C),
				0x160, INREG32(DDP_REG_BASE_DSI1 + 0x160),
				0x164, INREG32(DDP_REG_BASE_DSI1 + 0x164),
				0x168, INREG32(DDP_REG_BASE_DSI1 + 0x168));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x16C, INREG32(DDP_REG_BASE_DSI1 + 0x16C),
				0x170, INREG32(DDP_REG_BASE_DSI1 + 0x170),
				0x174, INREG32(DDP_REG_BASE_DSI1 + 0x174),
				0x178, INREG32(DDP_REG_BASE_DSI1 + 0x178));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x17C, INREG32(DDP_REG_BASE_DSI1 + 0x17C),
				0x180, INREG32(DDP_REG_BASE_DSI1 + 0x180),
				0x184, INREG32(DDP_REG_BASE_DSI1 + 0x184),
				0x188, INREG32(DDP_REG_BASE_DSI1 + 0x188));
			DDPDUMP("DSI1: 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x, 0x%04x=0x%08x\n",
				0x18C, INREG32(DDP_REG_BASE_DSI1 + 0x18C),
				0x190, INREG32(DDP_REG_BASE_DSI1 + 0x190),
				0x198, INREG32(DDP_REG_BASE_DSI1 + 0x198),
				0x19C, INREG32(DDP_REG_BASE_DSI1 + 0x19C));
			DDPDUMP("DSI1: 0x%04x=0x%08x\n",
				0x200, INREG32(DDP_REG_BASE_DSI1 + 0x200));
			DDPDUMP("-- END: DISP DSI1 REGS --\n");
		}
	} else {
		DDPDUMP("== DISP DSI REGS ==\n");
		if (level >= 0) {
			for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
				unsigned int DSI_DBG8_Status;
				unsigned int DSI_DBG9_Status;
				unsigned long dsi_base_addr = (unsigned long)DSI_REG[i];

				if (DSI_REG[0]->DSI_MODE_CTRL.MODE == CMD_MODE) {
					unsigned int DSI_DBG6_Status = (INREG32(dsi_base_addr + 0x160)) & 0xffff;

					DDPDUMP("DSI%d state6(cmd mode):%s\n",
						i, _dsi_cmd_mode_parse_state(DSI_DBG6_Status));
				} else {
					unsigned int DSI_DBG7_Status = (INREG32(dsi_base_addr + 0x164)) & 0xff;

					DDPDUMP("DSI%d state7(vdo mode):%s\n",
						i, _dsi_vdo_mode_parse_state(DSI_DBG7_Status));
				}
				DSI_DBG8_Status = (INREG32(dsi_base_addr + 0x168)) & 0x3fff;
				DDPDUMP("DSI%d state8 WORD_COUNTER(cmd mode):%s\n",
					i, _dsi_cmd_mode_parse_state(DSI_DBG8_Status));
				DSI_DBG9_Status = (INREG32(dsi_base_addr + 0x16C)) & 0x3fffff;
				DDPDUMP("DSI%d state9 LINE_COUNTER(cmd mode):%s\n",
					i, _dsi_cmd_mode_parse_state(DSI_DBG9_Status));
			}
		}
		if (level >= 1) {
			for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
				unsigned long dsi_base_addr = (unsigned long)DSI_REG[i];
#ifndef CONFIG_FPGA_EARLY_PORTING
				unsigned long mipi_base_addr = (unsigned long)DSI_PHY_REG[i];
#endif

				DDPDUMP("== DSI%d REGS ==\n", i);
				for (k = 0; k < sizeof(struct DSI_REGS); k += 16) {
					DDPDUMP("0x%04x: 0x%08x 0x%08x 0x%08x 0x%08x\n", k,
						INREG32(dsi_base_addr + k),
						INREG32(dsi_base_addr + k + 0x4),
						INREG32(dsi_base_addr + k + 0x8),
						INREG32(dsi_base_addr + k + 0xc));
				}

				DDPDUMP("- DSI%d CMD REGS -\n", i);
				for (k = 0; k < 32; k += 16) { /* only dump first 32 bytes cmd */
					DDPDUMP("0x%04x: 0x%08x 0x%08x 0x%08x 0x%08x\n", k,
						INREG32((dsi_base_addr + 0x200 + k)),
						INREG32((dsi_base_addr + 0x200 + k + 0x4)),
						INREG32((dsi_base_addr + 0x200 + k + 0x8)),
						INREG32((dsi_base_addr + 0x200 + k + 0xc)));
				}

#ifndef CONFIG_FPGA_EARLY_PORTING
				DDPDUMP("== DSI_PHY%d REGS ==\n", i);
				for (k = 0; k < sizeof(struct DSI_PHY_REGS); k += 16) {
					DDPDUMP("0x%04x: 0x%08x 0x%08x 0x%08x 0x%08x\n", k,
						INREG32((mipi_base_addr + k)),
						INREG32((mipi_base_addr + k + 0x4)),
						INREG32((mipi_base_addr + k + 0x8)),
						INREG32((mipi_base_addr + k + 0xc)));
				}
#endif
			}
		}
	}
	return DSI_STATUS_OK;
}

static void _DSI_INTERNAL_IRQ_Handler(enum DISP_MODULE_ENUM module, unsigned int param)
{
	int i = 0;
	struct DSI_INT_STATUS_REG status;
	struct DSI_TXRX_CTRL_REG txrx_ctrl;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		status = *(struct DSI_INT_STATUS_REG *)(&param);
		if (status.RD_RDY) {
			/* /write clear RD_RDY interrupt */

			/* / write clear RD_RDY interrupt must be before DSI_RACK */
			/* / because CMD_DONE will raise after DSI_RACK, */
			/* / so write clear RD_RDY after that will clear CMD_DONE too */
			/*do*/
			   /*{*/
			   /*/send read ACK*/
			   /*DSI_REG->DSI_RACK.DSI_RACK = 1;*/
			   /*DSI_OUTREGBIT(NULL, DSI_RACK_REG,DSI_REG[i]->DSI_RACK,DSI_RACK,1);*/
			   /*DDPMSG("send read ACK\n");*/
			   /*} while(DSI_REG[i]->DSI_INTSTA.BUSY); */
			waitRDDone = true;
			wake_up_interruptible(&_dsi_dcs_read_wait_queue[i]);
		}

		if (status.CMD_DONE || status.VM_DONE)
			wake_up_interruptible(&_dsi_cmd_done_wait_queue[i]);


		if (status.TE_RDY) {
			DSI_OUTREG32(NULL, &txrx_ctrl, INREG32(&DSI_REG[i]->DSI_TXRX_CTRL));
			if (txrx_ctrl.EXT_TE_EN == 1)
				wake_up_interruptible(&_dsi_wait_ext_te[i]);
			else
				wake_up_interruptible(&_dsi_wait_bta_te[i]);

		}

		if (status.VM_DONE)
			wake_up_interruptible(&_dsi_wait_vm_done_queue[i]);
		if (status.VM_CMD_DONE) {
			wait_vm_cmd_done = true;
			wake_up_interruptible(&_dsi_wait_vm_cmd_done_queue[i]);
		}
		if (status.SLEEPOUT_DONE) {
			wait_sleep_out_done = true;
			wake_up_interruptible(&_dsi_wait_sleep_out_done_queue[i]);
		}
	}
}

static enum DSI_STATUS DSI_Reset(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;

	if (!_dsi_is_video_mode(module)) {
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
			DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, DSI_START, 0);
	}

	/* do reset */
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[i]->DSI_COM_CTRL, DSI_RESET, 1);
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[i]->DSI_COM_CTRL, DSI_RESET, 0);
	}


	return DSI_STATUS_OK;
}

static int _dsi_is_video_mode(enum DISP_MODULE_ENUM module)
{
	int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (DSI_REG[i]->DSI_MODE_CTRL.MODE == CMD_MODE)
			return 0;
		else
			return 1;
	}
	return 0;
}

static enum DSI_STATUS DSI_SetMode(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, unsigned int mode)
{
	int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
		DSI_OUTREGBIT(cmdq, struct DSI_MODE_CTRL_REG, DSI_REG[i]->DSI_MODE_CTRL, MODE, mode);

	return DSI_STATUS_OK;
}

#if 0 /* defined but not used */
static enum DSI_STATUS DSI_SetVdoFrmMode(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, unsigned int mode)
{
	int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
		DSI_OUTREGBIT(cmdq, DSI_MODE_CTRL_REG, DSI_REG[i]->DSI_MODE_CTRL, FRM_MODE, mode);

	return DSI_STATUS_OK;
}

static enum DSI_STATUS DSI_SetSwitchMode(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, unsigned int mode)
{
	int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (mode == 0) {	/* V2C */
			DSI_OUTREGBIT(cmdq, DSI_MODE_CTRL_REG, DSI_REG[i]->DSI_MODE_CTRL,
				      V2C_SWITCH_ON, 1);
		} else		/* C2V */
			DSI_OUTREGBIT(cmdq, DSI_MODE_CTRL_REG, DSI_REG[i]->DSI_MODE_CTRL,
				      C2V_SWITCH_ON, 1);

	}

	return DSI_STATUS_OK;
}

static enum DSI_STATUS DSI_SetBypassRack(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq,
				    unsigned int bypass)
{
	int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (bypass == 0)
			DSI_OUTREGBIT(cmdq, DSI_RACK_REG, DSI_REG[i]->DSI_RACK, DSI_RACK_BYPASS, 0);
		else
			DSI_OUTREGBIT(cmdq, DSI_RACK_REG, DSI_REG[i]->DSI_RACK, DSI_RACK_BYPASS, 1);

	}

	return DSI_STATUS_OK;
}
#endif

void DSI_lane0_ULP_mode(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, bool enter)
{
	int i = 0;

	ASSERT(cmdq == NULL);

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (enter) {
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LD0CON_REG, DSI_REG[i]->DSI_PHY_LD0CON,
				      L0_RM_TRIG_EN, 0);
			mdelay(1);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LD0CON_REG, DSI_REG[i]->DSI_PHY_LD0CON,
				      Lx_ULPM_AS_L0, 1);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LD0CON_REG, DSI_REG[i]->DSI_PHY_LD0CON,
				      L0_ULPM_EN, 0);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LD0CON_REG, DSI_REG[i]->DSI_PHY_LD0CON,
				      L0_ULPM_EN, 1);
			mdelay(1);
		} else {
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LD0CON_REG, DSI_REG[i]->DSI_PHY_LD0CON,
				      L0_ULPM_EN, 0);
			mdelay(1);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LD0CON_REG, DSI_REG[i]->DSI_PHY_LD0CON,
				      Lx_ULPM_AS_L0, 0);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LD0CON_REG, DSI_REG[i]->DSI_PHY_LD0CON,
				      L0_WAKEUP_EN, 1);
			mdelay(1);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LD0CON_REG, DSI_REG[i]->DSI_PHY_LD0CON,
				      L0_WAKEUP_EN, 0);
			mdelay(1);
		}
	}
}


void DSI_clk_ULP_mode(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, bool enter)
{
	int i = 0;

	ASSERT(cmdq == NULL);

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (enter) {
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LCCON_REG, DSI_REG[i]->DSI_PHY_LCCON,
				      LC_ULPM_EN, 0);
			mdelay(1);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LCCON_REG, DSI_REG[i]->DSI_PHY_LCCON,
				      LC_ULPM_EN, 1);
			mdelay(1);
		} else {
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LCCON_REG, DSI_REG[i]->DSI_PHY_LCCON,
				      LC_ULPM_EN, 0);
			mdelay(1);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LCCON_REG, DSI_REG[i]->DSI_PHY_LCCON,
				      LC_WAKEUP_EN, 1);
			mdelay(1);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LCCON_REG, DSI_REG[i]->DSI_PHY_LCCON,
				      LC_WAKEUP_EN, 0);
			mdelay(1);
		}
	}
}

bool DSI_clk_HS_state(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;
	struct DSI_PHY_LCCON_REG tmpreg;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_READREG32(struct DSI_PHY_LCCON_REG *, &tmpreg, &DSI_REG[i]->DSI_PHY_LCCON);
		return tmpreg.LC_HS_TX_EN ? TRUE : FALSE;
	}
	return FALSE;
}

void DSI_clk_HS_mode(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, bool enter)
{
	int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (enter) {
			/* Olympus must set this */
			DSI_OUTREG32(NULL, &DSI_REG[i]->DSI_PHY_PCPAT, 0x55);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_LCCON_REG, DSI_REG[i]->DSI_PHY_LCCON,
					EARLY_HS_POE, 0);
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LCCON_REG, DSI_REG[i]->DSI_PHY_LCCON,
				      LC_HS_TX_EN, 1);
		} else if (!enter) {
			DSI_OUTREGBIT(cmdq, struct DSI_PHY_LCCON_REG, DSI_REG[i]->DSI_PHY_LCCON,
				      LC_HS_TX_EN, 0);
		}
	}

}

int DSI_WaitVMDone(enum DISP_MODULE_ENUM module)
{
	int i = 0;
	static const long WAIT_TIMEOUT = 2 * HZ;	/* 2 sec */
	int ret = 0;

	/*...dsi video is always in busy state... */
	if (_dsi_is_video_mode(module)) {
		DDPMSG("DSI_WaitVMDone error: should set DSI to CMD mode firstly\n");
		return -1;
	}

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		ret =
		    wait_event_interruptible_timeout(_dsi_wait_vm_done_queue[i],
						     !(DSI_REG[i]->DSI_INTSTA.BUSY), WAIT_TIMEOUT);
		if (ret == 0) {
			DISPERR("dsi wait VM done  timeout\n");
			DSI_DumpRegisters(module, 1);
			DSI_Reset(module, NULL);
			return -1;
		}
	}
	return 0;
}

static void DSI_WaitForNotBusy(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;
	unsigned int tmp = 0;
	unsigned int count = 0;
#if !defined(MTK_NO_DISP_IN_LK)
	static const long WAIT_TIMEOUT = 2 * HZ;	/* 2 sec */
	int ret = 0;
#endif

	if (cmdq) {
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
			DSI_POLLREG32(cmdq, &DSI_REG[i]->DSI_INTSTA, 0x80000000, 0x0);
		return;
	}


	/*...dsi video is always in busy state... */
	if (_dsi_is_video_mode(module))
		return;
#if defined(MTK_NO_DISP_IN_LK)
	i = DSI_MODULE_BEGIN(module);
	while (1) {
		tmp = INREG32(&DSI_REG[i]->DSI_INTSTA);
		if (!(tmp & 0x80000000))
			break;

		/* if(count %1000) */
		/* DISPMSG("dsi state:0x%08x, 0x%08x\n", tmp, INREG32(&DSI_REG[i]->DSI_STATE_DBG6)); */

		/* msleep(1); */

		if (count++ > 1000000000) {
			DISPERR("dsi%d wait for not busy timeout\n", i);
			DSI_DumpRegisters(module, 1);
			DSI_Reset(module, NULL);
			break;
		}
	}
#else
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (i == 0) {
			ret = wait_event_interruptible_timeout(_dsi_cmd_done_wait_queue[i],
							       !(DSI_REG[i]->DSI_INTSTA.BUSY), WAIT_TIMEOUT);
			if (ret == 0) {
				DISPERR("dsi%d wait event for not busy timeout\n", i);
				DSI_DumpRegisters(module, 1);
				DSI_Reset(module, NULL);
			} else if (ret < 0) {
				DISPERR("dsi%d wait_event is interrupted, %d\n", i, ret);
			}
		}
	}

	/* make sure DSI not busy in case wait_event is interrupted */
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		count = 0;
		while (1) {
			tmp = INREG32(&DSI_REG[i]->DSI_INTSTA);
			if (!(tmp & 0x80000000))
				break;

			/* if(count %1000) */
			/* DISPMSG("dsi state:0x%08x, 0x%08x\n", tmp, INREG32(&DSI_REG[i]->DSI_STATE_DBG6)); */

			/* msleep(1); */

			if (count++ > 1000000000) {
				DISPERR("polling dsi%d wait for not busy timeout\n", i);
				DSI_DumpRegisters(module, 1);
				DSI_Reset(module, NULL);
				break;
			}
		}
		if (count)
			DISPDBG("%s:dsi%d polling busy bit %d times\n", __func__, i, count);
	}
#endif
}

enum DSI_STATUS DSI_SleepOut(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;
	/* wake_up_prd *1024*cycle time > 1ms */
	int wake_up_prd = (_dsi_context[i].dsi_params.PLL_CLOCK * 2 * 1000) / (1024 * 8) + 0x1;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_MODE_CTRL_REG, DSI_REG[i]->DSI_MODE_CTRL, SLEEP_MODE, 1);
		DSI_OUTREGBIT(cmdq, struct DSI_TIME_CON0_REG, DSI_REG[i]->DSI_TIME_CON0, UPLS_WAKEUP_PRD, wake_up_prd);
	}
	DISPINFO("DSI_SleepOut\n");
	return DSI_STATUS_OK;
}

enum DSI_STATUS DSI_Wakeup(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;
	int ret = 0;
	int cnt = 0;
	u32 tmp = 0;

	DISPINFO("DSI_Wakeup+\n");
	/* set wait_sleep_out_done to false first to avoid race condition */
	wait_sleep_out_done = false;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, SLEEPOUT_START, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, SLEEPOUT_START, 1);
	}

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		cnt = 0;
		if (i == 0) { /* kernel only listens DSI0's IRQ */
			do {
				cnt++;
				ret = wait_event_interruptible_timeout(_dsi_wait_sleep_out_done_queue[i],
								       wait_sleep_out_done, 2 * HZ);
				wait_sleep_out_done = false;
			} while (ret <= 0 && cnt <= 2);
		}
		if (ret == 0) {
			DISPERR("dsi%d wait_sleepout timeout\n", i);
			DSI_DumpRegisters(module, 2);
			DSI_Reset(module, NULL);
		} else if (ret < 0) {
			DISPERR("dsi%d wait_sleepout is woken up by signal ret %d\n", i, ret);
			mdelay(5);
		}
	}

	/* polling DSI1's sleepout_done and reset to 0 if success */
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (i == 1) {
			cnt = 0;
			while (1) {
				tmp = INREG32(&DSI_REG[i]->DSI_INTSTA);
				if (tmp & 0x40) {
					DSI_OUTREGBIT(cmdq, struct DSI_INT_STATUS_REG,
						      DSI_REG[i]->DSI_INTSTA, SLEEPOUT_DONE, 0);
					break;
				}

				if (cnt++ > 1000000000) {
					DISPERR("polling dsi%d sleepout timeout\n", i);
					DSI_DumpRegisters(module, 1);
					DSI_Reset(module, NULL);
					break;
				}
			}
			if (cnt)
				DISPDBG("%s:dsi%d polling sleepout bit %d times\n", __func__, i, cnt);
		}
	}

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, SLEEPOUT_START, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_MODE_CTRL_REG, DSI_REG[i]->DSI_MODE_CTRL, SLEEP_MODE, 0);
	}
	DISPINFO("DSI_Wakeup-\n");
	return DSI_STATUS_OK;
}

enum DSI_STATUS DSI_BackupRegisters(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;
	struct DSI_REGS *regs = NULL;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		regs = &(_dsi_context[i].regBackup);

		DSI_OUTREG32(cmdq, &regs->DSI_INTEN, AS_UINT32(&DSI_REG[i]->DSI_INTEN));
		DSI_OUTREG32(cmdq, &regs->DSI_MODE_CTRL, AS_UINT32(&DSI_REG[i]->DSI_MODE_CTRL));
		DSI_OUTREG32(cmdq, &regs->DSI_TXRX_CTRL, AS_UINT32(&DSI_REG[i]->DSI_TXRX_CTRL));
		DSI_OUTREG32(cmdq, &regs->DSI_PSCTRL, AS_UINT32(&DSI_REG[i]->DSI_PSCTRL));

		DSI_OUTREG32(cmdq, &regs->DSI_VSA_NL, AS_UINT32(&DSI_REG[i]->DSI_VSA_NL));
		DSI_OUTREG32(cmdq, &regs->DSI_VBP_NL, AS_UINT32(&DSI_REG[i]->DSI_VBP_NL));
		DSI_OUTREG32(cmdq, &regs->DSI_VFP_NL, AS_UINT32(&DSI_REG[i]->DSI_VFP_NL));
		DSI_OUTREG32(cmdq, &regs->DSI_VACT_NL, AS_UINT32(&DSI_REG[i]->DSI_VACT_NL));

		DSI_OUTREG32(cmdq, &regs->DSI_HSA_WC, AS_UINT32(&DSI_REG[i]->DSI_HSA_WC));
		DSI_OUTREG32(cmdq, &regs->DSI_HBP_WC, AS_UINT32(&DSI_REG[i]->DSI_HBP_WC));
		DSI_OUTREG32(cmdq, &regs->DSI_HFP_WC, AS_UINT32(&DSI_REG[i]->DSI_HFP_WC));
		DSI_OUTREG32(cmdq, &regs->DSI_BLLP_WC, AS_UINT32(&DSI_REG[i]->DSI_BLLP_WC));

		DSI_OUTREG32(cmdq, &regs->DSI_HSTX_CKL_WC, AS_UINT32(&DSI_REG[i]->DSI_HSTX_CKL_WC));
		DSI_OUTREG32(cmdq, &regs->DSI_MEM_CONTI, AS_UINT32(&DSI_REG[i]->DSI_MEM_CONTI));

		DSI_OUTREG32(cmdq, &regs->DSI_PHY_TIMECON0,
			     AS_UINT32(&DSI_REG[i]->DSI_PHY_TIMECON0));
		DSI_OUTREG32(cmdq, &regs->DSI_PHY_TIMECON1,
			     AS_UINT32(&DSI_REG[i]->DSI_PHY_TIMECON1));
		DSI_OUTREG32(cmdq, &regs->DSI_PHY_TIMECON2,
			     AS_UINT32(&DSI_REG[i]->DSI_PHY_TIMECON2));
		DSI_OUTREG32(cmdq, &regs->DSI_PHY_TIMECON3,
			     AS_UINT32(&DSI_REG[i]->DSI_PHY_TIMECON3));
		DSI_OUTREG32(cmdq, &regs->DSI_VM_CMD_CON, AS_UINT32(&DSI_REG[i]->DSI_VM_CMD_CON));
	}

	return DSI_STATUS_OK;
}

enum DSI_STATUS DSI_RestoreRegisters(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;
	struct DSI_REGS *regs = NULL;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		regs = &(_dsi_context[i].regBackup);

		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_INTEN, AS_UINT32(&regs->DSI_INTEN));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_MODE_CTRL, AS_UINT32(&regs->DSI_MODE_CTRL));
		/* can not restore lane_num here */
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_TXRX_CTRL,
			     AS_UINT32(&regs->DSI_TXRX_CTRL) & 0xFFFFFFC3);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PSCTRL, AS_UINT32(&regs->DSI_PSCTRL));

		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VSA_NL, AS_UINT32(&regs->DSI_VSA_NL));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VBP_NL, AS_UINT32(&regs->DSI_VBP_NL));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VFP_NL, AS_UINT32(&regs->DSI_VFP_NL));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VACT_NL, AS_UINT32(&regs->DSI_VACT_NL));

		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HSA_WC, AS_UINT32(&regs->DSI_HSA_WC));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HBP_WC, AS_UINT32(&regs->DSI_HBP_WC));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HFP_WC, AS_UINT32(&regs->DSI_HFP_WC));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_BLLP_WC, AS_UINT32(&regs->DSI_BLLP_WC));

		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HSTX_CKL_WC, AS_UINT32(&regs->DSI_HSTX_CKL_WC));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_MEM_CONTI, AS_UINT32(&regs->DSI_MEM_CONTI));

		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON0,
			     AS_UINT32(&regs->DSI_PHY_TIMECON0));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON1,
			     AS_UINT32(&regs->DSI_PHY_TIMECON1));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON2,
			     AS_UINT32(&regs->DSI_PHY_TIMECON2));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON3,
			     AS_UINT32(&regs->DSI_PHY_TIMECON3));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VM_CMD_CON, AS_UINT32(&regs->DSI_VM_CMD_CON));
		DDPMSG("DSI_RestoreRegisters VM_CMD_EN %d TS_VFP_EN %d\n",
		       regs->DSI_VM_CMD_CON.VM_CMD_EN, regs->DSI_VM_CMD_CON.TS_VFP_EN);
	}
	return DSI_STATUS_OK;
}

enum DSI_STATUS DSI_BIST_Pattern_Test(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, bool enable,
				 unsigned int color)
{
	int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (enable) {
			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_BIST_PATTERN, color);
			/* DSI_OUTREG32(&DSI_REG->DSI_BIST_CON, AS_UINT32(&temp_reg)); */
			/* DSI_OUTREGBIT(DSI_BIST_CON_REG, DSI_REG->DSI_BIST_CON, SELF_PAT_MODE, 1); */
			DSI_OUTREGBIT(cmdq, struct DSI_BIST_CON_REG, DSI_REG[i]->DSI_BIST_CON,
				      SELF_PAT_MODE, 1);

			if (!_dsi_is_video_mode(module)) {
				struct DSI_T0_INS t0;

				t0.CONFG = 0x09;
				t0.Data_ID = 0x39;
				t0.Data0 = 0x2c;
				t0.Data1 = 0;

				DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[i]->data[0], AS_UINT32(&t0));
				DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_CMDQ_SIZE, 1);

				/* DSI_OUTREGBIT(struct DSI_START_REG,DSI_REG->DSI_START,DSI_START,0); */
				DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_START, 0);
				DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_START, 1);
				/* DSI_OUTREGBIT(struct DSI_START_REG,DSI_REG->DSI_START,DSI_START,1); */
			}
		} else {
			/* if disable dsi pattern, need enable mutex, can't just start dsi */
			/* so we just disable pattern bit, do not start dsi here */
			/* DSI_WaitForNotBusy(module,cmdq); */
			/* DSI_OUTREGBIT(cmdq, DSI_BIST_CON_REG, DSI_REG[i]->DSI_BIST_CON, SELF_PAT_MODE, 0); */
			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_BIST_CON, 0x00);
		}

	}
	return DSI_STATUS_OK;
}

int ddp_dsi_porch_setting(enum DISP_MODULE_ENUM module, void *handle,
		enum DSI_PORCH_TYPE type, unsigned int value)
{
	int i, ret = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (type == DSI_VFP) {
			DISPINFO("set dsi%d vfp to %d\n", i, value);
			DSI_OUTREG32(handle, &DSI_REG[i]->DSI_VFP_NL, value);
		}
		if (type == DSI_VSA) {
			DISPINFO("set dsi%d vsa to %d\n", i, value);
			DSI_OUTREG32(handle, &DSI_REG[i]->DSI_VSA_NL, value);
		}
		if (type == DSI_VBP) {
			DISPINFO("set dsi%d vbp to %d\n", i, value);
			DSI_OUTREG32(handle, &DSI_REG[i]->DSI_VBP_NL, value);
		}
		if (type == DSI_VACT) {
			DISPINFO("set dsi%d vact to %d\n", i, value);
			DSI_OUTREG32(handle, &DSI_REG[i]->DSI_VACT_NL, value);
		}
		if (type == DSI_HFP) {
			DISPINFO("set dsi%d hfp to %d\n", i, value);
			DSI_OUTREG32(handle, &DSI_REG[i]->DSI_HFP_WC, value);
		}
		if (type == DSI_HSA) {
			DISPINFO("set dsi%d hsa to %d\n", i, value);
			DSI_OUTREG32(handle, &DSI_REG[i]->DSI_HSA_WC, value);
		}
		if (type == DSI_HBP) {
			DISPINFO("set dsi%d hbp to %d\n", i, value);
			DSI_OUTREG32(handle, &DSI_REG[i]->DSI_HBP_WC, value);
		}
		if (type == DSI_BLLP) {
			DISPINFO("set dsi%d bllp to %d\n", i, value);
			DSI_OUTREG32(handle, &DSI_REG[i]->DSI_BLLP_WC, value);
		}
	}
	return ret;
}

static void DSI_Get_Porch_Addr(enum DISP_MODULE_ENUM module, unsigned long *pAddr)
{
	int i = 0;
	unsigned long porch_addr = 0;
	enum DSI_PORCH_TYPE porch_type = DSI_VFP;

	if (pAddr == NULL) {
		DISPERR("%s, NULL pointer !\n", __func__);
		return;
	}

	porch_type = (enum DSI_PORCH_TYPE)(*pAddr);
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (porch_type == DSI_VFP) {
			porch_addr = (unsigned long)(&DSI_REG[i]->DSI_VFP_NL);
			DISPINFO("get dsi%d vfp addr_va:%ld\n", i, porch_addr);
		}

		if (porch_addr)
			pAddr[i] = disp_addr_convert(porch_addr);
	}
}

void DSI_Config_VDO_Timing(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, LCM_DSI_PARAMS *dsi_params)
{
	int i = 0;
	unsigned int line_byte;
	unsigned int horizontal_sync_active_byte;
	unsigned int horizontal_backporch_byte;
	unsigned int horizontal_frontporch_byte;
	unsigned int horizontal_bllp_byte;
	unsigned int dsiTmpBufBpp;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (dsi_params->data_format.format == LCM_DSI_FORMAT_RGB565)
			dsiTmpBufBpp = 2;
		else
			dsiTmpBufBpp = 3;

		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VSA_NL, dsi_params->vertical_sync_active);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VBP_NL, dsi_params->vertical_backporch);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VFP_NL, dsi_params->vertical_frontporch);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_VACT_NL, dsi_params->vertical_active_line);

		line_byte =
		    (dsi_params->horizontal_sync_active + dsi_params->horizontal_backporch +
		     dsi_params->horizontal_frontporch +
		     dsi_params->horizontal_active_pixel) * dsiTmpBufBpp;
		horizontal_sync_active_byte =
		    (dsi_params->horizontal_sync_active * dsiTmpBufBpp - 4);

		if (dsi_params->mode == SYNC_EVENT_VDO_MODE || dsi_params->mode == BURST_VDO_MODE
		    || dsi_params->switch_mode == SYNC_EVENT_VDO_MODE
		    || dsi_params->switch_mode == BURST_VDO_MODE) {
			ASSERT((dsi_params->horizontal_backporch +
				dsi_params->horizontal_sync_active) * dsiTmpBufBpp > 9);
			horizontal_backporch_byte =
			    ((dsi_params->horizontal_backporch +
			      dsi_params->horizontal_sync_active) * dsiTmpBufBpp - 10);
		} else {
			ASSERT(dsi_params->horizontal_sync_active * dsiTmpBufBpp > 9);
			horizontal_sync_active_byte =
			    (dsi_params->horizontal_sync_active * dsiTmpBufBpp - 10);

			ASSERT(dsi_params->horizontal_backporch * dsiTmpBufBpp > 9);
			horizontal_backporch_byte =
			    (dsi_params->horizontal_backporch * dsiTmpBufBpp - 10);
		}

		ASSERT(dsi_params->horizontal_frontporch * dsiTmpBufBpp > 11);
		horizontal_frontporch_byte =
		    (dsi_params->horizontal_frontporch * dsiTmpBufBpp - 12);
		horizontal_bllp_byte = (dsi_params->horizontal_bllp * dsiTmpBufBpp);

		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HSA_WC,
			     ALIGN_TO((horizontal_sync_active_byte), 4));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HBP_WC,
			     ALIGN_TO((horizontal_backporch_byte), 4));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HFP_WC,
			     ALIGN_TO((horizontal_frontporch_byte), 4));
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_BLLP_WC, ALIGN_TO((horizontal_bllp_byte), 4));
	}
}

void DSI_Set_LFR(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, unsigned int mode,
		 unsigned int type, unsigned int enable, unsigned int skip_num)
{
	/* LFR_MODE 0 disable,1 static mode ,2 dynamic mode 3,both */
	unsigned int i = 0;
	/* DISPMSG("module=%d,mode=%d,type=%d,enable=%d,skip_num=%d\n",module,mode,type,enable,skip_num); */
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_LFR_CON_REG, DSI_REG[i]->DSI_LFR_CON, LFR_MODE, mode);
		DSI_OUTREGBIT(cmdq, struct DSI_LFR_CON_REG, DSI_REG[i]->DSI_LFR_CON, LFR_TYPE, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_LFR_CON_REG, DSI_REG[i]->DSI_LFR_CON, LFR_UPDATE, 1);
		DSI_OUTREGBIT(cmdq, struct DSI_LFR_CON_REG, DSI_REG[i]->DSI_LFR_CON, LFR_VSE_DIS, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_LFR_CON_REG, DSI_REG[i]->DSI_LFR_CON, LFR_SKIP_NUM,
			      skip_num);
		DSI_OUTREGBIT(cmdq, struct DSI_LFR_CON_REG, DSI_REG[i]->DSI_LFR_CON, LFR_EN, enable);
	}
}

void DSI_LFR_UPDATE(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	unsigned int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_LFR_CON_REG, DSI_REG[i]->DSI_LFR_CON, LFR_UPDATE, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_LFR_CON_REG, DSI_REG[i]->DSI_LFR_CON, LFR_UPDATE, 1);
	}
}

int DSI_LFR_Status_Check(void)
{
	unsigned int status = 0;

	struct DSI_LFR_STA_REG lfr_skip_sta;

	lfr_skip_sta = DSI_REG[0]->DSI_LFR_STA;
	status = lfr_skip_sta.LFR_SKIP_STA;
	DISPCHECK("LFR_SKIP_CNT 0x%x LFR_SKIP_STA 0x%x,status 0x%x\n", lfr_skip_sta.LFR_SKIP_CNT,
		  lfr_skip_sta.LFR_SKIP_STA, status);

	return status;
}

int _dsi_ps_type_to_bpp(LCM_PS_TYPE ps)
{
	switch (ps) {
	case LCM_PACKED_PS_16BIT_RGB565:
		return 2;
	case LCM_LOOSELY_PS_18BIT_RGB666:
		return 3;
	case LCM_PACKED_PS_24BIT_RGB888:
		return 3;
	case LCM_PACKED_PS_18BIT_RGB666:
		return 3;
	}
	return 0;
}

enum DSI_STATUS DSI_PS_Control(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, LCM_DSI_PARAMS *dsi_params,
			  int w, int h)
{
	int i = 0;
	unsigned int ps_sel_bitvalue = 0;
	unsigned int ps_wc_adjust = 0;
	unsigned int ps_wc = 0;

	/* /TODO: parameter checking */
	ASSERT((int)(dsi_params->PS) <= (int)PACKED_PS_18BIT_RGB666);

	if ((int)(dsi_params->PS) > (int)(LOOSELY_PS_18BIT_RGB666))
		ps_sel_bitvalue = (5 - dsi_params->PS);
	else
		ps_sel_bitvalue = dsi_params->PS;

	if (module == DISP_MODULE_DSIDUAL)
		w = w / 2;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_VACT_NL_REG, DSI_REG[i]->DSI_VACT_NL, VACT_NL, h);
		if (dsi_params->ufoe_enable && dsi_params->ufoe_params.lr_mode_en != 1) {
			if (dsi_params->ufoe_params.compress_ratio == 3) {
				unsigned int ufoe_internal_width = w + w % 4;

				if (ufoe_internal_width % 3 == 0) {
					ps_wc = (ufoe_internal_width / 3) * _dsi_ps_type_to_bpp(dsi_params->PS);
				} else {
					unsigned int temp_w = ufoe_internal_width / 3 + 1;

					temp_w = ((temp_w % 2) == 1) ? (temp_w + 1) : temp_w;
					ps_wc = temp_w  * _dsi_ps_type_to_bpp(dsi_params->PS);
				}
			} else { /* 1/2 */
				ps_wc = (w + w % 4) / 2 * _dsi_ps_type_to_bpp(dsi_params->PS);
			}
		} else if (dsi_params->dsc_enable) {
			ps_wc = dsi_params->word_count;
		} else {
			ps_wc = w * _dsi_ps_type_to_bpp(dsi_params->PS);
		}

		if (ps_wc_adjust)
			ps_wc *= dsi_params->packet_size_mult;

		DSI_OUTREGBIT(cmdq, struct DSI_PSCTRL_REG, DSI_REG[i]->DSI_PSCTRL, DSI_PS_WC, ps_wc);
		DSI_OUTREGBIT(cmdq, struct DSI_PSCTRL_REG, DSI_REG[i]->DSI_PSCTRL, DSI_PS_SEL,
			      ps_sel_bitvalue);
	}

	return DSI_STATUS_OK;
}

enum DSI_STATUS DSI_TXRX_Control(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq,
			    LCM_DSI_PARAMS *dsi_params)
{
	int i = 0;
	unsigned int lane_num_bitvalue = 0;
	int lane_num = dsi_params->LANE_NUM;
	int vc_num = 0;
	bool null_packet_en = FALSE;
	bool dis_eotp_en = FALSE;
	bool hstx_cklp_en = dsi_params->cont_clock ? FALSE : TRUE;
	int max_return_size = 0;

	switch (lane_num) {
	case LCM_ONE_LANE:
		lane_num_bitvalue = 0x1;
		break;
	case LCM_TWO_LANE:
		lane_num_bitvalue = 0x3;
		break;
	case LCM_THREE_LANE:
		lane_num_bitvalue = 0x7;
		break;
	case LCM_FOUR_LANE:
		lane_num_bitvalue = 0xF;
		break;
	}

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, VC_NUM, vc_num);
		DSI_OUTREGBIT(cmdq, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, DIS_EOT,
			      dis_eotp_en);
		DSI_OUTREGBIT(cmdq, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, BLLP_EN,
			      null_packet_en);
		DSI_OUTREGBIT(cmdq, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, MAX_RTN_SIZE,
			      max_return_size);
		DSI_OUTREGBIT(cmdq, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, HSTX_CKLP_EN,
			      hstx_cklp_en);
		DSI_OUTREGBIT(cmdq, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, LANE_NUM,
			      lane_num_bitvalue);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_MEM_CONTI, DSI_WMEM_CONTI);
		if (dsi_params->mode == CMD_MODE
		    || (dsi_params->mode != CMD_MODE && dsi_params->eint_disable)) {
			if (dsi_params->ext_te_edge == LCM_POLARITY_FALLING) {
				/*use ext te falling edge */
				DSI_OUTREGBIT(cmdq, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL,
					      EXT_TE_EDGE, 1);
			}
			DSI_OUTREGBIT(cmdq, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, EXT_TE_EN,
				      1);
		}
	}
	return DSI_STATUS_OK;
}

int MIPITX_IsEnabled(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;
	int ret = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0.RG_DSI0_MPPLL_PLL_EN)
			ret++;
	}

	DISPDBG("MIPITX for %s is %s\n", ddp_get_module_name(module), ret ? "on" : "off");
	return ret;
}

unsigned int dsi_phy_get_clk(enum DISP_MODULE_ENUM module)
{
	int i = 0;
	int j = 0;
	unsigned int pcw = (DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON2.RG_DSI_MPPLL_SDM_PCW >> 24) & 0x7f;
	unsigned int prediv = (1 << (DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0.RG_DSI0_MPPLL_PREDIV));
	unsigned int posdiv = (1 << (DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0.RG_DSI0_MPPLL_POSDIV));
	unsigned int S2Qdiv = (1 << (DSI_PHY_REG[i]->MIPITX_DSI_PLL_TOP.RG_MPPLL_S2QDIV));

	DISPERR("%s, pcw: %d, prediv: %d, posdiv: %d, S2Qdiv: %d\n", __func__, pcw,
		prediv, posdiv, S2Qdiv);
	j = prediv * posdiv * S2Qdiv;
	if (j > 0)
		return 26 * pcw / j;

	return 0;
}

void DSI_PHY_clk_change(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, LCM_DSI_PARAMS *dsi_params)
{
	int i = 0;
	unsigned int j = 0;
	unsigned int data_Rate = dsi_params->PLL_CLOCK*2;

	unsigned int pcw_ratio = 0;
	unsigned int S2Qdiv    = 0;
	unsigned int posdiv    = 0;
	unsigned int prediv    = 0;

	unsigned int pcw = 0;
	unsigned int delta1 = 2;
	/*Delta1 is SSC range, default is 0%~-5%*/
	unsigned int pdelta1 = 0;

	DISPFUNC();
	DISPCHECK("New mipitx Data Rate=%d\n", data_Rate);

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (data_Rate != 0) {
			unsigned int tmp = 0;

			if (data_Rate > 1500) {
				DISPERR("mipitx Data Rate exceed limitation(%d)\n", data_Rate);
				ASSERT(0);
			} else if (data_Rate >= 850) {
				pcw_ratio = 1;
				S2Qdiv    = 2;
				posdiv    = 0;
				prediv    = 0;
			} else if (data_Rate >= 425) {
				pcw_ratio = 2;
				S2Qdiv    = 2;
				posdiv    = 1;
				prediv    = 0;
			} else if (data_Rate >= 212) {
				pcw_ratio = 4;
				S2Qdiv    = 2;
				posdiv    = 2;
				prediv    = 0;
			} else if (data_Rate > 106) {
				pcw_ratio = 8;
				S2Qdiv    = 2;
				posdiv    = 3;
				prediv    = 0;
			} else if (data_Rate >= 47) {
				pcw_ratio = 16;
				S2Qdiv    = 2;
				posdiv    = 4;
				prediv    = 0;
			} else {
				DISPERR("dataRate is too low(%d)\n", data_Rate);
				ASSERT(0);
			}

			/* step 8 */
			DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_TOP_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_TOP,
						RG_MPPLL_S2QDIV, S2Qdiv);
			DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0,
						RG_DSI0_MPPLL_POSDIV, posdiv);
			DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0,
						RG_DSI0_MPPLL_PREDIV, prediv);
			/* 2. PLL PCW Config */

			/*PCW bit 24~30 = floor(pcw)*/
			/*PCW bit 16~23 = (pcw - floor(pcw))*256*/
			/*PCW bit 8~15 = (pcw*256 - floor(pcw)*256)*256*/
			/*PCW bit 8~15 = (pcw*256*256 - floor(pcw)*256*256)*256*/

			/*  pcw = (data_rate/2) * 2^(preDIV+postDIV+S2Q) / (26*2*(2^24))*/
			 /*  pcw_ration = 2^(preDIV+postDIV+S2Q-2) */
			pcw = data_Rate * pcw_ratio / 26;

			tmp = ((pcw & 0x7F) << 24) |
				(((256 * (data_Rate * pcw_ratio % 26) / 26) & 0xFF) << 16) |
				(((256 * (256 * (data_Rate * pcw_ratio % 26) % 26) / 26) & 0xFF) << 8) |
				((256 * (256 * (256 * (data_Rate * pcw_ratio % 26) % 26) % 26) / 26) & 0xFF);

			DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON2_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON2,
					 RG_DSI_MPPLL_SDM_PCW, tmp);

			/*3. SSC Config*/
			if (dsi_params->ssc_disable != 1) {
				DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
					      RG_DSI0_MPPLL_SDM_SSC_PH_INIT, 1);
				DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
					      RG_DSI0_MPPLL_SDM_SSC_PRD, 0x1B1);/*//PRD=ROUND(pmod) = 433;*/

				if (dsi_params->ssc_range != 0)
					delta1 = dsi_params->ssc_range;

				ASSERT(delta1 <= 8);
				pdelta1 = (delta1 * data_Rate * pcw_ratio * 262144 + 281664) / 563329;
				DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON3_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON3,
					      RG_DSI0_MPPLL_SDM_SSC_DELTA, pdelta1);
				DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON3_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON3,
					      RG_DSI0_MPPLL_SDM_SSC_DELTA1, pdelta1);
				DISPINFO("PLL config:data_rate=%d,pcw_ratio=%d,pcw=%d,delta1=%d,pdelta1=0x%x\n",
					data_Rate, pcw_ratio, DSI_INREG32(struct MIPITX_DSI_PLL_CON2_REG *,
					&DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON2), delta1, pdelta1);
			}
		} else {/*not use*/
			unsigned int tmp = 0;

			/*1. PLL TXDIV Config*/
			DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_TOP_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_TOP,
				      RG_MPPLL_S2QDIV, dsi_params->pll_s2qdiv);
			DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0,
				      RG_DSI0_MPPLL_POSDIV, dsi_params->pll_posdiv);
			DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0,
				      RG_DSI0_MPPLL_PREDIV, dsi_params->pll_prediv);
			/*2. PLL PCW Config*/
			tmp = ((dsi_params->fbk_div) << 2) << 24;

			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON2_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON2,
					 RG_DSI_MPPLL_SDM_PCW, tmp);

			/*3. SSC Config*/
			/*why no ssc config*/
		}

		DSI_OUTREGBIT(cmdq, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
				RG_DSI0_MPPLL_SDM_FRA_EN, 1);
		/*4. delay >30ns, need check, cmdq writewithmask 80ns *  9*/
			/*for(j = 0; j < 10; j++) */
			/*//DSI_OUTREG32(cmdq,&DSI_CMDQ_REG[0]->data[127],1);*/
			/*// 5. DSI_PLL_CHG = 0 */
		DSI_OUTREG32(cmdq, &DSI_PHY_REG[i]->MIPITX_DSI_PLL_CHG, 0);
		/*6. delay >20us , need check*/
		for (j = 0; j < 250; j++)
			DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[0]->data[126], 1);

		/*7. DSI_PLL_CHG = 1 */
		DSI_OUTREG32(cmdq, &DSI_PHY_REG[i]->MIPITX_DSI_PLL_CHG, 1);
	}
}

#if 0
void DSI_Enter_ULPS(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	DISPFUNC();
	ddp_dsi_power_off(module, cmdq);

}
void DSI_Exit_ULPS(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{

	DISPFUNC();
	ddp_dsi_power_on(module, cmdq);
	ddp_dsi_start(module, cmdq);
}
#endif

void DSI_PHY_clk_setting(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, LCM_DSI_PARAMS *dsi_params)
{
	int i = 0;
	unsigned int data_Rate = dsi_params->PLL_CLOCK * 2;

	unsigned int pcw_ratio = 0;
	unsigned int S2Qdiv    = 0;
	unsigned int posdiv    = 0;
	unsigned int prediv    = 0;

	unsigned int pcw = 0;
	/* unsigned int fmod = 30;//Fmod = 30KHz by default */
	unsigned int delta1 = 2; /* Delta1 is SSC range, default is 0%~-2% */
	unsigned int pdelta1 = 0;

	DISPFUNC();

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		/* step 0 MIPITX lane swap setting */
		if (dsi_params->lane_swap_en) {
			DISPINFO("MIPITX Lane Swap Enabled for DSI Port %d\n", i);
			DISPINFO("MIPITX Lane Swap mapping: %d|%d|%d|%d|%d|%d\n",
				dsi_params->lane_swap[i][MIPITX_PHY_LANE_0],
				dsi_params->lane_swap[i][MIPITX_PHY_LANE_1],
				dsi_params->lane_swap[i][MIPITX_PHY_LANE_2],
				dsi_params->lane_swap[i][MIPITX_PHY_LANE_3],
				dsi_params->lane_swap[i][MIPITX_PHY_LANE_CK],
				dsi_params->lane_swap[i][MIPITX_PHY_LANE_RX]);

			MIPITX_OUTREGBIT(struct MIPITX_DSI_PHY_SEL_REG, DSI_PHY_REG[i]->MIPITX_DSI_PHY_SEL,
					 MIPI_TX_PHY0_SEL, dsi_params->lane_swap[i][MIPITX_PHY_LANE_0]);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PHY_SEL_REG, DSI_PHY_REG[i]->MIPITX_DSI_PHY_SEL,
					 MIPI_TX_PHY1_SEL, dsi_params->lane_swap[i][MIPITX_PHY_LANE_1]);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PHY_SEL_REG, DSI_PHY_REG[i]->MIPITX_DSI_PHY_SEL,
					 MIPI_TX_PHY2_SEL, dsi_params->lane_swap[i][MIPITX_PHY_LANE_2]);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PHY_SEL_REG, DSI_PHY_REG[i]->MIPITX_DSI_PHY_SEL,
					 MIPI_TX_PHY3_SEL, dsi_params->lane_swap[i][MIPITX_PHY_LANE_3]);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PHY_SEL_REG, DSI_PHY_REG[i]->MIPITX_DSI_PHY_SEL,
					 MIPI_TX_PHYC_SEL, dsi_params->lane_swap[i][MIPITX_PHY_LANE_CK]);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PHY_SEL_REG, DSI_PHY_REG[i]->MIPITX_DSI_PHY_SEL,
					 MIPI_TX_LPRX_SEL, dsi_params->lane_swap[i][MIPITX_PHY_LANE_RX]);
		}
	}

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		/* step 0 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_CLOCK_LANE_REG, DSI_PHY_REG[i]->MIPITX_DSI_CLOCK_LANE,
				 RG_DSI_LNTC_RT_CODE, ((clock_lane[i] >> 8) & 0xf));
		MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE3_REG, DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE3,
				 RG_DSI_LNT3_RT_CODE, ((data_lane3[i] >> 8) & 0xf));
		MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE2_REG, DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE2,
				 RG_DSI_LNT2_RT_CODE, ((data_lane2[i] >> 8) & 0xf));
		MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE1_REG, DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE1,
				 RG_DSI_LNT1_RT_CODE, ((data_lane1[i] >> 8) & 0xf));
		MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE0_REG, DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE0,
				 RG_DSI_LNT0_RT_CODE, ((data_lane0[i] >> 8) & 0xf));

		DISPINFO("mipi_tx%d: clk=0x%x,lan0=0x%x,lan1=0x%x,lan2=0x%x,lan3=0x%x\n",
			i, INREG32(&DSI_PHY_REG[i]->MIPITX_DSI_CLOCK_LANE),
			INREG32(&DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE0),
			INREG32(&DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE1),
			INREG32(&DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE2),
			INREG32(&DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE3));

		/* step 1 */
		/* MIPITX_MASKREG32(APMIXED_BASE+0x00, (0x1<<6), 1); */

		/* step 2 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_BG_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_BG_CON,
				 RG_DSI_BG_CORE_EN, 1);
		MIPITX_OUTREGBIT(struct MIPITX_DSI_BG_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_BG_CON,
				 RG_DSI_BG_CKEN, 1);

		/* step 3 */
		mdelay(1);

		/* step 4 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_TOP_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_TOP_CON,
				 RG_DSI_LNT_HS_BIAS_EN, 1);

		/* step 5 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_CON,
				 RG_DSI_CKG_LDOOUT_EN, 1);
		MIPITX_OUTREGBIT(struct MIPITX_DSI_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_CON,
				 RG_DSI_LDOCORE_EN, 1);

		/* step 6 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_PWR_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_PWR,
				 DA_DSI_MPPLL_SDM_PWR_ON, 1);

		/* step 7 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_PWR_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_PWR,
				 DA_DSI_MPPLL_SDM_ISO_EN, 0);
		mdelay(1);

		if (data_Rate != 0) {
			unsigned int tmp = 0;

			if (data_Rate > 1500) {
				DISPERR("mipitx Data Rate exceed limitation(%d)\n", data_Rate);
				ASSERT(0);
			} else if (data_Rate >= 850) {
				pcw_ratio = 1;
				S2Qdiv    = 2;
				posdiv    = 0;
				prediv    = 0;
			} else if (data_Rate >= 425) {
				pcw_ratio = 2;
				S2Qdiv    = 2;
				posdiv    = 1;
				prediv    = 0;
			} else if (data_Rate >= 212) {
				pcw_ratio = 4;
				S2Qdiv    = 2;
				posdiv    = 2;
				prediv    = 0;
			} else if (data_Rate > 106) {
				pcw_ratio = 8;
				S2Qdiv    = 2;
				posdiv    = 3;
				prediv    = 0;
			} else if (data_Rate >= 47) {
				pcw_ratio = 16;
				S2Qdiv    = 2;
				posdiv    = 4;
				prediv    = 0;
			} else {
				DISPERR("dataRate is too low(%d)\n", data_Rate);
				ASSERT(0);
			}

			/* step 8 */
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_TOP_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_TOP,
						RG_MPPLL_S2QDIV, S2Qdiv);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0,
						RG_DSI0_MPPLL_POSDIV, posdiv);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0,
						RG_DSI0_MPPLL_PREDIV, prediv);

			/* step 10 */
			/* PLL PCW config */

			/*PCW bit 24~30 = floor(pcw)*/
			/*PCW bit 16~23 = (pcw - floor(pcw))*256*/
			/*PCW bit 8~15 = (pcw*256 - floor(pcw)*256)*256*/
			/*PCW bit 8~15 = (pcw*256*256 - floor(pcw)*256*256)*256*/

			/*  pcw = (data_rate/2) * 2^(preDIV+postDIV+S2Q) / (26*2*(2^24))*/
			 /*  pcw_ration = 2^(preDIV+postDIV+S2Q-2) */
			pcw = data_Rate * pcw_ratio / 26;

			tmp = ((pcw & 0x7F) << 24) |
				(((256 * (data_Rate * pcw_ratio % 26) / 26) & 0xFF) << 16) |
				(((256 * (256 * (data_Rate * pcw_ratio % 26) % 26) / 26) & 0xFF) << 8) |
				((256 * (256 * (256 * (data_Rate * pcw_ratio % 26) % 26) % 26) / 26) & 0xFF);

			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON2_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON2,
					 RG_DSI_MPPLL_SDM_PCW, tmp);

			if (dsi_params->ssc_disable != 1) {
				MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON1_REG,
						 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
						 RG_DSI0_MPPLL_SDM_SSC_PH_INIT, 1);
				MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
						 RG_DSI0_MPPLL_SDM_SSC_PRD, 0x1B1); /* PRD=ROUND(pmod) = 433; */
				if (dsi_params->ssc_range != 0)
					delta1 = dsi_params->ssc_range;
				ASSERT(delta1 <= 8);
				pdelta1 = (delta1 * data_Rate * pcw_ratio * 262144 + 281664) / 563329;
				MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON3_REG,
						 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON3,
						 RG_DSI0_MPPLL_SDM_SSC_DELTA, pdelta1);
				MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON3_REG,
						 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON3,
						 RG_DSI0_MPPLL_SDM_SSC_DELTA1, pdelta1);
				DISPINFO("PLL config:data_rate=%d,pcw_ratio=%d,pcw=%d,delta1=%d,pdelta1=0x%x\n",
					data_Rate, pcw_ratio, DSI_INREG32(struct MIPITX_DSI_PLL_CON2_REG *,
									  &DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON2),
					delta1, pdelta1);
			}
		} else {
			DISPERR("PLL clock should not be 0!!!\n");
			ASSERT(0);
		}

		MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
				 RG_DSI0_MPPLL_SDM_FRA_EN, 1);

		/* step 11 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_CLOCK_LANE_REG, DSI_PHY_REG[i]->MIPITX_DSI_CLOCK_LANE,
				 RG_DSI_LNTC_LDOOUT_EN, 1);

		if (dsi_params->lane_swap_en) {
			if (dsi_params->lane_swap[i][MIPITX_PHY_LANE_CK] == MIPITX_PHY_LANE_CK)
				MIPITX_OUTREGBIT(struct MIPITX_DSI_CLOCK_LANE_REG,
								 DSI_PHY_REG[i]->MIPITX_DSI_CLOCK_LANE,
								 RG_DSI_LNTC_CKLANE_EN, 1);
			else
				MIPITX_OUTREGBIT(struct MIPITX_DSI_CLOCK_LANE_REG,
								 DSI_PHY_REG[i]->MIPITX_DSI_CLOCK_LANE,
								 RG_DSI_LNTC_CKLANE_EN, 0);
		}

		/* step 12 */
		if (dsi_params->LANE_NUM > 0) {
			MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE0_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE0,
							 RG_DSI_LNT0_LDOOUT_EN, 1);
			if (dsi_params->lane_swap_en) {
				if (dsi_params->lane_swap[i][MIPITX_PHY_LANE_0] == MIPITX_PHY_LANE_CK)
					MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE0_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE0,
							 RG_DSI_LNT0_CKLANE_EN, 1);
				else
					MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE0_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE0,
							 RG_DSI_LNT0_CKLANE_EN, 0);
			}
		}
		/* step 13 */
		if (dsi_params->LANE_NUM > 1) {
			MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE1_REG, DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE1,
					 RG_DSI_LNT1_LDOOUT_EN, 1);
			if (dsi_params->lane_swap_en) {
				if (dsi_params->lane_swap[i][MIPITX_PHY_LANE_1] == MIPITX_PHY_LANE_CK)
					MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE1_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE1,
							 RG_DSI_LNT1_CKLANE_EN, 1);
				else
					MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE1_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE1,
							 RG_DSI_LNT1_CKLANE_EN, 0);
			}
		}
		/* step 14 */
		if (dsi_params->LANE_NUM > 2) {
			MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE2_REG, DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE2,
					 RG_DSI_LNT2_LDOOUT_EN, 1);
			if (dsi_params->lane_swap_en) {
				if (dsi_params->lane_swap[i][MIPITX_PHY_LANE_2] == MIPITX_PHY_LANE_CK)
					MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE2_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE2,
							 RG_DSI_LNT2_CKLANE_EN, 1);
				else
					MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE2_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE2,
							 RG_DSI_LNT2_CKLANE_EN, 0);
			}
		}
		/* step 15 */
		if (dsi_params->LANE_NUM > 3) {
			MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE3_REG, DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE3,
					 RG_DSI_LNT3_LDOOUT_EN, 1);
			if (dsi_params->lane_swap_en) {
				if (dsi_params->lane_swap[i][MIPITX_PHY_LANE_3] == MIPITX_PHY_LANE_CK)
					MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE3_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE3,
							 RG_DSI_LNT3_CKLANE_EN, 1);
				else
					MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE3_REG,
							 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE3,
							 RG_DSI_LNT3_CKLANE_EN, 0);
			}
		}

		/* step 16 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0,
				 RG_DSI0_MPPLL_PLL_EN, 1);

		/* step 17 */
		mdelay(1);

		MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CHG_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CHG,
				 RG_DSI0_MPPLL_SDM_PCW_CHG, 0);
		MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CHG_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CHG,
				 RG_DSI0_MPPLL_SDM_PCW_CHG, 1);

		if ((data_Rate != 0) && (dsi_params->ssc_disable != 1)) {
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON1_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
					 RG_DSI0_MPPLL_SDM_SSC_EN, 1);
		} else {
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON1_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
					 RG_DSI0_MPPLL_SDM_SSC_EN, 0);
		}

		/* step 18 */
		MIPITX_OUTREGBIT(struct MIPITX_DSI_TOP_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_TOP_CON,
				 RG_DSI_PAD_TIE_LOW_EN, 0);

		mdelay(1);
	}
}

void DSI_PHY_TIMCONFIG(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, LCM_DSI_PARAMS *dsi_params)
{
	struct DSI_PHY_TIMCON0_REG timcon0;
	struct DSI_PHY_TIMCON1_REG timcon1;
	struct DSI_PHY_TIMCON2_REG timcon2;
	struct DSI_PHY_TIMCON3_REG timcon3;
	int i = 0;
	unsigned int lane_no;
	unsigned int cycle_time = 0;
	unsigned int ui = 0;
	unsigned int hs_trail_m, hs_trail_n;
	unsigned char timcon_temp;

#ifdef CONFIG_FPGA_EARLY_PORTING
	return;
#endif

#if 0
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON0, 0x140f0708);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON1, 0x10280c20);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON2, 0x14280000);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON3, 0x00101a06);
		DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_PHY_TIMECON4, 0x00023000);
	}
	return;
#endif
	lane_no = dsi_params->LANE_NUM;
	if (dsi_params->PLL_CLOCK != 0) {
		ui = 1000 / (dsi_params->PLL_CLOCK * 2) + 0x01;
		cycle_time = 8000 / (dsi_params->PLL_CLOCK * 2) + 0x01;
		DISP_LOG_PRINT(ANDROID_LOG_INFO, "DSI",
			       "[DISP] - kernel - DSI_PHY_TIMCONFIG, Cycle Time = %d(ns), Unit Interval = %d(ns). , lane# = %d\n",
			       cycle_time, ui, lane_no);
	} else {
		DISPERR("[dsi_dsi.c] PLL clock should not be 0!!!\n");
		ASSERT(0);
	}

	/* div2_real=div2 ? div2*0x02 : 0x1; */
	/* cycle_time = (1000 * div2 * div1 * pre_div * post_div)/ (fbk_sel * (fbk_div+0x01) * 26) + 1; */
	/* ui = (1000 * div2 * div1 * pre_div * post_div)/ (fbk_sel * (fbk_div+0x01) * 26 * 2) + 1; */
#define NS_TO_CYCLE(n, c)	((n) / (c))

	hs_trail_m = 1;
	hs_trail_n =
	    (dsi_params->HS_TRAIL == 0) ? NS_TO_CYCLE(((hs_trail_m * 0x4 * ui) + 0x50),
						      cycle_time) : dsi_params->HS_TRAIL;
	/* +3 is recommended from designer becauase of HW latency */
	timcon0.HS_TRAIL = (hs_trail_m > hs_trail_n) ? hs_trail_m : hs_trail_n;

	timcon0.HS_PRPR =
	    (dsi_params->HS_PRPR == 0) ? NS_TO_CYCLE((0x40 + 0x5 * ui),
						     cycle_time) : dsi_params->HS_PRPR;
	/* HS_PRPR can't be 1. */
	if (timcon0.HS_PRPR < 1)
		timcon0.HS_PRPR = 1;

	timcon0.HS_ZERO =
	    (dsi_params->HS_ZERO == 0) ? NS_TO_CYCLE((0xC8 + 0x0a * ui),
		cycle_time) : dsi_params->HS_ZERO;
	timcon_temp = timcon0.HS_PRPR;
	if (timcon_temp < timcon0.HS_ZERO)
		timcon0.HS_ZERO -= timcon0.HS_PRPR;

	timcon0.LPX = (dsi_params->LPX == 0) ? NS_TO_CYCLE(0x50, cycle_time) : dsi_params->LPX;
	if (timcon0.LPX < 1)
		timcon0.LPX = 1;

	/* timcon1.TA_SACK         = (dsi_params->TA_SACK == 0) ? 1 : dsi_params->TA_SACK; */
	timcon1.TA_GET = (dsi_params->TA_GET == 0) ? (0x5 * timcon0.LPX) : dsi_params->TA_GET;
	timcon1.TA_SURE =
	    (dsi_params->TA_SURE == 0) ? (0x3 * timcon0.LPX / 0x2) : dsi_params->TA_SURE;
	timcon1.TA_GO = (dsi_params->TA_GO == 0) ? (0x4 * timcon0.LPX) : dsi_params->TA_GO;
	/* -------------------------------------------------------------- */
	/* NT35510 need fine tune timing */
	/* Data_hs_exit = 60 ns + 128UI */
	/* Clk_post = 60 ns + 128 UI. */
	/* -------------------------------------------------------------- */
	timcon1.DA_HS_EXIT =
	    (dsi_params->DA_HS_EXIT == 0) ? (0x2 * timcon0.LPX) : dsi_params->DA_HS_EXIT;

	timcon2.CLK_TRAIL =
	    ((dsi_params->CLK_TRAIL == 0) ? NS_TO_CYCLE(0x60,
							cycle_time) : dsi_params->CLK_TRAIL) + 0x01;
	/* CLK_TRAIL can't be 1. */
	if (timcon2.CLK_TRAIL < 2)
		timcon2.CLK_TRAIL = 2;

	/* timcon2.LPX_WAIT        = (dsi_params->LPX_WAIT == 0) ? 1 : dsi_params->LPX_WAIT; */
	timcon2.CONT_DET = dsi_params->CONT_DET;
	timcon2.CLK_ZERO =
	    (dsi_params->CLK_ZERO == 0) ? NS_TO_CYCLE(0x190, cycle_time) : dsi_params->CLK_ZERO;

	timcon3.CLK_HS_PRPR =
	    (dsi_params->CLK_HS_PRPR == 0) ? NS_TO_CYCLE(0x40,
							 cycle_time) : dsi_params->CLK_HS_PRPR;
	if (timcon3.CLK_HS_PRPR < 1)
		timcon3.CLK_HS_PRPR = 1;
	timcon3.CLK_HS_EXIT =
	    (dsi_params->CLK_HS_EXIT == 0) ? (0x2 * timcon0.LPX) : dsi_params->CLK_HS_EXIT;
	timcon3.CLK_HS_POST =
	    (dsi_params->CLK_HS_POST == 0) ? NS_TO_CYCLE((0x60 + 0x34 * ui),
							 cycle_time) : dsi_params->CLK_HS_POST;

	DISP_LOG_PRINT(ANDROID_LOG_INFO, "DSI",
		       "[DISP] - kernel - DSI_PHY_TIMCONFIG, HS_TRAIL = %d, HS_ZERO = %d, HS_PRPR = %d, LPX = %d, TA_GET = %d, TA_SURE = %d, TA_GO = %d, CLK_TRAIL = %d, CLK_ZERO = %d, CLK_HS_PRPR = %d\n",
		       timcon0.HS_TRAIL, timcon0.HS_ZERO, timcon0.HS_PRPR, timcon0.LPX,
		       timcon1.TA_GET, timcon1.TA_SURE, timcon1.TA_GO, timcon2.CLK_TRAIL,
		       timcon2.CLK_ZERO, timcon3.CLK_HS_PRPR);

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON0_REG, DSI_REG[i]->DSI_PHY_TIMECON0, LPX,
			      timcon0.LPX);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON0_REG, DSI_REG[i]->DSI_PHY_TIMECON0, HS_PRPR,
			      timcon0.HS_PRPR);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON0_REG, DSI_REG[i]->DSI_PHY_TIMECON0, HS_ZERO,
			      timcon0.HS_ZERO);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON0_REG, DSI_REG[i]->DSI_PHY_TIMECON0, HS_TRAIL,
			      timcon0.HS_TRAIL);

		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON1_REG, DSI_REG[i]->DSI_PHY_TIMECON1, TA_GO,
			      timcon1.TA_GO);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON1_REG, DSI_REG[i]->DSI_PHY_TIMECON1, TA_SURE,
			      timcon1.TA_SURE);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON1_REG, DSI_REG[i]->DSI_PHY_TIMECON1, TA_GET,
			      timcon1.TA_GET);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON1_REG, DSI_REG[i]->DSI_PHY_TIMECON1, DA_HS_EXIT,
			      timcon1.DA_HS_EXIT);

		DSI_OUTREGBIT(cmdq, struct  DSI_PHY_TIMCON2_REG, DSI_REG[i]->DSI_PHY_TIMECON2, CONT_DET,
			      timcon2.CONT_DET);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON2_REG, DSI_REG[i]->DSI_PHY_TIMECON2, CLK_ZERO,
			      timcon2.CLK_ZERO);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON2_REG, DSI_REG[i]->DSI_PHY_TIMECON2, CLK_TRAIL,
			      timcon2.CLK_TRAIL);

		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON3_REG, DSI_REG[i]->DSI_PHY_TIMECON3, CLK_HS_PRPR,
			      timcon3.CLK_HS_PRPR);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON3_REG, DSI_REG[i]->DSI_PHY_TIMECON3, CLK_HS_POST,
			      timcon3.CLK_HS_POST);
		DSI_OUTREGBIT(cmdq, struct DSI_PHY_TIMCON3_REG, DSI_REG[i]->DSI_PHY_TIMECON3, CLK_HS_EXIT,
			      timcon3.CLK_HS_EXIT);
		DISPINFO("%s, 0x%08x,0x%08x,0x%08x,0x%08x\n", __func__,
			  INREG32(&DSI_REG[i]->DSI_PHY_TIMECON0),
			  INREG32(&DSI_REG[i]->DSI_PHY_TIMECON1),
			  INREG32(&DSI_REG[i]->DSI_PHY_TIMECON2),
			  INREG32(&DSI_REG[i]->DSI_PHY_TIMECON3));
	}
}


void DSI_PHY_clk_switch(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, int on)
{
	int i = 0;

	/* can't use cmdq for this */
	ASSERT(cmdq == NULL);

	if (on) {
		DSI_PHY_clk_setting(module, cmdq, &(_dsi_context[i].dsi_params));
	} else {
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
			/* disable mipi clock */
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON0_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_PLL_EN,
					 0);
			mdelay(1);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_TOP_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_TOP_CON,
					 RG_DSI_PAD_TIE_LOW_EN, 1);


			MIPITX_OUTREGBIT(struct MIPITX_DSI_CLOCK_LANE_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_CLOCK_LANE,
					 RG_DSI_LNTC_LDOOUT_EN, 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE0_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE0,
					 RG_DSI_LNT0_LDOOUT_EN, 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE1_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE1,
					 RG_DSI_LNT1_LDOOUT_EN, 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE2_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE2,
					 RG_DSI_LNT2_LDOOUT_EN, 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_DATA_LANE3_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_DATA_LANE3,
					 RG_DSI_LNT3_LDOOUT_EN, 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_PWR_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_PWR,
					 DA_DSI_MPPLL_SDM_ISO_EN, 1);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_PWR_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_PWR,
					 DA_DSI_MPPLL_SDM_PWR_ON, 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_TOP_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_TOP_CON,
					 RG_DSI_LNT_HS_BIAS_EN, 0);

			MIPITX_OUTREGBIT(struct MIPITX_DSI_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_CON,
					 RG_DSI_CKG_LDOOUT_EN, 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_CON,
					 RG_DSI_LDOCORE_EN, 0);

			MIPITX_OUTREGBIT(struct MIPITX_DSI_BG_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_BG_CON,
					 RG_DSI_BG_CKEN, 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_BG_CON_REG, DSI_PHY_REG[i]->MIPITX_DSI_BG_CON,
					 RG_DSI_BG_CORE_EN, 0);

			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_TOP_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_PLL_TOP, RG_MPPLL_S2QDIV, 0);

			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON0_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_POSDIV,
					 0);

			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON0_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_PREDIV,
					 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON0_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV0,
					 0);
			MIPITX_OUTREGBIT(struct MIPITX_DSI_PLL_CON0_REG,
					 DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV1,
					 0);

			MIPITX_OUTREG32(&DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1, 0x00000000);
			MIPITX_OUTREG32(&DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON2, 0x50000000);
			mdelay(1);
		}
	}
}

enum DSI_STATUS DSI_EnableClk(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
#if 0
	DISPFUNC();
	int i = 0;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[i]->DSI_COM_CTRL, DSI_EN, 1);
#endif
	return DSI_STATUS_OK;
}

enum DSI_STATUS DSI_DisableClk(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
#if 0
	int i = 0;

	DISPFUNC();
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[i]->DSI_COM_CTRL, DSI_EN, 0);
#endif
	return DSI_STATUS_OK;
}

enum DSI_STATUS DSI_Start(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;

	if (module != DISP_MODULE_DSIDUAL) {
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
			DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, DSI_START, 0);
			DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, DSI_START, 1);
		}
	} else {
		/* TODO: do we need this? */
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[0]->DSI_START, DSI_START, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[0]->DSI_START, DSI_START, 1);
	}

	return DSI_STATUS_OK;
}

static int DSI_Stop(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i;

	if (module == DISP_MODULE_DSIDUAL) {
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[0]->DSI_COM_CTRL, DSI_DUAL_EN, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[1]->DSI_COM_CTRL, DSI_DUAL_EN, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[0]->DSI_START, DSI_START, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[1]->DSI_START, DSI_START, 0);
		/* DSI_OUTREG32(NULL, 0xF401A000, 4); */
	} else {
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
			DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, DSI_START, 0);
	}
	return 0;
}

void DSI_Set_VM_CMD(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{

	int i = 0;

	if (module != DISP_MODULE_DSIDUAL) {
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
			DSI_OUTREGBIT(cmdq, struct DSI_VM_CMD_CON_REG, DSI_REG[i]->DSI_VM_CMD_CON,
				      TS_VFP_EN, 1);
			DSI_OUTREGBIT(cmdq, struct DSI_VM_CMD_CON_REG, DSI_REG[i]->DSI_VM_CMD_CON,
				      VM_CMD_EN, 1);
			DDPMSG("DSI_Set_VM_CMD");
		}
	} else {
		DSI_OUTREGBIT(cmdq, struct DSI_VM_CMD_CON_REG, DSI_REG[i]->DSI_VM_CMD_CON, TS_VFP_EN, 1);
		DSI_OUTREGBIT(cmdq, struct DSI_VM_CMD_CON_REG, DSI_REG[i]->DSI_VM_CMD_CON, VM_CMD_EN, 1);
	}


}

enum DSI_STATUS DSI_EnableVM_CMD(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq)
{
	int i = 0;

	if (module != DISP_MODULE_DSIDUAL) {
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
			DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, VM_CMD_START, 0);
			DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[i]->DSI_START, VM_CMD_START, 1);
		}
	} else {
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[0]->DSI_START, VM_CMD_START, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[0]->DSI_START, VM_CMD_START, 1);
	}
	return DSI_STATUS_OK;
}

/* / return value: the data length we got */
UINT32 DSI_dcs_read_lcm_reg_v2(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, UINT8 cmd,
			       UINT8 *buffer, UINT8 buffer_size)
{
	int d = 0;
	UINT32 max_try_count = 5;
	UINT32 recv_data_cnt = 0;
	unsigned char packet_type;
	struct DSI_RX_DATA_REG read_data0;
	struct DSI_RX_DATA_REG read_data1;
	struct DSI_RX_DATA_REG read_data2;
	struct DSI_RX_DATA_REG read_data3;
	struct DSI_T0_INS t0;
	struct DSI_T0_INS t1;
	static const long WAIT_TIMEOUT = 2 * HZ;	/* 2 sec */
	long ret;
	int timeout = 0;

	for (d = DSI_MODULE_BEGIN(module); d <= DSI_MODULE_END(module); d++) {
		if (DSI_REG[d]->DSI_MODE_CTRL.MODE) {
			/* only support cmd mode read */
			DISPERR("DSI Read Fail: DSI Mode is %d\n",
				  DSI_REG[d]->DSI_MODE_CTRL.MODE);
			return 0;
		}

		if (buffer == NULL || buffer_size == 0) {
			/* illegal parameters */
			DISPERR("DSI Read Fail: buffer=%p and buffer_size=%d\n", buffer,
				  (unsigned int)buffer_size);
			return 0;
		}

		do {
			if (max_try_count == 0) {
				DISPERR("DSI Read Fail: try 5 times\n");
				return 0;
			}

			max_try_count--;
			recv_data_cnt = 0;
			/* read_timeout_ms = 20; */

			/* 1. wait dsi not busy => can't read if dsi busy */
			DSI_WaitForNotBusy(module, cmdq);

			/* 2. Check rd_rdy & cmd_done irq */
			if (DSI_REG[d]->DSI_INTEN.RD_RDY == 0) {
				DSI_OUTREGBIT(cmdq, struct DSI_INT_ENABLE_REG, DSI_REG[d]->DSI_INTEN,
					      RD_RDY, 1);

			}
			if (DSI_REG[d]->DSI_INTEN.CMD_DONE == 0) {
				DSI_OUTREGBIT(cmdq, struct DSI_INT_ENABLE_REG, DSI_REG[d]->DSI_INTEN,
					      CMD_DONE, 1);
			}
			if (DSI_REG[d]->DSI_INTSTA.RD_RDY != 0
			    || DSI_REG[d]->DSI_INTSTA.CMD_DONE != 0) {
				/* dump cmdq & rxdata */
				{
					unsigned int i;

					DISPCHECK("Last DSI Read Why not clear irq???\n");
					DISPCHECK("DSI_CMDQ_SIZE  : %d\n",
						  AS_UINT32(&DSI_REG[d]->DSI_CMDQ_SIZE));
					for (i = 0; i < DSI_REG[d]->DSI_CMDQ_SIZE.CMDQ_SIZE; i++) {
						DISPCHECK("DSI_CMDQ_DATA%d : 0x%08x\n", i,
							  AS_UINT32(&DSI_CMDQ_REG[d]->data[i]));
					}
					DISPCHECK("DSI_RX_DATA0   : 0x%08x\n",
						  AS_UINT32(&DSI_REG[d]->DSI_RX_DATA0));
					DISPCHECK("DSI_RX_DATA1   : 0x%08x\n",
						  AS_UINT32(&DSI_REG[d]->DSI_RX_DATA1));
					DISPCHECK("DSI_RX_DATA2   : 0x%08x\n",
						  AS_UINT32(&DSI_REG[d]->DSI_RX_DATA2));
					DISPCHECK("DSI_RX_DATA3   : 0x%08x\n",
						  AS_UINT32(&DSI_REG[d]->DSI_RX_DATA3));
				}
				/* clear irq */
				DSI_OUTREGBIT(cmdq, struct DSI_INT_STATUS_REG, DSI_REG[d]->DSI_INTSTA,
					      RD_RDY, 0);
				DSI_OUTREGBIT(cmdq, struct DSI_INT_STATUS_REG, DSI_REG[d]->DSI_INTSTA,
					      CMD_DONE, 0);
			}
			/* 3. Send cmd */
			t0.CONFG = 0x04;	/* /BTA */
			/* / 0xB0 is used to distinguish DCS cmd or Gerneric cmd, is that Right??? */
			t0.Data_ID =
			    (cmd <
			     0xB0) ? DSI_DCS_READ_PACKET_ID : DSI_GERNERIC_READ_LONG_PACKET_ID;
			t0.Data0 = cmd;
			t0.Data1 = 0;
			/* set max return size */
			t1.CONFG = 0x00;
			t1.Data_ID = 0x37;
			t1.Data0 = buffer_size <= 10 ? buffer_size : 10;
			t1.Data1 = 0;

			DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[d]->data[0], AS_UINT32(&t1));
			DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[d]->data[1], AS_UINT32(&t0));
			DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_CMDQ_SIZE, 2);

			DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_START, 0);
			DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_START, 1);

			/* / the following code is to */
			/* / 1: wait read ready */
			/* / 2: ack read ready */
			/* / 3: wait for CMDQ_DONE(interrupt handler do this op) */
			/* / 4: read data */
			ret =
			    wait_event_interruptible_timeout(_dsi_dcs_read_wait_queue[d],
							     waitRDDone, WAIT_TIMEOUT);
			waitRDDone = false;
			if (ret > 0) {
				do {
					timeout++;
					udelay(1);
					DSI_OUTREGBIT(cmdq, struct DSI_RACK_REG, DSI_REG[d]->DSI_RACK,
						      DSI_RACK, 1);
				} while (DSI_REG[d]->DSI_INTSTA.BUSY && (timeout < 1000));

				if (timeout == 1000) {
					/* wait cmd done timeout */
					DISPERR("DSI Read Fail: dsi wait cmd done timeout\n");
					DSI_DumpRegisters(module, 2);

					/* /do necessary reset here */
					DSI_OUTREGBIT(cmdq, struct DSI_RACK_REG, DSI_REG[d]->DSI_RACK,
						      DSI_RACK, 1);
					DSI_Reset(module, NULL);

					/* clear rd rdy interrupt */
					DSI_OUTREGBIT(cmdq, struct DSI_INT_ENABLE_REG,
						      DSI_REG[d]->DSI_INTSTA, RD_RDY, 0);
					return 0;
				}
			} else {
				if (ret == 0) {
					/* wait read ready timeout */
					DISPERR("DSI Read Fail: dsi wait read ready timeout\n");
					DSI_DumpRegisters(module, 2);

					/* /do necessary reset here */
					DSI_OUTREGBIT(cmdq, struct DSI_RACK_REG, DSI_REG[d]->DSI_RACK, DSI_RACK,
						      1);
					DSI_Reset(module, NULL);
				} else {
					/* wake up by other interrupt, need try again??? */
					DDPERR("DSI Read Fail: dsi wait read ready wake up by other interrupt\n");
					/* /do necessary reset here */
					DSI_OUTREGBIT(cmdq, struct DSI_RACK_REG, DSI_REG[d]->DSI_RACK, DSI_RACK,
						      1);
					DSI_Reset(module, NULL);
				}
				return 0;
			}


			/* clear interrupt */
			DSI_OUTREGBIT(cmdq, struct DSI_INT_ENABLE_REG, DSI_REG[d]->DSI_INTSTA, RD_RDY, 0);
			DSI_OUTREGBIT(cmdq, struct DSI_INT_ENABLE_REG, DSI_REG[d]->DSI_INTSTA, CMD_DONE,
				      0);

			/* read data */
			DSI_OUTREG32(cmdq, &read_data0, AS_UINT32(&DSI_REG[d]->DSI_RX_DATA0));
			DSI_OUTREG32(cmdq, &read_data1, AS_UINT32(&DSI_REG[d]->DSI_RX_DATA1));
			DSI_OUTREG32(cmdq, &read_data2, AS_UINT32(&DSI_REG[d]->DSI_RX_DATA2));
			DSI_OUTREG32(cmdq, &read_data3, AS_UINT32(&DSI_REG[d]->DSI_RX_DATA3));


			{
				unsigned int i;

				DISPCHECK("DSI read begin i = %d --------------------\n",
					  5 - max_try_count);
				DISPCHECK("DSI_RX_STA     : 0x%08x\n",
					  AS_UINT32(&DSI_REG[d]->DSI_TRIG_STA));
				DISPCHECK("DSI_CMDQ_SIZE  : %d\n",
					  AS_UINT32(&DSI_REG[d]->DSI_CMDQ_SIZE));
				for (i = 0; i < DSI_REG[d]->DSI_CMDQ_SIZE.CMDQ_SIZE; i++) {
					DISPCHECK("DSI_CMDQ_DATA%d : 0x%08x\n", i,
						  AS_UINT32(&DSI_CMDQ_REG[d]->data[i]));
				}
				DISPCHECK("DSI_RX_DATA0   : 0x%08x\n",
					  AS_UINT32(&DSI_REG[d]->DSI_RX_DATA0));
				DISPCHECK("DSI_RX_DATA1   : 0x%08x\n",
					  AS_UINT32(&DSI_REG[d]->DSI_RX_DATA1));
				DISPCHECK("DSI_RX_DATA2   : 0x%08x\n",
					  AS_UINT32(&DSI_REG[d]->DSI_RX_DATA2));
				DISPCHECK("DSI_RX_DATA3   : 0x%08x\n",
					  AS_UINT32(&DSI_REG[d]->DSI_RX_DATA3));
				DISPCHECK("DSI read end ----------------------------\n");
			}

			packet_type = read_data0.byte0;

			DISPCHECK("DSI read packet_type is 0x%x\n", packet_type);

			/* 0x02: acknowledge & error report */
			/* 0x11: generic short read response(1 byte return) */
			/* 0x12: generic short read response(2 byte return) */
			/* 0x1a: generic long read response */
			/* 0x1c: dcs long read response */
			/* 0x21: dcs short read response(1 byte return) */
			/* 0x22: dcs short read response(2 byte return) */
			if (packet_type == 0x1A || packet_type == 0x1C) {
				recv_data_cnt = read_data0.byte1 + read_data0.byte2 * 16;
				if (recv_data_cnt > 10) {
					DISPCHECK
					    ("DSI read long packet data exceeds 4 bytes return size: %d\n",
					     recv_data_cnt);
					recv_data_cnt = 10;
				}

				if (recv_data_cnt > buffer_size) {
					DISPCHECK
					    ("DSI read long packet data exceeds buffer size return size %d\n",
					     recv_data_cnt);
					recv_data_cnt = buffer_size;
				}
				DISPCHECK("DSI read long packet size: %d\n", recv_data_cnt);

				if (recv_data_cnt <= 4) {
					memcpy((void *)buffer, (void *)&read_data1, recv_data_cnt);
				} else if (recv_data_cnt <= 8) {
					memcpy((void *)buffer, (void *)&read_data1, 4);
					memcpy((void *)buffer + 4, (void *)&read_data2,
					       recv_data_cnt - 4);
				} else {
					memcpy((void *)buffer, (void *)&read_data1, 4);
					memcpy((void *)buffer + 4, (void *)&read_data2, 4);
					memcpy((void *)buffer + 8, (void *)&read_data2,
					       recv_data_cnt - 8);
				}
			} else if (packet_type == 0x11 || packet_type == 0x12 ||
					packet_type == 0x21 || packet_type == 0x22) {
				if (packet_type == 0x11 || packet_type == 0x21)
					recv_data_cnt = 1;
				else
					recv_data_cnt = 2;
				if (recv_data_cnt > buffer_size) {
					DISPCHECK
					    ("DSI read short packet data exceeds buffer size: %d\n",
					     buffer_size);
					recv_data_cnt = buffer_size;
					memcpy((void *)buffer, (void *)&read_data0.byte1,
					       recv_data_cnt);
				} else {
					memcpy((void *)buffer, (void *)&read_data0.byte1,
					       recv_data_cnt);
				}

			} else if (packet_type == 0x02) {
				DISPCHECK("read return type is 0x02, re-read\n");
			} else {
				DISPCHECK("read return type is non-recognite, type = 0x%x\n",
					  packet_type);
				return 0;
			}
		} while (packet_type == 0x02);
		/* / here: we may receive a ACK packet which packet type is 0x02 (incdicates some error happened) */
		/* / therefore we try re-read again until no ACK packet */
		/* / But: if it is a good way to keep re-trying ??? */
	}

	return recv_data_cnt;
}

void DSI_set_cmdq_V2(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, unsigned cmd, unsigned char count,
		     unsigned char *para_list, unsigned char force_update)
{
	UINT32 i = 0;
	int d = 0;
	unsigned long goto_addr, mask_para, set_para;
	struct DSI_T0_INS t0;
	struct DSI_T2_INS t2;

	memset(&t0, 0, sizeof(struct DSI_VM_CMD_CON_REG));
	memset(&t2, 0, sizeof(struct DSI_VM_CMD_CON_REG));
	/* DISPFUNC(); */
	for (d = DSI_MODULE_BEGIN(module); d <= DSI_MODULE_END(module); d++) {
		if (DSI_REG[d]->DSI_MODE_CTRL.MODE != 0) {	/* not in cmd mode */
			struct DSI_VM_CMD_CON_REG vm_cmdq;

			memset(&vm_cmdq, 0, sizeof(struct DSI_VM_CMD_CON_REG));
			DSI_READREG32(struct DSI_VM_CMD_CON_REG *, &vm_cmdq, &DSI_REG[d]->DSI_VM_CMD_CON);
			if (cmd < 0xB0) {
				if (count > 1) {
					vm_cmdq.LONG_PKT = 1;
					vm_cmdq.CM_DATA_ID = DSI_DCS_LONG_PACKET_ID;
					vm_cmdq.CM_DATA_0 = count + 1;
					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_VM_CMD_CON,
						     AS_UINT32(&vm_cmdq));

					goto_addr =
					    (unsigned long)(&DSI_VM_CMD_REG[d]->data[0].byte0);
					mask_para = (0xFF << ((goto_addr & 0x3) * 8));
					set_para = (cmd << ((goto_addr & 0x3) * 8));
					DSI_MASKREG32(cmdq, goto_addr & (~0x3), mask_para,
						      set_para);

					for (i = 0; i < count; i++) {
						goto_addr =
						    (unsigned long)(&DSI_VM_CMD_REG[d]->data[0].
								    byte1) + i;
						mask_para = (0xFF << ((goto_addr & 0x3) * 8));
						set_para =
						    (para_list[i] << ((goto_addr & 0x3) * 8));
						DSI_MASKREG32(cmdq, goto_addr & (~0x3), mask_para,
							      set_para);
					}
				} else {
					vm_cmdq.LONG_PKT = 0;
					vm_cmdq.CM_DATA_0 = cmd;
					if (count) {
						vm_cmdq.CM_DATA_ID = DSI_DCS_SHORT_PACKET_ID_1;
						vm_cmdq.CM_DATA_1 = para_list[0];
					} else {
						vm_cmdq.CM_DATA_ID = DSI_DCS_SHORT_PACKET_ID_0;
						vm_cmdq.CM_DATA_1 = 0;
					}
					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_VM_CMD_CON,
						     AS_UINT32(&vm_cmdq));
				}
			} else {
				if (count > 1) {
					vm_cmdq.LONG_PKT = 1;
					vm_cmdq.CM_DATA_ID = DSI_GERNERIC_LONG_PACKET_ID;
					vm_cmdq.CM_DATA_0 = count + 1;
					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_VM_CMD_CON,
						     AS_UINT32(&vm_cmdq));

					goto_addr =
					    (unsigned long)(&DSI_VM_CMD_REG[d]->data[0].byte0);
					mask_para = (0xFF << ((goto_addr & 0x3) * 8));
					set_para = (cmd << ((goto_addr & 0x3) * 8));
					DSI_MASKREG32(cmdq, goto_addr & (~0x3), mask_para,
						      set_para);

					for (i = 0; i < count; i++) {
						goto_addr =
						    (unsigned long)(&DSI_VM_CMD_REG[d]->data[0].
								    byte1) + i;
						mask_para = (0xFF << ((goto_addr & 0x3) * 8));
						set_para =
						    (para_list[i] << ((goto_addr & 0x3) * 8));
						DSI_MASKREG32(cmdq, goto_addr & (~0x3), mask_para,
							      set_para);
					}
				} else {
					vm_cmdq.LONG_PKT = 0;
					vm_cmdq.CM_DATA_0 = cmd;
					if (count) {
						vm_cmdq.CM_DATA_ID = DSI_GERNERIC_SHORT_PACKET_ID_2;
						vm_cmdq.CM_DATA_1 = para_list[0];
					} else {
						vm_cmdq.CM_DATA_ID = DSI_GERNERIC_SHORT_PACKET_ID_1;
						vm_cmdq.CM_DATA_1 = 0;
					}
					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_VM_CMD_CON,
						     AS_UINT32(&vm_cmdq));
				}
			}
		} else { /* cmd mode */
			DSI_WaitForNotBusy(module, cmdq);

			if (cmd < 0xB0) {
				if (count > 1) {
					t2.CONFG = 2;
					t2.Data_ID = DSI_DCS_LONG_PACKET_ID;
					t2.WC16 = count + 1;

					DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[d]->data[0],
						     AS_UINT32(&t2));

					goto_addr =
					    (unsigned long)(&DSI_CMDQ_REG[d]->data[1].byte0);
					mask_para = (0xFFu << ((goto_addr & 0x3u) * 8));
					set_para = (cmd << ((goto_addr & 0x3u) * 8));
					DSI_MASKREG32(cmdq, goto_addr & (~((unsigned long)0x3u)),
						      mask_para, set_para);

					for (i = 0; i < count; i++) {
						goto_addr =
						    (unsigned long)(&DSI_CMDQ_REG[d]->data[1].
								    byte1) + i;
						mask_para = (0xFFu << ((goto_addr & 0x3u) * 8));
						set_para =
						    (para_list[i] << ((goto_addr & 0x3u) * 8));
						DSI_MASKREG32(cmdq,
							      goto_addr & (~((unsigned long)0x3u)),
							      mask_para, set_para);
					}

					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_CMDQ_SIZE,
						     2 + (count) / 4);
				} else {
					t0.CONFG = 0;
					t0.Data0 = cmd;
					if (count) {
						t0.Data_ID = DSI_DCS_SHORT_PACKET_ID_1;
						t0.Data1 = para_list[0];
					} else {
						t0.Data_ID = DSI_DCS_SHORT_PACKET_ID_0;
						t0.Data1 = 0;
					}

					DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[d]->data[0],
						     AS_UINT32(&t0));
					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_CMDQ_SIZE, 1);
				}
			} else {
				if (count > 1) {
					t2.CONFG = 2;
					t2.Data_ID = DSI_GERNERIC_LONG_PACKET_ID;
					t2.WC16 = count + 1;

					DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[d]->data[0],
						     AS_UINT32(&t2));

					goto_addr =
					    (unsigned long)(&DSI_CMDQ_REG[d]->data[1].byte0);
					mask_para = (0xFFu << ((goto_addr & 0x3u) * 8));
					set_para = (cmd << ((goto_addr & 0x3u) * 8));
					DSI_MASKREG32(cmdq, goto_addr & (~((unsigned long)0x3u)),
						      mask_para, set_para);

					for (i = 0; i < count; i++) {
						goto_addr =
						    (unsigned long)(&DSI_CMDQ_REG[d]->data[1].
								    byte1) + i;
						mask_para = (0xFFu << ((goto_addr & 0x3u) * 8));
						set_para =
						    (para_list[i] << ((goto_addr & 0x3u) * 8));
						DSI_MASKREG32(cmdq,
							      goto_addr & (~((unsigned long)0x3u)),
							      mask_para, set_para);
					}

					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_CMDQ_SIZE,
						     2 + (count) / 4);

				} else {
					t0.CONFG = 0;
					t0.Data0 = cmd;
					if (count) {
						t0.Data_ID = DSI_GERNERIC_SHORT_PACKET_ID_2;
						t0.Data1 = para_list[0];
					} else {
						t0.Data_ID = DSI_GERNERIC_SHORT_PACKET_ID_1;
						t0.Data1 = 0;
					}
					DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[d]->data[0],
						     AS_UINT32(&t0));
					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_CMDQ_SIZE, 1);
				}
			}
		}
	}

	if (DSI_REG[0]->DSI_MODE_CTRL.MODE != 0) { /* not in cmd mode */
		/* start DSI VM CMDQ */
		if (force_update)
			DSI_EnableVM_CMD(module, cmdq);
	} else {
		if (force_update) {
			DSI_Start(module, cmdq);
			DSI_WaitForNotBusy(module, cmdq);
		}
	}
}


void DSI_set_cmdq_V3(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, LCM_setting_table_V3 *para_tbl,
		     unsigned int size, unsigned char force_update)
{

	UINT32 i;
	/* UINT32 layer, layer_state, lane_num; */
	unsigned long goto_addr, mask_para, set_para;
	/* UINT32 fbPhysAddr, fbVirAddr; */
	struct DSI_T0_INS t0;
	/* DSI_T1_INS t1; */
	struct DSI_T2_INS t2;
	UINT32 index = 0;
	unsigned char data_id, cmd, count;
	unsigned char *para_list;
	UINT32 d;

	for (d = DSI_MODULE_BEGIN(module); d <= DSI_MODULE_END(module); d++) {
		do {
			data_id = para_tbl[index].id;
			cmd = para_tbl[index].cmd;
			count = para_tbl[index].count;
			para_list = para_tbl[index].para_list;

			if (data_id == REGFLAG_ESCAPE_ID && cmd == REGFLAG_DELAY_MS_V3) {
				udelay(1000 * count);
				DDPMSG("DISP/DSI DSI_set_cmdq_V3[%d]. Delay %d (ms)\n", index, count);

				continue;
			}
			if (DSI_REG[d]->DSI_MODE_CTRL.MODE != 0) {	/* not in cmd mode */
				struct DSI_VM_CMD_CON_REG vm_cmdq;

				OUTREG32(&vm_cmdq, AS_UINT32(&DSI_REG[d]->DSI_VM_CMD_CON));
				DDPMSG("set cmdq in VDO mode\n");
				if (count > 1) {
					vm_cmdq.LONG_PKT = 1;
					vm_cmdq.CM_DATA_ID = data_id;
					vm_cmdq.CM_DATA_0 = count + 1;
					OUTREG32(&DSI_REG[d]->DSI_VM_CMD_CON, AS_UINT32(&vm_cmdq));

					goto_addr =
					    (unsigned long)(&DSI_VM_CMD_REG[d]->data[0].byte0);
					mask_para = (0xFF << ((goto_addr & 0x3) * 8));
					set_para = (cmd << ((goto_addr & 0x3) * 8));
					MASKREG32(goto_addr & (~0x3), mask_para, set_para);

					for (i = 0; i < count; i++) {
						goto_addr =
						    (unsigned long)(&DSI_VM_CMD_REG[d]->data[0].
								    byte1) + i;
						mask_para = (0xFF << ((goto_addr & 0x3) * 8));
						set_para =
						    (para_list[i] << ((goto_addr & 0x3) * 8));
						MASKREG32(goto_addr & (~0x3), mask_para, set_para);
					}
				} else {
					vm_cmdq.LONG_PKT = 0;
					vm_cmdq.CM_DATA_0 = cmd;
					if (count) {
						vm_cmdq.CM_DATA_ID = data_id;
						vm_cmdq.CM_DATA_1 = para_list[0];
					} else {
						vm_cmdq.CM_DATA_ID = data_id;
						vm_cmdq.CM_DATA_1 = 0;
					}
					OUTREG32(&DSI_REG[d]->DSI_VM_CMD_CON, AS_UINT32(&vm_cmdq));
				}
				/* start DSI VM CMDQ */
				if (force_update)
					DSI_EnableVM_CMD(module, cmdq);
			} else {
				DSI_WaitForNotBusy(module, cmdq);

				OUTREG32(&DSI_CMDQ_REG[d]->data[0], 0);

				if (count > 1) {
					t2.CONFG = 2;
					t2.Data_ID = data_id;
					t2.WC16 = count + 1;

					DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[d]->data[0].byte0,
						     AS_UINT32(&t2));

					goto_addr =
					    (unsigned long)(&DSI_CMDQ_REG[d]->data[1].byte0);
					mask_para = (0xFFu << ((goto_addr & 0x3u) * 8));
					set_para = (cmd << ((goto_addr & 0x3u) * 8));
					DSI_MASKREG32(cmdq, goto_addr & (~((unsigned long)0x3u)),
						      mask_para, set_para);

					for (i = 0; i < count; i++) {
						goto_addr =
						    (unsigned long)(&DSI_CMDQ_REG[d]->data[1].
								    byte1) + i;
						mask_para = (0xFFu << ((goto_addr & 0x3u) * 8));
						set_para =
						    (para_list[i] << ((goto_addr & 0x3u) * 8));
						DSI_MASKREG32(cmdq,
							      goto_addr & (~((unsigned long)0x3u)),
							      mask_para, set_para);
					}

					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_CMDQ_SIZE,
						     2 + (count) / 4);
				} else {
					t0.CONFG = 0;
					t0.Data0 = cmd;
					if (count) {
						t0.Data_ID = data_id;
						t0.Data1 = para_list[0];
					} else {
						t0.Data_ID = data_id;
						t0.Data1 = 0;
					}
					DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[d]->data[0],
						     AS_UINT32(&t0));
					DSI_OUTREG32(cmdq, &DSI_REG[d]->DSI_CMDQ_SIZE, 1);
				}

				if (force_update) {
					DSI_Start(module, cmdq);
					DSI_WaitForNotBusy(module, cmdq);
				}
			}
		} while (++index < size);
	}

}

void DSI_set_cmdq(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq, unsigned int *pdata,
		  unsigned int queue_size, unsigned char force_update)
{
	/* DISPFUNC(); */

	int j = 0;
	int i = 0;

	/* DISPCHECK("DSI_set_cmdq, module=%s, cmdq=0x%08x\n", ddp_get_module_name(module), cmdq); */

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (DSI_REG[i]->DSI_MODE_CTRL.MODE != 0) {
#if 0
			/* not in cmd mode */
			struct DSI_VM_CMD_CON_REG vm_cmdq;

			OUTREG32(&vm_cmdq, AS_UINT32(&DSI_REG[i]->DSI_VM_CMD_CON));
			DDPMSG("set cmdq in VDO mode\n");
			if (queue_size > 1) {	/* long packet */
				vm_cmdq.LONG_PKT = 1;
				vm_cmdq.CM_DATA_ID = ((pdata[0] >> 8) & 0xFF);
				vm_cmdq.CM_DATA_0 = ((pdata[0] >> 16) & 0xFF);
				vm_cmdq.CM_DATA_1 = 0;
				OUTREG32(&DSI_REG[i]->DSI_VM_CMD_CON, AS_UINT32(&vm_cmdq));
				for (j = 0; j < queue_size - 1; j++) {
					OUTREG32(&DSI_VM_CMD_REG->data[j],
						 AS_UINT32((pdata + j + 1)));
				}
			} else {
				vm_cmdq.LONG_PKT = 0;
				vm_cmdq.CM_DATA_ID = ((pdata[0] >> 8) & 0xFF);
				vm_cmdq.CM_DATA_0 = ((pdata[0] >> 16) & 0xFF);
				vm_cmdq.CM_DATA_1 = ((pdata[0] >> 24) & 0xFF);
				OUTREG32(&DSI_REG->DSI_VM_CMD_CON, AS_UINT32(&vm_cmdq));
			}
#endif
		} else {
			ASSERT(queue_size <= 32);
			DSI_WaitForNotBusy(module, cmdq);
#ifdef ENABLE_DSI_ERROR_REPORT
			if ((pdata[0] & 1)) {
				memcpy(_dsi_cmd_queue, pdata, queue_size * 4);
				_dsi_cmd_queue[queue_size++] = 0x4;
				pdata = (unsigned int *)_dsi_cmd_queue;
			} else {
				pdata[0] |= 4;
			}
#endif

			for (j = 0; j < queue_size; j++) {
				DSI_OUTREG32(cmdq, &DSI_CMDQ_REG[i]->data[j],
					     AS_UINT32((pdata + j)));
			}

			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_CMDQ_SIZE, queue_size);
		}
	}

	if (DSI_REG[0]->DSI_MODE_CTRL.MODE != 0) {
#if 0
		/* start DSI VM CMDQ */
		if (force_update) {
			mmprofile_log_ex(MTKFB_MMP_Events.DSICmd, MMPROFILE_FLAG_START,
				       *(unsigned int *)(&DSI_VM_CMD_REG->data[0]),
				       *(unsigned int *)(&DSI_VM_CMD_REG->data[1]));
			DSI_EnableVM_CMD();

			/* must wait VM CMD done? */
			mmprofile_log_ex(MTKFB_MMP_Events.DSICmd, MMPROFILE_FLAG_END,
				       *(unsigned int *)(&DSI_VM_CMD_REG->data[2]),
				       *(unsigned int *)(&DSI_VM_CMD_REG->data[3]));
		}
#endif
	} else {
		if (force_update) {
			DSI_Start(module, cmdq);
			DSI_WaitForNotBusy(module, cmdq);
		}
	}
}

void _copy_dsi_params(LCM_DSI_PARAMS *src, LCM_DSI_PARAMS *dst)
{
	memcpy((LCM_DSI_PARAMS *) dst, (LCM_DSI_PARAMS *) src, sizeof(LCM_DSI_PARAMS));
}

int DSI_Send_ROI(enum DISP_MODULE_ENUM module, void *handle, unsigned int x, unsigned int y,
		 unsigned int width, unsigned int height)
{
	unsigned int x0 = x;
	unsigned int y0 = y;
	unsigned int x1 = x0 + width - 1;
	unsigned int y1 = y0 + height - 1;

	unsigned char x0_MSB = ((x0 >> 8) & 0xFF);
	unsigned char x0_LSB = (x0 & 0xFF);
	unsigned char x1_MSB = ((x1 >> 8) & 0xFF);
	unsigned char x1_LSB = (x1 & 0xFF);
	unsigned char y0_MSB = ((y0 >> 8) & 0xFF);
	unsigned char y0_LSB = (y0 & 0xFF);
	unsigned char y1_MSB = ((y1 >> 8) & 0xFF);
	unsigned char y1_LSB = (y1 & 0xFF);

	unsigned int data_array[16];

	data_array[0] = 0x00053902;
	data_array[1] = (x1_MSB << 24) | (x0_LSB << 16) | (x0_MSB << 8) | 0x2a;
	data_array[2] = (x1_LSB);
	DSI_set_cmdq(module, handle, data_array, 3, 1);
	data_array[0] = 0x00053902;
	data_array[1] = (y1_MSB << 24) | (y0_LSB << 16) | (y0_MSB << 8) | 0x2b;
	data_array[2] = (y1_LSB);
	DSI_set_cmdq(module, handle, data_array, 3, 1);
	DDPMSG("DSI_Send_ROI(%d,%d,%d,%d)Done!\n", x, y, width, height);

	/* data_array[0]= 0x002c3909; */
	/* DSI_set_cmdq(module, handle, data_array, 1, 0); */
	return 0;
}



static void lcm_set_reset_pin(UINT32 value)
{
#if 1
	DSI_OUTREG32(NULL, DISPSYS_CONFIG_BASE + 0x150, value);
#else
#if !defined(CONFIG_MTK_LEGACY)
	if (value)
		disp_dts_gpio_select_state(DTS_GPIO_STATE_LCM_RST_OUT1);
	else
		disp_dts_gpio_select_state(DTS_GPIO_STATE_LCM_RST_OUT0);
#endif
#endif
}

static void lcm_udelay(UINT32 us)
{
	udelay(us);
}

static void lcm_mdelay(UINT32 ms)
{
	if (ms < 10)
		udelay(ms * 1000);
	else
		msleep(ms);
}

void DSI_set_cmdq_V11_wrapper_DSI0(void *cmdq, unsigned int *pdata, unsigned int queue_size, unsigned char force_update)
{
	DSI_set_cmdq(DISP_MODULE_DSI0, cmdq, pdata, queue_size, force_update);
}

void DSI_set_cmdq_V11_wrapper_DSI1(void *cmdq, unsigned int *pdata, unsigned int queue_size, unsigned char force_update)
{
	DSI_set_cmdq(DISP_MODULE_DSI1, cmdq, pdata, queue_size, force_update);
}

void DSI_set_cmdq_V2_DSI0(void *cmdq, unsigned cmd, unsigned char count, unsigned char *para_list,
			  unsigned char force_update)
{
	DSI_set_cmdq_V2(DISP_MODULE_DSI0, cmdq, cmd, count, para_list, force_update);
}

void DSI_set_cmdq_V2_DSI1(void *cmdq, unsigned cmd, unsigned char count, unsigned char *para_list,
			  unsigned char force_update)
{
	DSI_set_cmdq_V2(DISP_MODULE_DSI1, cmdq, cmd, count, para_list, force_update);
}

void DSI_set_cmdq_V2_DSIDual(void *cmdq, unsigned cmd, unsigned char count,
			     unsigned char *para_list, unsigned char force_update)
{
	DSI_set_cmdq_V2(DISP_MODULE_DSIDUAL, cmdq, cmd, count, para_list, force_update);
}

void DSI_set_cmdq_V2_Wrapper_DSI0(unsigned cmd, unsigned char count, unsigned char *para_list,
				  unsigned char force_update)
{
	DSI_set_cmdq_V2(DISP_MODULE_DSI0, NULL, cmd, count, para_list, force_update);
}

void DSI_set_cmdq_V2_Wrapper_DSI1(unsigned cmd, unsigned char count, unsigned char *para_list,
				  unsigned char force_update)
{
	DSI_set_cmdq_V2(DISP_MODULE_DSI1, NULL, cmd, count, para_list, force_update);
}

void DSI_set_cmdq_V2_Wrapper_DSIDual(unsigned cmd, unsigned char count, unsigned char *para_list,
				     unsigned char force_update)
{
	DSI_set_cmdq_V2(DISP_MODULE_DSIDUAL, NULL, cmd, count, para_list, force_update);
}

void DSI_set_cmdq_V3_Wrapper_DSI0(LCM_setting_table_V3 *para_tbl, unsigned int size,
				  unsigned char force_update)
{
	DSI_set_cmdq_V3(DISP_MODULE_DSI0, NULL, para_tbl, size, force_update);
}

void DSI_set_cmdq_V3_Wrapper_DSI1(LCM_setting_table_V3 *para_tbl, unsigned int size,
				  unsigned char force_update)
{
	DSI_set_cmdq_V3(DISP_MODULE_DSI1, NULL, para_tbl, size, force_update);
}

void DSI_set_cmdq_V3_Wrapper_DSIDual(LCM_setting_table_V3 *para_tbl, unsigned int size,
				     unsigned char force_update)
{
	DSI_set_cmdq_V3(DISP_MODULE_DSIDUAL, NULL, para_tbl, size, force_update);
}

void DSI_set_cmdq_wrapper_DSI0(unsigned int *pdata, unsigned int queue_size,
			       unsigned char force_update)
{
	DSI_set_cmdq(DISP_MODULE_DSI0, NULL, pdata, queue_size, force_update);
}

void DSI_set_cmdq_wrapper_DSI1(unsigned int *pdata, unsigned int queue_size,
			       unsigned char force_update)
{
	DSI_set_cmdq(DISP_MODULE_DSI1, NULL, pdata, queue_size, force_update);
}

void DSI_set_cmdq_wrapper_DSIDual(unsigned int *pdata, unsigned int queue_size,
				  unsigned char force_update)
{
	DSI_set_cmdq(DISP_MODULE_DSIDUAL, NULL, pdata, queue_size, force_update);
}

unsigned int DSI_dcs_read_lcm_reg_v2_wrapper_DSI0(UINT8 cmd, UINT8 *buffer, UINT8 buffer_size)
{
	return DSI_dcs_read_lcm_reg_v2(DISP_MODULE_DSI0, NULL, cmd, buffer, buffer_size);
}

unsigned int DSI_dcs_read_lcm_reg_v2_wrapper_DSI1(UINT8 cmd, UINT8 *buffer, UINT8 buffer_size)
{
	return DSI_dcs_read_lcm_reg_v2(DISP_MODULE_DSI1, NULL, cmd, buffer, buffer_size);
}

unsigned int DSI_dcs_read_lcm_reg_v2_wrapper_DSIDUAL(UINT8 cmd, UINT8 *buffer, UINT8 buffer_size)
{
	return DSI_dcs_read_lcm_reg_v2(DISP_MODULE_DSIDUAL, NULL, cmd, buffer, buffer_size);
}

long lcd_enp_bias_setting(unsigned int value)
{
	long ret = 0;

#if !defined(CONFIG_MTK_LEGACY)
	if (value)
		ret = disp_dts_gpio_select_state(DTS_GPIO_STATE_LCD_BIAS_ENP);
	else
		ret = disp_dts_gpio_select_state(DTS_GPIO_STATE_LCD_BIAS_ENN);
#endif

	return ret;
}
int ddp_dsi_set_lcm_utils(enum DISP_MODULE_ENUM module, LCM_DRIVER *lcm_drv)
{
	LCM_UTIL_FUNCS *utils = NULL;

	if (lcm_drv == NULL) {
		DISPERR("lcm_drv is null\n");
		return -1;
	}

	if (module == DISP_MODULE_DSI0) {
		utils = (LCM_UTIL_FUNCS *)&lcm_utils_dsi0;
	} else if (module == DISP_MODULE_DSI1) {
		utils = (LCM_UTIL_FUNCS *)&lcm_utils_dsi1;
	} else if (module == DISP_MODULE_DSIDUAL) {
		utils = (LCM_UTIL_FUNCS *)&lcm_utils_dsidual;
	} else {
		DISPERR("wrong module: %d\n", module);
		return -1;
	}

	utils->set_reset_pin = lcm_set_reset_pin;
	utils->udelay = lcm_udelay;
	utils->mdelay = lcm_mdelay;
	if (module == DISP_MODULE_DSI0) {
		utils->dsi_set_cmdq = DSI_set_cmdq_wrapper_DSI0;
		utils->dsi_set_cmdq_V2 = DSI_set_cmdq_V2_Wrapper_DSI0;
		utils->dsi_set_cmdq_V3 = DSI_set_cmdq_V3_Wrapper_DSI0;
		utils->dsi_dcs_read_lcm_reg_v2 = DSI_dcs_read_lcm_reg_v2_wrapper_DSI0;
		utils->dsi_set_cmdq_V22 = DSI_set_cmdq_V2_DSI0;
		utils->dsi_set_cmdq_V11 = DSI_set_cmdq_V11_wrapper_DSI0;
		utils->dsi_set_cmdq_V23 = DSI_set_cmdq_V2_DSI0;
	} else if (module == DISP_MODULE_DSI1) {
		utils->dsi_set_cmdq = DSI_set_cmdq_wrapper_DSI1;
		utils->dsi_set_cmdq_V2 = DSI_set_cmdq_V2_Wrapper_DSI1;
		utils->dsi_set_cmdq_V3 = DSI_set_cmdq_V3_Wrapper_DSI1;
		utils->dsi_dcs_read_lcm_reg_v2 = DSI_dcs_read_lcm_reg_v2_wrapper_DSI1;
		utils->dsi_set_cmdq_V22 = DSI_set_cmdq_V2_DSI1;
		utils->dsi_set_cmdq_V11 = DSI_set_cmdq_V11_wrapper_DSI1;
		utils->dsi_set_cmdq_V23 = DSI_set_cmdq_V2_DSI1;
	} else if (module == DISP_MODULE_DSIDUAL) {
		/* TODO: Ugly workaround, hope we can found better resolution */
		LCM_PARAMS lcm_param;

		lcm_drv->get_params(&lcm_param);

		if (lcm_param.lcm_cmd_if == LCM_INTERFACE_DSI0) {
			utils->dsi_set_cmdq = DSI_set_cmdq_wrapper_DSI0;
			utils->dsi_set_cmdq_V2 = DSI_set_cmdq_V2_Wrapper_DSI0;
			utils->dsi_set_cmdq_V3 = DSI_set_cmdq_V3_Wrapper_DSI0;
			utils->dsi_dcs_read_lcm_reg_v2 = DSI_dcs_read_lcm_reg_v2_wrapper_DSI0;
			utils->dsi_set_cmdq_V22 = DSI_set_cmdq_V2_DSI0;
			utils->dsi_set_cmdq_V23 = DSI_set_cmdq_V2_DSI0;
		} else if (lcm_param.lcm_cmd_if == LCM_INTERFACE_DSI1) {
			utils->dsi_set_cmdq = DSI_set_cmdq_wrapper_DSI1;
			utils->dsi_set_cmdq_V2 = DSI_set_cmdq_V2_Wrapper_DSI1;
			utils->dsi_set_cmdq_V3 = DSI_set_cmdq_V3_Wrapper_DSI1;
			utils->dsi_dcs_read_lcm_reg_v2 = DSI_dcs_read_lcm_reg_v2_wrapper_DSI1;
			utils->dsi_set_cmdq_V22	= DSI_set_cmdq_V2_DSI1;
			utils->dsi_set_cmdq_V23 = DSI_set_cmdq_V2_DSI1;
		} else {
			utils->dsi_set_cmdq = DSI_set_cmdq_wrapper_DSIDual;
			utils->dsi_set_cmdq_V2 = DSI_set_cmdq_V2_Wrapper_DSIDual;
			utils->dsi_dcs_read_lcm_reg_v2 = DSI_dcs_read_lcm_reg_v2_wrapper_DSIDUAL;
			/* utils->dsi_set_cmdq_V22 = DSI_set_cmdq_V22_Wrapper_DSIDual; */
			utils->dsi_set_cmdq_V23 = DSI_set_cmdq_V2_DSIDual;
		}
	}

#ifndef CONFIG_FPGA_EARLY_PORTING
#ifdef CONFIG_MTK_LEGACY
	utils->set_gpio_out = mt_set_gpio_out;
	utils->set_gpio_mode = mt_set_gpio_mode;
	utils->set_gpio_dir = mt_set_gpio_dir;
	utils->set_gpio_pull_enable = (int (*)(unsigned int, unsigned char))mt_set_gpio_pull_enable;
#else
	utils->set_gpio_lcd_enp_bias = lcd_enp_bias_setting;
#endif
#endif

	lcm_drv->set_util_funcs(utils);

	return 0;
}

void DSI_ChangeClk(enum DISP_MODULE_ENUM module, UINT32 clk)
{
	int i = 0;

	if (clk > 1250 || clk < 50)
		return;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		LCM_DSI_PARAMS *dsi_params = &_dsi_context[i].dsi_params;

		dsi_params->PLL_CLOCK = clk;
		DSI_WaitForNotBusy(module, NULL);
		DSI_PHY_clk_setting(module, NULL, dsi_params);
		DSI_PHY_TIMCONFIG(module, NULL, dsi_params);
	}
}

/* when clkmgr init, it will put dsi0/dsi1 default on*/
 /* If we don't use DSI1, its clock will always on !!*/
 /* So, here we enable+disable dsi clock, whick will close DSI1 clock.*/
 /* If we default use DSI_DUAL, this func does nothing*/
 /* (because DSI0/DSI1 clock is already enabled */
static int __close_dsi_default_clock(enum DISP_MODULE_ENUM module)
{
	static int not_first_time;
	int ret = 0;

	if (not_first_time)
		return 0;

	not_first_time = 1;

	ret += ddp_clk_enable(DISP1_DSI0_MM_CLOCK);
	ret += ddp_clk_enable(DISP1_DSI0_INTERFACE_CLOCK);
	ret += ddp_clk_disable(DISP1_DSI0_MM_CLOCK);
	ret += ddp_clk_disable(DISP1_DSI0_INTERFACE_CLOCK);

	ret += ddp_clk_enable(DISP1_DSI1_MM_CLOCK);
	ret += ddp_clk_enable(DISP1_DSI1_INTERFACE_CLOCK);
	ret += ddp_clk_disable(DISP1_DSI1_MM_CLOCK);
	ret += ddp_clk_disable(DISP1_DSI1_INTERFACE_CLOCK);

	/* mipi-tx */
	ddp_set_mipi26m(DISP_MODULE_DSI0, 1);
	ddp_set_mipi26m(DISP_MODULE_DSI0, 0);
	ddp_set_mipi26m(DISP_MODULE_DSI1, 1);
	ddp_set_mipi26m(DISP_MODULE_DSI1, 0);

	return ret;
}

int ddp_dsi_init(enum DISP_MODULE_ENUM module, void *cmdq)
{
	int i = 0;
#ifndef CONFIG_FPGA_EARLY_PORTING
#ifdef ENABLE_CLK_MGR
	enum DSI_STATUS ret = DSI_STATUS_OK;
#endif
#endif

	DISPFUNC();
	/* DSI_OUTREG32(cmdq, 0xf0000048, 0x80000000); */
	/* DSI_OUTREG32(cmdq, MMSYS_CONFIG_BASE+0x108, 0xffffffff); */
	/* DSI_OUTREG32(cmdq, MMSYS_CONFIG_BASE+0x118, 0xffffffff); */
	/* DSI_OUTREG32(MMSYS_CONFIG_BASE+0xC08, 0xffffffff); */
#ifdef ENABLE_CLK_MGR
#ifndef CONFIG_MTK_CLKMGR
	ddp_parse_apmixed_base();
#endif
#endif
	DSI_REG[0] = (struct DSI_REGS *)DISPSYS_DSI0_BASE;
	DSI_PHY_REG[0] = (struct DSI_PHY_REGS *)MIPITX0_BASE;
	DSI_CMDQ_REG[0] = (struct DSI_CMDQ_REGS *)(DISPSYS_DSI0_BASE + 0x200);
	DSI_REG[1] = (struct DSI_REGS *)DISPSYS_DSI1_BASE;
	DSI_PHY_REG[1] = (struct DSI_PHY_REGS *)MIPITX1_BASE;
	DSI_CMDQ_REG[1] = (struct DSI_CMDQ_REGS *)(DISPSYS_DSI1_BASE + 0x200);

	DSI_VM_CMD_REG[0] = (struct DSI_VM_CMDQ_REGS *)(DISPSYS_DSI0_BASE + 0x134);
	DSI_VM_CMD_REG[1] = (struct DSI_VM_CMDQ_REGS *)(DISPSYS_DSI1_BASE + 0x134);
	memset(&_dsi_context, 0, sizeof(_dsi_context));

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		init_waitqueue_head(&_dsi_cmd_done_wait_queue[i]);
		init_waitqueue_head(&_dsi_dcs_read_wait_queue[i]);
		init_waitqueue_head(&_dsi_wait_bta_te[i]);
		init_waitqueue_head(&_dsi_wait_ext_te[i]);
		init_waitqueue_head(&_dsi_wait_vm_done_queue[i]);
		init_waitqueue_head(&_dsi_wait_vm_cmd_done_queue[i]);
		init_waitqueue_head(&_dsi_wait_sleep_out_done_queue[i]);
		DISPINFO("dsi%d initializing event queue\n", i);
	}

	cmdqBackupAllocateSlot(&_h_intstat, 1);

	disp_register_module_irq_callback(DISP_MODULE_DSI0, _DSI_INTERNAL_IRQ_Handler);
	disp_register_module_irq_callback(DISP_MODULE_DSI1, _DSI_INTERNAL_IRQ_Handler);
	disp_register_module_irq_callback(DISP_MODULE_DSIDUAL, _DSI_INTERNAL_IRQ_Handler);

#ifndef CONFIG_FPGA_EARLY_PORTING
	if (MIPITX_IsEnabled(module, cmdq)) {
		s_isDsiPowerOn = TRUE;
#ifdef ENABLE_CLK_MGR
#ifdef CONFIG_MTK_CLKMGR
		set_mipi26m(1);
#else
		ddp_set_mipi26m(module, 1);
#endif
		if (module == DISP_MODULE_DSI0 || module == DISP_MODULE_DSIDUAL) {
#ifndef CONFIG_MTK_CLKMGR
			ret += ddp_clk_enable(DISP1_DSI0_MM_CLOCK);
			ret += ddp_clk_enable(DISP1_DSI0_INTERFACE_CLOCK);
#endif
			if (ret > 0) {
				DISP_LOG_PRINT(ANDROID_LOG_WARN, "DSI",
					       "DSI0 power manager API return FALSE\n");
			}
		}

		if (module == DISP_MODULE_DSI1 || module == DISP_MODULE_DSIDUAL) {
#ifndef CONFIG_MTK_CLKMGR
			ret += ddp_clk_enable(DISP1_DSI1_MM_CLOCK);
			ret += ddp_clk_enable(DISP1_DSI1_INTERFACE_CLOCK);
#endif
			if (ret > 0) {
				DISP_LOG_PRINT(ANDROID_LOG_WARN, "DSI",
					       "DSI1 power manager API return FALSE\n");
			}
		}
		__close_dsi_default_clock(module);
#endif

		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
			unsigned long mipi_tx_reg_base = (unsigned long)DSI_PHY_REG[i];

			if (i == 0) { /* only enable DSI0's INT */
				DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, CMD_DONE, 1);
				DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, RD_RDY, 1);
				DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, VM_DONE, 1);
				/* enable te_rdy when need, not here (both cmd mode & vdo mode) */
				/* DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG,DSI_REG[i]->DSI_INTEN,TE_RDY,1); */
				DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG,
							  DSI_REG[i]->DSI_INTEN, VM_CMD_DONE, 1);
				DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, SLEEPOUT_DONE, 1);
				 /* DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG,
				  *			DSI_REG[i]->DSI_INTEN,FRAME_DONE_INT_EN,0);
				  */
			}

			DSI_BackupRegisters(module, NULL);

			clock_lane[i] = (INREG32(mipi_tx_reg_base + 0x4)); /* MIPITX_DSI_CLOCK_LANE */
			data_lane0[i] = (INREG32(mipi_tx_reg_base + 0x8)); /* MIPITX_DSI_DATA_LANE0 */
			data_lane1[i] = (INREG32(mipi_tx_reg_base + 0xc)); /* MIPITX_DSI_DATA_LANE1 */
			data_lane2[i] = (INREG32(mipi_tx_reg_base + 0x10)); /* MIPITX_DSI_DATA_LANE2 */
			data_lane3[i] = (INREG32(mipi_tx_reg_base + 0x14)); /* MIPITX_DSI_DATA_LANE3 */
			DISPINFO("mipi_tx%d: clk=0x%x,lan0=0x%x,lan1=0x%x,lan2=0x%x,lan3=0x%x\n",
				  i, clock_lane[i], data_lane0[i], data_lane1[i], data_lane2[i], data_lane3[i]);
		}
	}
#endif

	return DSI_STATUS_OK;
}

int ddp_dsi_deinit(enum DISP_MODULE_ENUM module, void *cmdq_handle)
{
	return 0;
}

void _dump_dsi_params(LCM_DSI_PARAMS *dsi_config)
{
	if (dsi_config) {
		switch (dsi_config->mode) {
		case CMD_MODE:
			DISPDBG("[DDPDSI] DSI Mode: CMD_MODE\n");
			break;
		case SYNC_PULSE_VDO_MODE:
			DISPDBG("[DDPDSI] DSI Mode: SYNC_PULSE_VDO_MODE\n");
			break;
		case SYNC_EVENT_VDO_MODE:
			DISPDBG("[DDPDSI] DSI Mode: SYNC_EVENT_VDO_MODE\n");
			break;
		case BURST_VDO_MODE:
			DISPDBG("[DDPDSI] DSI Mode: BURST_VDO_MODE\n");
			break;
		default:
			DISPERR("[DDPDSI] DSI Mode: Unknown\n");
			break;
		}

		DISPDBG
		    ("[DDPDSI] vact: %d, vbp: %d, vfp: %d, vact_line: %d, hact: %d, hbp: %d, hfp: %d, hblank: %d\n",
		     dsi_config->vertical_sync_active, dsi_config->vertical_backporch,
		     dsi_config->vertical_frontporch, dsi_config->vertical_active_line,
		     dsi_config->horizontal_sync_active, dsi_config->horizontal_backporch,
		     dsi_config->horizontal_frontporch, dsi_config->horizontal_blanking_pixel);
		DISPDBG
		    ("[DDPDSI] pll_select: %d, pll_div1: %d, pll_div2: %d, fbk_div: %d,fbk_sel: %d, rg_bir: %d\n",
		     dsi_config->pll_select, dsi_config->pll_div1, dsi_config->pll_div2,
		     dsi_config->fbk_div, dsi_config->fbk_sel, dsi_config->rg_bir);
		DISPDBG("[DDPDSI] rg_bic: %d, rg_bp: %d, PLL_CLOCK: %d, dsi_clock: %d, ssc_range: %d\n",
		     dsi_config->rg_bic, dsi_config->rg_bp, dsi_config->PLL_CLOCK,
		     dsi_config->dsi_clock, dsi_config->ssc_range);

		DISPDBG("[DDPDSI] ssc_disable: %d, compatibility_for_nvk: %d, cont_clock: %d\n",
		     dsi_config->ssc_disable, dsi_config->compatibility_for_nvk, dsi_config->cont_clock);

		DISPDBG
		    ("[DDPDSI] lcm_ext_te_enable: %d, noncont_clock: %d, noncont_clock_period: %d\n",
		     dsi_config->lcm_ext_te_enable, dsi_config->noncont_clock,
		     dsi_config->noncont_clock_period);
	}

}

static void DSI_PHY_CLK_LP_PerLine_config(enum DISP_MODULE_ENUM module, struct cmdqRecStruct *cmdq,
					  LCM_DSI_PARAMS *dsi_params)
{
	int i;
	struct DSI_PHY_TIMCON0_REG timcon0;	/* LPX */
	struct DSI_PHY_TIMCON2_REG timcon2;	/* CLK_HS_TRAIL, CLK_HS_ZERO */
	struct DSI_PHY_TIMCON3_REG timcon3;	/* CLK_HS_EXIT, CLK_HS_POST, CLK_HS_PREP */
	struct DSI_HSA_WC_REG hsa;
	struct DSI_HBP_WC_REG hbp;
	struct DSI_HFP_WC_REG hfp, new_hfp;
	struct DSI_BLLP_WC_REG bllp;
	struct DSI_PSCTRL_REG ps;
	UINT32 hstx_ckl_wc, new_hstx_ckl_wc;
	UINT32 v_a, v_b, v_c, lane_num;
	LCM_DSI_MODE_CON dsi_mode;

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		lane_num = dsi_params->LANE_NUM;
		dsi_mode = dsi_params->mode;

		if (dsi_mode == CMD_MODE)
			continue;
		/* vdo mode */
		DSI_OUTREG32(cmdq, &hsa, AS_UINT32(&DSI_REG[i]->DSI_HSA_WC));
		DSI_OUTREG32(cmdq, &hbp, AS_UINT32(&DSI_REG[i]->DSI_HBP_WC));
		DSI_OUTREG32(cmdq, &hfp, AS_UINT32(&DSI_REG[i]->DSI_HFP_WC));
		DSI_OUTREG32(cmdq, &bllp, AS_UINT32(&DSI_REG[i]->DSI_BLLP_WC));
		DSI_OUTREG32(cmdq, &ps, AS_UINT32(&DSI_REG[i]->DSI_PSCTRL));
		DSI_OUTREG32(cmdq, &hstx_ckl_wc, AS_UINT32(&DSI_REG[i]->DSI_HSTX_CKL_WC));
		DSI_OUTREG32(cmdq, &timcon0, AS_UINT32(&DSI_REG[i]->DSI_PHY_TIMECON0));
		DSI_OUTREG32(cmdq, &timcon2, AS_UINT32(&DSI_REG[i]->DSI_PHY_TIMECON2));
		DSI_OUTREG32(cmdq, &timcon3, AS_UINT32(&DSI_REG[i]->DSI_PHY_TIMECON3));

		/* 1. sync_pulse_mode */
		/* Total    WC(A) = HSA_WC + HBP_WC + HFP_WC + PS_WC + 32 */
		/* CLK init WC(B) = (CLK_HS_EXIT + LPX + CLK_HS_PREP + CLK_HS_ZERO)*lane_num */
		/* CLK end  WC(C) = (CLK_HS_POST + CLK_HS_TRAIL)*lane_num */
		/* HSTX_CKLP_WC = A - B */
		/* Limitation: B + C < HFP_WC */
		if (dsi_mode == SYNC_PULSE_VDO_MODE) {
			v_a = hsa.HSA_WC + hbp.HBP_WC + hfp.HFP_WC + ps.DSI_PS_WC + 32;
			v_b =
			    (timcon3.CLK_HS_EXIT + timcon0.LPX + timcon3.CLK_HS_PRPR +
			     timcon2.CLK_ZERO) * lane_num;
			v_c = (timcon3.CLK_HS_POST + timcon2.CLK_TRAIL) * lane_num;

			DISPCHECK("===>v_a-v_b=0x%x,HSTX_CKLP_WC=0x%x\n", (v_a - v_b), hstx_ckl_wc);
/* DISPCHECK("===>v_b+v_c=0x%x,HFP_WC=0x%x\n",(v_b+v_c),hfp); */
			DISPCHECK("===>Will Reconfig in order to fulfill LP clock lane per line\n");

			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HFP_WC, (v_b + v_c + DIFF_CLK_LANE_LP));
			DSI_OUTREG32(cmdq, &new_hfp, AS_UINT32(&DSI_REG[i]->DSI_HFP_WC));
			v_a = hsa.HSA_WC + hbp.HBP_WC + new_hfp.HFP_WC + ps.DSI_PS_WC + 32;
			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HSTX_CKL_WC, (v_a - v_b));
			DSI_OUTREG32(cmdq, &new_hstx_ckl_wc,
				     AS_UINT32(&DSI_REG[i]->DSI_HSTX_CKL_WC));
			DISPCHECK("===>new HSTX_CKL_WC=0x%x, HFP_WC=0x%x\n", new_hstx_ckl_wc,
				  new_hfp.HFP_WC);
		}
		/* 2. sync_event_mode */
		/* Total    WC(A) = HBP_WC + HFP_WC + PS_WC + 26 */
		/* CLK init WC(B) = (CLK_HS_EXIT + LPX + CLK_HS_PREP + CLK_HS_ZERO)*lane_num */
		/* CLK end  WC(C) = (CLK_HS_POST + CLK_HS_TRAIL)*lane_num */
		/* HSTX_CKLP_WC = A - B */
		/* Limitation: B + C < HFP_WC */
		else if (dsi_mode == SYNC_EVENT_VDO_MODE) {
			v_a = hbp.HBP_WC + hfp.HFP_WC + ps.DSI_PS_WC + 26;
			v_b =
			    (timcon3.CLK_HS_EXIT + timcon0.LPX + timcon3.CLK_HS_PRPR +
			     timcon2.CLK_ZERO) * lane_num;
			v_c = (timcon3.CLK_HS_POST + timcon2.CLK_TRAIL) * lane_num;

			DISPCHECK("===>v_a-v_b=0x%x,HSTX_CKLP_WC=0x%x\n", (v_a - v_b), hstx_ckl_wc);
/* DISPCHECK("===>v_b+v_c=0x%x,HFP_WC=0x%x\n",(v_b+v_c),hfp); */
			DISPCHECK("===>Will Reconfig in order to fulfill LP clock lane per line\n");

			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HFP_WC, (v_b + v_c + DIFF_CLK_LANE_LP));
			DSI_OUTREG32(cmdq, &new_hfp, AS_UINT32(&DSI_REG[i]->DSI_HFP_WC));
			v_a = hbp.HBP_WC + new_hfp.HFP_WC + ps.DSI_PS_WC + 26;
			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HSTX_CKL_WC, (v_a - v_b));
			DSI_OUTREG32(cmdq, &new_hstx_ckl_wc,
				     AS_UINT32(&DSI_REG[i]->DSI_HSTX_CKL_WC));
			DISPCHECK("===>new HSTX_CKL_WC=0x%x, HFP_WC=0x%x\n", new_hstx_ckl_wc,
				  new_hfp.HFP_WC);

		}
		/* 3. burst_mode */
		/* Total    WC(A) = HBP_WC + HFP_WC + PS_WC + BLLP_WC + 32 */
		/* CLK init WC(B) = (CLK_HS_EXIT + LPX + CLK_HS_PREP + CLK_HS_ZERO)*lane_num */
		/* CLK end  WC(C) = (CLK_HS_POST + CLK_HS_TRAIL)*lane_num */
		/* HSTX_CKLP_WC = A - B */
		/* Limitation: B + C < HFP_WC */
		else if (dsi_mode == BURST_VDO_MODE) {
			v_a = hbp.HBP_WC + hfp.HFP_WC + ps.DSI_PS_WC + bllp.BLLP_WC + 32;
			v_b =
			    (timcon3.CLK_HS_EXIT + timcon0.LPX + timcon3.CLK_HS_PRPR +
			     timcon2.CLK_ZERO) * lane_num;
			v_c = (timcon3.CLK_HS_POST + timcon2.CLK_TRAIL) * lane_num;

			DISPCHECK("===>v_a-v_b=0x%x,HSTX_CKLP_WC=0x%x\n", (v_a - v_b), hstx_ckl_wc);
			/* DISPCHECK("===>v_b+v_c=0x%x,HFP_WC=0x%x\n",(v_b+v_c),hfp); */
			DISPCHECK("===>Will Reconfig in order to fulfill LP clock lane per line\n");

			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HFP_WC, (v_b + v_c + DIFF_CLK_LANE_LP));
			DSI_OUTREG32(cmdq, &new_hfp, AS_UINT32(&DSI_REG[i]->DSI_HFP_WC));
			v_a = hbp.HBP_WC + new_hfp.HFP_WC + ps.DSI_PS_WC + bllp.BLLP_WC + 32;
			DSI_OUTREG32(cmdq, &DSI_REG[i]->DSI_HSTX_CKL_WC, (v_a - v_b));
			DSI_OUTREG32(cmdq, &new_hstx_ckl_wc,
				     AS_UINT32(&DSI_REG[i]->DSI_HSTX_CKL_WC));
			DISPCHECK("===>new HSTX_CKL_WC=0x%x, HFP_WC=0x%x\n", new_hstx_ckl_wc,
				  new_hfp.HFP_WC);
		}
	}

}

void ddp_dsi_update_partial(enum DISP_MODULE_ENUM module, void *cmdq, void *params)
{
	struct disp_rect *roi = (struct disp_rect *)params;

	DSI_PS_Control(module, cmdq, &(_dsi_context[0].dsi_params),
			roi->width, roi->height);
	DSI_Send_ROI(DISP_MODULE_DSI0, cmdq, roi->x, roi->y, roi->width, roi->height);
}

int ddp_dsi_config(enum DISP_MODULE_ENUM module, struct disp_ddp_path_config *config, void *cmdq)
{
	int i = 0;
	LCM_DSI_PARAMS *dsi_config;

	if (!config->dst_dirty) {
		if (atomic_read(&PMaster_enable) == 0)
			return 0;
	}
	DISPFUNC();
	DISPDBG("===>run here 00 Pmaster: clk:%d\n", _dsi_context[0].dsi_params.PLL_CLOCK);
	dsi_config = &(config->dispif_config.dsi);

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		_copy_dsi_params(dsi_config, &(_dsi_context[i].dsi_params));
		_dsi_context[i].lcm_width = config->dst_w;
		_dsi_context[i].lcm_height = config->dst_h;
		_dump_dsi_params(&(_dsi_context[i].dsi_params));
		if (dsi_config->mode != CMD_MODE) {
			/* not enable TE in vdo mode */
			if (dsi_config->eint_disable == 1) {
				DSI_OUTREGBIT(cmdq, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN,
					      TE_RDY, 1);
				DISPDBG("DSI VDO Mode TEINT On\n");
			}
		} else {
			/*enable TE in cmd mode */
			if (i == 0) /* only enable DSI0's INT */
				DSI_OUTREGBIT(cmdq, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, TE_RDY, 1);
		}
	}
	DISPDBG("===>01Pmaster: clk:%d\n", _dsi_context[0].dsi_params.PLL_CLOCK);
	if (dsi_config->mode != CMD_MODE)
		dsi_currect_mode = 1;
#ifndef CONFIG_FPGA_EARLY_PORTING
	if ((MIPITX_IsEnabled(module, cmdq)) && (atomic_read(&PMaster_enable) == 0)) {
		DISPINFO("mipitx is already init\n");
		if (dsi_force_config)
			goto force_config;
		else
			goto done;
	} else
#endif
	{
#ifndef CONFIG_FPGA_EARLY_PORTING

		DISPCHECK("MIPITX is not inited, will config mipitx clk=%d\n",
			  _dsi_context[0].dsi_params.PLL_CLOCK);
		DSI_PHY_clk_switch(module, NULL, false);
		DSI_PHY_clk_setting(module, NULL, dsi_config);
#endif

#if defined(MTK_NO_DISP_IN_LK)
		s_isDsiPowerOn = 1;
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
			DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, CMD_DONE, 1);
			DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, RD_RDY, 1);
			DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, VM_DONE, 1);
			/* enable te_rdy when need, not here (both cmd mode & vdo mode) */
			/* DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG,DSI_REG[i]->DSI_INTEN,TE_RDY,1); */
			DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, VM_CMD_DONE, 1);
			DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, SLEEPOUT_DONE, 1);
			/* DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG,DSI_REG[i]->DSI_INTEN,FRAME_DONE_INT_EN,0); */
			DSI_BackupRegisters(module, NULL);
		}
#endif
	}

#ifndef CONFIG_FPGA_EARLY_PORTING
force_config:
#endif
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (dsi_config->mode == CMD_MODE
		    || ((dsi_config->switch_mode_enable == 1)
			&& (dsi_config->switch_mode == CMD_MODE))
		    || (dsi_config->mode != CMD_MODE && dsi_config->eint_disable))
			DSI_OUTREGBIT(cmdq, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, TE_RDY, 1);
	}
	/* DSI_Reset(module, cmdq_handle); */
	DSI_TXRX_Control(module, cmdq, dsi_config);
	DSI_PS_Control(module, cmdq, dsi_config, config->dst_w, config->dst_h);
	DSI_PHY_TIMCONFIG(module, cmdq, dsi_config);

	if (dsi_config->mode != CMD_MODE
	    || ((dsi_config->switch_mode_enable == 1) && (dsi_config->switch_mode != CMD_MODE))) {
		DSI_Config_VDO_Timing(module, cmdq, dsi_config);
		DSI_Set_VM_CMD(module, cmdq);
	}
	/* Enable clk low power per Line ; */
	if (dsi_config->clk_lp_per_line_enable)
		DSI_PHY_CLK_LP_PerLine_config(module, cmdq, dsi_config);


#ifndef CONFIG_FPGA_EARLY_PORTING
done:
#endif

	return 0;
}

/*TUI will use the api*/
int dsi_enable_irq(enum DISP_MODULE_ENUM module, void *handle, unsigned int enable)
{
	if (module == DISP_MODULE_DSI0)
		DSI_OUTREGBIT(handle, struct DSI_INT_ENABLE_REG, DSI_REG[0]->DSI_INTEN, FRAME_DONE, enable);

	return 0;
}

int ddp_dsi_start(enum DISP_MODULE_ENUM module, void *cmdq)
{
	int i = 0;
	int g_lcm_x = disp_helper_get_option(DISP_OPT_FAKE_LCM_X);
	int g_lcm_y = disp_helper_get_option(DISP_OPT_FAKE_LCM_Y);

	DISPFUNC();

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		if (disp_helper_get_option(DISP_OPT_SHADOW_REGISTER)) {
			if (disp_helper_get_option(DISP_OPT_SHADOW_MODE) == 0) {
				/* full shadow mode */
			} else if (disp_helper_get_option(DISP_OPT_SHADOW_MODE) == 1) {
				/* force commit */
				DSI_OUTREGBIT(cmdq, struct DSI_SHADOW_DBG_REG,
							  DSI_REG[i]->DSI_SHADOW_DBG, FORCE_COMMIT, 1);
			} else if (disp_helper_get_option(DISP_OPT_SHADOW_MODE) == 2) {
				/* bypass shadow */
				DSI_OUTREGBIT(cmdq, struct DSI_SHADOW_DBG_REG,
							  DSI_REG[i]->DSI_SHADOW_DBG, BYPASS_SHADOW, 1);
			}
			/* read shadow */
			DSI_OUTREGBIT(cmdq, struct DSI_SHADOW_DBG_REG, DSI_REG[i]->DSI_SHADOW_DBG, READ_WRK_REG, 0);
		}
	}

	if (module == DISP_MODULE_DSIDUAL) {
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[0]->DSI_COM_CTRL, DSI_DUAL_EN, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[1]->DSI_COM_CTRL, DSI_DUAL_EN, 0);

		DSI_Send_ROI(DISP_MODULE_DSI0, cmdq, g_lcm_x, g_lcm_y, _dsi_context[i].lcm_width,
			     _dsi_context[i].lcm_height);

		/* must set DSI_START to 0 before set dsi_dual_en, don't know why.2014.02.15 */
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[0]->DSI_START, DSI_START, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[1]->DSI_START, DSI_START, 0);

		if (_dsi_context[i].dsi_params.mode != CMD_MODE) {
			DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[0]->DSI_COM_CTRL, DSI_DUAL_EN, 1);
			DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[1]->DSI_COM_CTRL, DSI_DUAL_EN, 1);
		}

		DSI_SetMode(module, cmdq, _dsi_context[i].dsi_params.mode);
		DSI_clk_HS_mode(module, cmdq, TRUE);

	} else if (module == DISP_MODULE_DSI0) {
		DSI_Send_ROI(module, cmdq, g_lcm_x, g_lcm_y, _dsi_context[0].lcm_width,
			     _dsi_context[0].lcm_height);
		DSI_SetMode(module, cmdq, _dsi_context[0].dsi_params.mode);
		DSI_clk_HS_mode(module, cmdq, TRUE);
	}

	return 0;
}

static int dsi_stop_vdo_mode(enum DISP_MODULE_ENUM module, void *cmdq_handle)
{
	/* use cmdq to stop dsi vdo mode */
	/* set dsi cmd mode */
	DSI_SetMode(module, cmdq_handle, CMD_MODE);

	/* need do reset DSI_DUAL_EN/DSI_START */
	DSI_Stop(module, cmdq_handle);

	/* polling dsi not busy */
	DSI_WaitForNotBusy(module, cmdq_handle);
	return 0;
}

int ddp_dsi_stop(enum DISP_MODULE_ENUM module, void *cmdq_handle)
{
	DISPFUNC();

	if (_dsi_is_video_mode(module)) {
		dsi_stop_vdo_mode(module, cmdq_handle);
	} else {
		/* TODO: modify this with wait event */
		DSI_WaitForNotBusy(module, cmdq_handle);
		DSI_Stop(module, cmdq_handle);
	}
	DSI_clk_HS_mode(module, cmdq_handle, FALSE);
	return 0;
}

#if 0
int ddp_dsi_switch_lcm_mode(enum DISP_MODULE_ENUM module, void *params)
{
	int i = 0;
	LCM_DSI_MODE_SWITCH_CMD lcm_cmd = *((LCM_DSI_MODE_SWITCH_CMD *) (params));
	int mode = (int)(lcm_cmd.mode);

	if (dsi_currect_mode == mode) {
		DDPMSG("[ddp_dsi_switch_mode] not need switch mode, current mode = %d, switch to %d\n",
		dsi_currect_mode, mode);
		return 0;
	}
	if (lcm_cmd.cmd_if == (unsigned int)LCM_INTERFACE_DSI0)
		i = 0;
	else if (lcm_cmd.cmd_if == (unsigned int)LCM_INTERFACE_DSI1)
		i = 1;
	else {
		DDPMSG("dsi switch not support this cmd IF:%d\n", lcm_cmd.cmd_if);
		return -1;
	}

	if (mode == 0) {	/* V2C */
		DSI_OUTREG32(NULL, (unsigned long)(DSI_REG[i]) + 0x130,
				0x00001521 | (lcm_cmd.addr << 16) | (lcm_cmd.val[0] << 24));	/* RM = 1 */
		DSI_OUTREGBIT(NULL, struct DSI_START_REG, DSI_REG[i]->DSI_START, VM_CMD_START, 0);
		DSI_OUTREGBIT(NULL, struct DSI_START_REG, DSI_REG[i]->DSI_START, VM_CMD_START, 1);
		wait_vm_cmd_done = false;
		wait_event_interruptible(_dsi_wait_vm_cmd_done_queue[i], wait_vm_cmd_done);
	}
	return 0;
}
#endif

int ddp_dsi_switch_mode(enum DISP_MODULE_ENUM module, void *cmdq_handle, void *params)
{
	int i = 0;
	LCM_DSI_MODE_SWITCH_CMD lcm_cmd = *((LCM_DSI_MODE_SWITCH_CMD *) (params));
	LCM_DSI_PARAMS *dsi_params = &_dsi_context[0].dsi_params;
	int mode = (int)(lcm_cmd.mode);
	int wait_count = 100;

	if (dsi_currect_mode == mode) {
		DDPMSG
		    ("[ddp_dsi_switch_mode] not need switch mode, current mode = %d, switch to %d\n",
		     dsi_currect_mode, mode);
		return 0;
	}
	if (lcm_cmd.cmd_if == (unsigned int)LCM_INTERFACE_DSI0)
		i = 0;
	else if (lcm_cmd.cmd_if == (unsigned int)LCM_INTERFACE_DSI1)
		i = 1;
	else {
		DDPMSG("dsi switch not support this cmd IF:%d\n", lcm_cmd.cmd_if);
		/* return -1; */
	}

	if (mode == 0) {	/* V2C */
		DISPMSG("[C2V]v2c switch begin\n");
#if 0
		/* 1. enable dsi auto rack */
		DSI_SetBypassRack(module, cmdq_handle, 1);

		/* 2. DDIC GRAM ON */
		DSI_OUTREG32(cmdq_handle, &DSI_REG[i]->DSI_VM_CMD_CON,
			0x00001521|(lcm_cmd.addr << 16)|(lcm_cmd.val[0] << 24)); /* RM = 1 */
		DSI_OUTREGBIT(cmdq_handle, struct DSI_START_REG, DSI_REG[i]->DSI_START, VM_CMD_START, 0);
		DSI_OUTREGBIT(cmdq_handle, struct DSI_START_REG, DSI_REG[i]->DSI_START, VM_CMD_START, 1);
		/* may clear by cpu */
		DSI_POLLREG32(cmdq_handle, &DSI_REG[i]->DSI_INTSTA, 0x00000020, 0x20);

		/* 3. enable dsi frame mode */
		DSI_SetVdoFrmMode(module, cmdq_handle, 1);
		DSI_POLLREG32(cmdq_handle, &DSI_REG[i]->DSI_INTSTA, 0x80000000, 0x0);

		/* 4. flush another frame to DDIC, useless? */
		DSI_OUTREG32(cmdq_handle, &DSI_REG[i]->DSI_CMDQ_SIZE, 0);
		DSI_Start(module, cmdq_handle);
		DSI_POLLREG32(cmdq_handle, &DSI_REG[i]->DSI_INTSTA, 0x00000008, 0x8);

		/* 5. v2c switch on */
		DSI_SetSwitchMode(module, cmdq_handle, 0);

		DSI_OUTREG32(cmdq_handle, &DSI_REG[i]->DSI_VM_CMD_CON,
				0x00001539|(lcm_cmd.addr << 16)|(lcm_cmd.val[1] << 24)); /* DM = 0 */
		DSI_OUTREGBIT(cmdq_handle, struct DSI_START_REG, DSI_REG[i]->DSI_START, VM_CMD_START, 0);
		DSI_OUTREGBIT(cmdq_handle, struct DSI_START_REG, DSI_REG[i]->DSI_START, VM_CMD_START, 1);
		DSI_Start(module, cmdq_handle);

		/* 6. polling dsi idle -- vdo mode over */
		DSI_SetMode(module, cmdq_handle, 0);
		DSI_POLLREG32(cmdq_handle, &DSI_REG[i]->DSI_INTSTA, 0x80000000, 0x0);

		/* 7. mutex setting -- cmd mode */
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_RST, 0x1, 0x1); /* reset mutex for V2C */
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_RST, 0x1, 0x0);
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_SOF, 0x7, 0x0); /* mutex to cmd  mode */
		if (disp_helper_get_option(DISP_OPT_MUTEX_EOF_EN_FOR_CMD_MODE))
			DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_SOF, 0x1c0, 0x40); /* eof */

		/* 8. cmd mode setting */
		DSI_SetVdoFrmMode(module, cmdq_handle, 0);
		DSI_OUTREG32(cmdq_handle, &DSI_CMDQ_REG[i]->data[0], 0x002c3909);
		DSI_OUTREG32(cmdq_handle, &DSI_REG[i]->DSI_CMDQ_SIZE, 1);

		/* te_rdy irq enable in dsi config */
		DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, TE_RDY, 1);
		DSI_OUTREGBIT(cmdq_handle, DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, EXT_TE_EN, 1);
		/* 9. blocking flush */
		cmdqRecFlush(cmdq_handle);
		cmdqRecReset(cmdq_handle);

		dsi_analysis(module);
		DSI_DumpRegisters(module, 2);

		/* 10. disable dsi auto rack*/
		/*DSI_SetBypassRack(module, NULL, 0);*/
#else
		/* 1. polling dsi idle -- vdo mode over */
		DSI_SetMode(module, cmdq_handle, 0);
		DSI_POLLREG32(cmdq_handle, &DSI_REG[i]->DSI_INTSTA, 0x80000000, 0x0);
		DSI_OUTREGBIT(cmdq_handle, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
								  RG_DSI0_MPPLL_SDM_SSC_EN, 1);
		dsi_params->ssc_disable = 0;

		/* 2. mutex setting -- cmd mode */
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_RST, 0x1, 0x1); /* reset mutex for V2C */
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_RST, 0x1, 0x0);
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_SOF, 0x7, 0x0); /* mutex to cmd  mode */
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_SOF, 0x0, 0x40); /* eof */

		if (disp_helper_get_option(DISP_OPT_MUTEX_EOF_EN_FOR_CMD_MODE))
			DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_SOF, 0x1c0, 0x40); /* eof */

		/* 3.te_rdy irq enable in dsi config */
		DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, TE_RDY, 1);
		DSI_OUTREGBIT(cmdq_handle, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, EXT_TE_EN, 1);

		/* 4.Set packet_size_mult */
		if (dsi_params->packet_size_mult) {
			unsigned int ps_wc = 0, h = 0;

			h = DSI_INREG32(struct DSI_VACT_NL_REG, &DSI_REG[i]->DSI_VACT_NL);
			h /= dsi_params->packet_size_mult;
			DSI_OUTREGBIT(cmdq_handle, struct DSI_VACT_NL_REG, DSI_REG[i]->DSI_VACT_NL, VACT_NL, h);
			ps_wc = DSI_INREG32(struct DSI_PSCTRL_REG, &DSI_REG[i]->DSI_PSCTRL);
			ps_wc *= dsi_params->packet_size_mult;
			DSI_OUTREGBIT(cmdq_handle, struct DSI_PSCTRL_REG, DSI_REG[i]->DSI_PSCTRL, DSI_PS_WC, ps_wc);
		}

		/* 5. Adjust PLL clk */
		DSI_DisableClk(module, cmdq_handle);
		DSI_PHY_clk_change(module, cmdq_handle, dsi_params);
		DSI_EnableClk(module, cmdq_handle);
		DSI_PHY_TIMCONFIG(module, cmdq_handle, dsi_params);

		/* 6.update one frame */
		DSI_OUTREG32(cmdq_handle, &DSI_CMDQ_REG[i]->data[0], 0x002c3909);
		DSI_OUTREG32(cmdq_handle, &DSI_REG[i]->DSI_CMDQ_SIZE, 1);
		cmdqRecClearEventToken(cmdq_handle, CMDQ_EVENT_DISP_RDMA0_EOF);
		DISP_REG_SET(cmdq_handle, DISP_REG_CONFIG_MUTEX0_EN, 1);
		DSI_Start(module, cmdq_handle);
		cmdqRecWaitNoClear(cmdq_handle, CMDQ_EVENT_DISP_RDMA0_EOF);

		/* 7. blocking flush */
		cmdqRecFlush(cmdq_handle);
		cmdqRecReset(cmdq_handle);
#endif
		dsi_analysis(module);
		DSI_DumpRegisters(module, 2);

		DISPMSG("[C2V]v2c switch finished\n");
	} else {		/* C2V */
		DISPMSG("[C2V]c2v switch begin\n");
		/* 1. Adjust PLL clk */
		cmdqRecWaitNoClear(cmdq_handle, CMDQ_SYNC_TOKEN_STREAM_EOF);
		DSI_DisableClk(module, cmdq_handle);
		DSI_PHY_clk_change(module, cmdq_handle, dsi_params);
		DSI_EnableClk(module, cmdq_handle);
		DSI_PHY_TIMCONFIG(module, cmdq_handle, dsi_params);

		/* 2. wait TE */
		cmdqRecClearEventToken(cmdq_handle, CMDQ_EVENT_DSI_TE);
		cmdqRecWait(cmdq_handle, CMDQ_EVENT_DSI_TE);

		/* 3. disable DSI EXT TE, only BTA te could work, reason unknown */
		DSI_OUTREGBIT(cmdq_handle, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, EXT_TE_EN, 0);
		DSI_OUTREGBIT(cmdq_handle, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[i]->MIPITX_DSI_PLL_CON1,
								  RG_DSI0_MPPLL_SDM_SSC_EN, 0);
		dsi_params->ssc_disable = 1;

		/* 4. change to vdo mode */
		DSI_SetMode(module, cmdq_handle, mode);
		/* DSI_SetSwitchMode(module, cmdq_handle, 1); */
		/* DSI_SetBypassRack(module, cmdq_handle, 1); */

		/* 4. c2v switch on*/
		/*DSI_OUTREG32(cmdq_handle,  &DSI_CMDQ_REG[i]->data[0], 0x00000020);*/
		/*DSI_OUTREG32(cmdq_handle,  &DSI_CMDQ_REG[i]->data[1],*/
		/*		0x00001500 |(lcm_cmd.addr << 16)|(lcm_cmd.val[0]<< 24));*/
		/*DSI_OUTREG32(cmdq_handle,  &DSI_REG[i]->DSI_CMDQ_SIZE, 2);*/
		/*DSI_Start(module, cmdq_handle);*/

		/* 5. mutex setting */
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_SOF, 0x7, 0x1); /* sof */
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_SOF, 0x1c0, 0x40); /* eof */

		/* 6.Disable packet_size_mult */
		if (dsi_params->packet_size_mult) {
			unsigned int ps_wc = 0, h = 0;

			h = DSI_INREG32(struct DSI_VACT_NL_REG, &DSI_REG[i]->DSI_VACT_NL);
			h *= dsi_params->packet_size_mult;
			DSI_OUTREGBIT(cmdq_handle, struct DSI_VACT_NL_REG, DSI_REG[i]->DSI_VACT_NL, VACT_NL, h);
			ps_wc = DSI_INREG32(struct DSI_PSCTRL_REG, &DSI_REG[i]->DSI_PSCTRL);
			ps_wc /= dsi_params->packet_size_mult;
			DSI_OUTREGBIT(cmdq_handle, struct DSI_PSCTRL_REG, DSI_REG[i]->DSI_PSCTRL, DSI_PS_WC, ps_wc);
		}

		/* 7. trigger vdo mode frame update */
		DSI_MASKREG32(cmdq_handle, DISP_REG_CONFIG_MUTEX0_EN, 0x1, 0x1); /* release mutex for video mode */
		DSI_Start(module, cmdq_handle);

		/* 8. blocking flush */
		cmdqRecFlush(cmdq_handle);
		cmdqRecReset(cmdq_handle);

		DISPINFO("[C2V]after c2v switch, cmdq flushed\n");

		/* THIS IS NOT A GOOD DESIGN!!!!!*/
		/*TEMP WORKAROUND FOR ESD/CV SWITCH */
		/******************************************************************/
		while (wait_count) {
			DISPDBG("[C2V]wait loop %d\n", wait_count);
			if (DSI_REG[i]->DSI_STATE_DBG6.CMTRL_STATE == 0x1) {
				DISPDBG("[C2V]after c2v switch, dsi fsm is idle\n");
				break;
			}
			lcm_mdelay(1);
			wait_count--;
		}

		if (wait_count == 0) {
			DISPERR("[C2V]after c2v switch, dsi state is not idle[0x%08x]\n",
					DSI_REG[i]->DSI_STATE_DBG6.CMTRL_STATE);
			dsi_analysis(module);
			DSI_DumpRegisters(module, 2);
			DSI_Reset(module, NULL);
			DSI_OUTREGBIT(NULL, struct DSI_MODE_CTRL_REG, DSI_REG[i]->DSI_MODE_CTRL,
					C2V_SWITCH_ON, 0);
			DSI_OUTREG32(NULL,	&DSI_REG[i]->DSI_CMDQ_SIZE, 0);
			DSI_Start(module, NULL);

		}
		/******************************************************************/

		dsi_analysis(module);
		DSI_DumpRegisters(module, 2);

		/* 8. disable dsi auto rack  */
		/* DSI_SetBypassRack(module, NULL, 0); */

		DISPMSG("[C2V]c2v switch finished\n");

	}
	dsi_currect_mode = mode;
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
		_dsi_context[i].dsi_params.mode = mode;
	return 0;
}

int ddp_dsi_clk_on(enum DISP_MODULE_ENUM module, void *cmdq_handle, unsigned int level)
{
	int ret = 0;

	return ret;
}

int ddp_dsi_clk_off(enum DISP_MODULE_ENUM module, void *cmdq_handle, unsigned int level)
{
	int ret = 0;

	return ret;
}

int ddp_dsi_ioctl(enum DISP_MODULE_ENUM module, void *cmdq_handle, unsigned int ioctl_cmd,
		  unsigned long *params)
{
	int ret = 0;
	enum DDP_IOCTL_NAME ioctl = (enum DDP_IOCTL_NAME)ioctl_cmd;

	/* DISPFUNC(); */
	switch (ioctl) {
	case DDP_STOP_VIDEO_MODE:
		{
			ret = dsi_stop_vdo_mode(module, cmdq_handle);
			break;
		}

	case DDP_SWITCH_DSI_MODE:
		{
			ret = ddp_dsi_switch_mode(module, cmdq_handle, params);
			break;
		}
	case DDP_SWITCH_LCM_MODE:
		{
			/* ret = ddp_dsi_switch_lcm_mode(module, params); */
			break;
		}
	case DDP_BACK_LIGHT:
		{
			unsigned int cmd = 0x51;
			unsigned int count = 1;
			unsigned int level = params[0];

			DDPMSG("[ddp_dsi_ioctl] level = %d\n", level);
			DSI_set_cmdq_V2(module, cmdq_handle, cmd, count, (unsigned char *)&level, 1);
			break;
		}
	case DDP_DSI_IDLE_CLK_CLOSED:
		{
			unsigned int idle_cmd = params[0];

			if (idle_cmd == 0)
				ddp_dsi_clk_off(module, cmdq_handle, 0);
			else
				ddp_dsi_clk_off(module, cmdq_handle, idle_cmd);
			break;
		}
	case DDP_DSI_IDLE_CLK_OPEN:
		{
			unsigned int idle_cmd = params[0];

			if (idle_cmd == 0)
				ddp_dsi_clk_on(module, cmdq_handle, 0);
			else
				ddp_dsi_clk_on(module, cmdq_handle, idle_cmd);

			break;
		}
	case DDP_DSI_PORCH_CHANGE:
		{
			unsigned int vfp = params[0];

			ddp_dsi_porch_setting(module,
			cmdq_handle, DSI_VFP, vfp);
			break;
		}
	case DDP_DSI_PORCH_ADDR:
		{
			if (params)
				DSI_Get_Porch_Addr(module, params);

			break;
		}
	case DDP_PHY_CLK_CHANGE:
		{
			LCM_DSI_PARAMS *dsi_params = &_dsi_context[0].dsi_params;

			dsi_params->PLL_CLOCK = *params;
			/*DSI_WaitForNotBusy(module, cmdq_handle);*/
			DSI_DisableClk(module, cmdq_handle);
			DSI_PHY_clk_change(module, cmdq_handle, dsi_params);
			DSI_EnableClk(module, cmdq_handle);
			DSI_PHY_TIMCONFIG(module, cmdq_handle, dsi_params);
			break;
		}
	case DDP_UPDATE_PLL_CLK_ONLY:
		{
			LCM_DSI_PARAMS *dsi_params = &_dsi_context[0].dsi_params;

			dsi_params->PLL_CLOCK = *params;
			break;
		}
	case DDP_PARTIAL_UPDATE:
		{
			ddp_dsi_update_partial(module, cmdq_handle, params);
			break;
		}
#if 0
	case DDP_ENTER_ULPS:
		{
			DSI_Enter_ULPS(module, cmdq_handle);
			break;
		}
	case DDP_EXIT_ULPS:
		{
			DSI_Exit_ULPS(module, cmdq_handle);
			break;
		}
#endif
	default:
		break;
	}
	return ret;
}

int ddp_dsi_trigger(enum DISP_MODULE_ENUM module, void *cmdq)
{
	int i = 0;
	unsigned int data_array[16];

	if (_dsi_context[i].dsi_params.mode == CMD_MODE) {
		data_array[0] = 0x002c3909;
		DSI_set_cmdq(module, cmdq, data_array, 1, 0);

		if (module == DISP_MODULE_DSIDUAL) {
			/*
			 * DSI1 is only used for triggering video data; thus pull up DSI_DUAL_EN,
			 * and pull down DSI_DUAL_EN after triggering video data is done.
			 */
			DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[0]->DSI_START, DSI_START, 0);
			DSI_OUTREGBIT(cmdq, struct DSI_START_REG, DSI_REG[1]->DSI_START, DSI_START, 0);
			DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[0]->DSI_COM_CTRL, DSI_DUAL_EN, 1);
			DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[1]->DSI_COM_CTRL, DSI_DUAL_EN, 1);
		}
	}

	DSI_Start(module, cmdq);

	if (module == DISP_MODULE_DSIDUAL && _dsi_context[i].dsi_params.mode == CMD_MODE) {
		/* Reading one reg is only used for delay in order to pull down DSI_DUAL_EN. */
		if (cmdq)
			cmdqRecBackupRegisterToSlot(cmdq, _h_intstat, 0,
				disp_addr_convert((unsigned long)(&DSI_REG[0]->DSI_INTSTA)));
		else
			INREG32(&DSI_REG[0]->DSI_INTSTA);

		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[0]->DSI_COM_CTRL, DSI_DUAL_EN, 0);
		DSI_OUTREGBIT(cmdq, struct DSI_COM_CTRL_REG, DSI_REG[1]->DSI_COM_CTRL, DSI_DUAL_EN, 0);
	}

	return 0;
}

int ddp_dsi_reset(enum DISP_MODULE_ENUM module, void *cmdq_handle)
{
	DSI_Reset(module, cmdq_handle);

	return 0;
}

int ddp_dsi_power_on(enum DISP_MODULE_ENUM module, void *cmdq_handle)
{
#ifdef ENABLE_CLK_MGR
	int ret = 0;
	int i = 0;
#endif
	DISPFUNC();

	/* DSI_DumpRegisters(module,1); */
	if (!s_isDsiPowerOn) {
#ifdef ENABLE_CLK_MGR
#ifdef CONFIG_MTK_CLKMGR
		set_mipi26m(1);
#else
		ddp_set_mipi26m(module, 1);
#endif
		if (is_ipoh_bootup) {
			pr_err("IPOH bootup ??!! This feature is back again??!!\n");
			ASSERT(0);
		}
		DSI_PHY_clk_switch(module, NULL, true);

		if (module == DISP_MODULE_DSI0 || module == DISP_MODULE_DSIDUAL) {
#ifndef CONFIG_MTK_CLKMGR
			ret += ddp_clk_enable(DISP1_DSI0_MM_CLOCK);
			ret += ddp_clk_enable(DISP1_DSI0_INTERFACE_CLOCK);
#endif
			if (ret > 0) {
				DISP_LOG_PRINT(ANDROID_LOG_WARN, "DSI",
					       "DSI0 power manager API return FALSE\n");
			}
		}

		if (module == DISP_MODULE_DSI1 || module == DISP_MODULE_DSIDUAL) {
#ifndef CONFIG_MTK_CLKMGR
			ret += ddp_clk_enable(DISP1_DSI1_MM_CLOCK);
			ret += ddp_clk_enable(DISP1_DSI1_INTERFACE_CLOCK);
#endif
			if (ret > 0) {
				DISP_LOG_PRINT(ANDROID_LOG_WARN, "DSI",
					       "DSI1 power manager API return FALSE\n");
			}
		}
		__close_dsi_default_clock(module);

		/* restore dsi register */
		DSI_RestoreRegisters(module, NULL);

		/* enable sleep-out mode */
		DSI_SleepOut(module, NULL);

		/* restore lane_num */
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
			struct DSI_REGS *regs = NULL;

			regs = &(_dsi_context[i].regBackup);
			DSI_OUTREG32(NULL, &DSI_REG[i]->DSI_TXRX_CTRL,
				     AS_UINT32(&regs->DSI_TXRX_CTRL));
		}
		/* enter wakeup */
		DSI_Wakeup(module, NULL);

		/* enable clock */
		DSI_EnableClk(module, NULL);

		DSI_Reset(module, NULL);
#endif
		s_isDsiPowerOn = TRUE;
	}
	/* DSI_DumpRegisters(module,1); */
	return DSI_STATUS_OK;
}


int ddp_dsi_power_off(enum DISP_MODULE_ENUM module, void *cmdq_handle)
{
	int i = 0;
#ifdef ENABLE_CLK_MGR
	int ret = 0;
	unsigned int value = 0;
#endif
	DISPFUNC();
	/* DSI_DumpRegisters(module,1); */

	if (s_isDsiPowerOn) {
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
			/*disable TE when power off */
			DSI_OUTREGBIT(NULL, struct DSI_INT_ENABLE_REG, DSI_REG[i]->DSI_INTEN, TE_RDY, 0);
		}
		DSI_BackupRegisters(module, NULL);
#ifdef ENABLE_CLK_MGR
		/* disable HS mode */
		DSI_clk_HS_mode(module, NULL, FALSE);
		/* enter ULPS mode */
		DSI_lane0_ULP_mode(module, NULL, 1);
		DSI_clk_ULP_mode(module, NULL, 1);
		/* make sure enter ulps mode */
		while (1) {
			mdelay(1);
			value = INREG32(&DSI_REG[0]->DSI_STATE_DBG1);
			value = value >> 24;
			if (value == 0x20)
				break;
			DDPMSG("dsi not in ulps mode, try again...\n");
		}
		/* clear lane_num when enter ulps */
		for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++)
			DSI_OUTREGBIT(NULL, struct DSI_TXRX_CTRL_REG, DSI_REG[i]->DSI_TXRX_CTRL, LANE_NUM, 0);

		/* disable clock */
		DSI_DisableClk(module, NULL);

		if (module == DISP_MODULE_DSI0 || module == DISP_MODULE_DSIDUAL) {
#ifndef CONFIG_MTK_CLKMGR
			ddp_clk_disable(DISP1_DSI0_MM_CLOCK);
			ddp_clk_disable(DISP1_DSI0_INTERFACE_CLOCK);
#endif
			if (ret > 0) {
				DISP_LOG_PRINT(ANDROID_LOG_WARN, "DSI",
					       "DSI0 power manager API return FALSE\n");
			}
		}

		if (module == DISP_MODULE_DSI1 || module == DISP_MODULE_DSIDUAL) {
#ifndef CONFIG_MTK_CLKMGR
			ddp_clk_disable(DISP1_DSI1_MM_CLOCK);
			ddp_clk_disable(DISP1_DSI1_INTERFACE_CLOCK);
#endif
			if (ret > 0) {
				DISP_LOG_PRINT(ANDROID_LOG_WARN, "DSI",
					       "DSI1 power manager API return FALSE\n");
			}
		}

		/* disable mipi pll */
		DSI_PHY_clk_switch(module, NULL, false);
#ifdef CONFIG_MTK_CLKMGR
		set_mipi26m(0);
#else
		ddp_set_mipi26m(module, 0);
#endif

#endif
		s_isDsiPowerOn = FALSE;
	}
	/* DSI_DumpRegisters(module,1); */
	return DSI_STATUS_OK;
}


int ddp_dsi_is_busy(enum DISP_MODULE_ENUM module)
{
	int i = 0;
	int busy = 0;
	struct DSI_INT_STATUS_REG status;
	/* DISPFUNC(); */

	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
		status = DSI_REG[i]->DSI_INTSTA;

		if (status.BUSY)
			busy++;
	}

	DISPDBG("%s is %s\n", ddp_get_module_name(module), busy ? "busy" : "idle");
	return busy;
}

int ddp_dsi_is_idle(enum DISP_MODULE_ENUM module)
{
	return !ddp_dsi_is_busy(module);
}

static const char *dsi_mode_spy(LCM_DSI_MODE_CON mode)
{
	switch (mode) {
	case CMD_MODE:
		return "CMD_MODE";
	case SYNC_PULSE_VDO_MODE:
		return "SYNC_PULSE_VDO_MODE";
	case SYNC_EVENT_VDO_MODE:
		return "SYNC_EVENT_VDO_MODE";
	case BURST_VDO_MODE:
		return "BURST_VDO_MODE";
	default:
		return "unknown";
	}
}

void dsi_analysis(enum DISP_MODULE_ENUM module)
{
	int i = 0;

	DDPDUMP("== DISP DSI ANALYSIS ==\n");
	for (i = DSI_MODULE_BEGIN(module); i <= DSI_MODULE_END(module); i++) {
#ifndef CONFIG_FPGA_EARLY_PORTING
		DDPDUMP("MIPITX Clock: %d\n", dsi_phy_get_clk(module));
#endif
		DDPDUMP("DSI%d Start:%x, Busy:%d, DSI_DUAL_EN:%d, MODE:%s, High Speed:%d, FSM State:%s\n",
			i, DSI_REG[i]->DSI_START.DSI_START, DSI_REG[i]->DSI_INTSTA.BUSY,
			DSI_REG[i]->DSI_COM_CTRL.DSI_DUAL_EN, dsi_mode_spy(DSI_REG[i]->DSI_MODE_CTRL.MODE),
			DSI_REG[i]->DSI_PHY_LCCON.LC_HS_TX_EN,
			_dsi_cmd_mode_parse_state(DSI_REG[i]->DSI_STATE_DBG6.CMTRL_STATE));

		DDPDUMP("DSI%d IRQ,RD_RDY:%d, CMD_DONE:%d, SLEEPOUT_DONE:%d, TE_RDY:%d, VM_CMD_DONE:%d, VM_DONE:%d\n",
			i, DSI_REG[i]->DSI_INTSTA.RD_RDY, DSI_REG[i]->DSI_INTSTA.CMD_DONE,
			DSI_REG[i]->DSI_INTSTA.SLEEPOUT_DONE, DSI_REG[i]->DSI_INTSTA.TE_RDY,
			DSI_REG[i]->DSI_INTSTA.VM_CMD_DONE, DSI_REG[i]->DSI_INTSTA.VM_DONE);

		DDPDUMP("DSI%d Lane Num:%d, Ext_TE_EN:%d, Ext_TE_Edge:%d, HSTX_CKLP_EN:%d\n", i,
			DSI_REG[i]->DSI_TXRX_CTRL.LANE_NUM,
			DSI_REG[i]->DSI_TXRX_CTRL.EXT_TE_EN,
			DSI_REG[i]->DSI_TXRX_CTRL.EXT_TE_EDGE,
			DSI_REG[i]->DSI_TXRX_CTRL.HSTX_CKLP_EN);

		DDPDUMP("DSI%d LFR En:%d, LFR MODE:%d, LFR TYPE:%d, LFR SKIP NUMBER:%d\n", i,
			DSI_REG[i]->DSI_LFR_CON.LFR_EN,
			DSI_REG[i]->DSI_LFR_CON.LFR_MODE,
			DSI_REG[i]->DSI_LFR_CON.LFR_TYPE, DSI_REG[i]->DSI_LFR_CON.LFR_SKIP_NUM);
	}
}

int ddp_dsi_dump(enum DISP_MODULE_ENUM module, int level)
{
	if (!s_isDsiPowerOn) {
		DISPERR("sleep dump is invalid\n");
		return 0;
	}

	dsi_analysis(module);
	DSI_DumpRegisters(module, level);

	return 0;
}

int ddp_dsi_build_cmdq(enum DISP_MODULE_ENUM module, void *cmdq_trigger_handle, enum CMDQ_STATE state)
{
	int ret = 0;
	int i = 0;
	int dsi_i = 0;
	LCM_DSI_PARAMS *dsi_params = NULL;
	struct DSI_T0_INS t0;
	struct DSI_RX_DATA_REG read_data0;
	static cmdqBackupSlotHandle hSlot;

	if (module == DISP_MODULE_DSIDUAL)
		dsi_i = 0;
	else
		dsi_i = DSI_MODULE_to_ID(module);
	dsi_params = &_dsi_context[dsi_i].dsi_params;

	if (cmdq_trigger_handle == NULL) {
		DISPERR("cmdq_trigger_handle is NULL\n");
		return -1;
	}

	if (state == CMDQ_WAIT_LCM_TE) {
		/* need waiting te */
		if (module == DISP_MODULE_DSI0 || module == DISP_MODULE_DSIDUAL) {
			if (dsi0_te_enable == 0)
				return 0;

			if (disp_helper_get_option(DISP_OPT_USE_CMDQ)) {
				ret =
				    cmdqRecClearEventToken(cmdq_trigger_handle, CMDQ_EVENT_DSI_TE);
				ret = cmdqRecWait(cmdq_trigger_handle, CMDQ_EVENT_DSI_TE);
			}
		} else {
			DISPERR("wrong module: %s\n", ddp_get_module_name(module));
			return -1;
		}
	} else if (state == CMDQ_CHECK_IDLE_AFTER_STREAM_EOF) {
		/* need waiting te */
		if (module == DISP_MODULE_DSI0 || module == DISP_MODULE_DSIDUAL) {
			DSI_POLLREG32(cmdq_trigger_handle, &DSI_REG[dsi_i]->DSI_INTSTA, 0x80000000,
				      0);
		}

		else {
			DISPERR("wrong module: %s\n", ddp_get_module_name(module));
			return -1;
		}
	} else if (state == CMDQ_ESD_CHECK_READ) {
		/* enable dsi interrupt: RD_RDY/CMD_DONE (need do this here?) */
		DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_INT_ENABLE_REG, DSI_REG[dsi_i]->DSI_INTEN,
			      RD_RDY, 1);
		DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_INT_ENABLE_REG, DSI_REG[dsi_i]->DSI_INTEN,
			      CMD_DONE, 1);

		for (i = 0; i < 3; i++) {
			if (dsi_params->lcm_esd_check_table[i].cmd == 0)
				break;
			/* 0. send read lcm command(short packet) */
			t0.CONFG = 0x04;	/* /BTA */
			t0.Data0 = dsi_params->lcm_esd_check_table[i].cmd;
			/* / 0xB0 is used to distinguish DCS cmd or Gerneric cmd, is that Right??? */
			t0.Data_ID =
			    (t0.Data0 <
			     0xB0) ? DSI_DCS_READ_PACKET_ID : DSI_GERNERIC_READ_LONG_PACKET_ID;
			t0.Data1 = 0;

			/* write DSI CMDQ */
			DSI_OUTREG32(cmdq_trigger_handle, &DSI_CMDQ_REG[dsi_i]->data[0],
				     0x00013700);
			DSI_OUTREG32(cmdq_trigger_handle, &DSI_CMDQ_REG[dsi_i]->data[1],
				     AS_UINT32(&t0));
			DSI_OUTREG32(cmdq_trigger_handle, &DSI_REG[dsi_i]->DSI_CMDQ_SIZE, 2);

			/* start DSI */
			DSI_OUTREG32(cmdq_trigger_handle, &DSI_REG[dsi_i]->DSI_START, 0);
			DSI_OUTREG32(cmdq_trigger_handle, &DSI_REG[dsi_i]->DSI_START, 1);

			/* 1. wait DSI RD_RDY(must clear, in case of cpu RD_RDY interrupt handler) */
			if (dsi_i == 0) {
				DSI_POLLREG32(cmdq_trigger_handle, &DSI_REG[dsi_i]->DSI_INTSTA,
					      0x00000001, 0x1);
				DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_INT_STATUS_REG,
					      DSI_REG[dsi_i]->DSI_INTSTA, RD_RDY, 0x00000000);
			}
			/* 2. save RX data */
			if (hSlot) {
				DSI_BACKUPREG32(cmdq_trigger_handle, hSlot, i,
						&DSI_REG[0]->DSI_RX_DATA0);
			}
			/* 3. write RX_RACK */
			DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_RACK_REG, DSI_REG[dsi_i]->DSI_RACK,
				      DSI_RACK, 1);

			/* 4. polling not busy(no need clear) */
			if (dsi_i == 0) {
				DSI_POLLREG32(cmdq_trigger_handle, &DSI_REG[dsi_i]->DSI_INTSTA,
					      0x80000000, 0);
			}
			/* loop: 0~4 */
		}

		/* DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_INT_ENABLE_REG,DSI_REG[dsi_i]->DSI_INTEN,RD_RDY,0); */
	} else if (state == CMDQ_ESD_CHECK_CMP) {

		/* cmp just once and only 1 return value */
		for (i = 0; i < 3; i++) {
			if (dsi_params->lcm_esd_check_table[i].cmd == 0)
				break;

			/* read data */
			if (hSlot) {
				/* read from slot */
				cmdqBackupReadSlot(hSlot, i, (uint32_t *)&read_data0);
			} else {
				/* read from dsi , support only one cmd read */
				if (i == 0) {
					DSI_OUTREG32(NULL, &read_data0,
						     AS_UINT32(&DSI_REG[dsi_i]->DSI_RX_DATA0));
				}
			}

			DISPDBG("[DSI]enter cmp read_data0 byte0=0x%x byte1=0x%x byte2=0x%x byte3=0x%x\n",
			     read_data0.byte0, read_data0.byte1, read_data0.byte2,
			     read_data0.byte3);
			DISPDBG("[DSI]enter cmp check_table cmd=0x%x,count=0x%x,para_list[0]=0x%x,para_list[1]=0x%x\n",
			     dsi_params->lcm_esd_check_table[i].cmd,
			     dsi_params->lcm_esd_check_table[i].count,
			     dsi_params->lcm_esd_check_table[i].para_list[0],
			     dsi_params->lcm_esd_check_table[i].para_list[1]);
			DISPDBG("[DSI]enter cmp DSI+0x200=0x%x\n",
				AS_UINT32(DDP_REG_BASE_DSI0 + 0x200));
			DISPDBG("[DSI]enter cmp DSI+0x204=0x%x\n",
				AS_UINT32(DDP_REG_BASE_DSI0 + 0x204));
			DISPDBG("[DSI]enter cmp DSI+0x60=0x%x\n",
				AS_UINT32(DDP_REG_BASE_DSI0 + 0x60));
			DISPDBG("[DSI]enter cmp DSI+0x74=0x%x\n",
				AS_UINT32(DDP_REG_BASE_DSI0 + 0x74));
			DISPDBG("[DSI]enter cmp DSI+0x88=0x%x\n",
				AS_UINT32(DDP_REG_BASE_DSI0 + 0x88));
			DISPDBG("[DSI]enter cmp DSI+0x0c=0x%x\n",
				AS_UINT32(DDP_REG_BASE_DSI0 + 0x0c));

			if (read_data0.byte1 == dsi_params->lcm_esd_check_table[i].para_list[0]) {
				/* clear rx data */
				/* DSI_OUTREG32(NULL, &DSI_REG[dsi_i]->DSI_RX_DATA0,0); */
				ret = 0;	/* esd pass */
			} else {
				/* esd fail */
				DISPERR("[DSI]cmp fail 0x%x != 0x%x\n",
					read_data0.byte1, dsi_params->lcm_esd_check_table[i].para_list[0]);
				ret = 1;
				break;
			}
		}

	} else if (state == CMDQ_ESD_ALLC_SLOT) {
		/* create 3 slot */
		cmdqBackupAllocateSlot(&hSlot, 3);
	} else if (state == CMDQ_ESD_FREE_SLOT) {
		if (hSlot) {
			cmdqBackupFreeSlot(hSlot);
			hSlot = 0;
		}
	} else if (state == CMDQ_STOP_VDO_MODE) {
		dsi_stop_vdo_mode(module, cmdq_trigger_handle);
	} else if (state == CMDQ_START_VDO_MODE) {

		/* 0. dual dsi set DSI_START/DSI_DUAL_EN */
		if (module == DISP_MODULE_DSIDUAL) {
			/* must set DSI_START to 0 before set dsi_dual_en, don't know why.2014.02.15 */
			DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_START_REG, DSI_REG[0]->DSI_START,
				      DSI_START, 0);
			DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_START_REG, DSI_REG[1]->DSI_START,
				      DSI_START, 0);

			DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_COM_CTRL_REG,
				      DSI_REG[0]->DSI_COM_CTRL, DSI_DUAL_EN, 1);
			DSI_OUTREGBIT(cmdq_trigger_handle, struct DSI_COM_CTRL_REG,
				      DSI_REG[1]->DSI_COM_CTRL, DSI_DUAL_EN, 1);

		}
		/* 1. set dsi vdo mode */
		DSI_SetMode(module, cmdq_trigger_handle, dsi_params->mode);

		/* 2. enable HS */
		/* DSI_clk_HS_mode(module, cmdq_trigger_handle, TRUE); */

		/* 3. enable mutex */
		/* ddp_mutex_enable(mutex_id_for_latest_trigger,0,cmdq_trigger_handle); */

		/* 4. start dsi */
		/* DSI_Start(module, cmdq_trigger_handle); */

	} else if (state == CMDQ_DSI_RESET) {
		DISPCHECK("CMDQ Timeout, Reset DSI\n");
		DSI_DumpRegisters(module, 1);
		DSI_Reset(module, NULL);
	} else if (state == CMDQ_DSI_LFR_MODE) {
		if (dsi_params->lfr_mode == 2 || dsi_params->lfr_mode == 3)
			DSI_LFR_UPDATE(module, cmdq_trigger_handle);
	}

	return ret;
}

void *get_dsi_params_handle(UINT32 dsi_idx)
{
	if (dsi_idx != PM_DSI1)
		return (void *)(&_dsi_context[0].dsi_params);
	else
		return (void *)(&_dsi_context[1].dsi_params);
}

INT32 DSI_ssc_enable(UINT32 dsi_index, UINT32 en)
{
	UINT32 disable = en ? 0 : 1;

	if (dsi_index == PM_DSI0) {
		DSI_OUTREGBIT(NULL, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[0]->MIPITX_DSI_PLL_CON1,
			      RG_DSI0_MPPLL_SDM_SSC_EN, en);
		_dsi_context[0].dsi_params.ssc_disable = disable;
	} else if (dsi_index == PM_DSI1) {
		DSI_OUTREGBIT(NULL, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[1]->MIPITX_DSI_PLL_CON1,
			      RG_DSI0_MPPLL_SDM_SSC_EN, en);
		_dsi_context[1].dsi_params.ssc_disable = disable;
	} else if (dsi_index == PM_DSI_DUAL) {
		DSI_OUTREGBIT(NULL, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[0]->MIPITX_DSI_PLL_CON1,
			      RG_DSI0_MPPLL_SDM_SSC_EN, en);
		DSI_OUTREGBIT(NULL, struct MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG[1]->MIPITX_DSI_PLL_CON1,
			      RG_DSI0_MPPLL_SDM_SSC_EN, en);
		_dsi_context[0].dsi_params.ssc_disable = _dsi_context[1].dsi_params.ssc_disable =
		    disable;
	}
	return 0;
}

struct DDP_MODULE_DRIVER ddp_driver_dsi0 = {
	.module = DISP_MODULE_DSI0,
	.init = ddp_dsi_init,
	.deinit = ddp_dsi_deinit,
	.config = ddp_dsi_config,
	.build_cmdq = ddp_dsi_build_cmdq,
	.trigger = ddp_dsi_trigger,
	.start = ddp_dsi_start,
	.stop = ddp_dsi_stop,
	.reset = ddp_dsi_reset,
	.power_on = ddp_dsi_power_on,
	.power_off = ddp_dsi_power_off,
	.is_idle = ddp_dsi_is_idle,
	.is_busy = ddp_dsi_is_busy,
	.dump_info = ddp_dsi_dump,
	.set_lcm_utils = ddp_dsi_set_lcm_utils,
	.ioctl = (int (*)(enum DISP_MODULE_ENUM, void *, enum DDP_IOCTL_NAME, void *))ddp_dsi_ioctl
};

struct DDP_MODULE_DRIVER ddp_driver_dsi1 = {
	.module = DISP_MODULE_DSI1,
	.init = ddp_dsi_init,
	.deinit = ddp_dsi_deinit,
	.config = ddp_dsi_config,
	.build_cmdq = ddp_dsi_build_cmdq,
	.trigger = ddp_dsi_trigger,
	.start = ddp_dsi_start,
	.stop = ddp_dsi_stop,
	.reset = ddp_dsi_reset,
	.power_on = ddp_dsi_power_on,
	.power_off = ddp_dsi_power_off,
	.is_idle = ddp_dsi_is_idle,
	.is_busy = ddp_dsi_is_busy,
	.dump_info = ddp_dsi_dump,
	.set_lcm_utils = ddp_dsi_set_lcm_utils,
	.ioctl = (int (*)(enum DISP_MODULE_ENUM, void *, enum DDP_IOCTL_NAME, void *))ddp_dsi_ioctl
};

struct DDP_MODULE_DRIVER ddp_driver_dsidual = {
	.module = DISP_MODULE_DSIDUAL,
	.init = ddp_dsi_init,
	.deinit = ddp_dsi_deinit,
	.config = ddp_dsi_config,
	.build_cmdq = ddp_dsi_build_cmdq,
	.trigger = ddp_dsi_trigger,
	.start = ddp_dsi_start,
	.stop = ddp_dsi_stop,
	.reset = ddp_dsi_reset,
	.power_on = ddp_dsi_power_on,
	.power_off = ddp_dsi_power_off,
	.is_idle = ddp_dsi_is_idle,
	.is_busy = ddp_dsi_is_busy,
	.dump_info = ddp_dsi_dump,
	.set_lcm_utils = ddp_dsi_set_lcm_utils,
	.ioctl = (int (*)(enum DISP_MODULE_ENUM, void *, enum DDP_IOCTL_NAME, void *))ddp_dsi_ioctl
};

const LCM_UTIL_FUNCS PM_lcm_utils_dsi0 = {
	.set_reset_pin = lcm_set_reset_pin,
	.udelay = lcm_udelay,
	.mdelay = lcm_mdelay,
	.dsi_set_cmdq = DSI_set_cmdq_wrapper_DSI0,
	.dsi_set_cmdq_V2 = DSI_set_cmdq_V2_Wrapper_DSI0
};


/* /////////////////////// Panel Master ////////////////////////////////// */
UINT32 PanelMaster_get_TE_status(UINT32 dsi_idx)
{
	if (dsi_idx == 0)
		return dsi0_te_enable ? 1 : 0;
	/* else */
	/* return dsi1_te_enable ? 1:0 ; */
	return 0;
}

UINT32 PanelMaster_get_CC(UINT32 dsi_idx)
{
	struct DSI_TXRX_CTRL_REG tmp_reg;

	memset(&tmp_reg, 0, sizeof(struct DSI_TXRX_CTRL_REG));

	if ((dsi_idx == PM_DSI0) || (dsi_idx == PM_DSI_DUAL))
		DSI_READREG32(struct DSI_TXRX_CTRL_REG *, &tmp_reg, &DSI_REG[0]->DSI_TXRX_CTRL);
	else if (dsi_idx == PM_DSI1)
		DSI_READREG32(struct DSI_TXRX_CTRL_REG *, &tmp_reg, &DSI_REG[1]->DSI_TXRX_CTRL);
	return tmp_reg.HSTX_CKLP_EN ? 1 : 0;
}

void PanelMaster_set_CC(UINT32 dsi_index, UINT32 enable)
{
	DDPMSG("set_cc :%d\n", enable);
	if (dsi_index == PM_DSI0) {
		DSI_OUTREGBIT(NULL, struct DSI_TXRX_CTRL_REG, DSI_REG[0]->DSI_TXRX_CTRL, HSTX_CKLP_EN,
			      enable);
	} else if (dsi_index == PM_DSI1) {
		DSI_OUTREGBIT(NULL, struct DSI_TXRX_CTRL_REG, DSI_REG[1]->DSI_TXRX_CTRL, HSTX_CKLP_EN,
			      enable);
	} else if (dsi_index == PM_DSI_DUAL) {
		DSI_OUTREGBIT(NULL, struct DSI_TXRX_CTRL_REG, DSI_REG[0]->DSI_TXRX_CTRL, HSTX_CKLP_EN,
			      enable);
		DSI_OUTREGBIT(NULL, struct DSI_TXRX_CTRL_REG, DSI_REG[1]->DSI_TXRX_CTRL, HSTX_CKLP_EN,
			      enable);
	}
}

void PanelMaster_DSI_set_timing(UINT32 dsi_index, struct MIPI_TIMING timing)
{
	UINT32 hbp_byte;
	LCM_DSI_PARAMS *dsi_params;
	int fbconfig_dsiTmpBufBpp = 0;

	if (_dsi_context[dsi_index].dsi_params.data_format.format == LCM_DSI_FORMAT_RGB565)
		fbconfig_dsiTmpBufBpp = 2;
	else
		fbconfig_dsiTmpBufBpp = 3;
	dsi_params = get_dsi_params_handle(dsi_index);
	switch (timing.type) {
	case LPX:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[0]->DSI_PHY_TIMECON0, LPX,
				      timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[1]->DSI_PHY_TIMECON0, LPX,
				      timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[0]->DSI_PHY_TIMECON0, LPX,
				      timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[1]->DSI_PHY_TIMECON0, LPX,
				      timing.value);
		}
		break;
	case HS_PRPR:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[0]->DSI_PHY_TIMECON0,
				      HS_PRPR, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[1]->DSI_PHY_TIMECON0,
				      HS_PRPR, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[0]->DSI_PHY_TIMECON0,
				      HS_PRPR, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[1]->DSI_PHY_TIMECON0,
				      HS_PRPR, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON0_REG,DSI_REG->DSI_PHY_TIMECON0,HS_PRPR,timing.value); */
		break;
	case HS_ZERO:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[0]->DSI_PHY_TIMECON0,
				      HS_ZERO, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[1]->DSI_PHY_TIMECON0,
				      HS_ZERO, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[0]->DSI_PHY_TIMECON0,
				      HS_ZERO, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[1]->DSI_PHY_TIMECON0,
				      HS_ZERO, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON0_REG,DSI_REG->DSI_PHY_TIMECON0,HS_ZERO,timing.value); */
		break;
	case HS_TRAIL:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[0]->DSI_PHY_TIMECON0,
				      HS_TRAIL, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[1]->DSI_PHY_TIMECON0,
				      HS_TRAIL, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[0]->DSI_PHY_TIMECON0,
				      HS_TRAIL, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON0_REG, DSI_REG[1]->DSI_PHY_TIMECON0,
				      HS_TRAIL, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON0_REG,DSI_REG->DSI_PHY_TIMECON0,HS_TRAIL,timing.value); */
		break;
	case TA_GO:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[0]->DSI_PHY_TIMECON1,
				      TA_GO, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[1]->DSI_PHY_TIMECON1,
				      TA_GO, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[0]->DSI_PHY_TIMECON1,
				      TA_GO, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[1]->DSI_PHY_TIMECON1,
				      TA_GO, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON1_REG,DSI_REG->DSI_PHY_TIMECON1,TA_GO,timing.value); */
		break;
	case TA_SURE:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[0]->DSI_PHY_TIMECON1,
				      TA_SURE, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[1]->DSI_PHY_TIMECON1,
				      TA_SURE, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[0]->DSI_PHY_TIMECON1,
				      TA_SURE, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[1]->DSI_PHY_TIMECON1,
				      TA_SURE, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON1_REG,DSI_REG->DSI_PHY_TIMECON1,TA_SURE,timing.value); */
		break;
	case TA_GET:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[0]->DSI_PHY_TIMECON1,
				      TA_GET, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[1]->DSI_PHY_TIMECON1,
				      TA_GET, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[0]->DSI_PHY_TIMECON1,
				      TA_GET, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[1]->DSI_PHY_TIMECON1,
				      TA_GET, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON1_REG,DSI_REG->DSI_PHY_TIMECON1,TA_GET,timing.value); */
		break;
	case DA_HS_EXIT:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[0]->DSI_PHY_TIMECON1,
				      DA_HS_EXIT, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[1]->DSI_PHY_TIMECON1,
				      DA_HS_EXIT, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[0]->DSI_PHY_TIMECON1,
				      DA_HS_EXIT, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON1_REG, DSI_REG[1]->DSI_PHY_TIMECON1,
				      DA_HS_EXIT, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON1_REG,DSI_REG->DSI_PHY_TIMECON1,DA_HS_EXIT,timing.value); */
		break;
	case CONT_DET:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[0]->DSI_PHY_TIMECON2,
				      CONT_DET, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[1]->DSI_PHY_TIMECON2,
				      CONT_DET, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[0]->DSI_PHY_TIMECON2,
				      CONT_DET, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[1]->DSI_PHY_TIMECON2,
				      CONT_DET, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON2_REG,DSI_REG->DSI_PHY_TIMECON2,CONT_DET,timing.value); */
		break;
	case CLK_ZERO:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[0]->DSI_PHY_TIMECON2,
				      CLK_ZERO, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[1]->DSI_PHY_TIMECON2,
				      CLK_ZERO, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[0]->DSI_PHY_TIMECON2,
				      CLK_ZERO, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[1]->DSI_PHY_TIMECON2,
				      CLK_ZERO, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON2_REG,DSI_REG->DSI_PHY_TIMECON2,CLK_ZERO,timing.value); */
		break;
	case CLK_TRAIL:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[0]->DSI_PHY_TIMECON2,
				      CLK_TRAIL, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[1]->DSI_PHY_TIMECON2,
				      CLK_TRAIL, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[0]->DSI_PHY_TIMECON2,
				      CLK_TRAIL, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON2_REG, DSI_REG[1]->DSI_PHY_TIMECON2,
				      CLK_TRAIL, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON2_REG,DSI_REG->DSI_PHY_TIMECON2,CLK_TRAIL,timing.value); */
		break;
	case CLK_HS_PRPR:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[0]->DSI_PHY_TIMECON3,
				      CLK_HS_PRPR, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[1]->DSI_PHY_TIMECON3,
				      CLK_HS_PRPR, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[0]->DSI_PHY_TIMECON3,
				      CLK_HS_PRPR, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[1]->DSI_PHY_TIMECON3,
				      CLK_HS_PRPR, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON3_REG,DSI_REG->DSI_PHY_TIMECON3,CLK_HS_PRPR,timing.value); */
		break;
	case CLK_HS_POST:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[0]->DSI_PHY_TIMECON3,
				      CLK_HS_POST, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[1]->DSI_PHY_TIMECON3,
				      CLK_HS_POST, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[0]->DSI_PHY_TIMECON3,
				      CLK_HS_POST, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[1]->DSI_PHY_TIMECON3,
				      CLK_HS_POST, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON3_REG,DSI_REG->DSI_PHY_TIMECON3,CLK_HS_POST,timing.value); */
		break;
	case CLK_HS_EXIT:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[0]->DSI_PHY_TIMECON3,
				      CLK_HS_EXIT, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[1]->DSI_PHY_TIMECON3,
				      CLK_HS_EXIT, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[0]->DSI_PHY_TIMECON3,
				      CLK_HS_EXIT, timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_PHY_TIMCON3_REG, DSI_REG[1]->DSI_PHY_TIMECON3,
				      CLK_HS_EXIT, timing.value);
		}
		/* OUTREGBIT(DSI_PHY_TIMCON3_REG,DSI_REG->DSI_PHY_TIMECON3,CLK_HS_EXIT,timing.value); */
		break;
	case HPW:
		if (!(dsi_params->mode == SYNC_EVENT_VDO_MODE || dsi_params->mode == BURST_VDO_MODE))
			timing.value = (timing.value * fbconfig_dsiTmpBufBpp - 10);
		timing.value = ALIGN_TO((timing.value), 4);
		if (dsi_index == PM_DSI0) {
			DSI_OUTREG32(NULL, &DSI_REG[0]->DSI_HSA_WC, timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREG32(NULL, &DSI_REG[1]->DSI_HSA_WC, timing.value);
		} else if (dsi_index == PM_DSI_DUAL) {
			DSI_OUTREG32(NULL, &DSI_REG[0]->DSI_HSA_WC, timing.value);
			DSI_OUTREG32(NULL, &DSI_REG[1]->DSI_HSA_WC, timing.value);
		}
		break;
	case HFP:
		timing.value = timing.value * fbconfig_dsiTmpBufBpp - 12;
		timing.value = ALIGN_TO(timing.value, 4);
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_HFP_WC_REG, DSI_REG[0]->DSI_HFP_WC, HFP_WC,
				      timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_HFP_WC_REG, DSI_REG[1]->DSI_HFP_WC, HFP_WC,
				      timing.value);
		} else {
			DSI_OUTREGBIT(NULL, struct DSI_HFP_WC_REG, DSI_REG[0]->DSI_HFP_WC, HFP_WC,
				      timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_HFP_WC_REG, DSI_REG[1]->DSI_HFP_WC, HFP_WC,
				      timing.value);
		}
		break;
	case HBP:
		if (dsi_params->mode == SYNC_EVENT_VDO_MODE || dsi_params->mode == BURST_VDO_MODE) {
			hbp_byte =
			    ((timing.value +
			      dsi_params->horizontal_sync_active) * fbconfig_dsiTmpBufBpp - 10);
		} else {
			/* hsa_byte = (dsi_params->horizontal_sync_active * fbconfig_dsiTmpBufBpp - 10); */
			hbp_byte = timing.value * fbconfig_dsiTmpBufBpp - 10;
		}
		if (dsi_index == PM_DSI0) {
			DSI_OUTREG32(NULL, &DSI_REG[0]->DSI_HBP_WC, ALIGN_TO((hbp_byte), 4));
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREG32(NULL, &DSI_REG[1]->DSI_HBP_WC, ALIGN_TO((hbp_byte), 4));
		} else {
			DSI_OUTREG32(NULL, &DSI_REG[0]->DSI_HBP_WC, ALIGN_TO((hbp_byte), 4));
			DSI_OUTREG32(NULL, &DSI_REG[1]->DSI_HBP_WC, ALIGN_TO((hbp_byte), 4));
		}

		break;
	case VPW:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_VACT_NL_REG, DSI_REG[0]->DSI_VACT_NL, VACT_NL,
				      timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_VACT_NL_REG, DSI_REG[1]->DSI_VACT_NL, VACT_NL,
				      timing.value);
		} else {
			DSI_OUTREGBIT(NULL, struct DSI_VACT_NL_REG, DSI_REG[0]->DSI_VACT_NL, VACT_NL,
				      timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_VACT_NL_REG, DSI_REG[1]->DSI_VACT_NL, VACT_NL,
				      timing.value);
		}
		/* OUTREG32(&DSI_REG->DSI_VACT_NL,timing.value); */
		break;
	case VFP:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_VFP_NL_REG, DSI_REG[0]->DSI_VFP_NL, VFP_NL,
				      timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_VFP_NL_REG, DSI_REG[1]->DSI_VFP_NL, VFP_NL,
				      timing.value);
		} else {
			DSI_OUTREGBIT(NULL, struct DSI_VFP_NL_REG, DSI_REG[0]->DSI_VFP_NL, VFP_NL,
				      timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_VFP_NL_REG, DSI_REG[1]->DSI_VFP_NL, VFP_NL,
				      timing.value);
		}
		/* OUTREG32(&DSI_REG->DSI_VFP_NL, timing.value); */
		break;
	case VBP:
		if (dsi_index == PM_DSI0) {
			DSI_OUTREGBIT(NULL, struct DSI_VBP_NL_REG, DSI_REG[0]->DSI_VBP_NL, VBP_NL,
				      timing.value);
		} else if (dsi_index == PM_DSI1) {
			DSI_OUTREGBIT(NULL, struct DSI_VBP_NL_REG, DSI_REG[1]->DSI_VBP_NL, VBP_NL,
				      timing.value);
		} else {
			DSI_OUTREGBIT(NULL, struct DSI_VBP_NL_REG, DSI_REG[0]->DSI_VBP_NL, VBP_NL,
				      timing.value);
			DSI_OUTREGBIT(NULL, struct DSI_VBP_NL_REG, DSI_REG[1]->DSI_VBP_NL, VBP_NL,
				      timing.value);
		}
		/* OUTREG32(&DSI_REG->DSI_VBP_NL, timing.value); */
		break;
	case SSC_EN:
		DSI_ssc_enable(dsi_index, timing.value);
		break;
	default:
		DDPMSG("fbconfig dsi set timing :no such type!!\n");
	}
}

UINT32 PanelMaster_get_dsi_timing(UINT32 dsi_index, enum MIPI_SETTING_TYPE type)
{
	UINT32 dsi_val;
	struct DSI_REGS *dsi_reg;
	int fbconfig_dsiTmpBufBpp = 0;

	if (_dsi_context[dsi_index].dsi_params.data_format.format == LCM_DSI_FORMAT_RGB565)
		fbconfig_dsiTmpBufBpp = 2;
	else
		fbconfig_dsiTmpBufBpp = 3;
	if ((dsi_index == PM_DSI0) || (dsi_index == PM_DSI_DUAL))
		dsi_reg = DSI_REG[0];
	else
		dsi_reg = DSI_REG[1];
	switch (type) {
	case LPX:
		dsi_val = dsi_reg->DSI_PHY_TIMECON0.LPX;
		return dsi_val;
	case HS_PRPR:
		dsi_val = dsi_reg->DSI_PHY_TIMECON0.HS_PRPR;
		return dsi_val;
	case HS_ZERO:
		dsi_val = dsi_reg->DSI_PHY_TIMECON0.HS_ZERO;
		return dsi_val;
	case HS_TRAIL:
		dsi_val = dsi_reg->DSI_PHY_TIMECON0.HS_TRAIL;
		return dsi_val;
	case TA_GO:
		dsi_val = dsi_reg->DSI_PHY_TIMECON1.TA_GO;
		return dsi_val;
	case TA_SURE:
		dsi_val = dsi_reg->DSI_PHY_TIMECON1.TA_SURE;
		return dsi_val;
	case TA_GET:
		dsi_val = dsi_reg->DSI_PHY_TIMECON1.TA_GET;
		return dsi_val;
	case DA_HS_EXIT:
		dsi_val = dsi_reg->DSI_PHY_TIMECON1.DA_HS_EXIT;
		return dsi_val;
	case CONT_DET:
		dsi_val = dsi_reg->DSI_PHY_TIMECON2.CONT_DET;
		return dsi_val;
	case CLK_ZERO:
		dsi_val = dsi_reg->DSI_PHY_TIMECON2.CLK_ZERO;
		return dsi_val;
	case CLK_TRAIL:
		dsi_val = dsi_reg->DSI_PHY_TIMECON2.CLK_TRAIL;
		return dsi_val;
	case CLK_HS_PRPR:
		dsi_val = dsi_reg->DSI_PHY_TIMECON3.CLK_HS_PRPR;
		return dsi_val;
	case CLK_HS_POST:
		dsi_val = dsi_reg->DSI_PHY_TIMECON3.CLK_HS_POST;
		return dsi_val;
	case CLK_HS_EXIT:
		dsi_val = dsi_reg->DSI_PHY_TIMECON3.CLK_HS_EXIT;
		return dsi_val;
	case HPW:
		{
			struct DSI_HSA_WC_REG tmp_reg;

			DSI_READREG32(PDSI_HSA_WC_REG, &tmp_reg, &dsi_reg->DSI_HSA_WC);
			dsi_val = (tmp_reg.HSA_WC + 10) / fbconfig_dsiTmpBufBpp;
			return dsi_val;
		}
	case HFP:
		{
			struct DSI_HFP_WC_REG tmp_hfp;

			DSI_READREG32(struct DSI_HFP_WC_REG *, &tmp_hfp, &dsi_reg->DSI_HFP_WC);
			dsi_val = ((tmp_hfp.HFP_WC + 12) / fbconfig_dsiTmpBufBpp);
			return dsi_val;
		}
	case HBP:
		{
			struct DSI_HBP_WC_REG tmp_hbp;
			LCM_DSI_PARAMS *dsi_params;

			dsi_params = get_dsi_params_handle(dsi_index);
			OUTREG32(&tmp_hbp, AS_UINT32(&dsi_reg->DSI_HBP_WC));
			if (dsi_params->mode == SYNC_EVENT_VDO_MODE
			    || dsi_params->mode == BURST_VDO_MODE)
				return (tmp_hbp.HBP_WC + 10) / fbconfig_dsiTmpBufBpp -
					dsi_params->horizontal_sync_active;
			else
				return (tmp_hbp.HBP_WC + 10) / fbconfig_dsiTmpBufBpp;
		}
	case VPW:
		{
			struct DSI_VACT_NL_REG tmp_vpw;

			DSI_READREG32(PDSI_VACT_NL_REG, &tmp_vpw, &dsi_reg->DSI_VACT_NL);
			dsi_val = tmp_vpw.VACT_NL;
			return dsi_val;
		}
	case VFP:
		{
			struct DSI_VFP_NL_REG tmp_vfp;

			DSI_READREG32(PDSI_VFP_NL_REG, &tmp_vfp, &dsi_reg->DSI_VFP_NL);
			dsi_val = tmp_vfp.VFP_NL;
			return dsi_val;
		}
	case VBP:
		{
			struct DSI_VBP_NL_REG tmp_vbp;

			DSI_READREG32(PDSI_VBP_NL_REG, &tmp_vbp, &dsi_reg->DSI_VBP_NL);
			dsi_val = tmp_vbp.VBP_NL;
			return dsi_val;
		}
	case SSC_EN:
		{
			if (_dsi_context[dsi_index].dsi_params.ssc_disable)
				dsi_val = 0;
			else
				dsi_val = 1;
			return dsi_val;
		}
	default:
		DDPMSG("fbconfig dsi set timing :no such type!!\n");
	}
	dsi_val = 0;
	return dsi_val;
}

unsigned int PanelMaster_is_enable(void)
{
	if (atomic_read(&PMaster_enable) == 1)
		return 1;
	else
		return 0;
}

unsigned int PanelMaster_set_PM_enable(unsigned int value)
{
	atomic_set(&PMaster_enable, value);
	return 0;
}

/* ///////////////////////////////No DSI Driver //////////////////////////////////////////////// */
int DSI_set_roi(int x, int y)
{
	DDPMSG("[DSI](x0,y0,x1,y1)=(%d,%d,%d,%d)\n", x, y, _dsi_context[0].lcm_width,
	       _dsi_context[0].lcm_height);
	return DSI_Send_ROI(DISP_MODULE_DSI0, NULL, x, y, _dsi_context[0].lcm_width - x,
			    _dsi_context[0].lcm_height - y);
}

int DSI_check_roi(void)
{
	int ret = 0;
	unsigned char read_buf[4] = { 1, 1, 1, 1 };
	unsigned int data_array[16];
	int count;
	int x0;
	int y0;

	data_array[0] = 0x00043700;	/* read id return two byte,version and id */
	DSI_set_cmdq(DISP_MODULE_DSI0, NULL, data_array, 1, 1);
	msleep(20);
	count = DSI_dcs_read_lcm_reg_v2(DISP_MODULE_DSI0, NULL, 0x2a, read_buf, 4);
	msleep(20);
	x0 = (read_buf[0] << 8) | read_buf[1];
	DDPMSG("x0=%d count=%d,read_buf[0]=%d,read_buf[1]=%d,read_buf[2]=%d,read_buf[3]=%d\n", x0,
	       count, read_buf[0], read_buf[1], read_buf[2], read_buf[3]);
	if ((count == 0) || (x0 != 0)) {
		DDPMSG("[DSI]x count %d read_buf[0]=%d,read_buf[1]=%d,read_buf[2]=%d,read_buf[3]=%d\n",
		     count, read_buf[0], read_buf[1], read_buf[2], read_buf[3]);
		return -1;
	}
	msleep(20);
	count = DSI_dcs_read_lcm_reg_v2(DISP_MODULE_DSI0, NULL, 0x2b, read_buf, 4);
	y0 = (read_buf[0] << 8) | read_buf[1];
	DDPMSG("y0=%d count %d,read_buf[0]=%d,read_buf[1]=%d,read_buf[2]=%d,read_buf[3]=%d\n", y0,
	       count, read_buf[0], read_buf[1], read_buf[2], read_buf[3]);
	if ((count == 0) || (y0 != 0)) {
		DDPMSG("[DSI]y count %d read_buf[0]=%d,read_buf[1]=%d,read_buf[2]=%d,read_buf[3]=%d\n",
		     count, read_buf[0], read_buf[1], read_buf[2], read_buf[3]);
		return -1;
	}
	return ret;
}
void DSI_ForceConfig(int forceconfig)
{
	dsi_force_config = forceconfig;

	/*cv switch by resume*/
	if (disp_helper_get_option(DISP_OPT_CV_BYSUSPEND)) {
		if (lcm_mode_status != 0) {
			if (_dsi_context[0].dsi_params.PLL_CK_CMD == 0)
				_dsi_context[0].dsi_params.PLL_CK_CMD = _dsi_context[0].dsi_params.PLL_CLOCK;
			if (_dsi_context[0].dsi_params.PLL_CK_VDO == 0)
				_dsi_context[0].dsi_params.PLL_CK_VDO = _dsi_context[0].dsi_params.PLL_CLOCK;
			if (lcm_dsi_mode == CMD_MODE)
				_dsi_context[0].dsi_params.PLL_CLOCK = _dsi_context[0].dsi_params.PLL_CK_CMD;
			else if (lcm_dsi_mode == SYNC_PULSE_VDO_MODE ||
				 lcm_dsi_mode == SYNC_EVENT_VDO_MODE ||
					lcm_dsi_mode == BURST_VDO_MODE)
				_dsi_context[0].dsi_params.PLL_CLOCK = _dsi_context[0].dsi_params.PLL_CK_VDO;
		}
	}
}

