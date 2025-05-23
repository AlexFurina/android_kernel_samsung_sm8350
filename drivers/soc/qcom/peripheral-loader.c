// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2010-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/elf.h>
#include <linux/mutex.h>
#include <linux/memblock.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/rwsem.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <soc/qcom/ramdump.h>
#include <soc/qcom/subsystem_restart.h>
#include <soc/qcom/secure_buffer.h>
#include <linux/soc/qcom/smem.h>
#include <linux/kthread.h>

#include <linux/uaccess.h>
#include <asm/setup.h>
#define CREATE_TRACE_POINTS
#include <trace/events/trace_msm_pil_event.h>

#include "peripheral-loader.h"

#include <linux/sec_debug.h>
#include <soc/qcom/watchdog.h>
#include <linux/init.h>

#define pil_err(desc, fmt, ...)						\
	dev_err(desc->dev, "%s: " fmt, desc->name, ##__VA_ARGS__)
#define pil_info(desc, fmt, ...)					\
	dev_info(desc->dev, "%s: " fmt, desc->name, ##__VA_ARGS__)

#if defined(CONFIG_ARM)
#define pil_memset_io(d, c, count) memset(d, c, count)
#else
#define pil_memset_io(d, c, count) memset_io(d, c, count)
#endif

#define PIL_NUM_DESC		16
#define MAX_LEN 96
#define NUM_OF_ENCRYPTED_KEY	3

static void __iomem *pil_info_base;
static void __iomem *pil_disable_timeout_base;
static struct md_global_toc *g_md_toc;

/**
 * proxy_timeout - Override for proxy vote timeouts
 * -1: Use driver-specified timeout
 *  0: Hold proxy votes until shutdown
 * >0: Specify a custom timeout in ms
 */
static int proxy_timeout_ms = -1;
module_param(proxy_timeout_ms, int, 0644);

static bool disable_timeouts;

static struct workqueue_struct *pil_wq;
#if defined(CONFIG_QGKI)
static bool disable_cpboot;
#endif
/**
 * struct pil_mdt - Representation of <name>.mdt file in memory
 * @hdr: ELF32 header
 * @phdr: ELF32 program headers
 */
struct pil_mdt {
	struct elf32_hdr hdr;
	struct elf32_phdr phdr[];
};

/**
 * struct pil_seg - memory map representing one segment
 * @next: points to next seg mentor NULL if last segment
 * @paddr: physical start address of segment
 * @sz: size of segment
 * @filesz: size of segment on disk
 * @num: segment number
 * @relocated: true if segment is relocated, false otherwise
 *
 * Loosely based on an elf program header. Contains all necessary information
 * to load and initialize a segment of the image in memory.
 */
struct pil_seg {
	phys_addr_t paddr;
	unsigned long sz;
	size_t filesz;
	int num;
	struct list_head list;
	bool relocated;
};

/**
 * struct aux_minidumpinfo - State maintained for each aux minidump entry dumped
 * during SSR
 * @region_info_aux: region that contains an array of descriptors, where
 * each one describes the base address and size of a segment that should be
 * dumped during SSR minidump
 * @seg_cnt: the number of such regions
 */
struct aux_minidump_info {
	struct md_ss_region __iomem *region_info_aux;
	unsigned int seg_cnt;
};

/**
 * map_minidump_regions() - map the region containing the segment descriptors
 * for an entry in the global minidump table
 * @toc: the subsystem table of contents
 * @num_segs: the number of segment descriptors in the region defined by the
 * subsystem table of contents
 *
 * Maps the region containing num_segs segment descriptor into the kernel's
 * address space.
 */
static struct md_ss_region __iomem *map_minidump_regions(struct md_ss_toc *toc,
							int num_segs)
{
	u64 region_ptr = (u64)toc->md_ss_smem_regions_baseptr;
	void __iomem *segtable_base = ioremap((unsigned long)region_ptr,
					      num_segs *
					      sizeof(struct md_ss_region));
	struct md_ss_region __iomem *region_info = (struct md_ss_region __iomem
						  *)segtable_base;
	if (!region_info)
		return NULL;

	pr_debug("Minidump : Segments in minidump 0x%x\n", num_segs);

	return region_info;
}

/**
 * map_aux_minidump_regions() - map the region containing the segment
 * descriptors for a set of entries in the global minidump table
 * @desc: descriptor from pil_desc_init()
 * @aux_mdump_data: contains an array of pointers to segment descriptor regions
 * per auxiliary minidump ID
 * @total_aux_segs: value that is incremented to capture the total number of
 * segments that are needed to dump all auxiliary regions
 *
 * Maps the regions of segment descriptors for a set of auxiliary minidump IDs.
 */
static int map_aux_minidump_regions(struct pil_desc *desc,
				    struct aux_minidump_info *aux_mdump_data,
				    int *total_aux_segs)
{
	unsigned int i;
	struct md_ss_toc *toc;

	for (i = 0; i < desc->num_aux_minidump_ids; i++) {
		toc = desc->aux_minidump[i];
		if (toc && (toc->md_ss_toc_init == true) &&
		    (toc->md_ss_enable_status == MD_SS_ENABLED) &&
		    (toc->md_ss_smem_regions_baseptr) &&
		    (toc->encryption_status == MD_SS_ENCR_DONE)) {
			aux_mdump_data[i].seg_cnt = toc->ss_region_count;
			aux_mdump_data[i].region_info_aux =
				map_minidump_regions(desc->aux_minidump[i],
						aux_mdump_data[i].seg_cnt);
			if (!aux_mdump_data[i].region_info_aux)
				return -ENOMEM;
			*total_aux_segs += aux_mdump_data[i].seg_cnt;
		}
	}

	return 0;
}

/**
 * unmap_aux_minidump_regions() - unmap the regions containing the segment
 * descriptors for a set of entries in the global minidump table
 * @aux_mdump: contains an array of pointers to segment descriptor regions
 * per auxiliary minidump ID
 * @num_aux_md_ids: the number of auxiliary minidump IDs
 *
 * Unmaps the regions of segment descriptors for a set of auxiliary minidump
 * IDs.
 */
static void unmap_aux_minidump_regions(struct aux_minidump_info *aux_mdump,
				       int num_aux_md_ids)
{
	unsigned int i = 0;

	while (i < num_aux_md_ids && aux_mdump[i].region_info_aux) {
		iounmap(aux_mdump[i].region_info_aux);
		i++;
	}
}

/**
 * unmap_minidump_segments() - unmap the individual segments that were mapped
 * @rd_segs: The array of ramdump segments
 * @num_valid_segs: The number of valid segments from rd_segs
 *
 * The function unmaps the valid regions for the segments that were mapped.
 */
static void unmap_minidump_segments(struct ramdump_segment *rd_segs,
					int num_valid_segs)
{
	while (num_valid_segs--) {
		iounmap(rd_segs->v_address);
		rd_segs++;
	}
}

/**
 * prepare_minidump_segments() - Fills in the necessary information for the
 * ramdump driver to dump a region of memory, described by a segment.
 * @rd_segs: segments that will be filled in for ramdump collection
 * @region_info: the start of the region that contains the segment descriptors
 * @num_segs: the number of segment descriptors in region_info
 * @ss_valid_seg_cnt: the number of valid segments for this ramdump. Will be
 * decremented if a segment is found to be invalid.
 *
 * Fills in the necessary information in the ramdump_segment structures to
 * describe regions that should be dumped by the ramdump driver.
 */
static unsigned int prepare_minidump_segments(struct ramdump_segment *rd_segs,
				      struct md_ss_region __iomem *region_info,
				      int num_segs, int *ss_valid_seg_cnt)
{
	void __iomem *offset;
	unsigned int i;
	unsigned int val_segs = 0;


	for (i = 0; i < num_segs; i++) {
		memcpy(&offset, &region_info, sizeof(region_info));
		offset = offset + sizeof(region_info->name) +
				sizeof(region_info->seq_num);
		if (__raw_readl(offset) == MD_REGION_VALID) {
			memcpy(&rd_segs->name, &region_info,
			       sizeof(region_info));
			offset = offset +
				sizeof(region_info->md_valid);
			rd_segs->address = __raw_readl(offset);
			offset = offset +
				sizeof(region_info->region_base_address);
			rd_segs->size = __raw_readl(offset);

			rd_segs->v_address = ioremap_wc(rd_segs->address,
							rd_segs->size);
			if (!rd_segs->v_address)
				return -ENOMEM;

			pr_debug("Minidump : Dumping segment %s with address 0x%lx and size 0x%x\n",
				rd_segs->name, rd_segs->address,
				(unsigned int)rd_segs->size);
			rd_segs++;
			val_segs++;
		} else {
			(*ss_valid_seg_cnt)--;
		}

		region_info++;
	}

	return val_segs;
}

/**
 * prepare_aux_minidump_segments() - Fills in the necessary information for the
 * ramdump driver to dump a region of memory, described by a segment. This is
 * done for a set of auxiliary minidump IDs.
 * @rd_segs: segments that will be filled in for ramdump collection
 * @aux_mdump: contains an array of pointers to segment descriptor regions per
 * auxiliary minidump ID
 * @ss_valid_seg_cnt: the number of valid segments for this ramdump. Will be
 * decremented if a segment is found to be invalid.
 * @num_aux_md_ids: the number of auxiliary minidump IDs
 *
 * Fills in the necessary information in the ramdump_segment structures to
 * describe the regions that should be dumped by the ramdump driver for a set
 * of auxiliary minidump IDs.
 */
static int prepare_aux_minidump_segments(struct ramdump_segment *rd_segs,
					  struct aux_minidump_info *aux_mdump,
					  int *ss_valid_seg_cnt,
					  int num_aux_md_ids)
{
	unsigned int i;
	struct ramdump_segment *s = rd_segs;
	int next_offset = 0;

	for (i = 0; i < num_aux_md_ids; i++) {
		s = &rd_segs[next_offset];
		next_offset = prepare_minidump_segments(s,
					aux_mdump[i].region_info_aux,
					aux_mdump[i].seg_cnt,
					ss_valid_seg_cnt);
		if (next_offset < 0)
			return next_offset;
	}

	return 0;
}

static int pil_do_minidump(struct pil_desc *desc, void *ramdump_dev)
{
	struct md_ss_region __iomem *region_info_ss;
	struct ramdump_segment *ramdump_segs;
	struct pil_priv *priv = desc->priv;
	int ss_mdump_seg_cnt_ss = 0, total_segs;
	int total_aux_segs = 0;
	int ss_valid_seg_cnt;
	int ret;
	struct aux_minidump_info *aux_minidump_data = NULL;
	int next_offset;

	if (!ramdump_dev)
		return -ENODEV;

	ss_mdump_seg_cnt_ss = desc->minidump_ss->ss_region_count;
	region_info_ss = map_minidump_regions(desc->minidump_ss,
					     ss_mdump_seg_cnt_ss);
	if (!region_info_ss)
		return -EINVAL;

	if (desc->num_aux_minidump_ids > 0) {
		aux_minidump_data = kcalloc(desc->num_aux_minidump_ids,
					  sizeof(*aux_minidump_data),
					  GFP_KERNEL);
		if (!aux_minidump_data) {
			ret = -ENOMEM;
			goto setup_fail;
		}

		if (map_aux_minidump_regions(desc, aux_minidump_data,
					     &total_aux_segs) < 0) {
			ret = -ENOMEM;
			goto mapping_fail;
		}
	}

	total_segs = ss_mdump_seg_cnt_ss + total_aux_segs;
	ramdump_segs = kcalloc(total_segs,
			       sizeof(*ramdump_segs), GFP_KERNEL);
	if (!ramdump_segs) {
		ret = -ENOMEM;
		goto mapping_fail;
	}

	if (desc->subsys_vmid > 0)
		ret = pil_assign_mem_to_linux(desc, priv->region_start,
			(priv->region_end - priv->region_start));

	ss_valid_seg_cnt = total_segs;
	next_offset = prepare_minidump_segments(ramdump_segs, region_info_ss,
						 ss_mdump_seg_cnt_ss,
						 &ss_valid_seg_cnt);
	if (next_offset < 0) {
		ret = -ENOMEM;
		goto seg_mapping_fail;
	}

	if (desc->num_aux_minidump_ids > 0) {
		ret = prepare_aux_minidump_segments(&ramdump_segs[next_offset],
					      aux_minidump_data,
					      &ss_valid_seg_cnt,
					      desc->num_aux_minidump_ids);
		if (ret < 0)
			goto seg_mapping_fail;
	}

	if (desc->minidump_as_elf32)
		ret = do_minidump_elf32(ramdump_dev, ramdump_segs,
					ss_valid_seg_cnt);
	else
		ret = do_minidump(ramdump_dev, ramdump_segs, ss_valid_seg_cnt);
	if (ret)
		pil_err(desc, "%s: Minidump collection failed for subsys %s rc:%d\n",
			__func__, desc->name, ret);

#ifdef CONFIG_QGKI_MSM_BOOT_TIME_MARKER
	if (!strcmp(desc->name, "modem"))
		update_marker("M - Modem Dump completed");
#endif
	if (desc->subsys_vmid > 0)
		ret = pil_assign_mem_to_subsys(desc, priv->region_start,
			(priv->region_end - priv->region_start));

seg_mapping_fail:
	unmap_minidump_segments(ramdump_segs, ss_valid_seg_cnt);
	kfree(ramdump_segs);
mapping_fail:
	unmap_aux_minidump_regions(aux_minidump_data,
				   desc->num_aux_minidump_ids);
	kfree(aux_minidump_data);
setup_fail:
	iounmap(region_info_ss);
	return ret;
}

/**
 * pil_do_ramdump() - Ramdump an image
 * @desc: descriptor from pil_desc_init()
 * @ramdump_dev: ramdump device returned from create_ramdump_device()
 *
 * Calls the ramdump API with a list of segments generated from the addresses
 * that the descriptor corresponds to.
 */
int pil_do_ramdump(struct pil_desc *desc,
		   void *ramdump_dev, void *minidump_dev)
{
	struct ramdump_segment *ramdump_segs, *s;
	struct pil_priv *priv = desc->priv;
	struct pil_seg *seg;
	int count = 0, map_cnt = 0, ret;

	if (desc->minidump_ss) {
		pr_debug("Minidump : md_ss_toc->md_ss_toc_init is 0x%x\n",
			(unsigned int)desc->minidump_ss->md_ss_toc_init);
		pr_debug("Minidump : md_ss_toc->md_ss_enable_status is 0x%x\n",
			(unsigned int)desc->minidump_ss->md_ss_enable_status);
		pr_debug("Minidump : md_ss_toc->encryption_status is 0x%x\n",
			(unsigned int)desc->minidump_ss->encryption_status);
		pr_debug("Minidump : md_ss_toc->ss_region_count is 0x%x\n",
			(unsigned int)desc->minidump_ss->ss_region_count);
		pr_debug("Minidump : md_ss_toc->md_ss_smem_regions_baseptr is 0x%x\n",
			(unsigned int)
			desc->minidump_ss->md_ss_smem_regions_baseptr);
		/**
		 * Collect minidump if SS ToC is valid and segment table
		 * is initialized in memory and encryption status is set.
		 */
		if ((desc->minidump_ss->md_ss_smem_regions_baseptr != 0) &&
			(desc->minidump_ss->md_ss_toc_init == true) &&
			(desc->minidump_ss->md_ss_enable_status ==
				MD_SS_ENABLED)) {
			if (desc->minidump_ss->encryption_status ==
			    MD_SS_ENCR_DONE) {
				pr_debug("Dumping Minidump for %s\n",
					desc->name);
				return pil_do_minidump(desc, minidump_dev);
			}
			pr_debug("Minidump aborted for %s\n", desc->name);
			return -EINVAL;
		}
	}
	pr_debug("Continuing with full SSR dump for %s\n", desc->name);
	list_for_each_entry(seg, &priv->segs, list)
		count++;

	ramdump_segs = kcalloc(count, sizeof(*ramdump_segs), GFP_KERNEL);
	if (!ramdump_segs)
		return -ENOMEM;

	if (desc->subsys_vmid > 0)
		ret = pil_assign_mem_to_linux(desc, priv->region_start,
				(priv->region_end - priv->region_start));

	s = ramdump_segs;
	list_for_each_entry(seg, &priv->segs, list) {
		s->address = seg->paddr;
		s->size = seg->sz;
		s++;
		map_cnt++;
	}

	ret = do_elf_ramdump(ramdump_dev, ramdump_segs, count);

	kfree(ramdump_segs);

	if (ret)
		pil_err(desc, "%s: Ramdump collection failed for subsys %s rc:%d\n",
				__func__, desc->name, ret);
#ifdef CONFIG_QGKI_MSM_BOOT_TIME_MARKER
	if (!strcmp(desc->name, "modem"))
		update_marker("M - Modem Dump completed");
#endif
	if (desc->subsys_vmid > 0)
		ret = pil_assign_mem_to_subsys(desc, priv->region_start,
				(priv->region_end - priv->region_start));

	return ret;
}
EXPORT_SYMBOL(pil_do_ramdump);

int pil_assign_mem_to_subsys(struct pil_desc *desc, phys_addr_t addr,
							size_t size)
{
	int ret;
	int srcVM[1] = {VMID_HLOS};
	int destVM[1] = {desc->subsys_vmid};
	int destVMperm[1] = {PERM_READ | PERM_WRITE};

	ret = hyp_assign_phys(addr, size, srcVM, 1, destVM, destVMperm, 1);
	if (ret)
		pil_err(desc, "%s: failed for %pa address of size %zx - subsys VMid %d rc:%d\n",
				__func__, &addr, size, desc->subsys_vmid, ret);
	return ret;
}
EXPORT_SYMBOL(pil_assign_mem_to_subsys);

int pil_assign_mem_to_linux(struct pil_desc *desc, phys_addr_t addr,
							size_t size)
{
	int ret;
	int srcVM[1] = {desc->subsys_vmid};
	int destVM[1] = {VMID_HLOS};
	int destVMperm[1] = {PERM_READ | PERM_WRITE | PERM_EXEC};

	ret = hyp_assign_phys(addr, size, srcVM, 1, destVM, destVMperm, 1);
	if (ret)
		panic("%s: failed for %pa address of size %zx - subsys VMid %d rc:%d\n",
				__func__, &addr, size, desc->subsys_vmid, ret);

	return ret;
}
EXPORT_SYMBOL(pil_assign_mem_to_linux);

int pil_assign_mem_to_subsys_and_linux(struct pil_desc *desc,
						phys_addr_t addr, size_t size)
{
	int ret;
	int srcVM[1] = {VMID_HLOS};
	int destVM[2] = {VMID_HLOS, desc->subsys_vmid};
	int destVMperm[2] = {PERM_READ | PERM_WRITE, PERM_READ | PERM_WRITE};

	ret = hyp_assign_phys(addr, size, srcVM, 1, destVM, destVMperm, 2);
	if (ret)
		pil_err(desc, "%s: failed for %pa address of size %zx - subsys VMid %d rc:%d\n",
				__func__, &addr, size, desc->subsys_vmid, ret);

	return ret;
}
EXPORT_SYMBOL(pil_assign_mem_to_subsys_and_linux);

int pil_reclaim_mem(struct pil_desc *desc, phys_addr_t addr, size_t size,
						int VMid)
{
	int ret;
	int srcVM[2] = {VMID_HLOS, desc->subsys_vmid};
	int destVM[1] = {VMid};
	int destVMperm[1] = {PERM_READ | PERM_WRITE};

	if (VMid == VMID_HLOS)
		destVMperm[0] = PERM_READ | PERM_WRITE | PERM_EXEC;

	ret = hyp_assign_phys(addr, size, srcVM, 2, destVM, destVMperm, 1);
	if (ret)
		panic("%s: failed for %pa address of size %zx - subsys VMid %d. Fatal error.\n",
				__func__, &addr, size, desc->subsys_vmid);

	return ret;
}
EXPORT_SYMBOL(pil_reclaim_mem);

/**
 * pil_get_entry_addr() - Retrieve the entry address of a peripheral image
 * @desc: descriptor from pil_desc_init()
 *
 * Returns the physical address where the image boots at or 0 if unknown.
 */
phys_addr_t pil_get_entry_addr(struct pil_desc *desc)
{
	return desc->priv ? desc->priv->entry_addr : 0;
}
EXPORT_SYMBOL(pil_get_entry_addr);

static void __pil_proxy_unvote(struct pil_priv *priv)
{
	struct pil_desc *desc = priv->desc;

	desc->ops->proxy_unvote(desc);
	notify_proxy_unvote(desc->dev);
	__pm_relax(priv->ws);
	module_put(desc->owner);

}

static void pil_proxy_unvote_work(struct work_struct *work)
{
	struct delayed_work *delayed = to_delayed_work(work);
	struct pil_priv *priv = container_of(delayed, struct pil_priv, proxy);

	__pil_proxy_unvote(priv);
}

static int pil_proxy_vote(struct pil_desc *desc)
{
	int ret = 0;
	struct pil_priv *priv = desc->priv;

	if (desc->ops->proxy_vote) {
		__pm_stay_awake(priv->ws);
		ret = desc->ops->proxy_vote(desc);
		if (ret)
			__pm_relax(priv->ws);
	}

	if (desc->proxy_unvote_irq)
		enable_irq(desc->proxy_unvote_irq);
	notify_proxy_vote(desc->dev);

	return ret;
}

static void pil_proxy_unvote(struct pil_desc *desc, int immediate)
{
	struct pil_priv *priv = desc->priv;
	unsigned long timeout;

	if (proxy_timeout_ms == 0 && !immediate)
		return;
	else if (proxy_timeout_ms > 0)
		timeout = proxy_timeout_ms;
	else
		timeout = desc->proxy_timeout;

	if (desc->ops->proxy_unvote) {
		if (WARN_ON(!try_module_get(desc->owner)))
			return;

		if (immediate)
			timeout = 0;

		if (!desc->proxy_unvote_irq || immediate)
			schedule_delayed_work(&priv->proxy,
					      msecs_to_jiffies(timeout));
	}
}

static irqreturn_t proxy_unvote_intr_handler(int irq, void *dev_id)
{
	struct pil_desc *desc = dev_id;
	struct pil_priv *priv = desc->priv;

	pil_info(desc, "Power/Clock ready interrupt received\n");
	if (!desc->priv->unvoted_flag) {
		desc->priv->unvoted_flag = 1;
		__pil_proxy_unvote(priv);
	}

	return IRQ_HANDLED;
}

static bool segment_is_relocatable(const struct elf32_phdr *p)
{
	return !!(p->p_flags & BIT(27));
}

static phys_addr_t pil_reloc(const struct pil_priv *priv, phys_addr_t addr)
{
	return addr - priv->base_addr + priv->region_start;
}

static struct pil_seg *pil_init_seg(const struct pil_desc *desc,
				  const struct elf32_phdr *phdr, int num)
{
	bool reloc = segment_is_relocatable(phdr);
	const struct pil_priv *priv = desc->priv;
	struct pil_seg *seg;

	if (phdr->p_filesz > phdr->p_memsz) {
		pil_err(desc, "Segment %d: file size (%u) is greater than mem size (%u).\n",
			num, phdr->p_filesz, phdr->p_memsz);
		return ERR_PTR(-EINVAL);
	}

	seg = kmalloc(sizeof(*seg), GFP_KERNEL);
	if (!seg)
		return ERR_PTR(-ENOMEM);
	seg->num = num;
	seg->paddr = reloc ? pil_reloc(priv, phdr->p_paddr) : phdr->p_paddr;
	seg->filesz = phdr->p_filesz;
	seg->sz = phdr->p_memsz;
	seg->relocated = reloc;
	INIT_LIST_HEAD(&seg->list);

	return seg;
}

#define segment_is_hash(flag) (((flag) & (0x7 << 24)) == (0x2 << 24))

static int segment_is_loadable(const struct elf32_phdr *p)
{
	return (p->p_type == PT_LOAD) && !segment_is_hash(p->p_flags) &&
		p->p_memsz;
}

static void pil_dump_segs(const struct pil_priv *priv)
{
	struct pil_seg *seg;
	phys_addr_t seg_h_paddr;

	list_for_each_entry(seg, &priv->segs, list) {
		seg_h_paddr = seg->paddr + seg->sz;
		pil_info(priv->desc, "%d: %pa %pa\n", seg->num,
				&seg->paddr, &seg_h_paddr);
	}
}

/*
 * Ensure the entry address lies within the image limits and if the image is
 * relocatable ensure it lies within a relocatable segment.
 */
static int pil_init_entry_addr(struct pil_priv *priv, const struct pil_mdt *mdt)
{
	struct pil_seg *seg;
	phys_addr_t entry = mdt->hdr.e_entry;
	bool image_relocated = priv->is_region_allocated;

	if (image_relocated)
		entry = pil_reloc(priv, entry);
	priv->entry_addr = entry;

	if (priv->desc->flags & PIL_SKIP_ENTRY_CHECK)
		return 0;

	list_for_each_entry(seg, &priv->segs, list) {
		if (entry >= seg->paddr && entry < seg->paddr + seg->sz) {
			if (!image_relocated)
				return 0;
			else if (seg->relocated)
				return 0;
		}
	}
	pil_err(priv->desc, "entry address %pa not within range\n", &entry);
	pil_dump_segs(priv);
	return -EADDRNOTAVAIL;
}

static int pil_alloc_region(struct pil_priv *priv, phys_addr_t min_addr,
				phys_addr_t max_addr, size_t align)
{
	size_t size = max_addr - min_addr;
	size_t aligned_size;
	struct device_node *ofnode = priv->desc->dev->of_node;
	struct device_node *mem_node;
	int ret;
	struct resource res;

	/* Don't reallocate due to fragmentation concerns, just sanity check */
	if (priv->is_region_allocated) {
		if (WARN(priv->region_end - priv->region_start < size,
			"Can't reuse PIL memory, too small\n"))
			return -ENOMEM;
		return 0;
	}

	if (align >= SZ_4M)
		aligned_size = ALIGN(size, SZ_4M);
	else if (align >= SZ_1M)
		aligned_size = ALIGN(size, SZ_1M);
	else
		aligned_size = ALIGN(size, SZ_4K);

	/* read memory region here */
	mem_node = of_parse_phandle(ofnode, "memory-region", 0);
	if (!mem_node) {
		pil_err(priv->desc, "No memory-region associated\n");
		ret = -ENOMEM;
		goto err;
	}

	ret = of_address_to_resource(mem_node, 0, &res);
	of_node_put(mem_node);
	if (ret < 0) {
		pil_err(priv->desc, "Failed to get the resource\n");
		goto err;
	}

	priv->is_region_allocated = true;
	priv->region_start = res.start;
	priv->region_end = priv->region_start + size;
	priv->base_addr = min_addr;
	priv->region_size = aligned_size;

	return 0;

err:
	priv->region_start = 0;
	priv->region_end = 0;

	return ret;
}

static int pil_setup_region(struct pil_priv *priv, const struct pil_mdt *mdt)
{
	const struct elf32_phdr *phdr;
	phys_addr_t min_addr_r, min_addr_n, max_addr_r, max_addr_n, start, end;
	size_t align = 0;
	int i, ret = 0;
	bool relocatable = false;

	min_addr_n = min_addr_r = (phys_addr_t)ULLONG_MAX;
	max_addr_n = max_addr_r = 0;

	/* Find the image limits */
	for (i = 0; i < mdt->hdr.e_phnum; i++) {
		phdr = &mdt->phdr[i];
		if (!segment_is_loadable(phdr))
			continue;

		start = phdr->p_paddr;
		end = start + phdr->p_memsz;

		if (segment_is_relocatable(phdr)) {
			min_addr_r = min(min_addr_r, start);
			max_addr_r = max(max_addr_r, end);
			/*
			 * Lowest relocatable segment dictates alignment of
			 * relocatable region
			 */
			if (min_addr_r == start)
				align = phdr->p_align;
			relocatable = true;
		} else {
			min_addr_n = min(min_addr_n, start);
			max_addr_n = max(max_addr_n, end);
		}

	}

	/*
	 * Align the max address to the next 4K boundary to satisfy iommus and
	 * XPUs that operate on 4K chunks.
	 */
	max_addr_n = ALIGN(max_addr_n, SZ_4K);
	max_addr_r = ALIGN(max_addr_r, SZ_4K);

	if (relocatable) {
		ret = pil_alloc_region(priv, min_addr_r, max_addr_r, align);
	} else {
		priv->region_start = min_addr_n;
		priv->region_end = max_addr_n;
		priv->base_addr = min_addr_n;
	}

	if (priv->info) {
		__iowrite32_copy(&priv->info->start, &priv->region_start,
					sizeof(priv->region_start) / 4);
		writel_relaxed(priv->region_end - priv->region_start,
				&priv->info->size);
	}

	return ret;
}

static int pil_cmp_seg(void *priv, struct list_head *a, struct list_head *b)
{
	int ret = 0;
	struct pil_seg *seg_a = list_entry(a, struct pil_seg, list);
	struct pil_seg *seg_b = list_entry(b, struct pil_seg, list);

	if (seg_a->paddr < seg_b->paddr)
		ret = -1;
	else if (seg_a->paddr > seg_b->paddr)
		ret = 1;

	return ret;
}

static int pil_init_mmap(struct pil_desc *desc, const struct pil_mdt *mdt)
{
	struct pil_priv *priv = desc->priv;
	const struct elf32_phdr *phdr;
	struct pil_seg *seg;
	int i, ret;

	ret = pil_setup_region(priv, mdt);
	if (ret)
		return ret;

#ifdef CONFIG_QGKI_MSM_BOOT_TIME_MARKER
	if (!strcmp(desc->name, "modem"))
		update_marker("M - Modem Image Start Loading");
#endif

	pil_info(desc, "loading from %pa to %pa\n", &priv->region_start,
							&priv->region_end);

	priv->num_segs = 0;
	for (i = 0; i < mdt->hdr.e_phnum; i++) {
		phdr = &mdt->phdr[i];
		if (!segment_is_loadable(phdr))
			continue;

		seg = pil_init_seg(desc, phdr, i);
		if (IS_ERR(seg))
			return PTR_ERR(seg);

		list_add_tail(&seg->list, &priv->segs);
		priv->num_segs++;
	}
	list_sort(NULL, &priv->segs, pil_cmp_seg);

	return pil_init_entry_addr(priv, mdt);
}

struct pil_map_fw_info {
	void *region;
	unsigned long attrs;
	phys_addr_t base_addr;
	struct device *dev;
};

static void pil_release_mmap(struct pil_desc *desc)
{
	struct pil_priv *priv = desc->priv;
	struct pil_seg *p, *tmp;
	u64 zero = 0ULL;

	if (priv->info) {
		__iowrite32_copy(&priv->info->start, &zero,
					sizeof(zero) / 4);
		writel_relaxed(0, &priv->info->size);
	}

	list_for_each_entry_safe(p, tmp, &priv->segs, list) {
		list_del(&p->list);
		kfree(p);
	}
}

static void pil_clear_segment(struct pil_desc *desc)
{
	struct pil_priv *priv = desc->priv;
	void __iomem *buf;

	struct pil_map_fw_info map_fw_info = {
		.attrs = desc->attrs,
		.base_addr = priv->region_start,
		.dev = desc->dev,
	};

	void *map_data = desc->map_data ? desc->map_data : &map_fw_info;

	/* Clear memory so that unauthorized ELF code is not left behind */
	buf = desc->map_fw_mem(priv->region_start, (priv->region_end -
					priv->region_start), map_data);

	if (!buf) {
		pil_err(desc, "Failed to map memory\n");
		return;
	}

	pil_memset_io(buf, 0, (priv->region_end - priv->region_start));
	desc->unmap_fw_mem(buf, (priv->region_end - priv->region_start),
								map_data);

}

#define IOMAP_SIZE SZ_1M

static void __iomem *map_fw_mem(phys_addr_t paddr, size_t size, void *data)
{
	return ioremap_wc(paddr, size);
}

static void unmap_fw_mem(void __iomem *vaddr, size_t size, void *data)
{
	iounmap(vaddr);
}

static int pil_load_seg(struct pil_desc *desc, struct pil_seg *seg)
{
	int ret = 0, count;
	phys_addr_t paddr;
	char fw_name[30];
	int num = seg->num;
	const struct firmware *fw = NULL;
	void __iomem *firmware_buf;
	struct pil_map_fw_info map_fw_info = {
		.attrs = desc->attrs,
		.base_addr = desc->priv->region_start,
		.dev = desc->dev,
	};
	void *map_data = desc->map_data ? desc->map_data : &map_fw_info;

	if (seg->filesz) {
		snprintf(fw_name, ARRAY_SIZE(fw_name), "%s.b%02d",
				desc->fw_name, num);
		firmware_buf = desc->map_fw_mem(seg->paddr, seg->filesz,
						map_data);
		if (!firmware_buf) {
			pil_err(desc, "Failed to map memory for firmware buffer\n");
			return -ENOMEM;
		}

		ret = request_firmware_into_buf(&fw, fw_name, desc->dev,
						firmware_buf, seg->filesz);
		desc->unmap_fw_mem(firmware_buf, seg->filesz, map_data);

		if (ret) {
			pil_err(desc, "Failed to locate blob %s or blob is too big(rc:%d)\n",
				fw_name, ret);
			return ret;
		}

		if (fw->size != seg->filesz) {
			pil_err(desc, "Blob size %u doesn't match %lu\n",
					ret, seg->filesz);
			release_firmware(fw);
			return -EPERM;
		}

		release_firmware(fw);
	}

	/* Zero out trailing memory */
	paddr = seg->paddr + seg->filesz;
	count = seg->sz - seg->filesz;
	while (count > 0) {
		size_t size;
		void __iomem *buf;

		size = min_t(size_t, IOMAP_SIZE, count);
		buf = desc->map_fw_mem(paddr, size, map_data);
		if (!buf) {
			pil_err(desc, "Failed to map memory\n");
			return -ENOMEM;
		}
		pil_memset_io(buf, 0, size);

		desc->unmap_fw_mem(buf, size, map_data);

		count -= size;
		paddr += size;
	}

	if (desc->ops->verify_blob) {
		ret = desc->ops->verify_blob(desc, seg->paddr, seg->sz);
		if (ret)
			pil_err(desc, "Blob%u failed verification(rc:%d)\n",
								num, ret);
	}

	return ret;
}

static int pil_parse_devicetree(struct pil_desc *desc)
{
	struct device_node *ofnode = desc->dev->of_node;
	int clk_ready = 0;

	if (!ofnode)
		return -EINVAL;

	if (of_property_read_u32(ofnode, "qcom,mem-protect-id",
					&desc->subsys_vmid))
		pr_debug("Unable to read the addr-protect-id for %s\n",
					desc->name);

	if (desc->ops->proxy_unvote &&
			of_property_match_string(ofnode, "interrupt-names",
				"qcom,proxy-unvote") >= 0) {
		clk_ready = of_irq_get_byname(ofnode,
				"qcom,proxy-unvote");

		if (clk_ready < 0) {
			dev_dbg(desc->dev,
				"[%s]: Error getting proxy unvoting irq\n",
				desc->name);
			return clk_ready;
		}

	}
	desc->proxy_unvote_irq = clk_ready;


	return 0;
}

static int pil_notify_aop(struct pil_desc *desc, char *status)
{
	struct qmp_pkt pkt;
	char mbox_msg[MAX_LEN];

	if (!desc->signal_aop)
		return 0;

	snprintf(mbox_msg, MAX_LEN,
		"{class: image, res: load_state, name: %s, val: %s}",
		desc->name, status);
	pkt.size = MAX_LEN;
	pkt.data = mbox_msg;

	return mbox_send_message(desc->mbox, &pkt);
}

#if defined(CONFIG_QGKI)
static int __init get_cpboot_status(char *str)
{

	if(!strncmp(str,"disable",7))
		disable_cpboot = true;
	else
		disable_cpboot = false;

	pr_warn("%s : disable_cpboot:%u\n",__func__,disable_cpboot);
	return 0;
}
early_param("androidboot.cpboot", get_cpboot_status);
#endif

/* Synchronize request_firmware() with suspend */
static DECLARE_RWSEM(pil_pm_rwsem);

struct pil_seg_data {
	struct pil_desc *desc;
	struct pil_seg *seg;
	struct work_struct load_seg_work;
	int retval;
};

static void pil_load_seg_work_fn(struct work_struct *work)
{
	struct pil_seg_data *pil_seg_data = container_of(work,
							struct pil_seg_data,
							load_seg_work);
	struct pil_desc *desc = pil_seg_data->desc;
	struct pil_seg *seg = pil_seg_data->seg;

	pil_seg_data->retval = pil_load_seg(desc, seg);
}

static int pil_load_segs(struct pil_desc *desc)
{
	int ret = 0;
	int seg_id = 0;
	struct pil_priv *priv = desc->priv;
	struct pil_seg_data *pil_seg_data;
	struct pil_seg *seg;
	unsigned long *err_map;

	err_map = kcalloc(BITS_TO_LONGS(priv->num_segs), sizeof(*err_map),
			  GFP_KERNEL);
	if (!err_map)
		return -ENOMEM;

	pil_seg_data = kcalloc(priv->num_segs, sizeof(*pil_seg_data),
				GFP_KERNEL);
	if (!pil_seg_data) {
		ret = -ENOMEM;
		goto out;
	}

	/* Initialize and spawn a thread for each segment */
	list_for_each_entry(seg, &desc->priv->segs, list) {
		pil_seg_data[seg_id].desc = desc;
		pil_seg_data[seg_id].seg = seg;

		INIT_WORK(&pil_seg_data[seg_id].load_seg_work,
				pil_load_seg_work_fn);
		queue_work(pil_wq, &pil_seg_data[seg_id].load_seg_work);

		seg_id++;
	}

	bitmap_zero(err_map, priv->num_segs);

	/* Wait for the parallel loads to finish */
	seg_id = 0;
	list_for_each_entry(seg, &desc->priv->segs, list) {
		flush_work(&pil_seg_data[seg_id].load_seg_work);

		/* Don't exit if one of the thread fails. Wait for others to
		 * complete. Bitmap the return codes we get from the threads.
		 */
		if (pil_seg_data[seg_id].retval) {
			pil_err(desc,
				"Failed to load the segment[%d]. ret = %d\n",
				seg_id, pil_seg_data[seg_id].retval);
			__set_bit(seg_id, err_map);
		}

		seg_id++;
	}

	kfree(pil_seg_data);

	/* Each segment can fail due to different reason. Send a generic err */
	if (!bitmap_empty(err_map, priv->num_segs))
		ret = -EFAULT;

out:
	kfree(err_map);
	return ret;
}

/**
 * pil_boot() - Load a peripheral image into memory and boot it
 * @desc: descriptor from pil_desc_init()
 *
 * Returns 0 on success or -ERROR on failure.
 */
int pil_boot(struct pil_desc *desc)
{
	int ret;
	char fw_name[30];
	struct pil_seg *seg;
	const struct pil_mdt *mdt;
	const struct elf32_hdr *ehdr;
	const struct firmware *fw;
	struct pil_priv *priv = desc->priv;
	bool mem_protect = false;
	bool hyp_assign = false;
#if IS_ENABLED(CONFIG_SEC_PERIPHERAL_SECURE_CHK)
	bool secure_check_fail = false;
#endif

	pil_info(desc, "Sending ON message to AOP ...\n");
	ret = pil_notify_aop(desc, "on");
	if (ret < 0) {
		pil_err(desc, "Failed to send ON message to AOP rc:%d\n", ret);
#if IS_ENABLED(CONFIG_SEC_PERIPHERAL_SECURE_CHK)
		if (ret == -ETIME) {
			smp_send_stop();
			qcom_wdt_trigger_bite();
		}
#endif
		return ret;
	}

	if (desc->shutdown_fail)
		pil_err(desc, "Subsystem shutdown failed previously!\n");

	desc->clear_fw_region = true;
	/* Reinitialize for new image */
	pil_release_mmap(desc);

	down_read(&pil_pm_rwsem);
	snprintf(fw_name, sizeof(fw_name), "%s.mdt", desc->fw_name);
	ret = request_firmware(&fw, fw_name, desc->dev);
	if (ret) {
		pil_err(desc, "Failed to locate %s(rc:%d)\n", fw_name, ret);
		goto out;
	}

	if (fw->size < sizeof(*ehdr)) {
		pil_err(desc, "Not big enough to be an elf header\n");
		ret = -EIO;
		goto release_fw;
	}

	mdt = (const struct pil_mdt *)fw->data;
	ehdr = &mdt->hdr;

	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG)) {
		pil_err(desc, "Not an elf header\n");
		ret = -EIO;
		goto release_fw;
	}

	if (ehdr->e_phnum == 0) {
		pil_err(desc, "No loadable segments\n");
		ret = -EIO;
		goto release_fw;
	}
	if (sizeof(struct elf32_phdr) * ehdr->e_phnum +
	    sizeof(struct elf32_hdr) > fw->size) {
		pil_err(desc, "Program headers not within mdt\n");
		ret = -EIO;
		goto release_fw;
	}

#if defined(CONFIG_QGKI)
	if(disable_cpboot){
		if (!strcmp(desc->name, "mba") || !strcmp(desc->name, "modem")){
			ret = -EIO;
			goto release_fw;
		}
	}
#endif

	ret = pil_init_mmap(desc, mdt);
	if (ret)
		goto release_fw;

	desc->priv->unvoted_flag = 0;
	ret = pil_proxy_vote(desc);
	if (ret) {
		pil_err(desc, "Failed to proxy vote(rc:%d)\n", ret);
		goto release_fw;
	}

	trace_pil_event("before_init_image", desc);
	if (desc->ops->init_image)
		ret = desc->ops->init_image(desc, fw->data, fw->size);
	if (ret) {
		/* S2 mapping not yet done */
		desc->clear_fw_region = false;
		pil_err(desc, "Initializing image failed(rc:%d)\n", ret);
#if IS_ENABLED(CONFIG_SEC_PERIPHERAL_SECURE_CHK)
		secure_check_fail = true;
#endif
		goto err_boot;
	}

	trace_pil_event("before_mem_setup", desc);
	if (desc->ops->mem_setup)
		ret = desc->ops->mem_setup(desc, priv->region_start,
				priv->region_end - priv->region_start);
	if (ret) {
		/* S2 mapping is failed */
		desc->clear_fw_region = false;
		pil_err(desc, "Memory setup error(rc:%d)\n", ret);
		goto err_deinit_image;
	}

	if (desc->subsys_vmid > 0) {
		/**
		 * In case of modem ssr, we need to assign memory back to linux.
		 * This is not true after cold boot since linux already owns it.
		 * Also for secure boot devices, modem memory has to be released
		 * after MBA is booted
		 */
		trace_pil_event("before_assign_mem", desc);
		if (desc->modem_ssr) {
			ret = pil_assign_mem_to_linux(desc, priv->region_start,
				(priv->region_end - priv->region_start));
			if (ret)
				pil_err(desc, "Failed to assign to linux, ret- %d\n",
								ret);
		}
		ret = pil_assign_mem_to_subsys_and_linux(desc,
				priv->region_start,
				(priv->region_end - priv->region_start));
		if (ret) {
			pil_err(desc, "Failed to assign memory, ret - %d\n",
								ret);
			goto err_deinit_image;
		}
		hyp_assign = true;
	}

	trace_pil_event("before_load_seg", desc);

	/**
	 * Fallback to serial loading of blobs if the
	 * workqueue creatation failed during module init.
	 */
	if (pil_wq && !(desc->sequential_loading)) {
		ret = pil_load_segs(desc);
		if (ret)
			goto err_deinit_image;
	} else {
		list_for_each_entry(seg, &desc->priv->segs, list) {
			ret = pil_load_seg(desc, seg);
			if (ret)
				goto err_deinit_image;
		}
	}

	if (desc->subsys_vmid > 0) {
		trace_pil_event("before_reclaim_mem", desc);
		ret =  pil_reclaim_mem(desc, priv->region_start,
				(priv->region_end - priv->region_start),
				desc->subsys_vmid);
		if (ret) {
			pil_err(desc, "Failed to assign %s memory, ret - %d\n",
							desc->name, ret);
			goto err_deinit_image;
		}
		hyp_assign = false;
	}

	trace_pil_event("before_auth_reset", desc);
	notify_before_auth_and_reset(desc->dev);
	ret = desc->ops->auth_and_reset(desc);
	if (ret) {
		pil_err(desc, "Failed to bring out of reset(rc:%d)\n", ret);
#if IS_ENABLED(CONFIG_SEC_PERIPHERAL_SECURE_CHK)
		secure_check_fail = true;
#endif
		goto err_auth_and_reset;
	}
	trace_pil_event("reset_done", desc);

#ifdef CONFIG_QGKI_MSM_BOOT_TIME_MARKER
	if (!strcmp(desc->name, "modem"))
		update_marker("M - Modem out of reset");
#endif

	pil_info(desc, "Brought out of reset\n");
	desc->modem_ssr = false;
err_auth_and_reset:
#if IS_ENABLED(CONFIG_SEC_PERIPHERAL_SECURE_CHK)
	if (IS_ENABLED(CONFIG_SEC_PERIPHERAL_SECURE_CHK) &&
			secure_check_fail && (ret == -EINVAL) &&
#ifdef HDM_SUPPORT
			(!strcmp(desc->name, "mba") || (!strcmp(desc->name, "modem") && !hdm_cp_support)))
#else
			(!strcmp(desc->name, "mba") || !strcmp(desc->name, "modem")))
#endif

		sec_peripheral_secure_check_fail();
#endif
	if (ret && desc->subsys_vmid > 0) {
		pil_assign_mem_to_linux(desc, priv->region_start,
				(priv->region_end - priv->region_start));
		mem_protect = true;
	}
err_deinit_image:
	if (ret && desc->ops->deinit_image)
		desc->ops->deinit_image(desc);
err_boot:
	if (ret && desc->proxy_unvote_irq)
		disable_irq(desc->proxy_unvote_irq);
	pil_proxy_unvote(desc, ret);
release_fw:
	release_firmware(fw);
out:
	up_read(&pil_pm_rwsem);
	if (ret) {
		if (priv->is_region_allocated) {
			if (desc->subsys_vmid > 0 && !mem_protect &&
					hyp_assign) {
				pil_reclaim_mem(desc, priv->region_start,
					(priv->region_end -
						priv->region_start),
					VMID_HLOS);
			}
			if (desc->clear_fw_region && priv->region_start)
				pil_clear_segment(desc);
			priv->is_region_allocated = false;
		}
		pil_release_mmap(desc);
		pil_notify_aop(desc, "off");
#if IS_ENABLED(CONFIG_SEC_PERIPHERAL_SECURE_CHK)
		if (IS_ENABLED(CONFIG_SEC_PERIPHERAL_SECURE_CHK) &&
				secure_check_fail && (ret == -EINVAL) &&
#ifdef HDM_SUPPORT
				(!strcmp(desc->name, "mba") || (!strcmp(desc->name, "modem") && !hdm_cp_support)))
#else
				(!strcmp(desc->name, "mba") || !strcmp(desc->name, "modem")))
#endif
			sec_peripheral_secure_check_fail();
#endif
	}
	return ret;
}
EXPORT_SYMBOL(pil_boot);

/**
 * pil_shutdown() - Shutdown a peripheral
 * @desc: descriptor from pil_desc_init()
 */
void pil_shutdown(struct pil_desc *desc)
{
	int ret;
	struct pil_priv *priv = desc->priv;

	if (desc->ops->shutdown) {
		if (desc->ops->shutdown(desc))
			desc->shutdown_fail = true;
		else
			desc->shutdown_fail = false;
	}

	if (desc->proxy_unvote_irq) {
		disable_irq(desc->proxy_unvote_irq);
		if (!desc->priv->unvoted_flag)
			pil_proxy_unvote(desc, 1);
	} else if (!proxy_timeout_ms)
		pil_proxy_unvote(desc, 1);
	else
		flush_delayed_work(&priv->proxy);
	ret = pil_notify_aop(desc, "off");
	if (ret < 0)
		pr_warn("pil: failed to send OFF message to AOP rc:%d\n", ret);
	desc->modem_ssr = true;
}
EXPORT_SYMBOL(pil_shutdown);

/**
 * pil_free_memory() - Free memory resources associated with a peripheral
 * @desc: descriptor from pil_desc_init()
 */
void pil_free_memory(struct pil_desc *desc)
{
	struct pil_priv *priv = desc->priv;

	if (priv->is_region_allocated) {
		if (desc->subsys_vmid > 0)
			pil_assign_mem_to_linux(desc, priv->region_start,
				(priv->region_end - priv->region_start));
		priv->is_region_allocated = false;
	}
}
EXPORT_SYMBOL(pil_free_memory);

static DEFINE_IDA(pil_ida);

bool pil_is_timeout_disabled(void)
{
	return disable_timeouts;
}
EXPORT_SYMBOL(pil_is_timeout_disabled);

static int collect_aux_minidump_ids(struct pil_desc *desc)
{
	u32 id;
	const __be32 *p;
	struct property *prop;
	unsigned int i = 0;
	void *aux_toc_addr;
	struct device_node *node = desc->dev->of_node;
	int num_ids = of_property_count_u32_elems(node,
						 "qcom,aux-minidump-ids");

	if (num_ids > 0) {
		desc->num_aux_minidump_ids = num_ids;
		desc->aux_minidump_ids = kmalloc_array(num_ids,
						sizeof(*desc->aux_minidump_ids),
						GFP_KERNEL);
		if (!desc->aux_minidump_ids)
			return -ENOMEM;

		desc->aux_minidump = kmalloc_array(num_ids,
					     sizeof(*desc->aux_minidump),
					     GFP_KERNEL);
		if (!desc->aux_minidump) {
			kfree(desc->aux_minidump_ids);
			desc->aux_minidump_ids = NULL;
			return -ENOMEM;
		}

		of_property_for_each_u32(node, "qcom,aux-minidump-ids", prop,
					 p, id) {
			desc->aux_minidump_ids[i] = id;
			aux_toc_addr = &g_md_toc->md_ss_toc[id];
			pr_debug("Minidump: aux_toc_addr is %pa and id: %d\n",
				 &aux_toc_addr, id);
			memcpy(&desc->aux_minidump[i], &aux_toc_addr,
			       sizeof(aux_toc_addr));
			i++;
		}
	}

	return 0;
}

/**
 * pil_desc_init() - Initialize a pil descriptor
 * @desc: descriptor to initialize
 *
 * Initialize a pil descriptor for use by other pil functions. This function
 * must be called before calling pil_boot() or pil_shutdown().
 *
 * Returns 0 for success and -ERROR on failure.
 */
int pil_desc_init(struct pil_desc *desc)
{
	struct pil_priv *priv;
	void __iomem *addr;
	void *ss_toc_addr;
	int ret;
	size_t size;
	char buf[sizeof(priv->info->name)];
	struct device_node *ofnode = desc->dev->of_node;

	if (WARN(desc->ops->proxy_unvote && !desc->ops->proxy_vote,
				"Invalid proxy voting. Ignoring\n"))
		((struct pil_reset_ops *)desc->ops)->proxy_unvote = NULL;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	desc->priv = priv;
	priv->desc = desc;

	priv->id = ret = ida_simple_get(&pil_ida, 0, PIL_NUM_DESC, GFP_KERNEL);
	if (priv->id < 0)
		goto err;

	if (pil_info_base) {
		addr = pil_info_base + sizeof(struct pil_image_info) * priv->id;
		priv->info = (struct pil_image_info __iomem *)addr;

		strlcpy(buf, desc->name, sizeof(buf));
		__iowrite32_copy(priv->info->name, buf, sizeof(buf) / 4);
	}
	if (of_property_read_u32(ofnode, "qcom,minidump-id",
		&desc->minidump_id))
		pr_warn("minidump-id not found for %s\n", desc->name);
	else {
		if (IS_ERR_OR_NULL(g_md_toc)) {
			/* Get Global minidump ToC*/
			g_md_toc = qcom_smem_get(QCOM_SMEM_HOST_ANY,
				SBL_MINIDUMP_SMEM_ID, &size);
			if (PTR_ERR(g_md_toc) == -EPROBE_DEFER) {
				g_md_toc = NULL;
				pr_err("SMEM is not initialized.\n");
			}
		}
		if (g_md_toc && g_md_toc->md_toc_init == true) {
			ss_toc_addr = &g_md_toc->md_ss_toc[desc->minidump_id];
			pr_debug("Minidump : ss_toc_addr for ss is %pa and desc->minidump_id is %d\n",
				&ss_toc_addr, desc->minidump_id);
			memcpy(&desc->minidump_ss, &ss_toc_addr,
			       sizeof(ss_toc_addr));

			if (collect_aux_minidump_ids(desc) < 0)
				pr_err("Failed to get aux %s minidump ids\n",
				       desc->name);
		}
	}

	ret = pil_parse_devicetree(desc);
	if (ret)
		goto err_parse_dt;

	/* Ignore users who don't make any sense */
	WARN(desc->ops->proxy_unvote && desc->proxy_unvote_irq == 0
		 && !desc->proxy_timeout,
		 "Invalid proxy unvote callback or a proxy timeout of 0 was specified or no proxy unvote IRQ was specified.\n");

	if (desc->proxy_unvote_irq) {
		ret = request_threaded_irq(desc->proxy_unvote_irq,
				  NULL,
				  proxy_unvote_intr_handler,
				  IRQF_ONESHOT | IRQF_TRIGGER_RISING,
				  desc->name, desc);
		if (ret < 0) {
			dev_err(desc->dev,
				"Unable to request proxy unvote IRQ: %d\n",
				ret);
			goto err;
		}
		disable_irq(desc->proxy_unvote_irq);
	}

	snprintf(priv->wname, sizeof(priv->wname), "pil-%s", desc->name);

	priv->ws = wakeup_source_register(desc->dev, priv->wname);
	if (!priv->ws) {
		return -ENOMEM;
		goto err;
	}

	INIT_DELAYED_WORK(&priv->proxy, pil_proxy_unvote_work);
	INIT_LIST_HEAD(&priv->segs);

	/* Make sure mapping functions are set. */
	if (!desc->map_fw_mem)
		desc->map_fw_mem = map_fw_mem;

	if (!desc->unmap_fw_mem)
		desc->unmap_fw_mem = unmap_fw_mem;

	desc->minidump_as_elf32 = of_property_read_bool(
					ofnode, "qcom,minidump-as-elf32");

	return 0;
err_parse_dt:
	ida_simple_remove(&pil_ida, priv->id);
err:
	kfree(desc->aux_minidump);
	kfree(desc->aux_minidump_ids);
	kfree(priv);
	return ret;
}
EXPORT_SYMBOL(pil_desc_init);

/**
 * pil_desc_release() - Release a pil descriptor
 * @desc: descriptor to free
 */
void pil_desc_release(struct pil_desc *desc)
{
	struct pil_priv *priv = desc->priv;

	if (priv) {
		ida_simple_remove(&pil_ida, priv->id);
		flush_delayed_work(&priv->proxy);
		wakeup_source_unregister(priv->ws);
	}
	desc->priv = NULL;
	kfree(priv);
}
EXPORT_SYMBOL(pil_desc_release);

static int pil_pm_notify(struct notifier_block *b, unsigned long event, void *p)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
		down_write(&pil_pm_rwsem);
		break;
	case PM_POST_SUSPEND:
		up_write(&pil_pm_rwsem);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block pil_pm_notifier = {
	.notifier_call = pil_pm_notify,
};
static void __iomem * __init pil_get_resource(const char *prop,
					resource_size_t *out_size)
{
	void __iomem *base_addr;
	struct device_node *np;
	struct resource res;

	np = of_find_compatible_node(NULL, NULL, prop);
	if (!np) {
		pr_warn("pil: failed to find %s node\n", prop);
		return NULL;
	}

	if (of_address_to_resource(np, 0, &res)) {
		pr_warn("pil: address to resource for %s failed\n", prop);
		return NULL;
	}

	base_addr = ioremap(res.start, resource_size(&res));
	if (!base_addr) {
		pr_warn("pil: could not map region for %s\n", prop);
		return NULL;
	}

	if (out_size)
		*out_size = resource_size(&res);

	return base_addr;
}
static int __init msm_pil_init(void)
{
	resource_size_t res_size;
	int i;

	pil_info_base = pil_get_resource("qcom,msm-imem-pil", &res_size);
	if (pil_info_base) {
		for (i = 0; i < res_size / sizeof(u32); i++)
			writel_relaxed(0, pil_info_base + (i * sizeof(u32)));
	}

	pil_disable_timeout_base =
		pil_get_resource("qcom,msm-imem-pil-disable-timeout", NULL);
	if (pil_disable_timeout_base) {
		if (__raw_readl(pil_disable_timeout_base) == 0x53444247) {
			pr_info("pil: pil-imem set to disable pil timeouts\n");
			disable_timeouts = true;
		}

		iounmap(pil_disable_timeout_base);
	}

	pil_wq = alloc_workqueue("pil_workqueue", WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (!pil_wq)
		pr_warn("pil: Defaulting to sequential firmware loading.\n");

	return register_pm_notifier(&pil_pm_notifier);
}
subsys_initcall(msm_pil_init);

static void __exit msm_pil_exit(void)
{
	if (pil_wq)
		destroy_workqueue(pil_wq);
	unregister_pm_notifier(&pil_pm_notifier);
	if (pil_info_base)
		iounmap(pil_info_base);
}
module_exit(msm_pil_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Load peripheral images and bring peripherals out of reset");
