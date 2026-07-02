// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spreadtrum/Unisoc audio-DSP (AGDSP / AUDCP) boot driver.
 *
 * Brings the audio co-processor (a halfword-addressed DSP) out of reset and
 * points it at firmware that has been loaded into a reserved DRAM region, so
 * the in-tree sprd-agdsp mailbox IPC + vbc-v4-dsp ASoC stack can talk to it.
 *
 * This is a slimmed mainline-style rewrite of the vendor
 * sprd_audio/audiocpboot/sprd_audcp_boot.c: the vendor driver pulls the
 * firmware in from userspace via a sysfs download node and uses the out-of-tree
 * audio_mem allocator; here we use request_firmware() + a DT memory-region. The
 * firmware blob is the raw stock l_agdsp partition (header "AUDCP.SharkL5" at
 * offset 0, DSP code from 0x80); the boot vector is (load_addr + 0x80) >> 1.
 *
 * The boot-control register bits live in the AON-APB / PMU-APB syscons and are
 * described in DT as <&syscon reg mask> tuples, transcribed from the stock
 * "audiocp_boot" node.
 */

#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

/* Offset into the firmware image where executable DSP code begins. */
#define AGDSP_BOOT_OFFSET	0x80
/* Magic written to the boot-protect register to unlock the boot vector. */
#define AGDSP_BOOT_PROTECT_MAGIC	0x9620

/*
 * Boot-control registers, in start-sequence-friendly order. The first
 * CTRL_BOOT_MAX are required; the trailing status registers are optional
 * (used only for a post-boot read-back diagnostic).
 */
enum audcp_ctrl {
	CTRL_SYS_SHUTDOWN,
	CTRL_CORE_SHUTDOWN,
	CTRL_DEEP_SLEEP,
	CTRL_CORE_RESET,
	CTRL_SYS_RESET,
	CTRL_RESET_SEL,
	CTRL_BOOTPROTECT,
	CTRL_BOOTVECTOR,
	CTRL_BOOTADDRESS_SEL,
	CTRL_BOOT_MAX,
	CTRL_SYS_STATUS = CTRL_BOOT_MAX,
	CTRL_CORE_STATUS,
	CTRL_SLEEP_STATUS,
	CTRL_MAX,
};

/* DT property names (match the stock audiocp_boot node for 1:1 transcription). */
static const char * const audcp_ctrl_name[CTRL_MAX] = {
	[CTRL_SYS_SHUTDOWN]	= "sysshutdown",
	[CTRL_CORE_SHUTDOWN]	= "coreshutdown",
	[CTRL_DEEP_SLEEP]	= "deepsleep",
	[CTRL_CORE_RESET]	= "corereset",
	[CTRL_SYS_RESET]	= "sysreset",
	[CTRL_RESET_SEL]	= "reset_sel",
	[CTRL_BOOTPROTECT]	= "bootprotect",
	[CTRL_BOOTVECTOR]	= "bootvector",
	[CTRL_BOOTADDRESS_SEL]	= "bootaddress_sel",
	[CTRL_SYS_STATUS]	= "sysstatus",
	[CTRL_CORE_STATUS]	= "corestatus",
	[CTRL_SLEEP_STATUS]	= "sleepstatus",
};

struct audcp_boot {
	struct device *dev;
	struct regmap *map[CTRL_MAX];
	u32 reg[CTRL_MAX];
	u32 mask[CTRL_MAX];
	void __iomem *fw_virt;
	phys_addr_t fw_phys;
	size_t fw_size;
	void __iomem *smsg_virt;
	size_t smsg_size;
	u32 boot_vector;
};

/* Set the field selected by mask[c] to all-ones (val != 0) or all-zeros. */
static void audcp_set(struct audcp_boot *b, enum audcp_ctrl c, bool on)
{
	regmap_update_bits(b->map[c], b->reg[c], b->mask[c],
			   on ? b->mask[c] : 0);
}

/* Write a value into the field selected by mask[c] (shifted into place). */
static void audcp_write_field(struct audcp_boot *b, enum audcp_ctrl c, u32 val)
{
	regmap_update_bits(b->map[c], b->reg[c], b->mask[c],
			   val << (ffs(b->mask[c]) - 1));
}

static void audcp_boot_start(struct audcp_boot *b)
{
	/* Clean the IPC ring so rd/wr pointers start at 0. */
	if (b->smsg_virt)
		memset_io(b->smsg_virt, 0, b->smsg_size);

	/* Route reset control to the audio CP. */
	audcp_set(b, CTRL_RESET_SEL, false);

	/* Hold the DSP in reset while we set it up. */
	audcp_set(b, CTRL_CORE_RESET, true);
	audcp_set(b, CTRL_SYS_RESET, true);

	/* Power up: clear the force-shutdown / deep-sleep gates. */
	audcp_set(b, CTRL_SYS_SHUTDOWN, false);
	audcp_set(b, CTRL_CORE_SHUTDOWN, false);
	audcp_set(b, CTRL_DEEP_SLEEP, false);

	/* Unlock and program the boot vector. */
	audcp_write_field(b, CTRL_BOOTPROTECT, AGDSP_BOOT_PROTECT_MAGIC);
	audcp_write_field(b, CTRL_BOOTVECTOR, b->boot_vector);
	audcp_set(b, CTRL_BOOTADDRESS_SEL, true);

	/* Release reset - the DSP starts executing from the boot vector. */
	audcp_set(b, CTRL_CORE_RESET, false);
	audcp_set(b, CTRL_SYS_RESET, false);
}

static int audcp_boot_load_firmware(struct audcp_boot *b)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, "sprd/ums512-agdsp.bin", b->dev);
	if (ret)
		return dev_err_probe(b->dev, ret, "failed to load agdsp firmware\n");

	if (fw->size > b->fw_size) {
		dev_err(b->dev, "firmware (%zu) larger than region (%zu)\n",
			fw->size, b->fw_size);
		release_firmware(fw);
		return -EFBIG;
	}

	memcpy_toio(b->fw_virt, fw->data, fw->size);
	release_firmware(fw);

	b->boot_vector = (b->fw_phys + AGDSP_BOOT_OFFSET) >> 1;
	return 0;
}

static int audcp_boot_map_region(struct device *dev, int idx,
				 void __iomem **virt, phys_addr_t *phys,
				 size_t *size)
{
	struct reserved_mem *rmem;
	struct device_node *np;

	np = of_parse_phandle(dev->of_node, "memory-region", idx);
	if (!np)
		return -ENODEV;

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem)
		return -EINVAL;

	*virt = devm_ioremap_wc(dev, rmem->base, rmem->size);
	if (!*virt)
		return -ENOMEM;
	if (phys)
		*phys = rmem->base;
	if (size)
		*size = rmem->size;
	return 0;
}

static int audcp_boot_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct audcp_boot *b;
	int i, ret;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;

	for (i = 0; i < CTRL_MAX; i++) {
		u32 args[2];

		b->map[i] = syscon_regmap_lookup_by_phandle_args(dev->of_node,
				audcp_ctrl_name[i], 2, args);
		if (IS_ERR(b->map[i])) {
			/* Status registers are optional (read-back only). */
			if (i >= CTRL_BOOT_MAX) {
				b->map[i] = NULL;
				continue;
			}
			return dev_err_probe(dev, PTR_ERR(b->map[i]),
					     "missing boot-control '%s'\n",
					     audcp_ctrl_name[i]);
		}
		b->reg[i] = args[0];
		b->mask[i] = args[1];
	}

	/* memory-region 0: firmware load (DSP bin); 1 (optional): IPC ring. */
	ret = audcp_boot_map_region(dev, 0, &b->fw_virt, &b->fw_phys, &b->fw_size);
	if (ret)
		return dev_err_probe(dev, ret, "no firmware memory-region\n");

	audcp_boot_map_region(dev, 1, &b->smsg_virt, NULL, &b->smsg_size);

	ret = audcp_boot_load_firmware(b);
	if (ret)
		return ret;

	/*
	 * Power the AGCP/AGDSP domain (genpd power_on: AON access bit + PMU
	 * wakeup) before releasing the DSP from reset. Keep it on for the
	 * device's lifetime (no pm_runtime_put).
	 */
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "failed to power agdsp domain\n");
	}

	audcp_boot_start(b);
	platform_set_drvdata(pdev, b);

	dev_info(dev, "audio DSP booted (fw@%pa, vector=%#x)\n",
		 &b->fw_phys, b->boot_vector);

	/* Best-effort read-back of the DSP power/sleep status for diagnosis. */
	if (b->map[CTRL_CORE_STATUS] || b->map[CTRL_SYS_STATUS]) {
		u32 core = 0, sys = 0, sleep = 0;

		if (b->map[CTRL_CORE_STATUS])
			regmap_read(b->map[CTRL_CORE_STATUS],
				    b->reg[CTRL_CORE_STATUS], &core);
		if (b->map[CTRL_SYS_STATUS])
			regmap_read(b->map[CTRL_SYS_STATUS],
				    b->reg[CTRL_SYS_STATUS], &sys);
		if (b->map[CTRL_SLEEP_STATUS])
			regmap_read(b->map[CTRL_SLEEP_STATUS],
				    b->reg[CTRL_SLEEP_STATUS], &sleep);
		dev_info(dev, "status: core=%#x sys=%#x sleep=%#x\n",
			 core & b->mask[CTRL_CORE_STATUS],
			 sys & b->mask[CTRL_SYS_STATUS],
			 sleep & b->mask[CTRL_SLEEP_STATUS]);
	}
	return 0;
}

static const struct of_device_id audcp_boot_match[] = {
	{ .compatible = "sprd,ums512-audcp-boot" },
	{ }
};
MODULE_DEVICE_TABLE(of, audcp_boot_match);

static struct platform_driver audcp_boot_driver = {
	.probe = audcp_boot_probe,
	.driver = {
		.name = "sprd-audcp-boot",
		.of_match_table = audcp_boot_match,
	},
};
module_platform_driver(audcp_boot_driver);

MODULE_DESCRIPTION("Unisoc audio-DSP (AUDCP) boot driver");
MODULE_LICENSE("GPL");
