// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 vITS（虚拟 ITS）接口
 *
 * Phase 9: vITS 虚拟化
 *
 * vITS 是向 Guest 呈现的虚拟 Interrupt Translation Service，
 * Guest 通过标准 GITS 寄存器（CTLR/TYPER/CBASER/BASER/CWRITER/...）
 * 发起 ITS 命令，Hypervisor trap-and-emulate，翻译后下发到物理 ITS。
 *
 * 核心映射关系：
 *   Guest vDevID + vEventID + vICID  --vITS-->  vINTID（虚拟 LPI/SPI）
 *                                                |
 *                                                v
 *   物理 pDevID + pEventID + pICID  --pITS-->  pLPI（物理 LPI）
 *
 * 物理 LPI 到来时，查表找到 (vmid, vINTID) 并注入虚拟中断。
 *
 * 简化实现（Phase 9，够用即可）：
 *   - 单 VM（VM1）使用 vITS
 *   - 每个 vITS 实例有自己的命令队列、设备表、集合表（记录 Guest GPA）
 *   - vDevID 直接映射为物理 DevID（同一设备只有一个 Guest 访问）
 *   - vEventID 直接映射为物理 EventID（同一张 ITT）
 *   - vINTID 由 Guest 在 MAPTI 时指定（支持 vLPI 范围）
 *   - 维护 pLPI -> (vmid, vINTID) 全局映射表，中断注入用
 */

#pragma once

#include "common/types.h"
#include "common/sched.h"

/* vITS 虚拟基地址（Guest IPA 空间中的地址） */
#define VITS_BASE        0x0C000000UL
#define VITS_SIZE        0x00010000UL   /* 64KB */

/*
 * vITS 寄存器偏移（与 GICv3 ITS 规范一致，从 GITS_* 偏移复制）
 * 只模拟 Guest 驱动实际会用到的寄存器：
 *   CTLR / TYPER / CBASER / CWRITER / CREADR / BASER0 / BASER1 / TRANSLATER
 */
#define VITS_CTLR         0x0000   /* 控制：bit0=Enabled */
#define VITS_TYPER        0x0008   /* 能力（RO） */
#define VITS_CBASER       0x0080   /* 命令队列基址 */
#define VITS_CWRITER      0x0088   /* 命令队列写指针 */
#define VITS_CREADR       0x0090   /* 命令队列读指针 */
#define VITS_BASER(n)     (0x0100 + (n) * 8)  /* 表基址 n=0..7 */
#define VITS_TRANSLATER   0x10040  /* MSI doorbell（WO） */

/* ITS 命令操作码（与物理 ITS 一致） */
#define VITS_CMD_SYNC    0x05
#define VITS_CMD_MAPD    0x08
#define VITS_CMD_MAPC    0x09
#define VITS_CMD_MAPTI   0x0A

/* 命令大小：32 字节 */
#define VITS_CMD_SIZE    32

/*
 * per-VM vITS 状态
 *
 * 每个直通 MSI 设备的 VM 有一个 vITS 实例。
 * 记录 Guest 写入的寄存器值，以及 v->p 映射关系。
 */
struct vits_state {
    /* 寄存器值（Guest 视角的 GPA，不是 PA） */
    uint32_t ctlr;          /* GITS_CTLR */
    uint64_t cbaser;        /* GITS_CBASER（Guest GPA） */
    uint64_t cwriter;       /* GITS_CWRITER（字节偏移） */
    uint64_t creadr;        /* GITS_CREADR（字节偏移） */
    uint64_t baser[8];      /* GITS_BASERn（Guest GPA） */

    /*
     * 命令队列大小（字节）。
     * 由 CBASER.SIZE 字段决定，简化实现固定 4KB（128 条命令）。
     */
    uint32_t cmdq_bytes;

    /* 所属 VM */
    struct task_struct *vm;

    /* MMIO 写缓存：64 位寄存器分两次 32 位写时，缓存低 32 位 */
    uint32_t write_lo_cache[8];
};

/* ---------- 全局接口 ---------- */

/*
 * vITS 全局初始化（在物理 ITS 初始化之后调用）
 *   - 初始化全局 pLPI -> vINTID 映射表
 */
void vits_global_init(void);

/*
 * 初始化 per-VM vITS 状态
 *   - 清零状态结构
 *   - 设置 TYPER 默认值
 */
void vits_init_vm(struct vits_state *vits, struct task_struct *vm);

/*
 * vITS MMIO 读（Guest 读 GITS 寄存器时 trap 进来）
 */
uint64_t vits_mmio_read(struct vits_state *vits, uint32_t offset);

/*
 * vITS MMIO 写（Guest 写 GITS 寄存器时 trap 进来）
 *   - 写 CWRITER 触发命令处理
 */
void vits_mmio_write(struct vits_state *vits, uint32_t offset, uint64_t val);

/*
 * 查询物理 LPI 对应的虚拟中断，返回 0=找到，-1=未映射
 * 输出 vmid 和 vINTID。
 */
int vits_lpi_to_virq(unsigned int plpi, unsigned int *out_vmid,
                     unsigned int *out_vintid);
