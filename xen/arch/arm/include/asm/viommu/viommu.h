/*
 * xen/arch/arm/include/asm/viommu.h
 *
 * Copyright (C) 2022, EPAM Systems.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef __ARCH_ARM_VIOMMU_H
#define __ARCH_ARM_VIOMMU_H

struct viommu {
    struct mmio_mapping *mmio;
    void *priv;
};

#define domain_has_viommu(d)    ((d)->arch.viommu.priv != NULL)

typedef void (*mmio_callback_t)(struct vcpu *vcpu, uint64_t addr, uint8_t *data,
                                uint32_t size, uint8_t is_write, void *ptr);

struct mmio_mapping {
    mmio_callback_t mmio_fn;
    void *ptr;
    uint64_t addr;
    uint32_t size;
};

#ifdef CONFIG_VIRTIO_IOMMU
int domain_viommu_init(struct domain *d, struct xen_arch_domainconfig *config);
void domain_viommu_free(struct domain *d);
int viommu_relinquish_resources(struct domain *d);
void *viommu_map_guest_range(struct domain *d, uint64_t addr, uint64_t size);
void viommu_unmap_guest_range(void *virt, uint64_t size);
void viommu_irq_trigger(struct domain *d, uint32_t irq);
int viommu_register_mmio(struct domain *d, uint64_t addr, uint32_t size,
                         mmio_callback_t mmio_fn, void *ptr);
void viommu_deregister_mmio(struct domain *d, uint64_t addr);
#else
static inline int domain_viommu_init(struct domain *d,
                                     struct xen_arch_domainconfig *config)
{
    return 0;
}
static inline void domain_viommu_free(struct domain *d) { }
static inline int viommu_relinquish_resources(struct domain *d)
{
    return 0;
}
static inline void *viommu_map_guest_range(struct domain *d, uint64_t addr,
                                          uint64_t size)
{
    return NULL;
}
static inline void viommu_unmap_guest_range(void *virt, uint64_t size) {}
static inline void viommu_irq_trigger(struct domain *d, uint32_t irq) {}
static inline int viommu_register_mmio(struct domain *d, uint64_t addr,
                                       uint32_t size, mmio_callback_t mmio_fn,
                                       void *ptr)
{
    return -EINVAL;
}
static inline void viommu_deregister_mmio(struct domain *d, uint64_t addr) {}
#endif

#endif /* __ARCH_ARM_VIOMMU_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
