
#include <xen/err.h>
#include <xen/sched.h>
#include <xen/types.h>

#include <asm/viommu/viommu.h>

#include <asm/viommu/kvm/virtio-iommu.h>
#include <asm/viommu/kvm/devices.h>
#include <asm/viommu/kvm/virtio-mmio.h>
#include <asm/viommu/kvm/virtio.h>
#include <asm/viommu/kvm/kvm.h>

#include <asm/viommu/linux/virtio_mmio.h>

int virtio_mmio_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_VRING;
	viommu_irq_trigger(vmmio->kvm, vmmio->irq);

	return 0;
}

int virtio_mmio_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	return vdev->ops->init_vq(vmmio->kvm, vmmio->dev, vq);
}

void virtio_mmio_exit_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	virtio_exit_vq(kvm, vdev, vmmio->dev, vq);
}

int virtio_mmio_signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_CONFIG;
	viommu_irq_trigger(vmmio->kvm, vmmio->irq);

	return 0;
}

void virtio_mmio_device_specific(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				 u32 len, u8 is_write,
				 struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	if (is_write)
		virtio_write_config(vmmio->kvm, vdev, vmmio->dev, addr, data,
				    len);
	else
		virtio_read_config(vmmio->kvm, vdev, vmmio->dev, addr, data,
				   len);
}

#define mmio_dev_to_virtio(dev_hdr)					\
	container_of(dev_hdr, struct virtio_mmio, dev_hdr)->vdev

static int virtio_mmio_iommu_attach(void *domain, struct device_header *dev_hdr,
				    int flags)
{
	return virtio__iommu_attach(domain, dev_hdr,
				    mmio_dev_to_virtio(dev_hdr), flags);
}

static int virtio_mmio_iommu_detach(void *domain, struct device_header *dev_hdr)
{
	return virtio__iommu_detach(domain, dev_hdr,
				    mmio_dev_to_virtio(dev_hdr));
}

static struct viommu_ops virtio_mmio_iommu_ops = {
	.get_properties		= virtio__iommu_get_properties,
	.alloc_domain		= iommu_alloc_domain,
	.free_domain		= iommu_free_domain,
	.debug_domain		= iommu_debug_domain,
	.attach			= virtio_mmio_iommu_attach,
	.detach			= virtio_mmio_iommu_detach,
	.map			= iommu_map_range,
	.unmap			= iommu_unmap_range,
};

int virtio_mmio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class, u64 base, u32 irq)
{
	bool legacy = vdev->legacy;
	struct virtio_mmio *vmmio = vdev->virtio;
	int r;

	vmmio->addr	= base;
	vmmio->kvm	= kvm;
	vmmio->dev	= dev;
	vmmio->vdev	= vdev;

	if (!legacy)
		vdev->endian = VIRTIO_ENDIAN_LE;

	r = viommu_register_mmio(kvm, vmmio->addr, VIRTIO_MMIO_IO_SIZE,
			       legacy ? virtio_mmio_legacy_callback :
			       virtio_mmio_modern_callback, vdev);
	if (r < 0)
		return r;

	vmmio->hdr = (struct virtio_mmio_hdr) {
		.magic		= {'v', 'i', 'r', 't'},
		.version	= legacy ? 1 : 2,
		.device_id	= subsys_id,
		.vendor_id	= 0x4d564b4c , /* 'LKVM' */
		.queue_num_max	= 256,
	};

	vmmio->dev_hdr = (struct device_header) {
		.bus_type	= DEVICE_BUS_MMIO,
		.data		= NULL,
		/* XXX Hack */
		.iommu_ops	= !vdev->use_iommu ? &virtio_mmio_iommu_ops : NULL,
		.iommu_id	= GUEST_VIRTIO_MMIO_IOMMU_ID,
	};

	vmmio->irq = irq;

	vmmio->dev_hdr.dev_num = kvm->domain_id;
	vdev->dev = &vmmio->dev_hdr;

	/*
	 * Instantiate guest virtio-mmio devices using kernel command line
	 * (or module) parameter, e.g
	 *
	 * virtio_mmio.devices=0x200@0xd2000000:5,0x200@0xd2000200:6
	 */
	pr_debug("virtio-mmio.devices=0x%x@0x%lx:%d", VIRTIO_MMIO_IO_SIZE,
		 vmmio->addr, vmmio->irq);

	return 0;
}

int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)
{
	int vq;
	struct virtio_mmio *vmmio = vdev->virtio;

	for (vq = 0; vq < vdev->ops->get_vq_count(kvm, vmmio->dev); vq++)
		virtio_mmio_exit_vq(kvm, vdev, vq);

	return 0;
}

int virtio_mmio_exit(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	virtio_mmio_reset(kvm, vdev);
	viommu_deregister_mmio(kvm, vmmio->addr);

	return 0;
}
