#ifndef _XEN_PAGING_H
#define _XEN_PAGING_H

#define paging_mode_translate(d)              (1)
#define paging_mode_external(d)               (1)

static inline void paging_mark_pfn_dirty(struct domain *d, pfn_t pfn)
{
}

#endif /* XEN_PAGING_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
