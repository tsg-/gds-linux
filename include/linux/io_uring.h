/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_IO_URING_H
#define _LINUX_IO_URING_H

#include <linux/sched.h>
#include <linux/xarray.h>
#include <uapi/linux/io_uring.h>
#include <linux/blk-mq.h>
#include <linux/scatterlist.h>
#include <linux/dma-buf.h>

#if defined(CONFIG_IO_URING)

struct io_uring_dma_buf {
	struct			dma_buf_attachment *attach;
	struct			sg_table *sgt;
	unsigned int		dmabuf_fd;
	unsigned int		dmabuf_offset;
};

void io_uring_release_dmabuf(struct io_uring_dma_buf *uring_dmabuf);
struct io_uring_dma_buf *io_uring_get_dmabuf(struct request *req, struct device *dev);
void __io_uring_cancel(bool cancel_all);
void __io_uring_free(struct task_struct *tsk);
void io_uring_unreg_ringfd(void);
const char *io_uring_get_opcode(u8 opcode);
bool io_is_uring_fops(struct file *file);

static inline void io_uring_files_cancel(void)
{
	if (current->io_uring) {
		io_uring_unreg_ringfd();
		__io_uring_cancel(false);
	}
}
static inline void io_uring_task_cancel(void)
{
	if (current->io_uring)
		__io_uring_cancel(true);
}
static inline void io_uring_free(struct task_struct *tsk)
{
	if (tsk->io_uring)
		__io_uring_free(tsk);
}
#else
static inline void io_uring_release_dmabuf(struct io_uring_dma_buf *uring_dmabuf)
{
}
static inline struct io_uring_dma_buf *io_uring_get_dmabuf(struct request *req, struct device *dev)
{
	return NULL;
}
static inline void io_uring_task_cancel(void)
{
}
static inline void io_uring_files_cancel(void)
{
}
static inline void io_uring_free(struct task_struct *tsk)
{
}
static inline const char *io_uring_get_opcode(u8 opcode)
{
	return "";
}
static inline bool io_is_uring_fops(struct file *file)
{
	return false;
}
#endif

#endif
