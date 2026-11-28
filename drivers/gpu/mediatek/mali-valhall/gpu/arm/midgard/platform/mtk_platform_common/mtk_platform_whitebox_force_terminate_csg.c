// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 MediaTek Inc.
 */

#include <mali_kbase.h>
#include <mali_kbase_defs.h>

bool force_terminate_csg_enable;
bool force_terminate_csg_trigger_resource;

enum trigger_resource {
	TRIGGER_RESOURCE_NONE = 0,
	TRIGGER_RESOURCE_ADB_COMMAND = 1,
	TRIGGER_RESOURCE_FLOW = 2,
};

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int mtk_whitebox_force_terminate_csg_enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "force_terminate_csg_enable = %d\n", force_terminate_csg_enable);

	return 0;
}

static int mtk_whitebox_force_terminate_csg_enable_open(struct inode *in, struct file *file)
{
	struct kbase_device *kbdev = in->i_private;
	file->private_data = kbdev;

	if (file->f_mode & FMODE_WRITE)
		return 0;

	return single_open(file, mtk_whitebox_force_terminate_csg_enable_show, in->i_private);
}

static int mtk_whitebox_force_terminate_csg_enable_release(struct inode *in, struct file *file)
{
	if (!(file->f_mode & FMODE_WRITE)) {
		struct seq_file *m = (struct seq_file *)file->private_data;

		if (m)
			seq_release(in, file);
	}

	return 0;
}

static ssize_t mtk_whitebox_force_terminate_csg_enable_write(struct file *file, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	struct kbase_device *kbdev = (struct kbase_device *)file->private_data;
	int ret = 0;
    int temp = 0;
    CSTD_UNUSED(ppos);

	ret = kstrtoint_from_user(ubuf, count, 0, &temp);
	if (ret)
		return ret;

	if (temp == 1) {
		force_terminate_csg_enable = true;
		force_terminate_csg_trigger_resource = TRIGGER_RESOURCE_ADB_COMMAND;
	} else {
		force_terminate_csg_enable = false;
		force_terminate_csg_trigger_resource = TRIGGER_RESOURCE_NONE;
	}

#if IS_ENABLED(CONFIG_MALI_MTK_LOG_BUFFER)
	mtk_logbuffer_type_print(kbdev, MTK_LOGBUFFER_TYPE_CRITICAL | MTK_LOGBUFFER_TYPE_EXCEPTION,
		"[WHITEBOX] force_terminate_csg_enable = %d\n", force_terminate_csg_enable);
#endif /* CONFIG_MALI_MTK_LOG_BUFFER */
	dev_info(kbdev->dev, "[WHITEBOX] force_terminate_csg_enable = %d", force_terminate_csg_enable);

	return count;
}

static const struct file_operations mtk_whitebox_force_terminate_csg_enable_fops = {
	.open    = mtk_whitebox_force_terminate_csg_enable_open,
	.release = mtk_whitebox_force_terminate_csg_enable_release,
	.read    = seq_read,
	.write   = mtk_whitebox_force_terminate_csg_enable_write,
	.llseek  = seq_lseek
};

int mtk_whitebox_force_terminate_csg_debugfs_init(struct kbase_device *kbdev)
{
	if (IS_ERR_OR_NULL(kbdev))
		return -1;

	debugfs_create_file("force_terminate_csg_enable", 0444,
			kbdev->mali_debugfs_directory, kbdev,
			&mtk_whitebox_force_terminate_csg_enable_fops);
	return 0;
}
#else /* CONFIG_DEBUG_FS */
int mtk_whitebox_force_terminate_csg_debugfs_init(struct kbase_device *kbdev)
{
	return 0;
}
#endif /* CONFIG_DEBUG_FS */


static u32 kbase_reg_read(struct kbase_device *kbdev, u32 offset)
{
	u32 val;

	if (WARN_ON(!kbase_io_is_gpu_powered(kbdev)))
		return 0;

	if (WARN_ON(kbdev->dev == NULL))
		return 0;

	val = readl(kbdev->reg + offset);

#if IS_ENABLED(CONFIG_DEBUG_FS)
	if (unlikely(kbdev->io_history.enabled))
		kbase_io_history_add(&kbdev->io_history, kbdev->reg + offset,
				     val, 0);
#endif /* CONFIG_DEBUG_FS */
	dev_dbg(kbdev->dev, "r: reg %08x val %08x", offset, val);

	return val;
}

#define CSHW_BASE 0x0030000
#define CSHW_CSHWIF_0 0x4000 /* () CSHWIF 0 registers */
#define CSHWIF(n) (CSHW_BASE + CSHW_CSHWIF_0 + (n)*256)
#define CSHWIF_REG(n, r) (CSHWIF(n) + r)
#define NR_HW_INTERFACES 4

#define CSHW_CTRL_REG(r)    (CSHW_BASE + 0x0000 + r)
#define CSHW_IT_COMP_REG(r) (CSHW_BASE + 0x1000 + r)
#define CSHW_IT_FRAG_REG(r) (CSHW_BASE + 0x2000 + r)
#define CSHW_IT_TILER_REG(r)(CSHW_BASE + 0x3000 + r)

bool mtk_check_if_need_force_terminate_csg(struct kbase_device *kbdev)
{
	unsigned long flags;
	u32 irq_raw = 0;
	u32 ep_evt_status = 0;
	u32 queue_count = 0;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	if (kbase_io_is_gpu_powered(kbdev)) {
		irq_raw = kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0xD0));
		ep_evt_status = kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0xA4));
		queue_count = kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0x20));


		dev_info(kbdev->dev, "[Force Terminate Check] Tiler    CTRL: %x STATUS: %x JASID: %u IRQ_RAW: %8x IRQ_STATUS: %8x EP_EVT_STATUS: %x BLOCKED_SB_ENTRY: %8x FAULT_STATUS %x QUEUE_COUNT %x",
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0x0)),
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0x4)),
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0x8)),
			irq_raw,
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0xDC)),
			ep_evt_status,
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0xA0)),
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0xE0)),
			queue_count);

#if IS_ENABLED(CONFIG_MALI_MTK_LOG_BUFFER)
		mtk_logbuffer_type_print(kbdev, MTK_LOGBUFFER_TYPE_CRITICAL | MTK_LOGBUFFER_TYPE_EXCEPTION,
			"[Force Terminate Check] Tiler    CTRL: %x STATUS: %x JASID: %u IRQ_RAW: %8x IRQ_STATUS: %8x EP_EVT_STATUS: %x BLOCKED_SB_ENTRY: %8x FAULT_STATUS %x QUEUE_COUNT %x\n",
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0x0)),
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0x4)),
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0x8)),
			irq_raw,
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0xDC)),
			ep_evt_status,
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0xA0)),
			kbase_reg_read(kbdev, CSHW_IT_TILER_REG(0xE0)),
			queue_count);
#endif /* CONFIG_MALI_MTK_LOG_BUFFER */
	}
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	/* Check OOM event missing scenario */
	if ((irq_raw & 0x40) && (ep_evt_status & 0x1) && (queue_count & 0x200))
		return true;

	return false;
}

void mtk_whitebox_force_terminate_csg_enable_set(u32 enable)
{
	if (enable == 1) {
		force_terminate_csg_enable = true;
		force_terminate_csg_trigger_resource = TRIGGER_RESOURCE_FLOW;
	} else {
		force_terminate_csg_enable = false;
		force_terminate_csg_trigger_resource = TRIGGER_RESOURCE_NONE;
	}
}

bool mtk_whitebox_force_terminate_csg_enable_get(void)
{
	pr_info("force_terminate_csg_enable = %d, trigger_resource = %d",
			force_terminate_csg_enable, force_terminate_csg_trigger_resource);

	return force_terminate_csg_enable;
}

int mtk_whitebox_force_terminate_csg_init(void)
{
    force_terminate_csg_enable = false;
	force_terminate_csg_trigger_resource = TRIGGER_RESOURCE_NONE;
	return 0;
}
