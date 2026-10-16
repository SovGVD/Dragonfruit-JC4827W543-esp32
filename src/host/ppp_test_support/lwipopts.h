#pragma once
/* Native deterministic test of the PPP implementation from the firmware SDK.
 * This test runs synchronously; device thread/queue behavior is tested separately. */
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define MEM_ALIGNMENT 8
#define MEM_SIZE (128 * 1024)
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_DNS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define PPP_SUPPORT 1
#define PPPOS_SUPPORT 1
#define PPP_IPV4_SUPPORT 1
#define PPP_IPV6_SUPPORT 0
#define PPP_SERVER 1
#define MEMP_NUM_PPP_PCB 2
#define VJ_SUPPORT 0
#define PAP_SUPPORT 0
#define CHAP_SUPPORT 0
#define MSCHAP_SUPPORT 0
#define EAP_SUPPORT 0
#define CCP_SUPPORT 0
#define MPPE_SUPPORT 0
#define PPP_NOTIFY_PHASE 0
#define ESP_LWIP_DHCP_FINE_TIMERS_ONDEMAND 0
#define ESP_LWIP_IGMP_TIMERS_ONDEMAND 0
#define ESP_LWIP_DNS_TIMERS_ONDEMAND 0
#define ESP_LWIP_MLD6_TIMERS_ONDEMAND 0
#define DHCP_DOES_ARP_CHECK 1
