// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

/* [Feb 1997 T. Schoebel-Theuer] Complete rewrite of the pathname
 * lookup logic.
 */
/* [Feb-Apr 2000, AV] Rewrite to the new namespace architecture.
 */

#include <linux/init.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/wordpart.h>
#include <linux/fs.h>
#include <linux/filelock.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/sched/mm.h>
#include <linux/fsnotify.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/device_cgroup.h>
#include <linux/fs_struct.h>
#include <linux/posix_acl.h>
#include <linux/hash.h>
#include <linux/bitops.h>
#include <linux/init_task.h>
#include <linux/uaccess.h>

#include "internal.h"
#include "mount.h"

/* [Feb-1997 T. Schoebel-Theuer]
 * Fundamental changes in the pathname lookup mechanisms (namei)
 * were necessary because of omirr.  The reason is that omirr needs
 * to know the _real_ pathname, not the user-supplied one, in case
 * of symlinks (and also when transname replacements occur).
 *
 * The new code replaces the old recursive symlink resolution with
 * an iterative one (in case of non-nested symlink chains).  It does
 * this with calls to <fs>_follow_link().
 * As a side effect, dir_namei(), _namei() and follow_link() are now 
 * replaced with a single function lookup_dentry() that can handle all 
 * the special cases of the former code.
 *
 * With the new dcache, the pathname is stored at each inode, at least as
 * long as the refcount of the inode is positive.  As a side effect, the
 * size of the dcache depends on the inode cache and thus is dynamic.
 *
 * [29-Apr-1998 C. Scott Ananian] Updated above description of symlink
 * resolution to correspond with current state of the code.
 *
 * Note that the symlink resolution is not *completely* iterative.
 * There is still a significant amount of tail- and mid- recursion in
 * the algorithm.  Also, note that <fs>_readlink() is not used in
 * lookup_dentry(): lookup_dentry() on the result of <fs>_readlink()
 * may return different results than <fs>_follow_link().  Many virtual
 * filesystems (including /proc) exhibit this behavior.
 */

/* [24-Feb-97 T. Schoebel-Theuer] Side effects caused by new implementation:
 * New symlink semantics: when open() is called with flags O_CREAT | O_EXCL
 * and the name already exists in form of a symlink, try to create the new
 * name indicated by the symlink. The old code always complained that the
 * name already exists, due to not following the symlink even if its target
 * is nonexistent.  The new semantics affects also mknod() and link() when
 * the name is a symlink pointing to a non-existent name.
 *
 * I don't know which semantics is the right one, since I have no access
 * to standards. But I found by trial that HP-UX 9.0 has the full "new"
 * semantics implemented, while SunOS 4.1.1 and Solaris (SunOS 5.4) have the
 * "old" one. Personally, I think the new semantics is much more logical.
 * Note that "ln old new" where "new" is a symlink pointing to a non-existing
 * file does succeed in both HP-UX and SunOs, but not in Solaris
 * and in the old Linux semantics.
 */

/* [16-Dec-97 Kevin Buhr] For security reasons, we change some symlink
 * semantics.  See the comments in "open_namei" and "do_link" below.
 *
 * [10-Sep-98 Alan Modra] Another symlink change.
 */

/* [Feb-Apr 2000 AV] Complete rewrite. Rules for symlinks:
 *	inside the path - always follow.
 *	in the last component in creation/removal/renaming - never follow.
 *	if LOOKUP_FOLLOW passed - follow.
 *	if the pathname has trailing slashes - follow.
 *	otherwise - don't follow.
 * (applied in that order).
 *
 * [Jun 2000 AV] Inconsistent behaviour of open() in case if flags==O_CREAT
 * restored for 2.4. This is the last surviving part of old 4.2BSD bug.
 * During the 2.4 we need to fix the userland stuff depending on it -
 * hopefully we will be able to get rid of that wart in 2.5. So far only
 * XEmacs seems to be relying on it...
 */
/*
 * [Sep 2001 AV] Single-semaphore locking scheme (kudos to David Holland)
 * implemented.  Let's see if raised priority of ->s_vfs_rename_mutex gives
 * any extra contention...
 */

/* In order to reduce some races, while at the same time doing additional
 * checking and hopefully speeding things up, we copy filenames to the
 * kernel data space before using them..
 *
 * POSIX.1 2.4: an empty pathname is invalid (ENOENT).
 * PATH_MAX includes the nul terminator --RR.
 */

#define EMBEDDED_NAME_MAX	(PATH_MAX - offsetof(struct filename, iname))

static inline void initname(struct filename *name, const char __user *uptr)
{
	name->uptr = uptr;
	name->aname = NULL;
	atomic_set(&name->refcnt, 1);
}

struct filename *
getname_flags(const char __user *filename, int flags)
{
	struct filename *result;
	char *kname;
	int len;

	result = audit_reusename(filename);
	if (result)
		return result;

	result = __getname();
	if (unlikely(!result))
		return ERR_PTR(-ENOMEM);

	/*
	 * First, try to embed the struct filename inside the names_cache
	 * allocation
	 */
	kname = (char *)result->iname;
	result->name = kname;

	len = strncpy_from_user(kname, filename, EMBEDDED_NAME_MAX);
	/*
	 * Handle both empty path and copy failure in one go.
	 */
	if (unlikely(len <= 0)) {
		if (unlikely(len < 0)) {
			__putname(result);
			return ERR_PTR(len);
		}

		/* The empty path is special. */
		if (!(flags & LOOKUP_EMPTY)) {
			__putname(result);
			return ERR_PTR(-ENOENT);
		}
	}

	/*
	 * Uh-oh. We have a name that's approaching PATH_MAX. Allocate a
	 * separate struct filename so we can dedicate the entire
	 * names_cache allocation for the pathname, and re-do the copy from
	 * userland.
	 */
	if (unlikely(len == EMBEDDED_NAME_MAX)) {
		const size_t size = offsetof(struct filename, iname[1]);
		kname = (char *)result;

		/*
		 * size is chosen that way we to guarantee that
		 * result->iname[0] is within the same object and that
		 * kname can't be equal to result->iname, no matter what.
		 */
		result = kzalloc(size, GFP_KERNEL);
		if (unlikely(!result)) {
			__putname(kname);
			return ERR_PTR(-ENOMEM);
		}
		result->name = kname;
		len = strncpy_from_user(kname, filename, PATH_MAX);
		if (unlikely(len < 0)) {
			__putname(kname);
			kfree(result);
			return ERR_PTR(len);
		}
		/* The empty path is special. */
		if (unlikely(!len) && !(flags & LOOKUP_EMPTY)) {
			__putname(kname);
			kfree(result);
			return ERR_PTR(-ENOENT);
		}
		if (unlikely(len == PATH_MAX)) {
			__putname(kname);
			kfree(result);
			return ERR_PTR(-ENAMETOOLONG);
		}
	}
	initname(result, filename);
	audit_getname(result);
	return result;
}

struct filename *getname_uflags(const char __user *filename, int uflags)
{
	int flags = (uflags & AT_EMPTY_PATH) ? LOOKUP_EMPTY : 0;

	return getname_flags(filename, flags);
}

struct filename *__getname_maybe_null(const char __user *pathname)
{
	struct filename *name;
	char c;

	/* try to save on allocations; loss on um, though */
	if (get_user(c, pathname))
		return ERR_PTR(-EFAULT);
	if (!c)
		return NULL;

	name = getname_flags(pathname, LOOKUP_EMPTY);
	if (!IS_ERR(name) && !(name->name[0])) {
		putname(name);
		name = NULL;
	}
	return name;
}

struct filename *getname_kernel(const char * filename)
{
	struct filename *result;
	int len = strlen(filename) + 1;

	result = __getname();
	if (unlikely(!result))
		return ERR_PTR(-ENOMEM);

	if (len <= EMBEDDED_NAME_MAX) {
		result->name = (char *)result->iname;
	} else if (len <= PATH_MAX) {
		const size_t size = offsetof(struct filename, iname[1]);
		struct filename *tmp;

		tmp = kmalloc(size, GFP_KERNEL);
		if (unlikely(!tmp)) {
			__putname(result);
			return ERR_PTR(-ENOMEM);
		}
		tmp->name = (char *)result;
		result = tmp;
	} else {
		__putname(result);
		return ERR_PTR(-ENAMETOOLONG);
	}
	memcpy((char *)result->name, filename, len);
	initname(result, NULL);
	audit_getname(result);
	return result;
}
EXPORT_SYMBOL(getname_kernel);

void putname(struct filename *name)
{
	int refcnt;

	if (IS_ERR_OR_NULL(name))
		return;

	refcnt = atomic_read(&name->refcnt);
	if (refcnt != 1) {
		if (WARN_ON_ONCE(!refcnt))
			return;

		if (!atomic_dec_and_test(&name->refcnt))
			return;
	}

	if (name->name != name->iname) {
		__putname(name->name);
		kfree(name);
	} else
		__putname(name);
}
EXPORT_SYMBOL(putname);

/**
 * check_acl - perform ACL permission checking
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	inode to check permissions on
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC ...)
 *
 * This function performs the ACL permission checking. Since this function
 * retrieve POSIX acls it needs to know whether it is called from a blocking or
 * non-blocking context and thus cares about the MAY_NOT_BLOCK bit.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
static int check_acl(struct mnt_idmap *idmap,
		     struct inode *inode, int mask)
{
#ifdef CONFIG_FS_POSIX_ACL
	struct posix_acl *acl;

	if (mask & MAY_NOT_BLOCK) {
		acl = get_cached_acl_rcu(inode, ACL_TYPE_ACCESS);
	        if (!acl)
	                return -EAGAIN;
		/* no ->get_inode_acl() calls in RCU mode... */
		if (is_uncached_acl(acl))
			return -ECHILD;
	        return posix_acl_permission(idmap, inode, acl, mask);
	}

	acl = get_inode_acl(inode, ACL_TYPE_ACCESS);
	if (IS_ERR(acl))
		return PTR_ERR(acl);
	if (acl) {
	        int error = posix_acl_permission(idmap, inode, acl, mask);
	        posix_acl_release(acl);
	        return error;
	}
#endif

	return -EAGAIN;
}

/*
 * Very quick optimistic "we know we have no ACL's" check.
 *
 * Note that this is purely for ACL_TYPE_ACCESS, and purely
 * for the "we have cached that there are no ACLs" case.
 *
 * If this returns true, we know there are no ACLs. But if
 * it returns false, we might still not have ACLs (it could
 * be the is_uncached_acl() case).
 */
static inline bool no_acl_inode(struct inode *inode)
{
#ifdef CONFIG_FS_POSIX_ACL
	return likely(!READ_ONCE(inode->i_acl));
#else
	return true;
#endif
}

/**
 * acl_permission_check - perform basic UNIX permission checking
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	inode to check permissions on
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC ...)
 *
 * This function performs the basic UNIX permission checking. Since this
 * function may retrieve POSIX acls it needs to know whether it is called from a
 * blocking or non-blocking context and thus cares about the MAY_NOT_BLOCK bit.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
static int acl_permission_check(struct mnt_idmap *idmap,
				struct inode *inode, int mask)
{
	unsigned int mode = inode->i_mode;
	vfsuid_t vfsuid;

	/*
	 * Common cheap case: everybody has the requested
	 * rights, and there are no ACLs to check. No need
	 * to do any owner/group checks in that case.
	 *
	 *  - 'mask&7' is the requested permission bit set
	 *  - multiplying by 0111 spreads them out to all of ugo
	 *  - '& ~mode' looks for missing inode permission bits
	 *  - the '!' is for "no missing permissions"
	 *
	 * After that, we just need to check that there are no
	 * ACL's on the inode - do the 'IS_POSIXACL()' check last
	 * because it will dereference the ->i_sb pointer and we
	 * want to avoid that if at all possible.
	 */
	if (!((mask & 7) * 0111 & ~mode)) {
		if (no_acl_inode(inode))
			return 0;
		if (!IS_POSIXACL(inode))
			return 0;
	}

	/* Are we the owner? If so, ACL's don't matter */
	vfsuid = i_uid_into_vfsuid(idmap, inode);
	if (likely(vfsuid_eq_kuid(vfsuid, current_fsuid()))) {
		mask &= 7;
		mode >>= 6;
		return (mask & ~mode) ? -EACCES : 0;
	}

	/* Do we have ACL's? */
	if (IS_POSIXACL(inode) && (mode & S_IRWXG)) {
		int error = check_acl(idmap, inode, mask);
		if (error != -EAGAIN)
			return error;
	}

	/* Only RWX matters for group/other mode bits */
	mask &= 7;

	/*
	 * Are the group permissions different from
	 * the other permissions in the bits we care
	 * about? Need to check group ownership if so.
	 */
	if (mask & (mode ^ (mode >> 3))) {
		vfsgid_t vfsgid = i_gid_into_vfsgid(idmap, inode);
		if (vfsgid_in_group_p(vfsgid))
			mode >>= 3;
	}

	/* Bits in 'mode' clear that we require? */
	return (mask & ~mode) ? -EACCES : 0;
}

/**
 * generic_permission -  check for access rights on a Posix-like filesystem
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	inode to check access rights for
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC,
 *		%MAY_NOT_BLOCK ...)
 *
 * Used to check for read/write/execute permissions on a file.
 * We use "fsuid" for this, letting us set arbitrary permissions
 * for filesystem access without changing the "normal" uids which
 * are used for other things.
 *
 * generic_permission is rcu-walk aware. It returns -ECHILD in case an rcu-walk
 * request cannot be satisfied (eg. requires blocking or too much complexity).
 * It would then be called again in ref-walk mode.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int generic_permission(struct mnt_idmap *idmap, struct inode *inode,
		       int mask)
{
	int ret;

	/*
	 * Do the basic permission checks.
	 */
	ret = acl_permission_check(idmap, inode, mask);
	if (ret != -EACCES)
		return ret;

	if (S_ISDIR(inode->i_mode)) {
		/* DACs are overridable for directories */
		if (!(mask & MAY_WRITE))
			if (capable_wrt_inode_uidgid(idmap, inode,
						     CAP_DAC_READ_SEARCH))
				return 0;
		if (capable_wrt_inode_uidgid(idmap, inode,
					     CAP_DAC_OVERRIDE))
			return 0;
		return -EACCES;
	}

	/*
	 * Searching includes executable on directories, else just read.
	 */
	mask &= MAY_READ | MAY_WRITE | MAY_EXEC;
	if (mask == MAY_READ)
		if (capable_wrt_inode_uidgid(idmap, inode,
					     CAP_DAC_READ_SEARCH))
			return 0;
	/*
	 * Read/write DACs are always overridable.
	 * Executable DACs are overridable when there is
	 * at least one exec bit set.
	 */
	if (!(mask & MAY_EXEC) || (inode->i_mode & S_IXUGO))
		if (capable_wrt_inode_uidgid(idmap, inode,
					     CAP_DAC_OVERRIDE))
			return 0;

	return -EACCES;
}
EXPORT_SYMBOL(generic_permission);

/**
 * do_inode_permission - UNIX permission checking
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	inode to check permissions on
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC ...)
 *
 * We _really_ want to just do "generic_permission()" without
 * even looking at the inode->i_op values. So we keep a cache
 * flag in inode->i_opflags, that says "this has not special
 * permission function, use the fast case".
 */
static inline int do_inode_permission(struct mnt_idmap *idmap,
				      struct inode *inode, int mask)
{
	if (unlikely(!(inode->i_opflags & IOP_FASTPERM))) {
		if (likely(inode->i_op->permission))
			return inode->i_op->permission(idmap, inode, mask);

		/* This gets set once for the inode lifetime */
		spin_lock(&inode->i_lock);
		inode->i_opflags |= IOP_FASTPERM;
		spin_unlock(&inode->i_lock);
	}
	return generic_permission(idmap, inode, mask);
}

/**
 * sb_permission - Check superblock-level permissions
 * @sb: Superblock of inode to check permission on
 * @inode: Inode to check permission on
 * @mask: Right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * Separate out file-system wide checks from inode-specific permission checks.
 */
static int sb_permission(struct super_block *sb, struct inode *inode, int mask)
{
	if (unlikely(mask & MAY_WRITE)) {
		umode_t mode = inode->i_mode;

		/* Nobody gets write access to a read-only fs. */
		if (sb_rdonly(sb) && (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)))
			return -EROFS;
	}
	return 0;
}

/**
 * inode_permission - Check for access rights to a given inode
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	Inode to check permission on
 * @mask:	Right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * Check for read/write/execute permissions on an inode.  We use fs[ug]id for
 * this, letting us set arbitrary permissions for filesystem access without
 * changing the "normal" UIDs which are used for other things.
 *
 * When checking for MAY_APPEND, MAY_WRITE must also be set in @mask.
 */
int inode_permission(struct mnt_idmap *idmap,
		     struct inode *inode, int mask)
{
	int retval;

	retval = sb_permission(inode->i_sb, inode, mask);
	if (unlikely(retval))
		return retval;

	if (unlikely(mask & MAY_WRITE)) {
		/*
		 * Nobody gets write access to an immutable file.
		 */
		if (unlikely(IS_IMMUTABLE(inode)))
			return -EPERM;

		/*
		 * Updating mtime will likely cause i_uid and i_gid to be
		 * written back improperly if their true value is unknown
		 * to the vfs.
		 */
		if (unlikely(HAS_UNMAPPED_ID(idmap, inode)))
			return -EACCES;
	}

	retval = do_inode_permission(idmap, inode, mask);
	if (unlikely(retval))
		return retval;

	retval = devcgroup_inode_permission(inode, mask);
	if (unlikely(retval))
		return retval;

	return security_inode_permission(inode, mask);
}
EXPORT_SYMBOL(inode_permission);

/**
 * path_get - get a reference to a path
 * @path: path to get the reference to
 *
 * Given a path increment the reference count to the dentry and the vfsmount.
 */
void path_get(const struct path *path)
{
	mntget(path->mnt);
	dget(path->dentry);
}
EXPORT_SYMBOL(path_get);

/**
 * path_put - put a reference to a path
 * @path: path to put the reference to
 *
 * Given a path decrement the reference count to the dentry and the vfsmount.
 */
void path_put(const struct path *path)
{
	dput(path->dentry);
	mntput(path->mnt);
}
EXPORT_SYMBOL(path_put);

#define EMBEDDED_LEVELS 2
struct nameidata {
	struct path	path;
	struct qstr	last;
	struct path	root;
	struct inode	*inode; /* path.dentry.d_inode */
	unsigned int	flags, state;
	unsigned	seq, next_seq, m_seq, r_seq;
	int		last_type;
	unsigned	depth;
	int		total_link_count;
	struct saved {
		struct path link;
		struct delayed_call done;
		const char *name;
		unsigned seq;
	} *stack, internal[EMBEDDED_LEVELS];
	struct filename	*name;
	const char *pathname;
	struct nameidata *saved;
	unsigned	root_seq;
	int		dfd;
	vfsuid_t	dir_vfsuid;
	umode_t		dir_mode;
} __randomize_layout;

#define ND_ROOT_PRESET 1
#define ND_ROOT_GRABBED 2
#define ND_JUMPED 4

static void __set_nameidata(struct nameidata *p, int dfd, struct filename *name)
{
	struct nameidata *old = current->nameidata;
	p->stack = p->internal;
	p->depth = 0;
	p->dfd = dfd;
	p->name = name;
	p->pathname = likely(name) ? name->name : "";
	p->path.mnt = NULL;
	p->path.dentry = NULL;
	p->total_link_count = old ? old->total_link_count : 0;
	p->saved = old;
	current->nameidata = p;
}

static inline void set_nameidata(struct nameidata *p, int dfd, struct filename *name,
			  const struct path *root)
{
	__set_nameidata(p, dfd, name);
	p->state = 0;
	if (unlikely(root)) {
		p->state = ND_ROOT_PRESET;
		p->root = *root;
	}
}

static void restore_nameidata(void)
{
	struct nameidata *now = current->nameidata, *old = now->saved;

	current->nameidata = old;
	if (old)
		old->total_link_count = now->total_link_count;
	if (now->stack != now->internal)
		kfree(now->stack);
}

static bool nd_alloc_stack(struct nameidata *nd)
{
	struct saved *p;

	p= kmalloc_array(MAXSYMLINKS, sizeof(struct saved),
			 nd->flags & LOOKUP_RCU ? GFP_ATOMIC : GFP_KERNEL);
	if (unlikely(!p))
		return false;
	memcpy(p, nd->internal, sizeof(nd->internal));
	nd->stack = p;
	return true;
}

/**
 * path_connected - Verify that a dentry is below mnt.mnt_root
 * @mnt: The mountpoint to check.
 * @dentry: The dentry to check.
 *
 * Rename can sometimes move a file or directory outside of a bind
 * mount, path_connected allows those cases to be detected.
 */
static bool path_connected(struct vfsmount *mnt, struct dentry *dentry)
{
	struct super_block *sb = mnt->mnt_sb;

	/* Bind mounts can have disconnected paths */
	if (mnt->mnt_root == sb->s_root)
		return true;

	return is_subdir(dentry, mnt->mnt_root);
}

static void drop_links(struct nameidata *nd)
{
	int i = nd->depth;
	while (i--) {
		struct saved *last = nd->stack + i;
		do_delayed_call(&last->done);
		clear_delayed_call(&last->done);
	}
}

static void leave_rcu(struct nameidata *nd)
{
	nd->flags &= ~LOOKUP_RCU;
	nd->seq = nd->next_seq = 0;
	rcu_read_unlock();
}

static void terminate_walk(struct nameidata *nd)
{
	drop_links(nd);
	if (!(nd->flags & LOOKUP_RCU)) {
		int i;
		path_put(&nd->path);
		for (i = 0; i < nd->depth; i++)
			path_put(&nd->stack[i].link);
		if (nd->state & ND_ROOT_GRABBED) {
			path_put(&nd->root);
			nd->state &= ~ND_ROOT_GRABBED;
		}
	} else {
		leave_rcu(nd);
	}
	nd->depth = 0;
	nd->path.mnt = NULL;
	nd->path.dentry = NULL;
}

/* path_put is needed afterwards regardless of success or failure */
static bool __legitimize_path(struct path *path, unsigned seq, unsigned mseq)
{
	int res = __legitimize_mnt(path->mnt, mseq);
	if (unlikely(res)) {
		if (res > 0)
			path->mnt = NULL;
		path->dentry = NULL;
		return false;
	}
	if (unlikely(!lockref_get_not_dead(&path->dentry->d_lockref))) {
		path->dentry = NULL;
		return false;
	}
	return !read_seqcount_retry(&path->dentry->d_seq, seq);
}

static inline bool legitimize_path(struct nameidata *nd,
			    struct path *path, unsigned seq)
{
	return __legitimize_path(path, seq, nd->m_seq);
}

static bool legitimize_links(struct nameidata *nd)
{
	int i;
	if (unlikely(nd->flags & LOOKUP_CACHED)) {
		drop_links(nd);
		nd->depth = 0;
		return false;
	}
	for (i = 0; i < nd->depth; i++) {
		struct saved *last = nd->stack + i;
		if (unlikely(!legitimize_path(nd, &last->link, last->seq))) {
			drop_links(nd);
			nd->depth = i + 1;
			return false;
		}
	}
	return true;
}

static bool legitimize_root(struct nameidata *nd)
{
	/* Nothing to do if nd->root is zero or is managed by the VFS user. */
	if (!nd->root.mnt || (nd->state & ND_ROOT_PRESET))
		return true;
	nd->state |= ND_ROOT_GRABBED;
	return legitimize_path(nd, &nd->root, nd->root_seq);
}

/*
 * Path walking has 2 modes, rcu-walk and ref-walk (see
 * Documentation/filesystems/path-lookup.txt).  In situations when we can't
 * continue in RCU mode, we attempt to drop out of rcu-walk mode and grab
 * normal reference counts on dentries and vfsmounts to transition to ref-walk
 * mode.  Refcounts are grabbed at the last known good point before rcu-walk
 * got stuck, so ref-walk may continue from there. If this is not successful
 * (eg. a seqcount has changed), then failure is returned and it's up to caller
 * to restart the path walk from the beginning in ref-walk mode.
 */

/**
 * try_to_unlazy - try to switch to ref-walk mode.
 * @nd: nameidata pathwalk data
 * Returns: true on success, false on failure
 *
 * try_to_unlazy attempts to legitimize the current nd->path and nd->root
 * for ref-walk mode.
 * Must be called from rcu-walk context.
 * Nothing should touch nameidata between try_to_unlazy() failure and
 * terminate_walk().
 */
static bool try_to_unlazy(struct nameidata *nd)
{
	struct dentry *parent = nd->path.dentry;

	BUG_ON(!(nd->flags & LOOKUP_RCU));

	if (unlikely(!legitimize_links(nd)))
		goto out1;
	if (unlikely(!legitimize_path(nd, &nd->path, nd->seq)))
		goto out;
	if (unlikely(!legitimize_root(nd)))
		goto out;
	leave_rcu(nd);
	BUG_ON(nd->inode != parent->d_inode);
	return true;

out1:
	nd->path.mnt = NULL;
	nd->path.dentry = NULL;
out:
	leave_rcu(nd);
	return false;
}

/**
 * try_to_unlazy_next - try to switch to ref-walk mode.
 * @nd: nameidata pathwalk data
 * @dentry: next dentry to step into
 * Returns: true on success, false on failure
 *
 * Similar to try_to_unlazy(), but here we have the next dentry already
 * picked by rcu-walk and want to legitimize that in addition to the current
 * nd->path and nd->root for ref-walk mode.  Must be called from rcu-walk context.
 * Nothing should touch nameidata between try_to_unlazy_next() failure and
 * terminate_walk().
 */
static bool try_to_unlazy_next(struct nameidata *nd, struct dentry *dentry)
{
	int res;
	BUG_ON(!(nd->flags & LOOKUP_RCU));

	if (unlikely(!legitimize_links(nd)))
		goto out2;
	res = __legitimize_mnt(nd->path.mnt, nd->m_seq);
	if (unlikely(res)) {
		if (res > 0)
			goto out2;
		goto out1;
	}
	if (unlikely(!lockref_get_not_dead(&nd->path.dentry->d_lockref)))
		goto out1;

	/*
	 * We need to move both the parent and the dentry from the RCU domain
	 * to be properly refcounted. And the sequence number in the dentry
	 * validates *both* dentry counters, since we checked the sequence
	 * number of the parent after we got the child sequence number. So we
	 * know the parent must still be valid if the child sequence number is
	 */
	if (unlikely(!lockref_get_not_dead(&dentry->d_lockref)))
		goto out;
	if (read_seqcount_retry(&dentry->d_seq, nd->next_seq))
		goto out_dput;
	/*
	 * Sequence counts matched. Now make sure that the root is
	 * still valid and get it if required.
	 */
	if (unlikely(!legitimize_root(nd)))
		goto out_dput;
	leave_rcu(nd);
	return true;

out2:
	nd->path.mnt = NULL;
out1:
	nd->path.dentry = NULL;
out:
	leave_rcu(nd);
	return false;
out_dput:
	leave_rcu(nd);
	dput(dentry);
	return false;
}

static inline int d_revalidate(struct inode *dir, const struct qstr *name,
			       struct dentry *dentry, unsigned int flags)
{
	if (unlikely(dentry->d_flags & DCACHE_OP_REVALIDATE))
		return dentry->d_op->d_revalidate(dir, name, dentry, flags);
	else
		return 1;
}

/**
 * complete_walk - successful completion of path walk
 * @nd:  pointer nameidata
 *
 * If we had been in RCU mode, drop out of it and legitimize nd->path.
 * Revalidate the final result, unless we'd already done that during
 * the path walk or the filesystem doesn't ask for it.  Return 0 on
 * success, -error on failure.  In case of failure caller does not
 * need to drop nd->path.
 */
static int complete_walk(struct nameidata *nd)
{
	struct dentry *dentry = nd->path.dentry;
	int status;

	if (nd->flags & LOOKUP_RCU) {
		/*
		 * We don't want to zero nd->root for scoped-lookups or
		 * externally-managed nd->root.
		 */
		if (!(nd->state & ND_ROOT_PRESET))
			if (!(nd->flags & LOOKUP_IS_SCOPED))
				nd->root.mnt = NULL;
		nd->flags &= ~LOOKUP_CACHED;
		if (!try_to_unlazy(nd))
			return -ECHILD;
	}

	if (unlikely(nd->flags & LOOKUP_IS_SCOPED)) {
		/*
		 * While the guarantee of LOOKUP_IS_SCOPED is (roughly) "don't
		 * ever step outside the root during lookup" and should already
		 * be guaranteed by the rest of namei, we want to avoid a namei
		 * BUG resulting in userspace being given a path that was not
		 * scoped within the root at some point during the lookup.
		 *
		 * So, do a final sanity-check to make sure that in the
		 * worst-case scenario (a complete bypass of LOOKUP_IS_SCOPED)
		 * we won't silently return an fd completely outside of the
		 * requested root to userspace.
		 *
		 * Userspace could move the path outside the root after this
		 * check, but as discussed elsewhere this is not a concern (the
		 * resolved file was inside the root at some point).
		 */
		if (!path_is_under(&nd->path, &nd->root))
			return -EXDEV;
	}

	if (likely(!(nd->state & ND_JUMPED)))
		return 0;

	if (likely(!(dentry->d_flags & DCACHE_OP_WEAK_REVALIDATE)))
		return 0;

	status = dentry->d_op->d_weak_revalidate(dentry, nd->flags);
	if (status > 0)
		return 0;

	if (!status)
		status = -ESTALE;

	return status;
}

static int set_root(struct nameidata *nd)
{
	struct fs_struct *fs = current->fs;

	/*
	 * Jumping to the real root in a scoped-lookup is a BUG in namei, but we
	 * still have to ensure it doesn't happen because it will cause a breakout
	 * from the dirfd.
	 */
	if (WARN_ON(nd->flags & LOOKUP_IS_SCOPED))
		return -ENOTRECOVERABLE;

	if (nd->flags & LOOKUP_RCU) {
		unsigned seq;

		do {
			seq = read_seqbegin(&fs->seq);
			nd->root = fs->root;
			nd->root_seq = __read_seqcount_begin(&nd->root.dentry->d_seq);
		} while (read_seqretry(&fs->seq, seq));
	} else {
		get_fs_root(fs, &nd->root);
		nd->state |= ND_ROOT_GRABBED;
	}
	return 0;
}

static int nd_jump_root(struct nameidata *nd)
{
	if (unlikely(nd->flags & LOOKUP_BENEATH))
		return -EXDEV;
	if (unlikely(nd->flags & LOOKUP_NO_XDEV)) {
		/* Absolute path arguments to path_init() are allowed. */
		if (nd->path.mnt != NULL && nd->path.mnt != nd->root.mnt)
			return -EXDEV;
	}
	if (!nd->root.mnt) {
		int error = set_root(nd);
		if (error)
			return error;
	}
	if (nd->flags & LOOKUP_RCU) {
		struct dentry *d;
		nd->path = nd->root;
		d = nd->path.dentry;
		nd->inode = d->d_inode;
		nd->seq = nd->root_seq;
		if (read_seqcount_retry(&d->d_seq, nd->seq))
			return -ECHILD;
	} else {
		path_put(&nd->path);
		nd->path = nd->root;
		path_get(&nd->path);
		nd->inode = nd->path.dentry->d_inode;
	}
	nd->state |= ND_JUMPED;
	return 0;
}

/*
 * Helper to directly jump to a known parsed path from ->get_link,
 * caller must have taken a reference to path beforehand.
 */
int nd_jump_link(const struct path *path)
{
	int error = -ELOOP;
	struct nameidata *nd = current->nameidata;

	if (unlikely(nd->flags & LOOKUP_NO_MAGICLINKS))
		goto err;

	error = -EXDEV;
	if (unlikely(nd->flags & LOOKUP_NO_XDEV)) {
		if (nd->path.mnt != path->mnt)
			goto err;
	}
	/* Not currently safe for scoped-lookups. */
	if (unlikely(nd->flags & LOOKUP_IS_SCOPED))
		goto err;

	path_put(&nd->path);
	nd->path = *path;
	nd->inode = nd->path.dentry->d_inode;
	nd->state |= ND_JUMPED;
	return 0;

err:
	path_put(path);
	return error;
}

static inline void put_link(struct nameidata *nd)
{
	struct saved *last = nd->stack + --nd->depth;
	do_delayed_call(&last->done);
	if (!(nd->flags & LOOKUP_RCU))
		path_put(&last->link);
}

static int sysctl_protected_symlinks __read_mostly;
static int sysctl_protected_hardlinks __read_mostly;
static int sysctl_protected_fifos __read_mostly;
static int sysctl_protected_regular __read_mostly;

#ifdef CONFIG_SYSCTL
static const struct ctl_table namei_sysctls[] = {
	{
		.procname	= "protected_symlinks",
		.data		= &sysctl_protected_symlinks,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "protected_hardlinks",
		.data		= &sysctl_protected_hardlinks,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "protected_fifos",
		.data		= &sysctl_protected_fifos,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
	{
		.procname	= "protected_regular",
		.data		= &sysctl_protected_regular,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
};

static int __init init_fs_namei_sysctls(void)
{
	register_sysctl_init("fs", namei_sysctls);
	return 0;
}
fs_initcall(init_fs_namei_sysctls);

#endif /* CONFIG_SYSCTL */

/**
 * may_follow_link - Check symlink following for unsafe situations
 * @nd: nameidata pathwalk data
 * @inode: Used for idmapping.
 *
 * In the case of the sysctl_protected_symlinks sysctl being enabled,
 * CAP_DAC_OVERRIDE needs to be specifically ignored if the symlink is
 * in a sticky world-writable directory. This is to protect privileged
 * processes from failing races against path names that may change out
 * from under them by way of other users creating malicious symlinks.
 * It will permit symlinks to be followed only when outside a sticky
 * world-writable directory, or when the uid of the symlink and follower
 * match, or when the directory owner matches the symlink's owner.
 *
 * Returns 0 if following the symlink is allowed, -ve on error.
 */
static inline int may_follow_link(struct nameidata *nd, const struct inode *inode)
{
	struct mnt_idmap *idmap;
	vfsuid_t vfsuid;

	if (!sysctl_protected_symlinks)
		return 0;

	idmap = mnt_idmap(nd->path.mnt);
	vfsuid = i_uid_into_vfsuid(idmap, inode);
	/* Allowed if owner and follower match. */
	if (vfsuid_eq_kuid(vfsuid, current_fsuid()))
		return 0;

	/* Allowed if parent directory not sticky and world-writable. */
	if ((nd->dir_mode & (S_ISVTX|S_IWOTH)) != (S_ISVTX|S_IWOTH))
		return 0;

	/* Allowed if parent directory and link owner match. */
	if (vfsuid_valid(nd->dir_vfsuid) && vfsuid_eq(nd->dir_vfsuid, vfsuid))
		return 0;

	if (nd->flags & LOOKUP_RCU)
		return -ECHILD;

	audit_inode(nd->name, nd->stack[0].link.dentry, 0);
	audit_log_path_denied(AUDIT_ANOM_LINK, "follow_link");
	return -EACCES;
}

/**
 * safe_hardlink_source - Check for safe hardlink conditions
 * @idmap: idmap of the mount the inode was found from
 * @inode: the source inode to hardlink from
 *
 * Return false if at least one of the following conditions:
 *    - inode is not a regular file
 *    - inode is setuid
 *    - inode is setgid and group-exec
 *    - access failure for read and write
 *
 * Otherwise returns true.
 */
static bool safe_hardlink_source(struct mnt_idmap *idmap,
				 struct inode *inode)
{
	umode_t mode = inode->i_mode;

	/* Special files should not get pinned to the filesystem. */
	if (!S_ISREG(mode))
		return false;

	/* Setuid files should not get pinned to the filesystem. */
	if (mode & S_ISUID)
		return false;

	/* Executable setgid files should not get pinned to the filesystem. */
	if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
		return false;

	/* Hardlinking to unreadable or unwritable sources is dangerous. */
	if (inode_permission(idmap, inode, MAY_READ | MAY_WRITE))
		return false;

	return true;
}

/**
 * may_linkat - Check permissions for creating a hardlink
 * @idmap: idmap of the mount the inode was found from
 * @link:  the source to hardlink from
 *
 * Block hardlink when all of:
 *  - sysctl_protected_hardlinks enabled
 *  - fsuid does not match inode
 *  - hardlink source is unsafe (see safe_hardlink_source() above)
 *  - not CAP_FOWNER in a namespace with the inode owner uid mapped
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 *
 * Returns 0 if successful, -ve on error.
 */
int may_linkat(struct mnt_idmap *idmap, const struct path *link)
{
	struct inode *inode = link->dentry->d_inode;

	/* Inode writeback is not safe when the uid or gid are invalid. */
	if (!vfsuid_valid(i_uid_into_vfsuid(idmap, inode)) ||
	    !vfsgid_valid(i_gid_into_vfsgid(idmap, inode)))
		return -EOVERFLOW;

	if (!sysctl_protected_hardlinks)
		return 0;

	/* Source inode owner (or CAP_FOWNER) can hardlink all they like,
	 * otherwise, it must be a safe source.
	 */
	if (safe_hardlink_source(idmap, inode) ||
	    inode_owner_or_capable(idmap, inode))
		return 0;

	audit_log_path_denied(AUDIT_ANOM_LINK, "linkat");
	return -EPERM;
}

/**
 * may_create_in_sticky - Check whether an O_CREAT open in a sticky directory
 *			  should be allowed, or not, on files that already
 *			  exist.
 * @idmap: idmap of the mount the inode was found from
 * @nd: nameidata pathwalk data
 * @inode: the inode of the file to open
 *
 * Block an O_CREAT open of a FIFO (or a regular file) when:
 *   - sysctl_protected_fifos (or sysctl_protected_regular) is enabled
 *   - the file already exists
 *   - we are in a sticky directory
 *   - we don't own the file
 *   - the owner of the directory doesn't own the file
 *   - the directory is world writable
 * If the sysctl_protected_fifos (or sysctl_protected_regular) is set to 2
 * the directory doesn't have to be world writable: being group writable will
 * be enough.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 *
 * Returns 0 if the open is allowed, -ve on error.
 */
static int may_create_in_sticky(struct mnt_idmap *idmap, struct nameidata *nd,
				struct inode *const inode)
{
	umode_t dir_mode = nd->dir_mode;
	vfsuid_t dir_vfsuid = nd->dir_vfsuid, i_vfsuid;

	if (likely(!(dir_mode & S_ISVTX)))
		return 0;

	if (S_ISREG(inode->i_mode) && !sysctl_protected_regular)
		return 0;

	if (S_ISFIFO(inode->i_mode) && !sysctl_protected_fifos)
		return 0;

	i_vfsuid = i_uid_into_vfsuid(idmap, inode);

	if (vfsuid_eq(i_vfsuid, dir_vfsuid))
		return 0;

	if (vfsuid_eq_kuid(i_vfsuid, current_fsuid()))
		return 0;

	if (likely(dir_mode & 0002)) {
		audit_log_path_denied(AUDIT_ANOM_CREAT, "sticky_create");
		return -EACCES;
	}

	if (dir_mode & 0020) {
		if (sysctl_protected_fifos >= 2 && S_ISFIFO(inode->i_mode)) {
			audit_log_path_denied(AUDIT_ANOM_CREAT,
					      "sticky_create_fifo");
			return -EACCES;
		}

		if (sysctl_protected_regular >= 2 && S_ISREG(inode->i_mode)) {
			audit_log_path_denied(AUDIT_ANOM_CREAT,
					      "sticky_create_regular");
			return -EACCES;
		}
	}

	return 0;
}

/*
 * follow_up - Find the mountpoint of path's vfsmount
 *
 * Given a path, find the mountpoint of its source file system.
 * Replace @path with the path of the mountpoint in the parent mount.
 * Up is towards /.
 *
 * Return 1 if we went up a level and 0 if we were already at the
 * root.
 */
int follow_up(struct path *path)
{
	struct mount *mnt = real_mount(path->mnt);
	struct mount *parent;
	struct dentry *mountpoint;

	read_seqlock_excl(&mount_lock);
	parent = mnt->mnt_parent;
	if (parent == mnt) {
		read_sequnlock_excl(&mount_lock);
		return 0;
	}
	mntget(&parent->mnt);
	mountpoint = dget(mnt->mnt_mountpoint);
	read_sequnlock_excl(&mount_lock);
	dput(path->dentry);
	path->dentry = mountpoint;
	mntput(path->mnt);
	path->mnt = &parent->mnt;
	return 1;
}
EXPORT_SYMBOL(follow_up);

static bool choose_mountpoint_rcu(struct mount *m, const struct path *root,
				  struct path *path, unsigned *seqp)
{
	while (mnt_has_parent(m)) {
		struct dentry *mountpoint = m->mnt_mountpoint;

		m = m->mnt_parent;
		if (unlikely(root->dentry == mountpoint &&
			     root->mnt == &m->mnt))
			break;
		if (mountpoint != m->mnt.mnt_root) {
			path->mnt = &m->mnt;
			path->dentry = mountpoint;
			*seqp = read_seqcount_begin(&mountpoint->d_seq);
			return true;
		}
	}
	return false;
}

static bool choose_mountpoint(struct mount *m, const struct path *root,
			      struct path *path)
{
	bool found;

	rcu_read_lock();
	while (1) {
		unsigned seq, mseq = read_seqbegin(&mount_lock);

		found = choose_mountpoint_rcu(m, root, path, &seq);
		if (unlikely(!found)) {
			if (!read_seqretry(&mount_lock, mseq))
				break;
		} else {
			if (likely(__legitimize_path(path, seq, mseq)))
				break;
			rcu_read_unlock();
			path_put(path);
			rcu_read_lock();
		}
	}
	rcu_read_unlock();
	return found;
}

/*
 * Perform an automount
 * - return -EISDIR to tell follow_managed() to stop and return the path we
 *   were called with.
 */
static int follow_automount(struct path *path, int *count, unsigned lookup_flags)
{
	struct dentry *dentry = path->dentry;

	/* We don't want to mount if someone's just doing a stat -
	 * unless they're stat'ing a directory and appended a '/' to
	 * the name.
	 *
	 * We do, however, want to mount if someone wants to open or
	 * create a file of any type under the mountpoint, wants to
	 * traverse through the mountpoint or wants to open the
	 * mounted directory.  Also, autofs may mark negative dentries
	 * as being automount points.  These will need the attentions
	 * of the daemon to instantiate them before they can be used.
	 */
	if (!(lookup_flags & (LOOKUP_PARENT | LOOKUP_DIRECTORY |
			   LOOKUP_OPEN | LOOKUP_CREATE | LOOKUP_AUTOMOUNT)) &&
	    dentry->d_inode)
		return -EISDIR;

	if (count && (*count)++ >= MAXSYMLINKS)
		return -ELOOP;

	return finish_automount(dentry->d_op->d_automount(path), path);
}

/*
 * mount traversal - out-of-line part.  One note on ->d_flags accesses -
 * dentries are pinned but not locked here, so negative dentry can go
 * positive right under us.  Use of smp_load_acquire() provides a barrier
 * sufficient for ->d_inode and ->d_flags consistency.
 */
static int __traverse_mounts(struct path *path, unsigned flags, bool *jumped,
			     int *count, unsigned lookup_flags)
{
	struct vfsmount *mnt = path->mnt;
	bool need_mntput = false;
	int ret = 0;

	while (flags & DCACHE_MANAGED_DENTRY) {
		/* Allow the filesystem to manage the transit without i_rwsem
		 * being held. */
		if (flags & DCACHE_MANAGE_TRANSIT) {
			ret = path->dentry->d_op->d_manage(path, false);
			flags = smp_load_acquire(&path->dentry->d_flags);
			if (ret < 0)
				break;
		}

		if (flags & DCACHE_MOUNTED) {	// something's mounted on it..
			struct vfsmount *mounted = lookup_mnt(path);
			if (mounted) {		// ... in our namespace
				dput(path->dentry);
				if (need_mntput)
					mntput(path->mnt);
				path->mnt = mounted;
				path->dentry = dget(mounted->mnt_root);
				// here we know it's positive
				flags = path->dentry->d_flags;
				need_mntput = true;
				continue;
			}
		}

		if (!(flags & DCACHE_NEED_AUTOMOUNT))
			break;

		// uncovered automount point
		ret = follow_automount(path, count, lookup_flags);
		flags = smp_load_acquire(&path->dentry->d_flags);
		if (ret < 0)
			break;
	}

	if (ret == -EISDIR)
		ret = 0;
	// possible if you race with several mount --move
	if (need_mntput && path->mnt == mnt)
		mntput(path->mnt);
	if (!ret && unlikely(d_flags_negative(flags)))
		ret = -ENOENT;
	*jumped = need_mntput;
	return ret;
}

static inline int traverse_mounts(struct path *path, bool *jumped,
				  int *count, unsigned lookup_flags)
{
	unsigned flags = smp_load_acquire(&path->dentry->d_flags);

	/* fastpath */
	if (likely(!(flags & DCACHE_MANAGED_DENTRY))) {
		*jumped = false;
		if (unlikely(d_flags_negative(flags)))
			return -ENOENT;
		return 0;
	}
	return __traverse_mounts(path, flags, jumped, count, lookup_flags);
}

int follow_down_one(struct path *path)
{
	struct vfsmount *mounted;

	mounted = lookup_mnt(path);
	if (mounted) {
		dput(path->dentry);
		mntput(path->mnt);
		path->mnt = mounted;
		path->dentry = dget(mounted->mnt_root);
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(follow_down_one);

/*
 * Follow down to the covering mount currently visible to userspace.  At each
 * point, the filesystem owning that dentry may be queried as to whether the
 * caller is permitted to proceed or not.
 */
int follow_down(struct path *path, unsigned int flags)
{
	struct vfsmount *mnt = path->mnt;
	bool jumped;
	int ret = traverse_mounts(path, &jumped, NULL, flags);

	if (path->mnt != mnt)
		mntput(mnt);
	return ret;
}
EXPORT_SYMBOL(follow_down);

/*
 * Try to skip to top of mountpoint pile in rcuwalk mode.  Fail if
 * we meet a managed dentry that would need blocking.
 */
static bool __follow_mount_rcu(struct nameidata *nd, struct path *path)
{
	struct dentry *dentry = path->dentry;
	unsigned int flags = dentry->d_flags;

	if (likely(!(flags & DCACHE_MANAGED_DENTRY)))
		return true;

	if (unlikely(nd->flags & LOOKUP_NO_XDEV))
		return false;

	for (;;) {
		/*
		 * Don't forget we might have a non-mountpoint managed dentry
		 * that wants to block transit.
		 */
		if (unlikely(flags & DCACHE_MANAGE_TRANSIT)) {
			int res = dentry->d_op->d_manage(path, true);
			if (res)
				return res == -EISDIR;
			flags = dentry->d_flags;
		}

		if (flags & DCACHE_MOUNTED) {
			struct mount *mounted = __lookup_mnt(path->mnt, dentry);
			if (mounted) {
				path->mnt = &mounted->mnt;
				dentry = path->dentry = mounted->mnt.mnt_root;
				nd->state |= ND_JUMPED;
				nd->next_seq = read_seqcount_begin(&dentry->d_seq);
				flags = dentry->d_flags;
				// makes sure that non-RCU pathwalk could reach
				// this state.
				if (read_seqretry(&mount_lock, nd->m_seq))
					return false;
				continue;
			}
			if (read_seqretry(&mount_lock, nd->m_seq))
				return false;
		}
		return !(flags & DCACHE_NEED_AUTOMOUNT);
	}
}

static inline int handle_mounts(struct nameidata *nd, struct dentry *dentry,
			  struct path *path)
{
	bool jumped;
	int ret;

	path->mnt = nd->path.mnt;
	path->dentry = dentry;
	if (nd->flags & LOOKUP_RCU) {
		unsigned int seq = nd->next_seq;
		if (likely(__follow_mount_rcu(nd, path)))
			return 0;
		// *path and nd->next_seq might've been clobbered
		path->mnt = nd->path.mnt;
		path->dentry = dentry;
		nd->next_seq = seq;
		if (!try_to_unlazy_next(nd, dentry))
			return -ECHILD;
	}
	ret = traverse_mounts(path, &jumped, &nd->total_link_count, nd->flags);
	if (jumped) {
		if (unlikely(nd->flags & LOOKUP_NO_XDEV))
			ret = -EXDEV;
		else
			nd->state |= ND_JUMPED;
	}
	if (unlikely(ret)) {
		dput(path->dentry);
		if (path->mnt != nd->path.mnt)
			mntput(path->mnt);
	}
	return ret;
}

/*
 * This looks up the name in dcache and possibly revalidates the found dentry.
 * NULL is returned if the dentry does not exist in the cache.
 */
static struct dentry *lookup_dcache(const struct qstr *name,
				    struct dentry *dir,
				    unsigned int flags)
{
	struct dentry *dentry = d_lookup(dir, name);
	if (dentry) {
		int error = d_revalidate(dir->d_inode, name, dentry, flags);
		if (unlikely(error <= 0)) {
			if (!error)
				d_invalidate(dentry);
			dput(dentry);
			return ERR_PTR(error);
		}
	}
	return dentry;
}

/*
 * Parent directory has inode locked exclusive.  This is one
 * and only case when ->lookup() gets called on non in-lookup
 * dentries - as the matter of fact, this only gets called
 * when directory is guaranteed to have no in-lookup children
 * at all.
 * Will return -ENOENT if name isn't found and LOOKUP_CREATE wasn't passed.
 * Will return -EEXIST if name is found and LOOKUP_EXCL was passed.
 */
struct dentry *lookup_one_qstr_excl(const struct qstr *name,
				    struct dentry *base, unsigned int flags)
{
	struct dentry *dentry;
	struct dentry *old;
	struct inode *dir;

	dentry = lookup_dcache(name, base, flags);
	if (dentry)
		goto found;

	/* Don't create child dentry for a dead directory. */
	dir = base->d_inode;
	if (unlikely(IS_DEADDIR(dir)))
		return ERR_PTR(-ENOENT);

	dentry = d_alloc(base, name);
	if (unlikely(!dentry))
		return ERR_PTR(-ENOMEM);

	old = dir->i_op->lookup(dir, dentry, flags);
	if (unlikely(old)) {
		dput(dentry);
		dentry = old;
	}
found:
	if (IS_ERR(dentry))
		return dentry;
	if (d_is_negative(dentry) && !(flags & LOOKUP_CREATE)) {
		dput(dentry);
		return ERR_PTR(-ENOENT);
	}
	if (d_is_positive(dentry) && (flags & LOOKUP_EXCL)) {
		dput(dentry);
		return ERR_PTR(-EEXIST);
	}
	return dentry;
}
EXPORT_SYMBOL(lookup_one_qstr_excl);

/**
 * lookup_fast - do fast lockless (but racy) lookup of a dentry
 * @nd: current nameidata
 *
 * Do a fast, but racy lookup in the dcache for the given dentry, and
 * revalidate it. Returns a valid dentry pointer or NULL if one wasn't
 * found. On error, an ERR_PTR will be returned.
 *
 * If this function returns a valid dentry and the walk is no longer
 * lazy, the dentry will carry a reference that must later be put. If
 * RCU mode is still in force, then this is not the case and the dentry
 * must be legitimized before use. If this returns NULL, then the walk
 * will no longer be in RCU mode.
 */
static struct dentry *lookup_fast(struct nameidata *nd)
{
	struct dentry *dentry, *parent = nd->path.dentry;
	int status = 1;

	/*
	 * Rename seqlock is not required here because in the off chance
	 * of a false negative due to a concurrent rename, the caller is
	 * going to fall back to non-racy lookup.
	 */
	if (nd->flags & LOOKUP_RCU) {
		dentry = __d_lookup_rcu(parent, &nd->last, &nd->next_seq);
		if (unlikely(!dentry)) {
			if (!try_to_unlazy(nd))
				return ERR_PTR(-ECHILD);
			return NULL;
		}

		/*
		 * This sequence count validates that the parent had no
		 * changes while we did the lookup of the dentry above.
		 */
		if (read_seqcount_retry(&parent->d_seq, nd->seq))
			return ERR_PTR(-ECHILD);

		status = d_revalidate(nd->inode, &nd->last, dentry, nd->flags);
		if (likely(status > 0))
			return dentry;
		if (!try_to_unlazy_next(nd, dentry))
			return ERR_PTR(-ECHILD);
		if (status == -ECHILD)
			/* we'd been told to redo it in non-rcu mode */
			status = d_revalidate(nd->inode, &nd->last,
					      dentry, nd->flags);
	} else {
		dentry = __d_lookup(parent, &nd->last);
		if (unlikely(!dentry))
			return NULL;
		status = d_revalidate(nd->inode, &nd->last, dentry, nd->flags);
	}
	if (unlikely(status <= 0)) {
		if (!status)
			d_invalidate(dentry);
		dput(dentry);
		return ERR_PTR(status);
	}
	return dentry;
}

/* Fast lookup failed, do it the slow way */
static struct dentry *__lookup_slow(const struct qstr *name,
				    struct dentry *dir,
				    unsigned int flags)
{
	struct dentry *dentry, *old;
	struct inode *inode = dir->d_inode;
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq);

	/* Don't go there if it's already dead */
	if (unlikely(IS_DEADDIR(inode)))
		return ERR_PTR(-ENOENT);
again:
	dentry = d_alloc_parallel(dir, name, &wq);
	if (IS_ERR(dentry))
		return dentry;
	if (unlikely(!d_in_lookup(dentry))) {
		int error = d_revalidate(inode, name, dentry, flags);
		if (unlikely(error <= 0)) {
			if (!error) {
				d_invalidate(dentry);
				dput(dentry);
				goto again;
			}
			dput(dentry);
			dentry = ERR_PTR(error);
		}
	} else {
		old = inode->i_op->lookup(inode, dentry, flags);
		d_lookup_done(dentry);
		if (unlikely(old)) {
			dput(dentry);
			dentry = old;
		}
	}
	return dentry;
}

static struct dentry *lookup_slow(const struct qstr *name,
				  struct dentry *dir,
				  unsigned int flags)
{
	struct inode *inode = dir->d_inode;
	struct dentry *res;
	inode_lock_shared(inode);
	res = __lookup_slow(name, dir, flags);
	inode_unlock_shared(inode);
	return res;
}

static inline int may_lookup(struct mnt_idmap *idmap,
			     struct nameidata *restrict nd)
{
	int err, mask;

	mask = nd->flags & LOOKUP_RCU ? MAY_NOT_BLOCK : 0;
	err = inode_permission(idmap, nd->inode, mask | MAY_EXEC);
	if (likely(!err))
		return 0;

	// If we failed, and we weren't in LOOKUP_RCU, it's final
	if (!(nd->flags & LOOKUP_RCU))
		return err;

	// Drop out of RCU mode to make sure it wasn't transient
	if (!try_to_unlazy(nd))
		return -ECHILD;	// redo it all non-lazy

	if (err != -ECHILD)	// hard error
		return err;

	return inode_permission(idmap, nd->inode, MAY_EXEC);
}

static int reserve_stack(struct nameidata *nd, struct path *link)
{
	if (unlikely(nd->total_link_count++ >= MAXSYMLINKS))
		return -ELOOP;

	if (likely(nd->depth != EMBEDDED_LEVELS))
		return 0;
	if (likely(nd->stack != nd->internal))
		return 0;
	if (likely(nd_alloc_stack(nd)))
		return 0;

	if (nd->flags & LOOKUP_RCU) {
		// we need to grab link before we do unlazy.  And we can't skip
		// unlazy even if we fail to grab the link - cleanup needs it
		bool grabbed_link = legitimize_path(nd, link, nd->next_seq);

		if (!try_to_unlazy(nd) || !grabbed_link)
			return -ECHILD;

		if (nd_alloc_stack(nd))
			return 0;
	}
	return -ENOMEM;
}

enum {WALK_TRAILING = 1, WALK_MORE = 2, WALK_NOFOLLOW = 4};

static const char *pick_link(struct nameidata *nd, struct path *link,
		     struct inode *inode, int flags)
{
	struct saved *last;
	const char *res;
	int error = reserve_stack(nd, link);

	if (unlikely(error)) {
		if (!(nd->flags & LOOKUP_RCU))
			path_put(link);
		return ERR_PTR(error);
	}
	last = nd->stack + nd->depth++;
	last->link = *link;
	clear_delayed_call(&last->done);
	last->seq = nd->next_seq;

	if (flags & WALK_TRAILING) {
		error = may_follow_link(nd, inode);
		if (unlikely(error))
			return ERR_PTR(error);
	}

	if (unlikely(nd->flags & LOOKUP_NO_SYMLINKS) ||
			unlikely(link->mnt->mnt_flags & MNT_NOSYMFOLLOW))
		return ERR_PTR(-ELOOP);

	if (unlikely(atime_needs_update(&last->link, inode))) {
		if (nd->flags & LOOKUP_RCU) {
			if (!try_to_unlazy(nd))
				return ERR_PTR(-ECHILD);
		}
		touch_atime(&last->link);
		cond_resched();
	}

	error = security_inode_follow_link(link->dentry, inode,
					   nd->flags & LOOKUP_RCU);
	if (unlikely(error))
		return ERR_PTR(error);

	res = READ_ONCE(inode->i_link);
	if (!res) {
		const char * (*get)(struct dentry *, struct inode *,
				struct delayed_call *);
		get = inode->i_op->get_link;
		if (nd->flags & LOOKUP_RCU) {
			res = get(NULL, inode, &last->done);
			if (res == ERR_PTR(-ECHILD) && try_to_unlazy(nd))
				res = get(link->dentry, inode, &last->done);
		} else {
			res = get(link->dentry, inode, &last->done);
		}
		if (!res)
			goto all_done;
		if (IS_ERR(res))
			return res;
	}
	if (*res == '/') {
		error = nd_jump_root(nd);
		if (unlikely(error))
			return ERR_PTR(error);
		while (unlikely(*++res == '/'))
			;
	}
	if (*res)
		return res;
all_done: // pure jump
	put_link(nd);
	return NULL;
}

/*
 * Do we need to follow links? We _really_ want to be able
 * to do this check without having to look at inode->i_op,
 * so we keep a cache of "no, this doesn't need follow_link"
 * for the common case.
 *
 * NOTE: dentry must be what nd->next_seq had been sampled from.
 */
static const char *step_into(struct nameidata *nd, int flags,
		     struct dentry *dentry)
{
	struct path path;
	struct inode *inode;
	int err = handle_mounts(nd, dentry, &path);

	if (err < 0)
		return ERR_PTR(err);
	inode = path.dentry->d_inode;
	if (likely(!d_is_symlink(path.dentry)) ||
	   ((flags & WALK_TRAILING) && !(nd->flags & LOOKUP_FOLLOW)) ||
	   (flags & WALK_NOFOLLOW)) {
		/* not a symlink or should not follow */
		if (nd->flags & LOOKUP_RCU) {
			if (read_seqcount_retry(&path.dentry->d_seq, nd->next_seq))
				return ERR_PTR(-ECHILD);
			if (unlikely(!inode))
				return ERR_PTR(-ENOENT);
		} else {
			dput(nd->path.dentry);
			if (nd->path.mnt != path.mnt)
				mntput(nd->path.mnt);
		}
		nd->path = path;
		nd->inode = inode;
		nd->seq = nd->next_seq;
		return NULL;
	}
	if (nd->flags & LOOKUP_RCU) {
		/* make sure that d_is_symlink above matches inode */
		if (read_seqcount_retry(&path.dentry->d_seq, nd->next_seq))
			return ERR_PTR(-ECHILD);
	} else {
		if (path.mnt == nd->path.mnt)
			mntget(path.mnt);
	}
	return pick_link(nd, &path, inode, flags);
}

static struct dentry *follow_dotdot_rcu(struct nameidata *nd)
{
	struct dentry *parent, *old;

	if (path_equal(&nd->path, &nd->root))
		goto in_root;
	if (unlikely(nd->path.dentry == nd->path.mnt->mnt_root)) {
		struct path path;
		unsigned seq;
		if (!choose_mountpoint_rcu(real_mount(nd->path.mnt),
					   &nd->root, &path, &seq))
			goto in_root;
		if (unlikely(nd->flags & LOOKUP_NO_XDEV))
			return ERR_PTR(-ECHILD);
		nd->path = path;
		nd->inode = path.dentry->d_inode;
		nd->seq = seq;
		// makes sure that non-RCU pathwalk could reach this state
		if (read_seqretry(&mount_lock, nd->m_seq))
			return ERR_PTR(-ECHILD);
		/* we know that mountpoint was pinned */
	}
	old = nd->path.dentry;
	parent = old->d_parent;
	nd->next_seq = read_seqcount_begin(&parent->d_seq);
	// makes sure that non-RCU pathwalk could reach this state
	if (read_seqcount_retry(&old->d_seq, nd->seq))
		return ERR_PTR(-ECHILD);
	if (unlikely(!path_connected(nd->path.mnt, parent)))
		return ERR_PTR(-ECHILD);
	return parent;
in_root:
	if (read_seqretry(&mount_lock, nd->m_seq))
		return ERR_PTR(-ECHILD);
	if (unlikely(nd->flags & LOOKUP_BENEATH))
		return ERR_PTR(-ECHILD);
	nd->next_seq = nd->seq;
	return nd->path.dentry;
}

static struct dentry *follow_dotdot(struct nameidata *nd)
{
	struct dentry *parent;

	if (path_equal(&nd->path, &nd->root))
		goto in_root;
	if (unlikely(nd->path.dentry == nd->path.mnt->mnt_root)) {
		struct path path;

		if (!choose_mountpoint(real_mount(nd->path.mnt),
				       &nd->root, &path))
			goto in_root;
		path_put(&nd->path);
		nd->path = path;
		nd->inode = path.dentry->d_inode;
		if (unlikely(nd->flags & LOOKUP_NO_XDEV))
			return ERR_PTR(-EXDEV);
	}
	/* rare case of legitimate dget_parent()... */
	parent = dget_parent(nd->path.dentry);
	if (unlikely(!path_connected(nd->path.mnt, parent))) {
		dput(parent);
		return ERR_PTR(-ENOENT);
	}
	return parent;

in_root:
	if (unlikely(nd->flags & LOOKUP_BENEATH))
		return ERR_PTR(-EXDEV);
	return dget(nd->path.dentry);
}

static const char *handle_dots(struct nameidata *nd, int type)
{
	if (type == LAST_DOTDOT) {
		const char *error = NULL;
		struct dentry *parent;

		if (!nd->root.mnt) {
			error = ERR_PTR(set_root(nd));
			if (error)
				return error;
		}
		if (nd->flags & LOOKUP_RCU)
			parent = follow_dotdot_rcu(nd);
		else
			parent = follow_dotdot(nd);
		if (IS_ERR(parent))
			return ERR_CAST(parent);
		error = step_into(nd, WALK_NOFOLLOW, parent);
		if (unlikely(error))
			return error;

		if (unlikely(nd->flags & LOOKUP_IS_SCOPED)) {
			/*
			 * If there was a racing rename or mount along our
			 * path, then we can't be sure that ".." hasn't jumped
			 * above nd->root (and so userspace should retry or use
			 * some fallback).
			 */
			smp_rmb();
			if (__read_seqcount_retry(&mount_lock.seqcount, nd->m_seq))
				return ERR_PTR(-EAGAIN);
			if (__read_seqcount_retry(&rename_lock.seqcount, nd->r_seq))
				return ERR_PTR(-EAGAIN);
		}
	}
	return NULL;
}

static const char *walk_component(struct nameidata *nd, int flags)
{
	struct dentry *dentry;
	/*
	 * "." and ".." are special - ".." especially so because it has
	 * to be able to know about the current root directory and
	 * parent relationships.
	 */
	if (unlikely(nd->last_type != LAST_NORM)) {
		if (!(flags & WALK_MORE) && nd->depth)
			put_link(nd);
		return handle_dots(nd, nd->last_type);
	}
	dentry = lookup_fast(nd);
	if (IS_ERR(dentry))
		return ERR_CAST(dentry);
	if (unlikely(!dentry)) {
		dentry = lookup_slow(&nd->last, nd->path.dentry, nd->flags);
		if (IS_ERR(dentry))
			return ERR_CAST(dentry);
	}
	if (!(flags & WALK_MORE) && nd->depth)
		put_link(nd);
	return step_into(nd, flags, dentry);
}

/*
 * We can do the critical dentry name comparison and hashing
 * operations one word at a time, but we are limited to:
 *
 * - Architectures with fast unaligned word accesses. We could
 *   do a "get_unaligned()" if this helps and is sufficiently
 *   fast.
 *
 * - non-CONFIG_DEBUG_PAGEALLOC configurations (so that we
 *   do not trap on the (extremely unlikely) case of a page
 *   crossing operation.
 *
 * - Furthermore, we need an efficient 64-bit compile for the
 *   64-bit case in order to generate the "number of bytes in
 *   the final mask". Again, that could be replaced with a
 *   efficient population count instruction or similar.
 */
#ifdef CONFIG_DCACHE_WORD_ACCESS

#include <asm/word-at-a-time.h>

#ifdef HASH_MIX

/* Architecture provides HASH_MIX and fold_hash() in <asm/hash.h> */

#elif defined(CONFIG_64BIT)
/*
 * Register pressure in the mixing function is an issue, particularly
 * on 32-bit x86, but almost any function requires one state value and
 * one temporary.  Instead, use a function designed for two state values
 * and no temporaries.
 *
 * This function cannot create a collision in only two iterations, so
 * we have two iterations to achieve avalanche.  In those two iterations,
 * we have six layers of mixing, which is enough to spread one bit's
 * influence out to 2^6 = 64 state bits.
 *
 * Rotate constants are scored by considering either 64 one-bit input
 * deltas or 64*63/2 = 2016 two-bit input deltas, and finding the
 * probability of that delta causing a change to each of the 128 output
 * bits, using a sample of random initial states.
 *
 * The Shannon entropy of the computed probabilities is then summed
 * to produce a score.  Ideally, any input change has a 50% chance of
 * toggling any given output bit.
 *
 * Mixing scores (in bits) for (12,45):
 * Input delta: 1-bit      2-bit
 * 1 round:     713.3    42542.6
 * 2 rounds:   2753.7   140389.8
 * 3 rounds:   5954.1   233458.2
 * 4 rounds:   7862.6   256672.2
 * Perfect:    8192     258048
 *            (64*128) (64*63/2 * 128)
 */
#define HASH_MIX(x, y, a)	\
	(	x ^= (a),	\
	y ^= x,	x = rol64(x,12),\
	x += y,	y = rol64(y,45),\
	y *= 9			)

/*
 * Fold two longs into one 32-bit hash value.  This must be fast, but
 * latency isn't quite as critical, as there is a fair bit of additional
 * work done before the hash value is used.
 */
static inline unsigned int fold_hash(unsigned long x, unsigned long y)
{
	y ^= x * GOLDEN_RATIO_64;
	y *= GOLDEN_RATIO_64;
	return y >> 32;
}

#else	/* 32-bit case */

/*
 * Mixing scores (in bits) for (7,20):
 * Input delta: 1-bit      2-bit
 * 1 round:     330.3     9201.6
 * 2 rounds:   1246.4    25475.4
 * 3 rounds:   1907.1    31295.1
 * 4 rounds:   2042.3    31718.6
 * Perfect:    2048      31744
 *            (32*64)   (32*31/2 * 64)
 */
#define HASH_MIX(x, y, a)	\
	(	x ^= (a),	\
	y ^= x,	x = rol32(x, 7),\
	x += y,	y = rol32(y,20),\
	y *= 9			)

static inline unsigned int fold_hash(unsigned long x, unsigned long y)
{
	/* Use arch-optimized multiply if one exists */
	return __hash_32(y ^ __hash_32(x));
}

#endif

/*
 * Return the hash of a string of known length.  This is carfully
 * designed to match hash_name(), which is the more critical function.
 * In particular, we must end by hashing a final word containing 0..7
 * payload bytes, to match the way that hash_name() iterates until it
 * finds the delimiter after the name.
 */
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len)
{
	unsigned long a, x = 0, y = (unsigned long)salt;

	for (;;) {
		if (!len)
			goto done;
		a = load_unaligned_zeropad(name);
		if (len < sizeof(unsigned long))
			break;
		HASH_MIX(x, y, a);
		name += sizeof(unsigned long);
		len -= sizeof(unsigned long);
	}
	x ^= a & bytemask_from_count(len);
done:
	return fold_hash(x, y);
}
EXPORT_SYMBOL(full_name_hash);

/* Return the "hash_len" (hash and length) of a null-terminated string */
u64 hashlen_string(const void *salt, const char *name)
{
	unsigned long a = 0, x = 0, y = (unsigned long)salt;
	unsigned long adata, mask, len;
	const struct word_at_a_time constants = WORD_AT_A_TIME_CONSTANTS;

	len = 0;
	goto inside;

	do {
		HASH_MIX(x, y, a);
		len += sizeof(unsigned long);
inside:
		a = load_unaligned_zeropad(name+len);
	} while (!has_zero(a, &adata, &constants));

	adata = prep_zero_mask(a, adata, &constants);
	mask = create_zero_mask(adata);
	x ^= a & zero_bytemask(mask);

	return hashlen_create(fold_hash(x, y), len + find_zero(mask));
}
EXPORT_SYMBOL(hashlen_string);

/*
 * Calculate the length and hash of the path component, and
 * return the length as the result.
 */
static inline const char *hash_name(struct nameidata *nd,
				    const char *name,
				    unsigned long *lastword)
{
	unsigned long a, b, x, y = (unsigned long)nd->path.dentry;
	unsigned long adata, bdata, mask, len;
	const struct word_at_a_time constants = WORD_AT_A_TIME_CONSTANTS;

	/*
	 * The first iteration is special, because it can result in
	 * '.' and '..' and has no mixing other than the final fold.
	 */
	a = load_unaligned_zeropad(name);
	b = a ^ REPEAT_BYTE('/');
	if (has_zero(a, &adata, &constants) | has_zero(b, &bdata, &constants)) {
		adata = prep_zero_mask(a, adata, &constants);
		bdata = prep_zero_mask(b, bdata, &constants);
		mask = create_zero_mask(adata | bdata);
		a &= zero_bytemask(mask);
		*lastword = a;
		len = find_zero(mask);
		nd->last.hash = fold_hash(a, y);
		nd->last.len = len;
		return name + len;
	}

	len = 0;
	x = 0;
	do {
		HASH_MIX(x, y, a);
		len += sizeof(unsigned long);
		a = load_unaligned_zeropad(name+len);
		b = a ^ REPEAT_BYTE('/');
	} while (!(has_zero(a, &adata, &constants) | has_zero(b, &bdata, &constants)));

	adata = prep_zero_mask(a, adata, &constants);
	bdata = prep_zero_mask(b, bdata, &constants);
	mask = create_zero_mask(adata | bdata);
	a &= zero_bytemask(mask);
	x ^= a;
	len += find_zero(mask);
	*lastword = 0;		// Multi-word components cannot be DOT or DOTDOT

	nd->last.hash = fold_hash(x, y);
	nd->last.len = len;
	return name + len;
}

/*
 * Note that the 'last' word is always zero-masked, but
 * was loaded as a possibly big-endian word.
 */
#ifdef __BIG_ENDIAN
  #define LAST_WORD_IS_DOT	(0x2eul << (BITS_PER_LONG-8))
  #define LAST_WORD_IS_DOTDOT	(0x2e2eul << (BITS_PER_LONG-16))
#endif

#else	/* !CONFIG_DCACHE_WORD_ACCESS: Slow, byte-at-a-time version */

/* Return the hash of a string of known length */
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len)
{
	unsigned long hash = init_name_hash(salt);
	while (len--)
		hash = partial_name_hash((unsigned char)*name++, hash);
	return end_name_hash(hash);
}
EXPORT_SYMBOL(full_name_hash);

/* Return the "hash_len" (hash and length) of a null-terminated string */
u64 hashlen_string(const void *salt, const char *name)
{
	unsigned long hash = init_name_hash(salt);
	unsigned long len = 0, c;

	c = (unsigned char)*name;
	while (c) {
		len++;
		hash = partial_name_hash(c, hash);
		c = (unsigned char)name[len];
	}
	return hashlen_create(end_name_hash(hash), len);
}
EXPORT_SYMBOL(hashlen_string);

/*
 * We know there's a real path component here of at least
 * one character.
 */
static inline const char *hash_name(struct nameidata *nd, const char *name, unsigned long *lastword)
{
	unsigned long hash = init_name_hash(nd->path.dentry);
	unsigned long len = 0, c, last = 0;

	c = (unsigned char)*name;
	do {
		last = (last << 8) + c;
		len++;
		hash = partial_name_hash(c, hash);
		c = (unsigned char)name[len];
	} while (c && c != '/');

	// This is reliable for DOT or DOTDOT, since the component
	// cannot contain NUL characters - top bits being zero means
	// we cannot have had any other pathnames.
	*lastword = last;
	nd->last.hash = end_name_hash(hash);
	nd->last.len = len;
	return name + len;
}

#endif

#ifndef LAST_WORD_IS_DOT
  #define LAST_WORD_IS_DOT	0x2e
  #define LAST_WORD_IS_DOTDOT	0x2e2e
#endif

/*
 * Name resolution.
 * This is the basic name resolution function, turning a pathname into
 * the final dentry. We expect 'base' to be positive and a directory.
 *
 * Returns 0 and nd will have valid dentry and mnt on success.
 * Returns error and drops reference to input namei data on failure.
 */
static int link_path_walk(const char *name, struct nameidata *nd)
{
	int depth = 0; // depth <= nd->depth
	int err;

	nd->last_type = LAST_ROOT;
	nd->flags |= LOOKUP_PARENT;
	if (IS_ERR(name))
		return PTR_ERR(name);
	if (*name == '/') {
		do {
			name++;
		} while (unlikely(*name == '/'));
	}
	if (unlikely(!*name)) {
		nd->dir_mode = 0; // short-circuit the 'hardening' idiocy
		return 0;
	}

	/* At this point we know we have a real path component. */
	for(;;) {
		struct mnt_idmap *idmap;
		const char *link;
		unsigned long lastword;

		idmap = mnt_idmap(nd->path.mnt);
		err = may_lookup(idmap, nd);
		if (unlikely(err))
			return err;

		nd->last.name = name;
		name = hash_name(nd, name, &lastword);

		switch(lastword) {
		case LAST_WORD_IS_DOTDOT:
			nd->last_type = LAST_DOTDOT;
			nd->state |= ND_JUMPED;
			break;

		case LAST_WORD_IS_DOT:
			nd->last_type = LAST_DOT;
			break;

		default:
			nd->last_type = LAST_NORM;
			nd->state &= ~ND_JUMPED;

			struct dentry *parent = nd->path.dentry;
			if (unlikely(parent->d_flags & DCACHE_OP_HASH)) {
				err = parent->d_op->d_hash(parent, &nd->last);
				if (err < 0)
					return err;
			}
		}

		if (!*name)
			goto OK;
		/*
		 * If it wasn't NUL, we know it was '/'. Skip that
		 * slash, and continue until no more slashes.
		 */
		do {
			name++;
		} while (unlikely(*name == '/'));
		if (unlikely(!*name)) {
OK:
			/* pathname or trailing symlink, done */
			if (!depth) {
				nd->dir_vfsuid = i_uid_into_vfsuid(idmap, nd->inode);
				nd->dir_mode = nd->inode->i_mode;
				nd->flags &= ~LOOKUP_PARENT;
				return 0;
			}
			/* last component of nested symlink */
			name = nd->stack[--depth].name;
			link = walk_component(nd, 0);
		} else {
			/* not the last component */
			link = walk_component(nd, WALK_MORE);
		}
		if (unlikely(link)) {
			if (IS_ERR(link))
				return PTR_ERR(link);
			/* a symlink to follow */
			nd->stack[depth++].name = name;
			name = link;
			continue;
		}
		if (unlikely(!d_can_lookup(nd->path.dentry))) {
			if (nd->flags & LOOKUP_RCU) {
				if (!try_to_unlazy(nd))
					return -ECHILD;
			}
			return -ENOTDIR;
		}
	}
}

/* must be paired with terminate_walk() */
static const char *path_init(struct nameidata *nd, unsigned flags)
{
	int error;
	const char *s = nd->pathname;

	/* LOOKUP_CACHED requires RCU, ask caller to retry */
	if ((flags & (LOOKUP_RCU | LOOKUP_CACHED)) == LOOKUP_CACHED)
		return ERR_PTR(-EAGAIN);

	if (!*s)
		flags &= ~LOOKUP_RCU;
	if (flags & LOOKUP_RCU)
		rcu_read_lock();
	else
		nd->seq = nd->next_seq = 0;

	nd->flags = flags;
	nd->state |= ND_JUMPED;

	nd->m_seq = __read_seqcount_begin(&mount_lock.seqcount);
	nd->r_seq = __read_seqcount_begin(&rename_lock.seqcount);
	smp_rmb();

	if (nd->state & ND_ROOT_PRESET) {
		struct dentry *root = nd->root.dentry;
		struct inode *inode = root->d_inode;
		if (*s && unlikely(!d_can_lookup(root)))
			return ERR_PTR(-ENOTDIR);
		nd->path = nd->root;
		nd->inode = inode;
		if (flags & LOOKUP_RCU) {
			nd->seq = read_seqcount_begin(&nd->path.dentry->d_seq);
			nd->root_seq = nd->seq;
		} else {
			path_get(&nd->path);
		}
		return s;
	}

	nd->root.mnt = NULL;

	/* Absolute pathname -- fetch the root (LOOKUP_IN_ROOT uses nd->dfd). */
	if (*s == '/' && !(flags & LOOKUP_IN_ROOT)) {
		error = nd_jump_root(nd);
		if (unlikely(error))
			return ERR_PTR(error);
		return s;
	}

	/* Relative pathname -- get the starting-point it is relative to. */
	if (nd->dfd == AT_FDCWD) {
		if (flags & LOOKUP_RCU) {
			struct fs_struct *fs = current->fs;
			unsigned seq;

			do {
				seq = read_seqbegin(&fs->seq);
				nd->path = fs->pwd;
				nd->inode = nd->path.dentry->d_inode;
				nd->seq = __read_seqcount_begin(&nd->path.dentry->d_seq);
			} while (read_seqretry(&fs->seq, seq));
		} else {
			get_fs_pwd(current->fs, &nd->path);
			nd->inode = nd->path.dentry->d_inode;
		}
	} else {
		/* Caller must check execute permissions on the starting path component */
		CLASS(fd_raw, f)(nd->dfd);
		struct dentry *dentry;

		if (fd_empty(f))
			return ERR_PTR(-EBADF);

		if (flags & LOOKUP_LINKAT_EMPTY) {
			if (fd_file(f)->f_cred != current_cred() &&
			    !ns_capable(fd_file(f)->f_cred->user_ns, CAP_DAC_READ_SEARCH))
				return ERR_PTR(-ENOENT);
		}

		dentry = fd_file(f)->f_path.dentry;

		if (*s && unlikely(!d_can_lookup(dentry)))
			return ERR_PTR(-ENOTDIR);

		nd->path = fd_file(f)->f_path;
		if (flags & LOOKUP_RCU) {
			nd->inode = nd->path.dentry->d_inode;
			nd->seq = read_seqcount_begin(&nd->path.dentry->d_seq);
		} else {
			path_get(&nd->path);
			nd->inode = nd->path.dentry->d_inode;
		}
	}

	/* For scoped-lookups we need to set the root to the dirfd as well. */
	if (flags & LOOKUP_IS_SCOPED) {
		nd->root = nd->path;
		if (flags & LOOKUP_RCU) {
			nd->root_seq = nd->seq;
		} else {
			path_get(&nd->root);
			nd->state |= ND_ROOT_GRABBED;
		}
	}
	return s;
}

static inline const char *lookup_last(struct nameidata *nd)
{
	if (nd->last_type == LAST_NORM && nd->last.name[nd->last.len])
		nd->flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;

	return walk_component(nd, WALK_TRAILING);
}

static int handle_lookup_down(struct nameidata *nd)
{
	if (!(nd->flags & LOOKUP_RCU))
		dget(nd->path.dentry);
	nd->next_seq = nd->seq;
	return PTR_ERR(step_into(nd, WALK_NOFOLLOW, nd->path.dentry));
}

/* Returns 0 and nd will be valid on success; Returns error, otherwise. */
static int path_lookupat(struct nameidata *nd, unsigned flags, struct path *path)
{
	const char *s = path_init(nd, flags);
	int err;

	if (unlikely(flags & LOOKUP_DOWN) && !IS_ERR(s)) {
		err = handle_lookup_down(nd);
		if (unlikely(err < 0))
			s = ERR_PTR(err);
	}

	while (!(err = link_path_walk(s, nd)) &&
	       (s = lookup_last(nd)) != NULL)
		;
	if (!err && unlikely(nd->flags & LOOKUP_MOUNTPOINT)) {
		err = handle_lookup_down(nd);
		nd->state &= ~ND_JUMPED; // no d_weak_revalidate(), please...
	}
	if (!err)
		err = complete_walk(nd);

	if (!err && nd->flags & LOOKUP_DIRECTORY)
		if (!d_can_lookup(nd->path.dentry))
			err = -ENOTDIR;
	if (!err) {
		*path = nd->path;
		nd->path.mnt = NULL;
		nd->path.dentry = NULL;
	}
	terminate_walk(nd);
	return err;
}

int filename_lookup(int dfd, struct filename *name, unsigned flags,
		    struct path *path, struct path *root)
{
	int retval;
	struct nameidata nd;
	if (IS_ERR(name))
		return PTR_ERR(name);
	set_nameidata(&nd, dfd, name, root);
	retval = path_lookupat(&nd, flags | LOOKUP_RCU, path);
	if (unlikely(retval == -ECHILD))
		retval = path_lookupat(&nd, flags, path);
	if (unlikely(retval == -ESTALE))
		retval = path_lookupat(&nd, flags | LOOKUP_REVAL, path);

	if (likely(!retval))
		audit_inode(name, path->dentry,
			    flags & LOOKUP_MOUNTPOINT ? AUDIT_INODE_NOEVAL : 0);
	restore_nameidata();
	return retval;
}

/* Returns 0 and nd will be valid on success; Returns error, otherwise. */
static int path_parentat(struct nameidata *nd, unsigned flags,
				struct path *parent)
{
	const char *s = path_init(nd, flags);
	int err = link_path_walk(s, nd);
	if (!err)
		err = complete_walk(nd);
	if (!err) {
		*parent = nd->path;
		nd->path.mnt = NULL;
		nd->path.dentry = NULL;
	}
	terminate_walk(nd);
	return err;
}

/* Note: this does not consume "name" */
static int __filename_parentat(int dfd, struct filename *name,
			       unsigned int flags, struct path *parent,
			       struct qstr *last, int *type,
			       const struct path *root)
{
	int retval;
	struct nameidata nd;

	if (IS_ERR(name))
		return PTR_ERR(name);
	set_nameidata(&nd, dfd, name, root);
	retval = path_parentat(&nd, flags | LOOKUP_RCU, parent);
	if (unlikely(retval == -ECHILD))
		retval = path_parentat(&nd, flags, parent);
	if (unlikely(retval == -ESTALE))
		retval = path_parentat(&nd, flags | LOOKUP_REVAL, parent);
	if (likely(!retval)) {
		*last = nd.last;
		*type = nd.last_type;
		audit_inode(name, parent->dentry, AUDIT_INODE_PARENT);
	}
	restore_nameidata();
	return retval;
}

static int filename_parentat(int dfd, struct filename *name,
			     unsigned int flags, struct path *parent,
			     struct qstr *last, int *type)
{
	return __filename_parentat(dfd, name, flags, parent, last, type, NULL);
}

/* does lookup, returns the object with parent locked */
static struct dentry *__kern_path_locked(int dfd, struct filename *name, struct path *path)
{
	struct path parent_path __free(path_put) = {};
	struct dentry *d;
	struct qstr last;
	int type, error;

	error = filename_parentat(dfd, name, 0, &parent_path, &last, &type);
	if (error)
		return ERR_PTR(error);
	if (unlikely(type != LAST_NORM))
		return ERR_PTR(-EINVAL);
	inode_lock_nested(parent_path.dentry->d_inode, I_MUTEX_PARENT);
	d = lookup_one_qstr_excl(&last, parent_path.dentry, 0);
	if (IS_ERR(d)) {
		inode_unlock(parent_path.dentry->d_inode);
		return d;
	}
	path->dentry = no_free_ptr(parent_path.dentry);
	path->mnt = no_free_ptr(parent_path.mnt);
	return d;
}

struct dentry *kern_path_locked_negative(const char *name, struct path *path)
{
	struct path parent_path __free(path_put) = {};
	struct filename *filename __free(putname) = getname_kernel(name);
	struct dentry *d;
	struct qstr last;
	int type, error;

	error = filename_parentat(AT_FDCWD, filename, 0, &parent_path, &last, &type);
	if (error)
		return ERR_PTR(error);
	if (unlikely(type != LAST_NORM))
		return ERR_PTR(-EINVAL);
	inode_lock_nested(parent_path.dentry->d_inode, I_MUTEX_PARENT);
	d = lookup_one_qstr_excl(&last, parent_path.dentry, LOOKUP_CREATE);
	if (IS_ERR(d)) {
		inode_unlock(parent_path.dentry->d_inode);
		return d;
	}
	path->dentry = no_free_ptr(parent_path.dentry);
	path->mnt = no_free_ptr(parent_path.mnt);
	return d;
}

struct dentry *kern_path_locked(const char *name, struct path *path)
{
	struct filename *filename = getname_kernel(name);
	struct dentry *res = __kern_path_locked(AT_FDCWD, filename, path);

	putname(filename);
	return res;
}

struct dentry *user_path_locked_at(int dfd, const char __user *name, struct path *path)
{
	struct filename *filename = getname(name);
	struct dentry *res = __kern_path_locked(dfd, filename, path);

	putname(filename);
	return res;
}
EXPORT_SYMBOL(user_path_locked_at);

int kern_path(const char *name, unsigned int flags, struct path *path)
{
	struct filename *filename = getname_kernel(name);
	int ret = filename_lookup(AT_FDCWD, filename, flags, path, NULL);

	putname(filename);
	return ret;

}
EXPORT_SYMBOL(kern_path);

/**
 * vfs_path_parent_lookup - lookup a parent path relative to a dentry-vfsmount pair
 * @filename: filename structure
 * @flags: lookup flags
 * @parent: pointer to struct path to fill
 * @last: last component
 * @type: type of the last component
 * @root: pointer to struct path of the base directory
 */
int vfs_path_parent_lookup(struct filename *filename, unsigned int flags,
			   struct path *parent, struct qstr *last, int *type,
			   const struct path *root)
{
	return  __filename_parentat(AT_FDCWD, filename, flags, parent, last,
				    type, root);
}
EXPORT_SYMBOL(vfs_path_parent_lookup);

/**
 * vfs_path_lookup - lookup a file path relative to a dentry-vfsmount pair
 * @dentry:  pointer to dentry of the base directory
 * @mnt: pointer to vfs mount of the base directory
 * @name: pointer to file name
 * @flags: lookup flags
 * @path: pointer to struct path to fill
 */
int vfs_path_lookup(struct dentry *dentry, struct vfsmount *mnt,
		    const char *name, unsigned int flags,
		    struct path *path)
{
	struct filename *filename;
	struct path root = {.mnt = mnt, .dentry = dentry};
	int ret;

	filename = getname_kernel(name);
	/* the first argument of filename_lookup() is ignored with root */
	ret = filename_lookup(AT_FDCWD, filename, flags, path, &root);
	putname(filename);
	return ret;
}
EXPORT_SYMBOL(vfs_path_lookup);

static int lookup_noperm_common(struct qstr *qname, struct dentry *base)
{
	const char *name = qname->name;
	u32 len = qname->len;

	qname->hash = full_name_hash(base, name, len);
	if (!len)
		return -EACCES;

	if (is_dot_dotdot(name, len))
		return -EACCES;

	while (len--) {
		unsigned int c = *(const unsigned char *)name++;
		if (c == '/' || c == '\0')
			return -EACCES;
	}
	/*
	 * See if the low-level filesystem might want
	 * to use its own hash..
	 */
	if (base->d_flags & DCACHE_OP_HASH) {
		int err = base->d_op->d_hash(base, qname);
		if (err < 0)
			return err;
	}
	return 0;
}

static int lookup_one_common(struct mnt_idmap *idmap,
			     struct qstr *qname, struct dentry *base)
{
	int err;
	err = lookup_noperm_common(qname, base);
	if (err < 0)
		return err;
	return inode_permission(idmap, base->d_inode, MAY_EXEC);
}

/**
 * try_lookup_noperm - filesystem helper to lookup single pathname component
 * @name:	qstr storing pathname component to lookup
 * @base:	base directory to lookup from
 *
 * Look up a dentry by name in the dcache, returning NULL if it does not
 * currently exist.  The function does not try to create a dentry and if one
 * is found it doesn't try to revalidate it.
 *
 * Note that this routine is purely a helper for filesystem usage and should
 * not be called by generic code.  It does no permission checking.
 *
 * No locks need be held - only a counted reference to @base is needed.
 *
 */
struct dentry *try_lookup_noperm(struct qstr *name, struct dentry *base)
{
	int err;

	err = lookup_noperm_common(name, base);
	if (err)
		return ERR_PTR(err);

	return d_lookup(base, name);
}
EXPORT_SYMBOL(try_lookup_noperm);

/**
 * lookup_noperm - filesystem helper to lookup single pathname component
 * @name:	qstr storing pathname component to lookup
 * @base:	base directory to lookup from
 *
 * Note that this routine is purely a helper for filesystem usage and should
 * not be called by generic code.  It does no permission checking.
 *
 * The caller must hold base->i_rwsem.
 */
struct dentry *lookup_noperm(struct qstr *name, struct dentry *base)
{
	struct dentry *dentry;
	int err;

	WARN_ON_ONCE(!inode_is_locked(base->d_inode));

	err = lookup_noperm_common(name, base);
	if (err)
		return ERR_PTR(err);

	dentry = lookup_dcache(name, base, 0);
	return dentry ? dentry : __lookup_slow(name, base, 0);
}
EXPORT_SYMBOL(lookup_noperm);

/**
 * lookup_one - lookup single pathname component
 * @idmap:	idmap of the mount the lookup is performed from
 * @name:	qstr holding pathname component to lookup
 * @base:	base directory to lookup from
 *
 * This can be used for in-kernel filesystem clients such as file servers.
 *
 * The caller must hold base->i_rwsem.
 */
struct dentry *lookup_one(struct mnt_idmap *idmap, struct qstr *name,
			  struct dentry *base)
{
	struct dentry *dentry;
	int err;

	WARN_ON_ONCE(!inode_is_locked(base->d_inode));

	err = lookup_one_common(idmap, name, base);
	if (err)
		return ERR_PTR(err);

	dentry = lookup_dcache(name, base, 0);
	return dentry ? dentry : __lookup_slow(name, base, 0);
}
EXPORT_SYMBOL(lookup_one);

/**
 * lookup_one_unlocked - lookup single pathname component
 * @idmap:	idmap of the mount the lookup is performed from
 * @name:	qstr olding pathname component to lookup
 * @base:	base directory to lookup from
 *
 * This can be used for in-kernel filesystem clients such as file servers.
 *
 * Unlike lookup_one, it should be called without the parent
 * i_rwsem held, and will take the i_rwsem itself if necessary.
 */
struct dentry *lookup_one_unlocked(struct mnt_idmap *idmap, struct qstr *name,
				   struct dentry *base)
{
	int err;
	struct dentry *ret;

	err = lookup_one_common(idmap, name, base);
	if (err)
		return ERR_PTR(err);

	ret = lookup_dcache(name, base, 0);
	if (!ret)
		ret = lookup_slow(name, base, 0);
	return ret;
}
EXPORT_SYMBOL(lookup_one_unlocked);

/**
 * lookup_one_positive_unlocked - lookup single pathname component
 * @idmap:	idmap of the mount the lookup is performed from
 * @name:	qstr holding pathname component to lookup
 * @base:	base directory to lookup from
 *
 * This helper will yield ERR_PTR(-ENOENT) on negatives. The helper returns
 * known positive or ERR_PTR(). This is what most of the users want.
 *
 * Note that pinned negative with unlocked parent _can_ become positive at any
 * time, so callers of lookup_one_unlocked() need to be very careful; pinned
 * positives have >d_inode stable, so this one avoids such problems.
 *
 * This can be used for in-kernel filesystem clients such as file servers.
 *
 * The helper should be called without i_rwsem held.
 */
struct dentry *lookup_one_positive_unlocked(struct mnt_idmap *idmap,
					    struct qstr *name,
					    struct dentry *base)
{
	struct dentry *ret = lookup_one_unlocked(idmap, name, base);

	if (!IS_ERR(ret) && d_flags_negative(smp_load_acquire(&ret->d_flags))) {
		dput(ret);
		ret = ERR_PTR(-ENOENT);
	}
	return ret;
}
EXPORT_SYMBOL(lookup_one_positive_unlocked);

/**
 * lookup_noperm_unlocked - filesystem helper to lookup single pathname component
 * @name:	pathname component to lookup
 * @base:	base directory to lookup from
 *
 * Note that this routine is purely a helper for filesystem usage and should
 * not be called by generic code. It does no permission checking.
 *
 * Unlike lookup_noperm(), it should be called without the parent
 * i_rwsem held, and will take the i_rwsem itself if necessary.
 *
 * Unlike try_lookup_noperm() it *does* revalidate the dentry if it already
 * existed.
 */
struct dentry *lookup_noperm_unlocked(struct qstr *name, struct dentry *base)
{
	struct dentry *ret;
	int err;

	err = lookup_noperm_common(name, base);
	if (err)
		return ERR_PTR(err);

	ret = lookup_dcache(name, base, 0);
	if (!ret)
		ret = lookup_slow(name, base, 0);
	return ret;
}
EXPORT_SYMBOL(lookup_noperm_unlocked);

/*
 * Like lookup_noperm_unlocked(), except that it yields ERR_PTR(-ENOENT)
 * on negatives.  Returns known positive or ERR_PTR(); that's what
 * most of the users want.  Note that pinned negative with unlocked parent
 * _can_ become positive at any time, so callers of lookup_noperm_unlocked()
 * need to be very careful; pinned positives have ->d_inode stable, so
 * this one avoids such problems.
 */
struct dentry *lookup_noperm_positive_unlocked(struct qstr *name,
					       struct dentry *base)
{
	struct dentry *ret;

	ret = lookup_noperm_unlocked(name, base);
	if (!IS_ERR(ret) && d_flags_negative(smp_load_acquire(&ret->d_flags))) {
		dput(ret);
		ret = ERR_PTR(-ENOENT);
	}
	return ret;
}
EXPORT_SYMBOL(lookup_noperm_positive_unlocked);

#ifdef CONFIG_UNIX98_PTYS
int path_pts(struct path *path)
{
	/* Find something mounted on "pts" in the same directory as
	 * the input path.
	 */
	struct dentry *parent = dget_parent(path->dentry);
	struct dentry *child;
	struct qstr this = QSTR_INIT("pts", 3);

	if (unlikely(!path_connected(path->mnt, parent))) {
		dput(parent);
		return -ENOENT;
	}
	dput(path->dentry);
	path->dentry = parent;
	child = d_hash_and_lookup(parent, &this);
	if (IS_ERR_OR_NULL(child))
		return -ENOENT;

	path->dentry = child;
	dput(parent);
	follow_down(path, 0);
	return 0;
}
#endif

int user_path_at(int dfd, const char __user *name, unsigned flags,
		 struct path *path)
{
	struct filename *filename = getname_flags(name, flags);
	int ret = filename_lookup(dfd, filename, flags, path, NULL);

	putname(filename);
	return ret;
}
EXPORT_SYMBOL(user_path_at);

int __check_sticky(struct mnt_idmap *idmap, struct inode *dir,
		   struct inode *inode)
{
	kuid_t fsuid = current_fsuid();

	if (vfsuid_eq_kuid(i_uid_into_vfsuid(idmap, inode), fsuid))
		return 0;
	if (vfsuid_eq_kuid(i_uid_into_vfsuid(idmap, dir), fsuid))
		return 0;
	return !capable_wrt_inode_uidgid(idmap, inode, CAP_FOWNER);
}
EXPORT_SYMBOL(__check_sticky);

/*
 *	Check whether we can remove a link victim from directory dir, check
 *  whether the type of victim is right.
 *  1. We can't do it if dir is read-only (done in permission())
 *  2. We should have write and exec permissions on dir
 *  3. We can't remove anything from append-only dir
 *  4. We can't do anything with immutable dir (done in permission())
 *  5. If the sticky bit on dir is set we should either
 *	a. be owner of dir, or
 *	b. be owner of victim, or
 *	c. have CAP_FOWNER capability
 *  6. If the victim is append-only or immutable we can't do antyhing with
 *     links pointing to it.
 *  7. If the victim has an unknown uid or gid we can't change the inode.
 *  8. If we were asked to remove a directory and victim isn't one - ENOTDIR.
 *  9. If we were asked to remove a non-directory and victim isn't one - EISDIR.
 * 10. We can't remove a root or mountpoint.
 * 11. We don't allow removal of NFS sillyrenamed files; it's handled by
 *     nfs_async_unlink().
 */
static int may_delete(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *victim, bool isdir)
{
	struct inode *inode = d_backing_inode(victim);
	int error;

	if (d_is_negative(victim))
		return -ENOENT;
	BUG_ON(!inode);

	BUG_ON(victim->d_parent->d_inode != dir);

	/* Inode writeback is not safe when the uid or gid are invalid. */
	if (!vfsuid_valid(i_uid_into_vfsuid(idmap, inode)) ||
	    !vfsgid_valid(i_gid_into_vfsgid(idmap, inode)))
		return -EOVERFLOW;

	audit_inode_child(dir, victim, AUDIT_TYPE_CHILD_DELETE);

	error = inode_permission(idmap, dir, MAY_WRITE | MAY_EXEC);
	if (error)
		return error;
	if (IS_APPEND(dir))
		return -EPERM;

	if (check_sticky(idmap, dir, inode) || IS_APPEND(inode) ||
	    IS_IMMUTABLE(inode) || IS_SWAPFILE(inode) ||
	    HAS_UNMAPPED_ID(idmap, inode))
		return -EPERM;
	if (isdir) {
		if (!d_is_dir(victim))
			return -ENOTDIR;
		if (IS_ROOT(victim))
			return -EBUSY;
	} else if (d_is_dir(victim))
		return -EISDIR;
	if (IS_DEADDIR(dir))
		return -ENOENT;
	if (victim->d_flags & DCACHE_NFSFS_RENAMED)
		return -EBUSY;
	return 0;
}

/*	Check whether we can create an object with dentry child in directory
 *  dir.
 *  1. We can't do it if child already exists (open has special treatment for
 *     this case, but since we are inlined it's OK)
 *  2. We can't do it if dir is read-only (done in permission())
 *  3. We can't do it if the fs can't represent the fsuid or fsgid.
 *  4. We should have write and exec permissions on dir
 *  5. We can't do it if dir is immutable (done in permission())
 */
static inline int may_create(struct mnt_idmap *idmap,
			     struct inode *dir, struct dentry *child)
{
	audit_inode_child(dir, child, AUDIT_TYPE_CHILD_CREATE);
	if (child->d_inode)
		return -EEXIST;
	if (IS_DEADDIR(dir))
		return -ENOENT;
	if (!fsuidgid_has_mapping(dir->i_sb, idmap))
		return -EOVERFLOW;

	return inode_permission(idmap, dir, MAY_WRITE | MAY_EXEC);
}

// p1 != p2, both are on the same filesystem, ->s_vfs_rename_mutex is held
static struct dentry *lock_two_directories(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p = p1, *q = p2, *r;

	while ((r = p->d_parent) != p2 && r != p)
		p = r;
	if (r == p2) {
		// p is a child of p2 and an ancestor of p1 or p1 itself
		inode_lock_nested(p2->d_inode, I_MUTEX_PARENT);
		inode_lock_nested(p1->d_inode, I_MUTEX_PARENT2);
		return p;
	}
	// p is the root of connected component that contains p1
	// p2 does not occur on the path from p to p1
	while ((r = q->d_parent) != p1 && r != p && r != q)
		q = r;
	if (r == p1) {
		// q is a child of p1 and an ancestor of p2 or p2 itself
		inode_lock_nested(p1->d_inode, I_MUTEX_PARENT);
		inode_lock_nested(p2->d_inode, I_MUTEX_PARENT2);
		return q;
	} else if (likely(r == p)) {
		// both p2 and p1 are descendents of p
		inode_lock_nested(p1->d_inode, I_MUTEX_PARENT);
		inode_lock_nested(p2->d_inode, I_MUTEX_PARENT2);
		return NULL;
	} else { // no common ancestor at the time we'd been called
		mutex_unlock(&p1->d_sb->s_vfs_rename_mutex);
		return ERR_PTR(-EXDEV);
	}
}

/*
 * p1 and p2 should be directories on the same fs.
 */
struct dentry *lock_rename(struct dentry *p1, struct dentry *p2)
{
	if (p1 == p2) {
		inode_lock_nested(p1->d_inode, I_MUTEX_PARENT);
		return NULL;
	}

	mutex_lock(&p1->d_sb->s_vfs_rename_mutex);
	return lock_two_directories(p1, p2);
}
EXPORT_SYMBOL(lock_rename);

/*
 * c1 and p2 should be on the same fs.
 */
struct dentry *lock_rename_child(struct dentry *c1, struct dentry *p2)
{
	if (READ_ONCE(c1->d_parent) == p2) {
		/*
		 * hopefully won't need to touch ->s_vfs_rename_mutex at all.
		 */
		inode_lock_nested(p2->d_inode, I_MUTEX_PARENT);
		/*
		 * now that p2 is locked, nobody can move in or out of it,
		 * so the test below is safe.
		 */
		if (likely(c1->d_parent == p2))
			return NULL;

		/*
		 * c1 got moved out of p2 while we'd been taking locks;
		 * unlock and fall back to slow case.
		 */
		inode_unlock(p2->d_inode);
	}

	mutex_lock(&c1->d_sb->s_vfs_rename_mutex);
	/*
	 * nobody can move out of any directories on this fs.
	 */
	if (likely(c1->d_parent != p2))
		return lock_two_directories(c1->d_parent, p2);

	/*
	 * c1 got moved into p2 while we were taking locks;
	 * we need p2 locked and ->s_vfs_rename_mutex unlocked,
	 * for consistency with lock_rename().
	 */
	inode_lock_nested(p2->d_inode, I_MUTEX_PARENT);
	mutex_unlock(&c1->d_sb->s_vfs_rename_mutex);
	return NULL;
}
EXPORT_SYMBOL(lock_rename_child);

void unlock_rename(struct dentry *p1, struct dentry *p2)
{
	inode_unlock(p1->d_inode);
	if (p1 != p2) {
		inode_unlock(p2->d_inode);
		mutex_unlock(&p1->d_sb->s_vfs_rename_mutex);
	}
}
EXPORT_SYMBOL(unlock_rename);

/**
 * vfs_prepare_mode - prepare the mode to be used for a new inode
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	parent directory of the new inode
 * @mode:	mode of the new inode
 * @mask_perms:	allowed permission by the vfs
 * @type:	type of file to be created
 *
 * This helper consolidates and enforces vfs restrictions on the @mode of a new
 * object to be created.
 *
 * Umask stripping depends on whether the filesystem supports POSIX ACLs (see
 * the kernel documentation for mode_strip_umask()). Moving umask stripping
 * after setgid stripping allows the same ordering for both non-POSIX ACL and
 * POSIX ACL supporting filesystems.
 *
 * Note that it's currently valid for @type to be 0 if a directory is created.
 * Filesystems raise that flag individually and we need to check whether each
 * filesystem can deal with receiving S_IFDIR from the vfs before we enforce a
 * non-zero type.
 *
 * Returns: mode to be passed to the filesystem
 */
static inline umode_t vfs_prepare_mode(struct mnt_idmap *idmap,
				       const struct inode *dir, umode_t mode,
				       umode_t mask_perms, umode_t type)
{
	mode = mode_strip_sgid(idmap, dir, mode);
	mode = mode_strip_umask(dir, mode);

	/*
	 * Apply the vfs mandated allowed permission mask and set the type of
	 * file to be created before we call into the filesystem.
	 */
	mode &= (mask_perms & ~S_IFMT);
	mode |= (type & S_IFMT);

	return mode;
}

/**
 * vfs_create - create new file
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	inode of the parent directory
 * @dentry:	dentry of the child file
 * @mode:	mode of the child file
 * @want_excl:	whether the file must not yet exist
 *
 * Create a new file.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_create(struct mnt_idmap *idmap, struct inode *dir,
	       struct dentry *dentry, umode_t mode, bool want_excl)
{
	int error;

	error = may_create(idmap, dir, dentry);
	if (error)
		return error;

	if (!dir->i_op->create)
		return -EACCES;	/* shouldn't it be ENOSYS? */

	mode = vfs_prepare_mode(idmap, dir, mode, S_IALLUGO, S_IFREG);
	error = security_inode_create(dir, dentry, mode);
	if (error)
		return error;
	error = dir->i_op->create(idmap, dir, dentry, mode, want_excl);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_create);

int vfs_mkobj(struct dentry *dentry, umode_t mode,
		int (*f)(struct dentry *, umode_t, void *),
		void *arg)
{
	struct inode *dir = dentry->d_parent->d_inode;
	int error = may_create(&nop_mnt_idmap, dir, dentry);
	if (error)
		return error;

	mode &= S_IALLUGO;
	mode |= S_IFREG;
	error = security_inode_create(dir, dentry, mode);
	if (error)
		return error;
	error = f(dentry, mode, arg);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_mkobj);

bool may_open_dev(const struct path *path)
{
	return !(path->mnt->mnt_flags & MNT_NODEV) &&
		!(path->mnt->mnt_sb->s_iflags & SB_I_NODEV);
}

static int may_open(struct mnt_idmap *idmap, const struct path *path,
		    int acc_mode, int flag)
{
	struct dentry *dentry = path->dentry;
	struct inode *inode = dentry->d_inode;
	int error;

	if (!inode)
		return -ENOENT;

	switch (inode->i_mode & S_IFMT) {
	case S_IFLNK:
		return -ELOOP;
	case S_IFDIR:
		if (acc_mode & MAY_WRITE)
			return -EISDIR;
		if (acc_mode & MAY_EXEC)
			return -EACCES;
		break;
	case S_IFBLK:
	case S_IFCHR:
		if (!may_open_dev(path))
			return -EACCES;
		fallthrough;
	case S_IFIFO:
	case S_IFSOCK:
		if (acc_mode & MAY_EXEC)
			return -EACCES;
		flag &= ~O_TRUNC;
		break;
	case S_IFREG:
		if ((acc_mode & MAY_EXEC) && path_noexec(path))
			return -EACCES;
		break;
	default:
		VFS_BUG_ON_INODE(!IS_ANON_FILE(inode), inode);
	}

	error = inode_permission(idmap, inode, MAY_OPEN | acc_mode);
	if (error)
		return error;

	/*
	 * An append-only file must be opened in append mode for writing.
	 */
	if (IS_APPEND(inode)) {
		if  ((flag & O_ACCMODE) != O_RDONLY && !(flag & O_APPEND))
			return -EPERM;
		if (flag & O_TRUNC)
			return -EPERM;
	}

	/* O_NOATIME can only be set by the owner or superuser */
	if (flag & O_NOATIME && !inode_owner_or_capable(idmap, inode))
		return -EPERM;

	return 0;
}

static int handle_truncate(struct mnt_idmap *idmap, struct file *filp)
{
	const struct path *path = &filp->f_path;
	struct inode *inode = path->dentry->d_inode;
	int error = get_write_access(inode);
	if (error)
		return error;

	error = security_file_truncate(filp);
	if (!error) {
		error = do_truncate(idmap, path->dentry, 0,
				    ATTR_MTIME|ATTR_CTIME|ATTR_OPEN,
				    filp);
	}
	put_write_access(inode);
	return error;
}

static inline int open_to_namei_flags(int flag)
{
	if ((flag & O_ACCMODE) == 3)
		flag--;
	return flag;
}

static int may_o_create(struct mnt_idmap *idmap,
			const struct path *dir, struct dentry *dentry,
			umode_t mode)
{
	int error = security_path_mknod(dir, dentry, mode, 0);
	if (error)
		return error;

	if (!fsuidgid_has_mapping(dir->dentry->d_sb, idmap))
		return -EOVERFLOW;

	error = inode_permission(idmap, dir->dentry->d_inode,
				 MAY_WRITE | MAY_EXEC);
	if (error)
		return error;

	return security_inode_create(dir->dentry->d_inode, dentry, mode);
}

/*
 * Attempt to atomically look up, create and open a file from a negative
 * dentry.
 *
 * Returns 0 if successful.  The file will have been created and attached to
 * @file by the filesystem calling finish_open().
 *
 * If the file was looked up only or didn't need creating, FMODE_OPENED won't
 * be set.  The caller will need to perform the open themselves.  @path will
 * have been updated to point to the new dentry.  This may be negative.
 *
 * Returns an error code otherwise.
 */
static struct dentry *atomic_open(struct nameidata *nd, struct dentry *dentry,
				  struct file *file,
				  int open_flag, umode_t mode)
{
	struct dentry *const DENTRY_NOT_SET = (void *) -1UL;
	struct inode *dir =  nd->path.dentry->d_inode;
	int error;

	if (nd->flags & LOOKUP_DIRECTORY)
		open_flag |= O_DIRECTORY;

	file->f_path.dentry = DENTRY_NOT_SET;
	file->f_path.mnt = nd->path.mnt;
	error = dir->i_op->atomic_open(dir, dentry, file,
				       open_to_namei_flags(open_flag), mode);
	d_lookup_done(dentry);
	if (!error) {
		if (file->f_mode & FMODE_OPENED) {
			if (unlikely(dentry != file->f_path.dentry)) {
				dput(dentry);
				dentry = dget(file->f_path.dentry);
			}
		} else if (WARN_ON(file->f_path.dentry == DENTRY_NOT_SET)) {
			error = -EIO;
		} else {
			if (file->f_path.dentry) {
				dput(dentry);
				dentry = file->f_path.dentry;
			}
			if (unlikely(d_is_negative(dentry)))
				error = -ENOENT;
		}
	}
	if (error) {
		dput(dentry);
		dentry = ERR_PTR(error);
	}
	return dentry;
}

/*
 * Look up and maybe create and open the last component.
 *
 * Must be called with parent locked (exclusive in O_CREAT case).
 *
 * Returns 0 on success, that is, if
 *  the file was successfully atomically created (if necessary) and opened, or
 *  the file was not completely opened at this time, though lookups and
 *  creations were performed.
 * These case are distinguished by presence of FMODE_OPENED on file->f_mode.
 * In the latter case dentry returned in @path might be negative if O_CREAT
 * hadn't been specified.
 *
 * An error code is returned on failure.
 */
static struct dentry *lookup_open(struct nameidata *nd, struct file *file,
				  const struct open_flags *op,
				  bool got_write)
{
	struct mnt_idmap *idmap;
	struct dentry *dir = nd->path.dentry;
	struct inode *dir_inode = dir->d_inode;
	int open_flag = op->open_flag;
	struct dentry *dentry;
	int error, create_error = 0;
	umode_t mode = op->mode;
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq);

	if (unlikely(IS_DEADDIR(dir_inode)))
		return ERR_PTR(-ENOENT);

	file->f_mode &= ~FMODE_CREATED;
	dentry = d_lookup(dir, &nd->last);
	for (;;) {
		if (!dentry) {
			dentry = d_alloc_parallel(dir, &nd->last, &wq);
			if (IS_ERR(dentry))
				return dentry;
		}
		if (d_in_lookup(dentry))
			break;

		error = d_revalidate(dir_inode, &nd->last, dentry, nd->flags);
		if (likely(error > 0))
			break;
		if (error)
			goto out_dput;
		d_invalidate(dentry);
		dput(dentry);
		dentry = NULL;
	}
	if (dentry->d_inode) {
		/* Cached positive dentry: will open in f_op->open */
		return dentry;
	}

	if (open_flag & O_CREAT)
		audit_inode(nd->name, dir, AUDIT_INODE_PARENT);

	/*
	 * Checking write permission is tricky, bacuse we don't know if we are
	 * going to actually need it: O_CREAT opens should work as long as the
	 * file exists.  But checking existence breaks atomicity.  The trick is
	 * to check access and if not granted clear O_CREAT from the flags.
	 *
	 * Another problem is returing the "right" error value (e.g. for an
	 * O_EXCL open we want to return EEXIST not EROFS).
	 */
	if (unlikely(!got_write))
		open_flag &= ~O_TRUNC;
	idmap = mnt_idmap(nd->path.mnt);
	if (open_flag & O_CREAT) {
		if (open_flag & O_EXCL)
			open_flag &= ~O_TRUNC;
		mode = vfs_prepare_mode(idmap, dir->d_inode, mode, mode, mode);
		if (likely(got_write))
			create_error = may_o_create(idmap, &nd->path,
						    dentry, mode);
		else
			create_error = -EROFS;
	}
	if (create_error)
		open_flag &= ~O_CREAT;
	if (dir_inode->i_op->atomic_open) {
		dentry = atomic_open(nd, dentry, file, open_flag, mode);
		if (unlikely(create_error) && dentry == ERR_PTR(-ENOENT))
			dentry = ERR_PTR(create_error);
		return dentry;
	}

	if (d_in_lookup(dentry)) {
		struct dentry *res = dir_inode->i_op->lookup(dir_inode, dentry,
							     nd->flags);
		d_lookup_done(dentry);
		if (unlikely(res)) {
			if (IS_ERR(res)) {
				error = PTR_ERR(res);
				goto out_dput;
			}
			dput(dentry);
			dentry = res;
		}
	}

	/* Negative dentry, just create the file */
	if (!dentry->d_inode && (open_flag & O_CREAT)) {
		file->f_mode |= FMODE_CREATED;
		audit_inode_child(dir_inode, dentry, AUDIT_TYPE_CHILD_CREATE);
		if (!dir_inode->i_op->create) {
			error = -EACCES;
			goto out_dput;
		}

		error = dir_inode->i_op->create(idmap, dir_inode, dentry,
						mode, open_flag & O_EXCL);
		if (error)
			goto out_dput;
	}
	if (unlikely(create_error) && !dentry->d_inode) {
		error = create_error;
		goto out_dput;
	}
	return dentry;

out_dput:
	dput(dentry);
	return ERR_PTR(error);
}

static inline bool trailing_slashes(struct nameidata *nd)
{
	return (bool)nd->last.name[nd->last.len];
}

static struct dentry *lookup_fast_for_open(struct nameidata *nd, int open_flag)
{
	struct dentry *dentry;

	if (open_flag & O_CREAT) {
		if (trailing_slashes(nd))
			return ERR_PTR(-EISDIR);

		/* Don't bother on an O_EXCL create */
		if (open_flag & O_EXCL)
			return NULL;
	}

	if (trailing_slashes(nd))
		nd->flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;

	dentry = lookup_fast(nd);
	if (IS_ERR_OR_NULL(dentry))
		return dentry;

	if (open_flag & O_CREAT) {
		/* Discard negative dentries. Need inode_lock to do the create */
		if (!dentry->d_inode) {
			if (!(nd->flags & LOOKUP_RCU))
				dput(dentry);
			dentry = NULL;
		}
	}
	return dentry;
}

static const char *open_last_lookups(struct nameidata *nd,
		   struct file *file, const struct open_flags *op)
{
	struct dentry *dir = nd->path.dentry;
	int open_flag = op->open_flag;
	bool got_write = false;
	struct dentry *dentry;
	const char *res;

	nd->flags |= op->intent;

	if (nd->last_type != LAST_NORM) {
		if (nd->depth)
			put_link(nd);
		return handle_dots(nd, nd->last_type);
	}

	/* We _can_ be in RCU mode here */
	dentry = lookup_fast_for_open(nd, open_flag);
	if (IS_ERR(dentry))
		return ERR_CAST(dentry);

	if (likely(dentry))
		goto finish_lookup;

	if (!(open_flag & O_CREAT)) {
		if (WARN_ON_ONCE(nd->flags & LOOKUP_RCU))
			return ERR_PTR(-ECHILD);
	} else {
		if (nd->flags & LOOKUP_RCU) {
			if (!try_to_unlazy(nd))
				return ERR_PTR(-ECHILD);
		}
	}

	if (open_flag & (O_CREAT | O_TRUNC | O_WRONLY | O_RDWR)) {
		got_write = !mnt_want_write(nd->path.mnt);
		/*
		 * do _not_ fail yet - we might not need that or fail with
		 * a different error; let lookup_open() decide; we'll be
		 * dropping this one anyway.
		 */
	}
	if (open_flag & O_CREAT)
		inode_lock(dir->d_inode);
	else
		inode_lock_shared(dir->d_inode);
	dentry = lookup_open(nd, file, op, got_write);
	if (!IS_ERR(dentry)) {
		if (file->f_mode & FMODE_CREATED)
			fsnotify_create(dir->d_inode, dentry);
		if (file->f_mode & FMODE_OPENED)
			fsnotify_open(file);
	}
	if (open_flag & O_CREAT)
		inode_unlock(dir->d_inode);
	else
		inode_unlock_shared(dir->d_inode);

	if (got_write)
		mnt_drop_write(nd->path.mnt);

	if (IS_ERR(dentry))
		return ERR_CAST(dentry);

	if (file->f_mode & (FMODE_OPENED | FMODE_CREATED)) {
		dput(nd->path.dentry);
		nd->path.dentry = dentry;
		return NULL;
	}

finish_lookup:
	if (nd->depth)
		put_link(nd);
	res = step_into(nd, WALK_TRAILING, dentry);
	if (unlikely(res))
		nd->flags &= ~(LOOKUP_OPEN|LOOKUP_CREATE|LOOKUP_EXCL);
	return res;
}

/*
 * Handle the last step of open()
 */
static int do_open(struct nameidata *nd,
		   struct file *file, const struct open_flags *op)
{
	struct mnt_idmap *idmap;
	int open_flag = op->open_flag;
	bool do_truncate;
	int acc_mode;
	int error;

	if (!(file->f_mode & (FMODE_OPENED | FMODE_CREATED))) {
		error = complete_walk(nd);
		if (error)
			return error;
	}
	if (!(file->f_mode & FMODE_CREATED))
		audit_inode(nd->name, nd->path.dentry, 0);
	idmap = mnt_idmap(nd->path.mnt);
	if (open_flag & O_CREAT) {
		if ((open_flag & O_EXCL) && !(file->f_mode & FMODE_CREATED))
			return -EEXIST;
		if (d_is_dir(nd->path.dentry))
			return -EISDIR;
		error = may_create_in_sticky(idmap, nd,
					     d_backing_inode(nd->path.dentry));
		if (unlikely(error))
			return error;
	}
	if ((nd->flags & LOOKUP_DIRECTORY) && !d_can_lookup(nd->path.dentry))
		return -ENOTDIR;

	do_truncate = false;
	acc_mode = op->acc_mode;
	if (file->f_mode & FMODE_CREATED) {
		/* Don't check for write permission, don't truncate */
		open_flag &= ~O_TRUNC;
		acc_mode = 0;
	} else if (d_is_reg(nd->path.dentry) && open_flag & O_TRUNC) {
		error = mnt_want_write(nd->path.mnt);
		if (error)
			return error;
		do_truncate = true;
	}
	error = may_open(idmap, &nd->path, acc_mode, open_flag);
	if (!error && !(file->f_mode & FMODE_OPENED))
		error = vfs_open(&nd->path, file);
	if (!error)
		error = security_file_post_open(file, op->acc_mode);
	if (!error && do_truncate)
		error = handle_truncate(idmap, file);
	if (unlikely(error > 0)) {
		WARN_ON(1);
		error = -EINVAL;
	}
	if (do_truncate)
		mnt_drop_write(nd->path.mnt);
	return error;
}

/**
 * vfs_tmpfile - create tmpfile
 * @idmap:	idmap of the mount the inode was found from
 * @parentpath:	pointer to the path of the base directory
 * @file:	file descriptor of the new tmpfile
 * @mode:	mode of the new tmpfile
 *
 * Create a temporary file.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_tmpfile(struct mnt_idmap *idmap,
		const struct path *parentpath,
		struct file *file, umode_t mode)
{
	struct dentry *child;
	struct inode *dir = d_inode(parentpath->dentry);
	struct inode *inode;
	int error;
	int open_flag = file->f_flags;

	/* we want directory to be writable */
	error = inode_permission(idmap, dir, MAY_WRITE | MAY_EXEC);
	if (error)
		return error;
	if (!dir->i_op->tmpfile)
		return -EOPNOTSUPP;
	child = d_alloc(parentpath->dentry, &slash_name);
	if (unlikely(!child))
		return -ENOMEM;
	file->f_path.mnt = parentpath->mnt;
	file->f_path.dentry = child;
	mode = vfs_prepare_mode(idmap, dir, mode, mode, mode);
	error = dir->i_op->tmpfile(idmap, dir, file, mode);
	dput(child);
	if (file->f_mode & FMODE_OPENED)
		fsnotify_open(file);
	if (error)
		return error;
	/* Don't check for other permissions, the inode was just created */
	error = may_open(idmap, &file->f_path, 0, file->f_flags);
	if (error)
		return error;
	inode = file_inode(file);
	if (!(open_flag & O_EXCL)) {
		spin_lock(&inode->i_lock);
		inode->i_state |= I_LINKABLE;
		spin_unlock(&inode->i_lock);
	}
	security_inode_post_create_tmpfile(idmap, inode);
	return 0;
}

/**
 * kernel_tmpfile_open - open a tmpfile for kernel internal use
 * @idmap:	idmap of the mount the inode was found from
 * @parentpath:	path of the base directory
 * @mode:	mode of the new tmpfile
 * @open_flag:	flags
 * @cred:	credentials for open
 *
 * Create and open a temporary file.  The file is not accounted in nr_files,
 * hence this is only for kernel internal use, and must not be installed into
 * file tables or such.
 */
struct file *kernel_tmpfile_open(struct mnt_idmap *idmap,
				 const struct path *parentpath,
				 umode_t mode, int open_flag,
				 const struct cred *cred)
{
	struct file *file;
	int error;

	file = alloc_empty_file_noaccount(open_flag, cred);
	if (IS_ERR(file))
		return file;

	error = vfs_tmpfile(idmap, parentpath, file, mode);
	if (error) {
		fput(file);
		file = ERR_PTR(error);
	}
	return file;
}
EXPORT_SYMBOL(kernel_tmpfile_open);

static int do_tmpfile(struct nameidata *nd, unsigned flags,
		const struct open_flags *op,
		struct file *file)
{
	struct path path;
	int error = path_lookupat(nd, flags | LOOKUP_DIRECTORY, &path);

	if (unlikely(error))
		return error;
	error = mnt_want_write(path.mnt);
	if (unlikely(error))
		goto out;
	error = vfs_tmpfile(mnt_idmap(path.mnt), &path, file, op->mode);
	if (error)
		goto out2;
	audit_inode(nd->name, file->f_path.dentry, 0);
out2:
	mnt_drop_write(path.mnt);
out:
	path_put(&path);
	return error;
}

static int do_o_path(struct nameidata *nd, unsigned flags, struct file *file)
{
	struct path path;
	int error = path_lookupat(nd, flags, &path);
	if (!error) {
		audit_inode(nd->name, path.dentry, 0);
		error = vfs_open(&path, file);
		path_put(&path);
	}
	return error;
}

static struct file *path_openat(struct nameidata *nd,
			const struct open_flags *op, unsigned flags)
{
	struct file *file;
	int error;

	file = alloc_empty_file(op->open_flag, current_cred());
	if (IS_ERR(file))
		return file;

	if (unlikely(file->f_flags & __O_TMPFILE)) {
		error = do_tmpfile(nd, flags, op, file);
	} else if (unlikely(file->f_flags & O_PATH)) {
		error = do_o_path(nd, flags, file);
	} else {
		const char *s = path_init(nd, flags);
		while (!(error = link_path_walk(s, nd)) &&
		       (s = open_last_lookups(nd, file, op)) != NULL)
			;
		if (!error)
			error = do_open(nd, file, op);
		terminate_walk(nd);
	}
	if (likely(!error)) {
		if (likely(file->f_mode & FMODE_OPENED))
			return file;
		WARN_ON(1);
		error = -EINVAL;
	}
	fput_close(file);
	if (error == -EOPENSTALE) {
		if (flags & LOOKUP_RCU)
			error = -ECHILD;
		else
			error = -ESTALE;
	}
	return ERR_PTR(error);
}

struct file *do_filp_open(int dfd, struct filename *pathname,
		const struct open_flags *op)
{
	struct nameidata nd;
	int flags = op->lookup_flags;
	struct file *filp;

	set_nameidata(&nd, dfd, pathname, NULL);
	filp = path_openat(&nd, op, flags | LOOKUP_RCU);
	if (unlikely(filp == ERR_PTR(-ECHILD)))
		filp = path_openat(&nd, op, flags);
	if (unlikely(filp == ERR_PTR(-ESTALE)))
		filp = path_openat(&nd, op, flags | LOOKUP_REVAL);
	restore_nameidata();
	return filp;
}

struct file *do_file_open_root(const struct path *root,
		const char *name, const struct open_flags *op)
{
	struct nameidata nd;
	struct file *file;
	struct filename *filename;
	int flags = op->lookup_flags;

	if (d_is_symlink(root->dentry) && op->intent & LOOKUP_OPEN)
		return ERR_PTR(-ELOOP);

	filename = getname_kernel(name);
	if (IS_ERR(filename))
		return ERR_CAST(filename);

	set_nameidata(&nd, -1, filename, root);
	file = path_openat(&nd, op, flags | LOOKUP_RCU);
	if (unlikely(file == ERR_PTR(-ECHILD)))
		file = path_openat(&nd, op, flags);
	if (unlikely(file == ERR_PTR(-ESTALE)))
		file = path_openat(&nd, op, flags | LOOKUP_REVAL);
	restore_nameidata();
	putname(filename);
	return file;
}

static struct dentry *filename_create(int dfd, struct filename *name,
				      struct path *path, unsigned int lookup_flags)
{
	struct dentry *dentry = ERR_PTR(-EEXIST);
	struct qstr last;
	bool want_dir = lookup_flags & LOOKUP_DIRECTORY;
	unsigned int reval_flag = lookup_flags & LOOKUP_REVAL;
	unsigned int create_flags = LOOKUP_CREATE | LOOKUP_EXCL;
	int type;
	int err2;
	int error;

	error = filename_parentat(dfd, name, reval_flag, path, &last, &type);
	if (error)
		return ERR_PTR(error);

	/*
	 * Yucky last component or no last component at all?
	 * (foo/., foo/.., /////)
	 */
	if (unlikely(type != LAST_NORM))
		goto out;

	/* don't fail immediately if it's r/o, at least try to report other errors */
	err2 = mnt_want_write(path->mnt);
	/*
	 * Do the final lookup.  Suppress 'create' if there is a trailing
	 * '/', and a directory wasn't requested.
	 */
	if (last.name[last.len] && !want_dir)
		create_flags &= ~LOOKUP_CREATE;
	inode_lock_nested(path->dentry->d_inode, I_MUTEX_PARENT);
	dentry = lookup_one_qstr_excl(&last, path->dentry,
				      reval_flag | create_flags);
	if (IS_ERR(dentry))
		goto unlock;

	if (unlikely(err2)) {
		error = err2;
		goto fail;
	}
	return dentry;
fail:
	dput(dentry);
	dentry = ERR_PTR(error);
unlock:
	inode_unlock(path->dentry->d_inode);
	if (!err2)
		mnt_drop_write(path->mnt);
out:
	path_put(path);
	return dentry;
}

struct dentry *kern_path_create(int dfd, const char *pathname,
				struct path *path, unsigned int lookup_flags)
{
	struct filename *filename = getname_kernel(pathname);
	struct dentry *res = filename_create(dfd, filename, path, lookup_flags);

	putname(filename);
	return res;
}
EXPORT_SYMBOL(kern_path_create);

void done_path_create(struct path *path, struct dentry *dentry)
{
	if (!IS_ERR(dentry))
		dput(dentry);
	inode_unlock(path->dentry->d_inode);
	mnt_drop_write(path->mnt);
	path_put(path);
}
EXPORT_SYMBOL(done_path_create);

inline struct dentry *user_path_create(int dfd, const char __user *pathname,
				struct path *path, unsigned int lookup_flags)
{
	struct filename *filename = getname(pathname);
	struct dentry *res = filename_create(dfd, filename, path, lookup_flags);

	putname(filename);
	return res;
}
EXPORT_SYMBOL(user_path_create);

/**
 * vfs_mknod - create device node or file
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	inode of the parent directory
 * @dentry:	dentry of the child device node
 * @mode:	mode of the child device node
 * @dev:	device number of device to create
 *
 * Create a device node or file.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
	      struct dentry *dentry, umode_t mode, dev_t dev)
{
	bool is_whiteout = S_ISCHR(mode) && dev == WHITEOUT_DEV;
	int error = may_create(idmap, dir, dentry);

	if (error)
		return error;

	if ((S_ISCHR(mode) || S_ISBLK(mode)) && !is_whiteout &&
	    !capable(CAP_MKNOD))
		return -EPERM;

	if (!dir->i_op->mknod)
		return -EPERM;

	mode = vfs_prepare_mode(idmap, dir, mode, mode, mode);
	error = devcgroup_inode_mknod(mode, dev);
	if (error)
		return error;

	error = security_inode_mknod(dir, dentry, mode, dev);
	if (error)
		return error;

	error = dir->i_op->mknod(idmap, dir, dentry, mode, dev);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_mknod);

static int may_mknod(umode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
	case S_IFCHR:
	case S_IFBLK:
	case S_IFIFO:
	case S_IFSOCK:
	case 0: /* zero mode translates to S_IFREG */
		return 0;
	case S_IFDIR:
		return -EPERM;
	default:
		return -EINVAL;
	}
}

static int do_mknodat(int dfd, struct filename *name, umode_t mode,
		unsigned int dev)
{
	struct mnt_idmap *idmap;
	struct dentry *dentry;
	struct path path;
	int error;
	unsigned int lookup_flags = 0;

	error = may_mknod(mode);
	if (error)
		goto out1;
retry:
	dentry = filename_create(dfd, name, &path, lookup_flags);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto out1;

	error = security_path_mknod(&path, dentry,
			mode_strip_umask(path.dentry->d_inode, mode), dev);
	if (error)
		goto out2;

	idmap = mnt_idmap(path.mnt);
	switch (mode & S_IFMT) {
		case 0: case S_IFREG:
			error = vfs_create(idmap, path.dentry->d_inode,
					   dentry, mode, true);
			if (!error)
				security_path_post_mknod(idmap, dentry);
			break;
		case S_IFCHR: case S_IFBLK:
			error = vfs_mknod(idmap, path.dentry->d_inode,
					  dentry, mode, new_decode_dev(dev));
			break;
		case S_IFIFO: case S_IFSOCK:
			error = vfs_mknod(idmap, path.dentry->d_inode,
					  dentry, mode, 0);
			break;
	}
out2:
	done_path_create(&path, dentry);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out1:
	putname(name);
	return error;
}

SYSCALL_DEFINE4(mknodat, int, dfd, const char __user *, filename, umode_t, mode,
		unsigned int, dev)
{
	return do_mknodat(dfd, getname(filename), mode, dev);
}

SYSCALL_DEFINE3(mknod, const char __user *, filename, umode_t, mode, unsigned, dev)
{
	return do_mknodat(AT_FDCWD, getname(filename), mode, dev);
}

/**
 * vfs_mkdir - create directory returning correct dentry if possible
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	inode of the parent directory
 * @dentry:	dentry of the child directory
 * @mode:	mode of the child directory
 *
 * Create a directory.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 *
 * In the event that the filesystem does not use the *@dentry but leaves it
 * negative or unhashes it and possibly splices a different one returning it,
 * the original dentry is dput() and the alternate is returned.
 *
 * In case of an error the dentry is dput() and an ERR_PTR() is returned.
 */
struct dentry *vfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode)
{
	int error;
	unsigned max_links = dir->i_sb->s_max_links;
	struct dentry *de;

	error = may_create(idmap, dir, dentry);
	if (error)
		goto err;

	error = -EPERM;
	if (!dir->i_op->mkdir)
		goto err;

	mode = vfs_prepare_mode(idmap, dir, mode, S_IRWXUGO | S_ISVTX, 0);
	error = security_inode_mkdir(dir, dentry, mode);
	if (error)
		goto err;

	error = -EMLINK;
	if (max_links && dir->i_nlink >= max_links)
		goto err;

	de = dir->i_op->mkdir(idmap, dir, dentry, mode);
	error = PTR_ERR(de);
	if (IS_ERR(de))
		goto err;
	if (de) {
		dput(dentry);
		dentry = de;
	}
	fsnotify_mkdir(dir, dentry);
	return dentry;

err:
	dput(dentry);
	return ERR_PTR(error);
}
EXPORT_SYMBOL(vfs_mkdir);

int do_mkdirat(int dfd, struct filename *name, umode_t mode)
{
	struct dentry *dentry;
	struct path path;
	int error;
	unsigned int lookup_flags = LOOKUP_DIRECTORY;

retry:
	dentry = filename_create(dfd, name, &path, lookup_flags);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto out_putname;

	error = security_path_mkdir(&path, dentry,
			mode_strip_umask(path.dentry->d_inode, mode));
	if (!error) {
		dentry = vfs_mkdir(mnt_idmap(path.mnt), path.dentry->d_inode,
				  dentry, mode);
		if (IS_ERR(dentry))
			error = PTR_ERR(dentry);
	}
	done_path_create(&path, dentry);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out_putname:
	putname(name);
	return error;
}

SYSCALL_DEFINE3(mkdirat, int, dfd, const char __user *, pathname, umode_t, mode)
{
	return do_mkdirat(dfd, getname(pathname), mode);
}

SYSCALL_DEFINE2(mkdir, const char __user *, pathname, umode_t, mode)
{
	return do_mkdirat(AT_FDCWD, getname(pathname), mode);
}

/**
 * vfs_rmdir - remove directory
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	inode of the parent directory
 * @dentry:	dentry of the child directory
 *
 * Remove a directory.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_rmdir(struct mnt_idmap *idmap, struct inode *dir,
		     struct dentry *dentry)
{
	int error = may_delete(idmap, dir, dentry, 1);

	if (error)
		return error;

	if (!dir->i_op->rmdir)
		return -EPERM;

	dget(dentry);
	inode_lock(dentry->d_inode);

	error = -EBUSY;
	if (is_local_mountpoint(dentry) ||
	    (dentry->d_inode->i_flags & S_KERNEL_FILE))
		goto out;

	error = security_inode_rmdir(dir, dentry);
	if (error)
		goto out;

	error = dir->i_op->rmdir(dir, dentry);
	if (error)
		goto out;

	shrink_dcache_parent(dentry);
	dentry->d_inode->i_flags |= S_DEAD;
	dont_mount(dentry);
	detach_mounts(dentry);

out:
	inode_unlock(dentry->d_inode);
	dput(dentry);
	if (!error)
		d_delete_notify(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_rmdir);

int do_rmdir(int dfd, struct filename *name)
{
	int error;
	struct dentry *dentry;
	struct path path;
	struct qstr last;
	int type;
	unsigned int lookup_flags = 0;
retry:
	error = filename_parentat(dfd, name, lookup_flags, &path, &last, &type);
	if (error)
		goto exit1;

	switch (type) {
	case LAST_DOTDOT:
		error = -ENOTEMPTY;
		goto exit2;
	case LAST_DOT:
		error = -EINVAL;
		goto exit2;
	case LAST_ROOT:
		error = -EBUSY;
		goto exit2;
	}

	error = mnt_want_write(path.mnt);
	if (error)
		goto exit2;

	inode_lock_nested(path.dentry->d_inode, I_MUTEX_PARENT);
	dentry = lookup_one_qstr_excl(&last, path.dentry, lookup_flags);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto exit3;
	error = security_path_rmdir(&path, dentry);
	if (error)
		goto exit4;
	error = vfs_rmdir(mnt_idmap(path.mnt), path.dentry->d_inode, dentry);
exit4:
	dput(dentry);
exit3:
	inode_unlock(path.dentry->d_inode);
	mnt_drop_write(path.mnt);
exit2:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
exit1:
	putname(name);
	return error;
}

SYSCALL_DEFINE1(rmdir, const char __user *, pathname)
{
	return do_rmdir(AT_FDCWD, getname(pathname));
}

/**
 * vfs_unlink - unlink a filesystem object
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	parent directory
 * @dentry:	victim
 * @delegated_inode: returns victim inode, if the inode is delegated.
 *
 * The caller must hold dir->i_rwsem exclusively.
 *
 * If vfs_unlink discovers a delegation, it will return -EWOULDBLOCK and
 * return a reference to the inode in delegated_inode.  The caller
 * should then break the delegation on that inode and retry.  Because
 * breaking a delegation may take a long time, the caller should drop
 * dir->i_rwsem before doing so.
 *
 * Alternatively, a caller may pass NULL for delegated_inode.  This may
 * be appropriate for callers that expect the underlying filesystem not
 * to be NFS exported.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_unlink(struct mnt_idmap *idmap, struct inode *dir,
	       struct dentry *dentry, struct inode **delegated_inode)
{
	struct inode *target = dentry->d_inode;
	int error = may_delete(idmap, dir, dentry, 0);

	if (error)
		return error;

	if (!dir->i_op->unlink)
		return -EPERM;

	inode_lock(target);
	if (IS_SWAPFILE(target))
		error = -EPERM;
	else if (is_local_mountpoint(dentry))
		error = -EBUSY;
	else {
		error = security_inode_unlink(dir, dentry);
		if (!error) {
			error = try_break_deleg(target, delegated_inode);
			if (error)
				goto out;
			error = dir->i_op->unlink(dir, dentry);
			if (!error) {
				dont_mount(dentry);
				detach_mounts(dentry);
			}
		}
	}
out:
	inode_unlock(target);

	/* We don't d_delete() NFS sillyrenamed files--they still exist. */
	if (!error && dentry->d_flags & DCACHE_NFSFS_RENAMED) {
		fsnotify_unlink(dir, dentry);
	} else if (!error) {
		fsnotify_link_count(target);
		d_delete_notify(dir, dentry);
	}

	return error;
}
EXPORT_SYMBOL(vfs_unlink);

/*
 * Make sure that the actual truncation of the file will occur outside its
 * directory's i_rwsem.  Truncate can take a long time if there is a lot of
 * writeout happening, and we don't want to prevent access to the directory
 * while waiting on the I/O.
 */
int do_unlinkat(int dfd, struct filename *name)
{
	int error;
	struct dentry *dentry;
	struct path path;
	struct qstr last;
	int type;
	struct inode *inode = NULL;
	struct inode *delegated_inode = NULL;
	unsigned int lookup_flags = 0;
retry:
	error = filename_parentat(dfd, name, lookup_flags, &path, &last, &type);
	if (error)
		goto exit1;

	error = -EISDIR;
	if (type != LAST_NORM)
		goto exit2;

	error = mnt_want_write(path.mnt);
	if (error)
		goto exit2;
retry_deleg:
	inode_lock_nested(path.dentry->d_inode, I_MUTEX_PARENT);
	dentry = lookup_one_qstr_excl(&last, path.dentry, lookup_flags);
	error = PTR_ERR(dentry);
	if (!IS_ERR(dentry)) {

		/* Why not before? Because we want correct error value */
		if (last.name[last.len])
			goto slashes;
		inode = dentry->d_inode;
		ihold(inode);
		error = security_path_unlink(&path, dentry);
		if (error)
			goto exit3;
		error = vfs_unlink(mnt_idmap(path.mnt), path.dentry->d_inode,
				   dentry, &delegated_inode);
exit3:
		dput(dentry);
	}
	inode_unlock(path.dentry->d_inode);
	if (inode)
		iput(inode);	/* truncate the inode here */
	inode = NULL;
	if (delegated_inode) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry_deleg;
	}
	mnt_drop_write(path.mnt);
exit2:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		inode = NULL;
		goto retry;
	}
exit1:
	putname(name);
	return error;

slashes:
	if (d_is_dir(dentry))
		error = -EISDIR;
	else
		error = -ENOTDIR;
	goto exit3;
}

SYSCALL_DEFINE3(unlinkat, int, dfd, const char __user *, pathname, int, flag)
{
	if ((flag & ~AT_REMOVEDIR) != 0)
		return -EINVAL;

	if (flag & AT_REMOVEDIR)
		return do_rmdir(dfd, getname(pathname));
	return do_unlinkat(dfd, getname(pathname));
}

SYSCALL_DEFINE1(unlink, const char __user *, pathname)
{
	return do_unlinkat(AT_FDCWD, getname(pathname));
}

/**
 * vfs_symlink - create symlink
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	inode of the parent directory
 * @dentry:	dentry of the child symlink file
 * @oldname:	name of the file to link to
 *
 * Create a symlink.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, const char *oldname)
{
	int error;

	error = may_create(idmap, dir, dentry);
	if (error)
		return error;

	if (!dir->i_op->symlink)
		return -EPERM;

	error = security_inode_symlink(dir, dentry, oldname);
	if (error)
		return error;

	error = dir->i_op->symlink(idmap, dir, dentry, oldname);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_symlink);

int do_symlinkat(struct filename *from, int newdfd, struct filename *to)
{
	int error;
	struct dentry *dentry;
	struct path path;
	unsigned int lookup_flags = 0;

	if (IS_ERR(from)) {
		error = PTR_ERR(from);
		goto out_putnames;
	}
retry:
	dentry = filename_create(newdfd, to, &path, lookup_flags);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto out_putnames;

	error = security_path_symlink(&path, dentry, from->name);
	if (!error)
		error = vfs_symlink(mnt_idmap(path.mnt), path.dentry->d_inode,
				    dentry, from->name);
	done_path_create(&path, dentry);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out_putnames:
	putname(to);
	putname(from);
	return error;
}

SYSCALL_DEFINE3(symlinkat, const char __user *, oldname,
		int, newdfd, const char __user *, newname)
{
	return do_symlinkat(getname(oldname), newdfd, getname(newname));
}

SYSCALL_DEFINE2(symlink, const char __user *, oldname, const char __user *, newname)
{
	return do_symlinkat(getname(oldname), AT_FDCWD, getname(newname));
}

/**
 * vfs_link - create a new link
 * @old_dentry:	object to be linked
 * @idmap:	idmap of the mount
 * @dir:	new parent
 * @new_dentry:	where to create the new link
 * @delegated_inode: returns inode needing a delegation break
 *
 * The caller must hold dir->i_rwsem exclusively.
 *
 * If vfs_link discovers a delegation on the to-be-linked file in need
 * of breaking, it will return -EWOULDBLOCK and return a reference to the
 * inode in delegated_inode.  The caller should then break the delegation
 * and retry.  Because breaking a delegation may take a long time, the
 * caller should drop the i_rwsem before doing so.
 *
 * Alternatively, a caller may pass NULL for delegated_inode.  This may
 * be appropriate for callers that expect the underlying filesystem not
 * to be NFS exported.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_link(struct dentry *old_dentry, struct mnt_idmap *idmap,
	     struct inode *dir, struct dentry *new_dentry,
	     struct inode **delegated_inode)
{
	struct inode *inode = old_dentry->d_inode;
	unsigned max_links = dir->i_sb->s_max_links;
	int error;

	if (!inode)
		return -ENOENT;

	error = may_create(idmap, dir, new_dentry);
	if (error)
		return error;

	if (dir->i_sb != inode->i_sb)
		return -EXDEV;

	/*
	 * A link to an append-only or immutable file cannot be created.
	 */
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return -EPERM;
	/*
	 * Updating the link count will likely cause i_uid and i_gid to
	 * be writen back improperly if their true value is unknown to
	 * the vfs.
	 */
	if (HAS_UNMAPPED_ID(idmap, inode))
		return -EPERM;
	if (!dir->i_op->link)
		return -EPERM;
	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	error = security_inode_link(old_dentry, dir, new_dentry);
	if (error)
		return error;

	inode_lock(inode);
	/* Make sure we don't allow creating hardlink to an unlinked file */
	if (inode->i_nlink == 0 && !(inode->i_state & I_LINKABLE))
		error =  -ENOENT;
	else if (max_links && inode->i_nlink >= max_links)
		error = -EMLINK;
	else {
		error = try_break_deleg(inode, delegated_inode);
		if (!error)
			error = dir->i_op->link(old_dentry, dir, new_dentry);
	}

	if (!error && (inode->i_state & I_LINKABLE)) {
		spin_lock(&inode->i_lock);
		inode->i_state &= ~I_LINKABLE;
		spin_unlock(&inode->i_lock);
	}
	inode_unlock(inode);
	if (!error)
		fsnotify_link(dir, inode, new_dentry);
	return error;
}
EXPORT_SYMBOL(vfs_link);

/*
 * Hardlinks are often used in delicate situations.  We avoid
 * security-related surprises by not following symlinks on the
 * newname.  --KAB
 *
 * We don't follow them on the oldname either to be compatible
 * with linux 2.0, and to avoid hard-linking to directories
 * and other special files.  --ADM
 */
int do_linkat(int olddfd, struct filename *old, int newdfd,
	      struct filename *new, int flags)
{
	struct mnt_idmap *idmap;
	struct dentry *new_dentry;
	struct path old_path, new_path;
	struct inode *delegated_inode = NULL;
	int how = 0;
	int error;

	if ((flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) != 0) {
		error = -EINVAL;
		goto out_putnames;
	}
	/*
	 * To use null names we require CAP_DAC_READ_SEARCH or
	 * that the open-time creds of the dfd matches current.
	 * This ensures that not everyone will be able to create
	 * a hardlink using the passed file descriptor.
	 */
	if (flags & AT_EMPTY_PATH)
		how |= LOOKUP_LINKAT_EMPTY;

	if (flags & AT_SYMLINK_FOLLOW)
		how |= LOOKUP_FOLLOW;
retry:
	error = filename_lookup(olddfd, old, how, &old_path, NULL);
	if (error)
		goto out_putnames;

	new_dentry = filename_create(newdfd, new, &new_path,
					(how & LOOKUP_REVAL));
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto out_putpath;

	error = -EXDEV;
	if (old_path.mnt != new_path.mnt)
		goto out_dput;
	idmap = mnt_idmap(new_path.mnt);
	error = may_linkat(idmap, &old_path);
	if (unlikely(error))
		goto out_dput;
	error = security_path_link(old_path.dentry, &new_path, new_dentry);
	if (error)
		goto out_dput;
	error = vfs_link(old_path.dentry, idmap, new_path.dentry->d_inode,
			 new_dentry, &delegated_inode);
out_dput:
	done_path_create(&new_path, new_dentry);
	if (delegated_inode) {
		error = break_deleg_wait(&delegated_inode);
		if (!error) {
			path_put(&old_path);
			goto retry;
		}
	}
	if (retry_estale(error, how)) {
		path_put(&old_path);
		how |= LOOKUP_REVAL;
		goto retry;
	}
out_putpath:
	path_put(&old_path);
out_putnames:
	putname(old);
	putname(new);

	return error;
}

SYSCALL_DEFINE5(linkat, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname, int, flags)
{
	return do_linkat(olddfd, getname_uflags(oldname, flags),
		newdfd, getname(newname), flags);
}

SYSCALL_DEFINE2(link, const char __user *, oldname, const char __user *, newname)
{
	return do_linkat(AT_FDCWD, getname(oldname), AT_FDCWD, getname(newname), 0);
}

/**
 * vfs_rename - rename a filesystem object
 * @rd:		pointer to &struct renamedata info
 *
 * The caller must hold multiple mutexes--see lock_rename()).
 *
 * If vfs_rename discovers a delegation in need of breaking at either
 * the source or destination, it will return -EWOULDBLOCK and return a
 * reference to the inode in delegated_inode.  The caller should then
 * break the delegation and retry.  Because breaking a delegation may
 * take a long time, the caller should drop all locks before doing
 * so.
 *
 * Alternatively, a caller may pass NULL for delegated_inode.  This may
 * be appropriate for callers that expect the underlying filesystem not
 * to be NFS exported.
 *
 * The worst of all namespace operations - renaming directory. "Perverted"
 * doesn't even start to describe it. Somebody in UCB had a heck of a trip...
 * Problems:
 *
 *	a) we can get into loop creation.
 *	b) race potential - two innocent renames can create a loop together.
 *	   That's where 4.4BSD screws up. Current fix: serialization on
 *	   sb->s_vfs_rename_mutex. We might be more accurate, but that's another
 *	   story.
 *	c) we may have to lock up to _four_ objects - parents and victim (if it exists),
 *	   and source (if it's a non-directory or a subdirectory that moves to
 *	   different parent).
 *	   And that - after we got ->i_rwsem on parents (until then we don't know
 *	   whether the target exists).  Solution: try to be smart with locking
 *	   order for inodes.  We rely on the fact that tree topology may change
 *	   only under ->s_vfs_rename_mutex _and_ that parent of the object we
 *	   move will be locked.  Thus we can rank directories by the tree
 *	   (ancestors first) and rank all non-directories after them.
 *	   That works since everybody except rename does "lock parent, lookup,
 *	   lock child" and rename is under ->s_vfs_rename_mutex.
 *	   HOWEVER, it relies on the assumption that any object with ->lookup()
 *	   has no more than 1 dentry.  If "hybrid" objects will ever appear,
 *	   we'd better make sure that there's no link(2) for them.
 *	d) conversion from fhandle to dentry may come in the wrong moment - when
 *	   we are removing the target. Solution: we will have to grab ->i_rwsem
 *	   in the fhandle_to_dentry code. [FIXME - current nfsfh.c relies on
 *	   ->i_rwsem on parents, which works but leads to some truly excessive
 *	   locking].
 */
int vfs_rename(struct renamedata *rd)
{
	int error;
	struct inode *old_dir = d_inode(rd->old_parent);
	struct inode *new_dir = d_inode(rd->new_parent);
	struct dentry *old_dentry = rd->old_dentry;
	struct dentry *new_dentry = rd->new_dentry;
	struct inode **delegated_inode = rd->delegated_inode;
	unsigned int flags = rd->flags;
	bool is_dir = d_is_dir(old_dentry);
	struct inode *source = old_dentry->d_inode;
	struct inode *target = new_dentry->d_inode;
	bool new_is_dir = false;
	unsigned max_links = new_dir->i_sb->s_max_links;
	struct name_snapshot old_name;
	bool lock_old_subdir, lock_new_subdir;

	if (source == target)
		return 0;

	error = may_delete(rd->old_mnt_idmap, old_dir, old_dentry, is_dir);
	if (error)
		return error;

	if (!target) {
		error = may_create(rd->new_mnt_idmap, new_dir, new_dentry);
	} else {
		new_is_dir = d_is_dir(new_dentry);

		if (!(flags & RENAME_EXCHANGE))
			error = may_delete(rd->new_mnt_idmap, new_dir,
					   new_dentry, is_dir);
		else
			error = may_delete(rd->new_mnt_idmap, new_dir,
					   new_dentry, new_is_dir);
	}
	if (error)
		return error;

	if (!old_dir->i_op->rename)
		return -EPERM;

	/*
	 * If we are going to change the parent - check write permissions,
	 * we'll need to flip '..'.
	 */
	if (new_dir != old_dir) {
		if (is_dir) {
			error = inode_permission(rd->old_mnt_idmap, source,
						 MAY_WRITE);
			if (error)
				return error;
		}
		if ((flags & RENAME_EXCHANGE) && new_is_dir) {
			error = inode_permission(rd->new_mnt_idmap, target,
						 MAY_WRITE);
			if (error)
				return error;
		}
	}

	error = security_inode_rename(old_dir, old_dentry, new_dir, new_dentry,
				      flags);
	if (error)
		return error;

	take_dentry_name_snapshot(&old_name, old_dentry);
	dget(new_dentry);
	/*
	 * Lock children.
	 * The source subdirectory needs to be locked on cross-directory
	 * rename or cross-directory exchange since its parent changes.
	 * The target subdirectory needs to be locked on cross-directory
	 * exchange due to parent change and on any rename due to becoming
	 * a victim.
	 * Non-directories need locking in all cases (for NFS reasons);
	 * they get locked after any subdirectories (in inode address order).
	 *
	 * NOTE: WE ONLY LOCK UNRELATED DIRECTORIES IN CROSS-DIRECTORY CASE.
	 * NEVER, EVER DO THAT WITHOUT ->s_vfs_rename_mutex.
	 */
	lock_old_subdir = new_dir != old_dir;
	lock_new_subdir = new_dir != old_dir || !(flags & RENAME_EXCHANGE);
	if (is_dir) {
		if (lock_old_subdir)
			inode_lock_nested(source, I_MUTEX_CHILD);
		if (target && (!new_is_dir || lock_new_subdir))
			inode_lock(target);
	} else if (new_is_dir) {
		if (lock_new_subdir)
			inode_lock_nested(target, I_MUTEX_CHILD);
		inode_lock(source);
	} else {
		lock_two_nondirectories(source, target);
	}

	error = -EPERM;
	if (IS_SWAPFILE(source) || (target && IS_SWAPFILE(target)))
		goto out;

	error = -EBUSY;
	if (is_local_mountpoint(old_dentry) || is_local_mountpoint(new_dentry))
		goto out;

	if (max_links && new_dir != old_dir) {
		error = -EMLINK;
		if (is_dir && !new_is_dir && new_dir->i_nlink >= max_links)
			goto out;
		if ((flags & RENAME_EXCHANGE) && !is_dir && new_is_dir &&
		    old_dir->i_nlink >= max_links)
			goto out;
	}
	if (!is_dir) {
		error = try_break_deleg(source, delegated_inode);
		if (error)
			goto out;
	}
	if (target && !new_is_dir) {
		error = try_break_deleg(target, delegated_inode);
		if (error)
			goto out;
	}
	error = old_dir->i_op->rename(rd->new_mnt_idmap, old_dir, old_dentry,
				      new_dir, new_dentry, flags);
	if (error)
		goto out;

	if (!(flags & RENAME_EXCHANGE) && target) {
		if (is_dir) {
			shrink_dcache_parent(new_dentry);
			target->i_flags |= S_DEAD;
		}
		dont_mount(new_dentry);
		detach_mounts(new_dentry);
	}
	if (!(old_dir->i_sb->s_type->fs_flags & FS_RENAME_DOES_D_MOVE)) {
		if (!(flags & RENAME_EXCHANGE))
			d_move(old_dentry, new_dentry);
		else
			d_exchange(old_dentry, new_dentry);
	}
out:
	if (!is_dir || lock_old_subdir)
		inode_unlock(source);
	if (target && (!new_is_dir || lock_new_subdir))
		inode_unlock(target);
	dput(new_dentry);
	if (!error) {
		fsnotify_move(old_dir, new_dir, &old_name.name, is_dir,
			      !(flags & RENAME_EXCHANGE) ? target : NULL, old_dentry);
		if (flags & RENAME_EXCHANGE) {
			fsnotify_move(new_dir, old_dir, &old_dentry->d_name,
				      new_is_dir, NULL, new_dentry);
		}
	}
	release_dentry_name_snapshot(&old_name);

	return error;
}
EXPORT_SYMBOL(vfs_rename);

int do_renameat2(int olddfd, struct filename *from, int newdfd,
		 struct filename *to, unsigned int flags)
{
	struct renamedata rd;
	struct dentry *old_dentry, *new_dentry;
	struct dentry *trap;
	struct path old_path, new_path;
	struct qstr old_last, new_last;
	int old_type, new_type;
	struct inode *delegated_inode = NULL;
	unsigned int lookup_flags = 0, target_flags =
		LOOKUP_RENAME_TARGET | LOOKUP_CREATE;
	bool should_retry = false;
	int error = -EINVAL;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
		goto put_names;

	if ((flags & (RENAME_NOREPLACE | RENAME_WHITEOUT)) &&
	    (flags & RENAME_EXCHANGE))
		goto put_names;

	if (flags & RENAME_EXCHANGE)
		target_flags = 0;
	if (flags & RENAME_NOREPLACE)
		target_flags |= LOOKUP_EXCL;

retry:
	error = filename_parentat(olddfd, from, lookup_flags, &old_path,
				  &old_last, &old_type);
	if (error)
		goto put_names;

	error = filename_parentat(newdfd, to, lookup_flags, &new_path, &new_last,
				  &new_type);
	if (error)
		goto exit1;

	error = -EXDEV;
	if (old_path.mnt != new_path.mnt)
		goto exit2;

	error = -EBUSY;
	if (old_type != LAST_NORM)
		goto exit2;

	if (flags & RENAME_NOREPLACE)
		error = -EEXIST;
	if (new_type != LAST_NORM)
		goto exit2;

	error = mnt_want_write(old_path.mnt);
	if (error)
		goto exit2;

retry_deleg:
	trap = lock_rename(new_path.dentry, old_path.dentry);
	if (IS_ERR(trap)) {
		error = PTR_ERR(trap);
		goto exit_lock_rename;
	}

	old_dentry = lookup_one_qstr_excl(&old_last, old_path.dentry,
					  lookup_flags);
	error = PTR_ERR(old_dentry);
	if (IS_ERR(old_dentry))
		goto exit3;
	new_dentry = lookup_one_qstr_excl(&new_last, new_path.dentry,
					  lookup_flags | target_flags);
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto exit4;
	if (flags & RENAME_EXCHANGE) {
		if (!d_is_dir(new_dentry)) {
			error = -ENOTDIR;
			if (new_last.name[new_last.len])
				goto exit5;
		}
	}
	/* unless the source is a directory trailing slashes give -ENOTDIR */
	if (!d_is_dir(old_dentry)) {
		error = -ENOTDIR;
		if (old_last.name[old_last.len])
			goto exit5;
		if (!(flags & RENAME_EXCHANGE) && new_last.name[new_last.len])
			goto exit5;
	}
	/* source should not be ancestor of target */
	error = -EINVAL;
	if (old_dentry == trap)
		goto exit5;
	/* target should not be an ancestor of source */
	if (!(flags & RENAME_EXCHANGE))
		error = -ENOTEMPTY;
	if (new_dentry == trap)
		goto exit5;

	error = security_path_rename(&old_path, old_dentry,
				     &new_path, new_dentry, flags);
	if (error)
		goto exit5;

	rd.old_parent	   = old_path.dentry;
	rd.old_dentry	   = old_dentry;
	rd.old_mnt_idmap   = mnt_idmap(old_path.mnt);
	rd.new_parent	   = new_path.dentry;
	rd.new_dentry	   = new_dentry;
	rd.new_mnt_idmap   = mnt_idmap(new_path.mnt);
	rd.delegated_inode = &delegated_inode;
	rd.flags	   = flags;
	error = vfs_rename(&rd);
exit5:
	dput(new_dentry);
exit4:
	dput(old_dentry);
exit3:
	unlock_rename(new_path.dentry, old_path.dentry);
exit_lock_rename:
	if (delegated_inode) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry_deleg;
	}
	mnt_drop_write(old_path.mnt);
exit2:
	if (retry_estale(error, lookup_flags))
		should_retry = true;
	path_put(&new_path);
exit1:
	path_put(&old_path);
	if (should_retry) {
		should_retry = false;
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
put_names:
	putname(from);
	putname(to);
	return error;
}

SYSCALL_DEFINE5(renameat2, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname, unsigned int, flags)
{
	return do_renameat2(olddfd, getname(oldname), newdfd, getname(newname),
				flags);
}

SYSCALL_DEFINE4(renameat, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname)
{
	return do_renameat2(olddfd, getname(oldname), newdfd, getname(newname),
				0);
}

SYSCALL_DEFINE2(rename, const char __user *, oldname, const char __user *, newname)
{
	return do_renameat2(AT_FDCWD, getname(oldname), AT_FDCWD,
				getname(newname), 0);
}

int readlink_copy(char __user *buffer, int buflen, const char *link, int linklen)
{
	int copylen;

	copylen = linklen;
	if (unlikely(copylen > (unsigned) buflen))
		copylen = buflen;
	if (copy_to_user(buffer, link, copylen))
		copylen = -EFAULT;
	return copylen;
}

/**
 * vfs_readlink - copy symlink body into userspace buffer
 * @dentry: dentry on which to get symbolic link
 * @buffer: user memory pointer
 * @buflen: size of buffer
 *
 * Does not touch atime.  That's up to the caller if necessary
 *
 * Does not call security hook.
 */
int vfs_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct inode *inode = d_inode(dentry);
	DEFINE_DELAYED_CALL(done);
	const char *link;
	int res;

	if (inode->i_opflags & IOP_CACHED_LINK)
		return readlink_copy(buffer, buflen, inode->i_link, inode->i_linklen);

	if (unlikely(!(inode->i_opflags & IOP_DEFAULT_READLINK))) {
		if (unlikely(inode->i_op->readlink))
			return inode->i_op->readlink(dentry, buffer, buflen);

		if (!d_is_symlink(dentry))
			return -EINVAL;

		spin_lock(&inode->i_lock);
		inode->i_opflags |= IOP_DEFAULT_READLINK;
		spin_unlock(&inode->i_lock);
	}

	link = READ_ONCE(inode->i_link);
	if (!link) {
		link = inode->i_op->get_link(dentry, inode, &done);
		if (IS_ERR(link))
			return PTR_ERR(link);
	}
	res = readlink_copy(buffer, buflen, link, strlen(link));
	do_delayed_call(&done);
	return res;
}
EXPORT_SYMBOL(vfs_readlink);

/**
 * vfs_get_link - get symlink body
 * @dentry: dentry on which to get symbolic link
 * @done: caller needs to free returned data with this
 *
 * Calls security hook and i_op->get_link() on the supplied inode.
 *
 * It does not touch atime.  That's up to the caller if necessary.
 *
 * Does not work on "special" symlinks like /proc/$$/fd/N
 */
const char *vfs_get_link(struct dentry *dentry, struct delayed_call *done)
{
	const char *res = ERR_PTR(-EINVAL);
	struct inode *inode = d_inode(dentry);

	if (d_is_symlink(dentry)) {
		res = ERR_PTR(security_inode_readlink(dentry));
		if (!res)
			res = inode->i_op->get_link(dentry, inode, done);
	}
	return res;
}
EXPORT_SYMBOL(vfs_get_link);

/* get the link contents into pagecache */
static char *__page_get_link(struct dentry *dentry, struct inode *inode,
			     struct delayed_call *callback)
{
	struct folio *folio;
	struct address_space *mapping = inode->i_mapping;

	if (!dentry) {
		folio = filemap_get_folio(mapping, 0);
		if (IS_ERR(folio))
			return ERR_PTR(-ECHILD);
		if (!folio_test_uptodate(folio)) {
			folio_put(folio);
			return ERR_PTR(-ECHILD);
		}
	} else {
		folio = read_mapping_folio(mapping, 0, NULL);
		if (IS_ERR(folio))
			return ERR_CAST(folio);
	}
	set_delayed_call(callback, page_put_link, folio);
	BUG_ON(mapping_gfp_mask(mapping) & __GFP_HIGHMEM);
	return folio_address(folio);
}

const char *page_get_link_raw(struct dentry *dentry, struct inode *inode,
			      struct delayed_call *callback)
{
	return __page_get_link(dentry, inode, callback);
}
EXPORT_SYMBOL_GPL(page_get_link_raw);

/**
 * page_get_link() - An implementation of the get_link inode_operation.
 * @dentry: The directory entry which is the symlink.
 * @inode: The inode for the symlink.
 * @callback: Used to drop the reference to the symlink.
 *
 * Filesystems which store their symlinks in the page cache should use
 * this to implement the get_link() member of their inode_operations.
 *
 * Return: A pointer to the NUL-terminated symlink.
 */
const char *page_get_link(struct dentry *dentry, struct inode *inode,
					struct delayed_call *callback)
{
	char *kaddr = __page_get_link(dentry, inode, callback);

	if (!IS_ERR(kaddr))
		nd_terminate_link(kaddr, inode->i_size, PAGE_SIZE - 1);
	return kaddr;
}
EXPORT_SYMBOL(page_get_link);

/**
 * page_put_link() - Drop the reference to the symlink.
 * @arg: The folio which contains the symlink.
 *
 * This is used internally by page_get_link().  It is exported for use
 * by filesystems which need to implement a variant of page_get_link()
 * themselves.  Despite the apparent symmetry, filesystems which use
 * page_get_link() do not need to call page_put_link().
 *
 * The argument, while it has a void pointer type, must be a pointer to
 * the folio which was retrieved from the page cache.  The delayed_call
 * infrastructure is used to drop the reference count once the caller
 * is done with the symlink.
 */
void page_put_link(void *arg)
{
	folio_put(arg);
}
EXPORT_SYMBOL(page_put_link);

int page_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	const char *link;
	int res;

	DEFINE_DELAYED_CALL(done);
	link = page_get_link(dentry, d_inode(dentry), &done);
	res = PTR_ERR(link);
	if (!IS_ERR(link))
		res = readlink_copy(buffer, buflen, link, strlen(link));
	do_delayed_call(&done);
	return res;
}
EXPORT_SYMBOL(page_readlink);

int page_symlink(struct inode *inode, const char *symname, int len)
{
	struct address_space *mapping = inode->i_mapping;
	const struct address_space_operations *aops = mapping->a_ops;
	bool nofs = !mapping_gfp_constraint(mapping, __GFP_FS);
	struct folio *folio;
	void *fsdata = NULL;
	int err;
	unsigned int flags;

retry:
	if (nofs)
		flags = memalloc_nofs_save();
	err = aops->write_begin(NULL, mapping, 0, len-1, &folio, &fsdata);
	if (nofs)
		memalloc_nofs_restore(flags);
	if (err)
		goto fail;

	memcpy(folio_address(folio), symname, len - 1);

	err = aops->write_end(NULL, mapping, 0, len - 1, len - 1,
						folio, fsdata);
	if (err < 0)
		goto fail;
	if (err < len-1)
		goto retry;

	mark_inode_dirty(inode);
	return 0;
fail:
	return err;
}
EXPORT_SYMBOL(page_symlink);

const struct inode_operations page_symlink_inode_operations = {
	.get_link	= page_get_link,
};
EXPORT_SYMBOL(page_symlink_inode_operations);
