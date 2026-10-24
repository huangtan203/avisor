# Phase 46：DTB 有效输入验证与动态平台信息

## 1. 阶段目标

在 Phase 45 平台探针基础上，完成 DTB 有效输入验证，并保持无 DTB 的 QEMU `virt` 启动行为不变：

- 兼容启动入口通过 `x0` 或 `x2` 传入 DTB；
- 正确解析根节点 `#address-cells` 和 `#size-cells`；
- 从 DTB 读取 CPU 数量和内存范围；
- 读取 QEMU virt 的 UART 和 GICD 地址；
- 保留固定布局 fallback；
- 回归 Phase 18–27。

## 2. DTB 传递

启动汇编同时保存入口寄存器：

- `x0`：兼容部分固件的 DTB 传递约定；
- `x2`：兼容 AArch64 Linux/QEMU 常见启动约定。

探针先检查 `x0` 的 FDT magic，不匹配时尝试 `x2`。两个地址都无效时进入 QEMU 固定布局 fallback。

DTB 地址的全局保存延后到 BSS 清零完成后，避免保存值被 BSS 初始化覆盖。

## 3. FDT 解析修正

解析器现在读取根节点的：

```text
#address-cells
#size-cells
```

并按实际 cell 数量解析 `reg`。这修复了 QEMU virt DTB 使用 2 个 size cells 时内存大小被截断为 0 的问题。

对 QEMU virt DTB 的结果为：

```text
cpu=2
memory=0x40000000+1024MB
uart=0x09000000
gicd=0x08000000
gicr=0x080A0000
```

CPU 统计限定为带有 `device_type = "cpu"` 的 CPU 节点。

## 4. QEMU 验证

强制构建：

```bash
make -B -f Makefile.virt -j$(nproc)
```

有效 DTB 路径：

```bash
qemu-system-aarch64 ... -dtb /tmp/avisor-virt.dtb
```

输出确认 DTB 为 `valid`，并正确得到上述 CPU、内存、UART 和 GIC 信息。

无 DTB fallback 路径也完成回归，仍使用 QEMU 固定布局且行为不变。

两条路径均验证 Phase 18–27：

- SMP 冻结与事务冻结；
- VM 级冻结和回滚；
- 双 vCPU 迁移；
- 源 VM 回收；
- CANCEL/FINISH abort；
- EL2 task 栈遥测；
- zombie task 回收；
- task 槽位复用。

未发现新增 `PANIC` 或 `FAIL`。

## 5. 当前边界

DTB 信息目前用于启动探针和平台识别，现有 QEMU 驱动仍使用固定平台宏。下一阶段才将 DTB 解析结果接入 UART、GIC、timer 和内存管理的实际初始化；AGX Orin 仍需 Tegra234 启动链路、UART、GIC/ITS、PSCI 和 SMMU 适配。
