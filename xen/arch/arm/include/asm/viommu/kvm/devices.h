#ifndef KVM__DEVICES_H
#define KVM__DEVICES_H

#include <xen/types.h>

enum device_bus_type {
	DEVICE_BUS_PCI,
	DEVICE_BUS_MMIO,
	DEVICE_BUS_IOPORT,
	DEVICE_BUS_MAX,
};

/* XXX */
struct viommu_ops;

struct device_header {
	enum device_bus_type	bus_type;
	void			*data;
	int			dev_num;
	struct rb_node		node;
	struct viommu_ops	*iommu_ops;
	void			*iommu_data;
	long			iommu_id;
};

#endif /* KVM__DEVICES_H */
