// SPDX-License-Identifier: GPL-2.0-only
/*
 * Loongson IOMMU Driver
 *
 * Copyright (C) 2020-2021 Loongson Technology Ltd.
 * Author:	Lv Chen <lvchen@loongson.cn>
 *		Wang Yang <wangyang@loongson.cn>
 */

#ifndef LOONGSON_IOMMU_H
#define LOONGSON_IOMMU_H

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <asm/addrspace.h>

#define IOVA_WIDTH		47

/* Bit value definition for I/O PTE fields */
#define IOMMU_PTE_PR		(1ULL << 0)	/* Present */
#define IOMMU_PTE_HP		(1ULL << 1)	/* HugePage */
#define IOMMU_PTE_IR		(1ULL << 2)	/* Readable */
#define IOMMU_PTE_IW		(1ULL << 3)	/* Writeable */
#define IOMMU_PTE_RW		(IOMMU_PTE_PR | IOMMU_PTE_IR | IOMMU_PTE_IW)

#define IOMMU_PTE_PRESENT(pte)	((pte) & IOMMU_PTE_PR)
#define IOMMU_PTE_HUGEPAGE(pte)	((pte) & IOMMU_PTE_HP)

#define iommu_pte_present(ptep)	((*ptep != 0))
#define iommu_pte_huge(ptep)	((*ptep) & IOMMU_PTE_HP)

#define LA_IOMMU_PGSIZE		(SZ_16K | SZ_32M)

#define IOMMU_PT_LEVEL0		0x00
#define IOMMU_PT_LEVEL1		0x01

/* IOMMU page table */
#define IOMMU_PAGE_SHIFT	PAGE_SHIFT
#define IOMMU_PAGE_SIZE		(_AC(1, UL) << IOMMU_PAGE_SHIFT)
#define IOMMU_LEVEL_STRIDE	(IOMMU_PAGE_SHIFT - 3)
#define IOMMU_PTRS_PER_LEVEL	(IOMMU_PAGE_SIZE >> 3)
#define IOMMU_LEVEL_SHIFT(n)	(((n) * IOMMU_LEVEL_STRIDE) + IOMMU_PAGE_SHIFT)
#define IOMMU_LEVEL_SIZE(n)	(_AC(1, UL) << (((n) * IOMMU_LEVEL_STRIDE) + IOMMU_PAGE_SHIFT))
#define IOMMU_LEVEL_MASK(n)	(~(IOMMU_LEVEL_SIZE(n) - 1))
#define IOMMU_LEVEL_MAX	        DIV_ROUND_UP((IOVA_WIDTH - IOMMU_PAGE_SHIFT), IOMMU_LEVEL_STRIDE)
#define IOMMU_PAGE_MASK		(~(IOMMU_PAGE_SIZE - 1))

#define IOMMU_HPAGE_SIZE	(1UL << IOMMU_LEVEL_SHIFT(IOMMU_PT_LEVEL1))
#define IOMMU_HPAGE_MASK	(~(IOMMU_HPAGE_SIZE - 1))

/* wired | index | domain | shift */
#define LA_IOMMU_WIDS			0x10
/* valid | busy | tlbar/aw | cmd */
#define LA_IOMMU_VBTC			0x14
#define IOMMU_PGTABLE_BUSY		(1 << 16)
/* enable |index | valid | domain | bdf */
#define LA_IOMMU_EIVDB			0x18
/* enable | valid | cmd */
#define LA_IOMMU_CMD			0x1C
#define LA_IOMMU_PGD0_LO		0x20
#define LA_IOMMU_PGD0_HI		0x24
#define STEP_PGD			0x8
#define STEP_PGD_SHIFT			3
#define LA_IOMMU_PGD_LO(domain_id)	\
		(LA_IOMMU_PGD0_LO + ((domain_id) << STEP_PGD_SHIFT))
#define LA_IOMMU_PGD_HI(domain_id)	\
		(LA_IOMMU_PGD0_HI + ((domain_id) << STEP_PGD_SHIFT))

#define LA_IOMMU_DIR_CTRL0		0xA0
#define LA_IOMMU_DIR_CTRL1		0xA4
#define LA_IOMMU_DIR_CTRL(x)		(LA_IOMMU_DIR_CTRL0 + ((x) << 2))

#define LA_IOMMU_SAFE_BASE_HI		0xE0
#define LA_IOMMU_SAFE_BASE_LO		0xE4
#define LA_IOMMU_EX_ADDR_LO		0xE8
#define LA_IOMMU_EX_ADDR_HI		0xEC

#define LA_IOMMU_PFM_CNT_EN		0x100

#define LA_IOMMU_RD_HIT_CNT_0		0x110
#define LA_IOMMU_RD_MISS_CNT_O		0x114
#define LA_IOMMU_WR_HIT_CNT_0		0x118
#define LA_IOMMU_WR_MISS_CNT_0		0x11C
#define LA_IOMMU_RD_HIT_CNT_1		0x120
#define LA_IOMMU_RD_MISS_CNT_1		0x124
#define LA_IOMMU_WR_HIT_CNT_1		0x128
#define LA_IOMMU_WR_MISS_CNT_1		0x12C
#define LA_IOMMU_RD_HIT_CNT_2		0x130
#define LA_IOMMU_RD_MISS_CNT_2		0x134
#define LA_IOMMU_WR_HIT_CNT_2		0x138
#define LA_IOMMU_WR_MISS_CNT_2		0x13C

#define MAX_DOMAIN_ID			16
#define MAX_ATTACHED_DEV_ID		16
#define MAX_PAGES_NUM			(SZ_128M / IOMMU_PAGE_SIZE)

#define iommu_ptable_end(addr, end, level)								\
({	unsigned long __boundary = ((addr) + IOMMU_LEVEL_SIZE(level)) & IOMMU_LEVEL_MASK(level);	\
	(__boundary - 1 < (end) - 1) ? __boundary : (end);						\
})

/* To find an entry in an iommu page table directory */
#define iommu_page_index(addr, level)		\
		(((addr) >> ((level * IOMMU_LEVEL_STRIDE) + IOMMU_PAGE_SHIFT)) & (IOMMU_PTRS_PER_LEVEL - 1))

/* IOMMU iommu_table entry */
typedef struct { unsigned long iommu_pte; } iommu_pte;

typedef struct loongson_iommu {
	/* For loongson_iommu_list */
	struct list_head	list;
	/* Lock for domain allocing */
	spinlock_t		domain_bitmap_lock;
	/* Lock for dom_list */
	spinlock_t		dom_info_lock;
	/* Bitmap of global domains */
	void			*domain_bitmap;
	/* Bitmap of devtable */
	void			*devtable_bitmap;
	/* List of all domain privates */
	struct list_head	dom_list;
	/* PCI device id of the IOMMU device */
	u16			devid;
	/* PCI segment# */
	int			segment;
	/* iommu configures the register space base address */
	void			*confbase;
	/* iommu configures the register space physical base address */
	resource_size_t		confbase_phy;
	/* iommu configures the register space size */
	resource_size_t		conf_size;
	struct pci_dev		*pdev;
} loongson_iommu;

struct iommu_rlookup_entry
{
	struct list_head		list;
	struct loongson_iommu		**rlookup_table;
	int				pcisegment;
};

typedef struct iommu_info {
	struct list_head	list;		/* for loongson_iommu_pri->iommu_devlist */
	struct loongson_iommu	*iommu;
	spinlock_t		devlock;	/* priv dev list lock */
	struct list_head	dev_list;	/* List of all devices in this domain iommu */
	unsigned int		dev_cnt;	/* devices assigned to this domain iommu */
	short			id;
} iommu_info;

/* One vm is equal to a domain, one domain has a priv */
typedef struct dom_info {
	struct list_head	iommu_devlist;
	struct iommu_domain	domain;
	struct mutex		ptl_lock;       /* Lock for page table */
	void			*pgd;
	spinlock_t		lock;	/* Lock for dom_info->iommu_devlist */
} dom_info;

struct dom_entry {
	struct list_head	list;	/* for loongson_iommu->dom_list */
	dom_info                *domain_info;
};

/* A device for passthrough */
struct loongson_iommu_dev_data {
	struct list_head	list;		/* for iommu_entry->dev_list */
	struct loongson_iommu	*iommu;
	iommu_info		*iommu_entry;
	unsigned short		bdf;
	int			count;
	int			index;		/* index in device table */
};

static inline unsigned long *iommu_pte_offset(unsigned long *ptep,
						unsigned long addr, int level)
{
	return ptep + iommu_page_index(addr, level);
}

#endif	/* LOONGSON_IOMMU_H */
