/*
 * sound/soc/sprd/i2s-null-codec.c
 *
 * Copyright (C) 2015 SpreadTrum Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "sprd-asoc-debug.h"
#define pr_fmt(fmt) pr_sprd_fmt("I2SNC")""fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include "sprd-asoc-common.h"
#include "sprd-i2s.h"

#include "sprd-asoc-card-utils.h"

#define NAME_SIZE	32

/*
 * The external Awinic AW87391 speaker amp is enabled via a direct exported
 * call (the same one the VBC card's spk-ext-pa hook uses), not a GPIO pulse.
 * Drive it from a DAPM speaker widget so it powers with the i2s0 stream.
 */
extern int anbernic_rgds_amp_enable(int on);

static int i2s_ext_spk_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	int on = SND_SOC_DAPM_EVENT_ON(event);
	int ret;

	ret = anbernic_rgds_amp_enable(on);
	if (ret && ret != -ENODEV)
		pr_warn("%s: rgds amp %s failed: %d\n", __func__,
			on ? "enable" : "disable", ret);
	if (on)
		msleep(22);	/* let the AMP settle before audio (VBC parity) */
	return 0;
}

static const struct snd_soc_dapm_widget i2s_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Ext Spk", i2s_ext_spk_event),
};

/* "Playback" is the dummy codec DAI's playback stream widget. */
static const struct snd_soc_dapm_route i2s_dapm_routes[] = {
	{ "Ext Spk", NULL, "Playback" },
};

#ifdef CONFIG_PROC_FS
static int i2s_debug_init(struct snd_soc_card *card)
{
	struct snd_info_entry *entry;

	entry = snd_info_create_card_entry(card->snd_card, "i2s-debug",
				card->snd_card->proc_root);
	if (entry != NULL) {
		entry->c.text.read = i2s_debug_read;
		entry->c.text.write = i2s_debug_write;
		entry->mode |= 0200;
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			entry = NULL;
		}
	}
	return 0;
}

static void i2s_register_proc_init(struct snd_soc_card *card)
{
	struct snd_info_entry *entry;

	if (!snd_card_proc_new(card->snd_card, "i2s-reg", &entry))
		snd_info_set_text_ops(entry, NULL, i2s_register_proc_read);
}

#else /* !CONFIG_PROC_FS */

static void i2s_register_proc_init(struct snd_soc_card *card)
{
}

static int i2s_debug_init(struct snd_soc_card *card)
{
	return 0;
}

#endif

static int board_late_probe(struct snd_soc_card *card)
{
	i2s_debug_init(card);
	i2s_register_proc_init(card);
	return 0;
}

#define I2S_CONFIG(xname, xreg) \
	SOC_SINGLE_EXT(xname, FUN_REG(xreg), 0, INT_MAX, 0, \
		i2s_config_get, i2s_config_set)

static const struct snd_kcontrol_new i2s_config_snd_controls[] = {
	I2S_CONFIG("fs", FS),
	I2S_CONFIG("force_fs", FORCE_FS),
	I2S_CONFIG("hw_port", HW_PROT),
	I2S_CONFIG("slave_timeout", SLAVE_TIMEOUT),
	I2S_CONFIG("bus_type", BUS_TYPE),
	I2S_CONFIG("byte_per_chan", BYTE_PER_CHAN),
	I2S_CONFIG("mode", MODE),
	I2S_CONFIG("lsb", LSB),
	I2S_CONFIG("rtx_mode", TRX_MODE),
	I2S_CONFIG("lrck_inv", LRCK_INV),
	I2S_CONFIG("sync_mode", SYNC_MODE),
	I2S_CONFIG("clk_inv", CLK_INV),
	I2S_CONFIG("i2s_bus_mode", I2S_BUS_MODE),
	I2S_CONFIG("pcm_bus_mode", PCM_BUS_MODE),
	I2S_CONFIG("pcm_slot", PCM_SLOT),
	I2S_CONFIG("pcm_cycle", PCM_CYCLE),
	I2S_CONFIG("tx_watermark", TX_WATERMARK),
	I2S_CONFIG("rx_watermark", RX_WATERMARK),
};

struct sprd_array_size sprd_alli2s_card_controls = {
	.ptr = i2s_config_snd_controls,
	.size = ARRAY_SIZE(i2s_config_snd_controls),
};

static int sprd_asoc_i2s_probe(struct platform_device *pdev)
{
	int ret;
	struct snd_soc_card *card;

	ret = asoc_sprd_card_probe(pdev, &card);
	if (ret) {
		pr_err("ERR: %s, asoc_sprd_card_probe failed!\n", __func__);
		return ret;
	}

	/* Add your special configurations here */
	/* Add card special kcontrols */
	card->controls = sprd_alli2s_card_controls.ptr;
	card->num_controls = sprd_alli2s_card_controls.size;

	/* Drive the external speaker amp from the playback DAPM path. */
	card->dapm_widgets = i2s_dapm_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(i2s_dapm_widgets);
	card->dapm_routes = i2s_dapm_routes;
	card->num_dapm_routes = ARRAY_SIZE(i2s_dapm_routes);

	card->late_probe = board_late_probe;

	return asoc_sprd_register_card(&pdev->dev, card);

}

#ifdef CONFIG_OF
static const struct of_device_id i2s_null_codec_of_match[] = {
	{.compatible = "unisoc,i2s-null-codec",},
	{},
};

MODULE_DEVICE_TABLE(of, i2s_null_codec_of_match);
#endif

static struct platform_driver sprd_asoc_i2s_driver = {
	.driver = {
		.name = "i2s-null-codec",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = i2s_null_codec_of_match,
	},
	.probe = sprd_asoc_i2s_probe,
	.remove = asoc_sprd_card_remove,
};

static int __init sprd_asoc_i2s_init(void)
{
	return platform_driver_register(&sprd_asoc_i2s_driver);
}

late_initcall_sync(sprd_asoc_i2s_init);

MODULE_DESCRIPTION("ALSA SoC SpreadTrum I2S");
MODULE_AUTHOR("Ken Kuang <ken.kuang@spreadtrum.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("machine:i2s");
