/*
 * libhakios.sb.dylib: code of ours inside SpringBoard (see inject.c for how it gets there).
 *
 *   - The phone is "activated". SpringBoard asks mobileactivationd, which is not running (and
 *     could not talk to Apple anyway); without an answer it treats the device as bricked and
 *     shows the first-boot setup ("hello") instead of the lock screen.
 *   - The setup assistant is never needed, whatever the reason: nobody is going to go through
 *     it here.
 *   - SpringBoard starts a few seconds late where the kernel's boot-args say so (sb_delay=N;
 *     "xl run" does for the phone). /private/var is empty at every boot, so the applications
 *     are registered anew each time, one after another; one that is registered after
 *     SpringBoard has laid out the home screen ends up on a later page, and which one that is
 *     differs from boot to boot. On the phone that happens; under QEMU, where everything takes
 *     five times as long, it does not -- and there the delay is harmful: for a reason not yet
 *     found the window server then never takes our touch screen service for its own.
 *     (A wrapper program in the launchd job would be the obvious way to wait; launchd panics
 *     the system when a system job runs anything that is not Apple's.)
 *   - The picture SpringBoard keeps of an application that leaves the screen (for the application
 *     switcher and the next launch) is saved as JPEG or PNG, not as an ASTC texture: see
 *     snapshot_output_format().
 *   - Where CoreAnimation draws with its software renderer (QEMU; the phone without gpu=1) the
 *     "vibrant" views draw their content as it is: see plain_vibrancy().
 *   - A press of the side button that is long over is not taken for a long press (Siri) because
 *     SpringBoard was busy waking the display: see lock_long_press().
 *   - A long press on the lock screen's wallpaper does not open the poster switcher, which the
 *     window server here shows as a black screen: see poster_switcher_allowed().
 *   - Darwin notifications as a remote control, for scripts (rwtouch notify NAME):
 *       com.hakios.unlock      what the lock screen does after "swipe up to open"
 *       com.hakios.layers      write down the layers SpringBoard has on the screen: see layers()
 *       com.hakios.switcher    open the switcher of running applications: see switcher()
 *     In any other program given this library (INJECT in build.sh) only the last of them is
 *     answered, with that program's layers.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <os/lock.h>
#include <pthread.h>
#include <mach/mach_time.h>
typedef void *xpc_connection_t_sb;
typedef void *xpc_object_t_sb;
typedef void (^xpc_handler_t_sb)(xpc_object_t_sb);
extern const char *xpc_connection_get_name(xpc_connection_t_sb connection);
extern xpc_object_t_sb xpc_connection_send_message_with_reply_sync(xpc_connection_t_sb connection, xpc_object_t_sb message);
extern void xpc_connection_send_message_with_reply(xpc_connection_t_sb connection, xpc_object_t_sb message,
    dispatch_queue_t replyq, xpc_handler_t_sb handler);
extern xpc_connection_t_sb xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags);
extern xpc_connection_t_sb xpc_connection_create(const char *name, dispatch_queue_t targetq);
extern int xpc_connection_get_pid(xpc_connection_t_sb connection);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
extern char *xpc_copy_description(xpc_object_t_sb object);
extern xpc_object_t_sb xpc_dictionary_get_value(xpc_object_t_sb object, const char *key);
extern const void *xpc_data_get_bytes_ptr(xpc_object_t_sb object);
extern size_t xpc_data_get_length(xpc_object_t_sb object);
static void say_graph(const char *what, xpc_object_t_sb message);
static void say_apfs_stats(const char *when);
extern xpc_connection_t_sb xpc_connection_create_from_endpoint(xpc_object_t_sb endpoint);
extern void xpc_connection_send_message(xpc_connection_t_sb connection, xpc_object_t_sb message);
extern void xpc_connection_set_event_handler(xpc_connection_t_sb connection, xpc_handler_t_sb handler);
extern void xpc_connection_resume(xpc_connection_t_sb connection);
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/mman.h>

/* where what is said goes: the standard error, which is the console's for SpringBoard (an application's is nowhere) */
static int say_fd = 2;

/* Boot-arg sb_loud=N: how many XPC requests are written down with their reply and time
 * (the first 250 by default). */
static unsigned xpc_loud_limit = 250;

static void
say(const char *format, ...)
{
	char line[1280];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(say_fd, line, (size_t)n);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);
#define CACHE_BASE 0x180000000UL
static uintptr_t slide;
#include "collection_safety.h"

/* The names of what a trap came through: unslid addresses are what the log prints, dladdr wants the
 * ones the process is running at.  A "signal 5" in SpringBoard (p402: 27 times in a row, while a
 * ScreenTime widget extension was being launched) says nothing until its frames have names. */
static void
say_fault_near(const char *what, uintptr_t address)
{
	Dl_info info;

	if (address == 0 || dladdr((void *)address, &info) == 0) {
		return;
	}
	if (info.dli_sname != NULL) {
		say("hakios: fault   %s near %s+%lu\n", what, info.dli_sname,
		    (unsigned long)(address - (uintptr_t)info.dli_saddr));
	} else if (info.dli_fname != NULL) {
		say("hakios: fault   %s in %s+%#lx\n", what, info.dli_fname,
		    (unsigned long)(address - (uintptr_t)info.dli_fbase));
	}
}

static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
	}
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;

	uintptr_t frame_addresses[8];
	unsigned frame_count = 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		frame_addresses[frame_count++] = ret;
		fp = next > fp ? next : 0;
	}
	say("hakios: %s[%d]: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n",
	    getprogname(), getpid(), signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	say_fault_near("pc", pc);
	say_fault_near("lr", lr);
	for (unsigned i = 0; i < frame_count; i++) {
		say_fault_near("from", frame_addresses[i]);
	}
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

void
__assert_rtn(const char *func, const char *file, int line, const char *expr)
{
	say("hakios: %s[%d]: ASSERT FAILED: %s (%s:%d in %s)\n",
	    getprogname(), getpid(), expr ? expr : "?", file ? file : "?", line, func ? func : "?");
	abort();
}

void
abort(void)
{
	uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
	char frames[8 * 20 + 1] = "";
	size_t used = 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;
	}
	say("hakios: %s[%d]: abort() called from %p (unslid %#lx), stack:%s\n",
	    getprogname(), getpid(), __builtin_return_address(0),
	    (uintptr_t)__builtin_return_address(0) - slide, frames);
	signal(SIGABRT, SIG_DFL);
	raise(SIGABRT);
	for (;;) {}
}

static void
hakios_exception_handler(id exception)
{
	Class cls = object_getClass(exception);
	SEL sel_name = sel_registerName("name");
	SEL sel_reason = sel_registerName("reason");
	Method m_name = cls ? class_getInstanceMethod(cls, sel_name) : NULL;
	Method m_reason = cls ? class_getInstanceMethod(cls, sel_reason) : NULL;
	id name = (m_name && exception) ? ((id (*)(id, SEL))method_getImplementation(m_name))(exception, sel_name) : NULL;
	id reason = (m_reason && exception) ? ((id (*)(id, SEL))method_getImplementation(m_reason))(exception, sel_reason) : NULL;
	const char *c_name = name ? ((const char *(*)(id, SEL))class_getMethodImplementation(object_getClass(name), sel_registerName("UTF8String")))(name, sel_registerName("UTF8String")) : "?";
	const char *c_reason = reason ? ((const char *(*)(id, SEL))class_getMethodImplementation(object_getClass(reason), sel_registerName("UTF8String")))(reason, sel_registerName("UTF8String")) : "?";

	say("hakios: %s[%d]: UNCAUGHT EXCEPTION: %s: %s\n", getprogname(), getpid(), c_name, c_reason);
}

/*
 * Boot-arg touch_lat=1: a line for every turn of SpringBoard's main run loop -- when it woke up (a
 * touch, a timer, the display link ...), when CoreAnimation's commit of what that turn changed
 * began and when it was done (our observers just before and after its own, of order 2000000), in
 * microseconds of mach_absolute_time, as the other "tl:" lines (tools/latency.py, tools/frames.py):
 *   tl: a WOKE COMMIT DONE
 * (a commit with nothing in it takes some microseconds; one that sends changes, a good deal more)
 */
#define CA_COMMIT_ORDER 2000000
static uint64_t main_woke, commit_began;
static mach_timebase_info_data_t timebase;
static int turns_logged = 1;            /* the "tl: a" line per turn (touch_lat=1; stall_lat=1 leaves it out) */
static thread_t main_thread_port;       /* the main thread, for the watchdog (watch_main_loop) */
static volatile int stuck_level;        /* how far into the turn the last sample was taken: 1 = 250 ms, 2 = 1 s, 3 = 4 s */

static uint64_t
now_us(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
}

static void
main_loop_turn(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
	if (activity == kCFRunLoopAfterWaiting) {
		main_woke = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && info == NULL) {
		commit_began = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && main_woke != 0) {
		if (turns_logged) {
			say("tl: a %llu %llu %llu\n", main_woke, commit_began, now_us());
		}
		main_woke = 0;
	}
}

/* The watchdog's thread: a turn of the main run loop that lasts says nothing about what it waits
 * for, so the main thread is sampled while it lasts, once at each of 250 ms, 1 s and 4 s. */
static void sample_one(unsigned index, thread_t thread);
static void sample_threads(int seconds);

static void *
main_stuck_watch(void *unused)
{
	(void)unused;
	for (;;) {
		uint64_t woke, ms;
		int level;

		usleep(100000);
		woke = main_woke;
		if (woke == 0 || main_thread_port == MACH_PORT_NULL) {
			stuck_level = 0;
			continue;
		}
		ms = (now_us() - woke) / 1000;
		level = ms >= 4000 ? 3 : (ms >= 1000 ? 2 : (ms >= 250 ? 1 : 0));
		if (level > stuck_level) {
			stuck_level = level;
			say("hakios: stuck %llu ms in one turn of the main run loop\n", (unsigned long long)ms);
			sample_one(0, main_thread_port);
			/*
			 * A second in __psynch_mutexwait says the main thread waits for a lock, and nothing
			 * about who holds it: on the seconds-long ones every other thread is sampled too, so
			 * the holder's stack is in the log next to the waiter's.
			 */
			if (level >= 2) {
				say("hakios: the other threads, while the main one waits\n");
				sample_threads((int)(ms / 1000));
			}
		}
	}
	return NULL;
}


static void
watch_main_loop(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *lat, *stall;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return;
	}
	/*
	 * touch_lat=1 writes a line for every turn and every frame; on a 1 MB kernel ring that is
	 * minutes of log and nothing else.  stall_lat=1 keeps the watchdog and leaves those two lines
	 * out, so the whole run stays in the ring and is read after it -- the stalls are rare, and
	 * they are what is wanted when the screen is the user's own swiping.
	 */
	lat = strstr(args, "touch_lat=1");
	stall = strstr(args, "stall_lat=1");
	if ((lat == NULL || (lat != args && lat[-1] != ' ')) &&
	    (stall == NULL || (stall != args && stall[-1] != ' '))) {
		return;
	}
	turns_logged = lat != NULL && (lat == args || lat[-1] == ' ');
	mach_timebase_info(&timebase);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true, LONG_MIN,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, CA_COMMIT_ORDER - 1,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	{
		CFRunLoopObserverContext after = { .info = (void *)1 };

		CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
		    CA_COMMIT_ORDER + 1, main_loop_turn, &after), kCFRunLoopCommonModes);
	}
	/*
	 * And the watchdog that says where the main thread is while such a turn lasts: a turn of
	 * 11 s (p389) says the window server drew nothing, and nothing about the call that held it.
	 * From a thread of its own, at 250 ms into a turn, again at 1 s and at 4 s, the main
	 * thread's stack is written down -- "hakios: stuck N ms" then the "Sample t0" lines.
	 */
	main_thread_port = pthread_mach_thread_np(pthread_self());
	{
		pthread_t watchdog;

		if (pthread_create(&watchdog, NULL, main_stuck_watch, NULL) == 0) {
			pthread_detach(watchdog);
			say("hakios: the main thread's stalls are watched (250 ms, 1 s, 4 s)\n");
		}
	}
}

/* ---- methods of SpringBoard's classes, replaced --------------------------------------------------- */

/* (CACHE_BASE and slide defined above) */

/*
 * Replace -[CLASS SELECTOR] (or +[CLASS SELECTOR]: `kind` is that first character), whose code must now
 * be at the unslid address `expected` (iOS 16.5, 20F66: what a replacement does depends on what the
 * original did). Returns the original.
 * (dyld interposing of the C functions underneath does not reach calls made from inside the
 * shared cache: there one library branches straight into the other.)
 */
static IMP
replace_method_of_kind(char kind, const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, sel_registerName(selector)) :
	    class_getInstanceMethod(cls, sel_registerName(selector));
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != expected + slide) {
		say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", kind, class_name, selector,
		    original ? (uintptr_t)original - slide : 0, expected);
		return NULL;
	}
	method_setImplementation(method, replacement);
	return original;
}

static IMP
replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	return replace_method_of_kind('-', class_name, selector, expected, replacement);
}

/*
 * -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in the
 * int at +8: 2 = activated, 1 = not, 3 = unknown; it stays 0 when there is no answer.
 */
#define LOCKDOWN_STATE(manager)         (*(int *)((char *)(manager) + 8))
#define LOCKDOWN_ACTIVATED              2

static void
setup_activation_state(id self, __unused SEL selector)
{
	LOCKDOWN_STATE(self) = LOCKDOWN_ACTIVATED;
}

/* -[SBSetupManager _setSetupRequiredReason:]: 0 = no need for the setup assistant */
static void (*original_set_setup_required_reason)(id self, SEL selector, long reason);

static void
set_setup_required_reason(id self, SEL selector, long reason)
{
	if (reason != 0) {
		say("hakios: SpringBoard wants the setup assistant (reason %ld); not here\n", reason);
	}
	original_set_setup_required_reason(self, selector, 0);
}

/*
 * +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] chooses the file format of the picture
 * SpringBoard takes of an application that leaves the screen: 0 = PNG, 1 and 2 = ATX (an ASTC texture),
 * 3 = JPEG. A device with a wide colour display that can decode ASTC gets ATX, and iOS compresses it on
 * the processor: fast with the vector code of Apple's cores, but here it takes one core more than ten
 * seconds per picture (under QEMU; seconds on the phone) -- and all that time the application waits for
 * the answer, until the watchdog for scene updates (10 s, there is no setting to scale it) kills it:
 * no application survived going to the background. JPEG takes a fraction of a second; it has no
 * transparency, so a picture that needs it becomes a PNG (which is what iOS itself does on a device
 * without ASTC).
 */
#define SNAPSHOT_FORMAT_PNG             0
#define SNAPSHOT_FORMAT_JPEG            3

static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

/* ---- battery state from redwood's Qualcomm charger stack ----------------------------------------- */

/*
 * iOS reads battery level through UIDevice.batteryLevel and UIDevice.batteryState; SpringBoard's
 * status bar reads it the same way via SBBatteryController. Where the battery has no power source
 * of its own in the kernel (battery=1, the loader's way through Qualcomm's UEFI charger protocols),
 * we hook the UIDevice accessors to return what XNU publishes as kern.hakios.battery_soc (0..100)
 * and kern.hakios.battery_charging.
 * When the sysctl is missing (battery=1 not in boot-args, or the driver never started) we hand
 * back the value the real implementation computed; the status bar then shows whatever iOS last
 * knew, which is better than crashing.
 * With adsp=1 the battery is a power source (the service HakiosBattery, an IOPMPowerSource:
 * iokit/Kernel/hakios.battery.cpp), which powerd reads like an iPhone's own, and from it iOS knows
 * everything by itself; once that service is there, the hooks hand everything to iOS's own code.
 */
static float  (*original_battery_level)(id self, SEL sel);
static long   (*original_battery_state)(id self, SEL sel);
static BOOL   (*original_battery_view_shows_percentage)(id self, SEL sel);
static void   (*original_battery_view_set_shows_percentage)(id self, SEL sel, BOOL shows);
static BOOL   (*original_battery_view_should_show_bolt)(id self, SEL sel);
static double (*original_battery_view_charge_percent)(id self, SEL sel);
static void   (*original_battery_view_set_charge_percent)(id self, SEL sel, double p);
static double (*original_ring_charging_level)(id self, SEL sel);

static int
sysctl_int(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);

	sysctlbyname(name, &value, &size, NULL, 0);
	return value;
}

/* Is the battery a power source that iOS reads by itself (see above)? Once it is, it stays one. */
static bool
battery_power_source(void)
{
	static bool found;
	io_service_t service;

	if (!found && (service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosBattery"))) != 0) {
		IOObjectRelease(service);
		found = true;
	}
	return found;
}

/* ---- the cellular items of the status bar, from the kernel's modem ------------------------------ */

/*
 * SpringBoard's SBStatusBarStateAggregator fills the raw status bar data from CoreTelephony, which
 * has no baseband here. Its item updaters take the data and a getter and setter for the field
 * (iOS 16.5: -[SBStatusBarStateAggregator _updateSignalStrengthItem:withData:SIMInfo:barsGetter:
 * barsSetter:], and the like for the service string): the signal one is replaced so that, when the
 * kernel's modem is registered (sysctl kern.hakios.cellular_registered, _bars: hakios.mpss.c),
 * the bars are set and the item switched on (-[... _setItem:enabled:]). The aggregator's whole
 * method list is said once in the log, for the posting to be found; and the signal item is asked
 * to update itself every few seconds while the kernel's state changes.
 */
static void (*original_update_signal_item)(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter);
static id cellular_aggregator;
static int cellular_last_bars = -2, cellular_said;

/* The getters and setters are blocks (their type is "@?"): called through the block's own invoke pointer, with the block first. */
struct block_layout {
	void *isa;
	int flags, reserved;
	void (*invoke)(void *, ...);
};
#define BLOCK_CALL(block, type, ...) ((type)((struct block_layout *)(block))->invoke)((block), __VA_ARGS__)

static void
update_signal_item(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter)
{
	int up = sysctl_int("kern.hakios.cellular_up"), registered = sysctl_int("kern.hakios.cellular_registered");
	int bars = sysctl_int("kern.hakios.cellular_bars"), dbm = sysctl_int("kern.hakios.cellular_dbm");
	int shown;

	cellular_aggregator = self;
	if (original_update_signal_item) {
		original_update_signal_item(self, sel, item, data, sim, getter, setter);
	}
	if (cellular_said++ < 8) {
		say("hakios: cellular: signal item %ld: data %p, SIM info %p, getter %p, setter %p; the kernel says up %d registered %d bars %d dbm %d\n",
		    item, data, (void *)sim, getter, setter, up, registered, bars, dbm);
	}
	/*
	 * Force the signal item on. iOS hides it unless its own cellular stack (CommCenter, which is
	 * not here) says there is service, so the bars never appear on their own. The kernel knows
	 * the modem's state, so once it is up the item is shown here with the kernel's bars; when the
	 * kernel has a reading but zero bars (a brief "no signal" sample, or a reading just after
	 * registration) at least one bar is forced so the icon is visibly present rather than empty.
	 * modem_bars_off=1 (as a sysctl) is not honoured; set kern.hakios.cellular_up=0 to hide it.
	 */
	/*
	 * The kernel's NAS state alternates between two reports -- "registered, signal not read" and
	 * "not registered, -86 dBm" -- so at the moments it says registered the dBm reads 0. The last
	 * real reading (within a minute) stands in for it.
	 */
	{
		static int last_dbm;
		static time_t last_dbm_at;
		time_t now = time(NULL);

		if (dbm < 0) {
			last_dbm = dbm;
			last_dbm_at = now;
		} else if (last_dbm < 0 && now - last_dbm_at < 60) {
			dbm = last_dbm;
		}
	}
	shown = bars;
	if (shown <= 0 && dbm < 0) {
		shown = dbm >= -95 ? 4 : dbm >= -105 ? 3 : dbm >= -115 ? 2 : 1;   /* derive from dBm when bars is 0 */
	}
	if (shown <= 0 && (up > 0 || registered > 0)) {
		shown = 1;                      /* up but no reading yet: still show the icon */
	}
	if (shown > 5) {
		shown = 5;
	}
	if ((up > 0 || registered > 0) && shown > 0 && setter != NULL && data != NULL) {
		BLOCK_CALL(setter, void (*)(void *, void *, int), data, shown);
		((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
		if (shown != cellular_last_bars) {
			say("hakios: cellular: %d bars shown (item %ld; kernel bars %d, %d dBm, registered %d)\n", shown, item, bars, dbm, registered);
		}
	}
	cellular_last_bars = shown;
}

static void (*original_update_service_item)(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2, void *s2,
    void *g3, void *s3, void *g4, void *s4);

/*
 * The service item ("No SIM", the network's name): once the kernel's modem is registered, the
 * network's name (sysctl kern.hakios.cellular_operator) goes in through the item's string setter
 * and the item is switched on.
 */
static void
update_service_item(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2, void *s2, void *g3, void *s3,
    void *g4, void *s4)
{
	static int said;
	static char shown[64];
	char name[64] = "";
	size_t size = sizeof(name) - 1;
	int registered = sysctl_int("kern.hakios.cellular_registered");

	if (original_update_service_item) {
		original_update_service_item(self, sel, item, data, sim, other, g1, s1, g2, s2, g3, s3, g4, s4);
	}
	sysctlbyname("kern.hakios.cellular_operator", name, &size, NULL, 0);
	if (said++ < 4) {
		say("hakios: cellular: service item %ld: data %p, SIM info %p, other %p; string getter %p setter %p, content type getter %p "
		    "setter %p; the kernel's network \"%s\"\n", item, data, (void *)sim, (void *)other, g1, s1, g3, s3, name);
	}
	if (registered > 0 && name[0] != 0 && s1 != NULL && data != NULL) {
		CFStringRef string = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);

		if (string != NULL) {
			BLOCK_CALL(s1, void (*)(void *, void *, CFStringRef), data, string);
			((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
			if (strcmp(shown, name) != 0) {
				snprintf(shown, sizeof(shown), "%s", name);
				say("hakios: cellular: service \"%s\" shown (item %ld)\n", name, item);
			}
			CFRelease(string);
		}
	}
}

/*
 * The bars are drawn by UIKit's signal view, whose "signal mode" says how: inactive (all bars grey),
 * searching, or active (the bars that count dark). iOS's cellular stack (CommCenter, not here)
 * puts it in active mode once there is service; without it the bars stay grey whatever their
 * number. So for the cellular signal view (not the Wi-Fi one), while the kernel says the modem is
 * registered, the mode is made active. The modes seen are said in the log once each.
 */
static void (*original_set_signal_mode)(id self, SEL sel, long mode);
static long cellular_active_mode = 2;  /* _UIStatusBarSignalView: 0 inactive, 1 searching, 2 active */

static void
set_signal_mode(id self, SEL sel, long mode)
{
	static unsigned said_modes;
	const char *name = object_getClassName(self);
	bool cellular = name != NULL && strstr(name, "Cellular") != NULL;
	long wanted = mode;

	if (cellular && mode < 8 && !(said_modes & (1u << mode))) {
		said_modes |= 1u << mode;
		say("hakios: cellular: %s asked for signal mode %ld\n", name, mode);
	}
	if (cellular && sysctl_int("kern.hakios.cellular_registered") > 0 && mode != cellular_active_mode) {
		wanted = cellular_active_mode;
	}
	original_set_signal_mode(self, sel, wanted);
}

static void
signal_view_hook(void)
{
	static const char *const classes[] = { "_UIStatusBarCellularSignalView", "_UIStatusBarSignalView" };

	for (unsigned c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
		Class cls = objc_getClass(classes[c]);
		Method method = cls ? class_getInstanceMethod(cls, sel_registerName("setSignalMode:")) : NULL;

		if (method == NULL) {
			say("hakios: cellular: no -[%s setSignalMode:]\n", classes[c]);
			continue;
		}
		original_set_signal_mode = (void (*)(id, SEL, long))method_getImplementation(method);
		method_setImplementation(method, (IMP)set_signal_mode);
		say("hakios: cellular: -[%s setSignalMode:] replaced (types \"%s\"): active while the kernel says registered\n",
		    classes[c], method_getTypeEncoding(method));
		return;         /* the base class's method serves the subclasses */
	}
}

static void
cellular_hook(void)
{
	signal_view_hook();
	{
		const char *service_selector = "_updateServiceItem:withData:SIMInfo:otherSIMInfo:serviceStringGetter:serviceStringSetter:"
		    "serviceCrossfadeStringGetter:serviceCrossfadeStringSetter:serviceContentTypeGetter:serviceContentTypeSetter:"
		    "serviceBadgeStringGetter:serviceBadgeStringSetter:";
		Class cls = objc_getClass("SBStatusBarStateAggregator");
		Method method = cls ? class_getInstanceMethod(cls, sel_registerName(service_selector)) : NULL;

		if (method) {
			say("hakios: cellular: -[SBStatusBarStateAggregator %s] at %#lx, types \"%s\": looked at\n", service_selector,
			    (uintptr_t)method_getImplementation(method) - slide, method_getTypeEncoding(method));
			original_update_service_item = (void (*)(id, SEL, long, void *, id, id, void *, void *, void *, void *, void *, void *, void *,
			    void *))method_getImplementation(method);
			method_setImplementation(method, (IMP)update_service_item);
		}
	}
	const char *selector = "_updateSignalStrengthItem:withData:SIMInfo:barsGetter:barsSetter:";
	Class cls = objc_getClass("SBStatusBarStateAggregator");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		say("hakios: cellular: no -[SBStatusBarStateAggregator %s]\n", selector);
		return;
	}
	say("hakios: cellular: -[SBStatusBarStateAggregator %s] at %#lx, types \"%s\": replaced\n", selector,
	    (uintptr_t)method_getImplementation(method) - slide, method_getTypeEncoding(method));
	original_update_signal_item = (void (*)(id, SEL, long, void *, id, void *, void *))method_getImplementation(method);
	method_setImplementation(method, (IMP)update_signal_item);
	{
		/* the whole class, once: which updaters and posters there are */
		unsigned count = 0;
		Method *list = class_copyMethodList(cls, &count);
		char line[1024] = "";
		size_t at = 0;

		for (unsigned i = 0; list && i < count; i++) {
			const char *name = sel_getName(method_getName(list[i]));
			size_t n = strlen(name);

			if (at + n + 2 > sizeof(line)) {
				say("hakios: cellular: aggregator methods: %s\n", line);
				line[0] = 0;
				at = 0;
			}
			memcpy(line + at, name, n);
			at += n;
			line[at++] = ' ';
			line[at] = 0;
		}
		if (at) {
			say("hakios: cellular: aggregator methods: %s\n", line);
		}
		free(list);
	}
	/* every 3 s: when the kernel's registration or bars change, the item is asked to update itself */
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());

	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), 3 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
	dispatch_source_set_event_handler(timer, ^{
		static int last_registered = -2, last_bars = -2;
		int registered = sysctl_int("kern.hakios.cellular_registered"), bars = sysctl_int("kern.hakios.cellular_bars");

		if ((registered != last_registered || bars != last_bars) && cellular_aggregator != nil) {
			SEL update = sel_registerName("_updateSignalStrengthItem");

			last_registered = registered;
			last_bars = bars;
			if (class_getInstanceMethod(object_getClass(cellular_aggregator), update)) {
				((void (*)(id, SEL))objc_msgSend)(cellular_aggregator, update);
				say("hakios: cellular: the signal item asked to update (registered %d, bars %d)\n", registered, bars);
			}
		}
	});
	dispatch_resume(timer);
}

/* UIDevice.batteryLevel: 0.0..1.0; -1.0 if monitoring not enabled */
static float
battery_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_level ? original_battery_level(self, sel) : -1.0f;
	}
	return (float)soc / 100.0f;
}

/* UIDevice.batteryState: 0=unknown 1=unplugged 2=charging 3=full */
static long
battery_state(id self, SEL sel)
{
	int charging = sysctl_int("kern.hakios.battery_charging");
	int present = sysctl_int("kern.hakios.battery_present");
	int plugged = sysctl_int("kern.hakios.battery_charger_present");
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (charging == -1 || present == -1 || plugged == -1 || soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_state ? original_battery_state(self, sel) : 0;
	}
	if (!present) return 0;
	if (plugged && soc >= 100) return 3;
	return charging ? 2 : 1;
}

/*
 * UIKitCore's status-bar view has its own presentation policy, separate from UIDevice. With the power
 * source it is iOS's own again: the switch Battery Percentage in Settings > Battery, which the image
 * turns on (SBShowBatteryPercentage in userland/var/mobile/Library/Preferences/com.apple.springboard.plist).
 */
static BOOL
battery_view_shows_percentage(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return YES;
	return original_battery_view_shows_percentage ? original_battery_view_shows_percentage(self, sel) : NO;
}

static void
battery_view_set_shows_percentage(id self, SEL sel, BOOL shows)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	original_battery_view_set_shows_percentage(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? YES : shows);
}

static BOOL
battery_view_should_show_bolt(id self, SEL sel)
{
	int charging = sysctl_int("kern.hakios.battery_charging");
	int present = sysctl_int("kern.hakios.battery_present");
	int plugged = sysctl_int("kern.hakios.battery_charger_present");

	if (charging != -1 && present != -1 && plugged != -1 && !battery_power_source()) return charging && present && plugged;
	return original_battery_view_should_show_bolt ? original_battery_view_should_show_bolt(self, sel) : NO;
}

/*
 * The actual number the status bar draws. _UIBatteryView keeps the fill fraction (0.0..1.0) in
 * chargePercent; it is set through setChargePercent: and read back for layout. iOS feeds it from
 * BatteryCenter / IOPowerSources, which is empty on this phone, so without us it sits near zero
 * (the "1%" the user saw). We override both sides: setChargePercent: so the stored value is ours
 * no matter who writes it, and the getter so any layout path that reads it agrees.
 */
static double
battery_view_charge_percent(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_battery_view_charge_percent ? original_battery_view_charge_percent(self, sel) : 0.0;
}

static void
battery_view_set_charge_percent(id self, SEL sel, double p)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	original_battery_view_set_charge_percent(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? (double)soc / 100.0 : p);
}

/* The big charging ring on the lock screen (CoverSheet) reads its fill level the same way. */
static double
ring_charging_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_ring_charging_level ? original_ring_charging_level(self, sel) : 0.0;
}

/* Keep SpringBoard's normal battery observers awake while the XNU cache changes. */
static void
battery_post_notifications(void)
{
	Class uidev = objc_getClass("UIDevice");
	Class notification_center = objc_getClass("NSNotificationCenter");
	id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, sel_registerName("currentDevice")) : nil;
	id center = notification_center ? ((id (*)(Class, SEL))objc_msgSend)(notification_center,
	    sel_registerName("defaultCenter")) : nil;

	if (center != nil && !battery_power_source()) {
		SEL post = sel_registerName("postNotificationName:object:");
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryLevelDidChangeNotification"), device);
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryStateDidChangeNotification"), device);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		battery_post_notifications();
	});
}

/* ---- the lock screen's clock without a GPU ------------------------------------------------------- */

#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

/* Does the window server draw with OpenGL ES? The same question, asked the same way, as opengl_es() in inject.c. */
static bool
gpu_draws(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;

	if (access(HAKIOS_GL, R_OK) != 0) {
		return false;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				return argument[3] != '0';
			}
		}
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
	if (gpu != 0) {
		IOObjectRelease(gpu);
	}
	return gpu != 0;
}

/*
 * The clock of the lock screen is a "vibrant" view (BSUIVibrancyEffectView in BaseBoardUI): its
 * digits are not drawn, they are a mask -- a copy of them through a colour matrix that keeps red
 * as alpha -- through which the filter vibrantColorMatrix lets the wallpaper behind come out
 * lighter; and a gradient over the whole screen, red at the top, tones the wallpaper down the
 * same way. On the GPU that is what is seen (eagl.c draws the two filters). CoreAnimation's
 * software renderer shows neither: the digits come out red, and the top of the wallpaper as it
 * was. Why was not found out; that renderer is in QuartzCore, not here.
 * Such views have a switch, the user default BSUIVibrancyEffectViewDisabled: with it they show
 * their content as it is drawn, the clock in plain white. It is set for this run of SpringBoard
 * alone (nothing is written), and only where the software renderer draws.
 */
static void
plain_vibrancy(void)
{
	if (gpu_draws()) {
		return;
	}
	CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
	say("hakios: SpringBoard: no GPU draws, vibrant views are plain (the lock screen's clock is white)\n");
}

/* ---- the side button's long press ---------------------------------------------------------------- */

/*
 * -[SBLockHardwareButton longPress:] is what the side button's long press recognizer calls: Siri
 * (here, its "turn on Siri" sheet, nobody having turned it on). The recognizer is a timer of
 * SpringBoard's own: started when the main thread sees the press, stopped when it sees the release
 * -- and a release it has not got round to is no release. A press of the side button with the
 * display off wakes the display from inside the handling of that very press, which on an iPhone
 * takes no time. Here it takes as long as the software renderer needs for the first picture of the
 * lock screen: a tenth of a second on the phone, a second under QEMU. That is longer than a long
 * press, so there every wake by the button (and by a tap, which is a press of that button made in
 * inject.c) ended with Siri's sheet over the lock screen, however short the press had been.
 * Where the button's events come from -- inject.c, in backboardd -- it is known whether the button
 * is still held; a long press of a button that is not is dropped here.
 */
#define GESTURE_BEGAN                   1       /* UIGestureRecognizerState */
#define GESTURE_ENDED                   3       /* and cancelled, failed after it */

static void (*original_lock_long_press)(id self, SEL selector, id recognizer);

static bool
lock_button_held(void)
{
	static int token = NOTIFY_TOKEN_INVALID;
	uint64_t state = 0;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return true;
	}
	/* 1 = released; held, or not known: as SpringBoard has it */
	return notify_get_state(token, &state) != NOTIFY_STATUS_OK || state != 1;
}

static void
lock_long_press(id self, SEL selector, id recognizer)
{
	static bool dropping;
	SEL state_selector = sel_registerName("state");
	long state = recognizer != NULL && class_getInstanceMethod(object_getClass(recognizer), state_selector) != NULL ?
	    ((long (*)(id, SEL))objc_msgSend)(recognizer, state_selector) : 0;

	if (state == GESTURE_BEGAN) {
		dropping = !lock_button_held();
		if (dropping) {
			say("hakios: SpringBoard: a long press of the side button, which was released long ago: dropped\n");
		}
	}
	if (dropping) {
		if (state >= GESTURE_ENDED) {
			dropping = false;
		}
		return;
	}
	original_lock_long_press(self, selector, recognizer);
}

/* ---- remote control ------------------------------------------------------------------------------ */

#define UNLOCK_SOURCE_COVER_SHEET       24      /* what the lock screen itself passes */

static void
unlock(void)
{
	Class cls = objc_getClass("SBLockScreenManager");
	SEL shared = sel_registerName("sharedInstance"), locked = sel_registerName("isUILocked");
	SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
	id manager;

	if (cls == NULL || class_getClassMethod(cls, shared) == NULL || class_getInstanceMethod(cls, locked) == NULL ||
	    class_getInstanceMethod(cls, unlock_ui) == NULL) {
		say("hakios: unlock: SBLockScreenManager is not what it was\n");
		return;
	}
	manager = ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
	if (manager == NULL) {
		say("hakios: unlock: no lock screen manager yet\n");
	} else if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
		say("hakios: unlock: not locked\n");
	} else {
		BOOL done = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, NULL);

		say("hakios: unlock: asked, answer %d\n", done);
	}
}

/*
 * Boot-arg sb_unlock=N: after N seconds the lock screen is asked to unlock, three times fifteen
 * seconds apart (the manager may not be there the first time).  p227/p228: the lock screen with its
 * two "Waiting..." widgets is on the screen by the eighth minute, and the display goes black by the
 * eleventh; this is to see what is behind the lock screen, and whether "swipe up" works at all.
 */
static unsigned unlock_after;

static void *
unlock_later(void *unused)
{
	int i;

	(void)unused;
	sleep(unlock_after);
	for (i = 0; i < 3; i++) {
		dispatch_async(dispatch_get_main_queue(), ^{
			unlock();
		});
		sleep(15);
	}
	return NULL;
}

static void
unlock_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;
	const char *at;
	pthread_t th;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "sb_unlock=");
	if (at == NULL) {
		return;
	}
	unlock_after = (unsigned)strtoul(at + 10, NULL, 10);
	if (unlock_after < 30 || unlock_after > 900) {
		return;
	}
	say("hakios: unlock: the lock screen is asked to unlock at %u s\n", unlock_after);
	if (pthread_create(&th, NULL, unlock_later, NULL) == 0) {
		pthread_detach(th);
	}
}

/*
 * The poster switcher: on iOS 16 a long press on the lock screen's wallpaper shrinks it into a
 * row of posters to choose from or customize. The row is PosterBoard's, an application scene of
 * that process which the lock screen hosts (CSPosterSwitcherViewController, a CALayerHost of the
 * context of PosterBoard's window over an opaque black layer). PosterBoard does lay it out -- its
 * window has the posters, "Customize" and "+", the hosted context is that window's, the scene is
 * in the foreground -- but the window server draws nothing of that context, with either
 * renderer, so what is left is the black layer under it, until a swipe up takes the switcher
 * away. Why is not known: the display is not in secure mode, SpringBoard counts the user as
 * authenticated, the same nesting of hosted contexts works for the wallpaper itself.
 * Until it is, the switcher is not offered, the way SpringBoard itself withholds it where it has
 * none (an iPad, Guided Access): its policy says the capability is not there, and the long press
 * does nothing.
 */
static BOOL
poster_switcher_allowed(__unused id self, __unused SEL selector, id *explanation)
{
	if (explanation != NULL) {
		*explanation = (id)CFSTR("the poster switcher is not shown here (see inject_sb.c)");
	}
	return NO;
}

/*
 * What SpringBoard has put on the screen, as CoreAnimation has it: every layer of every window, a
 * line each -- class and name, place, opacity, what it shows (an image, a colour, another
 * process's layers), and what is done to it: its filters by type with their inputs, in order,
 * the compositing filter, the mask. That is where a picture that comes out wrong on one renderer
 * and right on another is described: the same description goes to both.
 */
#define SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)

static const char *layers_of = "";               /* in another program than SpringBoard: " of" and its name */

static const char *
text_of(id object)
{
	id text = object ? SEND(id, object, "description") : NULL;

	return text ? SEND(const char *, text, "UTF8String") : "-";
}

/*
 * The switcher of running applications lays a wash of wallpaper over every card behind the front
 * one, above the dimming: -[SBAppSwitcherPageView _createOrDestroyWallpaperOverlayView] makes it,
 * PaperBoardUI fills it from the image provider of the wallpaper, and the result is drawn at about
 * fifteen per cent (the layer tree: SBWallpaperEffectView at opacity 0.1497 -> PBUIFakeBlurView ->
 * PBUIWallpaperView -> PBUISnapshotReplicaView -> a UIImageView).
 *
 * Here there is no picture of the wallpaper to put in it. The wallpaper itself is drawn live by
 * PosterBoard, in another process, and hosted into SpringBoard's layer tree; SpringBoard has no
 * bitmap of it, and nothing ever bakes one -- after three and a half minutes of an idle phone
 * /var/mobile/Library holds sixty directories and not one of them is PaperBoard, PosterBoard or
 * Wallpaper. So the provider hands over a placeholder: 846 x 1827 pixels of one colour,
 * (179, 77, 179), which CoreAnimation compresses to ASTC and draws over the two cards on the left
 * at 14.9 per cent. That is what made them purple instead of merely dimmed.
 *
 * The dimming itself is right and stays: two draws of a flat black at 11.4 per cent, which is the
 * shadow one expects to see. Only the wash is left out, because a wash of a colour that is not the
 * wallpaper's is worse than no wash at all. When something here can give PaperBoardUI a real
 * picture of the wallpaper, this hook is what should go -- and until then switcher_wallpaper=1
 * among the boot-args puts it back as iOS asks for it, so that whoever works on the wallpaper can
 * see at once whether their picture arrives, without rebuilding an image to find out.
 */
static void (*original_switcher_wallpaper_overlay)(id self, SEL selector);

static void
switcher_wallpaper_overlay(id self, SEL selector)
{
	static int wanted = -1;         /* boot-arg switcher_wallpaper=1: have it back, placeholder and all */
	static unsigned told;

	if (wanted < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		wanted = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "switcher_wallpaper=1")) != NULL && (at == args || at[-1] == ' ');
	}
	if (told++ == 0) {
		say("hakios: the switcher's wash of wallpaper is %s\n", wanted ?
		    "drawn as iOS asks (switcher_wallpaper=1)" : "left out: there is no picture of the wallpaper for it");
	}
	if (wanted) {
		original_switcher_wallpaper_overlay(self, selector);
	}
}

static void
layer_filter(id filter, const char *what, int depth)
{
	static const char *const inputs[] = { "inputColorMatrix", "inputRadius", "inputAmount", "inputColor", "inputAngle",
		                              "inputPremultipliedValues", "inputNormalizeEdges", "inputReversed" };
	char line[1024];
	size_t n;

	if (!class_respondsToSelector(object_getClass(filter), sel_registerName("type"))) {
		say("hakios: layers%s: %*s  %s %s\n", layers_of, 2 * depth, "", what, text_of(filter));     /* a name alone */
		return;
	}
	n = (size_t)snprintf(line, sizeof(line), "%s %s \"%s\"%s", what, text_of(SEND(id, filter, "type")),
	    text_of(SEND(id, filter, "name")), SEND(BOOL, filter, "isEnabled") ? "" : " (disabled)");
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]) && n < sizeof(line); i++) {
		id key = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, inputs[i]);
		id value = SEND1(id, filter, "valueForKey:", id, key);
		float m[20];

		if (value == NULL) {
			continue;
		}
		if (i == 0 && class_respondsToSelector(object_getClass(value), sel_registerName("CAColorMatrixValue"))) {
			SEND1(void, value, "getValue:", void *, m);
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s [%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g | "
			    "%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g]", inputs[i], m[0], m[1], m[2], m[3], m[4], m[5], m[6],
			    m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]);
		} else {
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s %s", inputs[i], text_of(value));
		}
	}
	say("hakios: layers%s: %*s  %s\n", layers_of, 2 * depth, "", line);
}

static void
layer_line(id layer, const char *role, int depth)
{
	struct { double x, y, w, h; } frame = SEND(typeof(frame), layer, "frame");
	const char *class_name = object_getClassName(layer);
	id contents = SEND(id, layer, "contents"), filters = SEND(id, layer, "filters"), mask = SEND(id, layer, "mask");
	id compositing = SEND(id, layer, "compositingFilter"), sublayers = SEND(id, layer, "sublayers");
	id background = SEND(id, layer, "backgroundFilters");
	void *colour = SEND(void *, layer, "backgroundColor");
	char more[1024] = "";
	size_t n = 0;

	if (SEND(BOOL, layer, "isHidden")) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HIDDEN");
	}
	if (contents != NULL) {
		id text = (id)CFCopyDescription((CFTypeRef)contents);
		const char *from = text_of(text);
		char flat[300];
		size_t k = 0;

		/*
		 * What a CGImage says about itself runs over several lines -- its colour space, how big
		 * it is, how its pixels are laid out -- and all of that is worth having on the one line,
		 * because the size and the colour space are the first things to look at when a picture
		 * comes out wrong. Every run of spaces and newlines becomes one space.
		 */
		while (from != NULL && *from != 0 && k + 1 < sizeof(flat)) {
			if (*from == ' ' || *from == '\n' || *from == '\t' || *from == '\r') {
				if (k > 0 && flat[k - 1] != ' ') {
					flat[k++] = ' ';
				}
				from++;
				continue;
			}
			flat[k++] = *from++;
		}
		flat[k] = 0;
		n += (size_t)snprintf(more + n, sizeof(more) - n, " contents %s", flat);
		CFRelease((CFTypeRef)text);
	}
	if (contents != NULL && class_respondsToSelector(object_getClass(layer), sel_registerName("contentsMultiplyColor")) &&
	    SEND(void *, layer, "contentsMultiplyColor") != NULL) {
		id text = (id)CFCopyDescription(SEND(void *, layer, "contentsMultiplyColor"));
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " times %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (colour != NULL) {
		id text = (id)CFCopyDescription(colour);
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " colour %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (strcmp(class_name, "CAPortalLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " PORTAL of %p%s%s", (void *)SEND(id, layer, "sourceLayer"),
		    SEND(BOOL, layer, "hidesSourceLayer") ? " (hides it)" : "", SEND(BOOL, layer, "matchesOpacity") ? " (its opacity)" : "");
	} else if (strcmp(class_name, "CABackdropLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " BACKDROP group \"%s\" scale %g%s", text_of(SEND(id, layer, "groupName")),
		    SEND(double, layer, "scale"), SEND(BOOL, layer, "captureOnly") ? " (capture only)" : "");
	} else if (strcmp(class_name, "CAGradientLayer") == 0) {
		struct { double x, y; } from = SEND(typeof(from), layer, "startPoint"), to = SEND(typeof(to), layer, "endPoint");
		id colours = SEND(id, layer, "colors");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " GRADIENT %s from %g,%g to %g,%g at %.40s:", text_of(SEND(id, layer, "type")),
		    from.x, from.y, to.x, to.y, text_of(SEND(id, layer, "locations")));
		for (long i = 0; colours != NULL && i < SEND(long, colours, "count") && n < sizeof(more); i++) {
			id text = (id)CFCopyDescription((CFTypeRef)SEND1(id, colours, "objectAtIndex:", long, i));
			const char *numbers = strstr(text_of(text), "( ");

			n += (size_t)snprintf(more + n, sizeof(more) - n, " %.30s", numbers ? numbers : "?");
			CFRelease((CFTypeRef)text);
		}
	} else if (strcmp(class_name, "CAShapeLayer") == 0) {
		void *stroke = SEND(void *, layer, "strokeColor"), *fill = SEND(void *, layer, "fillColor");
		id text_stroke = stroke ? (id)CFCopyDescription(stroke) : NULL, text_fill = fill ? (id)CFCopyDescription(fill) : NULL;
		const char *ns = text_stroke ? strstr(text_of(text_stroke), "( ") : NULL, *nf = text_fill ? strstr(text_of(text_fill), "( ") : NULL;

		n += (size_t)snprintf(more + n, sizeof(more) - n, " SHAPE line %g stroke %.30s fill %.30s ends %g-%g", SEND(double, layer,
		    "lineWidth"), ns ? ns : "-", nf ? nf : "-", SEND(double, layer, "strokeStart"), SEND(double, layer, "strokeEnd"));
		if (text_stroke != NULL) {
			CFRelease((CFTypeRef)text_stroke);
		}
		if (text_fill != NULL) {
			CFRelease((CFTypeRef)text_fill);
		}
	} else if (strcmp(class_name, "CALayerHost") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HOST of context %#x%s%s", SEND(unsigned, layer, "contextId"),
		    SEND(BOOL, layer, "inheritsSecurity") ? " (inherits security)" : "",
		    SEND(BOOL, layer, "rendersAsynchronously") ? " (asynchronously)" : "");
	}
	{
		/* the view a layer belongs to, which is the only thing that names a layer UIKit left
		   unnamed -- and the switcher's wash of wallpaper is such a layer */
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " of %s", object_getClassName(delegate));
		}
	}
	{
		id keys = SEND(id, layer, "animationKeys");

		if (SEND(BOOL, layer, "shouldRasterize")) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " RASTERIZED");
		}
		for (long i = 0; keys != NULL && i < SEND(long, keys, "count") && n < sizeof(more); i++) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, "%s%s", i == 0 ? " ANIMATIONS " : ",",
			    text_of(SEND1(id, keys, "objectAtIndex:", long, i)));
		}
	}
	say("hakios: layers%s: %*s%s%s %p \"%s\" %g,%g %gx%g opacity %g%s\n", layers_of, 2 * depth, "", role, class_name, (void *)layer,
	    text_of(SEND(id, layer, "name")), frame.x, frame.y, frame.w, frame.h, (double)SEND(float, layer, "opacity"), more);
	for (long i = 0; filters != NULL && i < SEND(long, filters, "count"); i++) {
		layer_filter(SEND1(id, filters, "objectAtIndex:", long, i), "filter", depth);
	}
	for (long i = 0; background != NULL && i < SEND(long, background, "count"); i++) {
		layer_filter(SEND1(id, background, "objectAtIndex:", long, i), "background filter", depth);
	}
	if (compositing != NULL) {
		layer_filter(compositing, "compositing filter", depth);
	}
	if (mask != NULL) {
		layer_line(mask, "MASK ", depth + 1);
	}
	for (long i = 0; sublayers != NULL && i < SEND(long, sublayers, "count"); i++) {
		layer_line(SEND1(id, sublayers, "objectAtIndex:", long, i), "", depth + 1);
	}
}

/* Every WKWebView under a view: whether WebKit takes it for one in the background, its web process and whether that answers. */
static void
web_views(id view, int depth)
{
	Class web = objc_getClass("WKWebView");
	id subviews;
	bool is_web = false;

	if (view == NULL || depth > 40) {
		return;
	}

	for (Class c = object_getClass(view); web != NULL && c != NULL && !is_web; c = class_getSuperclass(c)) {
		is_web = c == web;
	}
	if (is_web) {
		SEL background = sel_registerName("_isBackground"), pid = sel_registerName("_webProcessIdentifier");
		SEL responsive = sel_registerName("_webProcessIsResponsive");

		say("hakios: layers%s: a WKWebView %p: in the background %s, web process %d, responsive %s, in a window %s, hidden %s\n",
		    layers_of, (void *)view, class_respondsToSelector(object_getClass(view), background) ?
		    (SEND(BOOL, view, "_isBackground") ? "YES" : "no") : "?", class_respondsToSelector(object_getClass(view), pid) ?
		    SEND(int, view, "_webProcessIdentifier") : -1, class_respondsToSelector(object_getClass(view), responsive) ?
		    (SEND(BOOL, view, "_webProcessIsResponsive") ? "yes" : "NO") : "?", SEND(id, view, "window") ? "yes" : "NO",
		    SEND(BOOL, view, "isHidden") ? "YES" : "no");
	}
	subviews = SEND(id, view, "subviews");
	for (long i = 0; subviews != NULL && i < SEND(long, subviews, "count"); i++) {
		web_views(SEND1(id, subviews, "objectAtIndex:", long, i), depth + 1);
	}
}

static void
layers(void)
{
	Class window = objc_getClass("UIWindow");
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows = window && class_getClassMethod(window, all) ? ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1) : NULL;
	id application = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id authentication = application && class_respondsToSelector(object_getClass(application),
	    sel_registerName("authenticationController")) ? SEND(id, application, "authenticationController") : NULL;

	say("hakios: layers%s: %ld windows\n", layers_of, windows ? SEND(long, windows, "count") : 0L);
	if (authentication != NULL) {
		say("hakios: layers: the user is %sauthenticated, %s passcode, the display %sin secure mode\n",
		    SEND(BOOL, authentication, "isAuthenticated") ? "" : "NOT ", SEND(BOOL, authentication, "hasPasscodeSet") ? "a" : "no",
		    SEND(BOOL, authentication, "inSecureDisplayMode") ? "" : "not ");
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		id w = SEND1(id, windows, "objectAtIndex:", long, i);
		id scene = class_respondsToSelector(object_getClass(w), sel_registerName("windowScene")) ? SEND(id, w, "windowScene") : NULL;
		id context = class_respondsToSelector(object_getClass(w), sel_registerName("_boundContext")) ? SEND(id, w, "_boundContext") : NULL;

		say("hakios: layers%s: window %s level %g, context %#x (%ssecure, %u commits), scene %s (activation state %ld)\n",
		    layers_of, object_getClassName(w), SEND(double, w, "windowLevel"), context ? SEND(unsigned, context, "contextId") : 0,
		    context && SEND(BOOL, context, "isSecure") ? "" : "not ", context ? (unsigned)SEND(unsigned long long, context, "commitId") : 0,
		    scene ? text_of(SEND(id, scene, "_persistenceIdentifier")) : "none", scene ? SEND(long, scene, "activationState") : -1L);
		layer_line(SEND(id, w, "layer"), "", 1);
	}
	if (application != NULL) {
		say("hakios: layers%s: the application's state %ld (0 active, 1 inactive, 2 in the background)\n", layers_of,
		    SEND(long, application, "applicationState"));
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		web_views(SEND1(id, windows, "objectAtIndex:", long, i), 0);
	}
	say("hakios: layers%s: end\n", layers_of);
}

/*
 * The switcher of running applications, on the notification com.hakios.switcher (rwtouch notify).
 * A drag up from the bottom edge is the way to it on a phone, but the touches a script puts in do
 * not reach iOS's system gestures -- nothing comes of such a drag, not even going home -- and two
 * presses of the home button only go home (there is no home button on a phone of this shape, and
 * what is sent as one goes home and no more). SpringBoard opens it itself for anything that is
 * not a gesture (a hardware keyboard, accessibility): SBMainSwitcherControllerCoordinator, which
 * is one for the whole program, takes "activate the main switcher not interactively, from this
 * source". The source is an analytics label; zero is "none", which is what the ones that have no
 * gesture behind them pass.
 * As it stands this is not enough: with three applications running it answers that it took the
 * request (source 0, animated and not animated alike) and the screen does not change -- the
 * application in front stays, and SpringBoard says nothing more. So the switcher still cannot be
 * reached from a script; what is left to try is the transition with a window scene
 * (_activateSwitcherValidatorWithEventLabel:animated:windowScene:), which is what its own
 * gestures use.
 */
static void
switcher(void)
{
	Class coordinator = objc_getClass("SBMainSwitcherControllerCoordinator");
	SEL activate = sel_registerName("activateMainSwitcherNoninteractivelyWithSource:animated:");
	id shared = coordinator != NULL ? SEND(id, coordinator, "sharedInstance") : NULL;
	Method method = shared != NULL ? class_getInstanceMethod(object_getClass(shared), activate) : NULL;

	if (method == NULL) {
		say("hakios: the switcher: %s has no %s\n",
		    coordinator != NULL ? "SBMainSwitcherControllerCoordinator" : "nothing", sel_getName(activate));
		return;
	}
	say("hakios: the switcher: opening it (%s, %s)\n", object_getClassName(shared),
	    method_getTypeEncoding(method) != NULL ? method_getTypeEncoding(method) : "?");
	/*
	 * It answers whether it took the request. The source is an analytics label and not every one
	 * of them is allowed from every state, so they are tried in turn until one is taken.
	 */
	/*
	 * Not animated: the animated one is taken and then nothing comes of it -- the transition it
	 * starts is the gesture's to carry, and there is no gesture here.
	 */
	for (long source = 0; source < 10; source++) {
		BOOL taken = ((BOOL (*)(id, SEL, long, BOOL))objc_msgSend)(shared, activate, source, NO);

		say("hakios: the switcher: from source %ld: %s\n", source, taken ? "taken" : "refused");
		if (taken) {
			return;
		}
	}
}

/*
 * Control Center's glyphs whole from its first frames. Its modules and their parts fade in as it
 * comes down: opacity animations, mostly springs that take up to a second to settle at 1. While a
 * layer's opacity is below 1, the window server's OpenGL ES renderer draws the vibrant glyphs in it
 * from a flattened copy that lacks the strokes composited against what is behind them, and draws them
 * whole only at full opacity: the strokes of the media controls, orientation lock, screen mirroring
 * and Focus came in only when the springs had settled, at the end of the presentation. (On an iPhone
 * they are whole all along.) So in Control Center's window an opacity animation is replaced with an
 * eased-out fade of cc_fade=MS between the same values: the modules still fade in and out, and are at
 * full opacity, glyphs whole, after a few frames. Off by default (iOS's own animations stay as they
 * are; the real fix belongs in the drawing, not in the animations): a workaround, for checks only.
 */
#define CC_FADE_MS      0
static void (*original_add_animation_cc)(id self, SEL selector, id animation, id key);
static double cc_fade_s = CC_FADE_MS / 1000.0;
static unsigned cc_fades;

static bool
in_control_center(id layer)
{
	for (int depth = 0; layer != NULL && depth < 64; depth++) {
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL && strcmp(object_getClassName(delegate), "SBControlCenterWindow") == 0) {
			return true;
		}
		layer = SEND(id, layer, "superlayer");
	}
	return false;
}

static void
add_animation_cc(id self, SEL selector, id animation, id key)
{
	Class basic = objc_getClass("CABasicAnimation");
	id path = animation != NULL && class_respondsToSelector(object_getClass(animation), sel_registerName("keyPath")) ?
	    SEND(id, animation, "keyPath") : NULL;

	if (path != NULL && strcmp(text_of(path), "opacity") == 0 &&
	    class_respondsToSelector(object_getClass(animation), sel_registerName("fromValue")) && in_control_center(self)) {
		id fade = SEND1(id, basic, "animationWithKeyPath:", id, path);
		id ease = SEND1(id, objc_getClass("CAMediaTimingFunction"), "functionWithName:", id,
		    SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, "easeOut"));

		SEND1(void, fade, "setFromValue:", id, SEND(id, animation, "fromValue"));
		SEND1(void, fade, "setToValue:", id, SEND(id, animation, "toValue"));
		SEND1(void, fade, "setByValue:", id, SEND(id, animation, "byValue"));
		SEND1(void, fade, "setAdditive:", BOOL, SEND(BOOL, animation, "isAdditive"));
		SEND1(void, fade, "setBeginTime:", double, SEND(double, animation, "beginTime"));
		SEND1(void, fade, "setFillMode:", id, SEND(id, animation, "fillMode"));
		SEND1(void, fade, "setRemovedOnCompletion:", BOOL, SEND(BOOL, animation, "isRemovedOnCompletion"));
		SEND1(void, fade, "setDelegate:", id, SEND(id, animation, "delegate"));
		SEND1(void, fade, "setDuration:", double, cc_fade_s);
		SEND1(void, fade, "setTimingFunction:", id, ease);
		if (cc_fades++ < 3) {
			say("hakios: Control Center: an opacity animation of %.3f s (%s) made a fade of %.3f s\n",
			    SEND(double, animation, "duration"), object_getClassName(animation), cc_fade_s);
		}
		animation = fade;
	}
	original_add_animation_cc(self, selector, animation, key);
}

static void
control_center_fades(const char *arguments)
{
	const char *setting = strstr(arguments, "cc_fade=");
	Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));

	if (setting != NULL) {
		cc_fade_s = atof(setting + 8) / 1000.0;
	}
	if (cc_fade_s <= 0 || add == NULL) {
		return;
	}
	original_add_animation_cc = (void (*)(id, SEL, id, id))method_getImplementation(add);
	method_setImplementation(add, (IMP)add_animation_cc);
}

/*
 * Open the tweak loader of the package manager's prefix, when there is one. Nothing here depends on
 * it: without a prefix (or without ElleKit in it) the file is not there and this does nothing.
 */
#define HAKIOS_TWEAK_LOADER    "/redwood/lib/libhakios.tweaks.dylib"

static void
load_tweaks(void)
{
	say("hakios: tweaks: disabled for early boot\n");
	return;
}

/* ---- startup milestones: the last one in the log is where SpringBoard stops ---- */

#define HOOK_INIT(clsname)                                                         \
	static id (*orig_init_##clsname)(id, SEL);                                     \
	static id log_init_##clsname(id self, SEL sel)                                 \
	{                                                                              \
		say("hakios: SB milestone: -[%s init]\n", #clsname);                     \
		return orig_init_##clsname(self, sel);                                      \
	}                                                                              \
	static void hook_init_##clsname(Class c)                                       \
	{                                                                              \
		Method m = c ? class_getInstanceMethod(c, sel_registerName("init")) : NULL; \
		if (m != NULL) {                                                            \
			orig_init_##clsname = (void *)method_getImplementation(m);               \
			method_setImplementation(m, (IMP)log_init_##clsname);                    \
		}                                                                          \
	}

#define HOOK_VOID(clsname, selname)                                                \
	static void (*orig_void_##clsname)(id, SEL);                                    \
	static void log_void_##clsname(id self, SEL sel)                                \
	{                                                                              \
		say("hakios: SB milestone: -[%s %s]\n", #clsname, #selname);               \
		if (orig_void_##clsname != NULL) { orig_void_##clsname(self, sel); }        \
	}                                                                              \
	static void hook_void_##clsname(Class c)                                        \
	{                                                                              \
		Method m = c ? class_getInstanceMethod(c, sel_registerName(#selname)) : NULL; \
		if (m != NULL) {                                                            \
			orig_void_##clsname = (void *)method_getImplementation(m);               \
			method_setImplementation(m, (IMP)log_void_##clsname);                    \
		}                                                                          \
	}

HOOK_INIT(SBUIController)
HOOK_INIT(SBApplicationController)
HOOK_INIT(SBLockScreenManager)
HOOK_INIT(SBWallpaperController)
HOOK_INIT(SBMainWorkspace)
HOOK_INIT(SBUserSessionController)
HOOK_VOID(SBApplicationController, _loadApplications)
HOOK_VOID(SBUIController, _bootComplete)


/* ---- where it stops, asked of its own threads ------------------------------------------------ */

static const char *
image_of(uintptr_t address, uintptr_t *offset)
{
	uint32_t count = _dyld_image_count();
	uint32_t i;

	for (i = 0; i < count; i++) {
		const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(i);
		intptr_t aslide = _dyld_get_image_vmaddr_slide(i);
		const struct load_command *command;
		uint32_t j;

		if (header == NULL || header->magic != MH_MAGIC_64) {
			continue;
		}
		command = (const struct load_command *)(header + 1);
		for (j = 0; j < header->ncmds && command->cmdsize >= sizeof(*command); j++) {
			if (command->cmd == LC_SEGMENT_64) {
				const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
				uintptr_t base = (uintptr_t)segment->vmaddr + (uintptr_t)aslide;

				if (address >= base && address < base + (uintptr_t)segment->vmsize) {
					*offset = address - base;
					return _dyld_get_image_name(i);
				}
			}
			command = (const struct load_command *)((const char *)command + command->cmdsize);
		}
	}
	return NULL;
}

/*
 * The nearest exported name at or before an address: dladdr reads the image's dynamic symbols, so a
 * private function of a framework shows up as the public one it lies after, with the distance --
 * enough to say "the wallpapers' code" or "CoreGraphics' image source" from a bare offset.
 */
static void
say_near(unsigned index, const char *what, uintptr_t address)
{
	Dl_info info;

	if (address != 0 && dladdr((void *)address, &info) != 0 && info.dli_sname != NULL) {
		say("hakios: Sample t%u   %s near %s+%lu\n", index, what, info.dli_sname,
		    (unsigned long)(address - (uintptr_t)info.dli_saddr));
	}
}

static void
sample_one(unsigned index, thread_t thread)
{
	arm_thread_state64_t state;
	mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
	struct thread_basic_info basic;
	mach_msg_type_number_t basic_count = THREAD_BASIC_INFO_COUNT;
	int run_state = -1;
	uintptr_t offset;
	const char *image;
	unsigned frame;
	uintptr_t fp;

	if (thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&basic, &basic_count) == KERN_SUCCESS) {
		run_state = basic.run_state;
	}
	if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &state_count) != KERN_SUCCESS) {
		say("hakios: Sample t%u: no state (run %d)\n", index, run_state);
		return;
	}
	fp = (uintptr_t)arm_thread_state64_get_fp(state);
	image = image_of((uintptr_t)arm_thread_state64_get_pc(state), &offset);
	if (image != NULL) {
		say("hakios: Sample t%u run %d pc %s+%#lx\n", index, run_state, image, (unsigned long)offset);
	} else {
		say("hakios: Sample t%u run %d pc %#lx\n", index, run_state,
		    (unsigned long)arm_thread_state64_get_pc(state));
	}
	say_near(index, "pc", (uintptr_t)arm_thread_state64_get_pc(state));
	image = image_of((uintptr_t)arm_thread_state64_get_lr(state), &offset);
	if (image != NULL) {
		say("hakios: Sample t%u   lr %s+%#lx\n", index, image, (unsigned long)offset);
	}
	say_near(index, "lr", (uintptr_t)arm_thread_state64_get_lr(state));
	for (frame = 0; frame < 8 && fp != 0 && (fp & 7) == 0; frame++) {
		struct { uintptr_t next, ret; } pair;
		vm_size_t got = 0;

		if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(pair),
		    (vm_address_t)&pair, &got) != KERN_SUCCESS || got != sizeof(pair)) {
			break;
		}
		if (pair.ret == 0) {
			break;
		}
		image = image_of(pair.ret, &offset);
		if (image != NULL) {
			say("hakios: Sample t%u   from %s+%#lx\n", index, image, (unsigned long)offset);
		}
		say_near(index, "from", pair.ret);
		if (pair.next <= fp) {
			break;
		}
		fp = pair.next;
	}
}

/*
 * Boot-arg sb_layers=N[,M]: the layer tree of SpringBoard is written down at those seconds (the
 * sampler's own times: 15, 40, 70, 110, 170, 280).  The screen stays black through a whole run while
 * the controllers are all made -- this says whether there is a window to show at all, and whether
 * the user counts as authenticated, without a Darwin notification from the host.
 */
static void
layers_at(int seconds)
{
	char arguments[2048];
	size_t size = sizeof(arguments) - 1;
	const char *at;
	int first = 0, second = 0;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "sb_layers=");
	if (at == NULL) {
		return;
	}
	first = (int)strtol(at + 10, NULL, 10);
	{
		char *end = NULL;

		second = (int)strtol(at + 10, &end, 10);
		if (end != NULL && *end == ',') {
			second = (int)strtol(end + 1, NULL, 10);
		}
	}
	if (first != seconds && second != seconds) {
		return;
	}
	say("hakios: layers: asked at %d s\n", seconds);
	/*
	 * On the main thread: p218 called this from the sampler thread and SpringBoard died of
	 * SIGTRAP (EXC_BREAKPOINT) right there, with four windows already counted -- the window
	 * server's objects are not to be read off the main thread.
	 */
	dispatch_async(dispatch_get_main_queue(), ^{
		layers();
	});
}

static void
sample_threads(int seconds)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	mach_port_t me = mach_task_self();
	unsigned i;

	say("hakios: Sample: %d s after start\n", seconds);
	say_apfs_stats("at the sample");
	if (task_threads(me, &threads, &count) != KERN_SUCCESS) {
		say("hakios: Sample: task_threads refused\n");
		return;
	}
	for (i = 0; i < count; i++) {
		sample_one(i, threads[i]);
	}
	for (i = 0; i < count; i++) {
		mach_port_deallocate(me, threads[i]);
	}
	vm_deallocate(me, (vm_address_t)threads, count * sizeof(threads[0]));
}

static void *
sampler(void *unused)
{
	static const int at[] = { 15, 40, 70, 110, 170, 280 };
	unsigned i;
	int now = 0;

	(void)unused;
	for (i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
		sleep((unsigned)(at[i] - now));
		now = at[i];
		sample_threads(now);
		layers_at(now);
	}
	return NULL;
}

static void
loud_limit_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;
	const char *at;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "sb_loud=");
	if (at != NULL) {
		unsigned long n = strtoul(at + 8, NULL, 10);

		if (n >= 250 && n <= 100000) {
			xpc_loud_limit = (unsigned)n;
			say("hakios: xpc: the first %lu requests are written down with their time\n", n);
		}
	}
}

static void
sample_watchdog(void)
{
	pthread_t th;

	if (pthread_create(&th, NULL, sampler, NULL) == 0) {
		pthread_detach(th);
	}
}


/* ---- what it waits for, by watching the calls that can wait ---------------------------------- */

static int xpc_watched;
static unsigned xpc_count;

static unsigned long long
millis_now(void)
{
	static mach_timebase_info_data_t base;
	uint64_t now = mach_absolute_time();

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return (unsigned long long)(now * base.numer / base.denom / 1000000ULL);
}

static const char *
peer_of(xpc_connection_t_sb connection, char *text, size_t size)
{
	int pid = xpc_connection_get_pid(connection);
	const char *name = xpc_connection_get_name(connection);

	if (name != NULL) {
		snprintf(text, size, "%s", name);
	} else if (pid > 0) {
		char program[64] = "";

		proc_name(pid, program, sizeof(program));
		snprintf(text, size, "(no name) pid %d %s", pid, program);
	} else {
		snprintf(text, size, "(no name) pid 0");
	}
	return text;
}

static void
say_frames(const char *what, uintptr_t frame, unsigned depth)
{
	unsigned i;

	for (i = 0; i < depth && frame != 0 && (frame & 7) == 0; i++) {
		struct { uintptr_t next, ret; } pair;
		vm_size_t got = 0;
		uintptr_t offset = 0;
		const char *image;

		if (vm_read_overwrite(mach_task_self(), (vm_address_t)frame, sizeof(pair),
		    (vm_address_t)&pair, &got) != KERN_SUCCESS || got != sizeof(pair)) {
			break;
		}
		if (pair.ret == 0) {
			break;
		}
		image = image_of(pair.ret, &offset);
		if (image != NULL) {
			say("hakios: %s   #%u %s+%#lx\n", what, i, image, (unsigned long)offset);
		}
		if (pair.next <= frame) {
			break;
		}
		frame = pair.next;
	}
}

/* ---- unfair locks: who holds the one the main thread waits for ------------------------------- */

/*
 * SpringBoard's main thread spends its life in __ulock_wait inside CoreServices: an unfair lock
 * of LaunchServices is taken by another thread and never given back (p133, p188, p192).  A sample
 * of the process says the main thread waits; it says nothing about the owner, because the owner
 * is between two syscalls at the moment of the sample.  So the lock itself is watched: every
 * acquire is recorded with the thread that owns it, a wait longer than half a second is said with
 * both stacks, and a lock held longer than three seconds is said by a watchdog even when nobody
 * is waiting on it yet.
 */
static void (*orig_lock)(os_unfair_lock_t);
static void (*orig_unlock)(os_unfair_lock_t);

struct lock_slot {
	os_unfair_lock_t lock;
	pthread_t owner;
	unsigned long long since;
	int reported;
	unsigned held;
};

#define LOCK_SLOTS 256
static struct lock_slot lock_slots[LOCK_SLOTS];
static pthread_mutex_t lock_slots_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
lock_slot_take(os_unfair_lock_t lock, pthread_t owner)
{
	unsigned i;
	int free_slot = -1;

	pthread_mutex_lock(&lock_slots_mutex);
	for (i = 0; i < LOCK_SLOTS; i++) {
		if (lock_slots[i].lock == lock) {
			free_slot = (int)i;
			break;
		}
		if (free_slot < 0 && lock_slots[i].lock == NULL) {
			free_slot = (int)i;
		}
	}
	if (free_slot >= 0) {
		if (lock_slots[free_slot].lock == lock) {
			lock_slots[free_slot].held++;
		} else {
			lock_slots[free_slot].held = 1;
		}
		lock_slots[free_slot].lock = lock;
		lock_slots[free_slot].owner = owner;
		lock_slots[free_slot].since = millis_now();
		lock_slots[free_slot].reported = 0;
	}
	pthread_mutex_unlock(&lock_slots_mutex);
}

static void
lock_slot_clear(os_unfair_lock_t lock)
{
	unsigned i;

	pthread_mutex_lock(&lock_slots_mutex);
	for (i = 0; i < LOCK_SLOTS; i++) {
		if (lock_slots[i].lock == lock) {
			if (lock_slots[i].held > 1) {
				lock_slots[i].held--;
			} else {
				lock_slots[i].lock = NULL;
				lock_slots[i].owner = NULL;
				lock_slots[i].since = 0;
				lock_slots[i].reported = 0;
				lock_slots[i].held = 0;
			}
		}
	}
	pthread_mutex_unlock(&lock_slots_mutex);
}

static pthread_t
lock_slot_owner(os_unfair_lock_t lock, unsigned long long *since)
{
	pthread_t owner = NULL;
	unsigned i;

	pthread_mutex_lock(&lock_slots_mutex);
	for (i = 0; i < LOCK_SLOTS; i++) {
		if (lock_slots[i].lock == lock) {
			owner = lock_slots[i].owner;
			if (since != NULL) {
				*since = lock_slots[i].since;
			}
			break;
		}
	}
	pthread_mutex_unlock(&lock_slots_mutex);
	return owner;
}

static void
say_thread_stack(const char *tag, pthread_t who, unsigned depth)
{
	arm_thread_state64_t state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	thread_t thread;
	uintptr_t fp, offset;
	const char *image;
	unsigned frame;

	if (who == NULL) {
		say("hakios: lock %s: no thread\n", tag);
		return;
	}
	thread = pthread_mach_thread_np(who);
	if (thread == THREAD_NULL ||
	    thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) {
		say("hakios: lock %s: no state\n", tag);
		return;
	}
	image = image_of((uintptr_t)arm_thread_state64_get_pc(state), &offset);
	if (image != NULL) {
		say("hakios: lock %s: pc %s+%#lx", tag, image, (unsigned long)offset);
	} else {
		say("hakios: lock %s: pc %#lx", tag, (unsigned long)arm_thread_state64_get_pc(state));
	}
	image = image_of((uintptr_t)arm_thread_state64_get_lr(state), &offset);
	if (image != NULL) {
		say(" lr %s+%#lx\n", image, (unsigned long)offset);
	} else {
		say(" lr %#lx\n", (unsigned long)arm_thread_state64_get_lr(state));
	}
	fp = (uintptr_t)arm_thread_state64_get_fp(state);
	for (frame = 0; frame < depth && fp != 0 && (fp & 7) == 0; frame++) {
		struct { uintptr_t next, ret; } pair;
		vm_size_t got = 0;

		if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(pair),
		    (vm_address_t)&pair, &got) != KERN_SUCCESS || got != sizeof(pair) ||
		    pair.ret == 0) {
			break;
		}
		image = image_of(pair.ret, &offset);
		if (image != NULL) {
			say("hakios: lock %s:   from %s+%#lx\n", tag, image, (unsigned long)offset);
		} else {
			say("hakios: lock %s:   from %#lx\n", tag, (unsigned long)pair.ret);
		}
		if (pair.next <= fp) {
			break;
		}
		fp = pair.next;
	}
}

static void
said_lock_wait(os_unfair_lock_t lock, unsigned long long started, pthread_t previous)
{
	unsigned long long waited = millis_now() - started;

	if (waited < 500) {
		return;
	}
	say("hakios: lock %p: waited %llu ms\n", lock, waited);
	say_frames("waiter", (uintptr_t)__builtin_frame_address(0), 10);
	if (previous != NULL) {
		say_thread_stack("owner", previous, 12);
	}
}

static void
hk_lock(os_unfair_lock_t lock)
{
	unsigned long long t0 = millis_now();
	pthread_t previous = lock_slot_owner(lock, NULL);

	orig_lock(lock);
	lock_slot_take(lock, pthread_self());
	said_lock_wait(lock, t0, previous);
}

static void
hk_unlock(os_unfair_lock_t lock)
{
	lock_slot_clear(lock);
	orig_unlock(lock);
}

#define LOCK_INTERPOSE(ours, theirs)                                                     \
	__attribute__((used)) static struct {                                             \
		const void *replacement, *replacee;                                        \
	} lock_interpose_##theirs __attribute__((section("__DATA,__interpose"))) = {     \
		(const void *)(ours), (const void *)(theirs)                               \
	}

LOCK_INTERPOSE(hk_lock, os_unfair_lock_lock);
LOCK_INTERPOSE(hk_unlock, os_unfair_lock_unlock);

/*
 * The interpose above could not be installed (libsystem_platform's symbols are not to be had
 * through dlsym in this port: RTLD_NEXT answers with our own hook and the library is not
 * openable by path).  So the lock is read instead of watched: the main thread, when it waits,
 * is inside __ulock_wait with the address of the lock word in x0, and the word itself says which
 * thread owns it (the contended value is (thread id << 1) | 1).  Every 250 ms the main thread is
 * looked at; when it is on a lock, the owner is found among the process's threads and its stack
 * is printed -- that is the answer to "who holds LaunchServices' lock".
 */
static thread_t main_thread_port;
static uint64_t main_thread_tid;

static void
lock_sampler_start(void)
{
	main_thread_port = mach_thread_self();
	if (pthread_threadid_np(NULL, &main_thread_tid) != 0) {
		main_thread_tid = 0;
	}
}

static uint64_t
tid_of(thread_t thread)
{
	struct thread_identifier_info info;
	mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;

	if (thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) {
		return 0;
	}
	return info.thread_id;
}

static void
say_owner_of(uint64_t tid, uint32_t word, uintptr_t lock_address)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	unsigned i;

	say("hakios: lock: main waits on %p (word %#x, owner tid %llu)\n", (void *)lock_address,
	    word, (unsigned long long)tid);
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	for (i = 0; i < count; i++) {
		if (tid_of(threads[i]) == tid) {
			{
				arm_thread_state64_t state;
				mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
				uintptr_t fp, offset;
				const char *image;
				unsigned frame;

				if (thread_get_state(threads[i], ARM_THREAD_STATE64,
				    (thread_state_t)&state, &state_count) == KERN_SUCCESS) {
					image = image_of((uintptr_t)arm_thread_state64_get_pc(state),
					    &offset);
					say("hakios: lock owner: pc %s+%#lx\n",
					    image != NULL ? image : "?",
					    image != NULL ? (unsigned long)offset : 0UL);
					fp = (uintptr_t)arm_thread_state64_get_fp(state);
					for (frame = 0; frame < 14 && fp != 0 && (fp & 7) == 0;
					    frame++) {
						struct { uintptr_t next, ret; } pair;
						vm_size_t got = 0;

						if (vm_read_overwrite(mach_task_self(),
						    (vm_address_t)fp, sizeof(pair),
						    (vm_address_t)&pair,
						    &got) != KERN_SUCCESS || got != sizeof(pair) ||
						    pair.ret == 0) {
							break;
						}
						image = image_of(pair.ret, &offset);
						if (image != NULL) {
							say("hakios: lock owner:   from %s+%#lx\n",
							    image, (unsigned long)offset);
						}
						if (pair.next <= fp) {
							break;
						}
						fp = pair.next;
					}
				}
			}
			break;
		}
	}
	for (i = 0; i < count; i++) {
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]));
}

static void *
lock_sampler(void *unused)
{
	uintptr_t last_lock = 0;
	unsigned long long last_said = 0;
	unsigned said = 0;

	(void)unused;
	for (;;) {
		arm_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		uintptr_t pc, x0;
		const char *image;
		uintptr_t offset = 0;

		usleep(250000);
		if (main_thread_port == THREAD_NULL) {
			continue;
		}
		if (thread_get_state(main_thread_port, ARM_THREAD_STATE64, (thread_state_t)&state,
		    &count) != KERN_SUCCESS) {
			continue;
		}
		pc = (uintptr_t)arm_thread_state64_get_pc(state);
		image = image_of(pc, &offset);
		if (image == NULL || strstr(image, "libsystem_kernel") == NULL || offset < 0x1a00 ||
		    offset > 0x1b00) {
			continue;                       /* not waiting on an unfair lock */
		}
		x0 = (uintptr_t)state.__x[0];
		if (x0 < 0x100000000ULL || (x0 & 3) != 0) {
			continue;
		}
		{
			uint32_t word = *(volatile uint32_t *)x0;

			if (word == 0 || word == 1) {
				continue;
			}
			if (x0 == last_lock && millis_now() - last_said < 5000) {
				continue;
			}
			if (said > 40) {
				continue;
			}
			said++;
			last_lock = x0;
			last_said = millis_now();
			say_owner_of((uint64_t)(word >> 1), word, x0);
		}
	}
	return NULL;
}

static void *
lock_watchdog(void *unused)
{
	(void)unused;
	for (;;) {
		unsigned i;

		sleep(1);
		for (i = 0; i < LOCK_SLOTS; i++) {
			pthread_t owner = NULL;
			unsigned long long since = 0, now;
			os_unfair_lock_t lock = NULL;

			now = millis_now();
			pthread_mutex_lock(&lock_slots_mutex);
			if (lock_slots[i].lock != NULL && lock_slots[i].owner != NULL &&
			    !lock_slots[i].reported && now > lock_slots[i].since &&
			    now - lock_slots[i].since >= 3000) {
				lock_slots[i].reported = 1;
				owner = lock_slots[i].owner;
				since = lock_slots[i].since;
				lock = lock_slots[i].lock;
			}
			pthread_mutex_unlock(&lock_slots_mutex);
			if (owner != NULL) {
				say("hakios: lock %p held for %llu ms\n", lock, now - since);
				say_thread_stack("holder", owner, 12);
			}
		}
	}
	return NULL;
}

/* A symbol of one named library: dlsym(RTLD_NEXT) can answer with our own hook (p187), and a
   lock that calls itself is a deadlock, so the library is opened by name as a fallback. */
static void *
symbol_of(const char *library, const char *name, const void *ours)
{
	void *found = dlsym(RTLD_NEXT, name);

	if (found != ours && found != NULL) {
		return found;
	}
	{
		void *handle = dlopen(library, RTLD_NOW | RTLD_NOLOAD);

		if (handle != NULL) {
			found = dlsym(handle, name);
			if (found != ours) {
				return found;
			}
		}
	}
	return NULL;
}

static void
lock_watchdogs(void)
{
	pthread_t th;

	orig_lock = (void (*)(os_unfair_lock_t))symbol_of("/usr/lib/system/libsystem_platform.dylib",
	    "os_unfair_lock_lock", (const void *)hk_lock);
	orig_unlock = (void (*)(os_unfair_lock_t))symbol_of("/usr/lib/system/libsystem_platform.dylib",
	    "os_unfair_lock_unlock", (const void *)hk_unlock);
	say("hakios: locks: lock %s, unlock %s\n", orig_lock != NULL ? "watched" : "NOT found",
	    orig_unlock != NULL ? "watched" : "NOT found");
	lock_sampler_start();
	if (pthread_create(&th, NULL, lock_sampler, NULL) == 0) {
		pthread_detach(th);
	}
	if (orig_lock == NULL || orig_unlock == NULL) {
		return;
	}
	if (pthread_create(&th, NULL, lock_watchdog, NULL) == 0) {
		pthread_detach(th);
	}
}

/*
 * sb_nostore2=1: the request SpringBoard makes for LaunchServices' server store, sent to an
 * anonymous endpoint of lsd, is answered here at once with "the connection is invalid" instead
 * of being sent -- that wait never ends on this port (see collab.md), and holding it is what
 * keeps the first controller from being made.  The point is to see whether the rest comes up
 * without the store.
 */
static int nostore2;
static xpc_object_t_sb nostore2_error;

/*
 * The Do Not Disturb query that never comes back.  p220/p221: the main thread waits in a
 * synchronous XPC from BoardServices+0x3128 to an endpoint with no name and pid 0, and the message
 * asks "queryStateWithRequestDetails:completionHandler:" (DoNotDisturb.framework, called from
 * UserNotificationsUIKit during the lock screen's setup).  The endpoint's process does not answer
 * -- the same shape as the LaunchServices store in p210 -- so boot-arg sb_nodnd=1 cancels that
 * connection before the send, and DND is taken for off (it is off: nothing else answered either).
 */
static int nodnd;
static unsigned nodnd_cancelled;

static int
is_dnd_request(xpc_object_t_sb message)
{
	char *text;
	int found;

	if (message == NULL) {
		return 0;
	}
	text = xpc_copy_description(message);
	found = text != NULL && strstr(text, "queryStateWithRequestDetails") != NULL;
	free(text);
	return found;
}

/*
 * Boot-arg sb_noanon=1: an endpoint with no name and no process (a listener endpoint handed over
 * by somebody that is gone) is answered at once with a nil reply instead of being waited for.  p225
 * shows why: with the Do Not Disturb query answered, the same BoardServices sync call keeps coming
 * back on other such endpoints -- ten of the eight-second timeouts in one run, requests numbered
 * past 1500 -- and the main thread never gets past them.
 */
static int noanon;
static unsigned noanon_count;

static void
noanon_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, "sb_noanon=1") != NULL) {
		noanon = 1;
		say("hakios: xpc: requests to anonymous endpoints are answered at once\n");
	}
}

static void
nodnd_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, "sb_nodnd=1") != NULL) {
		nodnd = 1;
		say("hakios: dnd: the Do Not Disturb state query is cancelled, not waited for\n");
	}
}

static void xpc_connection_cancel(xpc_connection_t_sb connection);

/*
 * Boot-arg sb_xpc_timeout=N: a synchronous XPC call that has not come back in N seconds has its
 * connection cancelled, so the caller gets "connection invalid" and goes on.
 *
 * p222/p223 made the shape of the remaining walls clear: each one is such a call.  First the Do
 * Not Disturb query on a dead anonymous endpoint (answered at once now, sb_nodnd), then cfprefsd --
 * which answers every other process in tens of milliseconds while SpringBoard's own request is
 * never answered.  This is a blunt instrument for finding out what is behind each of them, not
 * something for a working boot.
 *
 * The waits are kept in a small table and one thread walks it: a job allocated per call and freed
 * by the timer was written to by the caller after the timer had freed it, and that heap corruption
 * is what killed SpringBoard with SIGABRT in p224 (the frames were libmalloc under libxpc).
 */
#define SB_WAIT_SLOTS 16

struct sb_wait {
	xpc_connection_t_sb connection;
	unsigned number;
	unsigned long long began;
};

static struct sb_wait sb_waits[SB_WAIT_SLOTS];
static pthread_mutex_t sb_waits_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned sb_xpc_timeout;

static int
sb_wait_begin(xpc_connection_t_sb connection, unsigned number)
{
	int slot = -1;
	int i;

	pthread_mutex_lock(&sb_waits_lock);
	for (i = 0; i < SB_WAIT_SLOTS; i++) {
		if (sb_waits[i].connection == NULL) {
			sb_waits[i].connection = connection;
			sb_waits[i].number = number;
			sb_waits[i].began = millis_now();
			slot = i;
			break;
		}
	}
	pthread_mutex_unlock(&sb_waits_lock);
	return slot;
}

static void
sb_wait_end(int slot)
{
	if (slot < 0 || slot >= SB_WAIT_SLOTS) {
		return;
	}
	pthread_mutex_lock(&sb_waits_lock);
	sb_waits[slot].connection = NULL;
	pthread_mutex_unlock(&sb_waits_lock);
}

static void *
sb_wait_watch(void *unused)
{
	(void)unused;
	for (;;) {
		int i;

		usleep(250 * 1000);
		pthread_mutex_lock(&sb_waits_lock);
		for (i = 0; i < SB_WAIT_SLOTS; i++) {
			if (sb_waits[i].connection != NULL &&
			    millis_now() - sb_waits[i].began >= (unsigned long long)sb_xpc_timeout * 1000) {
				say("hakios: xpc %u: %u s and no answer: the connection is cancelled\n",
				    sb_waits[i].number, sb_xpc_timeout);
				xpc_connection_cancel(sb_waits[i].connection);
				sb_waits[i].connection = NULL;
			}
		}
		pthread_mutex_unlock(&sb_waits_lock);
	}
	return NULL;
}

static void
sb_xpc_timeout_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;
	const char *at;
	pthread_t watcher;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "sb_xpc_timeout=");
	if (at == NULL) {
		return;
	}
	sb_xpc_timeout = (unsigned)strtoul(at + 15, NULL, 10);
	if (sb_xpc_timeout != 0 && sb_xpc_timeout < 5) {
		sb_xpc_timeout = 5;
	}
	if (sb_xpc_timeout == 0) {
		return;
	}
	say("hakios: xpc: a call with no answer is cancelled after %u s\n", sb_xpc_timeout);
	if (pthread_create(&watcher, NULL, sb_wait_watch, NULL) == 0) {
		pthread_detach(watcher);
	}
}

/*
 * Boot-arg sb_store_wait=N: when SpringBoard asks for the server store, it is first waited for N
 * seconds -- once -- and then asked as usual.  p230 (the request not cancelled) sat on the apple for
 * the whole run: lsd is given its container by lsd_delay, and by the time SpringBoard asks, the store
 * may still be being made.  Asking later is the one difference that has not been tried.
 */
static unsigned store_wait;
static int store_waited;

static void
store_wait_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;
	const char *at;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "sb_store_wait=");
	if (at == NULL) {
		return;
	}
	store_wait = (unsigned)strtoul(at + 14, NULL, 10);
	if (store_wait == 0 || store_wait > 120) {
		store_wait = 0;
		return;
	}
	say("hakios: store: the first request waits %u s before it is sent\n", store_wait);
}

static int
is_store_request(xpc_object_t_sb message)
{
	static const char needle[] = "ServerStore";
	xpc_object_t_sb root;
	const char *bytes;
	size_t length, i;
	char *what;
	int found = 0;

	if (message == NULL) {
		return 0;
	}
	root = xpc_dictionary_get_value(message, "root");
	if (root != NULL) {
		bytes = xpc_data_get_bytes_ptr(root);
		length = xpc_data_get_length(root);
		/* (the archive has NULs in it: walk the bytes, not strstr) */
		for (i = 0; bytes != NULL && i + sizeof(needle) - 1 <= length; i++) {
			if (memcmp(bytes + i, needle, sizeof(needle) - 1) == 0) {
				found = 1;
				break;
			}
		}
	}
	if (found) {
		return 1;
	}
	what = xpc_copy_description(message);
	found = what != NULL && strstr(what, "ServerStore") != NULL;
	free(what);
	return found;
}

static void
xpc_connection_cancel(xpc_connection_t_sb connection);

static xpc_object_t_sb
nostore2_error_object(void)
{
	static const char *const names[] = {
		"XPC_ERROR_CONNECTION_INVALID", "_XPC_ERROR_CONNECTION_INVALID",
		"xpc_error_connection_invalid", "_xpc_error_connection_invalid",
	};
	unsigned i;

	if (nostore2_error != NULL) {
		return nostore2_error;
	}
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		nostore2_error = (xpc_object_t_sb)dlsym(RTLD_DEFAULT, names[i]);
		if (nostore2_error != NULL) {
			say("hakios: store: the error object is %s\n", names[i]);
			return nostore2_error;
		}
	}
	return NULL;
}

static xpc_object_t_sb
watched_sync(xpc_connection_t_sb connection, xpc_object_t_sb message)
{
	char peer[96];
	unsigned number;
	unsigned long long started;
	xpc_object_t_sb reply;
	int loud;

	if (noanon && xpc_connection_get_name(connection) == NULL &&
	    xpc_connection_get_pid(connection) <= 0) {
		char *body = xpc_copy_description(message);
		const char *at = body != NULL ? strstr(body, "bsxpc_SEL") : NULL;

		if (__sync_add_and_fetch(&noanon_count, 1) <= 100) {
			say("hakios: xpc: anonymous, answered at once: %s\n",
			    at != NULL ? at : "(no selector)");
		}
		free(body);
		xpc_connection_cancel(connection);
		return NULL;
	}
	if (nodnd && xpc_connection_get_name(connection) == NULL &&
	    xpc_connection_get_pid(connection) <= 0 && is_dnd_request(message)) {
		/*
		 * Nothing is sent: cancelling alone was not enough in p222 (requests 430 and 432
		 * were cancelled and the next one, 433, was sent on the same endpoint and waited
		 * for ever), so the caller gets a nil reply at once, which is what a dead
		 * connection gives it anyway.
		 */
		if (__sync_add_and_fetch(&nodnd_cancelled, 1) <= 20) {
			say("hakios: dnd: the Do Not Disturb query is answered at once (%u so far)\n",
			    nodnd_cancelled);
		}
		xpc_connection_cancel(connection);
		return NULL;
	}
	if (!nostore2 && store_wait != 0 && !store_waited && is_store_request(message)) {
		store_waited = 1;
		say("hakios: store: waiting %u s before the first request is sent\n", store_wait);
		sleep(store_wait);
	}
	if (nostore2 && xpc_connection_get_name(connection) == NULL && is_store_request(message)) {
		xpc_object_t_sb error = nostore2_error_object();

		if (error != NULL) {
			say("hakios: store: the server store request answered at once\n");
			return error;
		}
		/*
		 * No error object to hand back: the connection is cancelled instead, so libxpc
		 * answers the send with its own "connection invalid" right away.
		 */
		say("hakios: store: the server store request: the connection is cancelled\n");
		xpc_connection_cancel(connection);
	}
	if (!xpc_watched) {
		return xpc_connection_send_message_with_reply_sync(connection, message);
	}
	peer_of(connection, peer, sizeof(peer));
	number = __sync_add_and_fetch(&xpc_count, 1);
	loud = number <= xpc_loud_limit;
	/*
	 * The name of the peer, one line, for every request: p214..p219 the main thread waits
	 * in a synchronous XPC from BoardServices, and after the first 250 requests the peer was
	 * not said at all -- the one being waited on was always further down.
	 */
	if (!loud && number <= 1500) {
		uintptr_t at = 0;
		const char *who = image_of((uintptr_t)__builtin_return_address(0), &at);

		say("hakios: xpc %u: -> %s from %s+%#lx\n", number, peer,
		    who != NULL ? who : "?", (unsigned long)at);
	}
	/*
	 * An anonymous connection -- a listener endpoint given to us by another process, no name,
	 * no pid -- never answers: p220, requests 430 and 431 from BoardServices+0x3128, and the
	 * main thread waits there while the lock screen never shows.  What is being asked for is
	 * in the message: said here for the first twelve of them.
	 */
	if (number <= 1500 && xpc_connection_get_name(connection) == NULL &&
	    xpc_connection_get_pid(connection) <= 0) {
		static unsigned anonymous;

		if (__sync_add_and_fetch(&anonymous, 1) <= 12) {
			char *body = xpc_copy_description(message);

			say("hakios: xpc %u: anonymous, message %s\n", number,
			    body != NULL ? body : "(nothing)");
			free(body);
			say_frames("call", (uintptr_t)__builtin_frame_address(0), 10);
		}
	}
	if (loud) {
		uintptr_t offset = 0;
		const char *image = image_of((uintptr_t)__builtin_return_address(0), &offset);
		char *what = xpc_copy_description(connection);
		char *body = xpc_copy_description(message);

		say("hakios: xpc %u: -> %s (%p) from %s+%#lx\n     object %s\n     message %s\n",
		    number, peer, connection, image != NULL ? image : "?", (unsigned long)offset,
		    what != NULL ? what : "(nothing)", body != NULL ? body : "(nothing)");
		say_frames("call", (uintptr_t)__builtin_frame_address(0), 12);
		say_graph("request", message);
		free(what);
		free(body);
	}
	started = millis_now();
	{
		int slot = -1;

		if (sb_xpc_timeout != 0) {
			slot = sb_wait_begin(connection, number);
		}
		reply = xpc_connection_send_message_with_reply_sync(connection, message);
		sb_wait_end(slot);
	}
	if (loud || millis_now() - started > 900) {
		say("hakios: xpc %u: <- %s in %llu ms%s\n", number, peer, millis_now() - started,
		    reply == NULL ? " (nothing)" : "");
		if (millis_now() - started > 500 && reply != NULL) {
			char *said = xpc_copy_description(reply);

			say("hakios: xpc %u:   reply %s\n", number, said != NULL ? said : "(nothing)");
			free(said);
			say_graph("answer", reply);
		}
	}
	return reply;
}

static void
watched_async(xpc_connection_t_sb connection, xpc_object_t_sb message, dispatch_queue_t queue,
    xpc_handler_t_sb handler)
{
	char peer[96];
	unsigned number;
	unsigned long long started;
	int loud;

	if (!xpc_watched) {
		xpc_connection_send_message_with_reply(connection, message, queue, handler);
		return;
	}
	char *peer_copy;

	peer_of(connection, peer, sizeof(peer));
	peer_copy = strdup(peer);
	number = __sync_add_and_fetch(&xpc_count, 1);
	loud = number <= xpc_loud_limit;
	if (loud) {
		say("hakios: xpc %u: -> %s (async, connection %p)\n", number, peer_copy, connection);
	}
	started = millis_now();
	xpc_connection_send_message_with_reply(connection, message, queue, ^(xpc_object_t_sb reply) {
		if (loud || millis_now() - started > 900) {
			say("hakios: xpc %u: <- %s (async) in %llu ms%s\n", number,
			    peer_copy != NULL ? peer_copy : "(no name)", millis_now() - started,
			    reply == NULL ? " (nothing)" : "");
		}
		if (handler != NULL) {
			handler(reply);
		}
	});
}

static xpc_connection_t_sb
watched_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags)
{
	xpc_connection_t_sb connection = xpc_connection_create_mach_service(name, queue, flags);

	say("hakios: xpc: connect to %s -> %p\n", name != NULL ? name : "(no name)", connection);
	return connection;
}

static xpc_connection_t_sb
watched_create(const char *name, dispatch_queue_t queue)
{
	xpc_connection_t_sb connection = xpc_connection_create(name, queue);

	say("hakios: xpc: create %s -> %p\n", name != NULL ? name : "(no name)", connection);
	return connection;
}

static void
watched_send_message(xpc_connection_t_sb connection, xpc_object_t_sb message)
{
	const char *name = xpc_connection_get_name(connection);

	if (xpc_watched && __sync_add_and_fetch(&xpc_count, 1) <= xpc_loud_limit) {
		say("hakios: xpc: send to %s (no reply wanted)\n", name != NULL ? name : "(no name)");
	}
	xpc_connection_send_message(connection, message);
}

#define HAKIOS_INTERPOSE(ours, theirs)                                                \
	__attribute__((used)) static struct {                                          \
		const void *replacement, *replacee;                                     \
	} interpose_##theirs __attribute__((section("__DATA,__interpose"))) = {       \
		(const void *)(ours), (const void *)(theirs)                            \
	}

HAKIOS_INTERPOSE(watched_sync, xpc_connection_send_message_with_reply_sync);
HAKIOS_INTERPOSE(watched_async, xpc_connection_send_message_with_reply);
HAKIOS_INTERPOSE(watched_create_mach_service, xpc_connection_create_mach_service);
HAKIOS_INTERPOSE(watched_create, xpc_connection_create);
HAKIOS_INTERPOSE(watched_send_message, xpc_connection_send_message);

static xpc_connection_t_sb
watched_from_endpoint(xpc_object_t_sb endpoint)
{
	xpc_connection_t_sb connection = xpc_connection_create_from_endpoint(endpoint);
	char *what = xpc_copy_description(endpoint);

	say("hakios: xpc: from endpoint %s -> %p\n", what != NULL ? what : "(nothing)", connection);
	free(what);
	return connection;
}

HAKIOS_INTERPOSE(watched_from_endpoint, xpc_connection_create_from_endpoint);


/* ---- the images SpringBoard decodes for the home screen --------------------------------------- */

/*
 * The home screen's jank is not the panel: p399 measured 60 fps while a finger dragged, and then a
 * stall of 4 s with the stack ending in CGDataProviderRetainBytePtr inside QuartzCore -- the image
 * decode, whose stack does not name the file.  Under touch_lat=1 these say the path of every image
 * SpringBoard opens, how long the decode took when it was slow, and where a provider sat when the
 * bytes were actually wanted.
 */
static char image_file[256];
static unsigned image_lines;

/* (declared here: this library names ImageIO's functions without including its headers) */
extern void *CGImageSourceCreateWithURL(CFURLRef, CFDictionaryRef);
extern void *CGImageSourceCreateImageAtIndex(void *, size_t, CFDictionaryRef);

static int
image_log_on(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (strstr(args, "touch_lat=1") != NULL || strstr(args, "stall_lat=1") != NULL);
	}
	return on;
}

static uint64_t
image_ms(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000000;
}

static void
image_note_file(CFURLRef url)
{
	CFStringRef path;
	char buffer[sizeof(image_file)] = "";

	if (url == NULL) {
		return;
	}
	path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
	if (path != NULL) {
		CFStringGetCString(path, buffer, sizeof(buffer), kCFStringEncodingUTF8);
		CFRelease(path);
	}
	if (buffer[0] != 0) {
		strlcpy(image_file, buffer, sizeof(image_file));
	}
}

static void *
timed_image_source_url(CFURLRef url, CFDictionaryRef options)
{
	void *source;

	if (image_log_on()) {
		image_note_file(url);
	}
	/* (the real one: a call by name from this image is not interposed; a stored pointer would be
	 *  NULL, and jumping through it is what killed SpringBoard 27 times in p402) */
	source = CGImageSourceCreateWithURL(url, options);
	if (image_log_on() && image_lines < 40) {
		image_lines++;
		say("hakios: image source %s %s\n", image_file[0] != 0 ? image_file : "?",
		    source != NULL ? "opened" : "refused");
	}
	return source;
}

static void *
timed_image_source_image(void *source, size_t index, CFDictionaryRef options)
{
	uint64_t t0 = image_ms();
	void *image = CGImageSourceCreateImageAtIndex(source, index, options);
	uint64_t ms = image_ms() - t0;

	if (image_log_on() && image_lines < 400) {
		image_lines++;
		if (ms >= 10) {
			say("hakios: image %s[%zu] decoded in %llu ms\n",
			    image_file[0] != 0 ? image_file : "?", index, (unsigned long long)ms);
		}
	}
	return image;
}

static const void *
timed_provider_bytes(void *provider)
{
	uint64_t t0 = image_ms();
	const void *bytes = orig_provider_bytes(provider);
	uint64_t ms = image_ms() - t0;

	if (ms >= 50 && image_log_on() && image_lines < 400) {
		image_lines++;
		say("hakios: image %s: the bytes took %llu ms\n",
		    image_file[0] != 0 ? image_file : "?", (unsigned long long)ms);
	}
	return bytes;
}

HAKIOS_INTERPOSE(timed_image_source_url, CGImageSourceCreateWithURL);
HAKIOS_INTERPOSE(timed_image_source_image, CGImageSourceCreateImageAtIndex);

/* The wallpaper is not opened through ImageIO: p406's stall runs CGContextDrawImageWithOptions ->
 * CGAccessSessionCreate -> CGDataProviderRetainBytePtr, which is a data provider made from a URL
 * or a file name.  These two name it. */
extern void *CGDataProviderCreateWithURL(CFURLRef);
extern void *CGDataProviderCreateWithFilename(const char *);

static void *
timed_provider_url(CFURLRef url)
{
	void *provider;

	if (image_log_on()) {
		image_note_file(url);
	}
	provider = CGDataProviderCreateWithURL(url);
	if (image_log_on() && image_lines < 40) {
		image_lines++;
		say("hakios: provider %s %s\n", image_file[0] != 0 ? image_file : "?",
		    provider != NULL ? "opened" : "refused");
	}
	return provider;
}

static void *
timed_provider_filename(const char *filename)
{
	uint64_t t0 = image_ms();
	void *provider = CGDataProviderCreateWithFilename(filename);
	uint64_t ms = image_ms() - t0;

	if (ms >= 50 && image_log_on() && image_lines < 400) {
		image_lines++;
		say("hakios: provider %s in %llu ms\n", filename != NULL ? filename : "?",
		    (unsigned long long)ms);
	}
	return provider;
}

HAKIOS_INTERPOSE(timed_provider_url, CGDataProviderCreateWithURL);
HAKIOS_INTERPOSE(timed_provider_filename, CGDataProviderCreateWithFilename);

/* The stack of a stall (p408) ends in CGContextDrawImageWithOptions -> CGAccessSessionCreate ->
 * CGDataProviderRetainBytePtr: the provider's bytes are wanted, and the decode happens there.  The
 * two ways SpringBoard makes an image out of a provider are these; the time they take is the stall. */
extern void *CGImageCreateWithPNGDataProvider(void *provider, const void *decode, bool interpolate, int intent);
extern void *CGImageCreateWithJPEGDataProvider(void *provider, const void *decode, bool interpolate, int intent);
extern size_t CGImageGetWidth(void *image);
extern size_t CGImageGetHeight(void *image);

static void *
timed_png_image(void *provider, const void *decode, bool interpolate, int intent)
{
	uint64_t t0 = image_ms();
	void *image = CGImageCreateWithPNGDataProvider(provider, decode, interpolate, intent);
	uint64_t ms = image_ms() - t0;

	if (image_log_on()) {
		say("hakios: png image %s %zux%zu in %llu ms\n", image_file[0] != 0 ? image_file : "?",
		    image != NULL ? CGImageGetWidth(image) : 0, image != NULL ? CGImageGetHeight(image) : 0,
		    (unsigned long long)ms);
	}
	return image;
}

static void *
timed_jpeg_image(void *provider, const void *decode, bool interpolate, int intent)
{
	uint64_t t0 = image_ms();
	void *image = CGImageCreateWithJPEGDataProvider(provider, decode, interpolate, intent);
	uint64_t ms = image_ms() - t0;

	if (image_log_on()) {
		say("hakios: jpeg image %s %zux%zu in %llu ms\n", image_file[0] != 0 ? image_file : "?",
		    image != NULL ? CGImageGetWidth(image) : 0, image != NULL ? CGImageGetHeight(image) : 0,
		    (unsigned long long)ms);
	}
	return image;
}

HAKIOS_INTERPOSE(timed_png_image, CGImageCreateWithPNGDataProvider);
HAKIOS_INTERPOSE(timed_jpeg_image, CGImageCreateWithJPEGDataProvider);


/* ---- a mapping that takes its time ------------------------------------------------------------ */

/*
 * p404 sampled SpringBoard's main thread four seconds into a turn and found it in __mmap, called
 * from AppSupport by SpringBoardUIServices -- the wallpaper's image.  mmap on this port is where
 * the file's pages are made, and nothing in the stack says which file.  Under stall_lat=1 (or
 * touch_lat=1) a mapping of a file that took longer than 100 ms is written down with its path.
 */
static void *
timed_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
	uint64_t t0, ms;
	void *result;

	if (!image_log_on() || fd < 0) {
		return mmap(address, length, prot, flags, fd, offset);
	}
	t0 = image_ms();
	result = mmap(address, length, prot, flags, fd, offset);
	ms = image_ms() - t0;
	if (ms >= 100 && image_lines < 400) {
		char path[256] = "";

		image_lines++;
		if (fcntl(fd, F_GETPATH, path) != 0) {
			path[0] = 0;
		}
		say("hakios: mmap %s %llu ms (%zu bytes, flags %#x)\n", path[0] != 0 ? path : "?",
		    (unsigned long long)ms, length, (unsigned)flags);
	}
	return result;
}

HAKIOS_INTERPOSE(timed_mmap, mmap);


/* ---- a wait that never ends, made to end ------------------------------------------------------ */

/*
 * SpringBoard's main thread, on this port, waits in a raw mach message inside BackBoardServices
 * (the frame chain of p188/p192/p200: BKS+0x9ad4 ... FrontBoard+0x37a4c) and nothing ever answers
 * it.  boot-arg bks_timeout=N (milliseconds, 0 or absent -- off) makes every wait of that kind
 * time out after N ms instead: the caller then gets MACH_RCV_TIMED_OUT, which is what a dead
 * service would give it, and SpringBoard either retries or goes on.  The point is to see which.
 */
static mach_msg_return_t (*orig_sb_mach_msg)(mach_msg_header_t *, mach_msg_option_t,
    mach_msg_size_t, mach_msg_size_t, mach_port_t, mach_msg_timeout_t, mach_port_t);
static mach_msg_return_t hk_sb_mach_msg(mach_msg_header_t *, mach_msg_option_t, mach_msg_size_t,
    mach_msg_size_t, mach_port_t, mach_msg_timeout_t, mach_port_t);
static unsigned bks_timeout;
static unsigned bks_timeouts;
static unsigned bks_seen;

static void
bks_timeout_setup(void)
{
	char arguments[2048];
	size_t size = sizeof(arguments) - 1;
	const char *at;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "bks_timeout=");
	if (at != NULL) {
		bks_timeout = (unsigned)strtoul(at + 12, NULL, 10);
	}
	if (bks_timeout == 0) {
		return;
	}
	orig_sb_mach_msg = (mach_msg_return_t (*)(mach_msg_header_t *, mach_msg_option_t,
	    mach_msg_size_t, mach_msg_size_t, mach_port_t, mach_msg_timeout_t, mach_port_t))
	    dlsym(RTLD_NEXT, "mach_msg");
	if ((const void *)orig_sb_mach_msg == (const void *)hk_sb_mach_msg) {
		/*
		 * dlsym(RTLD_NEXT) answers with our own hook here (the same as with unlink in
		 * inject.c).  Unlike there, a direct call is safe: that is how the xpc hooks in this
		 * very file reach the real functions, and it has never recursed.
		 */
		say("hakios: bks: mach_msg comes by its own name\n");
		orig_sb_mach_msg = NULL;
	}
	say("hakios: bks: waits from BackBoardServices time out after %u ms\n", bks_timeout);
}

static mach_msg_return_t
hk_sb_mach_msg(mach_msg_header_t *msg, mach_msg_option_t options, mach_msg_size_t size,
    mach_msg_size_t rcv_size, mach_port_t rcv_name, mach_msg_timeout_t timeout,
    mach_port_t notify)
{
	uintptr_t offset = 0;
	const char *image = image_of((uintptr_t)__builtin_return_address(0), &offset);

	if (image != NULL && strstr(image, "BackBoardServices") != NULL &&
	    __sync_add_and_fetch(&bks_seen, 1) <= 30) {
		say("hakios: bks: mach_msg from %s+%#lx: send %d, recv %d, id %u, size %u\n",
		    image, (unsigned long)offset, (options & MACH_SEND_MSG) ? 1 : 0,
		    (options & MACH_RCV_MSG) ? 1 : 0, msg != NULL ? msg->msgh_id : 0,
		    msg != NULL ? msg->msgh_size : 0);
	}
	if (bks_timeout != 0 && (options & MACH_RCV_MSG) != 0 && image != NULL &&
	    strstr(image, "BackBoardServices") != NULL) {
		mach_msg_return_t result;

		if (orig_sb_mach_msg != NULL) {
			result = orig_sb_mach_msg(msg, options | MACH_RCV_TIMEOUT, size, rcv_size,
			    rcv_name, bks_timeout, notify);
		} else {
			result = mach_msg(msg, options | MACH_RCV_TIMEOUT, size, rcv_size, rcv_name,
			    bks_timeout, notify);
		}

		if (result == MACH_RCV_TIMED_OUT && __sync_add_and_fetch(&bks_timeouts, 1) <= 40) {
			say("hakios: bks: a wait from %s+%#lx timed out after %u ms (id %u, from %s)\n",
			    image, (unsigned long)offset, bks_timeout, msg != NULL ? msg->msgh_id : 0);
		}
		return result;
	}
	if (orig_sb_mach_msg != NULL) {
		return orig_sb_mach_msg(msg, options, size, rcv_size, rcv_name, timeout, notify);
	}
	return mach_msg(msg, options, size, rcv_size, rcv_name, timeout, notify);
}

static mach_msg_return_t
hk_sb_mach_msg_overwrite(mach_msg_header_t *msg, mach_msg_option_t options, mach_msg_size_t size,
    mach_msg_size_t rcv_size, mach_port_t rcv_name, mach_msg_timeout_t timeout, mach_port_t notify,
    mach_msg_header_t *rcv_msg, mach_msg_size_t rcv_msg_size)
{
	(void)rcv_msg;
	(void)rcv_msg_size;
	return hk_sb_mach_msg(msg, options, size, rcv_size, rcv_name, timeout, notify);
}

#define DYLD_INTERPOSE_SB(ours, theirs)                                                 \
	__attribute__((used)) static struct {                                             \
		const void *replacement, *replacee;                                        \
	} sb_interpose_##theirs __attribute__((section("__DATA,__interpose"))) = {       \
		(const void *)(ours), (const void *)(theirs)                               \
	}

DYLD_INTERPOSE_SB(hk_sb_mach_msg, mach_msg);
DYLD_INTERPOSE_SB(hk_sb_mach_msg_overwrite, mach_msg_overwrite);

/*
 * ---- the service SpringBoard waits for -------------------------------------------------------
 *
 * p210/p212: the main thread sits in a raw mach message inside BackBoardServices, and the names
 * of the dyld cache put that frame in __BKSDisplayGetMainScreenInfo -- the MIG call to the display
 * server, com.apple.backboard.display.services (backboardd), made with no timeout at all.  Nothing
 * here can name the port from that frame, so the look-ups are logged instead: SpringBoard asks the
 * bootstrap server the old way (bootstrap_look_up, from BackBoardServices), not through libxpc,
 * where the hooks above already are.  Boot-arg bs_loud=0 turns the lines off; the first 300 only.
 */
extern kern_return_t bootstrap_look_up(mach_port_t bootstrap_port, const char *service_name,
    mach_port_t *service_port);
extern kern_return_t bootstrap_look_up2(mach_port_t bootstrap_port, const char *service_name,
    mach_port_t *service_port, pid_t target_pid, uint64_t flags);

static unsigned bs_lines;
static int bs_silent;

static void
bs_say(const char *which, const char *service_name, pid_t target_pid, uint64_t flags,
    kern_return_t result, mach_port_t port, unsigned long long began, const char *image,
    uintptr_t offset)
{
	unsigned long long took = millis_now() - began;

	if (bs_silent || __sync_add_and_fetch(&bs_lines, 1) > 300) {
		return;
	}
	if (image != NULL) {
		say("hakios: bs: %llu ms %s \"%s\" pid %d flags %#llx -> %d port %u in %llu ms, "
		    "from %s+%#lx\n", began, which,
		    service_name != NULL ? service_name : "(no name)", (int)target_pid,
		    (unsigned long long)flags, (int)result, (unsigned)port, took, image,
		    (unsigned long)offset);
	} else {
		say("hakios: bs: %llu ms %s \"%s\" pid %d flags %#llx -> %d port %u in %llu ms\n",
		    began, which, service_name != NULL ? service_name : "(no name)", (int)target_pid,
		    (unsigned long long)flags, (int)result, (unsigned)port, took);
	}
}

static void
bs_setup(void)
{
	char arguments[2048];
	size_t size = sizeof(arguments) - 1;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, "bs_loud=0") != NULL) {
		bs_silent = 1;
	}
	say("hakios: bs: bootstrap look-ups are logged\n");
}

static kern_return_t
hk_sb_bootstrap_look_up(mach_port_t bootstrap_port, const char *service_name,
    mach_port_t *service_port)
{
	unsigned long long began = millis_now();
	uintptr_t offset = 0;
	const char *image = image_of((uintptr_t)__builtin_return_address(0), &offset);
	kern_return_t result;

	result = bootstrap_look_up(bootstrap_port, service_name, service_port);
	bs_say("look_up", service_name, 0, 0, result,
	    result == 0 && service_port != NULL ? *service_port : 0, began, image, offset);
	return result;
}

static kern_return_t
hk_sb_bootstrap_look_up2(mach_port_t bootstrap_port, const char *service_name,
    mach_port_t *service_port, pid_t target_pid, uint64_t flags)
{
	unsigned long long began = millis_now();
	uintptr_t offset = 0;
	const char *image = image_of((uintptr_t)__builtin_return_address(0), &offset);
	kern_return_t result;

	result = bootstrap_look_up2(bootstrap_port, service_name, service_port, target_pid, flags);
	bs_say("look_up2", service_name, target_pid, flags, result,
	    result == 0 && service_port != NULL ? *service_port : 0, began, image, offset);
	return result;
}

DYLD_INTERPOSE_SB(hk_sb_bootstrap_look_up, bootstrap_look_up);
DYLD_INTERPOSE_SB(hk_sb_bootstrap_look_up2, bootstrap_look_up2);

/*
 * ---- how long the display server takes --------------------------------------------------------
 *
 * BKSDisplayServicesStart asks the display server for the main screen's size and scale before
 * anything else of SpringBoard's is made, and the frame of p210/p212 is that call's mach message.
 * The call itself is timed here: it returns in milliseconds on a healthy system, and this says
 * whether the wait that holds the main thread is this one or something later.
 */
extern int BKSDisplayServicesGetMainScreenInfo(void *width, void *height, void *scale,
    void *rotation);

static unsigned display_lines;

static int
shown_display_info(void *width, void *height, void *scale, void *rotation)
{
	unsigned long long began = millis_now();
	int result = BKSDisplayServicesGetMainScreenInfo(width, height, scale, rotation);

	if (__sync_add_and_fetch(&display_lines, 1) <= 40) {
		say("hakios: display: main screen info %d in %llu ms\n", result, millis_now() - began);
	}
	return result;
}

DYLD_INTERPOSE_SB(shown_display_info, BKSDisplayServicesGetMainScreenInfo);

/* ---- the names behind the offsets ------------------------------------------------------------ */

static void
say_symbols(void)
{
	static const char *names[] = {
		"mach_msg_trap", "mach_msg2_trap", "mach_msg_overwrite_trap",
		"__psynch_cvwait", "__psynch_mutexwait", "__psynch_mutexdrop", "__psynch_cvbroad",
		"__ulock_wait", "__ulock_wait2", "__semwait_signal", "__workq_kernreturn",
		"__thread_switch", "swtch_pri", "read", "write", "pread", "open", "stat",
		"getdirentries", "__getdirentries64", "fcntl", "fsync", "kill",
		"getattrlist", "setattrlist", "getattrlistbulk", "access", "chdir", "getcwd",
		"__open_dprotected_np", "openat", "mkdir", "rmdir", "unlink", "rename", "chmod",
		"getxattr", "setxattr", "listxattr", "statfs", "getfsstat", "mmap", "__mmap",
		"munmap", "mprotect", "msync", "__msync", "dup", "pipe", "posix_spawn", "umask",
		"os_unfair_lock_lock", "os_unfair_lock_unlock", "os_unfair_lock_trylock",
		"os_unfair_recursive_lock_lock_with_options", "os_unfair_recursive_lock_unlock",
		"pthread_cond_wait", "pthread_mutex_lock", "_pthread_wqthread",
		"dispatch_semaphore_wait", "dispatch_sync", "dispatch_async", "dispatch_group_wait",
		"dispatch_sync_f", "dispatch_once_f", "semaphore_wait_trap",
	};
	unsigned i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		void *address = dlsym(RTLD_DEFAULT, names[i]);
		uintptr_t offset = 0;
		const char *image;

		if (address == NULL) {
			continue;
		}
		image = image_of((uintptr_t)address, &offset);
		if (image != NULL) {
			say("hakios: sym %s = %s+%#lx\n", names[i], image, (unsigned long)offset);
		}
	}
}


/* ---- which connections SpringBoard makes ----------------------------------------------------- */

static IMP
hook_method(const char *class_name, const char *selector, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls != NULL ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		say("hakios: xpc: -[%s %s] is not there\n", class_name, selector);
		return NULL;
	}
	IMP original = method_getImplementation(method);

	method_setImplementation(method, replacement);
	return original;
}

static id (*orig_init_service)(id, SEL, id);
static id (*orig_init_mach_service)(id, SEL, id, unsigned long);
static id (*orig_init_listener_endpoint)(id, SEL, id);
static id (*orig_init_endpoint)(id, SEL, id);

static id
log_init_service(id self, SEL sel, id name)
{
	id result = orig_init_service(self, sel, name);

	say("hakios: xpc: NSXPCConnection service %s -> %p\n", text_of(name), self);
	return result;
}

static id
log_init_mach_service(id self, SEL sel, id name, unsigned long options)
{
	id result = orig_init_mach_service(self, sel, name, options);

	say("hakios: xpc: NSXPCConnection mach service %s (options %#lx) -> %p\n", text_of(name), options,
	    self);
	return result;
}

static void
say_endpoint(id endpoint)
{
	id xpc = NULL;

	if (endpoint != NULL && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(endpoint,
	    sel_registerName("respondsToSelector:"), sel_registerName("_endpoint"))) {
		xpc = ((id (*)(id, SEL))objc_msgSend)(endpoint, sel_registerName("_endpoint"));
	}
	if (xpc != NULL) {
		char *what = xpc_copy_description(xpc);

		say("hakios: xpc:   endpoint %s (%p)\n", what != NULL ? what : "(nothing)", xpc);
		free(what);
	} else {
		say("hakios: xpc:   endpoint %p (no _endpoint)\n", endpoint);
	}
}

static id
log_init_listener_endpoint(id self, SEL sel, id endpoint)
{
	say("hakios: xpc: NSXPCConnection listener endpoint -> %p\n", self);
	say_endpoint(endpoint);
	return orig_init_listener_endpoint(self, sel, endpoint);
}

static id
log_init_endpoint(id self, SEL sel, id endpoint)
{
	say("hakios: xpc: NSXPCConnection endpoint -> %p\n", self);
	say_endpoint(endpoint);
	return orig_init_endpoint(self, sel, endpoint);
}

static void
watch_nsxpc(void)
{
	orig_init_service = (void *)hook_method("NSXPCConnection", "initWithServiceName:",
	    (IMP)log_init_service);
	orig_init_mach_service = (void *)hook_method("NSXPCConnection", "initWithMachServiceName:options:",
	    (IMP)log_init_mach_service);
	orig_init_listener_endpoint = (void *)hook_method("NSXPCConnection", "initWithListenerEndpoint:",
	    (IMP)log_init_listener_endpoint);
	orig_init_endpoint = (void *)hook_method("NSXPCConnection", "initWithEndpoint:",
	    (IMP)log_init_endpoint);
	say("hakios: xpc: NSXPCConnection initialisers hooked\n");
}


/* ---- where FSNode comes from, and what the endpoint arrived in ------------------------------- */

static void
say_class_family(const char *name)
{
	Class cls = objc_getClass(name);
	const char *image;

	if (cls == NULL) {
		say("hakios: class %s is not there\n", name);
		return;
	}
	image = class_getImageName(cls);
	say("hakios: class %s comes from %s\n", name, image != NULL ? image : "?");
	if (strcmp(name, "FSNode") == 0) {
		unsigned count = 0, i;
		Method *methods = class_copyMethodList(cls, &count);

		say("hakios: class FSNode has %u methods\n", count);
		for (i = 0; i < count && i < 120; i++) {
			const char *selector = sel_getName(method_getName(methods[i]));
			const char *encoding = method_getTypeEncoding(methods[i]);

			say("hakios: FSNode -%s  %s\n", selector, encoding != NULL ? encoding : "");
		}
		free(methods);
	}
}


/* ---- a start that waits ---------------------------------------------------------------------- */

static void
wait_if_asked(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *p;
	long seconds;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	if ((p = strstr(arguments, "sbwait=")) == NULL) {
		return;
	}
	seconds = strtol(p + 7, NULL, 10);
	if (seconds <= 0 || seconds > 300) {
		return;
	}
	say("hakios: SpringBoard waits %ld s before main (sbwait)\n", seconds);
	sleep((unsigned)seconds);
	say("hakios: SpringBoard goes on\n");
}


/* ---- the invocation, byte for byte ----------------------------------------------------------- */

static void
say_graph(const char *what, xpc_object_t_sb message)
{
	xpc_object_t_sb data;
	const unsigned char *bytes;
	size_t length, i, used = 0;
	char text[640];

	if (message == NULL) {
		return;
	}
	data = xpc_dictionary_get_value(message, "root");
	if (data == NULL) {
		return;
	}
	bytes = xpc_data_get_bytes_ptr(data);
	length = xpc_data_get_length(data);
	if (bytes == NULL || length == 0) {
		return;
	}
	for (i = 0; i < length && i < 190 && used + 3 < sizeof(text); i++) {
		used += (size_t)snprintf(text + used, sizeof(text) - used, "%02x", bytes[i]);
	}
	say("hakios: %s: %zu bytes %s\n", what, length, text);
}


/* ---- the driver's own account of the lock ---------------------------------------------------- */

static void
say_apfs_stats(const char *when)
{
	size_t size = 65536;
	char *text = malloc(size);

	if (text == NULL) {
		return;
	}
	memset(text, 0, size);
	if (sysctlbyname("kern.apfs_stats", text, &size, NULL, 0) != 0) {
		say("hakios: apfs stats (%s): the sysctl refused (%d)\n", when, errno);
		free(text);
		return;
	}
	{
		char *line = text;

		say("hakios: apfs stats, %s\n", when);
		while (line != NULL && *line != 0) {
			char *end = strchr(line, '\n');

			if (end != NULL) {
				*end = 0;
			}
			say("apfs| %s\n", line);
			if (end == NULL) {
				break;
			}
			line = end + 1;
		}
		say("hakios: apfs stats end\n");
	}
	free(text);
}

static id (*orig_view_did_appear)(id, SEL, BOOL);
static void log_view_did_appear(id self, SEL sel, BOOL animated)
{
	say("hakios: SB milestone: -[SBHomeScreenViewController viewDidAppear:]\n");
	if (orig_view_did_appear != NULL) { orig_view_did_appear(self, sel, animated); }
}

/* ---- getServerStoreBlockingWithCompletionHandler: the call that never comes back ------------- */

/*
 * Boot-arg sb_nostore=1: the LaunchServices store is asked for with a blocking XPC call that this
 * port never answers (see collab.md: "xpc 17/25 -> (no name)"), and SpringBoard waits in it
 * forever.  Every class is looked through for that selector; if one has it, its implementation
 * becomes one that answers the completion handler at once with "no store" -- the point is to see
 * whether the rest of SpringBoard can come up without it.  sb_store_scan=1 only lists what is
 * there.
 */
static int nostore_wanted;

static void
nostore_handler(id self, SEL _cmd, id handler)
{
	Class error_class = objc_getClass("NSError");
	id error = nil;

	if (error_class != NULL) {
		error = ((id (*)(id, SEL, id, long, id))objc_msgSend)(error_class,
		    sel_registerName("errorWithDomain:code:userInfo:"), nil, 1, nil);
	}
	say("hakios: store: %s getServerStoreBlocking... refused at once (no store)\n",
	    class_getName(object_getClass(self)));
	if (handler != NULL) {
		((void (^)(id, id))handler)(nil, error);
	}
}

static void
store_methods_setup(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	unsigned count, i, hooked = 0;
	Class *classes;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	if (strstr(arguments, "sb_nostore=1") == NULL && strstr(arguments, "sb_store_scan=1") == NULL) {
		return;
	}
	nostore_wanted = strstr(arguments, "sb_nostore=1") != NULL;
	count = objc_getClassList(NULL, 0);
	classes = count != 0 ? calloc(count, sizeof(Class)) : NULL;
	if (classes == NULL) {
		return;
	}
	count = objc_getClassList(classes, count);
	for (i = 0; i < count; i++) {
		Class cls = classes[i];
		unsigned pass;

		for (pass = 0; pass < 2; pass++) {
			Class owner = pass == 0 ? cls : object_getClass(cls);
			unsigned method_count = 0, m;
			Method *methods;

			if (owner == NULL) {
				continue;
			}
			methods = class_copyMethodList(owner, &method_count);
			for (m = 0; m < method_count; m++) {
				const char *name = sel_getName(method_getName(methods[m]));

				if (strstr(name, "ServerStore") == NULL &&
				    strstr(name, "serverStore") == NULL) {
					continue;
				}
				say("hakios: store method: %c[%s %s] %s\n", pass == 0 ? '-' : '+',
				    class_getName(cls), name, method_getTypeEncoding(methods[m]));
				if (nostore_wanted && strcmp(name, "getServerStoreBlockingWithCompletionHandler:") == 0) {
					method_setImplementation(methods[m], (IMP)nostore_handler);
					hooked++;
				}
			}
			free(methods);
		}
	}
	free(classes);
	say("hakios: store methods: %u, hooked %u\n", count, hooked);
}

static void
nostore2_setup(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, "sb_nostore2=1") != NULL) {
		nostore2 = 1;
		say("hakios: store: the blocking server store request is answered at once\n");
	}
}


/*
 * NSURL with a nil path.  The window server reaches LaunchServices' store and then makes an URL
 * of a path the container manager did not give it (nil): Foundation throws, and the process is
 * gone -- the same repair the other daemons already have (inject.c), the window server did not.
 */
static id
sb_fallback_path(void)
{
	static id fallback;

	if (fallback == nil) {
		fallback = (id)CFStringCreateWithCString(kCFAllocatorDefault, "/private/var/tmp",
		    kCFStringEncodingUTF8);
	}
	return fallback;
}

static id (*orig_url_path)(id, SEL, id);
static id (*orig_url_path_dir)(id, SEL, id, BOOL);
static id (*orig_url_path_dir_rel)(id, SEL, id, BOOL, id);

static id
sb_url_path(id self, SEL selector, id path)
{
	if (path == nil) {
		path = sb_fallback_path();
		say("hakios: NSURL: a nil path became /private/var/tmp\n");
	}
	return orig_url_path(self, selector, path);
}

static id
sb_url_path_dir(id self, SEL selector, id path, BOOL is_directory)
{
	if (path == nil) {
		path = sb_fallback_path();
		say("hakios: NSURL: a nil path became /private/var/tmp (isDirectory)\n");
	}
	return orig_url_path_dir(self, selector, path, is_directory);
}

static id
sb_url_path_dir_rel(id self, SEL selector, id path, BOOL is_directory, id base)
{
	if (path == nil) {
		path = sb_fallback_path();
		say("hakios: NSURL: a nil path became /private/var/tmp (relative)\n");
	}
	return orig_url_path_dir_rel(self, selector, path, is_directory, base);
}

static void
url_path_fixes(void)
{
	Class url = objc_getClass("NSURL");
	Class meta;
	Method method;

	if (url == NULL) {
		return;
	}
	meta = object_getClass(url);
	if ((method = class_getInstanceMethod(url, sel_registerName("initFileURLWithPath:"))) != NULL) {
		orig_url_path = (void *)method_getImplementation(method);
		method_setImplementation(method, (IMP)sb_url_path);
	}
	if ((method = class_getInstanceMethod(url, sel_registerName("initFileURLWithPath:isDirectory:"))) != NULL) {
		orig_url_path_dir = (void *)method_getImplementation(method);
		method_setImplementation(method, (IMP)sb_url_path_dir);
	}
	if ((method = class_getInstanceMethod(url, sel_registerName("initFileURLWithPath:isDirectory:relativeTo:"))) != NULL) {
		orig_url_path_dir_rel = (void *)method_getImplementation(method);
		method_setImplementation(method, (IMP)sb_url_path_dir_rel);
	}
	/* (+fileURLWithPath: goes through these inits: they are the place to stand) */
	say("hakios: NSURL: the nil path repairs are in\n");
}

/* ---- opening a URL by itself -------------------------------------------------------------- */

/*
 * Settings -> General -> iPhone Storage is the page that asks each application's usage plugin
 * for its size, and the Podcasts one traps in this port; to try a cure without a finger on the
 * screen, /redwood/etc/open.url holds lines "<seconds> <url>" and each is opened from here after
 * that many seconds, from the main queue.  The log says what was asked for and what came back,
 * and whether the plugin was loaded at all (inject_app.c says that).
 */
static void
open_url(const char *text)
{
	Class workspace_class = objc_getClass("LSApplicationWorkspace");
	SEL default_workspace = sel_registerName("defaultWorkspace");
	SEL open_sensitive = sel_registerName("openSensitiveURL:withOptions:");
	SEL open_plain = sel_registerName("openURL:withOptions:");
	id (*send_id)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*send_id_id)(id, SEL, id) = (id (*)(id, SEL, id))objc_msgSend;
	id (*send_id_text)(id, SEL, const char *) = (id (*)(id, SEL, const char *))objc_msgSend;
	BOOL (*send_bool)(id, SEL, id, id) = (BOOL (*)(id, SEL, id, id))objc_msgSend;
	id workspace, address, string;
	BOOL answer = NO;

	if (workspace_class == NULL || class_getClassMethod(workspace_class, default_workspace) == NULL) {
		say("hakios: open: there is no application workspace here\n");
		return;
	}
	workspace = send_id((id)workspace_class, default_workspace);
	/*
	 * "bundle:<identifier>" is the application itself, which is what most of these are: a URL
	 * scheme is not always the application's to answer (Photos answers photos-redirect: only
	 * where Shortcuts is the caller).
	 */
	if (strncmp(text, "bundle:", 7) == 0) {
		/*
		 * The application by its bundle: SpringBoardServices is the way SpringBoard itself starts
		 * one (SBSLaunchApplicationWithIdentifier is in the shared cache and exported), while
		 * -[LSApplicationWorkspace openApplicationWithBundleIdentifier:] does not exist on this
		 * system at all -- the name is openApplicationWithBundleID:, and p330 showed that asking
		 * for the longer one launches nothing and says nothing.
		 */
		SEL open_bundle = sel_registerName("openApplicationWithBundleID:");
		id identifier = send_id_text((id)objc_getClass("NSString"),
		    sel_registerName("stringWithUTF8String:"), text + 7);
		void *launch = dlsym(RTLD_DEFAULT, "SBSLaunchApplicationWithIdentifier");

		if (identifier != NULL && launch != NULL) {
			int status = ((int (*)(id, signed char))launch)(identifier, 0);

			say("hakios: open: the application %s launched, status %d\n", text + 7, status);
			return;
		}
		if (identifier != NULL && workspace != NULL &&
		    class_getInstanceMethod(workspace_class, open_bundle) != NULL) {
			answer = send_bool(workspace, open_bundle, identifier, NULL);
			say("hakios: open: the application %s asked for, answer %d\n", text + 7, answer);
			return;
		}
		say("hakios: open: no way to start %s here (no SBSLaunchApplicationWithIdentifier, no "
		    "-openApplicationWithBundleID:)\n", text + 7);
		return;
	}
	string = send_id_text((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), text);
	address = string != NULL ?
	    send_id_id((id)objc_getClass("NSURL"), sel_registerName("URLWithString:"), string) : NULL;
	if (address != NULL && workspace != NULL &&
	    class_getInstanceMethod(workspace_class, open_sensitive) != NULL) {
		answer = send_bool(workspace, open_sensitive, address, NULL);
	}
	if (!answer && address != NULL && workspace != NULL &&
	    class_getInstanceMethod(workspace_class, open_plain) != NULL) {
		answer = send_bool(workspace, open_plain, address, NULL);
	}
	/*
	 * -openSensitiveURL:withOptions: may well return nothing at all; p329 returned 0 and opened
	 * the page anyway, so what is printed is what came back, not what happened.
	 */
	say("hakios: open: %s asked for (%s), answer %d\n", text,
	    class_getInstanceMethod(workspace_class, open_sensitive) != NULL ? "openSensitiveURL is here" :
	    "no openSensitiveURL", answer);
}

static void *
open_later(void *unused)
{
	static char list[1024];
	char *line, *end = NULL;
	size_t got = 0;
	unsigned previous = 0;
	ssize_t n;
	int fd;

	(void)unused;
	if ((fd = open("/redwood/etc/open.url", O_RDONLY, 0)) < 0) {
		return NULL;
	}
	while (got < sizeof(list) - 1 && (n = read(fd, list + got, sizeof(list) - 1 - got)) > 0) {
		got += (size_t)n;
	}
	close(fd);
	list[got] = 0;
	for (line = list; line != NULL && line[0] != 0; line = end != NULL ? end + 1 : NULL) {
		unsigned when;
		char *space;

		end = strchr(line, '\n');
		if (end != NULL) {
			*end = 0;
		}
		space = strchr(line, ' ');
		if (space == NULL) {
			continue;
		}
		*space = 0;
		when = (unsigned)strtoul(line, NULL, 10);
		if (when < previous) {
			when = previous;
		}
		if (when > previous) {
			sleep(when - previous);
			previous = when;
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			open_url(space + 1);
		});
	}
	return NULL;
}

static void
open_setup(void)
{
	pthread_t th;

	if (access("/redwood/etc/open.url", F_OK) != 0) {
		return;
	}
	say("hakios: open: the URLs of /redwood/etc/open.url are asked for in their time\n");
	if (pthread_create(&th, NULL, open_later, NULL) == 0) {
		pthread_detach(th);
	}
}

/* ---- warming PosterBoard up before the home screen asks it for the wallpaper --------------- */

/*
 * Switch: /redwood/etc/hakios.posterwarm, the number of seconds into SpringBoard's life after
 * which the poster daemon is asked for (25 if the file is empty).
 *
 * The wallpaper is drawn by PosterBoard, an application launchd only starts on demand (its job,
 * com.apple.posterboardservices.services, is "the one that elided xpcproxy"), and p341 shows the
 * order: the home screen appeared at 17:42:20 and PosterBoard's process at the same moment, with
 * the wallpaper black until its poster and extension host were up a few seconds later.  A
 * connection made while SpringBoard is still busy starts that chain early, so the poster is
 * there when the home screen asks for it.
 */
static void *
posterwarm_later(void *argument)
{
	unsigned when = (unsigned)(uintptr_t)argument;
	xpc_connection_t_sb connection;

	say("hakios: posterwarm: the thread is up, %u s to go\n", when);
	sleep(when);
	connection = xpc_connection_create_mach_service("com.apple.posterboardservices.services", NULL, 0);
	if (connection == NULL) {
		say("hakios: posterwarm: no connection to com.apple.posterboardservices.services\n");
		return NULL;
	}
	xpc_connection_set_event_handler(connection, ^(xpc_object_t_sb event) { (void)event; });
	xpc_connection_resume(connection);
	say("hakios: posterwarm: %u s in, PosterBoard is asked for (pid %d)\n", when,
	    xpc_connection_get_pid(connection));
	/* the connection is kept: while it is open the daemon is not asked to go away */
	return NULL;
}

static void
posterwarm_setup(void)
{
	unsigned when = 25;
	pthread_t th;
	int fd;

	if ((fd = open("/redwood/etc/hakios.posterwarm", O_RDONLY, 0)) < 0) {
		return;
	}
	{
		char text[32];
		long got = read(fd, text, sizeof(text) - 1);

		close(fd);
		if (got > 0) {
			text[got] = 0;
			when = (unsigned)strtoul(text, NULL, 10);
		}
	}
	if (when == 0) {
		return;
	}
	say("hakios: posterwarm: PosterBoard is asked for at %u s\n", when);
	if (pthread_create(&th, NULL, posterwarm_later, (void *)(uintptr_t)when) == 0) {
		pthread_detach(th);
	}
}

static void
sb_milestones(void)
{
	Class c;
	Method m;

	hook_init_SBUIController(objc_getClass("SBUIController"));
	hook_init_SBApplicationController(objc_getClass("SBApplicationController"));
	hook_init_SBLockScreenManager(objc_getClass("SBLockScreenManager"));
	hook_init_SBWallpaperController(objc_getClass("SBWallpaperController"));
	hook_init_SBMainWorkspace(objc_getClass("SBMainWorkspace"));
	hook_init_SBUserSessionController(objc_getClass("SBUserSessionController"));
	hook_void_SBApplicationController(objc_getClass("SBApplicationController"));
	hook_void_SBUIController(objc_getClass("SBUIController"));
	if ((c = objc_getClass("SBHomeScreenViewController")) != NULL &&
	    (m = class_getInstanceMethod(c, sel_registerName("viewDidAppear:"))) != NULL) {
		orig_view_did_appear = (void *)method_getImplementation(m);
		method_setImplementation(m, (IMP)log_view_did_appear);
	}
	say("hakios: SB milestones hooked\n");
	url_path_fixes();
	say_class_family("FSNode");
	store_methods_setup();
	nostore2_setup();
	nodnd_setup();
	noanon_setup();
	unlock_setup();
	open_setup();
	posterwarm_setup();
	store_wait_setup();
	sb_xpc_timeout_setup();
	watch_nsxpc();
	say_symbols();
	sample_watchdog();
	lock_watchdogs();
	bks_timeout_setup();
	bs_setup();
	loud_limit_setup();
	xpc_watched = 1;
	xpc_count = 1;
	say("hakios: xpc watching\n");
	wait_if_asked();
}

__attribute__((constructor))
static void
hakios_springboard_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	IMP activation, snapshot_format, battery_lvl, battery_st, poster_switcher;
	int token;

	int console_fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (console_fd >= 0) {
		say_fd = console_fd;
	}
	/*
	 * Boot-arg hakios_quiet=1: the window server's stdout and stderr to /dev/null, our own
	 * lines still on the console.  A window server that writes a lot to the console can be
	 * held back by it, and with it every frame.
	 */
	{
		char arguments[1024] = "";
		size_t arguments_size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &arguments_size, NULL, 0) == 0 &&
		    strstr(arguments, "hakios_quiet=1") != NULL) {
			int null = open("/dev/null", O_WRONLY);

			if (null >= 0) {
				dup2(null, 1);
				dup2(null, 2);
				if (null > 2) {
					close(null);
				}
			}
			say("hakios: %s[%d]: stdout and stderr are /dev/null (hakios_quiet=1)\n",
			    getprogname(), getpid());
		}
	}

	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	watch_for_faults();
	{
		void (*set_uncaught)(void (*)(id)) = (void (*)(void (*)(id)))dlsym(RTLD_DEFAULT, "NSSetUncaughtExceptionHandler");
		if (set_uncaught != NULL) {
			set_uncaught(hakios_exception_handler);
		}
	}
	install_collection_safety_hooks();

	if (name == NULL) {
		return;
	}
	say("hakios: %s[%d] starting (slide %#lx)\n", name, getpid(), slide);
	if (strcmp(name, "SpringBoard") != 0) {
		static char who[64];

		snprintf(who, sizeof(who), " of %s", name);
		layers_of = who;
		if ((say_fd = open("/dev/console", O_WRONLY | O_NOCTTY)) < 0) {
			say_fd = 2;
		}
		say("hakios: in %s[%d]: its layers are said on the notification com.hakios.layers\n", name, getpid());
		notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
			layers();
		});
		return;
	}
	sb_milestones();
	/*
	 * The tweaks of the package manager's prefix, if one is installed: ElleKit's loader
	 * (/var/jb/usr/lib/TweakLoader.dylib) reads /var/jb/usr/lib/TweakInject and opens the tweaks
	 * whose Filter names this process. The kernel could insert it as well (etc/inject.conf), but
	 * only one inserted library per program is reliable here, and SpringBoard's is this one; so
	 * this one opens it, the way a jailbreak's own hook does.
	 */
	load_tweaks();
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	activation = replace_method("SBLockdownManager", "_setupActivationState", 0x1c4bc8dfcUL, (IMP)setup_activation_state);
	original_set_setup_required_reason = (void (*)(id, SEL, long))replace_method("SBSetupManager",
	    "_setSetupRequiredReason:", 0x1c4cdd3a4UL, (IMP)set_setup_required_reason);
	snapshot_format = replace_method_of_kind('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:",
	    0x1f1c91480UL, (IMP)snapshot_output_format);
	plain_vibrancy();
	original_lock_long_press = (void (*)(id, SEL, id))replace_method("SBLockHardwareButton", "longPress:", 0x1c4f6ab44UL,
	    (IMP)lock_long_press);
	poster_switcher = replace_method("SBMainDisplayPolicyAggregator", "_allowsCapabilityPosterSwitcherWithExplanation:",
	    0x1c507e0fcUL, (IMP)poster_switcher_allowed);
	original_switcher_wallpaper_overlay = (void (*)(id, SEL))replace_method("SBAppSwitcherPageView",
	    "_createOrDestroyWallpaperOverlayView", 0x1c4af7860UL, (IMP)switcher_wallpaper_overlay);
	/*
	 * Battery hooks on UIDevice: these are the methods every app and SpringBoard itself calls, so
	 * hooking them here in the SpringBoard process covers both the status bar and any app that
	 * reads [UIDevice currentDevice].batteryLevel. The addresses are for iOS 16.5 (20F66).
	 */
	original_battery_level = (float (*)(id, SEL))replace_method("UIDevice", "batteryLevel", 0x1897021b0UL, (IMP)battery_level);
	original_battery_state = (long (*)(id, SEL))replace_method("UIDevice", "batteryState", 0x189209794UL, (IMP)battery_state);
	original_battery_view_shows_percentage = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "showsPercentage",
	    0x189a9370cUL, (IMP)battery_view_shows_percentage);
	original_battery_view_set_shows_percentage = (void (*)(id, SEL, BOOL))replace_method("_UIBatteryView", "setShowsPercentage:",
	    0x18926e150UL, (IMP)battery_view_set_shows_percentage);
	original_battery_view_should_show_bolt = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "_shouldShowBolt",
	    0x189203e28UL, (IMP)battery_view_should_show_bolt);
	original_battery_view_charge_percent = (double (*)(id, SEL))replace_method("_UIBatteryView", "chargePercent",
	    0x189a935d8UL, (IMP)battery_view_charge_percent);
	original_battery_view_set_charge_percent = (void (*)(id, SEL, double))replace_method("_UIBatteryView", "setChargePercent:",
	    0x18928d310UL, (IMP)battery_view_set_charge_percent);
	original_ring_charging_level = (double (*)(id, SEL))replace_method("CSBatteryChargingRingView", "batteryChargingLevel",
	    0x1c48c2cf4UL, (IMP)ring_charging_level);
	battery_lvl = (IMP)original_battery_level;
	battery_st  = (IMP)original_battery_state;
	say("hakios: SpringBoard: activation %s, setup assistant %s, application snapshots %s, the side button's long press %s, "
	    "battery level %s, battery state %s, poster switcher %s\n",
	    activation ? "answered here" : "NOT replaced", original_set_setup_required_reason ? "refused" : "NOT replaced",
	    snapshot_format ? "as JPEG" : "NOT changed", original_lock_long_press ? "checked" : "NOT checked",
	    battery_lvl ? "from QG" : "NOT replaced", battery_st ? "from QG" : "NOT replaced",
	    poster_switcher ? "not offered" : "NOT withheld");
	/*
	 * Cellular, a survey first (the modem's bars come from the kernel: sysctl kern.hakios.cellular_*,
	 * bsd/dev/hakios.mpss.c): where UIKit makes the status bar's cellular entry from the raw data,
	 * and with which signature -- said in the log with the unslid address, for the hook to come.
	 */
	{
		static const char *const classes[] = { "_UIStatusBarDataCellularEntry", "_UIStatusBarData", "_UIStatusBarDataEntry" };
		const char *selector = "_cellularEntryWithData:signalStrengthEnabled:serviceEnabled:dataNetworkEnabled:serviceString:"
		    "serviceCrossfadeString:serviceBadgeString:serviceContentType:dataNetworkType:gsmSignalStrengthRaw:"
		    "gsmSignalStrengthBars:callForwardingType:lowDataModeActive:";

		for (unsigned c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
			Class cls = objc_getClass(classes[c]);
			Method plus = cls ? class_getClassMethod(cls, sel_registerName(selector)) : NULL;
			Method minus = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

			if (plus) {
				say("hakios: cellular: +[%s %s] at %#lx, types \"%s\"\n", classes[c], selector,
				    (uintptr_t)method_getImplementation(plus) - slide, method_getTypeEncoding(plus));
			}
			if (minus) {
				say("hakios: cellular: -[%s %s] at %#lx, types \"%s\"\n", classes[c], selector,
				    (uintptr_t)method_getImplementation(minus) - slide, method_getTypeEncoding(minus));
			}
			if (cls && !plus && !minus) {
				say("hakios: cellular: %s has no such method\n", classes[c]);
			}
		}
		say("hakios: cellular: the kernel says up=%d registered=%d bars=%d dbm=%d radio=%d\n", sysctl_int("kern.hakios.cellular_up"),
		    sysctl_int("kern.hakios.cellular_registered"), sysctl_int("kern.hakios.cellular_bars"),
		    sysctl_int("kern.hakios.cellular_dbm"), sysctl_int("kern.hakios.cellular_radio"));
	}
	cellular_hook();
	say("hakios: battery UI: shows%% %s, set-shows%% %s, bolt %s, chargePercent %s, setChargePercent %s, lockscreen ring %s\n",
	    original_battery_view_shows_percentage ? "ok" : "NO",
	    original_battery_view_set_shows_percentage ? "ok" : "NO",
	    original_battery_view_should_show_bolt ? "ok" : "NO",
	    original_battery_view_charge_percent ? "ok" : "NO",
	    original_battery_view_set_charge_percent ? "ok" : "NO",
	    original_ring_charging_level ? "ok" : "NO");
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *delay;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (delay = strstr(args, "sb_delay=")) != NULL &&
		    (delay == args || delay[-1] == ' ')) {
			usleep((useconds_t)(atof(delay + 9) * 1e6));
		}
	}
	/*
	 * UIDevice won't call batteryLevel/batteryState unless monitoring is enabled. Flip it on once
	 * SpringBoard's run loop is running (the dispatch_after fires on the main queue, which means
	 * we are past UIApplication startup).
	 */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		Class uidev = objc_getClass("UIDevice");
		SEL current = sel_registerName("currentDevice");
		SEL monitoring = sel_registerName("setBatteryMonitoringEnabled:");
		id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, current) : nil;

		if (device && class_getInstanceMethod(object_getClass(device), monitoring)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(device, monitoring, YES);
		}
		watch_main_loop();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
				control_center_fades(arguments);
			}
		}
		say("hakios: battery: SOC=%d%% battery=%d charger=%d charging=%d\n",
		    sysctl_int("kern.hakios.battery_soc"), sysctl_int("kern.hakios.battery_present"),
		    sysctl_int("kern.hakios.battery_charger_present"), sysctl_int("kern.hakios.battery_charging"));
		battery_post_notifications();
	});
	notify_register_dispatch("com.hakios.unlock", &token, dispatch_get_main_queue(), ^(__unused int t) {
		unlock();
	});
	notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
		layers();
	});
	notify_register_dispatch("com.hakios.switcher", &token, dispatch_get_main_queue(), ^(__unused int t) {
		switcher();
	});
	/*
	 * Belt-and-suspenders unlock. The user can always swipe up to open the home screen -- that
	 * works fine when the touch driver is doing its job. But if touch is still probing, has
	 * failed, or the panel is a FocalTech and the firmware upload is still chewing through
	 * ~114 KB of SPI (that takes a hot second during bring-up), the user ends up staring at a
	 * dark lock screen with nothing to swipe. So we just forcibly dismiss it after 10 seconds
	 * so iOS always lands on the home screen even in the worst case. Harmless if touch is
	 * already working: the user just doesn't need to swipe themselves.
	 * With autounlock=0 among the kernel's arguments the lock screen stays until it is swiped away
	 * (to show the phone starting the way an iPhone does).
	 */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *arg;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (arg = strstr(args, "autounlock=0")) != NULL &&
		    (arg == args || arg[-1] == ' ')) {
			say("hakios: the lock screen stays until it is swiped away (autounlock=0)\n");
			return;
		}
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		unlock();
	});
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		say("hakios: dumping SpringBoard layers after unlock:\n");
		layers();
	});
}
