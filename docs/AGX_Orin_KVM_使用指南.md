# AGX Orin 上使用 KVM 详细指导文档

## 目录

1. [概述](#1-概述)
2. [环境准备](#2-环境准备)
3. [检查虚拟化支持](#3-检查虚拟化支持)
4. [启用 KVM 内核模块](#4-启用-kvm-内核模块)
5. [安装用户态工具](#5-安装用户态工具)
6. [创建虚拟机镜像](#6-创建虚拟机镜像)
7. [启动第一个虚拟机](#7-启动第一个虚拟机)
8. [网络配置](#8-网络配置)
9. [GPU 直通 (vGPU / SR-IOV)](#9-gpu-直通-vgpu--sr-iov)
10. [性能调优](#10-性能调优)
11. [常见问题与排查](#11-常见问题与排查)
12. [参考资料](#12-参考资料)

---

## 1. 概述

### 1.1 AGX Orin 简介

NVIDIA Jetson AGX Orin 是面向 AI 和机器人应用的高性能边缘计算平台，基于 NVIDIA Orin SoC，具备以下关键特性：

- **CPU**：12 核 ARM Cortex-A78AE（最高 2.2 GHz），ARMv8.2 架构，支持硬件虚拟化扩展
- **GPU**：NVIDIA Ampere 架构，最高 2048 个 CUDA 核心 + 64 个 Tensor 核心
- **内存**：最高 64GB LPDDR5
- **虚拟化支持**：ARMv8.1-A VHE（Virtualization Host Extensions）+ GICv3 ITS

### 1.2 KVM 在 AGX Orin 上的定位

KVM（Kernel-based Virtual Machine）是 Linux 内核中的虚拟化模块，在 ARM 平台上利用 ARMv8 的硬件虚拟化扩展（EL2）实现虚拟机。

在 AGX Orin 上使用 KVM 的典型场景：
- 安全域隔离（安全关键域 + 非安全域）
- 多操作系统同时运行（Linux + RTOS）
- 开发/测试环境隔离
- 容器底层的硬件辅助隔离

### 1.3 前置知识要求

- Linux 系统基本操作（命令行、服务管理）
- ARM 架构基础概念（EL0/EL1/EL2、异常级别）
- 虚拟化基础概念（VM、vCPU、内存虚拟化、设备模拟）
- 基本的网络和存储知识

---

## 2. 环境准备

### 2.1 硬件要求

| 项目 | 最低要求 | 推荐配置 |
|------|---------|---------|
| 平台 | Jetson AGX Orin 8GB | Jetson AGX Orin 32GB/64GB |
| 存储 | 32GB SD 卡或 eMMC | 128GB NVMe SSD |
| 内存 | 8GB | 16GB+ |
| 网络 | 千兆以太网 | 千兆以太网 + WiFi |

### 2.2 软件版本

建议使用的 JetPack 版本：
- **JetPack 5.x**（基于 Ubuntu 20.04，内核 5.10）
- **JetPack 6.x**（基于 Ubuntu 22.04，内核 5.15+，推荐）

> **注意**：JetPack 5.x 起 NVIDIA 官方支持 KVM，但配置可能不完善。JetPack 6.x 的 KVM 支持更完整。

### 2.3 烧写系统

使用 NVIDIA SDK Manager 或命令行烧写 JetPack 到 AGX Orin：

**方法一：SDK Manager（推荐新手）**

1. 在 Ubuntu PC 上下载安装 [NVIDIA SDK Manager](https://developer.nvidia.com/sdk-manager)
2. 启动 SDK Manager，登录 NVIDIA 账号
3. 将 AGX Orin 进入 Recovery 模式：
   - 接通电源
   - 按住 FC REC 按钮（中间的）
   - 按一下 RST 按钮（最右边的）
   - 松开 FC REC
4. USB-C 连接 PC 和 Orin 的 USB OTG 口
5. SDK Manager 中选择对应的 Jetson 型号和 JetPack 版本
6. 按照向导完成烧写

**方法二：命令行烧写**

```bash
# 在 Ubuntu PC 上下载 BSP
wget https://developer.nvidia.com/downloads/jetson-linux-r36-3
tar xpf Jetson_Linux_R36.3.0_aarch64.tbz2
cd Linux_for_Tegra

# Orin 进入 Recovery 模式后，执行烧写
sudo ./flash.sh jetson-agx-orin-devkit mmcblk0p1
```

### 2.4 初始配置

烧写完成后首次启动，按照向导完成：
1. 接受许可协议
2. 设置用户名和密码
3. 配置时区和键盘布局
4. 连接网络
5. 完成系统初始化

---

## 3. 检查虚拟化支持

### 3.1 检查 CPU 虚拟化扩展

登录 AGX Orin 后，首先确认 CPU 是否支持虚拟化：

```bash
# 检查 FEAT_VHE（Virtualization Host Extensions）
cat /proc/cpuinfo | grep -E "features|flags"
```

在 features 行中查找以下特性：
- `fp` / `asimd` — 浮点/SIMD
- `evtstrm` — 事件流
- `aes` / `sha1` / `sha2` — 加密扩展
- **`crc32`** — CRC32 指令
- **`atomics`** — 原子操作（ARMv8.1）
- **`fphp` / `asimdhp`** — 半精度浮点

> **注意**：ARM Linux 默认不会在 `/proc/cpuinfo` 中显示虚拟化相关的 feature flag。KVM 支持需要 CPU 实现了 EL2 虚拟化扩展。

### 3.2 检查内核 KVM 支持

```bash
# 检查内核 KVM 配置
zcat /proc/config.gz | grep -i kvm
```

预期输出（至少包含以下几项）：
```
CONFIG_KVM=y
CONFIG_KVM_ARM_HOST=y
CONFIG_KVM_GENERIC_DIRTYLOG_READ_PROTECT=y
CONFIG_HAVE_KVM=y
CONFIG_HAVE_KVM_IRQCHIP=y
CONFIG_HAVE_KVM_EVENTFD=y
```

如果 `CONFIG_KVM` 是 `m`（模块），需要手动加载模块。
如果是 `# CONFIG_KVM is not set`，则需要重新编译内核。

### 3.3 检查 KVM 设备节点

```bash
# 检查 /dev/kvm 是否存在
ls -l /dev/kvm
```

如果存在，说明 KVM 已经启用：
```
crw-rw----+ 1 root kvm 10, 232 ... /dev/kvm
```

如果不存在，说明 KVM 没有被启用或内核不支持。继续阅读下一节。

### 3.4 检查当前异常级别

```bash
# 检查启动级别（需要 root 权限安装工具）
sudo apt install -y msr-tools
# 或者直接读系统寄存器（需要内核模块支持）
```

更简单的方法：检查启动日志中 KVM 的相关信息：

```bash
dmesg | grep -i kvm
```

如果看到类似 `kvm [1]: IPA Size Limit: 40bits` 的输出，说明 KVM 正常运行在 EL2。

---

## 4. 启用 KVM 内核模块

### 4.1 加载 KVM 模块

如果 KVM 编译为模块（`CONFIG_KVM=m`），手动加载：

```bash
# 加载 KVM ARM 模块
sudo modprobe kvm
sudo modprobe kvm_arm

# 验证加载
lsmod | grep kvm
```

预期输出：
```
kvm_arm               262144  0
kvm                   831488  1 kvm_arm
```

### 4.2 配置开机自动加载

```bash
# 添加到模块加载配置
echo "kvm" | sudo tee /etc/modules-load.d/kvm.conf
echo "kvm_arm" | sudo tee -a /etc/modules-load.d/kvm.conf
```

### 4.3 设置用户权限

将当前用户加入 `kvm` 组，避免每次使用都需要 root：

```bash
# 创建 kvm 组（如果不存在）
sudo groupadd -f kvm

# 将当前用户加入 kvm 组
sudo usermod -aG kvm $USER

# 重新登录或刷新组
newgrp kvm

# 验证
id | grep kvm
```

### 4.4 udev 规则设置权限

确保 `/dev/kvm` 的权限正确：

```bash
# 创建 udev 规则
sudo tee /etc/udev/rules.d/80-kvm.rules << 'EOF'
KERNEL=="kvm", GROUP="kvm", MODE="0660", OPTIONS+="static_node=kvm"
EOF

# 重新加载 udev 规则
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=kvm
```

### 4.5 验证 KVM 可用性

```bash
# 使用 kvm-ok 工具（如果安装了 cpu-checker）
sudo apt install -y cpu-checker
kvm-ok
```

预期输出：
```
INFO: /dev/kvm exists
KVM acceleration can be used
```

---

## 5. 安装用户态工具

### 5.1 QEMU 安装

QEMU 是 KVM 的主要用户态工具，负责设备模拟和虚拟机管理。

```bash
# 更新软件源
sudo apt update

# 安装 QEMU 和相关工具
sudo apt install -y \
    qemu-system-arm \
    qemu-utils \
    qemu-efi-aarch64 \
    libvirt-daemon-system \
    libvirt-clients \
    bridge-utils \
    virt-manager \
    virt-viewer
```

> **说明**：
> - `qemu-system-arm` — ARM 架构 QEMU（包含 aarch64 支持）
> - `qemu-utils` — qemu-img 等磁盘工具
> - `qemu-efi-aarch64` — ARM64 UEFI 固件（推荐用于虚拟机）
> - `libvirt-daemon-system` — libvirt 虚拟化管理服务
> - `virt-manager` — 图形化虚拟机管理器（可选）

### 5.2 验证 QEMU 安装

```bash
# 检查 QEMU 版本
qemu-system-aarch64 --version

# 检查 KVM 加速支持
qemu-system-aarch64 -accel help
```

预期输出中应包含 `kvm` 加速选项。

### 5.3 libvirt 配置

```bash
# 启动并启用 libvirtd 服务
sudo systemctl enable --now libvirtd

# 将用户加入 libvirt 组
sudo usermod -aG libvirt $USER
newgrp libvirt

# 验证
virsh list --all
```

---

## 6. 创建虚拟机镜像

### 6.1 创建磁盘镜像

使用 `qemu-img` 创建虚拟机磁盘：

```bash
# 创建一个 20GB 的 qcow2 格式磁盘镜像
qemu-img create -f qcow2 ubuntu-vm.qcow2 20G

# 查看镜像信息
qemu-img info ubuntu-vm.qcow2
```

输出示例：
```
image: ubuntu-vm.qcow2
file format: qcow2
virtual size: 20 GiB (21474836480 bytes)
disk size: 196 KiB
cluster_size: 65536
```

### 6.2 下载 ARM64 系统安装镜像

推荐使用 Ubuntu Server for ARM64：

```bash
# 下载 Ubuntu 22.04 Server ARM64（约 1.5GB）
wget https://cdimage.ubuntu.com/releases/22.04/release/ubuntu-22.04.3-live-server-arm64.iso

# 或者下载 Ubuntu 24.04
wget https://cdimage.ubuntu.com/releases/24.04/release/ubuntu-24.04-live-server-arm64.iso
```

也可以使用其他 ARM64 发行版：
- Debian ARM64：https://www.debian.org/ports/arm64/
- Fedora ARM64：https://fedoraproject.org/server/arm64
- Alpine Linux ARM64：https://alpinelinux.org/downloads/

### 6.3 准备 UEFI 固件

ARM64 虚拟机需要 UEFI 固件（类似 x86 的 BIOS）：

```bash
# qemu-efi-aarch64 包已经包含了固件，查找位置
dpkg -L qemu-efi-aarch64 | grep -E "(AAVMF|QEMU_EFI|efi)"

# 通常在以下路径：
ls /usr/share/qemu-efi-aarch64/
ls /usr/share/AAVMF/
```

找到 `AAVMF_CODE.fd`（或 `QEMU_EFI.fd`）作为虚拟机的 UEFI 固件。

为了持久化 UEFI 变量（如启动顺序），需要为每个 VM 复制一份可写的变量文件：

```bash
# 复制 UEFI 固件和变量模板
cp /usr/share/AAVMF/AAVMF_CODE.fd ./vm-code.fd
cp /usr/share/AAVMF/AAVMF_VARS.fd ./vm-vars.fd
```

---

## 7. 启动第一个虚拟机

### 7.1 最简命令行启动（测试用）

```bash
qemu-system-aarch64 \
    -machine virt,accel=kvm,gic-version=3 \
    -cpu host \
    -smp 2 \
    -m 2G \
    -bios /usr/share/AAVMF/AAVMF_CODE.fd \
    -drive if=none,file=ubuntu-vm.qcow2,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -cdrom ubuntu-22.04.3-live-server-arm64.iso \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-device,netdev=net0 \
    -nographic \
    -serial mon:stdio
```

参数说明：

| 参数 | 说明 |
|------|------|
| `-machine virt,accel=kvm,gic-version=3` | 使用 virt 机器类型，KVM 加速，GICv3 中断控制器 |
| `-cpu host` | 使用主机 CPU 特性（透传宿主 CPU 特性给 VM） |
| `-smp 2` | 2 个 vCPU |
| `-m 2G` | 2GB 内存 |
| `-bios ...` | UEFI 固件 |
| `-drive ... -device virtio-blk-device` | virtio 块设备（磁盘） |
| `-cdrom ...` | 挂载安装 ISO |
| `-netdev user,hostfwd=tcp::2222-:22` | 用户态网络，宿主 2222 端口转发到 VM 的 22 |
| `-nographic` | 无图形界面，串口输出到终端 |
| `-serial mon:stdio` | 串口和 QEMU monitor 都使用 stdio |

### 7.2 操作说明

- **退出 QEMU**：按 `Ctrl+A` 然后按 `X`
- **切换到 QEMU monitor**：按 `Ctrl+A` 然后按 `C`
- **从 monitor 返回串口**：输入 `cont` 回车

### 7.3 安装系统

1. 启动后进入 Ubuntu 安装界面
2. 按照向导完成语言、键盘、网络配置
3. 选择磁盘（`/dev/vda`）进行安装
4. 设置用户名和密码
5. 等待安装完成，选择重启

重启后，在 QEMU monitor 中弹出 ISO：
```
(qemu) eject ide0-cd0
```

然后重置虚拟机：
```
(qemu) system_reset
```

### 7.4 启动已安装的虚拟机

系统安装完成后，去掉 `-cdrom` 参数即可从磁盘启动：

```bash
qemu-system-aarch64 \
    -machine virt,accel=kvm,gic-version=3 \
    -cpu host \
    -smp 4 \
    -m 4G \
    -bios /usr/share/AAVMF/AAVMF_CODE.fd \
    -drive if=none,file=ubuntu-vm.qcow2,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-device,netdev=net0 \
    -nographic \
    -serial mon:stdio
```

### 7.5 SSH 登录虚拟机

```bash
# 从宿主登录到 VM（端口 2222 转发到 VM 的 22）
ssh -p 2222 username@localhost
```

### 7.6 使用 libvirt 管理（推荐）

创建虚拟机定义文件 `ubuntu-vm.xml`：

```xml
<domain type='kvm'>
  <name>ubuntu-vm</name>
  <uuid>12345678-1234-1234-1234-123456789abc</uuid>
  <memory unit='GiB'>4</memory>
  <vcpu placement='static'>4</vcpu>
  <os>
    <type arch='aarch64' machine='virt'>hvm</type>
    <loader readonly='yes' type='pflash'>/usr/share/AAVMF/AAVMF_CODE.fd</loader>
    <nvram>/var/lib/libvirt/qemu/nvram/ubuntu-vm_VARS.fd</nvram>
    <boot dev='hd'/>
  </os>
  <features>
    <gic version='3'/>
  </features>
  <clock offset='utc'/>
  <on_poweroff>destroy</on_poweroff>
  <on_reboot>restart</on_reboot>
  <on_crash>destroy</on_crash>
  <devices>
    <emulator>/usr/bin/qemu-system-aarch64</emulator>
    <disk type='file' device='disk'>
      <driver name='qemu' type='qcow2'/>
      <source file='/path/to/ubuntu-vm.qcow2'/>
      <target dev='vda' bus='virtio'/>
    </disk>
    <interface type='network'>
      <source network='default'/>
      <model type='virtio'/>
    </interface>
    <console type='pty'>
      <target type='serial' port='0'/>
    </console>
  </devices>
</domain>
```

导入并启动：

```bash
# 定义虚拟机
virsh define ubuntu-vm.xml

# 启动
virsh start ubuntu-vm

# 查看状态
virsh list

# 连接控制台
virsh console ubuntu-vm

# 关机
virsh shutdown ubuntu-vm

# 强制关机
virsh destroy ubuntu-vm
```

---

## 8. 网络配置

### 8.1 用户态网络（SLIRP）

最简单的网络模式，不需要额外配置：

```bash
-netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80 \
-device virtio-net-device,netdev=net0
```

特点：
- ✅ 配置简单，无需 root
- ✅ VM 可以访问外网
- ❌ 宿主主动访问 VM 需要端口转发
- ❌ 性能较差（走用户态协议栈）

### 8.2 桥接网络（推荐）

性能最好，VM 和宿主在同一个网络中：

**第 1 步：创建网桥**

```bash
# 安装桥接工具
sudo apt install -y bridge-utils

# 创建网桥 br0
sudo ip link add br0 type bridge

# 将物理网卡（如 eth0）加入网桥
sudo ip link set eth0 master br0

# 配置 IP（或用 DHCP）
sudo dhclient br0
```

**第 2 步：持久化网桥配置（Netplan）**

编辑 `/etc/netplan/01-netcfg.yaml`：

```yaml
network:
  version: 2
  ethernets:
    eth0:
      dhcp4: no
  bridges:
    br0:
      interfaces: [eth0]
      dhcp4: yes
```

应用配置：
```bash
sudo netplan apply
```

**第 3 步：QEMU 使用桥接网络**

```bash
-netdev bridge,id=net0,br=br0 \
-device virtio-net-device,netdev=net0
```

需要 QEMU 桥接辅助工具的权限：
```bash
sudo chmod u+s /usr/lib/qemu/qemu-bridge-helper
```

### 8.3 macvtap 模式

另一种高性能方案，VM 直接连接物理网卡：

```bash
# 创建 macvtap 接口
sudo ip link add link eth0 name macvtap0 type macvtap mode bridge
sudo ip link set macvtap0 up

# 获取接口号
ip -d link show macvtap0
```

QEMU 参数：
```bash
-netdev tap,id=net0,fd=3 3<>/dev/tap$(cat /sys/class/net/macvtap0/ifindex) \
-device virtio-net-device,netdev=net0,mac=$(cat /sys/class/net/macvtap0/address)
```

### 8.4 libvirt 网络

libvirt 提供了多种网络模式，通过 `virsh` 管理：

```bash
# 查看可用网络
virsh net-list --all

# 默认 NAT 网络（通常已经存在）
virsh net-start default
virsh net-autostart default

# 创建桥接网络
virsh net-define bridge-net.xml
virsh net-start bridge-net
```

---

## 9. GPU 直通 (vGPU / SR-IOV)

### 9.1 GPU 虚拟化方案对比

| 方案 | 说明 | 性能 | 隔离性 |
|------|------|------|--------|
| **GPU 直通** | 整个 GPU 分配给一个 VM | 最高（接近原生） | 最好 |
| **vGPU / SR-IOV** | GPU 划分为多个虚拟 GPU | 高 | 好 |
| **CUDA 仅 API 转发** | 宿主 CUDA 转发给 VM | 中等 | 一般 |

### 9.2 GPU 直通（整卡直通）

将整个 GPU 分配给一个虚拟机使用。

**第 1 步：检查 GPU 设备**

```bash
lspci | grep -i nvidia
```

记录 GPU 的 PCI BDF（如 `0000:00:00.0`）。

**第 2 步：启用 IOMMU**

编辑内核启动参数，添加 IOMMU 支持：

```bash
# 编辑 /boot/extlinux/extlinux.conf
sudo vi /boot/extlinux/extlinux.conf
```

在 APPEND 行添加：
```
iommu=pt arm-smmu.disable_bypass=0
```

重启后验证：
```bash
dmesg | grep -i iommu
dmesg | grep -i smmu
```

**第 3 步：绑定 vfio-pci 驱动**

```bash
# 加载 vfio 模块
sudo modprobe vfio
sudo modprobe vfio-pci

# 查看 GPU 厂商和设备 ID
lspci -n -s 0000:00:00.0
# 输出类似：0000:00:00.0 0300: 10de:2204 (rev a1)

# 绑定到 vfio-pci
echo "10de 2204" | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id
```

**第 4 步：QEMU 直通 GPU**

```bash
qemu-system-aarch64 \
    ... \
    -device vfio-pci,host=0000:00:00.0 \
    ...
```

### 9.3 vGPU / SR-IOV（多 VM 共享 GPU）

NVIDIA Orin 的 GPU 支持 SR-IOV，可以在多个 VM 之间共享 GPU。

> **注意**：AGX Orin 的 vGPU/SR-IOV 支持需要 NVIDIA 特定的驱动和配置，且需要商业授权。详细配置请参考 NVIDIA 官方文档。

基本步骤概述：

1. 安装 NVIDIA vGPU 驱动（vGPU Manager）
2. 启用 SR-IOV 并创建虚拟功能（VF）
3. 将 VF 分配给各个 VM
4. 在 VM 中安装 vGPU 客户机驱动

---

## 10. 性能调优

### 10.1 CPU 调优

```bash
# 使用 host-passthrough CPU 模式（最多特性）
-cpu host
# 或指定具体特性
-cpu host,aarch64=on,fp=on,asimd=on
```

vCPU 亲和性绑定：
```bash
# 在 libvirt 中配置
<vcpu placement='static' cpuset='4-7'>4</vcpu>
```

### 10.2 内存调优

```bash
# 使用大页内存（HugePages）
# 1. 预留大页
echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 2. QEMU 参数
-mem-path /dev/hugepages
-mem-prealloc
```

### 10.3 存储调优

```bash
# 使用 virtio-blk 而不是 ide/scsi
-device virtio-blk-device

# 使用 aio=native 和 cache=none 提高性能
-drive if=none,file=disk.qcow2,id=hd0,cache=none,aio=native
```

### 10.4 网络调优

```bash
# 使用 virtio-net + vhost-net
-netdev tap,id=net0,vhost=on
-device virtio-net-device,netdev=net0

# 增加队列数（多队列）
-netdev tap,id=net0,queues=4,vhost=on
-device virtio-net-device,netdev=net0,mq=on,vectors=10
```

### 10.5 中断调优

```bash
# 使用 GICv3 + ITS
-machine virt,gic-version=3,its=on
```

---

## 11. 常见问题与排查

### 11.1 /dev/kvm 不存在

**可能原因**：
1. 内核没有启用 KVM
2. CPU 没有启动到 EL2
3. 安全固件禁用了虚拟化

**排查方法**：
```bash
# 检查内核配置
zcat /proc/config.gz | grep KVM

# 检查 dmesg 中的 KVM 信息
dmesg | grep -i kvm

# 检查启动级别
# 如果内核启动在 EL1，KVM 无法工作
dmesg | grep -i "Booting Linux"
```

**JetPack 上启用 KVM**：
NVIDIA 官方的 JetPack 默认可能没有完全启用 KVM，可能需要：
1. 更新到最新 JetPack 版本
2. 检查设备树中 `hypervisor` 节点配置
3. 确保引导加载程序（UEFI/U-Boot）启动到 EL2

### 11.2 QEMU 报错 "kvm run failed"

检查：
```bash
dmesg | tail -30
```

常见原因：内存不足、CPU 模式不支持、GIC 版本不匹配。

### 11.3 虚拟机无法启动到 UEFI Shell

可能是固件路径不对或固件损坏。尝试：
```bash
# 重新安装固件包
sudo apt reinstall qemu-efi-aarch64

# 使用正确的固件路径
-bios /usr/share/AAVMF/AAVMF_CODE.fd
```

### 11.4 网络不通

```bash
# 检查 VM 内网络接口
ip addr
ip route

# 检查 QEMU 网络后端
# 对于 user 模式，确认 hostfwd 正确
# 对于 bridge 模式，确认网桥配置
brctl show
ip link show
```

### 11.5 性能很差

检查是否真的使用了 KVM 加速：
```bash
# 在 QEMU monitor 中
(qemu) info kvm

# 或使用 ps 查看
ps aux | grep qemu
# 确认命令行中有 -accel=kvm 或 -machine accel=kvm
```

如果 KVM 没有启用，QEMU 会用纯软件模拟，性能会很差。

### 11.6 GPU 直通失败

检查 IOMMU 是否启用：
```bash
dmesg | grep -i iommu
dmesg | grep -i smmu

# 检查 vfio 模块
lsmod | grep vfio

# 检查设备是否绑定到 vfio-pci
lspci -k -s 0000:00:00.0
```

---

## 12. 参考资料

### 官方文档
- [NVIDIA Jetson Linux Developer Guide](https://docs.nvidia.com/jetson/archives/r36.3/DeveloperGuide/index.html)
- [KVM 官方文档](https://www.kernel.org/doc/html/latest/virt/kvm/index.html)
- [QEMU 官方文档](https://www.qemu.org/docs/master/)
- [libvirt 官方文档](https://libvirt.org/docs.html)

### ARM 虚拟化
- [ARM Architecture Reference Manual (ARMv8)](https://developer.arm.com/documentation/ddi0487/latest)
- [ARM Virtualization Extensions](https://developer.arm.com/architectures/learn-the-architecture/virtualization)

### 社区资源
- [Jetson 论坛 - Virtualization](https://forums.developer.nvidia.com/c/agx-autonomous-machines/jetson-ebooks/jetson-virtualization/235)
- [KVM ARM 邮件列表](https://lore.kernel.org/kvmarm/)

---

**文档版本**：v1.0  
**最后更新**：2026-10-07  
**适用平台**：NVIDIA Jetson AGX Orin (JetPack 5.x / 6.x)
