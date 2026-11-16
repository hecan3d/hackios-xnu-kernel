/*
 * The flashlight of the Xiaomi Mi 9T Pro (raphael): the flash LED block of the PM8150L -- SPMI
 * slave 5, peripheral 0xd3, registers 0xd300..0xd3ff ("qcom,pm8150l@5/qcom,leds@d300",
 * compatible "qcom,qpnp-flash-led-v2"). Every address and number below is the phone's own, from
 * Android's device tree (dtbo, dumped through the loader; docs/raphael-haptic-flash.md) and
 * Qualcomm's driver for the block (drivers/leds/leds-qpnp-flash-v2.c):
 *
 *   the module    one flash block, three channels: flash_0/flash_1 (max 1500 mA, Android's 1000)
 *                 and flash_2 (750/500); the two torches torch_0/torch_1 (max 500 mA, Android's
 *                 300); a current resolution of 12.5 mA ("qcom,ires-ua")
 *   the headroom  325 mV over the LED's forward voltage, a 100 mV window
 *   the torch     Android's flashlight (its CameraFlash HAL) switches torch_0 and torch_1 through
 *                 led_switch_2 -- both channels, mask 3: IRES 12.5 mA, ITARGET round(300/12.5)-1 =
 *                 23, the software strobe, the safety timer off (0x40 = 0x13, as the driver
 *                 leaves it), then the module (0x46 = 0x80), then both channels (0x4c = 0x03)
 *
 * Writing to a PMIC is not like writing anywhere else: the PMICs make the phone's supplies. So
 * every write goes through one function that takes only this module's registers above (the
 * peripheral the arbiter's channel is checked to be, the registers by a list), every write is
 * read back, and any surprise ends all writing for the run. Transfers through the arbiter are made
 * one at a time with the buttons' driver (hakios_spmi_lock()): two at once, and it drops one.
 *
 * What iOS's flashlight is on this machine: the loader says the camera has a flash (with torch=N,
 * /product/camera "flash": MobileGestalt's camera-flash, Control Center's flashlight button), and
 * libhakios.media.dylib in mediaserverd hands FigFlashlight, which finds no camera here, a device
 * of ours whose torch level is written to /dev/hakios-torch (userland/src/inject_media.c).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   torch=1    a look, reading only: the arbiter's channel for the block and who may write
 *              through it, the block's type, its registers
 *   torch=2    ... and the light on once at Android's torch current for torch_secs (default 2),
 *              then off, every write read back
 *   torch=3    ... no light at the start; the light for iOS until the run ends: /dev/hakios-torch
 *              takes a level of 0 to 1000 (a 32-bit number; thousandths of Android's current, 0
 *              off) and the block is set to it -- never above Android's 300 mA; a read gives
 *              what is lit now
 *
 * Limits, whatever asks: Android's current at most; the light on for torch_max_s at a time at most
 * (default 30, never more than 120), after which it stays off until it is asked for anew (0, then
 * more); while it is on, ten times a second, the block's fault and over-temperature flags and the
 * battery's temperature (at most 45 C; from the audio DSP's battery manager with adsp=1, or the
 * firmware's with battery=1; with neither the other two guard): any of them, a read that fails or a
 * write that does not read back, and the light goes off and stays off for the rest of the run. At
 * the run's end (and on a panic's way out) it goes off.
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (arbiter version 5), Qualcomm's
 * leds-qpnp-flash-v2.c, drivers/thermal/qcom/qcom-spmi-temp-alarm.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level;
static bool broken, leaving;

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well (and to the console when that may be written) */
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

/*
 * The events that matter (lit, off, cut, trouble) are also kept, and said again at the run's end:
 * the log of a long run keeps its beginning and its end, and these happen in between.
 */
#define NOTES                   24
static char notes[NOTES][200];
static unsigned noted;

static void note(const char *format, ...) __printflike(1, 2);

static void
note(const char *format, ...)
{
	char *slot = notes[noted++ % NOTES];
	uint64_t ns;
	va_list list;
	int n;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	n = snprintf(slot, sizeof(notes[0]), "%llu.%03llu s: ", ns / NSEC_PER_SEC, ns / NSEC_PER_MSEC % 1000);
	va_start(list, format);
	vsnprintf(slot + n, sizeof(notes[0]) - (size_t)n, format, list);
	va_end(list);
	say("torch: %s\n", slot);
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
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
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

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral */
	uint16_t        channel;
	bool            found, writable;
	vm_offset_t     write;          /* its write channel, mapped (only the flash module's) */
};
static struct peripheral flash = { .name = "the PM8150L's flash LED block", .id = 0x5d3 };

static vm_offset_t arb_core, arb_cnfg, arb_observer;

/* (the arbiter one transfer at a time with the buttons' driver; at the run's end the others are stopped, perhaps holding it) */
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
static const char *spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count);

static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = spmi_read_once(p, reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static const char *
spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	const char *problem;
	uint32_t data[2];

	if (!p->found) {
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

/* ---- the flash LED module -------------------------------------------------------------------------- */

#define FLASH_REVISION1         0x00
#define FLASH_TYPE              0x04            /* 0x18: a flash LED block */
#define FLASH_SUBTYPE           0x05
#define FLASH_STATUS1           0x08            /* [2:0] a channel open or shorted */
#define FLASH_STATUS2           0x09            /* [4] the supply drooped, [2:0] over-temperature */
#define  STATUS2_VPH_LOW        0x10
#define  STATUS2_OTST_MASK      0x07
#define FLASH_INT_RT_STS        0x10
#define  RT_STS_FAULT           0x01            /* open, short, or its supply not ready */
#define FLASH_SAFETY_TIMER(id)  (0x40 + (id))   /* [7] the timer on, the rest of it the time */
#define  SAFETY_TIMER_OFF       0x13            /* (the driver's "off", as Android leaves it) */
#define FLASH_TGR_CURRENT(id)   (0x43 + (id))   /* [6:0]: (code + 1) x the resolution */
#define FLASH_MODULE_EN         0x46            /* [7] */
#define FLASH_IRESOLUTION       0x47            /* two bits per channel: 0 12.5 mA, 3 5 mA */
#define FLASH_STROBE_CFG        0x48
#define FLASH_STROBE_CTRL(id)   (0x49 + (id))   /* [2] the hardware strobe, [1:0] its kind */
#define  STROBE_SOFTWARE        0x00
#define FLASH_EN_LED_CTRL       0x4c            /* a bit per channel */
#define FLASH_HDRM_PROGRAM(id)  (0x4d + (id))   /* [7:4] (mV - 125) / 25, [3:0] the window / 25 */
#define FLASH_MULTI_STROBE      0x71            /* [1:0] a flash once only, per channel */

#define ANDROID_TORCH_MA        300             /* led:torch_0 and 1 while Android's flashlight is on */
#define IRES_UA                 12500           /* their resolution ("qcom,ires-ua") */
#define ANDROID_ITARGET         23              /* round(300000 / 12500) - 1: the most ever written */
#define CHANNELS                0x03            /* torch_0 and torch_1 (led_switch_2's mask) */
#define HOTTEST_BATTERY_C       45

static const uint8_t writable_regs[] = {
	FLASH_SAFETY_TIMER(0), FLASH_SAFETY_TIMER(1), FLASH_SAFETY_TIMER(2),
	FLASH_TGR_CURRENT(0), FLASH_TGR_CURRENT(1),
	FLASH_MODULE_EN, FLASH_IRESOLUTION, FLASH_STROBE_CFG,
	FLASH_STROBE_CTRL(0), FLASH_STROBE_CTRL(1),
	FLASH_EN_LED_CTRL, FLASH_HDRM_PROGRAM(0), FLASH_HDRM_PROGRAM(1), FLASH_MULTI_STROBE,
};

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		note("%s: no more writing to the flash module for the rest of the run", why);
	}
}

/*
 * One register of the flash module (and of the six above only) written and read back: false, and
 * no more writing, if it cannot be or does not read back.
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
	if (broken || !allowed || !flash.writable || flash.write == 0 || flash.id != 0x5d3) {
		give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		return false;
	}
	/* (the channel is still the module's: its entry in the arbiter's table is looked at again) */
	if (((rd(arb_core + ARB_CHANNEL(flash.channel)) >> 8) & 0xfff) != flash.id) {
		give_up("the arbiter's channel is no longer the flash module's");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(flash.write + CHANNEL_WDATA0, value);
		wr(flash.write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(flash.write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;          /* (a dropped write has not been made: asked for again, then read back) */
		}
	}
	if (problem != NULL || (problem = spmi_read(&flash, reg, &back, 1)) != NULL) {
		say("torch: %#04x <- %#04x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if (back != value) {
		say("torch: %#04x <- %#04x, but it reads %#04x\n", reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	return true;
}

static bool
get(const struct peripheral *p, uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(p, reg, value, 1);

	if (problem != NULL) {
		say("torch: reading %#04x of %s: %s\n", reg, p->name, problem);
		return false;
	}
	return true;
}

/* only the bits of `mask' changed, as the driver's masked writes do */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!get(&flash, reg, &now)) {
		give_up("a read that failed");
		return false;
	}
	return (now & mask) == (value & mask) || put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

static void
find_channel(struct peripheral *p, uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != p->id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("torch: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
			p->writable = owner == ARB_SYSTEM;
		}
	}
	if (!p->found) {
		say("torch: no channel of the arbiter is %s\n", p->name);
	}
}

static void
show_registers(void)
{
	uint8_t r[8];
	char line[200];

	for (unsigned base = 0; base < 0x100; base += 0x20) {
		int n = snprintf(line, sizeof(line), "torch: %#04x:", base);

		for (unsigned i = 0; i < 0x20; i += 8) {
			if (spmi_read(&flash, (uint8_t)(base + i), r, 8) != NULL) {
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

/* What would make the light go off now, or NULL. */
static const char *
trouble(void)
{
	static char why[180];
	uint8_t rt = 0, status1 = 0, status2 = 0;

	if (!get(&flash, FLASH_INT_RT_STS, &rt) || !get(&flash, FLASH_STATUS1, &status1) ||
	    !get(&flash, FLASH_STATUS2, &status2)) {
		return "a read of the flags that failed";
	}
	if ((rt & RT_STS_FAULT) || status1 != 0 || (status2 & (STATUS2_OTST_MASK | STATUS2_VPH_LOW))) {
		snprintf(why, sizeof(why), "the block says interrupt %#x, channels %#x, supply and temperature %#x", rt, status1,
		    status2);
		return why;
	}
	if (hakios_battery_temperature() >= HOTTEST_BATTERY_C) {
		snprintf(why, sizeof(why), "the battery is at %d C", hakios_battery_temperature());
		return why;
	}
	return NULL;
}

static bool configured, lit;

/* Android's probe, once: the headroom, the safety timer off, the software strobe, no hardware one. */
static bool
configure(void)
{
	if (configured) {
		return true;
	}
	if (!put(FLASH_HDRM_PROGRAM(0), 0x84) || !put(FLASH_HDRM_PROGRAM(1), 0x84)) {
		return false;
	}
	if (!put(FLASH_SAFETY_TIMER(0), SAFETY_TIMER_OFF) || !put(FLASH_SAFETY_TIMER(1), SAFETY_TIMER_OFF)) {
		return false;
	}
	if (!put_bits(FLASH_STROBE_CTRL(0), 0x07, STROBE_SOFTWARE) || !put_bits(FLASH_STROBE_CTRL(1), 0x07, STROBE_SOFTWARE)) {
		return false;
	}
	if (!put_bits(FLASH_MULTI_STROBE, 0x03, 0x00)) {
		return false;
	}
	configured = true;
	return true;
}

/* The light at `permille' of Android's current: its writes in its order (both torch channels). */
static bool
light(uint32_t permille)
{
	uint32_t ua = ANDROID_TORCH_MA * 1000 * permille / 1000, code;
	const char *why;

	if (ua < IRES_UA) {
		ua = IRES_UA;           /* (the driver's least: one step) */
	}
	code = (ua + IRES_UA / 2) / IRES_UA - 1;
	if (code > ANDROID_ITARGET) {
		code = ANDROID_ITARGET;
	}
	if (!lit && (why = trouble()) != NULL) {
		note("not lit: %s", why);
		give_up("trouble before the light");
		return false;
	}
	if (!configure() || !put(FLASH_IRESOLUTION, 0x00)) {
		return false;
	}
	if (!put(FLASH_TGR_CURRENT(0), (uint8_t)code) || !put(FLASH_TGR_CURRENT(1), (uint8_t)code)) {
		return false;
	}
	if (!lit) {
		if (!put_bits(FLASH_STROBE_CTRL(0), 0x07, STROBE_SOFTWARE) || !put_bits(FLASH_STROBE_CTRL(1), 0x07, STROBE_SOFTWARE)) {
			return false;
		}
		if (!put(FLASH_MODULE_EN, 0x80)) {
			return false;
		}
	}
	lit = true;
	return put_bits(FLASH_EN_LED_CTRL, CHANNELS, CHANNELS);
}

/* Off, in Android's order: the channels, the module, the target current, the timer's bit. */
static bool
dark(void)
{
	bool ok = put_bits(FLASH_EN_LED_CTRL, CHANNELS, 0x00) && put(FLASH_MODULE_EN, 0x00) &&
	    put(FLASH_TGR_CURRENT(0), 0x00) && put(FLASH_TGR_CURRENT(1), 0x00) &&
	    put(FLASH_SAFETY_TIMER(0), SAFETY_TIMER_OFF) && put(FLASH_SAFETY_TIMER(1), SAFETY_TIMER_OFF);

	lit = !ok && lit;
	return ok;
}

/* ---- torch=3: the light for iOS --------------------------------------------------------------------- */

static volatile uint32_t wanted;                /* the level asked for last, 0 to 1000 */
static volatile uint32_t asked;                 /* how many times one was */
static uint32_t max_secs = 30;

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value;
	int error;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
			return error;
		}
		wanted = value > 1000 ? 1000 : value;
		asked++;
	}
	return 0;
}

/* what is lit now: the level, 0 if nothing */
static uint32_t shown;

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = shown;

	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
serve(void)
{
	uint32_t applied = 0, times = 0, cut = 0;
	uint64_t since = 0, last_look = 0;
	bool waiting_for_off = false;           /* cut at the limit: dark until iOS asks for 0 */
	int major = cdevsw_add(-1, &torch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-torch") == NULL) {
		say("torch: /dev/hakios-torch cannot be made\n");
		return;
	}
	say("torch: THE LIGHT IS IOS'S (/dev/hakios-torch) from here on, until the run ends: at most Android's %u mA, at most %u s "
	    "at a time\n", ANDROID_TORCH_MA, max_secs);
	while (!broken) {
		uint32_t want = wanted;
		uint64_t now = now_ms();

		if (want == 0) {
			waiting_for_off = false;
		}
		if (waiting_for_off) {
			want = 0;
		}
		if (want != applied) {
			if (want == 0 ? dark() : light(want)) {
				if (applied == 0) {
					since = now;
					times++;
				}
				if (want == 0) {
					note("off (iOS asked for 0)");
				} else {
					note("ON (iOS asked for %u of 1000; the battery %d C)", wanted, hakios_battery_temperature());
				}
				applied = want;
				shown = want;
			} else {
				break;
			}
		}
		if (applied != 0 && now - since >= max_secs * 1000ull) {
			cut++;
			note("on for %u s, the limit: off until iOS asks again", max_secs);
			waiting_for_off = true;
			if (!dark()) {
				break;
			}
			applied = 0;
			shown = 0;
			continue;
		}
		if (applied != 0 && now - last_look >= 100) {
			const char *why;

			last_look = now;
			if ((why = trouble()) != NULL) {
				note("OFF: %s", why);
				(void)dark();
				give_up("trouble while lit");
				break;
			}
		}
		wait_ms(20);
	}
	if (lit) {
		(void)dark();
	}
	shown = 0;
	say("torch: the light is given up on (%u times lit, %u cut at %u s)\n", times, cut, max_secs);
}

/* ---- the start ------------------------------------------------------------------------------------- */

static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t version, channels;
	uint8_t id[2] = { 0, 0 }, revision = 0;

	say("torch: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the light on once" :
	    "the light for iOS");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("torch: PMIC arbiter version %#x, not 5: nothing done\n", version);
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	find_channel(&flash, channels);
	if (!flash.found || spmi_read(&flash, FLASH_TYPE, id, 2) != NULL || spmi_read(&flash, FLASH_REVISION1, &revision, 1) != NULL) {
		say("torch: the flash module cannot be read\n");
		goto out;
	}
	say("torch: the block says type %#x, subtype %#x, revision %#x%s\n", id[0], id[1], revision,
	    id[0] == 0x18 ? ": a flash LED block, as Android's driver expects" : ": NOT WHAT WAS EXPECTED");
	show_registers();
	if (level < 2) {
		goto out;
	}
	if (!flash.writable) {
		say("torch: the channel is not ours to write through: nothing written\n");
		goto out;
	}
	flash.write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * flash.channel, 0x1000);
	PE_parse_boot_argn("torch_max_s", &max_secs, sizeof(max_secs));
	max_secs = max_secs < 1 ? 1 : max_secs > 120 ? 120 : max_secs;
	if (level == 2) {
		uint32_t secs = 2;
		uint64_t until;
		const char *why = NULL;

		PE_parse_boot_argn("torch_secs", &secs, sizeof(secs));
		secs = secs < 1 ? 1 : secs > max_secs ? max_secs : secs;
		say("torch: the light ON at Android's %u mA (ITARGET %u, 12.5 mA steps) for %u s\n", ANDROID_TORCH_MA, ANDROID_ITARGET,
		    secs);
		if (light(1000)) {
			uint8_t rt = 0, s1 = 0, s2 = 0;

			wait_ms(50);
			(void)get(&flash, FLASH_INT_RT_STS, &rt);
			(void)get(&flash, FLASH_STATUS1, &s1);
			(void)get(&flash, FLASH_STATUS2, &s2);
			note("lit at Android's %u mA; 50 ms on: interrupt status %#x (%s), channels %#x, temperature and supply %#x",
			    ANDROID_TORCH_MA, rt, (rt & 0x10) ? "ramped up" : "NOT ramped up", s1, s2);
			show_registers();
			for (until = now_ms() + secs * 1000ull; now_ms() < until && (why = trouble()) == NULL;) {
				wait_ms(100);
			}
			if (why != NULL) {
				note("OFF early: %s", why);
			}
			if (dark()) {
				note("off again after %u s", secs);
			}
			show_registers();
		}
	} else {
		serve();
	}
out:
	say("torch: done\n");
	thread_terminate(current_thread());
}

void
hakios_torch_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("torch", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 3 ? 3 : level;
	if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the light off, if it was ever lit. */
void
hakios_torch_quiesce(void)
{
	uint8_t module = 0xff, channels = 0xff, target = 0xff;

	if (level < 2 || flash.write == 0) {
		return;
	}
	leaving = true;
	if (lit) {
		broken = false;         /* (one last time, whatever went before) */
		say("torch: at the run's end: the light %s\n", dark() ? "switched off" : "COULD NOT BE SWITCHED OFF");
	}
	say("torch: what happened in this run (%u events):\n", noted);
	for (unsigned i = noted > NOTES ? noted - NOTES : 0; i < noted; i++) {
		say("torch:   %s\n", notes[i % NOTES]);
	}
	(void)spmi_read(&flash, FLASH_MODULE_EN, &module, 1);
	(void)spmi_read(&flash, FLASH_EN_LED_CTRL, &channels, 1);
	(void)spmi_read(&flash, FLASH_TGR_CURRENT(0), &target, 1);
	say("torch: at the run's end the module reads: enable %#04x, channels %#04x, target %#04x%s\n", module, channels, target,
	    module == 0 && (channels & CHANNELS) == 0 && target == 0 ? " (dark)" : " (NOT DARK)");
}
