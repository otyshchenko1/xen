/*
 * arm/ioreq.c: hardware virtual machine I/O emulation
 *
 * Copyright (c) 2019 Arm ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/ctype.h>
#include <xen/hvm/ioreq.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/trace.h>
#include <xen/sched.h>
#include <xen/irq.h>
#include <xen/softirq.h>
#include <xen/domain.h>
#include <xen/domain_page.h>
#include <xen/event.h>
#include <xen/paging.h>
#include <xen/vpci.h>

#include <public/hvm/dm_op.h>
#include <public/hvm/ioreq.h>

bool handle_mmio(void)
{
    struct vcpu *v = current;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    const union hsr hsr = { .bits = regs->hsr };
    const struct hsr_dabt dabt = hsr.dabt;
    /* Code is similar to handle_read */
    uint8_t size = (1 << dabt.size) * 8;
    register_t r = v->arch.hvm.hvm_io.io_req.data;

    /* We should only be here on Guest Data Abort */
    ASSERT(dabt.ec == HSR_EC_DATA_ABORT_LOWER_EL);

    /* We are done with the IO */
    /* XXX: Is it the right place? */
    v->arch.hvm.hvm_io.io_req.state = STATE_IOREQ_NONE;

    /* XXX: Do we need to take care of write here ? */
    if ( dabt.write )
        return true;

    /*
     * Sign extend if required.
     * Note that we expect the read handler to have zeroed the bits
     * outside the requested access size.
     */
    if ( dabt.sign && (r & (1UL << (size - 1))) )
    {
        /*
         * We are relying on register_t using the same as
         * an unsigned long in order to keep the 32-bit assembly
         * code smaller.
         */
        BUILD_BUG_ON(sizeof(register_t) != sizeof(unsigned long));
        r |= (~0UL) << size;
    }

    set_user_reg(regs, dabt.reg, r);

    return true;
}

/* Ask ioemu mapcache to invalidate mappings. */
void send_invalidate_req(void)
{
    ioreq_t p = {
        .type = IOREQ_TYPE_INVALIDATE,
        .size = 4,
        .dir = IOREQ_WRITE,
        .data = ~0UL, /* flush all */
    };

    if ( hvm_broadcast_ioreq(&p, false) != 0 )
        gprintk(XENLOG_ERR, "Unsuccessful map-cache invalidate\n");
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
