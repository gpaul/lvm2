/*
 * device-mapper.c
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This software is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Changelog
 *
 *    14/08/2001 - First Version [Joe Thornber]
 */

#include "dm.h"

/* defines for blk.h */
#define MAJOR_NR DM_BLK_MAJOR
#define DEVICE_NR(device) MINOR(device)  /* has no partition bits */
#define DEVICE_NAME "device-mapper"      /* name for messaging */
#define DEVICE_NO_RANDOM                 /* no entropy to contribute */
#define DEVICE_OFF(d)                    /* do-nothing */

#include <linux/blk.h>

#define MAX_DEVICES 64
#define DEFAULT_READ_AHEAD 64

const char *_name = "device-mapper";
int _version[3] = {0, 1, 0};

struct io_hook {
	struct mapped_device *md;
	void (*end_io)(struct buffer_head *bh, int uptodate);
	void *context;
};

#define rl down_read(&_dev_lock)
#define ru up_read(&_dev_lock)
#define wl down_write(&_dev_lock)
#define wu up_write(&_dev_lock)

struct rw_semaphore _dev_lock;
static struct mapped_device *_devs[MAX_DEVICES];

/* block device arrays */
static int _block_size[MAX_DEVICES];
static int _blksize_size[MAX_DEVICES];
static int _hardsect_size[MAX_DEVICES];

static int blk_open(struct inode *inode, struct file *file);
static int blk_close(struct inode *inode, struct file *file);
static int blk_ioctl(struct inode *inode, struct file *file,
		     uint command, ulong a);

struct block_device_operations dm_blk_dops = {
	open:     blk_open,
	release:  blk_close,
	ioctl:    blk_ioctl
};

static int request(request_queue_t *q, int rw, struct buffer_head *bh);

/*
 * setup and teardown the driver
 */
static int init(void)
{
	int ret;

	init_rwsem(&_dev_lock);

	if ((ret = dm_init_fs()))
		return ret;

	if (dm_std_targets())
		return -EIO;	/* FIXME: better error value */

	/* set up the arrays */
	read_ahead[MAJOR_NR] = DEFAULT_READ_AHEAD;
	blk_size[MAJOR_NR] = _block_size;
	blksize_size[MAJOR_NR] = _blksize_size;
	hardsect_size[MAJOR_NR] = _hardsect_size;

	if (devfs_register_blkdev(MAJOR_NR, _name, &dm_blk_dops) < 0) {
		printk(KERN_ERR "%s -- register_blkdev failed\n", _name);
		return -EIO;
	}

	blk_queue_make_request(BLK_DEFAULT_QUEUE(MAJOR_NR), request);

	printk(KERN_INFO "%s %d.%d.%d initialised\n", _name,
	       _version[0], _version[1], _version[2]);
	return 0;
}

static void fin(void)
{
	dm_fin_fs();

	if (devfs_unregister_blkdev(MAJOR_NR, _name) < 0)
		printk(KERN_ERR "%s -- unregister_blkdev failed\n", _name);

	read_ahead[MAJOR_NR] = 0;
	blk_size[MAJOR_NR] = 0;
	blksize_size[MAJOR_NR] = 0;
	hardsect_size[MAJOR_NR] = 0;

	printk(KERN_INFO "%s %d.%d.%d cleaned up\n", _name,
	       _version[0], _version[1], _version[2]);
}

/*
 * block device functions
 */
static int blk_open(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	wl;
	md = _devs[minor];

	if (!md || !is_active(md)) {
		wu;
		return -ENXIO;
	}

	md->use_count++;
	wu;

	MOD_INC_USE_COUNT;
	return 0;
}

static int blk_close(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	wl;
	md = _devs[minor];
	if (!md || md->use_count < 1) {
		WARN("reference count in mapped_device incorrect");
		wu;
		return -ENXIO;
	}

	md->use_count--;
	wu;

	MOD_DEC_USE_COUNT;
	return 0;
}

static int blk_ioctl(struct inode *inode, struct file *file,
		      uint command, ulong a)
{
	/* FIXME: check in the latest Rubini that all expected ioctl's
	   are supported */

	int minor = MINOR(inode->i_rdev);
	long size;

	switch (command) {
	case BLKGETSIZE:
		size = _block_size[minor] * 1024 / _hardsect_size[minor];
		if (copy_to_user((void *) a, &size, sizeof(long)))
			return -EFAULT;
		break;

	case BLKFLSBUF:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		fsync_dev(inode->i_rdev);
		invalidate_buffers(inode->i_rdev);
		return 0;

	case BLKRAGET:
		if (copy_to_user((void *) a, &read_ahead[MAJOR(inode->i_rdev)],
				sizeof(long)))
			return -EFAULT;
		return 0;

	case BLKRASET:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		read_ahead[MAJOR(inode->i_rdev)] = a;
		return 0;

	case BLKRRPART:
		return -EINVAL;

	default:
		printk(KERN_WARNING "%s - unknown block ioctl %d",
		       _name, command);
		return -EINVAL;
	}

	return 0;
}

/* FIXME: These should have their own slab */
inline static struct io_hook *alloc_io_hook(void)
{
	return kmalloc(sizeof(struct io_hook), GFP_NOIO);
}

inline static void free_io_hook(struct io_hook *ih)
{
	kfree(ih);
}

inline static struct deferred_io *alloc_deferred(void)
{
	return kmalloc(sizeof(struct deferred_io), GFP_NOIO);
}

inline static void free_deferred(struct deferred_io *di)
{
	kfree(di);
}

static void dec_pending(struct buffer_head *bh, int uptodate)
{
	struct io_hook *ih = bh->b_private;

	if (atomic_dec_and_test(&ih->md->pending))
		/* nudge anyone waiting on suspend queue */
		wake_up_interruptible(&ih->md->wait);

	bh->b_end_io = ih->end_io;
	bh->b_private = ih->context;
	free_io_hook(ih);

	bh->b_end_io(bh, uptodate);
}

static int queue_io(struct mapped_device *md, struct buffer_head *bh, int rw)
{
	struct deferred_io *di = alloc_deferred();

	if (!di)
		return -ENOMEM;

	wl;
	if (test_bit(DM_ACTIVE, &md->state)) {
		wu;
		return 0;
	}

	di->bh = bh;
	di->rw = rw;
	di->next = md->deferred;
	md->deferred = di;
	wu;

	return 1;
}


inline static int __map_buffer(struct mapped_device *md,
			       struct buffer_head *bh, int node)
{
	dm_map_fn fn;
	void *context;
	struct io_hook *ih = 0;
	int r;
	struct target_instance *ti = md->targets + node;

	fn = ti->map;
	context = ti->private;

	if (!fn)
		return 0;

	ih = alloc_io_hook();

	if (!ih)
		return 0;

	ih->md = md;
	ih->end_io = bh->b_end_io;
	ih->context = bh->b_private;

	r = fn(bh, context);

	if (r > 0) {
		/* hook the end io request fn */
		atomic_inc(&md->pending);
		bh->b_end_io = dec_pending;
		bh->b_private = ih;

	} else if (r == 0)
		/* we don't need to hook */
		free_io_hook(ih);

	else if (r < 0) {
		free_io_hook(ih);
		return 0;
	}

	return 1;
}

inline static int __find_node(struct mapped_device *md, struct buffer_head *bh)
{
	int i = 0, l, r = 0;
	offset_t *node;

	/* search the btree for the correct target */
	for (l = 0; l < md->depth; l++) {
		r = ((KEYS_PER_NODE + 1) * r) + i;
		node = md->index[l] + (r * KEYS_PER_NODE);

		for (i = 0; i < KEYS_PER_NODE; i++)
			if (node[i] >= bh->b_rsector)
				break;
	}

	return (KEYS_PER_NODE * r) + i;
}

static int request(request_queue_t *q, int rw, struct buffer_head *bh)
{
	struct mapped_device *md;
	int r, minor = MINOR(bh->b_rdev);

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	rl;
	md = _devs[minor];

	if (!md || !test_bit(DM_LOADED, &md->state))
		goto bad;

	/* if we're suspended we have to queue this io for later */
	if (!test_bit(DM_ACTIVE, &md->state)) {
		ru;
		r = queue_io(md, bh, rw);

		if (r < 0) {
			buffer_IO_error(bh);
			return 0;

		} else if (r > 0)
			return 0; /* deferred successfully */

		rl;	/* FIXME: there's still a race here */
	}

	if (!__map_buffer(md, bh, __find_node(md, bh)))
		goto bad;

	ru;
	return 1;

 bad:
	ru;
	buffer_IO_error(bh);
	return 0;
}

static inline int __specific_dev(int minor)
{
	if (minor > MAX_DEVICES) {
		WARN("request for a mapped_device > than MAX_DEVICES");
		return 0;
	}

	if (!_devs[minor])
		return minor;

	return -1;
}

static inline int __any_old_dev(void)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++)
		if (!_devs[i])
			return i;

	return -1;
}

static struct mapped_device *alloc_dev(int minor)
{
	struct mapped_device *md = kmalloc(sizeof(*md), GFP_KERNEL);
	memset(md, 0, sizeof(*md));

	wl;
	minor = (minor < 0) ? __any_old_dev() : __specific_dev(minor);

	if (minor < 0) {
		WARN("no free devices available");
		wu;
		kfree(md);
		return 0;
	}

	md->dev = MKDEV(DM_BLK_MAJOR, minor);
	md->name[0] = '\0';
	md->state = 0;

	init_waitqueue_head(&md->wait);

	_devs[minor] = md;
	wu;

	return md;
}

static inline struct mapped_device *__find_name(const char *name)
{
	int i;
	for (i = 0; i < MAX_DEVICES; i++)
		if (_devs[i] && !strcmp(_devs[i]->name, name))
			return _devs[i];

	return 0;
}

static int open_dev(struct dev_list *d)
{
	int err;

	if (!(d->bd = bdget(kdev_t_to_nr(d->dev))))
		return -ENOMEM;

	if ((err = blkdev_get(d->bd, FMODE_READ|FMODE_WRITE, 0, BDEV_FILE))) {
		bdput(d->bd);
		return err;
	}

	return 0;
}

static void close_dev(struct dev_list *d)
{
	blkdev_put(d->bd, BDEV_FILE);
	bdput(d->bd);
	d->bd = 0;
}

static int __find_hardsect_size(struct mapped_device *md)
{
	int r = INT_MAX, s;
	struct dev_list *dl;

	for (dl = md->devices; dl; dl = dl->next) {
		s = get_hardsect_size(dl->dev);
		if (s < r)
			r = s;
	}

	return r;
}

struct mapped_device *dm_find_by_name(const char *name)
{
	struct mapped_device *md;

	rl;
	md = __find_name(name);
	ru;

	return md;
}

struct mapped_device *dm_find_by_minor(int minor)
{
	struct mapped_device *md;

	rl;
	md = _devs[minor];
	ru;

	return md;
}

int dm_create(const char *name, int minor)
{
	int r;
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	if (!(md = alloc_dev(minor)))
		return -ENOMEM;

	wl;
	if (__find_name(name)) {
		WARN("device with that name already exists");
		kfree(md);
		wu;
		return -EINVAL;
	}

	strcpy(md->name, name);
	_devs[minor] = md;

	if ((r = dm_fs_add(md))) {
		wu;
		return r;
	}
	wu;

	return 0;
}

int dm_remove(const char *name)
{
	struct mapped_device *md;
	struct dev_list *d, *n;
	int minor, r;

	wl;
	if (!(md = __find_name(name))) {
		wu;
		return -ENXIO;
	}

	if (md->use_count) {
		wu;
		return -EPERM;
	}

	if ((r = dm_fs_remove(md))) {
		wu;
		return r;
	}

	dm_free_table(md);
	for (d = md->devices; d; d = n) {
		n = d->next;
		kfree(d);
	}

	minor = MINOR(md->dev);
	kfree(md);
	_devs[minor] = 0;
	wu;

	return 0;
}

int dm_add_device(struct mapped_device *md, kdev_t dev)
{
	struct dev_list *d = kmalloc(sizeof(*d), GFP_KERNEL);

	if (!d)
		return -EINVAL;

	d->dev = dev;
	d->next = md->devices;
	md->devices = d;

	return 0;
}

static void __flush_deferred_io(struct mapped_device *md)
{
	struct deferred_io *c, *n;

	for (c = md->deferred, md->deferred = 0; c; c = n) {
		n = c->next;
		generic_make_request(c->rw, c->bh);
		free_deferred(c);
	}
}

int dm_activate(struct mapped_device *md)
{
	int ret, minor;
	struct dev_list *d, *od;

	wl;

	if (is_active(md)) {
		wu;
		return 0;
	}

	if (!md->num_targets) {
		wu;
		return -ENXIO;
	}

	/* open all the devices */
	for (d = md->devices; d; d = d->next)
		if ((ret = open_dev(d)))
			goto bad;

	minor = MINOR(md->dev);

	_block_size[minor] = (md->highs[md->num_targets - 1] + 1) >> 1;
	_blksize_size[minor] = BLOCK_SIZE; /* FIXME: this depends on
                                              the mapping table */
	_hardsect_size[minor] = __find_hardsect_size(md);

	register_disk(NULL, md->dev, 1, &dm_blk_dops, _block_size[minor]);

	set_bit(DM_ACTIVE, &md->state);

	__flush_deferred_io(md);
	wu;

	return 0;

 bad:
	od = d;
	for (d = md->devices; d != od; d = d->next)
		close_dev(d);
	ru;

	return ret;
}

void dm_suspend(struct mapped_device *md)
{
	DECLARE_WAITQUEUE(wait, current);
	struct dev_list *d;
	if (!is_active(md))
		return;

	/* wait for all the pending io to flush */
	add_wait_queue(&md->wait, &wait);
	current->state = TASK_INTERRUPTIBLE;
	do {
		wl;
		if (!atomic_read(&md->pending))
			break;

		wu;
		schedule();

	} while (1);

	current->state = TASK_RUNNING;
	remove_wait_queue(&md->wait, &wait);

	/* close all the devices */
	for (d = md->devices; d; d = d->next)
		close_dev(d);

	clear_bit(DM_ACTIVE, &md->state);
	wu;
}


/*
 * module hooks
 */
module_init(init);
module_exit(fin);

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
