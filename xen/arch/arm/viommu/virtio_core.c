#include <linux/virtio_ids.h>
#include <linux/virtio_ring.h>
#include <linux/types.h>
#include <sys/uio.h>
#include <stdbool.h>
#include <stdlib.h>

#include <linux/err.h>

#include "kvm/guest_compat.h"
#include "kvm/barrier.h"
#include "kvm/iommu.h"
#include "kvm/virtio.h"
#include "kvm/virtio-iommu.h"
#include "kvm/virtio-pci.h"
#include "kvm/virtio-mmio.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

static void *iommu = NULL;
static struct iommu_properties iommu_props = {
	.name			= "viommu-virtio",
	/*
	 * Note that legacy virtio devices aren't supported. Otherwise this
	 * would be 44, since in legacy virtio, vring addresses are defined by
	 * 32-bit PFNs + 12-bit granule.
	 */
	.input_addr_size	= 64,
};

const char* virtio_trans_name(enum virtio_trans trans)
{
	if (trans == VIRTIO_PCI || trans == VIRTIO_PCI_LEGACY)
		return "pci";
	else if (trans == VIRTIO_MMIO || trans == VIRTIO_MMIO_LEGACY)
		return "mmio";
	return "unknown";
}

static int virt_queue__realloc_iov(struct virt_buf *buf, size_t cnt)
{
	size_t i;

	if (cnt <= buf->cnt)
		return 0;

	buf->iov = realloc(buf->iov, cnt * sizeof(*buf->iov));
	if (!buf->iov)
		return -ENOMEM;

	for (i = buf->cnt; i < cnt; i++) {
		buf->iov[i].iov_base = NULL;
		buf->iov[i].iov_len = 0;
	}
	buf->cnt = cnt;

	return 0;
}

static void *virt_queue_access(struct kvm *kvm, struct virtio_device *vdev,
			       u64 addr, size_t size, size_t *out_size, int prot,
			       struct virt_buf *buf)
{
	u64 paddr = 0;

	if (!vdev->use_iommu) {
		*out_size = size;
		paddr = addr;
	} else {
		struct iommu_tlb_entry *tlbe;

		tlbe = iommu_access(vdev->dev, vdev->iommu_domain, addr, size,
				    prot);
		if (!tlbe)
			return NULL;
		*out_size = tlbe->virt_end - tlbe->virt_start + 1;
		paddr = tlbe->phys;

		/*
		 * Set the owner of this TLB entry, so that we can invalidate
		 * the buffer in virtio_invalidate_mappings.
		 */
		tlbe->dev = buf;
	}

	return guest_flat_to_host(kvm, paddr);
}

/*
 * Access element in a virtio structure. If @buf is empty, access is linear and
 * @ptr represents a Host-Virtual Address (HVA).
 *
 * Otherwise, the structure is scattered in the guest-physical space, and is
 * made virtually-contiguous by the virtual IOMMU. @buf describes the
 * structure's IOVA->HVA fragments, @base is the IOVA of the structure, and @ptr
 * an IOVA inside the structure.
 *
 *                                        HVA
 *                      IOVA      .----> +---+ buf->iov[0].base
 *              @base-> +---+ ----'      |   |
 *                      |   |            +---+
 *                      +---+ ----.      :   :
 *                      |   |     '----> +---+ buf->iov[1].base
 *               @ptr-> |   |            |   |
 *                      +---+            |   |--> out
 *                                       +---+
 */
void *virtio_access_buf(struct virt_buf *buf, void *base, void *ptr)
{
	size_t i;
	size_t off = ptr - base;

	if (!buf || !buf->iov)
		return ptr;

	for (i = 0; i < buf->cnt; i++) {
		size_t sz = buf->iov[i].iov_len;
		if (off < sz)
			return buf->iov[i].iov_base + off;
		off -= sz;
	}

	/* We're going to die momentarily, so dump as much info as we can. */
	pr_err("virtio_access_buf overflow");
	pr_err(" trying to access %p from base %p (off=0x%lx)", ptr, base,
	       (long)(ptr - base));
	for (i = 0; i < buf->cnt; i++)
		pr_err(" iov[%zu] = %p 0x%zx", i, buf->iov[i].iov_base,
		       buf->iov[i].iov_len);
	return NULL;
}

/*
 * Fill @buf starting at index @start_vec with translations of the (@addr,
 * @size) range. If @vdev doesn't have an IOMMU, fill a single vector with the
 * corresponding HVA. Otherwise, fill vectors with GVA->GPA->HVA translations.
 * Since the IOVA range may span over multiple IOMMU mappings, there may need to
 * be multiple vectors.
 *
 * If the vector isn't big enough to contain all translations, return -EAGAIN,
 * allowing the caller to reallocate the vector and retry. Otherwise return the
 * number of vectors filled.
 */
static int virtio_populate_buf(struct kvm *kvm, struct virtio_device *vdev,
			      u64 addr, size_t size, int prot, u16 start_vec,
			      struct virt_buf *buf)
{
	void *ptr;
	size_t vec = start_vec;
	size_t consumed = 0;

	while (size > 0 && vec < buf->cnt) {
		ptr = virt_queue_access(kvm, vdev, addr, size, &consumed, prot,
					buf);
		if (!ptr)
			break;

		buf->iov[vec].iov_len = consumed;
		buf->iov[vec].iov_base = ptr;

		size -= consumed;
		addr += consumed;
		vec++;
	}

	if (vec == buf->cnt && size)
		return -EAGAIN;

	return vec - start_vec;
}

void virt_queue__used_idx_advance(struct virt_queue *queue, u16 jump)
{
	u16 *ptr;
	u16 idx;

	virt_queue__get_buf(queue, &queue->used_buf);
	ptr = vring_get_used_ptr(queue, &queue->vring.used->idx);
	idx = virtio_guest_to_host_u16(queue, *ptr);

	/*
	 * Use wmb to assure that used elem was updated with head and len.
	 * We need a wmb here since we can't advance idx unless we're ready
	 * to pass the used element to the guest.
	 */
	wmb();
	idx += jump;
	*ptr = virtio_host_to_guest_u16(queue, idx);
	virt_queue__put_buf(queue, &queue->used_buf);
}

struct vring_used_elem *
virt_queue__set_used_elem_no_update(struct virt_queue *queue, u32 head,
				    u32 len, u16 offset)
{
	struct vring_used_elem *used_elem;
	u16 *ptr;
	u16 idx;

	virt_queue__get_buf(queue, &queue->used_buf);
	ptr = vring_get_used_ptr(queue, &queue->vring.used->idx);
	idx = virtio_guest_to_host_u16(queue, *ptr);
	idx = (idx + offset) % queue->vring.num;

	used_elem	= vring_get_used_ptr(queue, &queue->vring.used->ring[idx]);
	used_elem->id	= virtio_host_to_guest_u32(queue, head);
	used_elem->len	= virtio_host_to_guest_u32(queue, len);
	virt_queue__put_buf(queue, &queue->used_buf);

	return used_elem;
}

struct vring_used_elem *virt_queue__set_used_elem(struct virt_queue *queue, u32 head, u32 len)
{
	struct vring_used_elem *used_elem;

	used_elem = virt_queue__set_used_elem_no_update(queue, head, len, 0);
	virt_queue__used_idx_advance(queue, 1);

	return used_elem;
}

static inline bool virt_desc__test_flag(struct virt_queue *vq,
					struct vring_desc *desc, u16 flag)
{
	return !!(virtio_guest_to_host_u16(vq, desc->flags) & flag);
}

/*
 * Each buffer in the virtqueues is actually a chain of descriptors.  This
 * function returns the next descriptor in the chain, or max if we're at the
 * end.
 */
static unsigned next_desc(struct virt_queue *vq, struct vring_desc *desc,
			  unsigned int max)
{
	unsigned int next;

	/* If this descriptor says it doesn't chain, we're done. */
	if (!virt_desc__test_flag(vq, desc, VRING_DESC_F_NEXT))
		return max;

	next = virtio_guest_to_host_u16(vq, desc->next);

	/* Ensure they're not leading us off end of descriptors. */
	return min(next, max);
}

/*
 * Preload a virt_buf with IOMMU mappings. Used for structures that are accessed
 * multiple times: vring and indirect descriptor table. If an IOMMU is present,
 * (re)allocate the IOV and fill it with IOVA->HVA translations. Return the
 * table IOVA. Otherwise return the HVA directly, don't do anything with @buf.
 */
static void *virtio_map_table(struct kvm *kvm, struct virtio_device *vdev,
			      unsigned long addr, size_t len, int prot,
			      struct virt_buf *buf)
{
	int ret;
	size_t iov_len;

	if (!vdev->use_iommu)
		return guest_flat_to_host(kvm, addr);

	/*
	 * Virtio structure need to be accessed through the IOMMU, mapped at
	 * PAGE_SIZE granularity. Reuse the iov if available, and populate it
	 * with address translations of the given table. Sometimes the driver
	 * puts the indirect table at an offset, and it crosses page boundary.
	 * Add one page for safety.
	 */
	iov_len = DIV_ROUND_UP(len, PAGE_SIZE) + 1;

	ret = virt_queue__realloc_iov(buf, iov_len);
	if (ret)
		return NULL;

	ret = virtio_populate_buf(kvm, vdev, addr, len, prot, 0, buf);
	if (ret <= 0) {
		pr_err("failed to map table");
		return NULL;
	}

	buf->valid = true;
	return (void *)addr;
}

static int __virt_queue__get_head_iov(struct virt_queue *vq, struct virt_buf *buf,
				      u16 *out, u16 *in, u16 head, struct kvm *kvm)
{
	struct vring_desc *desc_base, *desc;
	struct virt_buf *desc_buf;
	unsigned long addr;
	bool is_write;
	size_t len;
	u16 idx;
	u16 max;
	int ret;

	idx = head;
	*out = *in = 0;
	max = vq->vring.num;
	desc_base = vq->vring.desc;
	desc_buf = virt_queue__get_buf(vq, &vq->desc_buf);
	desc = vring_get_desc_ptr(vq, &desc_base[idx]);

	if (virt_desc__test_flag(vq, desc, VRING_DESC_F_INDIRECT)) {
		idx = 0;
		len = virtio_guest_to_host_u32(vq, desc->len);
		addr = virtio_guest_to_host_u64(vq, desc->addr);

		max = len / sizeof(struct vring_desc);
		WARN_ON(len % sizeof(struct vring_desc));

		virt_queue__put_buf(vq, desc_buf);
		desc_buf = virt_queue__get_buf(vq, &vq->indirect_buf);
		desc_base = virtio_map_table(kvm, vq->vdev, addr, len,
					     PROT_READ, desc_buf);
		if (!desc_base) {
			ret = -EFAULT;
			goto out_put_buf;
		}
	}

	do {
		desc = virtio_access_buf(desc_buf, desc_base, &desc_base[idx]);
		is_write = virt_desc__test_flag(vq, desc, VRING_DESC_F_WRITE);

		/* Grab the first descriptor, and check it's OK. */
		len = virtio_guest_to_host_u32(vq, desc->len);
		addr = virtio_guest_to_host_u64(vq, desc->addr);

		ret = virtio_populate_buf(kvm, vq->vdev, addr, len, is_write ?
					  IOMMU_PROT_WRITE : IOMMU_PROT_READ,
					  *out + *in, buf);
		if (ret < 0)
			goto out_put_buf;

		/* If this is an input descriptor, increment that count. */
		if (is_write)
			(*in) += ret;
		else
			(*out) += ret;
	} while ((idx = next_desc(vq, desc, max)) != max);

	ret = head;
out_put_buf:
	virt_queue__put_buf(vq, desc_buf);
	return ret;
}

/*
 * Return a head >= 0 on success, and keep a reference to the buffer. Caller
 * must release it with virt_queue__put_iov.
 *
 * On failure, return an error < 0
 */
int virt_queue__get_head_iov(struct virt_queue *vq, struct virt_buf *buf,
			     u16 *out, u16 *in, u16 head, struct kvm *kvm)
{
	int ret = -ENOMEM;

	virt_queue__get_buf(vq, buf);
	/*
	 * Initialize the buffer if necessary. When no IOMMU is present, the iov
	 * can contain at most one entry per vring descriptor
	 */
	if (virt_queue__realloc_iov(buf, vq->vring.num))
		goto err_put_buf;

	do {
		ret = __virt_queue__get_head_iov(vq, buf, out, in, head, kvm);
		if (ret == -EAGAIN && virt_queue__realloc_iov(buf, buf->cnt * 2))
			break;
	} while (ret == -EAGAIN);

	if (ret < 0)
		goto err_put_buf;

	buf->valid = true;
	return ret;
err_put_buf:
	virt_queue__put_buf(vq, buf);
	return ret;
}

int virt_queue__get_iov(struct virt_queue *vq, u16 *out, u16 *in, struct kvm *kvm)
{
	u16 head;

	head = virt_queue__pop(vq);

	return virt_queue__get_head_iov(vq, &vq->buf, out, in, head, kvm);
}

static int __virt_queue__get_inout_iov(struct kvm *kvm, struct virt_queue *queue,
				       struct virt_buf *in_buf, struct virt_buf *out_buf)
{
	struct vring_desc *desc;
	struct virt_buf *buf;
	u16 head, idx;
	bool is_write;
	u16 out = 0;
	u16 in = 0;
	size_t len;
	u64 addr;
	int prot;
	u16 *cur;

	virt_queue__get_buf(queue, &queue->desc_buf);
	idx = head = virt_queue__pop(queue);
	do {
		desc = virt_queue__get_desc(queue, idx);
		is_write = virt_desc__test_flag(queue, desc, VRING_DESC_F_WRITE);
		len = virtio_guest_to_host_u32(queue, desc->len);
		addr = virtio_guest_to_host_u64(queue, desc->addr);
		if (is_write) {
			prot = IOMMU_PROT_WRITE;
			buf = in_buf;
			cur = &in;
		} else {
			prot = IOMMU_PROT_READ;
			buf = out_buf;
			cur = &out;
		}

		*cur += virtio_populate_buf(kvm, queue->vdev, addr, len, prot,
					    *cur, buf);

		if (virt_desc__test_flag(queue, desc, VRING_DESC_F_NEXT))
			idx = virtio_guest_to_host_u16(queue, desc->next);
		else
			break;
	} while (1);
	virt_queue__put_buf(queue, &queue->desc_buf);

	return head;
}

/* in and out are relative to guest */
int virt_queue__get_inout_iov(struct kvm *kvm, struct virt_queue *queue,
			      struct virt_buf *in_buf, struct virt_buf *out_buf)
{
	int ret = -ENOMEM;

	virt_queue__get_buf(queue, in_buf);
	virt_queue__get_buf(queue, out_buf);

	if (virt_queue__realloc_iov(in_buf, queue->vring.num))
		goto err_put_buf;
	if (virt_queue__realloc_iov(out_buf, queue->vring.num))
		goto err_put_buf;

	do {
		ret = __virt_queue__get_inout_iov(kvm, queue, in_buf, out_buf);
		if (ret == -EAGAIN) {
			if (virt_queue__realloc_iov(in_buf, in_buf->cnt * 2))
				break;
			if (virt_queue__realloc_iov(out_buf, out_buf->cnt * 2))
				break;
		}
	} while (ret == -EAGAIN);

	if (ret < 0)
		goto err_put_buf;

	in_buf->valid = out_buf->valid = true;
	return ret;
err_put_buf:
	virt_queue__put_buf(queue, out_buf);
	virt_queue__put_buf(queue, in_buf);
	return ret;
}

int virt_queue__init_avail(struct virt_queue *vq)
{
	void *p;
	size_t nr_descs = vq->vring.num;
	/* nr_descs + 1 because of the used_event_idx at the end */
	size_t buf_size = offsetof(struct vring_avail, ring) + (nr_descs + 1) *
		          sizeof(u16);

	p = virtio_map_table(vq->kvm, vq->vdev, (u64)vq->vring.avail, buf_size,
			     IOMMU_PROT_READ | IOMMU_PROT_WRITE,
			     &vq->avail_buf);
	if (!p) {
		pr_err("could not map avail");
		return -EFAULT;
	}
	return 0;
}

int virt_queue__init_used(struct virt_queue *vq)
{
	void *p;
	size_t nr_descs = vq->vring.num;
	size_t buf_size = offsetof(struct vring_used, ring) + nr_descs *
			  sizeof(struct vring_used_elem) + sizeof(u16);

	p = virtio_map_table(vq->kvm, vq->vdev, (u64)vq->vring.used, buf_size,
			     IOMMU_PROT_READ | IOMMU_PROT_WRITE, &vq->used_buf);
	if (!p) {
		pr_err("could not map used");
		return -EFAULT;
	}
	return 0;
}

int virt_queue__init_desc(struct virt_queue *vq)
{
	void *p;
	size_t nr_descs = vq->vring.num;
	size_t buf_size = nr_descs * sizeof(struct vring_desc);

	p = virtio_map_table(vq->kvm, vq->vdev, (u64)vq->vring.desc, buf_size,
			     IOMMU_PROT_READ | IOMMU_PROT_WRITE, &vq->desc_buf);
	if (!p) {
		pr_err("could not map desc");
		return -EFAULT;
	}
	return 0;
}

void virtio_init_device_vq(struct kvm *kvm, struct virtio_device *vdev,
			   struct virt_queue *vq, size_t nr_descs)
{
	struct vring_addr *addr = &vq->vring_addr;

	vq->kvm			= kvm;
	vq->vdev		= vdev;
	vq->endian		= vdev->endian;
	vq->use_event_idx	= (vdev->features & VIRTIO_RING_F_EVENT_IDX);
	vq->enabled		= true;
	virt_queue__init_buf(&vq->buf);
	virt_queue__init_buf(&vq->desc_buf);
	virt_queue__init_buf(&vq->avail_buf);
	virt_queue__init_buf(&vq->used_buf);
	virt_queue__init_buf(&vq->indirect_buf);

	if (addr->legacy) {
		unsigned long base = (u64)addr->pfn * addr->pgsize;
		void *p = guest_flat_to_host(kvm, base);

		/* The IOMMU doesn't exist as a legacy device */
		BUG_ON(vdev->iommu_domain);
		vring_init(&vq->vring, nr_descs, p, addr->align);
	} else {
		u64 desc = (u64)addr->desc_hi << 32 | addr->desc_lo;
		u64 avail = (u64)addr->avail_hi << 32 | addr->avail_lo;
		u64 used = (u64)addr->used_hi << 32 | addr->used_lo;

		if (vdev->use_iommu) {
			vq->use_iommu = true;
			/* These are IOVAs */
			vq->vring = (struct vring) {
				.desc = (void *)desc,
				.used = (void *)used,
				.avail = (void *)avail,
				.num = nr_descs,
			};
		} else {
			vq->vring = (struct vring) {
				.desc = guest_flat_to_host(kvm, desc),
				.used = guest_flat_to_host(kvm, used),
				.avail = guest_flat_to_host(kvm, avail),
				.num = nr_descs,
			};
		}
	}
}

void virtio_exit_vq(struct kvm *kvm, struct virtio_device *vdev,
			   void *dev, int num)
{
	struct virt_queue *vq = vdev->ops->get_vq(kvm, dev, num);

	if (vq->enabled && vdev->ops->exit_vq)
		vdev->ops->exit_vq(kvm, dev, num);
	memset(vq, 0, sizeof(*vq));
}

int virtio__get_dev_specific_field(int offset, bool msix, u32 *config_off)
{
	if (msix) {
		if (offset < 4)
			return VIRTIO_PCI_O_MSIX;
		else
			offset -= 4;
	}

	*config_off = offset;

	return VIRTIO_PCI_O_CONFIG;
}

bool virtio_queue__should_signal(struct virt_queue *vq)
{
	bool should_signal;
	u16 old_idx, new_idx, event_idx;
	u16 *event_ptr, *new_ptr, *flags_ptr;

	/*
	 * Use mb to assure used idx has been increased before we signal the
	 * guest, and we don't read a stale value for used_event. Without a mb
	 * here we might not send a notification that we need to send, or the
	 * guest may ignore the queue since it won't see an updated idx.
	 */
	mb();

	if (!vq->use_event_idx) {
		/*
		 * When VIRTIO_RING_F_EVENT_IDX isn't negotiated, interrupt the
		 * guest if it didn't explicitly request to be left alone.
		 */
		virt_queue__get_buf(vq, &vq->avail_buf);
		flags_ptr = vring_get_avail_ptr(vq, &vq->vring.avail->flags);
		should_signal = !(virtio_guest_to_host_u16(vq, *flags_ptr) &
				  VRING_AVAIL_F_NO_INTERRUPT);
		virt_queue__put_buf(vq, &vq->avail_buf);
		return should_signal;
	}

	virt_queue__get_buf(vq, &vq->avail_buf);
	virt_queue__get_buf(vq, &vq->used_buf);
	new_ptr		= vring_get_used_ptr(vq, &vq->vring.used->idx);
	event_ptr	= vring_get_avail_ptr(vq, &vring_used_event(&vq->vring));

	old_idx		= vq->last_used_signalled;
	new_idx		= virtio_guest_to_host_u16(vq, *new_ptr);
	event_idx	= virtio_guest_to_host_u16(vq, *event_ptr);
	virt_queue__put_buf(vq, &vq->used_buf);
	virt_queue__put_buf(vq, &vq->avail_buf);

	if (vring_need_event(event_idx, new_idx, old_idx)) {
		vq->last_used_signalled = new_idx;
		return true;
	}

	return false;
}

const struct iommu_properties *
virtio__iommu_get_properties(struct device_header *dev)
{
	return &iommu_props;
}

static int virtio_invalidate_mappings(void *domain, void *cookie,
				      struct device_header *dev,
				      struct iommu_tlb_entry *tlbe)
{
	struct virt_buf *buf = tlbe->dev;

	if (!buf)
		/* TODO: destroy MSI route */
		return 0;

	/* Wait for any access to finish, then invalidate the IOV */
	mutex_lock(&buf->mutex);
	buf->valid = false;
	mutex_unlock(&buf->mutex);
	return 0;
}

int virtio__iommu_attach(void *domain, struct device_header *dev,
			 struct virtio_device *vdev, int flags)
{
	if (!domain)
		return -ENOMEM;

	vdev->iommu_domain = domain;
	return iommu_attach(domain, dev, virtio_invalidate_mappings, vdev);
}

int virtio__iommu_detach(void *domain, struct device_header *dev,
			 struct virtio_device *vdev)
{
	int ret;

	if (vdev->iommu_domain != domain) {
		pr_err("wrong domain"); /* bug */
		return -EINVAL;
	}

	ret = iommu_detach(domain, dev);
	vdev->iommu_domain = NULL;
	return ret;
}

void virtio_set_guest_features(struct kvm *kvm, struct virtio_device *vdev,
			       void *dev, u32 features)
{
	/* TODO: fail negotiation if features & ~host_features */

	vdev->features = features;
}

u64 virtio_get_host_features(struct kvm *kvm, struct virtio_device *vdev,
			     void *dev)
{
	u64 features = 0;

	if (vdev->ops->get_host_features)
		features = vdev->ops->get_host_features(kvm, dev);

	if (vdev->use_iommu)
		features |= 1ULL << VIRTIO_F_IOMMU_PLATFORM;

	return features;
}

void virtio_notify_status(struct kvm *kvm, struct virtio_device *vdev,
			  void *dev, u8 status)
{
	u32 ext_status = status;

	vdev->status &= ~VIRTIO_CONFIG_S_MASK;
	vdev->status |= status;

	/* Add a few hints to help devices */
	if ((status & VIRTIO_CONFIG_S_DRIVER_OK) &&
	    !(vdev->status & VIRTIO__STATUS_START)) {
		vdev->status |= VIRTIO__STATUS_START;
		ext_status |= VIRTIO__STATUS_START;

	} else if (!status && (vdev->status & VIRTIO__STATUS_START)) {
		vdev->status &= ~VIRTIO__STATUS_START;
		ext_status |= VIRTIO__STATUS_STOP;

		/*
		 * Reset virtqueues and stop all traffic now, so that the device
		 * can safely reset the backend in notify_status().
		 */
		if (ext_status & VIRTIO__STATUS_STOP)
			vdev->ops->reset(kvm, vdev);
	}

	/* On first reset, swap device-specific config endianess. */
	if (!status && !(vdev->status & VIRTIO__STATUS_SWAB)) {
		vdev->status |= VIRTIO__STATUS_SWAB;
		ext_status |= VIRTIO__STATUS_SWAB;
	}

	if (vdev->ops->notify_status)
		vdev->ops->notify_status(kvm, dev, ext_status);
}

bool virtio_read_config(struct kvm *kvm, struct virtio_device *vdev, void *dev,
			unsigned long offset, void *data, size_t size)
{
	void *config = vdev->ops->get_config(kvm, dev) + offset;

	switch (size) {
	case 1:
		*(u8 *)data = *(u8 *)config;
		break;
	case 2:
		*(u16 *)data = *(u16 *)config;
		break;
	case 4:
		*(u32 *)data = *(u32 *)config;
		break;
	default:
		return false;
	}

	return true;
}

bool virtio_write_config(struct kvm *kvm, struct virtio_device *vdev, void *dev,
			unsigned long offset, void *data, size_t size)
{
	void *config = vdev->ops->get_config(kvm, dev) + offset;

	switch (size) {
	case 1:
		*(u8 *)config = *(u8 *)data;
		break;
	case 2:
		*(u16 *)config = *(u16 *)data;
		break;
	case 4:
		*(u32 *)config = *(u32 *)data;
		break;
	default:
		return false;
	}

	return true;
}

int virtio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		struct virtio_ops *ops, enum virtio_trans trans,
		int device_id, int subsys_id, int class)
{
	void *virtio;
	int r;

	if (!vdev->endian)
		vdev->endian = VIRTIO_ENDIAN_HOST;

	if (subsys_id != VIRTIO_ID_IOMMU) {
		/*
		 * If user selected it, try to instantiate a virtual IOMMU for
		 * this device.
		 */
		if (!iommu) {
			iommu = viommu_register(kvm, &iommu_props);
			if (IS_ERR(iommu)) {
				int ret = PTR_ERR(iommu);

				pr_err("cannot instantiate %s",
				       iommu_props.name);
				iommu = NULL;
				return ret;
			}
		}

		if (iommu) {
			vdev->use_iommu = true;
			vdev->translate_msi = !iommu_props.msi_window.start;
		}
	}

	switch (trans) {
	case VIRTIO_PCI_LEGACY:
		vdev->legacy			= true;
		/* fall through */
	case VIRTIO_PCI:
		virtio = calloc(sizeof(struct virtio_pci), 1);
		if (!virtio)
			return -ENOMEM;
		vdev->virtio			= virtio;
		vdev->ops			= ops;
		vdev->ops->signal_vq		= virtio_pci__signal_vq;
		vdev->ops->signal_config	= virtio_pci__signal_config;
		vdev->ops->init			= virtio_pci__init;
		vdev->ops->exit			= virtio_pci__exit;
		vdev->ops->reset		= virtio_pci__reset;
		r = vdev->ops->init(kvm, dev, vdev, device_id, subsys_id, class);
		break;
	case VIRTIO_MMIO_LEGACY:
		vdev->legacy			= true;
		/* fall through */
	case VIRTIO_MMIO:
		virtio = calloc(sizeof(struct virtio_mmio), 1);
		if (!virtio)
			return -ENOMEM;
		vdev->virtio			= virtio;
		vdev->ops			= ops;
		vdev->ops->signal_vq		= virtio_mmio_signal_vq;
		vdev->ops->signal_config	= virtio_mmio_signal_config;
		vdev->ops->init			= virtio_mmio_init;
		vdev->ops->exit			= virtio_mmio_exit;
		vdev->ops->reset		= virtio_mmio_reset;
		r = vdev->ops->init(kvm, dev, vdev, device_id, subsys_id, class);
		break;
	default:
		r = -1;
	};

	if (iommu && vdev->legacy) {
		pr_err("IOMMU isn't supported with legacy virtio");
		return -EINVAL;
	}

	return r;
}

int virtio_compat_add_message(const char *device, const char *config)
{
	int len = 1024;
	int compat_id;
	char *title;
	char *desc;

	title = malloc(len);
	if (!title)
		return -ENOMEM;

	desc = malloc(len);
	if (!desc) {
		free(title);
		return -ENOMEM;
	}

	snprintf(title, len, "%s device was not detected.", device);
	snprintf(desc,  len, "While you have requested a %s device, "
			     "the guest kernel did not initialize it.\n"
			     "\tPlease make sure that the guest kernel was "
			     "compiled with %s=y enabled in .config.",
			     device, config);

	compat_id = compat__add_message(title, desc);

	free(desc);
	free(title);

	return compat_id;
}
