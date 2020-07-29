/*
 * hvm.h: Hardware virtual machine assist interface definitions.
 *
 * Copyright (c) 2016 Citrix Systems Inc.
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

#ifndef __ASM_ARM_HVM_IOREQ_H__
#define __ASM_ARM_HVM_IOREQ_H__

#include <public/hvm/ioreq.h>
#include <public/hvm/dm_op.h>

#define has_vpci(d) (false)

bool handle_mmio(void);

static inline bool handle_pio(uint16_t port, unsigned int size, int dir)
{
    /* XXX */
    BUG();
    return true;
}

static inline paddr_t hvm_mmio_first_byte(const ioreq_t *p)
{
    return p->addr;
}

static inline paddr_t hvm_mmio_last_byte(const ioreq_t *p)
{
    unsigned long size = p->size;

    return p->addr + size - 1;
}

struct hvm_ioreq_server;

static inline int p2m_set_ioreq_server(struct domain *d,
                                       unsigned int flags,
                                       struct hvm_ioreq_server *s)
{
    return -EOPNOTSUPP;
}

static inline void msix_write_completion(struct vcpu *v)
{
}

static inline void handle_realmode_completion(void)
{
    ASSERT_UNREACHABLE();
}

static inline void paging_mark_pfn_dirty(struct domain *d, pfn_t pfn)
{
}

static inline void hvm_get_ioreq_server_range_type(struct domain *d,
                                                   ioreq_t *p,
                                                   uint8_t *type,
                                                   uint64_t *addr)
{
    *type = (p->type == IOREQ_TYPE_PIO) ?
             XEN_DMOP_IO_RANGE_PORT : XEN_DMOP_IO_RANGE_MEMORY;
    *addr = p->addr;
}

static inline void arch_hvm_ioreq_init(struct domain *d)
{
}

static inline void arch_hvm_ioreq_destroy(struct domain *d)
{
}

#define IOREQ_IO_HANDLED     IO_HANDLED
#define IOREQ_IO_UNHANDLED   IO_UNHANDLED
#define IOREQ_IO_RETRY       IO_RETRY

#endif /* __ASM_X86_HVM_IOREQ_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
