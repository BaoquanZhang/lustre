/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

#ifndef OSC_INTERNAL_H
#define OSC_INTERNAL_H

#define OAP_MAGIC 8675309

struct osc_async_page {
        int                     oap_magic;
        unsigned short          oap_cmd;
        unsigned short          oap_interrupted:1;

        struct list_head        oap_pending_item;
        struct list_head        oap_urgent_item;
        struct list_head        oap_rpc_item;

        obd_off                 oap_obj_off;
        unsigned                oap_page_off;
        enum async_flags        oap_async_flags;

        struct brw_page         oap_brw_page;

        struct oig_callback_context oap_occ;
        struct obd_io_group     *oap_oig;
        struct ptlrpc_request   *oap_request;
        struct client_obd       *oap_cli;
        struct lov_oinfo        *oap_loi;

	struct obd_async_page_ops *oap_caller_ops;
        void                    *oap_caller_data;
};

#define oap_page        oap_brw_page.pg
#define oap_count       oap_brw_page.count
#define oap_brw_flags   oap_brw_page.flag

#define OAP_FROM_COOKIE(c)                                                    \
        (LASSERT(((struct osc_async_page *)(c))->oap_magic == OAP_MAGIC),     \
         (struct osc_async_page *)(c))

struct osc_cache_waiter {
        struct list_head        ocw_entry;
        cfs_waitq_t             ocw_waitq;
        struct osc_async_page   *ocw_oap;
        int                     ocw_rc;
};

#define OSCC_FLAG_RECOVERING         0x01
#define OSCC_FLAG_CREATING           0x02
#define OSCC_FLAG_NOSPC              0x04 /* can't create more objects on OST */
#define OSCC_FLAG_SYNC_IN_PROGRESS   0x08 /* only allow one thread to sync */
#define OSCC_FLAG_LOW                0x10
#define OSCC_FLAG_EXITING            0x20

int osc_precreate(struct obd_export *exp, int need_create);
int osc_create(struct obd_export *exp, struct obdo *oa,
	       struct lov_stripe_md **ea, struct obd_trans_info *oti);
int osc_real_create(struct obd_export *exp, struct obdo *oa,
	       struct lov_stripe_md **ea, struct obd_trans_info *oti);
void oscc_init(struct obd_device *obd);
void osc_wake_cache_waiters(struct client_obd *cli);

#ifdef LPROCFS
int lproc_osc_attach_seqstat(struct obd_device *dev);
#else
static inline int lproc_osc_attach_seqstat(struct obd_device *dev) {return 0;}
#endif

#ifndef min_t
#define min_t(type,x,y) \
        ({ type __x = (x); type __y = (y); __x < __y ? __x: __y; })
#endif

static inline int osc_recoverable_error(int rc)
{
        return (rc == -EIO || rc == -EROFS || rc == -ENOMEM || rc == -EAGAIN);
}

/* return 1 if osc should be resend request */
static inline int osc_should_resend(int resend, struct client_obd *cli)
{
        return atomic_read(&cli->cl_resends) ? 
                atomic_read(&cli->cl_resends) > resend : 1; 
}


#endif /* OSC_INTERNAL_H */
