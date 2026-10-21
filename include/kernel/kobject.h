#ifndef XV6_KOBJECT_H
#define XV6_KOBJECT_H

// The kernel object model (compare Linux lib/kobject.c and
// include/linux/kobject.h).
//
// Linux sysfs is not a filesystem that reads data structures: it is one
// object tree, and every kernel subsystem hangs its state off that tree.
// The single node type is struct kobject -- a named object with a parent, a
// reference count, and a kobj_type that says how it renders itself into
// sysfs (a set of attribute files plus the show/store hooks that back them).
// bus_type, device and device_driver in drivers/base are not special: each
// one *embeds* a kobject and supplies a kobj_type, so /sys/bus, /sys/devices
// and the driver directories all fall out of the same generic walker.
//
// This port keeps the same shape.  Nothing in kobject.c knows about devices
// or buses; it only manages named objects, parent/child links, a reference
// count, and the attribute interface a kobj_type advertises.  The device
// model (device.c) embeds these structs; the sysfs pseudo-filesystem
// (vfs.c) walks the tree they form.  Together they reproduce Linux's
// separation: kobject is the mechanism, sysfs is one renderer of it, and the
// driver core is one set of users.

struct kobject;
struct kset;
struct kobj_type;

#define KOBJ_NAME_MAX   32   // length of one sysfs directory/component name
#define KOBJ_MAX       128   // kobjects registered from elsewhere (embedded)
#define KOBJ_SYNTH_MAX  48   // directories created by kobject_create_and_add

// A sysfs attribute file.  `mode` carries the access bits in the Linux
// convention (S_IRUSR/S_IWUSR folded into the low octal digits): the sysfs
// walker shows a file and only routes write(2) to store() when it is
// writable.  0444 read-only, 0644 writable.
struct attribute {
  char *name;
  int   mode;
};

// A kobject renders its attributes through its ktype's sysfs_ops, exactly as
// Linux's kernfs routes to `struct sysfs_ops`.  show() formats the attribute
// into buf (capacity `size`) and returns the byte count, mirroring the
// scnprintf() contract of a real _store/_show pair; store() consumes the text
// written to the file and returns bytes used, or -1 on error.
struct sysfs_ops {
  int (*show)(struct kobject *kobj, struct attribute *attr, char *buf, int size);
  int (*store)(struct kobject *kobj, struct attribute *attr, char *buf, int size);
};

// The type of a kobject: how it is released and how it appears in sysfs.
// Every kobject of one kind (all platform devices, all drivers) shares a
// kobj_type; per-object attribute sets hang off the kobject itself.
struct kobj_type {
  void (*release)(struct kobject *kobj);
  struct sysfs_ops *sysfs_ops;
  struct attribute **default_attrs;
};

// A kobject is one node of the device tree.  `parent`/child links give the
// sysfs path; `kset` groups siblings (Linux: the subsystem or bus a device
// belongs to); `ktype` says how to render it; `attrs` optionally overrides
// ktype->default_attrs for this instance.  `refcount` is Linux's kref,
// inlined: an object is released only when its last user drops it.
struct kobject {
  int   used;                 // slot in the registry, not a Linux field
  char  name[KOBJ_NAME_MAX];
  struct kobject *parent;
  struct kset    *kset;
  struct kobj_type *ktype;
  struct attribute **attrs;   // per-instance attributes, or 0 to use ktype's
  struct kobject *link;       // non-NULL => this node is a sysfs symlink to `link`
  int   refcount;
  void  *priv;                // back-pointer to the embedding struct
};

// A kset is a kobject that holds a set of other kobjects (Linux: the
// "devices" and "drivers" directories, and each bus/class).  `subsys` points
// at the kset this one is a member of, giving the two-level bus topology.
struct kset {
  struct kobject kobj;
  struct kset    *subsys;
  struct kobj_type *ktype;
};

// -- core lifecycle --------------------------------------------------------
void  kobject_init(struct kobject *kobj, struct kobj_type *ktype);
int   kobject_set_name(struct kobject *kobj, char *name);
// Link kobj under parent (NULL for a tree root) and register it.  name may be
// NULL if kobject_set_name() already gave it one.  Returns 0 or -1.
int   kobject_add(struct kobject *kobj, struct kobject *parent, char *name);
void  kobject_del(struct kobject *kobj);
struct kobject *kobject_create_and_add(char *name, struct kobject *parent);

// Sysfs symlink (Linux sysfs_create_link).  A link is a kobject that appears
// as a directory entry named `name` under `parent` but holds no state of its
// own: it points at `target`, another kobject in the same tree.  This is how
// /sys/class/<class>/<device> and the per-device subsystem/driver entries are
// built -- one object reached by many paths.  kobject_link_add takes a
// reference on `target` for the life of the link.  Returns the link kobject
// or 0.  kobject_follow_link resolves a node to what it points at (itself if
// it is not a link), the walker's dereference step.
struct kobject *kobject_link_add(char *name, struct kobject *parent,
                                 struct kobject *target);
struct kobject *kobject_follow_link(struct kobject *kobj);

// Reference counting (Linux kobject_get/kobject_put over kref_get/put).
// get() returns the object so it can wrap an assignment.  put() drops one
// reference and, at zero, calls ktype->release().
struct kobject *kobject_get(struct kobject *kobj);
void            kobject_put(struct kobject *kobj);

// -- tree accessors for the sysfs walker ----------------------------------
struct kobject *kobject_parent(struct kobject *kobj);
char           *kobject_name_of(struct kobject *kobj);
// Nth child (registered kobject whose parent is kobj), or 0 past the end.
struct kobject *kobject_child_at(struct kobject *kobj, int index);
// Child kobject by directory name, or 0.
struct kobject *kobject_child_by_name(struct kobject *kobj, char *name);
// Number of children.
int             kobject_child_count(struct kobject *kobj);
// Nth attribute this object exposes (per-instance attrs, else ktype default),
// or 0 past the end.
struct attribute *kobject_attr_at(struct kobject *kobj, int index);
// Number of attribute files this object exposes.
int              kobject_attr_count(struct kobject *kobj);
// Look up one attribute by name, or 0.
struct attribute *kobject_find_attr(struct kobject *kobj, char *name);
// Render the sysfs path "/a/b/c" of kobj into buf; returns buf or 0 on too-long.
char *kobject_path(struct kobject *kobj, char *buf, int size);

// -- kset / subsystem registration ----------------------------------------
void  kset_init(struct kset *kset, struct kobj_type *ktype, char *name);
int   kset_register(struct kset *kset, struct kobject *parent);
void  kset_unregister(struct kset *kset);
// Linux subsys_system_register(): put a bus/class kset on the tree.
int   subsys_register(struct kset *kset, struct kobj_type *ktype, char *name,
                      struct kobject *parent);

// The "/" kobject of the sysfs tree, created on first use.  NULL parent.
struct kobject *kobject_sys_root(void);

// One-time setup: builds the sysfs root.  Call before any driver registers.
void kobject_subsys_init(void);

#endif
