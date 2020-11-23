/*
 * ioreq.h: Hardware virtual machine assist interface definitions.
 *
 * Copyright (c) 2016 Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __XEN_IOREQ_H__
#define __XEN_IOREQ_H__

#include <xen/sched.h>

struct ioreq_page {
    gfn_t gfn;
    struct page_info *page;
    void *va;
};

struct ioreq_vcpu {
    struct list_head list_entry;
    struct vcpu      *vcpu;
    evtchn_port_t    ioreq_evtchn;
    bool             pending;
};

#define NR_IO_RANGE_TYPES (XEN_DMOP_IO_RANGE_PCI + 1)
#define MAX_NR_IO_RANGES  256

struct ioreq_server {
    struct domain          *target, *emulator;

    /* Lock to serialize toolstack modifications */
    spinlock_t             lock;

    struct ioreq_page      ioreq;
    struct list_head       ioreq_vcpu_list;
    struct ioreq_page      bufioreq;

    /* Lock to serialize access to buffered ioreq ring */
    spinlock_t             bufioreq_lock;
    evtchn_port_t          bufioreq_evtchn;
    struct rangeset        *range[NR_IO_RANGE_TYPES];
    bool                   enabled;
    uint8_t                bufioreq_handling;
};

/*
 * This should only be used when d == current->domain and it's not paused,
 * or when they're distinct and d is paused. Otherwise the result is
 * stale before the caller can inspect it.
 */
static inline bool domain_has_ioreq_server(const struct domain *d)
{
#ifdef CONFIG_IOREQ_SERVER
    return d->ioreq_server.nr_servers;
#else
    return false;
#endif
}

static inline paddr_t ioreq_mmio_first_byte(const ioreq_t *p)
{
    return unlikely(p->df) ?
           p->addr - (p->count - 1ul) * p->size :
           p->addr;
}

static inline paddr_t ioreq_mmio_last_byte(const ioreq_t *p)
{
    unsigned long size = p->size;

    return unlikely(p->df) ?
           p->addr + size - 1:
           p->addr + (p->count * size) - 1;
}

static inline bool ioreq_needs_completion(const ioreq_t *ioreq)
{
    return ioreq->state == STATE_IOREQ_READY &&
           !ioreq->data_is_ptr &&
           (ioreq->type != IOREQ_TYPE_PIO || ioreq->dir != IOREQ_WRITE);
}

#define HANDLE_BUFIOREQ(s) \
    ((s)->bufioreq_handling != HVM_IOREQSRV_BUFIOREQ_OFF)

bool vcpu_ioreq_pending(struct vcpu *v);
bool vcpu_ioreq_handle_completion(struct vcpu *v);
bool is_ioreq_server_page(struct domain *d, const struct page_info *page);

int ioreq_server_create(struct domain *d, int bufioreq_handling,
                        ioservid_t *id);
int ioreq_server_destroy(struct domain *d, ioservid_t id);
int ioreq_server_get_info(struct domain *d, ioservid_t id,
                          unsigned long *ioreq_gfn,
                          unsigned long *bufioreq_gfn,
                          evtchn_port_t *bufioreq_port);
int ioreq_server_get_frame(struct domain *d, ioservid_t id,
                           unsigned long idx, mfn_t *mfn);
int ioreq_server_map_io_range(struct domain *d, ioservid_t id,
                              uint32_t type, uint64_t start,
                              uint64_t end);
int ioreq_server_unmap_io_range(struct domain *d, ioservid_t id,
                                uint32_t type, uint64_t start,
                                uint64_t end);
int ioreq_server_map_mem_type(struct domain *d, ioservid_t id,
                              uint32_t type, uint32_t flags);

int ioreq_server_set_state(struct domain *d, ioservid_t id,
                           bool enabled);

int ioreq_server_add_vcpu_all(struct domain *d, struct vcpu *v);
void ioreq_server_remove_vcpu_all(struct domain *d, struct vcpu *v);
void ioreq_server_destroy_all(struct domain *d);

struct ioreq_server *ioreq_server_select(struct domain *d,
                                         ioreq_t *p);
int ioreq_send(struct ioreq_server *s, ioreq_t *proto_p,
               bool buffered);
unsigned int ioreq_broadcast(ioreq_t *p, bool buffered);

void ioreq_domain_init(struct domain *d);

#endif /* __XEN_IOREQ_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
