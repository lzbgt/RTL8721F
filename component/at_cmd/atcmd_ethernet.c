#include "platform_autoconf.h"
#include "kv.h"
#include "atcmd_service.h"
#include "wifi_api_types.h"
#include "ethernet_mii.h"
#ifdef CONFIG_LWIP_LAYER
#include "lwip/netif.h"
#include "lwip/inet.h"
#include "lwip_netconf.h"
#include "dhcp/dhcps.h"
#endif

#if defined(CONFIG_AMEBAGREEN2)
#include "ameba_soc.h"
#include "ameba_ethernet.h"
#include "ameba_intfcfg.h"
#include "ameba_gpio.h"
#endif

extern struct netif *pnetif_eth;
extern volatile u32 g_rmii_tx_call;
extern volatile u32 g_rmii_tx_submit;
extern volatile u32 g_rmii_tx_getbuf_null;
extern ETH_InitTypeDef eth_initstruct;
extern u8 ETHERNET_Pin_Grp;
extern void ethernet_pin_config(void);

static void at_ethip_help(void)
{
	RTK_LOGI(NOTAG, "\r\n");
	RTK_LOGI(NOTAG, "AT+ETHIP=<store_flag>,<ip>[,<gateway>,<netmask>]\r\n");
	RTK_LOGI(NOTAG, "\t<store_flag>:\t0: Apply Temporary Configuration, 1: Save Configuration to Flash, 2: Erase Configuration from Flash\r\n");
	RTK_LOGI(NOTAG, "\tThe <gateway> and <netmask> should be absent or present together\r\n");
}

void at_ethip(void *arg)
{
	char *argv[MAX_ARGC] = {0};
	int argc = 0;
	int error_no = 0;
	int store_flag = 0;
	unsigned int ip = 0;
	unsigned int netmask = 0xFFFFFF00;
	unsigned int gw = 0;
	struct netif *eth_netif = NULL;
	unsigned int ip_tmp = 0;
	unsigned int gw_tmp = 0;
	unsigned int netmask_tmp = 0;

	if (arg == NULL) {
		RTK_LOGW(NOTAG, "[+ETHIP] The parameters can not be ignored\r\n");
		error_no = 1;
		goto end;
	}

	argc = parse_param(arg, argv);
	/* argv[0] is reserved; argv[1] is the 1st parameter. */
	if (argc < 2 || argc > 5) {
		RTK_LOGW(NOTAG, "[+ETHIP] The parameters format ERROR\r\n");
		error_no = 1;
		goto end;
	}

	if (argv[1] == NULL || argv[1][0] == '\0') {
		RTK_LOGW(NOTAG, "[+ETHIP] The parameters format ERROR\r\n");
		error_no = 1;
		goto end;
	}

	store_flag = atoi(argv[1]);
	if (store_flag < 0 || store_flag > 2) {
		RTK_LOGW(NOTAG, "[+ETHIP] The parameters format ERROR\r\n");
		error_no = 1;
		goto end;
	}

	if (store_flag == 2) {
		/* Erase persisted config (no need to apply runtime IP). */
		rt_kv_delete("eth_ip");
		rt_kv_delete("eth_gw");
		rt_kv_delete("eth_netmask");
		goto end;
	}

	/* store_flag 0/1: require at least <ip>. Optional <gateway>,<netmask> must appear together. */
	if (argc < 3 || argv[2] == NULL || argv[2][0] == '\0') {
		RTK_LOGW(NOTAG, "[+ETHIP] The parameters format ERROR\r\n");
		error_no = 1;
		goto end;
	}

	if (argc == 4) {
		RTK_LOGW(NOTAG, "[+ETHIP] The <gateway> and <netmask> should be absent or present together\r\n");
		error_no = 1;
		goto end;
	}

	ip_tmp = inet_addr(argv[2]);
	if (ip_tmp == (unsigned int)INADDR_NONE) {
		RTK_LOGW(NOTAG, "[+ETHIP] Invalid IP\r\n");
		error_no = 1;
		goto end;
	}
	ip = PP_HTONL(ip_tmp);

	/* Default /24 gateway: x.x.x.1 */
	gw = (ip & 0xFFFFFF00) | 0x01;

	if (argc == 5) {
		gw_tmp = inet_addr(argv[3]);
		netmask_tmp = inet_addr(argv[4]);
		if (gw_tmp == (unsigned int)INADDR_NONE || netmask_tmp == (unsigned int)INADDR_NONE) {
			RTK_LOGW(NOTAG, "[+ETHIP] Invalid gateway/netmask\r\n");
			error_no = 1;
			goto end;
		}
		gw = PP_HTONL(gw_tmp);
		netmask = PP_HTONL(netmask_tmp);
	}

	/* Persist: always save a complete tuple to KV to avoid stale gw/netmask. */
	if (store_flag == 1) {
		rt_kv_set("eth_ip", &ip, 4);
		rt_kv_set("eth_gw", &gw, 4);
		rt_kv_set("eth_netmask", &netmask, 4);
	}

	/* Apply runtime IP only when Ethernet netif exists. */
	eth_netif = LwIP_idx_get_netif(NETIF_ETH_INDEX);
	if (eth_netif == NULL) {
		if (store_flag == 1) {
			RTK_LOGW(NOTAG, "[+ETHIP] Ethernet not initialized; saved to KV only\r\n");
			goto end;
		}
		RTK_LOGW(NOTAG, "[+ETHIP] Ethernet not initialized\r\n");
		error_no = 1;
		goto end;
	}

	if (netif_dhcp_data(eth_netif) != NULL) {
		LwIP_DHCP_stop(NETIF_ETH_INDEX);
	} else {
		LwIP_ReleaseIP(NETIF_ETH_INDEX);
	}

	LwIP_netif_set_down(NETIF_ETH_INDEX);
	LwIP_netif_set_up(NETIF_ETH_INDEX);
	LwIP_SetIP(NETIF_ETH_INDEX, ip, netmask, gw);

end:
	if (error_no == 0) {
		at_printf(ATCMD_OK_END_STR);
	} else {
		at_ethip_help();
		at_printf(ATCMD_ERROR_END_STR, error_no);
	}
}

#if defined(CONFIG_AMEBAGREEN2) && (defined(CONFIG_ETHERNET) || defined(CONFIG_LWIP_USB_ETHERNET)) && defined(CONFIG_LWIP_LAYER)
static void _pin_to_str(uint8_t pin, char *out, size_t out_len)
{
	if (out == NULL || out_len < 6) {
		return;
	}
	const char port = (char)('A' + (pin >> 5));
	const unsigned int num = (unsigned int)(pin & 0x1F);
	(void)snprintf(out, out_len, "P%c%u", port, num);
}

static void _print_eth_pins(void)
{
	static const char *const names[11] = {
		"RXERR", "CRS_DV", "TXEN", "TXD1", "TXD0", "REF_CLK", "RXD1", "RXD0", "MDC", "MDIO", "EXTCLK",
	};
	at_printf("ETHERNET_Pin_Grp=%u\r\n", (unsigned int)ETHERNET_Pin_Grp);
	if (ETHERNET_Pin_Grp >= 4) {
		at_printf("ETHERNET_PAD: invalid group\r\n");
		return;
	}
	for (unsigned int i = 0; i < 11; i++) {
		char pin_str[8] = {0};
		const uint8_t pin = ETHERNET_PAD[ETHERNET_Pin_Grp][i];
		_pin_to_str(pin, pin_str, sizeof(pin_str));
		/* Avoid printf field-width formatting: some console builds don't support it. */
		at_printf("PAD[%u] %s=%s (0x%02x)\r\n", i, names[i], pin_str, (unsigned int)pin);
	}
}

static void _print_phy_scan(void)
{
	uint16_t reg0 = 0, reg1 = 0, reg2 = 0, reg3 = 0;
	int ret0, ret1, ret2, ret3;
	unsigned int printed = 0;
	for (uint8_t phy = 0; phy <= 31; phy++) {
		ret2 = Ethernet_ReadPhyReg(phy, 2, &reg2);
		ret3 = Ethernet_ReadPhyReg(phy, 3, &reg3);
		ret0 = Ethernet_ReadPhyReg(phy, 0, &reg0);
		ret1 = Ethernet_ReadPhyReg(phy, 1, &reg1);
		if (ret2 == RTK_SUCCESS && ret3 == RTK_SUCCESS) {
			/* Heuristic: a real PHY typically isn't all-0 or all-1 for both ID regs. */
			if ((reg2 == 0x0000 && reg3 == 0x0000) || (reg2 == 0xFFFF && reg3 == 0xFFFF)) {
				continue;
			}
			at_printf("PHY[%u]: ID1=0x%04x ID2=0x%04x BMCR(0)=0x%04x BMSR(1)=0x%04x\r\n",
					  (unsigned int)phy, reg2, reg3,
					  (ret0 == RTK_SUCCESS) ? reg0 : 0xffff,
					  (ret1 == RTK_SUCCESS) ? reg1 : 0xffff);
			printed++;
		} else {
			at_printf("PHY[%u]: MDIO read failed (ret2=%d ret3=%d)\r\n", (unsigned int)phy, ret2, ret3);
		}
	}
	if (printed == 0) {
		at_printf("PHY scan: no valid PHY IDs found (all 0x0000/0xffff?)\r\n");
	}
}

static void at_ethup(void *arg)
{
	(void)arg;
	struct netif *eth_netif = LwIP_idx_get_netif(NETIF_ETH_INDEX);
	at_printf("[+ETHUP]\r\n");
	if (eth_netif == NULL) {
		at_printf("netif: NULL (Ethernet not initialized)\r\n");
		at_printf(ATCMD_OK_END_STR);
		return;
	}
	at_printf("before: up=%d link_up=%d flags=0x%08x\r\n",
			  netif_is_up(eth_netif) ? 1 : 0,
			  netif_is_link_up(eth_netif) ? 1 : 0,
			  (unsigned int)eth_netif->flags);
	netifapi_netif_set_up(eth_netif);
	at_printf("after : up=%d link_up=%d flags=0x%08x\r\n",
			  netif_is_up(eth_netif) ? 1 : 0,
			  netif_is_link_up(eth_netif) ? 1 : 0,
			  (unsigned int)eth_netif->flags);
	at_printf(ATCMD_OK_END_STR);
}

static void at_ethdown(void *arg)
{
	(void)arg;
	struct netif *eth_netif = LwIP_idx_get_netif(NETIF_ETH_INDEX);
	at_printf("[+ETHDOWN]\r\n");
	if (eth_netif == NULL) {
		at_printf("netif: NULL (Ethernet not initialized)\r\n");
		at_printf(ATCMD_OK_END_STR);
		return;
	}
	at_printf("before: up=%d link_up=%d flags=0x%08x\r\n",
			  netif_is_up(eth_netif) ? 1 : 0,
			  netif_is_link_up(eth_netif) ? 1 : 0,
			  (unsigned int)eth_netif->flags);
	netifapi_netif_set_down(eth_netif);
	at_printf("after : up=%d link_up=%d flags=0x%08x\r\n",
			  netif_is_up(eth_netif) ? 1 : 0,
			  netif_is_link_up(eth_netif) ? 1 : 0,
			  (unsigned int)eth_netif->flags);
	at_printf(ATCMD_OK_END_STR);
}

#if defined(CONFIG_AMEBAGREEN2)
/* Optional: pulse PA3 as a PHY reset candidate (board-dependent). */
static void at_ethrst(void *arg)
{
	(void)arg;
	at_printf("[+ETHRST]\r\n");

	GPIO_InitTypeDef GPIO_InitStruct;
	GPIO_InitStruct.GPIO_Pin = _PA_3;
	GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
	GPIO_Init(&GPIO_InitStruct);

	GPIO_WriteBit(_PA_3, 0);
	rtos_time_delay_ms(50);
	GPIO_WriteBit(_PA_3, 1);
	rtos_time_delay_ms(200);

	/* Return PA3 to input like ethernet_pin_config(). */
	GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN;
	GPIO_Init(&GPIO_InitStruct);

	at_printf("GPIO: PA3 level=%u\r\n", (unsigned int)GPIO_ReadDataBit(_PA_3));
	at_printf(ATCMD_OK_END_STR);
}
#endif

static void at_ethforce_help(void)
{
	RTK_LOGI(NOTAG, "\r\n");
	RTK_LOGI(NOTAG, "AT+ETHFORCE=<enable>,<speed>,<fulldup>\r\n");
	RTK_LOGI(NOTAG, "\t<enable>:\t0 disable (auto), 1 force link/speed\r\n");
	RTK_LOGI(NOTAG, "\t<speed>:\t10|100|1000\r\n");
	RTK_LOGI(NOTAG, "\t<fulldup>:\t0 half, 1 full\r\n");
}

static void at_ethforce(void *arg)
{
	char *argv[MAX_ARGC] = {0};
	int argc = 0;
	int error_no = 0;
	int enable = 0;
	int speed = 0;
	int fulldup = 1;
	u32 force_spd = 3; /* 3: not force */

	at_printf("[+ETHFORCE]\r\n");

	if (arg == NULL) {
		error_no = 1;
		goto end;
	}
	argc = parse_param(arg, argv);
	if (argc != 4) {
		error_no = 1;
		goto end;
	}

	enable = atoi(argv[1]);
	speed = atoi(argv[2]);
	fulldup = atoi(argv[3]);
	if (!(enable == 0 || enable == 1)) {
		error_no = 1;
		goto end;
	}
	if (!(fulldup == 0 || fulldup == 1)) {
		error_no = 1;
		goto end;
	}

	switch (speed) {
	case 10:
		force_spd = 1;
		break;
	case 100:
		force_spd = 0;
		break;
	case 1000:
		force_spd = 2;
		break;
	default:
		error_no = 1;
		goto end;
	}

	ETHERNET_TypeDef *rmii = (ETHERNET_TypeDef *)RMII_REG_BASE;
	if (TrustZone_IsSecure()) {
		rmii = (ETHERNET_TypeDef *)RMII_REG_BASE_S;
	}

	u32 msr = rmii->ETH_MSR;
	if (enable == 0) {
		/* Return to auto. */
		msr &= ~(BIT_FORCELINK | BIT_FORCEDFULLDUP | MASK_FORCE_SPD);
		msr |= FORCE_SPD(3);
	} else {
		msr |= BIT_FORCELINK;
		if (fulldup) {
			msr |= BIT_FORCEDFULLDUP;
		} else {
			msr &= ~BIT_FORCEDFULLDUP;
		}
		msr &= ~MASK_FORCE_SPD;
		msr |= FORCE_SPD(force_spd);
	}
	rmii->ETH_MSR = msr;
	rtos_time_delay_ms(2);

	at_printf("ETH_MSR=0x%08x\r\n", (unsigned int)rmii->ETH_MSR);

end:
	if (error_no == 0) {
		at_printf(ATCMD_OK_END_STR);
	} else {
		at_ethforce_help();
		at_printf(ATCMD_ERROR_END_STR, 1);
	}
}

void at_ethstat(void *arg)
{
	(void)arg;
	struct netif *eth_netif = LwIP_idx_get_netif(NETIF_ETH_INDEX);
	at_printf("[+ETHSTAT]\r\n");

	if (eth_netif == NULL) {
		at_printf("netif: NULL (Ethernet not initialized)\r\n");
		at_printf(ATCMD_OK_END_STR);
		return;
	}

	at_printf("netif: name=%c%c num=%d\r\n", eth_netif->name[0], eth_netif->name[1], (int)eth_netif->num);
	at_printf("netif: up=%d link_up=%d flags=0x%08x\r\n",
			  netif_is_up(eth_netif) ? 1 : 0,
			  netif_is_link_up(eth_netif) ? 1 : 0,
			  (unsigned int)eth_netif->flags);
#if defined(CONFIG_AMEBAGREEN2)
	/* ethernet_pin_config() sets PA3 to input ("disable phy reset pin"). If your board actually uses PA3 as PHY_RST_N,
	 * this helps spot a stuck-low reset condition. */
	at_printf("GPIO: PA3 level=%u\r\n", (unsigned int)GPIO_ReadDataBit(_PA_3));
#endif

	ETHERNET_TypeDef *rmii = (ETHERNET_TypeDef *)RMII_REG_BASE;
	if (TrustZone_IsSecure()) {
		rmii = (ETHERNET_TypeDef *)RMII_REG_BASE_S;
	}
	uint32_t miiar = rmii->ETH_MIIAR;
	at_printf("MAC: ETH_MIIAR=0x%08x (FLAG=%u MDIO_BUSY=%u DISABLE_AUTO_POLLING=%u PHYADDR=%u REG=%u DATA=0x%04x)\r\n",
			  (unsigned int)miiar,
			  (unsigned int)((miiar >> 31) & 0x1U),
			  (unsigned int)((miiar >> 25) & 0x1U),
			  (unsigned int)((miiar >> 22) & 0x1U),
			  (unsigned int)((miiar >> 26) & 0x1FU),
			  (unsigned int)((miiar >> 16) & 0x1FU),
			  (unsigned int)(miiar & 0xFFFFU));
	uint32_t msr = rmii->ETH_MSR;
	unsigned int link_ok = (((msr >> 26) & 0x1U) == 0) ? 1U : 0U;
	unsigned int speed_code = (unsigned int)((msr >> 27) & 0x3U);
	const char *speed_str = (speed_code == 0) ? "100M" : (speed_code == 1) ? "10M" : (speed_code == 2) ? "1000M" : "invalid";
	at_printf("MAC: ETH_MSR=0x%08x (LINK_OK=%u SPEED=%s FULLDUP=%u NWAY_DONE=%u)\r\n",
			  (unsigned int)msr,
			  link_ok,
			  speed_str,
			  (unsigned int)((msr >> 22) & 0x1U),
			  (unsigned int)((msr >> 21) & 0x1U));
	at_printf("MAC: msr_sel_rgmii=%u msr_sel_mii=%u gmac_phy_mode=%u forcelink=%u forcedfull=%u force_spd=%u\r\n",
			  (unsigned int)((msr >> 23) & 0x1U),
			  (unsigned int)((msr >> 20) & 0x1U),
			  (unsigned int)((msr >> 13) & 0x1U),
			  (unsigned int)((msr >> 18) & 0x1U),
			  (unsigned int)((msr >> 19) & 0x1U),
			  (unsigned int)((msr >> 16) & 0x3U));

	at_printf("MAC: TXOK=%u RXOK=%u TXERR=%u RXERR=%u MISSPKT=%u\r\n",
			  (unsigned int)rmii->ETH_TXOKCNT,
			  (unsigned int)rmii->ETH_RXOKCNT,
			  (unsigned int)rmii->ETH_TXERR,
			  (unsigned int)rmii->ETH_RXERR,
			  (unsigned int)rmii->ETH_MISSPKT);

	at_printf("MAC: RXOKPHY=%u RXOKBRD=%u RXOKMU1=%u\r\n",
			  (unsigned int)rmii->ETH_RXOKPHY,
			  (unsigned int)rmii->ETH_RXOKBRD,
			  (unsigned int)rmii->ETH_RXOKMU1);

	at_printf("SW: tx_call=%u tx_submit=%u tx_getbuf_null=%u\r\n",
			  (unsigned int)g_rmii_tx_call,
			  (unsigned int)g_rmii_tx_submit,
			  (unsigned int)g_rmii_tx_getbuf_null);
	at_printf("SW: tx_desc_cur=%u tx_desc_num=%u\r\n",
			  (unsigned int)eth_initstruct.ETH_TxDescCurrentNum,
			  (unsigned int)eth_initstruct.ETH_TxDescNum);
	for (unsigned int i = 0; i < (unsigned int)eth_initstruct.ETH_TxDescNum; i++) {
		at_printf("SW: TXDESC[%u].dw1=0x%08x\r\n", i, (unsigned int)eth_initstruct.ETH_TxDesc[i].dw1);
	}

	at_printf("MAC: ETH_CR=0x%08x ETH_TCR=0x%08x ETH_RCR=0x%08x\r\n",
			  (unsigned int)rmii->ETH_CR,
			  (unsigned int)rmii->ETH_TCR,
			  (unsigned int)rmii->ETH_RCR);
	at_printf("MAC: ETH_IO_CMD1=0x%08x ETH_ETHER_IO_CMD=0x%08x (TE=%u RE=%u TXFN1ST=%u)\r\n",
			  (unsigned int)rmii->ETH_IO_CMD1,
			  (unsigned int)rmii->ETH_ETHER_IO_CMD,
			  (unsigned int)((rmii->ETH_ETHER_IO_CMD >> 4) & 0x1U),
			  (unsigned int)((rmii->ETH_ETHER_IO_CMD >> 5) & 0x1U),
			  (unsigned int)((rmii->ETH_ETHER_IO_CMD >> 0) & 0x1U));
	at_printf("MAC: ETH_ISR_AND_IMR=0x%08x ETH_TXFDP1=0x%08x ETH_RXFDP1=0x%08x\r\n",
			  (unsigned int)rmii->ETH_ISR_AND_IMR,
			  (unsigned int)rmii->ETH_TXFDP1,
			  (unsigned int)rmii->ETH_RX_FDP1);

	at_printf("Build cfg: ");
#ifdef CONFIG_PHY_INT_XTAL
	at_printf("PHY_INT_XTAL ");
#endif
#ifdef CONFIG_MAC_OUTPUT_50M
	at_printf("MAC_OUTPUT_50M ");
#endif
#ifdef CONFIG_MAC_OUTPUT_25M
	at_printf("MAC_OUTPUT_25M ");
#endif
	at_printf("\r\n");

	at_printf("Pinmux:\r\n");
	_print_eth_pins();

	at_printf("PHY scan (MDIO addr 0..31):\r\n");
	/* Avoid racing the MAC's HW auto-polling while doing manual MDIO transactions. */
	Ethernet_AutoPolling(DISABLE);
	rtos_time_delay_ms(2);
	_print_phy_scan();
	Ethernet_AutoPolling(ENABLE);

	at_printf(ATCMD_OK_END_STR);
}

static void at_ethgrp_help(void)
{
	RTK_LOGI(NOTAG, "\r\n");
	RTK_LOGI(NOTAG, "AT+ETHGRP=<grp>\r\n");
	RTK_LOGI(NOTAG, "\t<grp>:\t0..3 (select RMII pin group)\r\n");
	RTK_LOGI(NOTAG, "\tNote:\tgrp=3 uses PA25/PA26 for MDC/MDIO and will conflict with the default AT UART pins on AmebaGreen2.\r\n");
}

static void at_ethgrp(void *arg)
{
	char *argv[MAX_ARGC] = {0};
	int argc = 0;
	int error_no = 0;
	int grp = -1;

	at_printf("[+ETHGRP]\r\n");
	if (arg == NULL) {
		error_no = 1;
		goto end;
	}

	argc = parse_param(arg, argv);
	if (argc != 2 || argv[1] == NULL || argv[1][0] == '\0') {
		error_no = 1;
		goto end;
	}
	grp = atoi(argv[1]);
	if (grp < 0 || grp > 3) {
		error_no = 1;
		goto end;
	}
	/* Group3 uses PA25/PA26 for MDC/MDIO, which are also the default AT UART pins (UART_RX/UART_TX) on AmebaGreen2.
	 * Switching to it can break the console or trigger WDT resets. */
	if (grp == 3) {
		at_printf("ERR: grp=3 conflicts with AT UART pins (PA25/PA26). Use 0/1/2 or move UART pins.\r\n");
		error_no = 1;
		goto end;
	}

	/* Switch global group and re-apply pinmux. This does not reset the MAC or lwIP. */
	ETHERNET_Pin_Grp = (u8)grp;
	ethernet_pin_config();
	rtos_time_delay_ms(2);

	at_printf("ETHERNET_Pin_Grp=%u\r\n", (unsigned int)ETHERNET_Pin_Grp);
	_print_eth_pins();

end:
	if (error_no == 0) {
		at_printf(ATCMD_OK_END_STR);
	} else {
		at_ethgrp_help();
		at_printf(ATCMD_ERROR_END_STR, 1);
	}
}

/* Quick helper to try alternative ETHERNET_Pin_Grp choices without reflashing.
 * This only remuxes MDC/MDIO, then scans for a valid PHY ID. */
void at_ethscan(void *arg)
{
	(void)arg;
	at_printf("[+ETHSCAN]\r\n");

	const unsigned int saved_grp = (unsigned int)ETHERNET_Pin_Grp;

	ETHERNET_TypeDef *rmii = (ETHERNET_TypeDef *)RMII_REG_BASE;
	if (TrustZone_IsSecure()) {
		rmii = (ETHERNET_TypeDef *)RMII_REG_BASE_S;
	}

	Ethernet_AutoPolling(DISABLE);
	rtos_time_delay_ms(2);

	for (unsigned int grp = 0; grp < 4; grp++) {
		at_printf("Try ETHERNET_Pin_Grp=%u\r\n", grp);
		Pinmux_Config(ETHERNET_PAD[grp][8], PINMUX_FUNCTION_RMII_MDC);
		Pinmux_Config(ETHERNET_PAD[grp][9], PINMUX_FUNCTION_RMII_MDIO);
		PAD_PullCtrl(ETHERNET_PAD[grp][9], GPIO_PuPd_UP);
		rtos_time_delay_ms(1);

		at_printf("MAC: ETH_MIIAR=0x%08x\r\n", (unsigned int)rmii->ETH_MIIAR);
		_print_phy_scan();
	}

	/* Restore configured group pins. */
	if (saved_grp < 4) {
		Pinmux_Config(ETHERNET_PAD[saved_grp][8], PINMUX_FUNCTION_RMII_MDC);
		Pinmux_Config(ETHERNET_PAD[saved_grp][9], PINMUX_FUNCTION_RMII_MDIO);
	}

	Ethernet_AutoPolling(ENABLE);
	at_printf(ATCMD_OK_END_STR);
}
#endif

log_item_t at_ethernet_items[ ] = {
	{"+ETHIP", at_ethip, {NULL, NULL}},
#if defined(CONFIG_AMEBAGREEN2) && (defined(CONFIG_ETHERNET) || defined(CONFIG_LWIP_USB_ETHERNET)) && defined(CONFIG_LWIP_LAYER)
	{"+ETHSTAT", at_ethstat, {NULL, NULL}},
	{"+ETHSTATE", at_ethstat, {NULL, NULL}},
	{"+ETHSCAN", at_ethscan, {NULL, NULL}},
	{"+ETHGRP", at_ethgrp, {NULL, NULL}},
	{"+ETHUP", at_ethup, {NULL, NULL}},
	{"+ETHDOWN", at_ethdown, {NULL, NULL}},
	{"+ETHFORCE", at_ethforce, {NULL, NULL}},
#if defined(CONFIG_AMEBAGREEN2)
	{"+ETHRST", at_ethrst, {NULL, NULL}},
#endif
#endif
};

void print_ethernet_at(void)
{
	int index;
	int cmd_len = 0;

	cmd_len = sizeof(at_ethernet_items) / sizeof(at_ethernet_items[0]);
	for (index = 0; index < cmd_len; index++) {
		at_printf("AT%s\r\n", at_ethernet_items[index].log_cmd);
	}
}

void at_ethernet_init(void)
{
	atcmd_service_add_table(at_ethernet_items, sizeof(at_ethernet_items) / sizeof(at_ethernet_items[0]));
}
