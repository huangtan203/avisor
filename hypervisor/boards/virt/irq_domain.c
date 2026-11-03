// SPDX-License-Identifier: GPL-2.0-or-later
#include "boards/virt/irq_domain.h"
#include "boards/virt/gicv3.h"
#include "common/sched.h"
#include "common/vm.h"
#include "common/utils.h"

static struct irq_route routes[IRQ_DOMAIN_MAX_ROUTES];
static volatile unsigned int route_lock;

static unsigned long domain_lock(void)
{
    unsigned long flags;
    unsigned int value, failed;

    asm volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(flags) :: "memory");
    for (;;) {
        asm volatile("1: ldaxr %w0, [%2]\n"
                     "cbnz %w0, 1b\n"
                     "mov %w0, #1\n"
                     "stxr %w1, %w0, [%2]\n"
                     "cbnz %w1, 1b\n"
                     : "=&r"(value), "=&r"(failed)
                     : "r"(&route_lock)
                     : "memory");
        if (!failed)
            return flags;
    }
}

static void domain_unlock(unsigned long flags)
{
    asm volatile("stlr wzr, [%0]\nmsr daif, %1"
                 :: "r"(&route_lock), "r"(flags) : "memory");
}

static int register_route(unsigned int pintid, unsigned long vmid,
                          int vcpu_id, unsigned int vintid, int target_cpu,
                          unsigned int owner)
{
    unsigned long flags = domain_lock();
    int slot = -1;

    for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
        if (!routes[i].enabled) {
            if (slot < 0)
                slot = i;
            continue;
        }
        if (routes[i].pintid != pintid)
            continue;
        if (routes[i].owner != owner || routes[i].vmid != vmid) {
            domain_unlock(flags);
            return -1;
        }
        slot = i;
        break;
    }
    if (slot >= 0)
        routes[slot] = (struct irq_route){
            .pintid = pintid,
            .vintid = vintid,
            .vmid = vmid,
            .vcpu_id = vcpu_id,
            .target_cpu = target_cpu,
            .owner = owner,
            .enabled = 1,
        };
    domain_unlock(flags);
    return slot < 0 ? -1 : 0;
}

int irq_domain_register(unsigned int pintid, unsigned long vmid,
                        int vcpu_id, unsigned int vintid)
{
    if (!vmid || vmid > VM_MAX_COUNT || vcpu_id < 0 ||
        vcpu_id >= VM_MAX_VCPUS ||
        !((vintid >= 32 && vintid < 1020) ||
          (vintid >= 8192 && vintid < 16384)) ||
        !((pintid >= 32 && pintid < 1020) ||
          (pintid >= 8192 && pintid < 16384)))
        return -1;
    return register_route(pintid, vmid, vcpu_id, vintid, 0,
                          IRQ_DOMAIN_OWNER_VM);
}

int irq_domain_lookup(unsigned int pintid, struct irq_route *route)
{
    unsigned long flags = domain_lock();
    int ret = -1;

    for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
        if (routes[i].enabled && routes[i].pintid == pintid) {
            *route = routes[i];
            ret = 0;
            break;
        }
    }
    domain_unlock(flags);
    return ret;
}

int irq_domain_unregister(unsigned int pintid)
{
    unsigned long flags = domain_lock();
    int ret = -1;

    for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
        if (routes[i].enabled && routes[i].pintid == pintid &&
            routes[i].owner == IRQ_DOMAIN_OWNER_VM) {
            routes[i].enabled = 0;
            ret = 0;
            break;
        }
    }
    domain_unlock(flags);
    return ret;
}

int irq_domain_unregister_owned(unsigned int pintid, unsigned long vmid)
{
    unsigned long flags = domain_lock();
    int ret = -1;

    for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
        if (routes[i].enabled && routes[i].pintid == pintid &&
            routes[i].owner == IRQ_DOMAIN_OWNER_VM &&
            routes[i].vmid == vmid) {
            routes[i].enabled = 0;
            ret = 0;
            break;
        }
    }
    domain_unlock(flags);
    return ret;
}

int irq_domain_unregister_owned_range(unsigned int first_pintid,
                                      unsigned int count, unsigned long vmid)
{
    unsigned long flags = domain_lock();
    unsigned int matched = 0;

    for (unsigned int n = 0; n < count; n++) {
        unsigned int pintid = first_pintid + n;
        int found = 0;

        for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
            if (!routes[i].enabled || routes[i].pintid != pintid)
                continue;
            if (routes[i].owner != IRQ_DOMAIN_OWNER_VM ||
                routes[i].vmid != vmid) {
                domain_unlock(flags);
                return -1;
            }
            found = 1;
            break;
        }
        if (!found) {
            domain_unlock(flags);
            return -1;
        }
        matched++;
    }

    for (unsigned int n = 0; n < matched; n++) {
        unsigned int pintid = first_pintid + n;
        for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
            if (routes[i].enabled && routes[i].pintid == pintid &&
                routes[i].owner == IRQ_DOMAIN_OWNER_VM &&
                routes[i].vmid == vmid) {
                routes[i].enabled = 0;
                break;
            }
        }
    }
    domain_unlock(flags);
    return 0;
}

int irq_domain_audit_owned_range(unsigned int first_pintid,
                                 unsigned int count, unsigned long vmid,
                                 unsigned int first_vintid)
{
    unsigned long flags = domain_lock();

    for (unsigned int n = 0; n < count; n++) {
        unsigned int pintid = first_pintid + n;
        int found = 0;

        for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
            if (!routes[i].enabled || routes[i].pintid != pintid)
                continue;
            if (routes[i].owner != IRQ_DOMAIN_OWNER_VM ||
                routes[i].vmid != vmid ||
                routes[i].vintid != first_vintid + n) {
                domain_unlock(flags);
                return -1;
            }
            found = 1;
            break;
        }
        if (!found) {
            domain_unlock(flags);
            return -1;
        }
    }
    domain_unlock(flags);
    return 0;
}

int irq_domain_unregister_vm(unsigned long vmid)
{
    unsigned long flags = domain_lock();
    int removed = 0;

    for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
        if (routes[i].enabled && routes[i].owner == IRQ_DOMAIN_OWNER_VM &&
            routes[i].vmid == vmid) {
            routes[i].enabled = 0;
            removed++;
        }
    }
    domain_unlock(flags);
    return removed;
}

int irq_domain_update_vcpu_affinity(unsigned long vmid, int vcpu_id,
                                    int target_cpu)
{
    unsigned int irqs[IRQ_DOMAIN_MAX_ROUTES];
    unsigned int count = 0;
    unsigned long flags;

    if (target_cpu < 0 || target_cpu >= MAX_CPUS)
        return -1;
    flags = domain_lock();
    for (int i = 0; i < IRQ_DOMAIN_MAX_ROUTES; i++) {
        if (routes[i].enabled && routes[i].owner == IRQ_DOMAIN_OWNER_VM &&
            routes[i].vmid == vmid && routes[i].vcpu_id == vcpu_id) {
            routes[i].target_cpu = target_cpu;
            if (routes[i].pintid >= 32 && routes[i].pintid < 1020)
                irqs[count++] = routes[i].pintid;
        }
    }
    domain_unlock(flags);

    for (unsigned int i = 0; i < count; i++)
        gic_set_spi_target(irqs[i], target_cpu);
    return (int)count;
}

int irq_domain_init(void)
{
    memzero(routes, sizeof(routes));
    route_lock = 0;
    for (unsigned int irq = 0; irq < 16; irq++)
        if (register_route(irq, 0, 0, 0, 0, IRQ_DOMAIN_OWNER_HYP) < 0)
            return -1;
    for (unsigned int irq = 16; irq < 32; irq++)
        if (register_route(irq, 0, 0, 0, 0, IRQ_DOMAIN_OWNER_HYP) < 0)
            return -1;
    if (register_route(16 + 26, 0, 0, 0, 0,
                      IRQ_DOMAIN_OWNER_HYP) < 0 ||
        register_route(16 + 27, 0, 0, 0, 0,
                       IRQ_DOMAIN_OWNER_HYP) < 0 ||
        register_route(65, 0, 0, 0, 0, IRQ_DOMAIN_OWNER_HYP) < 0)
        return -1;
    return 0;
}

int irq_domain_selftest(void)
{
    struct irq_route route;
    int ret = 0;

    if (irq_domain_register(16383, 1, 1, 80) < 0 ||
        irq_domain_register(16383, 2, 0, 81) != -1 ||
        irq_domain_register(65, 1, 0, 80) != -1 ||
        irq_domain_register(16382, 1, VM_MAX_VCPUS, 80) != -1 ||
        irq_domain_lookup(16383, &route) < 0 || route.vmid != 1 ||
        route.vcpu_id != 1 || route.vintid != 80 ||
        irq_domain_update_vcpu_affinity(1, 1, 1) != 0 ||
        irq_domain_lookup(16383, &route) < 0 || route.target_cpu != 1)
        ret = -1;
    if (irq_domain_audit_owned_range(16383, 1, 1, 80) < 0 ||
        irq_domain_audit_owned_range(16383, 1, 2, 80) != -1 ||
        irq_domain_audit_owned_range(16383, 1, 1, 81) != -1 ||
        irq_domain_unregister_owned(16383, 2) != -1 ||
        irq_domain_lookup(16383, &route) < 0 || route.vmid != 1 ||
        irq_domain_unregister_owned(16383, 1) < 0 ||
        irq_domain_lookup(16383, &route) != -1)
        ret = -1;
    return ret;
}
