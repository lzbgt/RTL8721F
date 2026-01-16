
#include "ameba_soc.h"
#include "main.h"
#if (defined CONFIG_WHC_HOST || defined CONFIG_WHC_NONE || defined CONFIG_WHC_WPA_SUPPLICANT_OFFLOAD)
#include "vfs.h"
#endif
#include "os_wrapper.h"
#include "ameba_rtos_version.h"
#include "ssl_rom_to_ram_map.h"
//#include "wifi_fast_connect.h"
#if defined(CONFIG_BT_COEXIST)
#include "rtw_coex_ipc.h"
#endif
#include "ameba_diagnose.h"
#include "ameba_chipinfo.h"
#include "ameba_syscfg.h"
#include "ameba_boot.h"
#include "ameba_spic.h"

static const char *const TAG = "MAIN";
u32 use_hw_crypto_func;

#if (defined(CONFIG_BT) && CONFIG_BT) && (defined(CONFIG_BT_INIC) && CONFIG_BT_INIC)
#include "bt_inic.h"
#endif

#if defined(CONFIG_FTL_ENABLED) && CONFIG_FTL_ENABLED
#include "ftl_int.h"

void app_ftl_init(void)
{
	u32 ftl_start_addr, ftl_end_addr;

	flash_get_layout_info(FTL, &ftl_start_addr, &ftl_end_addr);
	ftl_phy_page_start_addr = ftl_start_addr - SPI_FLASH_BASE;
	ftl_phy_page_num = (ftl_end_addr - ftl_start_addr + 1) / PAGE_SIZE_4K;
	ftl_init(ftl_phy_page_start_addr, ftl_phy_page_num);
}
#endif

void app_fullmac_init(void)
{

}

void app_IWDG_refresh(void *arg)
{
	UNUSED(arg);
	WDG_Refresh(IWDG_DEV);
}

void app_IWDG_init(void)
{
	/* usually IWDG will enable by HW, and the bark interval is 4095ms by default */
	if (0 == (HAL_READ32(SYSTEM_CTRL_BASE, REG_AON_FEN) & APBPeriph_IWDG)) {
		return;
	}

	IWDG_LP_Enable(IWDG_DEV, DISABLE);
	/*set IWDG timeout to 4s*/
	WDG_Timeout(IWDG_DEV, 0x0FFF);
	WDG_Refresh(IWDG_DEV);
	RTK_LOGI(TAG, "IWDG refresh thread Started!\n");

	/* Due to inaccurate of Aon clk(50% precision), IWDG should be refreshed every 2S with the lowest priority level thread. */
	/* Writing to FLASH during OTA makes SYSTICK stop and run frequently,
	and SYSTICK may be 3-4 times slower than expected, so reduce refresh period to 1/4 of 2s. */
	rtos_timer_t xTimer = NULL;
	rtos_timer_create(&xTimer, "WDG_Timer", NULL, 500, TRUE, app_IWDG_refresh);

	if (xTimer == NULL) {
		RTK_LOGE(TAG, "IWDG refresh timer create error\n");
	} else {
		rtos_timer_start(xTimer, 0);
	}
}

void app_mbedtls_rom_init(void)
{
	ssl_function_map.ssl_calloc = (void *(*)(unsigned int, unsigned int))rtos_mem_calloc;
	ssl_function_map.ssl_free = (void (*)(void *))rtos_mem_free;
	ssl_function_map.ssl_printf = (long unsigned int (*)(const char *, ...))DiagPrintf;
	ssl_function_map.ssl_snprintf = (int (*)(char *s, size_t n, const char *format, ...))DiagSnPrintf;
}

void app_pmu_init(void)
{
	SOCPS_SleepInit();
	pmu_init_wakeup_timer();
	pmu_set_sleep_type(SLEEP_PG);

	/* only one core in fullmac mode */
#if !(!defined (CONFIG_WHC_INTF_IPC) && defined (CONFIG_WHC_DEV))
	/* If the current cpu is np, need to hold the lock of another cpu */
	if ((HAL_READ32(PMC_BASE, SYSPMC_CTRL) & PMC_BIT_CPU_IS_AP) == 0) {
		pmu_acquire_wakelock(PMU_CPU1_RUN);
	}
#endif
}

extern int rt_kv_init(void);

void app_filesystem_init(void)
{
#if !(defined(CONFIG_MP_SHRINK)) && (defined CONFIG_WHC_HOST || defined CONFIG_WHC_NONE || defined CONFIG_WHC_WPA_SUPPLICANT_OFFLOAD)
	int ret = 0;
	vfs_init();
#ifdef CONFIG_FATFS_WITHIN_APP_IMG
	ret = vfs_user_register(VFS_R3_PREFIX, VFS_FATFS, VFS_INF_FLASH, VFS_REGION_3, VFS_RO);
	if (ret == 0) {
		RTK_LOGI(TAG, "VFS-FAT Init Success \n");
	} else {
		RTK_LOGI(TAG, "VFS-FAT Init Fail \n");
	}
#endif

	vfs_user_register(VFS_PREFIX, VFS_LITTLEFS, VFS_INF_FLASH, VFS_REGION_1, VFS_RW);
	ret = rt_kv_init();
	if (ret == 0) {
		RTK_LOGI(TAG, "File System Init Success \n");
		return;
	}

	RTK_LOGE(TAG, "File System Init Fail \n");
#endif
}

/*
 * This function will be replaced when Sdk example is compiled using CMD "make EXAMPLE=xxx" or "make xip xxx"
 * To aviod compile error when example is not compiled
 */
_WEAK void app_pre_example(void)
{


}

_WEAK void app_example(void)
{


}

void CPU1_WDG_RST_Handler(void)
{
	/* Let NP run */
	HAL_WRITE32(SYSTEM_CTRL_BASE, REG_LSYS_BOOT_CFG, HAL_READ32(SYSTEM_CTRL_BASE, REG_LSYS_BOOT_CFG) | LSYS_BIT_BOOT_CPU1_RUN);

	/* clear CPU1_WDG_RST intr*/
	HAL_WRITE32(SYSTEM_CTRL_BASE, REG_AON_BOOT_REASON_HW, AON_BIT_RSTF_WDG0_CPU);
}

static const char *bdnum_to_str(u16 bd_num)
{
	switch (bd_num) {
	case SYSCFG_BD_QFN32:
		return "QFN32";
	case SYSCFG_BD_QFN48_MCM_8MBFlash:
		return "QFN48_MCM_8MBFlash";
	case SYSCFG_BD_QFN48:
		return "QFN48";
	case SYSCFG_BD_QFN68:
		return "QFN68";
	case SYSCFG_BD_QFN68_NEW:
		return "QFN68_NEW";
	default:
		return "UNKNOWN";
	}
}

static const char *psram_vendor_to_str(u16 vendor)
{
	switch (vendor) {
	case MCM_PSRAM_VENDOR_APM:
		return "APM";
	case MCM_PSRAM_VENDOR_WB:
		return "WB";
	default:
		return "UNKNOWN";
	}
}

static const char *psram_dqx_to_str(u16 dqx)
{
	switch (dqx) {
	case MCM_PSRAM_DQ8:
		return "DQ8";
	case MCM_PSRAM_DQ16:
		return "DQ16";
	default:
		return "DQ?";
	}
}

static const char *flash_type_to_str(u16 mem_type)
{
	if (mem_type & MCM_TYPE_NOR_FLASH) {
		return "NOR";
	}
	if (mem_type & MCM_TYPE_NAND_FLASH) {
		return "NAND";
	}
	return "NONE";
}

static uint64_t spic_flash_size_bytes(void)
{
	SPIC_TypeDef *spic = SPIC;
	const u32 reg = spic->FLASE_SIZE;
	const u32 exp = GET_FLASH_SIZE(reg) + 15; /* log2(size) - 15 stored */
	if (exp >= 63) {
		return 0;
	}
	return 1ULL << exp;
}

static void app_hw_info_print(void)
{
	char soc_name[32] = {0};
	ChipInfo_GetSocName_ToBuf(soc_name, sizeof(soc_name));

	const u8 cut = EFUSE_GetChipVersion();
	const u8 chip_info = EFUSE_GetChipInfo();
	const u16 bd_num = ChipInfo_BDNum();
	const MCM_MemTypeDef mcm = ChipInfo_MCMInfo();

	const uint64_t psram_bytes = (mcm.mem_type & MCM_TYPE_PSRAM) ? (uint64_t)MCM_MEM_SIZE_IN_BYTES(mcm.dram_info.density) : 0ULL;
	const uint64_t mcm_flash_bytes = (mcm.mem_type & (MCM_TYPE_NOR_FLASH | MCM_TYPE_NAND_FLASH)) ? (uint64_t)MCM_MEM_SIZE_IN_BYTES(mcm.flash_density) : 0ULL;
	const uint64_t ext_flash_bytes = spic_flash_size_bytes();

	const size_t bdram_heap_bytes = (size_t)__bdram_heap_buffer_size__;
	const size_t psram_heap_bytes = (size_t)__psram_heap_buffer_size__;
	const size_t psram_heap_ext_bytes = (size_t)__psram_heap_extend_size__;

	const uint32_t heap_free_bytes = rtos_mem_get_free_heap_size();
	const uint32_t heap_min_ever_free_bytes = rtos_mem_get_minimum_ever_free_heap_size();

	RTK_LOGI(TAG, "HW: SoC=%s chip_info=0x%02x RL=0x%lx cut=%c bd=0x%04x(%s)\n",
			 soc_name,
			 (unsigned int)chip_info,
			 (u32)SYSCFG_GetRLNum(),
			 (cut == SYSCFG_CUT_VERSION_A) ? 'A' : (cut == SYSCFG_CUT_VERSION_B) ? 'B' : '?',
			 (unsigned int)bd_num,
			 bdnum_to_str(bd_num));

	RTK_LOGI(TAG, "HW: SRAM(heap)=0x%lx PSRAM(MCM)=%lu bytes(%s/%s) PSRAM(heap)=0x%lx ext=0x%lx\n",
			 (u32)bdram_heap_bytes,
			 (u32)psram_bytes,
			 psram_vendor_to_str(mcm.dram_info.model),
			 psram_dqx_to_str(mcm.dram_info.dqx),
			 (u32)psram_heap_bytes,
			 (u32)psram_heap_ext_bytes);

	RTK_LOGI(TAG, "HW: Flash(MCM/internal)=%lu bytes(%s) Flash(SPIC/external)=%lu bytes\n",
			 (u32)mcm_flash_bytes,
			 flash_type_to_str(mcm.mem_type),
			 (u32)ext_flash_bytes);

	RTK_LOGI(TAG, "HW: Heap free=%lu min_ever_free=%lu\n",
			 (u32)heap_free_bytes,
			 (u32)heap_min_ever_free_bytes);
}

//default main
int main(void)
{
	RTK_LOGI(TAG, "AP MAIN \n");
	ameba_rtos_get_version();
	app_hw_info_print();

#if (!defined (CONFIG_WHC_INTF_IPC) && defined (CONFIG_WHC_DEV))
	app_fullmac_init();
	app_IWDG_init();
#else
	/*IPC table initialization*/
	ipc_table_init(IPCAP_DEV);
	InterruptRegister(IPC_INTHandler, IPC_CPU0_IRQ, (u32)IPCAP_DEV, INT_PRI5);
	InterruptEn(IPC_CPU0_IRQ, INT_PRI5);
#endif

#if defined(CONFIG_MBEDTLS_ENABLED)
	app_mbedtls_rom_init();
#endif

	app_filesystem_init();

#if defined(CONFIG_FTL_ENABLED) && CONFIG_FTL_ENABLED
	app_ftl_init();
#endif

	/* pre-processor of application example */
	app_pre_example();

#ifdef CONFIG_ETHERNET
	ethernet_mii_init();
#endif

#if defined(CONFIG_BT_COEXIST)
	/* init coex ipc */
	coex_ipc_entry();
#endif

#if defined(CONFIG_WIFI_FW_EN) && defined(CONFIG_FW_DRIVER_COEXIST)
	wififw_task_create();
#endif

#ifdef CONFIG_WLAN
	wifi_init();
#endif

	/* initialize BT iNIC */
#if defined(CONFIG_BT) && defined(CONFIG_BT_INIC)
	bt_inic_init();
#endif

	/* init console */
	shell_init_rom(0, NULL);
	shell_init_ram();

#if (!defined (CONFIG_WHC_INTF_IPC) && defined (CONFIG_WHC_DEV))
	/* Register Log Uart Callback function */
	InterruptRegister((IRQ_FUN) shell_uart_irq_rom, UART_LOG_IRQ, (u32)NULL, INT_PRI_LOWEST);
	InterruptEn(UART_LOG_IRQ, INT_PRI_LOWEST);
	LOGUART_INTCoreConfig(LOGUART_DEV, LOGUART_BIT_INTR_MASK_AP, ENABLE);
#endif

	app_pmu_init();

	/* Register CPU1_WDG_RST_IRQ Callback function */
	InterruptRegister((IRQ_FUN) CPU1_WDG_RST_Handler, CPU1_WDG_RST_IRQ, (u32)NULL, INT_PRI_LOWEST);
	InterruptEn(CPU1_WDG_RST_IRQ, INT_PRI_LOWEST);

	rtk_diag_init(RTK_DIAG_HEAP_SIZE, RTK_DIAG_SEND_BUFFER_SIZE);

	/* Execute application example */
	app_example();

	RTK_LOGI(TAG, "AP START SCHEDULER \n");

	/* Set delay function & critical function for hw ipc sema */
	IPC_patch_function(&rtos_critical_enter, &rtos_critical_exit);
	IPC_SEMDelayStub(rtos_time_delay_ms);

	/* Enable Schedule, Start Kernel */
	rtos_sched_start();
}

