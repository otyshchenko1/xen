/*
 * xen/drivers/passthrough/arm/iommu_fwspec.c
 *
 * Contains functions to maintain per-device firmware data
 *
 * Based on Linux's iommu_fwspec support you can find at:
 *    drivers/iommu/iommu.c
 *
 * Copyright (C) 2019 EPAM Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/lib.h>
#include <xen/iommu.h>
#include <asm/device.h>
#include <asm/iommu_fwspec.h>

int iommu_fwspec_init(struct device *dev, struct device *iommu_dev)
{
    struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

    if ( fwspec )
        return 0;

    fwspec = xzalloc(struct iommu_fwspec);
    if ( !fwspec )
        return -ENOMEM;

    fwspec->iommu_dev = iommu_dev;
    dev_iommu_fwspec_set(dev, fwspec);

    return 0;
}

void iommu_fwspec_free(struct device *dev)
{
    struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

    if ( fwspec )
    {
        xfree(fwspec);
        dev_iommu_fwspec_set(dev, NULL);
    }
}

int iommu_fwspec_add_ids(struct device *dev, uint32_t *ids, int num_ids)
{
    struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
    size_t size;
    int i;

    if ( !fwspec )
        return -EINVAL;

    size = offsetof(struct iommu_fwspec, ids[fwspec->num_ids + num_ids]);
    if ( size > sizeof(*fwspec) )
    {
        fwspec = _xrealloc(fwspec, size, sizeof(void *));
        if ( !fwspec )
            return -ENOMEM;

        dev_iommu_fwspec_set(dev, fwspec);
    }

    for ( i = 0; i < num_ids; i++ )
        fwspec->ids[fwspec->num_ids + i] = ids[i];

    fwspec->num_ids += num_ids;

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
