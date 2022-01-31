#ifndef KVM_VIRTIO_IOMMU_H
#define KVM_VIRTIO_IOMMU_H

#include <xen/types.h>

#include "devices.h"
#include "iommu.h"

/* Command-line configuration */
struct iommu_config {
	const char			*name;
	bool				sw_msi;
	const char			*transport;
	int				debug_level;
};

#define IOMMU_PROT_NONE			0x0
#define IOMMU_PROT_READ			0x1
#define IOMMU_PROT_WRITE		0x2
#define IOMMU_PROT_EXEC			0x4
#define IOMMU_PROT_MMIO			0x8

/*
 * XXX struct viommu_ops already declared in include/xen/iommu.h
 * Add "v" prefix for now.
 */
struct viommu_ops {
	const struct iommu_properties *(*get_properties)(struct device_header *);

	void *(*alloc_domain)(struct device_header *);
	void (*free_domain)(void *);

	int (*attach)(void *, struct device_header *, int flags);
	int (*detach)(void *, struct device_header *);
	int (*map)(void *, u64 virt_start, u64 virt_end, u64 phys, int prot);
	int (*unmap)(void *, u64 virt_start, u64 virt_end, int flags);

	int (*debug_domain)(void *, int fd, struct iommu_debug_params *);
};

struct iommu_properties {
	const char			*name;
	u32				phandle;

	struct {
		u64			start;
		u64			end;
	}				msi_window;

	size_t				input_addr_size;
	u64				pgsize_mask;
};

static inline long device_to_iommu_id(struct device_header *dev)
{
	return dev->iommu_id;
}

struct kvm;

int viommu_update_config(void *viommu, struct iommu_properties *props);
void *viommu_register(struct kvm *kvm, struct iommu_properties *props, u64 base, u32 irq);
void viommu_unregister(struct kvm *kvm, void *cookie);

int viommu_debug(struct kvm *kvm, int fd, struct iommu_debug_params *);

#endif /* KVM_VIRTIO_IOMMU_H */
