// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 XFS dmabuf token support
 */
#ifndef __XFS_DMA_TOKEN_H__
#define __XFS_DMA_TOKEN_H__

struct file;
struct dma_token;
struct dma_token_params;

/* Main entry point for dmabuf registration */
struct dma_token *xfs_file_dma_map(struct file *file,
				   struct dma_token_params *params);

#endif /* __XFS_DMA_TOKEN_H__ */
