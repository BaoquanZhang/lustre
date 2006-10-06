/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Modified from NFSv4 project for Lustre
 * Copyright 2004 - 2006, Cluster File Systems, Inc.
 * All rights reserved
 * Author: Eric Mei <ericm@clusterfs.com>
 */

#ifndef __PTLRPC_GSS_GSS_INTERNAL_H_
#define __PTLRPC_GSS_GSS_INTERNAL_H_

#include <lustre_sec.h>

/*
 * rawobj stuff
 */
typedef struct netobj_s {
        __u32           len;
        __u8            data[0];
} netobj_t;

#define NETOBJ_EMPTY    ((netobj_t) { 0 })

typedef struct rawobj_s {
        __u32           len;
        __u8           *data;
} rawobj_t;

#define RAWOBJ_EMPTY    ((rawobj_t) { 0, NULL })

typedef struct rawobj_buf_s {
        __u32           dataoff;
        __u32           datalen;
        __u32           buflen;
        __u8           *buf;
} rawobj_buf_t;

int rawobj_alloc(rawobj_t *obj, char *buf, int len);
void rawobj_free(rawobj_t *obj);
int rawobj_equal(rawobj_t *a, rawobj_t *b);
int rawobj_dup(rawobj_t *dest, rawobj_t *src);
int rawobj_serialize(rawobj_t *obj, __u32 **buf, __u32 *buflen);
int rawobj_extract(rawobj_t *obj, __u32 **buf, __u32 *buflen);
int rawobj_extract_alloc(rawobj_t *obj, __u32 **buf, __u32 *buflen);
int rawobj_extract_local(rawobj_t *obj, __u32 **buf, __u32 *buflen);
int rawobj_from_netobj(rawobj_t *rawobj, netobj_t *netobj);
int rawobj_from_netobj_alloc(rawobj_t *obj, netobj_t *netobj);


/*
 * several timeout values. client refresh upcall timeout we using
 * default in pipefs implemnetation.
 */
#define __TIMEOUT_DELTA                 (10)

#define GSS_SECINIT_RPC_TIMEOUT                                         \
        (obd_timeout < __TIMEOUT_DELTA ?                                \
         __TIMEOUT_DELTA : obd_timeout - __TIMEOUT_DELTA)

#define GSS_SECFINI_RPC_TIMEOUT         (__TIMEOUT_DELTA)
#define GSS_SECSVC_UPCALL_TIMEOUT       (GSS_SECINIT_RPC_TIMEOUT)

static inline
unsigned long gss_round_ctx_expiry(unsigned long expiry,
                                   unsigned long sec_flags)
{
        if (sec_flags & PTLRPC_SEC_FL_REVERSE)
                return expiry;

        if (get_seconds() + __TIMEOUT_DELTA <= expiry)
                return expiry - __TIMEOUT_DELTA;

        return expiry;
}

/* we try to force reconnect import 20m eariler than real expiry.
 * kerberos 5 usually allow 5m time skew, but which is adjustable,
 * so if we set krb5 to allow > 20m time skew, we have chance that
 * server's reverse ctx expired but client still hasn't start to
 * refresh it -- it's BAD. So here we actually put a limit on the
 * enviroment of krb5 (or other authentication mechanism)
 */
#define GSS_MAX_TIME_SKEW       (20 * 60)

static inline
unsigned long gss_round_imp_reconnect(unsigned long expiry)
{
        unsigned long now = get_seconds();
        unsigned long nice = GSS_MAX_TIME_SKEW + __TIMEOUT_DELTA;

        while (nice && (now + nice >= expiry))
                nice = nice / 2;

        return (expiry - nice);
}

/*
 * Max encryption element in block cipher algorithms, most of which
 * are 64 bits, here we choose 128 bits to be safe for future extension.
 */
#define GSS_MAX_CIPHER_BLOCK               (16)

/*
 * XXX make it visible of kernel and lgssd/lsvcgssd
 */
#define GSSD_INTERFACE_VERSION          (1)

#define PTLRPC_GSS_VERSION              (1)


enum ptlrpc_gss_proc {
        PTLRPC_GSS_PROC_DATA            = 0,
        PTLRPC_GSS_PROC_INIT            = 1,
        PTLRPC_GSS_PROC_CONTINUE_INIT   = 2,
        PTLRPC_GSS_PROC_DESTROY         = 3,
        PTLRPC_GSS_PROC_ERR             = 4,
};

enum ptlrpc_gss_svc {
        PTLRPC_GSS_SVC_NONE             = 1,
        PTLRPC_GSS_SVC_INTEGRITY        = 2,
        PTLRPC_GSS_SVC_PRIVACY          = 3,
};

enum ptlrpc_gss_tgt {
        LUSTRE_GSS_TGT_MDS              = 0,
        LUSTRE_GSS_TGT_OSS              = 1,
};

/*
 * following 3 header must have the same size and offset
 */
struct gss_header {
        __u32                   gh_version;     /* gss version */
        __u32                   gh_flags;       /* wrap flags */
        __u32                   gh_proc;        /* proc */
        __u32                   gh_seq;         /* sequence */
        __u32                   gh_svc;         /* service */
        __u32                   gh_pad1;
        __u32                   gh_pad2;
        __u32                   gh_pad3;
        netobj_t                gh_handle;      /* context handle */
};

struct gss_rep_header {
        __u32                   gh_version;
        __u32                   gh_flags;
        __u32                   gh_proc;
        __u32                   gh_major;
        __u32                   gh_minor;
        __u32                   gh_seqwin;
        __u32                   gh_pad2;
        __u32                   gh_pad3;
        netobj_t                gh_handle;
};

struct gss_err_header {
        __u32                   gh_version;
        __u32                   gh_flags;
        __u32                   gh_proc;
        __u32                   gh_major;
        __u32                   gh_minor;
        __u32                   gh_pad1;
        __u32                   gh_pad2;
        __u32                   gh_pad3;
        netobj_t                gh_handle;
};

/*
 * part of wire context information send from client which be saved and
 * used later by server.
 */
struct gss_wire_ctx {
        __u32                   gw_proc;
        __u32                   gw_seq;
        __u32                   gw_svc;
        rawobj_t                gw_handle;
};

#define PTLRPC_GSS_MAX_HANDLE_SIZE      (8)
#define PTLRPC_GSS_HEADER_SIZE          (sizeof(struct gss_header) + \
                                         PTLRPC_GSS_MAX_HANDLE_SIZE)


#define GSS_SEQ_WIN                     (256)
#define GSS_SEQ_WIN_MAIN                GSS_SEQ_WIN
#define GSS_SEQ_WIN_BACK                (64)
#define GSS_SEQ_REPACK_THRESHOLD        (GSS_SEQ_WIN_MAIN / 2)

struct gss_svc_seq_data {
        spinlock_t              ssd_lock;
        /*
         * highest sequence number seen so far, for main and back window
         */
        __u32                   ssd_max_main;
        __u32                   ssd_max_back;
        /*
         * main and back window
         * for i such that ssd_max - GSS_SEQ_WIN < i <= ssd_max, the i-th bit
         * of ssd_win is nonzero iff sequence number i has been seen already.
         */
        unsigned long           ssd_win_main[GSS_SEQ_WIN_MAIN/BITS_PER_LONG];
        unsigned long           ssd_win_back[GSS_SEQ_WIN_BACK/BITS_PER_LONG];
};

struct gss_svc_ctx {
        unsigned int            gsc_usr_root:1,
                                gsc_usr_mds:1,
                                gsc_remote:1;
        uid_t                   gsc_uid;
        gid_t                   gsc_gid;
        uid_t                   gsc_mapped_uid;
        rawobj_t                gsc_rvs_hdl;
        struct gss_svc_seq_data gsc_seqdata;
        struct gss_ctx         *gsc_mechctx;
};

struct gss_svc_reqctx {
        struct ptlrpc_svc_ctx   src_base;
        struct gss_wire_ctx     src_wirectx;
        struct gss_svc_ctx     *src_ctx;
        unsigned int            src_init:1,
                                src_init_continue:1,
                                src_err_notify:1;
        int                     src_reserve_len;
};

struct gss_cli_ctx {
        struct ptlrpc_cli_ctx   gc_base;
        __u32                   gc_flavor;
        __u32                   gc_proc;
        __u32                   gc_win;
        atomic_t                gc_seq;
        rawobj_t                gc_handle;
        struct gss_ctx         *gc_mechctx;
};

struct gss_sec {
        struct ptlrpc_sec       gs_base;
        struct gss_api_mech    *gs_mech;
        spinlock_t              gs_lock;
        __u64                   gs_rvs_hdl;
};

#define GSS_CTX_INIT_MAX_LEN            (1024)

/*
 * This only guaranteed be enough for current krb5 des-cbc-crc . We might
 * adjust this when new enc type or mech added in.
 */
#define GSS_PRIVBUF_PREFIX_LEN         (32)
#define GSS_PRIVBUF_SUFFIX_LEN         (32)

static inline
struct gss_svc_reqctx *gss_svc_ctx2reqctx(struct ptlrpc_svc_ctx *ctx)
{
        LASSERT(ctx);
        return container_of(ctx, struct gss_svc_reqctx, src_base);
}

/* sec_gss.c */
struct gss_header *gss_swab_header(struct lustre_msg *msg, int segment);
netobj_t *gss_swab_netobj(struct lustre_msg *msg, int segment);

void gss_cli_ctx_uptodate(struct gss_cli_ctx *gctx);
int gss_pack_err_notify(struct ptlrpc_request *req, __u32 major, __u32 minor);
int gss_check_seq_num(struct gss_svc_seq_data *sd, __u32 seq_num, int set);

/* gss_bulk.c */
int gss_cli_ctx_wrap_bulk(struct ptlrpc_cli_ctx *ctx,
                          struct ptlrpc_request *req,
                          struct ptlrpc_bulk_desc *desc);
int gss_cli_ctx_unwrap_bulk(struct ptlrpc_cli_ctx *ctx,
                            struct ptlrpc_request *req,
                            struct ptlrpc_bulk_desc *desc);
int gss_svc_unwrap_bulk(struct ptlrpc_request *req,
                        struct ptlrpc_bulk_desc *desc);
int gss_svc_wrap_bulk(struct ptlrpc_request *req,
                      struct ptlrpc_bulk_desc *desc);

/* gss_mech_switch.c */
int init_kerberos_module(void);
void cleanup_kerberos_module(void);

/* gss_generic_token.c */
int g_token_size(rawobj_t *mech, unsigned int body_size);
void g_make_token_header(rawobj_t *mech, int body_size, unsigned char **buf);
__u32 g_verify_token_header(rawobj_t *mech, int *body_size,
                            unsigned char **buf_in, int toksize);


/* gss_upcall.c */
int gss_do_ctx_init_rpc(char *buffer, unsigned long count);
int gss_do_ctx_fini_rpc(struct gss_cli_ctx *gctx);
int gss_ctx_refresh_pipefs(struct ptlrpc_cli_ctx *ctx);
int gss_sec_upcall_init(struct gss_sec *gsec);
void gss_sec_upcall_cleanup(struct gss_sec *gsec);
int __init gss_init_upcall(void);
void __exit gss_exit_upcall(void);

/* gss_svc_upcall.c */
__u64 gss_get_next_ctx_index(void);
int gss_svc_upcall_install_rvs_ctx(struct obd_import *imp,
                                   struct gss_sec *gsec,
                                   struct gss_cli_ctx *gctx);
int gss_svc_upcall_handle_init(struct ptlrpc_request *req,
                               struct gss_svc_reqctx *grctx,
                               struct gss_wire_ctx *gw,
                               struct obd_device *target,
                               __u32 lustre_svc,
                               rawobj_t *rvs_hdl,
                               rawobj_t *in_token);
struct gss_svc_ctx *gss_svc_upcall_get_ctx(struct ptlrpc_request *req,
                                           struct gss_wire_ctx *gw);
void gss_svc_upcall_put_ctx(struct gss_svc_ctx *ctx);
void gss_svc_upcall_destroy_ctx(struct gss_svc_ctx *ctx);

int  __init gss_svc_init_upcall(void);
void __exit gss_svc_exit_upcall(void);

/* lproc_gss.c */
void gss_stat_oos_record_cli(int behind);
void gss_stat_oos_record_svc(int phase, int replay);
int  gss_init_lproc(void);
void gss_exit_lproc(void);

/* gss_krb5_mech.c */
int __init init_kerberos_module(void);
void __exit cleanup_kerberos_module(void);


/* debug */
static inline
void __dbg_memdump(char *name, void *ptr, int size)
{
        char *buf, *p = (char *) ptr;
        int bufsize = size * 2 + 1, i;

        OBD_ALLOC(buf, bufsize);
        if (!buf) {
                printk("DUMP ERROR: can't alloc %d bytes\n", bufsize);
                return;
        }

        for (i = 0; i < size; i++)
                sprintf(&buf[i+i], "%02x", (__u8) p[i]);
        buf[size + size] = '\0';
        printk("DUMP %s@%p(%d): %s\n", name, ptr, size, buf);
        OBD_FREE(buf, bufsize);
}

#endif /* __PTLRPC_GSS_GSS_INTERNAL_H_ */
