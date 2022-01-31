#include "kvm/devices.h"
#include "kvm/virtio-iommu.h"
#include "kvm/virtio-mmio.h"
#include "kvm/ioeventfd.h"
#include "kvm/virtio.h"
#include "kvm/kvm.h"
#include "kvm/irq.h"
#include "kvm/fdt.h"

#include <linux/virtio_mmio.h>
#include <string.h>

static u32 virtio_mmio_io_space_blocks = KVM_VIRTIO_MMIO_AREA;

static u32 virtio_mmio_get_io_space_block(u32 size)
{
	u32 block = virtio_mmio_io_space_blocks;
	virtio_mmio_io_space_blocks += size;

	return block;
}

static void virtio_mmio_ioevent_callback(struct kvm *kvm, void *param)
{
	struct virtio_mmio_ioevent_param *ioeventfd = param;
	struct virtio_mmio *vmmio = ioeventfd->vdev->virtio;

	ioeventfd->vdev->ops->notify_vq(kvm, vmmio->dev, ioeventfd->vq);
}

int virtio_mmio_init_ioeventfd(struct kvm *kvm, struct virtio_device *vdev,
			       u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct ioevent ioevent;
	int err;

	if (vdev->no_eventfd)
		return 0;

	vmmio->ioeventfds[vq] = (struct virtio_mmio_ioevent_param) {
		.vdev		= vdev,
		.vq		= vq,
	};

	ioevent = (struct ioevent) {
		.io_addr	= vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY,
		.io_len		= sizeof(u32),
		.fn		= virtio_mmio_ioevent_callback,
		.fn_ptr		= &vmmio->ioeventfds[vq],
		.datamatch	= vq,
		.fn_kvm		= kvm,
		.fd		= eventfd(0, 0),
	};

	if (vdev->use_vhost)
		/*
		 * Vhost will poll the eventfd in host kernel side,
		 * no need to poll in userspace.
		 */
		err = ioeventfd__add_event(&ioevent, 0);
	else
		/* Need to poll in userspace. */
		err = ioeventfd__add_event(&ioevent, IOEVENTFD_FLAG_USER_POLL);
	if (err)
		return err;

	if (vdev->ops->notify_vq_eventfd)
		vdev->ops->notify_vq_eventfd(kvm, vmmio->dev, vq, ioevent.fd);

	return 0;
}

int virtio_mmio_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_VRING;
	kvm__irq_trigger(vmmio->kvm, vmmio->irq);

	return 0;
}

int virtio_mmio_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
	int ret;
	struct virtio_mmio *vmmio = vdev->virtio;

	ret = virtio_mmio_init_ioeventfd(vmmio->kvm, vdev, vq);
	if (ret) {
		pr_err("couldn't add ioeventfd for vq %d: %d", vq, ret);
		return ret;
	}
	return vdev->ops->init_vq(vmmio->kvm, vmmio->dev, vq);
}

void virtio_mmio_exit_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	if (!vdev->no_eventfd)
		ioeventfd__del_event(vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY, vq);
	virtio_exit_vq(kvm, vdev, vmmio->dev, vq);
}

int virtio_mmio_signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_CONFIG;
	kvm__irq_trigger(vmmio->kvm, vmmio->irq);

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

#ifdef CONFIG_HAS_LIBFDT
#define DEVICE_NAME_MAX_LEN 32
static
void generate_virtio_mmio_fdt_node(void *fdt,
				   struct device_header *dev_hdr,
				   struct fdt_callbacks *cb)
{
	char dev_name[DEVICE_NAME_MAX_LEN];
	struct virtio_mmio *vmmio = container_of(dev_hdr,
						 struct virtio_mmio,
						 dev_hdr);
	u64 addr = vmmio->addr;
	u64 reg_prop[] = {
		cpu_to_fdt64(addr),
		cpu_to_fdt64(VIRTIO_MMIO_IO_SIZE),
	};

	snprintf(dev_name, DEVICE_NAME_MAX_LEN, "virtio@%llx", addr);

	_FDT(fdt_begin_node(fdt, dev_name));
	_FDT(fdt_property_string(fdt, "compatible", "virtio,mmio"));
	_FDT(fdt_property(fdt, "reg", reg_prop, sizeof(reg_prop)));
	_FDT(fdt_property(fdt, "dma-coherent", NULL, 0));
	cb->generate_irq_prop(fdt, vmmio->irq, IRQ_TYPE_EDGE_RISING);
	cb->generate_iommu_prop(fdt, dev_hdr);
	_FDT(fdt_end_node(fdt));
}
#else
static void generate_virtio_mmio_fdt_node(void *fdt,
					  struct device_header *dev_hdr,
					  struct fdt_callbacks *cb)
{
	die("Unable to generate device tree nodes without libfdt\n");
}
#endif

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

static struct iommu_ops virtio_mmio_iommu_ops = {
	.get_properties		= virtio__iommu_get_properties,
	.alloc_domain		= iommu_alloc_domain,
	.free_domain		= iommu_free_domain,
	.debug_domain		= iommu_debug_domain,
	.attach			= virtio_mmio_iommu_attach,
	.detach			= virtio_mmio_iommu_detach,
	.map			= iommu_map,
	.unmap			= iommu_unmap,
};

int virtio_mmio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class)
{
	bool legacy = vdev->legacy;
	struct virtio_mmio *vmmio = vdev->virtio;
	int r;

	vmmio->addr	= virtio_mmio_get_io_space_block(VIRTIO_MMIO_IO_SIZE);
	vmmio->kvm	= kvm;
	vmmio->dev	= dev;
	vmmio->vdev	= vdev;

	if (!legacy)
		vdev->endian = VIRTIO_ENDIAN_LE;

	r = kvm__register_mmio(kvm, vmmio->addr, VIRTIO_MMIO_IO_SIZE, false,
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
		.data		= generate_virtio_mmio_fdt_node,
		.iommu_ops	= vdev->use_iommu ? &virtio_mmio_iommu_ops : NULL,
	};

	vmmio->irq = irq__alloc_line();

	r = device__register(&vmmio->dev_hdr);
	if (r < 0) {
		kvm__deregister_mmio(kvm, vmmio->addr);
		return r;
	}
	vdev->dev = &vmmio->dev_hdr;

	/*
	 * Instantiate guest virtio-mmio devices using kernel command line
	 * (or module) parameter, e.g
	 *
	 * virtio_mmio.devices=0x200@0xd2000000:5,0x200@0xd2000200:6
	 */
	pr_debug("virtio-mmio.devices=0x%x@0x%x:%d", VIRTIO_MMIO_IO_SIZE,
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
	kvm__deregister_mmio(kvm, vmmio->addr);

	return 0;
}
