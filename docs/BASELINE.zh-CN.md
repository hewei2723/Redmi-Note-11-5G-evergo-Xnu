# 已验证基线

中文 | [**English**](BASELINE.md)

本仓库中「可用」的含义、它是如何确立的，以及如何检查一次构建是不是同一个东西。

## 里程碑

2026-11-09，设备显示了**真正的 UniXNU 启动菜单**——不是模拟图，也不是 UEFI 绘制的
仿制品：

* 竖屏 UniXNU 界面，带产品标识、`version 0.1.0` 与 `built-in configuration`
* 四个条目：`Return to firmware`、`Restart`、`Power off`、`Tools`
* 倒计时在走（`Booting Return to firmware in 8 s` → `7 s`）
* 状态行显示 `No input device`、`no boot entry found`、
  `drivers 0 loaded / 0 failed  kits 0  acpi 0`

证据：设备的屏幕录像，加上固件自身的进度信标显示 SEC、DXE 派发与 BDS
`AfterConsole` 均已完成。

## 它证明了什么，没证明什么

证明：

* 热跳路径在本设备上到达 UEFI，
* SEC 建立的内存映射在 MMU 开启后仍能撑住，
* DXE 派发了驱动集，BDS 运行了它的 `AfterConsole` 策略，
* 固件卷发布了 `UniXNU.efi`，启动管理器加载并启动了它，
* GOP 帧缓冲路径以原生几何到达面板。

**不**证明：

* XNU 执行了。从未观测到内核入口、`Darwin Kernel Version` 或任何内核控制台输出。
* 菜单输入可用。观测到的状态是 `No input device`。
* 存在启动项。观测到的状态是 `no boot entry found`。

## 确切的那次构建

```text
FD sha256   e4c674f60f9f854fbc4fabe893b8b503b3ec07876c4eb18042e8c975949c61ae
FD 大小     2097152 (0x200000)
FD 基址     0xB0000000
目标        DEBUG, CLANGPDB, AARCH64
```

那次构建时的源码状态，按当时的记录：

```text
Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/BootManager.c
  368d605005c27d5b27173bc41ea321fef4f01d4c2b1c5de70d27d1d878091df1
Platforms/Xiaomi/evergoPkg/evergo.dsc
  cb9194afd3522b12de301f1ad2636b14391afbc21d4d5919aef8035a3cc4228d
Platforms/Xiaomi/evergoPkg/Include/DXE.inc
  5e8b516fd5bd8d2b0043035d2a5f00e8767c77c0b099e87846d0d35f3d9df008
```

本仓库就是那份源码状态。此后开发的一切——独立的按键探测驱动、输入链信标、SoC
按键寄存器重写、信标页迁移、启动管理器跳过开关、UniXNU 菜单中的输入覆写——都是
实验性的，从未在真机上确认，且**不在**本代码树中。见下文「移除了什么」。

## 源码状态是如何恢复的

实验工作没有逐步提交，所以基线无法从 git 取得。它是被重建、再通过重新构建来证明的：

1. 设备自己的里程碑目录记录了产生可用 FD 那一刻上述三个文件的 SHA-256。那些哈希
   就是目标。
2. 从开发会话日志中，把那一刻之后的每一次编辑逐文件反向重放，直到每个重建文件都
   哈希到其记录值。
3. 然后把重建结果与归档的成功 FD 逐模块比对。

结果：**57 个固件卷模块中有 56 个与归档的成功构建逐字节一致。** 唯一的例外是
`PcdDxe`，它仅在其 8 字节 PDB 签名（`RSDS…`）上不同。重新构建未改动的源码会产生
同样的 8 字节差异，所以这是构建噪声，不是行为变化。

基线的模块清单（57 个模块）可用任意一次构建重新生成：

```bash
python3 - <<'PY'
import struct, uuid
from pathlib import Path
data = Path(".work/uefi/Build/evergoPkg/DEBUG_CLANGPDB/FV/FVMAIN.Fv").read_bytes()
base = data.find(b"_FVH") - 40
ext = struct.unpack_from("<H", data, base + 0x34)[0]
hdr = struct.unpack_from("<H", data, base + 0x30)[0]
off = base + max(hdr, ext + 0x14)
while data[off:off+4] == b"\xff"*4:
    off += 4
n = 0
while off + 24 <= len(data):
    name = data[off:off+16]
    if name == b"\xff"*16:
        break
    size = int.from_bytes(data[off+0x14:off+0x17], "little")
    if size == 0xFFFFFF:
        size = struct.unpack_from("<Q", data, off + 0x18)[0]
    print(f"{uuid.UUID(bytes_le=name)}  {size:#x}")
    off = (off + size + 7) & ~7
    n += 1
print(f"{n} modules")
PY
```

## 相对实验树移除了什么

以下每一项都是在已验证构建之后添加、且从未在真机上确认的。列出来是为了避免有人
误以为它们是可用版本的一部分而重新引入。

| 已移除 | 原因 |
| --- | --- |
| `KeypadInputTestDxe` 驱动及其 DSC/FDF 条目 | 一个独立的探测页；从未正确显示 |
| `BootManager.c` 中的 `EVERGO_SKIP_BOOTMGR_FOR_INPUT_TEST` 分支 | 同时绕过了菜单和 XNU 钩子 |
| BDS 钩子中的 TextIn 轮询循环 | 用测试屏替换了菜单 |
| KeypadDxe / KeypadDeviceDxe / GpioDxe / PmicWrapperDxe / MT6358PmicDxe 中的输入链信标 | 写入了一个会被 Android 内核覆盖的页 |
| 同一批驱动中的 TRC 派发探针 | 同上 |
| 信标页迁移 `0x4811FC00` → `0x48122000` | 新页在每次启动时都被清空 |
| `UniXNUPkg/.../Menu.c` 中的 `MenuTextIn()` 覆写 | 修改了随附的包；已验证构建运行的是上游代码 |
| `KeypadDeviceDxe` 的 depex 改为要求 GPIO+PMIC 协议 | 改变了相对已验证构建的派发顺序 |
| `KeypadDeviceLib.c` 中的 SoC 按键寄存器重写 | 从未产生按键事件 |
| `MemoryMapLib` 中新增的 `Keypad` 区域 | 改变了每个链接该库的模块的 `.rdata` 布局 |

## 复现比对

`scripts/compare-baseline.py` 把一次构建与基线 FVMAIN 比对，并对每个模块分类：

```bash
# 会替你解压 FD
scripts/compare-baseline.py --fd out/SILICIUM_UEFI.fd --baseline baseline-FVMAIN.Fv
```

| 类别 | 含义 |
| --- | --- |
| identical（一致） | `.text` 逐字节相同 |
| layout-only（仅布局） | 差异仅限于寻址重定位数据的 ADRP/ADD/MOV 立即数 |
| code（代码） | 指令字段的差异超出地址修正——是真实的行为变化 |

本项目上的实测：

| 构建 | 结果 |
| --- | --- |
| macOS，同路径长度，同 clang | 57 一致，0 仅布局，0 代码 |
| macOS，不同路径长度，同 clang | 46 一致，11 仅布局，0 代码 |
| Linux aarch64，发行版 clang 18.1.8 | 6 一致，0 仅布局，51 代码 |

**第三行不是回归。** 不同 clang 版本对同一份源码会产出不同指令。跨工具链比对无论
哪个方向都无法证明等价，所以工具把它报告为「代码不同」并附明确警告，而不是假装
它是故障。

这个工具真正擅长的是：改动某个源文件之后，把新构建与同一台机器、同一工具链下
同一代码树的一次构建相比。此时「代码」类发现意味着你改变了行为，「仅布局」类发现
意味着你只是改变了某个字符串落点。

机制：编译器把每个模块的 PDB 路径嵌入 `.rdata`，所以路径长度不同会改变该字符串的
大小并移动其后的一切，从而改变寻址它的立即数。代码相同；只有被嵌入字符串的地址
移动了。这就是为什么同长度构建逐字节一致、不同长度构建不一致——也解释了为什么
*代码*类发现才是要紧的那个。

## 保持基线可达

重建结果以带标签的分支形式保存在开发检出中，不在本仓库（本仓库*就是*那个状态）。
如果你 fork 并开始实验，在你收到的那个提交上保留一个标签，并在每次触及 SEC、BDS
或内存映射的改动之后重跑模块比对。
