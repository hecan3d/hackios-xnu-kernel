#ifndef HAKIOS_COLLECTION_SAFETY_H
#define HAKIOS_COLLECTION_SAFETY_H

#include <objc/runtime.h>
#include <objc/message.h>
#include <alloca.h>

static void say(const char *format, ...);

static IMP
hook_method_safely(const char *class_name, const char *sel_name, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	if (cls == NULL) {
		say("hakios: collection_safety: class %s not found\n", class_name);
		return NULL;
	}
	SEL sel = sel_registerName(sel_name);
	Method m = class_getInstanceMethod(cls, sel);
	if (m == NULL) {
		say("hakios: collection_safety: method -[%s %s] not found\n", class_name, sel_name);
		return NULL;
	}
	IMP orig = method_getImplementation(m);
	method_setImplementation(m, replacement);
	say("hakios: collection_safety: hooked -[%s %s]\n", class_name, sel_name);
	return orig;
}

/* __NSSetM */
static void (*orig_NSSetM_removeObject)(id self, SEL _cmd, id object);
static void
hook_NSSetM_removeObject(id self, SEL _cmd, id object)
{
	if (object == nil) {
		say("hakios: %s[%d]: caught -[__NSSetM removeObject:nil]\n", getprogname(), getpid());
		return;
	}
	orig_NSSetM_removeObject(self, _cmd, object);
}

static void (*orig_NSSetM_addObject)(id self, SEL _cmd, id object);
static void
hook_NSSetM_addObject(id self, SEL _cmd, id object)
{
	if (object == nil) {
		say("hakios: %s[%d]: caught -[__NSSetM addObject:nil]\n", getprogname(), getpid());
		return;
	}
	orig_NSSetM_addObject(self, _cmd, object);
}

/* __NSDictionaryM */
static void (*orig_NSDictionaryM_removeObjectForKey)(id self, SEL _cmd, id key);
static void
hook_NSDictionaryM_removeObjectForKey(id self, SEL _cmd, id key)
{
	if (key == nil) {
		say("hakios: %s[%d]: caught -[__NSDictionaryM removeObjectForKey:nil]\n", getprogname(), getpid());
		return;
	}
	orig_NSDictionaryM_removeObjectForKey(self, _cmd, key);
}

static void (*orig_NSDictionaryM_setObject_forKey)(id self, SEL _cmd, id object, id key);
static void
hook_NSDictionaryM_setObject_forKey(id self, SEL _cmd, id object, id key)
{
	if (key == nil) {
		say("hakios: %s[%d]: caught -[__NSDictionaryM setObject:%p forKey:nil]\n", getprogname(), getpid(), object);
		return;
	}
	if (object == nil) {
		say("hakios: %s[%d]: caught -[__NSDictionaryM setObject:nil forKey:%p] -> removeObjectForKey\n", getprogname(), getpid(), key);
		if (orig_NSDictionaryM_removeObjectForKey != NULL) {
			orig_NSDictionaryM_removeObjectForKey(self, sel_registerName("removeObjectForKey:"), key);
		}
		return;
	}
	orig_NSDictionaryM_setObject_forKey(self, _cmd, object, key);
}

static void (*orig_NSDictionaryM_setObject_forKeyedSubscript)(id self, SEL _cmd, id object, id key);
static void
hook_NSDictionaryM_setObject_forKeyedSubscript(id self, SEL _cmd, id object, id key)
{
	if (key == nil) {
		say("hakios: %s[%d]: caught -[__NSDictionaryM setObject:%p forKeyedSubscript:nil]\n", getprogname(), getpid(), object);
		return;
	}
	if (object == nil) {
		if (orig_NSDictionaryM_removeObjectForKey != NULL) {
			orig_NSDictionaryM_removeObjectForKey(self, sel_registerName("removeObjectForKey:"), key);
		}
		return;
	}
	if (orig_NSDictionaryM_setObject_forKeyedSubscript != NULL) {
		orig_NSDictionaryM_setObject_forKeyedSubscript(self, _cmd, object, key);
	} else if (orig_NSDictionaryM_setObject_forKey != NULL) {
		orig_NSDictionaryM_setObject_forKey(self, sel_registerName("setObject:forKey:"), object, key);
	}
}

/* __NSArrayM */
static void (*orig_NSArrayM_addObject)(id self, SEL _cmd, id object);
static void
hook_NSArrayM_addObject(id self, SEL _cmd, id object)
{
	if (object == nil) {
		say("hakios: %s[%d]: caught -[__NSArrayM addObject:nil]\n", getprogname(), getpid());
		return;
	}
	orig_NSArrayM_addObject(self, _cmd, object);
}

static void (*orig_NSArrayM_insertObject_atIndex)(id self, SEL _cmd, id object, unsigned long index);
static void
hook_NSArrayM_insertObject_atIndex(id self, SEL _cmd, id object, unsigned long index)
{
	if (object == nil) {
		say("hakios: %s[%d]: caught -[__NSArrayM insertObject:nil atIndex:%lu]\n", getprogname(), getpid(), index);
		return;
	}
	orig_NSArrayM_insertObject_atIndex(self, _cmd, object, index);
}

/* Placeholders (collection literals) */
static id (*orig_NSPlaceholderDictionary_initWithObjects_forKeys_count)(id self, SEL _cmd, const id *objects, const id *keys, unsigned long count);
static id
hook_NSPlaceholderDictionary_initWithObjects_forKeys_count(id self, SEL _cmd, const id *objects, const id *keys, unsigned long count)
{
	if (count > 0 && (objects == NULL || keys == NULL)) {
		say("hakios: %s[%d]: initWithObjects:forKeys:count:%lu with NULL array, returning empty dict\n", getprogname(), getpid(), count);
		return orig_NSPlaceholderDictionary_initWithObjects_forKeys_count(self, _cmd, NULL, NULL, 0);
	}
	BOOL has_nil = NO;
	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] == nil || keys[i] == nil) {
			has_nil = YES;
			break;
		}
	}
	if (!has_nil) {
		return orig_NSPlaceholderDictionary_initWithObjects_forKeys_count(self, _cmd, objects, keys, count);
	}
	say("hakios: %s[%d]: caught -[__NSPlaceholderDictionary initWithObjects:forKeys:count:%lu] with nil, filtering\n",
	    getprogname(), getpid(), count);
	id *filtered_objs = (id *)alloca(count * sizeof(id));
	id *filtered_keys = (id *)alloca(count * sizeof(id));
	unsigned long filtered_count = 0;
	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] != nil && keys[i] != nil) {
			filtered_objs[filtered_count] = objects[i];
			filtered_keys[filtered_count] = keys[i];
			filtered_count++;
		}
	}
	return orig_NSPlaceholderDictionary_initWithObjects_forKeys_count(self, _cmd, filtered_objs, filtered_keys, filtered_count);
}

static id (*orig_NSPlaceholderArray_initWithObjects_count)(id self, SEL _cmd, const id *objects, unsigned long count);
static id
hook_NSPlaceholderArray_initWithObjects_count(id self, SEL _cmd, const id *objects, unsigned long count)
{
	if (count > 0 && objects == NULL) {
		return orig_NSPlaceholderArray_initWithObjects_count(self, _cmd, NULL, 0);
	}
	BOOL has_nil = NO;
	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] == nil) {
			has_nil = YES;
			break;
		}
	}
	if (!has_nil) {
		return orig_NSPlaceholderArray_initWithObjects_count(self, _cmd, objects, count);
	}
	say("hakios: %s[%d]: caught -[__NSPlaceholderArray initWithObjects:count:%lu] with nil, filtering\n",
	    getprogname(), getpid(), count);
	id *filtered_objs = (id *)alloca(count * sizeof(id));
	unsigned long filtered_count = 0;
	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] != nil) {
			filtered_objs[filtered_count] = objects[i];
			filtered_count++;
		}
	}
	return orig_NSPlaceholderArray_initWithObjects_count(self, _cmd, filtered_objs, filtered_count);
}

static id (*orig_NSPlaceholderSet_initWithObjects_count)(id self, SEL _cmd, const id *objects, unsigned long count);
static id
hook_NSPlaceholderSet_initWithObjects_count(id self, SEL _cmd, const id *objects, unsigned long count)
{
	if (count > 0 && objects == NULL) {
		return orig_NSPlaceholderSet_initWithObjects_count(self, _cmd, NULL, 0);
	}
	BOOL has_nil = NO;
	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] == nil) {
			has_nil = YES;
			break;
		}
	}
	if (!has_nil) {
		return orig_NSPlaceholderSet_initWithObjects_count(self, _cmd, objects, count);
	}
	say("hakios: %s[%d]: caught -[__NSPlaceholderSet initWithObjects:count:%lu] with nil, filtering\n",
	    getprogname(), getpid(), count);
	id *filtered_objs = (id *)alloca(count * sizeof(id));
	unsigned long filtered_count = 0;
	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] != nil) {
			filtered_objs[filtered_count] = objects[i];
			filtered_count++;
		}
	}
	return orig_NSPlaceholderSet_initWithObjects_count(self, _cmd, filtered_objs, filtered_count);
}

static void
install_collection_safety_hooks(void)
{
	orig_NSSetM_removeObject = (void (*)(id, SEL, id))hook_method_safely("__NSSetM", "removeObject:", (IMP)hook_NSSetM_removeObject);
	orig_NSSetM_addObject = (void (*)(id, SEL, id))hook_method_safely("__NSSetM", "addObject:", (IMP)hook_NSSetM_addObject);
	orig_NSDictionaryM_removeObjectForKey = (void (*)(id, SEL, id))hook_method_safely("__NSDictionaryM", "removeObjectForKey:", (IMP)hook_NSDictionaryM_removeObjectForKey);
	orig_NSDictionaryM_setObject_forKey = (void (*)(id, SEL, id, id))hook_method_safely("__NSDictionaryM", "setObject:forKey:", (IMP)hook_NSDictionaryM_setObject_forKey);
	orig_NSDictionaryM_setObject_forKeyedSubscript = (void (*)(id, SEL, id, id))hook_method_safely("__NSDictionaryM", "setObject:forKeyedSubscript:", (IMP)hook_NSDictionaryM_setObject_forKeyedSubscript);
	orig_NSArrayM_addObject = (void (*)(id, SEL, id))hook_method_safely("__NSArrayM", "addObject:", (IMP)hook_NSArrayM_addObject);
	orig_NSArrayM_insertObject_atIndex = (void (*)(id, SEL, id, unsigned long))hook_method_safely("__NSArrayM", "insertObject:atIndex:", (IMP)hook_NSArrayM_insertObject_atIndex);
	orig_NSPlaceholderDictionary_initWithObjects_forKeys_count = (id (*)(id, SEL, const id *, const id *, unsigned long))hook_method_safely("__NSPlaceholderDictionary", "initWithObjects:forKeys:count:", (IMP)hook_NSPlaceholderDictionary_initWithObjects_forKeys_count);
	orig_NSPlaceholderArray_initWithObjects_count = (id (*)(id, SEL, const id *, unsigned long))hook_method_safely("__NSPlaceholderArray", "initWithObjects:count:", (IMP)hook_NSPlaceholderArray_initWithObjects_count);
	orig_NSPlaceholderSet_initWithObjects_count = (id (*)(id, SEL, const id *, unsigned long))hook_method_safely("__NSPlaceholderSet", "initWithObjects:count:", (IMP)hook_NSPlaceholderSet_initWithObjects_count);
}

#endif /* HAKIOS_COLLECTION_SAFETY_H */
