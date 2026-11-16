/*
 * libhakios.app.dylib: code of ours inside an application, to find out why it dies (see inject.c
 * for how it gets there; build.sh's INJECT names the programs).
 *
 * An application that is launched and gone again in a third of a second leaves nothing behind here:
 * there is no crash reporter in this system, and what it writes on its standard error goes nowhere.
 * This gives it a standard error -- the console -- so that the Swift runtime's last words are read,
 * and says the frames of the stack when a signal kills it, under the dyld shared cache's slide,
 * which `ipsw dyld a2s <cache> <address>` turns back into names.
 */
#include <dispatch/dispatch.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <pthread.h>
#include <util.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <execinfo.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL

static uintptr_t slide;
static int console = -1;
static const char *who = "?";

static void
say(const char *text, size_t length)
{
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	(void)write(console >= 0 ? console : 2, text, length);
}

static void
killed(int number, siginfo_t *info, void *context)
{
	void *frames[32];
	char line[900];
	int n = backtrace(frames, 32), k;

	(void)context;
	k = snprintf(line, sizeof(line), "hakios: %s: SIGNAL %d at %p (the cache's slide %#lx), the stack:", who, number,
	    info != NULL ? info->si_addr : NULL, (unsigned long)slide);
	for (int i = 0; i < n && k < (int)sizeof(line) - 24; i++) {
		k += snprintf(line + k, sizeof(line) - (size_t)k, " %#lx", (unsigned long)((uintptr_t)frames[i] - slide));
	}
	line[k++] = '\n';
	say(line, (size_t)k);
	signal(number, SIG_DFL);
	raise(number);
}

/*
 * Two things an application does on its way to a download, said out loud. Sileo queues a package and
 * nothing happens: no connection is made and no error is shown, so the question is how far it gets --
 * does it ask UIKit for a background task (a round trip to another daemon, which can wait for ever if
 * that daemon does not answer), and does it ever start a URL session task, for what address.
 */
static unsigned long (*begin_task)(id, SEL, id);
static void (*resume_task)(id, SEL);

static unsigned long
watched_begin_task(id self, SEL cmd, id handler)
{
	unsigned long task;
	char line[160];
	int n;

	n = snprintf(line, sizeof(line), "hakios: %s: asking UIKit for a background task\n", who);
	say(line, (size_t)(n > 0 ? n : 0));
	task = begin_task(self, cmd, handler);
	n = snprintf(line, sizeof(line), "hakios: %s: the background task is %lu\n", who, task);
	say(line, (size_t)(n > 0 ? n : 0));
	return task;
}

static void
watched_resume_task(id self, SEL cmd)
{
	const char *(*string)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *url = "?";
	char line[700];
	id request, address, text;
	int n;

	request = get(self, sel_registerName("originalRequest"));
	address = request != NULL ? get(request, sel_registerName("URL")) : NULL;
	text = address != NULL ? get(address, sel_registerName("absoluteString")) : NULL;
	if (text != NULL) {
		url = string(text, sel_registerName("UTF8String"));
	}
	n = snprintf(line, sizeof(line), "hakios: %s: starting a session task for %s\n", who,
	    url != NULL ? url : "?");
	say(line, (size_t)(n > 0 ? n : 0));
	resume_task(self, cmd);
}

/*
 * One setting of Sileo's, set for it at every start: "Show Packages From Canister" (ShowProvisional,
 * on by default). Canister is the search service Sileo asks about packages, and where its servers
 * cannot be reached the asking never stops -- 3810 session tasks in two minutes in one run here,
 * and the download of a package never gets its turn among them. With the setting off Sileo keeps to
 * the repositories themselves, which work. The prefix lives in memory and is new at every boot, so
 * there is nowhere to keep this but here. It is done on the main queue, not here in the library's
 * own initializer: at that point Foundation is loaded but has not run its own initializers yet, and
 * asking it for the defaults then kills the application before it says a word.
 */
static void
keep_to_the_repositories(void)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*from_text)(id, SEL, const char *) = (id (*)(id, SEL, const char *))objc_msgSend;
	void (*set_bool)(id, SEL, signed char, id) = (void (*)(id, SEL, signed char, id))objc_msgSend;
	id defaults = (id)objc_getClass("NSUserDefaults");
	id key = from_text((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "ShowProvisional");

	if (defaults == NULL || key == NULL) {
		return;
	}
	defaults = get(defaults, sel_registerName("standardUserDefaults"));
	if (defaults != NULL) {
		set_bool(defaults, sel_registerName("setBool:forKey:"), 0, key);
		say("hakios: Sileo: its Canister search is turned off (it is not reachable here)\n", 73);
	}
}

/*
 * apt-get install, not its simulation (-s...): the one that ends in "Unable to fetch some archives"
 * when Sileo runs it and installs the package when a terminal runs the same command.
 */
static int
apt_install(char *const argv[])
{
	int install = 0;

	if (argv == NULL || argv[0] == NULL || strcmp(argv[0], "apt-get") != 0) {
		return 0;
	}
	for (int i = 1; argv[i] != NULL; i++) {
		if (argv[i][0] == '-' && argv[i][1] == 's') {
			return 0;
		}
		install |= strcmp(argv[i], "install") == 0;
	}
	return install;
}

/*
 * What apt says goes to Sileo, which shows a line of it at most. Run the same apt-get, with the same
 * descriptors, credential and environment, under a shell that first says on the console who it runs
 * as, what lies in apt's archives and what Sileo's index says of the package, and afterwards how apt
 * ended and what the archives hold then. This is how "Unable to fetch some archives" was found: the
 * archives were empty, Sileo's /bin/mv having never run.
 */
static int
spawn_apt_said(pid_t *pid, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
    char *const argv[], char *const envp[])
{
	static char script[] =
	    "exec 7>/dev/console; J=/var/jb\n"
	    "echo \"hakios: apt-get runs as $($J/usr/bin/id), in $(pwd)\" >&7\n"
	    "$J/bin/ls -ln $J/var/cache/apt/archives $J/var/lib/apt/sileolists >&7 2>&1\n"
	    "for a in \"$@\"; do case $a in -*|install) ;; *) p=${a%%=*}\n"
	    "  echo \"hakios: $p in sileolists:\" >&7\n"
	    "  $J/usr/bin/grep -h -A30 \"^Package: $p\\$\" $J/var/lib/apt/sileolists/*Packages 2>&1 | $J/usr/bin/head -40 >&7 ;; esac; done\n"
	    "$J/usr/bin/apt-get \"$@\"\n"
	    "s=$?; echo \"hakios: apt-get exits $s; its archives after:\" >&7\n"
	    "$J/bin/ls -ln $J/var/cache/apt/archives >&7 2>&1; exit $s\n";
	char *wrapped[96];
	int k = 0;

	for (int i = 0; envp != NULL && envp[i] != NULL; i++) {
		char line[600];
		int n = snprintf(line, sizeof(line), "hakios: %s: apt-get's environment: %s\n", who, envp[i]);

		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
	wrapped[k++] = "sh";
	wrapped[k++] = "-c";
	wrapped[k++] = script;
	wrapped[k++] = "apt-get";
	for (int i = 1; argv[i] != NULL && k < 95; i++) {
		wrapped[k++] = argv[i];
	}
	wrapped[k] = NULL;
	return posix_spawn(pid, "/var/jb/bin/sh", actions, attr, wrapped, envp);
}

/*
 * Every program the application starts, with its arguments. Sileo does everything to the system by
 * spawning its set-user-ID helper, which execs apt or dpkg; the arguments are the whole question
 * when apt's answer to Sileo differs from apt's answer on the command line.
 */
static int
watched_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "hakios: %s: (uid %d) spawns %s", who, (int)getuid(),
	    path != NULL ? path : "?");

	for (int i = 0; argv != NULL && argv[i] != NULL && n < (int)sizeof(line) - 40; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", argv[i]);
	}
	/*
	 * The size of what it is about to move, while the file is still there: apt refuses the
	 * package Sileo puts in its archives ("Unable to fetch some archives") and takes the very
	 * same package when apt itself fetched it, so the first thing to know is whether the file
	 * is whole.
	 */
	if (argv != NULL && argv[0] != NULL && strcmp(argv[0], "mv") == 0 && argv[1] != NULL) {
		struct stat st;

		if (stat(argv[1], &st) == 0) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " [%lld bytes]", (long long)st.st_size);
		} else {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " [no such file]");
		}
	}
	n += snprintf(line + n, sizeof(line) - (size_t)n, "\n");
	say(line, (size_t)n);
	if (apt_install(argv)) {
		return spawn_apt_said(pid, actions, attr, argv, envp);
	}
	/*
	 * NewTerm's own way to a shell is /var/jb/usr/bin/login (p436: it starts, says nothing, and the
	 * pane ends with "Session Ended"; its euid is 501, not the 0 its setuid bit asks for, so the
	 * PAM login has nothing to switch to).  Until that is understood, the shell itself is tried in
	 * its place: the prefix's sh, as a login shell, on the very pty NewTerm made.
	 */
	if (path != NULL && strstr(path, "/usr/bin/login") != NULL && argv != NULL && argv[0] != NULL &&
	    strstr(argv[0], "login") != NULL) {
		/*
		 * NewTermLoginHelper (52 KB, main disassembled: setsid, ioctl(0, TIOCSCTTY), chdir(argv[1]),
		 * execvp(argv[2]) with argv[0] = "-<basename>") is what login is meant to end up in, and
		 * login itself dies before any execve (p437: no execve, no exit, and the pane says "Session
		 * Ended").  The helper is spawned in its place, with the very arguments login was given.
		 */
		char *helper_argv[] = { "-NewTermLoginHelper", "/var/jb/var/mobile", "/redwood/bin/hakios-shell", NULL };
		int r = posix_spawn(pid, "/Applications/NewTerm.app/NewTermLoginHelper", actions, attr, helper_argv, envp);

		snprintf(line, sizeof(line), "hakios: %s: login is replaced with NewTermLoginHelper (home %s, shell %s): %s\n",
		    who, helper_argv[1], helper_argv[2], r == 0 ? "spawned" : strerror(r));
		say(line, strlen(line));
		return r;
	}
	n = posix_spawn(pid, path, actions, attr, argv, envp);
	if (n != 0) {
		char said[300];
		int k = snprintf(said, sizeof(said), "hakios: %s: ... and that spawn failed: %s\n", who, strerror(n));

		say(said, (size_t)(k < (int)sizeof(said) ? k : (int)sizeof(said) - 1));
	}
	return n;
}

/*
 * NewTerm opens a pty, puts login into it, and the pane stays empty (p432/p433: the shell itself is
 * fine -- init runs /var/jb/bin/sh from the same prefix and it prints).  Two facts are missing: did
 * openpty work at all, and does the login process live or die.  The first is said as it happens;
 * the second, twenty-five seconds later, by listing this process's children and their states.
 */
static int
watched_openpty(int *amaster, int *aslave, char *name, const struct termios *termp, const struct winsize *winp)
{
	char line[400];
	int n = openpty(amaster, aslave, name, termp, winp);

	if (n == 0) {
		snprintf(line, sizeof(line), "hakios: %s: openpty: ok (master %d, slave %d, name %s)\n", who, *amaster,
		    *aslave, name != NULL ? name : "-");
	} else {
		snprintf(line, sizeof(line), "hakios: %s: openpty: failed: %s\n", who, strerror(errno));
	}
	say(line, strlen(line));
	return n;
}

/*
 * login starts and says nothing at all (p436: "hakios: login: started as uid 501", and then the pane
 * says "Session Ended").  Whether it exits (and with what) or tries to exec the shell is the
 * question, so both are said out loud.
 */
static void
watched_exit(int status)
{
	char line[200];
	int n = snprintf(line, sizeof(line), "hakios: %s: exit(%d)\n", who, status);

	say(line, (size_t)(n > 0 ? n : 0));
	exit(status);
}

static void
watched__exit(int status)
{
	char line[200];
	int n = snprintf(line, sizeof(line), "hakios: %s: _exit(%d)\n", who, status);

	say(line, (size_t)(n > 0 ? n : 0));
	_exit(status);
}

static int
watched_execve(const char *path, char *const argv[], char *const envp[])
{
	char line[300];
	int n = snprintf(line, sizeof(line), "hakios: %s: execve %s\n", who, path != NULL ? path : "?");
	int r;

	say(line, (size_t)(n > 0 ? n : 0));
	r = execve(path, argv, envp);
	n = snprintf(line, sizeof(line), "hakios: %s: execve %s: %s\n", who, path != NULL ? path : "?", strerror(errno));
	say(line, (size_t)(n > 0 ? n : 0));
	return r;
}

static const char *
state_name(int state)
{
	switch (state) {
	case 1: return "idle";
	case 2: return "running";
	case 3: return "sleeping";
	case 4: return "stopped";
	case 5: return "zombie";
	default: return "?";
	}
}

static void *
children_report(__unused void *argument)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	struct kinfo_proc *procs;
	size_t need = 0;
	char line[400];
	int self = getpid(), count, mine = 0;

	sleep(25);
	if (sysctl(mib, 3, NULL, &need, NULL, 0) != 0 || need == 0) {
		snprintf(line, sizeof(line), "hakios: %s: no process list: %s\n", who, strerror(errno));
		say(line, strlen(line));
		return NULL;
	}
	procs = malloc(need);
	if (procs == NULL) {
		return NULL;
	}
	if (sysctl(mib, 3, procs, &need, NULL, 0) != 0) {
		free(procs);
		return NULL;
	}
	count = (int)(need / sizeof(*procs));
	for (int i = 0; i < count; i++) {
		if (procs[i].kp_eproc.e_ppid != self) {
			continue;
		}
		mine++;
		snprintf(line, sizeof(line), "hakios: %s: child %s[%d]: %s (state %d), uid %d, tty %d\n", who,
		    procs[i].kp_proc.p_comm, procs[i].kp_proc.p_pid, state_name(procs[i].kp_proc.p_stat),
		    procs[i].kp_proc.p_stat, (int)procs[i].kp_eproc.e_ucred.cr_uid, (int)procs[i].kp_eproc.e_tdev);
		say(line, strlen(line));
	}
	snprintf(line, sizeof(line), "hakios: %s: %d children of %d processes\n", who, mine, count);
	say(line, strlen(line));
	free(procs);
	return NULL;
}

/*
 * A file the application moves or copies itself (NSFileManager, under Swift's FileManager), when it
 * fails: from where, to where, and what the error says. Sileo moves each download into place this
 * way, and says only "The operation couldn't be completed. No such file or directory".
 */
static const char *
text_of(id object)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id text;

	if (object == NULL) {
		return "(nil)";
	}
	text = (class_respondsToSelector(object_getClass(object), sel_registerName("path")) ?
	    get(object, sel_registerName("path")) : get(object, sel_registerName("description")));
	return text != NULL ? utf8(text, sel_registerName("UTF8String")) : "?";
}

static signed char (*move_url)(id, SEL, id, id, id *), (*copy_url)(id, SEL, id, id, id *);
static signed char (*move_path)(id, SEL, id, id, id *), (*copy_path)(id, SEL, id, id, id *);

static signed char
file_said(const char *what, signed char ok, id from, id to, id error)
{
	if (!ok) {
		char line[1400];
		int n = snprintf(line, sizeof(line), "hakios: %s: %s %s -> %s failed: %s\n", who, what, text_of(from),
		    text_of(to), error != NULL ? text_of(error) : "(no error given)");

		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
	return ok;
}

static signed char
watched_move_url(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = move_url(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("moveItemAtURL", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_copy_url(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = copy_url(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("copyItemAtURL", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_move_path(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = move_path(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("moveItemAtPath", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_copy_path(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = copy_path(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("copyItemAtPath", ok, from, to, error != NULL ? *error : mine);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} spawn_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)watched_spawn, (const void *)posix_spawn
};

struct {
	const void *replacement, *original;
} openpty_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)watched_openpty, (const void *)openpty
};

struct {
	const void *replacement, *original;
} exit_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)watched_exit, (const void *)exit
};

struct {
	const void *replacement, *original;
} _exit_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)watched__exit, (const void *)_exit
};

struct {
	const void *replacement, *original;
} execve_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)watched_execve, (const void *)execve
};

/* Put ours in the place of a method, keeping the one that was there. */
static void
watch(const char *name, const char *selector, IMP ours, void *theirs)
{
	Class class = objc_getClass(name);
	Method method = class != NULL ? class_getInstanceMethod(class, sel_registerName(selector)) : NULL;

	if (method != NULL && method_getImplementation(method) != ours) {
		*(IMP *)theirs = method_setImplementation(method, ours);
	}
}

/*
 * Every storage or settings plugin the application loads, said out loud.  Settings -> General ->
 * iPhone Storage asks each installed application's usage plugin for its size, and the Podcasts
 * one (PodcastsUsagePlugin, built on PodcastsUI) traps in this port.  A bundle that is hidden
 * behind an empty memory filesystem is one that never appears here; a plugin that does appear
 * here despite being hidden is the thing to explain next.
 */
static void
image_added(const struct mach_header *header, intptr_t added)
{
	uint32_t i, count = _dyld_image_count();

	(void)added;
	for (i = 0; i < count; i++) {
		const char *name;

		if (_dyld_get_image_header(i) != header) {
			continue;
		}
		name = _dyld_get_image_name(i);
		if (name != NULL && (strstr(name, "UsagePlugin") != NULL || strstr(name, "Podcasts") != NULL ||
		    strstr(name, "StorageSettings") != NULL)) {
			char line[400];
			int n = snprintf(line, sizeof(line), "hakios: %s: loaded %s\n", who, name);

			say(line, (size_t)(n > 0 ? n : 0));
		}
		return;
	}
}

/*
 * The photo library, as it is on disk when the application starts: every entry of PhotoData with
 * its size, and whether a file can be made there.  Photos asserts that its data source has one
 * section and dies at the start; a new library is made by assetsd, which in this port starts
 * after Photos has already died, so what is on disk at each start is what says whether the second
 * start has a library to open.
 */
static void
say_directory(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *entry;
	char line[900];
	int n;

	if (stat(path, &st) != 0) {
		n = snprintf(line, sizeof(line), "hakios: %s: %s: %s\n", who, path, strerror(errno));
		say(line, (size_t)(n > 0 ? n : 0));
		return;
	}
	n = snprintf(line, sizeof(line), "hakios: %s: %s is uid %d gid %d mode 0%o:\n", who, path,
	    (int)st.st_uid, (int)st.st_gid, (unsigned)st.st_mode & 07777);
	say(line, (size_t)(n > 0 ? n : 0));
	if ((dir = opendir(path)) == NULL) {
		n = snprintf(line, sizeof(line), "hakios: %s: opendir: %s\n", who, strerror(errno));
		say(line, (size_t)(n > 0 ? n : 0));
		return;
	}
	while ((entry = readdir(dir)) != NULL) {
		char at[700];
		long long size = -1;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		snprintf(at, sizeof(at), "%s/%s", path, entry->d_name);
		if (stat(at, &st) == 0) {
			size = (long long)st.st_size;
		}
		n = snprintf(line, sizeof(line), "hakios: %s:   %s %lld\n", who, entry->d_name, size);
		say(line, (size_t)(n > 0 ? n : 0));
	}
	closedir(dir);
	{
		char at[700];
		int fd;

		snprintf(at, sizeof(at), "%s/.hakios-write", path);
		fd = open(at, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		n = snprintf(line, sizeof(line), "hakios: %s: writing %s: %s\n", who, at,
		    fd >= 0 ? "ok" : strerror(errno));
		say(line, (size_t)(n > 0 ? n : 0));
		if (fd >= 0) {
			close(fd);
			unlink(at);
		}
	}
}

static bool
is_shell(const char *name)
{
	static const char *const shells[] = { "sh", "dash", "bash", "zsh", "ksh" };

	for (unsigned i = 0; i < sizeof(shells) / sizeof(shells[0]); i++) {
		if (strcmp(name, shells[i]) == 0) {
			return true;
		}
	}
	return false;
}

__attribute__((constructor))
static void
hakios_app_init(void)
{
	static const int signals[] = { SIGTRAP, SIGABRT, SIGILL, SIGSEGV, SIGBUS, SIGSYS };
	struct sigaction action = { 0 };
	const char *name = getprogname();
	char line[200];
	size_t length = 0;
	int n;

	if (name == NULL) {
		return;
	}
	who = name;
	_dyld_register_func_for_add_image(image_added);
	if (strcmp(name, "MobileSlideShow") == 0 || strcmp(name, "Photos") == 0) {
		struct statfs space;

		say_directory("/private/var/mobile/Media/PhotoData");
		/*
		 * What the kernel says about the volume, in the layout this program reads: the numbers
		 * are what SQLite decides its locking on (and what once came out shifted by one field).
		 */
		if (statfs("/private/var", &space) == 0) {
			n = snprintf(line, sizeof(line),
			    "hakios: %s: /private/var: bsize %u iosize %d blocks %llu bfree %llu bavail %llu "
			    "files %llu ffree %llu type %s flags %#x\n", who, (unsigned)space.f_bsize,
			    (int)space.f_iosize, (unsigned long long)space.f_blocks,
			    (unsigned long long)space.f_bfree, (unsigned long long)space.f_bavail,
			    (unsigned long long)space.f_files, (unsigned long long)space.f_ffree,
			    space.f_fstypename, (unsigned)space.f_flags);
			say(line, (size_t)(n > 0 ? n : 0));
		} else {
			n = snprintf(line, sizeof(line), "hakios: %s: statfs /private/var: %s\n", who,
			    strerror(errno));
			say(line, (size_t)(n > 0 ? n : 0));
		}
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	/*
	 * An application's standard error goes nowhere here, and that is where the Swift runtime says
	 * what it is about to die of ("Fatal error: ..."), so it is pointed at the console.
	 */
	console = open("/dev/console", O_WRONLY);
	/*
	 * Not for a shell: dash writes its prompt to standard error, so a shell whose 2 is the console
	 * prints its prompt where nobody looks (p441: two shells started, both said their line, and the
	 * pane stayed empty).  The console is for the applications' last words, not for a terminal's.
	 */
	if (console >= 0 && !is_shell(name)) {
		dup2(console, 2);
	}
	action.sa_sigaction = killed;
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &action, NULL);
	}
	if (strcmp(name, "Sileo") == 0) {
		dispatch_async(dispatch_get_main_queue(), ^{ keep_to_the_repositories(); });
	}
	if (strcmp(name, "NewTerm") == 0) {
		pthread_t thread;

		pthread_create(&thread, NULL, children_report, NULL);
		pthread_detach(thread);
	}
	watch("UIApplication", "beginBackgroundTaskWithExpirationHandler:", (IMP)watched_begin_task, &begin_task);
	/*
	 * NSURLSessionTask alone: its subclasses inherit the very same method, and watching it twice
	 * would leave ours as the "original" of itself -- a call that never returns.
	 */
	watch("NSURLSessionTask", "resume", (IMP)watched_resume_task, &resume_task);
	watch("NSFileManager", "moveItemAtURL:toURL:error:", (IMP)watched_move_url, &move_url);
	watch("NSFileManager", "copyItemAtURL:toURL:error:", (IMP)watched_copy_url, &copy_url);
	watch("NSFileManager", "moveItemAtPath:toPath:error:", (IMP)watched_move_path, &move_path);
	watch("NSFileManager", "copyItemAtPath:toPath:error:", (IMP)watched_copy_path, &copy_path);
	n = snprintf(line, sizeof(line), "hakios: %s: started as uid %d (effective %d), watching for what kills it\n",
	    name, (int)getuid(), (int)geteuid());
	say(line, (size_t)(n > 0 ? n : 0));
	/*
	 * The shell runs on NewTerm's pty and the pane stays empty (p439: the helper ran, the shell is
	 * "dash" as uid 501, its tty is 0x10000000 and it even exec'd /usr/bin/dircolors from the
	 * profile).  So: is a pty really on 0 and 1, and does a byte written to 1 come out in the pane?
	 */
	if (getenv("HAKIOS_TTY_TEST") != NULL &&
	    (strcmp(name, "sh") == 0 || strcmp(name, "zsh") == 0 || strcmp(name, "dash") == 0)) {
		const char *tty = ttyname(0);
		int n2 = snprintf(line, sizeof(line),
		    "hakios: %s: tty 0: %s (a terminal: %d), tty 1: %d; process group %d, the terminal's %d\n", name,
		    tty != NULL ? tty : "none", isatty(0), isatty(1), (int)getpgrp(), (int)tcgetpgrp(0));
		const char hello[] = "hakios: a line written to the terminal by the shell\r\n";

		say(line, (size_t)(n2 > 0 ? n2 : 0));
		(void)write(1, hello, sizeof(hello) - 1);
	}
}
