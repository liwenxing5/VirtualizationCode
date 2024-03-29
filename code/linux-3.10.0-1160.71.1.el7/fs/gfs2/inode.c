/*
 * Copyright (C) Sistina Software, Inc.  1997-2003 All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License version 2.
 */

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/namei.h>
#include <linux/mm.h>
#include <linux/xattr.h>
#include <linux/posix_acl.h>
#include <linux/gfs2_ondisk.h>
#include <linux/crc32.h>
#include <linux/iomap.h>
#include <linux/security.h>
#include <asm/uaccess.h>

#include "gfs2.h"
#include "incore.h"
#include "acl.h"
#include "bmap.h"
#include "dir.h"
#include "xattr.h"
#include "glock.h"
#include "inode.h"
#include "meta_io.h"
#include "quota.h"
#include "rgrp.h"
#include "trans.h"
#include "util.h"
#include "super.h"
#include "glops.h"

static int iget_test(struct inode *inode, void *opaque)
{
	u64 no_addr = *(u64 *)opaque;

	return GFS2_I(inode)->i_no_addr == no_addr;
}

static int iget_set(struct inode *inode, void *opaque)
{
	u64 no_addr = *(u64 *)opaque;

	GFS2_I(inode)->i_no_addr = no_addr;
	inode->i_ino = no_addr;
	return 0;
}

static struct inode *gfs2_iget(struct super_block *sb, u64 no_addr)
{
	struct inode *inode;

repeat:
	inode = iget5_locked(sb, no_addr, iget_test, iget_set, &no_addr);
	if (!inode)
		return inode;
	if (is_bad_inode(inode)) {
		iput(inode);
		goto repeat;
	}
	return inode;
}

/**
 * gfs2_set_iop - Sets inode operations
 * @inode: The inode with correct i_mode filled in
 *
 * GFS2 lookup code fills in vfs inode contents based on info obtained
 * from directory entry inside gfs2_inode_lookup().
 */

static void gfs2_set_iop(struct inode *inode)
{
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	umode_t mode = inode->i_mode;

	if (S_ISREG(mode)) {
		inode->i_op = &gfs2_file_iops;
		if (gfs2_localflocks(sdp))
			inode->i_fop = &gfs2_file_fops_nolock;
		else
			inode->i_fop = &gfs2_file_fops;
	} else if (S_ISDIR(mode)) {
		inode->i_flags |= S_IOPS_WRAPPER;
		inode->i_op = &gfs2_dir_iops.ops;
		if (gfs2_localflocks(sdp))
			inode->i_fop = &gfs2_dir_fops_nolock;
		else
			inode->i_fop = &gfs2_dir_fops;
	} else if (S_ISLNK(mode)) {
		inode->i_op = &gfs2_symlink_iops;
	} else {
		inode->i_op = &gfs2_file_iops;
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
	}
}

/**
 * gfs2_inode_lookup - Lookup an inode
 * @sb: The super block
 * @type: The type of the inode
 * @no_addr: The inode number
 * @no_formal_ino: The inode generation number
 * @blktype: Requested block type (GFS2_BLKST_DINODE or GFS2_BLKST_UNLINKED;
 *           GFS2_BLKST_FREE do indicate not to verify)
 *
 * If @type is DT_UNKNOWN, the inode type is fetched from disk.
 *
 * If @blktype is anything other than GFS2_BLKST_FREE (which is used as a
 * placeholder because it doesn't otherwise make sense), the on-disk block type
 * is verified to be @blktype.
 *
 * Returns: A VFS inode, or an error
 */

struct inode *gfs2_inode_lookup(struct super_block *sb, unsigned int type,
				u64 no_addr, u64 no_formal_ino,
				unsigned int blktype)
{
	struct inode *inode;
	struct gfs2_inode *ip;
	struct gfs2_glock *io_gl = NULL;
	struct gfs2_holder i_gh;
	int error;

	gfs2_holder_mark_uninitialized(&i_gh);
	inode = gfs2_iget(sb, no_addr);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ip = GFS2_I(inode);

	if (inode->i_state & I_NEW) {
		struct gfs2_sbd *sdp = GFS2_SB(inode);
		ip->i_no_formal_ino = no_formal_ino;

		error = gfs2_glock_get(sdp, no_addr, &gfs2_inode_glops, CREATE, &ip->i_gl);
		if (unlikely(error))
			goto fail;

		error = gfs2_glock_get(sdp, no_addr, &gfs2_iopen_glops, CREATE, &io_gl);
		if (unlikely(error))
			goto fail;

		if (type == DT_UNKNOWN || blktype != GFS2_BLKST_FREE) {
			/*
			 * The GL_SKIP flag indicates to skip reading the inode
			 * block.  We read the inode with gfs2_inode_refresh
			 * after possibly checking the block type.
			 */
			error = gfs2_glock_nq_init(ip->i_gl, LM_ST_EXCLUSIVE,
						   GL_SKIP, &i_gh);
			if (error)
				goto fail;

			if (blktype != GFS2_BLKST_FREE) {
				error = gfs2_check_blk_type(sdp, no_addr,
							    blktype);
				if (error)
					goto fail;
			}
		}
		flush_delayed_work(&ip->i_gl->gl_work);
		glock_set_object(ip->i_gl, ip);

		set_bit(GIF_INVALID, &ip->i_flags);
		error = gfs2_glock_nq_init(io_gl, LM_ST_SHARED, GL_EXACT, &ip->i_iopen_gh);
		if (unlikely(error))
			goto fail;
		glock_set_object(io_gl, ip);
		gfs2_glock_put(io_gl);
		io_gl = NULL;

		if (type == DT_UNKNOWN) {
			/* Inode glock must be locked already */
			error = gfs2_inode_refresh(GFS2_I(inode));
			if (error)
				goto fail;
		} else {
			inode->i_mode = DT2IF(type);
		}

		gfs2_set_iop(inode);

		/* Lowest possible timestamp; will be overwritten in gfs2_dinode_in. */
		inode->i_atime.tv_sec = 1LL << (8 * sizeof(inode->i_atime.tv_sec) - 1);
		inode->i_atime.tv_nsec = 0;

		unlock_new_inode(inode);
	}

	if (gfs2_holder_initialized(&i_gh))
		gfs2_glock_dq_uninit(&i_gh);
	return inode;

fail:
	if (io_gl)
		gfs2_glock_put(io_gl);
	if (gfs2_holder_initialized(&i_gh))
		gfs2_glock_dq_uninit(&i_gh);
	iget_failed(inode);
	return ERR_PTR(error);
}

struct inode *gfs2_lookup_by_inum(struct gfs2_sbd *sdp, u64 no_addr,
				  u64 *no_formal_ino, unsigned int blktype)
{
	struct super_block *sb = sdp->sd_vfs;
	struct inode *inode;
	int error;

	inode = gfs2_inode_lookup(sb, DT_UNKNOWN, no_addr, 0, blktype);
	if (IS_ERR(inode))
		return inode;

	/* Two extra checks for NFS only */
	if (no_formal_ino) {
		error = -ESTALE;
		if (GFS2_I(inode)->i_no_formal_ino != *no_formal_ino)
			goto fail_iput;

		error = -EIO;
		if (GFS2_I(inode)->i_diskflags & GFS2_DIF_SYSTEM)
			goto fail_iput;
	}
	return inode;

fail_iput:
	iput(inode);
	return ERR_PTR(error);
}


struct inode *gfs2_lookup_simple(struct inode *dip, const char *name)
{
	struct qstr qstr;
	struct inode *inode;
	gfs2_str2qstr(&qstr, name);
	inode = gfs2_lookupi(dip, &qstr, 1);
	/* gfs2_lookupi has inconsistent callers: vfs
	 * related routines expect NULL for no entry found,
	 * gfs2_lookup_simple callers expect ENOENT
	 * and do not check for NULL.
	 */
	if (inode == NULL)
		return ERR_PTR(-ENOENT);
	else
		return inode;
}


/**
 * gfs2_lookupi - Look up a filename in a directory and return its inode
 * @d_gh: An initialized holder for the directory glock
 * @name: The name of the inode to look for
 * @is_root: If 1, ignore the caller's permissions
 * @i_gh: An uninitialized holder for the new inode glock
 *
 * This can be called via the VFS filldir function when NFS is doing
 * a readdirplus and the inode which its intending to stat isn't
 * already in cache. In this case we must not take the directory glock
 * again, since the readdir call will have already taken that lock.
 *
 * Returns: errno
 */

struct inode *gfs2_lookupi(struct inode *dir, const struct qstr *name,
			   int is_root)
{
	struct super_block *sb = dir->i_sb;
	struct gfs2_inode *dip = GFS2_I(dir);
	struct gfs2_holder d_gh;
	int error = 0;
	struct inode *inode = NULL;

	gfs2_holder_mark_uninitialized(&d_gh);
	if (!name->len || name->len > GFS2_FNAMESIZE)
		return ERR_PTR(-ENAMETOOLONG);

	if ((name->len == 1 && memcmp(name->name, ".", 1) == 0) ||
	    (name->len == 2 && memcmp(name->name, "..", 2) == 0 &&
	     dir == sb->s_root->d_inode)) {
		igrab(dir);
		return dir;
	}

	if (gfs2_glock_is_locked_by_me(dip->i_gl) == NULL) {
		error = gfs2_glock_nq_init(dip->i_gl, LM_ST_SHARED, 0, &d_gh);
		if (error)
			return ERR_PTR(error);
	}

	if (!is_root) {
		error = gfs2_permission(dir, MAY_EXEC);
		if (error)
			goto out;
	}

	inode = gfs2_dir_search(dir, name, false);
	if (IS_ERR(inode))
		error = PTR_ERR(inode);
out:
	if (gfs2_holder_initialized(&d_gh))
		gfs2_glock_dq_uninit(&d_gh);
	if (error == -ENOENT)
		return NULL;
	return inode ? inode : ERR_PTR(error);
}

/**
 * create_ok - OK to create a new on-disk inode here?
 * @dip:  Directory in which dinode is to be created
 * @name:  Name of new dinode
 * @mode:
 *
 * Returns: errno
 */

static int create_ok(struct gfs2_inode *dip, const struct qstr *name,
		     umode_t mode)
{
	int error;

	error = gfs2_permission(&dip->i_inode, MAY_WRITE | MAY_EXEC);
	if (error)
		return error;

	/*  Don't create entries in an unlinked directory  */
	if (!dip->i_inode.i_nlink)
		return -ENOENT;

	if (dip->i_entries == (u32)-1)
		return -EFBIG;
	if (S_ISDIR(mode) && dip->i_inode.i_nlink == (u32)-1)
		return -EMLINK;

	return 0;
}

static void munge_mode_uid_gid(const struct gfs2_inode *dip,
			       struct inode *inode)
{
	if (GFS2_SB(&dip->i_inode)->sd_args.ar_suiddir &&
	    (dip->i_inode.i_mode & S_ISUID) &&
	    !uid_eq(dip->i_inode.i_uid, GLOBAL_ROOT_UID)) {
		if (S_ISDIR(inode->i_mode))
			inode->i_mode |= S_ISUID;
		else if (!uid_eq(dip->i_inode.i_uid, current_fsuid()))
			inode->i_mode &= ~07111;
		inode->i_uid = dip->i_inode.i_uid;
	} else
		inode->i_uid = current_fsuid();

	if (dip->i_inode.i_mode & S_ISGID) {
		if (S_ISDIR(inode->i_mode))
			inode->i_mode |= S_ISGID;
		inode->i_gid = dip->i_inode.i_gid;
	} else
		inode->i_gid = current_fsgid();
}

static int alloc_dinode(struct gfs2_inode *ip, u32 flags)
{
	struct gfs2_sbd *sdp = GFS2_SB(&ip->i_inode);
	struct gfs2_alloc_parms ap = { .target = RES_DINODE, .aflags = flags, };
	int error;
	int dblocks = 1;

	error = gfs2_quota_lock_check(ip, &ap);
	if (error)
		goto out;

	error = gfs2_inplace_reserve(ip, &ap);
	if (error)
		goto out_quota;

	error = gfs2_trans_begin(sdp, RES_RG_BIT + RES_STATFS + RES_QUOTA, 0);
	if (error)
		goto out_ipreserv;

	error = gfs2_alloc_blocks(ip, &ip->i_no_addr, &dblocks, 1, &ip->i_generation);
	ip->i_no_formal_ino = ip->i_generation;
	ip->i_inode.i_ino = ip->i_no_addr;
	ip->i_goal = ip->i_no_addr;

	gfs2_trans_end(sdp);

out_ipreserv:
	gfs2_inplace_release(ip);
out_quota:
	gfs2_quota_unlock(ip);
out:
	return error;
}

static void gfs2_init_dir(struct buffer_head *dibh,
			  const struct gfs2_inode *parent)
{
	struct gfs2_dinode *di = (struct gfs2_dinode *)dibh->b_data;
	struct gfs2_dirent *dent = (struct gfs2_dirent *)(di+1);

	gfs2_qstr2dirent(&gfs2_qdot, GFS2_DIRENT_SIZE(gfs2_qdot.len), dent);
	dent->de_inum = di->di_num; /* already GFS2 endian */
	dent->de_type = cpu_to_be16(DT_DIR);

	dent = (struct gfs2_dirent *)((char*)dent + GFS2_DIRENT_SIZE(1));
	gfs2_qstr2dirent(&gfs2_qdotdot, dibh->b_size - GFS2_DIRENT_SIZE(1) - sizeof(struct gfs2_dinode), dent);
	gfs2_inum_out(parent, dent);
	dent->de_type = cpu_to_be16(DT_DIR);
	
}

/**
 * init_dinode - Fill in a new dinode structure
 * @dip: The directory this inode is being created in
 * @ip: The inode
 * @symname: The symlink destination (if a symlink)
 * @bhp: The buffer head (returned to caller)
 *
 */

static void init_dinode(struct gfs2_inode *dip, struct gfs2_inode *ip,
			const char *symname)
{
	struct gfs2_dinode *di;
	struct buffer_head *dibh;

	dibh = gfs2_meta_new(ip->i_gl, ip->i_no_addr);
	gfs2_trans_add_meta(ip->i_gl, dibh);
	di = (struct gfs2_dinode *)dibh->b_data;
	gfs2_dinode_out(ip, di);

	di->di_major = cpu_to_be32(MAJOR(ip->i_inode.i_rdev));
	di->di_minor = cpu_to_be32(MINOR(ip->i_inode.i_rdev));
	di->__pad1 = 0;
	di->__pad2 = 0;
	di->__pad3 = 0;
	memset(&di->__pad4, 0, sizeof(di->__pad4));
	memset(&di->di_reserved, 0, sizeof(di->di_reserved));
	gfs2_buffer_clear_tail(dibh, sizeof(struct gfs2_dinode));

	switch(ip->i_inode.i_mode & S_IFMT) {
	case S_IFDIR:
		gfs2_init_dir(dibh, dip);
		break;
	case S_IFLNK:
		memcpy(dibh->b_data + sizeof(struct gfs2_dinode), symname, ip->i_inode.i_size);
		break;
	}

	set_buffer_uptodate(dibh);
	brelse(dibh);
}

static int link_dinode(struct gfs2_inode *dip, const struct qstr *name,
		       struct gfs2_inode *ip, int arq)
{
	struct gfs2_sbd *sdp = GFS2_SB(&dip->i_inode);
	struct gfs2_alloc_parms ap = { .target = sdp->sd_max_dirres, };
	int error;

	if (arq) {
		error = gfs2_quota_lock_check(dip, &ap);
		if (error)
			goto fail_quota_locks;

		error = gfs2_inplace_reserve(dip, &ap);
		if (error)
			goto fail_quota_locks;

		error = gfs2_trans_begin(sdp, sdp->sd_max_dirres +
					 dip->i_rgd->rd_length +
					 2 * RES_DINODE +
					 RES_STATFS + RES_QUOTA, 0);
		if (error)
			goto fail_ipreserv;
	} else {
		error = gfs2_trans_begin(sdp, RES_LEAF + 2 * RES_DINODE, 0);
		if (error)
			goto fail_quota_locks;
	}

	error = gfs2_dir_add(&dip->i_inode, name, ip);
	if (error)
		goto fail_end_trans;

fail_end_trans:
	gfs2_trans_end(sdp);
fail_ipreserv:
	gfs2_inplace_release(dip);
fail_quota_locks:
	gfs2_quota_unlock(dip);
	return error;
}

static int gfs2_initxattrs(struct inode *inode, const struct xattr *xattr_array,
		    void *fs_info)
{
	const struct xattr *xattr;
	int err = 0;

	for (xattr = xattr_array; xattr->name != NULL; xattr++) {
		err = __gfs2_xattr_set(inode, xattr->name, xattr->value,
				       xattr->value_len, 0,
				       GFS2_EATYPE_SECURITY);
		if (err < 0)
			break;
	}
	return err;
}

static int gfs2_security_init(struct gfs2_inode *dip, struct gfs2_inode *ip,
			      const struct qstr *qstr)
{
	return security_inode_init_security(&ip->i_inode, &dip->i_inode, qstr,
					    &gfs2_initxattrs, NULL);
}

/**
 * gfs2_create_inode - Create a new inode
 * @dir: The parent directory
 * @dentry: The new dentry
 * @file: If non-NULL, the file which is being opened
 * @mode: The permissions on the new inode
 * @dev: For device nodes, this is the device number
 * @symname: For symlinks, this is the link destination
 * @size: The initial size of the inode (ignored for directories)
 *
 * Returns: 0 on success, or error code
 */

static int gfs2_create_inode(struct inode *dir, struct dentry *dentry,
			     struct file *file,
			     umode_t mode, dev_t dev, const char *symname,
			     unsigned int size, int excl, int *opened)
{
	const struct qstr *name = &dentry->d_name;
	struct gfs2_holder ghs[2];
	struct inode *inode = NULL;
	struct gfs2_inode *dip = GFS2_I(dir), *ip;
	struct gfs2_sbd *sdp = GFS2_SB(&dip->i_inode);
	struct gfs2_glock *io_gl = NULL;
	int error, free_vfs_inode = 1;
	u32 aflags = 0;
	int arq;

	if (!name->len || name->len > GFS2_FNAMESIZE)
		return -ENAMETOOLONG;

	error = gfs2_qa_get(dip);
	if (error)
		return error;

	error = gfs2_rindex_update(sdp);
	if (error)
		goto fail;

	error = gfs2_glock_nq_init(dip->i_gl, LM_ST_EXCLUSIVE, 0, ghs);
	if (error)
		goto fail;
	gfs2_holder_mark_uninitialized(ghs + 1);

	error = create_ok(dip, name, mode);
	if (error)
		goto fail_gunlock;

	inode = gfs2_dir_search(dir, &dentry->d_name, !S_ISREG(mode) || excl);
	error = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		if (S_ISDIR(inode->i_mode)) {
			iput(inode);
			inode = ERR_PTR(-EISDIR);
			goto fail_gunlock;
		}
		d_instantiate(dentry, inode);
		error = 0;
		if (file) {
			if (S_ISREG(inode->i_mode))
				error = finish_open(file, dentry, gfs2_open_common, opened);
			else
				error = finish_no_open(file, NULL);
		}
		gfs2_glock_dq_uninit(ghs);
		goto fail;
	} else if (error != -ENOENT) {
		goto fail_gunlock;
	}

	arq = error = gfs2_diradd_alloc_required(dir, name);
	if (error < 0)
		goto fail_gunlock;

	inode = new_inode(sdp->sd_vfs);
	error = -ENOMEM;
	if (!inode)
		goto fail_gunlock;

	ip = GFS2_I(inode);
	error = gfs2_qa_get(ip);
	if (error)
		goto fail_free_inode;

	inode->i_mode = mode;
	set_nlink(inode, S_ISDIR(mode) ? 2 : 1);
	inode->i_rdev = dev;
	inode->i_size = size;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	gfs2_set_inode_blocks(inode, 1);
	munge_mode_uid_gid(dip, inode);
	check_and_update_goal(dip);
	ip->i_goal = dip->i_goal;
	ip->i_diskflags = 0;
	ip->i_eattr = 0;
	ip->i_height = 0;
	ip->i_depth = 0;
	ip->i_entries = 0;

	switch(mode & S_IFMT) {
	case S_IFREG:
		if ((dip->i_diskflags & GFS2_DIF_INHERIT_JDATA) ||
		    gfs2_tune_get(sdp, gt_new_files_jdata))
			ip->i_diskflags |= GFS2_DIF_JDATA;
		gfs2_set_aops(inode);
		break;
	case S_IFDIR:
		ip->i_diskflags |= (dip->i_diskflags & GFS2_DIF_INHERIT_JDATA);
		ip->i_diskflags |= GFS2_DIF_JDATA;
		ip->i_entries = 2;
		break;
	}

	/* Force SYSTEM flag on all files and subdirs of a SYSTEM directory */
	if (dip->i_diskflags & GFS2_DIF_SYSTEM)
		ip->i_diskflags |= GFS2_DIF_SYSTEM;

	gfs2_set_inode_flags(inode);

	if ((GFS2_I(sdp->sd_root_dir->d_inode) == dip) ||
	    (dip->i_diskflags & GFS2_DIF_TOPDIR))
		aflags |= GFS2_AF_ORLOV;

	error = alloc_dinode(ip, aflags);
	if (error)
		goto fail_free_inode;

	error = gfs2_glock_get(sdp, ip->i_no_addr, &gfs2_inode_glops, CREATE, &ip->i_gl);
	if (error)
		goto fail_free_inode;
	flush_delayed_work(&ip->i_gl->gl_work);

	glock_set_object(ip->i_gl, ip);
	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_EXCLUSIVE, GL_SKIP, ghs + 1);
	if (error)
		goto fail_free_inode;

	error = gfs2_trans_begin(sdp, RES_DINODE, 0);
	if (error)
		goto fail_gunlock2;

	init_dinode(dip, ip, symname);
	gfs2_trans_end(sdp);

	error = gfs2_glock_get(sdp, ip->i_no_addr, &gfs2_iopen_glops, CREATE, &io_gl);
	if (error)
		goto fail_free_inode;

	BUG_ON(test_and_set_bit(GLF_INODE_CREATING, &io_gl->gl_flags));

	error = gfs2_glock_nq_init(io_gl, LM_ST_SHARED, GL_EXACT, &ip->i_iopen_gh);
	if (error)
		goto fail_free_inode;

	glock_set_object(io_gl, ip);
	gfs2_set_iop(inode);
	insert_inode_hash(inode);

	free_vfs_inode = 0; /* After this point, the inode is no longer
			       considered free. Any failures need to undo
			       the gfs2 structures. */

	error = gfs2_acl_create(dip, inode);
	if (error)
		goto fail_gunlock3;

	error = gfs2_security_init(dip, ip, name);
	if (error)
		goto fail_gunlock3;

	error = link_dinode(dip, name, ip, arq);
	if (error)
		goto fail_gunlock3;

	mark_inode_dirty(inode);
	d_instantiate(dentry, inode);
	/* After instantiate, errors should result in evict which will destroy
	 * both inode and iopen glocks properly. */
	if (file) {
		*opened |= FILE_CREATED;
		error = finish_open(file, dentry, gfs2_open_common, opened);
	}
	gfs2_glock_dq_uninit(ghs);
	gfs2_qa_put(ip);
	gfs2_glock_dq_uninit(ghs + 1);
	clear_bit(GLF_INODE_CREATING, &io_gl->gl_flags);
	gfs2_qa_put(dip);
	gfs2_glock_put(io_gl);
	return error;

fail_gunlock3:
	glock_clear_object(io_gl, ip);
	gfs2_glock_dq_uninit(&ip->i_iopen_gh);
fail_gunlock2:
	clear_bit(GLF_INODE_CREATING, &io_gl->gl_flags);
	gfs2_glock_put(io_gl);
fail_free_inode:
	if (ip->i_gl) {
		glock_clear_object(ip->i_gl, ip);
		gfs2_glock_put(ip->i_gl);
	}
	gfs2_rs_delete(ip, NULL);
	gfs2_qa_put(ip);
fail_gunlock:
	gfs2_glock_dq_uninit(ghs);
	if (inode && !IS_ERR(inode)) {
		clear_nlink(inode);
		if (!free_vfs_inode)
			mark_inode_dirty(inode);
		set_bit(free_vfs_inode ? GIF_FREE_VFS_INODE : GIF_ALLOC_FAILED,
			&GFS2_I(inode)->i_flags);
		iput(inode);
	}
	if (gfs2_holder_initialized(ghs + 1))
		gfs2_glock_dq_uninit(ghs + 1);
fail:
	gfs2_qa_put(dip);
	return error;
}

/**
 * gfs2_create - Create a file
 * @dir: The directory in which to create the file
 * @dentry: The dentry of the new file
 * @mode: The mode of the new file
 *
 * Returns: errno
 */

static int gfs2_create(struct inode *dir, struct dentry *dentry,
		       umode_t mode, bool excl)
{
	return gfs2_create_inode(dir, dentry, NULL, S_IFREG | mode, 0, NULL, 0, excl, NULL);
}

/**
 * __gfs2_lookup - Look up a filename in a directory and return its inode
 * @dir: The directory inode
 * @dentry: The dentry of the new inode
 * @file: File to be opened
 * @opened: atomic_open flags
 *
 *
 * Returns: errno
 */

static struct dentry *__gfs2_lookup(struct inode *dir, struct dentry *dentry,
				    struct file *file, int *opened)
{
	struct inode *inode;
	struct dentry *d;
	struct gfs2_holder gh;
	struct gfs2_glock *gl;
	int error;

	inode = gfs2_lookupi(dir, &dentry->d_name, 0);
	if (inode == NULL) {
		d_add(dentry, NULL);
		return NULL;
	}
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	gl = GFS2_I(inode)->i_gl;
	error = gfs2_glock_nq_init(gl, LM_ST_SHARED, LM_FLAG_ANY, &gh);
	if (error) {
		iput(inode);
		return ERR_PTR(error);
	}

	d = d_materialise_unique(dentry, inode);
	if (d) {
		if (IS_ERR(d)) {
			gfs2_glock_dq_uninit(&gh);
			return d;
		}
		dentry = d;
	}
	if (file && S_ISREG(inode->i_mode))
		error = finish_open(file, dentry, gfs2_open_common, opened);

	gfs2_glock_dq_uninit(&gh);
	if (error) {
		dput(d);
		return ERR_PTR(error);
	}
	return d;
}

static struct dentry *gfs2_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned flags)
{
	return __gfs2_lookup(dir, dentry, NULL, NULL);
}

/**
 * gfs2_link - Link to a file
 * @old_dentry: The inode to link
 * @dir: Add link to this directory
 * @dentry: The name of the link
 *
 * Link the inode in "old_dentry" into the directory "dir" with the
 * name in "dentry".
 *
 * Returns: errno
 */

static int gfs2_link(struct dentry *old_dentry, struct inode *dir,
		     struct dentry *dentry)
{
	struct gfs2_inode *dip = GFS2_I(dir);
	struct gfs2_sbd *sdp = GFS2_SB(dir);
	struct inode *inode = old_dentry->d_inode;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder ghs[2];
	struct buffer_head *dibh;
	int alloc_required;
	int error;

	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	error = gfs2_qa_get(dip);
	if (error)
		return error;

	gfs2_holder_init(dip->i_gl, LM_ST_EXCLUSIVE, 0, ghs);
	gfs2_holder_init(ip->i_gl, LM_ST_EXCLUSIVE, 0, ghs + 1);

	error = gfs2_glock_nq(ghs); /* parent */
	if (error)
		goto out_parent;

	error = gfs2_glock_nq(ghs + 1); /* child */
	if (error)
		goto out_child;

	error = -ENOENT;
	if (inode->i_nlink == 0)
		goto out_gunlock;

	error = gfs2_permission(dir, MAY_WRITE | MAY_EXEC);
	if (error)
		goto out_gunlock;

	error = gfs2_dir_check(dir, &dentry->d_name, NULL);
	switch (error) {
	case -ENOENT:
		break;
	case 0:
		error = -EEXIST;
	default:
		goto out_gunlock;
	}

	error = -EINVAL;
	if (!dip->i_inode.i_nlink)
		goto out_gunlock;
	error = -EFBIG;
	if (dip->i_entries == (u32)-1)
		goto out_gunlock;
	error = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto out_gunlock;
	error = -EINVAL;
	if (!ip->i_inode.i_nlink)
		goto out_gunlock;
	error = -EMLINK;
	if (ip->i_inode.i_nlink == (u32)-1)
		goto out_gunlock;

	alloc_required = error = gfs2_diradd_alloc_required(dir, &dentry->d_name);
	if (error < 0)
		goto out_gunlock;
	error = 0;

	if (alloc_required) {
		struct gfs2_alloc_parms ap = { .target = sdp->sd_max_dirres, };
		error = gfs2_quota_lock_check(dip, &ap);
		if (error)
			goto out_gunlock;

		error = gfs2_inplace_reserve(dip, &ap);
		if (error)
			goto out_gunlock_q;

		error = gfs2_trans_begin(sdp, sdp->sd_max_dirres +
					 gfs2_rg_blocks(dip, sdp->sd_max_dirres) +
					 2 * RES_DINODE + RES_STATFS +
					 RES_QUOTA, 0);
		if (error)
			goto out_ipres;
	} else {
		error = gfs2_trans_begin(sdp, 2 * RES_DINODE + RES_LEAF, 0);
		if (error)
			goto out_ipres;
	}

	error = gfs2_meta_inode_buffer(ip, &dibh);
	if (error)
		goto out_end_trans;

	error = gfs2_dir_add(dir, &dentry->d_name, ip);
	if (error)
		goto out_brelse;

	gfs2_trans_add_meta(ip->i_gl, dibh);
	inc_nlink(&ip->i_inode);
	ip->i_inode.i_ctime = CURRENT_TIME;
	ihold(inode);
	d_instantiate(dentry, inode);
	mark_inode_dirty(inode);

out_brelse:
	brelse(dibh);
out_end_trans:
	gfs2_trans_end(sdp);
out_ipres:
	if (alloc_required)
		gfs2_inplace_release(dip);
out_gunlock_q:
	if (alloc_required)
		gfs2_quota_unlock(dip);
out_gunlock:
	gfs2_glock_dq(ghs + 1);
out_child:
	gfs2_glock_dq(ghs);
out_parent:
	gfs2_qa_put(dip);
	gfs2_holder_uninit(ghs);
	gfs2_holder_uninit(ghs + 1);
	return error;
}

/*
 * gfs2_unlink_ok - check to see that a inode is still in a directory
 * @dip: the directory
 * @name: the name of the file
 * @ip: the inode
 *
 * Assumes that the lock on (at least) @dip is held.
 *
 * Returns: 0 if the parent/child relationship is correct, errno if it isn't
 */

static int gfs2_unlink_ok(struct gfs2_inode *dip, const struct qstr *name,
			  const struct gfs2_inode *ip)
{
	int error;

	if (IS_IMMUTABLE(&ip->i_inode) || IS_APPEND(&ip->i_inode))
		return -EPERM;

	if ((dip->i_inode.i_mode & S_ISVTX) &&
	    !uid_eq(dip->i_inode.i_uid, current_fsuid()) &&
	    !uid_eq(ip->i_inode.i_uid, current_fsuid()) && !capable(CAP_FOWNER))
		return -EPERM;

	if (IS_APPEND(&dip->i_inode))
		return -EPERM;

	error = gfs2_permission(&dip->i_inode, MAY_WRITE | MAY_EXEC);
	if (error)
		return error;

	error = gfs2_dir_check(&dip->i_inode, name, ip);
	if (error)
		return error;

	return 0;
}

/**
 * gfs2_unlink_inode - Removes an inode from its parent dir and unlinks it
 * @dip: The parent directory
 * @name: The name of the entry in the parent directory
 * @inode: The inode to be removed
 *
 * Called with all the locks and in a transaction. This will only be
 * called for a directory after it has been checked to ensure it is empty.
 *
 * Returns: 0 on success, or an error
 */

static int gfs2_unlink_inode(struct gfs2_inode *dip,
			     const struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct gfs2_inode *ip = GFS2_I(inode);
	int error;

	error = gfs2_dir_del(dip, dentry);
	if (error)
		return error;

	ip->i_entries = 0;
	inode->i_ctime = CURRENT_TIME;
	if (S_ISDIR(inode->i_mode))
		clear_nlink(inode);
	else
		drop_nlink(inode);
	mark_inode_dirty(inode);
	if (inode->i_nlink == 0)
		gfs2_unlink_di(inode);
	return 0;
}


/**
 * gfs2_unlink - Unlink an inode (this does rmdir as well)
 * @dir: The inode of the directory containing the inode to unlink
 * @dentry: The file itself
 *
 * This routine uses the type of the inode as a flag to figure out
 * whether this is an unlink or an rmdir.
 *
 * Returns: errno
 */

static int gfs2_unlink(struct inode *dir, struct dentry *dentry)
{
	struct gfs2_inode *dip = GFS2_I(dir);
	struct gfs2_sbd *sdp = GFS2_SB(dir);
	struct inode *inode = dentry->d_inode;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder ghs[3];
	struct gfs2_rgrpd *rgd;
	int error;

	error = gfs2_rindex_update(sdp);
	if (error)
		return error;

	error = -EROFS;

	gfs2_holder_init(dip->i_gl, LM_ST_EXCLUSIVE, 0, ghs);
	gfs2_holder_init(ip->i_gl,  LM_ST_EXCLUSIVE, 0, ghs + 1);

	rgd = gfs2_blk2rgrpd(sdp, ip->i_no_addr, 1);
	if (!rgd)
		goto out_inodes;

	gfs2_holder_init(rgd->rd_gl, LM_ST_EXCLUSIVE, 0, ghs + 2);


	error = gfs2_glock_nq(ghs); /* parent */
	if (error)
		goto out_parent;

	error = gfs2_glock_nq(ghs + 1); /* child */
	if (error)
		goto out_child;

	error = -ENOENT;
	if (inode->i_nlink == 0)
		goto out_rgrp;

	if (S_ISDIR(inode->i_mode)) {
		error = -ENOTEMPTY;
		if (ip->i_entries > 2 || inode->i_nlink > 2)
			goto out_rgrp;
	}

	error = gfs2_glock_nq(ghs + 2); /* rgrp */
	if (error)
		goto out_rgrp;

	error = gfs2_unlink_ok(dip, &dentry->d_name, ip);
	if (error)
		goto out_gunlock;

	error = gfs2_trans_begin(sdp, 2*RES_DINODE + 3*RES_LEAF + RES_RG_BIT, 0);
	if (error)
		goto out_end_trans;

	error = gfs2_unlink_inode(dip, dentry);

out_end_trans:
	gfs2_trans_end(sdp);
out_gunlock:
	gfs2_glock_dq(ghs + 2);
out_rgrp:
	gfs2_glock_dq(ghs + 1);
out_child:
	gfs2_glock_dq(ghs);
out_parent:
	gfs2_holder_uninit(ghs + 2);
out_inodes:
	gfs2_holder_uninit(ghs + 1);
	gfs2_holder_uninit(ghs);
	return error;
}

/**
 * gfs2_symlink - Create a symlink
 * @dir: The directory to create the symlink in
 * @dentry: The dentry to put the symlink in
 * @symname: The thing which the link points to
 *
 * Returns: errno
 */

static int gfs2_symlink(struct inode *dir, struct dentry *dentry,
			const char *symname)
{
	unsigned int size;

	size = strlen(symname);
	if (size >= gfs2_max_stuffed_size(GFS2_I(dir)))
		return -ENAMETOOLONG;

	return gfs2_create_inode(dir, dentry, NULL, S_IFLNK | S_IRWXUGO, 0, symname, size, 0, NULL);
}

/**
 * gfs2_mkdir - Make a directory
 * @dir: The parent directory of the new one
 * @dentry: The dentry of the new directory
 * @mode: The mode of the new directory
 *
 * Returns: errno
 */

static int gfs2_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	unsigned dsize = gfs2_max_stuffed_size(GFS2_I(dir));
	return gfs2_create_inode(dir, dentry, NULL, S_IFDIR | mode, 0, NULL, dsize, 0, NULL);
}

/**
 * gfs2_mknod - Make a special file
 * @dir: The directory in which the special file will reside
 * @dentry: The dentry of the special file
 * @mode: The mode of the special file
 * @dev: The device specification of the special file
 *
 */

static int gfs2_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
		      dev_t dev)
{
	return gfs2_create_inode(dir, dentry, NULL, mode, dev, NULL, 0, 0, NULL);
}

/**
 * gfs2_atomic_open - Atomically open a file
 * @dir: The directory
 * @dentry: The proposed new entry
 * @file: The proposed new struct file
 * @flags: open flags
 * @mode: File mode
 * @opened: Flag to say whether the file has been opened or not
 *
 * Returns: error code or 0 for success
 */

static int gfs2_atomic_open(struct inode *dir, struct dentry *dentry,
                            struct file *file, unsigned flags,
                            umode_t mode, int *opened)
{
	struct dentry *d;
	bool excl = !!(flags & O_EXCL);

	if (!d_unhashed(dentry))
		goto skip_lookup;

	d = __gfs2_lookup(dir, dentry, file, opened);
	if (IS_ERR(d))
		return PTR_ERR(d);
	if (d != NULL)
		dentry = d;
	if (dentry->d_inode) {
		if (!(*opened & FILE_OPENED)) {
			if (d == NULL)
				dget(dentry);
			return finish_no_open(file, dentry);
		}
		dput(d);
		if (excl && (flags & O_CREAT)) {
			fput(file);
			return -EEXIST;
		}
		return 0;
	}

	BUG_ON(d != NULL);

skip_lookup:
	if (!(flags & O_CREAT))
		return -ENOENT;

	return gfs2_create_inode(dir, dentry, file, S_IFREG | mode, 0, NULL, 0, excl, opened);
}

/*
 * gfs2_ok_to_move - check if it's ok to move a directory to another directory
 * @this: move this
 * @to: to here
 *
 * Follow @to back to the root and make sure we don't encounter @this
 * Assumes we already hold the rename lock.
 *
 * Returns: errno
 */

static int gfs2_ok_to_move(struct gfs2_inode *this, struct gfs2_inode *to)
{
	struct inode *dir = &to->i_inode;
	struct super_block *sb = dir->i_sb;
	struct inode *tmp;
	int error = 0;

	igrab(dir);

	for (;;) {
		if (dir == &this->i_inode) {
			error = -EINVAL;
			break;
		}
		if (dir == sb->s_root->d_inode) {
			error = 0;
			break;
		}

		tmp = gfs2_lookupi(dir, &gfs2_qdotdot, 1);
		if (!tmp) {
			error = -ENOENT;
			break;
		}
		if (IS_ERR(tmp)) {
			error = PTR_ERR(tmp);
			break;
		}

		iput(dir);
		dir = tmp;
	}

	iput(dir);

	return error;
}

/**
 * update_moved_ino - Update an inode that's being moved
 * @ip: The inode being moved
 * @ndip: The parent directory of the new filename
 * @dir_rename: True of ip is a directory
 *
 * Returns: errno
 */

static int update_moved_ino(struct gfs2_inode *ip, struct gfs2_inode *ndip,
			    int dir_rename)
{
	if (dir_rename)
		return gfs2_dir_mvino(ip, &gfs2_qdotdot, ndip, DT_DIR);

	ip->i_inode.i_ctime = CURRENT_TIME;
	mark_inode_dirty_sync(&ip->i_inode);
	return 0;
}


/**
 * gfs2_rename - Rename a file
 * @odir: Parent directory of old file name
 * @odentry: The old dentry of the file
 * @ndir: Parent directory of new file name
 * @ndentry: The new dentry of the file
 *
 * Returns: errno
 */

static int gfs2_rename(struct inode *odir, struct dentry *odentry,
		       struct inode *ndir, struct dentry *ndentry)
{
	struct gfs2_inode *odip = GFS2_I(odir);
	struct gfs2_inode *ndip = GFS2_I(ndir);
	struct gfs2_inode *ip = GFS2_I(odentry->d_inode);
	struct gfs2_inode *nip = NULL;
	struct gfs2_sbd *sdp = GFS2_SB(odir);
	struct gfs2_holder ghs[4], r_gh, rd_gh;
	struct gfs2_rgrpd *nrgd;
	unsigned int num_gh;
	int dir_rename = 0;
	int alloc_required = 0;
	unsigned int x;
	int error;

	gfs2_holder_mark_uninitialized(&r_gh);
	gfs2_holder_mark_uninitialized(&rd_gh);
	if (ndentry->d_inode) {
		nip = GFS2_I(ndentry->d_inode);
		if (ip == nip)
			return 0;
	}

	error = gfs2_rindex_update(sdp);
	if (error)
		return error;

	error = gfs2_qa_get(ndip);
	if (error)
		return error;

	if (odip != ndip) {
		error = gfs2_glock_nq_init(sdp->sd_rename_gl, LM_ST_EXCLUSIVE,
					   0, &r_gh);
		if (error)
			goto out;

		if (S_ISDIR(ip->i_inode.i_mode)) {
			dir_rename = 1;
			/* don't move a directory into its subdir */
			error = gfs2_ok_to_move(ip, ndip);
			if (error)
				goto out_gunlock_r;
		}
	}

	num_gh = 1;
	gfs2_holder_init(odip->i_gl, LM_ST_EXCLUSIVE, GL_ASYNC, ghs);
	if (odip != ndip) {
		gfs2_holder_init(ndip->i_gl, LM_ST_EXCLUSIVE,GL_ASYNC,
				 ghs + num_gh);
		num_gh++;
	}
	gfs2_holder_init(ip->i_gl, LM_ST_EXCLUSIVE, GL_ASYNC, ghs + num_gh);
	num_gh++;

	if (nip) {
		gfs2_holder_init(nip->i_gl, LM_ST_EXCLUSIVE, GL_ASYNC,
				 ghs + num_gh);
		num_gh++;
	}

	for (x = 0; x < num_gh; x++) {
		error = gfs2_glock_nq(ghs + x);
		if (error)
			goto out_gunlock;
	}
	error = gfs2_glock_async_wait(num_gh, ghs);
	if (error)
		goto out_gunlock;

	if (nip) {
		/* Grab the resource group glock for unlink flag twiddling.
		 * This is the case where the target dinode already exists
		 * so we unlink before doing the rename.
		 */
		nrgd = gfs2_blk2rgrpd(sdp, nip->i_no_addr, 1);
		if (!nrgd) {
			error = -ENOENT;
			goto out_gunlock;
		}
		error = gfs2_glock_nq_init(nrgd->rd_gl, LM_ST_EXCLUSIVE, 0,
					   &rd_gh);
		if (error)
			goto out_gunlock;
	}

	error = -ENOENT;
	if (ip->i_inode.i_nlink == 0)
		goto out_gunlock;

	/* Check out the old directory */

	error = gfs2_unlink_ok(odip, &odentry->d_name, ip);
	if (error)
		goto out_gunlock;

	/* Check out the new directory */

	if (nip) {
		error = gfs2_unlink_ok(ndip, &ndentry->d_name, nip);
		if (error)
			goto out_gunlock;

		if (nip->i_inode.i_nlink == 0) {
			error = -EAGAIN;
			goto out_gunlock;
		}

		if (S_ISDIR(nip->i_inode.i_mode)) {
			if (nip->i_entries < 2) {
				gfs2_consist_inode(nip);
				error = -EIO;
				goto out_gunlock;
			}
			if (nip->i_entries > 2) {
				error = -ENOTEMPTY;
				goto out_gunlock;
			}
		}
	} else {
		error = gfs2_permission(ndir, MAY_WRITE | MAY_EXEC);
		if (error)
			goto out_gunlock;

		error = gfs2_dir_check(ndir, &ndentry->d_name, NULL);
		switch (error) {
		case -ENOENT:
			error = 0;
			break;
		case 0:
			error = -EEXIST;
		default:
			goto out_gunlock;
		};

		if (odip != ndip) {
			if (!ndip->i_inode.i_nlink) {
				error = -ENOENT;
				goto out_gunlock;
			}
			if (ndip->i_entries == (u32)-1) {
				error = -EFBIG;
				goto out_gunlock;
			}
			if (S_ISDIR(ip->i_inode.i_mode) &&
			    ndip->i_inode.i_nlink == (u32)-1) {
				error = -EMLINK;
				goto out_gunlock;
			}
		}
	}

	/* Check out the dir to be renamed */

	if (dir_rename) {
		error = gfs2_permission(odentry->d_inode, MAY_WRITE);
		if (error)
			goto out_gunlock;
	}

	if (nip == NULL)
		alloc_required = gfs2_diradd_alloc_required(ndir, &ndentry->d_name);
	error = alloc_required;
	if (error < 0)
		goto out_gunlock;

	if (alloc_required) {
		struct gfs2_alloc_parms ap = { .target = sdp->sd_max_dirres, };
		error = gfs2_quota_lock_check(ndip, &ap);
		if (error)
			goto out_gunlock;

		error = gfs2_inplace_reserve(ndip, &ap);
		if (error)
			goto out_gunlock_q;

		error = gfs2_trans_begin(sdp, sdp->sd_max_dirres +
					 gfs2_rg_blocks(ndip, sdp->sd_max_dirres) +
					 4 * RES_DINODE + 4 * RES_LEAF +
					 RES_STATFS + RES_QUOTA + 4, 0);
		if (error)
			goto out_ipreserv;
	} else {
		error = gfs2_trans_begin(sdp, 4 * RES_DINODE +
					 5 * RES_LEAF + 4, 0);
		if (error)
			goto out_gunlock;
	}

	/* Remove the target file, if it exists */

	if (nip)
		error = gfs2_unlink_inode(ndip, ndentry);

	error = update_moved_ino(ip, ndip, dir_rename);
	if (error)
		goto out_end_trans;

	error = gfs2_dir_del(odip, odentry);
	if (error)
		goto out_end_trans;

	error = gfs2_dir_add(ndir, &ndentry->d_name, ip);
	if (error)
		goto out_end_trans;

out_end_trans:
	gfs2_trans_end(sdp);
out_ipreserv:
	if (alloc_required)
		gfs2_inplace_release(ndip);
out_gunlock_q:
	if (alloc_required)
		gfs2_quota_unlock(ndip);
out_gunlock:
	if (gfs2_holder_initialized(&rd_gh))
		gfs2_glock_dq_uninit(&rd_gh);

	while (x--) {
		if (gfs2_holder_queued(ghs + x))
			gfs2_glock_dq(ghs + x);
		gfs2_holder_uninit(ghs + x);
	}
out_gunlock_r:
	if (gfs2_holder_initialized(&r_gh))
		gfs2_glock_dq_uninit(&r_gh);
out:
	gfs2_qa_put(ndip);
	return error;
}

/**
 * gfs2_exchange - exchange two files
 * @odir: Parent directory of old file name
 * @odentry: The old dentry of the file
 * @ndir: Parent directory of new file name
 * @ndentry: The new dentry of the file
 * @flags: The rename flags
 *
 * Returns: errno
 */

static int gfs2_exchange(struct inode *odir, struct dentry *odentry,
			 struct inode *ndir, struct dentry *ndentry,
			 unsigned int flags)
{
	struct gfs2_inode *odip = GFS2_I(odir);
	struct gfs2_inode *ndip = GFS2_I(ndir);
	struct gfs2_inode *oip = GFS2_I(odentry->d_inode);
	struct gfs2_inode *nip = GFS2_I(ndentry->d_inode);
	struct gfs2_sbd *sdp = GFS2_SB(odir);
	struct gfs2_holder ghs[4], r_gh;
	unsigned int num_gh;
	unsigned int x;
	umode_t old_mode = oip->i_inode.i_mode;
	umode_t new_mode = nip->i_inode.i_mode;
	int error;

	gfs2_holder_mark_uninitialized(&r_gh);
	error = gfs2_rindex_update(sdp);
	if (error)
		return error;

	if (odip != ndip) {
		error = gfs2_glock_nq_init(sdp->sd_rename_gl, LM_ST_EXCLUSIVE,
					   0, &r_gh);
		if (error)
			goto out;

		if (S_ISDIR(old_mode)) {
			/* don't move a directory into its subdir */
			error = gfs2_ok_to_move(oip, ndip);
			if (error)
				goto out_gunlock_r;
		}

		if (S_ISDIR(new_mode)) {
			/* don't move a directory into its subdir */
			error = gfs2_ok_to_move(nip, odip);
			if (error)
				goto out_gunlock_r;
		}
	}

	num_gh = 1;
	gfs2_holder_init(odip->i_gl, LM_ST_EXCLUSIVE, GL_ASYNC, ghs);
	if (odip != ndip) {
		gfs2_holder_init(ndip->i_gl, LM_ST_EXCLUSIVE, GL_ASYNC,
				 ghs + num_gh);
		num_gh++;
	}
	gfs2_holder_init(oip->i_gl, LM_ST_EXCLUSIVE, GL_ASYNC, ghs + num_gh);
	num_gh++;

	gfs2_holder_init(nip->i_gl, LM_ST_EXCLUSIVE, GL_ASYNC, ghs + num_gh);
	num_gh++;

	for (x = 0; x < num_gh; x++) {
		error = gfs2_glock_nq(ghs + x);
		if (error)
			goto out_gunlock;
	}

	error = gfs2_glock_async_wait(num_gh, ghs);
	if (error)
		goto out_gunlock;

	error = -ENOENT;
	if (oip->i_inode.i_nlink == 0 || nip->i_inode.i_nlink == 0)
		goto out_gunlock;

	error = gfs2_unlink_ok(odip, &odentry->d_name, oip);
	if (error)
		goto out_gunlock;
	error = gfs2_unlink_ok(ndip, &ndentry->d_name, nip);
	if (error)
		goto out_gunlock;

	if (S_ISDIR(old_mode)) {
		error = gfs2_permission(odentry->d_inode, MAY_WRITE);
		if (error)
			goto out_gunlock;
	}
	if (S_ISDIR(new_mode)) {
		error = gfs2_permission(ndentry->d_inode, MAY_WRITE);
		if (error)
			goto out_gunlock;
	}
	error = gfs2_trans_begin(sdp, 4 * RES_DINODE + 4 * RES_LEAF, 0);
	if (error)
		goto out_gunlock;

	error = update_moved_ino(oip, ndip, S_ISDIR(old_mode));
	if (error)
		goto out_end_trans;

	error = update_moved_ino(nip, odip, S_ISDIR(new_mode));
	if (error)
		goto out_end_trans;

	error = gfs2_dir_mvino(ndip, &ndentry->d_name, oip,
			       IF2DT(old_mode));
	if (error)
		goto out_end_trans;

	error = gfs2_dir_mvino(odip, &odentry->d_name, nip,
			       IF2DT(new_mode));
	if (error)
		goto out_end_trans;

	if (odip != ndip) {
		if (S_ISDIR(new_mode) && !S_ISDIR(old_mode)) {
			inc_nlink(&odip->i_inode);
			drop_nlink(&ndip->i_inode);
		} else if (S_ISDIR(old_mode) && !S_ISDIR(new_mode)) {
			inc_nlink(&ndip->i_inode);
			drop_nlink(&odip->i_inode);
		}
	}
	mark_inode_dirty(&ndip->i_inode);
	if (odip != ndip)
		mark_inode_dirty(&odip->i_inode);

out_end_trans:
	gfs2_trans_end(sdp);
out_gunlock:
	while (x--) {
		if (gfs2_holder_queued(ghs + x))
			gfs2_glock_dq(ghs + x);
		gfs2_holder_uninit(ghs + x);
	}
out_gunlock_r:
	if (gfs2_holder_initialized(&r_gh))
		gfs2_glock_dq_uninit(&r_gh);
out:
	return error;
}

static int gfs2_rename2(struct inode *odir, struct dentry *odentry,
			struct inode *ndir, struct dentry *ndentry,
			unsigned int flags)
{
	flags &= ~RENAME_NOREPLACE;

	if (flags & ~RENAME_EXCHANGE)
		return -EINVAL;

	if (flags & RENAME_EXCHANGE)
		return gfs2_exchange(odir, odentry, ndir, ndentry, flags);

	return gfs2_rename(odir, odentry, ndir, ndentry);
}

/**
 * gfs2_follow_link - Follow a symbolic link
 * @dentry: The dentry of the link
 * @nd: Data that we pass to vfs_follow_link()
 *
 * This can handle symlinks of any size.
 *
 * Returns: 0 on success or error code
 */

static void *gfs2_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct gfs2_inode *ip = GFS2_I(dentry->d_inode);
	struct gfs2_holder i_gh;
	struct buffer_head *dibh;
	unsigned int size;
	char *buf;
	int error;

	gfs2_holder_init(ip->i_gl, LM_ST_SHARED, 0, &i_gh);
	error = gfs2_glock_nq(&i_gh);
	if (error) {
		gfs2_holder_uninit(&i_gh);
		nd_set_link(nd, ERR_PTR(error));
		return NULL;
	}

	size = (unsigned int)i_size_read(&ip->i_inode);
	if (size == 0) {
		gfs2_consist_inode(ip);
		buf = ERR_PTR(-EIO);
		goto out;
	}

	error = gfs2_meta_inode_buffer(ip, &dibh);
	if (error) {
		buf = ERR_PTR(error);
		goto out;
	}

	buf = kzalloc(size + 1, GFP_NOFS);
	if (!buf)
		buf = ERR_PTR(-ENOMEM);
	else
		memcpy(buf, dibh->b_data + sizeof(struct gfs2_dinode), size);
	brelse(dibh);
out:
	gfs2_glock_dq_uninit(&i_gh);
	nd_set_link(nd, buf);
	return NULL;
}

/**
 * gfs2_permission -
 * @inode: The inode
 * @mask: The mask to be tested
 * @flags: Indicates whether this is an RCU path walk or not
 *
 * This may be called from the VFS directly, or from within GFS2 with the
 * inode locked, so we look to see if the glock is already locked and only
 * lock the glock if its not already been done.
 *
 * Returns: errno
 */

int gfs2_permission(struct inode *inode, int mask)
{
	struct gfs2_inode *ip;
	struct gfs2_holder i_gh;
	int error;

	gfs2_holder_mark_uninitialized(&i_gh);
	ip = GFS2_I(inode);
	if (gfs2_glock_is_locked_by_me(ip->i_gl) == NULL) {
		if (mask & MAY_NOT_BLOCK)
			return -ECHILD;
		error = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, LM_FLAG_ANY, &i_gh);
		if (error)
			return error;
	}

	if ((mask & MAY_WRITE) && IS_IMMUTABLE(inode))
		error = -EACCES;
	else
		error = generic_permission(inode, mask);
	if (gfs2_holder_initialized(&i_gh))
		gfs2_glock_dq_uninit(&i_gh);

	return error;
}

static int __gfs2_setattr_simple(struct inode *inode, struct iattr *attr)
{
	setattr_copy(inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

/**
 * gfs2_setattr_simple -
 * @ip:
 * @attr:
 *
 * Returns: errno
 */

int gfs2_setattr_simple(struct inode *inode, struct iattr *attr)
{
	int error;

	if (current->journal_info)
		return __gfs2_setattr_simple(inode, attr);

	error = gfs2_trans_begin(GFS2_SB(inode), RES_DINODE, 0);
	if (error)
		return error;

	error = __gfs2_setattr_simple(inode, attr);
	gfs2_trans_end(GFS2_SB(inode));
	return error;
}

static int setattr_chown(struct inode *inode, struct iattr *attr)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	kuid_t ouid, nuid;
	kgid_t ogid, ngid;
	int error;
	struct gfs2_alloc_parms ap;

	ouid = inode->i_uid;
	ogid = inode->i_gid;
	nuid = attr->ia_uid;
	ngid = attr->ia_gid;

	if (!(attr->ia_valid & ATTR_UID) || uid_eq(ouid, nuid))
		ouid = nuid = NO_UID_QUOTA_CHANGE;
	if (!(attr->ia_valid & ATTR_GID) || gid_eq(ogid, ngid))
		ogid = ngid = NO_GID_QUOTA_CHANGE;
	error = gfs2_qa_get(ip);
	if (error)
		return error;

	error = gfs2_rindex_update(sdp);
	if (error)
		goto out;

	error = gfs2_quota_lock(ip, nuid, ngid);
	if (error)
		goto out;

	ap.target = gfs2_get_inode_blocks(&ip->i_inode);

	if (!uid_eq(ouid, NO_UID_QUOTA_CHANGE) ||
	    !gid_eq(ogid, NO_GID_QUOTA_CHANGE)) {
		error = gfs2_quota_check(ip, nuid, ngid, &ap);
		if (error)
			goto out_gunlock_q;
	}

	error = gfs2_trans_begin(sdp, RES_DINODE + 2 * RES_QUOTA, 0);
	if (error)
		goto out_gunlock_q;

	error = gfs2_setattr_simple(inode, attr);
	if (error)
		goto out_end_trans;

	if (!uid_eq(ouid, NO_UID_QUOTA_CHANGE) ||
	    !gid_eq(ogid, NO_GID_QUOTA_CHANGE)) {
		gfs2_quota_change(ip, -(s64)ap.target, ouid, ogid);
		gfs2_quota_change(ip, ap.target, nuid, ngid);
	}

out_end_trans:
	gfs2_trans_end(sdp);
out_gunlock_q:
	gfs2_quota_unlock(ip);
out:
	gfs2_qa_put(ip);
	return error;
}

/**
 * gfs2_setattr - Change attributes on an inode
 * @dentry: The dentry which is changing
 * @attr: The structure describing the change
 *
 * The VFS layer wants to change one or more of an inodes attributes.  Write
 * that change out to disk.
 *
 * Returns: errno
 */

static int gfs2_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder i_gh;
	int error;

	error = gfs2_qa_get(ip);
	if (error)
		return error;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_EXCLUSIVE, 0, &i_gh);
	if (error)
		goto out;

	error = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto error;

	error = inode_change_ok(inode, attr);
	if (error)
		goto error;

	if (attr->ia_valid & ATTR_SIZE)
		error = gfs2_setattr_size(inode, attr->ia_size);
	else if (attr->ia_valid & (ATTR_UID | ATTR_GID))
		error = setattr_chown(inode, attr);
	else if ((attr->ia_valid & ATTR_MODE) && IS_POSIXACL(inode))
		error = gfs2_acl_chmod(ip, attr);
	else
		error = gfs2_setattr_simple(inode, attr);

error:
	if (!error)
		mark_inode_dirty(inode);
	gfs2_glock_dq_uninit(&i_gh);
out:
	gfs2_qa_put(ip);
	return error;
}

/**
 * gfs2_getattr - Read out an inode's attributes
 * @mnt: The vfsmount the inode is being accessed from
 * @dentry: The dentry to stat
 * @stat: The inode's stats
 *
 * This may be called from the VFS directly, or from within GFS2 with the
 * inode locked, so we look to see if the glock is already locked and only
 * lock the glock if its not already been done. Note that its the NFS
 * readdirplus operation which causes this to be called (from filldir)
 * with the glock already held.
 *
 * Returns: errno
 */

static int gfs2_getattr(struct vfsmount *mnt, struct dentry *dentry,
			struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder gh;
	int error;

	gfs2_holder_mark_uninitialized(&gh);
	if (gfs2_glock_is_locked_by_me(ip->i_gl) == NULL) {
		error = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, LM_FLAG_ANY, &gh);
		if (error)
			return error;
	}

	generic_fillattr(inode, stat);
	if (gfs2_holder_initialized(&gh))
		gfs2_glock_dq_uninit(&gh);

	return 0;
}

static int gfs2_setxattr(struct dentry *dentry, const char *name,
			 const void *data, size_t size, int flags)
{
	struct inode *inode = dentry->d_inode;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder gh;
	int ret;

	gfs2_holder_init(ip->i_gl, LM_ST_EXCLUSIVE, 0, &gh);
	ret = gfs2_glock_nq(&gh);
	if (ret == 0) {
		ret = gfs2_qa_get(ip);
		if (ret == 0)
			ret = generic_setxattr(dentry, name, data, size, flags);
		gfs2_qa_put(ip);
		gfs2_glock_dq(&gh);
	}
	gfs2_holder_uninit(&gh);
	return ret;
}

static ssize_t gfs2_getxattr(struct dentry *dentry, const char *name,
			     void *data, size_t size)
{
	struct inode *inode = dentry->d_inode;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder gh;
	int ret;

	/* For selinux during lookup */
	if (gfs2_glock_is_locked_by_me(ip->i_gl))
		return generic_getxattr(dentry, name, data, size);

	gfs2_holder_init(ip->i_gl, LM_ST_SHARED, LM_FLAG_ANY, &gh);
	ret = gfs2_glock_nq(&gh);
	if (ret == 0) {
		ret = generic_getxattr(dentry, name, data, size);
		gfs2_glock_dq(&gh);
	}
	gfs2_holder_uninit(&gh);
	return ret;
}

static int gfs2_removexattr(struct dentry *dentry, const char *name)
{
	struct inode *inode = dentry->d_inode;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder gh;
	int ret;

	gfs2_holder_init(ip->i_gl, LM_ST_EXCLUSIVE, 0, &gh);
	ret = gfs2_glock_nq(&gh);
	if (ret == 0) {
		ret = gfs2_qa_get(ip);
		if (ret == 0)
			ret = generic_removexattr(dentry, name);
		gfs2_qa_put(ip);
		gfs2_glock_dq(&gh);
	}
	gfs2_holder_uninit(&gh);
	return ret;
}

static int gfs2_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		       u64 start, u64 len)
{
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder gh;
	int ret;

	mutex_lock(&inode->i_mutex);

	ret = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, 0, &gh);
	if (ret)
		goto out;

	ret = iomap_fiemap(inode, fieinfo, start, len, &gfs2_iomap_ops);

	gfs2_glock_dq_uninit(&gh);

out:
	mutex_unlock(&inode->i_mutex);
	return ret;
}

loff_t gfs2_seek_data(struct file *file, loff_t offset)
{
	struct inode *inode = file->f_mapping->host;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder gh;
	loff_t ret;

	mutex_lock(&inode->i_mutex);
	ret = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, 0, &gh);
	if (!ret)
		ret = iomap_seek_data(inode, offset, &gfs2_iomap_ops);
	gfs2_glock_dq_uninit(&gh);
	mutex_unlock(&inode->i_mutex);

	if (ret < 0)
		return ret;
	return vfs_setpos(file, ret, inode->i_sb->s_maxbytes);
}

loff_t gfs2_seek_hole(struct file *file, loff_t offset)
{
	struct inode *inode = file->f_mapping->host;
	struct gfs2_inode *ip = GFS2_I(inode);
	struct gfs2_holder gh;
	loff_t ret;

	mutex_lock(&inode->i_mutex);
	ret = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, 0, &gh);
	if (!ret)
		ret = iomap_seek_hole(inode, offset, &gfs2_iomap_ops);
	gfs2_glock_dq_uninit(&gh);
	mutex_unlock(&inode->i_mutex);

	if (ret < 0)
		return ret;
	return vfs_setpos(file, ret, inode->i_sb->s_maxbytes);
}

const struct inode_operations gfs2_file_iops = {
	.permission = gfs2_permission,
	.setattr = gfs2_setattr,
	.getattr = gfs2_getattr,
	.setxattr = gfs2_setxattr,
	.getxattr = gfs2_getxattr,
	.listxattr = gfs2_listxattr,
	.removexattr = gfs2_removexattr,
	.fiemap = gfs2_fiemap,
	.get_acl = gfs2_get_acl,
};

const struct inode_operations_wrapper gfs2_dir_iops = {
	.ops = {
	.create = gfs2_create,
	.lookup = gfs2_lookup,
	.link = gfs2_link,
	.unlink = gfs2_unlink,
	.symlink = gfs2_symlink,
	.mkdir = gfs2_mkdir,
	.rmdir = gfs2_unlink,
	.mknod = gfs2_mknod,
	.rename = gfs2_rename,
	.permission = gfs2_permission,
	.setattr = gfs2_setattr,
	.getattr = gfs2_getattr,
	.setxattr = gfs2_setxattr,
	.getxattr = gfs2_getxattr,
	.listxattr = gfs2_listxattr,
	.removexattr = gfs2_removexattr,
	.fiemap = gfs2_fiemap,
	.get_acl = gfs2_get_acl,
	.atomic_open = gfs2_atomic_open,
	},
	.rename2 = gfs2_rename2,
};

const struct inode_operations gfs2_symlink_iops = {
	.readlink = generic_readlink,
	.follow_link = gfs2_follow_link,
	.put_link = kfree_put_link,
	.permission = gfs2_permission,
	.setattr = gfs2_setattr,
	.getattr = gfs2_getattr,
	.setxattr = gfs2_setxattr,
	.getxattr = gfs2_getxattr,
	.listxattr = gfs2_listxattr,
	.removexattr = gfs2_removexattr,
	.fiemap = gfs2_fiemap,
	.get_acl = gfs2_get_acl,
};

