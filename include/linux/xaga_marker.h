/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_XAGA_MARKER_H
#define _LINUX_XAGA_MARKER_H

#include <stdarg.h>
#include <linux/compiler_attributes.h>
#include <linux/types.h>

#ifdef CONFIG_XAGA_MARKER_WRITER
void xaga_marker_early_init(void);
void xaga_marker_stage(u32 stage);
__printf(1, 2) void xaga_marker_put(const char *fmt, ...);
__printf(1, 0) void xaga_marker_early_printk(const char *fmt, va_list args);
#else
static inline void xaga_marker_early_init(void) { }
static inline void xaga_marker_stage(u32 stage) { }
static inline void xaga_marker_early_printk(const char *fmt, va_list args) { }
#endif

#endif /* _LINUX_XAGA_MARKER_H */
