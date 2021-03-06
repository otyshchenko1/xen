/*
 * support.h: HVM support routines used by VT-x and SVM.
 *
 * Leendert van Doorn, leendert@watson.ibm.com
 * Copyright (c) 2005, International Business Machines Corporation.
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

#ifndef __ASM_X86_HVM_SUPPORT_H__
#define __ASM_X86_HVM_SUPPORT_H__

#include <xen/types.h>
#include <xen/sched.h>
#include <xen/hvm/save.h>
#include <asm/processor.h>

#define HVM_DELIVER_NO_ERROR_CODE  (~0U)

#ifndef NDEBUG
#define DBG_LEVEL_0                 (1 << 0)
#define DBG_LEVEL_1                 (1 << 1)
#define DBG_LEVEL_2                 (1 << 2)
#define DBG_LEVEL_3                 (1 << 3)
#define DBG_LEVEL_IO                (1 << 4)
#define DBG_LEVEL_VMMU              (1 << 5)
#define DBG_LEVEL_VLAPIC            (1 << 6)
#define DBG_LEVEL_VLAPIC_TIMER      (1 << 7)
#define DBG_LEVEL_VLAPIC_INTERRUPT  (1 << 8)
#define DBG_LEVEL_IOAPIC            (1 << 9)
#define DBG_LEVEL_HCALL             (1 << 10)
#define DBG_LEVEL_MSR               (1 << 11)

extern unsigned int opt_hvm_debug_level;
#define HVM_DBG_LOG(level, _f, _a...)                                         \
    do {                                                                      \
        if ( unlikely((level) & opt_hvm_debug_level) )                        \
            printk("[HVM:%d.%d] <%s> " _f "\n",                               \
                   current->domain->domain_id, current->vcpu_id, __func__,    \
                   ## _a);                                                    \
    } while (0)
#else
#define HVM_DBG_LOG(level, _f, _a...) do {} while (0)
#endif

extern unsigned long hvm_io_bitmap[];

enum hvm_copy_result {
    HVMCOPY_okay = 0,
    HVMCOPY_bad_gva_to_gfn,
    HVMCOPY_bad_gfn_to_mfn,
    HVMCOPY_unhandleable,
    HVMCOPY_gfn_paged_out,
    HVMCOPY_gfn_shared,
};

/*
 * Copy to/from a guest physical address.
 * Returns HVMCOPY_okay, else HVMCOPY_bad_gfn_to_mfn if the given physical
 * address range does not map entirely onto ordinary machine memory.
 */
enum hvm_copy_result hvm_copy_to_guest_phys(
    paddr_t paddr, void *buf, int size);
enum hvm_copy_result hvm_copy_from_guest_phys(
    void *buf, paddr_t paddr, int size);

/*
 * Copy to/from a guest virtual address. @pfec should include PFEC_user_mode
 * if emulating a user-mode access (CPL=3). All other flags in @pfec are
 * managed by the called function: it is therefore optional for the caller
 * to set them.
 * 
 * Returns:
 *  HVMCOPY_okay: Copy was entirely successful.
 *  HVMCOPY_bad_gfn_to_mfn: Some guest physical address did not map to
 *                          ordinary machine memory.
 *  HVMCOPY_bad_gva_to_gfn: Some guest virtual address did not have a valid
 *                          mapping to a guest physical address. In this case
 *                          a page fault exception is automatically queued
 *                          for injection into the current HVM VCPU.
 */
enum hvm_copy_result hvm_copy_to_guest_virt(
    unsigned long vaddr, void *buf, int size, uint32_t pfec);
enum hvm_copy_result hvm_copy_from_guest_virt(
    void *buf, unsigned long vaddr, int size, uint32_t pfec);
enum hvm_copy_result hvm_fetch_from_guest_virt(
    void *buf, unsigned long vaddr, int size, uint32_t pfec);

/*
 * As above (copy to/from a guest virtual address), but no fault is generated
 * when HVMCOPY_bad_gva_to_gfn is returned.
 */
enum hvm_copy_result hvm_copy_to_guest_virt_nofault(
    unsigned long vaddr, void *buf, int size, uint32_t pfec);
enum hvm_copy_result hvm_copy_from_guest_virt_nofault(
    void *buf, unsigned long vaddr, int size, uint32_t pfec);
enum hvm_copy_result hvm_fetch_from_guest_virt_nofault(
    void *buf, unsigned long vaddr, int size, uint32_t pfec);

#define HVM_HCALL_completed  0 /* hypercall completed - no further action */
#define HVM_HCALL_preempted  1 /* hypercall preempted - re-execute VMCALL */
#define HVM_HCALL_invalidate 2 /* invalidate ioemu-dm memory cache        */
int hvm_do_hypercall(struct cpu_user_regs *pregs);

void hvm_hlt(unsigned long rflags);
void hvm_triple_fault(void);

void hvm_rdtsc_intercept(struct cpu_user_regs *regs);

int __must_check hvm_handle_xsetbv(u32 index, u64 new_bv);

void hvm_shadow_handle_cd(struct vcpu *v, unsigned long value);

/* These functions all return X86EMUL return codes. */
int hvm_set_efer(uint64_t value);
int hvm_set_cr0(unsigned long value, bool_t may_defer);
int hvm_set_cr3(unsigned long value, bool_t may_defer);
int hvm_set_cr4(unsigned long value, bool_t may_defer);
int hvm_msr_read_intercept(unsigned int msr, uint64_t *msr_content);
int hvm_msr_write_intercept(
    unsigned int msr, uint64_t msr_content, bool_t may_defer);
int hvm_mov_to_cr(unsigned int cr, unsigned int gpr);
int hvm_mov_from_cr(unsigned int cr, unsigned int gpr);
void hvm_ud_intercept(struct cpu_user_regs *);

#endif /* __ASM_X86_HVM_SUPPORT_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
