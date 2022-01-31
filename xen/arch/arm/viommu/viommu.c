/*
 * Copyright (C) 2022, EPAM Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/domain_page.h>
#include <xen/err.h>
#include <xen/keyhandler.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/types.h>

#include <asm/mmio.h>
#include <asm/viommu/viommu.h>

#include <asm/viommu/kvm/kvm.h>
#include <asm/viommu/kvm/virtio-iommu.h>

extern struct iommu_properties iommu_props;

static int viommu_mmio_read(struct vcpu *v, mmio_info_t *info,
                            register_t *r, void *priv)
{
    struct hsr_dabt dabt = info->dabt;
    struct mmio_mapping *mmio = v->domain->arch.viommu.mmio;
    uint8_t data[8] = { 0 };

    if ( !mmio )
    {
        *r = 0;
        return 0;
    }

    mmio->mmio_fn(v, info->gpa, data, 1 << dabt.size, 0, mmio->ptr);
    *r = *(uint64_t *)&data;

    return 1;
}

static int viommu_mmio_write(struct vcpu *v, mmio_info_t *info,
                             register_t r, void *priv)
{
    struct hsr_dabt dabt = info->dabt;
    struct mmio_mapping *mmio = v->domain->arch.viommu.mmio;
    uint8_t data[8] = { 0 };

    if ( !mmio )
        return 0;

    *(uint64_t *)&data = r;
    mmio->mmio_fn(v, info->gpa, data, 1 << dabt.size, 1, mmio->ptr);

    return 1;
}

static const struct mmio_handler_ops viommu_mmio_handler = {
    .read  = viommu_mmio_read,
    .write = viommu_mmio_write,
};

int viommu_register_mmio(struct domain *d, uint64_t addr, uint32_t size,
                         mmio_callback_t mmio_fn, void *ptr)
{
    struct mmio_mapping *mmio = d->arch.viommu.mmio;

    if ( mmio )
        return -EEXIST;

    mmio = xmalloc(struct mmio_mapping);
    if ( !mmio )
        return -ENOMEM;

    *mmio = (struct mmio_mapping) {
        .mmio_fn = mmio_fn,
        .ptr     = ptr,
        .addr    = addr,
        .size    = size,
    };

    register_mmio_handler(d, &viommu_mmio_handler, addr, size, NULL);

    d->arch.viommu.mmio = mmio;

    return 0;
}

void viommu_deregister_mmio(struct domain *d, uint64_t addr)
{
    struct mmio_mapping *mmio = d->arch.viommu.mmio;

    if ( !mmio )
        return;

    xfree(mmio);
    d->arch.viommu.mmio = NULL;
}

/*
 * XXX Add ability to map bigger than PAGE_SIZE ranges,
 * look at access_guest_memory_by_ipa()
 */
void *viommu_map_guest_range(struct domain *d, uint64_t addr, uint64_t size)
{
    struct page_info *page;
    uint64_t offset = addr & ~PAGE_MASK;
    void *vaddr;

    BUG_ON(offset + size > PAGE_SIZE);

    page = get_page_from_gfn(d, addr >> PAGE_SHIFT, NULL, P2M_ALLOC);
    if ( !page )
        return NULL;

    if ( !get_page_type(page, PGT_writable_page) )
    {
        put_page(page);
        return NULL;
    }

    vaddr = __map_domain_page_global(page);
    if ( !vaddr )
    {
        put_page_and_type(page);
        return NULL;
    }

    return vaddr + offset;
}

void viommu_unmap_guest_range(void *virt, uint64_t size)
{
    struct page_info *page;

    if ( !virt )
        return;

    virt = (void *)((unsigned long)virt & PAGE_MASK);
    page = mfn_to_page(domain_page_map_to_mfn(virt));

    unmap_domain_page_global(virt);
    put_page_and_type(page);
}

void viommu_irq_trigger(struct domain *d, uint32_t irq)
{
    if ( (irq < NR_LOCAL_IRQS) || (irq >= vgic_num_irqs(d)) ||
         test_bit(irq, d->arch.vgic.allocated_irqs) )
        return;

    vgic_inject_irq(d, NULL, irq, 1);
}

int domain_viommu_init(struct domain *d, struct xen_arch_domainconfig *config)
{
    if ( !config->viommu_enable )
        return 0;

    d->arch.viommu.priv = viommu_register(d, &iommu_props, config->viommu_base,
                                          config->viommu_irq);
    if (IS_ERR(d->arch.viommu.priv)) {
        int ret = PTR_ERR(d->arch.viommu.priv);
        pr_err("cannot instantiate %s",iommu_props.name);
        d->arch.viommu.priv = NULL;
        return ret;
    }

    return 0;
}

void domain_viommu_free(struct domain *d)
{
    if ( !d->arch.viommu.priv )
        return;

    viommu_unregister(d, d->arch.viommu.priv);
    d->arch.viommu.priv = NULL;
}

int viommu_relinquish_resources(struct domain *d)
{
    domain_viommu_free(d);

    return 0;
}

static void viommu_usage_print_all(unsigned char key)
{
    struct iommu_debug_params params;

    printk("%s key '%c' pressed\n", __func__, key);

    params.selector[0] = IOMMU_DEBUG_SELECTOR_INVALID;
    params.selector[1] = IOMMU_DEBUG_SELECTOR_INVALID;

    switch ( key )
    {
    case '1':
        params.action = IOMMU_DEBUG_LIST;
        break;

    case '2':
        params.action = IOMMU_DEBUG_STATS;
        break;

    case '3':
        params.action = IOMMU_DEBUG_DUMP;
        break;

    case '4':
        params.action = IOMMU_DEBUG_SET_PRINT;
        params.debug_level = IOMMU_DEBUG_L_INFO;
        break;

    case '5':
        params.action = IOMMU_DEBUG_SET_PRINT;
        params.debug_level = IOMMU_DEBUG_L_VERBOSE;
        break;

    case '6':
        params.action = IOMMU_DEBUG_SET_PRINT;
        params.debug_level = IOMMU_DEBUG_L_DISABLED;
        break;

    case '7':
        params.action = IOMMU_DEBUG_FAULT;
        params.fault.num = 1;
        params.fault.reason = IOMMU_FAULT_MAPPING;
        params.fault.endpoint = 0;
        params.fault.addr = 0x100000000;
        break;
    }

    viommu_debug(NULL, 0, &params);
}

static int __init viommu_usage_init(void)
{
    register_keyhandler('1', viommu_usage_print_all, "viommu: list iommus and domains", 1);
    register_keyhandler('2', viommu_usage_print_all, "viommu: display statistics", 1);
    register_keyhandler('3', viommu_usage_print_all, "viommu: dump mappings", 1);
    register_keyhandler('4', viommu_usage_print_all, "viommu: enable debug print", 1);
    register_keyhandler('5', viommu_usage_print_all, "viommu: enable *verbose* debug print", 1);
    register_keyhandler('6', viommu_usage_print_all, "viommu: disable debug print", 1);
    register_keyhandler('7', viommu_usage_print_all, "viommu: inject a fault", 1);
    return 0;
}
__initcall(viommu_usage_init);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */

