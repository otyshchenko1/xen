#ifndef KVM_IOMMU_H
#define KVM_IOMMU_H

#include <stdbool.h>
#include <stdlib.h>
#include <linux/types.h>

#include "kvm/devices.h"

#define IOMMU_PROT_NONE			0x0
#define IOMMU_PROT_READ			0x1
#define IOMMU_PROT_WRITE		0x2
#define IOMMU_PROT_EXEC			0x4
#define IOMMU_PROT_MMIO			0x8

enum iommu_fault_reason {
	IOMMU_FAULT_UNKNOWN,
	IOMMU_FAULT_DOMAIN,
	IOMMU_FAULT_MAPPING,
};

enum iommu_debug_action {
	IOMMU_DEBUG_LIST,
	IOMMU_DEBUG_STATS,
	IOMMU_DEBUG_SET_PRINT,
	IOMMU_DEBUG_DUMP,
	IOMMU_DEBUG_FAULT,

	IOMMU_DEBUG_NUM_ACTIONS,
};

enum iommu_debug_level {
	IOMMU_DEBUG_L_DISABLED		= 0,
	IOMMU_DEBUG_L_INFO		= 4,
	IOMMU_DEBUG_L_VERBOSE		= 8,
};

#define IOMMU_DEBUG_SELECTOR_INVALID	((unsigned int)-1)

struct iommu_debug_fault {
	int num;
	int reason;
	int endpoint;
	unsigned long addr;
};

struct iommu_debug_params {
	enum iommu_debug_action		action;
	unsigned int			selector[2];
	int				debug_level;
	struct iommu_debug_fault	fault;
};

/*
 * Test if mapping is present. If not, return an error but do not report it to
 * stderr
 */
#define IOMMU_UNMAP_SILENT		0x1

struct iommu_endpoint;

struct iommu_tlb_entry {
	u64			virt_start;
	u64			virt_end;
	u64			phys;

	/* Data private to the device */
	void			*dev;

	/* IOMMU needs to keep track of a few things */
	struct iommu_endpoint	*ep;
	struct list_head	domain_head;
	struct list_head	map_head;
};

typedef int (*iommu_invalidate_cb)(void *domain, void *cookie,
				   struct device_header *dev,
				   struct iommu_tlb_entry *tlbe);

void *iommu_alloc_domain(struct device_header *dev);
void iommu_free_domain(void *domain);
int iommu_attach(void *domain, struct device_header *dev,
		 iommu_invalidate_cb invalidate, void *cookie);
int iommu_detach(void *domain, struct device_header *dev);
int iommu_map(void *domain, u64 virt_start, u64 virt_end, u64 phys, int prot);
int iommu_unmap(void *domain, u64 virt_start, u64 virt_end, int flags);
struct iommu_tlb_entry *
iommu_access(struct device_header *dev, void *priv, u64 addr, size_t size,
	     int prot);
void iommu_release(void *priv, struct iommu_tlb_entry *entry);
void iommu_release_locked(void *priv, struct iommu_tlb_entry *entry);
int iommu_debug_domain(void *domain, int fd, struct iommu_debug_params *params);

struct msi_msg;

int iommu_translate_msi(struct device_header *dev_hdr, void *domain,
			struct msi_msg *msi);

int viommu_report_fault(struct device_header *dev,
			enum iommu_fault_reason reason, unsigned long address,
			int flags);

#endif /* KVM_IOMMU_H */
