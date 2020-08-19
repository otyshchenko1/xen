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

#include <xen/ioreq.h>

#ifdef CONFIG_IOREQ_SERVER
enum io_state handle_ioserv(struct cpu_user_regs *regs, struct vcpu *v);
enum io_state try_fwd_ioserv(struct cpu_user_regs *regs,
                             struct vcpu *v, mmio_info_t *info);
#else
static inline enum io_state handle_ioserv(struct cpu_user_regs *regs,
                                          struct vcpu *v)
{
    return IO_UNHANDLED;
}

static inline enum io_state try_fwd_ioserv(struct cpu_user_regs *regs,
                                           struct vcpu *v, mmio_info_t *info)
{
    return IO_UNHANDLED;
}
#endif

bool ioreq_complete_mmio(void);

static inline bool handle_pio(uint16_t port, unsigned int size, int dir)
{
    /*
     * TODO: For Arm64, the main user will be PCI. So this should be
     * implemented when we add support for vPCI.
     */
    ASSERT_UNREACHABLE();
    return true;
}

static inline void msix_write_completion(struct vcpu *v)
{
}

static inline bool arch_vcpu_ioreq_completion(enum io_completion io_completion)
{
    ASSERT_UNREACHABLE();
    return true;
}

/*
 * The "legacy" mechanism of mapping magic pages for the IOREQ servers
 * is x86 specific, so the following hooks don't need to be implemented on Arm:
 * - arch_ioreq_server_map_pages
 * - arch_ioreq_server_unmap_pages
 * - arch_ioreq_server_enable
 * - arch_ioreq_server_disable
 */
static inline int arch_ioreq_server_map_pages(struct ioreq_server *s)
{
    return -EOPNOTSUPP;
}

static inline void arch_ioreq_server_unmap_pages(struct ioreq_server *s)
{
}

static inline void arch_ioreq_server_enable(struct ioreq_server *s)
{
}

static inline void arch_ioreq_server_disable(struct ioreq_server *s)
{
}

static inline void arch_ioreq_server_destroy(struct ioreq_server *s)
{
}

static inline int arch_ioreq_server_map_mem_type(struct domain *d,
                                                 struct ioreq_server *s,
                                                 uint32_t flags)
{
    return -EOPNOTSUPP;
}

static inline bool arch_ioreq_server_destroy_all(struct domain *d)
{
    return true;
}

static inline int arch_ioreq_server_get_type_addr(const struct domain *d,
                                                  const ioreq_t *p,
                                                  uint8_t *type,
                                                  uint64_t *addr)
{
    if ( p->type != IOREQ_TYPE_COPY && p->type != IOREQ_TYPE_PIO )
        return -EINVAL;

    *type = (p->type == IOREQ_TYPE_PIO) ?
             XEN_DMOP_IO_RANGE_PORT : XEN_DMOP_IO_RANGE_MEMORY;
    *addr = p->addr;

    return 0;
}

static inline void arch_ioreq_domain_init(struct domain *d)
{
}

#define IOREQ_STATUS_HANDLED     IO_HANDLED
#define IOREQ_STATUS_UNHANDLED   IO_UNHANDLED
#define IOREQ_STATUS_RETRY       IO_RETRY

#endif /* __ASM_ARM_HVM_IOREQ_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
