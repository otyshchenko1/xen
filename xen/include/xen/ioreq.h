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

#ifndef __IOREQ_H__
#define __IOREQ_H__

#include <xen/sched.h>

#include <asm/hvm/ioreq.h>

struct hvm_ioreq_page {
    gfn_t gfn;
    struct page_info *page;
    void *va;
};

struct hvm_ioreq_vcpu {
    struct list_head list_entry;
    struct vcpu      *vcpu;
    evtchn_port_t    ioreq_evtchn;
    bool             pending;
};

#define NR_IO_RANGE_TYPES (XEN_DMOP_IO_RANGE_PCI + 1)
#define MAX_NR_IO_RANGES  256

struct hvm_ioreq_server {
    struct domain          *target, *emulator;

    /* Lock to serialize toolstack modifications */
    spinlock_t             lock;

    struct hvm_ioreq_page  ioreq;
    struct list_head       ioreq_vcpu_list;
    struct hvm_ioreq_page  bufioreq;

    /* Lock to serialize access to buffered ioreq ring */
    spinlock_t             bufioreq_lock;
    evtchn_port_t          bufioreq_evtchn;
    struct rangeset        *range[NR_IO_RANGE_TYPES];
    bool                   enabled;
    uint8_t                bufioreq_handling;
};

static inline bool hvm_domain_has_ioreq_server(const struct domain *d)
{
    return (d->arch.hvm.ioreq_server.nr_servers > 0);
}

#define GET_IOREQ_SERVER(d, id) \
    (d)->arch.hvm.ioreq_server.server[id]

static inline struct hvm_ioreq_server *get_ioreq_server(const struct domain *d,
                                                        unsigned int id)
{
    if ( id >= MAX_NR_IOREQ_SERVERS )
        return NULL;

    return GET_IOREQ_SERVER(d, id);
}

static inline paddr_t hvm_mmio_first_byte(const ioreq_t *p)
{
    return unlikely(p->df) ?
           p->addr - (p->count - 1ul) * p->size :
           p->addr;
}

static inline paddr_t hvm_mmio_last_byte(const ioreq_t *p)
{
    unsigned long size = p->size;

    return unlikely(p->df) ?
           p->addr + size - 1:
           p->addr + (p->count * size) - 1;
}

static inline bool hvm_ioreq_needs_completion(const ioreq_t *ioreq)
{
    return ioreq->state == STATE_IOREQ_READY &&
           !ioreq->data_is_ptr &&
           (ioreq->type != IOREQ_TYPE_PIO || ioreq->dir != IOREQ_WRITE);
}

void send_invalidate_req(void);

bool hvm_io_pending(struct vcpu *v);
bool handle_hvm_io_completion(struct vcpu *v);
bool is_ioreq_server_page(struct domain *d, const struct page_info *page);

int hvm_create_ioreq_server(struct domain *d, int bufioreq_handling,
                            ioservid_t *id);
int hvm_destroy_ioreq_server(struct domain *d, ioservid_t id);
int hvm_get_ioreq_server_info(struct domain *d, ioservid_t id,
                              unsigned long *ioreq_gfn,
                              unsigned long *bufioreq_gfn,
                              evtchn_port_t *bufioreq_port);
int hvm_get_ioreq_server_frame(struct domain *d, ioservid_t id,
                               unsigned long idx, mfn_t *mfn);
int hvm_map_io_range_to_ioreq_server(struct domain *d, ioservid_t id,
                                     uint32_t type, uint64_t start,
                                     uint64_t end);
int hvm_unmap_io_range_from_ioreq_server(struct domain *d, ioservid_t id,
                                         uint32_t type, uint64_t start,
                                         uint64_t end);
int hvm_set_ioreq_server_state(struct domain *d, ioservid_t id,
                               bool enabled);

int hvm_all_ioreq_servers_add_vcpu(struct domain *d, struct vcpu *v);
void hvm_all_ioreq_servers_remove_vcpu(struct domain *d, struct vcpu *v);
void hvm_destroy_all_ioreq_servers(struct domain *d);

struct hvm_ioreq_server *hvm_select_ioreq_server(struct domain *d,
                                                 ioreq_t *p);
int hvm_send_ioreq(struct hvm_ioreq_server *s, ioreq_t *proto_p,
                   bool buffered);
unsigned int hvm_broadcast_ioreq(ioreq_t *p, bool buffered);

void hvm_ioreq_init(struct domain *d);

#endif /* __IOREQ_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
