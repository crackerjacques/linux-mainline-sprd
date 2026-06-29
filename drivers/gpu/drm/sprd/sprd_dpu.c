// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Unisoc Inc.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_framebuffer.h>

#include "sprd_drm.h"
#include "sprd_dpu.h"
#include "sprd_dsi.h"
#include "sprd_gem.h"

/* Global control registers */
#define REG_DPU_VERSION	0x00
#define REG_DPU_CTRL	0x04
#define REG_DPU_CFG0	0x08
#define REG_DPU_CFG1	0x0C
#define REG_DPU_CFG2	0x10
#define REG_PANEL_SIZE	0x20
#define REG_BLEND_SIZE	0x24
#define REG_BG_COLOR	0x2C

/* Layer0 control registers */
#define REG_LAY_BASE_ADDR0	0x30
#define REG_LAY_BASE_ADDR1	0x34
#define REG_LAY_BASE_ADDR2	0x38
#define REG_LAY_CTRL		0x40
#define REG_LAY_SIZE		0x44
#define REG_LAY_PITCH		0x48
#define REG_LAY_POS		0x4C
#define REG_LAY_ALPHA		0x50
#define REG_LAY_CROP_START	0x5C

/* Interrupt control registers */
#define REG_DPU_INT_EN		0x1E0
#define REG_DPU_INT_CLR		0x1E4
#define REG_DPU_INT_STS		0x1E8

/* DPI control registers */
#define REG_DPI_CTRL		0x1F0
#define REG_DPI_H_TIMING	0x1F4
#define REG_DPI_V_TIMING	0x1F8

/* Global control bits */
#define BIT_DPU_RUN			BIT(0)
#define BIT_DPU_STOP			BIT(1)
#define BIT_DPU_REG_UPDATE		BIT(2)
#define BIT_DPU_IF_EDPI			BIT(0)

/* Layer control bits */
#define BIT_DPU_LAY_EN				BIT(0)
#define BIT_DPU_LAY_LAYER_ALPHA			(0x01 << 2)
#define BIT_DPU_LAY_COMBO_ALPHA			(0x02 << 2)
#define BIT_DPU_LAY_FORMAT_YUV422_2PLANE		(0x00 << 4)
#define BIT_DPU_LAY_FORMAT_YUV420_2PLANE		(0x01 << 4)
#define BIT_DPU_LAY_FORMAT_YUV420_3PLANE		(0x02 << 4)
#define BIT_DPU_LAY_FORMAT_ARGB8888			(0x03 << 4)
#define BIT_DPU_LAY_FORMAT_RGB565			(0x04 << 4)
#define BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3		(0x00 << 8)
#define BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0		(0x01 << 8)
#define BIT_DPU_LAY_NO_SWITCH			(0x00 << 10)
#define BIT_DPU_LAY_RB_OR_UV_SWITCH		(0x01 << 10)
#define BIT_DPU_LAY_MODE_BLEND_NORMAL		(0x00 << 16)
#define BIT_DPU_LAY_MODE_BLEND_PREMULT		(0x01 << 16)
#define BIT_DPU_LAY_ROTATION_0		(0x00 << 20)
#define BIT_DPU_LAY_ROTATION_90		(0x01 << 20)
#define BIT_DPU_LAY_ROTATION_180	(0x02 << 20)
#define BIT_DPU_LAY_ROTATION_270	(0x03 << 20)
#define BIT_DPU_LAY_ROTATION_0_M	(0x04 << 20)
#define BIT_DPU_LAY_ROTATION_90_M	(0x05 << 20)
#define BIT_DPU_LAY_ROTATION_180_M	(0x06 << 20)
#define BIT_DPU_LAY_ROTATION_270_M	(0x07 << 20)

/* Interrupt control & status bits */
#define BIT_DPU_INT_DONE		BIT(0)
#define BIT_DPU_INT_TE			BIT(1)
#define BIT_DPU_INT_ERR			BIT(2)
#define BIT_DPU_INT_UPDATE_DONE		BIT(4)
#define BIT_DPU_INT_VSYNC		BIT(5)

/* DPI control bits */
#define BIT_DPU_EDPI_TE_EN		BIT(8)
#define BIT_DPU_EDPI_FROM_EXTERNAL_PAD	BIT(10)
#define BIT_DPU_DPI_HALT_EN		BIT(16)

static const u32 layer_fmts[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV61,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YVU420,
};

struct sprd_plane {
	struct drm_plane base;
};

static int dpu_wait_stop_done(struct sprd_dpu *dpu)
{
	struct dpu_context *ctx = &dpu->ctx;
	int rc;

	if (ctx->stopped)
		return 0;

	rc = wait_event_interruptible_timeout(ctx->wait_queue, ctx->evt_stop,
					      msecs_to_jiffies(500));
	ctx->evt_stop = false;

	ctx->stopped = true;

	if (!rc) {
		drm_err(dpu->drm, "dpu wait for stop done time out!\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int dpu_wait_update_done(struct sprd_dpu *dpu)
{
	struct dpu_context *ctx = &dpu->ctx;
	int rc;

	ctx->evt_update = false;

	rc = wait_event_interruptible_timeout(ctx->wait_queue, ctx->evt_update,
					      msecs_to_jiffies(500));

	if (!rc) {
		drm_err(dpu->drm, "dpu wait for reg update done time out!\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static u32 drm_format_to_dpu(struct drm_framebuffer *fb)
{
	u32 format = 0;

	switch (fb->format->format) {
	case DRM_FORMAT_BGRA8888:
		/* BGRA8888 -> ARGB8888 */
		format |= BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0;
		format |= BIT_DPU_LAY_FORMAT_ARGB8888;
		break;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		/* RGBA8888 -> ABGR8888 */
		format |= BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0;
		fallthrough;
	case DRM_FORMAT_ABGR8888:
		/* RB switch */
		format |= BIT_DPU_LAY_RB_OR_UV_SWITCH;
		fallthrough;
	case DRM_FORMAT_ARGB8888:
		format |= BIT_DPU_LAY_FORMAT_ARGB8888;
		break;
	case DRM_FORMAT_XBGR8888:
		/* RB switch */
		format |= BIT_DPU_LAY_RB_OR_UV_SWITCH;
		fallthrough;
	case DRM_FORMAT_XRGB8888:
		format |= BIT_DPU_LAY_FORMAT_ARGB8888;
		break;
	case DRM_FORMAT_BGR565:
		/* RB switch */
		format |= BIT_DPU_LAY_RB_OR_UV_SWITCH;
		fallthrough;
	case DRM_FORMAT_RGB565:
		format |= BIT_DPU_LAY_FORMAT_RGB565;
		break;
	case DRM_FORMAT_NV12:
		/* 2-Lane: Yuv420 */
		format |= BIT_DPU_LAY_FORMAT_YUV420_2PLANE;
		/* Y endian */
		format |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/* UV endian */
		format |= BIT_DPU_LAY_NO_SWITCH;
		break;
	case DRM_FORMAT_NV21:
		/* 2-Lane: Yuv420 */
		format |= BIT_DPU_LAY_FORMAT_YUV420_2PLANE;
		/* Y endian */
		format |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/* UV endian */
		format |= BIT_DPU_LAY_RB_OR_UV_SWITCH;
		break;
	case DRM_FORMAT_NV16:
		/* 2-Lane: Yuv422 */
		format |= BIT_DPU_LAY_FORMAT_YUV422_2PLANE;
		/* Y endian */
		format |= BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0;
		/* UV endian */
		format |= BIT_DPU_LAY_RB_OR_UV_SWITCH;
		break;
	case DRM_FORMAT_NV61:
		/* 2-Lane: Yuv422 */
		format |= BIT_DPU_LAY_FORMAT_YUV422_2PLANE;
		/* Y endian */
		format |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/* UV endian */
		format |= BIT_DPU_LAY_NO_SWITCH;
		break;
	case DRM_FORMAT_YUV420:
		format |= BIT_DPU_LAY_FORMAT_YUV420_3PLANE;
		/* Y endian */
		format |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/* UV endian */
		format |= BIT_DPU_LAY_NO_SWITCH;
		break;
	case DRM_FORMAT_YVU420:
		format |= BIT_DPU_LAY_FORMAT_YUV420_3PLANE;
		/* Y endian */
		format |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/* UV endian */
		format |= BIT_DPU_LAY_RB_OR_UV_SWITCH;
		break;
	default:
		break;
	}

	return format;
}

static u32 drm_rotation_to_dpu(struct drm_plane_state *state)
{
	u32 rotation = 0;

	switch (state->rotation) {
	default:
	case DRM_MODE_ROTATE_0:
		rotation = BIT_DPU_LAY_ROTATION_0;
		break;
	case DRM_MODE_ROTATE_90:
		rotation = BIT_DPU_LAY_ROTATION_90;
		break;
	case DRM_MODE_ROTATE_180:
		rotation = BIT_DPU_LAY_ROTATION_180;
		break;
	case DRM_MODE_ROTATE_270:
		rotation = BIT_DPU_LAY_ROTATION_270;
		break;
	case DRM_MODE_REFLECT_Y:
		rotation = BIT_DPU_LAY_ROTATION_180_M;
		break;
	case (DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_90):
		rotation = BIT_DPU_LAY_ROTATION_90_M;
		break;
	case DRM_MODE_REFLECT_X:
		rotation = BIT_DPU_LAY_ROTATION_0_M;
		break;
	case (DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_90):
		rotation = BIT_DPU_LAY_ROTATION_270_M;
		break;
	}

	return rotation;
}

static u32 drm_blend_to_dpu(struct drm_plane_state *state)
{
	u32 blend = 0;

	switch (state->pixel_blend_mode) {
	case DRM_MODE_BLEND_COVERAGE:
		/* alpha mode select - combo alpha */
		blend |= BIT_DPU_LAY_COMBO_ALPHA;
		/* Normal mode */
		blend |= BIT_DPU_LAY_MODE_BLEND_NORMAL;
		break;
	case DRM_MODE_BLEND_PREMULTI:
		/* alpha mode select - combo alpha */
		blend |= BIT_DPU_LAY_COMBO_ALPHA;
		/* Pre-mult mode */
		blend |= BIT_DPU_LAY_MODE_BLEND_PREMULT;
		break;
	case DRM_MODE_BLEND_PIXEL_NONE:
	default:
		/* don't do blending, maybe RGBX */
		/* alpha mode select - layer alpha */
		blend |= BIT_DPU_LAY_LAYER_ALPHA;
		break;
	}

	return blend;
}

static void sprd_dpu_layer(struct sprd_dpu *dpu, struct drm_plane_state *state)
{
	struct dpu_context *ctx = &dpu->ctx;
	struct sprd_gem_obj *sprd_gem;
	struct drm_framebuffer *fb = state->fb;
	u32 addr, size, offset, pitch, blend, format, rotation;
	u32 src_x = state->src_x >> 16;
	u32 src_y = state->src_y >> 16;
	u32 src_w = state->src_w >> 16;
	u32 src_h = state->src_h >> 16;
	u32 dst_x = state->crtc_x;
	u32 dst_y = state->crtc_y;
	u32 alpha = state->alpha;
	u32 index = state->zpos;
	static int dump_count;
	int i;

	offset = (dst_x & 0xffff) | (dst_y << 16);
	size = (src_w & 0xffff) | (src_h << 16);

	for (i = 0; i < fb->format->num_planes; i++) {
		sprd_gem = to_sprd_gem_obj(fb->obj[i]);
		addr = sprd_gem->dma_addr + fb->offsets[i];

		if (i == 0)
			layer_reg_wr(ctx, REG_LAY_BASE_ADDR0, addr, index);
		else if (i == 1)
			layer_reg_wr(ctx, REG_LAY_BASE_ADDR1, addr, index);
		else
			layer_reg_wr(ctx, REG_LAY_BASE_ADDR2, addr, index);
	}

	if (fb->format->num_planes == 3) {
		/* UV pitch is 1/2 of Y pitch */
		pitch = (fb->pitches[0] / fb->format->cpp[0]) |
				(fb->pitches[0] / fb->format->cpp[0] << 15);
	} else {
		pitch = fb->pitches[0] / fb->format->cpp[0];
	}

	layer_reg_wr(ctx, REG_LAY_POS, offset, index);
	layer_reg_wr(ctx, REG_LAY_SIZE, size, index);
	layer_reg_wr(ctx, REG_LAY_CROP_START,
		     src_y << 16 | src_x, index);
	layer_reg_wr(ctx, REG_LAY_ALPHA, alpha, index);
	layer_reg_wr(ctx, REG_LAY_PITCH, pitch, index);

	format = drm_format_to_dpu(fb);
	blend = drm_blend_to_dpu(state);
	rotation = drm_rotation_to_dpu(state);

	if (dump_count < 6) {
		dump_count++;
		drm_info(dpu->drm,
			 "LAYDUMP[%d] idx=%u base=%08x size=%08x pitch=%u fmt=%08x num_planes=%u\n",
			 dump_count, index, addr, size, pitch, format,
			 fb->format->num_planes);
	}

	layer_reg_wr(ctx, REG_LAY_CTRL, BIT_DPU_LAY_EN |
				format |
				blend |
				rotation,
				index);
}

static void sprd_dpu_flip(struct sprd_dpu *dpu)
{
	struct dpu_context *ctx = &dpu->ctx;
	static int dump_count;

	if (dump_count < 4) {
		dump_count++;
		drm_info(dpu->drm,
			 "DPUDUMP[%d] stopped=%d CTRL=%08x INT_EN=%08x INT_STS=%08x DPI_CTRL=%08x\n",
			 dump_count, ctx->stopped,
			 readl(ctx->base + REG_DPU_CTRL),
			 readl(ctx->base + REG_DPU_INT_EN),
			 readl(ctx->base + REG_DPU_INT_STS),
			 readl(ctx->base + REG_DPI_CTRL));
		drm_info(dpu->drm,
			 "DPUDUMP[%d] PANEL=%08x LAY_CTRL=%08x LAY_SIZE=%08x LAY_PITCH=%08x LAY_BASE0=%08x LAY_POS=%08x\n",
			 dump_count,
			 readl(ctx->base + REG_PANEL_SIZE),
			 readl(ctx->base + REG_LAY_CTRL),
			 readl(ctx->base + REG_LAY_SIZE),
			 readl(ctx->base + REG_LAY_PITCH),
			 readl(ctx->base + REG_LAY_BASE_ADDR0),
			 readl(ctx->base + REG_LAY_POS));
	}

	/*
	 * Make sure the dpu is in stop status. DPU has no shadow
	 * registers in EDPI mode. So the config registers can only be
	 * updated in the rising edge of DPU_RUN bit.
	 */
	if (ctx->if_type == SPRD_DPU_IF_EDPI)
		dpu_wait_stop_done(dpu);

	/* update trigger and wait */
	if (ctx->if_type == SPRD_DPU_IF_DPI) {
		if (!ctx->stopped) {
			dpu_reg_set(ctx, REG_DPU_CTRL, BIT_DPU_REG_UPDATE);
			dpu_wait_update_done(dpu);
		} else {
			dpu_reg_set(ctx, REG_DPU_CTRL, BIT_DPU_RUN);
			dpu_reg_set(ctx, REG_DPU_CTRL, BIT_DPU_REG_UPDATE);
			ctx->stopped = false;
		}

		dpu_reg_set(ctx, REG_DPU_INT_EN, BIT_DPU_INT_ERR);
	} else if (ctx->if_type == SPRD_DPU_IF_EDPI) {
		dpu_reg_set(ctx, REG_DPU_CTRL, BIT_DPU_RUN);

		ctx->stopped = false;
	}
}

static void sprd_dpu_reset(struct dpu_context *ctx)
{
	if (!ctx->rst_syscon)
		return;

	regmap_update_bits(ctx->rst_syscon, ctx->rst_offset,
			   ctx->rst_mask, ctx->rst_mask);
	udelay(10);
	regmap_update_bits(ctx->rst_syscon, ctx->rst_offset,
			   ctx->rst_mask, 0);
	udelay(10);

	/*
	 * The DPU is now in a clean, stopped state. Mark it so that the
	 * follow-up sprd_dpu_init() skips sprd_dpu_stop() - issuing STOP on a
	 * freshly-reset DPU just times out waiting for a stop-done IRQ and
	 * leaves it unable to start.
	 */
	ctx->stopped = true;
}

static void sprd_dpi_init(struct sprd_dpu *dpu);

static void sprd_dpu_init(struct sprd_dpu *dpu)
{
	struct dpu_context *ctx = &dpu->ctx;
	u32 int_mask = 0;
	u32 dpu_version;

	/* clear the bootloader's pinned state so our config can latch */
	sprd_dpu_reset(ctx);

	/*
	 * The reset above wipes every DPU register, including the panel size
	 * and DPI timing that sprd_dpi_init() programmed in mode_set_nofb
	 * (which runs before this atomic_enable). Re-apply them here, after the
	 * reset, or the DPU runs with PANEL_SIZE=0 / DPI timing=0 and can never
	 * complete a frame (reg update done timeout, underflow, black screen).
	 */
	sprd_dpi_init(dpu);

	dpu_version = readl(ctx->base + REG_DPU_VERSION);

	writel(0x00, ctx->base + REG_BG_COLOR);

	/*
	 * DDR QoS (CFG1) and CFG2. Without the QoS priorities the DPU's DDR
	 * read requests starve and the layer FIFO underflows on the first
	 * frame (no vsync). Values from the vendor dpu-r4p0 driver:
	 * awqos_high=0xa awqos_low=0xa arqos_high=0xd arqos_low=0xc | BIT18 | BIT22.
	 */
	writel((0xa << 12) | (0xa << 8) | (0xd << 4) | 0xc | BIT(18) | BIT(22),
	       ctx->base + REG_DPU_CFG1);
	writel(0x14002, ctx->base + REG_DPU_CFG2);

	if (ctx->if_type == SPRD_DPU_IF_DPI) {
		/* use dpi as interface */
		dpu_reg_clr(ctx, REG_DPU_CFG0, BIT_DPU_IF_EDPI);

		if (dpu_version < 0x300) {
			/* disable Halt function for SPRD DSI */
			dpu_reg_clr(ctx, REG_DPI_CTRL, BIT_DPU_DPI_HALT_EN);
			/* select te from external pad */
			dpu_reg_set(ctx, REG_DPI_CTRL, BIT_DPU_EDPI_FROM_EXTERNAL_PAD);
		} else {
			/*
			 * Keep BIT_DPU_DPI_HALT_EN CLEARED (free-running DPI), NOT
			 * set. The vendor BSP sets it so the DPU's DPI output gates
			 * on the DSI ready/ack handshake, and an earlier session
			 * re-enabled it on the theory that the flags=3 burst fix
			 * (lanes now streaming, PHY_STATUS=0x1f02) had finally made
			 * the handshake completable.
			 *
			 * The two captured dmesg logs (2026-06-26,
			 * good-fbcon-dmesg.log vs bad-fbcon-dmesg.log) disprove that:
			 *  - The only state that ever lights the panel (U-Boot
			 *    handoff, fbcon visible) has DPI_CTRL=0x00000000 AND
			 *    DSI_MODE_CFG=0x0 -- both halt halves OFF, DPU free-runs.
			 *  - The black kernel-native state had DPI_CTRL=0x00010000
			 *    (this HALT_EN bit) AND DSI_MODE_CFG=0x2. Both halves on,
			 *    lanes park in stopstate (PHY_STATUS=0x1f32), no scanout.
			 * Enabling the halt handshake has produced a black panel on
			 * every measurement; free-running is the only proven-good
			 * config. Pair this with DSI_MODE_CFG=0 in sprd_dsi.c
			 * (sprd_dsi_set_work_mode). See DISPLAY-KNOWN-GOOD-DSI-STATE.md.
			 */
			dpu_reg_clr(ctx, REG_DPI_CTRL, BIT_DPU_DPI_HALT_EN);
		}

		/* enable dpu update done INT */
		int_mask |= BIT_DPU_INT_UPDATE_DONE;
		/* enable dpu done INT */
		int_mask |= BIT_DPU_INT_DONE;
		/* enable dpu dpi vsync */
		int_mask |= BIT_DPU_INT_VSYNC;
		/* enable dpu TE INT */
		int_mask |= BIT_DPU_INT_TE;
		/* enable underflow err INT */
		int_mask |= BIT_DPU_INT_ERR;
	} else if (ctx->if_type == SPRD_DPU_IF_EDPI) {
		/* use edpi as interface */
		dpu_reg_set(ctx, REG_DPU_CFG0, BIT_DPU_IF_EDPI);
		/* use external te */
		dpu_reg_set(ctx, REG_DPI_CTRL, BIT_DPU_EDPI_FROM_EXTERNAL_PAD);
		/* enable te */
		dpu_reg_set(ctx, REG_DPI_CTRL, BIT_DPU_EDPI_TE_EN);

		/* enable stop done INT */
		int_mask |= BIT_DPU_INT_DONE;
		/* enable TE INT */
		int_mask |= BIT_DPU_INT_TE;
	}

	writel(int_mask, ctx->base + REG_DPU_INT_EN);

	/*
	 * The DPU is usually enabled by the bootloader to show
	 * a splash screen. Stop it here when the kernel initializes
	 * the display.
	 */
	if (!ctx->stopped)
		sprd_dpu_stop(dpu);
}

static void sprd_dpu_fini(struct sprd_dpu *dpu)
{
	struct dpu_context *ctx = &dpu->ctx;

	writel(0x00, ctx->base + REG_DPU_INT_EN);
	writel(0xff, ctx->base + REG_DPU_INT_CLR);
}

static void sprd_dpi_init(struct sprd_dpu *dpu)
{
	struct dpu_context *ctx = &dpu->ctx;
	u32 reg_val;
	u32 size;

	size = (ctx->vm.vactive << 16) | ctx->vm.hactive;
	writel(size, ctx->base + REG_PANEL_SIZE);
	writel(size, ctx->base + REG_BLEND_SIZE);

	if (ctx->if_type == SPRD_DPU_IF_DPI) {
		/* set dpi timing */
		reg_val = ctx->vm.hsync_len << 0 |
			  ctx->vm.hback_porch << 8 |
			  ctx->vm.hfront_porch << 20;
		writel(reg_val, ctx->base + REG_DPI_H_TIMING);

		reg_val = ctx->vm.vsync_len << 0 |
			  ctx->vm.vback_porch << 8 |
			  ctx->vm.vfront_porch << 20;
		writel(reg_val, ctx->base + REG_DPI_V_TIMING);
	}
}

void sprd_dpu_run(struct sprd_dpu *dpu)
{
	struct dpu_context *ctx = &dpu->ctx;

	dpu_reg_set(ctx, REG_DPU_CTRL, BIT_DPU_RUN);
	dpu_reg_set(ctx, REG_DPU_CTRL, BIT_DPU_REG_UPDATE);

	ctx->stopped = false;
}

void sprd_dpu_stop(struct sprd_dpu *dpu)
{
	struct dpu_context *ctx = &dpu->ctx;

	if (ctx->if_type == SPRD_DPU_IF_DPI)
		dpu_reg_set(ctx, REG_DPU_CTRL, BIT_DPU_STOP);

	dpu_wait_stop_done(dpu);
}

static int sprd_plane_atomic_check(struct drm_plane *plane,
				   struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state,
									     plane);
	struct drm_crtc_state *crtc_state;
	u32 fmt;

	if (!plane_state->fb || !plane_state->crtc)
		return 0;

	fmt = drm_format_to_dpu(plane_state->fb);
	if (!fmt)
		return -EINVAL;

	crtc_state = drm_atomic_get_crtc_state(plane_state->state, plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	return drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true, true);
}

static void sprd_plane_atomic_update(struct drm_plane *drm_plane,
				     struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   drm_plane);
	struct sprd_dpu *dpu = to_sprd_crtc(new_state->crtc);

	/* start configure dpu layers */
	sprd_dpu_layer(dpu, new_state);
}

static void sprd_plane_atomic_disable(struct drm_plane *drm_plane,
				      struct drm_atomic_state *state)
{
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state,
									   drm_plane);
	struct sprd_dpu *dpu = to_sprd_crtc(old_state->crtc);

	layer_reg_wr(&dpu->ctx, REG_LAY_CTRL, 0x00, old_state->zpos);
}

static void sprd_plane_create_properties(struct sprd_plane *plane, int index)
{
	unsigned int supported_modes = BIT(DRM_MODE_BLEND_PIXEL_NONE) |
				       BIT(DRM_MODE_BLEND_PREMULTI) |
				       BIT(DRM_MODE_BLEND_COVERAGE);

	/* create rotation property */
	drm_plane_create_rotation_property(&plane->base,
					   DRM_MODE_ROTATE_0,
					   DRM_MODE_ROTATE_MASK |
					   DRM_MODE_REFLECT_MASK);

	/* create alpha property */
	drm_plane_create_alpha_property(&plane->base);

	/* create blend mode property */
	drm_plane_create_blend_mode_property(&plane->base, supported_modes);

	/* create zpos property */
	drm_plane_create_zpos_immutable_property(&plane->base, index);
}

static const struct drm_plane_helper_funcs sprd_plane_helper_funcs = {
	.atomic_check = sprd_plane_atomic_check,
	.atomic_update = sprd_plane_atomic_update,
	.atomic_disable = sprd_plane_atomic_disable,
};

static const struct drm_plane_funcs sprd_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane	= drm_atomic_helper_disable_plane,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static struct sprd_plane *sprd_planes_init(struct drm_device *drm)
{
	struct sprd_plane *plane, *primary;
	enum drm_plane_type plane_type;
	int i;

	for (i = 0; i < 6; i++) {
		plane_type = (i == 0) ? DRM_PLANE_TYPE_PRIMARY :
					DRM_PLANE_TYPE_OVERLAY;

		plane = drmm_universal_plane_alloc(drm, struct sprd_plane, base,
						   1, &sprd_plane_funcs,
						   layer_fmts, ARRAY_SIZE(layer_fmts),
						   NULL, plane_type, NULL);
		if (IS_ERR(plane)) {
			drm_err(drm, "failed to init drm plane: %d\n", i);
			return plane;
		}

		drm_plane_helper_add(&plane->base, &sprd_plane_helper_funcs);

		sprd_plane_create_properties(plane, i);

		if (i == 0)
			primary = plane;
	}

	return primary;
}

static void sprd_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct sprd_dpu *dpu = to_sprd_crtc(crtc);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct drm_encoder *encoder;
	struct sprd_dsi *dsi;

	drm_display_mode_to_videomode(mode, &dpu->ctx.vm);

	drm_for_each_encoder_mask(encoder, crtc->dev,
				  crtc->state->encoder_mask) {
		dsi = encoder_to_dsi(encoder);

		if (dsi->slave->mode_flags & MIPI_DSI_MODE_VIDEO)
			dpu->ctx.if_type = SPRD_DPU_IF_DPI;
		else
			dpu->ctx.if_type = SPRD_DPU_IF_EDPI;
	}

	sprd_dpi_init(dpu);
}

static void sprd_crtc_atomic_enable(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct sprd_dpu *dpu = to_sprd_crtc(crtc);

	sprd_dpu_init(dpu);

	drm_crtc_vblank_on(&dpu->base);
}

static void sprd_crtc_atomic_disable(struct drm_crtc *crtc,
				     struct drm_atomic_state *state)
{
	struct sprd_dpu *dpu = to_sprd_crtc(crtc);
	struct drm_device *drm = dpu->base.dev;

	drm_crtc_vblank_off(&dpu->base);

	sprd_dpu_fini(dpu);

	spin_lock_irq(&drm->event_lock);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&drm->event_lock);
}

static void sprd_crtc_atomic_flush(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)

{
	struct sprd_dpu *dpu = to_sprd_crtc(crtc);
	struct drm_device *drm = dpu->base.dev;

	sprd_dpu_flip(dpu);

	spin_lock_irq(&drm->event_lock);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&drm->event_lock);
}

static int sprd_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct sprd_dpu *dpu = to_sprd_crtc(crtc);

	dpu_reg_set(&dpu->ctx, REG_DPU_INT_EN, BIT_DPU_INT_VSYNC);

	return 0;
}

static void sprd_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct sprd_dpu *dpu = to_sprd_crtc(crtc);

	dpu_reg_clr(&dpu->ctx, REG_DPU_INT_EN, BIT_DPU_INT_VSYNC);
}

static const struct drm_crtc_helper_funcs sprd_crtc_helper_funcs = {
	.mode_set_nofb	= sprd_crtc_mode_set_nofb,
	.atomic_flush	= sprd_crtc_atomic_flush,
	.atomic_enable	= sprd_crtc_atomic_enable,
	.atomic_disable	= sprd_crtc_atomic_disable,
};

static const struct drm_crtc_funcs sprd_crtc_funcs = {
	.set_config	= drm_atomic_helper_set_config,
	.page_flip	= drm_atomic_helper_page_flip,
	.reset		= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.enable_vblank	= sprd_crtc_enable_vblank,
	.disable_vblank	= sprd_crtc_disable_vblank,
};

static struct sprd_dpu *sprd_crtc_init(struct drm_device *drm,
				       struct drm_plane *primary, struct device *dev)
{
	struct device_node *port;
	struct sprd_dpu *dpu;

	dpu = drmm_crtc_alloc_with_planes(drm, struct sprd_dpu, base,
					  primary, NULL,
					&sprd_crtc_funcs, NULL);
	if (IS_ERR(dpu)) {
		drm_err(drm, "failed to init crtc\n");
		return dpu;
	}
	drm_crtc_helper_add(&dpu->base, &sprd_crtc_helper_funcs);

	/*
	 * set crtc port so that drm_of_find_possible_crtcs call works
	 */
	port = of_graph_get_port_by_id(dev->of_node, 0);
	if (!port) {
		drm_err(drm, "failed to found crtc output port for %s\n",
			dev->of_node->full_name);
		return ERR_PTR(-EINVAL);
	}
	dpu->base.port = port;
	of_node_put(port);

	return dpu;
}

static irqreturn_t sprd_dpu_isr(int irq, void *data)
{
	struct sprd_dpu *dpu = data;
	struct dpu_context *ctx = &dpu->ctx;
	u32 reg_val, int_mask = 0;

	reg_val = readl(ctx->base + REG_DPU_INT_STS);

	/* disable err interrupt */
	if (reg_val & BIT_DPU_INT_ERR) {
		int_mask |= BIT_DPU_INT_ERR;
		drm_warn(dpu->drm, "Warning: dpu underflow!\n");
	}

	/* dpu update done isr */
	if (reg_val & BIT_DPU_INT_UPDATE_DONE) {
		ctx->evt_update = true;
		wake_up_interruptible_all(&ctx->wait_queue);
	}

	/* dpu stop done isr */
	if (reg_val & BIT_DPU_INT_DONE) {
		ctx->evt_stop = true;
		wake_up_interruptible_all(&ctx->wait_queue);
	}

	if (reg_val & BIT_DPU_INT_VSYNC)
		drm_crtc_handle_vblank(&dpu->base);

	writel(reg_val, ctx->base + REG_DPU_INT_CLR);
	dpu_reg_clr(ctx, REG_DPU_INT_EN, int_mask);

	return IRQ_HANDLED;
}

static int sprd_dpu_context_init(struct sprd_dpu *dpu,
				 struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dpu_context *ctx = &dpu->ctx;
	int ret;

	ctx->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->base)) {
		dev_err(dev, "failed to map dpu registers\n");
		return PTR_ERR(ctx->base);
	}

	ctx->irq = platform_get_irq(pdev, 0);
	if (ctx->irq < 0)
		return ctx->irq;

	ctx->clk = devm_clk_get_optional_enabled(dev, "core");
	if (IS_ERR(ctx->clk)) {
		dev_err(dev, "failed to get DPU core clock\n");
		return PTR_ERR(ctx->clk);
	}

	/*
	 * The "core" clock above is actually CLK_DISPC_EB - the APB enable
	 * gate that lets us touch DPU registers. The DPU pipeline matrix
	 * clock (CLK_DISPC0, "dispc0-clk" in clk_summary) is a separate
	 * thing that vendor's BSP calls clk_dpu_core and explicitly
	 * prepare_enables. Without it the DPU register block is reachable
	 * but the pipeline itself can't tick - no scanout, no VSYNC, no
	 * frames. Until 2026-06 mainline relied on clk_ignore_unused
	 * leaving the bootloader's rate in place; with proper PM that
	 * stops working.
	 */
	ctx->pipe_clk = devm_clk_get_optional_enabled(dev, "clk");
	if (IS_ERR(ctx->pipe_clk)) {
		dev_err(dev, "failed to get DPU pipeline clock\n");
		return PTR_ERR(ctx->pipe_clk);
	}
	if (ctx->pipe_clk) {
		int ret = clk_set_rate(ctx->pipe_clk, 384000000);

		if (ret)
			dev_warn(dev, "failed to set pipe clock rate: %d\n", ret);
	}

	/*
	 * The DPU "dpi" clock is the pixel clock that feeds the DPI block.
	 * Mainline only requested "core" so the framework never had a real
	 * prepare/enable on the pixel clock: on sharkl5pro it only ran at all
	 * because clk_ignore_unused left the bootloader's rate alone, but the
	 * clock framework still saw it as unprepared, so transitions and any
	 * future PM activity would gate it. Without a properly framework-
	 * prepared pixel clock the DPI block can't drive valid frames into
	 * the DSI controller, which then never asserts ready and the
	 * DPU<->DSI halt handshake stalls forever.
	 */
	ctx->dpi_clk = devm_clk_get_optional_enabled(dev, "dpi");
	if (IS_ERR(ctx->dpi_clk)) {
		dev_err(dev, "failed to get DPU dpi clock\n");
		return PTR_ERR(ctx->dpi_clk);
	}
	if (ctx->dpi_clk) {
		int ret = clk_set_rate(ctx->dpi_clk, 38400000);

		if (ret)
			dev_warn(dev, "failed to set dpi clock rate: %d\n", ret);
	}

	/*
	 * Optional hardware reset. The bootloader leaves the DPU running and
	 * (with clk/regulator_ignore_unused) pinned; its shadow registers then
	 * never latch our config. A soft reset before bring-up clears that.
	 * reset-syscon = <&ap_ahb_regs offset mask>.
	 */
	{
		u32 args[2];

		ctx->rst_syscon = syscon_regmap_lookup_by_phandle_args(dev->of_node,
								       "reset-syscon",
								       2, args);
		if (IS_ERR(ctx->rst_syscon)) {
			ctx->rst_syscon = NULL;
		} else {
			ctx->rst_offset = args[0];
			ctx->rst_mask = args[1];
		}
	}

	/* disable and clear interrupts before register dpu IRQ. */
	writel(0x00, ctx->base + REG_DPU_INT_EN);
	writel(0xff, ctx->base + REG_DPU_INT_CLR);

	ret = devm_request_irq(dev, ctx->irq, sprd_dpu_isr,
			       IRQF_TRIGGER_NONE, "DPU", dpu);
	if (ret) {
		dev_err(dev, "failed to register dpu irq handler\n");
		return ret;
	}

	init_waitqueue_head(&ctx->wait_queue);

	return 0;
}

static int sprd_dpu_bind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm = data;
	struct sprd_dpu *dpu;
	struct sprd_plane *plane;
	int ret;

	plane = sprd_planes_init(drm);
	if (IS_ERR(plane))
		return PTR_ERR(plane);

	dpu = sprd_crtc_init(drm, &plane->base, dev);
	if (IS_ERR(dpu))
		return PTR_ERR(dpu);

	dpu->drm = drm;
	dev_set_drvdata(dev, dpu);

	ret = sprd_dpu_context_init(dpu, dev);
	if (ret)
		return ret;

	if (device_iommu_mapped(dev)) {
		ret = sprd_drm_iommu_attach(drm, dev);
		if (ret)
			return ret;
	}

	return 0;
}

static void sprd_dpu_unbind(struct device *dev,
			    struct device *master, void *data)
{
	struct drm_device *drm = data;

	sprd_drm_iommu_detach(drm, dev);
}

static const struct component_ops dpu_component_ops = {
	.bind = sprd_dpu_bind,
	.unbind = sprd_dpu_unbind,
};

static const struct of_device_id dpu_match_table[] = {
	{ .compatible = "sprd,sharkl3-dpu" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, dpu_match_table);

static int sprd_dpu_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &dpu_component_ops);
}

static void sprd_dpu_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dpu_component_ops);
}

struct platform_driver sprd_dpu_driver = {
	.probe = sprd_dpu_probe,
	.remove = sprd_dpu_remove,
	.driver = {
		.name = "sprd-dpu-drv",
		.of_match_table = dpu_match_table,
	},
};

MODULE_AUTHOR("Leon He <leon.he@unisoc.com>");
MODULE_AUTHOR("Kevin Tang <kevin.tang@unisoc.com>");
MODULE_DESCRIPTION("Unisoc Display Controller Driver");
MODULE_LICENSE("GPL v2");
