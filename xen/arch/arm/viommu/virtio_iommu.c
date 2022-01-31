#include <errno.h>
#include <stdbool.h>

#include <sys/uio.h>

#include <linux/bitops.h>
#include <linux/byteorder.h>
#include <linux/err.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_iommu.h>

#include "kvm/guest_compat.h"
#include "kvm/iovec.h"
#include "kvm/irq.h"
#include "kvm/of_pci.h"
#include "kvm/threadpool.h"
#include "kvm/virtio.h"
#include "kvm/virtio-iommu.h"

#define VIOMMU_DEFAULT_QUEUE_SIZE	256
/* 256 bytes should be enough for everybody */
#define VIOMMU_DEFAULT_PROBE_SIZE	(256 - \
					 (sizeof(struct virtio_iommu_req_probe) + \
					  sizeof(struct virtio_iommu_req_tail)))
#define VIOMMU_MAX_REQ_LEN		0x1000

#define VIOMMU_REQUEST_QUEUE		0
#define VIOMMU_EVENT_QUEUE		1
#define VIOMMU_NR_VQS			2

#define PCI_CLASS_IOMMU			0x0806

struct viommu_domain_stats {
	u64				map;
	u64				unmap;
	u64				resident;
};

struct viommu_stats {
	u64				kicks;
	u64				requests;
};

/*
 * Endpoints are allocated lazily the first time the driver attaches them to a
 * domain. They are freed on device reset.
 */
struct viommu_endpoint {
	struct viommu_dev		*viommu;
	struct device_header		*dev;
	struct viommu_domain		*domain;
	struct list_head		list;
	struct list_head		viommu_list;
};

/*
 * Domains are created when the driver attaches them to a device, and freed when
 * they aren't attached to anythign anymore.
 */
struct viommu_domain {
	u32				id;
	struct mutex			endpoints_mutex;
	struct list_head		endpoints;
	size_t				nr_endpoints;
	struct rb_node			node;

	struct iommu_ops		*ops;
	void				*priv;

	int				debug_level;
	struct viommu_domain_stats	stats;
};

struct viommu_dev {
	u32				id;

	struct virtio_device		vdev;
	struct virtio_iommu_config	config;
	enum virtio_trans		transport;
	u64				granularity;
	u64				addr_mask;

	const struct iommu_properties	*properties;
	struct virtio_ops		ops;

	struct virt_queue		vqs[VIOMMU_NR_VQS];
	bool				ioeventfd;
	pthread_t			request_thread;
	struct mutex			request_mutex;
	pthread_cond_t			request_cond;
	struct mutex			event_mutex;

	struct mutex			endpoints_mutex;
	struct list_head		endpoints;
	struct rb_root			domains;
	struct mutex			domains_mutex;
	struct kvm			*kvm;
	struct list_head		list;

	char				buf[VIOMMU_MAX_REQ_LEN];

	int				debug_level;
	struct viommu_stats		stats;
};

static int compat_id = -1;

static long long viommu_ids;
static LIST_HEAD(viommus);
static DEFINE_MUTEX(viommus_mutex);

#define domain_debug(domain, fmt, ...)					\
	do {								\
		if ((domain)->debug_level >= IOMMU_DEBUG_L_INFO)	\
			pr_info("domain[%d] " fmt, (domain)->id,	\
				##__VA_ARGS__);				\
	} while (0)

static struct viommu_domain *
viommu_get_domain(struct viommu_dev *viommu, u32 domain_id, bool speculative)
{
	struct rb_node *node;
	struct viommu_domain *domain, *found = NULL;

	mutex_lock(&viommu->domains_mutex);
	node = viommu->domains.rb_node;
	while (node) {
		domain = container_of(node, struct viommu_domain, node);
		if (domain->id > domain_id) {
			node = node->rb_left;
		} else if (domain->id < domain_id) {
			node = node->rb_right;
		} else {
			found = domain;
			break;
		}
	}
	mutex_unlock(&viommu->domains_mutex);

	if (!found && !speculative)
		pr_err("could not find domain %u", domain_id);

	return found;
}

#define viommu_find_domain(viommu, domain_id)	\
	viommu_get_domain(viommu, domain_id, false)
#define viommu_search_domain(viommu, domain_id)	\
	viommu_get_domain(viommu, domain_id, true)

static int viommu_for_each_domain(struct viommu_dev *viommu,
				  int (*fun)(struct viommu_dev *viommu,
					     struct viommu_domain *domain,
					     void *data),
				  void *data)
{
	int ret;
	struct viommu_domain *domain;
	struct rb_node *node, *next;

	mutex_lock(&viommu->domains_mutex);
	node = rb_first(&viommu->domains);
	while (node) {
		next = rb_next(node);
		domain = container_of(node, struct viommu_domain, node);

		ret = fun(viommu, domain, data);
		if (ret)
			break;

		node = next;
	}

	mutex_unlock(&viommu->domains_mutex);

	return ret;
}

static struct viommu_domain *
viommu_alloc_domain(struct viommu_dev *viommu, struct device_header *dev,
		    u32 domain_id)
{
	struct rb_node **node, *parent = NULL;
	struct viommu_domain *new_domain, *domain;
	struct iommu_ops *ops = dev->iommu_ops;

	if (!ops || !ops->get_properties || !ops->alloc_domain ||
	    !ops->free_domain || !ops->attach || !ops->detach ||
	    !ops->map || !ops->unmap) {
		/* Catch programming mistakes early */
		pr_err("Invalid IOMMU ops");
		return NULL;
	}

	new_domain = calloc(1, sizeof(*new_domain));
	if (!new_domain)
		return NULL;

	INIT_LIST_HEAD(&new_domain->endpoints);
	mutex_init(&new_domain->endpoints_mutex);
	new_domain->id		= domain_id;
	new_domain->ops		= ops;
	/* A NULL priv pointer is valid. */
	new_domain->priv	= ops->alloc_domain(dev);
	new_domain->debug_level	= viommu->debug_level;

	mutex_lock(&viommu->domains_mutex);

	node = &viommu->domains.rb_node;
	while (*node) {
		domain = container_of(*node, struct viommu_domain, node);
		parent = *node;

		if (domain->id > domain_id) {
			node = &((*node)->rb_left);
		} else if (domain->id < domain_id) {
			node = &((*node)->rb_right);
		} else {
			pr_err("domain exists!");
			free(new_domain);
			mutex_unlock(&viommu->domains_mutex);
			return NULL;
		}
	}

	rb_link_node(&new_domain->node, parent, node);
	rb_insert_color(&new_domain->node, &viommu->domains);

	mutex_unlock(&viommu->domains_mutex);

	return new_domain;
}

static void viommu_free_domain(struct viommu_dev *viommu,
			       struct viommu_domain *domain)
{
	if (domain->priv)
		domain->ops->free_domain(domain->priv);

	mutex_lock(&viommu->domains_mutex);
	rb_erase(&domain->node, &viommu->domains);
	mutex_unlock(&viommu->domains_mutex);
	free(domain);
}

static int viommu_domain_add_endpoint(struct viommu_domain *domain,
				      struct viommu_endpoint *vdev)
{
	mutex_lock(&domain->endpoints_mutex);
	list_add_tail(&vdev->list, &domain->endpoints);
	domain->nr_endpoints++;
	vdev->domain = domain;
	mutex_unlock(&domain->endpoints_mutex);

	return 0;
}

static int viommu_domain_del_endpoint(struct viommu_domain *domain,
				      struct viommu_endpoint *vdev)
{
	mutex_lock(&domain->endpoints_mutex);
	list_del(&vdev->list);
	domain->nr_endpoints--;
	vdev->domain = NULL;
	mutex_unlock(&domain->endpoints_mutex);

	return 0;
}

static int viommu_detach_endpoint(struct viommu_dev *viommu,
				  struct viommu_endpoint *vdev)
{
	int ret;
	struct viommu_domain *domain = vdev->domain;
	struct device_header *dev = vdev->dev;

	if (!domain)
		return -EINVAL;

	domain_debug(domain, "detaching endpoint %#lx",
		     device_to_iommu_id(dev));

	ret = dev->iommu_ops->detach(domain->priv, dev);
	if (!ret)
		ret = viommu_domain_del_endpoint(domain, vdev);

	if (!domain->nr_endpoints)
		viommu_free_domain(viommu, domain);

	return ret;
}

static struct viommu_endpoint *viommu_alloc_endpoint(struct viommu_dev *viommu,
						     struct device_header *dev)
{
	struct viommu_endpoint *vdev = calloc(1, sizeof(*vdev));

	if (!vdev)
		return NULL;

	dev->iommu_data = vdev;
	vdev->dev = dev;
	vdev->viommu = viommu;

	mutex_lock(&viommu->endpoints_mutex);
	list_add(&vdev->viommu_list, &viommu->endpoints);
	mutex_unlock(&viommu->endpoints_mutex);

	return vdev;
}

/* Caller must hold viommu->endpoints_mutex */
static void viommu_free_endpoint(struct viommu_dev *viommu,
				 struct viommu_endpoint *vdev)
{
	viommu_detach_endpoint(viommu, vdev);

	vdev->dev->iommu_data = NULL;
	list_del(&vdev->viommu_list);
	free(vdev);
}

static void viommu_free_all_endpoints(struct viommu_dev *viommu)
{
	struct viommu_endpoint *ep, *next;

	mutex_lock(&viommu->endpoints_mutex);
	list_for_each_entry_safe(ep, next, &viommu->endpoints, viommu_list)
		viommu_free_endpoint(viommu, ep);
	mutex_unlock(&viommu->endpoints_mutex);
}

static int viommu_handle_attach(struct viommu_dev *viommu,
				struct virtio_iommu_req_attach *attach)
{
	int ret;
	struct viommu_domain *domain;
	struct device_header *dev;
	struct viommu_endpoint *vdev;

	u32 endpoint_id	= le32_to_cpu(attach->endpoint);
	u32 domain_id	= le32_to_cpu(attach->domain);

	if (*(u64 *)attach->reserved)
		return -EINVAL;

	dev = iommu_get_device(endpoint_id);
	if (IS_ERR_OR_NULL(dev)) {
		pr_err("could not find endpoint %#x", endpoint_id);
		return -ENODEV;
	}

	vdev = dev->iommu_data;
	if (!vdev) {
		vdev = viommu_alloc_endpoint(viommu, dev);
		if (!vdev)
			return -ENOMEM;
	}

	if (vdev->domain) {
		if (vdev->domain->id == domain_id)
			return -EINVAL;

		ret = viommu_detach_endpoint(viommu, vdev);
		if (ret)
			return ret;
	}

	domain = viommu_search_domain(viommu, domain_id);
	if (!domain) {
		domain = viommu_alloc_domain(viommu, dev, domain_id);
		if (!domain)
			return -ENOMEM;
	} else if (domain->ops->map != dev->iommu_ops->map ||
		   domain->ops->unmap != dev->iommu_ops->unmap) {
		return -EINVAL;
	}

	ret = dev->iommu_ops->attach(domain->priv, dev, 0);
	if (!ret)
		ret = viommu_domain_add_endpoint(domain, vdev);

	if (ret && domain->nr_endpoints == 0)
		viommu_free_domain(viommu, domain);

	if (!ret)
		domain_debug(domain, "attached endpoint %#x", endpoint_id);

	return ret;
}

static int viommu_handle_detach(struct viommu_dev *viommu,
				struct virtio_iommu_req_detach *detach)
{
	struct device_header *dev;
	struct viommu_endpoint *vdev;
	struct viommu_domain *domain;

	u32 endpoint_id	= le32_to_cpu(detach->endpoint);
	u32 domain_id	= le32_to_cpu(detach->domain);

	if (detach->reserved)
		return -EINVAL;

	dev = iommu_get_device(endpoint_id);
	if (IS_ERR_OR_NULL(dev)) {
		pr_err("could not find endpoint %#x", endpoint_id);
		return -ENODEV;
	}

	vdev = dev->iommu_data;
	if (!vdev)
		return -ENODEV;

	domain = viommu_search_domain(viommu, domain_id);
	if (!domain || vdev->domain != domain)
		return -EINVAL;

	return viommu_detach_endpoint(viommu, vdev);
}

static int viommu_handle_map(struct viommu_dev *viommu,
			     struct virtio_iommu_req_map *map)
{
	int ret;
	int prot = 0;
	struct viommu_domain *domain;
	u64 granule = viommu->granularity;
	u64 addr_mask = viommu->addr_mask;

	u32 domain_id	= le32_to_cpu(map->domain);
	u64 virt_start	= le64_to_cpu(map->virt_start);
	u64 virt_end	= le64_to_cpu(map->virt_end);
	u64 phys_start	= le64_to_cpu(map->phys_start);
	u32 flags	= le64_to_cpu(map->flags);

	if (!IS_ALIGNED(virt_start | phys_start | (virt_end + 1), granule) ||
	    virt_start >= virt_end || virt_end & ~addr_mask)
		return -ERANGE;

	if (flags & ~VIRTIO_IOMMU_MAP_F_MASK)
		return -EINVAL;

	prot = (flags & VIRTIO_IOMMU_MAP_F_READ ? IOMMU_PROT_READ : 0) |
	       (flags & VIRTIO_IOMMU_MAP_F_WRITE ? IOMMU_PROT_WRITE : 0) |
	       (flags & VIRTIO_IOMMU_MAP_F_MMIO ? IOMMU_PROT_MMIO : 0);

	domain = viommu_find_domain(viommu, domain_id);
	if (!domain)
		return -ESRCH;

	domain_debug(domain, "map %#llx-%#llx -> %#llx", virt_start, virt_end,
		     phys_start);

	ret = domain->ops->map(domain->priv, virt_start, virt_end, phys_start,
			       prot);
	if (!ret) {
		domain->stats.resident += virt_end - virt_start + 1;
		domain->stats.map++;
	}

	return ret;
}

static int viommu_handle_unmap(struct viommu_dev *viommu,
			       struct virtio_iommu_req_unmap *unmap)
{
	int ret;
	struct viommu_domain *domain;
	u64 addr_mask = viommu->addr_mask;

	u32 domain_id	= le32_to_cpu(unmap->domain);
	u64 virt_start	= le64_to_cpu(unmap->virt_start);
	u64 virt_end	= le64_to_cpu(unmap->virt_end);

	if (virt_start >= virt_end || virt_end & ~addr_mask)
		return -ERANGE;

	domain = viommu_find_domain(viommu, domain_id);
	if (!domain)
		return -ESRCH;

	domain_debug(domain, "unmap %#llx-%#llx", virt_start, virt_end);

	ret = domain->ops->unmap(domain->priv, virt_start, virt_end, 0);
	if (!ret) {
		domain->stats.resident -= virt_end - virt_start + 1;
		domain->stats.unmap++;
	}

	return ret;
}

static int viommu_add_msi_window(struct virtio_iommu_req_probe *probe,
				 const struct iommu_properties *properties,
				 size_t *cur)
{
	struct virtio_iommu_probe_resv_mem *prop = (void *)probe->properties + *cur;

	if (*cur + sizeof(*prop) > VIOMMU_DEFAULT_PROBE_SIZE) {
		pr_err("cannot fill probe request, would overflow");
		return -ENOSPC;
	}

	*prop = (struct virtio_iommu_probe_resv_mem) {
		.head.type = cpu_to_le16(VIRTIO_IOMMU_PROBE_T_RESV_MEM),
		.head.length = cpu_to_le16(sizeof(*prop) - sizeof(prop->head)),
		.subtype = VIRTIO_IOMMU_RESV_MEM_T_MSI,
		.start = cpu_to_le64(properties->msi_window.start),
		.end = cpu_to_le64(properties->msi_window.end),
	};

	*cur += sizeof(*prop);

	return 0;
}

static size_t viommu_handle_probe(struct viommu_dev *viommu,
				  struct virtio_iommu_req_probe *probe,
				  ssize_t *written_len)
{
	u32 endpoint_id;
	size_t cur = 0;
	struct device_header *dev;
	const struct iommu_properties *properties;

	endpoint_id = le32_to_cpu(probe->endpoint);

	dev = iommu_get_device(endpoint_id);
	if (IS_ERR_OR_NULL(dev))
		return -ENODEV;

	properties = dev->iommu_ops->get_properties(dev);

	if (properties->msi_window.start)
		viommu_add_msi_window(probe, properties, &cur);

	/*
	 * The parser already filled the rest of the buffer with zeroes, so we
	 * can just pretend we wrote everything.
	 */
	*written_len += virtio_guest_to_host_u32(&viommu->vdev,
						 viommu->config.probe_size);

	return 0;
}

static size_t viommu_get_req_len(struct viommu_dev *viommu,
				 struct virtio_iommu_req_head *head,
				 size_t buf_len)
{
	if (buf_len < sizeof(*head))
		return 0;

	switch (head->type) {
	case VIRTIO_IOMMU_T_ATTACH:
		return sizeof(struct virtio_iommu_req_attach);
	case VIRTIO_IOMMU_T_DETACH:
		return sizeof(struct virtio_iommu_req_detach);
	case VIRTIO_IOMMU_T_MAP:
		return sizeof(struct virtio_iommu_req_map);
	case VIRTIO_IOMMU_T_UNMAP:
		return sizeof(struct virtio_iommu_req_unmap);
	case VIRTIO_IOMMU_T_PROBE:
		return sizeof(struct virtio_iommu_req_probe) +
			VIOMMU_DEFAULT_PROBE_SIZE +
			sizeof(struct virtio_iommu_req_tail);
	default:
		pr_err("unknown request type %x", head->type);
		return 0;
	}
}

static const char *viommu_get_req_name(unsigned int request_type)
{
	switch (request_type) {
	case VIRTIO_IOMMU_T_ATTACH:
		return "attach";
	case VIRTIO_IOMMU_T_DETACH:
		return "detach";
	case VIRTIO_IOMMU_T_MAP:
		return "map";
	case VIRTIO_IOMMU_T_UNMAP:
		return "unmap";
	case VIRTIO_IOMMU_T_PROBE:
		return "probe";
	default:
		return "???";
	}
}

static int viommu_errno_to_status(int err)
{
	switch (err) {
	case 0:
		return VIRTIO_IOMMU_S_OK;
	case EIO:
		return VIRTIO_IOMMU_S_IOERR;
	case ENOSYS:
		return VIRTIO_IOMMU_S_UNSUPP;
	case ERANGE:
		return VIRTIO_IOMMU_S_RANGE;
	case EFAULT:
		return VIRTIO_IOMMU_S_FAULT;
	case EINVAL:
		return VIRTIO_IOMMU_S_INVAL;
	case ENOENT:
	case ENODEV:
	case ESRCH:
		return VIRTIO_IOMMU_S_NOENT;
	case ENOMEM:
	case ENOSPC:
	default:
		return VIRTIO_IOMMU_S_DEVERR;
	}
}

static void viommu_dump_request(struct viommu_dev *viommu,
				struct virtio_iommu_req_head *head,
				size_t len)
{
	size_t i, j;
	size_t limit = 16;
	u8 *buf = (void *)head;

	pr_info("Request %s (0x%x) %zuB", viommu_get_req_name(head->type),
		head->type, len);

	for (i = 0; i < len; i += 16) {
		fprintf(stderr, "%05zx: ", i);

		limit = min_t(size_t, 16, len - i);

		for (j = 0; j < limit; j++)
			fprintf(stderr, "%02x%s", buf[i + j],
				j == 7 ? "  " : j == limit - 1 ? "" : " ");

		fprintf(stderr, "\n");
	}
}

static ssize_t viommu_dispatch_request(struct viommu_dev *viommu, void *buf,
				       size_t len)
{
	u32 *tail;
	int ret, status;
	ssize_t written_len = 0;
	struct virtio_iommu_req_head *head = buf;

	switch (head->type) {
	case VIRTIO_IOMMU_T_ATTACH:
		ret = viommu_handle_attach(viommu, buf);
		break;
	case VIRTIO_IOMMU_T_DETACH:
		ret = viommu_handle_detach(viommu, buf);
		break;
	case VIRTIO_IOMMU_T_MAP:
		ret = viommu_handle_map(viommu, buf);
		break;
	case VIRTIO_IOMMU_T_UNMAP:
		ret = viommu_handle_unmap(viommu, buf);
		break;
	case VIRTIO_IOMMU_T_PROBE:
		ret = viommu_handle_probe(viommu, buf, &written_len);
		break;
	default:
		pr_err("unhandled request %x", head->type);
		ret = -ENOSYS;
	}

	status = viommu_errno_to_status(-ret);

	tail = buf + len - sizeof(*tail);
	/* Fill the reserved bytes as well */
	*tail = cpu_to_le32(status);
	written_len += sizeof(*tail);

	if (viommu->debug_level >= IOMMU_DEBUG_L_VERBOSE) {
		viommu_dump_request(viommu, buf, len);
		pr_info("-> r:%d s:%d w:%zu", ret, status, written_len);
	}

	viommu->stats.requests++;
	return written_len;
}

static ssize_t viommu_parse_request(struct viommu_dev *viommu,
				    struct iovec *iov, u16 dev_rd, u16 dev_wr)
{
	void *buf = viommu->buf;
	size_t read_iov = dev_rd;
	ssize_t len, write_len, read_len, buf_len;

	/* Copy all the device-readable bits */
	len = memcpy_fromiovec_safe(buf, &iov, VIOMMU_MAX_REQ_LEN, &read_iov);
	read_len = VIOMMU_MAX_REQ_LEN - len;
	write_len = iov_size(iov, dev_wr);
	buf_len = read_len + write_len;

	if (read_iov || write_len > len) {
		pr_debug("request overflow!");
		return -EINVAL;
	}

	/* Check if the request matches expected length */
	len = viommu_get_req_len(viommu, buf, buf_len);
	if (len != buf_len) {
		pr_debug("invalid request length (%zd != %zd)", buf_len, len);
		return -EINVAL;
	}

	/* Clear all device-writeable bits (avoid leaking previous requests) */
	memset(buf + read_len, 0, write_len);

	len = viommu_dispatch_request(viommu, buf, buf_len);
	if (WARN_ON(len > write_len)) {
		return -EFAULT;
	} else if (len < 0) {
		pr_debug("failed to parse command (sz=%zd, cmd %x)", buf_len,
			 buf_len > 4 ? *(u32 *)buf : -1U);
		return len;
	} else if (len < write_len) {
		pr_debug("write buffer not fully written (%zd != %zd)", len,
			 write_len);
	}

	/* Write back from the end */
	memcpy_toiovecend(iov, buf + read_len, write_len - len, len);

	return len;
}

static int _viommu_handle_requests(struct kvm *kvm,
				   struct viommu_dev *viommu,
				   struct virt_queue *vq)
{
	u16 head;
	/* Device-write-only descriptors, device-read-only */
	u16 dev_wr, dev_rd;
	ssize_t written_len;

	viommu->stats.kicks++;
	while (virt_queue__available(vq)) {
		head = virt_queue__get_iov(vq, &dev_rd, &dev_wr, kvm);

		written_len = viommu_parse_request(viommu, vq->buf.iov, dev_rd, dev_wr);
		virt_queue__put_iov(vq);
		virt_queue__set_used_elem(vq, head, written_len > 0 ?
					  written_len : 0);
	}

	if (virtio_queue__should_signal(vq))
		viommu->vdev.ops->signal_vq(kvm, &viommu->vdev,
					    VIOMMU_REQUEST_QUEUE);

	return 0;
}

/*
 * When ioeventfd notification is used, the request handler runs in a thread.
 * Otherwise it's just executed by the vCPU thread.
 */
static void *viommu_handle_requests(void *p)
{
	struct virt_queue *vq;
	struct viommu_dev *viommu = p;
	struct kvm *kvm = viommu->kvm;

	kvm__set_thread_name("virtio-iommu-request");

	vq = &viommu->vqs[VIOMMU_REQUEST_QUEUE];

	while (1) {
		if (!virt_queue__available(vq)) {
			mutex_lock(&viommu->request_mutex);
			pthread_cond_wait(&viommu->request_cond,
					  &viommu->request_mutex.mutex);
			mutex_unlock(&viommu->request_mutex);
		}

		_viommu_handle_requests(kvm, viommu, vq);
	}

	return NULL;
}

static int viommu_report_faults_locked(struct viommu_dev *viommu,
				       struct virtio_iommu_fault *faults,
				       size_t nr)
{
	u16 head;
	void *buf;
	size_t i = 0;
	u16 dev_wr, dev_rd;
	struct virt_queue *vq;
	struct kvm *kvm = viommu->kvm;
	size_t len, copied, num_buffers, iovsize;

	vq = &viommu->vqs[VIOMMU_EVENT_QUEUE];

	if (nr < 1)
		return -EINVAL;

	if (!virt_queue__available(vq)) {
		pr_warning("no evt available\n");
		return -ENOSPC;
	}

	len = sizeof(*faults);
	copied = num_buffers = 0;
	buf = &faults[0];

	do {
		while (!virt_queue__available(vq))
			sleep(0);

		head = virt_queue__get_iov(vq, &dev_rd, &dev_wr, kvm);

		iovsize = min_t(size_t, len - copied, iov_size(vq->buf.iov, dev_wr));
		memcpy_toiovec(vq->buf.iov, buf + copied, iovsize);
		copied += iovsize;

		virt_queue__set_used_elem_no_update(vq, head, iovsize, num_buffers++);

		/*
		 * Not enough space in this buffer for the full error. Oh well,
		 * the truncated report should still work.
		 */
		if (copied != len)
			pr_warning("Fault buffer is too small");

		/* Next fault */
		virt_queue__put_iov(vq);
		virt_queue__used_idx_advance(vq, num_buffers);

		copied = num_buffers = 0;
		buf = &faults[++i];

		if (virtio_queue__should_signal(vq))
			viommu->vdev.ops->signal_vq(kvm, &viommu->vdev,
						    VIOMMU_EVENT_QUEUE);
	} while (i < nr);

	return 0;
}

static int viommu_report_faults(struct viommu_dev *viommu,
				struct virtio_iommu_fault *faults, size_t nr)
{
	int ret;

	mutex_lock(&viommu->event_mutex);
	ret = viommu_report_faults_locked(viommu, faults, nr);
	mutex_unlock(&viommu->event_mutex);

	return ret;
}

/**
 * viommu_report_fault - inject a single fault into the guest
 *
 * Add a fault report to the virtio event queue
 * @dev: the device causing the fault
 * @reason: fault reason
 * @address: Faulting address. -1UL means "no address"
 * @prot: protection flags of the access
 */
int viommu_report_fault(struct device_header *dev,
			enum iommu_fault_reason reason, unsigned long address,
			int prot)
{
	int flags;
	struct viommu_endpoint *vdev = dev->iommu_data;
	struct virtio_iommu_fault fault = {
		.address	= cpu_to_le64(address),
	};

	if (!vdev)
		return -ENODEV;

	switch (reason) {
	case IOMMU_FAULT_UNKNOWN:
		fault.reason = VIRTIO_IOMMU_FAULT_R_UNKNOWN;
		break;
	case IOMMU_FAULT_DOMAIN:
		fault.reason = VIRTIO_IOMMU_FAULT_R_DOMAIN;
		break;
	case IOMMU_FAULT_MAPPING:
		fault.reason = VIRTIO_IOMMU_FAULT_R_MAPPING;
		break;
	};

	flags = (prot & IOMMU_PROT_READ ? VIRTIO_IOMMU_FAULT_F_READ : 0) |
		(prot & IOMMU_PROT_WRITE ? VIRTIO_IOMMU_FAULT_F_WRITE : 0) |
		(address != -1UL ? VIRTIO_IOMMU_FAULT_F_ADDRESS : 0);

	fault.flags	= cpu_to_le32(flags);
	fault.endpoint	= cpu_to_le32(device_to_iommu_id(dev));

	return viommu_report_faults(vdev->viommu, &fault, 1);
}

/* Virtio API */
static u8 *viommu_get_config(struct kvm *kvm, void *dev)
{
	struct viommu_dev *viommu = dev;

	return (u8 *)&viommu->config;
}

static u32 viommu_get_host_features(struct kvm *kvm, void *dev)
{
	return 1ULL << VIRTIO_RING_F_EVENT_IDX
	     | 1ULL << VIRTIO_RING_F_INDIRECT_DESC
	     | 1ULL << VIRTIO_IOMMU_F_MAP_UNMAP
	     | 1ULL << VIRTIO_IOMMU_F_INPUT_RANGE
	     | 1ULL << VIRTIO_IOMMU_F_PROBE
	     | 1ULL << VIRTIO_IOMMU_F_MMIO;
}

static void viommu_notify_status(struct kvm *kvm, void *dev, u32 status)
{
	struct viommu_dev *viommu = dev;
	struct virtio_device *vdev = &viommu->vdev;

	/* Avoid warning when endianess helpers are compiled out */
	vdev = vdev;

	if (status & VIRTIO__STATUS_SWAB) {
		viommu->config.page_size_mask = virtio_host_to_guest_u64(vdev,
						 viommu->config.page_size_mask);
		viommu->config.input_range.start = virtio_host_to_guest_u64(vdev,
						 viommu->config.input_range.start);
		viommu->config.input_range.end = virtio_host_to_guest_u64(vdev,
						 viommu->config.input_range.end);
		viommu->config.probe_size = virtio_host_to_guest_u32(vdev,
					    viommu->config.probe_size);
	}

	if (status & VIRTIO__STATUS_STOP)
		viommu_free_all_endpoints(viommu);
}

static int viommu_init_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct viommu_dev *viommu = dev;

	if (vq >= VIOMMU_NR_VQS)
		return -ENODEV;

	compat__remove_message(compat_id);

	virtio_init_device_vq(kvm, &viommu->vdev, &viommu->vqs[vq],
			      VIOMMU_DEFAULT_QUEUE_SIZE);

	if (viommu->ioeventfd && vq == VIOMMU_REQUEST_QUEUE) {
		pthread_cond_init(&viommu->request_cond, NULL);
		mutex_init(&viommu->request_mutex);
		pthread_create(&viommu->request_thread, NULL,
			       viommu_handle_requests, viommu);
	}

	return 0;
}

static void viommu_exit_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct viommu_dev *viommu = dev;

	if (viommu->ioeventfd && vq == VIOMMU_REQUEST_QUEUE) {
		pthread_cancel(viommu->request_thread);
		pthread_join(viommu->request_thread, NULL);
	}
}

static struct virt_queue *viommu_get_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct viommu_dev *viommu = dev;

	if (vq >= VIOMMU_NR_VQS)
		return NULL;

	return &viommu->vqs[vq];
}

static int viommu_get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	/* FIXME: dynamic */
	return VIOMMU_DEFAULT_QUEUE_SIZE;
}

static int viommu_set_size_vq(struct kvm *kvm, void *dev, u32 vq, int size)
{
	/* FIXME: dynamic */
	return size;
}

static int viommu_notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct viommu_dev *viommu = dev;

	if (vq != VIOMMU_REQUEST_QUEUE)
		return 0;

	if (viommu->ioeventfd) {
		mutex_lock(&viommu->request_mutex);
		pthread_cond_signal(&viommu->request_cond);
		mutex_unlock(&viommu->request_mutex);
	} else {
		_viommu_handle_requests(kvm, viommu,
					&viommu->vqs[VIOMMU_REQUEST_QUEUE]);
	}

	return 0;
}

static void viommu_notify_vq_gsi(struct kvm *kvm, void *dev, u32 vq, u32 gsi)
{
	/* TODO: when implementing vhost */
}

static void viommu_notify_vq_eventfd(struct kvm *kvm, void *dev, u32 vq, u32 fd)
{
	/* TODO: when implementing vhost */
}

static int viommu_get_vq_count(struct kvm *kvm, void *dev)
{
	return VIOMMU_NR_VQS;
}

static const struct virtio_ops iommu_dev_virtio_ops = {
	.get_config		= viommu_get_config,
	.get_host_features	= viommu_get_host_features,
	.init_vq		= viommu_init_vq,
	.exit_vq		= viommu_exit_vq,
	.get_vq_count		= viommu_get_vq_count,
	.get_vq			= viommu_get_vq,
	.get_size_vq		= viommu_get_size_vq,
	.set_size_vq		= viommu_set_size_vq,
	.notify_vq		= viommu_notify_vq,
	.notify_vq_gsi		= viommu_notify_vq_gsi,
	.notify_vq_eventfd	= viommu_notify_vq_eventfd,
	.notify_status		= viommu_notify_status,
};

static const char *iommu_usage =
"--viommu bus[,opt[,opt...]]\n"
"  bus describes the subsystem that will be managed by the IOMMU.           \n"
"    Possible values are: vfio, virtio.                                     \n"
"  opt may be:                                                              \n"
"  * pci or mmio: override default transport                                \n"
"  * sw-msi: MSIs do not bypass IOMMU translation.                          \n"
"  * debug=<level>: set debug level ('none', 'print', 'flood')              \n"
"  * ioeventfd: use ioeventfd and thread sync (slower)                       ";

int viommu_bus_parser(const struct option *opt, const char *arg, int unset)
{
	int ret = -EINVAL;
	const char *instance;
	char *cur, *buf = strdup(arg);
	struct kvm *kvm = (void *)opt->ptr;
	struct iommu_config cfg = { 0 };

	cur = strtok(buf, ",");
	if (strncmp(cur, "vfio", 4) == 0) {
		instance = "viommu-vfio";
	} else if (strncmp(cur, "virtio", 6) == 0) {
		instance = "viommu-virtio";
	} else {
		pr_err("invalid IOMMU '%s'\n usage: %s", cur, iommu_usage);
		goto out_free;
	}

	while ((cur = strtok(NULL, ",")) != NULL) {
		if (strncmp(cur, "debug", 5) == 0) {
			/* Format is debug[=none|=print|=flood] */
			if (!cur[5]) {
				cfg.debug_level = IOMMU_DEBUG_L_INFO;
				continue;
			}

			if (strcmp(cur + 5, "=none") == 0) {
				cfg.debug_level = IOMMU_DEBUG_L_DISABLED;
			} else if (strcmp(cur + 5, "=flood") == 0) {
				cfg.debug_level = IOMMU_DEBUG_L_VERBOSE;
			} else if (strcmp(cur + 5, "=print") == 0) {
				cfg.debug_level = IOMMU_DEBUG_L_INFO;
			} else {
				pr_err("invalid debug argument '%s'", cur + 5);
				goto out_free;
			}

			continue;
		}

		if (strcmp(cur, "sw-msi") == 0) {
			cfg.sw_msi = true;
			continue;
		}

		if (strcmp(cur, "pci") == 0) {
			cfg.transport = "pci";
			continue;
		} else if (strcmp(cur, "mmio") == 0) {
			cfg.transport = "mmio";
			continue;
		}

		if (strcmp(cur, "ioeventfd") == 0) {
			cfg.ioeventfd = true;
			continue;
		}

		pr_err("invalid IOMMU option '%s'\n usage: %s", cur,
		       iommu_usage);
		goto out_free;
	}

	cfg.name = strdup(instance);
	if (!cfg.name) {
		ret = -ENOMEM;
		goto out_free;
	}

	kvm->cfg.num_viommus++;
	kvm->cfg.viommus = realloc(kvm->cfg.viommus, kvm->cfg.num_viommus *
				   sizeof(cfg));
	if (!kvm->cfg.viommus)
		die("Failed adding a new vIOMMU");
	kvm->cfg.viommus[kvm->cfg.num_viommus - 1] = cfg;
	ret = 0;

out_free:
	free(buf);
	return ret;
}

int viommu_update_config(void *dev, struct iommu_properties *props)
{
	bool changed = false;
	struct viommu_dev *viommu = dev;
	struct virtio_device *vdev = &viommu->vdev;
	u64 pgsize_mask = props->pgsize_mask ?: ~((u64)PAGE_SIZE - 1);
	u64 end_addr = props->input_addr_size % BITS_PER_LONG ?
		       (1UL << props->input_addr_size) - 1 : -1UL;

	vdev = vdev;
	changed |= pgsize_mask !=
		   virtio_guest_to_host_u64(vdev, viommu->config.page_size_mask);
	changed |= end_addr !=
		   virtio_guest_to_host_u64(vdev, viommu->config.input_range.end);

	if (!changed)
		return 0;
	/*
	 * TODO: notify guest of config change if necessary. (The case hasn't
	 * come up yet, the function is only used before probe for now.)
	 */

	viommu->config.page_size_mask = virtio_host_to_guest_u64(vdev, pgsize_mask);
	viommu->config.input_range.end = virtio_host_to_guest_u64(vdev, end_addr);
	viommu->addr_mask = end_addr;
	viommu->granularity = __builtin_ffs(pgsize_mask);

	viommu->config.probe_size = virtio_host_to_guest_u64(vdev, VIOMMU_DEFAULT_PROBE_SIZE);

	return 0;
}

#ifdef CONFIG_HAS_LIBFDT
struct of_pci_reg {
	struct of_pci_unit_address	addr;
	u32				size_hi, size_lo;
} __attribute__((packed));

void pci__generate_iommu_fdt_nodes(void *fdt)
{
	struct of_pci_reg reg = {0};
	struct viommu_dev *viommu;
	struct device_header *dev_hdr;

	mutex_lock(&viommus_mutex);
	list_for_each_entry(viommu, &viommus, list) {
		if (viommu->transport != VIRTIO_PCI)
			continue;

		dev_hdr = viommu->vdev.dev;
		reg.addr.hi = cpu_to_fdt32(of_pci_b_ddddd(dev_hdr->dev_num));

		_FDT(fdt_begin_node(fdt, viommu->properties->name));
		_FDT(fdt_property_string(fdt, "compatible", "virtio,pci-iommu"));
		_FDT(fdt_property(fdt, "reg", &reg, sizeof(reg)));
		_FDT(fdt_property_cell(fdt, "#iommu-cells", 0x1));
		_FDT(fdt_property_cell(fdt, "phandle", viommu->properties->phandle));
		_FDT(fdt_end_node(fdt));
	}
	mutex_unlock(&viommus_mutex);
}

void viommu_mmio_generate_fdt_props(void *fdt, struct device_header *dev_hdr)
{
	u32 iommus_prop[2];
	const struct iommu_properties *props;
	struct viommu_dev *viommu = NULL, *tmp;

	mutex_lock(&viommus_mutex);
	list_for_each_entry(tmp, &viommus, list) {
		if (tmp->vdev.dev == dev_hdr) {
			viommu = tmp;
			break;
		}
	}
	mutex_unlock(&viommus_mutex);

	/* If the device is an IOMMU, it has phandle and iommu-cells properties */
	if (viommu) {
		_FDT(fdt_property_cell(fdt, "phandle",
				       viommu->properties->phandle));
		_FDT(fdt_property_cell(fdt, "#iommu-cells", 1));
		return;
	}

	if (!dev_hdr->iommu_ops)
		return;

	/* Otherwise it has an iommus property */
	props = dev_hdr->iommu_ops->get_properties(dev_hdr);

	iommus_prop[0] = cpu_to_fdt32(props->phandle);
	iommus_prop[1] = cpu_to_fdt32(device_to_iommu_id(dev_hdr));
	_FDT(fdt_property(fdt, "iommus", iommus_prop, sizeof(iommus_prop)));
}
#endif

void *viommu_register(struct kvm *kvm, struct iommu_properties *props)
{
	int ret;
	int i = 0;
	struct iommu_config *cfg;
	struct viommu_dev *viommu;
	struct kvm_msi_doorbell *doorbell = irq__get_msi_doorbell(kvm);

	/* Find user configuration for this IOMMU, passed on the command line */
	for (i = 0; i < kvm->cfg.num_viommus; i++) {
		cfg = &kvm->cfg.viommus[i];
		if (cfg->name && strcmp(cfg->name, props->name) == 0)
			break;
	}

	if (i == kvm->cfg.num_viommus)
		return NULL;

	props->phandle = fdt_alloc_phandle();

	viommu = calloc(1, sizeof(struct viommu_dev));
	if (!viommu)
		return ERR_PTR(-ENOMEM);

	viommu->kvm			= kvm;
	viommu->domains			= (struct rb_root)RB_ROOT;
	viommu->endpoints_mutex		= (struct mutex)MUTEX_INITIALIZER;
	viommu->domains_mutex		= (struct mutex)MUTEX_INITIALIZER;
	viommu->event_mutex		= (struct mutex)MUTEX_INITIALIZER;
	viommu->properties		= props;
	viommu->ops			= iommu_dev_virtio_ops;
	viommu->transport		= VIRTIO_DEFAULT_TRANS(kvm);
	viommu->debug_level		= cfg->debug_level;
	viommu->ioeventfd		= cfg->ioeventfd;
	INIT_LIST_HEAD(&viommu->endpoints);

	if (cfg->transport) {
		if (strcmp(cfg->transport, "pci") == 0)
			viommu->transport = VIRTIO_PCI;
		else if (strcmp(cfg->transport, "mmio") == 0)
			viommu->transport = VIRTIO_MMIO;
	}

	if (viommu->transport == VIRTIO_MMIO_LEGACY ||
	    viommu->transport == VIRTIO_PCI_LEGACY) {
		pr_err("cannot use legacy transport for virtio-iommu");
		return ERR_PTR(-EINVAL);
	}

	if (!cfg->sw_msi && doorbell) {
		props->msi_window.start = doorbell->addr;
		props->msi_window.end = doorbell->addr + doorbell->size - 1;
	}

	ret = virtio_init(kvm, viommu, &viommu->vdev, &viommu->ops,
			  viommu->transport, 0, VIRTIO_ID_IOMMU,
			  PCI_CLASS_IOMMU);
	if (ret) {
		free(viommu);
		return ERR_PTR(ret);
	}

	viommu_update_config(viommu, props);
	viommu->vdev.no_eventfd = !cfg->ioeventfd;

	mutex_lock(&viommus_mutex);
	viommu->id = viommu_ids++;
	list_add_tail(&viommu->list, &viommus);
	mutex_unlock(&viommus_mutex);

	pr_info("Loaded virtual IOMMU %s with %s transport", props->name,
		virtio_trans_name(viommu->transport));

	if (compat_id == -1)
		compat_id = virtio_compat_add_message("virtio-iommu",
						      "CONFIG_VIRTIO_IOMMU");

	return viommu;
}

void viommu_unregister(struct kvm *kvm, void *dev)
{
	struct viommu_dev *viommu = dev;

	mutex_lock(&viommus_mutex);
	list_del(&viommu->list);
	mutex_unlock(&viommus_mutex);
	viommu_free_all_endpoints(viommu);
	free(viommu);
}

const char *debug_usage =
"  list [iommu [domain]]       list iommus and domains\n"
"  stat [iommu [domain]]       display statistics\n"
"  dump [iommu [domain]]       dump mappings\n"
"  print [iommu [domain]]      enable debug print\n"
"  flood [iommu [domain]]      enable *verbose* debug print\n"
"  noprint [iommu [domain]]    disable debug print\n"
"  fault [iommu [num [reason   inject a fault\n"
"         [endpoint [addr]]]]]\n"
;

int viommu_parse_debug_string(const char *cmdline, struct iommu_debug_params *params)
{
	int pos = 0;
	int ret = -EINVAL;
	unsigned long num;
	char *cur, *args = strdup(cmdline);
	params->action = IOMMU_DEBUG_NUM_ACTIONS;

	if (!args)
		return -ENOMEM;

	params->selector[0] = IOMMU_DEBUG_SELECTOR_INVALID;
	params->selector[1] = IOMMU_DEBUG_SELECTOR_INVALID;

	cur = strtok(args, " ,:");
	while (cur) {
		if (pos > 0 && params->action == IOMMU_DEBUG_FAULT) {
			/* Parse fault parameters */
			errno = 0;
			num = strtoul(cur, NULL, 0);
			if (errno) {
				ret = -errno;
				pr_err("Invalid number '%s'", cur);
				break;
			}

			if (pos == 1) {
				params->selector[0] = num;
			} else if (pos == 2) {
				params->fault.num = num;
			} else if (pos == 3) {
				params->fault.reason = num;
			} else if (pos == 4) {
				params->fault.endpoint = num;
			} else if (pos == 5) {
				params->fault.addr = num;
			} else {
				ret = -EINVAL;
				pr_err("Too many parameters");
				break;
			}
		} else if (pos > 0) {
			/* Parse parameters for any other command */
			if (pos > 2) {
				ret = -EINVAL;
				pr_err("Too many parameters");
				break;
			}

			errno = 0;
			params->selector[pos - 1] = strtoul(cur, NULL, 0);
			if (errno) {
				ret = -errno;
				pr_err("Invalid number '%s'", cur);
				break;
			}

			/* Parse command */
		} else if (strncmp(cur, "list", 4) == 0) {
			params->action = IOMMU_DEBUG_LIST;
		} else if (strncmp(cur, "stat", 5) == 0) {
			params->action = IOMMU_DEBUG_STATS;
		} else if (strncmp(cur, "dump", 4) == 0) {
			params->action = IOMMU_DEBUG_DUMP;
		} else if (strncmp(cur, "flood", 5) == 0) {
			params->action = IOMMU_DEBUG_SET_PRINT;
			params->debug_level = IOMMU_DEBUG_L_VERBOSE;
		} else if (strncmp(cur, "print", 5) == 0) {
			params->action = IOMMU_DEBUG_SET_PRINT;
			params->debug_level = IOMMU_DEBUG_L_INFO;
		} else if (strncmp(cur, "noprint", 7) == 0 ||
			   strncmp(cur, "noflood", 7) == 0) {
			params->action = IOMMU_DEBUG_SET_PRINT;
			params->debug_level = IOMMU_DEBUG_L_DISABLED;
		} else if (strncmp(cur, "fault", 5) == 0) {
			params->action = IOMMU_DEBUG_FAULT;
		} else {
			pr_err("Invalid command '%s'", cur);
			break;
		}

		cur = strtok(NULL, " ,:");
		pos++;
		ret = 0;
	}

	free(args);

	if (cur && cur[0])
		pr_err("Ignoring argument '%s'", cur);

	if (ret)
		pr_info("Usage:\n%s", debug_usage);

	return ret;
}

struct viommu_debug_context {
	int				sock;
	struct iommu_debug_params	*params;
	bool				disp;
};

static int viommu_debug_domain(struct viommu_dev *viommu,
			       struct viommu_domain *domain,
			       void *data)
{
	int ret = 0;
	struct viommu_endpoint *vdev;
	struct viommu_debug_context *ctx = data;

	if (ctx->disp)
		dprintf(ctx->sock, "  domain %u\n", domain->id);

	switch (ctx->params->action) {
	case IOMMU_DEBUG_LIST:
		mutex_lock(&domain->endpoints_mutex);
		list_for_each_entry(vdev, &domain->endpoints, list) {
			dprintf(ctx->sock, "    endpoint 0x%lx\n",
				device_to_iommu_id(vdev->dev));
		}
		mutex_unlock(&domain->endpoints_mutex);
		break;
	case IOMMU_DEBUG_STATS:
		dprintf(ctx->sock, "    maps                %llu\n",
			domain->stats.map);
		dprintf(ctx->sock, "    unmaps              %llu\n",
			domain->stats.unmap);
		dprintf(ctx->sock, "    resident            %llu\n",
			domain->stats.resident);
		break;
	case IOMMU_DEBUG_SET_PRINT:
		domain->debug_level = ctx->params->debug_level;
		break;
	default:
		ret = -ENOSYS;

	}

	if (domain->ops->debug_domain)
		ret = domain->ops->debug_domain(domain->priv, ctx->sock,
						ctx->params);

	return ret;
}

/* Inject fake faults, to check if the plumbing holds under pressure */
static int viommu_debug_faults(struct viommu_dev *viommu,
			       struct iommu_debug_fault *params)
{
	int ret;
	size_t i;
	size_t nr_faults;
	unsigned long address;
	unsigned int flags = 0;
	struct virtio_iommu_fault *faults;

	nr_faults = params->num;
	if (!nr_faults)
		nr_faults = 1;

	faults = calloc(nr_faults, sizeof(*faults));
	if (!faults)
		return -ENOMEM;

	address = params->addr;
	if (address != -1UL)
		flags |= VIRTIO_IOMMU_FAULT_F_ADDRESS;

	srand48(address);

	for (i = 0; i < nr_faults; i++) {
		/* Pick random prot flags */
		flags &= ~0xff;
		flags |= address >> 16 & 0x7;

		faults[i] = (struct virtio_iommu_fault) {
			.reason		= params->reason,
			.endpoint	= cpu_to_le32(params->endpoint),
			.flags		= cpu_to_le32(flags),
			.address	= cpu_to_le64(address),
		};

		address = lrand48();
	}

	ret = viommu_report_faults(viommu, faults, nr_faults);

	free(faults);

	return ret;
}

static int viommu_debug_iommu(struct viommu_dev *viommu,
			      struct viommu_debug_context *ctx)
{
	struct viommu_domain *domain;

	if (ctx->disp)
		dprintf(ctx->sock, "iommu %u \"%s\"\n", viommu->id,
			viommu->properties->name);

	if (ctx->params->selector[1] != IOMMU_DEBUG_SELECTOR_INVALID) {
		domain = viommu_find_domain(viommu, ctx->params->selector[1]);
		return domain ? viommu_debug_domain(viommu, domain, ctx) : -ESRCH;
	}

	switch (ctx->params->action) {
	case IOMMU_DEBUG_STATS:
		dprintf(ctx->sock, "  kicks                 %llu\n",
			viommu->stats.kicks);
		dprintf(ctx->sock, "  requests              %llu\n",
			viommu->stats.requests);
		break;
	case IOMMU_DEBUG_SET_PRINT:
		viommu->debug_level = ctx->params->debug_level;
		break;
	case IOMMU_DEBUG_FAULT:
		return viommu_debug_faults(viommu, &ctx->params->fault);
	default:
		break;
	}

	return viommu_for_each_domain(viommu, viommu_debug_domain, ctx);
}

int viommu_debug(struct kvm *kvm, int sock, struct iommu_debug_params *params)
{
	int ret = -ESRCH;
	bool match;
	struct viommu_dev *viommu;
	bool any = (params->selector[0] == IOMMU_DEBUG_SELECTOR_INVALID);

	struct viommu_debug_context ctx = {
		.sock		= sock,
		.params		= params,
	};

	if (params->action == IOMMU_DEBUG_LIST ||
	    params->action == IOMMU_DEBUG_STATS)
		ctx.disp = true;

	mutex_lock(&viommus_mutex);
	list_for_each_entry(viommu, &viommus, list) {
		match = (params->selector[0] == viommu->id);
		if (match || any) {
			ret = viommu_debug_iommu(viommu, &ctx);
			if (ret || match)
				break;
		}
	}
	mutex_unlock(&viommus_mutex);

	if (ret)
		dprintf(sock, "error: %s\n", strerror(-ret));

	return ret;
}
