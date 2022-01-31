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

#include <xen/err.h>
#include <xen/interval_tree.h>
#include <xen/sched.h>
#include <xen/types.h>

#include <asm/viommu/kvm/iommu.h>
#include <asm/viommu/kvm/kvm.h>

/*
 * Xen: The major change is to use Linux's interval_tree instead of
 * KVM's rbtree-interval.
 */

struct iommu_domain_stats {
	u64				accesses;
	u64				release;
	u64				invalidate;
};

struct iommu_mapping {
	struct interval_tree_node	iova_range;
	u64				phys;
	int				prot;

	struct list_head		tlb_entries;
};

struct iommu_domain {
	struct rb_root_cached		mappings;
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
		entry = xzalloc(struct iommu_tlb_entry);
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
	struct interval_tree_node *node;
	struct iommu_mapping *map;

	mutex_lock(&domain->mutex);

	dprintf(fd, "START IOMMU DUMP [[[\n"); /* You did ask for it. */
	for (node = interval_tree_iter_first(&domain->mappings, 0, -1UL); node;
			node = interval_tree_iter_next(node, 0, -1UL)) {
		map = container_of(node, struct iommu_mapping, iova_range);

		dprintf(fd, "%#lx-%#lx -> %#lx %#x\n", node->start,
			node->last, map->phys, map->prot);
	}
	dprintf(fd, "]]] END IOMMU DUMP\n");

	mutex_unlock(&domain->mutex);
}

int iommu_debug_domain(void *priv, int fd, struct iommu_debug_params *params)
{
	struct iommu_domain *domain = priv;

	switch (params->action) {
	case IOMMU_DEBUG_STATS:
		dprintf(fd, "    accesses            %lu\n",
			domain->stats.accesses);
		dprintf(fd, "    release             %lu\n",
			domain->stats.release);
		dprintf(fd, "    invalidate          %lu\n",
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
	struct iommu_domain *domain = xzalloc(struct iommu_domain);

	if (!domain)
		return NULL;

	domain->mappings = RB_ROOT_CACHED;
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
	struct interval_tree_node *node, *next;
	struct iommu_mapping *map;

	mutex_lock(&domain->mutex);
	/* All devices should have been detached */
	WARN_ON(!list_empty(&domain->used_tlb_entries));

	next = interval_tree_iter_first(&domain->mappings, 0, -1UL);
	while (next) {
		node = next;
		map = container_of(node, struct iommu_mapping, iova_range);
		next = interval_tree_iter_next(node, 0, -1UL);

		list_for_each_entry_safe(tlbe, next_tlbe, &map->tlb_entries, map_head) {
			list_del_init(&tlbe->map_head);
			list_move(&tlbe->domain_head, &domain->invalid_tlb_entries);
		}
		interval_tree_remove(node, &domain->mappings);
		xfree(map);
	}
	mutex_unlock(&domain->mutex);

	/* Move all entries to the free list */
	tlb_invalidate_entries(domain);

	list_for_each_entry_safe(tlbe, next_tlbe, &domain->free_tlb_entries, domain_head)
		xfree(tlbe);

	list_for_each_entry_safe(ep, ep_next, &domain->endpoints, head)
		xfree(ep);

	xfree(domain);
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

	ep = xzalloc(struct iommu_endpoint);
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

	xfree(ep);

	return 0;
}

int iommu_map_range(void *priv, u64 virt_start, u64 virt_end, u64 phys, int prot)
{
	struct iommu_domain *domain = priv;
	struct iommu_mapping *map;

	if (!domain)
		return -ENODEV;

	map = xmalloc(struct iommu_mapping);
	if (!map)
		return -ENOMEM;

	map->phys = phys;
	map->iova_range.start = virt_start;
	map->iova_range.last = virt_end;

	map->prot = prot;
	INIT_LIST_HEAD(&map->tlb_entries);

	mutex_lock(&domain->mutex);
	interval_tree_insert(&map->iova_range, &domain->mappings);
	mutex_unlock(&domain->mutex);

	return 0;
}

int iommu_unmap_range(void *priv, u64 start, u64 end, int flags)
{
	int ret = 0;
	struct interval_tree_node *node, *next;
	struct iommu_mapping *map;
	struct iommu_tlb_entry *tlbe, *next_tlbe;
	struct iommu_domain *domain = priv;
	bool silent = flags & IOMMU_UNMAP_SILENT;

	if (!domain)
		return -ENODEV;

	mutex_lock(&domain->mutex);
	next = interval_tree_iter_first(&domain->mappings, start, end);
	if (!next) {
		if (!silent)
			pr_debug("mapping not found");
		ret = -ENXIO;
	}

	while (next) {
		node = next;
		map = container_of(node, struct iommu_mapping, iova_range);
		next = interval_tree_iter_next(node, start, end);

		if (map->iova_range.start < start) {
			if (!silent)
				pr_debug("cannot split mapping");
			ret = -ERANGE;
			break;
		}

		/* Move cached entry to the invalidation queue */
		list_for_each_entry_safe(tlbe, next_tlbe, &map->tlb_entries, map_head) {
			list_del_init(&tlbe->map_head);
			list_move(&tlbe->domain_head, &domain->invalid_tlb_entries);
		}
		interval_tree_remove(node, &domain->mappings);
		xfree(map);
		break;
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
	struct interval_tree_node *node;
	size_t out_size;
	u64 out_addr = 0;

	if (!domain) {
		pr_err("no domain attached");
		viommu_report_fault(dev, IOMMU_FAULT_DOMAIN, addr, prot);
		/*errno = ENODEV;*/
		return 0;
	}

	mutex_lock(&domain->mutex);
	node = interval_tree_iter_first(&domain->mappings, addr, addr + size - 1);
	if (!node) {
		pr_err("fault at IOVA %#lx %zu", addr, size);
		viommu_report_fault(dev, IOMMU_FAULT_MAPPING, addr, prot);
		/*errno = EFAULT;*/
		goto out_unlock;
	}

	map = container_of(node, struct iommu_mapping, iova_range);
	if (prot & ~map->prot) {
		pr_err("permission fault at IOVA %#lx", addr);
		viommu_report_fault(dev, IOMMU_FAULT_MAPPING, addr, prot);
		/*errno = EPERM;*/
		goto out_unlock;
	}

	ep = domain_get_endpoint(domain, dev);
	if (WARN_ON(!ep))
		goto out_unlock;
	tlbe = tlb_get_free_entry(domain, ep);
	if (!tlbe)
		goto out_unlock;

	out_addr = map->phys + (addr - map->iova_range.start);
	out_size = min_t(size_t, map->iova_range.last - addr + 1, size);

	tlbe->virt_start = addr;
	tlbe->virt_end = addr + out_size - 1;
	tlbe->phys = out_addr;
	list_add(&tlbe->map_head, &map->tlb_entries);

	if (domain->debug_level >= IOMMU_DEBUG_L_INFO)
		pr_info("access %lx %zu/%zu %s%s -> %#lx", addr, out_size,
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
