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

#include <asm/hvm/emulate.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/vmx/vmx.h>

int hvm_map_mem_type_to_ioreq_server(struct domain *d, ioservid_t id,
                                     uint32_t type, uint32_t flags);

int arch_hvm_destroy_ioreq_server(struct hvm_ioreq_server *s);

bool arch_handle_hvm_io_completion(enum hvm_io_completion io_completion);

int hvm_get_ioreq_server_range_type(struct domain *d,
                                    ioreq_t *p,
                                    uint8_t *type,
                                    uint64_t *addr);

void arch_hvm_ioreq_init(struct domain *d);
void arch_hvm_ioreq_destroy(struct domain *d);

#define IOREQ_IO_HANDLED     X86EMUL_OKAY
#define IOREQ_IO_UNHANDLED   X86EMUL_UNHANDLEABLE
#define IOREQ_IO_RETRY       X86EMUL_RETRY

#define ioreq_handle_complete_mmio   handle_mmio

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
