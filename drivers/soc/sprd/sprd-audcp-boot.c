// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spreadtrum/Unisoc audio-DSP (AGDSP / AUDCP) boot driver.
 */

#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>

#define AGDSP_FIRMWARE_NAME		"sprd/ums512-agdsp.bin"
#define AGDSP_BOOT_OFFSET		0x80
#define AGDSP_BOOT_PROTECT_MAGIC	0x9620

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
	CTRL_ACCESS_ENABLE,
	CTRL_MAX,
};

static const char * const audcp_ctrl_name[CTRL_MAX] = {
	[CTRL_SYS_SHUTDOWN] = "sysshutdown",
	[CTRL_CORE_SHUTDOWN] = "coreshutdown",
	[CTRL_DEEP_SLEEP] = "deepsleep",
	[CTRL_CORE_RESET] = "corereset",
	[CTRL_SYS_RESET] = "sysreset",
	[CTRL_RESET_SEL] = "reset_sel",
	[CTRL_BOOTPROTECT] = "bootprotect",
	[CTRL_BOOTVECTOR] = "bootvector",
	[CTRL_BOOTADDRESS_SEL] = "bootaddress_sel",
	[CTRL_SYS_STATUS] = "sysstatus",
	[CTRL_CORE_STATUS] = "corestatus",
	[CTRL_SLEEP_STATUS] = "sleepstatus",
	[CTRL_ACCESS_ENABLE] = "accessenable",
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
	u32 dsp_reboot_mode;
	struct generic_pm_domain genpd;
};

static size_t audcp_firmware_size(const struct firmware *fw)
{
	static const u8 avb_magic[] = { 'A', 'V', 'B', 'f' };

	if (fw->size >= 64 &&
	    !memcmp(fw->data + fw->size - 64, avb_magic, sizeof(avb_magic)))
		return fw->size - 64;

	return fw->size;
}

static void audcp_set(struct audcp_boot *b, enum audcp_ctrl c, bool on)
{
	regmap_update_bits(b->map[c], b->reg[c], b->mask[c],
			   on ? b->mask[c] : 0);
}

static void audcp_write_field(struct audcp_boot *b, enum audcp_ctrl c, u32 val)
{
	regmap_update_bits(b->map[c], b->reg[c], b->mask[c],
			   val << (ffs(b->mask[c]) - 1));
}

static void audcp_boot_start(struct audcp_boot *b)
{
	if (b->smsg_virt)
		memset_io(b->smsg_virt, 0, b->smsg_size);

	audcp_set(b, CTRL_RESET_SEL, false);
	audcp_set(b, CTRL_CORE_RESET, true);
	audcp_set(b, CTRL_SYS_RESET, true);
	audcp_set(b, CTRL_SYS_SHUTDOWN, false);
	audcp_set(b, CTRL_CORE_SHUTDOWN, false);
	audcp_set(b, CTRL_DEEP_SLEEP, false);
	audcp_write_field(b, CTRL_BOOTPROTECT, AGDSP_BOOT_PROTECT_MAGIC);

	if (!b->dsp_reboot_mode)
		audcp_write_field(b, CTRL_BOOTVECTOR, b->boot_vector);

	audcp_set(b, CTRL_BOOTADDRESS_SEL, true);
	audcp_set(b, CTRL_CORE_RESET, false);
	audcp_set(b, CTRL_SYS_RESET, false);
}

static int audcp_boot_load_firmware(struct audcp_boot *b)
{
	const struct firmware *fw;
	size_t fw_size;
	int ret;

	ret = request_firmware(&fw, AGDSP_FIRMWARE_NAME, b->dev);
	if (ret)
		return dev_err_probe(b->dev, ret, "failed to load agdsp firmware\n");

	fw_size = audcp_firmware_size(fw);
	if (!fw_size || fw_size > b->fw_size) {
		dev_err(b->dev, "firmware (%#zx) larger than region (%#zx)\n",
			fw_size, b->fw_size);
		release_firmware(fw);
		return -EFBIG;
	}

	memcpy_toio(b->fw_virt, fw->data, fw_size);
	if (fw_size < b->fw_size)
		memset_io(b->fw_virt + fw_size, 0, b->fw_size - fw_size);

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

	of_property_read_u32(dev->of_node, "dsp-reboot-mode", &b->dsp_reboot_mode);

	for (i = 0; i < CTRL_MAX; i++) {
		u32 args[2];

		b->map[i] = syscon_regmap_lookup_by_phandle_args(dev->of_node,
				audcp_ctrl_name[i], 2, args);
		if (IS_ERR(b->map[i])) {
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

	ret = audcp_boot_map_region(dev, 0, &b->fw_virt, &b->fw_phys, &b->fw_size);
	if (ret)
		return dev_err_probe(dev, ret, "no firmware memory-region\n");

	audcp_boot_map_region(dev, 1, &b->smsg_virt, NULL, &b->smsg_size);

	ret = audcp_boot_load_firmware(b);
	if (ret)
		return ret;

	/*
	 * The boot sequence drives the AUDCP power/reset controls directly
	 * through the PMU/AON regmaps, exactly like the vendor
	 * sprd_audcp_boot driver.
	 */
	audcp_boot_start(b);

	/*
	 * Everything in AGCP address space (audcpahb/audcpapb clock gates,
	 * the AGCP DMA controller, VBC, MCDT, the digital codec) hangs the
	 * bus when touched before the DSP is up and AP access is granted.
	 * Mainline consumers (clk framework, sprd-dma) cannot take the
	 * vendor agdsp-access votes, so expose an always-on power domain:
	 * consumers gain both probe ordering (genpd attach defers them
	 * until this driver has booted the DSP) and a permanently-set
	 * AP-access-enable bit.
	 */
	if (b->map[CTRL_ACCESS_ENABLE])
		audcp_set(b, CTRL_ACCESS_ENABLE, true);

	b->genpd.name = dev_name(dev);
	b->genpd.flags = GENPD_FLAG_ALWAYS_ON;
	ret = pm_genpd_init(&b->genpd, NULL, false);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init power domain\n");

	ret = of_genpd_add_provider_simple(dev->of_node, &b->genpd);
	if (ret) {
		pm_genpd_remove(&b->genpd);
		return dev_err_probe(dev, ret, "failed to add genpd provider\n");
	}

	platform_set_drvdata(pdev, b);

	dev_info(dev, "audio DSP booted (fw@%pa, vector=%#x)\n",
		 &b->fw_phys, b->boot_vector);

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
