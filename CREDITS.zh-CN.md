# 署名与第三方声明

中文 | [**English**](CREDITS.md)

本次移植是派生作品。没有下面这些项目，这里什么都不会存在；它们的许可与作者身份
适用于它们所贡献的部分。

## 上游项目

### Project Silicium —— Mu-Silicium

* **仓库：** https://github.com/Project-Silicium/Mu-Silicium
* **固定版本：** `8221489a628aa47daa0776322ee17ce8dc7a9981`（见 `deps.lock`）
* **许可：** BSD-2-Clause-Patent（见 `LICENSE`）
* **我们使用：** 整个 UEFI 树。`Silicon/Silicium/SiliciumPkg`（SEC、DXE 胶合、
  `SimpleFbDxe`、`KeypadDxe`、`BootManagerLib`）、
  `Silicon/MediaTek/MediaTekPkg`（MT6833 支持、GPIO、PMIC、PMIC wrapper）、
  EDK2 构建集成，以及 `Platforms/` 布局约定。
* **我们为 evergo 改了什么：** 见本仓库的 `Silicon/` 与 `BootShim/` —— 那些是被
  修改的上游文件，为便于审阅而完整保留。设备包 `Platforms/Xiaomi/evergoPkg` 是
  为本次移植编写的。
* **说明：** 平台包起始于 Silicium 的 `lancelotPkg` / `MT6789Pkg` 布局，并针对
  evergo 的内存映射、帧缓冲与启动策略做了重构。凡是仍继承自上游的文件，其原始
  版权头都保持完整。

### UniXNU

* **仓库：** https://github.com/UniXNU/UniXNU
* **固定版本：** `d3de950a23a54d454d6ac21d8deceaa3f4ad0e73`（见 `deps.lock`）
* **许可：** BSD-2-Clause-Patent
* **我们使用：** `package/UniXNUPkg` —— 启动菜单应用、启动项模型、配置解析器与
  DarwinBoot 交接。由 `scripts/fetch-deps.sh` 在构建时拉取；**不**随本仓库分发，
  也未被修改。已验证构建运行的是原样上游代码。
* **我们改了什么：** 仅固件侧集成——启动管理器如何从固件卷找到、加载并启动
  `UniXNU.efi`（`BootManagerLib/BootManager.c`），以及 FV 包含哪些模块。

### hackios-pencilpen1 —— XNU loader 与 payload 容器设计

* **用于：** `Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c`
  与 `XnuLoader.h`，以及这两个文件所描述的 `XNUP` 容器格式。
* **我们使用：** stage-0 XNU loader 的设计：一个持有 `loader.efi` 的 `XNUP`
  容器、该容器的布局，以及基于信标的诊断。代码被移植后为 evergo 做了改动：本设备
  的热跳路径上没有 boot shim，所以容器被期待放在既有的 no-map DTB 预留
  `0xB5400000` 处，而非原版使用的地址；且缺少 `loader.efi` 时返回 BDS 而非冷复位。
* **来源：** 两个文件的源码注释都明确写出出处，使移植部分与 evergo 专属工作保持
  可区分。
* **许可：** BSD-2-Clause-Patent，与其余 UEFI 树一致。若你再分发本仓库，请保留
  来源注释。

### Microsoft Mu（mu_basecore、mu_plus、mu_oem_sample）

* **仓库：** https://github.com/microsoft/mu_basecore、
  https://github.com/microsoft/mu_plus、
  https://github.com/microsoft/mu_oem_sample
* **固定版本：** `3c9c7b2e211e62d2a954ad760a36fe05adc43aa3`、
  `b8d46c71610318e36a661819e1e3c0b8bdda413a`、
  `b70b4b7bb02c43bf9db2172dfb3ed0457e2361d6`
* **许可：** BSD-2-Clause-Patent
* **我们使用：** 固件所基于的 EDK2 核心。
* **我们改了什么：** `patches/0001-evergo-mu_basecore.patch` —— 19 个文件。实质性
  改动包括 GIC redistributor 唤醒与伪中断处理、针对热跳入口的通用定时器修正、
  面向 AArch64 的内存权限与异常处理器重构，以及用于诊断的 BDS/DXE 进度信标。补丁
  以 `git apply --ignore-whitespace` 应用到固定提交（上游文件使用 CRLF）。

### Project Silicium —— Device-Binaries 与 Silicium-ACPI

* **仓库：** https://github.com/Project-Silicium/Device-Binaries、
  https://github.com/Project-Silicium/Silicium-ACPI
* **固定版本：** `a1501e0c2e642d6ac0d8f52c85af2a67b75a7799`、
  `1c8bd63a7ffcaecf3c8764a6b01a11e053457543`
* **我们使用：** 预编译的 MT6833 blob（DDR 训练、PLL 与 PMIC 序列）以及 ACPI 表
  生成器。两者都作为依赖拉取，不在此提交。

### OpenSSL

* **仓库：** https://github.com/openssl/openssl
* **固定版本：** `67b5686b4419b4cb8caa502711c41815f5279751`
* **许可：** Apache-2.0（OpenSSL 3.x）
* **我们使用：** 由 `OpensslPkg` 链接，用于认证变量路径。作为子模块拉取，不提交。

### Android boot image 格式

* **来源：** Android Open Source Project，`system/tools/mkbootimg`
* **许可：** Apache-2.0
* **我们使用：** boot v1 头布局，在 `Resources/Scripts/mkbootimg.py` 与
  `scripts/make-boot-image.sh` 中重新实现。未复制任何 AOSP 代码。

## 设备专属材料

### `Resources/DTBs/evergo.dtb`

从本机型原厂固件中未经修改提取的设备树 blob。它是设备自身的硬件描述，并非在此
编写。厂商 DTB 的许可通常不明确，之所以包含它，是因为固件没有它就无法描述设备，
并在 README 中如此标注。若你要商业性再分发本仓库，请就你所在司法辖区核实其
法律地位。

### 实测硬件值

`MemoryMapLib`、`CamelliaBootMemoryDxe` 与 shim 中的若干常量是在真实设备上实测的
（GIC 与 PMIC 基址、显示子系统、面板几何、帧缓冲基址、预留 DRAM 窗口）。它们是
关于硬件的事实，记录在使用处旁边的注释中。

## 许可分层

适用两种不同的许可，它们兼容，因为这些作品是独立的：

| 部分 | 许可 | 原因 |
| --- | --- | --- |
| UEFI 固件、平台包、shim、文档 | BSD-2-Clause-Patent（`LICENSE`） | 继承自 UEFI 上游 |
| `stage1/evergo_stage1.c` | GPL-2.0 | 它是 Linux 内核模块；内核模块接口要求如此 |

内核模块链接的是 Linux 内核，而非固件。它自带 `MODULE_LICENSE("GPL")`，未被合并进
BSD 许可的固件二进制，所以二者不向对方施加条款。

## 未包含

本仓库有意**不**包含：

* 任何 Apple 代码、内核、kext、固件或二进制，
* `DarwinBoot` 或任何其他预编译的 XNU 启动组件，
* 预编译的热跳内核模块二进制（源码在 `stage1/`），
* 原厂固件镜像或分区转储，
* 第三方源码树（改为按固定版本拉取）。

## 如何署名本工作

如果你在本移植之上继续，请把 Project Silicium 与 UniXNU 作为上游署名，并单独注明
evergo 的适配：

> UEFI and XNU boot chain for the Redmi Note 11 5G (evergo, MT6833), derived
> from Project Silicium's Mu-Silicium and integrating UniXNU as the boot
> manager.
