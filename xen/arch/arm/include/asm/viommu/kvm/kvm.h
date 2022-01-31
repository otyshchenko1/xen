#ifndef KVM__KVM_H
#define KVM__KVM_H

#include <xen/types.h>

#define pr_debug(fmt, ...)    \
    printk(XENLOG_DEBUG fmt "\n", ## __VA_ARGS__)
#define pr_info(fmt, ...)    \
    printk(XENLOG_INFO fmt "\n", ## __VA_ARGS__)
#define pr_warning(fmt, ...)    \
    printk(XENLOG_WARNING fmt "\n", ## __VA_ARGS__)
#define pr_err(fmt, ...)     \
    printk(XENLOG_ERR fmt "\n", ## __VA_ARGS__)

/* Alias to Xen lock functions */
#define DEFINE_MUTEX DEFINE_SPINLOCK
#define mutex spinlock
#define mutex_init spin_lock_init
#define mutex_lock spin_lock
#define mutex_unlock spin_unlock

#define dprintf(fd, fmt, ...) printk(" " fmt, ## __VA_ARGS__)
#define fprintf(stderr, fmt, ...) printk(" " fmt, ## __VA_ARGS__)

/* XXX Ugly aliases to avoid modify ported code much */
#define kvm domain
#define kvm_cpu vcpu

#endif /* KVM__KVM_H */
