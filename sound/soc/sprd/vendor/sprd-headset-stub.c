// SPDX-License-Identifier: GPL-2.0-only
/*
 * No-op headset stubs: jack detection is not required for the
 * NORMAL_AP01 speaker playback path. sprd_codec_intc_irq/enable live in
 * sprd-codec.c itself.
 */
#include <linux/platform_device.h>

#include "sprd-headset.h"

void sprd_headset_set_global_variables(struct sprd_headset_global_vars *glb)
{
}

int sprd_headset_soc_probe(struct snd_soc_component *codec)
{
	return -ENODEV;
}

int headset_register_notifier(struct notifier_block *nb)
{
	return 0;
}

int headset_unregister_notifier(struct notifier_block *nb)
{
	return 0;
}

int headset_get_plug_state(void)
{
	return 0;
}

void sprd_headset_remove(void)
{
}

void headset_set_audio_state(bool enable)
{
}

/*
 * sprd_codec_intc_enable's real implementation lives in the (unported)
 * sprd-headset-sc2730 driver, which owns the codec interrupt controller.
 */
void sprd_codec_intc_enable(bool enable, u32 irq_bit)
{
}

/*
 * SIA81xx / SIPA smart-PA hooks: that external amplifier family is not
 * fitted on this board (the RG Rotate uses a GPIO-controlled aw87xxx),
 * so the vendor card's optional aux hooks collapse to no-ops.
 */
int sipa_audio_power_scene_set(int on)
{
	return 0;
}

int soc_aux_init_only_sia81xx(struct platform_device *pdev,
			      struct snd_soc_card *card)
{
	return 0;
}
