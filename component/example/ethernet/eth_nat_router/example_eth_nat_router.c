#include <platform_autoconf.h>

#include "example_eth_nat_router.h"

#include "os_wrapper.h"
#include "lwip_netconf.h"
#include "dhcp/dhcps.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"

static const char *const TAG = "ETH_NAT_ROUTER";

/* LAN side (Ethernet) */
#define LAN_IP0 192
#define LAN_IP1 168
#define LAN_IP2 50
#define LAN_IP3 1

#define LAN_MSK0 255
#define LAN_MSK1 255
#define LAN_MSK2 255
#define LAN_MSK3 0

/* DHCP pool handed to peer1 */
#define LAN_POOL_START0 192
#define LAN_POOL_START1 168
#define LAN_POOL_START2 50
#define LAN_POOL_START3 100

#define LAN_POOL_END0 192
#define LAN_POOL_END1 168
#define LAN_POOL_END2 50
#define LAN_POOL_END3 150

extern struct netif *pnetif_eth;
extern struct netif *pnetif_sta;

/* ethernet_mii.c globals */
extern int dhcp_ethernet_mii;
extern int ethernet_if_default;

static void eth_nat_router_thread(void *param)
{
	(void)param;

	/* In router mode, Ethernet is LAN. Keep default route on STA (WAN). */
	ethernet_if_default = 0;
	/* Avoid Ethernet DHCP client. We'll set a static LAN IP. */
	dhcp_ethernet_mii = 0;

	RTK_LOGI(TAG, "Waiting for Ethernet link...\n");
	while (pnetif_eth == NULL || !netif_is_link_up(pnetif_eth)) {
		rtos_time_delay_ms(200);
	}
	RTK_LOGI(TAG, "Ethernet link up\n");

	/* Bring up LAN IP on NETIF_ETH_INDEX. */
	u32 lan_ip = CONCAT_TO_UINT32(LAN_IP0, LAN_IP1, LAN_IP2, LAN_IP3);
	u32 lan_msk = CONCAT_TO_UINT32(LAN_MSK0, LAN_MSK1, LAN_MSK2, LAN_MSK3);
	u32 lan_gw = lan_ip; /* gateway = router itself */

	if (netif_dhcp_data(pnetif_eth) != NULL) {
		LwIP_DHCP_stop(NETIF_ETH_INDEX);
	} else {
		LwIP_ReleaseIP(NETIF_ETH_INDEX);
	}

	LwIP_netif_set_down(NETIF_ETH_INDEX);
	LwIP_netif_set_up(NETIF_ETH_INDEX);
	LwIP_SetIP(NETIF_ETH_INDEX, lan_ip, lan_msk, lan_gw);
	/* Ensure default route stays on STA (WAN). */
	netifapi_netif_set_default(pnetif_sta);

	/* Start DHCP server on Ethernet LAN. */
	struct ip_addr start_ip;
	struct ip_addr end_ip;
	IP4_ADDR(ip_2_ip4(&start_ip), LAN_POOL_START0, LAN_POOL_START1, LAN_POOL_START2, LAN_POOL_START3);
	IP4_ADDR(ip_2_ip4(&end_ip), LAN_POOL_END0, LAN_POOL_END1, LAN_POOL_END2, LAN_POOL_END3);
	dhcps_set_addr_pool(1, &start_ip, &end_ip);
	dhcps_deinit();
	dhcps_init(pnetif_eth);

	RTK_LOGI(TAG, "LAN ready: ethernet %d.%d.%d.%d/24, DHCP pool %d.%d.%d.%d-%d.%d.%d.%d\n",
			 LAN_IP0, LAN_IP1, LAN_IP2, LAN_IP3,
			 LAN_POOL_START0, LAN_POOL_START1, LAN_POOL_START2, LAN_POOL_START3,
			 LAN_POOL_END0, LAN_POOL_END1, LAN_POOL_END2, LAN_POOL_END3);

#if defined(CONFIG_IP_NAT) && (CONFIG_IP_NAT == 1)
	RTK_LOGI(TAG, "NAT enabled (CONFIG_IP_NAT=1). Connect STA (WAN) with AT+WLCONN.\n");
#else
	RTK_LOGW(TAG, "NAT not enabled. Enable it in menuconfig (CONFIG_IP_NAT_MENU) for router behavior.\n");
#endif

	rtos_task_delete(NULL);
}

void example_eth_nat_router(void)
{
	RTK_LOGI(TAG, "Example start: Ethernet LAN + STA WAN router\n");
	RTK_LOGI(TAG, "1) Connect peer1 to Ethernet (DHCP)\n");
	RTK_LOGI(TAG, "2) Connect STA to upstream AP via AT: AT+WLCONN=ssid,<SSID>,pw,<PASS>\n");
	RTK_LOGI(TAG, "3) On peer1: ping peer2 under upstream AP\n");

	if (rtos_task_create(NULL, "eth_nat_router", eth_nat_router_thread, NULL, 2048, 1) != RTK_SUCCESS) {
		RTK_LOGE(TAG, "Failed to create thread\n");
	}
}
