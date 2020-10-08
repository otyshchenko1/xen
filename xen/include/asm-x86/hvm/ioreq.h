/*
 * hvm.h: Hardware virtual machine assist interface definitions.
 *
 * Copyright (c) 2016 Citrix Systems Inc.
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

#ifndef __ASM_X86_HVM_IOREQ_H__
#define __ASM_X86_HVM_IOREQ_H__

#include <xen/ioreq.h>

#include <asm/hvm/emulate.h>
#include <asm/hvm/vmx/vmx.h>

#include <public/hvm/params.h>

static inline bool arch_hvm_io_completion(enum hvm_io_completion io_completion)
{
    switch ( io_completion )
    {
    case HVMIO_realmode_completion:
    {
        struct hvm_emulate_ctxt ctxt;

        hvm_emulate_init_once(&ctxt, NULL, guest_cpu_user_regs());
        vmx_realmode_emulate_one(&ctxt);
        hvm_emulate_writeback(&ctxt);

        break;
    }

    default:
        ASSERT_UNREACHABLE();
        break;
    }

    return true;
}

/* Called when target domain is paused */
static inline void arch_hvm_destroy_ioreq_server(struct hvm_ioreq_server *s)
{
    p2m_set_ioreq_server(s->target, 0, s);
}

/*
 * Map or unmap an ioreq server to specific memory type. For now, only
 * HVMMEM_ioreq_server is supported, and in the future new types can be
 * introduced, e.g. HVMMEM_ioreq_serverX mapped to ioreq server X. And
 * currently, only write operations are to be forwarded to an ioreq server.
 * Support for the emulation of read operations can be added when an ioreq
 * server has such requirement in the future.
 */
static inline int hvm_map_mem_type_to_ioreq_server(struct domain *d,
                                                   ioservid_t id,
                                                   uint32_t type,
                                                   uint32_t flags)
{
    struct hvm_ioreq_server *s;
    int rc;

    if ( type != HVMMEM_ioreq_server )
        return -EINVAL;

    if ( flags & ~XEN_DMOP_IOREQ_MEM_ACCESS_WRITE )
        return -EINVAL;

    spin_lock_recursive(&d->arch.hvm.ioreq_server.lock);

    s = get_ioreq_server(d, id);

    rc = -ENOENT;
    if ( !s )
        goto out;

    rc = -EPERM;
    if ( s->emulator != current->domain )
        goto out;

    rc = p2m_set_ioreq_server(d, flags, s);

 out:
    spin_unlock_recursive(&d->arch.hvm.ioreq_server.lock);

    if ( rc == 0 && flags == 0 )
    {
        struct p2m_domain *p2m = p2m_get_hostp2m(d);

        if ( read_atomic(&p2m->ioreq.entry_count) )
            p2m_change_entry_type_global(d, p2m_ioreq_server, p2m_ram_rw);
    }

    return rc;
}

static inline int hvm_ioreq_server_get_type_addr(const struct domain *d,
                                                 const ioreq_t *p,
                                                 uint8_t *type,
                                                 uint64_t *addr)
{
    uint32_t cf8 = d->arch.hvm.pci_cf8;

    if ( p->type != IOREQ_TYPE_COPY && p->type != IOREQ_TYPE_PIO )
        return -EINVAL;

    if ( p->type == IOREQ_TYPE_PIO &&
         (p->addr & ~3) == 0xcfc &&
         CF8_ENABLED(cf8) )
    {
        uint32_t x86_fam;
        pci_sbdf_t sbdf;
        unsigned int reg;

        reg = hvm_pci_decode_addr(cf8, p->addr, &sbdf);

        /* PCI config data cycle */
        *type = XEN_DMOP_IO_RANGE_PCI;
        *addr = ((uint64_t)sbdf.sbdf << 32) | reg;
        /* AMD extended configuration space access? */
        if ( CF8_ADDR_HI(cf8) &&
             d->arch.cpuid->x86_vendor == X86_VENDOR_AMD &&
             (x86_fam = get_cpu_family(
                 d->arch.cpuid->basic.raw_fms, NULL, NULL)) >= 0x10 &&
             x86_fam < 0x17 )
        {
            uint64_t msr_val;

            if ( !rdmsr_safe(MSR_AMD64_NB_CFG, msr_val) &&
                 (msr_val & (1ULL << AMD64_NB_CFG_CF8_EXT_ENABLE_BIT)) )
                *addr |= CF8_ADDR_HI(cf8);
        }
    }
    else
    {
        *type = (p->type == IOREQ_TYPE_PIO) ?
                 XEN_DMOP_IO_RANGE_PORT : XEN_DMOP_IO_RANGE_MEMORY;
        *addr = p->addr;
    }

    return 0;
}

static inline int hvm_access_cf8(
    int dir, unsigned int port, unsigned int bytes, uint32_t *val)
{
    struct domain *d = current->domain;

    if ( dir == IOREQ_WRITE && bytes == 4 )
        d->arch.hvm.pci_cf8 = *val;

    /* We always need to fall through to the catch all emulator */
    return X86EMUL_UNHANDLEABLE;
}

static inline void arch_hvm_ioreq_init(struct domain *d)
{
    register_portio_handler(d, 0xcf8, 4, hvm_access_cf8);
}

static inline bool arch_hvm_ioreq_destroy(struct domain *d)
{
    if ( !relocate_portio_handler(d, 0xcf8, 0xcf8, 4) )
        return false;

    return true;
}

#define IOREQ_STATUS_HANDLED     X86EMUL_OKAY
#define IOREQ_STATUS_UNHANDLED   X86EMUL_UNHANDLEABLE
#define IOREQ_STATUS_RETRY       X86EMUL_RETRY

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
