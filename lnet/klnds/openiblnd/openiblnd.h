/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 *   Author: Eric Barton <eric@bartonsoftware.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <linux/uio.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <linux/kmod.h>
#include <linux/sysctl.h>

#include <net/sock.h>
#include <linux/in.h>

#define DEBUG_SUBSYSTEM S_NAL

#include <libcfs/kp30.h>
#include <portals/p30.h>
#include <portals/lib-p30.h>

#include <ts_ib_core.h>
#include <ts_ib_cm.h>
#include <ts_ib_sa_client.h>

#if CONFIG_SMP
# define IBNAL_N_SCHED      num_online_cpus()   /* # schedulers */
#else
# define IBNAL_N_SCHED      1                   /* # schedulers */
#endif

#define IBNAL_FMR                    1
//#define IBNAL_CALLBACK_CTXT  IB_CQ_CALLBACK_PROCESS
#define IBNAL_CALLBACK_CTXT  IB_CQ_CALLBACK_INTERRUPT


/* defaults for modparams/tunables */
#define IBNAL_N_CONND                4          /* # connection daemons */
#define IBNAL_MIN_RECONNECT_INTERVAL 1          /* first failed connection retry... */
#define IBNAL_MAX_RECONNECT_INTERVAL 60         /* ...exponentially increasing to this (seconds) */
#define IBNAL_CONCURRENT_PEERS       1024       /* # nodes all talking at once to me */
#define IBNAL_CKSUM                  0          /* checksum kib_msg_t? */
#define IBNAL_TIMEOUT                50         /* default comms timeout (seconds) */
#define IBNAL_NTX                    64         /* # tx descs */
#define IBNAL_NTX_NBLK               256        /* # reserved tx descs */

/* tunables fixed at compile time */
#define IBNAL_PEER_HASH_SIZE         101        /* # peer lists */
#define IBNAL_RESCHED                100        /* # scheduler loops before reschedule */
#define IBNAL_MSG_QUEUE_SIZE         8          /* # messages/RDMAs in-flight */
#define IBNAL_CREDIT_HIGHWATER       6          /* when to eagerly return credits */
#define IBNAL_MSG_SIZE              (4<<10)     /* max size of queued messages (inc hdr) */
#define IBNAL_RDMA_BASE              0x0eeb0000

/* QP tunables */
#define IBNAL_RETRY                  7          /* # times to retry */
#define IBNAL_RNR_RETRY              7          /*  */
#define IBNAL_CM_RETRY               7          /* # times to retry connection */
#define IBNAL_FLOW_CONTROL           1
#define IBNAL_RESPONDER_RESOURCES    8

/************************/
/* derived constants... */

/* TX messages (shared by all connections) */
#define IBNAL_TX_MSGS()       (*kibnal_tunables.kib_ntx + \
                               *kibnal_tunables.kib_ntx_nblk)
#define IBNAL_TX_MSG_BYTES()  (IBNAL_TX_MSGS() * IBNAL_MSG_SIZE)
#define IBNAL_TX_MSG_PAGES()  ((IBNAL_TX_MSG_BYTES() + PAGE_SIZE - 1)/PAGE_SIZE)

/* RX messages (per connection) */
#define IBNAL_RX_MSGS         IBNAL_MSG_QUEUE_SIZE
#define IBNAL_RX_MSG_BYTES    (IBNAL_RX_MSGS * IBNAL_MSG_SIZE)
#define IBNAL_RX_MSG_PAGES    ((IBNAL_RX_MSG_BYTES + PAGE_SIZE - 1)/PAGE_SIZE)

/* we may have up to 2 completions per transmit +
   1 completion per receive, per connection */
#define IBNAL_CQ_ENTRIES()  ((2*IBNAL_TX_MSGS()) +                                      \
                             (IBNAL_RX_MSGS * *kibnal_tunables.kib_concurrent_peers))

typedef struct
{
        int      *kib_n_connd;                  /* # connection daemons */
        int      *kib_min_reconnect_interval;   /* min connect retry seconds... */
        int      *kib_max_reconnect_interval;   /* max connect retry seconds */
        int      *kib_concurrent_peers;         /* max # peers */
        int      *kib_cksum;                    /* checksum kib_msg_t? */
        int      *kib_timeout;                  /* comms timeout (seconds) */
        int      *kib_ntx;                      /* # tx descs */
        int      *kib_ntx_nblk;                 /* # reserved tx descs */

        struct ctl_table_header *kib_sysctl;    /* sysctl interface */
} kib_tunables_t;

typedef struct
{
        int               ibp_npages;           /* # pages */
        int               ibp_mapped;           /* mapped? */
        __u64             ibp_vaddr;            /* mapped region vaddr */
        __u32             ibp_lkey;             /* mapped region lkey */
        __u32             ibp_rkey;             /* mapped region rkey */
        struct ib_mr     *ibp_handle;           /* mapped region handle */
        struct page      *ibp_pages[0];
} kib_pages_t;

typedef struct
{
        int               kib_init;             /* initialisation state */
        __u64             kib_incarnation;      /* which one am I */
        int               kib_shutdown;         /* shut down? */
        atomic_t          kib_nthreads;         /* # live threads */
        ptl_ni_t         *kib_ni;               /* _the_ openib interface */

        __u64             kib_svc_id;           /* service number I listen on */
        tTS_IB_GID        kib_svc_gid;          /* device/port GID */
        __u16             kib_svc_pkey;         /* device/port pkey */
        
        void             *kib_listen_handle;    /* IB listen handle */
        
        rwlock_t          kib_global_lock;      /* stabilize peer/conn ops */

        struct list_head *kib_peers;            /* hash table of all my known peers */
        int               kib_peer_hash_size;   /* size of kib_peers */
        int               kib_nonewpeers;       /* prevent new peers? */
        atomic_t          kib_npeers;           /* # peers extant */
        atomic_t          kib_nconns;           /* # connections extant */

        struct list_head  kib_reaper_conns;     /* connections to reap */
        wait_queue_head_t kib_reaper_waitq;     /* reaper sleeps here */
        unsigned long     kib_reaper_waketime;  /* when reaper will wake */
        spinlock_t        kib_reaper_lock;      /* serialise */

        struct list_head  kib_connd_peers;      /* peers waiting for a connection */
        struct list_head  kib_connd_acceptq;    /* accepted sockets to handle */
        wait_queue_head_t kib_connd_waitq;      /* connection daemons sleep here */
        spinlock_t        kib_connd_lock;       /* serialise */

        wait_queue_head_t kib_sched_waitq;      /* schedulers sleep here */
        struct list_head  kib_sched_txq;        /* tx requiring attention */
        struct list_head  kib_sched_rxq;        /* rx requiring attention */
        spinlock_t        kib_sched_lock;       /* serialise */

        struct kib_tx    *kib_tx_descs;         /* all the tx descriptors */
        kib_pages_t      *kib_tx_pages;         /* premapped tx msg pages */

        struct list_head  kib_idle_txs;         /* idle tx descriptors */
        struct list_head  kib_idle_nblk_txs;    /* idle reserved tx descriptors */
        wait_queue_head_t kib_idle_tx_waitq;    /* block here for tx descriptor */
        __u64             kib_next_tx_cookie;   /* RDMA completion cookie */
        spinlock_t        kib_tx_lock;          /* serialise */

        struct ib_device *kib_device;           /* "the" device */
        struct ib_device_properties kib_device_props; /* its properties */
        int               kib_port;             /* port on the device */
        struct ib_port_properties kib_port_props; /* its properties */
        struct ib_pd     *kib_pd;               /* protection domain */
#if IBNAL_FMR
        struct ib_fmr_pool *kib_fmr_pool;       /* fast memory region pool */
#endif
        struct ib_cq     *kib_cq;               /* completion queue */

} kib_data_t;

#define IBNAL_INIT_NOTHING         0
#define IBNAL_INIT_DATA            1
#define IBNAL_INIT_LIB             2
#define IBNAL_INIT_PD              3
#define IBNAL_INIT_FMR             4
#define IBNAL_INIT_TXD             5
#define IBNAL_INIT_CQ              6
#define IBNAL_INIT_ALL             7

typedef struct kib_acceptsock                   /* accepted socket queued for connd */
{
        struct list_head     ibas_list;         /* queue for attention */
        struct socket       *ibas_sock;         /* the accepted socket */
} kib_acceptsock_t;

/************************************************************************
 * IB Wire message format.
 * These are sent in sender's byte order (i.e. receiver flips).
 * They may be sent via TCP/IP (service ID,GID,PKEY query/response),
 * as private data in the connection request/response, or "normally".
 */

typedef struct kib_svcrsp                       /* service response */
{
        __u64             ibsr_svc_id;          /* service's id */
        __u8              ibsr_svc_gid[16];     /* service's gid */
        __u16             ibsr_svc_pkey;        /* service's pkey */
} WIRE_ATTR kib_svcrsp_t;

typedef struct kib_connparams
{
        __u32             ibcp_queue_depth;
} WIRE_ATTR kib_connparams_t;

typedef struct
{
        union {
                struct ib_mr    *mr;
                struct ib_fmr   *fmr;
        }                 md_handle;
        __u32             md_lkey;
        __u32             md_rkey;
        __u64             md_addr;
} kib_md_t;

typedef struct
{
        __u32             rd_key;               /* remote key */
        __u32             rd_nob;               /* # of bytes */
        __u64             rd_addr;              /* remote io vaddr */
} WIRE_ATTR kib_rdma_desc_t;

typedef struct
{
        ptl_hdr_t         ibim_hdr;             /* portals header */
        char              ibim_payload[0];      /* piggy-backed payload */
} WIRE_ATTR kib_immediate_msg_t;

typedef struct
{
        ptl_hdr_t         ibrm_hdr;             /* portals header */
        __u64             ibrm_cookie;          /* opaque completion cookie */
        kib_rdma_desc_t   ibrm_desc;            /* where to suck/blow */
} WIRE_ATTR kib_rdma_msg_t;

typedef struct
{
        __u64             ibcm_cookie;          /* opaque completion cookie */
        __u32             ibcm_status;          /* completion status */
} WIRE_ATTR kib_completion_msg_t;

typedef struct
{
        /* First 2 fields fixed FOR ALL TIME */
        __u32             ibm_magic;            /* I'm an openibnal message */
        __u16             ibm_version;          /* this is my version number */

        __u8              ibm_type;             /* msg type */
        __u8              ibm_credits;          /* returned credits */
        __u32             ibm_nob;              /* # bytes in whole message */
        __u32             ibm_cksum;            /* checksum (0 == no checksum) */
        __u64             ibm_srcnid;           /* sender's NID */
        __u64             ibm_srcstamp;         /* sender's incarnation */
        __u64             ibm_dstnid;           /* destination's NID */
        __u64             ibm_dststamp;         /* destination's incarnation */
        union {
                kib_svcrsp_t          svcrsp;
                kib_connparams_t      connparams;
                kib_immediate_msg_t   immediate;
                kib_rdma_msg_t        rdma;
                kib_completion_msg_t  completion;
        } WIRE_ATTR       ibm_u;
} WIRE_ATTR kib_msg_t;

#define IBNAL_MSG_MAGIC  PTL_PROTO_OPENIB_MAGIC /* unique magic */
#define IBNAL_MSG_VERSION              2        /* current protocol version */

#define IBNAL_MSG_SVCQRY            0xb0        /* service query */
#define IBNAL_MSG_SVCRSP            0xb1        /* service response */
#define IBNAL_MSG_CONNREQ           0xc0        /* connection request */
#define IBNAL_MSG_CONNACK           0xc1        /* connection acknowledge */
#define IBNAL_MSG_NOOP              0xd0        /* nothing (just credits) */
#define IBNAL_MSG_IMMEDIATE         0xd1        /* portals hdr + payload */
#define IBNAL_MSG_PUT_RDMA          0xd2        /* portals PUT hdr + source rdma desc */
#define IBNAL_MSG_PUT_DONE          0xd3        /* signal PUT rdma completion */
#define IBNAL_MSG_GET_RDMA          0xd4        /* portals GET hdr + sink rdma desc */
#define IBNAL_MSG_GET_DONE          0xd5        /* signal GET rdma completion */

/***********************************************************************/

typedef struct kib_rx                           /* receive message */
{
        struct list_head          rx_list;      /* queue for attention */
        struct kib_conn          *rx_conn;      /* owning conn */
        int                       rx_rdma;      /* RDMA completion posted? */
        int                       rx_posted;    /* posted? */
        __u64                     rx_vaddr;     /* pre-mapped buffer (hca vaddr) */
        kib_msg_t                *rx_msg;       /* pre-mapped buffer (host vaddr) */
        struct ib_receive_param   rx_sp;        /* receive work item */
        struct ib_gather_scatter  rx_gl;        /* and it's memory */
} kib_rx_t;

typedef struct kib_tx                           /* transmit message */
{
        struct list_head          tx_list;      /* queue on idle_txs ibc_tx_queue etc. */
        int                       tx_isnblk;    /* I'm reserved for non-blocking sends */
        struct kib_conn          *tx_conn;      /* owning conn */
        int                       tx_mapped;    /* mapped for RDMA? */
        int                       tx_sending;   /* # tx callbacks outstanding */
        int                       tx_status;    /* completion status */
        unsigned long             tx_deadline;  /* completion deadline */
        int                       tx_passive_rdma; /* peer sucks/blows */
        int                       tx_passive_rdma_wait; /* waiting for peer to complete */
        __u64                     tx_passive_rdma_cookie; /* completion cookie */
        ptl_msg_t                *tx_ptlmsg[2]; /* ptl msgs to finalize on completion */
        kib_md_t                  tx_md;        /* RDMA mapping (active/passive) */
        __u64                     tx_vaddr;     /* pre-mapped buffer (hca vaddr) */
        kib_msg_t                *tx_msg;       /* pre-mapped buffer (host vaddr) */
        int                       tx_nsp;       /* # send work items */
        struct ib_send_param      tx_sp[2];     /* send work items... */
        struct ib_gather_scatter  tx_gl[2];     /* ...and their memory */
} kib_tx_t;

#define KIB_TX_UNMAPPED       0
#define KIB_TX_MAPPED         1
#define KIB_TX_MAPPED_FMR     2

typedef struct kib_connreq
{
        /* active connection-in-progress state */
        struct kib_conn           *cr_conn;
        kib_msg_t                  cr_msg;
        __u64                      cr_tid;
        tTS_IB_GID                 cr_gid;
        kib_svcrsp_t               cr_svcrsp;
        struct ib_path_record      cr_path;
        struct ib_cm_active_param  cr_connparam;
} kib_connreq_t;

typedef struct kib_conn
{
        struct kib_peer    *ibc_peer;           /* owning peer */
        struct list_head    ibc_list;           /* stash on peer's conn list */
        __u64               ibc_incarnation;    /* which instance of the peer */
        atomic_t            ibc_refcount;       /* # users */
        int                 ibc_state;          /* what's happening */
        atomic_t            ibc_nob;            /* # bytes buffered */
        int                 ibc_nsends_posted;  /* # uncompleted sends */
        int                 ibc_credits;        /* # credits I have */
        int                 ibc_outstanding_credits; /* # credits to return */
        struct list_head    ibc_tx_queue;       /* send queue */
        struct list_head    ibc_active_txs;     /* active tx awaiting completion */
        spinlock_t          ibc_lock;           /* serialise */
        kib_rx_t           *ibc_rxs;            /* the rx descs */
        kib_pages_t        *ibc_rx_pages;       /* premapped rx msg pages */
        struct ib_qp       *ibc_qp;             /* queue pair */
        __u32               ibc_qpn;            /* queue pair number */
        tTS_IB_CM_COMM_ID   ibc_comm_id;        /* connection ID? */
        kib_connreq_t      *ibc_connreq;        /* connection request state */
} kib_conn_t;

#define IBNAL_CONN_INIT_NOTHING      0          /* initial state */
#define IBNAL_CONN_INIT_QP           1          /* ibc_qp set up */
#define IBNAL_CONN_CONNECTING        2          /* started to connect */
#define IBNAL_CONN_ESTABLISHED       3          /* connection established */
#define IBNAL_CONN_DEATHROW          4          /* waiting to be closed */
#define IBNAL_CONN_ZOMBIE            5          /* waiting to be freed */

typedef struct kib_peer
{
        struct list_head    ibp_list;           /* stash on global peer list */
        struct list_head    ibp_connd_list;     /* schedule on kib_connd_peers */
        lnet_nid_t           ibp_nid;            /* who's on the other end(s) */
        __u32               ibp_ip;             /* IP to query for peer conn params */
        int                 ibp_port;           /* port to qery for peer conn params */
        __u64               ibp_incarnation;    /* peer's incarnation */
        atomic_t            ibp_refcount;       /* # users */
        int                 ibp_persistence;    /* "known" peer refs */
        struct list_head    ibp_conns;          /* all active connections */
        struct list_head    ibp_tx_queue;       /* msgs waiting for a conn */
        int                 ibp_connecting;     /* connecting+accepting */
        unsigned long       ibp_reconnect_time; /* when reconnect may be attempted */
        unsigned long       ibp_reconnect_interval; /* exponential backoff */
} kib_peer_t;

extern kib_data_t      kibnal_data;
extern kib_tunables_t  kibnal_tunables;

static inline struct list_head *
kibnal_nid2peerlist (lnet_nid_t nid)
{
        unsigned int hash = ((unsigned int)nid) % kibnal_data.kib_peer_hash_size;

        return (&kibnal_data.kib_peers [hash]);
}

static inline int
kibnal_peer_active(kib_peer_t *peer)
{
        /* Am I in the peer hash table? */
        return (!list_empty(&peer->ibp_list));
}

static inline void
kibnal_queue_tx_locked (kib_tx_t *tx, kib_conn_t *conn)
{
        /* CAVEAT EMPTOR: tx takes caller's ref on conn */

        LASSERT (tx->tx_nsp > 0);               /* work items set up */
        LASSERT (tx->tx_conn == NULL);          /* only set here */

        tx->tx_conn = conn;
        tx->tx_deadline = jiffies + *kibnal_tunables.kib_timeout * HZ;
        list_add_tail(&tx->tx_list, &conn->ibc_tx_queue);
}

#if 0
static inline void
kibnal_show_rdma_attr (kib_conn_t *conn)
{
        struct ib_qp_attribute qp_attr;
        int                    rc;

        memset (&qp_attr, 0, sizeof(qp_attr));
        rc = ib_qp_query(conn->ibc_qp, &qp_attr);
        if (rc != 0) {
                CERROR ("Can't get qp attrs: %d\n", rc);
                return;
        }

        CWARN ("RDMA CAPABILITY: write %s read %s\n",
               (qp_attr.valid_fields & TS_IB_QP_ATTRIBUTE_RDMA_ATOMIC_ENABLE) ?
               (qp_attr.enable_rdma_write ? "enabled" : "disabled") : "invalid",
               (qp_attr.valid_fields & TS_IB_QP_ATTRIBUTE_RDMA_ATOMIC_ENABLE) ?
               (qp_attr.enable_rdma_read ? "enabled" : "disabled") : "invalid");
}
#endif

static inline __u64
kibnal_page2phys (struct page *p)
{
        return page_to_phys(p);
}

/* CAVEAT EMPTOR:
 * We rely on tx/rx descriptor alignment to allow us to use the lowest bit
 * of the work request id as a flag to determine if the completion is for a
 * transmit or a receive.  It seems that that the CQ entry's 'op' field
 * isn't always set correctly on completions that occur after QP teardown. */

static inline __u64
kibnal_ptr2wreqid (void *ptr, int isrx)
{
        unsigned long lptr = (unsigned long)ptr;

        LASSERT ((lptr & 1) == 0);
        return (__u64)(lptr | (isrx ? 1 : 0));
}

static inline void *
kibnal_wreqid2ptr (__u64 wreqid)
{
        return (void *)(((unsigned long)wreqid) & ~1UL);
}

static inline int
kibnal_wreqid_is_rx (__u64 wreqid)
{
        return (wreqid & 1) != 0;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
# define sk_allocation  allocation
# define sk_data_ready  data_ready
# define sk_write_space write_space
# define sk_user_data   user_data
# define sk_prot        prot
# define sk_sndbuf      sndbuf
# define sk_socket      socket
# define sk_wmem_queued wmem_queued
# define sk_err         err
# define sk_sleep       sleep
#endif

int kibnal_startup (ptl_ni_t *ni);
void kibnal_shutdown (ptl_ni_t *ni);
int kibnal_ctl(ptl_ni_t *ni, unsigned int cmd, void *arg);
int kibnal_send (ptl_ni_t *ni, void *private,
                       ptl_msg_t *ptlmsg, ptl_hdr_t *hdr,
                       int type, lnet_process_id_t tgt, int routing,
                       unsigned int payload_niov, struct iovec *payload_iov,
                       size_t payload_offset, size_t payload_nob);
int kibnal_send_pages (ptl_ni_t *ni, void *private,
                             ptl_msg_t *ptlmsg, ptl_hdr_t *hdr,
                             int type, lnet_process_id_t tgt, int routing,
                             unsigned int payload_niov, lnet_kiov_t *payload_kiov,
                             size_t payload_offset, size_t payload_nob);
int kibnal_recv(ptl_ni_t *ni, void *private,
                      ptl_msg_t *ptlmsg, unsigned int niov,
                      struct iovec *iov, size_t offset,
                      size_t mlen, size_t rlen);
int kibnal_recv_pages(ptl_ni_t *ni, void *private,
                            ptl_msg_t *ptlmsg, unsigned int niov,
                            lnet_kiov_t *kiov, size_t offset,
                            size_t mlen, size_t rlen);
int kibnal_accept(ptl_ni_t *ni, struct socket *sock);

extern void kibnal_init_msg(kib_msg_t *msg, int type, int body_nob);
extern void kibnal_pack_msg(kib_msg_t *msg, int credits, 
                            lnet_nid_t dstnid, __u64 dststamp);
extern int kibnal_unpack_msg(kib_msg_t *msg, int nob);
extern void kibnal_handle_svcqry (struct socket *sock);
extern int kibnal_make_svcqry (kib_conn_t *conn);
extern void kibnal_free_acceptsock (kib_acceptsock_t *as);
extern int kibnal_listener_procint(ctl_table *table, int write, 
                                   struct file *filp, void *buffer, 
                                   size_t *lenp);
extern int kibnal_create_peer (kib_peer_t **peerp, lnet_nid_t nid);
extern void kibnal_put_peer (kib_peer_t *peer);
extern int kibnal_add_persistent_peer(lnet_nid_t nid, __u32 ip, int port);
extern int kibnal_del_peer (lnet_nid_t nid);
extern kib_peer_t *kibnal_find_peer_locked (lnet_nid_t nid);
extern void kibnal_unlink_peer_locked (kib_peer_t *peer);
extern int  kibnal_close_stale_conns_locked (kib_peer_t *peer,
                                              __u64 incarnation);
extern kib_conn_t *kibnal_create_conn (void);
extern void kibnal_put_conn (kib_conn_t *conn);
extern void kibnal_destroy_conn (kib_conn_t *conn);
extern int kibnal_alloc_pages (kib_pages_t **pp, int npages, int access);
extern void kibnal_free_pages (kib_pages_t *p);

extern void kibnal_check_sends (kib_conn_t *conn);

extern tTS_IB_CM_CALLBACK_RETURN
kibnal_bad_conn_callback (tTS_IB_CM_EVENT event, tTS_IB_CM_COMM_ID cid,
                          void *param, void *arg);
extern tTS_IB_CM_CALLBACK_RETURN
kibnal_conn_callback (tTS_IB_CM_EVENT event, tTS_IB_CM_COMM_ID cid,
                       void *param, void *arg);
extern tTS_IB_CM_CALLBACK_RETURN
kibnal_passive_conn_callback (tTS_IB_CM_EVENT event, tTS_IB_CM_COMM_ID cid,
                               void *param, void *arg);

extern void kibnal_close_conn_locked (kib_conn_t *conn, int error);
extern void kibnal_destroy_conn (kib_conn_t *conn);
extern int  kibnal_thread_start (int (*fn)(void *arg), void *arg);
extern int  kibnal_scheduler(void *arg);
extern int  kibnal_connd (void *arg);
extern int  kibnal_reaper (void *arg);
extern void kibnal_callback (struct ib_cq *cq, struct ib_cq_entry *e, void *arg);
extern void kibnal_init_tx_msg (kib_tx_t *tx, int type, int body_nob);
extern int  kibnal_close_conn (kib_conn_t *conn, int why);
extern void kibnal_start_active_rdma (int type, int status,
                                      kib_rx_t *rx, ptl_msg_t *ptlmsg,
                                      unsigned int niov,
                                      struct iovec *iov, lnet_kiov_t *kiov,
                                      int offset, int nob);

extern int  kibnal_tunables_init(void);
extern void kibnal_tunables_fini(void);
