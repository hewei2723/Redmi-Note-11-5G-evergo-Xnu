# `evergo_stage1` —— 热跳内核模块

中文 | [**English**](README.md)

这是启动链的设备侧那一半。顶层仓库里的一切都运行在你的构建机上；这个模块运行在
手机上、Android 内部、以 root 身份，把控制权交给 UEFI 固件。

之所以包含它，是因为没有它，固件就只能靠刷写分区来启动，「我到底该怎么运行这个」
将无从回答。

## 它做什么

Android 正常运行时，模块：

1. 从文件系统打开固件 FD，
2. 把它拷到设备自身设备树预留的一个物理地址（`0xB0000000`），并逐字节校验拷贝，
3. 可选地把一个 `XNUP` payload 拷到 payload 窗口（`0xB5400000`），
4. 把设备树指针写到固件期望的位置，
5. 停止其他 CPU，关闭 MMU，跳入 FD。

SoC 从不复位：preloader 与 LK 被完全跳过，DRAM 保留其内容。这就是为什么信标页能
存活，以及为什么失败后靠断电重启而非重新刷写来恢复。

## 为什么放在 `stage1/` 而不作为二进制随附

本仓库早期草稿把这个模块描述为「设备侧工具，不包含」。那是错的——这 56 KB 源码
是启动链中用户无法自行重建的唯一部分。编译好的 `.ko` 是 314 KB，*不*提交，因为它
必须匹配你正在运行的内核：同内核版本、同 LTO/CFI 配置、同符号地址。

## 构建它

模块必须针对本设备准备好的 Android 5.10 内核树构建。仅有内核源码**不**够；代码树
必须已配置、并构建到 `Module.symvers` 已存在。

```bash
# 1. 内核源码。小米已发布；此处使用的树是：
#    https://github.com/Xiaomi-MT6833/kernel_xiaomi_evergo
export KDIR=~/kernel_xiaomi_evergo

# 2. 准备它。厂商 defconfig 名称取决于你的源码版本；
#    此处使用的树以 evergo defconfig 并开启 LTO+CFI 构建。
make -C "$KDIR" ARCH=arm64 O="$KDIR/out" evergo_defconfig
make -C "$KDIR" ARCH=arm64 O="$KDIR/out" CC=clang LD=ld.lld \
     CONFIG_LTO_CLANG=y CONFIG_CFI_CLANG=y modules_prepare

# 3. 构建模块。
cd stage1
make KDIR="$KDIR/out" \
     CLANG_DIR=~/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin
```

输出：`stage1/` 下的 `evergo_stage1.ko`。

defconfig 名称与确切内核版本，是你的源码版本上最可能不同的两样东西。在
`arch/arm64/configs/` 中查找 `evergo*` 文件，并把手机上的 `uname -r` 与你构建所
依据的树对照。

### LTO 与 CFI 不是可选项

厂商内核会对 `mod->init` 间接调用做 CFI 检查。未开启
`CONFIG_LTO_CLANG=y CONFIG_CFI_CLANG=y` 构建的模块，要么加载失败，要么崩溃。
这是自建模块不工作的最常见原因，而且它不会产生明显的错误信息。

## 加载它

模块接受很多参数。mode 5（热跳）需要的那些是从运行中的内核读取的符号地址，这就是
为什么调用方式是一段 shell 脚本，而不是一行你能记住的命令：

```bash
su -c '
  S=$(awk "\$3==\"smp_send_stop\"{print \$1}" /proc/kallsyms)
  F=$(awk "\$3==\"filp_open\"{print \$1}" /proc/kallsyms)
  R=$(awk "\$3==\"kernel_read\"{print \$1}" /proc/kallsyms)
  FA=$(awk "\$3==\"__flush_dcache_area\"{print \$1}" /proc/kallsyms)
  taskset 1 insmod /data/adb/evergo_stage1.ko mode=5 stop=1 \
    fd_path=/data/adb/evergo-hotjump.fd \
    p_fd_base=0xb0000000 p_jump_pa=0xb0000000 \
    p_smp_send_stop=0x$S p_filp_open=0x$F p_kernel_read=0x$R \
    p_flush_dcache_area=0x$FA p_flush_pa=0x0 p_flush_len=0x0
'
```

`fd_path` 是 `scripts/build.sh` 产出的**2 MiB FD**，不是 `scripts/make-boot-image.sh`
产出的 boot image。模块直接拷贝 FD；boot image 包装器是为 `--flash` 路径存在的，
那里由 LK 负责解包。

`taskset 1` 把模块钉在 CPU 0 上，与 trampoline 中的假设一致。`stop=1` 在跳转前
停住其他核。

## 看起来像别的问题的故障模式

| 症状 | 原因 |
| --- | --- |
| `insmod` 失败，报 `Invalid argument` / 无信息 | 未开启 LTO+CFI 构建 |
| `stage1: cannot open /data/adb/...` | 路径错误，或 SELinux 拦截；通过 `su -mm` 运行 |
| `stage1: FD has no _FVH at 0x28` | 你传了 boot image 而不是原始 FD |
| 跳转后黑屏、无彩条 | FD 已拷贝但入口点崩溃；怀疑 `p_jump_pa` 错误，或 FD 链接到了不同基址 |
| 跳转后立即返回 Android | `p_jump_pa`/`p_fd_base` 与 FD 被拷贝到的位置不符 |

## 安全性

本模块只写入预留 DRAM。它不修改任何分区，冷复位会恢复 Android。这是这条特定代码
路径的属性，不是普遍保证：模块还包含诊断模式（1、3、9），它们会故意复位 SoC 或
停住 CPU，且 mode 3 永不返回。

不要启用 Makefile 中被注释掉的 `uefi_jump.o`。它的 init 会写
`0x98000000`/`0x9C000000`，那是调制解调器预留区，曾在 2026-11-04 引发 EMI 违规
风暴与 panic。

## 设计说明

源码头部引用了 `PHASE1-SAFE-TEST-DESIGN.md`，那是一份未在此发布的内部设计文档：
它是（中文的）诊断模式工作工程日志，带逐条观测的证据标签，并描述了从未在真机上
执行过的测试流程。构建或使用本模块并不需要它。

它所确立、且在此要紧的，是代码在热跳路径上遵循的安全规则：除设备树已预留的两个
地址（FD 用 `0xB0000000`，payload 用 `0xB5400000`）外，不写任何固定 DRAM 物理
地址；且 `smp_send_stop()` 之后只运行内联汇编。

## 许可

`GPL-2.0`，内核模块接口所要求。仓库其余部分的许可见 `../LICENSE`；二者不冲突，
因为本模块是链接内核而非固件的独立作品。
