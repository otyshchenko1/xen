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

bool arch_vcpu_ioreq_completion(enum hvm_io_completion io_completion);
int arch_ioreq_server_map_pages(struct ioreq_server *s);
void arch_ioreq_server_unmap_pages(struct ioreq_server *s);
void arch_ioreq_server_enable(struct ioreq_server *s);
void arch_ioreq_server_disable(struct ioreq_server *s);
void arch_ioreq_server_destroy(struct ioreq_server *s);
int arch_ioreq_server_map_mem_type(struct domain *d,
                                   struct ioreq_server *s,
                                   uint32_t flags);
bool arch_ioreq_server_destroy_all(struct domain *d);
int arch_ioreq_server_get_type_addr(const struct domain *d,
                                    const ioreq_t *p,
                                    uint8_t *type,
                                    uint64_t *addr);
void arch_ioreq_domain_init(struct domain *d);

bool ioreq_complete_mmio(void);

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
