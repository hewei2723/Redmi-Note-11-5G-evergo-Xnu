/*
 * evergo_stage1.c - evergo (MT6833P) Phase-1 safe low-level handoff tests.
 *
 * Design & acceptance criteria: PHASE1-SAFE-TEST-DESIGN.md (same directory).
 *
 * Safety invariants:
 *   - No fixed DRAM physical address constants anywhere. The only memory we
 *     write is pages the kernel allocator gave us. No FD copy, no payload, and
 *     never 0x98000000 / 0x9C000000 / 0xA4000000 / 0xB0000000.
 *   - After smp_send_stop() only inline asm runs: no printk, no ioremap,
 *     no memremap, no VFS, no allocator, no locks.
 *   - The only non-DRAM MMIO touched is the toprgu watchdog at 0x10007000
 *     (DT node toprgu@10007000; WDT_SWRST=0x14 key=0x1209, cf.
 *     drivers/watchdog/mtk_wdt.c), and only when wdt_reset=1.
 *
 * Parameters (see PHASE1-SAFE-TEST-DESIGN.md for the test matrix):
 *   mode=1  Test A: optional smp_send_stop, marker, then WDT reset or wfi.
 *   mode=2  Test B: module-owned identity map round trip (MMU stays on).
 *   mode=3  Test C: MMU-off trampoline. Requires confirm=1. Never returns.
 *   mode=4  Read-only probe of a marker page PA (warm-reset DRAM test).
 *   mode=8  Test-D beacon/jump-block mapping probe; no stop, MMU-off, or jump.
 *   stop=1              call smp_send_stop() before the marker (A and C)
 *   wdt_reset=1         trigger toprgu SW reset instead of wfi loop
 *   confirm=1           required for mode=3
 *   p_smp_send_stop=0x  smp_send_stop text address, from /proc/kallsyms
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <asm/page.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>
#include <asm/sysreg.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/iommu.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("evergo hot-jump project");
MODULE_DESCRIPTION("evergo Phase-1 safe low-level tests (no fixed phys addr)");
MODULE_VERSION("0.1");

static int mode;
static int stop;
static int wdt_reset;
static int confirm;
static int p_marker_count;
static unsigned long p_smp_send_stop;
static unsigned long p_flush_dcache_area;
static unsigned long p_flush_pa;
static unsigned long p_flush_len;
static unsigned long p_marker_pa;
static unsigned long p_wval;   /* mode=9: 64-bit witness value */
static unsigned long p_kernel_write;
static char *fd_path;
static unsigned long p_filp_open;
static unsigned long p_kernel_read;
static unsigned long p_fd_base;
static unsigned long p_jump_pa;
static unsigned long p_dtb_pa;
static char *p_payload_path;
static unsigned long p_payload_base;
static unsigned long p_payload_limit;

module_param(mode, int, 0444);
module_param(stop, int, 0444);
module_param(wdt_reset, int, 0444);
module_param(confirm, int, 0444);
module_param(p_smp_send_stop, ulong, 0444);
module_param(p_flush_dcache_area, ulong, 0444);
module_param(p_flush_pa, ulong, 0444);
module_param(p_flush_len, ulong, 0444);
module_param(p_marker_pa, ulong, 0444);
module_param(p_marker_count, int, 0444);
module_param(fd_path, charp, 0444);
module_param(p_filp_open, ulong, 0444);
module_param(p_kernel_read, ulong, 0444);
module_param(p_fd_base, ulong, 0444);
module_param(p_jump_pa, ulong, 0444);
module_param(p_dtb_pa, ulong, 0444);
module_param(p_kernel_write, ulong, 0444);
module_param(p_wval, ulong, 0444);   /* mode=9: 64-bit witness value to write */
module_param(p_payload_path, charp, 0444);
module_param(p_payload_base, ulong, 0444);
module_param(p_payload_limit, ulong, 0444);
MODULE_PARM_DESC(mode, "1=Test A (stop+marker+reset), 2=Test B (idmap round trip), 3=Test C (MMU-off trampoline)");
MODULE_PARM_DESC(stop, "1: call smp_send_stop() before the marker (needs p_smp_send_stop)");
MODULE_PARM_DESC(wdt_reset, "1: trigger toprgu WDT SW reset (0x10007014) instead of wfi loop");
MODULE_PARM_DESC(confirm, "1: required to run mode=3");
MODULE_PARM_DESC(p_smp_send_stop, "smp_send_stop() address from /proc/kallsyms");
MODULE_PARM_DESC(p_flush_dcache_area, "__flush_dcache_area(kaddr,size) address from /proc/kallsyms; dc civac - clean+invalidate, required for stop=1");
MODULE_PARM_DESC(p_flush_pa, "physical base of the region whose stale dirty cache lines must be dropped (DXE Heap head, UEFI HOB area)");
MODULE_PARM_DESC(p_flush_len, "length of the flush region (bytes)");
MODULE_PARM_DESC(p_payload_path, "mode=5: XNUP container to place in the reserved payload window");
MODULE_PARM_DESC(p_payload_base, "mode=5: physical address of the payload window (0xB5400000)");
MODULE_PARM_DESC(p_payload_limit, "mode=5: maximum accepted container size (default 0x3000000)");
MODULE_PARM_DESC(p_marker_pa, "marker page block physical address to probe (mode=4, read-only)");
MODULE_PARM_DESC(p_marker_count, "number of marker pages to scan in mode=4 (default 16)");
MODULE_PARM_DESC(fd_path, "mode=5: path of the 2 MiB UEFI FD file to copy and jump into");
MODULE_PARM_DESC(p_filp_open, "mode=5: filp_open address from /proc/kallsyms");
MODULE_PARM_DESC(p_kernel_read, "mode=5: kernel_read address from /proc/kallsyms");
MODULE_PARM_DESC(p_fd_base, "mode=5: reserved physical base for the FD (default 0xB0000000)");
MODULE_PARM_DESC(p_jump_pa, "mode=5: physical entry to branch to after MMU off (FD base)");
MODULE_PARM_DESC(p_dtb_pa, "mode=5: physical address of the device DTB (x0 at entry)");
MODULE_PARM_DESC(p_kernel_write, "mode=7: kernel_write address from /proc/kallsyms");

#define TOPRGU_BASE		0x10007000UL
#define TOPRGU_SWRST_KEY	0x1209U

/* Test D handoff constants (mirror BootShim.S cold-boot contract) */
#define FD_SIZE_BYTES		0x200000UL
#define FD_FVH_OFFSET		0x28UL
#define DTB_PTR_SLOT_DEFAULT	0xA40007F8UL
#define FD_SLOT_BYTES		0x400000UL	/* whole DTB-reserved slot */
#define TESTD_BEACON_BASE	0x4811FC00UL /* C, asm, stub, jump block */
#define TESTD_BEACON_BYTES	0x400UL
#define PAYLOAD_SLOT		0x4811FF00UL	/* jump block: jump_pa@+0, dtb_pa@+8 */
#define SEC_BEACON_AREA		0x100UL

/* marker page layout (kernel-allocated, physical address recorded) */
#define MARKER_MAGIC		0x4556415354315F4DULL	/* "M_1TS AVE" LE tag */
#define MARKER_MAGIC_PA		0x08
#define MARKER_RESULT		0x10
#define MARKER_PRESTOP		0x18
#define TESTA_MAGIC2		0x0000000041325453ULL	/* "STA2" */
#define TESTB_MAGIC		0x0000000042325453ULL	/* "STB2" */
#define TESTC_MAGIC		0x0000000043325453ULL	/* "STC2" */

static unsigned long s_marker_va;	/* linear map address of marker page */
static unsigned long s_marker_pa;
static unsigned long s_pgd_va, s_pgd_pa, s_pmd_va, s_pmd_pa;
static unsigned long s_stub_pa;
static void __iomem *s_pb_map;

typedef void (*smp_send_stop_t)(void);

static int publish_fb_handoff(void);   /* defined below, called from mode=5/11 */

/* Indirect calls through inline asm: the vendor kernel's CFI rejects
 * plain C function pointers of mismatched type ids (proven on device). */
static __attribute__((__noinline__)) void call0(void *fn)
{
	asm volatile("blr %0" : : "r"(fn) : "x30", "memory");
}

static __attribute__((__noinline__)) unsigned long call3(void *fn, unsigned long a0,
					     unsigned long a1, unsigned long a2)
{
	register unsigned long x0 asm("x0") = a0;
	register unsigned long x1 asm("x1") = a1;
	register unsigned long x2 asm("x2") = a2;
	asm volatile("blr %3" : "+r"(x0) : "r"(x1), "r"(x2), "r"(fn)
		     : "x30", "memory");
	return x0;
}

static __attribute__((__noinline__)) unsigned long call4(void *fn, unsigned long a0,
					     unsigned long a1, unsigned long a2,
					     unsigned long a3)
{
	register unsigned long x0 asm("x0") = a0;
	register unsigned long x1 asm("x1") = a1;
	register unsigned long x2 asm("x2") = a2;
	register unsigned long x3 asm("x3") = a3;
	asm volatile("blr %4" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(fn)
		     : "x30", "memory");
	return x0;
}

/* Clean D-cache by VA for [va, va+len): makes writes visible in DRAM before
 * the MMU is turned off (post-jump fetches bypass the cache). */
static void clean_dcache_range(unsigned long va, unsigned long len)
{
	unsigned long p;

	for (p = va & ~(64UL - 1); p < va + len; p += 64)
		asm volatile("dc cvac, %0" : : "r"(p) : "memory");
	asm volatile("dsb sy" : : : "memory");
}

/* ------------------------------------------------------------------ */
/* Low-level stubs, entered at their identity physical address.        */
/* ------------------------------------------------------------------ */

/*
 * Test B stub: x0 = identity PA of the marker result slot, x1 = magic.
 * Runs with TTBR0 = our idmap, MMU on, TTBR1 untouched, DAIF masked.
 * Hardware PAN (CONFIG_ARM64_PAN=y) blocks EL1 data access to the TTBR0
 * region, so clear PSTATE.PAN for the store and restore it afterwards.
 */
asm(
".pushsection .text, \"ax\"\n"
".align 4\n"
".global evergo_stage1_stub_b\n"
".hidden evergo_stage1_stub_b\n"
"evergo_stage1_stub_b:\n"
"	.word	0xd5384269	/* mrs x9, pan (ARMv8.1; assembler arch-gated) */\n"
"	.word	0xd500409f	/* msr pan, #0 */\n"
"	str	x1, [x0]\n"
"	dsb	sy\n"
"	.word	0xd5184269	/* msr pan, x9 */\n"
"	ret\n"
"\n"
/*
 * Test C/D stub: x0 = marker PA, x1 = magic, x2 = wdt_reset flag,
 * x3 = SCTLR_ELx_FLAGS, x4 = jump target PA (0 = no jump), x7 = value
 * for x0 at the jump target (DTB PA for the UEFI FD contract). Entered
 * identity-mapped, MMU on, DAIF masked. Clears M/A/C/SA/I exactly like
 * arm64 cpu-reset.S, then everything is physical: marker store, optional
 * toprgu SW reset, wfi loop. No stack, no return path.
 */
".global evergo_stage1_stub_c\n"
".hidden evergo_stage1_stub_c\n"
"evergo_stage1_stub_c:\n"
"	/* v3: zero register dependencies. The jump block lives at the\n"
"	 * DTB-reserved physical constant 0xB5400100 (written by the module\n"
"	 * through memremap before the jump). */\n"
"	movz	x9, #0x4811, lsl #16\n"
"	movk	x9, #0xFE00		/* stub beacons = 0x4811FE00 */\n"
"	mrs	x10, sctlr_el1\n"
"	movz	x11, #0x100f	/* SCTLR_ELx_FLAGS (static-asserted) */\n"
"	bic	x10, x10, x11\n"
"	dsb	sy\n"
"	msr	sctlr_el1, x10\n"
"	isb\n"
"	/* MMU off from here on: physical addresses. */\n"
"	movz	w11, #0x0001\n"
"	str	w11, [x9, #0x20]	/* step 1: stub entered */\n"
"	dsb	sy\n"
"	ldr	x4, [x9, #0x100]	/* jump_pa */\n"
"	movz	x13, #0xB000, lsl #16	/* expected 0xB0000000 */\n"
"	cmp	x4, x13\n"
"	b.eq	2f\n"
"	movz	w11, #0x0002\n"
"	str	w11, [x9, #0x24]	/* step 2: jump_pa mismatch */\n"
"	dsb	sy\n"
"	movz	w10, #0x1209\n"
"	movz	x11, #0x7014\n"
"	movk	x11, #0x1000, lsl #16\n"
"	str	w10, [x11]		/* toprgu WDT SW reset */\n"
"	dsb	sy\n"
"3:	wfi\n"
"	b	3b\n"
"2:	movz	w11, #0x0003\n"
"	str	w11, [x9, #0x28]	/* step 3: jumping into FD */\n"
"	/* Record the first two FD words as read from DRAM with the MMU off.\n"
"	 * 0x14000400 means the FD bytes are really there at jump time; 0 or\n"
"	 * 0xffffffff means the copy never reached this physical address. */\n"
"	ldr	w12, [x4]\n"
"	str	w12, [x9, #0x30]\n"
"	ldr	w13, [x4, #4]\n"
"	str	w13, [x9, #0x34]\n"
"	/* Container witness: read the XNUP magic at 0xB5400000 with the MMU\n"
"	 * OFF, at jump time. 0x50554E58 = payload reached DRAM; 0 = the copy\n"
"	 * never landed (isolates module-write vs UEFI-read in one run). */\n"
"	movz	x14, #0xB540, lsl #16\n"
"	ldr	w15, [x14]\n"
"	str	w15, [x9, #0x40]\n"
"	ldr	w15, [x14, #4]\n"
"	str	w15, [x9, #0x44]\n"
"	/* Three-address witness READS at jump time (MMU off). The module no\n"
"	 * longer writes W1/W3 magics (they corrupted the FD FVMAIN); keep the\n"
"	 * reads only for the container (0x40) which stays meaningful. */\n"
"	movz	x14, #0xB000, lsl #16\n"
"	movk	x14, #0x3000\n"
"	ldr	w15, [x14]\n"
"	str	w15, [x9, #0x50]\n"
"	movz	x14, #0xB540, lsl #16\n"
"	movk	x14, #0x8000\n"
"	ldr	w15, [x14]\n"
"	str	w15, [x9, #0x58]\n"
"	movz	x14, #0x5110, lsl #16\n"
"	ldr	w15, [x14]\n"
"	str	w15, [x9, #0x60]\n"
"	dsb	sy\n"
"	ldr	x0, [x9, #0x108]	/* x0 at FD entry = dtb_pa */\n"
"	br	x4\n"
".popsection\n"
);

extern char evergo_stage1_stub_b[];
extern char evergo_stage1_stub_c[];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static unsigned long vmalloc_text_pa(const void *addr)
{
	struct page *pg = vmalloc_to_page(addr);

	if (!pg)
		return 0;
	return page_to_phys(pg) + ((unsigned long)addr & ~PAGE_MASK);
}

#define MARKER_ORDER	4	/* 16 pages / 64 KiB, one allocation */
#define MARKER_PAGES	(1 << MARKER_ORDER)

static int alloc_marker(void)
{
	unsigned long va;
	int i;

	va = __get_free_pages(GFP_KERNEL | __GFP_ZERO, MARKER_ORDER);
	if (!va)
		return -ENOMEM;
	s_marker_va = va;
	s_marker_pa = page_to_phys(virt_to_page((const void *)va));
	for (i = 0; i < MARKER_PAGES; i++) {
		unsigned long p = s_marker_va + (i << PAGE_SHIFT);
		*((volatile unsigned long *)p) = MARKER_MAGIC;
		*((volatile unsigned long *)(p + 8)) =
			s_marker_pa + (i << PAGE_SHIFT);
		*((volatile unsigned long *)(p + MARKER_RESULT)) = 0;
		*((volatile unsigned long *)(p + MARKER_PRESTOP)) = 0;
	}
	return 0;
}

static void free_marker(void)
{
	if (s_marker_va) {
		free_pages(s_marker_va, MARKER_ORDER);
		s_marker_va = 0;
		s_marker_pa = 0;
	}
}

/*
 * Build a minimal 3-level identity map (PGD -> PMD 2 MiB blocks) covering
 * the regions that contain pa1 and pa2. Block attributes are the kernel's
 * early-idmap attributes (head.S): PMD_ATTRINDX(MT_NORMAL) | PMD_TYPE_SECT
 * | PMD_SECT_AF | PMD_SECT_S. No hand-written attribute constants.
 */
static int build_idmap(unsigned long pa1, unsigned long pa2)
{
	unsigned long block_attr;
	u64 *pgd, *pmd;

	s_pgd_va = __get_free_pages(GFP_KERNEL | __GFP_ZERO, 0);
	s_pmd_va = __get_free_pages(GFP_KERNEL | __GFP_ZERO, 0);
	if (!s_pgd_va || !s_pmd_va) {
		if (s_pgd_va)
			free_pages(s_pgd_va, 0);
		if (s_pmd_va)
			free_pages(s_pmd_va, 0);
		s_pgd_va = s_pmd_va = 0;
		return -ENOMEM;
	}
	s_pgd_pa = page_to_phys(virt_to_page((const void *)s_pgd_va));
	s_pmd_pa = page_to_phys(virt_to_page((const void *)s_pmd_va));

	pgd = (u64 *)s_pgd_va;
	pmd = (u64 *)s_pmd_va;
	pgd[(pa1 >> 30) & 0x1ff] = s_pmd_pa | PMD_TYPE_TABLE;
	pgd[(pa2 >> 30) & 0x1ff] = s_pmd_pa | PMD_TYPE_TABLE;

	block_attr = PMD_ATTRINDX(MT_NORMAL) | PMD_TYPE_SECT |
		     PMD_SECT_AF | PMD_SECT_S;
	pmd[(pa1 >> 21) & 0x1ff] = (pa1 & ~(SZ_2M - 1)) | block_attr;
	pmd[(pa2 >> 21) & 0x1ff] = (pa2 & ~(SZ_2M - 1)) | block_attr;

	return 0;
}

static void free_idmap(void)
{
	if (s_pgd_va) {
		free_pages(s_pgd_va, 0);
		s_pgd_va = s_pgd_pa = 0;
	}
	if (s_pmd_va) {
		free_pages(s_pmd_va, 0);
		s_pmd_va = s_pmd_pa = 0;
	}
}

static void report_sysregs(void)
{
	unsigned long el, sctlr, tcr, ttbr0, cntfrq;

	asm volatile("mrs %0, CurrentEL" : "=r"(el));
	asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
	asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
	asm volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));

	pr_err("stage1: CurrentEL=%ld SCTLR=0x%lx TCR=0x%lx TTBR0=0x%lx CNTFRQ=0x%lx\n",
		el >> 2, sctlr, tcr, ttbr0, cntfrq);
	pr_err("stage1: SCTLR_ELx_FLAGS=0x%lx (kernel macro)\n",
		(unsigned long)SCTLR_ELx_FLAGS);
}

/* ------------------------------------------------------------------ */
/* Test bodies                                                         */
/* ------------------------------------------------------------------ */

/*
 * Test A tail. Runs after smp_send_stop() (optional). Inline asm only:
 * mask DAIF, write the result marker and clean it to DRAM, then either
 * trigger the toprgu SW reset or spin in wfi until the user hard-resets.
 * Never returns.
 */
static __attribute__((__noinline__, __noreturn__)) void test_a_tail(unsigned long result_va,
							unsigned long magic,
							unsigned long wdt_va,
							unsigned long do_wdt)
{
	register unsigned long r0 asm("x0") = result_va;
	register unsigned long r1 asm("x1") = magic;
	register unsigned long r2 asm("x2") = wdt_va;
	register unsigned long r3 asm("x3") = do_wdt;

	asm volatile(
		"msr	daifset, #0xf\n"
		"str	%1, [%0]\n"
		"dc	cvac, %0\n"
		"dsb	sy\n"
		"cbz	%3, 1f\n"
		"movz	w9, #0x1209\n"
		"str	w9, [%2]\n"
		"dsb	sy\n"
		"1:	wfi\n"
		"	b	1b\n"
		:
		: "r"(r0), "r"(r1), "r"(r2), "r"(r3)
		: "x9", "x30", "memory");
	while (1)
		cpu_relax();
}

static int run_test_a(void)
{
	unsigned long wdt_va = 0, do_wdt = 0;

	if (wdt_reset) {
		void __iomem *m = ioremap(TOPRGU_BASE, 0x1000);

		if (!m) {
			pr_err("stage1: cannot ioremap toprgu, will wfi instead\n");
		} else {
			pr_err("stage1: toprgu mapped, WDT_MODE=0x%08x\n",
				readl(m));
			wdt_va = (unsigned long)m;
			do_wdt = 1;
		}
	}

	if (stop) {
		if (!p_smp_send_stop) {
			pr_err("stage1: stop=1 requires p_smp_send_stop\n");
			if (wdt_va)
				iounmap((void __iomem *)wdt_va);
			return -EINVAL;
		}
		*((volatile unsigned long *)(s_marker_va + MARKER_PRESTOP)) = 1;
		/* last printk before the point of no return */
		pr_err("stage1: A-GO marker pa=0x%lx smp_send_stop@0x%lx wdt=%lu\n",
			s_marker_pa, p_smp_send_stop, do_wdt);
		msleep(300);	/* let the ramoops console settle */
		call0((void *)p_smp_send_stop);
		/* no printk here: only asm until reset (invariant #3) */
	} else {
		pr_err("stage1: A-GO (no smp_send_stop) marker pa=0x%lx wdt=%lu\n",
			s_marker_pa, do_wdt);
		msleep(300);
	}

	test_a_tail(s_marker_va + MARKER_RESULT, TESTA_MAGIC2, wdt_va, do_wdt);
}

/*
 * Test B: switch TTBR0 to our own PGD, call the stub at its identity
 * physical address (TTBR1 stays live so the kernel-side VA path keeps
 * working), then restore TTBR0. All inside one asm block with DAIF
 * masked. Returns 0; the marker tells whether the identity data path
 * worked. PAN is untouched here (the stub saves/clears/restores it).
 */
static __attribute__((__noinline__)) void test_b_run(unsigned long pgd_pa,
						     unsigned long stub_pa,
						     unsigned long result_pa,
						     unsigned long magic)
{
	register unsigned long s_daif asm("x19");
	register unsigned long s_ttbr0 asm("x20");
	register unsigned long r0 asm("x0") = result_pa;
	register unsigned long r1 asm("x1") = magic;
	register unsigned long fn asm("x5") = stub_pa;
	register unsigned long pg asm("x6") = pgd_pa;

	asm volatile(
		"mrs	%0, daif\n"
		"msr	daifset, #0xf\n"
		"mrs	%1, ttbr0_el1\n"
		"msr	ttbr0_el1, %5\n"
		"	isb\n"
		"	tlbi	vmalle1\n"
		"	dsb	sy\n"
		"	isb\n"
		"	blr	%4\n"
		"msr	ttbr0_el1, %1\n"
		"	isb\n"
		"	tlbi	vmalle1\n"
		"	dsb	sy\n"
		"	isb\n"
		"msr	daif, %0\n"
		: "=r"(s_daif), "=r"(s_ttbr0), "+r"(r0), "+r"(r1),
		  "+r"(fn), "+r"(pg)
		:
		: "x9", "x30", "memory");
}

static int run_test_b(void)
{
	unsigned long stub_pa, result_before;
	int ret;

	stub_pa = vmalloc_text_pa((const void *)evergo_stage1_stub_b);
	if (!stub_pa) {
		pr_err("stage1: cannot resolve stub B physical address\n");
		return -ENOENT;
	}
	ret = build_idmap(stub_pa, s_marker_pa);
	if (ret)
		return ret;

	pr_err("stage1: B-ARMED pgd=0x%lx pmd=0x%lx stub=0x%lx marker=0x%lx attr=0x%lx\n",
		s_pgd_pa, s_pmd_pa, stub_pa, s_marker_pa,
		(unsigned long)(PMD_ATTRINDX(MT_NORMAL) | PMD_TYPE_SECT |
				PMD_SECT_AF | PMD_SECT_S));
	msleep(200);

	result_before = *((volatile unsigned long *)(s_marker_va + MARKER_RESULT));
	test_b_run(s_pgd_pa, stub_pa, s_marker_pa + MARKER_RESULT, TESTB_MAGIC);

	if (*((volatile unsigned long *)(s_marker_va + MARKER_RESULT)) ==
	    TESTB_MAGIC && result_before != TESTB_MAGIC)
		pr_err("stage1: B-OK identity data path verified\n");
	else
		pr_err("stage1: B-FAIL result slot 0x%lx\n",
		       *((volatile unsigned long *)(s_marker_va + MARKER_RESULT)));

	free_idmap();
	return 0;
}

/*
 * Test C: same TTBR0 switch as Test B, but the stub clears SCTLR.M/A/C/SA/I
 * and continues physically. No return path. Requires confirm=1.
 */
static __attribute__((__noinline__, __noreturn__)) void test_c_run(void)
{
	register unsigned long r_pg asm("x0") = s_pgd_pa;
	register unsigned long r_stub asm("x1") = s_stub_pa;

	/* v3: the stub takes no register arguments; it reads the jump block
	 * from the reserved physical constant 0xB5400100. The pgd/stub pair
	 * is only used by this template via named placeholders. */
	asm volatile(
		"msr	daifset, #0xf\n"
		"mrs	x20, ttbr0_el1\n"
		"msr	ttbr0_el1, %0\n"
		"	isb\n"
		"	tlbi	vmalle1\n"
		"	dsb	sy\n"
		"	isb\n"
		"	blr	%1\n"
		: "+r"(r_pg), "+r"(r_stub)
		:
		: "x9", "x10", "x11", "x12", "x13", "x20", "x30", "memory");
	while (1)
		cpu_relax();
}
/* Write the stub jump block into the reserved payload slot:
 *   0xB5400100 = jump_pa, 0xB5400108 = dtb_pa. */
static int map_testd_beacons(void)
{
	void *b;

	if (s_pb_map)
		return 0;
	b = memremap(TESTD_BEACON_BASE, TESTD_BEACON_BYTES, MEMREMAP_WB);
	if (!b)
		return -EIO;
	s_pb_map = b;
	memset(b, 0, TESTD_BEACON_BYTES);
	clean_dcache_range((unsigned long)b, TESTD_BEACON_BYTES);
	return 0;
}

/* s_pb_map is the complete 0x4811FC00..0x4811FFFF page.  The FD-side
 * stub reads jump_pa at +0x300 and dtb_pa at +0x308. */
static int write_jump_block(unsigned long jump_pa, unsigned long dtb_pa)
{
	volatile unsigned long *b;

	if (!s_pb_map)
		return -EIO;
	b = (volatile unsigned long *)((char *)s_pb_map +
			(PAYLOAD_SLOT - TESTD_BEACON_BASE));
	b[0] = jump_pa;
	b[1] = dtb_pa;
	clean_dcache_range((unsigned long)b, 0x10);
	if (b[0] != jump_pa || b[1] != dtb_pa)
		return -EIO;
	return 0;
}

/*
 * Test C (mode=3): jump_pa = 0 -> the stub takes the mismatch path:
 * beacon step 2 + toprgu WDT reset (controlled recovery, beacon evidence).
 */
static int run_test_c(void)
{
	unsigned long stub_pa;
	int ret;

	if (!confirm) {
		pr_err("stage1: mode=3 (MMU-off trampoline) requires confirm=1\n");
		return -EINVAL;
	}
	/* Publish the framebuffer handoff as late as possible: the DPU is still
	 * scanning and Android still flips buffers, so the OVL address read here
	 * is the freshest one we can get before the CPUs stop. */
	if (publish_fb_handoff() == 0) {
		pr_err("stage1: D-FB handoff published (late)\n");
	} else {
		pr_err("stage1: D-FB handoff NOT published\n");
	}

	stub_pa = vmalloc_text_pa((const void *)evergo_stage1_stub_c);
	if (!stub_pa) {
		pr_err("stage1: cannot resolve stub C physical address\n");
		return -ENOENT;
	}
	ret = build_idmap(stub_pa, s_marker_pa);
	if (ret)
		return ret;

	ret = map_testd_beacons();
	if (ret)
		return ret;
	ret = write_jump_block(0, 0);
	if (ret)
		return ret;
	s_stub_pa = stub_pa;

	if (stop) {
		if (!p_smp_send_stop) {
			pr_err("stage1: stop=1 requires p_smp_send_stop\n");
			free_idmap();
			return -EINVAL;
		}
		*((volatile unsigned long *)(s_marker_va + MARKER_PRESTOP)) = 1;
		pr_err("stage1: C-GO marker pa=0x%lx smp_send_stop@0x%lx wdt=%d\n",
			s_marker_pa, p_smp_send_stop, wdt_reset);
		msleep(300);
		call0((void *)p_smp_send_stop);
		/* no printk here: only asm until reset (invariant #3) */
	}

	pr_err("stage1: C-ARMED pgd=0x%lx pmd=0x%lx stub=0x%lx marker=0x%lx (last log before MMU off)\n",
		s_pgd_pa, s_pmd_pa, stub_pa, s_marker_pa);
	test_c_run();
}

/*
 * mode=4: read-only probe of a previously recorded marker page physical
 * address. Used after a WDT reset to test whether DRAM content survives a
 * warm reset (design question #2). No writes anywhere.
 */
static int run_probe_marker(void)
{
	void *m;
	int i, hits = 0, total;

	if (!p_marker_pa) {
		pr_err("stage1: mode=4 requires p_marker_pa\n");
		return -EINVAL;
	}
	total = p_marker_count > 0 ? p_marker_count : MARKER_PAGES;
	m = memremap(p_marker_pa, total << PAGE_SHIFT, MEMREMAP_WB);
	if (!m) {
		pr_err("stage1: probe memremap 0x%lx failed\n", p_marker_pa);
		return -EIO;
	}
	for (i = 0; i < total; i++) {
		unsigned long long v =
			((volatile unsigned long long *)m)[i << (PAGE_SHIFT - 3)];
		if (v == MARKER_MAGIC)
			hits++;
	}
	pr_err("stage1: probe pa=0x%lx pages=%d magic_hits=%d page0_q0=0x%llx\n",
		p_marker_pa, total, hits,
		((volatile unsigned long long *)m)[0]);
	memunmap(m);
	return 0;
}

/*
 * mode=5: Test D - copy the 2 MiB UEFI FD into the boot-time reserved region
 * at p_fd_base (default 0xB0000000), verify it byte-exact, clean the D-cache,
 * then run the trampoline with the jump target set to the FD entry. The FD
 * region MUST be protected by the boot DTB reserved-memory node first.
 */
static int run_test_d(void)
{
	unsigned long stub_pa, fd_va_mapped, dtb_pa = 0;
	void *fd_buf, *pb, *flush_va = NULL;
	struct file *f;
	loff_t pos = 0;
	int ret;

	if (!p_jump_pa || !p_fd_base) {
		pr_err("stage1: mode=5 requires p_jump_pa and p_fd_base\n");
		return -EINVAL;
	}
	if (!fd_path || !p_filp_open || !p_kernel_read) {
		pr_err("stage1: mode=5 requires fd_path, p_filp_open, p_kernel_read\n");
		return -EINVAL;
	}

	f = (struct file *)call3((void *)p_filp_open, (unsigned long)fd_path,
				 0 /*O_RDONLY*/, 0);
	pr_err("stage1: D-filp_open ret=0x%lx\n", (unsigned long)f);
	if (!f) {
		pr_err("stage1: cannot open %s\n", fd_path);
		return -ENOENT;
	}

	fd_buf = vmalloc(FD_SIZE_BYTES);
	if (!fd_buf)
		return -ENOMEM;
	ret = (int)call4((void *)p_kernel_read, (unsigned long)f,
			 (unsigned long)fd_buf, FD_SIZE_BYTES,
			 (unsigned long)&pos);
	pr_err("stage1: D-kernel_read ret=%d first16=%08x %08x %08x %08x\n",
	       ret,
	       *(volatile unsigned int *)fd_buf,
	       *((volatile unsigned int *)fd_buf + 1),
	       *((volatile unsigned int *)fd_buf + 2),
	       *((volatile unsigned int *)fd_buf + 3));

	if (*(volatile unsigned int *)((char *)fd_buf + FD_FVH_OFFSET) !=
	    0x4856465f /* "_FVH" */) {
		pr_err("stage1: FD has no _FVH at 0x%lx\n", FD_FVH_OFFSET);
		vfree(fd_buf);
		return -EINVAL;
	}
	pr_err("stage1: D-FD loaded, _FVH ok\n");

	/* Map the whole 4 MiB reserved slot WRITE-THROUGH/UNCACHED: writes go
	 * straight to DRAM (no dependency on cache cleaning), so the post-MMU-off
	 * fetch sees the FD bytes for certain. */
	fd_va_mapped = (unsigned long)memremap(p_fd_base, FD_SLOT_BYTES,
					       MEMREMAP_WT);
	if (!fd_va_mapped) {
		pr_err("stage1: memremap fd base 0x%lx failed\n", p_fd_base);
		vfree(fd_buf);
		return -EIO;
	}
	memcpy((void *)fd_va_mapped, fd_buf, FD_SIZE_BYTES);
	if (memcmp((const void *)fd_va_mapped, fd_buf, FD_SIZE_BYTES) != 0) {
		pr_err("stage1: FD copy verify FAILED\n");
		memunmap((void *)fd_va_mapped);
		vfree(fd_buf);
		return -EIO;
	}
	/* CIVAC the FD slot too: the previous UEFI session left WB lines here
	 * as well; today they happen to match the new content, which is why FD
	 * always "worked". Force miss-to-DRAM for certainty. */
	{
		unsigned long q;
		for (q = (unsigned long)fd_va_mapped & ~63UL;
		     q < (unsigned long)fd_va_mapped + FD_SIZE_BYTES; q += 64)
			asm volatile("dc civac, %0" : : "r"(q) : "memory");
		asm volatile("dsb sy" : : : "memory");
	}
	pr_err("stage1: D-FD copied %lu bytes to 0x%lx, verify OK, civac done\n",
	       (unsigned long)FD_SIZE_BYTES, p_fd_base);
	vfree(fd_buf);

	memunmap((void *)fd_va_mapped);

	/*
	 * Optional: place an "XNUP" container in the reserved payload window so
	 * the firmware-side XnuLoader (BootManagerLib) can find it after the
	 * jump. The window is the no-map DTB reservation 0xB5400000+0x3000000,
	 * so this only writes memory Linux never maps. Any error aborts BEFORE
	 * the CPUs are stopped.
	 */
	if (p_payload_path != NULL) {
		const unsigned long limit = p_payload_limit ? p_payload_limit :
					    0x03000000UL;
		struct file *pf;
		void *pl_buf = NULL;
		void *pl_map = NULL;
		loff_t pl_pos = 0;
		int pl_ret;
		unsigned int magic_word;

		if (p_payload_base != 0xB5400000UL) {
			pr_err("stage1: payload base must be 0xb5400000\n");
			return -EINVAL;
		}

		pf = (struct file *)call3((void *)p_filp_open,
					  (unsigned long)p_payload_path,
					  0 /*O_RDONLY*/, 0);
		if (!pf) {
			pr_err("stage1: cannot open payload %s\n", p_payload_path);
			return -ENOENT;
		}

		pl_buf = vmalloc(limit);
		if (!pl_buf) {
			return -ENOMEM;
		}
		pl_ret = (int)call4((void *)p_kernel_read, (unsigned long)pf,
				    (unsigned long)pl_buf, limit,
				    (unsigned long)&pl_pos);
		if (pl_ret <= 0) {
			pr_err("stage1: payload read ret=%d\n", pl_ret);
			vfree(pl_buf);
			return -EIO;
		}

		magic_word = *(volatile unsigned int *)pl_buf;
		if (magic_word != 0x50554e58 /* "XNUP" */ ||
		    (unsigned int)pl_ret < 8 ||
		    *(volatile unsigned int *)((char *)pl_buf + 4) >
		    (unsigned int)(pl_ret - 8)) {
			pr_err("stage1: payload is not a valid XNUP container (magic=%08x len=%d)\n",
			       magic_word, pl_ret);
			vfree(pl_buf);
			return -EINVAL;
		}

		pl_map = memremap(p_payload_base, (unsigned long)pl_ret,
				  MEMREMAP_WT);
		if (!pl_map) {
			pr_err("stage1: memremap payload 0x%lx failed\n",
			       p_payload_base);
			vfree(pl_buf);
			return -EIO;
		}
		memcpy(pl_map, pl_buf, (size_t)pl_ret);
		if (memcmp(pl_map, pl_buf, (size_t)pl_ret) != 0) {
			pr_err("stage1: payload copy verify FAILED\n");
			memunmap(pl_map);
			vfree(pl_buf);
			return -EIO;
		}
		/*
		 * Clean AND INVALIDATE the payload window's cache lines before
		 * the jump. Root cause of the 2026-11-07 "loader sees no
		 * container" runs: stale WB lines from the PREVIOUS UEFI session
		 * (which wrote TRC/marks inside this window) survive in this
		 * core's dcache across the MMU-off jump; SEC's WB read of
		 * 0xB5400000 then HITS those lines and never reaches DRAM.
		 * plain CVAC (clean only) cannot fix a stale clean line - only
		 * CIVAC (clean+invalidate) forces the next read to miss to DRAM.
		 */
		{
			unsigned long q;
			for (q = (unsigned long)pl_map & ~63UL;
			     q < (unsigned long)pl_map + (unsigned long)pl_ret; q += 64)
				asm volatile("dc civac, %0" : : "r"(q) : "memory");
			asm volatile("dsb sy" : : : "memory");
		}
		pr_err("stage1: D-PAYLOAD copied %d bytes to 0x%lx, magic OK, dcache cleaned\n",
		       pl_ret, p_payload_base);
		memunmap(pl_map);
		vfree(pl_buf);
	}

	/* Clear all four live beacon partitions before stopping CPUs and retain
	 * the complete mapping so the jump block uses exactly the same mapping. */
	ret = map_testd_beacons();
	if (ret) {
		pr_err("stage1: cannot map complete Test-D beacon area\n");
		return ret;
	}
	pb = s_pb_map;

	/* BootShim cold-boot contract: DTB PA in the handoff slot, x0 = DTB */
	if (p_dtb_pa) {
		void *slot = memremap(DTB_PTR_SLOT_DEFAULT, 8, MEMREMAP_WB);

		if (slot) {
			*(unsigned long *)slot = p_dtb_pa;
			clean_dcache_range((unsigned long)slot, 8);
			memunmap(slot);
			dtb_pa = p_dtb_pa;
			pr_err("stage1: DTB pa=0x%lx written to handoff slot\n",
			       p_dtb_pa);
		}
	}

	stub_pa = vmalloc_text_pa((const void *)evergo_stage1_stub_c);
	if (!stub_pa) {
		pr_err("stage1: cannot resolve stub C physical address\n");
		return -ENOENT;
	}
	ret = build_idmap(stub_pa, s_marker_pa);
	if (ret)
		return ret;

	/* Everything (jump block, beacons) is prepared BEFORE the SMP stop:
	 * after smp_send_stop only printk and raw asm are safe here. */
	ret = write_jump_block(p_jump_pa, dtb_pa);
	if (ret) {
		pr_err("stage1: jump block write/readback failed; refusing stop/jump\n");
		return ret;
	}
	s_stub_pa = stub_pa;
	if (s_pb_map) {
		*(volatile unsigned int *)((char *)s_pb_map + 0x10) =
			0x6d617274; /* "tram": pre-trampoline prep done */
		clean_dcache_range((unsigned long)s_pb_map, SEC_BEACON_AREA);
	}

	pr_err("stage1: D-ARMED jump=0x%lx x0=0x%lx pgd=0x%lx stub=0x%lx\n",
	       p_jump_pa, dtb_pa, s_pgd_pa, stub_pa);
	if (stop) {
		if (!p_smp_send_stop) {
			pr_err("stage1: stop=1 requires p_smp_send_stop\n");
			free_idmap();
			return -EINVAL;
		}
		/*
		 * The DXE heap lives in a no-map DTB reservation, so Linux never
		 * maps it and no stale lines can exist for it: the old DC CIVAC
		 * sweep is unnecessary. p_flush_len=0 skips it; an explicit pair is
		 * still honoured for A/B runs but must stay inside that window.
		 */
		if (p_flush_len != 0) {
			if (!p_flush_dcache_area ||
			    (p_flush_pa & (PAGE_SIZE - 1)) ||
			    (p_flush_len & (PAGE_SIZE - 1)) ||
			    p_flush_pa < 0xB8400000UL ||
			    p_flush_pa + p_flush_len > 0xB9400000UL) {
				pr_err("stage1: unsupported heap cache-maintenance range\n");
				free_idmap();
				return -EINVAL;
			}
			/* Prepare the mapping while the scheduler and other CPUs
			 * still run: after the stop only asm and printk are safe. */
			flush_va = memremap(p_flush_pa, p_flush_len, MEMREMAP_WB);
			if (!flush_va) {
				free_idmap();
				return -ENOMEM;
			}
			pr_err("stage1: D-FLUSH prepared pa=0x%lx len=0x%lx fn=0x%lx\n",
			       p_flush_pa, p_flush_len, p_flush_dcache_area);
		} else {
			pr_err("stage1: D-FLUSH skipped (heap is a no-map reservation)\n");
		}
	/*
	 * Publish the framebuffer handoff inside the stop window: from here only
	 * one printk and a fixed 300 ms sleep remain before smp_send_stop(), so
	 * the OVL address read is at most ~300 ms stale - versus seconds when it
	 * was issued from userspace before the FD/payload copies. A DRM flip in
	 * this window costs at most one double-buffer slot; the DPU keeps
	 * scanning whatever OVL_ADDR holds at stop time. Failure is not fatal:
	 * the earlier (staler) handoff still stands.
	 */
	if (publish_fb_handoff() == 0) {
		pr_err("stage1: D-FB handoff published (pre-stop)\n");
	} else {
		pr_err("stage1: D-FB pre-stop publish failed; keeping earlier one\n");
	}
	*((volatile unsigned long *)(s_marker_va + MARKER_PRESTOP)) = 1;
	pr_err("stage1: D-GO jump_pa=0x%lx x0=0x%lx smp_send_stop@0x%lx\n",
	       p_jump_pa, dtb_pa, p_smp_send_stop);
	msleep(300);
	call0((void *)p_smp_send_stop);
		/* Empirical mitigation: __flush_dcache_area uses DC CIVAC to PoC.
		 * The DXE heap now lives in its own no-map DTB reservation, so
		 * Linux never maps those pages; this only drops stale lines left
		 * by a previous UEFI run across a warm reset. Keep the mapping
		 * until reset: memunmap may take locks and cannot run here. */
		if (flush_va != NULL) {
			call3((void *)p_flush_dcache_area, (unsigned long)flush_va,
			      p_flush_len, 0);
		}
	}

	test_c_run();
}

/*
 * mode=7: dump a physical region (p_marker_pa, p_marker_count pages) into a
 * file (fd_path) via filp_open + kernel_write, for post-crash forensics of
 * regions the kernel cannot otherwise reach (no /dev/mem on this device).
 */
static int run_beacon_probe(void)
{
	volatile unsigned long *p;
	unsigned long test_jump = 0x1122334455667788UL;
	unsigned long test_dtb = 0x8877665544332211UL;
	int ret;

	ret = map_testd_beacons();
	if (ret)
		return ret;
	ret = write_jump_block(test_jump, test_dtb);
	if (ret)
		return ret;
	p = (volatile unsigned long *)((char *)s_pb_map +
			(PAYLOAD_SLOT - TESTD_BEACON_BASE));
	pr_err("stage1: mode=8 jump block expected=%lx/%lx read=%lx/%lx\n",
	       test_jump, test_dtb, p[0], p[1]);
	return (p[0] == test_jump && p[1] == test_dtb) ? 0 : -EIO;
}

static int run_dump_region(void)
{
	void *m;
	struct file *f;
	loff_t pos = 0;
	unsigned long total;
	int ret;

	if (!p_marker_pa || !fd_path || !p_filp_open || !p_kernel_write) {
		pr_err("stage1: mode=7 requires p_marker_pa, fd_path, p_filp_open, p_kernel_write\n");
		return -EINVAL;
	}
	total = (p_marker_count > 0 ? p_marker_count : 16) << PAGE_SHIFT;
	m = memremap(p_marker_pa, total, MEMREMAP_WB);
	if (!m) {
		pr_err("stage1: dump memremap 0x%lx failed\n", p_marker_pa);
		return -EIO;
	}
	f = (struct file *)call3((void *)p_filp_open, (unsigned long)fd_path,
				 0x41 /* O_WRONLY|O_CREAT */, 0x180 /* 0600 */);
	if (!f) {
		pr_err("stage1: cannot create %s\n", fd_path);
		memunmap(m);
		return -ENOENT;
	}
	ret = (int)call4((void *)p_kernel_write, (unsigned long)f,
			 (unsigned long)m, total, (unsigned long)&pos);
	pr_err("stage1: dump wrote %d of %lu bytes from 0x%lx to %s\n",
	       ret, total, p_marker_pa, fd_path);
	memunmap(m);
	return 0;
}

/*
  FB_PAGE_MAP layout, mirroring (byte for byte, packed) the firmware-side
  definition in
  uefi-camellia/Silicon/Silicium/SiliciumPkg/Include/Library/FrameBufferMap.h.
  This module builds out-of-tree against the kernel headers only, so the UEFI
  header is not included; the two definitions must stay in sync. The static
  asserts below fail the build if the local restatement ever drifts.
*/
#define FB_PAGE_MAP_MAX_RUNS_LOCAL  100u

/* Flags word published right after the FB_PAGE_MAP struct, at the fixed
 * offset contract defined in FrameBufferMap.h (FB_MAP_FLAGS_OFFSET). */
#define FB_MAP_FLAGS_OFFSET_LOCAL   0x658u
#define FB_MAP_FLAG_FBDC_LOCAL      (1u << 0)

struct fb_run {
	u64 physical;
	u32 pages;
	u32 reserved;
} __attribute__((packed));

struct fb_page_map_hdr {
	u64 magic;
	u64 logical_size;
	u32 page_size;
	u32 run_count;
} __attribute__((packed));

_Static_assert(sizeof(struct fb_run) == 16, "fb_run must match FB_PAGE_MAP_RUN");
_Static_assert(sizeof(struct fb_page_map_hdr) == 24, "fb_page_map_hdr must match FB_PAGE_MAP header");
_Static_assert(sizeof(struct fb_page_map_hdr) +
	       FB_PAGE_MAP_MAX_RUNS_LOCAL * sizeof(struct fb_run) == 0x658,
	       "FB_PAGE_MAP size drifted from the FB_MAP_FLAGS_OFFSET contract");
_Static_assert(FB_MAP_FLAGS_OFFSET_LOCAL + 8 <= 0x680,
	       "flags word runs past the reserved handoff region");

/*
  Publish FB_HANDOFF (0xA4000000) and an FB_PAGE_MAP (0xA4000100) describing the
  framebuffer the DPU is scanning RIGHT NOW.

  The buffer is contiguous in IOVA space but NOT necessarily in physical memory,
  so the map carries one run per physically contiguous extent (up to
  FB_PAGE_MAP_MAX_RUNS); consumers translate through the runs rather than
  assuming a linear layout.

  Geometry and base come from the live OVL registers translated through the
  device IOMMU domain. Called from mode=11 (standalone) and from mode=5
  immediately before the jump: Android page-flips the scanout every frame,
  so publishing a second early makes the handoff point at a stale buffer.
*/
static int publish_fb_handoff(void)
{
		/* Write FB_HANDOFF (0xA4000000) and a single-run FB_PAGE_MAP
		 * (0xA4000100) describing the framebuffer the DPU is actually
		 * scanning. Geometry and physical base come from the live OVL
		 * registers translated through the device's IOMMU domain.
		 * No jump, no reset, no display reconfiguration. */
		void __iomem *b;
		void *ho;
		u32 ovl_addr = 0, src_size = 0, pitch = 0, pitch_msb = 0, src_con = 0, raw_pitch = 0;
		u32 width, height;
		unsigned long ovl_base = 0x14006000UL;   /* OVL0_2L first, then OVL0 */
		const char *dev_name = "14006000.disp_ovl0_2l";
		struct device *dev;
		struct iommu_domain *dom;
		unsigned long iova, pa0;
		u64 total;
		u32 pages;
		/* One entry per physically contiguous extent of the scanout. */
		struct fb_run runs[FB_PAGE_MAP_MAX_RUNS_LOCAL];
		u32 nruns;
		u32 dp_con = 0;   /* OVL DATAPATH_CON of the active layer */
		u32 hdr_addr = 0; /* 0x0f44: AFBC header start == buffer base */
		u32 fbdc;         /* 1 = layer runs through FBDC/AFBC */
		int i, used = -1;

		pr_err("stage1: mode=11 handoff write begin\n");

		/* pick the active OVL */
		for (i = 0; i < 2; i++) {
			unsigned long base = i ? 0x14005000UL : 0x14006000UL;
			const char *nm = i ? "14005000.disp_ovl0" : "14006000.disp_ovl0_2l";
			b = ioremap(base, 0x1000);
			if (!b)
				continue;
			ovl_addr = readl(b + 0x0f40);
			hdr_addr = readl(b + 0x0f44);   /* AFBC header = buffer base */
			src_size = readl(b + 0x0038);
			pitch    = readl(b + 0x0044) | ((readl(b + 0x0040) & 0xf) << 16);
			pitch_msb= 0;   /* not an address field on this SoC */
			src_con  = readl(b + 0x002c);
			dp_con   = readl(b + 0x024);   /* DATAPATH_CON: FBDC bits */
			iounmap(b);
			if (ovl_addr && (src_con & 0xf)) {
				ovl_base = base;
				dev_name = nm;
				used = i;
				break;
			}
		}
		if (used < 0) {
			pr_err("stage1: mode=11 no active OVL; aborting\n");
			return -EIO;
		}
		/*
		 * Pitch semantics depend on the layer's compression state
		 * (compr_l_config_AFBC_V1_2 in mtk_disp_ovl.c):
		 *
		 *   FBDC enabled  (DATAPATH_CON bit(4+layer)=1):
		 *       SRC_PITCH holds pitch_bytes * tile_h, and the buffer is
		 *       AFBC-framed (header before body). A linear consumer
		 *       CANNOT render this - the layer must be switched to
		 *       linear first.
		 *   FBDC disabled:
		 *       SRC_PITCH is the plain byte pitch.
		 *
		 * Read DATAPATH_CON BEFORE iounmap and publish both the raw
		 * value and the resolved byte pitch, so the firmware side can
		 * tell which regime it inherited.
		 */
		raw_pitch = pitch;
		fbdc = (dp_con >> (4 + used)) & 1;
		if (fbdc) {
			pitch = (raw_pitch & 7) == 0 ? (raw_pitch >> 3) : 0;
		} else {
			pitch = raw_pitch;   /* plain byte pitch, no tiling */
		}
		if (!pitch) {
			pr_err("stage1: mode=11 raw pitch 0x%x undecodable (fbdc=%u)\n",
			       raw_pitch, fbdc);
			return -EIO;
		}

		width  = src_size & 0xffff;
		height = (src_size >> 16) & 0xffff;
		if (!width || width > 4096 || height < 475 || height > 8192 ||
		    pitch < width * 4 || pitch > 65536) {
			pr_err("stage1: mode=11 bad geometry %ux%u byte_pitch %u (raw 0x%x)\n",
			       width, height, pitch, raw_pitch);
			return -EIO;
		}
		pr_err("stage1: mode=11 geometry %ux%u byte_pitch=%u (raw 0x%x) fbdc=%u dp=0x%08x\n",
		       width, height, pitch, raw_pitch, fbdc, dp_con);
		dev = bus_find_device_by_name(&platform_bus_type, NULL, dev_name);
		if (!dev) {
			pr_err("stage1: mode=11 dev %s not found\n", dev_name);
			return -EIO;
		}
		dom = iommu_get_domain_for_dev(dev);
		if (!dom) {
			pr_err("stage1: mode=11 no IOMMU domain\n");
			put_device(dev);
			return -EIO;
		}
		/*
		 * AFBC layout: the buffer is [header(0x28000 for this panel)] +
		 * [body]. 0x0f40 holds the BODY address; 0x0f44 holds the header,
		 * i.e. the real buffer base. Scanning/writing 10,444,800 bytes
		 * from the body start would run 0x28000 past the allocation, so
		 * when the layer is FBDC we publish the buffer base instead - the
		 * firmware then switches 0x0f40 there and draws linearly from it.
		 */
		iova = (fbdc && hdr_addr) ? (unsigned long)hdr_addr
					   : (unsigned long)ovl_addr;
		pa0 = (unsigned long)iommu_iova_to_phys(dom, (dma_addr_t)iova);
		put_device(dev);
		if (!pa0 || (pa0 & 0xfff)) {
			pr_err("stage1: mode=11 iova 0x%08lx -> bad pa 0x%08lx\n",
			       iova, pa0);
			return -EIO;
		}
		total = (u64)pitch * height;
		pages = (u32)((total + 0xfff) >> 12);

		/*
		 * Walk EVERY page and describe the buffer as physical runs.
		 *
		 * ION/dmabuf only guarantees contiguity in the *IOMMU* address space,
		 * not in physical memory: the scanout is one linear IOVA range backed
		 * by scattered PA pages. The previous code demanded a single contiguous
		 * PA run and returned -EIO at the first break (observed at page
		 * 8/2550), so no handoff was ever published.
		 *
		 * Aggregate instead: extend the current run while the PA equals
		 * (run.physical + run.pages * 0x1000), otherwise open a new run. Every
		 * page is still verified, so a hole or a misaligned page stays fatal,
		 * and the published map covers exactly the pages the DPU scans.
		 */
		nruns = 0;
		for (i = 0; i < (int)pages; i++) {
			unsigned long p2 = (unsigned long)iommu_iova_to_phys(
				dom, (dma_addr_t)(iova + ((unsigned long)i << 12)));

			if (!p2 || (p2 & 0xfff)) {
				pr_err("stage1: mode=11 bad fb page %d/%u "
				       "(iova 0x%lx pa 0x%lx)\n",
				       i, pages, iova + ((unsigned long)i << 12), p2);
				return -EIO;
			}
			if (nruns && p2 == (unsigned long)(runs[nruns - 1].physical +
					((u64)runs[nruns - 1].pages << 12))) {
				runs[nruns - 1].pages++;
				continue;
			}
			if (nruns >= FB_PAGE_MAP_MAX_RUNS_LOCAL) {
				pr_err("stage1: mode=11 fb needs more than %u runs "
				       "at page %d/%u; refusing\n",
				       FB_PAGE_MAP_MAX_RUNS_LOCAL, i, pages);
				return -EIO;
			}
			runs[nruns].physical = (u64)p2;
			runs[nruns].pages = 1;
			runs[nruns].reserved = 0;
			nruns++;
		}
		if (!nruns) {
			pr_err("stage1: mode=11 produced no fb runs\n");
			return -EIO;
		}
		/* FB_HANDOFF.Base must name the first run's physical address. */
		pa0 = (unsigned long)runs[0].physical;

		ho = memremap(0xA4000000UL, 0x800, MEMREMAP_WB);
		if (!ho) {
			pr_err("stage1: mode=11 memremap 0xA4000000 failed\n");
			return -EIO;
		}
		{
			volatile u64 *h = ho;
			u8 *mp = (u8 *)ho + 0x100;
			struct fb_page_map_hdr *mh = (struct fb_page_map_hdr *)mp;
			struct fb_run *mr = (struct fb_run *)(mp + sizeof(*mh));
			u32 k;

			/* FB_HANDOFF: magic, base, width, height, pitch, depth */
			h[0] = 0x3130424649464555ULL;
			h[1] = pa0;
			((volatile u32 *)h)[4] = width;
			((volatile u32 *)h)[5] = height;
			((volatile u32 *)h)[6] = pitch;
			((volatile u32 *)h)[7] = 32;

			/* FB_PAGE_MAP: header + one entry per physical run */
			mh->magic = 0x314D504649465545ULL;
			mh->logical_size = total;
			mh->page_size = 0x1000;
			mh->run_count = nruns;
			for (k = 0; k < nruns; k++) {
				mr[k].physical = runs[k].physical;
				mr[k].pages = runs[k].pages;
				mr[k].reserved = 0;
			}
			/* Flags word (contract offset, see FrameBufferMap.h):
			 * bit0 tells the firmware the layer still runs through
			 * FBDC/AFBC, so a linear writer must not draw into this
			 * buffer before switching the layer to linear. */
			*(volatile u32 *)((u8 *)ho + 0x100 + FB_MAP_FLAGS_OFFSET_LOCAL) =
				fbdc ? FB_MAP_FLAG_FBDC_LOCAL : 0;
		}
		clean_dcache_range((unsigned long)ho, 0x800);
		pr_err("stage1: mode=11 wrote handoff base=0x%08lx %ux%u pitch=%u "
		       "pages=%u runs=%u fbdc=%u (OVL%d)\n",
		       pa0, width, height, pitch, pages, nruns, fbdc, used);
		memunmap(ho);
		return 0;
}

static int __init evergo_stage1_init(void)
{
	int ret;

	pr_err("stage1: enter mode=%d stop=%d wdt_reset=%d confirm=%d\n",
		mode, stop, wdt_reset, confirm);
	report_sysregs();

	if (mode < 1 || (mode > 5 && mode != 7 && mode != 8 && mode != 9 && mode != 10 && mode != 11 && mode != 12 && mode != 13)) {
		pr_err("stage1: invalid mode=%d\n", mode);
		return -EINVAL;
	}

	if (mode == 9) {
		/* P1 positive control: write p_wval (8 bytes) to physical
		 * p_marker_pa, clean dcache, read back. No jump, no stop, no
		 * reset: the caller reboots normally, so the write must
		 * survive a WARM reboot to validate the read channel. */
		void *m;
		unsigned long before, written;

		if (!p_marker_pa) {
			pr_err("stage1: mode=9 requires p_marker_pa\n");
			return -EINVAL;
		}
		m = memremap(p_marker_pa, 8, MEMREMAP_WB);
		if (!m) {
			pr_err("stage1: mode=9 memremap 0x%lx failed\n", p_marker_pa);
			return -EIO;
		}
		before = *(volatile unsigned long *)m;
		written = p_wval ? p_wval : 0x5A5A0000DEAD0000UL;
		*(volatile unsigned long *)m = written;
		clean_dcache_range((unsigned long)m, 8);
		pr_err("stage1: mode=9 wrote 0x%lx to pa 0x%lx (was 0x%lx, readback 0x%lx)\n",
		       written, p_marker_pa, before,
		       *(volatile unsigned long *)m);
		memunmap(m);
		return 0;
	}

	if (mode == 10) {
		/* Read-only DPU scan probe: reports what the panel is actually
		 * scanning RIGHT NOW (RDMA0 start, OVL address -> iommu PA,
		 * geometry, contiguity) plus the FB handoff/page-map state at
		 * 0xA4000000/0xA4000100. Writes nothing. No jump, no reset. */
		void __iomem *b;
		u32 rdma_start = 0, ovl_addr[2] = {0, 0}, size[2] = {0, 0};
		u32 pitch[2] = {0, 0}, msb[2] = {0, 0}, con[2] = {0, 0};
		static const unsigned long ovl_base[2] = { 0x14005000UL, 0x14006000UL };
		static const char *ovl_name[2] = { "14005000.disp_ovl0",
						   "14006000.disp_ovl0_2l" };
		int i;
		void *ho, *mo;

		pr_err("stage1: mode=10 DPU probe begin\n");

		b = ioremap(0x14007000UL, 0x1000);
		if (b) {
			rdma_start = readl(b + 0x0f00);
			iounmap(b);
			pr_err("stage1: RDMA0 mem_start = 0x%08x\n", rdma_start);
		} else {
			pr_err("stage1: RDMA0 ioremap failed\n");
		}

		for (i = 0; i < 2; i++) {
			struct device *dev;
			struct iommu_domain *dom;
			unsigned long iova;
			unsigned long pa = 0;

			b = ioremap(ovl_base[i], 0x1000);
			if (!b) {
				pr_err("stage1: OVL%d ioremap failed\n", i);
				continue;
			}
			ovl_addr[i] = readl(b + 0x0f40);
			size[i]     = readl(b + 0x0038);
			pitch[i]    = readl(b + 0x0044);
			msb[i]      = readl(b + 0x0040);
			con[i]      = readl(b + 0x002c);
			iounmap(b);

			/*
			 * MTK DRM register semantics (drivers/gpu/drm/mediatek/
			 * mtk_disp_ovl.c):
			 *   0x044 = SRC_PITCH low 16 bits  (L_PITCH_FLD_SRC_PITCH
			 *           = REG_FLD_MSB_LSB(15,0))
			 *   0x040 = SRC_PITCH high 4 bits  (SRC_PITCH_MSB), not an
			 *           address field - the IOVA high bits do not live
			 *           here on this SoC.
			 *   the programmed value is pitch_bytes * tile_h, so the
			 *   byte pitch is (raw >> 3) for the 8-row tiled mode the
			 *   panel uses (4352 * 8 = 34816).
			 */
			{
				u32 raw = pitch[i] | ((msb[i] & 0xf) << 16);
				u32 pb  = (raw & 7) == 0 ? (raw >> 3) : 0;

				pr_err("stage1: OVL%d addr=0x%08x size=%ux%u con=0x%08x raw_pitch=0x%05x byte_pitch=%u(%s)\n",
				       i, ovl_addr[i], size[i] & 0xffff,
				       (size[i] >> 16) & 0xffff, con[i], raw,
				       pb, pb ? "tile8" : "UNKNOWN");
			}

			if (!ovl_addr[i] || !(con[i] & 0xf))
				continue;   /* not active */

			dev = bus_find_device_by_name(&platform_bus_type, NULL,
						      ovl_name[i]);
			if (!dev) {
				pr_err("stage1: OVL%d dev %s not found\n", i, ovl_name[i]);
				continue;
			}
			dom = iommu_get_domain_for_dev(dev);
			if (!dom) {
				pr_err("stage1: OVL%d has no IOMMU domain\n", i);
				put_device(dev);
				continue;
			}
			iova = ovl_addr[i];   /* IOVA from OVL_ADDR; 0x040 is pitch MSB, not address */
			pa = (unsigned long)iommu_iova_to_phys(dom, (dma_addr_t)iova);
			put_device(dev);
			pr_err("stage1: OVL%d iova 0x%08lx -> pa 0x%08lx %s\n",
			       i, iova, pa,
			       (pa && !(pa & 0xfff)) ? "ALIGNED" : "unaligned/absent");

			/* contiguity: sample next three pages */
			{
				int j;
				unsigned long prev = pa;
				int contiguous = 1;
				for (j = 1; j <= 3; j++) {
					unsigned long p2 = (unsigned long)iommu_iova_to_phys(
						dom, (dma_addr_t)(iova + ((unsigned long)j << 12)));
					if (!p2 || p2 != prev + 0x1000) {
						contiguous = 0;
						break;
					}
					prev = p2;
				}
				pr_err("stage1: OVL%d first pages %s\n", i,
				       contiguous ? "CONTIGUOUS" : "SCATTERED");
			}
		}

		/* FB handoff / page map state */
		ho = memremap(0xA4000000UL, 0x800, MEMREMAP_WB);
		if (ho) {
			u64 magic = *(volatile u64 *)ho;
			u64 hbase = *((volatile u64 *)ho + 1);
			u32 w = *((volatile u32 *)ho + 4);
			u32 h = *((volatile u32 *)ho + 5);
			u32 p = *((volatile u32 *)ho + 6);
			pr_err("stage1: FB_HANDOFF magic=0x%016llx base=0x%llx %ux%u pitch=%u\n",
			       magic, hbase, w, h, p);
			mo = (u8 *)ho + 0x100;
			{
				u64 mmagic = *(volatile u64 *)mo;
				u64 lsize = *((volatile u64 *)mo + 1);
				u32 psize = *((volatile u32 *)mo + 4);
				u32 runs = *((volatile u32 *)mo + 5);
				pr_err("stage1: FB_PAGE_MAP magic=0x%016llx logical=%llu page=%u runs=%u\n",
				       mmagic, lsize, psize, runs);
				if (runs > 0 && runs <= 100) {
					u64 r0phys = *((volatile u64 *)mo + 3);
					u32 r0pages = *((volatile u32 *)mo + 8);
					pr_err("stage1: PAGE_MAP run0 phys=0x%llx pages=%u\n",
					       r0phys, r0pages);
				}
			}
			memunmap(ho);
		} else {
			pr_err("stage1: cannot memremap 0xA4000000\n");
		}

		/* Compression / layer-format evidence. DATAPATH_CON(0x024) bit
		 * (4+layer) is the per-layer FBDC enable (compr_l_config_AFBC_V1_2
		 * in mtk_disp_ovl.c). When set, SRC_PITCH(0x044) holds
		 * pitch*tile_h and the buffer carries an AFBC header before the
		 * body (LX_HDR_ADDR 0xF44 / LX_HDR_PITCH 0xF48). Dump those
		 * explicitly plus the per-layer RDMA block; analysis offline. */
		{
			int r;
			for (i = 0; i < 2; i++) {
				u32 dp, hdr_addr, hdr_pitch;
				b = ioremap(ovl_base[i], 0x1000);
				if (!b)
					continue;
				dp = readl(b + 0x024);
				pr_err("stage1: OVL%d DATAPATH_CON=0x%08x fbdc_l0=%u fbdc_l1=%u\n",
				       i, dp, (dp >> 4) & 1, (dp >> 5) & 1);
				for (r = 0; r < 0x48; r += 4) {
					u32 v = readl(b + r);
					if (v || (r & 0xf) == 0)
						pr_err("stage1: OVL%d[%03x]=%08x%s",
						       i, r, v,
						       ((r & 0xf) == 0xc) ? "\n" : " ");
				}
				pr_err("\n");
				hdr_addr  = readl(b + 0x0f44);
				hdr_pitch = readl(b + 0x0f48);
				pr_err("stage1: OVL%d L0_HDR_ADDR=0x%08x L0_HDR_PITCH=0x%08x\n",
				       i, hdr_addr, hdr_pitch);
				for (r = 0x0f30; r < 0x0f80; r += 4) {
					u32 v = readl(b + r);
					if (v)
						pr_err("stage1: OVL%d [%03x]=%08x\n",
						       i, r, v);
				}
				/* layer0 RDMA control/gmc block */
				for (r = 0x0c0; r < 0x100; r += 4) {
					u32 v = readl(b + r);
					if (v)
						pr_err("stage1: OVL%d RDMA[%03x]=%08x\n",
						       i, r, v);
				}
				iounmap(b);
			}
		}

		pr_err("stage1: mode=10 DPU probe end\n");
		return 0;
	}

	if (mode == 12) {
		/* DRAM write-probe over a caller-selected physical range.
		 * For every 1 MiB chunk: write a pattern at the first and the
		 * middle word, clean dcache, read back, compare. A page that
		 * aborts on non-secure access fails as a logged MISMATCH /
		 * fault instead of taking Android down. Read-only wrt the rest
		 * of the system; runs and returns. p_marker_pa = base,
		 * p_marker_count = length in MiB (default 512 => 0x48e00000 +
		 * 0x20000000, the kernel window region). */
		unsigned long base = p_marker_pa ? p_marker_pa : 0x48e00000UL;
		unsigned long mib  = p_marker_count ? p_marker_count : 512;
		unsigned long i;
		char *map;

		pr_err("stage1: mode=12 DRAM probe base=0x%lx mib=%lu\n", base, mib);
		map = memremap(base, mib << 20, MEMREMAP_WB);
		if (!map) {
			pr_err("stage1: mode=12 memremap failed\n");
			return -EIO;
		}
		for (i = 0; i < mib; i++) {
			volatile unsigned long *p = (volatile unsigned long *)(map + (i << 20));
			unsigned long pat = 0x5A5A0000UL + (unsigned long)i;
			unsigned long rb1, rb2;

			rb1 = 0; rb2 = 0;
			probe_kernel_write((void *)p, &pat, sizeof(pat));
			clean_dcache_range((unsigned long)p, 8);
			rb1 = *(volatile unsigned long *)p;
			probe_kernel_write((void *)(p + 0x10000), &pat, sizeof(pat));
			clean_dcache_range((unsigned long)(p + 0x10000), 8);
			rb2 = *(volatile unsigned long *)(p + 0x10000);
			pr_err("stage1: m12 +%04luMiB pa=0x%lx pat=%lx rb1=%lx rb2=%lx %s\n",
			       i, base + (i << 20), pat, rb1, rb2,
			       (rb1 == pat && rb2 == pat) ? "OK" : "MISMATCH");
			cond_resched();
		}
		memunmap(map);
		pr_err("stage1: mode=12 done\n");
		return 0;
	}

	if (mode == 13) {
		/* KPD live probe: poll KP_MEM1/MEM2 at 0x10010004/8 from Linux,
		 * print on any change. Android keyboard path proof. */
		volatile u16 *mem1, *mem2;
		void *m = memremap(0x10010000UL, 0x1000, MEMREMAP_WB);
		if (!m) { pr_err("stage1: m13 memremap failed\n"); return -EIO; }
		mem1 = (volatile u16 *)(m + 0x04);
		mem2 = (volatile u16 *)(m + 0x08);
		pr_err("stage1: m13 KPD poll start (press volume keys now)\n");
		{
			u16 last1 = *mem1, last2 = *mem2;
			int iter;
			for (iter = 0; iter < 600; iter++) {   /* ~60 s @100ms */
				u16 v1 = *mem1, v2 = *mem2;
				if (v1 != last1 || v2 != last2) {
					pr_err("stage1: m13 MEM1=%04x MEM2=%04x (prev %04x/%04x)\n",
					       v1, v2, last1, last2);
					last1 = v1; last2 = v2;
				}
				msleep(100);
				cond_resched();
			}
		}
		pr_err("stage1: m13 done\n");
		memunmap(m);
		return 0;
	}

	if (mode == 11)
		return publish_fb_handoff();

	if (mode == 7)
		return run_dump_region();
	if (mode == 8)
		return run_beacon_probe();

	/* Every arming mode needs the marker pages; the mode=4 probe and the
	 * mode=5 copy path run after this allocation too (the mode=5 PRESTOP
	 * write crashes with s_marker_va == 0 if allocation is skipped). */
	ret = alloc_marker();
	if (ret)
		return ret;
	pr_err("stage1: marker va=0x%lx pa=0x%lx magic=0x%lx\n",
		s_marker_va, s_marker_pa, MARKER_MAGIC);

	if (mode == 4)
		return run_probe_marker();
	if (mode == 5)
		return run_test_d();

	if (mode == 1) {
		ret = run_test_a();
		free_marker();
		return ret;
	}
	if (mode == 2) {
		ret = run_test_b();
		free_marker();
		return ret;
	}
	/* mode == 3 never returns */
	return run_test_c();
}

static void __exit evergo_stage1_exit(void)
{
	free_idmap();
	free_marker();
	pr_err("stage1: unloaded\n");
}

module_init(evergo_stage1_init);
module_exit(evergo_stage1_exit);
