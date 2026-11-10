# evergo（MT6833）移植

中文 | [**English**](PORTING-evergo.md)

> **进行中的工作。不要把这个分支当作可发布的固件。**

本包派生自 `lancelotPkg` 与 `MT6789Pkg`。它是 Redmi Note 11 5G（`evergo`，
联发科 MT6833 / 天玑 700）的可编译起点。

## 当前状态

Linux 热跳路径已在真机验证：

```text
LK
-> 项目 Linux 内核
-> 最小 initramfs
-> 线性 DRM 帧缓冲
-> uefi_jump.ko
-> UEFI
-> UniXNU
-> DarwinBoot
-> XNU 入口
```

UEFI 固件、散页帧缓冲影子、RamDisk payload 与 UniXNU 菜单已在真机上工作。
DarwinBoot 到达了 MMU 关闭的 XNU 入口。XNU 目前停在它的第一个 bootstrap 页表映射处；
因此本分支是开发快照，而非可启动的发行版。

## 已知硬件值

```text
SoC                 MT6833
CPU                 6 x Cortex-A55 + 2 x Cortex-A76
GICD                0x0C000000
GICR                0x0C040000
UART                0x11002000
Pinctrl             0x10005000
UFS HCI             0x11270000
UFS PHY             0x11FA0000
显示子系统          0x14000000
DSI0                0x14013000
帧缓冲              0x7BA10000
帧缓冲大小          0x01DE2000
帧缓冲模式          1088 x 7200 x 32 bpp（三个 1088 x 2400 缓冲）
像素格式            B,G,R,X（GOP PixelBlueGreenRedReserved8BitPerPixel）
```

boot image 配置使用实测的原厂值：

```text
base           = 0x40000000
kernel_offset  = 0x00080000
ramdisk_offset = 0x11100000
dtb_offset     = 0x07C80000
tags_offset    = 0x07C80000
header_version = 2
```

## 明确的占位

以下各项有意未完成，不得当作硬件支持：

1. `Resources/DTBs/evergo.dtb` 是从本设备官方 V13.0.9.0 boot image 中提取的、
   未经修改的 FDT。LK 的运行时修补仍需在真机上验证。
2. ACPI 仍使用 lancelot 的占位表。
3. `MemoryMapLib` 仍带有 lancelot 的预留与寄存器窗口。
4. `ClockImplLib`、`GpioImplLib`、`PmicWrapperImplLib` 与 `MsdcImplLib` 复制自
   MT6789，需要 MT6833/MT6359 的实现工作。
5. `PcdStorageIsEMMC` 被禁用，因为 evergo 使用 UFS 2.2。目前还没有 UFS DXE。
6. `SimpleFbDxe` 使用 `uefi_jump` 产生的页映射。它向 GOP 消费者提供线性影子，
   并把完成的更新拷入分散的扫描输出页。

## 下一个里程碑

- 在不绕过映射的前提下，诊断 XNU bootstrap 页表循环。
- 确认运行时启动参数、固定窗口归属与 MMU 交接。
- 到达 XNU `arm_init`，然后添加平台与显示驱动。
- 在打包发行镜像之前，移除所有剩余的移植期诊断。
