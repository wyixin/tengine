#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <ngx_http_ipstat_module.h>


typedef struct {
    uintptr_t              key;
    unsigned               ipv6:1;
    unsigned               port:16;
} ngx_http_ipstat_vip_index_t;


typedef struct {
    ngx_cycle_t           *cycle;
    void                  *data;
} ngx_http_ipstat_zone_ctx_t;


typedef enum {
    op_count, op_min, op_max, op_avg, op_rate
} ngx_http_ipstat_op_t;


typedef struct {
    off_t                  offset;
    ngx_http_ipstat_op_t   type;
} ngx_http_ipstat_field_t;


#define VIP_INDEX_START(start)                                            \
    ((ngx_http_ipstat_vip_index_t *)                                      \
        ((char *) (start) + sizeof(ngx_pid_t)))

#define VIP_FIELD(vip, offset) ((ngx_uint_t *) ((char *) vip + offset))

#define VIP_LOCATE(start, boff, voff, off)                                \
    ((ngx_http_ipstat_vip_t *)                                            \
         ((char *) (start) + (boff) + (voff)                              \
                           + sizeof(ngx_http_ipstat_vip_t) * (off)))


static ngx_str_t vip_zn = ngx_string("vip_status_zone");


static ngx_http_ipstat_field_t fields[] = {
    { NGX_HTTP_IPSTAT_CONN_CURRENT, op_count },
    { NGX_HTTP_IPSTAT_CONN_TOTAL, op_count },
    { NGX_HTTP_IPSTAT_REQ_CURRENT, op_count },
    { NGX_HTTP_IPSTAT_REQ_TOTAL, op_count },
    { NGX_HTTP_IPSTAT_BYTES_IN, op_count },
    { NGX_HTTP_IPSTAT_BYTES_OUT, op_count },
    { NGX_HTTP_IPSTAT_RT_MIN, op_min },
    { NGX_HTTP_IPSTAT_RT_MAX, op_max },
    { NGX_HTTP_IPSTAT_RT_AVG, op_avg },
    { NGX_HTTP_IPSTAT_CONN_RATE, op_rate },
    { NGX_HTTP_IPSTAT_REQ_RATE, op_rate }
};


const ngx_uint_t field_num = sizeof(fields) / sizeof(ngx_http_ipstat_field_t);


static void *ngx_http_ipstat_create_main_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_ipstat_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_ipstat_init_vip_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static ngx_int_t ngx_http_ipstat_init_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_ipstat_log_handler(ngx_http_request_t *r);


static void
    ngx_http_ipstat_insert_vip_index(ngx_http_ipstat_vip_index_t *start,
    ngx_http_ipstat_vip_index_t *end, ngx_http_ipstat_vip_index_t *insert);
static ngx_http_ipstat_vip_index_t *
    ngx_http_ipstat_lookup_vip_index(ngx_uint_t key,
    ngx_http_ipstat_vip_index_t *start, ngx_http_ipstat_vip_index_t *end);
static ngx_uint_t
    ngx_http_ipstat_distinguish_same_vip(ngx_http_ipstat_vip_index_t *key,
    ngx_cycle_t *old_cycle);

static char *ngx_http_ipstat_show(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_ipstat_show_handler(ngx_http_request_t *r);


static ngx_command_t   ngx_http_ipstat_commands[] = {

    { ngx_string("vip_status_show"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_ipstat_show,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_ipstat_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_ipstat_init,                  /* postconfiguration */

    ngx_http_ipstat_create_main_conf,      /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_ipstat_module = {
    NGX_MODULE_V1,
    &ngx_http_ipstat_module_ctx,           /* module context */
    ngx_http_ipstat_commands,              /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_http_ipstat_init_process,          /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_http_ipstat_create_main_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_http_ipstat_main_conf_t));
}


static ngx_int_t
ngx_http_ipstat_init(ngx_conf_t *cf)
{
    size_t                        size;
    ngx_int_t                     workers;
    ngx_uint_t                    i, n;
    ngx_shm_zone_t               *shm_zone;
    ngx_core_conf_t              *ccf;
    ngx_http_handler_pt          *h;
    ngx_http_conf_port_t         *port;
    ngx_http_core_main_conf_t    *cmcf;
    ngx_http_ipstat_zone_ctx_t   *ctx;
    ngx_http_ipstat_main_conf_t  *smcf;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cf->cycle->conf_ctx,
                                           ngx_core_module);
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    smcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_ipstat_module);

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_ipstat_zone_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    port = cmcf->ports->elts;
    for (i = 0, n = 0; i < cmcf->ports->nelts; i++) {
        n += port[i].addrs.nelts;
    }

    /* comparible to cpu affinity */

    workers = ccf->worker_processes;

    if (workers == NGX_CONF_UNSET || workers == 0) {
        workers = ngx_ncpu;
    }

    smcf->workers = workers;
    smcf->num = n;
    smcf->index_size = ngx_align(sizeof(ngx_http_ipstat_vip_index_t) * n
                                     + sizeof(ngx_pid_t), 128);
    smcf->block_size = ngx_align(sizeof(ngx_http_ipstat_vip_t) * n
                                     + smcf->index_size, 128);
    size = smcf->block_size * smcf->workers + 256;

    shm_zone = ngx_shared_memory_lc_add(cf, &vip_zn, size,
                                        &ngx_http_ipstat_module, 0);
    if (shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "the vip status zone already exists");
        return NGX_ERROR;
    }

    ctx->cycle = cf->cycle;
    shm_zone->data = ctx;
    shm_zone->init = ngx_http_ipstat_init_vip_zone;
    smcf->vip_zone = shm_zone;

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_ipstat_log_handler;

    return NGX_OK;
}


static void
ngx_http_ipstat_insert_vip_index(ngx_http_ipstat_vip_index_t *start,
    ngx_http_ipstat_vip_index_t *end, ngx_http_ipstat_vip_index_t *insert)
{
    while (insert->key > start->key && start < end) {
        ++start;
    }

    while (end > start) {
        *end = *(end - 1);
        --end;
    }

    *start = *insert;
}


static ngx_http_ipstat_vip_index_t *
ngx_http_ipstat_lookup_vip_index(ngx_uint_t key,
    ngx_http_ipstat_vip_index_t *start, ngx_http_ipstat_vip_index_t *end)
{
    ngx_http_ipstat_vip_index_t  *mid;

    while (start < end) {
        mid = start + (end - start) / 2;

        if (mid->key == key) {
            return mid;
        } else if (mid->key < key) {
            start = mid + 1;
        } else {
            end = mid;
        }
    }

    return NULL;
}


/**
 * In this function, we divide the zone into pieces,
 * whose number equals the number of worker processes.
 * Each worker uses a piece independantly, so no mutax is needed.
 * Each piece aligns at 128 byte address so that when cpu affinity is set,
 * no cpu cache line overlap occurs. Finally, we copy data from last cycle.
 */

static ngx_int_t
ngx_http_ipstat_init_vip_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_uint_t                    i, j, k, n, okey, workers, val;
    ngx_listening_t              *ls;
    ngx_http_port_t              *port;
    ngx_http_in_addr_t           *addr;
#if (NGX_HAVE_INET6)
    ngx_http_in6_addr_t          *addr6;
#endif
    ngx_http_ipstat_vip_t        *vip, *ovip;
    ngx_http_ipstat_rate_t       *rate, *orate;
    ngx_http_ipstat_zone_ctx_t   *ctx, *octx;
    ngx_http_ipstat_main_conf_t  *smcf, *osmcf;
    ngx_http_ipstat_vip_index_t  *idx, *oidx, key, *oidx_c;

    ctx = (ngx_http_ipstat_zone_ctx_t *) shm_zone->data;
    smcf = ngx_http_cycle_get_module_main_conf(ctx->cycle,
                                               ngx_http_ipstat_module);

    ngx_memzero(shm_zone->shm.addr, shm_zone->shm.size);

    if (ngx_shmtx_create(&smcf->mutex, (ngx_shmtx_sh_t *) shm_zone->shm.addr,
                         ctx->cycle->lock_file.data)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    ctx->data = ngx_align_ptr(shm_zone->shm.addr, 128) + 128;
    ls = ctx->cycle->listening.elts;
    idx = VIP_INDEX_START(ctx->data);
    
    for (i = 0, n = 0; i < ctx->cycle->listening.nelts; i++) {

        port = ls[i].servers;
        key.ipv6 = 0;
        key.port = port->port;
        addr = NULL;

#if (NGX_HAVE_INET6)
        addr6 = NULL;

        if (port->ipv6) {
            key.ipv6 = 1;
        }
#endif

        if (port->naddrs > 1) {

#if (NGX_HAVE_INET6)
            if (port->ipv6) {
                addr6 = port->addrs;

            } else {
#endif
                addr = port->addrs;

#if (NGX_HAVE_INET6)
            }
#endif

            for (j = 0; j < port->naddrs; j++) {

#if (NGX_HAVE_INET6)
                if (port->ipv6) {
                    key.key = (uintptr_t) &addr6[j];

                } else {
#endif
                    key.key = (uintptr_t) &addr[j];

#if (NGX_HAVE_INET6)
                }
#endif
                ngx_http_ipstat_insert_vip_index(idx, idx + (n++), &key);
            }

        } else {
            key.key = (uintptr_t) port->addrs;
            ngx_http_ipstat_insert_vip_index(idx, idx + (n++), &key);
        }
    }

    for (i = 1; i < (ngx_uint_t) smcf->workers; i++) {
        ngx_memcpy((char *) ctx->data + i * smcf->block_size, ctx->data,
                   smcf->index_size);
    }

    /* copy vip data from last cycle */

    if (data == NULL) {
        return NGX_OK;
    }

    octx = data;
    oidx = VIP_INDEX_START(octx->data);
    osmcf = ngx_http_cycle_get_module_main_conf(octx->cycle,
                                                ngx_http_ipstat_module);
    workers = ngx_min(smcf->workers, osmcf->workers);

    for (i = 0; i < n; ++i, ++idx) {
        okey = ngx_http_ipstat_distinguish_same_vip(idx, octx->cycle);
        if (okey == 0) {
            continue;
        }

        oidx_c = ngx_http_ipstat_lookup_vip_index(okey, oidx,
                                                  oidx + osmcf->num);
        if (oidx_c == NULL) {
            continue;
        }

        for (j = 0; j < workers; ++j) {
            vip = VIP_LOCATE(ctx->data, j * smcf->block_size,
                             smcf->index_size, i);
            ovip = VIP_LOCATE(octx->data, j * osmcf->block_size,
                              osmcf->index_size, oidx_c - oidx);
            ngx_memcpy(vip, ovip, sizeof(ngx_http_ipstat_vip_t));

            ovip->next = vip;
        }

        if (workers >= osmcf->workers) {
            continue;
        }

        /* reduce number of workers */

        for (j = workers; j < osmcf->workers; ++j) {
            vip = VIP_LOCATE(ctx->data, (j % workers) * smcf->block_size,
                             smcf->index_size, i);
            ovip = VIP_LOCATE(octx->data, j * osmcf->block_size,
                              osmcf->index_size, oidx_c - oidx);
            for (k = 0; k < field_num; ++k) {
                switch (fields[k].type) {

                case op_count:
                    *VIP_FIELD(vip, fields[k].offset)
                                       += *VIP_FIELD(ovip, fields[k].offset);
                    break;

                case op_min:
                    val = ngx_min(*VIP_FIELD(vip, fields[k].offset),
                                  *VIP_FIELD(ovip, fields[k].offset));
                    if (val) {
                        *VIP_FIELD(vip, fields[k].offset) = val;
                    }
                    break;

                case op_max:
                    *VIP_FIELD(vip, fields[k].offset) =
                        ngx_max(*VIP_FIELD(vip, fields[k].offset),
                                *VIP_FIELD(ovip, fields[k].offset));
                    break;

                case op_avg:
                    if (*VIP_FIELD(vip, NGX_HTTP_IPSTAT_REQ_TOTAL)) {
                        *VIP_FIELD(vip, fields[k].offset) +=
                            (*VIP_FIELD(ovip, fields[k].offset)
                                - *VIP_FIELD(vip, fields[k].offset))
                            / *VIP_FIELD(vip, NGX_HTTP_IPSTAT_REQ_TOTAL);
                    }
                    break;

                default:
                    rate = (ngx_http_ipstat_rate_t *)
                                    VIP_FIELD(vip, fields[k].offset);
                    orate = (ngx_http_ipstat_rate_t *)
                                    VIP_FIELD(ovip, fields[k].offset);
                    if (rate->t == orate->t) {
                        rate->last_rate += orate->last_rate;
                        rate->curr_rate += orate->curr_rate;
                    } else if (rate->t + 1 == orate->t) {
                        rate->t = orate->t;
                        rate->last_rate = rate->curr_rate + orate->last_rate;
                        rate->curr_rate = orate->curr_rate;
                    } else if (rate->t + 1 < orate->t) {
                        *rate = *orate;
                    } else if (rate->t == orate->t + 1) {
                        rate->last_rate += orate->curr_rate;
                    }
                    break;
                }
            }
        }
    }

    return NGX_OK;
}


static ngx_uint_t
ngx_http_ipstat_distinguish_same_vip(ngx_http_ipstat_vip_index_t *key,
    ngx_cycle_t *old_cycle)
{
    ngx_uint_t                    i, j;
    ngx_listening_t              *ls;
    ngx_http_port_t              *port;

    ngx_http_in_addr_t           *oaddr, *addr;
#if (NGX_HAVE_INET6)
    ngx_http_in6_addr_t          *oaddr6, *addr6;
#endif

    addr = NULL;

#if (NGX_HAVE_INET6)
    addr6 = NULL;
#endif

    switch (key->ipv6) {
#if (NGX_HAVE_INET6)
    case 1:
        addr6 = (ngx_http_in6_addr_t *) key->key;
        break;
#endif
    default:
        addr = (ngx_http_in_addr_t *) key->key;
        break;
    }

    ls = old_cycle->listening.elts;

    for (i = 0; i < old_cycle->listening.nelts; i++) {

        port = ls[i].servers;

        if (port->port != key->port) {
            continue;
        }

#if (NGX_HAVE_INET6)
        if (port->ipv6 != key->ipv6) {
            continue;
        }
#endif

        if (port->naddrs > 1) {
            switch (key->ipv6) {

#if (NGX_HAVE_INET6)
            case 1:
                oaddr6 = port->addrs;

                for (j = 0; j + 1 < port->naddrs; i++) {
                    if (ngx_memcmp(&oaddr6[j].addr6, &addr6->addr6, 16) == 0) {
                        break;
                    }
                }

                return (uintptr_t) &oaddr6[j];
#endif
            default:
                oaddr = port->addrs;

                for (j = 0; j + 1 < port->naddrs; j++) {
                    if (oaddr[j].addr == addr->addr) {
                        break;
                    }
                }

                return (uintptr_t) &oaddr[j];
            }

        } else {
            switch (key->ipv6) {

#if (NGX_HAVE_INET6)
            case 1:
                oaddr6 = port->addrs;

                if (ngx_memcmp(&oaddr6->addr6, &addr6->addr6, 16) == 0) {
                    return (uintptr_t) oaddr6;
                }

                break;
#endif
            default:
                oaddr = port->addrs;

                if (oaddr->addr == addr->addr) {
                    return (uintptr_t) oaddr;
                }

                break;
            }

            return 0;
        }
    }

    return 0;
}


static ngx_int_t
ngx_http_ipstat_init_process(ngx_cycle_t *cycle)
{
    ngx_int_t                     j;
    ngx_pid_t                    *ppid;
    ngx_uint_t                    i;
    ngx_http_ipstat_zone_ctx_t   *ctx;
    ngx_http_ipstat_main_conf_t  *smcf;

    ppid = NULL;
    smcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_ipstat_module);
    ctx = (ngx_http_ipstat_zone_ctx_t *) smcf->vip_zone->data;

    for (i = 0; i < smcf->workers; i++) {

        ppid = (ngx_pid_t *) ((char *) ctx->data + i * smcf->block_size);

        ngx_shmtx_lock(&smcf->mutex);

        if (*ppid == 0) {
            goto found;
        }

        /* when a worker is down, the new one will take place its position */

        for (j = 0; j < ngx_last_process; j++) {
            if (ngx_processes[j].pid == -1) {
                continue;
            }

            if (ngx_processes[j].pid != *ppid) {
                continue;
            }

            if (ngx_processes[j].exited) {
                goto found;
            }
        }

        ngx_shmtx_unlock(&smcf->mutex);
    }

    /* never reach this point */

    return NGX_OK;

found:

    *ppid = ngx_pid;
    smcf->data = (void *) ppid;

    ngx_shmtx_unlock(&smcf->mutex);

    return NGX_OK;
}


static char *
ngx_http_ipstat_show(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t     *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_ipstat_show_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_ipstat_show_handler(ngx_http_request_t *r)
{
    time_t                        now;
    ngx_int_t                     rc;
    ngx_buf_t                    *b;
    ngx_uint_t                   *f, i, j, k, n, result;
    struct sockaddr_in            sin;
    ngx_http_in_addr_t           *addr;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6           sin6;
    ngx_http_in6_addr_t          *addr6;
#endif
    ngx_chain_t                  *tl, *free, *busy;
    ngx_http_ipstat_vip_t        *vip;
    ngx_http_ipstat_rate_t       *rate;
    ngx_http_ipstat_zone_ctx_t   *ctx;
    ngx_http_ipstat_main_conf_t  *smcf;
    ngx_http_ipstat_vip_index_t  *idx;

    smcf = ngx_http_get_module_main_conf(r, ngx_http_ipstat_module);
    ctx = (ngx_http_ipstat_zone_ctx_t *) smcf->vip_zone->data;
    idx = VIP_INDEX_START(ctx->data);
    vip = VIP_LOCATE(ctx->data, 0, smcf->index_size, 0);
    free = busy = NULL;
    now = ngx_time();

    r->headers_out.status = NGX_HTTP_OK;
    ngx_http_clear_content_length(r);

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    tl = ngx_chain_get_free_buf(r->pool, &free);
    if (tl == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b = tl->buf;
    b->start = ngx_pcalloc(r->pool, 512);
    if (b->start == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->end = b->start + 512;
    b->pos = b->start;
    b->memory = 1;
    b->temporary = 1;
    b->last = ngx_slprintf(b->pos, b->end, "%d\n", smcf->workers);

    if (ngx_http_output_filter(r, tl) == NGX_ERROR) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_chain_update_chains(r->pool, &free, &busy, &tl,
                            (ngx_buf_tag_t) &ngx_http_ipstat_module);

    for (i = 0; i < smcf->num; i++, vip++, idx++) {
        tl = ngx_chain_get_free_buf(r->pool, &free);
        if (tl == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        b = tl->buf;
        if (b->start == NULL) {
            b->start = ngx_pcalloc(r->pool, 512);
            if (b->start == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            b->end = b->start + 512;
        }

        b->last = b->pos = b->start;
        b->memory = 1;
        b->temporary = 1;

        switch (idx->ipv6) {
#if (NGX_HAVE_INET6)
        case 1:
            addr6 = (ngx_http_in6_addr_t *) idx->key;
            sin6.sin6_family = AF_INET6;
            ngx_memcpy(&sin6.sin6_addr.s6_addr, &addr6->addr6, 16);
            sin6.sin6_port = idx->port;
            b->last += ngx_sock_ntop((struct sockaddr *) &sin6,
                                     b->last, 512, 1);
            break;
#endif
        default:
            addr = (ngx_http_in_addr_t *) idx->key;
            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr = addr->addr;
            sin.sin_port = idx->port;
            b->last += ngx_sock_ntop((struct sockaddr *) &sin,
                                     b->last, 512, 1);
            break;
        }

        *b->last++ = ',';

        for (n = 0, k = 0; k < smcf->workers; k++) {
            n += *VIP_FIELD(vip, NGX_HTTP_IPSTAT_REQ_TOTAL
                                                    + k * smcf->block_size);
        }

        for (j = 0; j < field_num; j++) {
            for (result = 0, k = 0; k < smcf->workers; k++) {

                f = VIP_FIELD(vip, fields[j].offset + k * smcf->block_size);

                switch (fields[j].type) {

                case op_count:
                    result += *f;
                    break;

                case op_min:
                    result = ngx_min(result, *f) ? ngx_min(result, *f) : result;
                    break;

                case op_max:
                    result = ngx_max(result, *f);
                    break;

                case op_avg:
                    if (n) {
                        result += (*f - result) / n;
                    }
                    break;

                default:
                    rate = (ngx_http_ipstat_rate_t *) f;
                    if (now == rate->t) {
                        result += rate->last_rate;
                    } else if (now == rate->t + 1) {
                        result += rate->curr_rate;
                    }
                    break;
                }
            }

            b->last = ngx_slprintf(b->last, b->end, "%ud,", result);
        }

        *(b->last - 1) = '\n';

        if (ngx_http_output_filter(r, tl) == NGX_ERROR) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_chain_update_chains(r->pool, &free, &busy, &tl,
                                (ngx_buf_tag_t) &ngx_http_ipstat_module);
    }

    tl = ngx_chain_get_free_buf(r->pool, &free);
    if (tl == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b = tl->buf;
    b->last_buf = 1;

    return ngx_http_output_filter(r, tl);
}


void
ngx_http_ipstat_close_request(void *data)
{
    ngx_connection_t             *c;

    c = data;

    ngx_http_ipstat_count(c->status, NGX_HTTP_IPSTAT_REQ_CURRENT, -1);
}


ngx_http_ipstat_vip_t *
ngx_http_ipstat_find_vip(ngx_uint_t key)
{
    ngx_http_ipstat_main_conf_t  *smcf;
    ngx_http_ipstat_vip_index_t  *idx, *idx_c;
    
    smcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
                                               ngx_http_ipstat_module);

    idx = VIP_INDEX_START(smcf->data);
    idx_c = ngx_http_ipstat_lookup_vip_index(key, idx, idx + smcf->num);

    if (idx_c == NULL) {
        return NULL;
    }

    return VIP_LOCATE(smcf->data, 0, smcf->index_size, idx_c - idx);
}


static ngx_int_t
ngx_http_ipstat_log_handler(ngx_http_request_t *r)
{
    ngx_time_t                   *tp;
    ngx_msec_int_t                ms;

    tp = ngx_timeofday();
    ms = (ngx_msec_int_t)
             ((tp->sec - r->start_sec) * 1000 + (tp->msec - r->start_msec));

    ms = ngx_max(ms, 0);

    ngx_http_ipstat_count(r->connection->status, NGX_HTTP_IPSTAT_BYTES_IN,
                          r->connection->received);
    ngx_http_ipstat_count(r->connection->status, NGX_HTTP_IPSTAT_BYTES_OUT,
                          r->connection->sent);
    ngx_http_ipstat_min(r->connection->status, NGX_HTTP_IPSTAT_RT_MIN,
                        (ngx_uint_t) ms);
    ngx_http_ipstat_max(r->connection->status, NGX_HTTP_IPSTAT_RT_MAX,
                        (ngx_uint_t) ms);
    ngx_http_ipstat_avg(r->connection->status, NGX_HTTP_IPSTAT_RT_AVG,
                        (ngx_uint_t) ms);

    return NGX_OK;
}


void
ngx_http_ipstat_count(void *data, off_t offset, ngx_int_t incr)
{
    ngx_http_ipstat_vip_t        *vip;

    for (vip = data; vip; vip = vip->next) {
        *VIP_FIELD(vip, offset) += incr;
    }
}


void
ngx_http_ipstat_min(void *data, off_t offset, ngx_uint_t val)
{
    ngx_uint_t                   *f, v;
    ngx_http_ipstat_vip_t        *vip;

    for (vip = data; vip; vip = vip->next) {
        f = VIP_FIELD(vip, offset);
        v = ngx_min(*f, val);
        if (v) {
            *f = v;
        }
    }
}


void
ngx_http_ipstat_max(void *data, off_t offset, ngx_uint_t val)
{
    ngx_uint_t                   *f;
    ngx_http_ipstat_vip_t        *vip;

    for (vip = data; vip; vip = vip->next) {
        f = VIP_FIELD(vip, offset);
        if (*f < val) {
            *f = val;
        }
    }
}


void
ngx_http_ipstat_avg(void *data, off_t offset, ngx_uint_t val)
{
    ngx_uint_t                   *f, *n;
    ngx_http_ipstat_vip_t        *vip;

    for (vip = data; vip; vip = vip->next) {
        f = VIP_FIELD(vip, offset);
        n = VIP_FIELD(vip, NGX_HTTP_IPSTAT_REQ_TOTAL);
        *f += (val - *f) / *n;
    }
}


void
ngx_http_ipstat_rate(void *data, off_t offset, ngx_uint_t val)
{
    time_t                        now;
    ngx_http_ipstat_rate_t       *rate;
    ngx_http_ipstat_vip_t        *vip;

    now = ngx_time();

    for (vip = data; vip; vip = vip->next) {
        rate = (ngx_http_ipstat_rate_t *) VIP_FIELD(vip, offset);

        if (rate->t == now) {
            rate->curr_rate += val;
            continue;
        }

        rate->last_rate = (now - rate->t == 1) ? rate->curr_rate : 0;
        rate->curr_rate = val;
        rate->t = now;
    }
}
