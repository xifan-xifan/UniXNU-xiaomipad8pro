import pathlib
s=pathlib.Path('/private/tmp/paclr70-build.py').read_text()
s=s.replace('/private/tmp/paclr70','/private/tmp/paclr71').replace('/private/tmp/piano-paclr70','/private/tmp/piano-paclr71')
anchor='shutil.copy2(root / "src/iokit/Kernel/IOService.cpp", dst / "IOService.cpp")'
assert s.count(anchor)==1
gic_code = r'''
# ---- paclr71: program GICv3 so the architected timer reaches sleh_fiq ----
shutil.copy2(root / "src/osfmk/arm/arm_init.c", dst / "arm_init.c")
ap = (dst / "arm_init.c").read_text()
_gic = r"""
/* piano (SM8750): the kernel's FIQ path expects an interrupt controller that
 * routes the architected timer, as ArmCpu does on Apple silicon. Nothing in
 * this platform's kext set programs the GIC, so fiq_context_init() left
 * CNTV_CTL_EL0 enabled with no way for PPI 27 to arrive. This is the minimal
 * GICv3 setup for group 1: distributor affinity routing, this CPU's
 * redistributor, priority 0xa0, level trigger, and the ICC_* CPU interface.
 * piano-gic=0 disables it for an A/B control. */
#define PIANO_GICD_PHYS   0x16000000ULL
#define PIANO_GICR_PHYS   0x16080000ULL
#define PIANO_GICR_STRIDE 0x40000ULL
#define PIANO_GICR_FRAMES 8
#define PIANO_GICD_CTLR        0x0000u
#define PIANO_GICR_TYPER       0x0008u
#define PIANO_GICR_WAKER       0x0014u
#define PIANO_GICR_SGI         0x10000u
#define PIANO_GICR_IGROUPR0    (PIANO_GICR_SGI + 0x0080u)
#define PIANO_GICR_ISENABLER0  (PIANO_GICR_SGI + 0x0100u)
#define PIANO_GICR_ICENABLER0  (PIANO_GICR_SGI + 0x0180u)
#define PIANO_GICR_IPRIORITYR  (PIANO_GICR_SGI + 0x0400u)
#define PIANO_GICR_ICFGR1      (PIANO_GICR_SGI + 0x0C04u)
#define PIANO_TIMER_INTID      27u
#define PIANO_GICD_ARE_NS      (1u << 4)
#define PIANO_GICD_ENGRP1NS    (1u << 1)

void
piano_gicv3_init(void)
{
	static volatile uint8_t *gicd;
	static volatile uint8_t *gicr;
	static int cfg = -1;
	static int dist_done;
	uint64_t mpidr, sre, igrpen1;
	uint32_t typer_lo, typer_hi, waker, ctlr, i, frame = PIANO_GICR_FRAMES;
	uint32_t bit = 1u << (PIANO_TIMER_INTID - 16u);
	volatile uint8_t *rd;

	if (cfg < 0) {
		cfg = 1;
		PE_parse_boot_argn("piano-gic", &cfg, sizeof(cfg));
	}
	if (!cfg) {
		return;
	}
	if (gicd == NULL) {
		gicd = (volatile uint8_t *)ml_io_map(PIANO_GICD_PHYS, 0x10000);
	}
	if (gicr == NULL) {
		gicr = (volatile uint8_t *)ml_io_map(PIANO_GICR_PHYS, 0x200000);
	}
	if (gicd == NULL || gicr == NULL) {
		printf("piano-gic: mapping failed gicd=%p gicr=%p\n", gicd, gicr);
		return;
	}

	if (!dist_done) {
		ctlr = *(volatile uint32_t *)(gicd + PIANO_GICD_CTLR);
		*(volatile uint32_t *)(gicd + PIANO_GICD_CTLR) =
		    ctlr | PIANO_GICD_ARE_NS | PIANO_GICD_ENGRP1NS;
		dist_done = 1;
		printf("piano-gic: distributor ctlr 0x%x -> 0x%x\n", ctlr,
		    *(volatile uint32_t *)(gicd + PIANO_GICD_CTLR));
	}

	mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	for (i = 0; i < PIANO_GICR_FRAMES; i++) {
		volatile uint8_t *f = gicr + (uint64_t)i * PIANO_GICR_STRIDE;
		typer_lo = *(volatile uint32_t *)(f + PIANO_GICR_TYPER);
		typer_hi = *(volatile uint32_t *)(f + PIANO_GICR_TYPER + 4);
		if ((typer_hi & 0x00FFFFFFu) == ((uint32_t)mpidr & 0x00FFFFFFu)) {
			frame = i;
			break;
		}
		if (typer_lo & (1u << 4)) {
			break;
		}
	}
	if (frame == PIANO_GICR_FRAMES) {
		printf("piano-gic: no redistributor for mpidr 0x%llx\n", mpidr);
		return;
	}
	rd = gicr + (uint64_t)frame * PIANO_GICR_STRIDE;

	waker = *(volatile uint32_t *)(rd + PIANO_GICR_WAKER);
	*(volatile uint32_t *)(rd + PIANO_GICR_WAKER) = waker & ~1u;
	for (i = 0; i < 1000000u; i++) {
		if ((*(volatile uint32_t *)(rd + PIANO_GICR_WAKER) & 2u) == 0) {
			break;
		}
	}

	*(volatile uint32_t *)(rd + PIANO_GICR_IGROUPR0) =
	    *(volatile uint32_t *)(rd + PIANO_GICR_IGROUPR0) | bit;
	*(volatile uint32_t *)(rd + PIANO_GICR_ICFGR1) &=
	    ~(3u << ((PIANO_TIMER_INTID - 16u) * 2u));
	*(volatile uint8_t *)(rd + PIANO_GICR_IPRIORITYR + PIANO_TIMER_INTID) = 0xa0;
	*(volatile uint32_t *)(rd + PIANO_GICR_ICENABLER0) = bit;
	*(volatile uint32_t *)(rd + PIANO_GICR_ISENABLER0) = bit;

	sre = __builtin_arm_rsr64("S3_0_C12_C12_5");     /* ICC_SRE_EL1 */
	if ((sre & 1) == 0) {
		__builtin_arm_wsr64("S3_0_C12_C12_5", sre | 1);
		__builtin_arm_isb(ISB_SY);
	}
	__builtin_arm_wsr64("S3_0_C4_C6_0", 0xff);       /* ICC_PMR_EL1 */
	__builtin_arm_wsr64("S3_0_C12_C12_4", 0);        /* ICC_CTLR_EL1 */
	__builtin_arm_wsr64("S3_0_C12_C12_7", 1);        /* ICC_IGRPEN1_EL1 */
	__builtin_arm_isb(ISB_SY);

	igrpen1 = __builtin_arm_rsr64("S3_0_C12_C12_7");
	printf("piano-gic: cpu mpidr 0x%llx frame %u waker 0x%x sre 0x%llx igrpen1 0x%llx\n",
	    mpidr, frame, *(volatile uint32_t *)(rd + PIANO_GICR_WAKER),
	    __builtin_arm_rsr64("S3_0_C12_C12_5"), igrpen1);
}
"""
ap = ap.replace("collection_base_pointers);", "collection_base_pointers, 0, 0);")
_a = "#include <pexpert/pexpert.h>\n"
assert ap.count(_a) == 1
ap = ap.replace(_a, _a + _gic, 1)
_b = "\tfiq_context_init(TRUE);\n"
assert ap.count(_b) == 2
ap = ap.replace(_b, "\tpiano_gicv3_init();\n" + _b)
(dst / "arm_init.c").write_text(ap)
print("paclr71 gic patch", len(ap))
'''
s=s.replace(anchor, gic_code.strip('\n')+'\n'+anchor, 1)
pathlib.Path('/private/tmp/paclr71-build.py').write_text(s)
print('wrote paclr71')

