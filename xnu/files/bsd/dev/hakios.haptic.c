/*
 * The vibration motor of the Xiaomi Mi 9T Pro (raphael): an eccentric rotating mass motor (ERM)
 * driven by the haptics block of the PM8150B -- SPMI slave 3, peripheral 0xc0, registers
 * 0xc000..0xc0ff ("qcom,pm8150b@3/qcom,haptics@c000", compatible "qcom,haptics"). Every number
 * below is the phone's own, from Android's device tree (dtbo, dumped through the loader; see
 * docs/raphael-haptic-flash.md), from the registers Android's own kernel leaves in the block
 * (read through debugfs in OrangeFox, see android_left[]) and from Qualcomm's driver for the block
 * (drivers/input/misc/qti-haptics.c, which Android's kernel here carries):
 *
 *   actuator      ERM ("qcom,actuator-type" in the dtbo is "erm", and Android's register 0x4c is
 *                 0x01): no resonance to follow, no auto resonance, no shape
 *   vmax          3000 mV ("qcom,vmax-mv"), in steps of 116 mV
 *   play rate     6667 us ("qcom,play-rate-us"): how long one waveform byte lasts
 *   effects       six predefined waveforms, wf_0..wf_5 ("qcom,effect-id" 0..5): a pattern of up
 *                 to eight bytes written into the block's buffer (0x60), the rate, the number of
 *                 repeats (2^[6:4] times 2^[1:0], plus a brake pattern for wf_0 and wf_5)
 *
 * The block is reached over SPMI, through the SoC's PMIC arbiter -- the same bus the phone's keys
 * hang off (bsd/dev/hakios.buttons.c). Reads go through this system's observer channels; writes
 * only through a channel the arbiter says is ours. One transfer at a time, with the buttons'
 * driver's lock (hakios_spmi_lock()): two at once, and the arbiter drops one, as p353/p355 showed
 * when this file still bit-banged the redwood's I2C pins and took the whole bus down with it.
 *
 * Writing to a PMIC is not like writing anywhere else: the PMICs make the phone's supplies. So
 * every write goes through one function that takes only this block's registers (a list), every
 * write is read back, and any surprise ends all writing for the run.
 *
 * Nothing is done unless a boot-arg asks for it:
 *
 *   haptic=1   a look, reading only: the arbiter's channel for the block and who may write
 *              through it, the block's type and revision, its registers
 *   haptic=2   ... and the registers read against what Android's driver leaves in them (what this
 *              file would write); nothing written, nothing moves
 *   haptic=3   ... and Android's configuration written (each write read back), the module left
 *              off; nothing moves
 *   haptic=4   ... and one effect played once (haptic_effect=N, default 1: wf_0; 1 to 6)
 *   haptic=5   ... configured, and instead of the effect the motor for iOS until the run ends:
 *              what iOS plays on its "Actuator" device (userland/src/audio_plugin.c: the system's
 *              feedbacks and vibrations, CoreHaptics' patterns, UIKit's feedbacks) is written to
 *              /dev/hakios-haptic as samples of one channel, and the motor plays wf_3 by it: each
 *              waveform of iOS's (a stretch of samples after a pause) a short tap, and one that
 *              goes on longer a repeat of the same waveform until it ends -- or for HAPTIC_MAX_MS
 *              at most. The strength follows how loud iOS's waveform is: Android's vmax from a
 *              third of full scale up, five eighths of it for the softest (vmax_for()).
 *
 * Limits, whatever asks: Android's own values only (its vmax, its rates, its effects); no effect
 * longer than HAPTIC_MAX_MS, after which the motor rests as long before it plays again; any fault
 * the block reports (a short circuit), a write that does not read back, and nothing more is
 * written for the rest of the run. At the run's end (and on a panic's way out) the module is left
 * off. Nothing is written anywhere but to this block's registers.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level;
static bool broken, leaving;

/* every line to the loader's log as well (and to the console when that may be written) */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
wait_ms(uint32_t ms)
{
	if (ms != 0) {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/* ---- the PMIC arbiter (version 5) ---------------------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL    /* the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL    /* which system may write through each channel */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* reads: 0x80 per channel for our system (the first) */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL    /* writes: 0x10000 per channel */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* [19:8] slave and peripheral */
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define CHANNEL_WDATA1          0x14
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)      /* "extended register write long"; [11:4] register, [2:0] bytes - 1 */
#define COMMAND_READ            (1u << 27)      /* "extended register read long" */
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

/* the PM8150B's haptics block: slave 3, peripheral 0xc0 */
static struct peripherals {
	const char *    name;
	uint16_t        id;
	uint16_t        channel;
	bool            found, writable;
	vm_offset_t     write;
} hap = { .name = "the PM8150B's haptics block", .id = 0x3c0 };

static vm_offset_t arb_core, arb_cnfg, arb_observer;

static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

/*
 * `count' (1 to 8) registers from `reg' on; NULL, or what went wrong. A transfer the arbiter drops
 * now and then (the buttons' driver sees one in a thousand or so, from whatever else is on the bus)
 * is asked for again, twice at most.
 */
static const char *spmi_read_once(uint8_t reg, uint8_t *bytes, uint32_t count);

static const char *
spmi_read(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = spmi_read_once(reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static const char *
spmi_read_once(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * hap.channel;
	const char *problem;
	uint32_t data[2];

	if (!hap.found) {
		return "no channel";
	}
	lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = wait_done(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	unlock();
	if (problem != NULL) {
		return problem;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/* `count' (1 to 8) registers from `reg' on written in one transfer; NULL, or what went wrong. */
static const char *
spmi_write(uint8_t reg, const uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;
	uint32_t data[2] = { 0, 0 };

	if (!hap.found || !hap.writable || hap.write == 0) {
		return "no channel to write through";
	}
	for (uint32_t i = 0; i < count; i++) {
		data[i / 4] |= (uint32_t)bytes[i] << (8 * (i % 4));
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(hap.write + CHANNEL_WDATA0, data[0]);
		if (count > 4) {
			wr(hap.write + CHANNEL_WDATA1, data[1]);
		}
		wr(hap.write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4 | (count - 1));
		problem = wait_done(hap.write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static void
find_channel(uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != hap.id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("haptic: channel %u is %s (%#x); written by system %u%s\n", n, hap.name, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!hap.found || owner == ARB_SYSTEM) {
			hap.found = true;
			hap.channel = (uint16_t)n;
			hap.writable = owner == ARB_SYSTEM;
		}
	}
	if (!hap.found) {
		say("haptic: no channel of the arbiter is %s (slave %u, peripheral %#x)\n", hap.name, hap.id >> 8, hap.id & 0xff);
	}
}

/* ---- the haptics block -------------------------------------------------------------------------- */

#define HAP_STATUS1             0x0a            /* [3] a short circuit was seen, [1] busy */
#define HAP_SC_DET              (1u << 3)
#define HAP_BUSY                (1u << 1)
#define HAP_EN_CTL1             0x46            /* [7] the module on */
#define HAP_EN_BIT              (1u << 7)
#define HAP_EN_CTL2             0x48            /* [1] auto standby, [0] brake on */
#define HAP_BRAKE_EN            (1u << 0)
#define HAP_EN_CTL3             0x4a            /* the bridge, the limiting, the DAC */
#define HAP_AUTO_RES_CTRL       0x4b            /* [7] auto resonance on */
#define HAP_AUTO_RES_EN         (1u << 7)
#define HAP_CFG1                0x4c            /* [0] the actuator: 0 LRA, 1 ERM */
#define HAP_CFG2                0x4d            /* the LRA's signal shape: 0 sine */
#define HAP_SEL                 0x4e            /* [5:4] the waveform's source, [0] the external trigger */
#define HAP_WF_SOURCE_MASK      0x30
#define HAP_WF_SOURCE_BUFFER    0x10
#define HAP_WF_SOURCE_VMAX      0x00
#define HAP_AUTO_RES_CFG        0x4f
#define HAP_ZX_CFG              0x5a            /* [2:0] the zero-crossing filter */
#define HAP_VMAX_CFG            0x51            /* [5:1] in steps of 116 mV */
#define HAP_VMAX_MV_MASK        0x3e
#define HAP_VMAX_MV_SHIFT       1
#define HAP_VMAX_MV_LSB         116
#define HAP_ILIM_CFG            0x52            /* 0: 400 mA, 1: 800 mA */
#define HAP_RATE_CFG1           0x54            /* [11:0] the play rate, in 5 us steps */
#define HAP_RATE_CFG2           0x55
#define HAP_SC_CLR              0x59            /* [0] clear the short-circuit latch */
#define HAP_BRAKE               0x5c            /* four brake patterns, two bits each */
#define HAP_WF_REPEAT           0x5e            /* [6:4] 2^n, [1:0] 2^n */
#define HAP_WF_S1               0x60            /* eight waveform bytes: [7] sign, [5:1] amplitude */
#define HAP_WF_AMP_MASK         0x3e
#define HAP_WF_AMP_SHIFT        1
#define HAP_PLAY                0x70            /* [7] play */
#define HAP_PLAY_BIT            (1u << 7)

#define ANDROID_VMAX_MV         3000
#define ANDROID_PLAY_RATE_US    6667
#define HAPTIC_MAX_MS           1000            /* one vibration's length at most, then it rests as long */
#define TOUCH_MS                600             /* how long after a write a fault is looked for */

static const uint8_t wf_repeat_count[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
static const uint8_t wf_s_repeat_count[4] = { 1, 2, 4, 8 };

/*
 * Android's six effects, as the phone's dtbo has them (docs/raphael-haptic-flash.md): the pattern
 * Android's driver would write into the buffer, its rate, its repeats, its brake pattern.
 */
struct effect {
	const char *    name;
	uint8_t         id;             /* qcom,effect-id */
	uint8_t         pattern[8];
	uint8_t         length;
	uint16_t        rate_us;
	uint8_t         repeat_n;
	uint8_t         s_repeat_n;
	uint8_t         brake;          /* qcom,wf-brake-pattern, packed as Android packs it */
	bool            auto_res_disable;
};
static const struct effect effects[] = {
	{ "wf_0", 0, { 0x3e, 0x3e, 0x3e }, 3, 6667, 0, 0, 0x01, false },
	{ "wf_1", 1, { 0x7e, 0x7e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02 }, 8, 7143, 1, 0, 0x00, true },
	{ "wf_2", 2, { 0x7e, 0x7e }, 2, 4000, 0, 0, 0x00, true },
	{ "wf_3", 3, { 0x7e, 0x7e, 0x7e }, 3, 6667, 0, 0, 0x00, true },
	{ "wf_4", 4, { 0x7e, 0x7e }, 2, 5000, 0, 0, 0x00, true },
	{ "wf_5", 5, { 0x7e, 0x7e, 0x7e }, 3, 6667, 0, 0, 0x03, true },
};
#define EFFECTS                 (sizeof(effects) / sizeof(effects[0]))

/*
 * What Android's own kernel leaves in the block, read out of it through debugfs in OrangeFox
 * (the recovery's kernel is Android's, its qti-haptics driver is the one the phone runs):
 *
 *   c046 80    the module enabled, and left enabled
 *   c048 02    auto standby on, the brake off between effects
 *   c04a fd    bridge, PWM, DAC, limiting: all on but the zero-crossing hysteresis
 *   c04b 00    auto resonance not used (the motor is an ERM, see below)
 *   c04c 01    the actuator: ERM ("qcom,actuator-type" in the phone's dtbo is "erm", 0x65726d00)
 *   c04d 01    its signal shape (the driver does not touch this for an ERM)
 *   c04f 90  c050 01  c053 01  c056 01  c058 01  c05a 03
 *   c051 32    2900 mV: the driver's 3000 mV in steps of 116
 *   c052 00    400 mA
 *   c054 35  c055 05   6665 us: 6667 in steps of 5
 *   c070 00    not playing
 *
 * An ERM is not an LRA: there is no resonance to track, so the guidance (and the equations) of
 * qti_haptics_hw_init() for LRA -- the shape and the auto resonance registers -- do not apply,
 * and the driver returns before writing them. The first version of this file had the actuator as
 * LRA (0x00), the one value that is certainly wrong for this phone: it played nothing.
 */
static const uint8_t android_left[][2] = {
	{ HAP_EN_CTL1, 0x80 },                  /* the module on, and left on */
	{ HAP_EN_CTL2, 0x02 },                  /* auto standby on, the brake off between effects */
	{ HAP_EN_CTL3, 0xfd },                  /* bridge, PWM, DAC, limiting; no zero-crossing hysteresis */
	{ HAP_AUTO_RES_CTRL, 0x00 },            /* (an ERM: no resonance to follow) */
	{ HAP_CFG1, 0x01 },                     /* the actuator: ERM */
	{ HAP_CFG2, 0x01 },
	{ HAP_SEL, 0x00 },
	{ HAP_AUTO_RES_CFG, 0x90 },
	{ HAP_ILIM_CFG, 0x00 },                 /* 400 mA */
	{ 0x50, 0x01 },
	{ HAP_VMAX_CFG, (uint8_t)((ANDROID_VMAX_MV / HAP_VMAX_MV_LSB) << HAP_VMAX_MV_SHIFT) },
	{ 0x53, 0x01 },
	{ HAP_RATE_CFG1, (uint8_t)((ANDROID_PLAY_RATE_US / 5) & 0xff) },
	{ HAP_RATE_CFG2, (uint8_t)(((ANDROID_PLAY_RATE_US / 5) >> 8) & 0x0f) },
	{ 0x56, 0x01 },
	{ 0x58, 0x01 },
	{ HAP_ZX_CFG, 0x03 },
	{ HAP_BRAKE, 0x00 },
	{ HAP_WF_REPEAT, 0x00 },
};

static const uint8_t writable_regs[] = {
	HAP_EN_CTL1, HAP_EN_CTL2, HAP_EN_CTL3, HAP_AUTO_RES_CTRL, HAP_CFG1, HAP_CFG2, HAP_SEL,
	HAP_AUTO_RES_CFG, HAP_ZX_CFG, HAP_VMAX_CFG, HAP_ILIM_CFG, HAP_RATE_CFG1, HAP_RATE_CFG2,
	0x50, 0x53, 0x56, 0x58,
	HAP_SC_CLR, HAP_BRAKE, HAP_WF_REPEAT,
	HAP_WF_S1 + 0, HAP_WF_S1 + 1, HAP_WF_S1 + 2, HAP_WF_S1 + 3,
	HAP_WF_S1 + 4, HAP_WF_S1 + 5, HAP_WF_S1 + 6, HAP_WF_S1 + 7,
	HAP_PLAY,
};

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("haptic: %s: no more writing to the haptics block for the rest of the run\n", why);
	}
}

static bool get(uint8_t reg, uint8_t *value);
static bool put(uint8_t reg, uint8_t value);

/* only the bits of `mask' changed, as Android's masked writes do */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!get(reg, &now)) {
		return false;
	}
	if ((now & mask) == (value & mask)) {
		return true;
	}
	return put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

static bool
get(uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(reg, value, 1);

	if (problem != NULL) {
		say("haptic: reading %#04x: %s\n", reg, problem);
		give_up("a read that failed");
		return false;
	}
	return true;
}

/*
 * One register of the block (and of the list above only) written and read back: false, and no
 * more writing, if it cannot be or does not read back.
 */
static bool
put(uint8_t reg, uint8_t value)
{
	const char *problem;
	uint8_t back = 0xff;
	bool allowed = false;

	for (size_t i = 0; i < sizeof(writable_regs); i++) {
		allowed |= writable_regs[i] == reg;
	}
	if (broken || !allowed || !hap.writable || hap.write == 0 || hap.id != 0x3c0) {
		if (!allowed) {
			say("haptic: %#04x is not in the list of registers this file may write\n", reg);
		}
		give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		return false;
	}
	if (((rd(arb_core + ARB_CHANNEL(hap.channel)) >> 8) & 0xfff) != hap.id) {
		give_up("the arbiter's channel is no longer the haptics block's");
		return false;
	}
	if ((problem = spmi_write(reg, &value, 1)) != NULL || (problem = spmi_read(reg, &back, 1)) != NULL) {
		say("haptic: %#04x <- %#04x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if (back != value) {
		say("haptic: %#04x <- %#04x, but it reads %#04x\n", reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	return true;
}

/*
 * A write to the waveform buffer: it does not read back (the block answers 0 for it, p357), so
 * what is checked is the transfer itself -- the arbiter's status, and nothing else.
 */
static bool
put_blind(uint8_t reg, uint8_t value)
{
	const char *problem;
	bool allowed = false;

	for (size_t i = 0; i < sizeof(writable_regs); i++) {
		allowed |= writable_regs[i] == reg;
	}
	if (broken || !allowed || !hap.writable || hap.write == 0) {
		give_up("a write that may not be made");
		return false;
	}
	if ((problem = spmi_write(reg, &value, 1)) != NULL) {
		say("haptic: %#04x <- %#04x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	return true;
}

/*
 * Eight waveform bytes in one transfer, as Android's regmap_bulk_write does: the block's buffer
 * is a FIFO that is latched by the burst, not by single bytes (that is the difference that made
 * the first attempts silent).
 */
static bool
put_pattern(const uint8_t *pattern, uint8_t length)
{
	uint8_t bytes[8] = { 0 };
	const char *problem;

	for (uint8_t i = 0; i < length && i < 8; i++) {
		bytes[i] = pattern[i];
	}
	if (broken || !hap.writable || hap.write == 0) {
		give_up("a write that may not be made");
		return false;
	}
	if ((problem = spmi_write(HAP_WF_S1, bytes, 8)) != NULL) {
		say("haptic: the waveform buffer <- %02x %02x %02x %02x %02x %02x %02x %02x: %s\n", bytes[0], bytes[1], bytes[2],
		    bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], problem);
		give_up("a write that failed");
		return false;
	}
	return true;
}

static bool
busy(void)
{
	uint8_t status = 0;

	if (!get(HAP_STATUS1, &status)) {
		return false;
	}
	return (status & HAP_BUSY) != 0;
}

/* What would make the motor stop now, or NULL. */
static const char *
trouble(void)
{
	uint8_t status = 0;

	if (!get(HAP_STATUS1, &status)) {
		return "the block does not answer";
	}
	if (status & HAP_SC_DET) {
		uint8_t clear = 1;

		(void)put_blind(HAP_SC_CLR, clear);
		return "the block reports a short circuit";
	}
	return NULL;
}

/* ---- the effects --------------------------------------------------------------------------------- */

static uint32_t
effect_ms(const struct effect *e, uint8_t repeat_n, uint8_t s_repeat_n){
	uint32_t us = (uint32_t)e->length * e->rate_us;

	us *= wf_s_repeat_count[s_repeat_n & 3];
	us *= wf_repeat_count[repeat_n & 7];
	return us / 1000;
}

static bool
play(const struct effect *e, uint8_t repeat_n, uint8_t s_repeat_n, uint16_t vmax_mv)
{
	uint8_t vmax = (uint8_t)(((uint32_t)vmax_mv / HAP_VMAX_MV_LSB) << HAP_VMAX_MV_SHIFT);

	if (!put_bits(HAP_VMAX_CFG, HAP_VMAX_MV_MASK, vmax)) {
		return false;
	}
	if (!put(HAP_RATE_CFG1, (uint8_t)((e->rate_us / 5) & 0xff)) || !put(HAP_RATE_CFG2, (uint8_t)(((e->rate_us / 5) >> 8) & 0x0f))) {
		return false;
	}
	if (!put(HAP_BRAKE, e->brake) || !put_bits(HAP_EN_CTL2, HAP_BRAKE_EN, HAP_BRAKE_EN)) {
		return false;
	}
	if (!put_pattern(e->pattern, e->length)) {
		return false;
	}
	if (!put_blind(HAP_WF_REPEAT, (uint8_t)((repeat_n << 4) | (s_repeat_n & 3)))) {
		return false;
	}
	if (!put_blind(HAP_SEL, HAP_WF_SOURCE_BUFFER)) {
		return false;
	}
	if (!put(HAP_EN_CTL1, HAP_EN_BIT)) {
		return false;
	}
	return put_blind(HAP_PLAY, HAP_PLAY_BIT);
}

/* play stopped, the brake off, the module left on as Android leaves it: no current in the motor */
static bool
rest(void)
{
	if (!put_blind(HAP_PLAY, 0x00)) {
		return false;
	}
	spin_us(1000);
	return put_bits(HAP_EN_CTL2, HAP_BRAKE_EN, 0x00);
}

/* Android's configuration, once: every write read back, the module left off. */
static bool
configure(void)
{
	for (size_t i = 0; i < sizeof(android_left) / sizeof(android_left[0]); i++) {
		if (!put(android_left[i][0], android_left[i][1])) {
			return false;
		}
	}
	/* the buffer as Android's driver leaves it after a probe (a one-sample pattern) */
	if (!put_pattern((const uint8_t[]){ 1, 0, 0, 0, 0, 0, 0, 0 }, 8)) {
		return false;
	}
	return put_blind(HAP_PLAY, 0x00);
}

static void
show_registers(void)
{
	uint8_t r[8];
	char line[200];

	for (unsigned base = 0; base < 0x80; base += 0x20) {
		int n = snprintf(line, sizeof(line), "haptic: %#04x:", base);

		for (unsigned i = 0; i < 0x20; i += 8) {
			if (spmi_read((uint8_t)(base + i), r, 8) != NULL) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " (unreadable)");
				continue;
			}
			for (unsigned j = 0; j < 8; j++) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", r[j]);
			}
		}
		say("%s\n", line);
	}
}

/* What the block holds now, in words: the values Android's driver left and this file uses. */
static void
summarise(void)
{
	uint8_t vmax = 0, rate1 = 0, rate2 = 0, ctl1 = 0, sel = 0, play = 0, status = 0;

	if (spmi_read(HAP_VMAX_CFG, &vmax, 1) != NULL || spmi_read(HAP_RATE_CFG1, &rate1, 1) != NULL ||
	    spmi_read(HAP_RATE_CFG2, &rate2, 1) != NULL || spmi_read(HAP_EN_CTL1, &ctl1, 1) != NULL ||
	    spmi_read(HAP_SEL, &sel, 1) != NULL || spmi_read(HAP_PLAY, &play, 1) != NULL ||
	    spmi_read(HAP_STATUS1, &status, 1) != NULL) {
		say("haptic: the block's summary cannot be read\n");
		return;
	}
	say("haptic: %u mV, a play rate of %u us, the module %s, the waveform source %s, play %s, status %#x\n",
	    ((vmax & HAP_VMAX_MV_MASK) >> HAP_VMAX_MV_SHIFT) * HAP_VMAX_MV_LSB,
	    (((rate2 & 0x0f) << 8) | rate1) * 5, (ctl1 & HAP_EN_BIT) ? "on" : "off",
	    (sel & HAP_WF_SOURCE_MASK) == HAP_WF_SOURCE_BUFFER ? "the buffer" : "vmax", (play & HAP_PLAY_BIT) ? "set" : "clear",
	    status);
}

/* ---- haptic=4: one effect once -------------------------------------------------------------------- */

/* The block's other way to drive the motor: no buffer, the vmax itself for as long as it is asked. */
static bool
play_vmax(uint32_t ms, uint16_t vmax_mv)
{
	uint8_t vmax = (uint8_t)(((uint32_t)vmax_mv / HAP_VMAX_MV_LSB) << HAP_VMAX_MV_SHIFT);
	uint8_t status = 0;

	say("haptic: and the block's own waveform: vmax %u mV for %u ms (Android's way for a long vibration)\n", vmax_mv, ms);
	if (!put_bits(HAP_VMAX_CFG, HAP_VMAX_MV_MASK, vmax)) {
		return false;
	}
	if (!put_bits(HAP_EN_CTL2, HAP_BRAKE_EN, 0x00)) {
		return false;
	}
	if (!put_blind(HAP_SEL, HAP_WF_SOURCE_VMAX)) {
		return false;
	}
	if (!put(HAP_EN_CTL1, HAP_EN_BIT)) {
		return false;
	}
	if (!put_blind(HAP_PLAY, HAP_PLAY_BIT)) {
		return false;
	}
	wait_ms(2);
	(void)spmi_read(HAP_STATUS1, &status, 1);
	say("haptic: 2 ms in: status %#x\n", status);
	wait_ms(ms > 2 ? ms - 2 : 0);
	(void)spmi_read(HAP_STATUS1, &status, 1);
	say("haptic: %u ms in: status %#x, stopping\n", ms, status);
	return rest();
}

static bool
play_once(const struct effect *e, uint16_t vmax_mv)
{
	uint64_t began = mach_absolute_time(), ended;
	uint32_t ms = effect_ms(e, 0, 0);
	uint8_t s1 = 0, s2 = 0;

	say("haptic: %s is played once: %u bytes at %u us, %u ms, %u mV (Android's own)\n", e->name, e->length, e->rate_us, ms,
	    vmax_mv);
	if (!play(e, 0, 0, vmax_mv)) {
		return false;
	}
	wait_ms(2);
	(void)spmi_read(HAP_STATUS1, &s1, 1);
	say("haptic: 2 ms in: status %#x (%s)\n", s1, (s1 & HAP_BUSY) ? "busy" : "NOT busy");
	wait_ms(ms);
	(void)spmi_read(HAP_STATUS1, &s2, 1);
	(void)rest();
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ended);
	say("haptic: %u ms in: status %#x; stopped, the brake off (the whole attempt took %llu ms)\n", ms, s2,
	    ended / NSEC_PER_MSEC);
	return true;
}

/* ---- haptic=5: the motor for iOS ------------------------------------------------------------------- */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define TAP_US                  60000           /* longer than this, and it is a vibration, not a tap */
#define SETTLE_US               8000            /* how long iOS's waveform is listened to before a tap: its loudness */
#define FOLLOW_US               20000           /* how often a vibration's strength follows iOS's */

#define TOLD_EVENTS             24              /* the first so many taps and vibrations are each said */

static volatile uint64_t heard_us;              /* when the last write that was not silence came */
static volatile uint64_t quiet_us;              /* when the last write that was silence came */
static volatile uint64_t onset_us;              /* when iOS's last waveform began: the first write not silence after a pause */
static volatile uint32_t heard_peak;            /* the largest sample of that waveform, or since the service last looked */
static uint64_t written_frames;
static unsigned taps, loops, capped;            /* what has been played, for the run's end */

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();

			if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
				onset_us = now;
				heard_peak = 0;
			}
			heard_us = now;
			if (peak > heard_peak) {
				heard_peak = peak;
			}
		} else {
			quiet_us = now_us();
		}
	}
	return error;
}

/*
 * A test's stand-in for iOS's waveform (hakios.mic.c, mic_tap=1: the microphone hears the motor): as if
 * a write of this loudness (of 32767) had just come, or silence for 0. The service plays it with all its
 * own limits, as it plays iOS's.
 */
void
hakios_haptic_pretend(uint32_t peak)
{
	uint64_t now = now_us();

	if (peak <= SILENT_BELOW) {
		quiet_us = now;
		return;
	}
	if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
		onset_us = now;
		heard_peak = 0;
	}
	heard_us = now;
	if (peak > heard_peak) {
		heard_peak = peak;
	}
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * How strong to play what iOS plays. iOS drives its Taptic Engine with waveforms far under full
 * scale for most feedbacks (a tap's peak anywhere from 1000 to 33000 of 32768, a vibration's
 * about 17000), and followed in proportion (vmax = Android's * peak / full scale) most of them
 * would hardly turn the motor. Android plays its own clicks and vibrations at its vmax, 3000 mV.
 * So: the top from a third of full scale up, and below that down to five eighths of it for the
 * softest. The top is a boot-arg, haptic_vmax (default 1200 mV, a hand chose it: 336 is not felt, 1681 is too strong):
 * the phone's keyboard, whose taps peak at 6277, came out too strong even at 2521 and at 1681 mV,
 * and this is the knob for a hand to set.
 */
static uint32_t top_mv = 1200;

static uint16_t
vmax_for(uint32_t peak)
{
	uint32_t top = top_mv > ANDROID_VMAX_MV ? ANDROID_VMAX_MV : top_mv, low = top * 5 / 8, third = 32767 / 3;

	return (uint16_t)(peak >= third ? top : low + (top - low) * peak / third);
}

/* the tap of iOS: wf_3 once, Android's own short click, at the strength iOS played it */
static bool
start_tap(uint32_t peak)
{
	return play(&effects[3], 0, 0, vmax_for(peak));
}

/* a vibration of iOS's: the block's own waveform at that strength, for as long as it goes on */
static bool
start_loop(uint32_t peak)
{
	uint8_t vmax = (uint8_t)(((uint32_t)vmax_for(peak) / HAP_VMAX_MV_LSB) << HAP_VMAX_MV_SHIFT);

	if (!put_bits(HAP_VMAX_CFG, HAP_VMAX_MV_MASK, vmax)) {
		return false;
	}
	if (!put_bits(HAP_EN_CTL2, HAP_BRAKE_EN, 0x00)) {
		return false;
	}
	if (!put_blind(HAP_SEL, HAP_WF_SOURCE_VMAX)) {
		return false;
	}
	if (!put(HAP_EN_CTL1, HAP_EN_BIT)) {
		return false;
	}
	return put_blind(HAP_PLAY, HAP_PLAY_BIT);
}

static void
serve(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, last_look = 0, tap_onset = 0, followed = 0;
	uint16_t playing_vmax = 0;
	unsigned follows = 0;                   /* how often the vibration playing now has changed strength */
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("haptic: /dev/hakios-haptic cannot be made\n");
		return;
	}
	say("haptic: THE MOTOR PLAYS WHAT IOS PLAYS ON ITS ACTUATOR (/dev/hakios-haptic) from here on, until the run ends\n");
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak;
		uint16_t vmax = vmax_for(peak);

		switch (state) {
		case IDLE:
		case TAP:
			/*
			 * A waveform of iOS's that has begun (after a pause: a tap of a series is a tap of its own) is
			 * a tap -- once its first milliseconds are heard: a waveform begins softly, and they say how
			 * loud it is; one that goes on without a pause, a vibration.
			 */
			if (active && onset_us != tap_onset && now - onset_us >= SETTLE_US) {
				uint64_t began = onset_us;

				if (state == TAP) {
					(void)rest();
				}
				if (!start_tap(peak)) {
					break;
				}
				state = TAP;
				since = now;
				tap_onset = began;
				taps++;
				if (taps + loops <= TOLD_EVENTS) {
					wait_ms(2);
					say("haptic: tap %u: iOS's waveform peaks at %u of 32768: wf_3 at %u mV; the block says %#x\n", taps,
					    peak, vmax, (busy() ? HAP_BUSY : 0));
				}
				break;
			}
			if (state == IDLE) {
				break;
			}
			if (active && quiet_us < tap_onset && now - since > TAP_US) {
				(void)rest();
				if (start_loop(peak)) {
					state = LOOP;
					since = followed = now;
					playing_vmax = vmax;
					follows = 0;
					loops++;
					heard_peak = 0;
					if (taps + loops <= TOLD_EVENTS) {
						wait_ms(2);
						say("haptic: vibration %u: iOS goes on playing (peak %u): wf_3 over and over at %u mV, %u ms "
						    "at most; the block says %#x\n", loops, peak, vmax, HAPTIC_MAX_MS, (busy() ? HAP_BUSY : 0));
					}
				}
			} else if (!active && now - since > TAP_US) {
				if (rest()) {
					state = IDLE;
				}
			}
			break;
		case LOOP:
			/* as strong as iOS's waveform is now (Android's vmax at full scale, never more) */
			if (active && now - followed >= FOLLOW_US) {
				followed = now;
				heard_peak = 0;
				if (peak != 0 && (vmax > playing_vmax + 100 || vmax + 100 < playing_vmax)) {
					uint8_t bits = (uint8_t)(((uint32_t)vmax / HAP_VMAX_MV_LSB) << HAP_VMAX_MV_SHIFT);

					if (!put_bits(HAP_VMAX_CFG, HAP_VMAX_MV_MASK, bits)) {
						break;
					}
					playing_vmax = vmax;
					follows++;
				}
			}
			if (!active || now - since > HAPTIC_MAX_MS * 1000ull) {
				bool cut = now - since > HAPTIC_MAX_MS * 1000ull;

				if (cut) {
					capped++;
					rest_until = now + HAPTIC_MAX_MS * 1000ull;
				}
				if (rest()) {
					state = rest_until > now ? REST : IDLE;
					if (taps + loops <= TOLD_EVENTS + 1) {
						say("haptic: vibration %u: %s after %llu ms (its strength changed %u times, last to %u mV); "
						    "the module is off\n", loops, cut ? "CUT at the limit" : "iOS stopped, and so does the motor",
						    (now - since) / 1000, follows, playing_vmax);
					}
				}
			}
			break;
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		if (state != IDLE && state != REST && now - last_look > 50000) {
			last_look = now;
			{
				const char *why = trouble();

				if (why != NULL) {
					say("haptic: while playing: %s\n", why);
					give_up("trouble while playing");
					break;
				}
			}
		}
		wait_ms(4);
	}
	if (state != IDLE) {
		(void)rest();
	}
	say("haptic: the motor is given up on (%u taps and %u vibrations played, %u cut at %u ms)\n", taps, loops, capped,
	    HAPTIC_MAX_MS);
}

/* ---- the start ------------------------------------------------------------------------------------- */

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t version, channels;
	uint8_t id[2] = { 0, 0 }, revision = 0, status = 0;

	say("haptic: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "what the block holds" :
	    level == 3 ? "Android's configuration written, nothing played" : level == 4 ? "one effect played" :
	    "the motor for iOS");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("haptic: PMIC arbiter version %#x, not 5: nothing done\n", version);
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	find_channel(channels);
	if (!hap.found || spmi_read(0x04, id, 2) != NULL || spmi_read(0x00, &revision, 1) != NULL || spmi_read(HAP_STATUS1, &status, 1) != NULL) {
		say("haptic: the haptics block cannot be read (the whole bus answers %s)\n", hap.found ? "partly" : "nowhere");
		goto out;
	}
	say("haptic: the block says type %#x, subtype %#x, revision %#x, status %#x%s\n", id[0], id[1], revision, status,
	    (status & (HAP_BUSY | HAP_SC_DET)) == 0 ? ": idle, nothing wrong" : ": NOT idle");
	show_registers();
	if (level < 2) {
		goto out;
	}
	summarise();
	if (level < 3 || !hap.writable) {
		if (!hap.writable) {
			say("haptic: the channel is not ours to write through: nothing written\n");
		}
		goto out;
	}
	hap.write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * hap.channel, 0x1000);
	if (!configure()) {
		goto out;
	}
	say("haptic: Android's configuration is written and reads back (nothing played, nothing moves)\n");
	show_registers();
	summarise();
	if (level == 4) {
		uint32_t which = 0;

		PE_parse_boot_argn("haptic_effect", &which, sizeof(which));
		if (which == 0) {
			/* all six, one after another, so that a hand can say which of them moves the motor */
			for (size_t i = 0; i < EFFECTS && !broken; i++) {
				say("haptic: %u of %u: %s\n", (unsigned)i + 1, (unsigned)EFFECTS, effects[i].name);
				(void)play_once(&effects[i], ANDROID_VMAX_MV);
				wait_ms(700);
			}
		} else {
			if (which > EFFECTS) {
				which = 1;
			}
			(void)play_once(&effects[which - 1], ANDROID_VMAX_MV);
		}
		(void)play_vmax(500, ANDROID_VMAX_MV);
	} else if (level == 5 && !broken) {
		/*
		 * A ladder for a hand (boot-arg haptic_ladder=1): pairs of taps at 800, 1100, 1400 and
		 * 1700 mV with pauses, the keyboard's waveform (wf_3) exactly, so that the strength of a
		 * real tap can be chosen in one run instead of one run per number. Without it, one tap at
		 * what the keyboard will get, and then the service.
		 */
		static const uint16_t ladder[] = { 800, 1100, 1400, 1700 };
		uint32_t asked = 0;

		PE_parse_boot_argn("haptic_ladder", &asked, sizeof(asked));
		if (asked) {
			say("haptic: a ladder: pairs of taps at 800, 1100, 1400, 1700 mV (2 s apart)\n");
			for (size_t l = 0; l < sizeof(ladder) / sizeof(ladder[0]) && !broken; l++) {
				say("haptic: ladder: %u mV, twice\n", ladder[l]);
				for (unsigned i = 0; i < 2 && !broken; i++) {
					(void)play_once(&effects[3], ladder[l]);
					wait_ms(400);
				}
				wait_ms(1600);
			}
		} else {
			say("haptic: one tap at the keyboard's strength (%u mV, with the top at %u) before the service\n",
			    vmax_for(6277), top_mv);
			(void)play_once(&effects[3], vmax_for(6277));
		}
		serve();
	}
out:
	say("haptic: done\n");
	thread_terminate(current_thread());
}

void
hakios_haptic_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 5) {
		level = 5;
	}
	PE_parse_boot_argn("haptic_vmax", &top_mv, sizeof(top_mv));
	if (top_mv < 400) {
		top_mv = 400;
	}
	if (top_mv > ANDROID_VMAX_MV) {
		top_mv = ANDROID_VMAX_MV;
	}
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the module off, so no current is left in the motor. */
void
hakios_haptic_quiesce(void)
{
	if (level != 0 && hap.write != 0) {
		leaving = true;
		if (!broken) {
			(void)spmi_write(HAP_PLAY, (const uint8_t[]){ 0 }, 1);
			(void)spmi_write(HAP_EN_CTL1, (const uint8_t[]){ 0 }, 1);
		}
		if (level == 5) {
			say("haptic: at the run's end: %u taps and %u vibrations played (%u cut at %u ms), from %llu frames iOS "
			    "wrote; the module is off\n", taps, loops, capped, HAPTIC_MAX_MS, written_frames);
		}
	}
}
