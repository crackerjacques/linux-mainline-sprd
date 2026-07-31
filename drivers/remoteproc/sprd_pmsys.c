// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Otto Pflüger
 */

#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>

#include "sprd_common.h"

struct sprd_pmsys_info {
	u32 corereset_reg;
	u32 corereset_mask;
	/*
	 * Some SoCs leave the whole SP subsystem in reset and in forced deep
	 * sleep out of AP reset. Its IRAM - the reg window the boot stub is
	 * written to - does not answer until both are cleared, and touching it
	 * early hangs the bus rather than failing. Where that applies, these
	 * describe the two extra controls; sysreset lives in AON_APB next to
	 * corereset, deepsleep in PMU_APB. Zero means the SoC needs no such
	 * sequence and no PMU_APB handle.
	 */
	u32 sysreset_reg;
	u32 sysreset_mask;
	u32 deepsleep_reg;
	u32 deepsleep_mask;
};

struct sprd_pmsys {
	struct device *dev;
	struct sprd_sipc_subdev sipc;
	struct reset_control *reset;
	struct regmap *aon_apb_regs;
	struct regmap *pmu_apb_regs;
	const struct sprd_pmsys_info *info;
	phys_addr_t mem_base;
	size_t mem_size;
	const char *sys_fw_name;
	void *bootmem;
	size_t bootmem_size;
};

/*
 * Bring the SP subsystem far enough up that its IRAM is reachable. rproc calls
 * prepare before load, which is where the boot stub is written, so this has to
 * happen here rather than in start. The order mirrors u-boot's
 * pmic_arm7_RAM_active(): SP_SYS out of reset, forced deep sleep cleared, then
 * CM4_SYS out of reset. The core itself stays in reset until start.
 */
static int sprd_pmsys_prepare(struct rproc *rproc)
{
	struct sprd_pmsys *p = rproc->priv;
	int ret;

	if (!p->info->sysreset_mask)
		return 0;

	/*
	 * Hold the core first. Its reset state out of AP reset is not
	 * guaranteed, and once SP_SYS comes up a released core would start
	 * executing whatever its IRAM happens to hold - before load has put
	 * the boot stub there.
	 */
	ret = regmap_set_bits(p->aon_apb_regs, p->info->corereset_reg,
			      p->info->corereset_mask);
	if (ret)
		return ret;

	ret = reset_control_deassert(p->reset);
	if (ret < 0)
		return ret;

	ret = regmap_clear_bits(p->pmu_apb_regs, p->info->deepsleep_reg,
				p->info->deepsleep_mask);
	if (ret)
		return ret;

	ret = regmap_clear_bits(p->aon_apb_regs, p->info->sysreset_reg,
				p->info->sysreset_mask);
	if (ret)
		return ret;

	/* vendor code settles for 50ms before touching the subsystem */
	msleep(50);

	return 0;
}

static int sprd_pmsys_unprepare(struct rproc *rproc)
{
	struct sprd_pmsys *p = rproc->priv;

	if (!p->info->sysreset_mask)
		return 0;

	regmap_set_bits(p->aon_apb_regs, p->info->sysreset_reg,
			p->info->sysreset_mask);
	regmap_set_bits(p->pmu_apb_regs, p->info->deepsleep_reg,
			p->info->deepsleep_mask);

	return reset_control_assert(p->reset);
}

static int sprd_pmsys_load(struct rproc *rproc, const struct firmware *fw)
{
	struct sprd_pmsys *p = rproc->priv;
	int ret = 0;
	void *mem;

	if (fw->size > p->bootmem_size) {
		dev_err(p->dev, "bootcode firmware too large\n");
		return -ENOMEM;
	}

	memcpy_toio(p->bootmem, fw->data, fw->size);

	mem = memremap(p->mem_base, p->mem_size, MEMREMAP_WC);
	if (!mem) {
		dev_err(p->dev, "failed to map memory region\n");
		return -EBUSY;
	}

	ret = request_firmware(&fw, p->sys_fw_name, p->dev);
	if (ret)
		goto out_unmap;

	if (fw->size > p->mem_size) {
		dev_err(p->dev, "pmsys firmware too large\n");
		ret = -ENOMEM;
		goto out_release;
	}

	memcpy(mem, fw->data, fw->size);

out_release:
	release_firmware(fw);
out_unmap:
	memunmap(mem);
	return ret;
}

static int sprd_pmsys_start(struct rproc *rproc)
{
	struct sprd_pmsys *p = rproc->priv;
	int ret;

	ret = reset_control_deassert(p->reset);
	if (ret < 0)
		return ret;

	/* start processor */
	regmap_clear_bits(p->aon_apb_regs, p->info->corereset_reg,
			  p->info->corereset_mask);

	return 0;
}

static int sprd_pmsys_stop(struct rproc *rproc)
{
	struct sprd_pmsys *p = rproc->priv;

	/* stop processor */
	regmap_set_bits(p->aon_apb_regs, p->info->corereset_reg,
			p->info->corereset_mask);

	reset_control_assert(p->reset);

	return 0;
}

static const struct rproc_ops sprd_pmsys_ops = {
	.prepare	= sprd_pmsys_prepare,
	.unprepare	= sprd_pmsys_unprepare,
	.load		= sprd_pmsys_load,
	.start		= sprd_pmsys_start,
	.stop		= sprd_pmsys_stop,
};

static int sprd_pmsys_parse_memory_region(struct sprd_pmsys *p)
{
	struct device_node *np;
	struct reserved_mem *rmem;

	np = of_parse_phandle(p->dev->of_node, "memory-region", 0);
	if (!np) {
		dev_err(p->dev, "no memory region specified\n");
		return -EINVAL;
	}

	rmem = of_reserved_mem_lookup(np);
	if (!rmem) {
		of_node_put(np);
		dev_err(p->dev, "failed to look up memory region\n");
		return -EINVAL;
	}

	p->mem_base = rmem->base;
	p->mem_size = rmem->size;

	of_node_put(np);

	return 0;
}

static int sprd_pmsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *bootmem;
	struct sprd_pmsys *p;
	struct rproc *rproc;
	const char *fw_name;
	int ret;

	ret = of_property_read_string_index(dev->of_node, "firmware-name",
					    0, &fw_name);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to read firmware-name\n");
		return ret;
	}

	rproc = devm_rproc_alloc(dev, dev->of_node->name, &sprd_pmsys_ops,
				 fw_name, sizeof(*p));
	if (!rproc)
		return -ENOMEM;

	platform_set_drvdata(pdev, rproc);

	p = rproc->priv;
	p->dev = dev;

	ret = of_property_read_string_index(dev->of_node, "firmware-name",
					    1, &p->sys_fw_name);
	if (ret < 0) {
		dev_err(p->dev, "unable to read second firmware-name\n");
		return ret;
	}

	p->info = of_device_get_match_data(dev);
	if (!p->info)
		return -EINVAL;

	p->bootmem = devm_platform_get_and_ioremap_resource(pdev, 0, &bootmem);
	if (IS_ERR(p->bootmem))
		return PTR_ERR(p->bootmem);

	p->bootmem_size = bootmem->end - bootmem->start + 1;

	ret = sprd_pmsys_parse_memory_region(p);
	if (ret < 0)
		return ret;

	p->aon_apb_regs = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-aon-apb");
	if (IS_ERR(p->aon_apb_regs)) {
		dev_err(p->dev, "failed to get aon-apb syscon handle\n");
		return PTR_ERR(p->aon_apb_regs);
	}

	if (p->info->deepsleep_mask) {
		p->pmu_apb_regs = syscon_regmap_lookup_by_phandle(dev->of_node,
								  "sprd,syscon-pmu-apb");
		if (IS_ERR(p->pmu_apb_regs))
			return dev_err_probe(dev, PTR_ERR(p->pmu_apb_regs),
					     "failed to get pmu-apb syscon handle\n");
	}

	p->reset = devm_reset_control_get_optional(dev, NULL);
	if (IS_ERR(p->reset)) {
		dev_err(p->dev, "failed to get pmsys reset\n");
		return PTR_ERR(p->reset);
	}

	ret = devm_sprd_rproc_add_sipc_subdev(rproc, dev->of_node, &p->sipc);
	if (ret < 0) {
		dev_err(dev, "failed to add SIPC subdev\n");
		return ret;
	}

	ret = devm_rproc_add(dev, rproc);
	if (ret < 0) {
		dev_err(dev, "failed to add rproc\n");
		return ret;
	}

	return 0;
}

static const struct sprd_pmsys_info ums9230_pmsys_info = {
	.corereset_reg = 0x008c,
	.corereset_mask = BIT(0),
};

/*
 * ums512 keeps the SP core reset in the same AON_APB bit as ums9230. The
 * vendor 5.4 DT states it independently - its unisoc,modem node carries
 * syscon2 = <&aon_apb 0x8c 0x1> under syscon-names "corereset" - so the two
 * agreeing is a cross-check, not an assumption inherited from ums9230.
 */
static const struct sprd_pmsys_info ums512_pmsys_info = {
	.corereset_reg = 0x008c,
	.corereset_mask = BIT(0),
	/* AON_APB CM4_SYS_SOFT_RST shares the register with corereset */
	.sysreset_reg = 0x008c,
	.sysreset_mask = BIT(4),
	/* PMU_APB SLEEP_CTRL, the register audcp-boot also uses */
	.deepsleep_reg = 0x00cc,
	.deepsleep_mask = BIT(20),
};

static const struct of_device_id sprd_pmsys_of_match[] = {
	{ .compatible = "sprd,ums9230-pmsys", .data = &ums9230_pmsys_info },
	{ .compatible = "sprd,ums512-pmsys", .data = &ums512_pmsys_info },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_pmsys_of_match);

static struct platform_driver sprd_pmsys_driver = {
	.probe = sprd_pmsys_probe,
	.driver = {
		.name = "sprd-pmsys",
		.of_match_table = sprd_pmsys_of_match,
	},
};

module_platform_driver(sprd_pmsys_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unisoc PMSYS remoteproc driver");
