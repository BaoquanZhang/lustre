/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002, 2003 Cluster File Systems, Inc.
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

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LOV

#ifdef __KERNEL__
#include <asm/div64.h>
#else
#include <liblustre.h>
#endif

#include <linux/obd_class.h>
#include <linux/obd_lov.h>

#include "lov_internal.h"

/* compute object size given "stripeno" and the ost size */
obd_size lov_stripe_size(struct lov_stripe_md *lsm, obd_size ost_size,
                         int stripeno)
{
        unsigned long ssize  = lsm->lsm_stripe_size;
        unsigned long swidth = ssize * lsm->lsm_stripe_count;
        unsigned long stripe_size;
        obd_size lov_size;
        ENTRY;

        if (ost_size == 0)
                RETURN(0);

        /* do_div(a, b) returns a % b, and a = a / b */
        stripe_size = do_div(ost_size, ssize);
        if (stripe_size)
                lov_size = ost_size * swidth + stripeno * ssize + stripe_size;
        else
                lov_size = (ost_size - 1) * swidth + (stripeno + 1) * ssize;

        RETURN(lov_size);
}

/* we have an offset in file backed by an lov and want to find out where
 * that offset lands in our given stripe of the file.  for the easy
 * case where the offset is within the stripe, we just have to scale the
 * offset down to make it relative to the stripe instead of the lov.
 *
 * the harder case is what to do when the offset doesn't intersect the
 * stripe.  callers will want start offsets clamped ahead to the start
 * of the nearest stripe in the file.  end offsets similarly clamped to the
 * nearest ending byte of a stripe in the file:
 *
 * all this function does is move offsets to the nearest region of the
 * stripe, and it does its work "mod" the full length of all the stripes.
 * consider a file with 3 stripes:
 *
 *             S                                              E
 * ---------------------------------------------------------------------
 * |    0    |     1     |     2     |    0    |     1     |     2     |
 * ---------------------------------------------------------------------
 *
 * to find stripe 1's offsets for S and E, it divides by the full stripe
 * width and does its math in the context of a single set of stripes:
 *
 *             S         E
 * -----------------------------------
 * |    0    |     1     |     2     |
 * -----------------------------------
 *
 * it'll notice that E is outside stripe 1 and clamp it to the end of the
 * stripe, then multiply it back out by lov_off to give the real offsets in
 * the stripe:
 *
 *   S                   E
 * ---------------------------------------------------------------------
 * |    1    |     1     |     1     |    1    |     1     |     1     |
 * ---------------------------------------------------------------------
 *
 * it would have done similarly and pulled S forward to the start of a 1
 * stripe if, say, S had landed in a 0 stripe.
 *
 * this rounding isn't always correct.  consider an E lov offset that lands
 * on a 0 stripe, the "mod stripe width" math will pull it forward to the
 * start of a 1 stripe, when in fact it wanted to be rounded back to the end
 * of a previous 1 stripe.  this logic is handled by callers and this is why:
 *
 * this function returns < 0 when the offset was "before" the stripe and
 * was moved forward to the start of the stripe in question;  0 when it
 * falls in the stripe and no shifting was done; > 0 when the offset
 * was outside the stripe and was pulled back to its final byte. */
int lov_stripe_offset(struct lov_stripe_md *lsm, obd_off lov_off,
                      int stripeno, obd_off *obd_off)
{
        unsigned long ssize  = lsm->lsm_stripe_size;
        unsigned long swidth = ssize * lsm->lsm_stripe_count;
        unsigned long stripe_off, this_stripe;
        int ret = 0;

        if (lov_off == OBD_OBJECT_EOF) {
                *obd_off = OBD_OBJECT_EOF;
                return 0;
        }

        /* do_div(a, b) returns a % b, and a = a / b */
        stripe_off = do_div(lov_off, swidth);

        this_stripe = stripeno * ssize;
        if (stripe_off < this_stripe) {
                stripe_off = 0;
                ret = -1;
        } else {
                stripe_off -= this_stripe;

                if (stripe_off >= ssize) {
                        stripe_off = ssize;
                        ret = 1;
                }
        }

        *obd_off = lov_off * ssize + stripe_off;
        return ret;
}

/* Given a whole-file size and a stripe number, give the file size which
 * corresponds to the individual object of that stripe.
 *
 * This behaves basically in the same was as lov_stripe_offset, except that
 * file sizes falling before the beginning of a stripe are clamped to the end
 * of the previous stripe, not the beginning of the next:
 *
 *                                               S
 * ---------------------------------------------------------------------
 * |    0    |     1     |     2     |    0    |     1     |     2     |
 * ---------------------------------------------------------------------
 *
 * if clamped to stripe 2 becomes:
 *
 *                                   S
 * ---------------------------------------------------------------------
 * |    0    |     1     |     2     |    0    |     1     |     2     |
 * ---------------------------------------------------------------------
 */
obd_off lov_size_to_stripe(struct lov_stripe_md *lsm, obd_off file_size,
                           int stripeno)
{
        unsigned long ssize  = lsm->lsm_stripe_size;
        unsigned long swidth = ssize * lsm->lsm_stripe_count;
        unsigned long stripe_off, this_stripe;

        if (file_size == OBD_OBJECT_EOF)
                return OBD_OBJECT_EOF;

        /* do_div(a, b) returns a % b, and a = a / b */
        stripe_off = do_div(file_size, swidth);

        this_stripe = stripeno * ssize;
        if (stripe_off < this_stripe) {
                /* Move to end of previous stripe, or zero */
                if (file_size > 0) {
                        file_size--;
                        stripe_off = ssize;
                } else {
                        stripe_off = 0;
                }
        } else {
                stripe_off -= this_stripe;

                if (stripe_off >= ssize) {
                        /* Clamp to end of this stripe */
                        stripe_off = ssize;
                }
        }

        return (file_size * ssize + stripe_off);
}

/* given an extent in an lov and a stripe, calculate the extent of the stripe
 * that is contained within the lov extent.  this returns true if the given
 * stripe does intersect with the lov extent. */
int lov_stripe_intersects(struct lov_stripe_md *lsm, int stripeno,
                          obd_off start, obd_off end,
                          obd_off *obd_start, obd_off *obd_end)
{
        int start_side, end_side;

        start_side = lov_stripe_offset(lsm, start, stripeno, obd_start);
        end_side = lov_stripe_offset(lsm, end, stripeno, obd_end);

        CDEBUG(D_INODE, "["LPU64"->"LPU64"] -> [(%d) "LPU64"->"LPU64" (%d)]\n",
               start, end, start_side, *obd_start, *obd_end, end_side);

        /* this stripe doesn't intersect the file extent when neither
         * start or the end intersected the stripe and obd_start and
         * obd_end got rounded up to the save value. */
        if (start_side != 0 && end_side != 0 && *obd_start == *obd_end)
                return 0;

        /* as mentioned in the lov_stripe_offset commentary, end
         * might have been shifted in the wrong direction.  This
         * happens when an end offset is before the stripe when viewed
         * through the "mod stripe size" math. we detect it being shifted
         * in the wrong direction and touch it up.
         * interestingly, this can't underflow since end must be > start
         * if we passed through the previous check.
         * (should we assert for that somewhere?) */
        if (end_side != 0)
                (*obd_end)--;

        return 1;
}

/* compute which stripe number "lov_off" will be written into */
int lov_stripe_number(struct lov_stripe_md *lsm, obd_off lov_off)
{
        unsigned long ssize  = lsm->lsm_stripe_size;
        unsigned long swidth = ssize * lsm->lsm_stripe_count;
        unsigned long stripe_off;

        stripe_off = do_div(lov_off, swidth);

        return stripe_off / ssize;
}
