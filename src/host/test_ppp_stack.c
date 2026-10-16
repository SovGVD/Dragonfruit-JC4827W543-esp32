#include "portal_protocol.h"
#include "lwip/init.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "netif/ppp/pppos.h"
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
