// SPDX-License-Identifier: GPL-2.0
/* XAGR writer from zhaoqing412/android_kernel_oddo_mt6895. */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/xaga_marker.h>

#include <asm/fixmap.h>

#define XAGA_LOG_STORE_PA	0x7ffbf000UL
#define XAGA_MAGIC		0x52474158U
#define XAGA_STAGE_OFF		0x1000U
#define XAGA_RING_OFF		0x2000U
#define XAGA_RING_SZ		0xE000U
#define XAGA_MAX_MSG		256

static void __iomem *xaga_mr_base;
static DEFINE_RAW_SPINLOCK(xaga_ring_lock);

static void xaga_marker_ring_write(const char *buf, int len)
{
	void __iomem *ring = xaga_mr_base + XAGA_RING_OFF;
	u32 cursor;
	int i;

	writel(XAGA_MAGIC, xaga_mr_base);
	cursor = readl(xaga_mr_base + 0x0004);
	for (i = 0; i < len; i++)
		writeb(buf[i], ring + ((cursor + i) % XAGA_RING_SZ));
	writel(cursor + len, xaga_mr_base + 0x0004);
	writel(readl(xaga_mr_base + 0x0008) + len, xaga_mr_base + 0x0008);
}

static void xaga_marker_vwrite(const char *fmt, va_list args, bool mirror)
{
	static unsigned int mirror_count;
	unsigned long flags;
	char buf[XAGA_MAX_MSG];
	int len;

	/* Drop the record if a writer already holds the lock. */
	if (!READ_ONCE(xaga_mr_base) ||
	    !raw_spin_trylock_irqsave(&xaga_ring_lock, flags))
		return;

	len = vscnprintf(buf, sizeof(buf), fmt, args);
	if (len > 0) {
		xaga_marker_ring_write(buf, len);
		if (mirror && ++mirror_count % 64 == 0) {
			len = scnprintf(buf, sizeof(buf), "MIRROR:%u\n", mirror_count);
			xaga_marker_ring_write(buf, len);
		}
	}
	raw_spin_unlock_irqrestore(&xaga_ring_lock, flags);
}

void xaga_marker_put(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	xaga_marker_vwrite(fmt, args, false);
	va_end(args);
}
EXPORT_SYMBOL_GPL(xaga_marker_put);

void xaga_marker_stage(u32 stage)
{
	unsigned long flags;
	char buf[32];
	int len;

	if (!READ_ONCE(xaga_mr_base) ||
	    !raw_spin_trylock_irqsave(&xaga_ring_lock, flags))
		return;

	writel(stage, xaga_mr_base + XAGA_STAGE_OFF);
	len = scnprintf(buf, sizeof(buf), "stage=%u\n", stage);
	xaga_marker_ring_write(buf, len);
	raw_spin_unlock_irqrestore(&xaga_ring_lock, flags);
}
EXPORT_SYMBOL_GPL(xaga_marker_stage);

void xaga_marker_early_printk(const char *fmt, va_list args)
{
	va_list copy;

	va_copy(copy, args);
	xaga_marker_vwrite(fmt, copy, true);
	va_end(copy);
}

void __init xaga_marker_early_init(void)
{
	void __iomem *base = (void __iomem *)fix_to_virt(FIX_XAGA_MARKER);
	unsigned int i;

	for (i = 0; i < SZ_64K / PAGE_SIZE; i++)
		set_fixmap_io(FIX_XAGA_MARKER - i, XAGA_LOG_STORE_PA + i * PAGE_SIZE);

	writel(XAGA_MAGIC, base);
	writel(0, base + 0x0004);
	writel(0, base + 0x0008);
	writel(0, base + XAGA_STAGE_OFF);
	WRITE_ONCE(xaga_mr_base, base);

	pr_info("xaga-marker-writer: XAGR ring armed at 0x%08lx\n", XAGA_LOG_STORE_PA);
	xaga_marker_stage(1);
}

#ifdef CONFIG_MODULES
static int xaga_marker_module_event(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	struct module *mod = data;

	if (action == MODULE_STATE_COMING || action == MODULE_STATE_LIVE)
		xaga_marker_put("module: %s\n", mod->name);

	return NOTIFY_OK;
}

static struct notifier_block xaga_marker_nb = {
	.notifier_call = xaga_marker_module_event,
};
#endif

static int __init xaga_marker_init(void)
{
	xaga_marker_put("marker writer built-in init\n");
#ifdef CONFIG_MODULES
	return register_module_notifier(&xaga_marker_nb);
#else
	return 0;
#endif
}
core_initcall(xaga_marker_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Xaga boot-stage marker writer");
