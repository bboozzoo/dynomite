#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <netdb.h>
#include <arpa/nameser.h>
#include <resolv.h>
#ifdef __APPLE__
#include <arpa/nameser_compat.h>
#endif

#include "dyn_seeds_provider.h"
#include "dyn_core.h"
#include "dyn_string.h"

// Keep poling DNS server for the TXT record with seeds, same format as for Florida seeds
//
//
// DYNOMITE_DNS_TXT_NAME=_dynomite.yourhost.com src/dynomite -c conf/dynomite_dns_single.yml -v 11
//
// To compile the domain use make CFLAGS="-DDNS_TXT_NAME=_dynomite.yourhost.com"
//
//


#ifndef DNS_TXT_NAME
#define DNS_TXT_NAME "_dynomite.ec2-internal"
#endif

static char * txtName  = NULL;
static int64_t last = 0; //storing last time for seeds check
static uint32_t last_seeds_hash = 0;

static bool seeds_check()
{
    int64_t now = dn_msec_now();

    int64_t delta = (int64_t)(now - last);
    log_debug(LOG_VERB, "Delta or elapsed time : %lu", delta);
    log_debug(LOG_VERB, "Seeds check internal %d", SEEDS_CHECK_INTERVAL);

    if (delta > SEEDS_CHECK_INTERVAL) {
        last = now;
        return true;
    }

    return false;
}


static uint32_t
hash_seeds(uint8_t *seeds, size_t length)
{
    const uint8_t *ptr = seeds;
    uint32_t value = 0;

    while (length--) {
        uint32_t val = (uint32_t) *ptr++;
        value += val;
        value += (value << 10);
        value ^= (value >> 6);
    }
    value += (value << 3);
    value ^= (value >> 11);
    value += (value << 15);

    return value;
}

uint8_t
dns_get_seeds(struct context * ctx, struct mbuf *seeds_buf) 
{
    static int _env_checked = 0;

    if (!_env_checked) {
        _env_checked = 1;
        txtName = getenv("DYNOMITE_DNS_TXT_NAME");
        if (txtName == NULL)  txtName = DNS_TXT_NAME;
    }

    log_debug(LOG_VVERB, "checking for %s", txtName);

    if (!seeds_check()) {
        return DN_NOOPS;
    }

    unsigned char buf[BUFSIZ];
    int qr = res_query(txtName, C_IN, T_TXT, buf, sizeof(buf));
    if (qr == -1) {
        log_debug(LOG_DEBUG, "DNS response for %s: %s", txtName, hstrerror(h_errno));
        return DN_NOOPS;
    }
    if (qr >= sizeof(buf)) {
        log_debug(LOG_DEBUG, "DNS reply is too large for %s: %d, bufsize: %d", txtName, r, sizeof(buf));
        return DN_NOOPS;
    }
    HEADER *hdr = (HEADER*)buf;
    if (hdr->rcode != NOERROR) {
        log_debug(LOG_DEBUG, "DNS reply code for %s: %d", txtName, hdr->rcode);
        return DN_NOOPS;
    }
    int na = ntohs(hdr->ancount);

    ns_msg m;
    int k = ns_initparse(buf, qr, &m);
    if (k == -1) {
        log_debug(LOG_DEBUG, "ns_initparse error for %s: %s", txtName, strerror(errno));
        return DN_NOOPS;
    }
    int i;
    ns_rr rr;
    for (i = 0; i < na; ++i) {
        int pk = ns_parserr(&m, ns_s_an, i, &rr);
        if (pk == -1) {
            log_debug(LOG_DEBUG, "ns_parserr for %s: %s", txtName, strerror (errno));
            return DN_NOOPS;
        }
        mbuf_rewind(seeds_buf);
        unsigned char *r = ns_rr_rdata(rr);
        if (r[0] >= ns_rr_rdlen(rr)) {
            log_debug(LOG_DEBUG, "invalid TXT length for %s: %d < %d", txtName, r[0], ns_rr_rdlen(rr));
            return DN_NOOPS;
        }
        log_debug(LOG_VERB, "seeds for %s: %.*s", txtName, r[0], r +1);
        mbuf_copy(seeds_buf, r + 1, r[0]);
    }

    uint32_t seeds_hash = hash_seeds(seeds_buf->pos, mbuf_length(seeds_buf));
    if (last_seeds_hash != seeds_hash) {
        last_seeds_hash = seeds_hash;
    } else {
        return DN_NOOPS;
    }
    return DN_OK;
}


