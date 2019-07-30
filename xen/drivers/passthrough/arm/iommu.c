/*
 * Generic IOMMU framework via the device tree
 *
 * Julien Grall <julien.grall@linaro.org>
 * Copyright (c) 2014 Linaro Limited.
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

#include <xen/lib.h>
#include <xen/iommu.h>
#include <xen/device_tree.h>
#include <asm/device.h>

/*
 * Used to keep track of devices for which driver requested deferred probing
 * (returns -EAGAIN).
 */
static LIST_HEAD(deferred_probe_list);

static const struct iommu_ops *iommu_ops;

const struct iommu_ops *iommu_get_ops(void)
{
    return iommu_ops;
}

void __init iommu_set_ops(const struct iommu_ops *ops)
{
    BUG_ON(ops == NULL);

    if ( iommu_ops && iommu_ops != ops )
    {
        printk("WARNING: Cannot set IOMMU ops, already set to a different value\n");
        return;
    }

    iommu_ops = ops;
}

int __init iommu_hardware_setup(void)
{
    struct dt_device_node *np, *tmp;
    int rc;
    unsigned int num_iommus = 0;

    dt_for_each_device_node(dt_host, np)
    {
        rc = device_init(np, DEVICE_IOMMU, NULL);
        if ( !rc )
            num_iommus++;
        else if (rc == -EAGAIN)
            /*
             * Driver requested deferred probing, so add this device to
             * the deferred list for further processing.
             */
            list_add(&np->deferred_probe, &deferred_probe_list);
    }

    /*
     * Process devices in the deferred list if at least one successfully
     * probed device is present.
     */
    while ( !list_empty(&deferred_probe_list) && num_iommus )
    {
        list_for_each_entry_safe ( np, tmp, &deferred_probe_list,
                                   deferred_probe )
        {
            rc = device_init(np, DEVICE_IOMMU, NULL);
            if ( !rc )
                num_iommus++;
            if ( rc != -EAGAIN )
                /*
                 * Driver didn't request deferred probing, so remove this device
                 * from the deferred list.
                 */
                list_del_init(&np->deferred_probe);
        }
    }

    return ( num_iommus > 0 ) ? 0 : -ENODEV;
}

void __hwdom_init arch_iommu_check_autotranslated_hwdom(struct domain *d)
{
    /* ARM doesn't require specific check for hwdom */
    return;
}

int arch_iommu_domain_init(struct domain *d)
{
    return iommu_dt_domain_init(d);
}

void arch_iommu_domain_destroy(struct domain *d)
{
}

int arch_iommu_populate_page_table(struct domain *d)
{
    /* The IOMMU shares the p2m with the CPU */
    return -ENOSYS;
}

void __hwdom_init arch_iommu_hwdom_init(struct domain *d)
{
}
