/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Light common routines
 *
 *  Copyright (c) 2002-2004 Cluster File Systems, Inc.
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
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>

#ifdef HAVE_XTIO_H
#include <xtio.h>
#endif
#include <sysio.h>
#include <fs.h>
#include <mount.h>
#include <inode.h>
#ifdef HAVE_FILE_H
#include <file.h>
#endif

/* both sys/queue.h (libsysio require it) and portals/lists.h have definition
 * of 'LIST_HEAD'. undef it to suppress warnings
 */
#undef LIST_HEAD
#include <lnet/lnetctl.h>     /* needed for parse_dump */

#include "lutil.h"
#include "llite_lib.h"
#include <linux/lustre_ver.h>

static int lllib_init(void)
{
        if (liblustre_init_current("liblustre") ||
            init_lib_portals() ||
            init_obdclass() ||
            ptlrpc_init() ||
            mdc_init() ||
            lov_init() ||
            osc_init())
                return -1;

        return _sysio_fssw_register("llite", &llu_fssw_ops);
}

int liblustre_process_log(struct config_llog_instance *cfg,
                          char *mdsnid, char *mdsname, char *profile,
                          int allow_recov)
{
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg *lcfg;
        char  *peer = "MDS_PEER_UUID";
        struct obd_device *obd;
        struct lustre_handle mdc_conn = {0, };
        struct obd_export *exp;
        char  *name = "mdc_dev";
        class_uuid_t uuid;
        struct obd_uuid mdc_uuid;
        struct llog_ctxt *ctxt;
        lnet_nid_t nid = 0;
        int err, rc = 0;
        struct obd_connect_data *ocd = NULL;
        ENTRY;

        generate_random_uuid(uuid);
        class_uuid_unparse(uuid, &mdc_uuid);

        nid = libcfs_str2nid(mdsnid);
        if (nid == LNET_NID_ANY) {
                CERROR("Can't parse NID %s\n", mdsnid);
                RETURN(-EINVAL);
        }

        lustre_cfg_bufs_reset(&bufs, NULL);
        lustre_cfg_bufs_set_string(&bufs, 1, peer);
        lcfg = lustre_cfg_new(LCFG_ADD_UUID, &bufs);
        lcfg->lcfg_nid = nid;
        rc = class_process_config(lcfg);
        lustre_cfg_free(lcfg);
        if (rc < 0)
                GOTO(out, rc);

        lustre_cfg_bufs_reset(&bufs, name);
        lustre_cfg_bufs_set_string(&bufs, 1, LUSTRE_MDC_NAME);//FIXME connect to mgc
        lustre_cfg_bufs_set_string(&bufs, 2, mdc_uuid.uuid);
        lcfg = lustre_cfg_new(LCFG_ATTACH, &bufs);
        rc = class_process_config(lcfg);
        lustre_cfg_free(lcfg);
        if (rc < 0)
                GOTO(out_del_uuid, rc);

        lustre_cfg_bufs_reset(&bufs, name);
        lustre_cfg_bufs_set_string(&bufs, 1, mdsname);
        lustre_cfg_bufs_set_string(&bufs, 2, peer);
        lcfg = lustre_cfg_new(LCFG_SETUP, &bufs);
        rc = class_process_config(lcfg);
        lustre_cfg_free(lcfg);
        if (rc < 0)
                GOTO(out_detach, rc);

        obd = class_name2obd(name);
        if (obd == NULL)
                GOTO(out_cleanup, rc = -EINVAL);

        OBD_ALLOC(ocd, sizeof(*ocd));
        if (ocd == NULL)
                GOTO(out_cleanup, rc = -ENOMEM);

        ocd->ocd_version = LUSTRE_VERSION_CODE;

        /* Disable initial recovery on this import */
        rc = obd_set_info(obd->obd_self_export,
                          strlen(KEY_INIT_RECOV), KEY_INIT_RECOV,
                          sizeof(allow_recov), &allow_recov);

        rc = obd_connect(&mdc_conn, obd, &mdc_uuid, ocd);
        if (rc) {
                CERROR("cannot connect to %s: rc = %d\n", mdsname, rc);
                GOTO(out_cleanup, rc);
        }

        exp = class_conn2export(&mdc_conn);

        ctxt = exp->exp_obd->obd_llog_ctxt[LLOG_CONFIG_REPL_CTXT];
        rc = class_config_parse_llog(ctxt, profile, cfg);
        if (rc) {
                CERROR("class_config_parse_llog failed: rc = %d\n", rc);
        }

        /* We don't so much care about errors in cleaning up the config llog
         * connection, as we have already read the config by this point. */
        err = obd_disconnect(exp);
        if (err)
                CERROR("obd_disconnect failed: rc = %d\n", err);

out_cleanup:
        if (ocd)
                OBD_FREE(ocd, sizeof(*ocd));

        lustre_cfg_bufs_reset(&bufs, name);
        lcfg = lustre_cfg_new(LCFG_CLEANUP, &bufs);
        err = class_process_config(lcfg);
        lustre_cfg_free(lcfg);
        if (err)
                CERROR("mdc_cleanup failed: rc = %d\n", err);

out_detach:
        lustre_cfg_bufs_reset(&bufs, name);
        lcfg = lustre_cfg_new(LCFG_DETACH, &bufs);
        err = class_process_config(lcfg);
        lustre_cfg_free(lcfg);
        if (err)
                CERROR("mdc_detach failed: rc = %d\n", err);

out_del_uuid:
        lustre_cfg_bufs_reset(&bufs, name);
        lustre_cfg_bufs_set_string(&bufs, 1, peer);
        lcfg = lustre_cfg_new(LCFG_DEL_UUID, &bufs);
        err = class_process_config(lcfg);
        if (err)
                CERROR("del MDC UUID failed: rc = %d\n", err);
        lustre_cfg_free(lcfg);
out:

        RETURN(rc);
}

/* parse host:/mdsname/profile string */
int ll_parse_mount_target(const char *target, char **mdsnid,
                          char **mdsname, char **profile)
{
        static char buf[256];
        char *s;

        buf[255] = 0;
        strncpy(buf, target, 255);

        if ((s = strchr(buf, ':'))) {
                *mdsnid = buf;
                *s = '\0';

                while (*++s == '/')
                        ;
                *mdsname = s;
                if ((s = strchr(*mdsname, '/'))) {
                        *s = '\0';
                        *profile = s + 1;
                        return 0;
                }
        }

        return -1;
}

/*
 * early liblustre init
 * called from C startup in catamount apps, before main()
 *
 * The following is a skeleton sysio startup sequence,
 * as implemented in C startup (skipping error handling).
 * In this framework none of these calls need be made here
 * or in the apps themselves.  The NAMESPACE_STRING specifying
 * the initial set of fs ops (creates, mounts, etc.) is passed
 * as an environment variable.
 *
 *      _sysio_init();
 *      _sysio_incore_init();
 *      _sysio_native_init();
 *      _sysio_lustre_init();
 *      _sysio_boot(NAMESPACE_STRING);
 *
 * the name _sysio_lustre_init() follows the naming convention
 * established in other fs drivers from libsysio:
 *  _sysio_incore_init(), _sysio_native_init()
 *
 * _sysio_lustre_init() must be called before _sysio_boot()
 * to enable libsysio's processing of namespace init strings containing
 * lustre filesystem operations
 */
int _sysio_lustre_init(void)
{
        int err;
        char *timeout = NULL;
        char *debug_mask = NULL;
        char *debug_subsys = NULL;
#ifndef INIT_SYSIO
        extern void __liblustre_cleanup_(void);
#endif

#if 0
        libcfs_debug = -1;
        libcfs_subsystem_debug = -1;
#endif

        liblustre_init_random();

        err = lllib_init();
        if (err) {
                perror("init llite driver");
                return err;
        }
        timeout = getenv("LIBLUSTRE_TIMEOUT");
        if (timeout) {
                obd_timeout = (unsigned int) strtol(timeout, NULL, 0);
                printf("LibLustre: set obd timeout as %u seconds\n",
                        obd_timeout);
        }

        /* debug masks */
        debug_mask = getenv("LIBLUSTRE_DEBUG_MASK");
        if (debug_mask)
                libcfs_debug = (unsigned int) strtol(debug_mask, NULL, 0);

        debug_subsys = getenv("LIBLUSTRE_DEBUG_SUBSYS");
        if (debug_subsys)
                libcfs_subsystem_debug =
                                (unsigned int) strtol(debug_subsys, NULL, 0);

#ifndef INIT_SYSIO
        (void)atexit(__liblustre_cleanup_);
#endif
        return err;
}

extern int _sysio_native_init();
extern unsigned int obd_timeout;

char *lustre_path = NULL;

void __liblustre_setup_(void)
{
        char *target = NULL;
        char *root_driver = "native";
        char *lustre_driver = "llite";
        char *root_path = "/";
        unsigned mntflgs = 0;
        int err;

        lustre_path = getenv("LIBLUSTRE_MOUNT_POINT");
        if (!lustre_path) {
                lustre_path = "/mnt/lustre";
        }

        /* mount target */
        target = getenv("LIBLUSTRE_MOUNT_TARGET");
        if (!target) {
                printf("LibLustre: no mount target specified\n");
                exit(1);
        }
        printf("LibLustre: mount point %s, target %s\n",
                lustre_path, target);


#ifdef INIT_SYSIO
        /* initialize libsysio & mount rootfs */
        if (_sysio_init()) {
                perror("init sysio");
                exit(1);
        }
        _sysio_native_init();

        err = _sysio_mount_root(root_path, root_driver, mntflgs, NULL);
        if (err) {
                fprintf(stderr, "sysio mount failed: %s\n", strerror(errno));
                exit(1);
        }

        if (_sysio_lustre_init())
                exit(1);
#endif /* INIT_SYSIO */

        err = mount(target, lustre_path, lustre_driver, mntflgs, NULL);
        if (err) {
                fprintf(stderr, "Lustre mount failed: %s\n", strerror(errno));
                exit(1);
        }
}

void __liblustre_cleanup_(void)
{
#ifndef INIT_SYSIO
        /* guard against being called multiple times */
        static int cleaned = 0;

        if (cleaned)
                return;
        cleaned++;
#endif

        /* user app might chdir to a lustre directory, and leave busy pnode
         * during finaly libsysio cleanup. here we chdir back to "/".
         * but it can't fix the situation that liblustre is mounted
         * at "/".
         */
        chdir("/");
#if 0
        umount(lustre_path);
#endif
        /* we can't call umount here, because libsysio will not cleanup
         * opening files for us. _sysio_shutdown() will cleanup fds at
         * first but which will also close the sockets we need for umount
         * liblutre. this dilema lead to another hack in
         * libsysio/src/file_hack.c FIXME
         */
#ifdef INIT_SYSIO
        _sysio_shutdown();
        cleanup_lib_portals();
        LNetFini();
#endif
}
