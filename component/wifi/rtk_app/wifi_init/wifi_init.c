/**
  ******************************************************************************
  * @file    wifi_init.c
  * @author
  * @version
  * @date
  * @brief
  ******************************************************************************
  * @attention
  *
  * This module is a confidential and proprietary property of RealTek and
  * possession or use of this module requires written permission of RealTek.
  *
  * Copyright(c) 2024, Realtek Semiconductor Corporation. All rights reserved.
  ******************************************************************************
  */
#include "ameba_soc.h"
#ifdef CONFIG_WLAN
#if defined(CONFIG_WHC_HOST) && !defined(CONFIG_WHC_INTF_IPC)
#include "whc_host_api.h"
#elif defined(CONFIG_WHC_INTF_IPC)
#include "whc_ipc.h"
#endif
#include "rtw_skbuff.h"
#if defined(CONFIG_LWIP_LAYER) && CONFIG_LWIP_LAYER
#include "lwip_netconf.h"
#endif
#include "wifi_intf_drv_to_upper.h"

#if defined(CONFIG_WHC_CMD_PATH) && !defined(CONFIG_WHC_HOST)
#include "whc_dev_api.h"
#endif

//todo clarify
#if defined(CONFIG_WHC_INTF_SDIO)
#if defined(CONFIG_WHC_HOST)
#include "whc_sdio_host.h"
#endif
#include "whc_sdio_dev.h"
#elif defined(CONFIG_WHC_INTF_SPI)
#if defined(CONFIG_WHC_HOST)
#include "whc_spi_host.h"
#else
#include "whc_spi_dev.h"
#endif
#elif defined(CONFIG_WHC_INTF_USB)
#include "whc_usb_dev.h"
#elif defined(CONFIG_WHC_INTF_UART)
#if defined(CONFIG_WHC_HOST)
#include "whc_uart_host.h"
#else
#include "whc_uart_dev.h"
#endif
#endif

#define WIFI_STACK_SIZE_INIT ((512 + 768) * 4)

/* kv.h isn't in the default include path for this component in some SDK builds,
 * but KV APIs are still linked in (used by AT commands, fast reconnect, etc.). */
extern int32_t rt_kv_get(const char *key, void *buffer, int32_t len);

/* Persistent Wi-Fi boot policy (KV):
 * - key: "wifi_on_boot" (u8)
 * - 0: do not call wifi_on() during boot (keeps Wi-Fi off unless user enables it later)
 * - 1: call wifi_on() during boot (default behavior)
 *
 * If the key doesn't exist, fall back to compile-time default CONFIG_WIFI_ON_BOOT (default y).
 */
#if defined(CONFIG_WHC_HOST) || defined(CONFIG_WHC_NONE)
static int wifi_auto_on_boot_enabled(void)
{
	uint8_t v = 0;
	if (rt_kv_get("wifi_on_boot", &v, 1) == 1) {
		return (v != 0);
	}
#ifdef CONFIG_WIFI_ON_BOOT
	return 1;
#else
	return 0;
#endif
}
#endif

/**********************************************************************************************
 *                                WHC host performs wifi init
 *********************************************************************************************/
#if defined(CONFIG_WHC_HOST)
void wifi_init_thread(void *param)
{
	UNUSED(param);
#ifdef CONFIG_LWIP_LAYER
	LwIP_Init();
#endif

	whc_host_init();

#if defined(CONFIG_WHC_WIFI_API_PATH)
	if (wifi_auto_on_boot_enabled()) {
		wifi_on(RTW_MODE_STA);
		RTK_LOGS(TAG_WLAN_DRV, RTK_LOG_INFO, "Available heap after wifi init %d\n", rtos_mem_get_free_heap_size() + WIFI_STACK_SIZE_INIT);
	} else {
		RTK_LOGS(TAG_WLAN_DRV, RTK_LOG_WARN, "Wi-Fi boot policy: off (skip wifi_on); enable later via AT command\n");
	}
#endif

	rtos_task_delete(NULL);
}

/**********************************************************************************************
 *                                 WHC Device performs wifi init
 *********************************************************************************************/
#elif defined(CONFIG_WHC_DEV)
void wifi_init_thread(void *param)
{
	UNUSED(param);

#if defined(CONFIG_LWIP_LAYER) && defined(CONFIG_WHC_DUAL_TCPIP)
	LwIP_Init();
#endif

#ifdef CONFIG_WHC_CMD_PATH
	whc_dev_init_cmd_path_task();
#endif
	whc_dev_init();
	rtos_task_delete(NULL);
}

/**********************************************************************************************
 *                               Single core mode performs wifi init
 *********************************************************************************************/
#elif defined(CONFIG_WHC_NONE)/*Single core processor do wifi init*/
void wifi_init_thread(void *param)
{
	UNUSED(param);
#if defined(CONFIG_ARM_CORE_CM4) && defined(configENABLE_TRUSTZONE) && (configENABLE_TRUSTZONE == 1)
	rtos_create_secure_context(configMINIMAL_SECURE_STACK_SIZE);
#endif

#ifdef CONFIG_LWIP_LAYER
	LwIP_Init();
#endif

	if (wifi_auto_on_boot_enabled()) {
		wifi_on(RTW_MODE_STA);
		RTK_LOGS(TAG_WLAN_DRV, RTK_LOG_INFO, "Available heap after wifi init %d\n", rtos_mem_get_free_heap_size() + WIFI_STACK_SIZE_INIT);
	} else {
		RTK_LOGS(TAG_WLAN_DRV, RTK_LOG_WARN, "Wi-Fi boot policy: off (skip wifi_on); enable later via AT command\n");
	}

	/* Kill init thread after all init tasks done */
	rtos_task_delete(NULL);
}

#endif

void wifi_init(void)
{
	wifi_set_rom2flash();
	if (rtos_task_create(NULL, ((const char *)"wifi_init_thread"), wifi_init_thread, NULL, WIFI_STACK_SIZE_INIT, 5) != RTK_SUCCESS) {
		RTK_LOGS(TAG_WLAN_DRV, RTK_LOG_ERROR, "wifi_init failed\n");
	}
}
#endif
