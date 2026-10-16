#include "portal_protocol.h"
#include "lwip/init.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "netif/ppp/pppos.h"
#include "lwip/udp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ppp_pcb *pcb;
    struct netif netif;
    unsigned char bytes[32768];
    size_t length;
    bool up;
} endpoint_t;
static endpoint_t gateway, client;

static bool udp_rx_called = false;
static bool cl_rx_called = false;
static void test_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                          const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    udp_rx_called = true;
    struct pbuf *resp = pbuf_alloc(PBUF_TRANSPORT, 44, PBUF_RAM);
    assert(resp);
    printf("dest: %s\n", ip4addr_ntoa(ip_2_ip4(addr)));
    for (struct netif *n = netif_list; n != NULL; n = n->next) {
        char s_ip[16], s_nm[16], s_gw[16];
        ip4addr_ntoa_r(netif_ip4_addr(n), s_ip, sizeof(s_ip));
        ip4addr_ntoa_r(netif_ip4_netmask(n), s_nm, sizeof(s_nm));
        ip4addr_ntoa_r(netif_ip4_gw(n), s_gw, sizeof(s_gw));
        printf("netif %c%c%d: up=%d link=%d bc=%d ip=%s nm=%s gw=%s\n",
               n->name[0], n->name[1], n->num,
               netif_is_up(n), netif_is_link_up(n), (n->flags & NETIF_FLAG_BROADCAST) != 0,
               s_ip, s_nm, s_gw);
    }
    fflush(stdout);
    err_t err = udp_sendto(pcb, resp, addr, port);
    printf("udp_sendto error: %d, addr: %s, port: %d\n", (int)err, ip4addr_ntoa(ip_2_ip4(addr)), port);
    fflush(stdout);
    assert(err == ERR_OK);
    pbuf_free(resp);
    pbuf_free(p);
}

static void test_cl_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)addr; (void)port;
    cl_rx_called = true;
    pbuf_free(p);
}
static u32_t clock_ms;
u32_t sys_now(void) { return clock_ms; }
u32_t sys_jiffies(void) { return clock_ms; }

static u32_t output(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    (void)pcb;
    endpoint_t *ep = ctx;
    assert(ep->length + len <= sizeof(ep->bytes));
    memcpy(ep->bytes + ep->length, data, len);
    ep->length += len;
    return len;
}

static void status(ppp_pcb *pcb, int error, void *ctx)
{
    (void)pcb;
    ((endpoint_t *)ctx)->up = error == PPPERR_NONE;
}

static void transfer(endpoint_t *from, endpoint_t *to)
{
    /* Fragment frames across calls, as the emulated UART transport does. */
    for (size_t offset = 0; offset < from->length;) {
        size_t n = from->length - offset;
        if (n > 37) n = 37;
        pppos_input(to->pcb, from->bytes + offset, (int)n);
        offset += n;
    }
    from->length = 0;
}

int main(void)
{
    lwip_init();
    gateway.pcb = pppos_create(&gateway.netif, output, status, &gateway);
    client.pcb = pppos_create(&client.netif, output, status, &client);
    assert(gateway.pcb && client.pcb);
    ip4_addr_t addr;
    ip4addr_aton(PALM_GATEWAY_IP, &addr);
    ppp_set_ipcp_ouraddr(gateway.pcb, &addr);
    ppp_set_ipcp_dnsaddr(gateway.pcb, 0, &addr);
    ip4addr_aton(PALM_CLIENT_IP, &addr);
    ppp_set_ipcp_hisaddr(gateway.pcb, &addr);
    ppp_set_usepeerdns(gateway.pcb, 0);
    ppp_set_usepeerdns(client.pcb, 1);
    for (int session = 0; session < 3; session++) {
        assert(ppp_listen(gateway.pcb) == ERR_OK);
        assert(ppp_connect(client.pcb, 0) == ERR_OK);
        for (int pass = 0; pass < 1000 && (!gateway.up || !client.up); pass++) {
            transfer(&client, &gateway);
            transfer(&gateway, &client);
            clock_ms += 10;
            sys_check_timeouts();
        }
        assert(gateway.up && client.up);
        ip4addr_aton(PALM_CLIENT_IP, &addr);
        assert(ip4_addr_cmp(netif_ip4_addr(&client.netif), &addr));
        ip4addr_aton(PALM_GATEWAY_IP, &addr);
        assert(ip4_addr_cmp(netif_ip4_gw(&client.netif), &addr));
        assert(ip4_addr_cmp(netif_ip4_addr(&gateway.netif), &addr));
        assert(ip4_addr_cmp(ip_2_ip4(dns_getserver(0)), &addr));

        struct udp_pcb *gw_udp = udp_new();
        assert(gw_udp);
        ip_addr_t gw_ip;
        ip4addr_aton(PALM_GATEWAY_IP, &gw_ip);
        assert(udp_bind(gw_udp, &gw_ip, 53) == ERR_OK);
        udp_bind_netif(gw_udp, &gateway.netif);
        udp_recv(gw_udp, test_udp_recv, NULL);

        struct udp_pcb *cl_udp = udp_new();
        assert(cl_udp);
        udp_bind_netif(cl_udp, &client.netif);
        udp_recv(cl_udp, test_cl_recv, NULL);
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 28, PBUF_RAM);
        assert(p);
        udp_rx_called = false;
        cl_rx_called = false;
        assert(udp_sendto(cl_udp, p, &gw_ip, 53) == ERR_OK);
        pbuf_free(p);
        for (int pass = 0; pass < 20 && (!udp_rx_called || !cl_rx_called); pass++) {
            transfer(&client, &gateway);
            transfer(&gateway, &client);
            clock_ms += 10;
            sys_check_timeouts();
        }
        assert(udp_rx_called);
        assert(cl_rx_called);
        udp_remove(gw_udp);
        udp_remove(cl_udp);

        assert(ppp_close(client.pcb, 1) == ERR_OK);
        assert(ppp_close(gateway.pcb, 1) == ERR_OK);
        assert(!gateway.up && !client.up);
        gateway.length = client.length = 0;
    }
    assert(ppp_free(client.pcb) == ERR_OK);
    assert(ppp_free(gateway.pcb) == ERR_OK);
    puts("SDK PPP: LCP/IPCP, fixed peer IP, DNS and three reconnects passed");
    return 0;
}
