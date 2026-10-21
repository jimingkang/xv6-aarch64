// Kernel object model: named objects, parent/child links, reference counts,
// and the attribute interface a kobj_type advertises (compare Linux
// lib/kobject.c).  This file knows nothing about devices, buses or sysfs --
// it is the mechanism the driver core (device.c) hangs objects on and the
// only thing the sysfs renderer (vfs.c) walks.  See kobject.h for the model.

#include <kernel/types.h>
#include <kernel/param.h>
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/kobject.h>
#include <kernel/defs.h>

static struct {
  struct spinlock lock;
  struct kobject *table[KOBJ_MAX];        // registered objects, slot order kept
  struct kobject  synth[KOBJ_SYNTH_MAX];  // pool for kobject_create_and_add
  struct kobject  sys_root;               // the "" node that maps to /sys
  int             root_ready;
} kobj_core;

// Directories the core creates for itself carry no attributes and are never
// freed (they live in .bss), so release is a no-op and there is no sysfs_ops.
static void
null_release(struct kobject *kobj)
{
  // A created-and-added directory has no owner to notify; dropping the last
  // reference just makes the slot reusable, which kobject_put() already did.
  (void) kobj;
}

static struct kobj_type empty_ktype = {
  .release = null_release,
  .sysfs_ops = 0,
  .default_attrs = 0,
};

// -- internal, lock-free helpers (callers hold kobj_core.lock) ------------

static struct kobject *
find_slot(struct kobject *kobj)
{
  for(int i = 0; i < KOBJ_MAX; i++)
    if(kobj_core.table[i] == kobj)
      return kobj;
  return 0;
}

static void
grab(struct kobject *kobj)             // +1 ref, caller holds the lock
{
  if(kobj && kobj->refcount < 0x7fffffff)
    kobj->refcount++;
}

// -- lifecycle ------------------------------------------------------------

void
kobject_init(struct kobject *kobj, struct kobj_type *ktype)
{
  acquire(&kobj_core.lock);
  kobj->used = 0;
  kobj->name[0] = 0;
  kobj->parent = 0;
  kobj->kset = 0;
  kobj->attrs = 0;
  kobj->ktype = ktype;
  kobj->refcount = 1;               // Linux kref_init: the owner holds one.
  release(&kobj_core.lock);
}

int
kobject_set_name(struct kobject *kobj, char *name)
{
  if(name == 0)
    return -1;
  acquire(&kobj_core.lock);
  safestrcpy(kobj->name, name, sizeof(kobj->name));
  release(&kobj_core.lock);
  return 0;
}

int
kobject_add(struct kobject *kobj, struct kobject *parent, char *name)
{
  if(kobj == 0)
    return -1;
  acquire(&kobj_core.lock);
  if(name && name[0])
    safestrcpy(kobj->name, name, sizeof(kobj->name));
  if(kobj->name[0] == 0 || kobj->ktype == 0 || find_slot(kobj)){
    release(&kobj_core.lock);
    return -1;
  }
  // A kobject born into a kset lands under that kset's directory when no
  // explicit parent was given (Linux kobject_add with kobj->kset).
  if(parent == 0 && kobj->kset)
    parent = &kobj->kset->kobj;
  for(int i = 0; i < KOBJ_MAX; i++){
    if(kobj_core.table[i] == 0){
      kobj_core.table[i] = kobj;
      break;
    }
    if(i == KOBJ_MAX - 1){
      release(&kobj_core.lock);
      return -1;
    }
  }
  kobj->used = 1;
  kobj->parent = parent;
  grab(parent);                    // being in the tree pins the parent alive
  release(&kobj_core.lock);
  return 0;
}

void
kobject_del(struct kobject *kobj)
{
  struct kobject *parent, *target;
  if(kobj == 0)
    return;
  acquire(&kobj_core.lock);
  parent = kobj->parent;
  target = kobj->link;
  if(find_slot(kobj)){
    for(int i = 0; i < KOBJ_MAX; i++)
      if(kobj_core.table[i] == kobj){
        kobj_core.table[i] = 0;
        break;
      }
    kobj->used = 0;
    kobj->parent = 0;
    kobj->link = 0;
    grab(parent);                  // undo the tree reference below
  }
  release(&kobj_core.lock);
  if(target)
    kobject_put(target);           // release the reference the link pinned
  if(parent)
    kobject_put(parent);
}

struct kobject*
kobject_create_and_add(char *name, struct kobject *parent)
{
  struct kobject *kobj = 0;
  acquire(&kobj_core.lock);
  for(int i = 0; i < KOBJ_SYNTH_MAX; i++)
    if(!kobj_core.synth[i].used){
      kobj = &kobj_core.synth[i];
      break;
    }
  if(kobj == 0){
    release(&kobj_core.lock);
    return 0;
  }
  memset(kobj, 0, sizeof(*kobj));
  kobj->used = 1;
  kobj->ktype = &empty_ktype;
  kobj->refcount = 1;
  if(name)
    safestrcpy(kobj->name, name, sizeof(kobj->name));
  // Place it now that the identity is set; table slots stay lock-guarded.
  int placed = -1;
  for(int i = 0; i < KOBJ_MAX; i++)
    if(kobj_core.table[i] == 0){
      kobj_core.table[i] = kobj;
      placed = i;
      break;
    }
  if(placed < 0){
    kobj->used = 0;
    release(&kobj_core.lock);
    return 0;
  }
  kobj->parent = parent;
  grab(parent);                    // being in the tree pins the parent alive
  release(&kobj_core.lock);
  return kobj;
}

struct kobject*
kobject_link_add(char *name, struct kobject *parent, struct kobject *target)
{
  struct kobject *kobj;
  if(name == 0 || parent == 0 || target == 0)
    return 0;
  // Carve a synth node exactly like a created directory, then mark it a link
  // and pin the target: dropping the link must not leave a dangling arrow.
  if((kobj = kobject_create_and_add(name, parent)) == 0)
    return 0;
  kobj->link = target;
  kobject_get(target);
  return kobj;
}

struct kobject*
kobject_follow_link(struct kobject *kobj)
{
  return (kobj && kobj->link) ? kobj->link : kobj;
}

struct kobject*
kobject_get(struct kobject *kobj)
{
  if(kobj == 0)
    return 0;
  acquire(&kobj_core.lock);
  grab(kobj);
  release(&kobj_core.lock);
  return kobj;
}

void
kobject_put(struct kobject *kobj)
{
  void (*release_fn)(struct kobject *);
  struct kobject *parent;
  struct kobject *target;
  if(kobj == 0)
    return;
  acquire(&kobj_core.lock);
  if(kobj->refcount > 0)
    kobj->refcount--;
  if(kobj->refcount > 0){
    release(&kobj_core.lock);
    return;
  }
  release_fn = kobj->ktype ? kobj->ktype->release : 0;
  parent = kobj->parent;
  // Unlink before releasing so no walker sees a freed object.
  for(int i = 0; i < KOBJ_MAX; i++)
    if(kobj_core.table[i] == kobj){
      kobj_core.table[i] = 0;
      break;
    }
  kobj->used = 0;
  kobj->parent = 0;
  target = kobj->link;
  kobj->link = 0;
  release(&kobj_core.lock);
  if(release_fn)
    release_fn(kobj);
  if(target)
    kobject_put(target);           // a freed link drops its target
  if(parent)
    kobject_put(parent);
}

// -- tree accessors -------------------------------------------------------

struct kobject*
kobject_parent(struct kobject *kobj)
{
  return kobj ? kobj->parent : 0;
}

char*
kobject_name_of(struct kobject *kobj)
{
  return kobj ? kobj->name : 0;
}

int
kobject_child_count(struct kobject *kobj)
{
  int n = 0;
  if(kobj == 0)
    return 0;
  acquire(&kobj_core.lock);
  for(int i = 0; i < KOBJ_MAX; i++)
    if(kobj_core.table[i] && kobj_core.table[i]->parent == kobj)
      n++;
  release(&kobj_core.lock);
  return n;
}

struct kobject*
kobject_child_at(struct kobject *kobj, int index)
{
  struct kobject *found = 0;
  if(kobj == 0 || index < 0)
    return 0;
  acquire(&kobj_core.lock);
  for(int i = 0; i < KOBJ_MAX; i++)
    if(kobj_core.table[i] && kobj_core.table[i]->parent == kobj &&
       kobj_core.table[i] != kobj){
      if(index-- == 0){
        found = kobj_core.table[i];
        break;
      }
    }
  release(&kobj_core.lock);
  return found;
}

struct kobject*
kobject_child_by_name(struct kobject *kobj, char *name)
{
  struct kobject *found = 0;
  if(kobj == 0 || name == 0)
    return 0;
  acquire(&kobj_core.lock);
  for(int i = 0; i < KOBJ_MAX; i++)
    if(kobj_core.table[i] && kobj_core.table[i]->parent == kobj &&
       strncmp(kobj_core.table[i]->name, name, strlen(name) + 1) == 0){
      found = kobj_core.table[i];
      break;
    }
  release(&kobj_core.lock);
  return found;
}

// A kobject's attribute files are the concatenation of its type's
// default_attrs (shared by every object of that kind -- Linux's core device
// attributes such as "name", "modalias", "subsystem") and the instance list
// kobj->attrs (the attribute groups a particular driver registered).  The
// instance list adds to, rather than replaces, the defaults, as in Linux.
static struct attribute**
kobj_defaults(struct kobject *kobj)
{
  return kobj->ktype ? kobj->ktype->default_attrs : 0;
}

static struct attribute**
kobj_extras(struct kobject *kobj)
{
  return kobj->attrs;
}

static int
list_len(struct attribute **list)
{
  int n = 0;
  while(list && list[n])
    n++;
  return n;
}

struct attribute*
kobject_attr_at(struct kobject *kobj, int index)
{
  struct attribute **def, **ext;
  int n;
  if(kobj == 0 || index < 0)
    return 0;
  def = kobj_defaults(kobj);
  n = list_len(def);
  if(index < n)
    return def[index];
  ext = kobj_extras(kobj);
  index -= n;
  return index < list_len(ext) ? ext[index] : 0;
}

int
kobject_attr_count(struct kobject *kobj)
{
  if(kobj == 0)
    return 0;
  return list_len(kobj_defaults(kobj)) + list_len(kobj_extras(kobj));
}

struct attribute*
kobject_find_attr(struct kobject *kobj, char *name)
{
  struct attribute **def, **ext;
  if(kobj == 0 || name == 0)
    return 0;
  def = kobj_defaults(kobj);
  for(int i = 0; def && def[i]; i++)
    if(strncmp(def[i]->name, name, strlen(name) + 1) == 0)
      return def[i];
  ext = kobj_extras(kobj);
  for(int i = 0; ext && ext[i]; i++)
    if(strncmp(ext[i]->name, name, strlen(name) + 1) == 0)
      return ext[i];
  return 0;
}

char*
kobject_path(struct kobject *kobj, char *buf, int size)
{
  char *names[KOBJ_MAX];
  int n = 0, pos = 0, i;

  if(buf == 0 || size <= 1)
    return 0;
  for(struct kobject *k = kobj; k; k = k->parent){
    if(n >= KOBJ_MAX)
      return 0;
    names[n] = k->name;
    n++;
    if(k == &kobj_core.sys_root)     // the sysfs root renders as "", not "/sys"
      break;
  }
  buf[0] = 0;
  for(i = n - 1; i >= 0; i--){       // walk root -> leaf
    if(names[i][0] == 0)             // the root's empty name
      continue;
    if(pos + 1 + strlen(names[i]) + 1 > size)
      return 0;
    buf[pos++] = '/';
    for(char *c = names[i]; *c; )
      buf[pos++] = *c++;
    buf[pos] = 0;
  }
  if(pos == 0){
    buf[pos++] = '/';
    buf[pos] = 0;
  }
  return buf;
}

// -- kset / subsystem -----------------------------------------------------

void
kset_init(struct kset *kset, struct kobj_type *ktype, char *name)
{
  kobject_init(&kset->kobj, ktype ? ktype : &empty_ktype);
  kset->ktype = ktype;
  if(name)
    kobject_set_name(&kset->kobj, name);
}

int
kset_register(struct kset *kset, struct kobject *parent)
{
  if(kset == 0)
    return -1;
  return kobject_add(&kset->kobj, parent, 0);
}

void
kset_unregister(struct kset *kset)
{
  if(kset)
    kobject_del(&kset->kobj);
}

int
subsys_register(struct kset *kset, struct kobj_type *ktype, char *name,
                struct kobject *parent)
{
  if(kset == 0)
    return -1;
  kset_init(kset, ktype, name);
  return kset_register(kset, parent);
}

// -- sysfs root -----------------------------------------------------------

struct kobject*
kobject_sys_root(void)
{
  acquire(&kobj_core.lock);
  if(!kobj_core.root_ready){
    memset(&kobj_core.sys_root, 0, sizeof(kobj_core.sys_root));
    kobj_core.sys_root.used = 1;
    kobj_core.sys_root.ktype = &empty_ktype;
    kobj_core.sys_root.refcount = 1;
    kobj_core.sys_root.name[0] = 0;        // renders as "/"
    kobj_core.table[0] = &kobj_core.sys_root;
    kobj_core.root_ready = 1;
  }
  release(&kobj_core.lock);
  return &kobj_core.sys_root;
}

void
kobject_subsys_init(void)
{
  initlock(&kobj_core.lock, "kobject");
  memset(kobj_core.table, 0, sizeof(kobj_core.table));
  memset(kobj_core.synth, 0, sizeof(kobj_core.synth));
  kobj_core.root_ready = 0;
  kobject_sys_root();
}
