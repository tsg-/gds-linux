// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 XFS dmabuf token support
 * Proof-of-concept implementation for direct GPU-NVMe I/O
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_bmap_btree.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include <linux/dma_token.h>
#include <linux/blk-mq-dma-token.h>
#include <linux/blkdev.h>

/*
 * XFS-specific dma_token structure
 * Extends base dma_token with filesystem-specific state
 */
struct xfs_dma_token {
	struct dma_token		base;
	struct xfs_inode		*ip;
	struct dma_buf			*dmabuf;
	struct dma_token		*blk_token;  /* underlying block token */
	
	loff_t				offset;
	size_t				length;
	xfs_fileoff_t			bno;        /* starting block offset */
	xfs_fsblock_t			startblock; /* physical block */
	xfs_filblks_t			blockcount; /* number of blocks */
	
	enum dma_data_direction		dir;
	struct mutex			lock;
	refcount_t			refs;
};

static void xfs_dma_token_release(struct dma_token *token)
{
	struct xfs_dma_token *xfs_token = container_of(token, 
						struct xfs_dma_token, base);
	
	/* Release underlying block device token */
	if (xfs_token->blk_token)
		dma_token_release(xfs_token->blk_token);
	
	/* Release inode reference */
	if (xfs_token->ip)
		iput(VFS_I(xfs_token->ip));
	
	dma_buf_put(xfs_token->dmabuf);
	kfree(xfs_token);
}

/*
 * Verify that file extents are suitable for DMA
 * Returns 0 if extents are contiguous and properly aligned
 */
static int
xfs_verify_dma_extents(
	struct xfs_inode	*ip,
	loff_t			offset,
	size_t			length,
	xfs_fileoff_t		*bno_out,
	xfs_fsblock_t		*startblock_out,
	xfs_filblks_t		*blockcount_out)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_bmbt_irec	imap;
	xfs_fileoff_t		offset_fsb;
	xfs_filblks_t		count_fsb;
	int			nimaps = 1;
	int			error;
	
	/* Must be O_DIRECT aligned */
	if (offset & (XFS_FSB_TO_B(mp, 1) - 1))
		return -EINVAL;
	if (length & (XFS_FSB_TO_B(mp, 1) - 1))
		return -EINVAL;
	
	offset_fsb = XFS_B_TO_FSBT(mp, offset);
	count_fsb = XFS_B_TO_FSB(mp, length);
	
	/* Lock inode for extent lookup */
	xfs_ilock(ip, XFS_ILOCK_SHARED);
	
	/* Look up extent mapping */
	error = xfs_bmapi_read(ip, offset_fsb, count_fsb, &imap, &nimaps, 0);
	if (error)
		goto out_unlock;
	
	/* Must map to exactly one extent */
	if (nimaps != 1) {
		error = -EOPNOTSUPP;
		goto out_unlock;
	}
	
	/* Extent must not be a hole */
	if (imap.br_startblock == HOLESTARTBLOCK) {
		error = -EOPNOTSUPP;
		goto out_unlock;
	}
	
	/* Extent must not be delayed allocation */
	if (imap.br_startblock == DELAYSTARTBLOCK) {
		error = -EOPNOTSUPP;
		goto out_unlock;
	}
	
	/* Extent must cover entire requested range */
	if (imap.br_startoff > offset_fsb ||
	    imap.br_startoff + imap.br_blockcount < offset_fsb + count_fsb) {
		error = -EOPNOTSUPP;
		goto out_unlock;
	}
	
	/* Extent must be written (not unwritten/preallocated) */
	if (imap.br_state != XFS_EXT_NORM) {
		error = -EOPNOTSUPP;
		goto out_unlock;
	}
	
	/* Calculate actual physical block and offset within extent */
	*bno_out = offset_fsb;
	*startblock_out = imap.br_startblock + (offset_fsb - imap.br_startoff);
	*blockcount_out = count_fsb;
	
out_unlock:
	xfs_iunlock(ip, XFS_ILOCK_SHARED);
	return error;
}

/*
 * Invalidate page cache for the file range
 * Required to prevent stale data from buffered reads
 */
static int
xfs_invalidate_range(
	struct xfs_inode	*ip,
	loff_t			offset,
	size_t			length)
{
	struct address_space	*mapping = VFS_I(ip)->i_mapping;
	pgoff_t			start = offset >> PAGE_SHIFT;
	pgoff_t			end = (offset + length - 1) >> PAGE_SHIFT;
	
	/* Flush any dirty pages first */
	filemap_write_and_wait_range(mapping, offset, offset + length - 1);
	
	/* Invalidate clean pages */
	return invalidate_inode_pages2_range(mapping, start, end);
}

/*
 * Main dmabuf mapping function for XFS
 * Called from io_uring when registering a dmabuf for file I/O
 */
#include "xfs_dma_token.h"

struct dma_token *
xfs_file_dma_map(
	struct file			*file,
	struct dma_token_params		*params)
{
	struct inode			*inode = file_inode(file);
	struct xfs_inode		*ip = XFS_I(inode);
	struct super_block		*sb = inode->i_sb;
	struct block_device		*bdev = sb->s_bdev;
	struct xfs_dma_token		*xfs_token;
	xfs_fileoff_t			bno;
	xfs_fsblock_t			startblock;
	xfs_filblks_t			blockcount;
	loff_t				offset = 0;  /* Would come from params */
	size_t				length;
	int				error;
	
	/* Only support O_DIRECT files */
	if (!(file->f_flags & O_DIRECT))
		return ERR_PTR(-EINVAL);
	
	/* Get dmabuf size for length */
	length = params->dmabuf->size;
	
	/* Verify extent layout is suitable for DMA */
	error = xfs_verify_dma_extents(ip, offset, length, &bno, 
				       &startblock, &blockcount);
	if (error)
		return ERR_PTR(error);
	
	/* Allocate XFS token */
	xfs_token = kzalloc(sizeof(*xfs_token), GFP_KERNEL);
	if (!xfs_token)
		return ERR_PTR(-ENOMEM);
	
	/* Initialize token */
	xfs_token->base.release = xfs_dma_token_release;
	xfs_token->ip = ip;
	xfs_token->offset = offset;
	xfs_token->length = length;
	xfs_token->bno = bno;
	xfs_token->startblock = startblock;
	xfs_token->blockcount = blockcount;
	xfs_token->dir = params->dir;
	xfs_token->dmabuf = params->dmabuf;
	mutex_init(&xfs_token->lock);
	refcount_set(&xfs_token->refs, 1);
	
	/* Hold reference to inode */
	ihold(VFS_I(ip));
	get_dma_buf(params->dmabuf);
	
	/* Invalidate page cache for this range */
	error = xfs_invalidate_range(ip, offset, length);
	if (error) {
		xfs_token->ip = NULL;  /* Prevent double release */
		xfs_dma_token_release(&xfs_token->base);
		return ERR_PTR(error);
	}
	
	/* Delegate to block layer for actual DMA mapping */
	xfs_token->blk_token = blk_mq_dma_map(bdev_get_queue(bdev), params);
	if (IS_ERR(xfs_token->blk_token)) {
		error = PTR_ERR(xfs_token->blk_token);
		xfs_token->blk_token = NULL;
		xfs_token->ip = NULL;
		xfs_dma_token_release(&xfs_token->base);
		return ERR_PTR(error);
	}
	
	/* TODO: Add trace point for debugging */
	
	return &xfs_token->base;
}
