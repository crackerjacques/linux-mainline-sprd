# SPDX-License-Identifier: GPL-2.0-only

# The vendor BSP passes this per-board from its external Kbuild files
# (sharkl5pro boards build the SC2730 codec flavour).
ccflags-y += -DCONFIG_SND_SOC_UNISOC_CODEC_SC2730

# Link order matters: these are all built-in platform drivers registered
# at device_initcall time, and probes fire in registration order. The
# shared-memory manager and agdsp-access must be live before audio-sipc,
# which must precede the DAI/codec/card drivers.
snd-soc-unisoc-vbc-v4-sc2730-y := \
	audio_mem.o \
	agdsp_access.o \
	audio-smsg.o \
	audio-sipc.o \
	mcdt_hw.o \
	sprd-asoc-common.o \
	dummy-codec.o \
	sprd-dmaengine-pcm.o \
	sprd-platform-pcm-routing.o \
	sprd-fe-dai.o \
	vbc-dai.o \
	vbc-phy-v4.o \
	sprd-audio-power.o \
	sprd-audio-power-dev.o \
	sprd-codec.o \
	sprd-headset-stub.o \
	sprd-asoc-card-utils.o \
	sprd-asoc-card-utils-hook.o \
	sprd-asoc-card-utils-legacy.o \
	vbc-rxpx-codec-sc27xx.o

obj-$(CONFIG_SND_SOC_UNISOC_VBC_V4_SC2730) += snd-soc-unisoc-vbc-v4-sc2730.o
