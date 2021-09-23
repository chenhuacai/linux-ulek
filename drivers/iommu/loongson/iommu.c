// SPDX-License-Identifier: GPL-2.0-only
/*
 * Loongson IOMMU Driver
 *
 * Copyright (C) 2020-2021 Loongson Technology Ltd.
 * Author:	Lv Chen <lvchen@loongson.cn>
 *		Wang Yang <wangyang@loongson.cn>
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/printk.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "iommu.h"

#define LOOP_TIMEOUT		100000
#define IOVA_START		(SZ_256M)
#define IOVA_END0		(SZ_2G + SZ_256M)

#define IVRS_HEADER_LENGTH		48
#define ACPI_IVHD_TYPE_MAX_SUPPORTED	0x40
#define IVHD_DEV_ALL                    0x01
#define IVHD_DEV_SELECT                 0x02
#define IVHD_DEV_SELECT_RANGE_START     0x03
#define IVHD_DEV_RANGE_END              0x04
#define IVHD_DEV_ALIAS                  0x42
#define IVHD_DEV_EXT_SELECT             0x46
#define IVHD_DEV_ACPI_HID		0xf0

#define IVHD_HEAD_TYPE10		0x10
#define IVHD_HEAD_TYPE11		0x11
#define IVHD_HEAD_TYPE40		0x40

#define MAX_BDF_NUM			0xffff

#define RLOOKUP_TABLE_ENTRY_SIZE	(sizeof(void *))

/*
 * structure describing one IOMMU in the ACPI table. Typically followed by one
 * or more ivhd_entrys.
 */
struct ivhd_header {
	u8 type;
	u8 flags;
	u16 length;
	u16 devid;
	u16 cap_ptr;
	u64 mmio_phys;
	u16 pci_seg;
	u16 info;
	u32 efr_attr;

	/* Following only valid on IVHD type 11h and 40h */
	u64 efr_reg; /* Exact copy of MMIO_EXT_FEATURES */
	u64 res;
} __attribute__((packed));

/*
 * A device entry describing which devices a specific IOMMU translates and
 * which requestor ids they use.
 */
struct ivhd_entry {
	u8 type;
	u16 devid;
	u8 flags;
	u32 ext;
	u32 hidh;
	u64 cid;
	u8 uidf;
	u8 uidl;
	u8 uid;
} __attribute__((packed));

LIST_HEAD(loongson_iommu_list);			/* list of all IOMMUs in the system */
LIST_HEAD(loongson_rlookup_iommu_list);

static u32 rlookup_table_size;			/* size if the rlookup table */
static int loongson_iommu_target_ivhd_type;
u16	loongson_iommu_last_bdf;		/* largest PCI device id we have to handle */

int loongson_iommu_disable;
static struct iommu_ops loongson_iommu_ops;

#define iommu_read_regl(iommu, off)	  readl(iommu->confbase + off)
#define iommu_write_regl(iommu, off, val) writel(val, iommu->confbase + off)

static void switch_huge_to_page(unsigned long *ptep, unsigned long start);

static void iommu_translate_disable(loongson_iommu *iommu)
{
	u32 val;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	/* Disable */
	val = iommu_read_regl(iommu, LA_IOMMU_PFM_CNT_EN);
	val &= ~(1 << 31);
	iommu_write_regl(iommu, LA_IOMMU_PFM_CNT_EN, val);

	/* Write cmd */
	val = iommu_read_regl(iommu, LA_IOMMU_CMD);
	val &= 0xfffffffc;
	iommu_write_regl(iommu, LA_IOMMU_CMD, val);
}

static void iommu_translate_enable(loongson_iommu *iommu)
{
	u32 val = 0;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	/* Enable use mem */
	val = iommu_read_regl(iommu, LA_IOMMU_PFM_CNT_EN);
	val |= (1 << 29);
	iommu_write_regl(iommu, LA_IOMMU_PFM_CNT_EN, val);

	/* Enable */
	val = iommu_read_regl(iommu, LA_IOMMU_PFM_CNT_EN);
	val |= (1 << 31);
	iommu_write_regl(iommu, LA_IOMMU_PFM_CNT_EN, val);

	/* Write cmd */
	val = iommu_read_regl(iommu, LA_IOMMU_CMD);
	val &= 0xfffffffc;
	iommu_write_regl(iommu, LA_IOMMU_CMD, val);
}

static bool loongson_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	default:
		return false;
	}
}

static dom_info *to_dom_info(struct iommu_domain *dom)
{
	return container_of(dom, dom_info, domain);
}

static void flush_iotlb_by_domain_id(struct loongson_iommu *iommu, u16 domain_id, bool read)
{
	u32 val;
	u32 flush_read_tlb = read ? 1 : 0;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	val = iommu_read_regl(iommu, LA_IOMMU_EIVDB);
	val &= ~0xf0000;
	val |= ((u32)domain_id) << 16;
	iommu_write_regl(iommu, LA_IOMMU_EIVDB, val);

	/* Flush all  */
	val = iommu_read_regl(iommu, LA_IOMMU_VBTC);
	val &= ~0x10f;
	val |= (flush_read_tlb << 8) | 4;
	iommu_write_regl(iommu, LA_IOMMU_VBTC, val);
}

static int flush_pgtable_is_busy(struct loongson_iommu *iommu)
{
	u32 val;

	val = iommu_read_regl(iommu, LA_IOMMU_VBTC);

	return val & IOMMU_PGTABLE_BUSY;
}

static int iommu_flush_iotlb_by_domain(struct loongson_iommu_dev_data *dev_data)
{
	u16 domain_id;
	u32 retry = 0;
	struct loongson_iommu *iommu;

	if (dev_data == NULL) {
		pr_err("%s dev_data is NULL", __func__);
		return 0;
	}

	if (dev_data->iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return 0;
	}

	if (dev_data->iommu_entry == NULL) {
		pr_err("%s iommu_entry is NULL", __func__);
		return 0;
	}

	iommu = dev_data->iommu;
	domain_id = dev_data->iommu_entry->id;

	flush_iotlb_by_domain_id(iommu, domain_id, 0);
	while (flush_pgtable_is_busy(iommu)) {
		if (retry == LOOP_TIMEOUT) {
			pr_err("LA-IOMMU: %s %d iotlb flush busy\n",
					__func__, __LINE__);
			return -EIO;
		}
		retry++;
		udelay(1);
	}

	flush_iotlb_by_domain_id(iommu, domain_id, 1);
	while (flush_pgtable_is_busy(iommu)) {
		if (retry == LOOP_TIMEOUT) {
			pr_err("LA-IOMMU: %s %d iotlb flush busy\n",
					__func__, __LINE__);
			return -EIO;
		}
		retry++;
		udelay(1);
	}
	iommu_translate_enable(iommu);

	return 0;
}

static int update_dev_table(struct loongson_iommu_dev_data *dev_data, int flag)
{
	u32 val = 0;
	int index;
	unsigned short bdf;
	loongson_iommu *iommu;
	u16 domain_id;

	if (dev_data == NULL) {
		pr_err("%s dev_data is NULL", __func__);
		return 0;
	}

	if (dev_data->iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return 0;
	}

	if (dev_data->iommu_entry == NULL) {
		pr_err("%s iommu_entry is NULL", __func__);
		return 0;
	}

	iommu = dev_data->iommu;
	domain_id = dev_data->iommu_entry->id;
	bdf = dev_data->bdf;

	/* Set device table */
	if (flag) {
		index = find_first_zero_bit(iommu->devtable_bitmap, MAX_ATTACHED_DEV_ID);
		if (index < MAX_ATTACHED_DEV_ID) {
			__set_bit(index, iommu->devtable_bitmap);
			dev_data->index = index;
		} else {
			pr_err("%s get id from dev table failed\n", __func__);
			return 0;
		}

		pr_info("%s bdf %x domain_id %d iommu devid %x"
				" iommu segment %d flag %x\n",
				__func__, bdf, domain_id, iommu->devid,
				iommu->segment, flag);

		val = bdf & 0xffff;
		val |= ((domain_id & 0xf) << 16);	/* domain id */
		val |= ((index & 0xf) << 24);		/* index */
		val |= (0x1 << 20);			/* valid */
		iommu_write_regl(iommu, LA_IOMMU_EIVDB, val);

		val = (0x1 << 31) | (0xf << 0);
		val |= (0x1 << 29);			/* 1: use main memory */
		iommu_write_regl(iommu, LA_IOMMU_PFM_CNT_EN, val);

		val = iommu_read_regl(iommu, LA_IOMMU_CMD);
		val &= 0xfffffffc;
		iommu_write_regl(iommu, LA_IOMMU_CMD, val);
	} else {
		/* Flush device table */
		index = dev_data->index;
		pr_info("%s bdf %x domain_id %d iommu devid %x"
				" iommu segment %d flag %x\n",
				__func__, bdf, domain_id, iommu->devid,
				iommu->segment, flag);

		val = iommu_read_regl(iommu, LA_IOMMU_EIVDB);
		val &= ~(0xffffffff);
		val |= ((index & 0xf) << 24);	/* index */
		iommu_write_regl(iommu, LA_IOMMU_EIVDB, val);

		val = iommu_read_regl(iommu, LA_IOMMU_PFM_CNT_EN);
		val |= (0x1 << 29);			/* 1: use main memory */
		iommu_write_regl(iommu, LA_IOMMU_PFM_CNT_EN, val);

		if (index < MAX_ATTACHED_DEV_ID)
			__clear_bit(index, iommu->devtable_bitmap);
	}

	iommu_flush_iotlb_by_domain(dev_data);

	return 0;
}

static void flush_iotlb(loongson_iommu *iommu)
{
	u32 val;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	/* Flush all tlb */
	val = iommu_read_regl(iommu, LA_IOMMU_VBTC);
	val &= ~0x1f;
	val |= 0x5;
	iommu_write_regl(iommu, LA_IOMMU_VBTC, val);
}

static int iommu_flush_iotlb(loongson_iommu *iommu)
{
	u32 retry = 0;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return 0;
	}

	flush_iotlb(iommu);
	while (flush_pgtable_is_busy(iommu)) {
		if (retry == LOOP_TIMEOUT) {
			pr_err("Loongson-IOMMU: iotlb flush busy\n");
			return -EIO;
		}
		retry++;
		udelay(1);
	}
	iommu_translate_enable(iommu);

	return 0;
}

static void loongson_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	dom_info *priv = to_dom_info(domain);
	iommu_info *info;

	spin_lock(&priv->lock);
	list_for_each_entry(info, &priv->iommu_devlist, list) {
		iommu_flush_iotlb(info->iommu);
	}
	spin_unlock(&priv->lock);
}

static void do_attach(iommu_info *info, struct loongson_iommu_dev_data *dev_data)
{
	if (dev_data->count)
		return;

	dev_data->count++;
	dev_data->iommu_entry = info;

	spin_lock(&info->devlock);
	list_add(&dev_data->list, &info->dev_list);
	info->dev_cnt += 1;
	spin_unlock(&info->devlock);

	update_dev_table(dev_data, 1);
}

static void do_detach(struct loongson_iommu_dev_data *dev_data)
{
	iommu_info *info;

	if (!dev_data || !dev_data->iommu_entry || (dev_data->count == 0)) {
		pr_err("%s dev_data or iommu_entry is NULL", __func__);
		return;
	}
	dev_data->count--;
	info = dev_data->iommu_entry;
	list_del(&dev_data->list);
	info->dev_cnt -= 1;
	update_dev_table(dev_data, 0);
	dev_data->iommu_entry = NULL;
}

static void detach_all_dev_by_domain(iommu_info *info)
{
	struct loongson_iommu_dev_data *dev_data = NULL;

	spin_lock(&info->devlock);
	while (!list_empty(&info->dev_list)) {
		dev_data = list_first_entry(&info->dev_list,
				struct loongson_iommu_dev_data, list);
		do_detach(dev_data);
	}

	spin_unlock(&info->devlock);
}

static int domain_id_alloc(loongson_iommu *iommu)
{
	int id = -1;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return id;
	}

	spin_lock(&iommu->domain_bitmap_lock);
	id = find_first_zero_bit(iommu->domain_bitmap, MAX_DOMAIN_ID);
	if (id < MAX_DOMAIN_ID)
		__set_bit(id, iommu->domain_bitmap);
	spin_unlock(&iommu->domain_bitmap_lock);

	if (id >= MAX_DOMAIN_ID)
		pr_err("Loongson-IOMMU: Alloc domain id over max domain id\n");

	return id;
}

static void domain_id_free(loongson_iommu *iommu, int id)
{
	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	if ((id >= 0) && (id < MAX_DOMAIN_ID)) {
		spin_lock(&iommu->domain_bitmap_lock);
		__clear_bit(id, iommu->domain_bitmap);
		spin_unlock(&iommu->domain_bitmap_lock);
	}
}

/*
 * Check whether the system has a priv.
 * If yes, it returns 1 and if not, it returns 0
 */
static int has_dom(loongson_iommu *iommu)
{
	int ret = 0;

	spin_lock(&iommu->dom_info_lock);
	while (!list_empty(&iommu->dom_list)) {
		ret = 1;
		break;
	}
	spin_unlock(&iommu->dom_info_lock);

	return ret;
}

/*
 *  This function adds a private domain to the global domain list
 */
static struct dom_entry *find_domain_in_list(loongson_iommu *iommu, dom_info *priv)
{
	struct dom_entry *entry, *found = NULL;

	if (priv == NULL)
		return found;

	spin_lock(&iommu->dom_info_lock);
	list_for_each_entry(entry, &iommu->dom_list, list) {
		if (entry->domain_info == priv) {
			found = entry;
			break;
		}
	}
	spin_unlock(&iommu->dom_info_lock);

	return found;
}

static void add_domain_to_list(loongson_iommu *iommu, dom_info *priv)
{
	struct dom_entry *entry;

	if (priv == NULL)
		return;
	entry = find_domain_in_list(iommu, priv);
	if (entry != NULL)
		return;
	entry = kzalloc(sizeof(struct dom_entry), GFP_KERNEL);
	entry->domain_info = priv;
	spin_lock(&iommu->dom_info_lock);
	list_add(&entry->list, &iommu->dom_list);
	spin_unlock(&iommu->dom_info_lock);
}

static void del_domain_from_list(loongson_iommu *iommu, dom_info *priv)
{
	struct dom_entry *entry;

	entry = find_domain_in_list(iommu, priv);
	if (entry == NULL)
		return;
	spin_lock(&iommu->dom_info_lock);
	list_del(&entry->list);
	spin_unlock(&iommu->dom_info_lock);
}

static void free_pagetable(void *pt_base, int level)
{
	int i;
	unsigned long *ptep, *pgtable;

	ptep = pt_base;
	if (level == IOMMU_PT_LEVEL1) {
		kfree(pt_base);
		return;
	}
	for (i = 0; i < IOMMU_PTRS_PER_LEVEL; i++, ptep++) {
		if (!iommu_pte_present(ptep))
			continue;

		if (((level - 1) == IOMMU_PT_LEVEL1) && iommu_pte_huge(ptep)) {
			*ptep = 0;
			continue;
		}

		pgtable = phys_to_virt(*ptep & IOMMU_PAGE_MASK);
		free_pagetable(pgtable, level - 1);
	}
	kfree(pt_base);
}

static void iommu_free_pagetable(dom_info *info)
{
	free_pagetable(info->pgd, IOMMU_LEVEL_MAX);
	info->pgd = NULL;
}

static dom_info *dom_info_alloc(void)
{
	dom_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (info == NULL) {
		pr_err("%s alloc memory for info failed\n", __func__);
		return NULL;
	}

	info->pgd = kzalloc(IOMMU_PAGE_SIZE, GFP_KERNEL_ACCOUNT);
	if (info->pgd == NULL) {
		kfree(info);
		return NULL;
	}

	INIT_LIST_HEAD(&info->iommu_devlist);
	spin_lock_init(&info->lock);
	mutex_init(&info->ptl_lock);

	return info;
}

static void dom_info_free(dom_info *info)
{
	if (info->pgd != NULL) {
		kfree(info->pgd);
		info->pgd = NULL;
	}

	kfree(info);
}

static struct iommu_domain *loongson_iommu_domain_alloc(unsigned int type)
{
	dom_info *info;

	switch (type) {
	case IOMMU_DOMAIN_UNMANAGED:
		info = dom_info_alloc();
		if (info == NULL)
			return NULL;

		info->domain.geometry.aperture_start	= 0;
		info->domain.geometry.aperture_end	= ~0ULL;
		info->domain.geometry.force_aperture	= true;
		break;

	default:
		return NULL;
	}

	return &info->domain;
}

void domain_deattach_iommu(dom_info *priv, iommu_info *info)
{
	if ((priv == NULL) || (info == NULL) ||
		(info->dev_cnt != 0) || (info->iommu == NULL)) {
		pr_err("%s invalid parameter", __func__);
		return;
	}
	del_domain_from_list(info->iommu, priv);
	domain_id_free(info->iommu, info->id);
	spin_lock(&priv->lock);
	list_del(&info->list);
	spin_unlock(&priv->lock);
	kfree(info);
}

static void loongson_iommu_domain_free(struct iommu_domain *domain)
{

	dom_info *priv;
	loongson_iommu *iommu = NULL;
	struct iommu_info *info, *tmp;

	priv = to_dom_info(domain);

	spin_lock(&priv->lock);
	list_for_each_entry_safe(info, tmp, &priv->iommu_devlist, list) {
		if (info->dev_cnt > 0)
			detach_all_dev_by_domain(info);
		iommu = info->iommu;
		spin_unlock(&priv->lock);
		domain_deattach_iommu(priv, info);
		spin_lock(&priv->lock);

		iommu_flush_iotlb(iommu);

		if (!has_dom(iommu))
			iommu_translate_disable(iommu);

	}
	spin_unlock(&priv->lock);

	mutex_lock(&priv->ptl_lock);
	iommu_free_pagetable(priv);
	mutex_unlock(&priv->ptl_lock);
	dom_info_free(priv);
}

struct iommu_rlookup_entry *lookup_rlooptable(int pcisegment)
{
	struct iommu_rlookup_entry *rlookupentry = NULL;

	list_for_each_entry(rlookupentry, &loongson_rlookup_iommu_list, list) {
		if (rlookupentry->pcisegment == pcisegment)
			return rlookupentry;
	}

	return NULL;
}

loongson_iommu *find_iommu_by_dev(struct pci_dev  *pdev)
{
	int pcisegment;
	unsigned short devid;
	struct pci_bus *bus = pdev->bus;
	struct iommu_rlookup_entry *rlookupentry = NULL;
	loongson_iommu *iommu = NULL;

	devid = PCI_DEVID(bus->number, pdev->devfn);
	pcisegment = pci_domain_nr(bus);
	rlookupentry = lookup_rlooptable(pcisegment);
	if (rlookupentry == NULL) {
		pr_info("%s find segment %d rlookupentry failed\n", __func__,
				pcisegment);
		return iommu;
	}

	iommu = rlookupentry->rlookup_table[devid];

	return iommu;
}

static int iommu_init_device(struct device *dev)
{
	unsigned short devid;
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_bus *bus = pdev->bus;
	struct loongson_iommu_dev_data *dev_data;
	loongson_iommu *iommu = NULL;

	if (dev_iommu_priv_get(dev)) {
		pr_info("Loongson-IOMMU: bdf:0x%x has added\n", pdev->devfn);
		return 0;
	}

	dev_data = kzalloc(sizeof(*dev_data), GFP_KERNEL);
	if (!dev_data)
		return -ENOMEM;

	devid = PCI_DEVID(bus->number, pdev->devfn);
	dev_data->bdf = devid;

	pci_info(pdev, "%s bdf %x\n", __func__, dev_data->bdf);
	iommu = find_iommu_by_dev(pdev);
	if (iommu == NULL)
		pci_info(pdev, "%s find iommu failed by dev\n", __func__);
	else
		pci_info(pdev, "%s bdf %#x iommu dev id %#x\n", __func__,
				dev_data->bdf, iommu->devid);

	/* The initial state is 0, and 1 is added only when attach dev */
	dev_data->count = 0;
	dev_data->iommu = iommu;

	dev_iommu_priv_set(dev, dev_data);

	return 0;
}

static struct iommu_device *loongson_iommu_probe_device(struct device *dev)
{
	int ret = 0;

	ret = iommu_init_device(dev);
	if (ret < 0)
		pr_err("Loongson-IOMMU: unable to alloc memory for dev_data\n");

	return 0;
}

static struct iommu_group *loongson_iommu_device_group(struct device *dev)
{
	struct iommu_group *group;

	/*
	 * We don't support devices sharing stream IDs other than PCI RID
	 * aliases, since the necessary ID-to-device lookup becomes rather
	 * impractical given a potential sparse 32-bit stream ID space.
	 */
	if (dev_is_pci(dev))
		group = pci_device_group(dev);
	else
		group = generic_device_group(dev);

	return group;
}

static void loongson_iommu_release_device(struct device *dev)
{
	struct loongson_iommu_dev_data *dev_data;

	dev_data = dev_iommu_priv_get(dev);
	dev_iommu_priv_set(dev, NULL);
	kfree(dev_data);
}

iommu_info *get_iommu_info_from_dom(dom_info *priv, loongson_iommu *iommu)
{
	struct iommu_info *info;

	spin_lock(&priv->lock);
	list_for_each_entry(info, &priv->iommu_devlist, list) {
		if (info->iommu == iommu) {
			spin_unlock(&priv->lock);
			return info;
		}
	}
	spin_unlock(&priv->lock);

	return NULL;
}

iommu_info *domain_attach_iommu(dom_info *priv, loongson_iommu *iommu)
{
	u32 dir_ctrl;
	unsigned long phys;
	struct iommu_info *info;

	info = get_iommu_info_from_dom(priv, iommu);
	if (info)
		return info;

	info = kzalloc(sizeof(struct iommu_info), GFP_KERNEL_ACCOUNT);
	if (!info) {
		pr_info("%s alloc memory for iommu_entry failed\n", __func__);
		return NULL;
	}

	INIT_LIST_HEAD(&info->dev_list);
	info->iommu = iommu;
	info->id = domain_id_alloc(iommu);
	if (info->id == -1) {
		pr_info("%s alloc id for domain failed\n", __func__);
		kfree(info);
		return NULL;
	}

	phys = virt_to_phys(priv->pgd);
	dir_ctrl = (IOMMU_LEVEL_STRIDE << 26) | (IOMMU_LEVEL_SHIFT(2) << 20);
	dir_ctrl |= (IOMMU_LEVEL_STRIDE <<  16) | (IOMMU_LEVEL_SHIFT(1) << 10);
	dir_ctrl |= (IOMMU_LEVEL_STRIDE << 6) | IOMMU_LEVEL_SHIFT(0);
	iommu_write_regl(iommu, LA_IOMMU_DIR_CTRL(info->id), dir_ctrl);
	iommu_write_regl(iommu, LA_IOMMU_PGD_HI(info->id), phys >> 32);
	iommu_write_regl(iommu, LA_IOMMU_PGD_LO(info->id), phys & UINT_MAX);

	spin_lock(&priv->lock);
	list_add(&info->list, &priv->iommu_devlist);
	spin_unlock(&priv->lock);
	add_domain_to_list(iommu, priv);

	return info;
}

static struct loongson_iommu_dev_data *get_devdata_from_iommu_info(dom_info *info,
		loongson_iommu *iommu, unsigned long bdf)
{
	struct iommu_info *entry;
	struct loongson_iommu_dev_data *dev_data, *found = NULL;

	entry = get_iommu_info_from_dom(info, iommu);
	if (!entry)
		return found;

	spin_lock(&entry->devlock);
	list_for_each_entry(dev_data, &entry->dev_list, list) {
		if (dev_data->bdf == bdf) {
			found = dev_data;
			break;
		}
	}

	spin_unlock(&entry->devlock);
	return found;
}

static int loongson_iommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	unsigned short bdf;
	struct pci_dev *pdev = to_pci_dev(dev);
	unsigned char busnum = pdev->bus->number;
	struct loongson_iommu_dev_data *dev_data;
	dom_info *priv = to_dom_info(domain);
	iommu_info *info;
	loongson_iommu *iommu;

	bdf = PCI_DEVID(busnum, pdev->devfn);
	dev_data = (struct loongson_iommu_dev_data *)dev_iommu_priv_get(dev);
	if (dev_data == NULL) {
		pci_info(pdev, "%s dev_data is Invalid\n", __func__);
		return 0;
	}

	iommu = dev_data->iommu;
	if (iommu == NULL) {
		pci_info(pdev, "%s iommu is Invalid\n", __func__);
		return 0;
	}

	pci_info(pdev, "%s bdf %#x priv %lx iommu devid %#x\n", __func__,
			bdf, (unsigned long)priv, iommu->devid);
	dev_data = get_devdata_from_iommu_info(priv, iommu, bdf);
	if (dev_data) {
		pci_info(pdev, "Loongson-IOMMU: bdf 0x%x devfn %x has attached,"
				" count:0x%x\n",
			bdf, pdev->devfn, dev_data->count);
		return 0;
	} else {
		dev_data = (struct loongson_iommu_dev_data *)dev_iommu_priv_get(dev);
	}

	info = domain_attach_iommu(priv, iommu);
	if (!info) {
		pci_info(pdev, "domain attach iommu failed\n");
		return 0;
	}

	do_attach(info, dev_data);

	return 0;
}

static unsigned long *iommu_get_pte(void *pt_base,
				unsigned long vaddr, int level)
{
	int i;
	unsigned long *ptep, *pgtable;

	if (level > (IOMMU_LEVEL_MAX - 1))
		return NULL;
	pgtable = pt_base;
	for (i = IOMMU_LEVEL_MAX - 1; i >= level; i--) {
		ptep = iommu_pte_offset(pgtable, vaddr, i);
		if (!iommu_pte_present(ptep))
			break;
		if (iommu_pte_huge(ptep))
			break;
		pgtable = phys_to_virt(*ptep & IOMMU_PAGE_MASK);
	}
	return ptep;
}

static int iommu_get_page_table(unsigned long *ptep)
{
	void *addr;
	unsigned long pte;

	if (!iommu_pte_present(ptep)) {
		addr = kzalloc(IOMMU_PAGE_SIZE, GFP_KERNEL_ACCOUNT);
		if (!addr)
			return -ENOMEM;
		pte = virt_to_phys(addr) & IOMMU_PAGE_MASK;
		pte |= IOMMU_PTE_RW;
		*ptep = pte;
	}
	return 0;
}

static size_t iommu_page_map(void *pt_base,
		unsigned long start, unsigned long end,
		phys_addr_t paddr, int level)
{
	int ret, huge, switch_page;
	unsigned long next, old, step;
	unsigned long pte, *ptep, *pgtable;

	old = start;
	ptep = iommu_pte_offset(pt_base, start, level);
	if (level == IOMMU_PT_LEVEL0) {
		paddr = paddr & IOMMU_PAGE_MASK;
		do {
			pte =  paddr | IOMMU_PTE_RW;
			*ptep = pte;
			ptep++;
			start += IOMMU_PAGE_SIZE;
			paddr += IOMMU_PAGE_SIZE;
		} while (start < end);

		return start - old;
	}

	do {
		next = iommu_ptable_end(start, end, level);
		step = next - start;
		huge = 0;
		switch_page = 0;
		if (level == IOMMU_PT_LEVEL1) {
			if ((step == IOMMU_HPAGE_SIZE) &&
			    (!iommu_pte_present(ptep) || iommu_pte_huge(ptep)))
				huge = 1;
			else if (iommu_pte_present(ptep) && iommu_pte_huge(ptep))
				switch_page = 1;
		}

		if (switch_page)
			switch_huge_to_page(ptep, start);

		if (huge) {
			pte =  (paddr & IOMMU_HPAGE_MASK) |
				IOMMU_PTE_RW | IOMMU_PTE_HP;
			*ptep = pte;
		} else {
			ret = iommu_get_page_table(ptep);
			if (ret != 0)
				break;
			pgtable = phys_to_virt(*ptep & IOMMU_PAGE_MASK);
			iommu_page_map(pgtable, start, next, paddr, level - 1);
		}

		ptep++;
		paddr += step;
		start = next;
	} while (start < end);

	return start - old;
}

static void switch_huge_to_page(unsigned long *ptep, unsigned long start)
{
	int ret;
	phys_addr_t paddr = *ptep & IOMMU_HPAGE_MASK;
	unsigned long next = start + IOMMU_HPAGE_SIZE;
	unsigned long *pgtable;

	*ptep = 0;
	ret = iommu_get_page_table(ptep);
	if (ret == 0) {
		pgtable = phys_to_virt(*ptep & IOMMU_PAGE_MASK);
		iommu_page_map(pgtable, start, next, paddr, 0);
	}
}

static int domain_map_page(dom_info *priv, unsigned long start,
			phys_addr_t paddr, size_t size)
{
	int ret = 0;
	phys_addr_t end;
	size_t map_size;

	end = start + size;
	mutex_lock(&priv->ptl_lock);
	map_size = iommu_page_map(priv->pgd, start,
			end, paddr, IOMMU_LEVEL_MAX - 1);
	if (map_size != size)
		ret = -EFAULT;
	mutex_unlock(&priv->ptl_lock);
	loongson_iommu_flush_iotlb_all(&priv->domain);

	return ret;
}

static size_t iommu_page_unmap(void *pt_base,
		unsigned long start, unsigned long end, int level)
{
	unsigned long next, old;
	unsigned long *ptep, *pgtable;

	old = start;
	ptep = iommu_pte_offset(pt_base, start, level);
	if (level == IOMMU_PT_LEVEL0) {
		do {
			*ptep++ = 0;
			start += IOMMU_PAGE_SIZE;
		} while (start < end);
	} else {
		do {
			next = iommu_ptable_end(start, end, level);
			if (!iommu_pte_present(ptep))
				continue;

			if ((level == IOMMU_PT_LEVEL1) &&
			    iommu_pte_huge(ptep) && ((next - start) < IOMMU_HPAGE_SIZE))
				switch_huge_to_page(ptep, start);

			if (iommu_pte_huge(ptep)) {
				if ((next - start) != IOMMU_HPAGE_SIZE)
					pr_err("Map pte on hugepage not supported now\n");
				*ptep = 0;
			} else {
				pgtable = phys_to_virt(*ptep & IOMMU_PAGE_MASK);
				iommu_page_unmap(pgtable, start, next, level - 1);
			}
		} while (ptep++, start = next, start < end);
	}
	return start - old;
}

static size_t domain_unmap_page(dom_info *priv,
		unsigned long start, size_t size)
{
	size_t unmap_len;
	unsigned long end;

	end = start + size;
	mutex_lock(&priv->ptl_lock);
	unmap_len = iommu_page_unmap(priv->pgd, start,
			end, (IOMMU_LEVEL_MAX - 1));
	mutex_unlock(&priv->ptl_lock);
	loongson_iommu_flush_iotlb_all(&priv->domain);

	return unmap_len;
}

static int loongson_iommu_map(struct iommu_domain *domain, unsigned long vaddr,
			      phys_addr_t paddr, size_t len, size_t count, int prot, gfp_t gfp, size_t *mapped)
{
	int ret;
	dom_info *priv = to_dom_info(domain);

	ret = domain_map_page(priv, vaddr, paddr, len);
	if (ret == 0)
		*mapped = len;

	return ret;
}

static size_t loongson_iommu_unmap(struct iommu_domain *domain, unsigned long vaddr,
				   size_t len, size_t count, struct iommu_iotlb_gather *gather)
{
	dom_info *priv = to_dom_info(domain);

	return domain_unmap_page(priv, vaddr, len);
}

static phys_addr_t _iommu_iova_to_phys(dom_info *info, dma_addr_t vaddr)
{
	unsigned long *ptep;
	unsigned long page_size, page_mask;
	phys_addr_t paddr;

	mutex_lock(&info->ptl_lock);
	ptep = iommu_get_pte(info->pgd, vaddr, IOMMU_PT_LEVEL0);
	mutex_unlock(&info->ptl_lock);

	if (!ptep || !iommu_pte_present(ptep)) {
		pr_warn_once("Loonson-IOMMU: shadow pte is null or not present with vaddr %llx\n", vaddr);
		paddr = 0;
		return paddr;
	}

	if (iommu_pte_huge(ptep)) {
		page_size = IOMMU_HPAGE_SIZE;
		page_mask = IOMMU_HPAGE_MASK;
	} else {
		page_size = IOMMU_PAGE_SIZE;
		page_mask = IOMMU_PAGE_MASK;
	}
	paddr = *ptep & page_mask;
	paddr |= vaddr & (page_size - 1);

	return paddr;
}

static phys_addr_t loongson_iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t vaddr)
{
	phys_addr_t phys;
	dom_info *priv = to_dom_info(domain);

	spin_lock(&priv->lock);
	phys = _iommu_iova_to_phys(priv, vaddr);
	spin_unlock(&priv->lock);

	return phys;
}

static void loongson_iommu_iotlb_sync(struct iommu_domain *domain,
				      struct iommu_iotlb_gather *gather)
{
	loongson_iommu_flush_iotlb_all(domain);
}

static struct iommu_ops loongson_iommu_ops = {
	.capable = loongson_iommu_capable,
	.domain_alloc = loongson_iommu_domain_alloc,
	.probe_device = loongson_iommu_probe_device,
	.release_device = loongson_iommu_release_device,
	.device_group = loongson_iommu_device_group,
	.pgsize_bitmap = LA_IOMMU_PGSIZE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= loongson_iommu_attach_dev,
		.free		= loongson_iommu_domain_free,
		.map_pages	= loongson_iommu_map,
		.unmap_pages	= loongson_iommu_unmap,
		.iova_to_phys	= loongson_iommu_iova_to_phys,
		.iotlb_sync	= loongson_iommu_iotlb_sync,
		.flush_iotlb_all = loongson_iommu_flush_iotlb_all,
	}
};

loongson_iommu *loongarch_get_iommu_by_devid(struct pci_dev *pdev)
{
	int pcisegment;
	unsigned short devid;
	struct pci_bus *bus = pdev->bus;
	loongson_iommu *iommu = NULL;

	devid = PCI_DEVID(bus->number, pdev->devfn);
	pcisegment = pci_domain_nr(pdev->bus);

	list_for_each_entry(iommu, &loongson_iommu_list, list) {
		if ((iommu->segment == pcisegment) &&
		    (iommu->devid == devid)) {
			return iommu;
		}
	}

	return NULL;
}

bool check_device_compat(struct pci_dev *pdev)
{
	bool compat = true;

	if ((pdev->revision == 0) && (pdev->device == 0x7a1f))
		compat = false;

	return compat;
}

static int loongson_iommu_probe(struct pci_dev *pdev,
				const struct pci_device_id *ent)
{
	int ret = 1;
	int bitmap_sz = 0;
	int tmp;
	bool compat = false;
	struct loongson_iommu *iommu = NULL;
	resource_size_t base, size;

	iommu = loongarch_get_iommu_by_devid(pdev);
	if (iommu == NULL) {
		pci_info(pdev, "%s can't find iommu\n", __func__);
		return -ENODEV;
	}

	compat = check_device_compat(pdev);
	if (!compat) {
		pci_info(pdev, "%s The iommu driver is not compatible with this device\n", __func__);
		return -ENODEV;
	}

	iommu->pdev = pdev;
	base = pci_resource_start(pdev, 0);
	size = pci_resource_len(pdev, 0);
	if (!request_mem_region(base, size, "loongson_iommu")) {
		pci_err(pdev, "%d can't reserve mmio registers base %llx size %llx\n", __LINE__, base, size);
		return -ENOMEM;
	}

	iommu->confbase_phy = base;
	iommu->conf_size = size;
	iommu->confbase = ioremap(base, size);
	if (iommu->confbase == NULL) {
		pci_info(pdev, "%s iommu pci dev bar0 is NULL\n", __func__);
		return ret;
	}

	pr_info("iommu confbase %llx pgtsize %llx\n", (u64)iommu->confbase, size);
	tmp = MAX_DOMAIN_ID / 8;
	bitmap_sz = (MAX_DOMAIN_ID % 8) ? (tmp + 1) : tmp;
	iommu->domain_bitmap = bitmap_zalloc(bitmap_sz, GFP_KERNEL);
	if (iommu->domain_bitmap == NULL) {
		pr_err("Loongson-IOMMU: domain bitmap alloc err bitmap_sz:%d\n", bitmap_sz);
		goto out_err;
	}

	tmp = MAX_ATTACHED_DEV_ID / 8;
	bitmap_sz = (MAX_ATTACHED_DEV_ID % 8) ? (tmp + 1) : tmp;
	iommu->devtable_bitmap = bitmap_zalloc(bitmap_sz, GFP_KERNEL);
	if (iommu->devtable_bitmap == NULL) {
		pr_err("Loongson-IOMMU: devtable bitmap alloc err bitmap_sz:%d\n", bitmap_sz);
		goto out_err_1;
	}

	return 0;

out_err_1:
	iommu->pdev = NULL;
	iounmap(iommu->confbase);
	iommu->confbase = NULL;
	release_mem_region(iommu->confbase_phy, iommu->conf_size);
	iommu->confbase_phy = 0;
	iommu->conf_size = 0;
	kfree(iommu->domain_bitmap);
	iommu->domain_bitmap = NULL;
out_err:
	return ret;
}

static void loongson_iommu_remove(struct pci_dev *pdev)
{
	struct  loongson_iommu *iommu = NULL;

	iommu = loongarch_get_iommu_by_devid(pdev);
	if (iommu == NULL)
		return;

	if (iommu->domain_bitmap != NULL) {
		kfree(iommu->domain_bitmap);
		iommu->domain_bitmap = NULL;
	}
	if (iommu->devtable_bitmap != NULL) {
		kfree(iommu->devtable_bitmap);
		iommu->devtable_bitmap = NULL;
	}
	if (iommu->confbase != NULL) {
		iounmap(iommu->confbase);
		iommu->confbase = NULL;
	}
	if (iommu->confbase_phy != 0) {
		release_mem_region(iommu->confbase_phy, iommu->conf_size);
		iommu->confbase_phy = 0;
		iommu->conf_size = 0;
	}

}

static int __init check_ivrs_checksum(struct acpi_table_header *table)
{
	int i;
	u8 checksum = 0, *p = (u8 *)table;

	for (i = 0; i < table->length; ++i)
		checksum += p[i];
	if (checksum != 0) {
		/* ACPI table corrupt */
		pr_err("IVRS invalid checksum\n");
		return -ENODEV;
	}

	return 0;
}

struct iommu_rlookup_entry *create_rlookup_entry(int pcisegment)
{
	struct iommu_rlookup_entry *rlookupentry = NULL;

	rlookupentry = kzalloc(sizeof(struct iommu_rlookup_entry), GFP_KERNEL);
	if (rlookupentry == NULL)
		return rlookupentry;

	rlookupentry->pcisegment = pcisegment;

	/* IOMMU rlookup table - find the IOMMU for a specific device */
	rlookupentry->rlookup_table = (void *)__get_free_pages(
			GFP_KERNEL | __GFP_ZERO, get_order(rlookup_table_size));
	if (rlookupentry->rlookup_table == NULL) {
		kfree(rlookupentry);
		rlookupentry = NULL;
	} else {
		list_add(&rlookupentry->list, &loongson_rlookup_iommu_list);
	}

	return rlookupentry;
}

/* Writes the specific IOMMU for a device into the rlookup table */
static void __init set_iommu_for_device(loongson_iommu *iommu,
		u16 devid)
{
	struct iommu_rlookup_entry *rlookupentry = NULL;

	rlookupentry = lookup_rlooptable(iommu->segment);
	if (rlookupentry == NULL)
		rlookupentry = create_rlookup_entry(iommu->segment);

	if (rlookupentry != NULL)
		rlookupentry->rlookup_table[devid] = iommu;
}

static inline u32 get_ivhd_header_size(struct ivhd_header *h)
{
	u32 size = 0;

	switch (h->type) {
	case IVHD_HEAD_TYPE10:
		size = 24;
		break;
	case IVHD_HEAD_TYPE11:
	case IVHD_HEAD_TYPE40:
		size = 40;
		break;
	}
	return size;
}

static inline void update_last_devid(u16 devid)
{
	if (devid > loongson_iommu_last_bdf)
		loongson_iommu_last_bdf = devid;
}

/*
 * This function calculates the length of a given IVHD entry
 */
static inline int ivhd_entry_length(u8 *ivhd)
{
	u32 type = ((struct ivhd_entry *)ivhd)->type;

	if (type < 0x80) {
		return 0x04 << (*ivhd >> 6);
	} else if (type == IVHD_DEV_ACPI_HID) {
		/* For ACPI_HID, offset 21 is uid len */
		return *((u8 *)ivhd + 21) + 22;
	}
	return 0;
}

/*
 * After reading the highest device id from the IOMMU PCI capability header
 * this function looks if there is a higher device id defined in the ACPI table
 */
static int __init find_last_devid_from_ivhd(struct ivhd_header *h)
{
	u8 *p = (void *)h, *end = (void *)h;
	struct ivhd_entry *dev;

	u32 ivhd_size = get_ivhd_header_size(h);

	if (!ivhd_size) {
		pr_err("loongson-iommu: Unsupported IVHD type %#x\n", h->type);
		return -EINVAL;
	}

	p += ivhd_size;
	end += h->length;

	while (p < end) {
		dev = (struct ivhd_entry *)p;
		switch (dev->type) {
		case IVHD_DEV_ALL:
			/* Use maximum BDF value for DEV_ALL */
			update_last_devid(MAX_BDF_NUM);
			break;
		case IVHD_DEV_SELECT:
		case IVHD_DEV_RANGE_END:
		case IVHD_DEV_ALIAS:
		case IVHD_DEV_EXT_SELECT:
			/* all the above subfield types refer to device ids */
			update_last_devid(dev->devid);
			break;
		default:
			break;
		}
		p += ivhd_entry_length(p);
	}

	WARN_ON(p != end);

	return 0;
}

/*
 * Iterate over all IVHD entries in the ACPI table and find the highest device
 * id which we need to handle. This is the first of three functions which parse
 * the ACPI table. So we check the checksum here.
 */
static int __init find_last_devid_acpi(struct acpi_table_header *table)
{
	u8 *p = (u8 *)table, *end = (u8 *)table;
	struct ivhd_header *h;

	p += IVRS_HEADER_LENGTH;

	end += table->length;
	while (p < end) {
		h = (struct ivhd_header *)p;
		if (h->type == loongson_iommu_target_ivhd_type) {
			int ret = find_last_devid_from_ivhd(h);

			if (ret)
				return ret;
		}

		if (h->length == 0)
			break;

		p += h->length;
	}

	if (p != end)
		return -EINVAL;


	return 0;
}

/*
 * Takes a pointer to an loongarch IOMMU entry in the ACPI table and
 * initializes the hardware and our data structures with it.
 */
static int __init init_iommu_from_acpi(loongson_iommu *iommu,
					struct ivhd_header *h)
{
	u8 *p = (u8 *)h;
	u8 *end = p;
	u16 devid = 0, devid_start = 0;
	u32 dev_i, ivhd_size;
	struct ivhd_entry *e;

	/*
	 * Done. Now parse the device entries
	 */
	ivhd_size = get_ivhd_header_size(h);
	if (!ivhd_size) {
		pr_err("loongarch iommu: Unsupported IVHD type %#x\n", h->type);
		return -EINVAL;
	}

	if (h->length == 0)
		return -EINVAL;

	p += ivhd_size;
	end += h->length;

	while (p < end) {
		e = (struct ivhd_entry *)p;
		switch (e->type) {
		case IVHD_DEV_ALL:
			for (dev_i = 0; dev_i <= loongson_iommu_last_bdf; ++dev_i)
				set_iommu_for_device(iommu, dev_i);
			break;

		case IVHD_DEV_SELECT:
			pr_info("  DEV_SELECT\t\t\t devid: %02x:%02x.%x\n",
				PCI_BUS_NUM(e->devid), PCI_SLOT(e->devid), PCI_FUNC(e->devid));

			devid = e->devid;
			set_iommu_for_device(iommu, devid);
			break;

		case IVHD_DEV_SELECT_RANGE_START:
			pr_info("  DEV_SELECT_RANGE_START\t devid: %02x:%02x.%x\n",
				PCI_BUS_NUM(e->devid), PCI_SLOT(e->devid), PCI_FUNC(e->devid));

			devid_start = e->devid;
			break;

		case IVHD_DEV_RANGE_END:
			pr_info("  DEV_RANGE_END\t\t devid: %02x:%02x.%x\n",
				PCI_BUS_NUM(e->devid), PCI_SLOT(e->devid), PCI_FUNC(e->devid));

			devid = e->devid;
			for (dev_i = devid_start; dev_i <= devid; ++dev_i)
				set_iommu_for_device(iommu, dev_i);
			break;

		default:
			break;
		}

		p += ivhd_entry_length(p);
	}

	return 0;
}

/*
 * This function clues the initialization function for one IOMMU
 * together and also allocates the command buffer and programs the
 * hardware. It does NOT enable the IOMMU. This is done afterwards.
 */
static int __init init_iommu_one(loongson_iommu *iommu, struct ivhd_header *h)
{
	int ret;
	struct iommu_rlookup_entry *rlookupentry = NULL;

	spin_lock_init(&iommu->domain_bitmap_lock);
	spin_lock_init(&iommu->dom_info_lock);

	/* Add IOMMU to internal data structures */
	INIT_LIST_HEAD(&iommu->dom_list);

	list_add_tail(&iommu->list, &loongson_iommu_list);

	/*
	 * Copy data from ACPI table entry to the iommu struct
	 */
	iommu->devid   = h->devid;
	iommu->segment = h->pci_seg;

	ret = init_iommu_from_acpi(iommu, h);
	if (ret) {
		pr_err("%s init iommu from acpi failed\n", __func__);
		return ret;
	}

	rlookupentry = lookup_rlooptable(iommu->segment);
	if (rlookupentry != NULL) {
		/*
		 * Make sure IOMMU is not considered to translate itself.
		 * The IVRS table tells us so, but this is a lie!
		 */
		rlookupentry->rlookup_table[iommu->devid] = NULL;
	}

	return 0;
}

/*
 * Iterates over all IOMMU entries in the ACPI table, allocates the
 * IOMMU structure and initializes it with init_iommu_one()
 */
static int __init init_iommu_all(struct acpi_table_header *table)
{
	int ret;
	u8 *p = (u8 *)table, *end = (u8 *)table;
	struct ivhd_header *h;
	loongson_iommu *iommu;

	end += table->length;
	p += IVRS_HEADER_LENGTH;

	while (p < end) {
		h = (struct ivhd_header *)p;

		if (h->length == 0)
			break;

		if (*p == loongson_iommu_target_ivhd_type) {
			pr_info("device: %02x:%02x.%01x seg: %d\n",
				PCI_BUS_NUM(h->devid), PCI_SLOT(h->devid), PCI_FUNC(h->devid), h->pci_seg);

			iommu = kzalloc(sizeof(loongson_iommu), GFP_KERNEL);
			if (iommu == NULL) {
				pr_info("%s alloc memory for iommu failed\n", __func__);
				return -ENOMEM;
			}

			ret = init_iommu_one(iommu, h);
			if (ret) {
				kfree(iommu);
				pr_info("%s init iommu failed\n", __func__);
				return ret;
			}
		}
		p += h->length;
	}

	if (p != end)
		return -EINVAL;

	return 0;
}

/**
 * get_highest_supported_ivhd_type - Look up the appropriate IVHD type
 * @ivrs          Pointer to the IVRS header
 *
 * This function search through all IVDB of the maximum supported IVHD
 */
static u8 get_highest_supported_ivhd_type(struct acpi_table_header *ivrs)
{
	u8 *base = (u8 *)ivrs;
	struct ivhd_header *ivhd = (struct ivhd_header *)(base + IVRS_HEADER_LENGTH);
	u8 last_type = ivhd->type;
	u16 devid = ivhd->devid;

	while (((u8 *)ivhd - base < ivrs->length) &&
	       (ivhd->type <= ACPI_IVHD_TYPE_MAX_SUPPORTED) &&
	       (ivhd->length > 0)) {
		u8 *p = (u8 *) ivhd;

		if (ivhd->devid == devid)
			last_type = ivhd->type;
		ivhd = (struct ivhd_header *)(p + ivhd->length);
	}

	return last_type;
}

static inline unsigned long tbl_size(int entry_size)
{
	unsigned int shift = PAGE_SHIFT +
			 get_order(((int)loongson_iommu_last_bdf + 1) * entry_size);

	return 1UL << shift;
}

static int __init loongson_iommu_ivrs_init(void)
{
	int ret = 0;
	acpi_status status;
	struct acpi_table_header *ivrs_base;

	status = acpi_get_table("IVRS", 0, &ivrs_base);
	if (status == AE_NOT_FOUND) {
		pr_info("%s get ivrs table failed\n", __func__);
		return -ENODEV;
	}

	/*
	 * Validate checksum here so we don't need to do it when
	 * we actually parse the table
	 */
	ret = check_ivrs_checksum(ivrs_base);
	if (ret)
		goto out;

	loongson_iommu_target_ivhd_type = get_highest_supported_ivhd_type(ivrs_base);
	pr_info("Using IVHD type %#x\n", loongson_iommu_target_ivhd_type);

	/*
	 * First parse ACPI tables to find the largest Bus/Dev/Func
	 * we need to handle. Upon this information the shared data
	 * structures for the IOMMUs in the system will be allocated
	 */
	ret = find_last_devid_acpi(ivrs_base);
	if (ret) {
		pr_err("%s find last devid failed\n", __func__);
		goto out;
	}

	rlookup_table_size = tbl_size(RLOOKUP_TABLE_ENTRY_SIZE);

	/*
	 * now the data structures are allocated and basically initialized
	 * start the real acpi table scan
	 */
	ret = init_iommu_all(ivrs_base);

out:
	/* Don't leak any ACPI memory */
	acpi_put_table(ivrs_base);
	ivrs_base = NULL;

	return ret;
}

static void free_iommu_rlookup_entry(void)
{
	loongson_iommu *iommu = NULL;
	struct iommu_rlookup_entry *rlookupentry = NULL;

	while (!list_empty(&loongson_iommu_list)) {
		iommu = list_first_entry(&loongson_iommu_list, loongson_iommu, list);
		list_del(&iommu->list);
		kfree(iommu);
	}

	while (!list_empty(&loongson_rlookup_iommu_list)) {
		rlookupentry = list_first_entry(&loongson_rlookup_iommu_list,
				struct iommu_rlookup_entry, list);

		list_del(&rlookupentry->list);
		if (rlookupentry->rlookup_table != NULL) {
			free_pages((unsigned long)rlookupentry->rlookup_table,
			get_order(rlookup_table_size));

			rlookupentry->rlookup_table = NULL;
		}

		kfree(rlookupentry);
	}
}

static int __init loonson_iommu_setup(char *str)
{
	if (!str)
		return -EINVAL;

	while (*str) {
		if (!strncmp(str, "on", 2)) {
			loongson_iommu_disable = 0;
			pr_info("IOMMU enabled\n");
		} else if (!strncmp(str, "off", 3)) {
			loongson_iommu_disable = 1;
			pr_info("IOMMU disabled\n");
		}
		str += strcspn(str, ",");
		while (*str == ',')
			str++;
	}
	return 0;
}
__setup("loongson_iommu=", loonson_iommu_setup);

static const struct pci_device_id loongson_iommu_pci_tbl[] = {
	{ PCI_DEVICE(0x14, 0x3c0f) },
	{ PCI_DEVICE(0x14, 0x7a1f) },
	{ 0, }
};

static struct pci_driver loongson_iommu_driver = {
	.name = "loongson-iommu",
	.probe	= loongson_iommu_probe,
	.remove	= loongson_iommu_remove,
	.id_table = loongson_iommu_pci_tbl,
};

static int __init loongson_iommu_driver_init(void)
{
	int ret = 0;

	if (!loongson_iommu_disable) {
		ret = loongson_iommu_ivrs_init();
		if (ret < 0) {
			free_iommu_rlookup_entry();
			pr_err("Failed to init iommu by ivrs\n");
		}

		ret = pci_register_driver(&loongson_iommu_driver);
		if (ret < 0) {
			pr_err("Failed to register IOMMU driver\n");
			return ret;
		}
	}

	return ret;
}

static void __exit loongson_iommu_driver_exit(void)
{
	loongson_iommu *iommu = NULL;

	if (!loongson_iommu_disable) {
		list_for_each_entry(iommu, &loongson_iommu_list, list) {
			loongson_iommu_remove(iommu->pdev);
		}
		free_iommu_rlookup_entry();
		pci_unregister_driver(&loongson_iommu_driver);
	}
}

module_init(loongson_iommu_driver_init);
module_exit(loongson_iommu_driver_exit);

MODULE_LICENSE("GPL");
