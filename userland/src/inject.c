#include <libkern/OSCacheControl.h>
/*
 * libhakios.dylib: code of ours inside stock iOS programs.
 *
 * launchd starts the jobs named in INJECT (see build.sh / mkjobs.py) with
 * DYLD_INSERT_LIBRARIES pointing here; dyld honours that because the kernel's AMFI stand-in
 * says so. What is done depends on the program:
 *
 *   backboardd   the window server (CoreAnimation in QuartzCore). There is no GPU, so the
 *                render server must use CoreAnimation's own software rasterizer, and we want to
 *                see why a frame is or is not drawn. The C++ objects are reached by replacing
 *                entries of their virtual tables.
 *                It is also where input enters the system: backboardd is the HID event server,
 *                and without Apple's multitouch driver it has no touch screen. We give it one:
 *                a "virtual" HID service that turns the samples of /dev/hakios-touch into the
 *                events that driver would have sent.
 *
 *                And it does not wait five seconds for an ambient light sensor.
 *
 *   containermanagerd (both of them)
 *                They ask the user manager for the "personas" of the user, of which this
 *                system has none, twenty times a second apart before they give up -- and
 *                everything that needs a container (the window server, for one) waits with
 *                them. Told that personas are not supported, they do not ask.
 *
 * Addresses of things inside the dyld shared cache are those of iOS 16.5 (20F66) for iPhone10,3;
 * every one is checked against what is expected to be there before it is touched.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <notify.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <pthread.h>
#include <removefile.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "hakios.gl.h"
#include "hakios.input.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);
extern intptr_t _dyld_get_image_vmaddr_slide(uint32_t image_index);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

static uintptr_t slide;
#include "collection_safety.h"

/*
 * Where our own lines go: the console, whatever the program's stderr was made.  With
 * boot-arg hakios_quiet=1 the program's stdout and stderr go to /dev/null and only these
 * lines keep the console -- to tell whether a boot that crawls is being held back by the
 * log itself (every write to the console waits for the kernel's console, and launchd gives
 * every job /dev/console on fd 2).
 */
static int say_fd = 2;

static void
say(const char *format, ...)
{
	char line[256];
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

/* Replace one entry of a virtual table (unslid address `slot`) that must now hold `expected`. */
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if (*entry != expected + slide) {
		say("hakios: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: get a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* ---- backboardd: CA::WindowServer ------------------------------------------------------------ */

#define VT_APPLE_CLCD_DISPLAY   0x1d5b44520UL   /* vtable for CA::WindowServer::AppleCLCDDisplay */
#define VT_EAGL_SERVER          0x1d5b45c58UL   /* vtable for CA::WindowServer::EAGLServer */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

#define DISPLAY_FLAGS(display)  (*(uint64_t *)((char *)(display) + 0x9f2))
#define DISPLAY_B233(display)   (*(uint8_t *)((char *)(display) + 0x233))

static int (*orig_is_ready)(void *display);
static int (*orig_ignore_update_p)(void *display, void *update);
static uintptr_t (*orig_begin_update)(void *display, void *update);
static uintptr_t (*orig_finish_update)(void *display, void *update, unsigned flags);
static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);

static unsigned calls_ready, calls_ignore, calls_begin, calls_finish, calls_render;
static int last_ready = -1, last_ignore = -1;

static int
my_is_ready(void *display)
{
	int ready = orig_is_ready(display);

	if (ready != last_ready || calls_ready < 3) {
		say("hakios: is_ready -> %d (call %u, flags %#llx, b233 %#x)\n", ready, calls_ready,
		    (unsigned long long)DISPLAY_FLAGS(display), DISPLAY_B233(display));
		last_ready = ready;
	}
	calls_ready++;
	return ready;
}

static int
my_ignore_update_p(void *display, void *update)
{
	int ignore = orig_ignore_update_p(display, update) & 1;

	if (ignore != last_ignore || calls_ignore < 3) {
		uint64_t *q = (uint64_t *)((char *)display + 0x220);

		say("hakios: ignore_update_p -> %d (call %u, flags %#llx, update flags %#x)\n", ignore, calls_ignore,
		    (unsigned long long)DISPLAY_FLAGS(display), *(uint32_t *)((char *)update + 0xf0));
		say("hakios:   display +0x220: %#llx %#llx %#llx %#llx %#llx %#llx\n", (unsigned long long)q[0],
		    (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
		    (unsigned long long)q[5]);
		last_ignore = ignore;
	}
	calls_ignore++;
	return ignore;
}

static uintptr_t
my_begin_update(void *display, void *update)
{
	uintptr_t r = orig_begin_update(display, update);

	if (calls_begin++ < 6) {
		say("hakios: begin_update -> %#lx (flags %#llx)\n", r, (unsigned long long)DISPLAY_FLAGS(display));
	}
	return r;
}

static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
	uintptr_t r = orig_finish_update(display, update, flags);

	if (calls_finish++ < 6) {
		say("hakios: finish_update(%#x) -> %#lx\n", flags, r);
	}
	return r;
}

/*
 * Every frame the server asks for its renderer, and while there is none it tries again to make a
 * Metal and then an OpenGL ES context, which costs a lot and cannot succeed here. One try is enough:
 * without a renderer the server draws with CA::WindowServer::Server::sw_renderer().
 */
static void *
my_renderer(void *server)
{
	static int failed, said;
	void *renderer;

	if (failed) {
		return NULL;
	}
	renderer = orig_renderer(server);
	if (renderer == NULL) {
		failed = 1;
		say("hakios: no accelerated renderer; the software one will draw\n");
	} else if (!said) {
		said = 1;
		say("hakios: the server has an accelerated renderer (%p)\n", renderer);
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		say("hakios: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
	}
	return r;
}

/*
 * CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each: an
 * image as it is, an image through a colour matrix, a blur of so many samples, ...), and asks
 * its renderer which of them it has. Its OpenGL ES renderer has not half of them (the Metal one
 * has all). Which ones are asked for and missing is worth knowing: said once each.
 * (The last of the ninety, "cst", is not asked about: the OpenGL ES renderer draws it with no
 * program at all. The lock screen's wallpaper goes that way; see plain_draw_begin() in eagl.c.)
 */
#define VT_GLES_CONTEXT         0x1d5b3b540UL   /* vtable for CA::OGL::GLESContext */

static int gl_loaded;                           /* CoreAnimation draws with OpenGL ES (opengl_es() below) */
static void (*gl_function_refused)(unsigned function);  /* our OpenGL ES library keeps count (eagl.c, the census) */
static int (*orig_function_supported)(void *context);

/*
 * Boot-arg glyes=N[,N...]: the renderer is made to answer that it has those functions after all.
 * It has no shader for them, so CoreAnimation draws them with no program -- which is the way the
 * functions it never asks about are drawn, and which our OpenGL ES library stands in for
 * (eagl.c, plain_draw_begin()); without this, what needs such a function is not drawn at all.
 * For finding out what a missing function is for: with it the draw is made, its numbers arrive
 * (function_uniform below), and the picture says whether the thing was that function's.
 */
/*
 * Saying yes to a function the renderer has no shader for is not enough and not safe. Told yes for
 * "sb3" (40) -- the four samples of a shadow's blur, which our OpenGL ES library has a stand-in for
 * (eagl.c, blur4_fragment) -- CoreAnimation takes the way of one it has a shader for, finds none,
 * and the window server dies; it came back every half a minute, with SpringBoard after it, and the
 * stand-in was never reached: a function it asks about and is told yes to is not one it draws with
 * no program. What is needed is to hand it a program where it looks for one, which is another
 * entry of the same table. Until then it is told the truth, and what needs that function is not
 * drawn -- the shadow under a card of the switcher of running applications.
 */
/*
 * Where the window server died. Its crash reports never arrive here -- the kernel says "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is the end of it -- so a SIGSEGV in CoreAnimation has
 * been a silent restart every half a minute with nothing to go on. This turns one into a line: the
 * address it touched and where it was, both as they would be in the shared cache as built, which
 * "ipsw dyld a2s" turns into a name. Then the signal is let through to kill the process as before.
 */
/* Forward declarations */
id objc_retain(id obj);
void objc_release(id obj);
void objc_storeStrong(id *location, id obj);

static inline bool
is_tagged_obj(uintptr_t val)
{
	return ((val & (1ULL << 63)) != 0) || ((val & 1) != 0);
}

static inline bool
is_valid_isa(uintptr_t isa)
{
	if (isa == 0) return false;
	/* If bit 0 is 0, it is a raw class pointer */
	if ((isa & 1) == 0) {
		if (isa < 0x100000000ULL || (isa & 7) != 0) return false;
		return true;
	}
	/* If bit 0 is 1, it is an indexed/nonpointer isa */
	uintptr_t cls = isa & 0x0000000ffffffff8ULL;
	if (cls != 0 && cls < 0x100000000ULL) return false;
	return true;
}

static inline bool
is_valid_heap_obj(uintptr_t val)
{
	if (val == 0) return true;
	if (is_tagged_obj(val)) return true;
	if (val < 0x100000000ULL || (val & 7) != 0) return false;
	uintptr_t isa = *(uintptr_t *)val;
	if (!is_valid_isa(isa)) return false;
	return true;
}

static void
my_objc_storeStrong(id *location, id obj)
{
	uintptr_t val = (uintptr_t)obj;
	if (!is_valid_heap_obj(val)) {
		static int bad_count = 0;
		if (bad_count++ < 25) {
			say("hakios: %s[%d]: intercepted bad objc_storeStrong pointer %p (location=%p)\n",
			    getprogname(), getpid(), obj, location);
		}
		if (location != NULL) {
			id prev = *location;
			*location = NULL;
			if (prev != NULL && is_valid_heap_obj((uintptr_t)prev) && !is_tagged_obj((uintptr_t)prev)) {
				objc_release(prev);
			}
		}
		return;
	}

	id prev = location != NULL ? *location : NULL;
	if (obj == prev) {
		return;
	}
	if (obj != NULL && !is_tagged_obj(val)) {
		objc_retain(obj);
	}
	if (location != NULL) {
		*location = obj;
	}
	if (prev != NULL) {
		uintptr_t pval = (uintptr_t)prev;
		if (is_valid_heap_obj(pval) && !is_tagged_obj(pval)) {
			objc_release(prev);
		}
	}
}

#define DYLD_INTERPOSE(_replacment,_replacee) \
   __attribute__((used)) static struct{ const void* replacment; const void* replacee; } _interpose_##_replacee \
            __attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacment, (const void*)(unsigned long)&_replacee };

DYLD_INTERPOSE(my_objc_storeStrong, objc_storeStrong)

static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
	}
	/*
	 * And who called, by walking the frame pointers: on arm64 each frame holds the previous one
	 * at [fp] and its return address at [fp + 8]. The crash itself is usually in a library
	 * function -- strdup of a null name, say -- and the frame that matters is two or three up.
	 */
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("hakios: %s[%d]: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n",
	    getprogname(), getpid(), signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	{
		Dl_info dli;
		if (dladdr((void *)pc, &dli)) {
			if (dli.dli_sname) {
				say("hakios: %s[%d]:   pc %s in %s (+%#lx)\n",
				    getprogname(), getpid(), dli.dli_sname, dli.dli_fname ? dli.dli_fname : "?",
				    pc - (uintptr_t)dli.dli_saddr);
			} else {
				say("hakios: %s[%d]:   pc <%#lx unslid> in %s (+%#lx from base)\n",
				    getprogname(), getpid(), pc - slide, dli.dli_fname ? dli.dli_fname : "?",
				    pc - (uintptr_t)dli.dli_fbase);
			}
		}
		if (dladdr((void *)lr, &dli)) {
			if (dli.dli_sname) {
				say("hakios: %s[%d]:   lr %s in %s (+%#lx)\n",
				    getprogname(), getpid(), dli.dli_sname, dli.dli_fname ? dli.dli_fname : "?",
				    lr - (uintptr_t)dli.dli_saddr);
			} else {
				say("hakios: %s[%d]:   lr <%#lx unslid> in %s (+%#lx from base)\n",
				    getprogname(), getpid(), lr - slide, dli.dli_fname ? dli.dli_fname : "?",
				    lr - (uintptr_t)dli.dli_fbase);
			}
		}
		uintptr_t fp_walk = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;
		for (unsigned i = 0; i < 8 && fp_walk != 0 && (fp_walk & 7) == 0; i++) {
			uintptr_t next = ((uintptr_t *)fp_walk)[0], ret = ((uintptr_t *)fp_walk)[1];
			if (ret == 0) break;
			if (dladdr((void *)ret, &dli)) {
				if (dli.dli_sname) {
					say("hakios: %s[%d]:   #%u %s in %s (+%#lx)\n",
					    getprogname(), getpid(), i, dli.dli_sname, dli.dli_fname ? dli.dli_fname : "?",
					    ret - (uintptr_t)dli.dli_saddr);
				} else {
					say("hakios: %s[%d]:   #%u <%#lx unslid> in %s (+%#lx from base)\n",
					    getprogname(), getpid(), i, ret - slide, dli.dli_fname ? dli.dli_fname : "?",
					    ret - (uintptr_t)dli.dli_fbase);
				}
			}
			fp_walk = next > fp_walk ? next : 0;
		}
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
	/* An Objective-C exception that nobody catches ends in abort(), not in a fault, and a program
	   that dies that way says nothing at all here -- a run that restarted a daemon 31 times was
	   silent until these three were watched as well. */
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
	if (strstr(getprogname(), "containermanagerd") != NULL) {
		/* the manager traps about every eight seconds; this line says the handler is in */
		say("hakios: %s[%d]: faults watched, console on fd %d\n", getprogname(), getpid(),
		    say_fd);
	}
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
	id name = m_name ? ((id (*)(id, SEL))method_getImplementation(m_name))(exception, sel_name) : nil;
	id reason = m_reason ? ((id (*)(id, SEL))method_getImplementation(m_reason))(exception, sel_reason) : nil;
	char buf_name[128] = "", buf_reason[256] = "";

	if (name != nil) {
		CFStringGetCString((CFStringRef)name, buf_name, sizeof(buf_name), kCFStringEncodingUTF8);
	}
	if (reason != nil) {
		CFStringGetCString((CFStringRef)reason, buf_reason, sizeof(buf_reason), kCFStringEncodingUTF8);
	}
	say("hakios: %s[%d]: UNCAUGHT EXCEPTION: %s: %s\n",
	    getprogname(), getpid(), buf_name[0] ? buf_name : "(nil)", buf_reason[0] ? buf_reason : "(nil)");
}

/*
 * CoreAnimation's OpenGL ES renderer keeps the source of one fragment shader per function in a
 * table of its own -- unslid 0x1d5b54790, indexed by the function's number -- and the functions it
 * has no shader for are null entries there. That is the whole of "the renderer has no function
 * 40": GLContext::create_shader takes the source from that table, and function_supported answers
 * from the same place. Made to answer yes without the entry, create_shader hands the null to
 * strdup and the window server dies in strlen, which is what glyes=40 did for a whole afternoon.
 *
 * So the way to give the renderer a function is to give it a source. "sb3" (40) is the single
 * pass of a blur -- GaussianBlurFilter::render -> Context::blur_surface ->
 * BlurState::single_pass_blur -- the one that lays the shadow under a card of the switcher of
 * running applications. It is given the same four places as "ds4" (26) and has no weights of its
 * own, so this is ds4's own shader with the weights taken out, in the dialect the others are
 * written in (sampler2DRect, varying, gl_FragData -- the preamble is added by the renderer).
 *
 * Where this stands. With the entry filled the window server no longer dies: thirty-odd crashes a
 * run become none, with glyes=40 as well, and the function really is drawn -- the renderer rewrites
 * our source into its own dialect (sampler2DRect becomes sampler2D) and compiles it, and the
 * picture changes by a level or two across the whole screen where before it did not change at all.
 *
 * What it does not do is bring back a shadow, and the reason is that none was missing. This
 * function was taken up because a note here said the shadow under a card of the switcher was not
 * drawn; that note was reasoning from "the function is refused", not an observation. Against the
 * same frame drawn by CoreAnimation's software renderer -- which draws everything correctly, only
 * slowly -- the edge of a card is the same to the level: 18, 17, 17, then the card. There is no
 * soft falloff there in either, so there is nothing of that kind to restore.
 *
 * Its effect is nil for a second reason worth writing down: the four places reach the shader as
 * varyings that a vertex shader computes from "uniform highp vec2 offset[4]", and the two groups
 * of four numbers function_uniform delivers for it are of the order of 5e-19 -- zero, for a
 * blur. CoreAnimation asks for the single-pass blur exactly when the radius is too small to need
 * the multi-pass one, so the four samples coincide and the pass copies what it was given.
 */
#define CA_SHADER_TABLE         0x1d5b54790UL

static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";

static void
give_the_renderer_a_shader(unsigned function, const char *source)
{
	uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)function);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	if (*entry != 0) {
		say("hakios: CoreAnimation's function %u has a shader of its own; left alone\n", function);
		return;
	}
	/* the table is read-only memory shared by every process: a private writable copy of the page */
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("hakios: CoreAnimation's function %u: cannot make the table of shaders writable\n", function);
		return;
	}
	*entry = (uintptr_t)source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("hakios: CoreAnimation's function %u (\"sb3\") is given a shader of ours\n", function);
}

static uint8_t say_yes[90];
static int compile_shaders_said;                /* boot-arg glcompile=1: write down every shader source */

static void
functions_said_yes(const char *arguments)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	const char *argument = strstr(arguments, "glno=");

	/* glno=N[,N...]: and the other way about, for seeing what a function of ours stands in for */
	for (argument = argument != NULL ? argument + 5 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 0;
			say("hakios: the renderer will not answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
	argument = strstr(arguments, "glyes=");
	for (argument = argument != NULL ? argument + 6 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 1;
			say("hakios: the renderer will answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
}

/*
 * CA::OGL::Context::compile_shader(const char *) -- slot 0x060 of the same table. The OpenGL ES
 * renderer does not carry its shaders ready-made: it assembles the text of each from a preamble
 * (GLESContext::shader_preamble, slot 0x400) and pieces chosen by what the draw needs, and
 * compiles it here. That makes this the place where it looks for a program, and therefore the
 * place where one can be handed to it for a function it has no pieces for ("sb3", 40: the four
 * samples of a shadow's blur, missing under the cards of the switcher). This writes down what it
 * is asked to compile, so that a run with glyes=40 says whether a source is assembled at all
 * before the window server dies, and what it looks like if so.
 */
static uintptr_t (*orig_compile_shader)(void *context, const char *source);

static uintptr_t
my_compile_shader(void *context, const char *source)
{
	static unsigned told, seen;

	/* that it is called at all is worth one line, because a renderer that compiles nothing here
	   would mean the text is assembled and compiled somewhere this table does not reach */
	if (seen++ == 0) {
		say("hakios: CoreAnimation compiles its shaders through the table (the first of them, %zu characters)\n",
		    source != NULL ? strlen(source) : 0);
	}
	if (compile_shaders_said && told < 24) {
		told++;
		say("hakios: CoreAnimation compiles a shader, %zu characters:\n", source != NULL ? strlen(source) : 0);
		for (size_t at = 0, length = source != NULL ? strlen(source) : 0; at < length; at += 700) {
			say("hakios:  %.700s\n", source + at);
		}
	}
	return orig_compile_shader(context, source);
}

static int
my_function_supported(void *context)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function < 90 && say_yes[function] && gl_loaded) {
		if (!said[function]) {
			said[function] = 1;
			say("hakios: CoreAnimation's function %u (\"%.3s\"): the renderer is made to say it has it\n",
			    function, names + 4 * function);
		}
		return 1;
	}
	if (!supported && gl_function_refused != NULL) {
		gl_function_refused(function);
	}
	if (!supported && !said[function]) {
		said[function] = 1;
		say("hakios: CoreAnimation's OpenGL ES renderer has no function %u (\"%.3s\"): what needs it is not drawn\n",
		    function, function < 90 ? names + 4 * function : "?");
	}
	return supported;
}

/*
 * A function that takes numbers besides its textures (a colour matrix, say) is given them through
 * GLContext::function_uniform(), which hands them to the function's shader. The functions the
 * OpenGL ES renderer has no shader for are drawn all the same, with no program, and their numbers
 * go nowhere. Our OpenGL ES library draws some of those with programs of its own (eagl.c,
 * plain_draw_begin()): it is told the numbers here, and with them the context, in which it
 * finds which function a draw is.
 */
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	/*
	 * Who is calling. Told yes for a function it has no shader for (glyes=40), CoreAnimation gets
	 * this far -- it hands the function its numbers -- and then dies in whatever comes next, which
	 * is where it takes the program for the function out of a table of its own. These addresses,
	 * unslid, name that caller for "ipsw dyld a2s", and it is the only thread to pull: nothing is
	 * ever compiled for the function (no shader arrives at our library), and the compiler is not
	 * reached through the renderer's table either.
	 */
	if (say_yes[function < 90 ? function : 0]) {
		static uint8_t told[90];

		if (function < 90 && !told[function]) {
			told[function] = 1;
			say("hakios: function %u takes its numbers from %#lx %#lx %#lx\n", function,
			    (uintptr_t)__builtin_return_address(0) - slide,
			    (uintptr_t)__builtin_return_address(1) - slide,
			    (uintptr_t)__builtin_return_address(2) - slide);
		}
	}
	if (gl_function_values != NULL) {
		gl_function_values(context, function, index, components, count, values);
	}
	orig_function_uniform(context, index, components, count, values);
}

static void
window_server_fixes(void)
{
	orig_is_ready = replace_virtual("is_ready", VSLOT(VT_APPLE_CLCD_DISPLAY, 0xb0), 0x1882f0908UL, my_is_ready);
	orig_ignore_update_p = replace_virtual("ignore_update_p", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x138), 0x1882e0c90UL,
	    my_ignore_update_p);
	orig_begin_update = replace_virtual("begin_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x170), 0x1882ddda4UL,
	    my_begin_update);
	orig_finish_update = replace_virtual("finish_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x178), 0x1882c8568UL,
	    my_finish_update);
	orig_renderer = replace_virtual("renderer", VSLOT(VT_EAGL_SERVER, 0x1a0), 0x1882e2548UL, my_renderer);
	orig_render_update = replace_virtual("render_update", VSLOT(VT_EAGL_SERVER, 0x178), 0x18835770cUL,
	    my_render_update);
	if (gl_loaded) {
		orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), 0x1884a9308UL,
		    my_function_supported);
		if (gl_function_values != NULL) {
			orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), 0x1884a9350UL,
			    my_function_uniform);
		}
		/* always, because the boot-args are read in another place than this and the order of the
		   two is not ours to rely on; the hook writes nothing unless glcompile=1 */
		orig_compile_shader = (uintptr_t (*)(void *, const char *))replace_virtual("compile_shader",
		    VSLOT(VT_GLES_CONTEXT, 0x060), 0x1883fc808UL, my_compile_shader);
		say("hakios: the shader compiler of CoreAnimation is %s\n", orig_compile_shader ? "watched" : "NOT watched");
		watch_for_faults();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
			    strstr(arguments, "glsb3=1") != NULL) {
				give_the_renderer_a_shader(40, shadow_blur_source);
			}
		}
	}
	say("hakios: window server hooks %s\n", orig_is_ready && orig_ignore_update_p && orig_begin_update &&
	    orig_finish_update && orig_renderer && orig_render_update ? "installed" : "INCOMPLETE");
}

/* ---- backboardd: no ambient light sensor ------------------------------------------------------------ */

/*
 * CoreBrightness (-[BLControl waitForALSArrival], called while the window server starts) waits
 * five seconds for the ambient light sensor to turn up among the HID services. There is none
 * and none will come: do not wait for that.
 *
 * Wait for something else instead, at this convenient point: our own touch screen service.
 * backboardd takes for its touch screen a service that is there when its touch processing
 * starts, a moment later; one that turns up afterwards it does not notice. (For as long as
 * the five seconds were waited here, ours was always in time by accident.)
 */
static dispatch_semaphore_t input_services_made;

static void
wait_for_als_arrival(__unused id self, __unused SEL selector)
{
	if (input_services_made != NULL &&
	    dispatch_semaphore_wait(input_services_made, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
		say("hakios: input: no touch screen service after five seconds; going on without\n");
	}
}

__attribute__((unused)) static void
brightness_fixes(void)
{
	Class cls = objc_getClass("BLControl");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1af043734UL + slide) {
		say("hakios: -[BLControl waitForALSArrival]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)wait_for_als_arrival);
}

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on iOS (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("hakios: input: the event server asks for property \"%s\", which we do not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static volatile int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touch_mode) {
			say("hakios: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = value ? CFCopyDescription(value) : NULL;
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("hakios: input: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("hakios: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct hakios_touch *t)
{
	uint32_t change = t->type == HAKIOS_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != HAKIOS_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != HAKIOS_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == HAKIOS_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	CFRelease(hand);
}

/*
 * Whether the side button is held right now, for our code in SpringBoard (inject_sb.c,
 * lock_long_press()): kept as the state of a Darwin notification, 2 = held, 1 = released
 * (0, what a name nobody has set says, = not known), and set before the event is sent.
 */
static void
publish_lock_button(bool down)
{
	static int token = NOTIFY_TOKEN_INVALID;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}

static void
send_button(IOHIDServiceClientRef buttons, const struct hakios_touch *t)
{
	uint32_t usage = t->finger == HAKIOS_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == HAKIOS_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == HAKIOS_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == HAKIOS_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == HAKIOS_BUTTON_LOCK) {
		publish_lock_button(t->type == HAKIOS_BUTTON_DOWN);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == HAKIOS_BUTTON_DOWN, 0);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	CFRelease(event);
}

/*
 * Boot-arg touch_lat=1: a line for every touch sample, "tl: i TYPE posted US read US sent US" -- when
 * the kernel stamped it, when this thread had it, when the event system had the event -- in
 * microseconds of mach_absolute_time, the kernel's own time (tools/latency.py).
 */
static bool touch_latency;
static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

static void *
input_thread(__unused void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct hakios_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("redwood input");
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
	device = open(HAKIOS_TOUCH_DEVICE, O_RDONLY);
	if (device < 0) {
		say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
		return NULL;
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		/* this waits until the server (this very program) answers */
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			say("hakios: input: no HID client of type %d\n", types[i]);
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("redwood.input", NULL));
		screen = make_service(client, "HakiOS touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
		if (screen == NULL) {
			say("hakios: input: a HID client of type %d may not create services\n", types[i]);
		}
	}
	if (screen != NULL) {
		buttons = make_service(client, "HakiOS buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
	}
	say("hakios: input: touch screen service %p, buttons %p\n", (void *)screen, (void *)buttons);
	dispatch_semaphore_signal(input_services_made); /* see wait_for_als_arrival() */
	if (screen == NULL) {
		return NULL;
	}

	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		uint64_t read_at = mach_absolute_time();

		if (count++ < 4) {
			say("hakios: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > HAKIOS_TOUCH_UP) {
			if (buttons != NULL) {
				send_button(buttons, &t);
			}
		} else if (t.type == HAKIOS_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct hakios_touch press = { .time = t.time, .type = HAKIOS_BUTTON_DOWN, .finger = HAKIOS_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = HAKIOS_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == HAKIOS_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(screen, &t);
			if (touch_latency) {
				say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
				    micros(mach_absolute_time()));
			}
		}
	}
	say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
	return NULL;
}

static void
input_start(void)
{
	pthread_t thread;

	input_services_made = dispatch_semaphore_create(0);
	if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

/* ---- containermanagerd ---------------------------------------------------------------------------- */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

/*
 * The containers the system groups need, before the container manager looks for them.  It finds
 * a container by the .com.apple.mobile_container_manager.metadata.plist in it; the skeleton of
 * the data volume makes the directories but not those plists, so every system group container
 * is "not there" (MCM answers 127 / ENOENT, see collab.md 16:40) and LaunchServices' own store
 * never becomes ready.  The staging directory is where the manager builds a container before
 * moving it in: without it the same 127 comes back for every new one.
 */
#define MCMM_PLIST "/.com.apple.mobile_container_manager.metadata.plist"

static void
container_seed(void)
{
	static const char *const staging[] = {
		"/private/var/root/Library/Caches/com.apple.containermanagerd",
		"/private/var/root/Library/Caches/com.apple.containermanagerd/Staging",
	};
	static const struct {
		const char *path;
		const char *identifier;
		const char *uuid;
	} groups[] = {
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd",
		  "systemgroup.com.apple.lsd", "E7A1C205-0000-0000-0000-000000000001" },
		/*
		 * A live iPhone 6s (15.8.8) has this one next to it, and it is the icons cache: a
		 * named group container with Library/Caches/com.apple.IconsCache inside, owned by
		 * _iconservices, 355 entries.  The skeleton has neither, so the agent that draws every
		 * icon asks the container manager for a group it cannot have -- and lsd's one request
		 * to com.apple.iconservices is the last thing the file log shows on APFS (p264).
		 */
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache",
		  "systemgroup.com.apple.lsd.iconscache", "E7A1C205-0000-0000-0000-000000000003" },
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.powerlog",
		  "systemgroup.com.apple.powerlog", "E7A1C205-0000-0000-0000-000000000002" },
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.configurationprofiles",
		  "systemgroup.com.apple.configurationprofiles", "E7A1C205-0000-0000-0000-000000000004" },
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache",
		  "systemgroup.com.apple.mobilegestaltcache", "E7A1C205-0000-0000-0000-000000000005" },
		/*
		 * The rest of the named groups a live iPhone 6s has next to those (15.8.8, 26 in all).
		 * installcoordinationd is the interesting one here: LaunchServices' install progress
		 * sits in its journal recovery, and that is the thread that holds the database lock on
		 * the APFS volume -- its group container is one of the things it may be asking for.
		 */
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.installcoordinationd",
		  "systemgroup.com.apple.installcoordinationd", "E7A1C205-0000-0000-0000-000000000006" },
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.nsurlstoragedresources",
		  "systemgroup.com.apple.nsurlstoragedresources", "E7A1C205-0000-0000-0000-000000000007" },
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.ondemandresources",
		  "systemgroup.com.apple.ondemandresources", "E7A1C205-0000-0000-0000-000000000008" },
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.icloud.findmydevice.managed",
		  "systemgroup.com.apple.icloud.findmydevice.managed",
		  "E7A1C205-0000-0000-0000-000000000009" },
		{ "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.pisco.suinfo",
		  "systemgroup.com.apple.pisco.suinfo", "E7A1C205-0000-0000-0000-00000000000A" },
	};
	unsigned i;

	say("hakios: container seed: uid %d\n", getuid());
	/*
	 * mcm_fresh=1: the containers the image came with are moved aside before the manager scans
	 * them, so that it makes its own (a container the manager did not create is one it calls
	 * dead and moves into .../Dead: p188/p191).  Nothing is deleted -- the old trees are left
	 * with a .seeded suffix.
	 */
	{
		/* 2048: this port's boot args are over 600 characters, and sysctlbyname answers
		   ENOMEM with the buffer left empty when the one offered is too small -- which is
		   why the first attempt at mcm_fresh never took. */
		static char arguments[2048];
		size_t size = sizeof(arguments) - 1;

		arguments[0] = 0;
		sysctlbyname("kern.bootargs", arguments, &size, NULL, 0);
		say("hakios: container seed: fresh %d\n",
		    strstr(arguments, "mcm_fresh=1") != NULL);
		if (strstr(arguments, "mcm_fresh=1") != NULL) {
			static const char *const aside[] = {
				"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd",
				"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.powerlog",
				"/private/var/containers/Data/System/com.apple.lsd",
			};

			for (i = 0; i < sizeof(aside) / sizeof(aside[0]); i++) {
				char to[256];

				if (access(aside[i], F_OK) != 0) {
					say("hakios: container seed: %s is not there (%s)\n", aside[i],
					    strerror(errno));
					continue;
				}
				/* out of the containers tree: the manager reads every entry there and
				   an unknown one makes it throw (p198: ten SIGABRTs) */
				snprintf(to, sizeof(to), "/private/var/tmp/seeded-%.90s",
				    strrchr(aside[i], '/') + 1);
				if (syscall(SYS_rename, aside[i], to) == 0) {
					say("hakios: container seed: %s moved aside\n", aside[i]);
				} else {
					say("hakios: container seed: %s: %s\n", aside[i],
					    strerror(errno));
				}
			}
			i = 0;
		}
	}
	for (i = 0; i < sizeof(staging) / sizeof(staging[0]); i++) {
		if (mkdir(staging[i], 0700) != 0 && errno != EEXIST) {
			say("hakios: container seed: %s: %s\n", staging[i], strerror(errno));
		}
	}
	for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		char path[256], sub[256];
		char xml[1024];
		int fd, length;

		snprintf(path, sizeof(path), "%s" MCMM_PLIST, groups[i].path);
		if (access(path, F_OK) == 0) {
			continue;
		}
		mkdir(groups[i].path, 0777);
		snprintf(sub, sizeof(sub), "%s/Library", groups[i].path);
		mkdir(sub, 0777);
		snprintf(sub, sizeof(sub), "%s/Library/Caches", groups[i].path);
		mkdir(sub, 0777);
		snprintf(sub, sizeof(sub), "%s/Library/Preferences", groups[i].path);
		mkdir(sub, 0777);
		/*
		 * The icons cache itself.  A live iPhone 6s has it (355 entries, _iconservices), and
		 * p276 shows the agent asking for it every fifteen seconds and getting ENOENT: it
		 * never gets as far as answering lsd, which is what the store build waits for.
		 */
		if (strstr(groups[i].identifier, "iconscache") != NULL) {
			snprintf(sub, sizeof(sub), "%s/Library/Caches/com.apple.IconsCache",
			    groups[i].path);
			mkdir(sub, 0777);
		}
		/*
		 * The same plist a real data volume has (data48b.apfs: class 7 is a system group,
		 * schema 2).  Written as XML: the property list readers take either.
		 */
		length = snprintf(xml, sizeof(xml),
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
		    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
		    "<plist version=\"1.0\">\n<dict>\n"
		    "\t<key>MCMMetadataClass</key>\n\t<integer>7</integer>\n"
		    "\t<key>MCMMetadataIdentifier</key>\n\t<string>%s</string>\n"
		    "\t<key>MCMMetadataInfoCode</key>\n\t<integer>1</integer>\n"
		    "\t<key>MCMMetadataSchemaVersion</key>\n\t<integer>2</integer>\n"
		    "\t<key>MCMMetadataUUID</key>\n\t<string>%s</string>\n"
		    "</dict>\n</plist>\n",
		    groups[i].identifier, groups[i].uuid);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			say("hakios: container seed: %s: %s\n", path, strerror(errno));
			continue;
		}
		{
			ssize_t wrote = write(fd, xml, (size_t)length);

			if (wrote != length) {
				say("hakios: container seed: %s: wrote %ld of %d: %s\n", path,
				    (long)wrote, length, strerror(errno));
			}
		}
		close(fd);
		chown(path, 501, 501);
		/* Read it back: a plist that came out short is not a container the manager will take. */
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			char back[1024];
			ssize_t got = read(fd, back, sizeof(back) - 1);

			close(fd);
			say("hakios: container seed: %s: made, %ld of %d bytes back, starts \"%.24s\"\n",
			    groups[i].identifier, (long)got, length, got > 0 ? back : "");
		} else {
			say("hakios: container seed: %s: made, but cannot be read back: %s\n",
			    groups[i].identifier, strerror(errno));
		}
	}
}

/* ---- who removes a container ------------------------------------------------------------------ */

/*
 * The container manager makes and removes containers through NSFileManager.  The lsd one is there
 * when it starts and gone twenty seconds later (p168); the two calls below say which of them is
 * taken away, and when.
 */
static BOOL (*orig_remove_path)(id, SEL, id, id *);
static BOOL (*orig_remove_url)(id, SEL, id, id *);
static BOOL (*orig_move_path)(id, SEL, id, id, id *);
static BOOL (*orig_move_url)(id, SEL, id, id, id *);

static const char *
fm_text(id object, int as_url)
{
	id string;

	if (object == NULL) {
		return NULL;
	}
	string = as_url ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("path")) : object;
	if (string == NULL) {
		return NULL;
	}
	return ((const char *(*)(id, SEL))objc_msgSend)(string, sel_registerName("UTF8String"));
}

static BOOL
fm_remove_path(id self, SEL selector, id path, id *error)
{
	const char *text = fm_text(path, 0);

	if (text != NULL && strstr(text, "/containers/") != NULL) {
		say("hakios: mcm: removeItemAtPath %s\n", text);
	}
	return orig_remove_path(self, selector, path, error);
}

static BOOL
fm_remove_url(id self, SEL selector, id url, id *error)
{
	const char *text = fm_text(url, 1);

	if (text != NULL && strstr(text, "/containers/") != NULL) {
		say("hakios: mcm: removeItemAtURL %s\n", text);
	}
	return orig_remove_url(self, selector, url, error);
}

static BOOL
fm_move_path(id self, SEL selector, id from, id to, id *error)
{
	const char *source = fm_text(from, 0);
	const char *destination = fm_text(to, 0);

	if ((source != NULL && strstr(source, "/containers/") != NULL) ||
	    (destination != NULL && strstr(destination, "/containers/") != NULL)) {
		say("hakios: mcm: moveItemAtPath %s -> %s\n", source != NULL ? source : "-",
		    destination != NULL ? destination : "-");
	}
	return orig_move_path(self, selector, from, to, error);
}

static BOOL
fm_move_url(id self, SEL selector, id from, id to, id *error)
{
	const char *source = fm_text(from, 1);
	const char *destination = fm_text(to, 1);

	if ((source != NULL && strstr(source, "/containers/") != NULL) ||
	    (destination != NULL && strstr(destination, "/containers/") != NULL)) {
		say("hakios: mcm: moveItemAtURL %s -> %s\n", source != NULL ? source : "-",
		    destination != NULL ? destination : "-");
	}
	return orig_move_url(self, selector, from, to, error);
}

/*
 * The manager does not use NSFileManager for this: the container is renamed into the staging
 * directory and back (p170: the lsd container appears in Staging at t=16 s and is gone by 20 s).
 * These say the two names of every rename that touches /containers or /Staging.
 */

static int
worth_saying(const char *path)
{
	if (path == NULL) {
		return 0;
	}
	/* (the manager often works from a directory and a relative name: match those too) */
	return strstr(path, "containers") != NULL || strstr(path, "Staging") != NULL ||
	    strstr(path, "com.apple.lsd") != NULL || strstr(path, "SystemGroup") != NULL;
}

static int (*orig_rename)(const char *, const char *);
static int (*orig_renameat)(int, const char *, int, const char *);
static int (*orig_renamex_np)(const char *, const char *, unsigned int);
static int (*orig_renameatx_np)(int, const char *, int, const char *, unsigned int);
static int (*orig_unlink)(const char *);
static int (*orig_rmdir)(const char *);
static int (*orig_unlinkat)(int, const char *, int);

/*
 * The real function behind an interposed one.  dlsym(RTLD_NEXT, name) is supposed to find it,
 * but with __DATA,__interpose in the image it can come back as our own replacement: then every
 * call to it is a call to ourselves, and a process that removes a file named after /containers
 * dies of a stack overflow on the guard page (the container manager's SIGILL, p187: ten frames
 * all at hk_unlink).  When the name answers with our own address, the syscall is used instead.
 */
static void *
next_of(const char *name, const void *ours)
{
	void *found = dlsym(RTLD_NEXT, name);

	if (found == ours) {
		say("hakios: mcm: %s answers with our own hook; the syscall is used instead\n", name);
		return NULL;
	}
	return found;
}

static int
call_rename(const char *from, const char *to)
{
	return orig_rename != NULL ? orig_rename(from, to) : (int)syscall(SYS_rename, from, to);
}

static int
call_renameat(int from_fd, const char *from, int to_fd, const char *to)
{
	return orig_renameat != NULL ? orig_renameat(from_fd, from, to_fd, to) :
	       (int)syscall(SYS_renameat, from_fd, from, to_fd, to);
}

static int
call_renamex_np(const char *from, const char *to, unsigned int flags)
{
	return orig_renamex_np != NULL ? orig_renamex_np(from, to, flags) : ENOTSUP;
}

static int
call_renameatx_np(int from_fd, const char *from, int to_fd, const char *to, unsigned int flags)
{
	return orig_renameatx_np != NULL ? orig_renameatx_np(from_fd, from, to_fd, to, flags) :
	       (int)syscall(SYS_renameatx_np, from_fd, from, to_fd, to, flags);
}

static int
call_unlink(const char *path)
{
	return orig_unlink != NULL ? orig_unlink(path) : (int)syscall(SYS_unlink, path);
}

static int
call_rmdir(const char *path)
{
	return orig_rmdir != NULL ? orig_rmdir(path) : (int)syscall(SYS_rmdir, path);
}

static int
call_unlinkat(int fd, const char *path, int flags)
{
	return orig_unlinkat != NULL ? orig_unlinkat(fd, path, flags) :
	       (int)syscall(SYS_unlinkat, fd, path, flags);
}

static int
hk_rename(const char *from, const char *to)
{
	int result;
	int error;

	if (!worth_saying(from) && !worth_saying(to)) {
		return call_rename(from, to);
	}
	errno = 0;
	result = call_rename(from, to);
	error = errno;
	say("hakios: mcm: rename %s -> %s: %d%s%s\n", from != NULL ? from : "-",
	    to != NULL ? to : "-", result, result != 0 ? " " : "", result != 0 ? strerror(error) : "");
	errno = error;
	return result;
}

static int
hk_renameat(int from_fd, const char *from, int to_fd, const char *to)
{
	int result;
	int error;

	if (!worth_saying(from) && !worth_saying(to)) {
		return call_renameat(from_fd, from, to_fd, to);
	}
	errno = 0;
	result = call_renameat(from_fd, from, to_fd, to);
	error = errno;
	say("hakios: mcm: renameat %s -> %s: %d%s%s\n", from != NULL ? from : "-",
	    to != NULL ? to : "-", result, result != 0 ? " " : "", result != 0 ? strerror(error) : "");
	errno = error;
	return result;
}

DYLD_INTERPOSE(hk_rename, rename)
DYLD_INTERPOSE(hk_renameat, renameat)

/* Darwin's own variants, and the removals that do not go through NSFileManager */

static int
rename_said(const char *which, const char *from, const char *to, int result, int error)
{
	say("hakios: mcm: %s %s -> %s: %d%s%s\n", which, from != NULL ? from : "-",
	    to != NULL ? to : "-", result, result != 0 ? " " : "",
	    result != 0 ? strerror(error) : "");
	return result;
}

static int
hk_renamex_np(const char *from, const char *to, unsigned int flags)
{
	int result, error;

	if (!worth_saying(from) && !worth_saying(to)) {
		return call_renamex_np(from, to, flags);
	}
	errno = 0;
	result = call_renamex_np(from, to, flags);
	error = errno;
	errno = error;
	return rename_said("renamex_np", from, to, result, error);
}

static int
hk_renameatx_np(int from_fd, const char *from, int to_fd, const char *to, unsigned int flags)
{
	int result, error;

	if (!worth_saying(from) && !worth_saying(to)) {
		return call_renameatx_np(from_fd, from, to_fd, to, flags);
	}
	errno = 0;
	result = call_renameatx_np(from_fd, from, to_fd, to, flags);
	error = errno;
	errno = error;
	return rename_said("renameatx_np", from, to, result, error);
}

static int
hk_unlink(const char *path)
{
	int result, error;

	if (!worth_saying(path)) {
		return call_unlink(path);
	}
	errno = 0;
	result = call_unlink(path);
	error = errno;
	errno = error;
	return rename_said("unlink", path, "-", result, error);
}

static int
hk_rmdir(const char *path)
{
	int result, error;

	if (!worth_saying(path)) {
		return call_rmdir(path);
	}
	errno = 0;
	result = call_rmdir(path);
	error = errno;
	errno = error;
	return rename_said("rmdir", path, "-", result, error);
}

static int
hk_unlinkat(int fd, const char *path, int flags)
{
	int result, error;

	if (!worth_saying(path)) {
		return call_unlinkat(fd, path, flags);
	}
	errno = 0;
	result = call_unlinkat(fd, path, flags);
	error = errno;
	errno = error;
	return rename_said("unlinkat", path, "-", result, error);
}

DYLD_INTERPOSE(hk_renamex_np, renamex_np)
DYLD_INTERPOSE(hk_renameatx_np, renameatx_np)
DYLD_INTERPOSE(hk_unlink, unlink)
DYLD_INTERPOSE(hk_rmdir, rmdir)
DYLD_INTERPOSE(hk_unlinkat, unlinkat)

/*
 * The container manager does not remove containers through NSFileManager or the libc wrappers
 * (p170..p178: not one line from those, and the driver still sees a remove).  libremovefile is
 * the other way a directory tree goes: removefile() walks it itself and ends with rmdir, which
 * is where the tree refuses with ENOTEMPTY and the manager traps.  These say every call, its
 * flags and what it answered.
 */
static int (*orig_removefile)(const char *, removefile_state_t, removefile_flags_t);
static int (*orig_removefileat)(int, const char *, removefile_state_t, removefile_flags_t);

static const char *
removefile_flags_text(removefile_flags_t flags, char *buffer, size_t size)
{
	size_t used = 0;

	buffer[0] = 0;
#define FLAG_NAME(bit, text)                                                                    \
	if ((flags & (bit)) != 0) {                                                              \
		used += (size_t)snprintf(buffer + used, used < size ? size - used : 0, "%s%s",   \
		    used != 0 ? "|" : "", (text));                                              \
	}
	FLAG_NAME(REMOVEFILE_RECURSIVE, "recursive")
	FLAG_NAME(REMOVEFILE_KEEP_PARENT, "keep-parent")
	FLAG_NAME(REMOVEFILE_CROSS_MOUNT, "cross-mount")
	FLAG_NAME(REMOVEFILE_CLEAR_PURGEABLE, "clear-purgeable")
#undef FLAG_NAME
	if (used == 0) {
		(void)snprintf(buffer, size, "-");
	}
	return buffer;
}

static int
removefile_said(const char *which, const char *path, removefile_flags_t flags, int result,
    int error)
{
	char text[96];

	say("hakios: mcm: %s %s flags %s: %d%s%s\n", which, path != NULL ? path : "-",
	    removefile_flags_text(flags, text, sizeof(text)), result,
	    result != 0 ? " " : "", result != 0 ? strerror(error) : "");
	return result;
}

static int
hk_removefile(const char *path, removefile_state_t state, removefile_flags_t flags)
{
	int result, error;

	if (orig_removefile == NULL) {
		return -1;
	}
	if (!worth_saying(path)) {
		return orig_removefile(path, state, flags);
	}
	errno = 0;
	result = orig_removefile(path, state, flags);
	error = errno;
	errno = error;
	return removefile_said("removefile", path, flags, result, error);
}

static int
hk_removefileat(int fd, const char *path, removefile_state_t state, removefile_flags_t flags)
{
	int result, error;

	if (orig_removefileat == NULL) {
		return -1;
	}
	if (!worth_saying(path)) {
		return orig_removefileat(fd, path, state, flags);
	}
	errno = 0;
	result = orig_removefileat(fd, path, state, flags);
	error = errno;
	errno = error;
	return removefile_said("removefileat", path, flags, result, error);
}

DYLD_INTERPOSE(hk_removefile, removefile)
DYLD_INTERPOSE(hk_removefileat, removefileat)

/*
 * Which method of the container manager's own file manager does the removal.  The class list is
 * read at start (the framework is in the shared cache, so there is no file to look at), the
 * remove/delete selectors are said, and the ones whose arguments are plain (no floating point,
 * no structure returned) are hooked: one forwarder for all of them, the original kept by
 * selector.  The line says the selector, the answer says whether it went through.
 */
#define MCM_HOOK_MAX 64
static struct {
	SEL selector;
	IMP original;
} mcm_hooks[MCM_HOOK_MAX];
static unsigned mcm_hook_count;

static uint64_t
mcm_forward(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
    uint64_t a6, uint64_t a7)
{
	SEL selector = (SEL)a1;
	unsigned i;

	for (i = 0; i < mcm_hook_count; i++) {
		if (mcm_hooks[i].selector == selector) {
			uint64_t result = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
			    uint64_t, uint64_t, uint64_t, uint64_t))mcm_hooks[i].original)(a0, a1, a2,
			    a3, a4, a5, a6, a7);
			const char *cname = "-";

			if (a0 != 0) {
				Class c = object_getClass((id)a0);

				if (c != NULL) {
					cname = class_getName(c);
				}
			}
			say("hakios: mcm: %s -%s -> %llu\n", cname,
			    selector != NULL ? sel_getName(selector) : "?",
			    (unsigned long long)result);
			return result;
		}
	}
	say("hakios: mcm: -%s (original not kept; not called)\n",
	    selector != NULL ? sel_getName(selector) : "?");
	return 0;
}

/*
 * The move into the death row.  The manager calls a container dead by moving it to
 * .../com.apple.containermanagerd/Dead/<name> (p188/p190: the rename of com.apple.lsd into that
 * directory, done by containermanagerd itself), and the reclaimer deletes it later.  To find out
 * what the rest of the boot does when the container stays where it is, the move is intercepted:
 * it is said, and then answered with "yes" without moving anything.  Nothing else changes.
 */
static BOOL (*orig_mcm_move_item)(id, SEL, id, id, id *);
static BOOL (*orig_mcm_delete_url)(id, SEL, id, BOOL, id *);

static BOOL
mcm_move_item(id self, SEL selector, id from, id to, id *error)
{
	const char *source = fm_text(from, 1);
	const char *destination = fm_text(to, 1);

	say("hakios: mcm: moveItemAtURL %s -> %s\n", source != NULL ? source : "-",
	    destination != NULL ? destination : "-");
	if (destination != NULL && strstr(destination, "/Dead/") != NULL) {
		say("hakios: mcm: the move into Dead is answered yes, not moved\n");
		return YES;
	}
	return orig_mcm_move_item(self, selector, from, to, error);
}

static BOOL
mcm_delete_url(id self, SEL selector, id url, BOOL reclaim_soon, id *error)
{
	const char *text = fm_text(url, 1);

	if (text != NULL && strstr(text, "/Dead/") != NULL) {
		say("hakios: mcm: deleteURL %s (reclaim %d) is answered yes, not deleted\n", text,
		    reclaim_soon);
		return YES;
	}
	say("hakios: mcm: deleteURL %s (reclaim %d)\n", text != NULL ? text : "-", reclaim_soon);
	return orig_mcm_delete_url(self, selector, url, reclaim_soon, error);
}

static void
mcm_file_manager_watch(void)
{
	Class cls;
	Method method;

	cls = objc_getClass("MCMFileManager");
	if (cls != NULL) {
		method = class_getInstanceMethod(cls, sel_registerName("moveItemAtURL:toURL:error:"));
		if (method != NULL) {
			orig_mcm_move_item = (BOOL (*)(id, SEL, id, id, id *))method_getImplementation(method);
			method_setImplementation(method, (IMP)mcm_move_item);
			say("hakios: mcm: MCMFileManager moveItemAtURL:toURL:error: watched\n");
		}
	}
	cls = objc_getClass("MCMContainerFactory");
	if (cls != NULL) {
		method = class_getInstanceMethod(cls, sel_registerName("deleteURL:reclaimSoon:error:"));
		if (method != NULL) {
			orig_mcm_delete_url = (BOOL (*)(id, SEL, id, BOOL, id *))
			    method_getImplementation(method);
			method_setImplementation(method, (IMP)mcm_delete_url);
			say("hakios: mcm: MCMContainerFactory deleteURL:reclaimSoon:error: watched\n");
		}
	}
}

static int
plain_encoding(const char *types)
{
	if (types == NULL || types[0] == '{' || types[0] == '[') {
		return 0;
	}
	for (; *types != 0; types++) {
		if (*types == 'f' || *types == 'd' || *types == 'D') {
			return 0;
		}
	}
	return 1;
}

static int
removal_selector(const char *name)
{
	return strstr(name, "emove") != NULL || strstr(name, "elete") != NULL ||
	    strstr(name, "estroy") != NULL || strstr(name, "evacuate") != NULL;
}

/*
 * The same question one step earlier: which container is dead, which cache entry goes, which
 * cleanup runs.  The move into "Dead" is not a removal from the manager's point of view -- it is
 * a cache decision -- so the selectors that name it are watched as well.
 */
static int
decision_selector(const char *name)
{
	static const char *parts[] = {
		"Dead", "dead", "purge", "Purge", "stale", "Stale", "reclaim", "Reclaim",
		"clean", "Clean", "evict", "Evict", "prune", "Prune", "invalidate", "Invalidate",
		"orphan", "Orphan", "repair", "Repair"
	};

	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		if (strstr(name, parts[i]) != NULL) {
			return 1;
		}
	}
	return 0;
}

/*
 * Boot-arg mcm_calls=1: the manager's own remove/delete selectors are hooked, one forwarder for
 * all of them, and every call is said.  It was that way for p188..p213 and it stayed on by
 * accident: p213 crashed in mcm_forward eighteen times (SIGBUS at 0x1ed, objc_retain on a
 * forwarded argument of one of the selectors whose shape the forwarder does not know), and the
 * thousands of lines per boot ("libraryRepairForUserIdentity:", "ReclaimDiskSpace") were paid for
 * by the manager itself, which is the daemon every other one waits for.  Off unless asked for.
 * The move into Dead is not this: see mcm_file_manager_watch().
 */
static int
mcm_calls_on(void)
{
	static int state = -1;

	if (state < 0) {
		char arguments[2048];
		size_t size = sizeof(arguments) - 1;

		arguments[0] = 0;
		state = sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
		    strstr(arguments, "mcm_calls=1") != NULL;
	}
	return state;
}

static void
mcm_methods_watch(void)
{
	if (!mcm_calls_on()) {
		say("hakios: mcm: the removal selectors are not hooked (mcm_calls=1 turns them on)\n");
		return;
	}
	Class cls = objc_getClass("MCMFileManager");
	unsigned count = 0, i;
	Method *methods;

	if (cls == NULL) {
		say("hakios: mcm: no MCMFileManager class\n");
	} else {
		methods = class_copyMethodList(cls, &count);
		say("hakios: mcm: MCMFileManager has %u methods; the ones that remove:\n", count);
		for (i = 0; i < count; i++) {
			const char *name = sel_getName(method_getName(methods[i]));
			const char *types = method_getTypeEncoding(methods[i]);

			if (!removal_selector(name) && strstr(name, "reateDirectory") == NULL) {
				continue;
			}
			say("hakios: mcm:   -%s %s\n", name, types != NULL ? types : "?");
			if (removal_selector(name) && plain_encoding(types) &&
			    mcm_hook_count < MCM_HOOK_MAX) {
				mcm_hooks[mcm_hook_count].selector = method_getName(methods[i]);
				mcm_hooks[mcm_hook_count].original = method_getImplementation(methods[i]);
				method_setImplementation(methods[i], (IMP)mcm_forward);
				mcm_hook_count++;
			}
		}
		free(methods);
	}
	/* and every other class of the manager that has such a selector, by name only */
	{
		int total = objc_getClassList(NULL, 0);
		Class *classes;
		int j;

		if (total <= 0) {
			return;
		}
		classes = malloc(sizeof(Class) * (size_t)total);
		if (classes == NULL) {
			return;
		}
		total = objc_getClassList(classes, total);
		for (j = 0; j < total; j++) {
			const char *cname = class_getName(classes[j]);
			unsigned n = 0;
			Method *list;

			if (strncmp(cname, "MCM", 3) != 0) {
				continue;
			}
			list = class_copyMethodList(classes[j], &n);
			for (i = 0; i < n; i++) {
				const char *name = sel_getName(method_getName(list[i]));
				const char *types = method_getTypeEncoding(list[i]);

				if (removal_selector(name)) {
					say("hakios: mcm:   %s: -%s %s\n", cname, name,
					    types != NULL ? types : "?");
				} else if (decision_selector(name)) {
					say("hakios: mcm:   %s: -%s %s\n", cname, name,
					    types != NULL ? types : "?");
				}
				if ((removal_selector(name) || decision_selector(name)) &&
				    plain_encoding(types) && mcm_hook_count < MCM_HOOK_MAX) {
					int seen = 0;

					for (unsigned k = 0; k < mcm_hook_count; k++) {
						if (mcm_hooks[k].selector == method_getName(list[i])) {
							seen = 1;
						}
					}
					if (!seen) {
						mcm_hooks[mcm_hook_count].selector = method_getName(list[i]);
						mcm_hooks[mcm_hook_count].original =
						    method_getImplementation(list[i]);
						method_setImplementation(list[i], (IMP)mcm_forward);
						mcm_hook_count++;
					}
				}
			}
			free(list);
		}
		free(classes);
	}
}

static void
container_removal_watch(void)
{
	Class file_manager = objc_getClass("NSFileManager");
	Method method;

	if (file_manager == NULL) {
		return;
	}
	if ((method = class_getInstanceMethod(file_manager,
	    sel_registerName("removeItemAtPath:error:"))) != NULL) {
		orig_remove_path = (void *)method_getImplementation(method);
		method_setImplementation(method, (IMP)fm_remove_path);
	}
	if ((method = class_getInstanceMethod(file_manager,
	    sel_registerName("removeItemAtURL:error:"))) != NULL) {
		orig_remove_url = (void *)method_getImplementation(method);
		method_setImplementation(method, (IMP)fm_remove_url);
	}
	if ((method = class_getInstanceMethod(file_manager,
	    sel_registerName("moveItemAtPath:toPath:error:"))) != NULL) {
		orig_move_path = (void *)method_getImplementation(method);
		method_setImplementation(method, (IMP)fm_move_path);
	}
	if ((method = class_getInstanceMethod(file_manager,
	    sel_registerName("moveItemAtURL:toURL:error:"))) != NULL) {
		orig_move_url = (void *)method_getImplementation(method);
		method_setImplementation(method, (IMP)fm_move_url);
	}
	say("hakios: mcm: removals and moves under /containers are said\n");
	mcm_file_manager_watch();
	orig_rename = (int (*)(const char *, const char *))next_of("rename", (const void *)hk_rename);
	orig_renameat = (int (*)(int, const char *, int, const char *))next_of("renameat",
	    (const void *)hk_renameat);
	orig_renamex_np = (int (*)(const char *, const char *, unsigned int))next_of("renamex_np",
	    (const void *)hk_renamex_np);
	orig_renameatx_np = (int (*)(int, const char *, int, const char *, unsigned int))
	    next_of("renameatx_np", (const void *)hk_renameatx_np);
	orig_unlink = (int (*)(const char *))next_of("unlink", (const void *)hk_unlink);
	orig_rmdir = (int (*)(const char *))next_of("rmdir", (const void *)hk_rmdir);
	orig_unlinkat = (int (*)(int, const char *, int))next_of("unlinkat", (const void *)hk_unlinkat);
	orig_removefile = (int (*)(const char *, removefile_state_t, removefile_flags_t))
	    next_of("removefile", (const void *)hk_removefile);
	orig_removefileat = (int (*)(int, const char *, removefile_state_t, removefile_flags_t))
	    next_of("removefileat", (const void *)hk_removefileat);
	say("hakios: mcm: rename %s, renameat %s, renamex_np %s, unlink %s, removefile %s\n",
	    orig_rename != NULL ? "watched" : "not found",
	    orig_renameat != NULL ? "watched" : "not found",
	    orig_renamex_np != NULL ? "watched" : "not found",
	    orig_unlink != NULL ? "watched" : "not found",
	    orig_removefile != NULL ? "watched" : "not found");
	mcm_methods_watch();
}

/*
 * -[NSURL fixAndRetryIfPermissionsErrorWithURL:containerPath:containerIdentifier:error:duringBlock:]
 * is what the container schema runs its directory work in.  In this port the selector is not on
 * NSURL at all -- the manager's schema throws "unrecognized selector" and dies (p198/p199, ten
 * times a boot, but only now that the manager makes containers itself).  The method belongs to a
 * category of a library the daemon does not load here: the libraries are tried, and if the
 * selector is still missing, ours is added -- the block is run once, and the answer is yes.
 */
static BOOL
mcm_fix_retry(id self, SEL selector, id url, id container_path, id identifier, id *error,
    id block)
{
	say("hakios: mcm: fixAndRetry (ours) for %s\n", fm_text(url, 1) != NULL ? fm_text(url, 1) : "-");
	if (block != NULL) {
		/*
		 * The block is handed the URL to do its work on: called with nothing it reads the
		 * argument out of a register that is not there and dies at once (p201, SIGSEGV at
		 * 0x70 inside ContainerManagerCommon, from this very call).
		 */
		((void (^)(id))block)(url);
	}
	return YES;
}

static void
mcm_url_category(void)
{
	SEL selector = sel_registerName(
	    "fixAndRetryIfPermissionsErrorWithURL:containerPath:containerIdentifier:error:duringBlock:");
	Class cls = objc_getClass("NSURL");
	static const char *const libraries[] = {
		"/usr/lib/system/libsystem_containermanager.dylib",
		"/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager",
	};

	if (cls == NULL) {
		return;
	}
	if (class_getInstanceMethod(cls, selector) != NULL) {
		say("hakios: mcm: NSURL has fixAndRetry\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(libraries) / sizeof(libraries[0]); i++) {
		void *handle = dlopen(libraries[i], RTLD_NOW);

		say("hakios: mcm: dlopen %s -> %p\n", libraries[i], handle);
		if (handle != NULL && class_getInstanceMethod(cls, selector) != NULL) {
			say("hakios: mcm: NSURL has fixAndRetry after %s\n", libraries[i]);
			return;
		}
	}
	if (!class_addMethod(cls, selector, (IMP)mcm_fix_retry,
	    "B48@0:8@16@24@32^@40@?48")) {
		say("hakios: mcm: NSURL fixAndRetry could not be added\n");
		return;
	}
	say("hakios: mcm: NSURL fixAndRetry added\n");
}

/*
 * boot-arg lsd_delay=N: lsd waits N seconds before its first line of work.  Its container is made
 * by the container manager at start (p200/p203), and lsd is spawned before that: it looks for
 * .../SystemGroup/systemgroup.com.apple.lsd, finds it missing, and never makes its store -- which
 * is why the store request SpringBoard sends to its endpoint is never answered.  Waiting a few
 * seconds is the cheapest way to see whether the order is the whole of it.
 */
static void
start_delays(void)
{
	static char arguments[2048];
	size_t size = sizeof(arguments) - 1;
	const char *at;
	unsigned seconds;

	if (strcmp(getprogname(), "lsd") != 0) {
		return;
	}
	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "lsd_delay=");
	if (at == NULL) {
		return;
	}
	seconds = (unsigned)strtoul(at + 10, NULL, 10);
	if (seconds == 0 || seconds > 120) {
		return;
	}
	say("hakios: lsd: waiting %u s for the container manager\n", seconds);
	sleep(seconds);
	say("hakios: lsd: going on after %u s\n", seconds);
}

static void
container_manager_fixes(void)
{
	Class cls;
	Method method;
	IMP original;

	container_seed();
	container_removal_watch();
	mcm_url_category();
	cls = objc_getClass("MCMUserIdentityCache");
	method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1c3ea6298UL + slide) {
		say("hakios: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	say("hakios: container manager: no personas\n");
}

/* ---- NSURL --------------------------------------------------------------------------------------- */

/*
 * backboardd, when it sets up the display, asks MobileContainerManager for the caches path of the
 * process's system container and makes an NSURL of it. Here the container manager has no containers
 * to give (the Data volume starts with the empty containers/ tree, and MCM does not create one), the
 * path comes back nil, and Foundation throws "*** -[NSURL initFileURLWithPath:]: nil string
 * parameter" in -[MCMContainer cachesPath]; the exception kills backboardd each time. A nil path is
 * no reason to lose the window server: put a writable directory in its place.
 */
static id (*original_init_file_url)(id, SEL, id);
static id (*original_init_file_url_dir)(id, SEL, id, BOOL);
static id (*original_init_file_url_dir_rel)(id, SEL, id, BOOL, id);

static id
fallback_path(void)
{
	static id fallback;

	if (fallback == nil) {
		fallback = (id)CFStringCreateWithCString(kCFAllocatorDefault, "/private/var/tmp",
		    kCFStringEncodingUTF8);
	}
	return fallback;
}

static id
init_file_url_with_path(id self, SEL selector, id path)
{
	if (path == nil) {
		path = fallback_path();
		say("hakios: NSURL: a nil path became /private/var/tmp\n");
	}
	return original_init_file_url(self, selector, path);
}

static id
init_file_url_with_path_dir(id self, SEL selector, id path, BOOL is_directory)
{
	if (path == nil) {
		path = fallback_path();
		say("hakios: NSURL: a nil path became /private/var/tmp (isDirectory)\n");
	}
	return original_init_file_url_dir(self, selector, path, is_directory);
}

static id
init_file_url_with_path_dir_rel(id self, SEL selector, id path, BOOL is_directory, id base)
{
	if (path == nil) {
		path = fallback_path();
		say("hakios: NSURL: a nil path became /private/var/tmp (relative)\n");
	}
	return original_init_file_url_dir_rel(self, selector, path, is_directory, base);
}

static id ls_create_file_url(const char *cpath, BOOL isDir);

/* ---- NSFileManager ------------------------------------------------------------------------------ */

/*
 * The same missing container, one level up: -URLsForDirectory: and -URLForDirectory: answer nil
 * for the caches and application-support directories the container manager would have made, and
 * -enumeratorAtURL: is then called with that nil.  Foundation throws, and the process is gone.
 * A writable directory stands in: /private/var/tmp, which every one of these processes may use.
 */
static id
fm_fallback_url(void)
{
	return ls_create_file_url("/private/var/tmp", YES);
}

static id (*original_urls_for_directory)(id, SEL, unsigned long, unsigned long);
static id (*original_url_for_directory)(id, SEL, unsigned long, unsigned long, id, BOOL, id *);
static id (*original_enumerator_at_url)(id, SEL, id, id, unsigned long, id);
static id (*original_contents_of_dir)(id, SEL, id, id, unsigned long, id *);

static id
urls_for_directory(id self, SEL selector, unsigned long directory, unsigned long domains)
{
	id urls = original_urls_for_directory(self, selector, directory, domains);

	if (urls == nil ||
	    ((unsigned long (*)(id, SEL))objc_msgSend)(urls, sel_registerName("count")) == 0) {
		say("hakios: NSFileManager: no URL for directory %lu; /private/var/tmp\n", directory);
		return ((id (*)(id, SEL, id))objc_msgSend)(objc_getClass("NSArray"),
		    sel_registerName("arrayWithObject:"), fm_fallback_url());
	}
	return urls;
}

static id
url_for_directory(id self, SEL selector, unsigned long directory, unsigned long domain, id url,
    BOOL create, id *error)
{
	id result = original_url_for_directory(self, selector, directory, domain, url, create, error);

	if (result == nil) {
		say("hakios: NSFileManager: no URL for directory %lu (domain %lu); /private/var/tmp\n",
		    directory, domain);
		return fm_fallback_url();
	}
	return result;
}

static id
enumerator_at_url(id self, SEL selector, id url, id keys, unsigned long options, id handler)
{
	if (url == nil) {
		say("hakios: NSFileManager: enumerator with a nil URL; /private/var/tmp\n");
		url = fm_fallback_url();
	}
	return original_enumerator_at_url(self, selector, url, keys, options, handler);
}

static id
contents_of_dir(id self, SEL selector, id url, id keys, unsigned long options, id *error)
{
	if (url == nil) {
		say("hakios: NSFileManager: contents of a nil URL; /private/var/tmp\n");
		url = fm_fallback_url();
	}
	return original_contents_of_dir(self, selector, url, keys, options, error);
}

static void
file_manager_fixes(void)
{
	Class cls = objc_getClass("NSFileManager");
	Method m;

	if (cls == NULL) {
		say("hakios: NSFileManager: not there, left alone\n");
		return;
	}
	if ((m = class_getInstanceMethod(cls, sel_registerName("URLsForDirectory:inDomains:"))) != NULL) {
		original_urls_for_directory = (void *)method_getImplementation(m);
		method_setImplementation(m, (IMP)urls_for_directory);
	}
	if ((m = class_getInstanceMethod(cls,
	    sel_registerName("URLForDirectory:inDomain:appropriateForURL:create:error:"))) != NULL) {
		original_url_for_directory = (void *)method_getImplementation(m);
		method_setImplementation(m, (IMP)url_for_directory);
	}
	if ((m = class_getInstanceMethod(cls,
	    sel_registerName("enumeratorAtURL:includingPropertiesForKeys:options:errorHandler:"))) != NULL) {
		original_enumerator_at_url = (void *)method_getImplementation(m);
		method_setImplementation(m, (IMP)enumerator_at_url);
	}
	if ((m = class_getInstanceMethod(cls,
	    sel_registerName("contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:"))) != NULL) {
		original_contents_of_dir = (void *)method_getImplementation(m);
		method_setImplementation(m, (IMP)contents_of_dir);
	}
	say("hakios: NSFileManager: a missing directory URL becomes /private/var/tmp\n");
}

static void
url_path_fixes(void)
{
	Class cls = objc_getClass("NSURL");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("initFileURLWithPath:")) : NULL;
	Method dir = cls ? class_getInstanceMethod(cls, sel_registerName("initFileURLWithPath:isDirectory:")) : NULL;
	Method rel = cls ? class_getInstanceMethod(cls,
	    sel_registerName("initFileURLWithPath:isDirectory:relativeToURL:")) : NULL;

	if (method == NULL) {
		say("hakios: NSURL: no -initFileURLWithPath:, left alone\n");
		return;
	}
	original_init_file_url = (void *)method_getImplementation(method);
	method_setImplementation(method, (IMP)init_file_url_with_path);
	if (dir != NULL) {
		original_init_file_url_dir = (void *)method_getImplementation(dir);
		method_setImplementation(dir, (IMP)init_file_url_with_path_dir);
	}
	if (rel != NULL) {
		original_init_file_url_dir_rel = (void *)method_getImplementation(rel);
		method_setImplementation(rel, (IMP)init_file_url_with_path_dir_rel);
	}
	say("hakios: NSURL: a nil path becomes /private/var/tmp (%s%s)\n",
	    dir ? "isDirectory" : "no isDirectory", rel ? ", relative" : "");
}

/* ---- a nil key in a dictionary ------------------------------------------------------------------ */

/*
 * -[__NSDictionaryM setObject:forKeyedSubscript:] with a nil key (or a nil object) throws -- "key
 * cannot be nil" -- and takes the program with it: imagent did it in a loop on this port, and
 * assetsd once the permission daemon let it as far as the photo library.  The call returns void,
 * so there is nothing to answer: the call is dropped, once per program with a line, and the
 * caller goes on.  What it wanted to remember under no name is not something a port can know.
 */
static void (*dictionary_set_subscript)(id, SEL, id, id);

static void
watched_set_subscript(id self, SEL cmd, id object, id key)
{
	static int said;

	if (key == NULL || object == NULL) {
		if (said++ < 8) {
			char line[220];
			int n = snprintf(line, sizeof(line),
			    "hakios: %s: setObject:forKeyedSubscript: with a nil %s, the write is dropped\n",
			    getprogname(), key == NULL ? "key" : "object");

			say(line, (size_t)(n > 0 ? n : 0));
		}
		return;
	}
	dictionary_set_subscript(self, cmd, object, key);
}

static void
dictionary_fixes(void)
{
	static const char *const classes[] = { "__NSDictionaryM", "NSMutableDictionary" };
	int hooked = 0;

	for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
		Class cls = objc_getClass(classes[i]);
		Method method = cls != NULL ?
		    class_getInstanceMethod(cls, sel_registerName("setObject:forKeyedSubscript:")) : NULL;

		if (method == NULL || method_getImplementation(method) == (IMP)watched_set_subscript) {
			continue;
		}
		dictionary_set_subscript = (void *)method_getImplementation(method);
		method_setImplementation(method, (IMP)watched_set_subscript);
		hooked++;
	}
	say("hakios: NSMutableDictionary: a nil key or object is dropped (%d classes)\n", hooked);
}

/* ---- _LSDefaults --------------------------------------------------------------------------------- */

/*
 * LaunchServices daemon (lsd) queries containermanagerd for systemgroup.com.apple.lsd
 * via -[_LSDefaults userContainerURL]. Because containermanagerd cannot find persona type 6,
 * it returns NULL, and lsd calls abort() ("Could not get user container URL! Must abort.").
 * We provide valid file URLs pointing directly to the pre-created directories in /private/var/containers.
 */
static id
ls_create_file_url(const char *cpath, BOOL isDir)
{
	Class cls_url = objc_getClass("NSURL");
	Class cls_str = objc_getClass("NSString");
	SEL sel_str = sel_registerName("stringWithUTF8String:");
	SEL sel_url = sel_registerName("fileURLWithPath:isDirectory:");

	if (cls_url && cls_str && sel_str && sel_url) {
		id str = ((id (*)(Class, SEL, const char *))objc_msgSend)(cls_str, sel_str, cpath);
		if (str) {
			id url = ((id (*)(Class, SEL, id, BOOL))objc_msgSend)(cls_url, sel_url, str, isDir);
			if (url) {
				return url;
			}
		}
	}
	CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, cpath, kCFStringEncodingUTF8);
	CFURLRef cfurl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, path, kCFURLPOSIXPathStyle, (Boolean)isDir);
	CFRelease(path);
	SEL sel_ar = sel_registerName("autorelease");
	if (sel_ar && cfurl) {
		return ((id (*)(id, SEL))objc_msgSend)((id)cfurl, sel_ar);
	}
	return (id)cfurl;
}

static id
ls_create_url(const char *cpath)
{
	return ls_create_file_url(cpath, YES);
}

static id
hook_LSDefaults_userContainerURL(__unused id self, __unused SEL _cmd)
{
	static int logged;
	if (!logged++) {
		say("hakios: -[_LSDefaults userContainerURL] -> /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd\n");
	}
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd", YES);
}

static id
hook_LSDefaults_systemGroupContainerURL(__unused id self, __unused SEL _cmd)
{
	static int logged;
	if (!logged++) {
		say("hakios: -[_LSDefaults systemGroupContainerURL] -> /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd\n");
	}
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd", YES);
}

static id
hook_LSDefaults_systemContainerURL(__unused id self, __unused SEL _cmd)
{
	static int logged;
	if (!logged++) {
		say("hakios: -[_LSDefaults systemContainerURL] -> /private/var/containers/Data/System/com.apple.lsd\n");
	}
	return ls_create_file_url("/private/var/containers/Data/System/com.apple.lsd", YES);
}

static id
hook_LSDefaults_systemGroupContainerURL_arg(__unused id self, __unused SEL _cmd, id identifier)
{
	const char *ident_c = NULL;
	if (identifier) {
		SEL sel_utf8 = sel_registerName("UTF8String");
		if (class_getInstanceMethod(object_getClass(identifier), sel_utf8)) {
			ident_c = ((const char *(*)(id, SEL))objc_msgSend)(identifier, sel_utf8);
		}
	}
	if (ident_c && ident_c[0]) {
		char path[512];
		snprintf(path, sizeof(path), "/private/var/containers/Shared/SystemGroup/%s", ident_c);
		return ls_create_file_url(path, YES);
	}
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd", YES);
}

static id
hook_LSDefaults_systemContainerURL_arg(__unused id self, __unused SEL _cmd, id identifier)
{
	const char *ident_c = NULL;
	if (identifier) {
		SEL sel_utf8 = sel_registerName("UTF8String");
		if (class_getInstanceMethod(object_getClass(identifier), sel_utf8)) {
			ident_c = ((const char *(*)(id, SEL))objc_msgSend)(identifier, sel_utf8);
		}
	}
	if (ident_c && ident_c[0]) {
		char path[512];
		snprintf(path, sizeof(path), "/private/var/containers/Data/System/%s", ident_c);
		return ls_create_file_url(path, YES);
	}
	return ls_create_file_url("/private/var/containers/Data/System/com.apple.lsd", YES);
}

static id
hook_LSDefaults_databaseStoreFileURL(__unused id self, __unused SEL _cmd)
{
	static int logged;
	if (!logged++) {
		say("hakios: -[_LSDefaults databaseStoreFileURL] -> /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.datastore\n");
	}
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.datastore", NO);
}

static id
hook_LSDefaults_databaseStoreDirectoryURL(__unused id self, __unused SEL _cmd)
{
	static int logged;
	if (!logged++) {
		say("hakios: -[_LSDefaults databaseStoreDirectoryURL] -> /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd\n");
	}
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd", YES);
}

static id
hook_LSDefaults_systemContentDatabaseStoreFileURL(__unused id self, __unused SEL _cmd)
{
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.systemdatastore", NO);
}

static id
hook_LSDefaults_userContentDatabaseStoreFileURL(__unused id self, __unused SEL _cmd)
{
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.userdatastore", NO);
}

static id
hook_LSDefaults_dbRecoveryFileURL(__unused id self, __unused SEL _cmd)
{
	static int logged;
	if (!logged++) {
		say("hakios: -[_LSDefaults dbRecoveryFileURL] -> /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.dbRecovery\n");
	}
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.dbRecovery", NO);
}

static id
hook_LSDefaults_dbSentinelFileURL(__unused id self, __unused SEL _cmd)
{
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.dbSentinel", NO);
}

static id
hook_LSDefaults_queriedSchemesMapFileURL(__unused id self, __unused SEL _cmd)
{
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.queriedSchemesMap", NO);
}

static id
hook_LSDefaults_identifiersFileURL(__unused id self, __unused SEL _cmd)
{
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.identifiers", NO);
}

static id
hook_LSDefaults_preferencesFileURL(__unused id self, __unused SEL _cmd)
{
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.preferences", NO);
}

static id
hook_LSDefaults_securePropertiesFileURL(__unused id self, __unused SEL _cmd)
{
	return ls_create_file_url("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd/com.apple.lsd.secureProperties", NO);
}

static void
hook_or_add_instance_method(Class cls, const char *sel_name, IMP imp, const char *types)
{
	SEL sel = sel_registerName(sel_name);
	Method m = class_getInstanceMethod(cls, sel);
	if (m != NULL) {
		method_setImplementation(m, imp);
		say("hakios: _LSDefaults: hooked -[%s %s] (replaced)\n", class_getName(cls), sel_name);
	} else {
		class_addMethod(cls, sel, imp, types);
		say("hakios: _LSDefaults: hooked -[%s %s] (added)\n", class_getName(cls), sel_name);
	}
}

static void
hook_class_method_if_present(Class cls, const char *sel_name, IMP imp)
{
	SEL sel = sel_registerName(sel_name);
	Method m = class_getClassMethod(cls, sel);
	if (m != NULL) {
		method_setImplementation(m, imp);
		say("hakios: _LSDefaults: hooked +[%s %s] (replaced)\n", class_getName(cls), sel_name);
	}
}

static void
ls_defaults_fixes(void)
{
	Class cls = objc_getClass("_LSDefaults");
	if (cls == NULL) {
		return;
	}

	mkdir("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd", 0777);

	hook_or_add_instance_method(cls, "userContainerURL", (IMP)hook_LSDefaults_userContainerURL, "@@:");
	hook_or_add_instance_method(cls, "systemContainerURL", (IMP)hook_LSDefaults_systemContainerURL, "@@:");
	hook_or_add_instance_method(cls, "systemGroupContainerURL", (IMP)hook_LSDefaults_systemGroupContainerURL, "@@:");

	SEL sel_grp_id = sel_registerName("systemGroupContainerURLForIdentifier:");
	if (class_getInstanceMethod(cls, sel_grp_id) != NULL) {
		hook_or_add_instance_method(cls, "systemGroupContainerURLForIdentifier:", (IMP)hook_LSDefaults_systemGroupContainerURL_arg, "@@:@");
	}
	SEL sel_sys_id = sel_registerName("systemContainerURLForIdentifier:");
	if (class_getInstanceMethod(cls, sel_sys_id) != NULL) {
		hook_or_add_instance_method(cls, "systemContainerURLForIdentifier:", (IMP)hook_LSDefaults_systemContainerURL_arg, "@@:@");
	}

	hook_or_add_instance_method(cls, "databaseStoreFileURL", (IMP)hook_LSDefaults_databaseStoreFileURL, "@@:");
	hook_or_add_instance_method(cls, "databaseStoreDirectoryURL", (IMP)hook_LSDefaults_databaseStoreDirectoryURL, "@@:");
	hook_or_add_instance_method(cls, "systemContentDatabaseStoreFileURL", (IMP)hook_LSDefaults_systemContentDatabaseStoreFileURL, "@@:");
	hook_or_add_instance_method(cls, "userContentDatabaseStoreFileURL", (IMP)hook_LSDefaults_userContentDatabaseStoreFileURL, "@@:");
	hook_or_add_instance_method(cls, "dbRecoveryFileURL", (IMP)hook_LSDefaults_dbRecoveryFileURL, "@@:");
	hook_or_add_instance_method(cls, "dbSentinelFileURL", (IMP)hook_LSDefaults_dbSentinelFileURL, "@@:");
	hook_or_add_instance_method(cls, "queriedSchemesMapFileURL", (IMP)hook_LSDefaults_queriedSchemesMapFileURL, "@@:");
	hook_or_add_instance_method(cls, "identifiersFileURL", (IMP)hook_LSDefaults_identifiersFileURL, "@@:");
	hook_or_add_instance_method(cls, "preferencesFileURL", (IMP)hook_LSDefaults_preferencesFileURL, "@@:");
	hook_or_add_instance_method(cls, "securePropertiesFileURL", (IMP)hook_LSDefaults_securePropertiesFileURL, "@@:");

	hook_class_method_if_present(cls, "userContainerURL", (IMP)hook_LSDefaults_userContainerURL);
	hook_class_method_if_present(cls, "systemContainerURL", (IMP)hook_LSDefaults_systemContainerURL);
	hook_class_method_if_present(cls, "systemGroupContainerURL", (IMP)hook_LSDefaults_systemGroupContainerURL);
	hook_class_method_if_present(cls, "databaseStoreFileURL", (IMP)hook_LSDefaults_databaseStoreFileURL);
	hook_class_method_if_present(cls, "databaseStoreDirectoryURL", (IMP)hook_LSDefaults_databaseStoreDirectoryURL);
	hook_class_method_if_present(cls, "systemContentDatabaseStoreFileURL", (IMP)hook_LSDefaults_systemContentDatabaseStoreFileURL);
	hook_class_method_if_present(cls, "userContentDatabaseStoreFileURL", (IMP)hook_LSDefaults_userContentDatabaseStoreFileURL);
	hook_class_method_if_present(cls, "dbRecoveryFileURL", (IMP)hook_LSDefaults_dbRecoveryFileURL);
	hook_class_method_if_present(cls, "dbSentinelFileURL", (IMP)hook_LSDefaults_dbSentinelFileURL);

	say("hakios: _LSDefaults fixes installed\n");
}

/* ---------------------------------------------------------------------------------------------- */

/*
 * An OpenGL ES implementation for CoreAnimation (src/eagl.c, Mesa behind it): loaded, it puts
 * itself behind EAGLContext, and the renderer the server then gets is the OpenGL ES one. That
 * is worth it where a GPU does the drawing, and only there: on the phone, when the kernel has
 * started the GPU and keeps it (boot-arg gpu=1), which the service HakiosGPU says (it is there
 * before the first program runs, or not at all). Everywhere else -- QEMU, a run without the
 * boot-arg, a phone without the GPU's firmware in the image, a GPU that did not start --
 * nothing is loaded, and CoreAnimation draws with its own software renderer as in an image
 * without the library.
 * Boot-arg gl=0: not even with a GPU; gl=1: also without one, by Mesa's software driver (slow;
 * for working on eagl.c in QEMU).
 */
/*
 * Debug: boot-arg hakios_trap=SECS makes this process trap itself after that many seconds,
 * so the fault handler prints where it was: a daemon or SpringBoard that never comes up can
 * be read off the stack instead of guessed at.
 */
static pthread_t main_thread;

/*
 * The calls SQLite makes for a WAL database's shared-memory file, from this process and
 * with this user: which one fails where the store reports SQLITE_IOERR_SHMSIZE.
 */
static void
shm_probe(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	char path[128];
	struct stat st;
	void *m;
	int fd, r, e;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 ||
	    strstr(arguments, "shm_probe=1") == NULL) {
		return;
	}
	snprintf(path, sizeof(path), "/private/var/tmp/shmprobe-%d", getpid());
	say("hakios: shm probe pid %d uid %d: %s\n", getpid(), getuid(), path);
	errno = 0; fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644); e = errno;
	say("  open: %d (%s)\n", fd, fd < 0 ? strerror(e) : "ok");
	if (fd < 0) {
		return;
	}
	errno = 0; r = ftruncate(fd, 32768); e = errno;
	say("  ftruncate: %d (%s)\n", r, r ? strerror(e) : "ok");
	errno = 0; r = fstat(fd, &st); e = errno;
	say("  fstat: %d, size %lld (%s)\n", r, (long long)st.st_size, r ? strerror(e) : "ok");
	errno = 0; m = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); e = errno;
	say("  mmap: %p (%s)\n", m, m == MAP_FAILED ? strerror(e) : "ok");
	if (m != MAP_FAILED) {
		((volatile char *)m)[0] = 1;
		((volatile char *)m)[32767] = 1;
		errno = 0; r = msync(m, 32768, MS_SYNC); e = errno;
		say("  msync: %d (%s)\n", r, r ? strerror(e) : "ok");
		munmap(m, 32768);
	}
	close(fd);
	unlink(path);
}

static void *
trap_after(void *arg)
{
	long secs = (long)(intptr_t)arg;

	sleep((unsigned)secs);
	say("hakios: watchdog: %ld s, SIGTRAP for the main thread's stack\n", secs);
	if (main_thread != NULL) {
		pthread_kill(main_thread, SIGTRAP);
	} else {
		kill(getpid(), SIGTRAP);
	}
	return NULL;
}


/* ---- who is waiting, in every daemon that has this library ----------------------------------- */

static const char *
sample_image_of(uintptr_t address, uintptr_t *offset)
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
 * Which syscall a thread is inside.  libsystem_kernel's plain wrappers are "movz x16, #N;
 * svc #0x80; ret", and the pc the thread is saved with while it is in svc is the instruction
 * after it: read the words around it and take the number out of the movz.  -1: not such a
 * wrapper (a function of its own, or the pc is not in libsystem_kernel).
 */
static int
sample_syscall_at(uintptr_t pc)
{
	uint32_t words[6];
	mach_vm_size_t got = 0;
	unsigned k;

	if (pc < 16) {
		return -1;
	}
	if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(pc - 16), sizeof(words),
	    (mach_vm_address_t)words, &got) != KERN_SUCCESS || got != sizeof(words)) {
		return -1;
	}
	for (k = 0; k + 1 < 6; k++) {
		uint32_t op = words[k];

		if (words[k + 1] == 0xd4001001u &&          /* svc #0x80 */
		    ((op & 0xffe0001fu) == 0xd2800010u ||   /* movz x16, #imm */
		    (op & 0xffe0001fu) == 0x52800010u)) {   /* movz w16, #imm */
			return (int)((op >> 5) & 0xffff);
		}
	}
	return -1;
}

static void
sample_threads(const char *who, int seconds)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	mach_port_t me = mach_task_self();
	unsigned i;

	say("hakios: Sample: %s, %d s after start\n", who, seconds);
	if (task_threads(me, &threads, &count) != KERN_SUCCESS) {
		return;
	}
	for (i = 0; i < count; i++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
		struct thread_basic_info basic;
		mach_msg_type_number_t basic_count = THREAD_BASIC_INFO_COUNT;
		int run_state = -1;
		uintptr_t fp, offset;
		const char *image;
		unsigned frame;

		if (thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&basic, &basic_count) == KERN_SUCCESS) {
			run_state = basic.run_state;
		}
		if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &state_count) != KERN_SUCCESS) {
			continue;
		}
		image = sample_image_of((uintptr_t)arm_thread_state64_get_pc(state), &offset);
		if (image != NULL) {
			say("hakios: Sample %s t%u run %d pc %s+%#lx\n", who, i, run_state, image,
			    (unsigned long)offset);
		}
		{
			int sysc = sample_syscall_at((uintptr_t)arm_thread_state64_get_pc(state));

			if (sysc >= 0) {
				say("hakios: Sample %s t%u   syscall %d\n", who, i, sysc);
			}
		}
		image = sample_image_of((uintptr_t)arm_thread_state64_get_lr(state), &offset);
		if (image != NULL) {
			say("hakios: Sample %s t%u   lr %s+%#lx\n", who, i, image, (unsigned long)offset);
		}
		fp = (uintptr_t)arm_thread_state64_get_fp(state);
		for (frame = 0; frame < 6 && fp != 0 && (fp & 7) == 0; frame++) {
			struct { uintptr_t next, ret; } pair;
			mach_vm_size_t got = 0;

			if (mach_vm_read_overwrite(me, (mach_vm_address_t)fp, sizeof(pair),
			    (mach_vm_address_t)&pair, &got) != KERN_SUCCESS || got != sizeof(pair)) {
				break;
			}
			if (pair.ret == 0) {
				break;
			}
			image = sample_image_of(pair.ret, &offset);
			if (image != NULL) {
				say("hakios: Sample %s t%u   from %s+%#lx\n", who, i, image, (unsigned long)offset);
			}
			if (pair.next <= fp) {
				break;
			}
			fp = pair.next;
		}
	}
	for (i = 0; i < count; i++) {
		mach_port_deallocate(me, threads[i]);
	}
	vm_deallocate(me, (vm_address_t)threads, count * sizeof(threads[0]));
}

struct sample_job {
	char who[24];
	int at;
	int again;
};

static void *
sample_later(void *argument)
{
	struct sample_job *job = argument;

	sleep((unsigned)job->at);
	sample_threads(job->who, job->at);
	if (job->again > 0) {
		sleep((unsigned)job->again);
		sample_threads(job->who, job->at + job->again);
	}
	free(job);
	return NULL;
}

static void
sample_watchdog(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *p;
	struct sample_job *job;
	pthread_t th;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	if ((p = strstr(arguments, "hakios_sample=")) == NULL) {
		return;
	}
	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		return;
	}
	snprintf(job->who, sizeof(job->who), "%s[%d]", getprogname(), getpid());
	job->again = 0;
	{
		char *end = NULL;

		job->at = (int)strtol(p + 14, &end, 10);
		if (end != NULL && *end == ',') {
			job->again = (int)strtol(end + 1, NULL, 10);
		}
	}
	if (job->at <= 0 || job->at > 600 || job->again < 0 || job->again > 600) {
		free(job);
		return;
	}
	if (pthread_create(&th, NULL, sample_later, job) == 0) {
		pthread_detach(th);
	} else {
		free(job);
	}
}

/*
 * Boot-arg hakios_every=START,STEP,COUNT: the same sample, COUNT times, the first at START seconds
 * and one every STEP.  For a program that has to be seen *during* a wait, not before it: lsd is
 * wanted while the window server sits on the endpoint it was handed, which is 30 s per request in
 * p232, and one sample at 170 s has missed that every time.
 */
struct sample_every_job {
	char who[24];
	int start;
	int step;
	int count;
};

static void *
sample_every_later(void *argument)
{
	struct sample_every_job *job = argument;
	int i;

	sleep((unsigned)job->start);
	for (i = 0; i < job->count; i++) {
		sample_threads(job->who, job->start + i * job->step);
		if (i + 1 < job->count) {
			sleep((unsigned)job->step);
		}
	}
	free(job);
	return NULL;
}

static void
sample_every_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;
	char settings[64] = "";
	char name[160];
	const char *program = getprogname();
	const char *p = NULL;
	struct sample_every_job *job;
	char *end = NULL;
	pthread_t th;
	int values[3] = { 0, 0, 0 };
	int fd, i;
	int is_lsd = strcmp(program, "lsd") == 0;

	/*
	 * lsd was the first program worth sampling, hence the command line argument; iconservicesagent
	 * is the second (p260/p261: lsd asks it once and never hears back).  Both are started by the
	 * same switch file, per program, and the command line stays as small as it is now.
	 */
	if (!is_lsd && strcmp(program, "iconservicesagent") != 0 &&
	    strcmp(program, "containermanagerd") != 0) {
		return;
	}
	if (is_lsd && sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    (p = strstr(arguments, "hakios_every=")) != NULL) {
		p += 13;
	} else {
		/*
		 * The command line is nearly full (~600 of the ~640 characters this loader takes),
		 * and p233 lost this argument to the limit.  The build script writes the same three
		 * numbers into /redwood/etc/hakios.every instead.
		 */
		snprintf(name, sizeof(name), "/redwood/etc/hakios.every.%s", program);
		fd = open(name, O_RDONLY);
		if (fd < 0 && is_lsd) {
			fd = open("/redwood/etc/hakios.every", O_RDONLY);
		}
		if (fd < 0) {
			return;
		}
		{
			ssize_t got = read(fd, settings, sizeof(settings) - 1);

			close(fd);
			if (got <= 0) {
				return;
			}
			settings[got] = 0;
		}
		p = settings;
	}
	{
		char *at = (char *)p;

		for (i = 0; i < 3; i++) {
			values[i] = (int)strtol(at, &end, 10);
			if (end == at) {
				values[i] = 0;
				break;
			}
			at = end;
			while (*at == ',' || *at == ' ' || *at == '\t' || *at == '\n') {
				at++;
			}
			if (*at == 0) {
				break;
			}
		}
	}
	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		return;
	}
	snprintf(job->who, sizeof(job->who), "%s[%d]", getprogname(), getpid());
	job->start = values[0];
	job->step = values[1];
	job->count = values[2];
	say("hakios: sample every: \"%s\" -> start %d, step %d, count %d\n", p, job->start,
	    job->step, job->count);
	if (job->start <= 0 || job->start > 7200 || job->step <= 0 || job->step > 600 ||
	    job->count <= 0 || job->count > 40) {
		free(job);
		return;
	}
	if (pthread_create(&th, NULL, sample_every_later, job) == 0) {
		pthread_detach(th);
	} else {
		free(job);
	}
}

/* ---- lsd: is the store being written at all ------------------------------------------------ */

/*
 * Switch: /redwood/etc/hakios.files (or boot-arg lsd_files=1).  Every 30 s lsd prints the size of
 * the four files of its own store.  If they grow, the database build is going and only slow; if
 * they stay at "-" or one size, it is wedged -- p236 left lsd with three threads inside the same
 * CoreServices lock and not one byte of answer in 120 s, and this tells the two apart next run.
 */
static void *
lsd_files_later(void *argument)
{
	static const char *const names[] = {
		"com.apple.lsd.datastore",
		"com.apple.lsd.datastore-wal",
		"com.apple.lsd.datastore-shm",
		"com.apple.lsd.systemdatastore",
		"com.apple.lsd.userdatastore",
		"com.apple.lsd.dbRecovery",
	};
	static const char *const dir =
	    "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd";
	int round;
	unsigned i;

	(void)argument;
	{
		static const char *const dirs[] = {
			"/private/var/containers/Shared/SystemGroup",
			"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd",
			"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library",
			"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches",
			"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd",
			"/private/var/containers/Data/System/com.apple.lsd",
		};
		unsigned d;

		for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
			struct stat st;
			DIR *listing;
			struct dirent *entry;
			char entries[256] = "";
			unsigned seen = 0;

			listing = opendir(dirs[d]);
			while (listing != NULL && (entry = readdir(listing)) != NULL && seen < 8) {
				if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
					continue;
				}
				strlcat(entries, " ", sizeof(entries));
				strlcat(entries, entry->d_name, sizeof(entries));
				seen++;
			}
			if (listing != NULL) {
				closedir(listing);
			}
			say("hakios: store dir %s: %s, %u entries:%s\n", dirs[d],
			    stat(dirs[d], &st) == 0 ? (S_ISDIR(st.st_mode) ? "dir" : "file") : strerror(errno),
			    seen, entries);
		}
	}
	for (round = 0; round < 22; round++) {
		char line[512] = "";
		size_t at = 0;

		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			char path[512];
			char part[80];
			struct stat st;
			int present;
			int written;

			snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
			present = stat(path, &st) == 0;
			snprintf(part, sizeof(part), " %s=%lld",
			    strncmp(names[i], "com.apple.lsd.", 14) == 0 ? names[i] + 14 : names[i],
			    present ? (long long)st.st_size : -1LL);
			written = snprintf(line + at, sizeof(line) - at, "%s", part);
			if (written > 0 && (size_t)written < sizeof(line) - at) {
				at += (size_t)written;
			}
		}
		say("hakios: store files t=%us:%s\n", round * 30, line);
		sleep(30);
	}
	return NULL;
}

static void
lsd_files_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;
	pthread_t th;

	if (strcmp(getprogname(), "lsd") != 0) {
		return;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		arguments[0] = 0;
	}
	if (strstr(arguments, "lsd_files=1") == NULL &&
	    access("/redwood/etc/hakios.files", F_OK) != 0) {
		return;
	}
	if (pthread_create(&th, NULL, lsd_files_later, NULL) == 0) {
		pthread_detach(th);
	}
}

/* ---- xpc: what the container query talks to, and whether the answer comes -------------------- */

/*
 * Boot-arg xpc_loud=1: every XPC connection to a service whose name has "container" in it --
 * made, and every request sent on it, with the time the answer took (or that it never came).
 * The container query of lsd (libsystem_containermanager) is a silent XPC client: it does not
 * go through the NSXPCConnection methods, so only the C calls below see it.
 */
typedef void *hk_xpc_connection;
typedef void *hk_xpc_object;
extern const char *xpc_connection_get_name(hk_xpc_connection connection);
extern hk_xpc_connection xpc_connection_create_mach_service(const char *name,
    dispatch_queue_t targetq, uint64_t flags);
extern hk_xpc_connection xpc_connection_create(const char *name, dispatch_queue_t targetq);
extern void xpc_connection_send_message(hk_xpc_connection connection, hk_xpc_object message);
extern void xpc_connection_send_message_with_reply(hk_xpc_connection connection,
    hk_xpc_object message, dispatch_queue_t replyq, void (^handler)(hk_xpc_object));
extern hk_xpc_object xpc_connection_send_message_with_reply_sync(hk_xpc_connection connection,
    hk_xpc_object message);
extern hk_xpc_connection xpc_connection_create_from_endpoint(hk_xpc_object endpoint);
extern char *xpc_copy_description(hk_xpc_object object);

static int xpc_watch;
static int lsd_watching(void);

static unsigned xpc_lines;
#define XPC_LINES_MAX 800

/* every line says which program it is: the log is one stream for all of them */
static void
xpc_log(const char *format, ...)
{
	char line[512];
	va_list arguments;
	int at;

	if ((!xpc_watch && !lsd_watching()) || xpc_lines >= XPC_LINES_MAX) {
		return;
	}
	xpc_lines++;
	at = snprintf(line, sizeof(line), "hakios: xpc %s[%d]: ", getprogname(), getpid());
	if (at < 0 || (size_t)at >= sizeof(line)) {
		return;
	}
	va_start(arguments, format);
	vsnprintf(line + at, sizeof(line) - (size_t)at, format, arguments);
	va_end(arguments);
	say("%s", line);
}

static uint64_t
xpc_millis(void)
{
	static mach_timebase_info_data_t tb;
	uint64_t now = mach_absolute_time();

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return tb.denom != 0 ? now * tb.numer / tb.denom / 1000000 : now / 1000000;
}

/* ---- lsd: its endpoint, and the peer on the other side of it ---------------------------------- */

/*
 * Boot-arg lsd_watch=1: lsd says which mach service it listens on (that is the endpoint the window
 * server is handed), and every event the listener connection is given -- a peer at all, a message,
 * an error -- with the time the handler took.  The window server's side of the same exchange is in
 * the sb library (xpc_loud); p232 has SpringBoard asking and no answer coming, and this side says
 * whether lsd ever picked the request up, or only picked it up after its own sync calls.
 * The replies on lsd's own client connections are printed too, so a request to the container
 * manager shows its cost: lsd has been seen in that loop for whole runs.
 */
typedef const struct _xpc_type_s *hk_xpc_type;
extern hk_xpc_type xpc_get_type(hk_xpc_object object);
extern const struct _xpc_type_s _xpc_type_connection;
extern const struct _xpc_type_s _xpc_type_dictionary;
extern const struct _xpc_type_s _xpc_type_error;
extern void xpc_connection_set_event_handler(hk_xpc_connection connection,
    void (^handler)(hk_xpc_object));

static int lsd_watch;
static int no_barrier;
static int no_icons_db;
static int no_mcm_log;
static int icons_reply;
static int icons_replay;
static unsigned icons_replies;
static unsigned char icons_blob[512];
static size_t icons_blob_len;
static unsigned lsd_lines;
#define LSD_LINES_MAX 600
static hk_xpc_connection lsd_listener[4];
static unsigned lsd_listeners;

static int
lsd_watching(void)
{
	const char *program = getprogname();

	if (!lsd_watch || lsd_lines >= LSD_LINES_MAX) {
		return 0;
	}
	/*
	 * iconservicesagent is watched with the same hooks: lsd's one request to it is answered in
	 * 45 ms in RAM and never on APFS (p266 against p266ram), and the agent turns out to make
	 * two file calls in its whole life (p269) -- so whether it ever creates its listener and
	 * who it talks to is the thing to see, and those are the hooks below.
	 */
	return strcmp(program, "lsd") == 0 || strcmp(program, "iconservicesagent") == 0 ||
	    strcmp(program, "containermanagerd") == 0;
}

static void
lsd_log(const char *format, ...)
{
	char line[512];
	va_list arguments;
	int at;

	if (!lsd_watching()) {
		return;
	}
	lsd_lines++;
	at = snprintf(line, sizeof(line), "hakios: %s xpc t=%llus: ", getprogname(),
	    (unsigned long long)(xpc_millis() / 1000));
	if (at < 0 || (size_t)at >= sizeof(line)) {
		return;
	}
	va_start(arguments, format);
	vsnprintf(line + at, sizeof(line) - (size_t)at, format, arguments);
	va_end(arguments);
	say("%s", line);
}

static int
lsd_is_listener(hk_xpc_connection connection)
{
	unsigned i;

	for (i = 0; i < lsd_listeners; i++) {
		if (lsd_listener[i] == connection) {
			return 1;
		}
	}
	return 0;
}

static void
lsd_listener_made(const char *name, hk_xpc_connection connection)
{
	if (!lsd_watching()) {
		return;
	}
	if (lsd_listeners < sizeof(lsd_listener) / sizeof(lsd_listener[0])) {
		lsd_listener[lsd_listeners++] = connection;
	}
	lsd_log("listening on \"%s\" (%p)\n", name != NULL ? name : "-", connection);
}

static const char *
lsd_event_name(hk_xpc_object event)
{
	hk_xpc_type type = xpc_get_type(event);

	if (type == &_xpc_type_connection) {
		return "a peer";
	}
	if (type == &_xpc_type_dictionary) {
		return "a message";
	}
	if (type == &_xpc_type_error) {
		return "an error";
	}
	return "an event of another kind";
}

static void
hk_lsd_set_event_handler(hk_xpc_connection connection, void (^handler)(hk_xpc_object))
{	if (!lsd_watching()) {
		xpc_connection_set_event_handler(connection, handler);
		return;
	}
	lsd_log("%s %p: handler set%s\n", lsd_is_listener(connection) ? "listener" : "connection",
	    connection, handler == NULL ? " to nothing" : "");
	xpc_connection_set_event_handler(connection, ^(hk_xpc_object event) {
		uint64_t started;
		int listener = lsd_is_listener(connection);

		if (lsd_watching()) {
			char *what = xpc_copy_description(event);

			lsd_log("%s %p got %s: %s\n", listener ? "listener" : "connection", connection,
			    lsd_event_name(event), what != NULL ? what : "-");
			if (what != NULL) {
				free(what);
			}
		}
		started = xpc_millis();
		if (handler != NULL) {
			handler(event);
		}
		if (lsd_watching()) {
			lsd_log("%s %p: handler done in %llu ms\n",
			    listener ? "listener" : "connection", connection, xpc_millis() - started);
		}
	});
}

/*
 * NSXPCConnection, in lsd.  The C calls above see libsystem_containermanager and nothing else: lsd
 * also has NSXPC connections (keybagd, usermanagerd, iconservices -- p301 prints their resume),
 * and their requests go through libxpc's private path, so a pause of seconds between two file
 * calls is invisible.  The name is remembered on the connection itself, and a send that takes
 * more than a tenth of a second says which service and which selector it was.
 */
static const char *lsd_text(id object);
static void persona_hook_try(void);
static char nsxpc_key;

struct nsxpc_stat {
	char service[48];
	char selector[72];
	unsigned count;
	unsigned long long total;
};
static struct nsxpc_stat nsxpc_stats[24];
static unsigned nsxpc_stats_n;

static void
nsxpc_stat_note(const char *service, const char *selector, unsigned long long took)
{
	unsigned i;

	for (i = 0; i < nsxpc_stats_n; i++) {
		if (strcmp(nsxpc_stats[i].service, service) == 0 &&
		    strcmp(nsxpc_stats[i].selector, selector) == 0) {
			nsxpc_stats[i].count++;
			nsxpc_stats[i].total += took;
			return;
		}
	}
	if (nsxpc_stats_n >= sizeof(nsxpc_stats) / sizeof(nsxpc_stats[0])) {
		return;
	}
	snprintf(nsxpc_stats[nsxpc_stats_n].service, sizeof(nsxpc_stats[0].service), "%s", service);
	snprintf(nsxpc_stats[nsxpc_stats_n].selector, sizeof(nsxpc_stats[0].selector), "%s",
	    selector);
	nsxpc_stats[nsxpc_stats_n].count = 1;
	nsxpc_stats[nsxpc_stats_n].total = took;
	nsxpc_stats_n++;
}

/* The whole budget, not only the calls over a tenth of a second: which service, which selector,
 * how many times, how many milliseconds in all.  Said every minute next to the file counters. */
static void
nsxpc_stats_say(void)
{
	unsigned i, j;

	for (i = 0; i < nsxpc_stats_n; i++) {
		for (j = i + 1; j < nsxpc_stats_n; j++) {
			if (nsxpc_stats[j].total > nsxpc_stats[i].total) {
				struct nsxpc_stat swap = nsxpc_stats[i];

				nsxpc_stats[i] = nsxpc_stats[j];
				nsxpc_stats[j] = swap;
			}
		}
	}
	for (i = 0; i < nsxpc_stats_n && i < 8; i++) {
		say("hakios: %s nsxpc: %s %s %u calls %llums\n", getprogname(),
		    nsxpc_stats[i].service, nsxpc_stats[i].selector, nsxpc_stats[i].count,
		    nsxpc_stats[i].total);
	}
}
static IMP orig_nsxpc_init_service;
static IMP orig_nsxpc_init_mach;
static IMP orig_nsxpc_send;

static id
hk_nsxpc_init_service(id self, SEL cmd, id name)
{
	id result = ((id (*)(id, SEL, id))orig_nsxpc_init_service)(self, cmd, name);

	if (result != nil && name != nil) {
		objc_setAssociatedObject(result, &nsxpc_key, name, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	return result;
}

static id
hk_nsxpc_init_mach(id self, SEL cmd, id name, unsigned long options)
{
	id result = ((id (*)(id, SEL, id, unsigned long))orig_nsxpc_init_mach)(self, cmd, name, options);

	if (result != nil && name != nil) {
		objc_setAssociatedObject(result, &nsxpc_key, name, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	return result;
}

static void
hk_nsxpc_send(id self, SEL cmd, id invocation, id *arguments, unsigned long count, id signature,
    SEL selector, id proxy)
{
	unsigned long long started = xpc_millis();

	((void (*)(id, SEL, id, id *, unsigned long, id, SEL, id))orig_nsxpc_send)(self, cmd, invocation,
	    arguments, count, signature, selector, proxy);
	{
		unsigned long long took = xpc_millis() - started;
		id name = objc_getAssociatedObject(self, &nsxpc_key);

		nsxpc_stat_note(name != nil ? lsd_text(name) : "-",
		    selector != NULL ? sel_getName(selector) : "-", took);
		if (selector != NULL &&
		    strcmp(sel_getName(selector), "fetchPersonaListforPid:withCompletionHandler:") == 0) {
			persona_hook_try();
		}
		if (took >= 500) {
			say("hakios: nsxpc %s[%d]: %s %s took %llums\n", getprogname(), getpid(),
			    name != nil ? lsd_text(name) : "-",
			    selector != NULL ? sel_getName(selector) : "-", took);
		}
	}
}

/*
 * fetchPersonaListforPid:withCompletionHandler: -- one answer per run, then from the cache.
 * p302: every bundle costs lsd a persona query and usermanagerd answers it in 0.5-1.7 s, while
 * the same call in RAM takes 4-6 ms.  The list cannot change inside one boot (the device is
 * single-user), so the first answer is kept and handed back afterwards.  The class that has the
 * method is looked for the first time the call is seen -- by then CoreServices is loaded, and at
 * library load it might not be.  Arguments are passed through untouched: an integer pid and a
 * block pointer both travel in a register, so the forwarder does not need to know which is which.
 */
static int persona_state;               /* 0: not looked for, -1: looked and not found, 1: hooked, 2: cached */
static id persona_list;
static id persona_error;
static void (*orig_persona)(id, SEL, void *, id);

static id
hk_retain(id object)
{
	return object != nil ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("retain")) : nil;
}

static void
hk_persona(id self, SEL cmd, void *argument, id handler)
{
	if (persona_state == 2) {
		if (handler != nil) {
			((void (^)(id, id))handler)(persona_list, persona_error);
		}
		return;
	}
	orig_persona(self, cmd, argument, (id)^(id list, id error) {
		if (persona_state != 2) {
			persona_list = hk_retain(list);
			persona_error = hk_retain(error);
			persona_state = 2;
			say("hakios: persona: the answer is kept (%s, %s) -- the next ones come from it\n",
			    list != nil ? "a list" : "nothing", error != nil ? "an error" : "no error");
		}
		if (handler != nil) {
			((void (^)(id, id))handler)(list, error);
		}
	});
}

static void
persona_hook_try(void)
{
	SEL selector = sel_registerName("fetchPersonaListforPid:withCompletionHandler:");
	unsigned count = 0;
	Class *classes;
	unsigned i;

	if (persona_state != 0) {
		return;
	}
	persona_state = -1;
	classes = objc_copyClassList(&count);
	if (classes == NULL) {
		return;
	}
	for (i = 0; i < count; i++) {
		Method method = class_getInstanceMethod(classes[i], selector);

		if (method != NULL) {
			orig_persona = (void *)method_setImplementation(method, (IMP)hk_persona);
			persona_state = 1;
			say("hakios: persona: -[%s %s] is hooked, encoding %s\n", class_getName(classes[i]),
			    sel_getName(selector), method_getTypeEncoding(method));
			break;
		}
	}
	free(classes);
}

static void
lsd_nsxpc_watch(void)
{
	Class cls = objc_getClass("NSXPCConnection");
	Method method;

	if (cls == NULL) {
		return;
	}
	method = class_getInstanceMethod(cls, sel_registerName("initWithServiceName:"));
	if (method != NULL) {
		orig_nsxpc_init_service = method_setImplementation(method, (IMP)hk_nsxpc_init_service);
	}
	method = class_getInstanceMethod(cls, sel_registerName("initWithMachServiceName:options:"));
	if (method != NULL) {
		orig_nsxpc_init_mach = method_setImplementation(method, (IMP)hk_nsxpc_init_mach);
	}
	method = class_getInstanceMethod(cls, sel_registerName(
	    "_sendInvocation:orArguments:count:methodSignature:selector:withProxy:"));
	if (method != NULL) {
		orig_nsxpc_send = method_setImplementation(method, (IMP)hk_nsxpc_send);
	} else {
		say("hakios: nsxpc: the send method is not there\n");
	}
}

/*
 * The listener object is Foundation's, and it never comes by xpc_connection_set_event_handler in
 * this port: p233 had not one line of it, while every one of lsd's own xpc requests was printed.
 * So the listener is caught where lsd touches it -- the initialiser, -resume, -endpoint (that is
 * the object handed to SpringBoard and then left unanswered), the connection handler and the
 * delegate's "take this new connection?" -- and each is only replaced with a copy that says what
 * it was given and calls the original.
 */
static int lsd_hooks_in;
static void (*lsd_init)(id, SEL, id);
static void (*lsd_resume)(id, SEL);
static id (*lsd_endpoint)(id, SEL);
static void (*lsd_set_handler)(id, SEL, id);
static void (*lsd_set_delegate)(id, SEL, id);
static void (*lsd_connection_resume)(id, SEL);

static const char *
lsd_text(id object)
{
	id text;

	if (object == nil) {
		return "-";
	}
	text = ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description"));
	if (text == nil) {
		return "-";
	}
	return ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String"));
}

static id
hk_lsd_listener_init(id self, SEL cmd, id name)
{
	id result = ((id (*)(id, SEL, id))lsd_init)(self, cmd, name);

	lsd_log("listener %p made for \"%s\"\n", result, lsd_text(name));
	return result;
}

static void
hk_lsd_listener_resume(id self, SEL cmd)
{
	lsd_log("listener %p: resume\n", self);
	((void (*)(id, SEL))lsd_resume)(self, cmd);
	lsd_log("listener %p: listening\n", self);
}

static id
hk_lsd_listener_endpoint(id self, SEL cmd)
{
	id result = ((id (*)(id, SEL))lsd_endpoint)(self, cmd);

	lsd_log("listener %p: endpoint handed out: %s\n", self, lsd_text(result));
	return result;
}

static void
hk_lsd_listener_set_handler(id self, SEL cmd, void (^handler)(id))
{
	if (handler == NULL) {
		((void (*)(id, SEL, id))lsd_set_handler)(self, cmd, (id)NULL);
		return;
	}
	lsd_log("listener %p: a connection handler is set\n", self);
	((void (*)(id, SEL, id))lsd_set_handler)(self, cmd, (id)^(id connection) {
		uint64_t started = xpc_millis();

		lsd_log("listener %p: the handler is called for %s\n", self, lsd_text(connection));
		handler(connection);
		lsd_log("listener %p: the handler is done in %llu ms\n", self,
		    xpc_millis() - started);
	});
}

/*
 * p234: with the delegate's connection-accepting method replaced on its class, lsd took SIGTRAP in
 * libdispatch on the first new connection -- that method asserts the queue it is on, and an IMP put
 * on a class by hand is not called the way it expects.  So the acceptance is followed one step
 * further instead: every taken connection is resumed, and -[NSXPCConnection resume] is a plain
 * method that can be replaced safely.  It says the service and the process the connection is.
 */
static void
hk_lsd_connection_resume(id self, SEL cmd)
{
	long pid = ((long (*)(id, SEL))objc_msgSend)(self, sel_registerName("processIdentifier"));
	id name = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("serviceName"));

	lsd_log("connection %p: resume, service \"%s\", pid %ld\n", self, lsd_text(name), pid);
	((void (*)(id, SEL))lsd_connection_resume)(self, cmd);
	lsd_log("connection %p: resumed\n", self);
}


static void
hk_lsd_listener_set_delegate(id self, SEL cmd, id delegate)
{
	((void (*)(id, SEL, id))lsd_set_delegate)(self, cmd, delegate);
	lsd_log("listener %p: delegate %p of class %s\n", self, delegate,
	    delegate != nil ? class_getName(object_getClass(delegate)) : "-");
}

static void
lsd_listener_hooks(void)
{
	Class listener;
	Method method;

	if (lsd_hooks_in || !lsd_watch) {
		return;
	}
	if (strcmp(getprogname(), "lsd") != 0 && strcmp(getprogname(), "iconservicesagent") != 0) {
		return;
	}
	listener = objc_getClass("NSXPCListener");
	if (listener == Nil) {
		return;                         /* Foundation is not in the runtime yet */
	}
	lsd_hooks_in = 1;
	lsd_nsxpc_watch();
	if ((method = class_getInstanceMethod(listener, sel_registerName("initWithMachServiceName:"))) != NULL) {
		lsd_init = (void (*)(id, SEL, id))method_getImplementation(method);
		method_setImplementation(method, (IMP)hk_lsd_listener_init);
	}
	if ((method = class_getInstanceMethod(listener, sel_registerName("resume"))) != NULL) {
		lsd_resume = (void (*)(id, SEL))method_getImplementation(method);
		method_setImplementation(method, (IMP)hk_lsd_listener_resume);
	}
	if ((method = class_getInstanceMethod(listener, sel_registerName("endpoint"))) != NULL) {
		lsd_endpoint = (id (*)(id, SEL))method_getImplementation(method);
		method_setImplementation(method, (IMP)hk_lsd_listener_endpoint);
	}
	if ((method = class_getInstanceMethod(listener, sel_registerName("setConnectionHandler:"))) != NULL) {
		lsd_set_handler = (void (*)(id, SEL, id))method_getImplementation(method);
		method_setImplementation(method, (IMP)hk_lsd_listener_set_handler);
	}
	if ((method = class_getInstanceMethod(listener, sel_registerName("setDelegate:"))) != NULL) {
		lsd_set_delegate = (void (*)(id, SEL, id))method_getImplementation(method);
		method_setImplementation(method, (IMP)hk_lsd_listener_set_delegate);
	}
	if ((method = class_getInstanceMethod(objc_getClass("NSXPCConnection"), sel_registerName("resume"))) != NULL) {
		lsd_connection_resume = (void (*)(id, SEL))method_getImplementation(method);
		method_setImplementation(method, (IMP)hk_lsd_connection_resume);
	}
	say("hakios: lsd xpc: listener hooks: init %d, resume %d, endpoint %d, handler %d, delegate %d, connection %d\n",
	    lsd_init != NULL, lsd_resume != NULL, lsd_endpoint != NULL, lsd_set_handler != NULL,
	    lsd_set_delegate != NULL, lsd_connection_resume != NULL);
}

static hk_xpc_connection
hk_xpc_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags)
{
	hk_xpc_connection connection = xpc_connection_create_mach_service(name, targetq, flags);

	xpc_log("connection to %s (mach service) -> %p\n", name != NULL ? name : "-", connection);
	if ((flags & 1) != 0) {                         /* XPC_CONNECTION_MACH_SERVICE_LISTENER */
		lsd_listener_made(name, connection);
	}
	return connection;
}

static hk_xpc_connection
hk_xpc_create(const char *name, dispatch_queue_t targetq)
{
	hk_xpc_connection connection = xpc_connection_create(name, targetq);

	xpc_log("connection to %s -> %p\n", name != NULL ? name : "-", connection);
	return connection;
}

static hk_xpc_connection
hk_xpc_create_from_endpoint(hk_xpc_object endpoint)
{
	hk_xpc_connection connection = xpc_connection_create_from_endpoint(endpoint);

	xpc_log("connection from an endpoint %p -> %p\n", endpoint, connection);
	return connection;
}

static const char *
xpc_peer(hk_xpc_connection connection)
{
	const char *name = xpc_connection_get_name(connection);

	return name != NULL ? name : "(anonymous)";
}

static void
hk_xpc_send(hk_xpc_connection connection, hk_xpc_object message)
{
	xpc_log("message to %s, no answer wanted\n", xpc_peer(connection));
	xpc_connection_send_message(connection, message);
}

static void
hk_xpc_send_with_reply(hk_xpc_connection connection, hk_xpc_object message,
    dispatch_queue_t replyq, void (^handler)(hk_xpc_object))
{
	uint64_t started = xpc_millis();

	xpc_log("request to %s, the answer wanted\n", xpc_peer(connection));
	xpc_connection_send_message_with_reply(connection, message, replyq, ^(hk_xpc_object reply) {
		xpc_log("%s answered in %llu ms%s\n", xpc_peer(connection), xpc_millis() - started,
		    reply == NULL ? " (nothing)" : "");
		if (handler != NULL) {
			handler(reply);
		}
	});
}

static const char *lsd_caller(uintptr_t address);

/*
 * Switch: /redwood/etc/hakios.noiconsdb.  p273: the store build runs on the database queue and asks
 * com.apple.iconservices; the agent cannot answer because it is itself waiting on
 * com.apple.lsd.mapdb, which needs that queue.  Answering the agent's mapdb request at once with an
 * empty dictionary (no applications) lets it answer lsd, and the build can go on -- icons are not
 * what the store is made of.
 */
static hk_xpc_object
hk_xpc_send_with_reply_sync(hk_xpc_connection connection, hk_xpc_object message)
{
	uint64_t started = xpc_millis();
	hk_xpc_object reply;

	/*
	 * Switch: /redwood/etc/hakios.iconsreplay.  In RAM the icon request is answered with a
	 * 47-byte bplist under "root" (p279ram); on APFS the agent never answers, and the store
	 * build waits for it while holding the database queue.  The captured reply is put in
	 * /redwood/etc/icons.reply and handed back here, so the build can go on.
	 */
	if (icons_replay && strcmp(getprogname(), "lsd") == 0) {
		const char *peer = xpc_peer(connection);

		if (peer != NULL && strcmp(peer, "com.apple.iconservices") == 0) {
			extern hk_xpc_object xpc_dictionary_create(const void *keys,
			    const void *const *values, size_t count);
			extern void xpc_dictionary_set_data(hk_xpc_object dictionary, const char *key,
			    const void *bytes, size_t length);
			hk_xpc_object answer;

			if (icons_blob_len == 0) {
				int fd = open("/redwood/etc/icons.reply", O_RDONLY);

				if (fd >= 0) {
					ssize_t got = read(fd, icons_blob, sizeof(icons_blob));

					close(fd);
					if (got > 0) {
						icons_blob_len = (size_t)got;
					}
				}
				say("hakios: icons replay: %zu bytes of a reply\n", icons_blob_len);
			}
			if (icons_blob_len != 0) {
				answer = xpc_dictionary_create(NULL, NULL, 0);
				if (answer != NULL) {
					xpc_dictionary_set_data(answer, "root", icons_blob, icons_blob_len);
					xpc_log("request to %s: the captured reply is returned\n", peer);
					return answer;
				}
			}
		}
	}
	/*
	 * Switch: /redwood/etc/hakios.nomcmlog.  containermanagerd logs through a synchronous XPC to
	 * com.apple.MobileSystemServices.Logging, 130-300 ms a call and several calls per container
	 * query (p290); LaunchServices asks it once per application while building its store.  The
	 * answer is not used for anything the store needs, so an empty dictionary goes back at once.
	 */
	if (no_mcm_log && strcmp(getprogname(), "containermanagerd") == 0) {
		const char *peer = xpc_peer(connection);

		if (peer != NULL && strcmp(peer, "com.apple.MobileSystemServices.Logging") == 0) {
			extern hk_xpc_object xpc_dictionary_create(const void *keys,
			    const void *const *values, size_t count);
			hk_xpc_object empty = xpc_dictionary_create(NULL, NULL, 0);

			return empty;
		}
	}
	if (no_icons_db && strcmp(getprogname(), "iconservicesagent") == 0) {
		const char *peer = xpc_peer(connection);

		if (peer != NULL && strcmp(peer, "com.apple.lsd.mapdb") == 0) {
			extern hk_xpc_object xpc_dictionary_create(const void *keys,
			    const void *const *values, size_t count);
			hk_xpc_object empty = xpc_dictionary_create(NULL, NULL, 0);

			xpc_log("request to %s: an empty dictionary is returned at once\n", peer);
			return empty;
		}
	}
	xpc_log("request to %s, waiting for the answer, from %s\n", xpc_peer(connection),
	    lsd_caller((uintptr_t)__builtin_return_address(0)));
	reply = xpc_connection_send_message_with_reply_sync(connection, message);
	xpc_log("%s answered in %llu ms%s\n", xpc_peer(connection), xpc_millis() - started,
	    reply == NULL ? " (nothing)" : "");
	/*
	 * Switch: /redwood/etc/hakios.iconsreply.  In RAM the icon request is answered in 45 ms and
	 * on APFS it never is (p266 against p266ram); the reply's own description is what a stand-in
	 * answer would have to look like, so it is written down here -- in RAM, where it exists.
	 */
	if (icons_reply && reply != NULL && strcmp(getprogname(), "lsd") == 0) {
		const char *peer = xpc_peer(connection);

		if (peer != NULL && strcmp(peer, "com.apple.iconservices") == 0 && icons_replies < 4) {
			extern char *xpc_copy_description(hk_xpc_object object);
			char *text = xpc_copy_description(reply);

			icons_replies++;
			if (text != NULL) {
				say("hakios: icons reply from %s: %.1500s\n", peer, text);
				free(text);
			}
			{
				extern hk_xpc_object xpc_dictionary_get_value(hk_xpc_object dictionary,
				    const char *key);
				extern const void *xpc_data_get_bytes_ptr(hk_xpc_object object);
				extern size_t xpc_data_get_length(hk_xpc_object object);
				hk_xpc_object root = xpc_dictionary_get_value(reply, "root");
				const unsigned char *bytes = root != NULL ? xpc_data_get_bytes_ptr(root) : NULL;
				size_t length = root != NULL ? xpc_data_get_length(root) : 0;
				size_t i;

				if (bytes != NULL && length != 0 && length <= 200) {
					say("hakios: icons reply root, %zu bytes:", length);
					for (i = 0; i < length; i++) {
						say("%02x", bytes[i]);
					}
					say("\n");
				}
			}
		}
	}
	return reply;
}

DYLD_INTERPOSE(hk_xpc_create_mach_service, xpc_connection_create_mach_service)
DYLD_INTERPOSE(hk_xpc_create, xpc_connection_create)
DYLD_INTERPOSE(hk_xpc_create_from_endpoint, xpc_connection_create_from_endpoint)
DYLD_INTERPOSE(hk_xpc_send, xpc_connection_send_message)
DYLD_INTERPOSE(hk_xpc_send_with_reply, xpc_connection_send_message_with_reply)
DYLD_INTERPOSE(hk_xpc_send_with_reply_sync, xpc_connection_send_message_with_reply_sync)
DYLD_INTERPOSE(hk_lsd_set_event_handler, xpc_connection_set_event_handler)

/* ---- lsd: the dispatch queue it waits on --------------------------------------------------- */

/*
 * Switch: /redwood/etc/hakios.queues.  p242 leaves lsd with two threads inside one CoreServices lock
 * (one waits in libdispatch; the caller frame is CoreServices+0x136bfc).  A wait that never ends is a
 * dispatch_sync whose block never runs, so every sync is logged with the queue's label, the caller
 * and the time it took: the line without a "done" after it names the queue nobody drains.
 */
static const char *
lsd_caller(uintptr_t address)
{
	static char text[96];
	uintptr_t offset = 0;
	const char *image = sample_image_of(address, &offset);

	if (image == NULL) {
		return "?";
	}
	snprintf(text, sizeof(text), "%s+%#lx", image, (unsigned long)offset);
	return text;
}

static const char *
lsd_queue_name(dispatch_queue_t queue)
{
	const char *label = dispatch_queue_get_label(queue);

	return label != NULL && label[0] != 0 ? label : "(no label)";
}

static void
hk_lsd_dispatch_sync(dispatch_queue_t queue, dispatch_block_t block)
{
	int watch = lsd_watching();
	uint64_t started = 0;

	if (watch) {
		started = xpc_millis();
		lsd_log("sync \"%s\" (%p) from %s\n", lsd_queue_name(queue), queue,
		    lsd_caller((uintptr_t)__builtin_return_address(0)));
	}
	dispatch_sync(queue, block);
	if (watch) {
		lsd_log("sync \"%s\" (%p) done in %llu ms\n", lsd_queue_name(queue), queue,
		    xpc_millis() - started);
	}
}

DYLD_INTERPOSE(hk_lsd_dispatch_sync, dispatch_sync)

static void
hk_lsd_dispatch_sync_f(dispatch_queue_t queue, void *context, dispatch_function_t function)
{
	int watch = lsd_watching();
	uint64_t started = 0;

	if (watch) {
		started = xpc_millis();
		lsd_log("sync_f \"%s\" (%p) from %s\n", lsd_queue_name(queue), queue,
		    lsd_caller((uintptr_t)__builtin_return_address(0)));
	}
	dispatch_sync_f(queue, context, function);
	if (watch) {
		lsd_log("sync_f \"%s\" (%p) done in %llu ms\n", lsd_queue_name(queue), queue,
		    xpc_millis() - started);
	}
}

static void
hk_lsd_barrier_sync(dispatch_queue_t queue, dispatch_block_t block)
{
	int watch = lsd_watching();
	uint64_t started = 0;
	const char *label = queue != NULL ? dispatch_queue_get_label(queue) : NULL;

	/*
	 * Switch: /redwood/etc/hakios.nobarrier.  p273: the store build runs inside
	 * dispatch_barrier_sync("com.apple.lsd.database") and, inside that block, asks
	 * com.apple.iconservices for something; the agent cannot answer because it is itself waiting
	 * on com.apple.lsd.mapdb, which needs this very queue.  Running the block without taking the
	 * queue breaks the circle: the block is the only user of the database at that moment anyway,
	 * and a dispatch_sync to the queue from inside it still works (the queue is free).
	 */
	if (no_barrier && label != NULL && strcmp(label, "com.apple.lsd.database") == 0) {
		lsd_log("barrier_sync \"%s\": the block is run without the barrier\n", label);
		block();
		return;
	}
	if (watch) {
		started = xpc_millis();
		lsd_log("barrier_sync \"%s\" (%p) from %s\n", lsd_queue_name(queue), queue,
		    lsd_caller((uintptr_t)__builtin_return_address(0)));
	}
	dispatch_barrier_sync(queue, block);
	if (watch) {
		lsd_log("barrier_sync \"%s\" (%p) done in %llu ms\n", lsd_queue_name(queue), queue,
		    xpc_millis() - started);
	}
}

DYLD_INTERPOSE(hk_lsd_dispatch_sync_f, dispatch_sync_f)
DYLD_INTERPOSE(hk_lsd_barrier_sync, dispatch_barrier_sync)

/* ---- lsd: the install-progress service that holds the database ------------------------------ */

/*
 * p245 resolved the three offsets from the samples, and they are one thing:
 *   +0x136654  +[_LSInstallProgressService beginListening] + 0x50
 *   +0x136bfc  -[_LSInstallProgressService restoreInactiveInstalls] + 0x78
 *   +0x13cea8  -[_LSInstallProgressService performJournalRecovery] + 0x58
 * The main thread is inside performJournalRecovery (its nested run loop in kevent), and every other
 * thread that wants the database waits on the lock it holds.  On a real phone that recovery reads
 * the installation journal of MobileInstallation; here installd has never run (p242: not one mention
 * of it), so the journal is never given and the store build never starts -- which is why the store
 * directory stays empty and no DB file is ever made, no matter how long the window server waits.
 * The service belongs to install *progress* (the "Installing..." UI), not to the application list,
 * so with /redwood/etc/hakios.noinstall its three entry points return at once.
 */
static int lsd_noinstall;
static unsigned lsd_noinstall_said;

static void
lsd_install_progress_skip(id self, SEL cmd)
{
	if (lsd_noinstall_said < 6) {
		lsd_noinstall_said++;
		say("hakios: lsd install: %c[%s %s] skipped\n",
		    class_isMetaClass(object_getClass(self)) ? '+' : '-',
		    class_getName(object_getClass(self)), sel_getName(cmd));
	}
}

static void
lsd_install_progress_fixes(void)
{
	Class service;
	Method method;
	int all;

	if (strcmp(getprogname(), "lsd") != 0) {
		return;
	}
	all = access("/redwood/etc/hakios.noinstall", F_OK) == 0;
	if (!all && access("/redwood/etc/hakios.nojournal", F_OK) != 0) {
		return;
	}
	lsd_noinstall = 1;
	service = objc_getClass("_LSInstallProgressService");
	if (service == Nil) {
		say("hakios: lsd install: no _LSInstallProgressService in the runtime\n");
		return;
	}
	if (all) {
		method = class_getClassMethod(service, sel_registerName("beginListening"));
		if (method != NULL) {
			method_setImplementation(method, (IMP)lsd_install_progress_skip);
		}
	}
	method = class_getInstanceMethod(service, sel_registerName("performJournalRecovery"));
	if (method != NULL) {
		method_setImplementation(method, (IMP)lsd_install_progress_skip);
	}
	method = class_getInstanceMethod(service, sel_registerName("restoreInactiveInstalls"));
	if (method != NULL) {
		method_setImplementation(method, (IMP)lsd_install_progress_skip);
	}
	say("hakios: lsd install: %s skipped\n",
	    all ? "beginListening, journal recovery, restore" : "journal recovery, restore");
}

/* ---- lsd: the files it touches while the store does not build ------------------------------- */

/*
 * Switch: /redwood/etc/hakios.fslog.  Every file call lsd makes under the install and store paths
 * is counted, and the interesting ones are written down: a failure always, and everything in the
 * eighty calls that follow the MobileGestalt read, which is the last thing APFS does and the point
 * RAM goes on from (p255 against p257).  A plain "log every call" run (p264) wrote 1800 lines in
 * ninety seconds -- one file call per application -- and landed in the part of the loader log that
 * is dropped, so the printing is kept small and done on a timer instead.
 *
 * Switch: /redwood/etc/hakios.emptystore.  An empty file is put where the store should be, right
 * before lsd opens it for reading, to see whether "there is no file" is what stops the build.
 */
static int fs_log_on;
static int empty_store;
static unsigned fs_lines;
static unsigned dirent_lines;
#define FS_LINES_MAX 4000
#define DIRENT_LINES_MAX 400
#define FS_RING 300
#define FS_RING_LINE 220

enum {
	FS_OPEN, FS_STAT, FS_LSTAT, FS_ACCESS, FS_READLINK, FS_GETATTRLIST,
	FS_READDIR, FS_MKDIR, FS_FTRUNCATE, FS_STATFS, FS_READ, FS_KINDS
};
static const char *const fs_kind_name[FS_KINDS] = {
	"open", "stat", "lstat", "access", "readlink", "getattrlist",
	"readdir", "mkdir", "trunc", "statfs", "read",
};
static unsigned fs_calls[FS_KINDS];
static unsigned fs_fails[FS_KINDS];
static int fs_detail_left;
static int fs_all_after = -1;   /* seconds: from then on every call is written down */
static int fs_live_left;        /* calls still to be printed as they happen */
static int fs_live_done;

static unsigned long long fs_last_ms;
static unsigned long long fs_gap_ms;
static char fs_ring[FS_RING][FS_RING_LINE];
static unsigned fs_ring_next;
static unsigned fs_ring_total;
static unsigned fs_ring_printed;

static void
fs_keep(const char *line)
{
	if (!fs_log_on) {
		return;
	}
	snprintf(fs_ring[fs_ring_next], FS_RING_LINE, "%s", line);
	fs_ring_next = (fs_ring_next + 1) % FS_RING;
	fs_ring_total++;
}

/* One call: always counted, written down when it failed or while the detailed window is open.
 * Applications are the exception for failures: LaunchServices probes every bundle for macOS style
 * paths (Contents/PkgInfo, _CodeSignature/CodeRequirements-1) and most of those are not there on
 * iOS at all, so those lines would fill the ring and push the store's own sequence out of it -- as
 * p266 did, where the datastore open never made it into a dump. */
static void
fs_note(int kind, int failed, const char *format, ...)
{
	char line[FS_RING_LINE];
	va_list arguments;
	int at;

	if (!fs_log_on) {
		return;
	}
	fs_calls[kind]++;
	if (failed) {
		fs_fails[kind]++;
	}
	{
		unsigned long long now = xpc_millis();

		fs_gap_ms = fs_last_ms != 0 ? now - fs_last_ms : 0;
		fs_last_ms = now;
	}
	if (!failed && fs_detail_left <= 0) {
		if (fs_all_after < 0 || (long long)(xpc_millis() / 1000) < fs_all_after) {
			return;
		}
	}
	if (failed && fs_detail_left <= 0) {
		char probe[FS_RING_LINE];

		va_start(arguments, format);
		vsnprintf(probe, sizeof(probe), format, arguments);
		va_end(arguments);
		if (strstr(probe, "/Applications/") != NULL) {
			return;
		}
		at = snprintf(line, sizeof(line), "t=%llus +%llums: ",
		    (unsigned long long)(xpc_millis() / 1000), fs_gap_ms);
		if (at < 0 || (size_t)at >= sizeof(line)) {
			return;
		}
		snprintf(line + at, sizeof(line) - (size_t)at, "%s", probe);
		fs_keep(line);
		return;
	}
	if (fs_detail_left > 0) {
		fs_detail_left--;
	}
	at = snprintf(line, sizeof(line), "t=%llus +%llums: ",
	    (unsigned long long)(xpc_millis() / 1000), fs_gap_ms);
	if (at < 0 || (size_t)at >= sizeof(line)) {
		return;
	}
	va_start(arguments, format);
	vsnprintf(line + at, sizeof(line) - (size_t)at, format, arguments);
	va_end(arguments);
	fs_keep(line);
	/*
	 * A live window, opened by the first call on a bundle inside a PlugIns directory: the walk
	 * over the extensions is where the seconds go, and the ring alone shows only the last sixty
	 * calls of each minute -- a burst, not the sequence.  Six hundred calls printed as they
	 * happen are the whole passage, and they still fit in the tail of the log.  The trigger is a
	 * call, not a time: this kernel's clock runs faster than the wall clock (p299: 21009 s for
	 * ninety minutes), so a time-based window lands anywhere.
	 */
	if (!fs_live_done && strstr(line, ".appex") != NULL) {
		fs_live_left = 600;
		fs_live_done = 1;
		say("hakios: %s live window opens at %s\n", getprogname(), line);
	}
	if (fs_live_left > 0) {
		fs_live_left--;
		say("hakios: %s live %s\n", getprogname(), line);
	}
}

/* Said out loud on a timer, so the lines land in the part of the log the loader keeps. */
static void *
fs_dump_later(void *argument)
{
	int period = (int)(intptr_t)argument;

	sleep((unsigned)period);
	for (;;) {
		unsigned total = fs_ring_total;
		unsigned since = total < 60 ? total : 60;
		unsigned k;

		/*
		 * The last sixty calls, every minute, and not "everything since the last dump": the
		 * loader keeps only the first 256 KB and the last 1 MB of the log, and a walk over
		 * five hundred bundles at seventy lines a minute pushes the interesting part out of
		 * the tail.  Sixty lines a minute always leaves the recent window in the log.
		 */
		say("hakios: %s fs: t=%llus fail/all", getprogname(),
		    (unsigned long long)(xpc_millis() / 1000));
		for (k = 0; k < FS_KINDS; k++) {
			if (fs_calls[k] != 0) {
				say(" %s %u/%u", fs_kind_name[k], fs_fails[k], fs_calls[k]);
			}
		}
		say(", %u detail lines\n", since);
		nsxpc_stats_say();
		for (k = 0; k < since; k++) {
			unsigned index = (fs_ring_next + FS_RING - since + k) % FS_RING;

			say("hakios: %s fs #%u %s\n", getprogname(), total - since + k, fs_ring[index]);
		}
		fs_ring_printed = total;
		sleep(60);
	}
	return NULL;
}

static int
lsd_fs_wanted(const char *path)
{
	if (path == NULL) {
		return 0;
	}
	if (strstr(path, "MobileInstallation") != NULL) {
		return 1;
	}
	if (strstr(path, "com.apple.lsd") != NULL) {
		return 1;
	}
	if (strstr(path, "LaunchServices") != NULL) {
		return 1;
	}
	if (strstr(path, "staged_system_apps") != NULL) {
		return 1;
	}
	if (strstr(path, "/private/var/containers") != NULL) {
		return 1;
	}
	if (strstr(path, "/Applications") != NULL) {
		return 1;
	}
	/* the photo library is made by assetsd, one SQLite database at a time */
	if (strstr(path, "PhotoData") != NULL || strstr(path, "Photos.sqlite") != NULL) {
		return 1;
	}
	/* the widget engine and the hosts it starts: what they read and write */
	if (strstr(path, "idget") != NULL || strstr(path, "hrono") != NULL) {
		return 1;
	}
	/* Spotlight's index, which searchd makes and removes */
	if (strstr(path, "Spotlight") != NULL || strstr(path, "spotlight") != NULL) {
		return 1;
	}
	/* PosterBoard: the poster, its snapshots and wherever it keeps them ("oster" covers both
	 * the framework's spelling and the file names' one) */
	if (strstr(path, "oster") != NULL || strstr(path, "allpaper") != NULL ||
	    strstr(path, "napshot") != NULL) {
		return 1;
	}
	return 0;
}

static int
fs_all_now(void)
{
	return fs_all_after >= 0 && (long long)(xpc_millis() / 1000) >= fs_all_after;
}

/* The install paths always; every path at all once the detailed window is open.  p298 showed the
 * seconds per bundle are spent between two instant getattrlist calls, and the calls in between are
 * on /System/Library/... -- which lsd_fs_wanted() drops, so they were never written down. */
static int
fs_wanted(const char *path)
{
	return lsd_fs_wanted(path) || fs_all_now();
}

static int
hk_lsd_open(const char *path, int flags, int mode)
{
	int result;

	if (empty_store && path != NULL && flags == 0 &&
	    strstr(path, "com.apple.lsd.datastore") != NULL &&
	    strstr(path, "-wal") == NULL && strstr(path, "-shm") == NULL) {
		int made = (int)syscall(463, -2, path, O_RDWR | O_CREAT, 0644);

		if (made >= 0) {
			close(made);
			empty_store = 0;
			fs_note(FS_OPEN, 0, "an empty file is put at %s", path);
		}
	}
	result = (int)syscall(463, -2, path, flags, mode);
	if (fs_wanted(path)) {
		fs_note(FS_OPEN, result < 0, "open %s (flags %#x) -> %d", path, flags, result);
		if (strstr(path, "MobileGestalt.plist") != NULL) {
			fs_detail_left = 80;
			fs_note(FS_OPEN, 0, "--- the store probe: the next calls are written down ---");
		}
	}
	return result;
}

static int
hk_lsd_getattrlist(const char *path, void *list, void *buffer, size_t size, unsigned long options)
{
	unsigned long long started = xpc_millis();
	int result = (int)syscall(220, path, list, buffer, size, options);
	unsigned long long took = xpc_millis() - started;

	/*
	 * The options and the time of the call itself: a pair of calls on the same path, one of
	 * them fast and the other seconds long, is either two different attribute lists (the
	 * cryptex probe and the full one) or our ramfs taking a slow path for one of them.
	 */
	if (fs_wanted(path)) {
		fs_note(FS_GETATTRLIST, result < 0, "getattrlist %s (opts %#lx) -> %d in %llums", path,
		    options, result, took);
	}
	return result;
}

static int
hk_lsd_stat(const char *path, struct stat *information)
{
	int result = (int)syscall(338, path, information);

	if (fs_wanted(path)) {
		fs_note(FS_STAT, result < 0, "stat %s -> %d", path, result);
	}
	return result;
}

static int
hk_lsd_lstat(const char *path, struct stat *information)
{
	int result = (int)syscall(340, path, information);

	if (fs_wanted(path)) {
		fs_note(FS_LSTAT, result < 0, "lstat %s -> %d", path, result);
	}
	return result;
}

static int
hk_lsd_access(const char *path, int mode)
{
	int result = (int)syscall(33, path, mode);

	if (fs_wanted(path)) {
		fs_note(FS_ACCESS, result < 0, "access %s mode %d -> %d", path, mode, result);
	}
	return result;
}

static ssize_t
hk_lsd_readlink(const char *path, char *buffer, size_t size)
{
	ssize_t result = (ssize_t)syscall(58, path, buffer, size);

	if (fs_wanted(path)) {
		fs_note(FS_READLINK, result < 0, "readlink %s -> %ld", path, (long)result);
	}
	return result;
}

/* Directory listing.  getdirentries64 has no path, only the descriptor, so the "open ... -> fd N"
 * line is what it is matched against.  It is declared here by hand: the SDK ships getdirentries(2)
 * but not this one, and the sampler found the symbol in libsystem_kernel (p263: __getdirentries64
 * at +0x2534), which is what a directory read in libsystem_c goes through. */
extern ssize_t __getdirentries64(int fd, void *buffer, size_t size, off_t *base);

static ssize_t
hk_lsd_getdirentries64(int fd, void *buffer, size_t size, off_t *base)
{
	ssize_t result = (ssize_t)syscall(344, fd, buffer, size, base);

	if (fs_log_on && dirent_lines < DIRENT_LINES_MAX) {
		dirent_lines++;
		fs_note(FS_READDIR, result < 0, "getdirentries64 fd %d -> %ld", fd, (long)result);
	}
	return result;
}

static int
hk_lsd_mkdir(const char *path, int mode)
{
	int result = (int)syscall(136, path, mode);

	if (path != NULL) {
		fs_note(FS_MKDIR, result < 0, "mkdir %s -> %d", path, result);
	}
	return result;
}

static int
hk_lsd_ftruncate(int fd, off_t length)
{
	int result = (int)syscall(201, fd, length);

	fs_note(FS_FTRUNCATE, result < 0, "ftruncate fd %d to %lld -> %d", fd, (long long)length,
	    result);
	return result;
}

/*
 * Reads, failures only.  iconservicesagent opens /System/Library/FeatureFlags/Domain/
 * LaunchServices.plist -- a decmpfs file with its data in the attribute -- and then goes quiet;
 * if the read of it comes back broken, the feature flags are missing and the agent has nothing
 * to work with.  A failed read is rare enough to be written down every time.
 */
static ssize_t
hk_lsd_read(int fd, void *buffer, size_t size)
{
	ssize_t result = (ssize_t)syscall(3, fd, buffer, size);

	if (fs_log_on && result < 0) {
		fs_note(FS_READ, 1, "read fd %d of %zu -> %ld: %s", fd, size, (long)result,
		    strerror(errno));
	}
	return result;
}

static ssize_t
hk_lsd_pread(int fd, void *buffer, size_t size, off_t offset)
{
	ssize_t result = (ssize_t)syscall(153, fd, buffer, size, offset);

	if (fs_log_on && result < 0) {
		fs_note(FS_READ, 1, "pread fd %d of %zu at %lld -> %ld: %s", fd, size,
		    (long long)offset, (long)result, strerror(errno));
	}
	return result;
}

static int
hk_lsd_statfs(const char *path, struct statfs *buffer)
{
	int result = (int)syscall(157, path, buffer);

	if (result == 0 && buffer != NULL) {
		fs_note(FS_STATFS, 0,
		    "statfs %s: bsize %u blocks %llu bfree %llu bavail %llu ffree %llu flags %#x",
		    path != NULL ? path : "-", (unsigned)buffer->f_bsize,
		    (unsigned long long)buffer->f_blocks, (unsigned long long)buffer->f_bfree,
		    (unsigned long long)buffer->f_bavail, (unsigned long long)buffer->f_ffree,
		    (unsigned)buffer->f_flags);
	} else {
		fs_note(FS_STATFS, 1, "statfs %s: error %d", path != NULL ? path : "-", result);
	}
	return result;
}

DYLD_INTERPOSE(hk_lsd_open, open)
DYLD_INTERPOSE(hk_lsd_getattrlist, getattrlist)
DYLD_INTERPOSE(hk_lsd_stat, stat)
DYLD_INTERPOSE(hk_lsd_lstat, lstat)
DYLD_INTERPOSE(hk_lsd_access, access)
DYLD_INTERPOSE(hk_lsd_readlink, readlink)
DYLD_INTERPOSE(hk_lsd_getdirentries64, __getdirentries64)
DYLD_INTERPOSE(hk_lsd_mkdir, mkdir)
DYLD_INTERPOSE(hk_lsd_ftruncate, ftruncate)
DYLD_INTERPOSE(hk_lsd_read, read)
DYLD_INTERPOSE(hk_lsd_pread, pread)
DYLD_INTERPOSE(hk_lsd_statfs, statfs)


static void
lsd_fslog_setup(void)
{
	const char *program = getprogname();
	int is_lsd = strcmp(program, "lsd") == 0;
	/*
	 * assetsd makes the photo library: Photos dies on an empty data source and PhotoData holds
	 * only Thumbnails, so what assetsd does with the library's files is the thing left to see.
	 * Its own messages go to logd and never reach the console, but its file calls can be counted
	 * here, under the same switch as lsd's.
	 */
	int is_assetsd = strcmp(program, "assetsd") == 0;

	/*
	 * PosterBoard draws the wallpaper and keeps its poster and the snapshots of it on the
	 * volume.  The wallpaper is black for the first seconds of every boot (p341: PosterBoard's
	 * process appears only when the home screen does), so what it opens, makes and fails to
	 * make is written down under its own switch; that costs nothing in other runs.
	 */
	int is_poster = strcmp(program, "PosterBoard") == 0;

	/*
	 * searchd makes the Spotlight index, and p341 has it crash four times a boot, always right
	 * after it removes index.spotlightV2, with a null read at 0xb00 inside SICreateNewIndex
	 * (the frames are MobileSpotlightIndex; llvm-nm names the exported calls).  What it asks
	 * the filesystem for before that is the thing to see, so it gets the same hooks.
	 */
	int is_searchd = strcmp(program, "searchd") == 0;

	/*
	 * chronod is ChronoCore, the widget engine: it decides which extension host draws a widget
	 * and hands the host's layer to SpringBoard.  A home screen widget that stays a black
	 * rectangle (p344) is either a host that never comes up or one that comes up and says
	 * nothing, and both are decided here, so its file calls are written down too.
	 */
	int is_chronod = strcmp(program, "chronod") == 0;

	/*
	 * iconservicesagent is watched with the same hooks and its own switch: lsd's one request to
	 * it is answered in 45 ms in RAM and never on APFS (p266 against p266ram), and the agent's
	 * own threads look idle in both, so what it does with files is the thing left to see.
	 */
	if (!is_lsd && !is_assetsd && !is_poster && !is_searchd && !is_chronod &&
	    strcmp(program, "iconservicesagent") != 0) {
		return;
	}
	if (is_lsd && access("/redwood/etc/hakios.fslog", F_OK) != 0) {
		return;
	}
	if (is_assetsd && access("/redwood/etc/hakios.fslog.assets", F_OK) != 0) {
		return;
	}
	if (is_poster && access("/redwood/etc/hakios.fslog.poster", F_OK) != 0) {
		return;
	}
	if (is_searchd && access("/redwood/etc/hakios.fslog.searchd", F_OK) != 0) {
		return;
	}
	if (is_chronod && access("/redwood/etc/hakios.fslog.chronod", F_OK) != 0) {
		return;
	}
	if (!is_lsd && !is_assetsd && !is_poster && !is_searchd && !is_chronod &&
	    access("/redwood/etc/hakios.fslog.icons", F_OK) != 0) {
		return;
	}
	fs_log_on = 1;
	{
		int fd = (int)syscall(5, "/redwood/etc/hakios.fslog.all", 0);
		char text[32];
		long got;

		if (fd >= 0) {
			got = (long)syscall(3, fd, text, sizeof(text) - 1);
			syscall(6, fd);
			if (got > 0) {
				text[got] = 0;
				fs_all_after = atoi(text);
			} else {
				fs_all_after = 2400;
			}
		}
	}
	empty_store = access("/redwood/etc/hakios.emptystore", F_OK) == 0;
	if (!is_lsd) {
		fs_detail_left = 300;           /* a small process: its whole start can be written down */
	}
	{
		pthread_t th;

		if (pthread_create(&th, NULL, fs_dump_later, (void *)(intptr_t)45) == 0) {
			pthread_detach(th);
		}
	}
	say("hakios: %s fs: file calls under the install paths are counted%s\n", program,
	    empty_store ? ", an empty store file will be put in place" : "");
}

/* ---- lsd: the state left on the data volume ------------------------------------------------ */

/*
 * Switch: /redwood/etc/hakios.cleanvar.  In RAM boot the whole /private/var is made anew from the
 * skeleton every run; on the APFS data volume it persists, and p252/p242 show stock lsd hanging in
 * _LSInstallProgressService's journal recovery there while the same stock lsd builds its store in
 * RAM in a minute.  The likeliest difference is what earlier interrupted runs left behind: a half
 * written installation journal (or a store that never reached its commit).  This removes those
 * places before lsd touches them, so the next build starts as clean as a RAM one.
 */
static void
remove_tree(const char *path)
{
	DIR *dir = opendir(path);
	struct dirent *entry;

	if (dir == NULL) {
		unlink(path);
		return;
	}
	while ((entry = readdir(dir)) != NULL) {
		char child[512];

		if (entry->d_name[0] == '.') {
			continue;
		}
		snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		remove_tree(child);
	}
	closedir(dir);
	rmdir(path);
}

/*
 * Switch: /redwood/etc/hakios.rm.  A list of paths, one a line, that lsd removes at its start --
 * before anything else looks at them.  Needed when a single file on the volume is what a program
 * dies of: Notes keeps its database with a -wal and a -shm beside it, and a transaction left half
 * written by an earlier boot is the first thing to take out of the way.
 */
static void
lsd_remove_list(void)
{
	char list[2048];
	int fd;
	ssize_t got;

	if (strcmp(getprogname(), "lsd") != 0) {
		return;
	}
	fd = open("/redwood/etc/hakios.rm", O_RDONLY);
	if (fd < 0) {
		return;
	}
	got = read(fd, list, sizeof(list) - 1);
	close(fd);
	if (got <= 0) {
		return;
	}
	list[got] = 0;
	for (char *line = list; *line != 0; ) {
		char *end = strchr(line, '\n');
		char *path = line;

		if (end != NULL) {
			*end = 0;
		}
		while (*path == ' ') {
			path++;
		}
		if (*path == '/') {
			remove_tree(path);
			say("hakios: rm: %s %s\n", path,
			    access(path, F_OK) == 0 ? "is still there" : "gone");
		}
		if (end == NULL) {
			break;
		}
		line = end + 1;
	}
}

static void
lsd_clean_setup(void)
{
	static const char *const paths[] = {
		"/private/var/mobile/Library/MobileInstallation",
		"/private/var/installd",
		"/private/var/mobile/Library/Caches/com.apple.mobile.installation",
		"/private/var/mobile/Library/Caches/com.apple.LaunchServices",
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/com.apple.LaunchServices.syncInterrupted",
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd",
	};
	unsigned i;

	lsd_remove_list();
	if (strcmp(getprogname(), "lsd") != 0 ||
	    access("/redwood/etc/hakios.cleanvar", F_OK) != 0) {
		return;
	}
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		remove_tree(paths[i]);
		say("hakios: clean: %s %s\n", paths[i], access(paths[i], F_OK) == 0 ? "is still there" : "gone");
	}
}

/* ---- lsd: what the store build waits for ---------------------------------------------------- */

/*
 * p242..p244 left one question open: the store is built in memory (CSStore) and written to disk only
 * at the end, so an empty directory means the build never finishes -- and one thread waits on an
 * unfair lock in the CoreServices module that gates the database.  Four cheap probes name it, all of
 * them switches in /redwood/etc (the command line is nearly full):
 *   hakios.sym  the offsets from the samples, resolved to the Objective-C method just below them;
 *   hakios.rl   every nested CFRunLoopRunSpecific with the description of its sources and timers,
 *               which is what the main thread actually waits on;
 *   hakios.mkb  MobileKeyBag's own answer to "unlocked since boot" and "device lock state";
 *   plus, with lsd_watch, every notify_register_dispatch: a wait for the first-unlock Darwin
 *   notification would then be visible by name.
 */
static void
lsd_symbols(void)
{
	static const uintptr_t targets[] = { 0x136654, 0x136bfc, 0x13cea8, 0xf3990 };
	Class *classes;
	unsigned int count = 0, i;
	size_t t;

	if (strcmp(getprogname(), "lsd") != 0 ||
	    access("/redwood/etc/hakios.sym", F_OK) != 0) {
		return;
	}
	classes = objc_copyClassList(&count);
	for (t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
		uintptr_t best = 0;
		const char *best_class = "?";
		const char *best_sel = "?";
		int best_meta = 0;

		for (i = 0; i < count; i++) {
			Class pair[2];
			int m;

			pair[0] = classes[i];
			pair[1] = object_getClass(classes[i]);
			for (m = 0; m < 2; m++) {
				unsigned int methods = 0, j;
				Method *list = class_copyMethodList(pair[m], &methods);

				for (j = 0; j < methods; j++) {
					uintptr_t imp = (uintptr_t)method_getImplementation(list[j]);
					uintptr_t offset = 0;
					const char *image = sample_image_of(imp, &offset);

					if (image == NULL || strstr(image, "CoreServices") == NULL) {
						continue;
					}
					if (offset <= targets[t] && offset > best) {
						best = offset;
						best_class = class_getName(classes[i]);
						best_sel = sel_getName(method_getName(list[j]));
						best_meta = m;
					}
				}
				free(list);
			}
		}
		say("hakios: lsd symbol: CoreServices+%#lx -> %c[%s %s] + %#lx\n",
		    (unsigned long)targets[t], best_meta ? '+' : '-', best_class, best_sel,
		    (unsigned long)(targets[t] - best));
	}
	free(classes);
}

extern uint32_t notify_register_dispatch(const char *name, int *token, dispatch_queue_t queue,
    void (^handler)(int));

static uint32_t
hk_lsd_notify_register_dispatch(const char *name, int *token, dispatch_queue_t queue,
    void (^handler)(int))
{
	if (lsd_watching()) {
		lsd_log("notify: \"%s\" on \"%s\"\n", name != NULL ? name : "-",
		    queue != NULL ? lsd_queue_name(queue) : "-");
	}
	return notify_register_dispatch(name, token, queue, handler);
}

DYLD_INTERPOSE(hk_lsd_notify_register_dispatch, notify_register_dispatch)

extern SInt32 CFRunLoopRunSpecific(CFRunLoopRef runloop, CFStringRef mode, CFTimeInterval seconds,
    Boolean return_after_source);

static SInt32
hk_lsd_runloop_run_specific(CFRunLoopRef runloop, CFStringRef mode, CFTimeInterval seconds,
    Boolean return_after_source)
{
	static unsigned dumps;
	static uint64_t last;

	if (lsd_watching() && dumps < 6 && xpc_millis() - last > 2000 &&
	    access("/redwood/etc/hakios.rl", F_OK) == 0) {
		char buffer[900] = "";
		char mode_name[64] = "";
		CFStringRef text = CFCopyDescription(runloop);

		dumps++;
		last = xpc_millis();
		if (mode != NULL) {
			CFStringGetCString(mode, mode_name, sizeof(mode_name), kCFStringEncodingUTF8);
		}
		if (text != NULL) {
			CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8);
			CFRelease(text);
		}
		lsd_log("runloop \"%s\" (%.1f s): %s\n", mode_name, seconds, buffer);
	}
	return CFRunLoopRunSpecific(runloop, mode, seconds, return_after_source);
}

DYLD_INTERPOSE(hk_lsd_runloop_run_specific, CFRunLoopRunSpecific)

static void *
lsd_mkb_later(void *argument)
{
	void *library;
	int (*unlocked)(void);
	int (*lock_state)(void *);
	int round;

	(void)argument;
	sleep(15);
	library = dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",
	    RTLD_LAZY);
	if (library == NULL) {
		say("hakios: mkb: %s\n", dlerror());
		return NULL;
	}
	unlocked = (int (*)(void))dlsym(library, "MKBDeviceUnlockedSinceBoot");
	lock_state = (int (*)(void *))dlsym(library, "MKBGetDeviceLockState");
	for (round = 0; round < 12; round++) {
		say("hakios: mkb t=%ds: unlocked since boot %d, lock state %d\n", 15 + round * 30,
		    unlocked != NULL ? unlocked() : -1, lock_state != NULL ? lock_state(NULL) : -1);
		sleep(30);
	}
	return NULL;
}

static void *
lsd_runloop_later(void *argument)
{
	int round;

	(void)argument;
	sleep(300);                     /* the console log keeps only the first 256 KB and the last 1 MB */
	for (round = 0; round < 8; round++) {
		CFStringRef text;
		char buffer[1200] = "";

		sleep(20);
		text = CFCopyDescription(CFRunLoopGetMain());
		if (text != NULL) {
			CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8);
			CFRelease(text);
		}
		lsd_log("main run loop t=%ds: %s\n", 320 + 20 * round, buffer);
	}
	return NULL;
}

static void
lsd_runloop_setup(void)
{
	pthread_t th;

	if (strcmp(getprogname(), "lsd") != 0 ||
	    access("/redwood/etc/hakios.rl", F_OK) != 0) {
		return;
	}
	if (pthread_create(&th, NULL, lsd_runloop_later, NULL) == 0) {
		pthread_detach(th);
	}
}

static void *
lsd_symbols_later(void *argument)
{
	(void)argument;
	sleep(8);                       /* let lsd realize the LaunchServices classes first */
	lsd_symbols();
	return NULL;
}

static void
lsd_symbols_setup(void)
{
	pthread_t th;

	if (strcmp(getprogname(), "lsd") != 0 ||
	    access("/redwood/etc/hakios.sym", F_OK) != 0) {
		return;
	}
	if (pthread_create(&th, NULL, lsd_symbols_later, NULL) == 0) {
		pthread_detach(th);
	}
}

static void
lsd_mkb_setup(void)
{
	pthread_t th;

	if (strcmp(getprogname(), "lsd") != 0 ||
	    access("/redwood/etc/hakios.mkb", F_OK) != 0) {
		return;
	}
	if (pthread_create(&th, NULL, lsd_mkb_later, NULL) == 0) {
		pthread_detach(th);
	}
}

/* ---- lsd: who holds the CoreServices lock -------------------------------------------------- */

/*
 * Switch: /redwood/etc/hakios.locks.  p243 ruled out dispatch_sync (sixteen of them, all done in
 * 0 ms, all in the first second).  So the wait at CoreServices+0x136bfc is a plain unfair lock.
 * Every half second all threads are looked at: a thread inside __ulock_wait gives the address of the
 * lock word in x0, the word names the owner (thread id << 1 | 1), and the owner's stack is printed --
 * that is the answer to "who holds the lock lsd waits on".
 */
static uint64_t
lsd_tid_of(thread_t thread)
{
	struct thread_identifier_info info;
	mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;

	if (thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) {
		return 0;
	}
	return info.thread_id;
}

static void
lsd_owner_stack(uint64_t tid)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	unsigned i;

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	for (i = 0; i < count; i++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
		uintptr_t fp, offset;
		const char *image;
		unsigned frame;

		if (lsd_tid_of(threads[i]) != tid) {
			continue;
		}
		if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state,
		    &state_count) == KERN_SUCCESS) {
			image = sample_image_of((uintptr_t)arm_thread_state64_get_pc(state), &offset);
			say("hakios: lsd lock owner %llu: pc %s+%#lx\n", (unsigned long long)tid,
			    image != NULL ? image : "?", image != NULL ? (unsigned long)offset : 0UL);
			fp = (uintptr_t)arm_thread_state64_get_fp(state);
			for (frame = 0; frame < 12 && fp != 0 && (fp & 7) == 0; frame++) {
				struct { uintptr_t next, ret; } pair;
				mach_vm_size_t got = 0;

				if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)fp,
				    sizeof(pair), (mach_vm_address_t)&pair, &got) != KERN_SUCCESS ||
				    got != sizeof(pair) || pair.ret == 0) {
					break;
				}
				image = sample_image_of(pair.ret, &offset);
				if (image != NULL) {
					say("hakios: lsd lock owner %llu:   from %s+%#lx\n",
					    (unsigned long long)tid, image, (unsigned long)offset);
				}
				if (pair.next <= fp) {
					break;
				}
				fp = pair.next;
			}
		}
		break;
	}
	for (i = 0; i < count; i++) {
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]));
}

static void *
lsd_locks_later(void *argument)
{
	uintptr_t last_lock = 0;
	uint64_t last_said = 0;
	unsigned said = 0;
	int round;

	(void)argument;
	for (round = 0; round < 900 && said < 30; round++) {
		thread_act_array_t threads = NULL;
		mach_msg_type_number_t count = 0;
		unsigned i;

		usleep(500000);
		if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
			continue;
		}
		for (i = 0; i < count && said < 30; i++) {
			arm_thread_state64_t state;
			mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
			uintptr_t pc, offset = 0, x0;
			const char *image;
			uint32_t word;

			if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state,
			    &state_count) != KERN_SUCCESS) {
				continue;
			}
			pc = (uintptr_t)arm_thread_state64_get_pc(state);
			image = sample_image_of(pc, &offset);
			if (image == NULL || strstr(image, "libsystem_kernel") == NULL ||
			    offset < 0x1a00 || offset > 0x1b00) {
				continue;
			}
			x0 = (uintptr_t)state.__x[0];
			if (x0 < 0x100000000ULL || (x0 & 3) != 0) {
				continue;
			}
			word = *(volatile uint32_t *)x0;
			if (word == 0 || word == 1) {
				continue;
			}
			if (x0 == last_lock && xpc_millis() - last_said < 5000) {
				continue;
			}
			last_lock = x0;
			last_said = xpc_millis();
			said++;
			say("hakios: lsd lock: t%u waits on %p (word %#x, owner %llu)\n", i,
			    (void *)x0, word, (unsigned long long)(word >> 1));
			lsd_owner_stack((uint64_t)(word >> 1));
		}
		for (i = 0; i < count; i++) {
			mach_port_deallocate(mach_task_self(), threads[i]);
		}
		vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]));
	}
	say("hakios: lsd lock: done, %u waits seen\n", said);
	return NULL;
}

static void
lsd_locks_setup(void)
{
	pthread_t th;

	if (strcmp(getprogname(), "lsd") != 0 ||
	    access("/redwood/etc/hakios.locks", F_OK) != 0) {
		return;
	}
	if (pthread_create(&th, NULL, lsd_locks_later, NULL) == 0) {
		pthread_detach(th);
	}
}

/* ---- lsd: bring com.apple.iconservices up before the store build asks it --------------------- */

/*
 * Switch: /redwood/etc/hakios.prewarm.  p241 showed the two ways this goes wrong: lsd's own first
 * request to com.apple.iconservices hangs forever when it lands in the same second the agent is
 * spawned, and answering it with nothing (p240's hakios.noicons) makes lsd abort with "encoded data
 * has been corrupted, root object is not an XPC dictionary" -- it expects a reply of its own shape.
 * So two seconds in, a throwaway connection to that service is made and an empty message is sent:
 * the agent is brought up while lsd is already listening, and lsd's real request later finds it
 * answering in the 1 ms the later requests took in p239/p241.
 */
static void *
lsd_prewarm_later(void *argument)
{
	extern void xpc_connection_resume(hk_xpc_connection connection);
	extern void xpc_connection_cancel(hk_xpc_connection connection);
	extern hk_xpc_object xpc_dictionary_create(const void *keys, const void *const *values,
	    size_t count);
	static const char *const services[] = {
		"com.apple.iconservices",
		"com.apple.mobile.installd",
		"com.apple.pluginkit.pkd",
	};
	unsigned i;

	(void)argument;
	sleep(2);
	for (i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
		hk_xpc_connection connection;
		hk_xpc_object message;

		connection = xpc_connection_create_mach_service(services[i], NULL, 0);
		if (connection == NULL) {
			continue;
		}
		xpc_connection_set_event_handler(connection, ^(hk_xpc_object event) { (void)event; });
		xpc_connection_resume(connection);
		message = xpc_dictionary_create(NULL, NULL, 0);
		if (message != NULL) {
			xpc_connection_send_message(connection, message);
			lsd_log("prewarm: an empty message went to %s\n", services[i]);
		}
		sleep(2);
		xpc_connection_cancel(connection);
	}
	return NULL;
}

static void
lsd_prewarm_setup(void)
{
	pthread_t th;

	if (strcmp(getprogname(), "lsd") != 0 ||
	    access("/redwood/etc/hakios.prewarm", F_OK) != 0) {
		return;
	}
	if (pthread_create(&th, NULL, lsd_prewarm_later, NULL) == 0) {
		pthread_detach(th);
	}
}


/*
 * Boot-arg os_log_loud=1 (or 2 for every program with this library): the os_log messages of
 * lsd and the container manager (2: of everybody) as they are written, the format string as it
 * is -- "Failed to create container %{public}s: ..." names the step even without its arguments.
 * logd cannot be asked: it has no helper here, so its store is not written at all.
 */
typedef void *hk_os_log_t;
extern void _os_log_impl(void *dso, hk_os_log_t log, uint8_t type, const char *format,
    uint8_t *buffer, unsigned int size);
extern void _os_log_error_impl(void *dso, hk_os_log_t log, uint8_t type, const char *format,
    uint8_t *buffer, unsigned int size);
extern void _os_log_fault_impl(void *dso, hk_os_log_t log, uint8_t type, const char *format,
    uint8_t *buffer, unsigned int size);

static int os_log_watch;
static int os_log_all;
static unsigned os_log_lines;

static int
os_log_wanted(void)
{
	if (!os_log_watch || os_log_lines >= 800) {
		return 0;
	}
	if (os_log_all) {
		return 1;
	}
	return strcmp(getprogname(), "lsd") == 0 ||
	    strncmp(getprogname(), "containermanagerd", 17) == 0;
}

static void
os_log_said(const char *which, uint8_t type, const char *format)
{
	if (format != NULL && os_log_wanted()) {
		os_log_lines++;
		say("hakios: os_log %s[%d] %s type %u: %s\n", getprogname(), getpid(), which, type,
		    format);
	}
}

static void
hk_os_log_impl(void *dso, hk_os_log_t log, uint8_t type, const char *format, uint8_t *buffer,
    unsigned int size)
{
	static void (*original)(void *, hk_os_log_t, uint8_t, const char *, uint8_t *, unsigned int);

	if (original == NULL) {
		original = (void (*)(void *, hk_os_log_t, uint8_t, const char *, uint8_t *,
		    unsigned int))dlsym(RTLD_NEXT, "_os_log_impl");
	}
	os_log_said("log", type, format);
	if (original != NULL) {
		original(dso, log, type, format, buffer, size);
	}
}

static void
hk_os_log_error_impl(void *dso, hk_os_log_t log, uint8_t type, const char *format, uint8_t *buffer,
    unsigned int size)
{
	static void (*original)(void *, hk_os_log_t, uint8_t, const char *, uint8_t *, unsigned int);

	if (original == NULL) {
		original = (void (*)(void *, hk_os_log_t, uint8_t, const char *, uint8_t *,
		    unsigned int))dlsym(RTLD_NEXT, "_os_log_error_impl");
	}
	os_log_said("error", type, format);
	if (original != NULL) {
		original(dso, log, type, format, buffer, size);
	}
}

static void
hk_os_log_fault_impl(void *dso, hk_os_log_t log, uint8_t type, const char *format, uint8_t *buffer,
    unsigned int size)
{
	static void (*original)(void *, hk_os_log_t, uint8_t, const char *, uint8_t *, unsigned int);

	if (original == NULL) {
		original = (void (*)(void *, hk_os_log_t, uint8_t, const char *, uint8_t *,
		    unsigned int))dlsym(RTLD_NEXT, "_os_log_fault_impl");
	}
	os_log_said("fault", type, format);
	if (original != NULL) {
		original(dso, log, type, format, buffer, size);
	}
}

/*
 * (not installed: libsystem_trace's _os_log_impl is not given to dyld's interpose in this port --
 * with it in the list every program that called os_log crashed, and not one message was seen.
 * The code stays for the day it can be checked differently; see collab.md.)
 */
#if 0
DYLD_INTERPOSE(hk_os_log_impl, _os_log_impl)
DYLD_INTERPOSE(hk_os_log_error_impl, _os_log_error_impl)
DYLD_INTERPOSE(hk_os_log_fault_impl, _os_log_fault_impl)
#endif

static void
os_log_watch_setup(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	os_log_watch = strstr(arguments, "os_log_loud=1") != NULL ||
	    strstr(arguments, "os_log_loud=2") != NULL;
	os_log_all = strstr(arguments, "os_log_loud=2") != NULL;
	if (os_log_watch) {
		say("hakios: os_log: %s watched\n", os_log_all ? "every program" : "lsd and the container managers");
	}
}

/*
 * Boot-arg hakios_quiet=1: this program's stdout and stderr to /dev/null; our own lines stay on
 * the console (say_fd).
 */
static void
stdio_quiet_setup(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	int console, null;
	int quiet;

	/*
	 * Our lines go to the console always, whatever the program's stderr was made.  With
	 * logd_helper back in the image, launchd hands every job a pipe to logd on fd 2, and a
	 * daemon's own words -- the ones that say why it is about to trap -- then never reach the
	 * USB log.  That cost a whole run of the container manager (p179: eleven SIGILLs and not
	 * one line of its own).
	 */
	console = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (console >= 0) {
		say_fd = console;
	}
	/*
	 * /redwood/etc/hakios.console.<program>: the program's own stderr on the console as well.
	 * libc's assertion message is what names the numbers behind a trap (searchd's crash is
	 * "failed assertion 'inOffset < inStorage->_freeRegion' ... offset:%lld, free_region:%lld"
	 * in SpotlightIndex's storage.h), and a job's stderr is a pipe to logd, which has no reader
	 * here.  Only the programs asked for by name pay for it.
	 */
	{
		char path[160];

		snprintf(path, sizeof(path), "/redwood/etc/hakios.console.%s", getprogname());
		if (console >= 0 && access(path, F_OK) == 0) {
			dup2(console, 2);
			say("hakios: %s[%d]: stderr is the console too\n", getprogname(), getpid());
		}
	}
	quiet = sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, "hakios_quiet=1") != NULL;
	if (!quiet) {
		return;
	}
	null = open("/dev/null", O_WRONLY);
	if (null >= 0) {
		dup2(null, 1);
		dup2(null, 2);
		if (null > 2) {
			close(null);
		}
	}
	say("hakios: %s[%d]: stdout and stderr are /dev/null (hakios_quiet=1)\n", getprogname(),
	    getpid());
}

/* ---- what the kernel's device tree says the serial is --------------------------------------- */

/*
 * Switch: /redwood/etc/hakios.dtserial.  Settings -> About says RDW00POCn3d while the loader's
 * device tree was patched to say mi9thecan3d (p341, p343), so one of the two is not what the kernel
 * holds.  This reads the property the kernel really has -- MobileGestalt reads the same registry --
 * and says it out loud, as text: a DT property arrives as data, and only the printable bytes matter.
 */
static void
dt_serial_setup(void)
{
	io_registry_entry_t tree;
	CFTypeRef value;
	char text[160];

	if (access("/redwood/etc/hakios.dtserial", F_OK) != 0) {
		return;
	}
	tree = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/");
	if (tree == MACH_PORT_NULL) {
		say("hakios: %s: dtserial: no IODeviceTree here\n", getprogname());
		return;
	}
	value = IORegistryEntryCreateCFProperty(tree, CFSTR("serial-number"), kCFAllocatorDefault, 0);
	if (value == NULL) {
		say("hakios: %s: dtserial: the tree has no serial-number\n", getprogname());
	} else if (CFGetTypeID(value) == CFDataGetTypeID()) {
		CFIndex length = CFDataGetLength(value), n;
		const UInt8 *bytes = CFDataGetBytePtr(value);
		int at = 0;

		for (n = 0; n < length && at < (int)sizeof(text) - 2; n++) {
			unsigned char c = bytes[n];

			if (c >= 0x20 && c < 0x7f) {
				text[at++] = (char)c;
			}
		}
		text[at] = 0;
		say("hakios: %s: dtserial: %ld bytes \"%s\"\n", getprogname(), (long)length, text);
	} else if (CFGetTypeID(value) == CFStringGetTypeID()) {
		if (CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8)) {
			say("hakios: %s: dtserial: \"%s\"\n", getprogname(), text);
		}
	} else {
		say("hakios: %s: dtserial: a property of type %lu\n", getprogname(),
		    (unsigned long)CFGetTypeID(value));
	}
	if (value != NULL) {
		CFRelease(value);
	}
	IOObjectRelease(tree);
}

static void
xpc_watch_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	xpc_watch = strstr(arguments, "xpc_loud=1") != NULL;
	if (lsd_watch && strcmp(getprogname(), "lsd") == 0) {
		xpc_watch = 1;                  /* its own calls are what lsd_watch=1 was asked for */
	}
	if (xpc_watch) {
		say("hakios: xpc: connections with \"container\" in their name are watched\n");
	}
}

static void
lsd_watch_setup(void)
{
	char arguments[2048] = "";
	size_t size = sizeof(arguments) - 1;

	/*
	 * The boot arguments are nearly full (~600 of the ~640 this loader takes), so the switch
	 * is a file in the boot tree as well: /redwood/etc/hakios.lsd, made by the build script.
	 * A file costs nothing in the command line, and this run has already had one argument cut
	 * in half at the limit.
	 */
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		arguments[0] = 0;
	}
	lsd_watch = strstr(arguments, "lsd_watch=1") != NULL ||
	    access("/redwood/etc/hakios.lsd", F_OK) == 0;
	no_barrier = access("/redwood/etc/hakios.nobarrier", F_OK) == 0;
	no_icons_db = access("/redwood/etc/hakios.noiconsdb", F_OK) == 0;
	no_mcm_log = access("/redwood/etc/hakios.nomcmlog", F_OK) == 0;
	icons_reply = access("/redwood/etc/hakios.iconsreply", F_OK) == 0;
	icons_replay = access("/redwood/etc/hakios.iconsreplay", F_OK) == 0;
	if (lsd_watch && strcmp(getprogname(), "lsd") == 0) {
		say("hakios: lsd xpc: the listener and its connections are watched\n");
	}
}

/*
 * Boot-arg logdump=S (seconds): after S seconds, the printable runs of logd's own store
 * (/private/var/db/diagnostics) are printed.  The daemons' os_log messages are in there when
 * logd and its helper run -- the format strings alone name the failing steps, and logd's
 * format is not one this port can read from the host.
 */
static void
log_strings_of(const char *path, unsigned *lines, unsigned most)
{
	char chunk[65536];
	int fd = open(path, O_RDONLY);
	ssize_t got;
	char run[200];
	size_t at = 0;
	ssize_t i;

	if (fd < 0) {
		return;
	}
	while ((got = read(fd, chunk, sizeof(chunk))) > 0 && *lines < most) {
		for (i = 0; i < got && *lines < most; i++) {
			unsigned char c = (unsigned char)chunk[i];

			if (c >= 0x20 && c < 0x7f) {
				if (at + 1 < sizeof(run)) {
					run[at++] = (char)c;
				}
				continue;
			}
			if (at >= 24) {
				run[at] = 0;
				(*lines)++;
				say("hakios: logstr: %s\n", run);
			}
			at = 0;
		}
	}
	if (at >= 24 && *lines < most) {
		run[at] = 0;
		(*lines)++;
		say("hakios: logstr: %s\n", run);
	}
	close(fd);
}

static void *
log_dump_later(void *argument)
{
	static const char *const dirs[] = {
		"/private/var/db/diagnostics/Persist",
		"/private/var/db/diagnostics/Special",
		"/private/var/db/diagnostics/HighVolume",
	};
	unsigned lines = 0, d;

	sleep((unsigned)(intptr_t)argument);
	for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && lines < 300; d++) {
		DIR *dir = opendir(dirs[d]);
		struct dirent *entry;

		if (dir == NULL) {
			say("hakios: logdump: %s: %s\n", dirs[d], strerror(errno));
			continue;
		}
		while ((entry = readdir(dir)) != NULL && lines < 300) {
			char path[256];

			if (entry->d_name[0] == '.') {
				continue;
			}
			snprintf(path, sizeof(path), "%s/%s", dirs[d], entry->d_name);
			say("hakios: logdump: %s\n", path);
			log_strings_of(path, &lines, 300);
		}
		closedir(dir);
	}
	say("hakios: logdump: %u lines\n", lines);
	return NULL;
}


/* ---- the crash reports the last boot left on the volume ------------------------------------- */

/*
 * Boot-arg crashdump=S: after S seconds, the text of the crash reports in
 * /private/var/mobile/Library/Logs/CrashReporter is printed.  installcoordinationd crashes seven
 * times in a row when Settings asks for iPhone Storage ("Corpse allowed 1 of 5" is the kernel
 * taking the report), and its os_log never reaches the console; the report is on the data volume,
 * which is a real APFS volume now, so it is read here and comes out in the log.
 */
static void
crash_file(const char *path, unsigned most)
{
	char chunk[3072];
	int fd = open(path, O_RDONLY);
	ssize_t got;
	char line[220];
	size_t at = 0;
	ssize_t i;

	if (fd < 0) {
		return;
	}
	got = read(fd, chunk, sizeof(chunk) - 1);
	close(fd);
	if (got <= 0) {
		return;
	}
	say("hakios: crashdump: %s (%ld bytes)\n", path, (long)got);
	for (i = 0; i < got; i++) {
		unsigned char c = (unsigned char)chunk[i];

		if (c == '\n' || at + 1 >= sizeof(line)) {
			line[at] = 0;
			if (at >= 3) {
				most--;
				say("hakios: crash: %s\n", line);
			}
			at = 0;
			if (most == 0) {
				break;
			}
			continue;
		}
		if (c >= 0x20 && c < 0x7f) {
			line[at++] = (char)c;
		}
	}
}

static void *
crash_dump_later(void *argument)
{
	static const char *const dirs[] = {
		"/private/var/mobile/Library/Logs/CrashReporter",
		"/private/var/mobile/Library/Logs/CrashReporter/DiagnosticLogs",
	};
	unsigned d, seen = 0;

	sleep((unsigned)(intptr_t)argument);
	for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && seen < 6; d++) {
		DIR *dir = opendir(dirs[d]);
		struct dirent *entry;

		if (dir == NULL) {
			say("hakios: crashdump: %s: %s\n", dirs[d], strerror(errno));
			continue;
		}
		while ((entry = readdir(dir)) != NULL && seen < 6) {
			char path[300];

			if (entry->d_name[0] == '.') {
				continue;
			}
			snprintf(path, sizeof(path), "%s/%s", dirs[d], entry->d_name);
			seen++;
			crash_file(path, 60);
		}
		closedir(dir);
	}
	say("hakios: crashdump: %u files\n", seen);
	return NULL;
}

static void
crash_dump_setup(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *at;
	pthread_t thread;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "crashdump=");
	if (at == NULL) {
		return;
	}
	at += 10;
	if (*at < '1' || *at > '9') {
		return;
	}
	if (pthread_create(&thread, NULL, crash_dump_later, (void *)(intptr_t)strtol(at, NULL, 10)) == 0) {
		pthread_detach(thread);
	}
}

static void
log_dump_setup(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *at;
	pthread_t thread;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	at = strstr(arguments, "logdump=");
	if (at == NULL) {
		return;
	}
	at += 8;
	if (*at < '1' || *at > '9') {
		return;
	}
	if (pthread_create(&thread, NULL, log_dump_later, (void *)(intptr_t)strtol(at, NULL, 10)) == 0) {
		pthread_detach(thread);
	}
}

/* ---- the store of LaunchServices: can it be written here ------------------------------------- */

/*
 * Boot-arg store_probe=1: lsd, 20 s in, says what the directories of its own container are on this
 * data volume and whether a file can be made in them -- the store lives there, and "the store is
 * not ready" is the one thing the window server cannot be told to wait for.
 */
static void *
store_probe_later(void *argument)
{
	static const char *const paths[] = {
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd",
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library",
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches",
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd",
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/.com.apple.mobile_container_manager.metadata.plist",
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.configurationprofiles/.com.apple.mobile_container_manager.metadata.plist",
	};
	unsigned i;
	const char *cache = "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd/Library/Caches/com.apple.lsd";

	sleep((unsigned)(intptr_t)argument);
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		struct stat st;
		int error = stat(paths[i], &st);

		say("hakios: store probe: %s: %s%s\n", paths[i],
		    error == 0 ? "ok" : strerror(errno),
		    error == 0 ? (S_ISDIR(st.st_mode) ? " (dir)" : " (file)") : "");
		if (error == 0 && S_ISDIR(st.st_mode)) {
			DIR *dir = opendir(paths[i]);
			struct dirent *entry;
			unsigned n = 0;

			while (dir != NULL && (entry = readdir(dir)) != NULL && n < 12) {
				say("hakios: store probe:   %s/%s\n", paths[i], entry->d_name);
				n++;
			}
			if (dir != NULL) {
				closedir(dir);
			}
		}
	}
	{
		char test[256];
		int fd;

		snprintf(test, sizeof(test), "%s/.hakios-write-test", cache);
		fd = open(test, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		say("hakios: store probe: write a file: %s\n", fd >= 0 ? "ok" : strerror(errno));
		if (fd >= 0) {
			close(fd);
			unlink(test);
		}
	}
	say("hakios: store probe: done (uid %d)\n", getuid());
	return NULL;
}

static void
store_probe_setup(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	pthread_t thread;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 ||
	    strstr(arguments, "store_probe=") == NULL) {
		return;
	}
	if (strcmp(getprogname(), "lsd") != 0) {
		return;
	}
	if (pthread_create(&thread, NULL, store_probe_later, (void *)(intptr_t)20) == 0) {
		pthread_detach(thread);
	}
}

/* ---- who takes the containers away ----------------------------------------------------------- */

/*
 * Boot-arg mcm_watch=1: the container manager says, every two seconds, what is in the system
 * group and system container directories -- and so shows the moment one disappears.  lsd's own
 * store directory is there at the boot and gone by 20 s (p168), and no one has said yet who
 * takes it.
 */
static void *
mcm_watch_later(void *argument)
{
	static const char *const dirs[] = {
		"/private/var/containers/Shared/SystemGroup",
		"/private/var/containers/Data/System",
		"/private/var/root/Library/Caches/com.apple.containermanagerd/Staging",
	};
	char last[3][512] = { { 0 }, { 0 }, { 0 } };
	unsigned round;

	(void)argument;
	for (round = 0; round < 90; round++) {
		unsigned d;

		for (d = 0; d < 3; d++) {
			char listing[512] = "";
			DIR *dir = opendir(dirs[d]);
			struct dirent *entry;
			int lsd;

			while (dir != NULL && (entry = readdir(dir)) != NULL) {
				if (entry->d_name[0] == '.') {
					continue;
				}
				if (strlen(listing) + strlen(entry->d_name) + 2 < sizeof(listing)) {
					strcat(listing, entry->d_name);
					strcat(listing, " ");
				}
			}
			if (dir != NULL) {
				closedir(dir);
			}
			if (strcmp(last[d], listing) != 0) {
				strlcpy(last[d], listing, sizeof(last[d]));
				lsd = access("/private/var/containers/Shared/SystemGroup/"
				    "systemgroup.com.apple.lsd/.com.apple.mobile_container_manager.metadata.plist",
				    F_OK) == 0;
				say("hakios: mcm watch: t=%us %s: [%s] lsd plist %s\n", round * 2, dirs[d],
				    listing, lsd ? "there" : "gone");
			}
		}
		sleep(2);
	}
	say("hakios: mcm watch: done\n");
	return NULL;
}

static void
mcm_watch_setup(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	pthread_t thread;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 ||
	    strstr(arguments, "mcm_watch=1") == NULL) {
		return;
	}
	if (strncmp(getprogname(), "containermanagerd", 17) != 0) {
		return;
	}
	if (pthread_create(&thread, NULL, mcm_watch_later, NULL) == 0) {
		pthread_detach(thread);
	}
}

static void
trap_watchdog(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *p;
	long secs;
	pthread_t th;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	if ((p = strstr(arguments, "hakios_trap=")) == NULL) {
		return;
	}
	secs = strtol(p + 12, NULL, 10);
	if (secs <= 0 || secs > 3600) {
		return;
	}
	say("hakios: watchdog: SIGTRAP in %ld s (hakios_trap)\n", secs);
	if (pthread_create(&th, NULL, trap_after, (void *)(intptr_t)secs) == 0) {
		pthread_detach(th);
	}
}

static void
opengl_es(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;
	void *library;
	int wanted = -1, told = 0;

	if (access(HAKIOS_GL, R_OK) != 0) {
		return;                                 /* (an image built without it) */
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		functions_said_yes(arguments);
		compile_shaders_said = strstr(arguments, "glcompile=1") != NULL;
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				wanted = argument[3] != '0';
				told = 1;
			}
		}
	}
	if (wanted < 0) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
		wanted = gpu != 0;
		if (gpu != 0) {
			IOObjectRelease(gpu);
		}
	}
	if (!wanted) {
		say("hakios: %s: CoreAnimation keeps its software renderer\n", told ? "gl=0" : "no GPU to draw with");
		return;
	}
	if ((library = dlopen(HAKIOS_GL, RTLD_NOW)) == NULL) {
		say("hakios: %s: %s\n", HAKIOS_GL, dlerror());
		return;
	}
	gl_loaded = 1;
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(library,
	    "hakios_gl_function_values");
	gl_function_refused = (void (*)(unsigned))dlsym(library, "hakios_gl_function_refused");
}

__attribute__((constructor))
static void
hakios_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	/*
	 * Any program of ours that goes through MobileContainerManager can get a nil path out of it
	 * on this port (the containers are not made); the window server was the first to crash on
	 * it, lsd the next. Make it a general repair, not a backboardd one.
	 */
	main_thread = pthread_self();
	{
		/*
		 * Our lines go to the console whatever the program's stderr was made: with logd's
		 * helper running, launchd wires a job's stderr into logd, and the boot log we read
		 * over USB would not carry them.
		 */
		int console = open("/dev/console", O_WRONLY | O_NOCTTY);

		if (console >= 0) {
			say_fd = console;
		}
	}
	shm_probe();
	url_path_fixes();
	dictionary_fixes();
	file_manager_fixes();
	trap_watchdog();
	sample_watchdog();
	sample_every_setup();
	lsd_files_setup();
	lsd_locks_setup();
	lsd_fslog_setup();
	lsd_clean_setup();
	lsd_install_progress_fixes();
	lsd_runloop_setup();
	lsd_symbols_setup();
	lsd_mkb_setup();
	lsd_prewarm_setup();
	lsd_watch_setup();
	say("hakios: %s: library in (uid %d, effective %d)\n", name, (int)getuid(), (int)geteuid());
	xpc_watch_setup();
	os_log_watch_setup();
	log_dump_setup();
	crash_dump_setup();
	store_probe_setup();
	mcm_watch_setup();
	stdio_quiet_setup();
	dt_serial_setup();
	/*
	 * Switch: /redwood/etc/hakios.nolsfix.  The LaunchServices store paths are ours (the lsd
	 * system-group container); the port's own notes never touch them, and the store on a phone
	 * with ramfs is written where stock _LSDefaults points by itself.  Leaving the overrides out
	 * is the closest this image gets to what upstream boots with.
	 */
	if (access("/redwood/etc/hakios.nolsfix", F_OK) != 0) {
		ls_defaults_fixes();
	}
	install_collection_safety_hooks();
	watch_for_faults();
	{
		void (*set_uncaught)(void (*)(id)) = (void (*)(void (*)(id)))dlsym(RTLD_DEFAULT, "NSSetUncaughtExceptionHandler");
		if (set_uncaught != NULL) {
			set_uncaught(hakios_exception_handler);
		}
	}
	if (strcmp(name, "logd") == 0) {
		intptr_t exe_slide = _dyld_get_image_vmaddr_slide(0);
		say("hakios: logd[%d] starting (exe slide 0x%lx), uid=%d euid=%d gid=%d\n",
		    getpid(), (unsigned long)exe_slide, getuid(), geteuid(), getgid());
		uintptr_t target = 0x100016538UL + exe_slide;
		vm_address_t page = (vm_address_t)target & ~(vm_address_t)(vm_page_size - 1);
		kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE,
		    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
		if (kr == KERN_SUCCESS) {
			uint32_t *code = (uint32_t *)target;
			code[0] = 0x52800000; /* mov w0, #0 */
			code[1] = 0xd65f03c0; /* ret */
			vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
			sys_icache_invalidate((void *)target, 8);
			say("hakios: logd: directory migration check at %p patched -> return 0\n", (void *)target);
		} else {
			say("hakios: logd: vm_protect failed on %p (%d)\n", (void *)target, kr);
		}
	}
	if (strcmp(name, "lsd") == 0) {
		say("hakios: lsd[%d] starting, uid=%d euid=%d gid=%d egid=%d\n", getpid(), getuid(), geteuid(), getgid(), getegid());
		lsd_listener_hooks();
		if (objc_getClass("_LSDefaults") == NULL) {
			say("hakios: lsd: warning: _LSDefaults class not yet in runtime\n");
		}
		if (slide != 0) {
			uintptr_t got_slot = 0x1dae50860UL + slide;
			vm_address_t page = (vm_address_t)got_slot & ~(vm_address_t)(vm_page_size - 1);
			kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
			if (kr == KERN_SUCCESS) {
				*(uintptr_t *)got_slot = (uintptr_t)my_objc_storeStrong;
				say("hakios: lsd: CoreServices GOT objc_storeStrong at %p patched -> %p\n", (void *)got_slot, my_objc_storeStrong);
			} else {
				say("hakios: lsd: vm_protect failed on CoreServices GOT (%d)\n", kr);
			}
		}
	}
	if (strcmp(name, "backboardd") == 0) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		/*
		 * Boot-arg ca_vbl=1: CoreAnimation's frame clock from the display driver's refresh
		 * notifications. QuartzCore takes them (IOMobileFramebufferEnableVSyncNotifications) only on
		 * the phones with an A7 to A10, or when CA_FORCE_TIMER is in the environment
		 * (CADeviceUseVBL()); on the iPhone X it runs a timer of its own instead (CADisplayTimer),
		 * whose phase it would set from the frame timings of a display driver this one is not.
		 */
		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "ca_vbl=1") != NULL) {
			setenv("CA_FORCE_TIMER", "1", 1);
			say("hakios: ca_vbl=1: CoreAnimation is to take the display driver's refresh notifications\n");
		}
		/*
		 * Boot-arg caenv=NAME=VALUE (as many as wanted): into the window server's environment, for
		 * CoreAnimation's own switches (CA_LOG_TRIPLE_BUFFERING=1, CA_FORCE_DOUBLE_BUFFERING=1 ...).
		 */
		for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
			char setting[128];
			size_t length = strcspn(argument + 6, " ");
			char *equals;

			if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
				continue;
			}
			memcpy(setting, argument + 6, length);
			setting[length] = 0;
			if ((equals = strchr(setting, '=')) != NULL) {
				*equals = 0;
				setenv(setting, equals + 1, 1);
				say("hakios: caenv: %s=%s for the window server\n", setting, equals + 1);
			}
		}
		opengl_es();
		window_server_fixes();
		/*
		 * brightness_fixes() is not used for now: with it backboardd no longer took our touch
		 * screen service for its own ("still waiting for a main screen digitizer"), whether the
		 * service was made before or after the touch processing starts. (Most likely it only
		 * moved the start of the event server onto the moment make_service() now knows how to
		 * survive; not tried again since, and until it is, the five seconds stay.)
		 */
		input_start();
	} else if (strncmp(name, "lsd", 3) == 0) {
		start_delays();
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
	}
}
