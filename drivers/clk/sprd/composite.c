// SPDX-License-Identifier: GPL-2.0
//
// Spreadtrum composite clock driver
//
// Copyright (C) 2017 Spreadtrum, Inc.
// Author: Chunyan Zhang <chunyan.zhang@spreadtrum.com>

#include <linux/clk-provider.h>

#include "composite.h"

static int sprd_comp_determine_rate(struct clk_hw *hw,
				    struct clk_rate_request *req)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return divider_determine_rate(hw, req, NULL, cc->div.width, 0);
}

static int sprd_comp_determine_rate_reparent(struct clk_hw *hw,
					     struct clk_rate_request *req)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);
	unsigned int i, num_parents = clk_hw_get_num_parents(hw);
	unsigned long max_div = 1UL << cc->div.width;
	unsigned long floor_rate = 0, floor_prate = 0;
	unsigned long any_rate = ULONG_MAX, any_prate = 0;
	struct clk_hw *floor_parent = NULL, *any_parent = NULL;

	for (i = 0; i < num_parents; i++) {
		struct clk_hw *parent = clk_hw_get_parent_by_index(hw, i);
		unsigned long prate, div, rate;

		if (!parent)
			continue;
		prate = clk_hw_get_rate(parent);
		if (!prate)
			continue;

		div = DIV_ROUND_UP(prate, req->rate);
		div = clamp(div, 1UL, max_div);
		rate = prate / div;

		/* best rate <= target */
		if (rate <= req->rate && rate > floor_rate) {
			floor_rate = rate;
			floor_prate = prate;
			floor_parent = parent;
		}
		/* fallback: lowest rate overall if everything overshoots */
		if (rate < any_rate) {
			any_rate = rate;
			any_prate = prate;
			any_parent = parent;
		}
	}

	if (floor_parent) {
		req->best_parent_hw = floor_parent;
		req->best_parent_rate = floor_prate;
		req->rate = floor_rate;
	} else if (any_parent) {
		req->best_parent_hw = any_parent;
		req->best_parent_rate = any_prate;
		req->rate = any_rate;
	} else {
		return -EINVAL;
	}

	return 0;
}

static unsigned long sprd_comp_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_div_helper_recalc_rate(&cc->common, &cc->div, parent_rate);
}

static int sprd_comp_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_div_helper_set_rate(&cc->common, &cc->div,
				       rate, parent_rate);
}

static unsigned int sprd_comp_gate_off(const struct sprd_clk_common *common)
{
	unsigned int en;

	regmap_read(common->regmap, common->reg, &en);
	en &= BIT(0);
	if (en)
		regmap_update_bits(common->regmap, common->reg, BIT(0), 0);
	return en;
}

static void sprd_comp_gate_restore(const struct sprd_clk_common *common,
				   unsigned int en)
{
	if (en)
		regmap_update_bits(common->regmap, common->reg, BIT(0), BIT(0));
}

static int sprd_comp_set_parent_gated(struct clk_hw *hw, u8 index)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);
	unsigned int en;
	int ret;

	en = sprd_comp_gate_off(&cc->common);
	ret = sprd_mux_helper_set_parent(&cc->common, &cc->mux, index);
	sprd_comp_gate_restore(&cc->common, en);

	return ret;
}

static int sprd_comp_set_rate_gated(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);
	unsigned int en;
	int ret;

	en = sprd_comp_gate_off(&cc->common);
	ret = sprd_div_helper_set_rate(&cc->common, &cc->div, rate, parent_rate);
	sprd_comp_gate_restore(&cc->common, en);

	return ret;
}

/*
 * When a rate change also needs a new parent, the clk core prefers this single
 * op — so the whole reparent+redivide happens inside one gated (stopped) window
 * instead of two.
 */
static int sprd_comp_set_rate_and_parent_gated(struct clk_hw *hw,
					       unsigned long rate,
					       unsigned long parent_rate,
					       u8 index)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);
	unsigned int en;
	int ret;

	en = sprd_comp_gate_off(&cc->common);
	ret = sprd_mux_helper_set_parent(&cc->common, &cc->mux, index);
	if (!ret)
		ret = sprd_div_helper_set_rate(&cc->common, &cc->div, rate,
					       parent_rate);
	sprd_comp_gate_restore(&cc->common, en);

	return ret;
}

static u8 sprd_comp_get_parent(struct clk_hw *hw)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_mux_helper_get_parent(&cc->common, &cc->mux);
}

static int sprd_comp_set_parent(struct clk_hw *hw, u8 index)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_mux_helper_set_parent(&cc->common, &cc->mux, index);
}

const struct clk_ops sprd_comp_ops = {
	.get_parent	= sprd_comp_get_parent,
	.set_parent	= sprd_comp_set_parent,

	.determine_rate	= sprd_comp_determine_rate,
	.recalc_rate	= sprd_comp_recalc_rate,
	.set_rate	= sprd_comp_set_rate,
};
EXPORT_SYMBOL_GPL(sprd_comp_ops);

/* Like sprd_comp_ops but reparents the mux to reach rates on other parents. */
const struct clk_ops sprd_comp_reparent_ops = {
	.get_parent		= sprd_comp_get_parent,
	.set_parent		= sprd_comp_set_parent_gated,

	.determine_rate		= sprd_comp_determine_rate_reparent,
	.recalc_rate		= sprd_comp_recalc_rate,
	.set_rate		= sprd_comp_set_rate_gated,
	.set_rate_and_parent	= sprd_comp_set_rate_and_parent_gated,
};
EXPORT_SYMBOL_GPL(sprd_comp_reparent_ops);
