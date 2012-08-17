/*
 * Softmmu related functions
 *
 * Copyright (C) 2012 Guan Xuetao
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation, or any later version.
 * See the COPYING file in the top-level directory.
 */
#include "cpu.h"
#include "dyngen-exec.h"
#include "helper.h"

#ifdef CONFIG_USER_ONLY
#error This file only exist under softmmu circumstance
#endif

#undef DEBUG_UC64

#ifdef DEBUG_UC64
#define DPRINTF(fmt, ...) printf("%s: " fmt , __func__, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define SUPERPAGE_SIZE             (1 << 20)
#define UC64_PAGETABLE_READ        (1 << 7)
#define UC64_PAGETABLE_WRITE       (1 << 6)
#define UC64_PAGETABLE_EXEC        (1 << 5)
#define UC64_PAGETABLE_EXIST       (1 << 2)

#define MMUSUFFIX _mmu

#define SHIFT 0
#include "softmmu_template.h"

#define SHIFT 1
#include "softmmu_template.h"

#define SHIFT 2
#include "softmmu_template.h"

#define SHIFT 3
#include "softmmu_template.h"

#undef DEBUG_UC64

#ifdef DEBUG_UC64
#define DPRINTF(fmt, ...) printf("%s: " fmt , __func__, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

void tlb_fill(CPUUniCore64State *env1, target_ulong addr, int is_write,
        int mmu_idx, uintptr_t retaddr)
{
    TranslationBlock *tb;
    CPUUniCore64State *saved_env;
    int ret;

    saved_env = env;
    env = env1;

    ret = uc64_cpu_handle_mmu_fault(env, addr, is_write, mmu_idx);
    if (unlikely(ret)) {
        if (retaddr) {
            /* now we have a real cpu fault */
            tb = tb_find_pc(retaddr);
            if (tb) {
                /* the PC is inside the translated code.
                   It means that we have a virtual CPU fault */
                cpu_restore_state(tb, env, retaddr);
            }
        }
        cpu_loop_exit(env);
    }
    env = saved_env;
}

void switch_mode(CPUUniCore64State *env, int mode)
{
    int old_mode;
    int old_mode_idx;
    int mode_idx;

    old_mode = env->uncached_asr & ASR_MODE_SELECT;
    old_mode_idx = (old_mode == ASR_MODE_PRIV) ?
                    ASR_IDX_PRIV : ASR_IDX_USER;
    mode_idx = (mode == ASR_MODE_PRIV) ?
                    ASR_IDX_PRIV : ASR_IDX_USER;
    if (mode_idx == old_mode_idx) {
        return;
    }

    env->banked_r29[old_mode_idx] = env->regs[29];
    env->banked_r30[old_mode_idx] = env->regs[30];
    env->banked_bsr[old_mode_idx] = env->uncached_asr;
    env->banked_bfr[old_mode_idx] = env->uncached_afr;

    env->regs[29] = env->banked_r29[mode_idx];
    env->regs[30] = env->banked_r30[mode_idx];
    env->uncached_asr = env->banked_bsr[mode_idx];
    env->uncached_afr = env->banked_bfr[mode_idx];
}

void do_interrupt(CPUUniCore64State *env)
{
    uint64_t addr;

    switch (env->exception_index) {
    case UC64_EXCP_PRIV:
        addr = UC64_EXCP_PRIV;
        break;
    case UC64_EXCP_ITRAP:
        DPRINTF("itrap happend at %x\n", env->regs[31]);
        addr = UC64_EXCP_ITRAP;
        break;
    case UC64_EXCP_DTRAP:
        DPRINTF("dtrap happend at %x\n", env->regs[31]);
        addr = UC64_EXCP_DTRAP;
        break;
    case UC64_INTR_ITIMER:
    default:
        cpu_abort(env, "Unhandled exception 0x%x\n", env->exception_index);
        return;
    }
    /* Get exception virtual base address, only least 39 bits available */
    addr += (env->cp0.c9_excpbase);

    switch_mode(env, ASR_MODE_PRIV);
    env->uncached_asr = (env->uncached_asr & ~ASR_MODE_SELECT) | ASR_MODE_PRIV;
    env->uncached_asr |= ASR_INTR_SELECT;
    /* the PC already points to the proper instruction. */
    env->cp0.c4_epc = env->regs[31];
    env->regs[31] = addr;
    env->interrupt_request |= CPU_INTERRUPT_EXITTB;
}

static int get_phys_addr(CPUUniCore64State *env, target_ulong address,
        int access_type, int is_user, target_phys_addr_t *phys_ptr, int *prot,
        target_ulong *page_size)
{
    int code;
    uint64_t table;
    uint64_t desc;
    target_phys_addr_t phys_addr;

    /* Pagetable walk.  */
    /* Lookup l1 descriptor.  */
    table = env->cp0.c2_base & 0xffffff000ULL;
    table |= (address >> 27) & 0xff8;
    desc = ldl_phys(table);
    code = 0;
    if (!(desc & UC64_PAGETABLE_EXIST)) {
        code = 0x02; /* second pagetable miss */
        goto do_fault;
    }

    /* Lookup l2 descriptor.  */
    table = (desc & 0xffffff000ULL) | ((address >> 18) & 0xff8);
    desc = ldl_phys(table);
    switch (desc & 1) {
    case 1:
        /* Superpage  */
        if (!(desc & UC64_PAGETABLE_EXIST)) {
            code = 0x05; /* superpage miss */
            goto do_fault;
        }
        phys_addr = (desc & 0xfffe00000ULL) | (address & 0x1fffff);
        *page_size = SUPERPAGE_SIZE;
        break;
    case 0:
        cpu_abort(env, "4k page type not supported yet!");
    default:
        cpu_abort(env, "wrong page type!");
    }

    *phys_ptr = phys_addr;
    *prot = 0;
    /* Check access permissions.  */
    if (desc & UC64_PAGETABLE_READ) {
        *prot |= PAGE_READ;
    } else {
        if (is_user && (access_type == 0)) {
            code = 0x6; /* access unreadable area */
            goto do_fault;
        }
    }

    if (desc & UC64_PAGETABLE_WRITE) {
        *prot |= PAGE_WRITE;
    } else {
        if (is_user && (access_type == 1)) {
            code = 0x7; /* access unwritable area */
            goto do_fault;
        }
    }

    if (desc & UC64_PAGETABLE_EXEC) {
        *prot |= PAGE_EXEC;
    } else {
        if (is_user && (access_type == 2)) {
            code = 0x6; /* access unexecutable area */
            goto do_fault;
        }
    }

do_fault:
    if (code) {
        env->cp0.c4_epc = address;
        if (access_type == 2) {
            env->cp0.c3_ifaultstatus = code;
            env->exception_index = UC64_EXCP_ITRAP;
        } else {
            env->cp0.c3_dfaultstatus = code;
            env->cp0.c4_dtrapaddr = address;
            env->exception_index = UC64_EXCP_DTRAP;
        }
    }
    return code;
}

int uc64_cpu_handle_mmu_fault(CPUUniCore64State *env, target_ulong address,
                              int access_type, int mmu_idx)
{
    target_phys_addr_t phys_addr;
    target_ulong page_size;
    int prot;
    int ret, is_user;

    is_user = mmu_idx == MMU_USER_IDX;
    if ((env->cp0.c1_sys & 1) == 0) {
        /* MMU disabled.  */
        phys_addr = address;
        prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        page_size = TARGET_PAGE_SIZE;
        ret = 0;
    } else {
        ret = get_phys_addr(env, address, access_type, is_user, &phys_addr,
                            &prot, &page_size);
    }

    if (ret == 0) {
        /* Map a single page.  */
        phys_addr &= TARGET_PAGE_MASK;
        address &= TARGET_PAGE_MASK;
        tlb_set_page(env, address, phys_addr, prot, mmu_idx, page_size);
    }

    return ret;
}

target_phys_addr_t cpu_get_phys_page_debug(CPUUniCore64State *env,
        target_ulong addr)
{
    cpu_abort(env, "%s not supported yet\n", __func__);
    return addr;
}
