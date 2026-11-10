# RUNBOOK —— evergo UEFI 真机移植流程

中文 | [**English**](RUNBOOK.md)

把本仓库带到一台真实的 Redmi Note 11 5G 上、并观察固件行为的逐步流程。
每一步都说明它应当产生什么，以及何时应当停止。

请先读 [`BASELINE.md`](BASELINE.md)：它定义了「可用」的含义，这样你就不会去追一个
本项目从未到达过的阶段。

---

## 0. 前置条件

| 项目 | 检查 | 失败时 |
| --- | --- | --- |
| 设备是 `evergo` | `adb shell getprop ro.product.device` → `evergo` | 停止，设备不对 |
| Bootloader 已解锁 | `fastboot oem device-info` | 热跳路径需要它；停止 |
| Root shell | `adb shell su -c id` → `uid=0` | 停止 |
| Stage1 模块已构建并推送 | `/data/adb/evergo_stage1.ko` | 见 §1 与 `stage1/README.md` |
| 本机上有原厂 `boot_b` 镜像 | `ls stock-boot_b.img` | 仅在你打算刷写时需要 |

不要在一台 `ro.product.device` 不是 `evergo` 的设备上继续。内存映射与预留窗口都是
本机型专属的。

---

## 1. stage1 模块

模块源码在 [`../stage1/`](../stage1/README.md)。它执行热跳：把 2 MiB 的 FD 拷到
`0xB0000000`，校验拷贝，清理缓存，关闭 MMU 并跳入 FD。preloader 与 LK 被完全跳过，
DRAM 保留其内容，这就是为什么失败后靠断电重启即可恢复。

它接受的是**原始 FD**，不是 boot image：boot image 只用于 `--flash` 路径，那里由
LK 负责解包。

用 `stage1/Makefile` 针对设备内核树构建。编译好的 `.ko` 不随附，因为它必须与正在
运行的内核完全匹配（版本、LTO/CFI、符号地址）。

如果你无法构建它，替代方案是 `deploy.sh --flash`，它会替换 `boot_b`，具有破坏性。
选择之前先看 §6。

---

## 2. 构建

```bash
scripts/fetch-deps.sh
scripts/build.sh --tree .work/uefi
scripts/build-shim.sh --tree .work/uefi
scripts/make-boot-image.sh \
  --fd   out/SILICIUM_UEFI.fd \
  --shim out/BootShim.bin \
  --dtb  .work/uefi/Resources/DTBs/evergo.dtb \
  --out  out/evergo-boot.img
```

预期：`out/SILICIUM_UEFI.fd` 正好 2097152 字节；`make-boot-image.sh` 打印一次
往返校验并以 0 退出。

若 FD 大小不符就停止——那说明热跳 `-D` 参数丢了，镜像被链接到了冷启动地址。

---

## 3. 改动任何东西之前的基线检查

把未修改的镜像跑一次。这确立你的构建环境与设备能复现已知状态，从而让后续失败
可归因。

```bash
scripts/deploy.sh --fd out/SILICIUM_UEFI.fd --serial <serial> --yes
```

观察屏幕。预期顺序：

1. 全屏彩条（SEC 已接管帧缓冲），
2. UniXNU 菜单，倒计时在走，
3. 要么菜单等待，要么设备复位回 Android。

然后读取信标：

```bash
scripts/deploy.sh --fd out/SILICIUM_UEFI.fd --serial <serial> --yes --read-back
```

预期：信标 magic 存在，阶段 `0xA2` 或 `0xA3`。

**如果你没看到菜单，先停下来把它修好。** 下游一切都以这一步成立为前提。

---

## 4. 观察单次改动

每次运行只改一处。运行前写下：

* 你改了什么，
* 哪个阶段应当变化，
* 哪个观测结果会证伪你的假设，
* 你打算等多久才宣布本次运行作废。

运行后记录：

* 最后的信标阶段（数字与含义），
* magic 是否有效（作废的页什么都证明不了），
* 屏幕序列是否符合预测。

---

## 5. 不重新部署也能读信标

如果你只需要当前的信标页（例如手动复位之后）：

```bash
adb shell "su -c 'dd if=/dev/mem bs=256 skip=$((0x4811fc00/256)) count=1 2>/dev/null'" > beacon.bin
adb shell "su -c 'dd if=/dev/mem bs=2048 skip=$((0x4811f800/2048)) count=1 2>/dev/null'" > trace.bin
scripts/decode-beacons.py --beacon beacon.bin --trace trace.bin
```

这只读内存。它不加载模块，也不复位任何东西。

---

## 6. 恢复

### 默认（热跳）路径

存储上什么都没变。给手机断电重启；Android 正常启动。

### 若屏幕一直黑、手机唤不醒

长按电源约 15 秒强制复位。热跳只改动 DRAM，所以冷复位会回到 Android。

### 若你刷了 `boot_b` 且失败

```bash
fastboot flash boot_b stock-boot_b.img
fastboot reboot
```

你需要你确切机型与版本的原厂镜像。不要用别的机型的 `boot_b`；DTB 与加载地址都
不同。

### 若手机进不了 fastboot

联发科设备通过 USB 暴露 BootROM/preloader 模式。从那里恢复需要厂商工具，超出本文
范围；这正是 `--flash` 被显式开关和警告拦住的原因。

---

## 7. 实际验证过什么

摘自 [`BASELINE.md`](BASELINE.md)，以便本文件可单独阅读：

| 观测 | 状态 |
| --- | --- |
| 热跳到达 UEFI | 已验证 |
| SEC/DXE/BDS 进度信标 | 已验证 |
| 面板上的彩条测试图案 | 已验证 |
| UniXNU 菜单带倒计时渲染 | 已验证（视频） |
| 菜单响应按键 | **未验证** —— 菜单报告 `No input device` |
| 菜单中存在启动项 | **未验证** —— `no boot entry found` |
| XNU 内核入口 | **未验证** |
| `Darwin Kernel Version` 输出 | **从未观测** |

若某次运行看起来与本表矛盾，先检查测量，再更新本表。这个错误在本项目上被反复
犯过。
