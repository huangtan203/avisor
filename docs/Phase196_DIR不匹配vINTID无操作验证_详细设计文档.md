# Phase 196：DIR 写不匹配 vINTID 的无操作验证

## 1. 背景

Phase 194 第 7 节列出一项未测语义：`ICC_DIR_EL1` 写入一个不存在 active 软件 LR 的 vINTID 时，应为无操作。`vgic_handle_dir()` 的实现按 vINTID 匹配 active 的软件 LR，未匹配时直接返回，但此路径没有实测。

本阶段补充该探针，只验证这一点。

## 2. 实现

`guests/el1_test/main.c`，位于 `probe_dir_semantics()` 之后：

- `probe_dir_nomatch()`：注入 `P194_DIR_VIRQ`（101）。该 vINTID 在 guest handler 中只执行 EOIR 并返回，LR 保持 active。
- 读取 LR1 得到 `LR1_before_DIR`。
- 对 vINTID 102 执行 `icc_dir_write(P196_NOMATCH_VIRQ)`。102 没有对应 active 条目。
- 再读取 LR1 得到 `LR1_after_DIR(102)`。
- 判据：注入返回 0、EOIR 完成、`LR1_before_DIR` 的状态位为 ACTIVE（bit 63:62 = `10`），且 `LR1_after_DIR` 与 `LR1_before_DIR` 相等。

`main()` 在 `probe_dir_semantics()` 之后调用 `probe_dir_nomatch()`。

被测代码路径不变：`hypervisor/arch/aarch64/sync_exc.c` 的 DIR trap 分发调用 `vgic_handle_dir()`，后者在 vINTID 不匹配时不修改 LR。

## 3. 实测输出

构建：`make -B -f Makefile.virt -j2`，`build=0`。

QEMU 两轮（`timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img -display none -monitor none`，退出码 124）：

```
[vm2] Probe: ICC_DIR_EL1 no-match (VEOIM=1)
[vm2]   inject_ret=0 eoir_done=1 LR1_before_DIR=90a0000000000065 LR1_after_DIR(102)=90a0000000000065
[vm2]   PASSED: DIR 写不存在 active 条目的 vINTID 时 LR 不变
[vm1] Probe: ICC_DIR_EL1 no-match (VEOIM=1)
[vm1]   inject_ret=0 eoir_done=1 LR1_before_DIR=90a0000000000065 LR1_after_DIR(102)=90a0000000000065
[vm1]   PASSED: DIR 写不存在 active 条目的 vINTID 时 LR 不变
```

第二轮结果一致。`PANIC=0 FAIL=0 WARN=21`，与基线相同。

`LR1_before_DIR = 0x90a0000000000065`：bit 63:62 为 `10`（ACTIVE），bit 61 为 1（HW），低 32 位 `0x65` = 101。DIR(102) 后 LR1 不变，说明不匹配的写入没有清除 101 的 active 条目。

## 4. 结论

- DIR 写入不存在 active 条目的 vINTID（102）时，LR1 中 vINTID 101 的 active 条目保持不变。
- 该结论在 QEMU 两轮两个 VM 上一致。

## 5. 未验证的部分

- 只测了单条 active 条目（101）。多条同时 active 时，不匹配写入是否误清其他条目未测。
- 测的是软件 LR（LR1），HW 映射 LR 的行为未测。
- QEMU 环境的 GIC 行为可能与 Orin 实机存在差异，实机未验证。

## 6. 代码位置

| 文件 | 内容 |
|---|---|
| `hypervisor/boards/virt/vgic.c` | `vgic_handle_dir()`，被测实现 |
| `hypervisor/arch/aarch64/sync_exc.c` | DIR trap 分发 |
| `guests/el1_test/main.c` | `probe_dir_nomatch()`，`main()` 调用处 |

## 7. 复现

```
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
    -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
    -display none -monitor none -serial file:/tmp/run.out
```

退出码 124 为正常超时。检查日志中 `no-match (VEOIM=1)` 段落出现两次 PASSED。
