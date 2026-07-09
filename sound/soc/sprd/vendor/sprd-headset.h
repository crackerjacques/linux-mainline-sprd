/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal headset interface for the ported UMS512 vendor audio stack.
 *
 * The full sprd-headset-sc2730 jack-detection driver is not ported yet;
 * sprd-headset-stub.c provides no-op implementations so sprd-codec.c can
 * drive the playback path (speaker routes via "HP Pin") without it.
 */
#ifndef __HEADSET_SPRD_H__
#define __HEADSET_SPRD_H__

#define TO_STRING(e) #e

#include <linux/notifier.h>
#include <linux/regmap.h>
#include <sound/soc.h>

struct sprd_headset_global_vars {
	struct regmap *regmap;
	unsigned long codec_reg_offset;
};

void sprd_headset_set_global_variables(struct sprd_headset_global_vars *glb);
int sprd_headset_soc_probe(struct snd_soc_component *codec);
int headset_register_notifier(struct notifier_block *nb);
int headset_unregister_notifier(struct notifier_block *nb);
int headset_get_plug_state(void);
void sprd_headset_remove(void);
void sprd_codec_intc_irq(struct snd_soc_component *codec, u32 int_shadow);
void headset_set_audio_state(bool enable);
void sprd_codec_intc_enable(bool enable, u32 irq_bit);

#endif
