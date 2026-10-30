// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

struct task_struct;

#define VM_MAX_COUNT 4
#define VM_MAX_VCPUS 4

struct vm {
    unsigned long vmid;
    unsigned int nr_vcpus;
    struct task_struct *vcpus[VM_MAX_VCPUS];
};

int vm_attach_vcpu(struct task_struct *vcpu, unsigned long vmid, int vcpu_id);
int vm_rebind_vcpu(struct task_struct *vcpu, unsigned long vmid, int vcpu_id);
void vm_detach_vcpu(struct task_struct *vcpu);
struct task_struct *vm_find_vcpu(unsigned long vmid, int vcpu_id);
int vm_registry_selftest(void);
