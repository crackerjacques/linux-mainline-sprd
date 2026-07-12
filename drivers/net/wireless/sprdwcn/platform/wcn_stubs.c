// SPDX-License-Identifier: GPL-2.0
/*
 * Stubs for WCN subsystems dropped from the boot-milestone build of the
 * external SDIO Marlin3-Lite (SC2355) driver:
 *
 *  - crash dump (wcn_dump / wcn_gnss_dump), debug char dev (wcn_chr),
 *    Trusty secure firmware verify (wcn_ca_trusty), GNSS (gnss / gnss_dump),
 *    debug-bus, RF calibration (rf), and the whole SoC-integrated WCN path
 *    (wcn_integrate*), which is never taken on this external-SDIO board.
 *
 * Every symbol below is referenced only on runtime-guarded integrated paths or
 * on debug/diagnostic paths that are inactive during bring-up. Providing no-op
 * definitions lets the trimmed module link. Re-add the corresponding source
 * files to the Makefile to restore real functionality.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include "wcn_glb.h"
#include "wcn_boot.h"
#include "../boot/wcn_integrate_dev.h"

/* ---- data symbols (were defined in the integrated core) ---------------- */
int is_wcn_shutdown;
struct wcn_device_manage s_wcn_device;

/* ---- wcn_dump.c / wcn_dump_integrate.c --------------------------------- */
int mdbg_dump_mem(void) { return 0; }
int dump_arm_reg(void) { return 0; }
int dump_arm_reg_integ(void) { return 0; }
void mdbg_dump_mem_integ(void) { }
void mdbg_hold_cpu(void) { }
int mdbg_snap_shoot_iram(void *buf) { return 0; }

/* ---- wcn_gnss_dump.c ---------------------------------------------------- */
int wcn_gnss_dump_init(void) { return 0; }
void wcn_gnss_dump_exit(void) { }

/* ---- gnss/gnss.c + gnss_dump.c ----------------------------------------- */
int gnss_data_init(void) { return 0; }
int gnss_boot_wait(void) { return 0; }
int gnss_backup_data(void) { return 0; }
int gnss_write_data(void) { return 0; }
void gnss_file_path_set(char *buf) { }

/* ---- wcn_chr.c ---------------------------------------------------------- */
int wcn_chr_init(void) { return 0; }
int wcn_chr_write(char *buf, size_t len) { return 0; }
int wcn_chr_report_event(char *str, u32 index) { return 0; }

/* ---- wcn_ca_trusty.c (no TEE here; report "verified") ------------------ */
int wcn_firmware_sec_verify(u32 wcn_or_gnss_bin, phys_addr_t bin_base_addr,
			    u32 bin_length)
{
	return 0;
}

/* ---- wcn_debug_bus.c ---------------------------------------------------- */
void debug_bus_show(char *show) { }

/* ---- reset_test.c (test-only) ------------------------------------------ */
int reset_test_init(void) { return 0; }

/* ---- integrated WCN core (wcn_integrate*): guarded-out on external SDIO - */
int start_integ_marlin(u32 subsys) { return 0; }
int stop_integ_marlin(u32 subsys) { return 0; }
int integ_marlin_get_power(void) { return 0; }
int integ_marlin_get_module_status(void) { return 0; }
void integ_wcn_chip_power_off(void) { }
const char *integ_wcn_get_chip_name(void) { return ""; }
int integ_wcn_get_module_status_changed(void) { return 0; }
void integ_wcn_set_module_status_changed(bool status) { }
enum wcn_clock_mode integ_wcn_get_xtal_26m_clk_mode(void) { return WCN_CLOCK_MODE_UNKNOWN; }
enum wcn_clock_type integ_wcn_get_xtal_26m_clk_type(void) { return WCN_CLOCK_TYPE_UNKNOWN; }
char *integ_gnss_firmware_path_get(void) { return NULL; }
