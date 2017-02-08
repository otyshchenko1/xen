/*
 * xen/arch/arm/coproc/coproc.c
 *
 * Generic Remote processors framework
 *
 * Oleksandr Tyshchenko <Oleksandr_Tyshchenko@epam.com>
 * Copyright (C) 2016 EPAM Systems Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/init.h>
#include <xen/err.h>
#include <xen/guest_access.h>
#include <xen/keyhandler.h>

#include "plat/common.h"
#include "coproc.h"

/* dom0_coprocs: comma-separated list of coprocs for domain 0 */
static char __initdata opt_dom0_coprocs[128] = "";
string_param("dom0_coprocs", opt_dom0_coprocs);

/*
 * the "framework's" global list is used to keep track
 * of all coproc devices that have been registered in the framework
 */
static LIST_HEAD(coprocs);
/* to protect both operations with the coproc and global coprocs list here */
static DEFINE_SPINLOCK(coprocs_lock);
/* the number of registered coproc devices */
static int num_coprocs;

s_time_t vcoproc_context_switch(struct vcoproc_instance *curr,
                                struct vcoproc_instance *next)
{
    struct coproc_device *coproc;
    int ret;

    if ( unlikely(curr == next) )
        return 0;

    coproc = next ? next->coproc : curr->coproc;

    if ( likely(curr) )
    {
        s_time_t wait_time;

        ASSERT(curr->state == VCOPROC_RUNNING ||
               curr->state == VCOPROC_ASKED_TO_SLEEP);

        wait_time = coproc->ops->ctx_switch_from(curr);

        if ( wait_time == 0 )
        {
            if (curr->state == VCOPROC_RUNNING)
                curr->state = VCOPROC_WAITING;
            else
                curr->state = VCOPROC_SLEEPING;
        }
        if ( wait_time )
            return wait_time;
    }

    if ( likely(next) )
    {
        ASSERT(next->state == VCOPROC_WAITING);

        /* TODO What to do if we failed to switch to "next"? */
        ret = coproc->ops->ctx_switch_to(next);
        if ( unlikely(ret) )
            panic("Failed to switch context to vcoproc \"%s\" (%d)\n",
                  dev_path(coproc->dev), ret);
        else
            next->state = VCOPROC_RUNNING;
    }

    return 0;
}

void vcoproc_continue_running(struct vcoproc_instance *same)
{
    /* nothing to do */
}

static struct coproc_device *coproc_find_by_path(const char *path)
{
    struct coproc_device *coproc;
    bool_t found = false;

    if ( !path )
        return NULL;

    spin_lock(&coprocs_lock);

    if ( list_empty(&coprocs) )
        goto out;

    list_for_each_entry(coproc, &coprocs, coproc_elem)
    {
        if ( !strncmp(dev_path(coproc->dev), path, strlen(path)) )
        {
            found = true;
            break;
        }
    }

out:
    spin_unlock(&coprocs_lock);

    return found ? coproc : NULL;
}

static struct vcoproc_instance *coproc_init_vcoproc(struct domain *d,
                                                    struct coproc_device *coproc)
{
    struct vcoproc_instance *vcoproc;
    int ret;

    vcoproc = xzalloc(struct vcoproc_instance);
    if ( !vcoproc )
    {
        printk("Failed to allocate vcoproc_instance for %s\n",
               dev_path(coproc->dev));
        return ERR_PTR(-ENOMEM);
    }

    vcoproc->coproc = coproc;
    vcoproc->domain = d;
    vcoproc->state = VCOPROC_UNKNOWN;
    spin_lock_init(&vcoproc->lock);

    ret = coproc->ops->vcoproc_init(d, coproc, vcoproc);
    if ( ret )
    {
        printk("Failed to initialize vcoproc_instance for %s\n",
               dev_path(coproc->dev));
        goto out;
    }

    spin_lock(&coproc->vcoprocs_lock);
    list_add(&vcoproc->vcoproc_elem, &coproc->vcoprocs);
    spin_unlock(&coproc->vcoprocs_lock);

    return vcoproc;

out:
    xfree(vcoproc);
    return ERR_PTR( ret );
}

static inline bool_t coproc_is_created_vcoproc(struct domain *d,
                                               struct coproc_device *coproc)
{
    return coproc_get_vcoproc(d, coproc) ? true : false;
}

static void coproc_deinit_vcoproc(struct domain *d,
                                  struct vcoproc_instance *vcoproc)
{
    struct coproc_device *coproc;

    if ( !vcoproc )
        return;

    coproc = vcoproc->coproc;
    coproc->ops->vcoproc_deinit(d, vcoproc);
    spin_lock(&coproc->vcoprocs_lock);
    list_del(&vcoproc->vcoproc_elem);
    spin_unlock(&coproc->vcoprocs_lock);
    xfree(vcoproc);
}

static int coproc_attach_to_domain(struct domain *d,
                                   struct coproc_device *coproc)
{
    struct vcoproc *vcoproc_d = &d->arch.vcoproc;
    struct vcoproc_instance *vcoproc;
    int ret;

    if ( !coproc )
        return -EINVAL;

    spin_lock(&coprocs_lock);

    if ( coproc_is_created_vcoproc(d, coproc) )
    {
        ret = -EEXIST;
        goto out;
    }

    vcoproc = coproc_init_vcoproc(d, coproc);
    if ( IS_ERR(vcoproc) )
    {
        ret = PTR_ERR(vcoproc);
        goto out;
    }

    ret = vcoproc_scheduler_vcoproc_init(coproc->sched, vcoproc);
    if ( ret )
    {
        coproc_deinit_vcoproc(d, vcoproc);
        goto out;
    }

    BUG_ON(vcoproc_d->num_instances >= num_coprocs);
    list_add_tail(&vcoproc->instance_elem, &vcoproc_d->instances);
    vcoproc_d->num_instances++;

    printk("Created vcoproc \"%s\" for dom%u\n",
           dev_path(coproc->dev), d->domain_id);

out:
    spin_unlock(&coprocs_lock);

    return ret;
}

static int coproc_find_and_attach_to_domain(struct domain *d, const char *path)
{
    struct coproc_device *coproc;

    coproc = coproc_find_by_path(path);
    if ( !coproc )
        return -ENODEV;

    return coproc_attach_to_domain(d, coproc);
}

static int coproc_detach_from_domain(struct domain *d,
                                     struct vcoproc_instance *vcoproc)
{
    struct vcoproc *vcoproc_d = &d->arch.vcoproc;
    struct coproc_device *coproc;
    int ret;

    if ( !vcoproc )
        return 0;

    spin_lock(&coprocs_lock);

    coproc = vcoproc->coproc;

    ret = vcoproc_scheduler_vcoproc_destroy(coproc->sched, vcoproc);
    if ( ret )
    {
        if ( ret == -EBUSY )
            ret = -ERESTART;
        goto out;
    }

    BUG_ON(!vcoproc_d->num_instances);
    list_del_init(&vcoproc->instance_elem);
    vcoproc_d->num_instances--;

    coproc_deinit_vcoproc(d, vcoproc);

    printk("Destroyed vcoproc \"%s\" for dom%u\n",
            dev_path(coproc->dev), d->domain_id);

out:
    spin_unlock(&coprocs_lock);

    return ret;
}

bool_t coproc_is_attached_to_domain(struct domain *d, const char *path)
{
    struct coproc_device *coproc;
    bool_t is_created;

    coproc = coproc_find_by_path(path);
    if ( !coproc )
        return false;

    spin_lock(&coprocs_lock);
    is_created = coproc_is_created_vcoproc(d, coproc);
    spin_unlock(&coprocs_lock);

    return is_created;
}

static int __init vcoproc_dom0_init(struct domain *d)
{
    const char *curr, *next;
    int len, ret = 0;

    if ( !strcmp(opt_dom0_coprocs, "") )
        return 0;

    printk("Got list of coprocs \"%s\" for dom%u\n",
           opt_dom0_coprocs, d->domain_id);

    /*
     * For the moment, we'll create vcoproc for each registered coproc
     * which is described in the list of coprocs for domain 0 in bootargs.
     */
    for ( curr = opt_dom0_coprocs; curr; curr = next )
    {
        struct dt_device_node *node = NULL;
        char *buf;
        bool_t is_alias = false;

        next = strchr(curr, ',');
        if ( next )
        {
            len = next - curr;
            next++;
        }
        else
            len = strlen(curr);

        if ( *curr != '/' )
            is_alias = true;

        buf = xmalloc_array(char, len + 1);
        if ( !buf )
        {
            ret = -ENOMEM;
            break;
        }

        strlcpy(buf, curr, len + 1);
        if ( is_alias )
            node = dt_find_node_by_alias(buf);
        else
            node = dt_find_node_by_path(buf);
        if ( !node )
        {
            printk("Unable to find node by %s \"%s\"\n",
                   is_alias ? "alias" : "path", buf);
            ret = -EINVAL;
        }
        xfree(buf);
        if ( ret )
            break;

        curr = dt_node_full_name(node);

        ret = coproc_find_and_attach_to_domain(d, curr);
        if (ret)
        {
            printk("Failed to attach coproc \"%s\" to dom%u (%d)\n",
                   curr, d->domain_id, ret);
            break;
        }
    }

    return ret;
}

int vcoproc_domain_init(struct domain *d)
{
    struct vcoproc *vcoproc_d = &d->arch.vcoproc;
    int ret = 0;

    vcoproc_d->num_instances = 0;
    INIT_LIST_HEAD(&vcoproc_d->instances);

    /*
     * We haven't known yet if the guest domain are going to use coprocs.
     * So, just return okay for the moment. It won't be an issue later if
     * guest domain doesn't request any.
     * But we definitely know when domain 0 is being created.
     */
    if ( !num_coprocs )
    {
        if ( d->domain_id == 0 && strcmp(opt_dom0_coprocs, "") )
        {
            printk("There is no registered coproc for creating vcoproc\n");
            return -ENODEV;
        }

        return 0;
    }

    /* We already have the list of coprocs for domain 0 only. */
    if ( d->domain_id == 0 )
        ret = vcoproc_dom0_init(d);

    return ret;
}

void vcoproc_domain_free(struct domain *d)
{
    coproc_release_vcoprocs(d);
}

int coproc_release_vcoprocs(struct domain *d)
{
    struct vcoproc *vcoproc_d = &d->arch.vcoproc;
    struct vcoproc_instance *vcoproc, *temp;
    int ret;

    list_for_each_entry_safe( vcoproc, temp, &vcoproc_d->instances,
                              instance_elem )
    {
        ret = coproc_detach_from_domain(d, vcoproc);
        if ( ret )
            return ret;
    }

    return 0;
}

int coproc_do_domctl(struct xen_domctl *domctl, struct domain *d,
                     XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl)
{
    char *path;
    int ret;

    switch ( domctl->cmd )
    {
    case XEN_DOMCTL_attach_coproc:
        if ( unlikely(d->is_dying) )
        {
            ret = -EINVAL;
            break;
        }

        path = safe_copy_string_from_guest(domctl->u.attach_coproc.path,
                                           domctl->u.attach_coproc.size,
                                           PAGE_SIZE);
        if ( IS_ERR(path) )
        {
            ret = PTR_ERR(path);
            break;
        }

        printk("Got coproc \"%s\" for dom%u\n", path, d->domain_id);

        ret = coproc_find_and_attach_to_domain(d, path);
        if ( ret )
            printk("Failed to attach coproc \"%s\" to dom%u (%d)\n",
                   path, d->domain_id, ret);
        xfree(path);
        break;

    default:
        ret = -ENOSYS;
        break;
    }

    return ret;
}

int __init coproc_register(struct coproc_device *coproc)
{
    if ( !coproc || !coproc->ops )
        return -EINVAL;

    if ( coproc_find_by_path(dev_path(coproc->dev)) )
        return -EEXIST;

    coproc->sched = vcoproc_scheduler_init(coproc);
    if ( IS_ERR(coproc->sched) )
        return PTR_ERR(coproc->sched);

    spin_lock(&coprocs_lock);
    list_add_tail(&coproc->coproc_elem, &coprocs);
    num_coprocs++;
    spin_unlock(&coprocs_lock);

    printk("Registered new coproc \"%s\"\n", dev_path(coproc->dev));

    return 0;
}

bool_t coproc_debug = true;

void coproc_debug_toggle(unsigned char key)
{
    coproc_debug = !coproc_debug;
    printk("coproc debug is %s\n", coproc_debug ? "enabled" : "disabled");
}

void __init coproc_init(void)
{
    struct dt_device_node *node;
    unsigned int num_coprocs = 0;
    int ret;

    register_keyhandler('c', coproc_debug_toggle, "toggle debug for coproc", 0);

    /*
     * For the moment, we'll create coproc for each device that presents
     * in the device tree and has "xen,coproc" property.
     */
    dt_for_each_device_node(dt_host, node)
    {
        if ( !dt_get_property(node, "xen,coproc", NULL) )
            continue;

        ret = device_init(node, DEVICE_COPROC, NULL);
        if ( !ret )
            num_coprocs++;
    }

    if ( !num_coprocs )
        printk("Unable to find compatible coprocs in the device tree\n");
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
