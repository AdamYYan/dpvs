/*
 * DPVS is a software load balancer (Virtual Server) based on DPDK.
 *
 * Copyright (C) 2018 iQIYI (www.iqiyi.com).
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/**
 * IPv6 protocol for "lite stack".
 * Linux Kernel net/ipv6/ is referred.
 *
 * Lei Chen <raychen@qiyi.com>, initial, Jul 2018.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <netinet/ip6.h>
#include "common.h"
#include "mbuf.h"
#include "inet.h"
#include "ipv6.h"
#include "route6.h"
#include "parser/parser.h"
#include "neigh.h"

/*
 * IPv6 inet hooks
 */
static const struct inet6_protocol *inet6_prots[INET_MAX_PROTS];
static rte_rwlock_t inet6_prot_lock;

/*
 * IPv6 configures with default values.
 */
static bool conf_ipv6_forwarding = false;
static bool conf_ipv6_disable = false;

/*
 * IPv6 statistics
 */
static RTE_DEFINE_PER_LCORE(struct inet_stats, ip6_stats);
#define this_ip6_stats  RTE_PER_LCORE(ip6_stats)

#define IP6_INC_STATS(__f__) \
    do { \
        this_ip6_stats.__f__++; \
    } while (0)

#define IP6_DEC_STATS(__f__) \
    do { \
        this_ip6_stats.__f__--; \
    } while (0)

#define IP6_ADD_STATS(__f__, val) \
    do { \
        this_ip6_stats.__f__ += (val); \
    } while (0)

#define IP6_UPD_PO_STATS(__f__, val) \
    do { \
        this_ip6_stats.__f__##pkts ++; \
        this_ip6_stats.__f__##octets += (val); \
    } while (0)

/*
 * internal functions
 */
static void ip6_prot_init(void)
{
    int i;

    rte_rwlock_init(&inet6_prot_lock);
    rte_rwlock_write_lock(&inet6_prot_lock);

    for (i = 0; i < NELEMS(inet6_prots); i++)
        inet6_prots[i] = NULL;

    rte_rwlock_write_unlock(&inet6_prot_lock);
}

static void ip6_conf_forward(vector_t tokens)
{
    char *str = set_value(tokens);

    assert(str);

    if (strcasecmp(str, "on") == 0)
        conf_ipv6_forwarding = true;
    else if (strcasecmp(str, "off") == 0)
        conf_ipv6_forwarding = false;
    else
        RTE_LOG(WARNING, IPV6, "%s: bad config %s\n", __func__, str);

    FREE_PTR(str);
}

static void ip6_conf_disable(vector_t tokens)
{
    char *str = set_value(tokens);

    assert(str);

    if (strcasecmp(str, "on") == 0)
        conf_ipv6_disable = true;
    else if (strcasecmp(str, "off") == 0)
        conf_ipv6_disable = false;
    else
        RTE_LOG(WARNING, IPV6, "%s: bad config %s\n", __func__, str);

    FREE_PTR(str);
}

/* refer linux:ip6_input_finish() */
static int ip6_local_in_fin(struct rte_mbuf *mbuf)
{
    uint8_t nexthdr;
    int (*handler)(struct rte_mbuf *mbuf) = NULL;
    struct route6 *rt = mbuf->userdata;
    bool is_final, have_final = false;
    const struct inet6_protocol *prot;
    struct ip6_hdr *hdr = ip6_hdr(mbuf);
    int ret = EDPVS_INVAL;

    /*
     * release route info saved in @userdata
     * and set it to IPv6 fixed header for upper layer.
     */
    if (rt) {
        route6_put(rt);
        mbuf->userdata = NULL;
    }

    mbuf->userdata = (void *)hdr;
    nexthdr = hdr->ip6_nxt;

    /* parse extension headers */
resubmit:
    /*
     * l3_len is not the transport header length.
     * we just borrow it to save info for each step when processing
     * fixed header and extension header.
     *
     * l3_len is initially the fix header size (ipv6_rcv),
     * and being set to ext-header size by each non-final protocol.
     */
    if (rte_pktmbuf_adj(mbuf, mbuf->l3_len) == NULL)
        goto discard;

resubmit_final:
    rte_rwlock_read_lock(&inet6_prot_lock);

    prot = inet6_prots[nexthdr];
    if (unlikely(!prot)) {
        /* no proto, kni may like it.*/
        rte_rwlock_read_unlock(&inet6_prot_lock);
        IP6_INC_STATS(inunknownprotos);
        goto kni;
    }

    is_final = (prot->flags & INET6_PROTO_F_FINAL);

    if (have_final) {
        /* final proto don't allow encap non-final */
        if (!is_final) {
            rte_rwlock_read_unlock(&inet6_prot_lock);
            goto discard;
        }
    } else if (is_final) {
        have_final = true;

        /* check mcast, if failed, kni may like it. */
        if (ipv6_addr_is_multicast(&hdr->ip6_dst) &&
            !inet_chk_mcast_addr(AF_INET6, netif_port_get(mbuf->port),
                                 (union inet_addr *)&hdr->ip6_dst, 
                                 (union inet_addr *)&hdr->ip6_src)) {
            rte_rwlock_read_unlock(&inet6_prot_lock);
            goto kni;
        }
    }

    handler = prot->handler;

    /* tunnel may try lock again, need release lock */
    rte_rwlock_read_unlock(&inet6_prot_lock);

    assert(handler);
    ret = handler(mbuf);

    /*
     * 1. if return > 0, it's always "nexthdr",
     *    no matter if proto is final or not.
     * 2. if return == 0, the pkt is consumed.
     * 3. should not return < 0, or it'll be ignored.
     * 4. mbuf->l3_len must be adjusted by handler.
     */
    if (ret > 0) {
        nexthdr = ret;

        if (is_final)
            goto resubmit_final;
        else
            goto resubmit;
    } else {
        IP6_INC_STATS(indelivers);
    }

    return ret;

kni:
    return EDPVS_KNICONTINUE;

discard:
    IP6_INC_STATS(indiscards);
    rte_pktmbuf_free(mbuf);
    return EDPVS_INVAL;
}

static int ip6_local_in(struct rte_mbuf *mbuf)
{
    return INET_HOOK(AF_INET6, INET_HOOK_LOCAL_IN, mbuf,
                     netif_port_get(mbuf->port), NULL, ip6_local_in_fin);
}

static int ip6_mc_local_in(struct rte_mbuf *mbuf)
{
    struct ip6_hdr *iph = ip6_hdr(mbuf);
    struct route6 *rt = mbuf->userdata;

    IP6_UPD_PO_STATS(inmcast, mbuf->pkt_len);

    if (inet_chk_mcast_addr(AF_INET6, netif_port_get(mbuf->port), 
                            (union inet_addr *)&iph->ip6_dst, NULL))
        return ip6_local_in(mbuf);
    else {
        route6_put(rt);
        return EDPVS_KNICONTINUE; /* not drop */
    }
}

static inline struct in6_addr *ip6_rt_nexthop(struct route6 *rt,
                                              struct in6_addr *daddr)
{
    if ((rt->rt6_flags & RTF_GATEWAY) && !ipv6_addr_any(&rt->rt6_gateway))
        return &rt->rt6_gateway;
    else
        return daddr;
}

static inline unsigned int ip6_mtu_forward(struct route6 *rt)
{
    if (rt->rt6_mtu)
        return rt->rt6_mtu;
    else if (rt->rt6_dev && rt->rt6_dev->mtu)
        return rt->rt6_dev->mtu;
    else
        return IPV6_MIN_MTU;
}

static int ip6_fragment(struct rte_mbuf *mbuf, uint32_t mtu,
                        int (*out)(struct rte_mbuf *))
{
    struct route6 *rt = mbuf->userdata;

    /* TODO: */

    IP6_INC_STATS(fragfails);
    route6_put(rt);
    rte_pktmbuf_free(mbuf);
    return EDPVS_FRAG;
}

static int ip6_output_fin2(struct rte_mbuf *mbuf)
{
    struct ip6_hdr *hdr = ip6_hdr(mbuf);
    struct route6 *rt = mbuf->userdata;
    struct in6_addr *nexthop;
    int err;

    if (ipv6_addr_is_multicast(&hdr->ip6_dst)) {
        IP6_UPD_PO_STATS(outmcast, mbuf->pkt_len);

        if (IPV6_ADDR_MC_SCOPE(&hdr->ip6_dst) <= IPV6_ADDR_SCOPE_NODELOCAL) {
            IP6_INC_STATS(outdiscards);
            rte_pktmbuf_free(mbuf);
            route6_put(rt);
            return EDPVS_INVAL;
        }
    }

    nexthop = ip6_rt_nexthop(rt, &hdr->ip6_dst);
    mbuf->packet_type = ETHER_TYPE_IPv6;

    err = neigh_output(AF_INET6, (union inet_addr *)nexthop, mbuf, rt->rt6_dev);
    route6_put(rt);

    return err;
}

static int ip6_output_fin(struct rte_mbuf *mbuf)
{
    struct route6 *rt = mbuf->userdata;

    if (mbuf->pkt_len > rt->rt6_mtu)
        return ip6_fragment(mbuf, rt->rt6_mtu, ip6_output_fin2);
    else
        return ip6_output_fin2(mbuf);
}

static int ip6_output(struct rte_mbuf *mbuf)
{
    struct route6 *rt = mbuf->userdata;
    assert(rt);

    IP6_UPD_PO_STATS(out, mbuf->pkt_len);
    mbuf->port = rt->rt6_dev->id;

    if (unlikely(conf_ipv6_disable)) {
        IP6_INC_STATS(outdiscards);
        route6_put(rt);
        rte_pktmbuf_free(mbuf);
        return EDPVS_OK;
    }

    return INET_HOOK(AF_INET6, INET_HOOK_POST_ROUTING, mbuf, NULL,
                     rt->rt6_dev, ip6_output_fin);
}

static int ip6_local_out(struct rte_mbuf *mbuf)
{
    struct route6 *rt = mbuf->userdata;

    return INET_HOOK(AF_INET6, INET_HOOK_LOCAL_OUT, mbuf, NULL,
                     rt->rt6_dev, ip6_output);
}

static int ip6_forward_fin(struct rte_mbuf *mbuf)
{
    IP6_INC_STATS(outforwdatagrams);
    IP6_ADD_STATS(outoctets, mbuf->pkt_len);

    return ip6_output(mbuf);
}

static int ip6_forward(struct rte_mbuf *mbuf)
{
    struct ip6_hdr *hdr = ip6_hdr(mbuf);
    struct route6 *rt = mbuf->userdata;
    int addrtype;
    uint32_t mtu;

    if (!conf_ipv6_forwarding)
        goto error;

    if (mbuf->packet_type != ETH_PKT_HOST)
        goto drop;

    /* not support forward multicast */
    if (ipv6_addr_is_multicast(&hdr->ip6_dst))
        goto error;

    if (hdr->ip6_hlim <= 1) {
        mbuf->port = rt->rt6_dev->id;
        //icmpv6_send(mbuf, ICMPV6_TIME_EXCEED, ICMPV6_EXC_HOPLIMIT, 0);
        IP6_INC_STATS(inhdrerrors);
        rte_pktmbuf_free(mbuf);
        return EDPVS_INVAL;
    }

    /* security critical */
    addrtype = ipv6_addr_type(&hdr->ip6_src);

    if (addrtype == IPV6_ADDR_ANY ||
        addrtype & (IPV6_ADDR_MULTICAST | IPV6_ADDR_LOOPBACK))
        goto error;

    if (addrtype & IPV6_ADDR_LINKLOCAL) {
        //icmpv6_send(mbuf, ICMPV6_DEST_UNREACH, ICMPV6_NOT_NEIGHBOUR, 0);
        goto error;
    }

    /* is packet too big ? */
    mtu = ip6_mtu_forward(rt);
    if (mtu < IPV6_MIN_MTU)
        mtu = IPV6_MIN_MTU;

    if (mbuf->pkt_len > mtu) {
        mbuf->port = rt->rt6_dev->id;
        //icmpv6_send(mbuf, ICMPV6_PKT_TOOBIG, 0, mtu);

        IP6_INC_STATS(intoobigerrors);
        IP6_INC_STATS(fragfails);
        goto drop;
    }

    /* decrease TTL */
    hdr->ip6_hlim--;

    return INET_HOOK(AF_INET6, INET_HOOK_FORWARD, mbuf,
                     netif_port_get(mbuf->port), rt->rt6_dev, ip6_forward_fin);

error:
    IP6_INC_STATS(inaddrerrors);
drop:
    rte_pktmbuf_free(mbuf);
    return EDPVS_INVAL;
}

static struct route6 *ip6_route_input(struct rte_mbuf *mbuf)
{
    struct ip6_hdr *hdr = ip6_hdr(mbuf);
    struct flow6 fl6 = {
        .fl6_iif    = netif_port_get(mbuf->port),
        .fl6_daddr  = hdr->ip6_dst,
        .fl6_saddr  = hdr->ip6_src,
        .fl6_proto  = hdr->ip6_nxt,
    };

    return route6_input(mbuf, &fl6);
}

static int ip6_rcv_fin(struct rte_mbuf *mbuf)
{
    struct route6 *rt = NULL;
    eth_type_t etype = mbuf->packet_type;
    struct ip6_hdr *iph = ip6_hdr(mbuf);

    rt = ip6_route_input(mbuf);
    if (!rt) {
        IP6_INC_STATS(innoroutes);
        goto kni;
    }

    /*
     * @userdata is used for route info in L3.
     * someday, we may use extended mbuf if have more L3 info
     * then route need to be saved into mbuf.
     */
    mbuf->userdata = (void *)rt;

    if (rt->rt6_flags & RTF_LOCALIN) {
        return ip6_local_in(mbuf);
    } else if (ipv6_addr_type(&iph->ip6_dst) & IPV6_ADDR_MULTICAST) {
        return ip6_mc_local_in(mbuf);
    } else if (rt->rt6_flags & RTF_FORWARD) {
        /* pass multi-/broad-cast to kni */
        if (etype != ETH_PKT_HOST)
            goto kni;

        return ip6_forward(mbuf);
    }

    IP6_INC_STATS(innoroutes);

    /* to kni */

kni:
    if (rt) {
        route6_put(rt);
        mbuf->userdata = NULL;
    }
    return EDPVS_KNICONTINUE;
}

static int ip6_rcv(struct rte_mbuf *mbuf, struct netif_port *dev)
{
    const struct ip6_hdr *hdr;
    uint32_t pkt_len, tot_len;
    eth_type_t etype = mbuf->packet_type;

    if (unlikely(etype == ETH_PKT_OTHERHOST || !dev)) {
        rte_pktmbuf_free(mbuf);
        return EDPVS_DROP;
    }

    IP6_UPD_PO_STATS(in, mbuf->pkt_len);

    if (unlikely(conf_ipv6_disable)) {
        IP6_INC_STATS(indiscards);
        goto drop;
    }

    if (unlikely(mbuf_may_pull(mbuf, sizeof(*hdr)) != 0))
        goto err;

    hdr = ip6_hdr(mbuf);

    if (unlikely(((hdr->ip6_vfc&0xf0)>>4) != 6))
        goto err;

    /*
     * we do not have loopback dev for DPVS at all,
     * as RFC4291, loopback must be send/recv from lo dev.
     * so let's drop all pkt with loopback address.
     */
    if (ipv6_addr_loopback(&hdr->ip6_src) ||
        ipv6_addr_loopback(&hdr->ip6_dst))
        goto err;

    /*
     * RFC4291 Errata ID: 3480
     * interface-local scope is useful only for loopback transmission of
     * multicast but we do not have loopback dev.
     */
    if (ipv6_addr_is_multicast(&hdr->ip6_dst) &&
        IPV6_ADDR_MC_SCOPE(&hdr->ip6_dst) == 1)
        goto err;

    /*
     * drop unicast encapsulated in link-layer multicast/broadcast.
     * kernel is configurable, so need we ?
     */
    if (!ipv6_addr_is_multicast(&hdr->ip6_dst) &&
        (etype == ETH_PKT_BROADCAST || etype == ETH_PKT_MULTICAST))
        goto err;

    /* RFC4291 2.7 */
    if (ipv6_addr_is_multicast(&hdr->ip6_dst) &&
        IPV6_ADDR_MC_SCOPE(&hdr->ip6_dst) == 0)
        goto err;

    /*
     * RFC4291 2.7
     * source address must not be multicast.
     */
    if (ipv6_addr_is_multicast(&hdr->ip6_src))
        goto err;

    pkt_len = ntohs(hdr->ip6_plen);
    tot_len = pkt_len + sizeof(*hdr);

    /* check pkt_len, note it's zero if jumbo payload option is present. */
    if (pkt_len || hdr->ip6_nxt != NEXTHDR_HOP) {
        if (tot_len > mbuf->pkt_len) {
            IP6_INC_STATS(intruncatedpkts);
            goto drop;
        }

        if (mbuf->pkt_len > tot_len) {
            if (rte_pktmbuf_trim(mbuf, mbuf->pkt_len - tot_len) != 0)
                goto err;
        }
    }

    /*
     * now @l3_len record fix header only,
     * it may change, when parsing extension headers.
     * @userdata is used to save route info in L3.
     */
    mbuf->l3_len = sizeof(*hdr);
    mbuf->userdata = NULL;

    /* hop-by-hop option header */
    if (hdr->ip6_nxt == NEXTHDR_HOP) {
        if (ipv6_parse_hopopts(mbuf) != EDPVS_OK)
            goto err;
    }

    return INET_HOOK(AF_INET6, INET_HOOK_PRE_ROUTING, mbuf,
                     dev, NULL, ip6_rcv_fin);

err:
    IP6_INC_STATS(inhdrerrors);
drop:
    rte_pktmbuf_free(mbuf);
    return EDPVS_DROP;
}

static struct pkt_type ip6_pkt_type = {
    /*.type    =  */
    .func   = ip6_rcv,
    .port   = NULL,
};

/*
 * IPv6 APIs
 */
int ipv6_init(void)
{
    int err;

    ip6_prot_init();

    err = ipv6_exthdrs_init();
    if (err)
        return err;

    /* htons, cpu_to_be16 not work when struct initialization :( */
    ip6_pkt_type.type = htons(ETHER_TYPE_IPv6);

    err = netif_register_pkt(&ip6_pkt_type);
    if (err)
        goto reg_pkt_err;

    err = ipv6_ctrl_init();
    if (err)
        goto ctrl_err;

    return EDPVS_OK;

reg_pkt_err:
    ipv6_exthdrs_term();
ctrl_err:
    netif_unregister_pkt(&ip6_pkt_type);

    return err;
}

int ipv6_term(void)
{
    int err;

    err = ipv6_ctrl_term();
    if (err)
        return err;

    err = netif_unregister_pkt(&ip6_pkt_type);
    if (err)
        return err;

    ipv6_exthdrs_term();

    return EDPVS_OK;
}

int ipv6_xmit(struct rte_mbuf *mbuf, struct flow6 *fl6)
{
    struct route6 *rt;
    struct ip6_hdr *hdr;

    if (unlikely(!mbuf || !fl6 || ipv6_addr_any(&fl6->fl6_daddr))) {
        if (mbuf)
            rte_pktmbuf_free(mbuf);
        return EDPVS_INVAL;
    }

    /* TODO: to support jumbo packet */
    if (mbuf->pkt_len > IPV6_MAXPLEN) {
        IP6_INC_STATS(outdiscards);
        rte_pktmbuf_free(mbuf);
        return EDPVS_NOROOM;
    }

    /* route decision */
    rt = route6_output(mbuf, fl6);
    if (!rt) {
        IP6_INC_STATS(outnoroutes);
        rte_pktmbuf_free(mbuf);
        return EDPVS_NOROUTE;
    }

    mbuf->userdata = (void *)rt;

    hdr = (void *)rte_pktmbuf_prepend(mbuf, sizeof(*hdr));
    if (unlikely(!hdr)) {
        route6_put(rt);
        rte_pktmbuf_free(mbuf);
        IP6_INC_STATS(outdiscards);
        return EDPVS_NOROOM;
    }

    memset(hdr, 0, sizeof(*hdr));
    hdr->ip6_vfc    = 0x60;
    hdr->ip6_flow  |= htonl(((uint64_t)fl6->fl6_tos<<20) | \
                            (ntohl(fl6->fl6_flow)&0xfffffUL));
    hdr->ip6_plen   = htons(mbuf->pkt_len - sizeof(*hdr));
    hdr->ip6_nxt    = fl6->fl6_proto;
    hdr->ip6_hlim   = fl6->fl6_ttl ? : INET_DEF_TTL;
    hdr->ip6_src    = fl6->fl6_saddr;
    hdr->ip6_dst    = fl6->fl6_daddr;

    if (ipv6_addr_any(&hdr->ip6_src) &&
        hdr->ip6_nxt != IPPROTO_ICMPV6) {
        union inet_addr saddr;

        inet_addr_select(AF_INET6, rt->rt6_dev, (void *)&fl6->fl6_daddr,
                         fl6->fl6_scope, &saddr);
        hdr->ip6_src = saddr.in6;
    }

    return ip6_local_out(mbuf);
}

int ipv6_register_protocol(struct inet6_protocol *prot,
                           unsigned char protocol)
{
    int err = EDPVS_OK;

    rte_rwlock_write_lock(&inet6_prot_lock);
    if (inet6_prots[protocol])
        err = EDPVS_EXIST;
    else
        inet6_prots[protocol] = prot;
    rte_rwlock_write_unlock(&inet6_prot_lock);

    return err;
}

int ipv6_unregister_protocol(struct inet6_protocol *prot,
                             unsigned char protocol)
{
    int err = EDPVS_OK;

    rte_rwlock_write_lock(&inet6_prot_lock);
    if (inet6_prots[protocol] != prot)
        err = EDPVS_NOTEXIST;
    else
        inet6_prots[protocol] = NULL;
    rte_rwlock_write_unlock(&inet6_prot_lock);

    return err;
}

int ipv6_stats_cpu(struct inet_stats *stats)
{
    if (!stats)
        return EDPVS_INVAL;

    memcpy(stats, &this_ip6_stats, sizeof(*stats));

    return EDPVS_OK;
}

/*
 * configure file
 */
void ipv6_conf_install(void)
{
    install_keyword_root("ipv6", NULL);
    install_keyword("forwarding", ip6_conf_forward, KW_TYPE_NORMAL);
    install_keyword("disable", ip6_conf_disable, KW_TYPE_NORMAL);
}
