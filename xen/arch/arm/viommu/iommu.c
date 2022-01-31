/*
 * Implement basic IOMMU operations - map, unmap and translate
 *
 * Invalidation of mappings is the difficult bit. When virtio devices call
 * iommu_access(), we return a tlb_entry structure that describes the IOVA->GPA
 * mapping. When the guest removes this mapping, the device needs to relinquish
 * all associated tlb_entry structures. Some may still be in use, sometimes even
 * by the host kernel! The invalidate() callback passed by the device during
 * attach must wait for all concurrent DMA to finish, and make sure that
 * subsequent DMA won't see a cached version of the mapping.
 */
#include <errno.h>

#include "kvm/iommu.h"
#include "kvm/kvm.h"
#include "kvm/msi.h"
#include "kvm/mutex.h"
#include "kvm/rbtree-interval.h"

#include <linux/list.h>

struct iommu_domain_stats {
	u64				accesses;
	u64				release;
	u64				invalidate;
};

struct iommu_mapping {
	struct rb_int_node		iova_range;
	u64				phys;
	int				prot;

	struct list_head		tlb_entries;
};

struct iommu_domain {
	struct rb_root			mappings;
	struct mutex			mutex;
	struct list_head		endpoints;

	struct list_head		free_tlb_entries;
	struct list_head		used_tlb_entries;
	struct list_head		invalid_tlb_entries;

	struct iommu_domain_stats	stats;
	int				debug_level;
};

struct iommu_endpoint {
	struct device_header		*dev;
	iommu_invalidate_cb		invalidate;
	void				*cookie;
	struct list_head		head;
};

/* Caller must hold domain->mutex */
static struct iommu_tlb_entry *
tlb_get_free_entry(struct iommu_domain *domain, struct iommu_endpoint *ep)
{
	struct iommu_tlb_entry *entry;

	if (list_empty(&domain->free_tlb_entries)) {
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return NULL;

		list_add(&entry->domain_head, &domain->used_tlb_entries);
	} else {
		entry = list_first_entry(&domain->free_tlb_entries,
					 struct iommu_tlb_entry, domain_head);
		list_move(&entry->domain_head, &domain->used_tlb_entries);
	}
	entry->ep = ep;

	return entry;
}

static int tlb_invalidate_entries(struct iommu_domain *domain)
{
	struct iommu_tlb_entry *tlbe;

	mutex_lock(&domain->mutex);
	while (!list_empty(&domain->invalid_tlb_entries)) {
		tlbe = list_first_entry(&domain->invalid_tlb_entries,
					struct iommu_tlb_entry, domain_head);

		/* Make sure another invalidate_entries cannot find this */
		list_del(&tlbe->domain_head);

		/*
		 * Release the domain mutex to avoid any nasty deadlock. For
		 * example the virtio core holds the buf lock while performing
		 * IOMMU accesses (which takes domain->mutex). Its invalidate()
		 * callback also takes this buf lock to wait for any concurrent
		 * DMA.
		 */
		mutex_unlock(&domain->mutex);

		/* Ask politely to stop using this entry */
		tlbe->ep->invalidate(domain, tlbe->ep->cookie, tlbe->ep->dev,
				     tlbe);

		mutex_lock(&domain->mutex);
		domain->stats.invalidate++;
		list_add(&tlbe->domain_head, &domain->free_tlb_entries);
	}
	mutex_unlock(&domain->mutex);

	return 0;
}

static void iommu_dump(struct iommu_domain *domain, int fd)
{
	struct rb_node *node;
	struct iommu_mapping *map;

	mutex_lock(&domain->mutex);

	dprintf(fd, "START IOMMU DUMP [[[\n"); /* You did ask for it. */
	for (node = rb_first(&domain->mappings); node; node = rb_next(node)) {
		struct rb_int_node *int_node = rb_int(node);
		map = container_of(int_node, struct iommu_mapping, iova_range);

		dprintf(fd, "%#llx-%#llx -> %#llx %#x\n", int_node->low,
			int_node->high, map->phys, map->prot);
	}
	dprintf(fd, "]]] END IOMMU DUMP\n");

	mutex_unlock(&domain->mutex);
}

int iommu_debug_domain(void *priv, int fd, struct iommu_debug_params *params)
{
	struct iommu_domain *domain = priv;

	switch (params->action) {
	case IOMMU_DEBUG_STATS:
		dprintf(fd, "    accesses            %llu\n",
			domain->stats.accesses);
		dprintf(fd, "    release             %llu\n",
			domain->stats.release);
		dprintf(fd, "    invalidate          %llu\n",
			domain->stats.invalidate);
		break;
	case IOMMU_DEBUG_SET_PRINT:
		domain->debug_level = params->debug_level;
		break;
	case IOMMU_DEBUG_DUMP:
		iommu_dump(domain, fd);
	default:
		break;
	}

	return 0;
}

void *iommu_alloc_domain(struct device_header *unused)
{
	struct iommu_domain *domain = calloc(1, sizeof(*domain));

	if (!domain)
		return NULL;

	domain->mappings = (struct rb_root)RB_ROOT;
	mutex_init(&domain->mutex);
	INIT_LIST_HEAD(&domain->endpoints);
	INIT_LIST_HEAD(&domain->used_tlb_entries);
	INIT_LIST_HEAD(&domain->free_tlb_entries);
	INIT_LIST_HEAD(&domain->invalid_tlb_entries);

	return domain;
}

void iommu_free_domain(void *priv)
{
	struct iommu_tlb_entry *tlbe, *next_tlbe;
	struct iommu_endpoint *ep, *ep_next;
	struct iommu_domain *domain = priv;
	struct rb_int_node *int_node;
	struct rb_node *node, *next;
	struct iommu_mapping *map;

	mutex_lock(&domain->mutex);
	/* All devices should have been detached */
	WARN_ON(!list_empty(&domain->used_tlb_entries));

	/* Postorder allows to free leaves first. */
	node = rb_first_postorder(&domain->mappings);
	while (node) {
		next = rb_next_postorder(node);

		int_node = rb_int(node);
		map = container_of(int_node, struct iommu_mapping, iova_range);
		list_for_each_entry_safe(tlbe, next_tlbe, &map->tlb_entries, map_head) {
			list_del_init(&tlbe->map_head);
			list_move(&tlbe->domain_head, &domain->invalid_tlb_entries);
		}
		free(map);

		node = next;
	}
	mutex_unlock(&domain->mutex);

	/* Move all entries to the free list */
	tlb_invalidate_entries(domain);

	list_for_each_entry_safe(tlbe, next_tlbe, &domain->free_tlb_entries, domain_head)
		free(tlbe);

	list_for_each_entry_safe(ep, ep_next, &domain->endpoints, head)
		free(ep);

	free(domain);
}

static struct iommu_endpoint *domain_get_endpoint(struct iommu_domain *domain,
						  struct device_header *dev)
{
	struct iommu_endpoint *ep;

	list_for_each_entry(ep, &domain->endpoints, head) {
		if (ep->dev == dev)
			return ep;
	}

	return NULL;
}

int iommu_attach(void *priv, struct device_header *dev,
		 iommu_invalidate_cb inval, void *cookie)
{
	struct iommu_endpoint *ep;
	struct iommu_domain *domain = priv;

	ep = calloc(1, sizeof(*ep));
	if (!ep)
		return -ENOMEM;

	ep->dev		= dev;
	ep->invalidate	= inval;
	ep->cookie	= cookie;

	mutex_lock(&domain->mutex);
	list_add(&ep->head, &domain->endpoints);
	mutex_unlock(&domain->mutex);

	return 0;
}

int iommu_detach(void *priv, struct device_header *dev)
{
	struct iommu_endpoint *ep;
	struct iommu_domain *domain = priv;
	struct iommu_tlb_entry *tlbe, *next_tlbe;

	mutex_lock(&domain->mutex);
	ep = domain_get_endpoint(domain, dev);
	if (!ep) {
		mutex_unlock(&domain->mutex);
		return -EINVAL;
	}

	list_for_each_entry_safe(tlbe, next_tlbe, &domain->used_tlb_entries,
				 domain_head) {
		if (tlbe->ep == ep)
			list_move(&tlbe->domain_head,
				  &domain->invalid_tlb_entries);
	}
	list_del(&ep->head);
	mutex_unlock(&domain->mutex);

	tlb_invalidate_entries(domain);

	free(ep);

	return 0;
}

int iommu_map(void *priv, u64 virt_start, u64 virt_end, u64 phys, int prot)
{
	struct iommu_domain *domain = priv;
	struct iommu_mapping *map;

	if (!domain)
		return -ENODEV;

	map = malloc(sizeof(*map));
	if (!map)
		return -ENOMEM;

	map->phys = phys;
	map->iova_range = RB_INT_INIT(virt_start, virt_end);
	map->prot = prot;
	INIT_LIST_HEAD(&map->tlb_entries);

	mutex_lock(&domain->mutex);
	rb_int_insert(&domain->mappings, &map->iova_range);
	mutex_unlock(&domain->mutex);

	return 0;
}

int iommu_unmap(void *priv, u64 start, u64 end, int flags)
{
	int ret = 0;
	struct rb_int_node *node;
	struct iommu_mapping *map;
	struct iommu_tlb_entry *tlbe, *next_tlbe;
	struct iommu_domain *domain = priv;
	bool silent = flags & IOMMU_UNMAP_SILENT;

	if (!domain)
		return -ENODEV;

	mutex_lock(&domain->mutex);
	node = rb_int_search_single(&domain->mappings, start);
	if (!node) {
		if (!silent)
			pr_debug("mapping not found");
		ret = -ENXIO;
	}

	while (node) {
		struct rb_node *next = rb_next(&node->node);
		map = container_of(node, struct iommu_mapping, iova_range);

		if (node->low > end)
			break;

		if (node->high > end) {
			if (!silent)
				pr_debug("cannot split mapping");
			ret = -ERANGE;
			break;
		}

		rb_erase(&node->node, &domain->mappings);

		/* Move cached entry to the invalidation queue */
		list_for_each_entry_safe(tlbe, next_tlbe, &map->tlb_entries, map_head) {
			list_del_init(&tlbe->map_head);
			list_move(&tlbe->domain_head, &domain->invalid_tlb_entries);
		}
		free(map);
		node = next ? container_of(next, struct rb_int_node, node) : NULL;
	}
	mutex_unlock(&domain->mutex);

	tlb_invalidate_entries(domain);

	return ret;
}

/*
 * Translate a virtual address into a physical one. Perform an access of @size
 * bytes with protection @prot. If @addr isn't mapped in @domain, return NULL.
 * If the permissions of the mapping don't match, return NULL. If the access
 * range specified by (addr, size) spans over multiple mappings, only access the
 * first mapping and return the accessed size in @out_size. It is up to the
 * caller to complete the access by calling the function again on the remaining
 * range. Subsequent accesses are not guaranteed to succeed.
 *
 * The TLB entry can be released with iommu_release(). Otherwise, the device
 * will receive a invalidate() notification when the guest removes the mapping.
 */
struct iommu_tlb_entry *
iommu_access(struct device_header *dev, void *priv, u64 addr, size_t size,
	     int prot)
{
	struct iommu_tlb_entry *tlbe = NULL;
	struct iommu_domain *domain = priv;
	struct iommu_endpoint *ep;
	struct iommu_mapping *map;
	struct rb_int_node *node;
	size_t out_size;
	u64 out_addr = 0;

	if (!domain) {
		pr_err("no domain attached");
		viommu_report_fault(dev, IOMMU_FAULT_DOMAIN, addr, prot);
		errno = ENODEV;
		return 0;
	}

	mutex_lock(&domain->mutex);
	node = rb_int_search_single(&domain->mappings, addr);
	if (!node) {
		pr_err("fault at IOVA %#llx %zu", addr, size);
		viommu_report_fault(dev, IOMMU_FAULT_MAPPING, addr, prot);
		errno = EFAULT;
		goto out_unlock;
	}

	map = container_of(node, struct iommu_mapping, iova_range);
	if (prot & ~map->prot) {
		pr_err("permission fault at IOVA %#llx", addr);
		viommu_report_fault(dev, IOMMU_FAULT_MAPPING, addr, prot);
		errno = EPERM;
		goto out_unlock;
	}

	ep = domain_get_endpoint(domain, dev);
	if (WARN_ON(!ep))
		goto out_unlock;
	tlbe = tlb_get_free_entry(domain, ep);
	if (!tlbe)
		goto out_unlock;

	out_addr = map->phys + (addr - node->low);
	out_size = min_t(size_t, node->high - addr + 1, size);

	tlbe->virt_start = addr;
	tlbe->virt_end = addr + out_size - 1;
	tlbe->phys = out_addr;
	list_add(&tlbe->map_head, &map->tlb_entries);

	if (domain->debug_level >= IOMMU_DEBUG_L_INFO)
		pr_info("access %llx %zu/%zu %s%s -> %#llx", addr, out_size,
			size, prot & IOMMU_PROT_READ ? "R" : "",
			prot & IOMMU_PROT_WRITE ? "W" : "", out_addr);

	domain->stats.accesses++;
out_unlock:
	mutex_unlock(&domain->mutex);

	return tlbe;
}

/*
 * Release a TLB entry obtained by iommu_access.
 */
void iommu_release(void *priv, struct iommu_tlb_entry *entry)
{
	struct iommu_domain *domain = priv;

	mutex_lock(&domain->mutex);
	if (!list_empty(&entry->map_head))
		list_del(&entry->map_head);

	domain->stats.release++;
	list_move(&entry->domain_head, &domain->free_tlb_entries);
	mutex_unlock(&domain->mutex);
}

int iommu_translate_msi(struct device_header *dev, void *domain,
			struct msi_msg *msg)
{
	size_t size = 4;
	struct iommu_tlb_entry *tlbe;
	u64 addr = ((u64)msg->address_hi << 32) | msg->address_lo;

	tlbe = iommu_access(dev, domain, addr, size, IOMMU_PROT_WRITE);
	if (!tlbe) {
		pr_err("could not translate MSI doorbell");
		return -EFAULT;
	}

	msg->address_lo = tlbe->phys & 0xffffffff;
	msg->address_hi = tlbe->phys >> 32;

	/*
	 * TODO: release TLBE once the message is removed from the MSI-X table.
	 * Currently we have to release early, otherwise an invalidate would
	 * block indefinitely on this address. But we have no guarantee that the
	 * device won't access it after an invalidate.
	 */
	iommu_release(domain, tlbe);

	return 0;
}
