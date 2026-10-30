// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/vm.h"
#include "common/sched.h"
#include "common/utils.h"

static struct vm vm_table[VM_MAX_COUNT];
static volatile unsigned int vm_table_lock;

static void vm_lock(void)
{
    unsigned int failed;
    unsigned int value;

    for (;;) {
        asm volatile("1: ldaxr %w0, [%2]\n"
                     "cbnz %w0, 1b\n"
                     "mov %w0, #1\n"
                     "stxr %w1, %w0, [%2]\n"
                     "cbnz %w1, 1b\n"
                     : "=&r"(value), "=&r"(failed)
                     : "r"(&vm_table_lock)
                     : "memory");
        return;
    }
}

static void vm_unlock(void)
{
    asm volatile("stlr wzr, [%0]" :: "r"(&vm_table_lock) : "memory");
}

static struct vm *vm_slot(unsigned long vmid)
{
    if (vmid == 0 || vmid > VM_MAX_COUNT)
        return NULL;
    return &vm_table[vmid - 1];
}

int vm_attach_vcpu(struct task_struct *vcpu, unsigned long vmid, int vcpu_id)
{
    struct vm *vm = vm_slot(vmid);

    if (!vcpu || !vm || vcpu_id < 0 || vcpu_id >= VM_MAX_VCPUS)
        return -1;

    vm_lock();
    if (vm->vcpus[vcpu_id] && vm->vcpus[vcpu_id] != vcpu) {
        vm_unlock();
        return -1;
    }
    if (vcpu->vm && vcpu->vm != vm) {
        vm_unlock();
        return -1;
    }

    vm->vmid = vmid;
    if (!vm->vcpus[vcpu_id]) {
        vm->vcpus[vcpu_id] = vcpu;
        vm->nr_vcpus++;
    }
    vcpu->vm = vm;
    vcpu->vmid = vmid;
    vcpu->vcpu_id = vcpu_id;
    vcpu->cpu_sysregs.mpidr_el1 = (unsigned long)vcpu_id;
    vm_unlock();
    return 0;
}

int vm_rebind_vcpu(struct task_struct *vcpu, unsigned long vmid, int vcpu_id)
{
    struct vm *old_vm;
    struct vm *new_vm = vm_slot(vmid);
    int old_vcpu_id;

    if (!vcpu || !new_vm || vcpu_id < 0 || vcpu_id >= VM_MAX_VCPUS)
        return -1;

    vm_lock();
    old_vm = vcpu->vm;
    old_vcpu_id = vcpu->vcpu_id;
    if (new_vm->vcpus[vcpu_id] && new_vm->vcpus[vcpu_id] != vcpu) {
        vm_unlock();
        return -1;
    }

    if (old_vm && old_vcpu_id >= 0 && old_vcpu_id < VM_MAX_VCPUS &&
        old_vm->vcpus[old_vcpu_id] == vcpu) {
        old_vm->vcpus[old_vcpu_id] = NULL;
        if (old_vm->nr_vcpus)
            old_vm->nr_vcpus--;
        if (!old_vm->nr_vcpus)
            old_vm->vmid = 0;
    }

    new_vm->vmid = vmid;
    if (!new_vm->vcpus[vcpu_id]) {
        new_vm->vcpus[vcpu_id] = vcpu;
        new_vm->nr_vcpus++;
    }
    vcpu->vm = new_vm;
    vcpu->vmid = vmid;
    vcpu->vcpu_id = vcpu_id;
    vcpu->cpu_sysregs.mpidr_el1 = (unsigned long)vcpu_id;
    vm_unlock();
    return 0;
}

void vm_detach_vcpu(struct task_struct *vcpu)
{
    struct vm *vm;
    int vcpu_id;

    if (!vcpu || !vcpu->vm)
        return;

    vm_lock();
    vm = vcpu->vm;
    vcpu_id = vcpu->vcpu_id;
    if (vcpu_id >= 0 && vcpu_id < VM_MAX_VCPUS &&
        vm->vcpus[vcpu_id] == vcpu) {
        vm->vcpus[vcpu_id] = NULL;
        if (vm->nr_vcpus)
            vm->nr_vcpus--;
    }
    vcpu->vm = NULL;
    if (!vm->nr_vcpus)
        vm->vmid = 0;
    vm_unlock();
}

struct task_struct *vm_find_vcpu(unsigned long vmid, int vcpu_id)
{
    struct vm *vm = vm_slot(vmid);
    struct task_struct *vcpu;

    if (!vm || vcpu_id < 0 || vcpu_id >= VM_MAX_VCPUS)
        return NULL;

    vm_lock();
    vcpu = vm->vmid == vmid ? vm->vcpus[vcpu_id] : NULL;
    vm_unlock();
    return vcpu;
}

int vm_registry_selftest(void)
{
    int result = 0;

    vm_lock();
    for (int i = 0; i < VM_MAX_COUNT; i++) {
        struct vm *vm = &vm_table[i];
        unsigned int count = 0;

        if (!vm->vmid)
            continue;
        if (vm->vmid != (unsigned long)i + 1) {
            result = -1;
            break;
        }
        for (int v = 0; v < VM_MAX_VCPUS; v++) {
            struct task_struct *vcpu = vm->vcpus[v];

            if (!vcpu)
                continue;
            if (vcpu->vm != vm || vcpu->vmid != vm->vmid ||
                vcpu->vcpu_id != v) {
                result = -1;
                break;
            }
            count++;
        }
        if (result || count != vm->nr_vcpus) {
            result = -1;
            break;
        }
    }
    vm_unlock();
    return result;
}
