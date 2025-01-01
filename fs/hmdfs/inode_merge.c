// SPDX-License-Identifier: GPL-2.0
/*
 * fs/hmdfs/inode_merge.c
 *
 * Copyright (c) 2020-2021 Huawei Device Co., Ltd.
 */

#include "hmdfs_merge_view.h"
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/fs_stack.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/types.h>
#include "authority/authentication.h"
#include "hmdfs_trace.h"

struct kmem_cache *hmdfs_dentry_merge_cachep;

struct dentry *hmdfs_get_fst_lo_d(struct dentry *dentry)
{
	struct hmdfs_dentry_info_merge *dim = hmdfs_dm(dentry);
	struct hmdfs_dentry_comrade *comrade = NULL;
	struct dentry *d = NULL;

	mutex_lock(&dim->comrade_list_lock);
	comrade = list_first_entry_or_null(&dim->comrade_list,
					   struct hmdfs_dentry_comrade, list);
	if (comrade)
		d = dget(comrade->lo_d);
	mutex_unlock(&dim->comrade_list_lock);
	return d;
}

struct dentry *hmdfs_get_lo_d(struct dentry *dentry, int dev_id)
{
	struct hmdfs_dentry_info_merge *dim = hmdfs_dm(dentry);
	struct hmdfs_dentry_comrade *comrade = NULL;
	struct dentry *d = NULL;

	mutex_lock(&dim->comrade_list_lock);
	list_for_each_entry(comrade, &dim->comrade_list, list) {
		if (comrade->dev_id == dev_id) {
			d = dget(comrade->lo_d);
			break;
		}
	}
	mutex_unlock(&dim->comrade_list_lock);
	return d;
}

void update_inode_attr(struct inode *inode, struct dentry *child_dentry)
{
	struct inode *li = NULL;
	struct hmdfs_dentry_info_merge *cdi = hmdfs_dm(child_dentry);
	struct hmdfs_dentry_comrade *comrade = NULL;
	struct hmdfs_dentry_comrade *fst_comrade = NULL;

	mutex_lock(&cdi->comrade_list_lock);
	fst_comrade = list_first_entry(&cdi->comrade_list,
				       struct hmdfs_dentry_comrade, list);
	list_for_each_entry(comrade, &cdi->comrade_list, list) {
		li = d_inode(comrade->lo_d);
		if (!li)
			continue;

		if (comrade == fst_comrade) {
			inode->i_atime = li->i_atime;
			inode->__i_ctime = li->__i_ctime;
			inode->i_mtime = li->i_mtime;
			inode->i_size = li->i_size;
			continue;
		}

		if (hmdfs_time_compare(&inode->i_mtime, &li->i_mtime) < 0)
			inode->i_mtime = li->i_mtime;
	}
	mutex_unlock(&cdi->comrade_list_lock);
}

int get_num_comrades(struct dentry *dentry)
{
	struct list_head *pos;
	struct hmdfs_dentry_info_merge *dim = hmdfs_dm(dentry);
	int count = 0;

	mutex_lock(&dim->comrade_list_lock);
	list_for_each(pos, &dim->comrade_list)
		count++;
	mutex_unlock(&dim->comrade_list_lock);
	return count;
}

static struct inode *fill_inode_merge(struct super_block *sb,
				      struct inode *parent_inode,
				      struct dentry *child_dentry,
				      struct dentry *lo_d_dentry)
{
	int ret = 0;
	struct dentry *fst_lo_d = NULL;
	struct hmdfs_inode_info *info = NULL;
	struct inode *inode = NULL;
	umode_t mode;

	if (lo_d_dentry) {
		fst_lo_d = lo_d_dentry;
		dget(fst_lo_d);
	} else {
		fst_lo_d = hmdfs_get_fst_lo_d(child_dentry);
	}
	if (!fst_lo_d) {
		inode = ERR_PTR(-EINVAL);
		goto out;
	}
	if (hmdfs_i(parent_inode)->inode_type == HMDFS_LAYER_ZERO)
		inode = hmdfs_iget_locked_root(sb, HMDFS_ROOT_MERGE, NULL,
					       NULL);
	else
		inode = hmdfs_iget5_locked_merge(sb, fst_lo_d);
	if (!inode) {
		hmdfs_err("iget5_locked get inode NULL");
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}
	if (!(inode->i_state & I_NEW))
		goto out;
	info = hmdfs_i(inode);
	if (hmdfs_i(parent_inode)->inode_type == HMDFS_LAYER_ZERO)
		info->inode_type = HMDFS_LAYER_FIRST_MERGE;
	else
		info->inode_type = HMDFS_LAYER_OTHER_MERGE;

	inode->i_uid = KUIDT_INIT((uid_t)1000);
	inode->i_gid = KGIDT_INIT((gid_t)1000);

	update_inode_attr(inode, child_dentry);
	mode = d_inode(fst_lo_d)->i_mode;

	if (S_ISREG(mode)) {
		inode->i_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
		inode->i_op = &hmdfs_file_iops_merge;
		inode->i_fop = &hmdfs_file_fops_merge;
		set_nlink(inode, 1);
	} else if (S_ISDIR(mode)) {
		inode->i_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IXOTH;
		inode->i_op = &hmdfs_dir_iops_merge;
		inode->i_fop = &hmdfs_dir_fops_merge;
		set_nlink(inode, get_num_comrades(child_dentry) + 2);
	} else {
		ret = -EIO;
		goto bad_inode;
	}

	unlock_new_inode(inode);
out:
	dput(fst_lo_d);
	return inode;
bad_inode:
	iget_failed(inode);
	return ERR_PTR(ret);
}

struct hmdfs_dentry_comrade *alloc_comrade(struct dentry *lo_d, int dev_id)
{
	struct hmdfs_dentry_comrade *comrade = NULL;

	// 文件只有一个 comrade，考虑 {comrade, list + list lock}
	comrade = kzalloc(sizeof(*comrade), GFP_KERNEL);
	if (unlikely(!comrade))
		return ERR_PTR(-ENOMEM);

	comrade->lo_d = lo_d;
	comrade->dev_id = dev_id;
	dget(lo_d);
	return comrade;
}

void link_comrade(struct list_head *onstack_comrades_head,
		  struct hmdfs_dentry_comrade *comrade)
{
	struct hmdfs_dentry_comrade *c = NULL;

	list_for_each_entry(c, onstack_comrades_head, list) {
		if (likely(c->dev_id != comrade->dev_id))
			continue;
		hmdfs_err("Redundant comrade of device %llu", c->dev_id);
		dput(comrade->lo_d);
		kfree(comrade);
		WARN_ON(1);
		return;
	}

	if (comrade_is_local(comrade))
		list_add(&comrade->list, onstack_comrades_head);
	else
		list_add_tail(&comrade->list, onstack_comrades_head);
}

/**
 * assign_comrades_unlocked - assign a child dentry with comrades
 *
 * We tend to setup a local list of all the comrades we found and place the
 * list onto the dentry_info to achieve atomicity.
 */
void assign_comrades_unlocked(struct dentry *child_dentry,
			      struct list_head *onstack_comrades_head)
{
	struct hmdfs_dentry_info_merge *cdi = hmdfs_dm(child_dentry);

	mutex_lock(&cdi->comrade_list_lock);
	WARN_ON(!list_empty(&cdi->comrade_list));
	list_splice_init(onstack_comrades_head, &cdi->comrade_list);
	mutex_unlock(&cdi->comrade_list_lock);
}

struct hmdfs_dentry_comrade *lookup_comrade(struct path lower_path,
					    const char *d_name,
					    int dev_id,
					    unsigned int flags)
{
	struct path path;
	struct hmdfs_dentry_comrade *comrade = NULL;
	int err;

	err = vfs_path_lookup(lower_path.dentry, lower_path.mnt, d_name, flags,
			      &path);
	if (err)
		return ERR_PTR(err);

	comrade = alloc_comrade(path.dentry, dev_id);
	path_put(&path);
	return comrade;
}

/**
 * conf_name_trans_nop - do nothing but copy
 *
 * WARNING: always check before translation
 */
static char *conf_name_trans_nop(struct dentry *d)
{
	return kstrndup(d->d_name.name, d->d_name.len, GFP_KERNEL);
}

/**
 * conf_name_trans_dir - conflicted name translation for directory
 *
 * WARNING: always check before translation
 */
static char *conf_name_trans_dir(struct dentry *d)
{
	int len = d->d_name.len - strlen(CONFLICTING_DIR_SUFFIX);

	return kstrndup(d->d_name.name, len, GFP_KERNEL);
}

/**
 * conf_name_trans_reg - conflicted name translation for regular file
 *
 * WARNING: always check before translation
 */
static char *conf_name_trans_reg(struct dentry *d, int *dev_id)
{
	int dot_pos, start_cpy_pos, num_len, i;
	int len = d->d_name.len;
	char *name = kstrndup(d->d_name.name, d->d_name.len, GFP_KERNEL);

	if (unlikely(!name))
		return NULL;

	// find the last dot if possible
	for (dot_pos = len - 1; dot_pos >= 0; dot_pos--) {
		if (name[dot_pos] == '.')
			break;
	}
	if (dot_pos == -1)
		dot_pos = len;

	// retrieve the conf sn (i.e. dev_id)
	num_len = 0;
	for (i = dot_pos - 1; i >= 0; i--) {
		if (name[i] >= '0' && name[i] <= '9')
			num_len++;
		else
			break;
	}

	*dev_id = 0;
	for (i = 0; i < num_len; i++)
		*dev_id = *dev_id * 10 + name[dot_pos - num_len + i] - '0';

	// move the file suffix( '\0' included) right after the file name
	start_cpy_pos =
		dot_pos - num_len - strlen(CONFLICTING_FILE_CONST_SUFFIX);
	memmove(name + start_cpy_pos, name + dot_pos, len - dot_pos + 1);
	return name;
}

int check_filename(const char *name, int len)
{
	int cmp_res = 0;

	if (len >= strlen(CONFLICTING_DIR_SUFFIX)) {
		cmp_res = strncmp(name + len - strlen(CONFLICTING_DIR_SUFFIX),
				  CONFLICTING_DIR_SUFFIX,
				  strlen(CONFLICTING_DIR_SUFFIX));
		if (cmp_res == 0)
			return DT_DIR;
	}

	if (len >= strlen(CONFLICTING_FILE_CONST_SUFFIX)) {
		int dot_pos, start_cmp_pos, num_len, i;

		for (dot_pos = len - 1; dot_pos >= 0; dot_pos--) {
			if (name[dot_pos] == '.')
				break;
		}
		if (dot_pos == -1)
			dot_pos = len;

		num_len = 0;
		for (i = dot_pos - 1; i >= 0; i--) {
			if (name[i] >= '0' && name[i] <= '9')
				num_len++;
			else
				break;
		}

		start_cmp_pos = dot_pos - num_len -
				strlen(CONFLICTING_FILE_CONST_SUFFIX);
		cmp_res = strncmp(name + start_cmp_pos,
				  CONFLICTING_FILE_CONST_SUFFIX,
				  strlen(CONFLICTING_FILE_CONST_SUFFIX));
		if (cmp_res == 0)
			return DT_REG;
	}

	return 0;
}

static struct hmdfs_dentry_comrade *merge_lookup_comrade(
	struct hmdfs_sb_info *sbi, const char *name, int devid,
	unsigned int flags)
{
	int err;
	struct path root, path;
	struct hmdfs_dentry_comrade *comrade = NULL;
	const struct cred *old_cred = hmdfs_override_creds(sbi->cred);

	err = kern_path(sbi->real_dst, LOOKUP_DIRECTORY, &root);
	if (err) {
		comrade = ERR_PTR(err);
		goto out;
	}

	err = vfs_path_lookup(root.dentry, root.mnt, name, flags, &path);
	if (err) {
		comrade = ERR_PTR(err);
		goto root_put;
	}
	
	comrade = alloc_comrade(path.dentry, devid);

	path_put(&path);
root_put:
	path_put(&root);
out:
	hmdfs_revert_creds(old_cred);
	return comrade;
}

bool is_valid_comrade(struct hmdfs_dentry_info_merge *mdi, umode_t mode)
{
	if (mdi->type == DT_UNKNOWN) {
		mdi->type = S_ISDIR(mode) ? DT_DIR : DT_REG;
		return true;
	}

	if (mdi->type == DT_DIR && S_ISDIR(mode)) {
		return true;
	}

	if (mdi->type == DT_REG && list_empty(&mdi->comrade_list) &&
		!S_ISDIR(mode)) {
		return true;
	}

	return false;
}

static void merge_lookup_work_func(struct work_struct *work)
{
	struct merge_lookup_work *ml_work;
	struct hmdfs_dentry_comrade *comrade;
	struct hmdfs_dentry_info_merge *mdi;
	int found = false;
	
	ml_work = container_of(work, struct merge_lookup_work, work);
	mdi = container_of(ml_work->wait_queue,	struct hmdfs_dentry_info_merge,
		wait_queue);
	
	trace_hmdfs_merge_lookup_work_enter(ml_work);

	comrade = merge_lookup_comrade(ml_work->sbi, ml_work->name,
		ml_work->devid, ml_work->flags);
	if (IS_ERR(comrade)) {
		mutex_lock(&mdi->work_lock);
		goto out;
	}

	mutex_lock(&mdi->work_lock);
	mutex_lock(&mdi->comrade_list_lock);
	if (!is_valid_comrade(mdi, hmdfs_cm(comrade))) {
		destroy_comrade(comrade);
	} else {
		found = true;
		link_comrade(&mdi->comrade_list, comrade);
	}
	mutex_unlock(&mdi->comrade_list_lock);

out:
	if (--mdi->work_count == 0 || found)
		wake_up_all(ml_work->wait_queue);
	mutex_unlock(&mdi->work_lock);

	trace_hmdfs_merge_lookup_work_exit(ml_work, found);
	kfree(ml_work->name);
	kfree(ml_work);
}

int merge_lookup_async(struct hmdfs_dentry_info_merge *mdi,
	struct hmdfs_sb_info *sbi, int devid, const char *name,
	unsigned int flags)
{
	int err = -ENOMEM;
	struct merge_lookup_work *ml_work;

	ml_work = kmalloc(sizeof(*ml_work), GFP_KERNEL);
	if (!ml_work)
		goto out;
		
	ml_work->name = kstrdup(name, GFP_KERNEL);
	if (!ml_work->name) {
		kfree(ml_work);
		goto out;
	}

	ml_work->devid = devid;
	ml_work->flags = flags;
	ml_work->sbi = sbi;
	ml_work->wait_queue = &mdi->wait_queue;
	INIT_WORK(&ml_work->work, merge_lookup_work_func);

	schedule_work(&ml_work->work);
	++mdi->work_count;
	err = 0;
out:
	return err;
}

char *hmdfs_get_real_dname(struct dentry *dentry, int *devid, int *type)
{
	char *rname;

	*type = check_filename(dentry->d_name.name, dentry->d_name.len);
	if (*type == DT_REG)
		rname = conf_name_trans_reg(dentry, devid);
	else if (*type == DT_DIR)
		rname = conf_name_trans_dir(dentry);
	else
		rname = conf_name_trans_nop(dentry);

	return rname;
}

static int lookup_merge_normal(struct dentry *dentry, unsigned int flags)
{
	int ret = -ENOMEM;
	int err = 0;
	int devid = -1;
	struct dentry *pdentry = dget_parent(dentry);
	struct hmdfs_dentry_info_merge *mdi = hmdfs_dm(dentry);
	struct hmdfs_sb_info *sbi = hmdfs_sb(dentry->d_sb);
	struct hmdfs_peer *peer;
	char *rname, *ppath, *cpath;

	rname = hmdfs_get_real_dname(dentry, &devid, &mdi->type);
	if (unlikely(!rname)) {
		goto out;
	}

	ppath = hmdfs_merge_get_dentry_relative_path(pdentry);
	if (unlikely(!ppath)) {
		hmdfs_err("failed to get parent relative path");
		goto out_rname;
	}

	cpath = kzalloc(PATH_MAX, GFP_KERNEL);
	if (unlikely(!cpath)) {
		hmdfs_err("failed to get child device_view path");
		goto out_ppath;
	}

	mutex_lock(&mdi->work_lock);
	mutex_lock(&sbi->connections.node_lock);
	if (mdi->type != DT_REG || devid == 0) {
		snprintf(cpath, PATH_MAX, "device_view/local%s/%s", ppath,
			rname);
		err = merge_lookup_async(mdi, sbi, 0, cpath, flags);
		if (err)
			hmdfs_err("failed to create local lookup work");
	}

	list_for_each_entry(peer, &sbi->connections.node_list, list) {
		if (mdi->type == DT_REG && peer->device_id != devid)
			continue;
		snprintf(cpath, PATH_MAX, "device_view/%s%s/%s", peer->cid,
			ppath, rname);
		err = merge_lookup_async(mdi, sbi, peer->device_id, cpath,
			flags);
		if (err)
			hmdfs_err("failed to create remote lookup work");
	}
	mutex_unlock(&sbi->connections.node_lock);
	mutex_unlock(&mdi->work_lock);

	wait_event(mdi->wait_queue, is_merge_lookup_end(mdi));
	
	ret = -ENOENT;
	if (!is_comrade_list_empty(mdi))
		ret = 0;

	kfree(cpath);
out_ppath:
	kfree(ppath);
out_rname:
	kfree(rname);
out:
	dput(pdentry);
	return ret;
}

/**
 * do_lookup_merge_root - lookup the root of the merge view(root/merge_view)
 *
 * It's common for a network filesystem to incur various of faults, so we
 * intent to show mercy for faults here, except faults reported by the local.
 */
static int do_lookup_merge_root(struct path path_dev,
				struct dentry *child_dentry, unsigned int flags)
{
	struct hmdfs_sb_info *sbi = hmdfs_sb(child_dentry->d_sb);
	struct hmdfs_dentry_comrade *comrade;
	const int buf_len =
		max((int)HMDFS_CID_SIZE + 1, (int)sizeof(DEVICE_VIEW_LOCAL));
	char *buf = kzalloc(buf_len, GFP_KERNEL);
	struct hmdfs_peer *peer;
	LIST_HEAD(head);
	int ret;

	if (!buf)
		return -ENOMEM;

	// lookup real_dst/device_view/local
	memcpy(buf, DEVICE_VIEW_LOCAL, sizeof(DEVICE_VIEW_LOCAL));
	comrade = lookup_comrade(path_dev, buf, HMDFS_DEVID_LOCAL, flags);
	if (IS_ERR(comrade)) {
		ret = PTR_ERR(comrade);
		goto out;
	}
	link_comrade(&head, comrade);

	// lookup real_dst/device_view/cidxx
	mutex_lock(&sbi->connections.node_lock);
	list_for_each_entry(peer, &sbi->connections.node_list, list) {
		mutex_unlock(&sbi->connections.node_lock);
		memcpy(buf, peer->cid, HMDFS_CID_SIZE);
		comrade = lookup_comrade(path_dev, buf, peer->device_id, flags);
		if (IS_ERR(comrade))
			continue;

		link_comrade(&head, comrade);
		mutex_lock(&sbi->connections.node_lock);
	}
	mutex_unlock(&sbi->connections.node_lock);

	assign_comrades_unlocked(child_dentry, &head);
	ret = 0;

out:
	kfree(buf);
	return ret;
}

// mkdir -p
void lock_root_inode_shared(struct inode *root, bool *locked, bool *down)
{
	struct rw_semaphore *sem = &root->i_rwsem;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
#define RWSEM_READER_OWNED     (1UL << 0)
#define RWSEM_RD_NONSPINNABLE  (1UL << 1)
#define RWSEM_WR_NONSPINNABLE  (1UL << 2)
#define RWSEM_NONSPINNABLE     (RWSEM_RD_NONSPINNABLE | RWSEM_WR_NONSPINNABLE)
#define RWSEM_OWNER_FLAGS_MASK (RWSEM_READER_OWNED | RWSEM_NONSPINNABLE)
	struct task_struct *sem_owner =
		(struct task_struct *)(atomic_long_read(&sem->owner) &
				       ~RWSEM_OWNER_FLAGS_MASK);
#else
	struct task_struct *sem_owner = sem->owner;
#endif

	*locked = false;
	*down = false;

	if (sem_owner != current)
		return;

	// It's us that takes the wsem
	if (!inode_trylock_shared(root)) {
		downgrade_write(sem);
		*down = true;
	}
	*locked = true;
}

void restore_root_inode_sem(struct inode *root, bool locked, bool down)
{
	if (!locked)
		return;

	inode_unlock_shared(root);
	if (down)
		inode_lock(root);
}

static int lookup_merge_root(struct inode *root_inode,
			     struct dentry *child_dentry, unsigned int flags)
{
	struct hmdfs_sb_info *sbi = hmdfs_sb(child_dentry->d_sb);
	struct path path_dev;
	int ret = -ENOENT;
	int buf_len;
	char *buf = NULL;
	bool locked, down;

	// consider additional one slash and one '\0'
	buf_len = strlen(sbi->real_dst) + 1 + sizeof(DEVICE_VIEW_ROOT);
	if (buf_len > PATH_MAX)
		return -ENAMETOOLONG;

	buf = kmalloc(buf_len, GFP_KERNEL);
	if (unlikely(!buf))
		return -ENOMEM;

	sprintf(buf, "%s/%s", sbi->real_dst, DEVICE_VIEW_ROOT);
	lock_root_inode_shared(root_inode, &locked, &down);
	ret = hmdfs_get_path_in_sb(child_dentry->d_sb, buf, LOOKUP_DIRECTORY,
				   &path_dev);
	if (ret)
		goto free_buf;

	ret = do_lookup_merge_root(path_dev, child_dentry, flags);
	path_put(&path_dev);

free_buf:
	kfree(buf);
	restore_root_inode_sem(root_inode, locked, down);
	return ret;
}

int init_hmdfs_dentry_info_merge(struct hmdfs_sb_info *sbi,
	struct dentry *dentry)
{
	struct hmdfs_dentry_info_merge *mdi = NULL;

	mdi = kmem_cache_zalloc(hmdfs_dentry_merge_cachep, GFP_NOFS);
	if (!mdi)
		return -ENOMEM;

	mdi->ctime = jiffies;
	mdi->type = DT_UNKNOWN;
	mdi->work_count = 0;
	mutex_init(&mdi->work_lock);
	init_waitqueue_head(&mdi->wait_queue);
	INIT_LIST_HEAD(&mdi->comrade_list);
	mutex_init(&mdi->comrade_list_lock);
	
	d_set_d_op(dentry, &hmdfs_dops_merge);
	dentry->d_fsdata = mdi;
	return 0;
}

// do this in a map-reduce manner
struct dentry *hmdfs_lookup_merge(struct inode *parent_inode,
				  struct dentry *child_dentry,
				  unsigned int flags)
{
	bool create = flags & (LOOKUP_CREATE | LOOKUP_RENAME_TARGET);
	struct hmdfs_sb_info *sbi = hmdfs_sb(child_dentry->d_sb);
	struct hmdfs_inode_info *pii = hmdfs_i(parent_inode);
	struct inode *child_inode = NULL;
	struct dentry *ret_dentry = NULL;
	int err = 0;

	/*
	 * Internal flags like LOOKUP_CREATE should not pass to device view.
	 * LOOKUP_REVAL is needed because dentry cache in hmdfs might be stale
	 * after rename in lower fs. LOOKUP_DIRECTORY is not needed because
	 * merge_view can do the judgement that whether result is directory or
	 * not.
	 */
	flags = flags & LOOKUP_REVAL;

	child_dentry->d_fsdata = NULL;

	if (child_dentry->d_name.len > NAME_MAX) {
		err = -ENAMETOOLONG;
		goto out;
	}

	err = init_hmdfs_dentry_info_merge(sbi, child_dentry);
	if (unlikely(err))
		goto out;

	if (pii->inode_type == HMDFS_LAYER_ZERO) {
		hmdfs_dm(child_dentry)->dentry_type = HMDFS_LAYER_FIRST_MERGE;
		err = lookup_merge_root(parent_inode, child_dentry, flags);
	} else {
		hmdfs_dm(child_dentry)->dentry_type = HMDFS_LAYER_OTHER_MERGE;
		err = lookup_merge_normal(child_dentry, flags);
	}

	if (!err) {
		struct hmdfs_inode_info *info = NULL;

		child_inode = fill_inode_merge(parent_inode->i_sb, parent_inode,
					       child_dentry, NULL);
		if (IS_ERR(child_inode)) {
			err = PTR_ERR(child_inode);
			goto out;
		}
		info = hmdfs_i(child_inode);
		if (info->inode_type == HMDFS_LAYER_FIRST_MERGE)
			hmdfs_root_inode_perm_init(child_inode);
		else
			check_and_fixup_ownership_remote(parent_inode,
							 child_inode,
							 child_dentry);

		ret_dentry = d_splice_alias(child_inode, child_dentry);
		if (IS_ERR(ret_dentry)) {
			clear_comrades(child_dentry);
			err = PTR_ERR(ret_dentry);
			goto out;
		}
		if (ret_dentry)
			child_dentry = ret_dentry;

		goto out;
	}

	if ((err == -ENOENT) && create)
		err = 0;

out:
	return err ? ERR_PTR(err) : ret_dentry;
}

int hmdfs_getattr_merge(struct mnt_idmap *idmap, const struct path *path, struct kstat *stat,
			       u32 request_mask, unsigned int flags)
{
	int ret;
	struct path lower_path = {
		.dentry = hmdfs_get_fst_lo_d(path->dentry),
		.mnt = path->mnt,
	};

	if (unlikely(!lower_path.dentry)) {
		hmdfs_err("Fatal! No comrades");
		ret = -EINVAL;
		goto out;
	}

	ret = vfs_getattr(&lower_path, stat, request_mask, flags);
out:
	dput(lower_path.dentry);
	return ret;
}

int hmdfs_setattr_merge(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *ia)
{
	struct inode *inode = d_inode(dentry);
	struct dentry *lower_dentry = hmdfs_get_fst_lo_d(dentry);
	struct inode *lower_inode = NULL;
	struct iattr lower_ia;
	unsigned int ia_valid = ia->ia_valid;
	int err = 0;
	kuid_t tmp_uid;

	if (!lower_dentry) {
		WARN_ON(1);
		err = -EINVAL;
		goto out;
	}

	lower_inode = d_inode(lower_dentry);
	memcpy(&lower_ia, ia, sizeof(lower_ia));
	if (ia_valid & ATTR_FILE)
		lower_ia.ia_file = hmdfs_f(ia->ia_file)->lower_file;
	lower_ia.ia_valid &= ~(ATTR_UID | ATTR_GID | ATTR_MODE);

	inode_lock(lower_inode);
	tmp_uid = hmdfs_override_inode_uid(lower_inode);

	err = notify_change(&nop_mnt_idmap, lower_dentry, &lower_ia, NULL);
	i_size_write(inode, i_size_read(lower_inode));
	inode->i_atime = lower_inode->i_atime;
	inode->i_mtime = lower_inode->i_mtime;
	inode->__i_ctime = lower_inode->__i_ctime;
	hmdfs_revert_inode_uid(lower_inode, tmp_uid);

	inode_unlock(lower_inode);

out:
	dput(lower_dentry);
	return err;
}

const struct inode_operations hmdfs_file_iops_merge = {
	.getattr = hmdfs_getattr_merge,
	.setattr = hmdfs_setattr_merge,
	.permission = hmdfs_permission,
};

int do_mkdir_merge(struct inode *parent_inode, struct dentry *child_dentry,
		   umode_t mode, struct inode *lo_i_parent,
		   struct dentry *lo_d_child)
{
	int ret = 0;
	struct super_block *sb = parent_inode->i_sb;
	struct inode *child_inode = NULL;

	ret = vfs_mkdir(&nop_mnt_idmap, lo_i_parent, lo_d_child, mode);
	if (ret)
		goto out;

	child_inode =
		fill_inode_merge(sb, parent_inode, child_dentry, lo_d_child);
	if (IS_ERR(child_inode)) {
		ret = PTR_ERR(child_inode);
		goto out;
	}
	check_and_fixup_ownership_remote(parent_inode, child_inode,
					 child_dentry);

	d_add(child_dentry, child_inode);
	/* nlink should be increased with the joining of children */
	set_nlink(parent_inode, 2);
out:
	return ret;
}

int do_create_merge(struct inode *parent_inode, struct dentry *child_dentry,
		    umode_t mode, bool want_excl, struct inode *lo_i_parent,
		    struct dentry *lo_d_child)
{
	int ret = 0;
	struct super_block *sb = parent_inode->i_sb;
	struct inode *child_inode = NULL;

	ret = vfs_create(&nop_mnt_idmap, lo_i_parent, lo_d_child, mode, want_excl);
	if (ret)
		goto out;

	child_inode =
		fill_inode_merge(sb, parent_inode, child_dentry, lo_d_child);
	if (IS_ERR(child_inode)) {
		ret = PTR_ERR(child_inode);
		goto out;
	}
	check_and_fixup_ownership_remote(parent_inode, child_inode,
					 child_dentry);

	d_add(child_dentry, child_inode);
	/* nlink should be increased with the joining of children */
	set_nlink(parent_inode, 2);
out:
	return ret;
}

int hmdfs_do_ops_merge(struct inode *i_parent, struct dentry *d_child,
		       struct dentry *lo_d_child, struct path path,
		       struct hmdfs_recursive_para *rec_op_para)
{
	int ret = 0;

	if (rec_op_para->is_last) {
		switch (rec_op_para->opcode) {
		case F_MKDIR_MERGE:
			ret = do_mkdir_merge(i_parent, d_child,
					     rec_op_para->mode,
					     d_inode(path.dentry), lo_d_child);
			break;
		case F_CREATE_MERGE:
			ret = do_create_merge(i_parent, d_child,
					      rec_op_para->mode,
					      rec_op_para->want_excl,
					      d_inode(path.dentry), lo_d_child);
			break;
		default:
			ret = -EINVAL;
			break;
		}
	} else {
		ret = vfs_mkdir(&nop_mnt_idmap, d_inode(path.dentry), lo_d_child,
				rec_op_para->mode);
	}
	if (ret)
		hmdfs_err("vfs_ops failed, ops %d, err = %d",
			  rec_op_para->opcode, ret);
	return ret;
}

int hmdfs_create_lower_dentry(struct inode *i_parent, struct dentry *d_child,
			      struct dentry *lo_d_parent, bool is_dir,
			      struct hmdfs_recursive_para *rec_op_para)
{
	struct hmdfs_sb_info *sbi = i_parent->i_sb->s_fs_info;
	struct hmdfs_dentry_comrade *new_comrade = NULL;
	struct dentry *lo_d_child = NULL;
	char *path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	char *absolute_path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	char *path_name = NULL;
	struct path path = { .mnt = NULL, .dentry = NULL };
	int ret = 0;

	if (unlikely(!path_buf || !absolute_path_buf)) {
		ret = -ENOMEM;
		goto out;
	}

	path_name = dentry_path_raw(lo_d_parent, path_buf, PATH_MAX);
	if (IS_ERR(path_name)) {
		ret = PTR_ERR(path_name);
		goto out;
	}
	if ((strlen(sbi->real_dst) + strlen(path_name) +
	     strlen(d_child->d_name.name) + 2) > PATH_MAX) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	sprintf(absolute_path_buf, "%s%s/%s", sbi->real_dst, path_name,
		d_child->d_name.name);

	if (is_dir)
		lo_d_child = kern_path_create(AT_FDCWD, absolute_path_buf,
					      &path, LOOKUP_DIRECTORY);
	else
		lo_d_child = kern_path_create(AT_FDCWD, absolute_path_buf,
					      &path, 0);
	if (IS_ERR(lo_d_child)) {
		ret = PTR_ERR(lo_d_child);
		goto out;
	}
	// to ensure link_comrade after vfs_mkdir succeed
	ret = hmdfs_do_ops_merge(i_parent, d_child, lo_d_child, path,
				 rec_op_para);
	if (ret)
		goto out_put;
	new_comrade = alloc_comrade(lo_d_child, HMDFS_DEVID_LOCAL);
	if (IS_ERR(new_comrade)) {
		ret = PTR_ERR(new_comrade);
		goto out_put;
	} else {
		link_comrade_unlocked(d_child, new_comrade);
	}

	update_inode_attr(d_inode(d_child), d_child);

out_put:
	done_path_create(&path, lo_d_child);
out:
	kfree(absolute_path_buf);
	kfree(path_buf);
	return ret;
}

static int create_lo_d_parent_recur(struct dentry *d_parent,
				    struct dentry *d_child, umode_t mode,
				    struct hmdfs_recursive_para *rec_op_para)
{
	struct dentry *lo_d_parent, *d_pparent;
	struct hmdfs_dentry_info_merge *pmdi = NULL;
	int ret = 0;

	pmdi = hmdfs_dm(d_parent);
	wait_event(pmdi->wait_queue, !has_merge_lookup_work(pmdi));
	lo_d_parent = hmdfs_get_lo_d(d_parent, HMDFS_DEVID_LOCAL);
	if (!lo_d_parent) {
		d_pparent = dget_parent(d_parent);
		ret = create_lo_d_parent_recur(d_pparent, d_parent,
					       d_inode(d_parent)->i_mode,
					       rec_op_para);
		dput(d_pparent);
		if (ret)
			goto out;
		lo_d_parent = hmdfs_get_lo_d(d_parent, HMDFS_DEVID_LOCAL);
		if (!lo_d_parent) {
			ret = -ENOENT;
			goto out;
		}
	}
	rec_op_para->is_last = false;
	rec_op_para->mode = mode;
	ret = hmdfs_create_lower_dentry(d_inode(d_parent), d_child, lo_d_parent,
					true, rec_op_para);
out:
	dput(lo_d_parent);
	return ret;
}

int create_lo_d_child(struct inode *i_parent, struct dentry *d_child,
		      bool is_dir, struct hmdfs_recursive_para *rec_op_para)
{
	struct dentry *d_pparent, *lo_d_parent, *lo_d_child;
	struct dentry *d_parent = dget_parent(d_child);
	struct hmdfs_dentry_info_merge *pmdi = hmdfs_dm(d_parent);
	int ret = 0;
	mode_t d_child_mode = rec_op_para->mode;

	wait_event(pmdi->wait_queue, !has_merge_lookup_work(pmdi));

	lo_d_parent = hmdfs_get_lo_d(d_parent, HMDFS_DEVID_LOCAL);
	if (!lo_d_parent) {
		d_pparent = dget_parent(d_parent);
		ret = create_lo_d_parent_recur(d_pparent, d_parent,
					       d_inode(d_parent)->i_mode,
					       rec_op_para);
		dput(d_pparent);
		if (unlikely(ret)) {
			lo_d_child = ERR_PTR(ret);
			goto out;
		}
		lo_d_parent = hmdfs_get_lo_d(d_parent, HMDFS_DEVID_LOCAL);
		if (!lo_d_parent) {
			lo_d_child = ERR_PTR(-ENOENT);
			goto out;
		}
	}
	rec_op_para->is_last = true;
	rec_op_para->mode = d_child_mode;
	ret = hmdfs_create_lower_dentry(i_parent, d_child, lo_d_parent, is_dir,
					rec_op_para);

out:
	dput(d_parent);
	dput(lo_d_parent);
	return ret;
}

void hmdfs_init_recursive_para(struct hmdfs_recursive_para *rec_op_para,
			       int opcode, mode_t mode, bool want_excl,
			       const char *name)
{
	rec_op_para->is_last = true;
	rec_op_para->opcode = opcode;
	rec_op_para->mode = mode;
	rec_op_para->want_excl = want_excl;
	rec_op_para->name = name;
}

int hmdfs_mkdir_merge(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int ret = 0;
	struct hmdfs_recursive_para *rec_op_para = NULL;

	// confict_name  & file_type is checked by hmdfs_mkdir_local
	if (hmdfs_file_type(dentry->d_name.name) != HMDFS_TYPE_COMMON) {
		ret = -EACCES;
		goto out;
	}
	rec_op_para = kmalloc(sizeof(*rec_op_para), GFP_KERNEL);
	if (!rec_op_para) {
		ret = -ENOMEM;
		goto out;
	}

	hmdfs_init_recursive_para(rec_op_para, F_MKDIR_MERGE, mode, false,
				  NULL);
	ret = create_lo_d_child(dir, dentry, true, rec_op_para);
out:
	hmdfs_trace_merge(trace_hmdfs_mkdir_merge, dir, dentry, ret);
	if (ret)
		d_drop(dentry);
	kfree(rec_op_para);
	return ret;
}

int hmdfs_create_merge(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode,
		       bool want_excl)
{
	struct hmdfs_recursive_para *rec_op_para = NULL;
	int ret = 0;

	rec_op_para = kmalloc(sizeof(*rec_op_para), GFP_KERNEL);
	if (!rec_op_para) {
		ret = -ENOMEM;
		goto out;
	}
	hmdfs_init_recursive_para(rec_op_para, F_CREATE_MERGE, mode, want_excl,
				  NULL);
	// confict_name  & file_type is checked by hmdfs_create_local
	ret = create_lo_d_child(dir, dentry, false, rec_op_para);
out:
	hmdfs_trace_merge(trace_hmdfs_create_merge, dir, dentry, ret);
	if (ret)
		d_drop(dentry);
	kfree(rec_op_para);
	return ret;
}

int do_rmdir_merge(struct inode *dir, struct dentry *dentry)
{
	int ret = 0;
	struct hmdfs_dentry_info_merge *dim = hmdfs_dm(dentry);
	struct hmdfs_dentry_comrade *comrade = NULL;
	struct dentry *lo_d = NULL;
	struct dentry *lo_d_dir = NULL;
	struct inode *lo_i_dir = NULL;

	wait_event(dim->wait_queue, !has_merge_lookup_work(dim));

	mutex_lock(&dim->comrade_list_lock);
	list_for_each_entry(comrade, &(dim->comrade_list), list) {
		lo_d = comrade->lo_d;
		lo_d_dir = lock_parent(lo_d);
		lo_i_dir = d_inode(lo_d_dir);
		ret = vfs_rmdir(&nop_mnt_idmap, lo_i_dir, lo_d);
		unlock_dir(lo_d_dir);
		if (ret)
			break;
	}
	mutex_unlock(&dim->comrade_list_lock);
	hmdfs_trace_merge(trace_hmdfs_rmdir_merge, dir, dentry, ret);
	return ret;
}

int hmdfs_rmdir_merge(struct inode *dir, struct dentry *dentry)
{
	int ret = 0;

	if (hmdfs_file_type(dentry->d_name.name) != HMDFS_TYPE_COMMON) {
		ret = -EACCES;
		goto out;
	}

	ret = do_rmdir_merge(dir, dentry);
	if (ret) {
		hmdfs_err("rm dir failed:%d", ret);
		goto out;
	}

	hmdfs_update_meta(dir);
	d_drop(dentry);
out:
	hmdfs_trace_merge(trace_hmdfs_rmdir_merge, dir, dentry, ret);
	return ret;
}

int do_unlink_merge(struct inode *dir, struct dentry *dentry)
{
	int ret = 0;
	struct hmdfs_dentry_info_merge *dim = hmdfs_dm(dentry);
	struct hmdfs_dentry_comrade *comrade = NULL;
	struct dentry *lo_d = NULL;
	struct dentry *lo_d_dir = NULL;
	struct dentry *lo_d_lookup = NULL;
	struct inode *lo_i_dir = NULL;

	wait_event(dim->wait_queue, !has_merge_lookup_work(dim));

	mutex_lock(&dim->comrade_list_lock);
	list_for_each_entry(comrade, &(dim->comrade_list), list) {
		lo_d = comrade->lo_d;
		dget(lo_d);
		lo_d_dir = lock_parent(lo_d);
		/* lo_d could be unhashed, need to lookup again here */
		lo_d_lookup = lookup_one_len(lo_d->d_name.name, lo_d_dir,
					     strlen(lo_d->d_name.name));
		if (IS_ERR(lo_d_lookup)) {
			ret = PTR_ERR(lo_d_lookup);
			hmdfs_err("lookup_one_len failed, err = %d", ret);
			unlock_dir(lo_d_dir);
			dput(lo_d);
			break;
		}
		lo_i_dir = d_inode(lo_d_dir);
		ret = vfs_unlink(&nop_mnt_idmap, lo_i_dir, lo_d_lookup, NULL);
		dput(lo_d_lookup);
		unlock_dir(lo_d_dir);
		dput(lo_d);
		if (ret)
			break;
	}
	mutex_unlock(&dim->comrade_list_lock);

	return ret;
}

int hmdfs_unlink_merge(struct inode *dir, struct dentry *dentry)
{
	int ret = 0;

	if (hmdfs_file_type(dentry->d_name.name) != HMDFS_TYPE_COMMON) {
		ret = -EACCES;
		goto out;
	}

	ret = do_unlink_merge(dir, dentry);
	if (ret) {
		hmdfs_err("unlink failed:%d", ret);
		goto out;
	} else {
		hmdfs_update_meta(dir);
	}

	d_drop(dentry);
out:
	return ret;
}

int do_rename_merge(struct inode *old_dir, struct dentry *old_dentry,
		    struct inode *new_dir, struct dentry *new_dentry,
		    unsigned int flags)
{
	int ret = 0;
	struct hmdfs_sb_info *sbi = (old_dir->i_sb)->s_fs_info;
	struct hmdfs_dentry_info_merge *dim = hmdfs_dm(old_dentry);
	struct hmdfs_dentry_comrade *comrade = NULL, *new_comrade = NULL;
	struct path lo_p_new = { .mnt = NULL, .dentry = NULL };
	struct inode *lo_i_old_dir = NULL, *lo_i_new_dir = NULL;
	struct dentry *lo_d_old_dir = NULL, *lo_d_old = NULL,
		      *lo_d_new_dir = NULL, *lo_d_new = NULL;
	struct dentry *d_new_dir = NULL;
	char *path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	char *abs_path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	char *path_name = NULL;
	struct hmdfs_dentry_info_merge *pmdi = NULL;
	struct renamedata rename_data;

	if (flags & ~RENAME_NOREPLACE) {
		ret = -EINVAL;
		goto out;
	}

	if (unlikely(!path_buf || !abs_path_buf)) {
		ret = -ENOMEM;
		goto out;
	}

	wait_event(dim->wait_queue, !has_merge_lookup_work(dim));

	list_for_each_entry(comrade, &dim->comrade_list, list) {
		lo_d_old = comrade->lo_d;
		d_new_dir = d_find_alias(new_dir);
		pmdi = hmdfs_dm(d_new_dir);
		wait_event(pmdi->wait_queue, !has_merge_lookup_work(pmdi));
		lo_d_new_dir = hmdfs_get_lo_d(d_new_dir, comrade->dev_id);
		dput(d_new_dir);

		if (!lo_d_new_dir)
			continue;
		path_name = dentry_path_raw(lo_d_new_dir, path_buf, PATH_MAX);
		dput(lo_d_new_dir);
		if (IS_ERR(path_name)) {
			ret = PTR_ERR(path_name);
			continue;
		}

		if (strlen(sbi->real_dst) + strlen(path_name) +
		    strlen(new_dentry->d_name.name) + 2 > PATH_MAX) {
			ret = -ENAMETOOLONG;
			goto out;
		}

		snprintf(abs_path_buf, PATH_MAX, "%s%s/%s", sbi->real_dst,
			 path_name, new_dentry->d_name.name);
		if (S_ISDIR(d_inode(old_dentry)->i_mode))
			lo_d_new = kern_path_create(AT_FDCWD, abs_path_buf,
						    &lo_p_new,
						    LOOKUP_DIRECTORY);
		else
			lo_d_new = kern_path_create(AT_FDCWD, abs_path_buf,
						    &lo_p_new, 0);
		if (IS_ERR(lo_d_new)) {
			ret = PTR_ERR(lo_d_new);
			goto out;
		}

		lo_d_new_dir = dget_parent(lo_d_new);
		lo_i_new_dir = d_inode(lo_d_new_dir);
		lo_d_old_dir = dget_parent(lo_d_old);
		lo_i_old_dir = d_inode(lo_d_old_dir);

		rename_data.old_mnt_idmap = &nop_mnt_idmap;
		rename_data.old_dir = lo_i_old_dir;
		rename_data.old_dentry = lo_d_old;
		rename_data.new_mnt_idmap  = &nop_mnt_idmap;
		rename_data.new_dir = lo_i_new_dir;
		rename_data.new_dentry = lo_d_new;
		rename_data.flags = flags;
		ret = vfs_rename(&rename_data);

		new_comrade = alloc_comrade(lo_p_new.dentry, comrade->dev_id);
		if (IS_ERR(new_comrade)) {
			ret = PTR_ERR(new_comrade);
			goto no_comrade;
		}

		link_comrade_unlocked(new_dentry, new_comrade);
no_comrade:
		done_path_create(&lo_p_new, lo_d_new);
		dput(lo_d_old_dir);
		dput(lo_d_new_dir);
	}
out:
	kfree(abs_path_buf);
	kfree(path_buf);
	return ret;
}

int hmdfs_rename_merge(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry,
		       unsigned int flags)
{
	char *old_dir_buf = NULL;
	char *new_dir_buf = NULL;
	char *old_dir_path = NULL;
	char *new_dir_path = NULL;
	struct dentry *old_dir_dentry = NULL;
	struct dentry *new_dir_dentry = NULL;
	int ret = 0;

	if (hmdfs_file_type(old_dentry->d_name.name) != HMDFS_TYPE_COMMON ||
	    hmdfs_file_type(new_dentry->d_name.name) != HMDFS_TYPE_COMMON) {
		ret = -EACCES;
		goto rename_out;
	}

	if (hmdfs_i(old_dir)->inode_type != hmdfs_i(new_dir)->inode_type) {
		hmdfs_err("in different view");
		ret = -EPERM;
		goto rename_out;
	}

	old_dir_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	new_dir_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!old_dir_buf || !new_dir_buf) {
		ret = -ENOMEM;
		goto rename_out;
	}

	new_dir_dentry = d_find_alias(new_dir);
	if (!new_dir_dentry) {
		ret = -EINVAL;
		goto rename_out;
	}

	old_dir_dentry = d_find_alias(old_dir);
	if (!old_dir_dentry) {
		ret = -EINVAL;
		dput(new_dir_dentry);
		goto rename_out;
	}

	old_dir_path = dentry_path_raw(old_dir_dentry, old_dir_buf, PATH_MAX);
	new_dir_path = dentry_path_raw(new_dir_dentry, new_dir_buf, PATH_MAX);
	dput(new_dir_dentry);
	dput(old_dir_dentry);
	if (strcmp(old_dir_path, new_dir_path)) {
		ret = -EPERM;
		goto rename_out;
	}

	trace_hmdfs_rename_merge(old_dir, old_dentry, new_dir, new_dentry,
				 flags);
	ret = do_rename_merge(old_dir, old_dentry, new_dir, new_dentry, flags);

	if (ret != 0)
		d_drop(new_dentry);

	if (S_ISREG(old_dentry->d_inode->i_mode) && !ret)
		d_invalidate(old_dentry);

rename_out:
	kfree(old_dir_buf);
	kfree(new_dir_buf);
	return ret;
}

const struct inode_operations hmdfs_dir_iops_merge = {
	.lookup = hmdfs_lookup_merge,
	.mkdir = hmdfs_mkdir_merge,
	.create = hmdfs_create_merge,
	.rmdir = hmdfs_rmdir_merge,
	.unlink = hmdfs_unlink_merge,
	.rename = hmdfs_rename_merge,
	.permission = hmdfs_permission,
};
