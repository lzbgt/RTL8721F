# Example Description

Ethernet LAN + Wi-Fi STA WAN “router” setup using lwIP NAT.

This example:

- Sets Ethernet (`NETIF_ETH_INDEX`) to a static LAN IP `192.168.50.1/24`
- Starts a DHCP server on Ethernet (pool `192.168.50.100` - `192.168.50.150`)
- Keeps the default route on STA so traffic from LAN goes out via Wi-Fi STA

# HW Configuration

- Ethernet port connected to a switch/PC (peer1)
- Wi-Fi STA connects to an upstream AP (peer2 is on the upstream network)

# SW Configuration

Build the example:

- `py -3.11 build.py -a eth_nat_router`

Flash (GUI or CLI) using the correct AmebaGreen2 NOR layout.

Runtime:

1. Plug peer1 into Ethernet and enable DHCP.
2. In UART console, connect STA to upstream AP:
   - `AT+WLCONN=ssid,<SSID>,pw,<PASSWORD>`
3. On peer1, ping peer2 (peer2 must allow ICMP echo).

# Supported IC

RTL8721F (AmebaGreen2)

