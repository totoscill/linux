/**
 * Copyright (c) 2010-2012 Broadcom. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>

#include "vc_cma.h"

#include "vchiq_util.h"
#include "vchiq_connected.h"
//#include "debug_sym.h"
//#include "vc_mem.h"

#define DRIVER_NAME  "vc-cma"

#define LOG_DBG(fmt, ...) \
	if (vc_cma_debug) \
		printk(KERN_INFO fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) \
	printk(KERN_ERR fmt "\n", ##__VA_ARGS__)

#define VC_CMA_FOURCC VCHIQ_MAKE_FOURCC('C', 'M', 'A', ' ')
#define VC_CMA_VERSION 2

#define VC_CMA_CHUNK_ORDER 6	/* 256K */
#define VC_CMA_CHUNK_SIZE (4096 << VC_CMA_CHUNK_ORDER)
#define VC_CMA_MAX_PARAMS_PER_MSG \
	((VCHIQ_MAX_MSG_SIZE - sizeof(unsigned short))/sizeof(unsigned short))
#define VC_CMA_RESERVE_COUNT_MAX 16

#define PAGES_PER_CHUNK (VC_CMA_CHUNK_SIZE / PAGE_SIZE)

#define VCADDR_TO_PHYSADDR(vcaddr) (mm_vc_mem_phys_addr + vcaddr)

#define loud_error(...) \
	LOG_ERR("===== " __VA_ARGS__)

enum {
	VC_CMA_MSG_QUIT,
	VC_CMA_MSG_OPEN,
	VC_CMA_MSG_TICK,
	VC_CMA_MSG_ALLOC,	/* chunk count */
	VC_CMA_MSG_FREE,	/* chunk, chunk, ... */
	VC_CMA_MSG_ALLOCATED,	/* chunk, chunk, ... */
	VC_CMA_MSG_REQUEST_ALLOC,	/* chunk count */
	VC_CMA_MSG_REQUEST_FREE,	/* chunk count */
	VC_CMA_MSG_RESERVE,	/* bytes lo, bytes hi */
	VC_CMA_MSG_UPDATE_RESERVE,
	VC_CMA_MSG_MAX
};

struct cma_msg {
	unsigned short type;
	unsigned short params[VC_CMA_MAX_PARAMS_PER_MSG];
};

struct vc_cma_reserve_user {
	unsigned int pid;
	unsigned int reserve;
};

/* Device (/dev) related variables */
static dev_t vc_cma_devnum;
static struct class *vc_cma_class;
static struct cdev vc_cma_cdev;
static int vc_cma_inited;
static int vc_cma_debug;

/* Proc entry */
static struct proc_dir_entry *vc_cma_proc_entry;

phys_addr_t vc_cma_base;
struct page *vc_cma_base_page;
unsigned int vc_cma_size;
EXPORT_SYMBOL(vc_cma_size);
unsigned int vc_cma_initial;
unsigned int vc_cma_chunks;
unsigned int vc_cma_chunks_used;
unsigned int vc_cma_chunks_reserved;

static int in_loud_error;

unsigned int vc_cma_reserve_total;
unsigned int vc_cma_reserve_count;
struct vc_cma_reserve_user vc_cma_reserve_users[VC_CMA_RESERVE_COUNT_MAX];
static DEFINE_SEMAPHORE(vc_cma_reserve_mutex);
static DEFINE_SEMAPHORE(vc_cma_worker_queue_push_mutex);

static u64 vc_cma_dma_mask = DMA_BIT_MASK(32);
static struct platform_device vc_cma_device = {
	.name = "vc-cma",
	.id = 0,
	.dev = {
		.dma_mask = &vc_cma_dma_mask,
		.coherent_dma_mask = DMA_BIT_MASK(32),
		},
};

static VCHIQ_INSTANCE_T cma_instance;
static VCHIQ_SERVICE_HANDLE_T cma_service;
static VCHIU_QUEUE_T cma_msg_queue;
static struct task_struct *cma_worker;

static int vc_cma_set_reserve(unsigned int reserve, unsigned int pid);
static int vc_cma_alloc_chunks(int num_chunks, struct cma_msg *reply);
static VCHIQ_STATUS_T cma_service_callback(VCHIQ_REASON_T reason,
					   VCHIQ_HEADER_T * header,
					   VCHIQ_SERVICE_HANDLE_T service,
					   void *bulk_userdata);
static void send_vc_msg(unsigned short type,
			unsigned short param1, unsigned short param2);
static bool send_worker_msg(VCHIQ_HEADER_T * msg);

static int early_vc_cma_mem(char *p)
{
	unsigned int new_size;
	printk(KERN_NOTICE "early_vc_cma_mem(%s)", p);
	vc_cma_size = memparse(p, &p);
	vc_cma_initial = vc_cma_size;
	if (*p == '/')
		vc_cma_size = memparse(p + 1, &p);
	if (*p == '@')
		vc_cma_base = memparse(p + 1, &p);

	new_size = (vc_cma_size - ((-vc_cma_base) & (VC_CMA_CHUNK_SIZE - 1)))
	    & ~(VC_CMA_CHUNK_SIZE - 1);
	if (new_size > vc_cma_size)
		vc_cma_size = 0;
	vc_cma_initial = (vc_cma_initial + VC_CMA_CHUNK_SIZE - 1)
	    & ~(VC_CMA_CHUNK_SIZE - 1);
	if (vc_cma_initial > vc_cma_size)
		vc_cma_initial = vc_cma_size;
	vc_cma_base = (vc_cma_base + VC_CMA_CHUNK_SIZE - 1)
	    & ~(VC_CMA_CHUNK_SIZE - 1);

	printk(KERN_NOTICE " -> initial %x, size %x, base %x", vc_cma_initial,
	       vc_cma_size, (unsigned int)vc_cma_base);

	return 0;
}

early_param("vc-cma-mem", early_vc_cma_mem);

void vc_cma_early_init(void)
{
	LOG_DBG("vc_cma_early_init - vc_cma_chunks = %d", vc_cma_chunks);
	if (vc_cma_size) {
		int rc = platform_device_register(&vc_cma_device);
		LOG_DBG("platform_device_register -> %d", rc);
	}
}

void vc_cma_reserve(void)
{
	/* if vc_cma_size is set, then declare vc CMA area of the same
	 * size from the end of memory
	 */
	if (vc_cma_size) {
		if (dma_declare_contiguous(NULL /*&vc_cma_device.dev*/, vc_cma_size,
					   vc_cma_base, 0) == 0) {
		} else {
			LOG_ERR("vc_cma: dma_declare_contiguous(%x,%x) failed",
				vc_cma_size, (unsigned int)vc_cma_base);
			vc_cma_size = 0;
		}
	}
	vc_cma_chunks = vc_cma_size / VC_CMA_CHUNK_SIZE;
}

/****************************************************************************
*
*   vc_cma_open
*
***************************************************************************/

static int vc_cma_open(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;

	return 0;
}

/****************************************************************************
*
*   vc_cma_release
*
***************************************************************************/

static int vc_cma_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;

	vc_cma_set_reserve(0, current->tgid);

	return 0;
}

/****************************************************************************
*
*   vc_cma_ioctl
*
***************************************************************************/

static long vc_cma_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc = 0;

	(void)cmd;
	(void)arg;

	switch (cmd) {
	case VC_CMA_IOC_RESERVE:
		rc = vc_cma_set_reserve((unsigned int)arg, current->tgid);
		if (rc >= 0)
			rc = 0;
		break;
	default:
		LOG_ERR("vc-cma: Unknown ioctl %x", cmd);
		return -ENOTTY;
	}

	return rc;
}

/****************************************************************************
*
*   File Operations for the driver.
*
***************************************************************************/

static const struct file_operations vc_cma_fops = {
	.owner = THIS_MODULE,
	.open = vc_cma_open,
	.release = vc_cma_release,
	.unlocked_ioctl = vc_cma_ioctl,
};

/****************************************************************************
*
*   vc_cma_proc_open
*
***************************************************************************/

static int vc_cma_show_info(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "Videocore CMA:\n");
	seq_printf(m, "   Base       : %08x\n", (unsigned int)vc_cma_base);
	seq_printf(m, "   Length     : %08x\n", vc_cma_size);
	seq_printf(m, "   Initial    : %08x\n", vc_cma_initial);
	seq_printf(m, "   Chunk size : %08x\n", VC_CMA_CHUNK_SIZE);
	seq_printf(m, "   Chunks     : %4d (%d bytes)\n",
		   (int)vc_cma_chunks,
		   (int)(vc_cma_chunks * VC_CMA_CHUNK_SIZE));
	seq_printf(m, "   Used       : %4d (%d bytes)\n",
		   (int)vc_cma_chunks_used,
		   (int)(vc_cma_chunks_used * VC_CMA_CHUNK_SIZE));
	seq_printf(m, "   Reserved   : %4d (%d bytes)\n",
		   (unsigned int)vc_cma_chunks_reserved,
		   (int)(vc_cma_chunks_reserved * VC_CMA_CHUNK_SIZE));

	for (i = 0; i < vc_cma_reserve_count; i++) {
		struct vc_cma_reserve_user *user = &vc_cma_reserve_users[i];
		seq_printf(m, "     PID %5d: %d bytes\n", user->pid,
			   user->reserve);
	}

	seq_printf(m, "\n");

	return 0;
}

static int vc_cma_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, vc_cma_show_info, NULL);
}

/****************************************************************************
*
*   vc_cma_proc_write
*
***************************************************************************/

static int vc_cma_proc_write(struct file *file,
			     const char __user *buffer,
			     size_t size, loff_t *ppos)
{
	int rc = -EFAULT;
	char input_str[20];

	memset(input_str, 0, sizeof(input_str));

	if (size > sizeof(input_str)) {
		LOG_ERR("%s: input string length too long", __func__);
		goto out;
	}

	if (copy_from_user(input_str, buffer, size - 1)) {
		LOG_ERR("%s: failed to get input string", __func__);
		goto out;
	}
#define ALLOC_STR "alloc"
#define FREE_STR "free"
#define DEBUG_STR "debug"
#define RESERVE_STR "reserve"
	if (strncmp(input_str, ALLOC_STR, strlen(ALLOC_STR)) == 0) {
		int size;
		char *p = input_str + strlen(ALLOC_STR);

		while (*p == ' ')
			p++;
		size = memparse(p, NULL);
		LOG_ERR("/proc/vc-cma: alloc %d", size);
		if (size)
			send_vc_msg(VC_CMA_MSG_REQUEST_FREE,
				    size / VC_CMA_CHUNK_SIZE, 0);
		else
			LOG_ERR("invalid size '%s'", p);
		rc = size;
	} else if (strncmp(input_str, FREE_STR, strlen(FREE_STR)) == 0) {
		int size;
		char *p = input_str + strlen(FREE_STR);

		while (*p == ' ')
			p++;
		size = memparse(p, NULL);
		LOG_ERR("/proc/vc-cma: free %d", size);
		if (size)
			send_vc_msg(VC_CMA_MSG_REQUEST_ALLOC,
				    size / VC_CMA_CHUNK_SIZE, 0);
		else
			LOG_ERR("invalid size '%s'", p);
		rc = size;
	} else if (strncmp(input_str, DEBUG_STR, strlen(DEBUG_STR)) == 0) {
		char *p = input_str + strlen(DEBUG_STR);
		while (*p == ' ')
			p++;
		if ((strcmp(p, "on") == 0) || (strcmp(p, "1") == 0))
			vc_cma_debug = 1;
		else if ((strcmp(p, "off") == 0) || (strcmp(p, "0") == 0))
			vc_cma_debug = 0;
		LOG_ERR("/proc/vc-cma: debug %s", vc_cma_debug ? "on" : "off");
		rc = size;
	} else if (strncmp(input_str, RESERVE_STR, strlen(RESERVE_STR)) == 0) {
		int size;
		int reserved;
		char *p = input_str + strlen(RESERVE_STR);
		while (*p == ' ')
			p++;
		size = memparse(p, NULL);

		reserved = vc_cma_set_reserve(size, current->tgid);
		rc = (reserved >= 0) ? size : reserved;
	}

out:
	return rc;
}

/****************************************************************************
*
*   File Operations for /proc interface.
*
***************************************************************************/

static const struct file_operations vc_cma_proc_fops = {
	.open = vc_cma_proc_open,
	.read = seq_read,
	.write = vc_cma_proc_write,
	.llseek = seq_lseek,
	.release = single_release
};

static int vc_cma_set_reserve(unsigned int reserve, unsigned int pid)
{
	struct vc_cma_reserve_user *user = NULL;
	int delta = 0;
	int i;

	if (down_interruptible(&vc_cma_reserve_mutex))
		return -ERESTARTSYS;

	for (i = 0; i < vc_cma_reserve_count; i++) {
		if (pid == vc_cma_reserve_users[i].pid) {
			user = &vc_cma_reserve_users[i];
			delta = reserve - user->reserve;
			if (reserve)
				user->reserve = reserve;
			else {
				/* Remove this entry by copying downwards */
				while ((i + 1) < vc_cma_reserve_count) {
					user[0].pid = user[1].pid;
					user[0].reserve = user[1].reserve;
					user++;
					i++;
				}
				vc_cma_reserve_count--;
				user = NULL;
			}
			break;
		}
	}

	if (reserve && !user) {
		if (vc_cma_reserve_count == VC_CMA_RESERVE_COUNT_MAX) {
			LOG_ERR("vc-cma: Too many reservations - "
				"increase CMA_RESERVE_COUNT_MAX");
			up(&vc_cma_reserve_mutex);
			return -EBUSY;
		}
		user = &vc_cma_reserve_users[vc_cma_reserve_count];
		user->pid = pid;
		user->reserve = reserve;
		delta = reserve;
		vc_cma_reserve_count++;
	}

	vc_cma_reserve_total += delta;

	send_vc_msg(VC_CMA_MSG_RESERVE,
		    vc_cma_reserve_total & 0xffff, vc_cma_reserve_total >> 16);

	send_worker_msg((VCHIQ_HEADER_T *) VC_CMA_MSG_UPDATE_RESERVE);

	LOG_DBG("/proc/vc-cma: reserve %d (PID %d) - total %u",
		reserve, pid, vc_cma_reserve_total);

	up(&vc_cma_reserve_mutex);

	return vc_cma_reserve_total;
}

static VCHIQ_STATUS_T cma_service_callback(VCHIQ_REASON_T reason,
					   VCHIQ_HEADER_T * header,
					   VCHIQ_SERVICE_HANDLE_T service,
					   void *bulk_userdata)
{
	switch (reason) {
	case VCHIQ_MESSAGE_AVAILABLE:
		if (!send_worker_msg(header))
			return VCHIQ_RETRY;
		break;
	case VCHIQ_SERVICE_CLOSED:
		LOG_DBG("CMA service closed");
		break;
	default:
		LOG_ERR("Unexpected CMA callback reason %d", reason);
		break;
	}
	return VCHIQ_SUCCESS;
}

static void send_vc_msg(unsigned short type,
			unsigned short param1, unsigned short param2)
{
	unsigned short msg[] = { type, param1, param2 };
	VCHIQ_ELEMENT_T elem = { &msg, sizeof(msg) };
	VCHIQ_STATUS_T ret;
	vchiq_use_service(cma_service);
	ret = vchiq_queue_message(cma_service, &elem, 1);
	vchiq_release_service(cma_service);
	if (ret != VCHIQ_SUCCESS)
		LOG_ERR("vchiq_queue_message returned %x", ret);
}

static bool send_worker_msg(VCHIQ_HEADER_T * msg)
{
	if (down_interruptible(&vc_cma_worker_queue_push_mutex))
		return false;
	vchiu_queue_push(&cma_msg_queue, msg);
	up(&vc_cma_worker_queue_push_mutex);
	return true;
}

static int vc_cma_alloc_chunks(int num_chunks, struct cma_msg *reply)
{
	int i;
	for (i = 0; i < num_chunks; i++) {
		struct page *chunk;
		unsigned int chunk_num;
		uint8_t *chunk_addr;
		size_t chunk_size = PAGES_PER_CHUNK << PAGE_SHIFT;

		chunk = dma_alloc_from_contiguous(NULL /*&vc_cma_device.dev*/,
						  PAGES_PER_CHUNK,
						  VC_CMA_CHUNK_ORDER);
		if (!chunk)
			break;

		chunk_addr = page_address(chunk);
		dmac_flush_range(chunk_addr, chunk_addr + chunk_size);
		outer_inv_range(__pa(chunk_addr), __pa(chunk_addr) +
			chunk_size);

		chunk_num =
		    (page_to_phys(chunk) - vc_cma_base) / VC_CMA_CHUNK_SIZE;
		BUG_ON(((page_to_phys(chunk) - vc_cma_base) %
			VC_CMA_CHUNK_SIZE) != 0);
		if (chunk_num >= vc_cma_chunks) {
			LOG_ERR("%s: ===============================",
				__func__);
			LOG_ERR("%s: chunk phys %x, vc_cma %x-%x - "
				"bad SPARSEMEM configuration?",
				__func__, (unsigned int)page_to_phys(chunk),
				vc_cma_base, vc_cma_base + vc_cma_size - 1);
			LOG_ERR("%s: dev->cma_area = %p\n", __func__,
				vc_cma_device.dev.cma_area);
			LOG_ERR("%s: ===============================",
				__func__);
			break;
		}
		reply->params[i] = chunk_num;
		vc_cma_chunks_used++;
	}

	if (i < num_chunks) {
		LOG_ERR("%s: dma_alloc_from_contiguous failed "
			"for %x bytes (alloc %d of %d, %d free)",
			__func__, VC_CMA_CHUNK_SIZE, i,
			num_chunks, vc_cma_chunks - vc_cma_chunks_used);
		num_chunks = i;
	}

	LOG_DBG("CMA allocated %d chunks -> %d used",
		num_chunks, vc_cma_chunks_used);
	reply->type = VC_CMA_MSG_ALLOCATED;

	{
		VCHIQ_ELEMENT_T elem = {
			reply,
			offsetof(struct cma_msg, params[0]) +
			    num_chunks * sizeof(reply->params[0])
		};
		VCHIQ_STATUS_T ret;
		vchiq_use_service(cma_service);
		ret = vchiq_queue_message(cma_service, &elem, 1);
		vchiq_release_service(cma_service);
		if (ret != VCHIQ_SUCCESS)
			LOG_ERR("vchiq_queue_message return " "%x", ret);
	}

	return num_chunks;
}

static int cma_worker_proc(void *param)
{
	static struct cma_msg reply;
	(void)param;

	while (1) {
		VCHIQ_HEADER_T *msg;
		static struct cma_msg msg_copy;
		struct cma_msg *cma_msg = &msg_copy;
		int type, msg_size;

		msg = vchiu_queue_pop(&cma_msg_queue);
		if ((unsigned int)msg >= VC_CMA_MSG_MAX) {
			msg_size = msg->size;
			memcpy(&msg_copy, msg->data, msg_size);
			type = cma_msg->type;
			vchiq_release_message(cma_service, msg);
		} else {
			msg_size = 0;
			type = (int)msg;
			if (type == VC_CMA_MSG_QUIT)
				break;
			else if (type == VC_CMA_MSG_UPDATE_RESERVE) {
				msg = NULL;
				cma_msg = NULL;
			} else {
				BUG();
				continue;
			}
		}

		switch (type) {
		case VC_CMA_MSG_ALLOC:{
				int num_chunks, free_chunks;
				num_chunks = cma_msg->params[0];
				free_chunks =
				    vc_cma_chunks - vc_cma_chunks_used;
				LOG_DBG("CMA_MSG_ALLOC(%d chunks)", num_chunks);
				if (num_chunks > VC_CMA_MAX_PARAMS_PER_MSG) {
					LOG_ERR
					    ("CMA_MSG_ALLOC - chunk count (%d) "
					     "exceeds VC_CMA_MAX_PARAMS_PER_MSG (%d)",
					     num_chunks,
					     VC_CMA_MAX_PARAMS_PER_MSG);
					num_chunks = VC_CMA_MAX_PARAMS_PER_MSG;
				}

				if (num_chunks > free_chunks) {
					LOG_ERR
					    ("CMA_MSG_ALLOC - chunk count (%d) "
					     "exceeds free chunks (%d)",
					     num_chunks, free_chunks);
					num_chunks = free_chunks;
				}

				vc_cma_alloc_chunks(num_chunks, &reply);
			}
			break;

		case VC_CMA_MSG_FREE:{
				int chunk_count =
				    (msg_size -
				     offsetof(struct cma_msg,
					      params)) /
				    sizeof(cma_msg->params[0]);
				int i;
				BUG_ON(chunk_count <= 0);

				LOG_DBG("CMA_MSG_FREE(%d chunks - %x, ...)",
					chunk_count, cma_msg->params[0]);
				for (i = 0; i < chunk_count; i++) {
					int chunk_num = cma_msg->params[i];
					struct page *page = vc_cma_base_page +
					    chunk_num * PAGES_PER_CHUNK;
					if (chunk_num >= vc_cma_chunks) {
						LOG_ERR
						    ("CMA_MSG_FREE - chunk %d of %d"
						     " (value %x) exceeds maximum "
						     "(%x)", i, chunk_count,
						     chunk_num,
						     vc_cma_chunks - 1);
						break;
					}

					if (!dma_release_from_contiguous
					    (NULL /*&vc_cma_device.dev*/, page,
					     PAGES_PER_CHUNK)) {
						LOG_ERR
						    ("CMA_MSG_FREE - failed to "
						     "release chunk %d (phys %x, "
						     "page %x)", chunk_num,
						     page_to_phys(page),
						     (unsigned int)page);
					}
					vc_cma_chunks_used--;
				}
				LOG_DBG("CMA released %d chunks -> %d used",
					i, vc_cma_chunks_used);
			}
			break;

		case VC_CMA_MSG_UPDATE_RESERVE:{
				int chunks_needed =
				    ((vc_cma_reserve_total + VC_CMA_CHUNK_SIZE -
				      1)
				     / VC_CMA_CHUNK_SIZE) -
				    vc_cma_chunks_reserved;

				LOG_DBG
				    ("CMA_MSG_UPDATE_RESERVE(%d chunks needed)",
				     chunks_needed);

				/* Cap the reservations to what is available */
				if (chunks_needed > 0) {
					if (chunks_needed >
					    (vc_cma_chunks -
					     vc_cma_chunks_used))
						chunks_needed =
						    (vc_cma_chunks -
						     vc_cma_chunks_used);

					chunks_needed =
					    vc_cma_alloc_chunks(chunks_needed,
								&reply);
				}

				LOG_DBG
				    ("CMA_MSG_UPDATE_RESERVE(%d chunks allocated)",
				     chunks_needed);
				vc_cma_chunks_reserved += chunks_needed;
			}
			break;

		default:
			LOG_ERR("unexpected msg type %d", type);
			break;
		}
	}

	LOG_DBG("quitting...");
	return 0;
}

/****************************************************************************
*
*   vc_cma_connected_init
*
*   This function is called once the videocore has been connected.
*
***************************************************************************/

static void vc_cma_connected_init(void)
{
	VCHIQ_SERVICE_PARAMS_T service_params;

	LOG_DBG("vc_cma_connected_init");

	if (!vchiu_queue_init(&cma_msg_queue, 16)) {
		LOG_ERR("could not create CMA msg queue");
		goto fail_queue;
	}

	if (vchiq_initialise(&cma_instance) != VCHIQ_SUCCESS)
		goto fail_vchiq_init;

	vchiq_connect(cma_instance);

	service_params.fourcc = VC_CMA_FOURCC;
	service_params.callback = cma_service_callback;
	service_params.userdata = NULL;
	service_params.version = VC_CMA_VERSION;
	service_params.version_min = VC_CMA_VERSION;

	if (vchiq_open_service(cma_instance, &service_params,
			       &cma_service) != VCHIQ_SUCCESS) {
		LOG_ERR("failed to open service - already in use?");
		goto fail_vchiq_open;
	}

	vchiq_release_service(cma_service);

	cma_worker = kthread_create(cma_worker_proc, NULL, "cma_worker");
	if (!cma_worker) {
		LOG_ERR("could not create CMA worker thread");
		goto fail_worker;
	}
	set_user_nice(cma_worker, -20);
	wake_up_process(cma_worker);

	return;

fail_worker:
	vchiq_close_service(cma_service);
fail_vchiq_open:
	vchiq_shutdown(cma_instance);
fail_vchiq_init:
	vchiu_queue_delete(&cma_msg_queue);
fail_queue:
	return;
}

void
loud_error_header(void)
{
	if (in_loud_error)
		return;

	LOG_ERR("============================================================"
		"================");
	LOG_ERR("============================================================"
		"================");
	LOG_ERR("=====");

	in_loud_error = 1;
}

void
loud_error_footer(void)
{
	if (!in_loud_error)
		return;

	LOG_ERR("=====");
	LOG_ERR("============================================================"
		"================");
	LOG_ERR("============================================================"
		"================");

	in_loud_error = 0;
}

#if 1
static int check_cma_config(void) { return 1; }
#else
static int
read_vc_debug_var(VC_MEM_ACCESS_HANDLE_T handle,
	const char *symbol,
	void *buf, size_t bufsize)
{
	VC_MEM_ADDR_T vcMemAddr;
	size_t vcMemSize;
	uint8_t *mapAddr;
	off_t  vcMapAddr;

	if (!LookupVideoCoreSymbol(handle, symbol,
		&vcMemAddr,
		&vcMemSize)) {
		loud_error_header();
		loud_error(
			"failed to find VC symbol \"%s\".",
			symbol);
		loud_error_footer();
		return 0;
	}

	if (vcMemSize != bufsize) {
		loud_error_header();
		loud_error(
			"VC symbol \"%s\" is the wrong size.",
			symbol);
		loud_error_footer();
		return 0;
	}

	vcMapAddr = (off_t)vcMemAddr & VC_MEM_TO_ARM_ADDR_MASK;
	vcMapAddr += mm_vc_mem_phys_addr;
	mapAddr = ioremap_nocache(vcMapAddr, vcMemSize);
	if (mapAddr == 0) {
		loud_error_header();
		loud_error(
			"failed to ioremap \"%s\" @ 0x%x "
			"(phys: 0x%x, size: %u).",
			symbol,
			(unsigned int)vcMapAddr,
			(unsigned int)vcMemAddr,
			(unsigned int)vcMemSize);
		loud_error_footer();
		return 0;
	}

	memcpy(buf, mapAddr, bufsize);
	iounmap(mapAddr);

	return 1;
}


static int
check_cma_config(void)
{
	VC_MEM_ACCESS_HANDLE_T mem_hndl;
	VC_MEM_ADDR_T mempool_start;
	VC_MEM_ADDR_T mempool_end;
	VC_MEM_ADDR_T mempool_offline_start;
	VC_MEM_ADDR_T mempool_offline_end;
	VC_MEM_ADDR_T cam_alloc_base;
	VC_MEM_ADDR_T cam_alloc_size;
	VC_MEM_ADDR_T cam_alloc_end;
	int success = 0;

	if (OpenVideoCoreMemory(&mem_hndl) != 0)
		goto out;

	/* Read the relevant VideoCore variables */
	if (!read_vc_debug_var(mem_hndl, "__MEMPOOL_START",
		&mempool_start,
		sizeof(mempool_start)))
		goto close;

	if (!read_vc_debug_var(mem_hndl, "__MEMPOOL_END",
		&mempool_end,
		sizeof(mempool_end)))
		goto close;

	if (!read_vc_debug_var(mem_hndl, "__MEMPOOL_OFFLINE_START",
		&mempool_offline_start,
		sizeof(mempool_offline_start)))
		goto close;

	if (!read_vc_debug_var(mem_hndl, "__MEMPOOL_OFFLINE_END",
		&mempool_offline_end,
		sizeof(mempool_offline_end)))
		goto close;

	if (!read_vc_debug_var(mem_hndl, "cam_alloc_base",
		&cam_alloc_base,
		sizeof(cam_alloc_base)))
		goto close;

	if (!read_vc_debug_var(mem_hndl, "cam_alloc_size",
		&cam_alloc_size,
		sizeof(cam_alloc_size)))
		goto close;

	cam_alloc_end = cam_alloc_base + cam_alloc_size;

	success = 1;

	/* Now the sanity checks */
	if (!mempool_offline_start)
		mempool_offline_start = mempool_start;
	if (!mempool_offline_end)
		mempool_offline_end = mempool_end;

	if (VCADDR_TO_PHYSADDR(mempool_offline_start) != vc_cma_base) {
		loud_error_header();
		loud_error(
			"__MEMPOOL_OFFLINE_START(%x -> %lx) doesn't match "
			"vc_cma_base(%x)",
			mempool_offline_start,
			VCADDR_TO_PHYSADDR(mempool_offline_start),
			vc_cma_base);
		success = 0;
	}

	if (VCADDR_TO_PHYSADDR(mempool_offline_end) !=
		(vc_cma_base + vc_cma_size)) {
		loud_error_header();
		loud_error(
			"__MEMPOOL_OFFLINE_END(%x -> %lx) doesn't match "
			"vc_cma_base(%x) + vc_cma_size(%x) = %x",
			mempool_offline_start,
			VCADDR_TO_PHYSADDR(mempool_offline_end),
			vc_cma_base, vc_cma_size, vc_cma_base + vc_cma_size);
		success = 0;
	}

	if (mempool_end < mempool_start) {
		loud_error_header();
		loud_error(
			"__MEMPOOL_END(%x) must not be before "
			"__MEMPOOL_START(%x)",
			mempool_end,
			mempool_start);
		success = 0;
	}

	if (mempool_offline_end < mempool_offline_start) {
		loud_error_header();
		loud_error(
			"__MEMPOOL_OFFLINE_END(%x) must not be before "
			"__MEMPOOL_OFFLINE_START(%x)",
			mempool_offline_end,
			mempool_offline_start);
		success = 0;
	}

	if (mempool_offline_start < mempool_start) {
		loud_error_header();
		loud_error(
			"__MEMPOOL_OFFLINE_START(%x) must not be before "
			"__MEMPOOL_START(%x)",
			mempool_offline_start,
			mempool_start);
		success = 0;
	}

	if (mempool_offline_end > mempool_end) {
		loud_error_header();
		loud_error(
			"__MEMPOOL_OFFLINE_END(%x) must not be after "
			"__MEMPOOL_END(%x)",
			mempool_offline_end,
			mempool_end);
		success = 0;
	}

	if ((cam_alloc_base < mempool_end) &&
		(cam_alloc_end > mempool_start)) {
		loud_error_header();
		loud_error(
			"cam_alloc pool(%x-%x) overlaps "
			"mempool(%x-%x)",
			cam_alloc_base, cam_alloc_end,
			mempool_start, mempool_end);
		success = 0;
	}

	loud_error_footer();

close:
	CloseVideoCoreMemory(mem_hndl);

out:
	return success;
}
#endif

static int vc_cma_init(void)
{
	int rc = -EFAULT;
	struct device *dev;

	if (!check_cma_config())
		goto out_release;

	printk(KERN_INFO "vc-cma: Videocore CMA driver\n");
	printk(KERN_INFO "vc-cma: vc_cma_base      = 0x%08x\n", vc_cma_base);
	printk(KERN_INFO "vc-cma: vc_cma_size      = 0x%08x (%u MiB)\n",
	       vc_cma_size, vc_cma_size / (1024 * 1024));
	printk(KERN_INFO "vc-cma: vc_cma_initial   = 0x%08x (%u MiB)\n",
	       vc_cma_initial, vc_cma_initial / (1024 * 1024));

	vc_cma_base_page = phys_to_page(vc_cma_base);

	if (vc_cma_chunks) {
		int chunks_needed = vc_cma_initial / VC_CMA_CHUNK_SIZE;

		for (vc_cma_chunks_used = 0;
		     vc_cma_chunks_used < chunks_needed; vc_cma_chunks_used++) {
			struct page *chunk;
			chunk = dma_alloc_from_contiguous(NULL /*&vc_cma_device.dev*/,
							  PAGES_PER_CHUNK,
							  VC_CMA_CHUNK_ORDER);
			if (!chunk)
				break;
			BUG_ON(((page_to_phys(chunk) - vc_cma_base) %
				VC_CMA_CHUNK_SIZE) != 0);
		}
		if (vc_cma_chunks_used != chunks_needed) {
			LOG_ERR("%s: dma_alloc_from_contiguous failed (%d "
				"bytes, allocation %d of %d)",
				__func__, VC_CMA_CHUNK_SIZE,
				vc_cma_chunks_used, chunks_needed);
			goto out_release;
		}

		vchiq_add_connected_callback(vc_cma_connected_init);
	}

	rc = alloc_chrdev_region(&vc_cma_devnum, 0, 1, DRIVER_NAME);
	if (rc < 0) {
		LOG_ERR("%s: alloc_chrdev_region failed (rc=%d)", __func__, rc);
		goto out_release;
	}

	cdev_init(&vc_cma_cdev, &vc_cma_fops);
	rc = cdev_add(&vc_cma_cdev, vc_cma_devnum, 1);
	if (rc != 0) {
		LOG_ERR("%s: cdev_add failed (rc=%d)", __func__, rc);
		goto out_unregister;
	}

	vc_cma_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(vc_cma_class)) {
		rc = PTR_ERR(vc_cma_class);
		LOG_ERR("%s: class_create failed (rc=%d)", __func__, rc);
		goto out_cdev_del;
	}

	dev = device_create(vc_cma_class, NULL, vc_cma_devnum, NULL,
			    DRIVER_NAME);
	if (IS_ERR(dev)) {
		rc = PTR_ERR(dev);
		LOG_ERR("%s: device_create failed (rc=%d)", __func__, rc);
		goto out_class_destroy;
	}

	vc_cma_proc_entry = proc_create(DRIVER_NAME, 0444, NULL, &vc_cma_proc_fops);
	if (vc_cma_proc_entry == NULL) {
		rc = -EFAULT;
		LOG_ERR("%s: proc_create failed", __func__);
		goto out_device_destroy;
	}
    
	vc_cma_inited = 1;
	return 0;

out_device_destroy:
	device_destroy(vc_cma_class, vc_cma_devnum);

out_class_destroy:
	class_destroy(vc_cma_class);
	vc_cma_class = NULL;

out_cdev_del:
	cdev_del(&vc_cma_cdev);

out_unregister:
	unregister_chrdev_region(vc_cma_devnum, 1);

out_release:
	/* It is tempting to try to clean up by calling
	   dma_release_from_contiguous for all allocated chunks, but it isn't
	   a very safe thing to do. If vc_cma_initial is non-zero it is because
	   VideoCore is already using that memory, so giving it back to Linux
	   is likely to be fatal.
	 */
	return -1;
}

/****************************************************************************
*
*   vc_cma_exit
*
***************************************************************************/

static void __exit vc_cma_exit(void)
{
	LOG_DBG("%s: called", __func__);

	if (vc_cma_inited) {
		remove_proc_entry(DRIVER_NAME, NULL);
		device_destroy(vc_cma_class, vc_cma_devnum);
		class_destroy(vc_cma_class);
		cdev_del(&vc_cma_cdev);
		unregister_chrdev_region(vc_cma_devnum, 1);
	}
}

module_init(vc_cma_init);
module_exit(vc_cma_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Broadcom Corporation");
