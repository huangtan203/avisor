// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#define IRQ_DOMAIN_MAX_ROUTES 64
#define IRQ_DOMAIN_OWNER_HYP  0
#define IRQ_DOMAIN_OWNER_VM   1

struct irq_route {
    unsigned int pintid;
    unsigned int vintid;
    unsigned long vmid;
    int vcpu_id;
    int target_cpu;
    unsigned int owner;
    unsigned int enabled;
};

int irq_domain_init(void);
int irq_domain_register(unsigned int pintid, unsigned long vmid,
                        int vcpu_id, unsigned int vintid);
int irq_domain_unregister(unsigned int pintid);
int irq_domain_unregister_vm(unsigned long vmid);
int irq_domain_update_vcpu_affinity(unsigned long vmid, int vcpu_id,
                                    int target_cpu);
int irq_domain_lookup(unsigned int pintid, struct irq_route *route);
int irq_domain_selftest(void);
