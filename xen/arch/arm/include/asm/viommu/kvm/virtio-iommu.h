#ifndef KVM_VIRTIO_IOMMU_H
#define KVM_VIRTIO_IOMMU_H

#include <stdbool.h>
#include <stdlib.h>

#include "devices.h"
#include "iommu.h"

/* Command-line configuration */
struct iommu_config {
	const char			*name;
	bool				sw_msi;
	bool				ioeventfd;
	const char			*transport;
	int				debug_level;
};

#define IOMMU_PROT_NONE			0x0
#define IOMMU_PROT_READ			0x1
#define IOMMU_PROT_WRITE		0x2
#define IOMMU_PROT_EXEC			0x4
#define IOMMU_PROT_MMIO			0x8

struct iommu_ops {
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

/*
 * All devices presented to the system have a device ID, that allows the IOMMU
 * to identify them. Since multiple buses can share an IOMMU, this device ID
 * must be unique system-wide. We define it here as:
 *
 *	(bus_type << 16) + dev_num
 *
 * Where dev_num is the device number on the bus as allocated by devices.c
 *
 * TODO: enforce this limit, by checking that the device number allocator
 * doesn't overflow BUS_SIZE.
 */

#define BUS_SIZE 0x10000

static inline long device_to_iommu_id(struct device_header *dev)
{
	return dev->bus_type * BUS_SIZE + dev->dev_num;
}

#define iommu_id_to_bus(device_id)	((device_id) / BUS_SIZE)
#define iommu_id_to_devnum(device_id)	((device_id) % BUS_SIZE)

static inline struct device_header *iommu_get_device(u32 device_id)
{
	enum device_bus_type bus = iommu_id_to_bus(device_id);
	u32 dev_num = iommu_id_to_devnum(device_id);

	return device__find_dev(bus, dev_num);
}

struct kvm;
struct option;

int viommu_bus_parser(const struct option *opt, const char *arg, int unset);
int viommu_update_config(void *viommu, struct iommu_properties *props);
void *viommu_register(struct kvm *kvm, struct iommu_properties *props);
void viommu_unregister(struct kvm *kvm, void *cookie);

int viommu_parse_debug_string(const char *options, struct iommu_debug_params *);
int viommu_debug(struct kvm *kvm, int fd, struct iommu_debug_params *);

#ifdef CONFIG_HAS_LIBFDT
void viommu_mmio_generate_fdt_props(void *fdt, struct device_header *dev_hdr);
#endif

#endif /* KVM_VIRTIO_IOMMU_H */
