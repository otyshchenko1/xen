/*
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

#include <xen/dm.h>
#include <xen/guest_access.h>
#include <xen/hypercall.h>
#include <xen/ioreq.h>
#include <xen/nospec.h>

#include <asm/vgic.h>

static int set_pci_intx_level(struct domain *d, uint16_t domain,
                              uint8_t bus, uint8_t device,
                              uint8_t intx, uint8_t level)
{
    struct hvm_irq *hvm_irq = hvm_domain_irq(d);
    unsigned int irq, link;

    if ( domain != 0 || bus != 0 || device > 0x1f || intx > 3 )
        return -EINVAL;

    link = hvm_pci_intx_link(device, intx);
    //isa_irq = hvm_irq->pci_link.route[link];
    irq = link + GUEST_VIRTIO_PCI_SPI_FIRST;

    switch ( level )
    {
    case 0:
        if ( !__test_and_clear_bit(device*4 + intx, &hvm_irq->pci_intx.i) ) {
            printk("--- SKIP LOW: dev %d link %d irq %d level %d /// count %d\n",
                   device, link, irq, level, hvm_irq->pci_link_assert_count[link]);
            return 0;
        }

        if ( --hvm_irq->pci_link_assert_count[link] == 0 )
            vgic_inject_irq(d, NULL, irq, false);
        break;
    case 1:
        if ( __test_and_set_bit(device*4 + intx, &hvm_irq->pci_intx.i) ) {
            printk("--- SKIP HIGH: dev %d link %d irq %d level %d /// count %d\n",
                   device, link, irq, level, hvm_irq->pci_link_assert_count[link]);
            return 0;
        }

        if ( hvm_irq->pci_link_assert_count[link]++ == 0 )
            vgic_inject_irq(d, NULL, irq, true);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

int dm_op(const struct dmop_args *op_args)
{
    struct domain *d;
    struct xen_dm_op op;
    bool const_op = true;
    long rc;
    size_t offset;

    static const uint8_t op_size[] = {
        [XEN_DMOP_create_ioreq_server]              = sizeof(struct xen_dm_op_create_ioreq_server),
        [XEN_DMOP_get_ioreq_server_info]            = sizeof(struct xen_dm_op_get_ioreq_server_info),
        [XEN_DMOP_map_io_range_to_ioreq_server]     = sizeof(struct xen_dm_op_ioreq_server_range),
        [XEN_DMOP_unmap_io_range_from_ioreq_server] = sizeof(struct xen_dm_op_ioreq_server_range),
        [XEN_DMOP_set_ioreq_server_state]           = sizeof(struct xen_dm_op_set_ioreq_server_state),
        [XEN_DMOP_destroy_ioreq_server]             = sizeof(struct xen_dm_op_destroy_ioreq_server),
        [XEN_DMOP_set_irq_level]                    = sizeof(struct xen_dm_op_set_irq_level),
        [XEN_DMOP_nr_vcpus]                         = sizeof(struct xen_dm_op_nr_vcpus),
        [XEN_DMOP_set_pci_intx_level]               = sizeof(struct xen_dm_op_set_pci_intx_level),
    };

    rc = rcu_lock_remote_domain_by_id(op_args->domid, &d);
    if ( rc )
        return rc;

    rc = xsm_dm_op(XSM_DM_PRIV, d);
    if ( rc )
        goto out;

    offset = offsetof(struct xen_dm_op, u);

    rc = -EFAULT;
    if ( op_args->buf[0].size < offset )
        goto out;

    if ( copy_from_guest_offset((void *)&op, op_args->buf[0].h, 0, offset) )
        goto out;

    if ( op.op >= ARRAY_SIZE(op_size) )
    {
        rc = -EOPNOTSUPP;
        goto out;
    }

    op.op = array_index_nospec(op.op, ARRAY_SIZE(op_size));

    if ( op_args->buf[0].size < offset + op_size[op.op] )
        goto out;

    if ( copy_from_guest_offset((void *)&op.u, op_args->buf[0].h, offset,
                                op_size[op.op]) )
        goto out;

    rc = -EINVAL;
    if ( op.pad )
        goto out;

    switch ( op.op )
    {
    case XEN_DMOP_set_irq_level:
    {
        const struct xen_dm_op_set_irq_level *data =
            &op.u.set_irq_level;
        unsigned int i;

        /* Only SPIs are supported */
        if ( (data->irq < NR_LOCAL_IRQS) || (data->irq >= vgic_num_irqs(d)) )
        {
            rc = -EINVAL;
            break;
        }

        if ( data->level != 0 && data->level != 1 )
        {
            rc = -EINVAL;
            break;
        }

        /* Check that padding is always 0 */
        for ( i = 0; i < sizeof(data->pad); i++ )
        {
            if ( data->pad[i] )
            {
                rc = -EINVAL;
                break;
            }
        }

        /*
         * Allow to set the logical level of a line for non-allocated
         * interrupts only.
         */
        if ( test_bit(data->irq, d->arch.vgic.allocated_irqs) )
        {
            rc = -EINVAL;
            break;
        }

        vgic_inject_irq(d, NULL, data->irq, data->level);
        rc = 0;
        break;
    }

    case XEN_DMOP_nr_vcpus:
    {
        struct xen_dm_op_nr_vcpus *data = &op.u.nr_vcpus;

        data->vcpus = d->max_vcpus;
        const_op = false;
        rc = 0;
        break;
    }

    case XEN_DMOP_set_pci_intx_level:
    {
        const struct xen_dm_op_set_pci_intx_level *data =
            &op.u.set_pci_intx_level;

        spin_lock(&d->arch.hvm.irq_lock);
        rc = set_pci_intx_level(d, data->domain, data->bus, data->device,
                                data->intx, data->level);
        spin_unlock(&d->arch.hvm.irq_lock);
        break;
    }

    default:
        rc = ioreq_server_dm_op(&op, d, &const_op);
        break;
    }

    if ( (!rc || rc == -ERESTART) &&
         !const_op && copy_to_guest_offset(op_args->buf[0].h, offset,
                                           (void *)&op.u, op_size[op.op]) )
        rc = -EFAULT;

 out:
    rcu_unlock_domain(d);

    return rc;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
