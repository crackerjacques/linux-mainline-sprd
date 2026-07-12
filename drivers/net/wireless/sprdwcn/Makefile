# SPDX-License-Identifier: GPL-2.0
#
# Unisoc Marlin3-Lite (SC2355) external WCN combo over SDIO.
# Trimmed from the 5.4 vendor BSP to the external-SDIO path only:
# integrated-chip boot (wcn_integrate*), SIPC and PCIe transports removed.
#
subdir-ccflags-y := \
	-I $(srctree)/$(src)/include \
	-I $(srctree)/$(src)/boot \
	-I $(srctree)/$(src)/platform \
	-I $(srctree)/$(src)/platform/rf \
	-I $(srctree)/$(src)/platform/gnss \
	-I $(srctree)/$(src)/sleep

obj-$(CONFIG_UNISOC_WCN_BSP) += wcn_bsp.o

# core bus + unified platform-driver entry
wcn_bsp-y += wcn_bus.o
wcn_bsp-y += sprd_wcn.o

# platform (external marlin3 boot + glue)
wcn_bsp-y += platform/wcn_parn_parser.o
wcn_bsp-y += platform/wcn_txrx.o
wcn_bsp-y += platform/wcn_log.o
wcn_bsp-y += platform/wcn_misc.o
wcn_bsp-y += platform/bufring.o
wcn_bsp-y += platform/wcn_op.o
wcn_bsp-y += platform/sysfs.o
wcn_bsp-y += platform/reset.o
wcn_bsp-y += platform/loopcheck.o
wcn_bsp-y += platform/wcn_boot.o
wcn_bsp-y += platform/mem_pd_mgr.o
wcn_bsp-y += platform/wcn_pm_qos.o
wcn_bsp-y += platform/wcn_procfs.o
# Tier-2 dropped for the boot milestone (see wcn_stubs.c for the few symbols the
# core still references): wcn_debugfs (0 core calls), wcn_dump (0 core calls,
# set_fs file I/O), wcn_chr (debug char dev), wcn_ca_trusty (Trusty TEE, absent
# from mainline), gnss/gnss_dump/wcn_gnss_dump (GNSS, deferred past boot).
wcn_bsp-y += platform/wcn_stubs.o

# sleep / power
wcn_bsp-y += sleep/sdio_int.o
wcn_bsp-y += sleep/slp_mgr.o
wcn_bsp-y += sleep/slp_sdio.o

# sdio transport (sdiohal)
wcn_bsp-y += sdio/sdiohal_main.o
wcn_bsp-y += sdio/sdiohal_common.o
wcn_bsp-y += sdio/sdiohal_tx.o
wcn_bsp-y += sdio/sdiohal_rx.o
wcn_bsp-y += sdio/sdiohal_if.o
