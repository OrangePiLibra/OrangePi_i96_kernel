#include <linux/module.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/semaphore.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <asm/sizes.h>
#include <asm/unaligned.h>
#include <mach/hardware.h>
#include <mach/irqs.h>
#include <mach/ifc.h>
#include <mach/timex.h>
#include <mach/rda_clk_name.h>
#include <plat/devices.h>
#include <plat/reg_ifc.h>
#include <plat/reg_nand.h>
#include <plat/rda_debug.h>
#include <plat/cpu.h>

#include <rda/tgt_ap_board_config.h>
#ifdef _TGT_AP_NAND_V1_HIGHDENSITY
/*
 * performance counter is used to measure memcpy from NAND controller buffer to ram
 * typical time for copying 4224 bytes is 9us to 19us
 * so DMA isn't needed
 * #define CONFIG_MTD_NAND_RDA_DMA
 * */
#ifdef CONFIG_MTD_NAND_RDA_DMA
#include <plat/dma.h>
#endif /* CONFIG_MTD_NAND_RDA_DMA */

#define NAND_CMD_TIMEOUT_MS    ( 2000 )
#define NAND_DMA_TIMEOUT_MS    ( 100 )

#define NAND_STATISTIC

#define PROCNAME "driver/nand_hd"

#define FIX_CHIP_DMA_CROSS_MEM_PAGE_BUG
#define DMA_NEED_PERIOD(addr, len) \
	( (!IS_ALIGNED((addr), 32)) && (((addr + len - 1) & PAGE_MASK) != (addr & PAGE_MASK)) )
#define NAND_CONTROLLER_TEMP_BUFFER_SIZE	4320

#define __FTL_OOB_STRUCT_LENGTH__ 12  //bytes, when FTL oob struct add member, this sould increase.
#define OOB_SW_PROTECT_SWITCH_ON 1

/*
 * Default mtd partitions, used when mtdparts= not present in cmdline
 */
static struct mtd_partition partition_info[] = {
	{
	 .name = "reserved",
	 .offset = 0,.size = 24 * SZ_1M},
	{
	 .name = "customer",
	 .offset = 24 * SZ_1M,.size = SZ_8M},
	{
	 .name = "system",
	 .offset = SZ_32M,.size = 192 * SZ_1M},
	{
	 .name = "cache",
	 .offset = 224 * SZ_1M,.size = SZ_8M},
	{
	 .name = "userdata",
	 .offset = 232 * SZ_1M,.size = 280 * SZ_1M},
};

static struct nand_ecclayout rda_nand_oob_8 = {
	.eccbytes = 0,
	.eccpos = {},
	.oobfree = {
		{.offset = 2,
		 .length = 6} }
};

static struct nand_ecclayout rda_nand_oob_16 = {
	.eccbytes = 0,
	.eccpos = {},
	.oobfree = {
		{.offset = 2,
		 .length = 14} }
};

static struct nand_ecclayout rda_nand_oob_64 = {
	.eccbytes = 0,
	.eccpos = {},
	.oobfree = {
		{.offset = 2,
		 .length = 62} }
};

static struct nand_ecclayout rda_nand_oob_128 = {
	.eccbytes = 0,
	.eccpos = {},
	.oobfree = {
		{.offset = 2,
		 .length = 126} }
};

static struct nand_ecclayout rda_nand_oob_256 = {
	.eccbytes = 0,
	.eccpos = {},
	.oobfree = {
		{.offset = 2,
		 .length = 254} }
};

static struct nand_ecclayout rda_nand_oob_512 = {
	.eccbytes = 0,
	.eccpos = {},
	.oobfree = {
		{.offset = 2,
		 .length = 510} }
};

struct rda_nand_info {
	struct nand_chip nand_chip;
	struct mtd_info mtd;

	struct rda_nand_device_data *plat_data;
	struct device *dev;
	NAND_FLASH_TYPE type;
	int hard_ecc_hec;
	int bus_width_16;

	int cmd;
	int col_addr;
	int page_addr;
	int read_ptr;
	u32 byte_buf[4];	// for read_id, status, etc
	int ecc_bits;
	int ecc_bits_supported;
	int index;
	int write_ptr;

	void __iomem *base;
	void __iomem *reg_base;
	void __iomem *data_base;
//#ifdef CONFIG_MTD_NAND_RDA_DMA
	dma_addr_t phy_addr;
	/* For waiting for dma */
	struct completion comp;
	u8 dma_ch;
//#else
	u8 *databuf;
//#endif/* CONFIG_MTD_NAND_RDA_DMA */

#ifndef NAND_IRQ_POLL
	u32 nand_status;
	struct completion nand_comp;
#endif/* NAND_IRQ_POLL */

	struct clk *master_clk;
	unsigned long clk;
	int cmd_flag;

	/*8810:8K/16k setting*/
	/*nand_use_type = 1: 8K used as 2 4k or 3K*/
	/*nand_use_type = 3: 16K used as 4 4k or 3K*/
	/*nand_use_type = 0: 2K or  4k*/
	u8  nand_use_type;
	int logic_page_size;
	int logic_oob_size;
	int logic_operate_time;

#ifdef NAND_STATISTIC
	u64 bytes_read;
	u64 bytes_write;
	u64 ecc_flips;
	u64 read_fails;
	u64 write_fails;
#endif
};

#if (OOB_SW_PROTECT_SWITCH_ON == 1)
static uint32_t nand_checksum32( unsigned char* data, uint32_t data_size)
{
	uint32_t checksum = 0;
	uint32_t i;

	for(i = 0; i < data_size; i++)
		checksum = ((checksum << 31) | (checksum >> 1)) + (uint32_t)data[i];

	return checksum;
}

struct select_data{
	u8 data;
	u8 count;
};

static void nand_sw_ecc_check(u8 * buf, u8 *dst, u8 copy_number, u8 array_length)
{
	u8 i, j, k, m;
	struct select_data select[30];
	u8 temp;

	for(i = 0; i < array_length; i++){
		m = 0;
		select[m].data = buf[i];
		select[m].count = 1;
		for(j = 1; j < copy_number; j++){
			k = 0;
			while(1){
				if(select[k].data == buf[i + j * (array_length + 4)]){
					select[k].count++;
					break;
				}

				if(k >= m){
					select[m+1].data = buf[i + j * (array_length + 4)];
					select[m+1].count = 1;
					m++;
					break;
				}
				k++;
			}
		}

		temp = select[0].count;
		dst[i] = select[0].data;
		for(j = 1; j <= m; j++){
			if(select[j].count > temp){
				dst[i] = select[j].data;
				temp = select[j].count;
			}
		}
	}
}
#endif

static void hal_send_cmd(struct rda_nand_info *info,
				unsigned char cmd, unsigned int page_addr)
{
	unsigned long cmd_reg;

	cmd_reg = NANDFC_DCMD(cmd) | NANDFC_PAGE_ADDR(page_addr);

	rda_dbg_nand("  hal_send_cmd 0x%08lx\n", cmd_reg);

	__raw_writel(cmd_reg, info->reg_base + NANDFC_REG_DCMD_ADDR);
}

static void hal_set_col_addr(struct rda_nand_info *info,
				unsigned int col_addr)
{
	__raw_writel(col_addr, info->reg_base + NANDFC_REG_COL_ADDR);
}

static void hal_set_col_cmd(struct rda_nand_info *info, unsigned int col_cmd)
{
	__raw_writel(0x50010000 |col_cmd, info->reg_base + NANDFC_REG_CMD_DEF_B);
}

static void hal_flush_buf(struct rda_nand_info *info)
{
	__raw_writel(0x07, info->reg_base + NANDFC_REG_BUF_CTRL);
}

#ifdef NAND_IRQ_POLL
static u32 hal_wait_cmd_complete(struct rda_nand_info *info)
{
	unsigned long int_stat;
	unsigned long timeout;

	timeout = jiffies + msecs_to_jiffies(NAND_TIMEOUT_MS);
	/* wait done */
	do {
		int_stat = __raw_readl(info->reg_base + NANDFC_REG_INT_STAT);
	} while (!(int_stat & NANDFC_INT_DONE) && time_before(jiffies, timeout));

	/* to clear */
	__raw_writel((int_stat & NANDFC_INT_CLR_MASK),
			info->reg_base + NANDFC_REG_INT_STAT);

	if (time_after(jiffies, timeout)) {
		dev_err(info->dev, "cmd timeout\n");
		return -ETIME;
	}

	if (int_stat & NANDFC_INT_ERR_ALL) {
		dev_err(info->dev, "int error, int_stat = %lx\n", int_stat);
		return (int_stat & NANDFC_INT_ERR_ALL);
	}

	return 0;
}
#else
static void hal_irq_enable(struct rda_nand_info *info)
{
	__raw_writel(0x10, info->reg_base + NANDFC_REG_INT_MASK);
	return;
}

static void hal_irq_disable(struct rda_nand_info *info)
{
	__raw_writel(0x0, info->reg_base + NANDFC_REG_INT_MASK);
	return;
}

static void hal_irq_clear(struct rda_nand_info *info, u32 int_status)
{
	__raw_writel((int_status & NANDFC_INT_CLR_MASK),
			info->reg_base + NANDFC_REG_INT_STAT);
	return;
}

static u32 hal_irq_get_status(struct rda_nand_info *info)
{
	return __raw_readl(info->reg_base + NANDFC_REG_INT_STAT);
}
#endif /* NAND_IRQ_POLL */

static u32 hal_get_flipbits(struct rda_nand_info *info)
{
	u32 raw;
	raw = __raw_readl(info->reg_base + NANDFC_REG_CMD_DEF_B);
	return (raw & 0xFE000000) >> 25;
}

static unsigned long rda_nand_cld_div_tbl[16] = {
	3, 4, 5, 6, 7, 8, 9, 10,
	12, 14, 16, 18, 20, 22, 24, 28
};

static unsigned long hal_calc_divider(struct rda_nand_info *info)
{
	unsigned long mclk = clk_get_rate(info->master_clk);
	unsigned long div, clk_div;
	int i;

	div = mclk / info->clk;
	if (mclk % info->clk)
		div += 1;

	if (div < 7) {
		/* 7 is minimal divider by hardware */
		dev_warn(info->dev, "clk %d is too high, use min_div = 7\n",
			(int)info->clk);
		div = 7;
	}

	for (i=0;i<16;i++) {
		if(div <= rda_nand_cld_div_tbl[i]) {
			clk_div = i;
			break;
		}
	}
	if (i>=16) {
		dev_warn(info->dev, "clk %d is too low, use max_div = 28\n",
			(int)info->clk);
		clk_div = 15;
	}

	dev_info(info->dev, "set clk %d, bus_clk = %d, divider = %d\n",
		(int)info->clk, (int)mclk, (int)clk_div);

	return clk_div;
}

static int hal_init(struct rda_nand_info *info, int nand_type)
{
	unsigned long config_a;
	unsigned long config_b;
	unsigned long clk_div = hal_calc_divider(info);

	rda_dbg_nand("hal_init, nand_type = %d\n", nand_type);

	/* setup config_a and config_b */
	config_b = 0;
	config_a = NANDFC_CYCLE(clk_div);
	config_a |= NANDFC_CHIP_SEL(0x0e);
	config_a |= NANDFC_POLARITY_IO(0);	// set 0 invert IO
	config_a |= NANDFC_POLARITY_MEM(1);	// set 1 invert MEM

	switch (nand_type) {
	case NAND_TYPE_512S:
	case NAND_TYPE_512L:
		dev_err(info->dev, "not support 512 nand anymore\n");
		return -EINVAL;
	case NAND_TYPE_2KS:
		config_a |= NANDFC_TIMING(0x7B);
		break;
	case NAND_TYPE_2KL:
	case NAND_TYPE_MLC2K:
		config_a |= NANDFC_TIMING(0x8B);
		break;
	case NAND_TYPE_MLC4K:
	case NAND_TYPE_SLC4K:
		config_a |= NANDFC_TIMING(0x8C);
		break;
	case NAND_TYPE_MLC8K:
	case NAND_TYPE_SLC8K:
		config_a |= NANDFC_TIMING(0x8c);
		config_b |= 0x20;
		break;
	case NAND_TYPE_MLC16K:
	case NAND_TYPE_SLC16K:
		config_a |= NANDFC_TIMING(0x8c) | (0x00 << 20);
		config_b |= 0x10;
		break;
	default:
		dev_err(info->dev, "invalid nand_type %d\n", nand_type);
		return -EINVAL;
	}

	/* set 16bit mode if needed */
	if (info->bus_width_16)
		config_a |= NANDFC_WDITH_16BIT(1);

	config_b |= NANDFC_HWECC(1);
	if (info->hard_ecc_hec)
		config_b |= NANDFC_ECC_MODE(0x02);

	__raw_writel(config_a, info->reg_base + NANDFC_REG_CONFIG_A);
	__raw_writel(config_b, info->reg_base + NANDFC_REG_CONFIG_B);

	/* set readid type, 0x06 for 8 bytes ID */
	__raw_writel(0x06, info->reg_base + NANDFC_REG_IDTPYE);

#ifdef _TGT_AP_NAND_READDELAY
	{
	unsigned int delay;
	/* Set an interval of filter for erasing operation. */
	delay = __raw_readl(info->reg_base + NANDFC_REG_DELAY);
	delay |= _TGT_AP_NAND_READDELAY;
	__raw_writel(delay, info->reg_base + NANDFC_REG_DELAY);
	}
#endif /* #if _TGT_AP_NAND_READDELAY */

	dev_info(info->dev, "nand init, %08lx %08lx\n",
		config_a, config_b);

	return 0;
}

#ifndef NAND_IRQ_POLL
static irqreturn_t nand_rda_irq_handler(int irq, void *data)
{
	struct rda_nand_info *info = (struct rda_nand_info *)data;
	u32 int_status;

	/* get and clear interrupts */
	int_status = hal_irq_get_status(info);
	hal_irq_clear(info, int_status);

	if (int_status & NANDFC_INT_ERR_ALL) {
		info->nand_status = int_status & NANDFC_INT_ERR_ALL;
	} else if (int_status & NANDFC_INT_DONE) {
		info->nand_status = 0;
	} else {
		/* Nothing to do */
	}

	complete(&info->nand_comp);

	return IRQ_HANDLED;
}
#endif /* NAND_IRQ_POLL */

static u8 nand_rda_read_byte(struct mtd_info *mtd)
{
	u8 ret;
	struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;
	u8 *ptr = (u8 *) info->byte_buf;

	ret = ptr[info->index];
	info->index++;

	rda_dbg_nand("nand_read_byte, ret = %02x\n", ret);

	return ret;
}

static u16 nand_rda_read_word(struct mtd_info *mtd)
{
	u16 ret;
	struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;
	u16 *ptr = (u16 *) info->byte_buf;

	ret = ptr[info->index];
	info->index += 2;

	rda_dbg_nand("nand_read_word, ret = %04x\n", ret);

	return ret;
}

static inline void __nand_rda_copyfrom_iomem(void *dst_buf, void *io_mem,
					     size_t size)
{
	memcpy(dst_buf, io_mem, size);
	return;
}

#ifdef CONFIG_MTD_NAND_RDA_DMA
static void nand_rda_dma_move_data(struct mtd_info *mtd,
		uint8_t * dst,
		uint8_t * src,
		int len,
		enum dma_data_direction dir)
{
	struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;
	struct rda_dma_chan_params dma_param;
	dma_addr_t dst_phys_addr, src_phys_addr;
	void *dst_addr = (void *)dst;
	void *src_addr = (void *)src;
	int ret = 0;

	if (((u32)dst_addr & 0x7) != 0 || ((u32)src_addr & 0x7) != 0) {
		pr_info("ERROR, nand dma read buffer dst %p or src %p is not 8bytes aligned\n",
			dst_addr, src_addr);
		return;
	}

	if(dir == DMA_FROM_DEVICE){
		dst_phys_addr = dma_map_single(info->dev, dst_addr, len, DMA_FROM_DEVICE);
		if (dma_mapping_error(info->dev, dst_phys_addr)) {
			dev_err(info->dev, "failed to dma_map_single\n");
			return;
		}
		src_phys_addr = (dma_addr_t)src;
	}
	else if(dir == DMA_TO_DEVICE){
		dst_phys_addr = (dma_addr_t)dst;
		src_phys_addr = dma_map_single(info->dev, src_addr, len, DMA_TO_DEVICE);
		if (dma_mapping_error(info->dev, src_phys_addr)) {
			dev_err(info->dev, "failed to dma_map_single\n");
			return;
		}
	}
	else{
		dst_phys_addr = dma_map_single(info->dev, dst_addr, len, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(info->dev, dst_phys_addr)) {
			dev_err(info->dev, "failed to dma_map_single\n");
			return;
		}
		src_phys_addr = dma_map_single(info->dev, src_addr, len, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(info->dev, src_phys_addr)) {
			dev_err(info->dev, "failed to dma_map_single\n");
			return;
		}
	}

	dma_param.src_addr = src_phys_addr;
	dma_param.dst_addr = dst_phys_addr;
	dma_param.xfer_size = len;
	//dma_param.dma_mode = RDA_DMA_FR_MODE;
	dma_param.dma_mode = RDA_DMA_NOR_MODE;
#ifdef NAND_DMA_POLLING
	dma_param.enable_int = 0;
#else
	dma_param.enable_int = 1;
#endif /* NAND_DMA_POLLING */

	ret = rda_set_dma_params(info->dma_ch, &dma_param);
	if (ret < 0) {
		pr_info("rda nand : DMA failed to set parameter!\n");
		if(dir == DMA_FROM_DEVICE)
			dma_unmap_single(info->dev, dst_phys_addr, len, DMA_FROM_DEVICE);
		else if(dir == DMA_TO_DEVICE)
			dma_unmap_single(info->dev, src_phys_addr, len, DMA_TO_DEVICE);
		else{
			dma_unmap_single(info->dev, dst_phys_addr, len, DMA_BIDIRECTIONAL);
			dma_unmap_single(info->dev, src_phys_addr, len, DMA_BIDIRECTIONAL);
		}
		return;
	}

	init_completion(&info->comp);

	/* use flush to avoid annoying unaligned warning */
	/* however, invalidate after the dma it the right thing to do */
	/*
	if(dir == DMA_FROM_DEVICE)
		flush_dcache_range((u32)dst_addr, (u32)(dst_addr + len));
	else if(dir == DMA_TO_DEVICE)
		flush_dcache_range((u32)src_addr, (u32)(src_addr + len));
	else{
		flush_dcache_range((u32)dst_addr, (u32)(dst_addr + len));
		flush_dcache_range((u32)src_addr, (u32)(src_addr + len));
	}
	*/
	rda_start_dma(info->dma_ch);
#ifndef NAND_DMA_POLLING
	ret = wait_for_completion_timeout(&info->comp,
				msecs_to_jiffies(NAND_DMA_TIMEOUT_MS));
	if (ret <= 0) {
		dev_err(info->dev, "dma read timeout, ret = 0x%08x\n", ret);

		if(dir == DMA_FROM_DEVICE)
			dma_unmap_single(info->dev, dst_phys_addr, len, DMA_FROM_DEVICE);
		else if(dir == DMA_TO_DEVICE)
			dma_unmap_single(info->dev, src_phys_addr, len, DMA_TO_DEVICE);
		else{
			dma_unmap_single(info->dev, dst_phys_addr, len, DMA_BIDIRECTIONAL);
			dma_unmap_single(info->dev, src_phys_addr, len, DMA_BIDIRECTIONAL);
		}

		return;
	}
#else
	rda_poll_dma(info->dma_ch);
	rda_stop_dma(info->dma_ch);
#endif /* #if 0 */

	/* use flush to avoid annoying unaligned warning */
	//invalidate_dcache_range((u32)addr, (u32)(addr + len));

	/* Free the specified physical address */
	if(dir == DMA_FROM_DEVICE)
		dma_unmap_single(info->dev, dst_phys_addr, len, DMA_FROM_DEVICE);
	else if(dir == DMA_TO_DEVICE)
		dma_unmap_single(info->dev, src_phys_addr, len, DMA_TO_DEVICE);
	else{
		dma_unmap_single(info->dev, dst_phys_addr, len, DMA_BIDIRECTIONAL);
		dma_unmap_single(info->dev, src_phys_addr, len, DMA_BIDIRECTIONAL);
	}
}
#endif/*CONFIG_MTD_NAND_RDA_DMA*/

static void nand_rda_logic_read_cache(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;
#ifdef NAND_IRQ_POLL
	unsigned long cmd_ret;
#else
	unsigned long timeout;
#endif /* NAND_IRQ_POLL */

	hal_set_col_addr(info, info->col_addr);
	init_completion(&info->nand_comp);
	hal_send_cmd(info, (unsigned char)info->cmd, info->page_addr);
#ifdef NAND_IRQ_POLL
	cmd_ret = hal_wait_cmd_complete(info);
	if (cmd_ret) {
		dev_err(info->dev, "nand_rda_logic_read_cache:read cmd fail, cmd = %x,"
			" page_addr = %x, col_addr = %x\n",
			info->cmd, info->page_addr, info->col_addr);
	}
#else
	timeout = wait_for_completion_timeout(&info->nand_comp,
					      msecs_to_jiffies(NAND_CMD_TIMEOUT_MS));
	if (timeout == 0) {
		dev_err(info->dev, "nand_rda_logic_read_cache: read cmd timeout, cmd = 0x%x,"
			" page_addr = 0x%x, col_addr = 0x%x\n",
			info->cmd, info->page_addr, info->col_addr);
	}

	if (info->nand_status) {
		dev_err(info->dev, "nand_rda_logic_read_cache:read cmd fail, cmd = 0x%x,"
			" page_addr = 0x%x, col_addr = 0x%x, status = 0x%x\n",
			info->cmd, info->page_addr, info->col_addr, info->nand_status);
	}
#endif /* NAND_IRQ_POLL */
}

static void nand_rda_read_cache(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;

	if(info->nand_use_type > 0){
#ifndef CONFIG_MTD_NAND_RDA_DMA
	u8 *nand_ptr = (u8*)info->data_base;
#else
	u8 *nand_ptr = (u8*)info->phy_addr;
#endif
	u8 *data_dst = (u8 *) (chip->IO_ADDR_R);
	u8 *oob_dst = data_dst + mtd->writesize;
	u8 *oob_src = ((u8*)info->data_base) + info->logic_page_size;
	u8 loop_count = 2;

	if(info->type == NAND_TYPE_SLC16K || info->type == NAND_TYPE_MLC16K)
		loop_count = 4;

		do {
			/*move data from controller buffer to middle buffer*/
#ifndef CONFIG_MTD_NAND_RDA_DMA
			memcpy((void *)data_dst,
					(void *)nand_ptr,
					info->logic_page_size);
#else
			nand_ptr = (u8*)info->phy_addr;
			nand_rda_dma_move_data(mtd,
					(uint8_t *)data_dst,
					nand_ptr,
					info->logic_page_size,
					DMA_FROM_DEVICE);
			nand_ptr = (u8*)info->data_base;
#endif
			/*move oob from controller buffer to middle buffer*/
			memcpy((void *)oob_dst,
					(void *)oob_src,
					info->logic_oob_size);

			if(loop_count > 1) {
				info->col_addr += NAND_CONTROLLER_TEMP_BUFFER_SIZE;
				hal_set_col_cmd(info, 0xe0);
				info->cmd = NAND_CMD_RNDOUT;
				nand_rda_logic_read_cache(mtd);

				data_dst += info->logic_page_size;
				oob_dst += info->logic_oob_size;
			}
		}while(--loop_count);
		hal_set_col_cmd(info, 0x30);
		hal_set_col_addr(info, 0);
	}
}

static void nand_rda_read_buf(struct mtd_info *mtd, uint8_t * buf, int len)
{
	struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;
	u8 *u_temp_ptr, *nand;

	if (info->nand_use_type > 0)
		u_temp_ptr = chip->IO_ADDR_R;
	else
		u_temp_ptr =  info->data_base;

		nand = u_temp_ptr + info->read_ptr;
#if 1//ndef CONFIG_MTD_NAND_RDA_DMA
	{
#if (OOB_SW_PROTECT_SWITCH_ON == 1)
		uint8_t oob_temp_ptr[NAND_MAX_OOBSIZE];
		uint8_t oob_byte_count = 2 + __FTL_OOB_STRUCT_LENGTH__ + 2 + 4;
		uint8_t oob_copy_number = info->mtd.oobsize / oob_byte_count;
		uint32_t checksum, checksum_load;
		u8 i;

		if(info->read_ptr > 0){
			if(info->mtd.writesize == 3072 || info->mtd.writesize == 6144
				|| info->mtd.writesize == 12288) {
				for(i = 0; i < oob_copy_number; i++){
					u32 index = i * oob_byte_count + oob_byte_count;

					checksum = nand_checksum32(&nand[i * oob_byte_count],
									oob_byte_count - 4);
					checksum_load = le32_to_cpu(get_unaligned((const u32 *)&nand[index-4]));

					if(checksum == checksum_load || checksum_load == ~0x0){
						memcpy(&oob_temp_ptr[0], &nand[i * oob_byte_count], oob_byte_count);
						break;
					}
				}
				if(i == oob_copy_number){
					pr_info("3k or 6k or 12k oob ecc error!!!! \n");
					nand_sw_ecc_check(nand, &oob_temp_ptr[0], oob_copy_number, oob_byte_count - 4);
				}

				nand = &oob_temp_ptr[0];
			}
		}
#endif
	}

	memcpy((void *)buf, (void *)nand, len);
	info->read_ptr += len;

	/* very verbose, not used normally
	 * if (rda_debug & RDA_DBG_NAND)
		rda_dump_buf((char *)buf, len);
	*/
#else
	struct rda_dma_chan_params dma_param;
	dma_addr_t phys_addr;
	dma_addr_t nand_phys_addr = info->phy_addr;
	void *addr = (void *)buf;
	int ret = 0;
#ifdef FIX_CHIP_DMA_CROSS_MEM_PAGE_BUG
	int __dma_periods_need = 0, __dma_bytes_done = 0, __dma_bytes_all = 0;
	unsigned int __dma_nand_align_len = 0, __dma_ram_align_len = 0;
#endif
	u8 *nand_ptr = (u8 *)(chip->IO_ADDR_R + info->read_ptr);
	struct page *p1;

	rda_dbg_nand("read_buf, buf addr = 0x%p, len = %d, read_ptr = %d\n",
			buf, len, info->read_ptr);

	/*
	 * If size is less than the size of oob,
	 * we copy directly them to mapping buffer.
	 */
	if (len <= info->mtd.oobsize) {
		__nand_rda_copyfrom_iomem(buf, (void *)nand_ptr, len);
		info->read_ptr = 0;
		return;
	}

	/*
	 * Check if the address of buffer is an address allocated by vmalloc.
	 * If so, we don't directly use these address, it might be un-continous
	 * in physical address. We have to copy them via memcpy.
	 */
	if (addr >= high_memory) {
		if (((size_t) addr & PAGE_MASK) !=
		    ((size_t) (addr + len - 1) & PAGE_MASK)) {
			goto out_copy;
		}

		p1 = vmalloc_to_page(addr);
		if (!p1) {
			goto out_copy;
		}
		addr = page_address(p1) + ((size_t) addr & ~PAGE_MASK);
	}

	phys_addr = dma_map_single(info->dev, addr, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(info->dev, phys_addr)) {
		dev_err(info->dev, "failed to dma_map_single\n");
		return;
	}

#ifdef FIX_CHIP_DMA_CROSS_MEM_PAGE_BUG
	/*
	 * Chip bug: dma cross memory page
	 * We should guarantee dma do NOT cross memory page (PAGE_SIZE)
	 */
	__dma_bytes_all = len;
	__dma_bytes_done = 0;
	__dma_periods_need = 0;
	if (DMA_NEED_PERIOD(phys_addr, len)) {
		__dma_periods_need = 1;
	}

	while (__dma_bytes_done < __dma_bytes_all) {
		// need period dma?
		if (__dma_periods_need) {
			nand_phys_addr = (unsigned int)nand_phys_addr + __dma_bytes_done;
			phys_addr = (unsigned int)phys_addr + __dma_bytes_done;
			// min of (nand reg next memory page - nand_phys_addr) and (ram next memory page - phys_addr)
			__dma_ram_align_len = (unsigned int)((phys_addr + PAGE_SIZE) & PAGE_MASK) - (unsigned int)phys_addr;
			__dma_nand_align_len = (unsigned int)((nand_phys_addr + PAGE_SIZE) & PAGE_MASK) - (unsigned int)nand_phys_addr;
			len = __dma_ram_align_len < __dma_nand_align_len?__dma_ram_align_len:__dma_nand_align_len;
			if (len > __dma_bytes_all - __dma_bytes_done) {
				len = __dma_bytes_all - __dma_bytes_done;
			}
			pr_info("nand read_buf : nand_phys_addr is 0x%x, phys_addr is 0x%x, len is %d, __dma_bytes_done %d\n",
					nand_phys_addr, phys_addr, len, __dma_bytes_done);
		}
#endif

		dma_param.src_addr = nand_phys_addr;
		dma_param.dst_addr = phys_addr;
		dma_param.xfer_size = len;
		dma_param.dma_mode = RDA_DMA_FR_MODE;
#ifdef NAND_DMA_POLLING
		dma_param.enable_int = 0;
#else
		dma_param.enable_int = 1;
#endif /* NAND_DMA_POLLING */

		ret = rda_set_dma_params(info->dma_ch, &dma_param);
		if (ret < 0) {
			dev_err(info->dev, "failed to set parameter\n");
			dma_unmap_single(info->dev, phys_addr, len, DMA_FROM_DEVICE);
			return;
		}

		init_completion(&info->comp);

		rda_start_dma(info->dma_ch);
#ifndef NAND_DMA_POLLING
		ret = wait_for_completion_timeout(&info->comp,
				msecs_to_jiffies(NAND_DMA_TIMEOUT_MS));
		if (ret <= 0) {
			dev_err(info->dev, "dma read timeout, ret = 0x%08x\n", ret);
			dma_unmap_single(info->dev, phys_addr, len, DMA_FROM_DEVICE);
			return;
		}
#else
		rda_poll_dma(info->dma_ch);
		rda_stop_dma(info->dma_ch);
#endif /* #if 0 */

#ifdef FIX_CHIP_DMA_CROSS_MEM_PAGE_BUG
		__dma_bytes_done += len;
	}
#endif

	/* Free the specified physical address */
	dma_unmap_single(info->dev, phys_addr, len, DMA_FROM_DEVICE);
	info->read_ptr += len;
	TODO - update statistic if you need to use DMA
	return;

out_copy:
	__nand_rda_copyfrom_iomem(buf, (void *)nand_ptr, len);
	info->read_ptr += len;
	TODO - update statistic if you need to use DMA
	return;

#endif /* CONFIG_MTD_NAND_RDA_DMA */

#ifdef NAND_STATISTIC
	info->bytes_read += len;
	if (info->nand_status == 0)
		info->ecc_flips += info->ecc_bits;
	else
		info->read_fails++;

#endif
}

static inline void __nand_rda_copyto_iomem(void *io_mem, void *src_buf,
					   size_t size)
{
	memcpy(io_mem, src_buf, size);
	return;
}

static void nand_rda_write_buf(struct mtd_info *mtd, const uint8_t * buf,
			       int len)
{
	struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;
#if 1//ndef CONFIG_MTD_NAND_RDA_DMA
	u8 *nand = chip->IO_ADDR_W + info->write_ptr;
	u8 *u_nand_temp_buffer = nand;
	u8 *u_temp_ptr = chip->IO_ADDR_W;
	uint8_t *temp_ptr = (uint8_t *)buf;
#if (OOB_SW_PROTECT_SWITCH_ON == 1)
	uint8_t oob_temp_ptr[NAND_MAX_OOBSIZE];
	uint8_t oob_byte_count = 2 + __FTL_OOB_STRUCT_LENGTH__ + 2 + 4;
	uint8_t oob_copy_number = info->mtd.oobsize / oob_byte_count;
	uint32_t checksum;
#endif

	if(info->nand_use_type > 0){
		if(info->write_ptr > 0){/*write OOB to buffer*/
#if (OOB_SW_PROTECT_SWITCH_ON == 1)
			if(info->mtd.writesize == 6144 ||info->mtd.writesize == 12288){
				checksum = nand_checksum32(temp_ptr, oob_byte_count - 4);
				memcpy(&oob_temp_ptr[0], temp_ptr, oob_byte_count - 4);
				oob_temp_ptr[oob_byte_count - 4] = (uint8_t)(checksum & 0xff);
				oob_temp_ptr[oob_byte_count - 3] = (uint8_t)((checksum >> 8) & 0xff);
				oob_temp_ptr[oob_byte_count - 2] = (uint8_t)((checksum >> 16) & 0xff);
				oob_temp_ptr[oob_byte_count - 1] = (uint8_t)((checksum >> 24) & 0xff);
				while(oob_copy_number > 1){
					oob_copy_number--;
					memcpy(&oob_temp_ptr[oob_copy_number * oob_byte_count],
						&oob_temp_ptr[0], oob_byte_count);
				}
				temp_ptr = &oob_temp_ptr[0];
			}
#endif
			info->write_ptr = info->logic_page_size;
			u_nand_temp_buffer = (u8 *)(u_temp_ptr + info->write_ptr);
			memcpy((void *)u_nand_temp_buffer, (void *)temp_ptr, info->logic_oob_size);
			memcpy((void *)(u_nand_temp_buffer + NAND_CONTROLLER_TEMP_BUFFER_SIZE),
							(void *)(temp_ptr + info->logic_oob_size), info->logic_oob_size);
			if(info->type == NAND_TYPE_SLC16K || info->type == NAND_TYPE_MLC16K){
				memcpy((void *)(u_nand_temp_buffer + NAND_CONTROLLER_TEMP_BUFFER_SIZE * 2),
							(void *)(temp_ptr + info->logic_oob_size * 2), info->logic_oob_size);
				memcpy((void *)(u_nand_temp_buffer + NAND_CONTROLLER_TEMP_BUFFER_SIZE * 3),
							(void *)(temp_ptr + info->logic_oob_size * 3), info->logic_oob_size);
			}
		}else{/*write data to buffer*/
			memcpy((void *)u_nand_temp_buffer, (void *)temp_ptr, info->logic_page_size);
			memcpy((void *)(u_nand_temp_buffer + NAND_CONTROLLER_TEMP_BUFFER_SIZE),
							(void *)(temp_ptr + info->logic_page_size), info->logic_page_size);
			if(info->type == NAND_TYPE_SLC16K || info->type == NAND_TYPE_MLC16K){
				memcpy((void *)(u_nand_temp_buffer + NAND_CONTROLLER_TEMP_BUFFER_SIZE * 2),
							(void *)(temp_ptr + info->logic_page_size * 2), info->logic_page_size);
				memcpy((void *)(u_nand_temp_buffer + NAND_CONTROLLER_TEMP_BUFFER_SIZE * 3),
							(void *)(temp_ptr + info->logic_page_size * 3), info->logic_page_size);
			}
			info->write_ptr += info->logic_page_size;
		}
	}else{
#if (OOB_SW_PROTECT_SWITCH_ON == 1)
		if(info->write_ptr > 0 && info->mtd.writesize == 3072){
			checksum = nand_checksum32(temp_ptr, oob_byte_count - 4);
			memcpy(&oob_temp_ptr[0], temp_ptr, oob_byte_count - 4);
			oob_temp_ptr[oob_byte_count - 4] = (uint8_t)(checksum & 0xff);
			oob_temp_ptr[oob_byte_count - 3] = (uint8_t)((checksum >> 8) & 0xff);
			oob_temp_ptr[oob_byte_count - 2] = (uint8_t)((checksum >> 16) & 0xff);
			oob_temp_ptr[oob_byte_count - 1] = (uint8_t)((checksum >> 24) & 0xff);
			while(oob_copy_number > 1){
				oob_copy_number--;
				memcpy(&oob_temp_ptr[oob_copy_number * oob_byte_count],
					&oob_temp_ptr[0], oob_byte_count);
			}
			temp_ptr = &oob_temp_ptr[0];
		}
#endif
		nand = info->data_base + info->write_ptr;
		memcpy((void *)nand, (void *)temp_ptr, len);
		info->write_ptr += len;
	}

#else
	struct rda_dma_chan_params dma_param;
	dma_addr_t phys_addr;
	void *addr = (void *)buf;
	dma_addr_t nand_phys_addr = info->phy_addr;
	int ret = 0;
#ifdef FIX_CHIP_DMA_CROSS_MEM_PAGE_BUG
	int __dma_periods_need = 0, __dma_bytes_done = 0, __dma_bytes_all = 0;
	unsigned int __dma_nand_align_len = 0, __dma_ram_align_len = 0;
#endif
	u8 *buf_ptr = (u8 *)(chip->IO_ADDR_W + info->write_ptr);
	struct page *p1;

	rda_dbg_nand("write_buf, addr = 0x%p, len = %d, write_ptr = %d\n",
	       buf, len, info->write_ptr);

	/*
	 * If size is less than the size of oob,
	 * we copy directly them to mapping buffer.
	 */
	if (len <= info->mtd.oobsize) {
		__nand_rda_copyto_iomem((void *)buf_ptr, (void *)buf, len);
		info->write_ptr += len;
		return;
	}

	/*
	 * Check if the address of buffer is an address allocated by vmalloc.
	 * If so, we don't directly use these address, it might be un-continous
	 * in physical address. We have to copy them via memcpy.
	 */
	if (addr >= high_memory) {
		if (((size_t) addr & PAGE_MASK) !=
		    ((size_t) (addr + len - 1) & PAGE_MASK)) {
			goto out_copy;
		}

		p1 = vmalloc_to_page(addr);
		if (!p1) {
			goto out_copy;
		}
		addr = page_address(p1) + ((size_t) addr & ~PAGE_MASK);
	}

	phys_addr = dma_map_single(info->dev, addr, len, DMA_TO_DEVICE);
	if (dma_mapping_error(info->dev, phys_addr)) {
		dev_err(info->dev, "failed to dma_map_single\n");
		return;
	}

#ifdef FIX_CHIP_DMA_CROSS_MEM_PAGE_BUG
	/*
	 * Chip bug: dma cross memory page
	 * We should guarantee dma do NOT cross memory page (PAGE_SIZE)
	 */
	__dma_bytes_all = len;
	__dma_bytes_done = 0;
	__dma_periods_need = 0;
	if (DMA_NEED_PERIOD(phys_addr, len)) {
		__dma_periods_need = 1;
	}

	while (__dma_bytes_done < __dma_bytes_all) {
		// need period dma?
		if (__dma_periods_need) {
			nand_phys_addr = (unsigned int)nand_phys_addr + __dma_bytes_done;
			phys_addr = (unsigned int)phys_addr + __dma_bytes_done;
			// min of (nand reg next memory page - nand_phys_addr) and (ram next memory page - phys_addr)
			__dma_ram_align_len = (unsigned int)((phys_addr + PAGE_SIZE) & PAGE_MASK) - (unsigned int)phys_addr;
			__dma_nand_align_len = (unsigned int)((nand_phys_addr + PAGE_SIZE) & PAGE_MASK) - (unsigned int)nand_phys_addr;
			len = __dma_ram_align_len < __dma_nand_align_len?__dma_ram_align_len:__dma_nand_align_len;
			if (len > __dma_bytes_all - __dma_bytes_done) {
				len = __dma_bytes_all - __dma_bytes_done;
			}
			rda_dbg_nand("nand write_buf : nand_phys_addr is 0x%x, phys_addr is 0x%x, len is %d, __dma_bytes_done %d \n",
					nand_phys_addr, phys_addr, len, __dma_bytes_done);
		}
#endif

		dma_param.src_addr = phys_addr;
		dma_param.dst_addr = nand_phys_addr;
		dma_param.xfer_size = len;
		dma_param.dma_mode = RDA_DMA_FW_MODE;
#ifdef NAND_DMA_POLLING
		dma_param.enable_int = 0;
#else
		dma_param.enable_int = 1;
#endif /* NAND_DMA_POLLING */

		ret = rda_set_dma_params(info->dma_ch, &dma_param);
		if (ret < 0) {
			dev_err(info->dev, "failed to set parameter\n");
			dma_unmap_single(info->dev, phys_addr, len, DMA_TO_DEVICE);
			return;
		}

		init_completion(&info->comp);

		rda_start_dma(info->dma_ch);
#ifndef NAND_DMA_POLLING
		ret = wait_for_completion_timeout(&info->comp,
				msecs_to_jiffies(NAND_DMA_TIMEOUT_MS));
		if (ret <= 0) {
			dev_err(info->dev, "dma write timeout, ret = 0x%08x\n", ret);
			dma_unmap_single(info->dev, phys_addr, len, DMA_TO_DEVICE);
			return;
		}
#else
		rda_poll_dma(info->dma_ch);
		rda_stop_dma(info->dma_ch);
#endif /* #if 0 */

#ifdef FIX_CHIP_DMA_CROSS_MEM_PAGE_BUG
		__dma_bytes_done += len;
	}
#endif

	/* Free the specified physical address */
	dma_unmap_single(info->dev, phys_addr, len, DMA_TO_DEVICE);
	info->write_ptr += len;

	return;

out_copy:
	__nand_rda_copyto_iomem((void *)buf_ptr, (void *)buf, len);
	info->write_ptr += len;
	return;

#endif /* CONFIG_MTD_NAND_RDA_DMA */
#ifdef NAND_STATISTIC
	info->bytes_write += len;
#endif
}

static void nand_rda_write_cache(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;
#ifndef CONFIG_MTD_NAND_RDA_DMA
	u8 *nand_ptr = (u8 *) (info->data_base);
#else
	u8 *nand_ptr = (u8 *) (info->phy_addr);
#endif
	u8 *u_nand_temp_buffer = (u8 *) (chip->IO_ADDR_W + info->col_addr);

	if(info->nand_use_type > 0){
#ifndef CONFIG_MTD_NAND_RDA_DMA
		memcpy((void *)nand_ptr,
				(void *)u_nand_temp_buffer,
				info->logic_page_size + info->logic_oob_size);
#else
		nand_rda_dma_move_data(mtd, nand_ptr, (u8*)u_nand_temp_buffer,
				(info->logic_page_size + info->logic_oob_size), DMA_TO_DEVICE);
#endif
	}
}

static void nand_rda_do_cmd_pre(struct mtd_info *mtd)
{
	struct nand_chip *this = mtd->priv;
	struct rda_nand_info *info = this->priv;

//#ifndef CONFIG_MTD_NAND_RDA_DMA
//	u8 *IO_ADDR_W = (u8 *)this->IO_ADDR_W;
//#endif /* CONFIG_MTD_NAND_RDA_DMA */

	switch (info->cmd) {
	case NAND_CMD_SEQIN:
//#ifndef CONFIG_MTD_NAND_RDA_DMA
		hal_flush_buf(info);

		if(info->nand_use_type > 0)
			nand_rda_write_cache(mtd);
/*
		else
			memcpy((void *)info->data_base,
					(void *)IO_ADDR_W, mtd->writesize + mtd->oobsize);
*/
//#endif /* CONFIG_MTD_NAND_RDA_DMA */
		info->write_ptr = 0;
		break;
	case NAND_CMD_READ0:
		hal_flush_buf(info);
		break;
	case NAND_CMD_RNDIN:
//#ifndef CONFIG_MTD_NAND_RDA_DMA
		if(info->nand_use_type > 0){
			nand_rda_write_cache(mtd);
		}
//#endif /* CONFIG_MTD_NAND_RDA_DMA */
		break;
	default:
		break;
	}

	return;
}

static void nand_rda_do_cmd_post(struct mtd_info *mtd)
{
	struct nand_chip *this = mtd->priv;
	struct rda_nand_info *info = this->priv;
//#ifndef CONFIG_MTD_NAND_RDA_DMA
	u8 *IO_ADDR_R = (u8 *) this->IO_ADDR_R;
//#endif /* CONFIG_MTD_NAND_RDA_DMA */
	u32 temp;

	switch (info->cmd) {
	case NAND_CMD_RESET:
		break;

	case NAND_CMD_READID:
		temp = __raw_readl(info->reg_base + NANDFC_REG_IDCODE_A);
		info->byte_buf[0] = temp;
		temp = __raw_readl(info->reg_base + NANDFC_REG_IDCODE_B);
		info->byte_buf[1] = temp;
		info->index = 0;
		break;

	case NAND_CMD_STATUS:
		temp = __raw_readl(info->reg_base + NANDFC_REG_OP_STATUS);
		info->byte_buf[0] = (temp & 0xFF);
		info->index = 0;
		if (info->byte_buf[0] & 0x3) {
			/* lower 2 bits are indicator of error
			 * see NAND_STATUS_FAIL
			 * */
			dev_err(info->dev, "error, op status = %x\n",
			       info->byte_buf[0]);
		}
		break;

	case NAND_CMD_READ0:	// NAND_CMD_READOOB goes here too
		info->index = 0;
//#ifndef CONFIG_MTD_NAND_RDA_DMA
		if(info->nand_use_type > 0){
			nand_rda_read_cache(mtd);
			if(NAND_CMD_READOOB == info->cmd_flag){
				temp = IO_ADDR_R[mtd->writesize];
				temp |= IO_ADDR_R[mtd->writesize+1] << 8;
				info->byte_buf[0] = temp;
				info->cmd_flag = NAND_CMD_NONE;
			}
		}else{
		//	memcpy((void *)IO_ADDR_R,
		//			(void *)(info->data_base + info->col_addr),
		//			mtd->writesize + mtd->oobsize - info->col_addr);
			if(NAND_CMD_READOOB == info->cmd_flag){
				u8 *nand_ptr = (u8 *) (info->data_base + mtd->writesize);
				memcpy((u8*)&info->byte_buf[0], nand_ptr, 2);
				info->cmd_flag = NAND_CMD_NONE;
			}
		}
//#endif /* CONFIG_MTD_NAND_RDA_DMA */

		if (info->nand_status == 0) {
			if (info->ecc_bits_supported)
				info->ecc_bits = hal_get_flipbits(info);
			else
				info->ecc_bits = 0;
		} else {
			info->ecc_bits = -EIO;
		}
		break;

	case NAND_CMD_SEQIN:
		if(info->nand_use_type > 0){
			info->logic_operate_time += 1;
			info->col_addr += NAND_CONTROLLER_TEMP_BUFFER_SIZE;
			info->cmd = NAND_CMD_RNDIN;
		}

#ifdef NAND_STATISTIC
		if (info->nand_status != 0)
			info->write_fails++;
#endif
		break;

	case NAND_CMD_RNDIN:
		if(info->nand_use_type > 0){
			info->logic_operate_time += 1;
			info->col_addr += NAND_CONTROLLER_TEMP_BUFFER_SIZE;
		}
		break;
		/* delay 10ms, for erase to complete */
	case NAND_CMD_ERASE1:
		mdelay(10);
		break;

	default:
		break;
	}

	return;
}

static void nand_rda_hwcontrol(struct mtd_info *mtd, int cmd, unsigned int ctrl)
{
	rda_dbg_nand
	    ("nand_rda_hwcontrol, cmd = %x, ctrl = %x, not use anymore\n", cmd,
	     ctrl);
}

static void nand_rda_cmdfunc(struct mtd_info *mtd, unsigned int command,
			     int column, int page_addr)
{
	register struct nand_chip *chip = mtd->priv;
	struct rda_nand_info *info = chip->priv;
#ifdef NAND_IRQ_POLL
	unsigned long cmd_ret;
#else
	unsigned long timeout;
#endif /* NAND_IRQ_POLL */

	rda_dbg_nand
	    ("nand_rda_cmdfunc, cmd = %x, page_addr = %x, col_addr = %x\n",
	     command, page_addr, column);

	/* remap command */
	switch (command) {
	case NAND_CMD_SEQIN:	/* 0x80 do nothing, wait for 0x10 */
		info->page_addr = page_addr;
		info->col_addr = column;
		info->cmd = NAND_CMD_NONE;
		/* Hold the offset we want to write. */
		info->write_ptr = column;
		info->logic_operate_time = 0;
		break;
	case NAND_CMD_READSTART:	/* hw auto gen 0x30 for read */
	case NAND_CMD_ERASE2:	/* hw auto gen 0xd0 for erase */
		rda_dbg_nand("erase block, erase_size = %x\n", mtd->erasesize);
		info->cmd = NAND_CMD_NONE;
		break;
	case NAND_CMD_PAGEPROG:	/* 0x10 do the real program */
		info->cmd = NAND_CMD_SEQIN;
		/*
		 * As facing only oob case,
		 * we'll modify column address to zero to fake a writing page.
		 */
//		if (info->col_addr == mtd->writesize ) {
			info->col_addr = 0;
//		}
		break;

	case NAND_CMD_READOOB:
		/* Need to read the page out
		 * 1. different storage layout in controller buffer and flash
		 * 2. To read out OOB only, need to disable HW ECC check
		 * */
		info->page_addr = page_addr;
		info->col_addr = column;
		info->cmd = NAND_CMD_READ0;

		/* Read data of oob */
		info->read_ptr = mtd->writesize;
		info->cmd_flag = NAND_CMD_READOOB;
		break;
	case NAND_CMD_READ0:
		info->read_ptr = 0;
	default:
		info->page_addr = page_addr;
		info->col_addr = column;
		info->cmd = command;
		break;
	}

	if (info->cmd == NAND_CMD_NONE) {
		return;
	}

loop:
	if (info->col_addr != -1) {
		hal_set_col_addr(info, info->col_addr);
	}

	if (info->page_addr == -1) {
		info->page_addr = 0;
	}

	nand_rda_do_cmd_pre(mtd);

	init_completion(&info->nand_comp);

	hal_send_cmd(info, (unsigned char)info->cmd, info->page_addr);

#ifdef NAND_IRQ_POLL
	cmd_ret = hal_wait_cmd_complete(info);
	if (cmd_ret) {
		dev_err(info->dev, "cmd fail, cmd = %x,"
			" page_addr = %x, col_addr = %x\n",
			info->cmd, info->page_addr, info->col_addr);
	}
#else
	timeout = wait_for_completion_timeout(&info->nand_comp,
					      msecs_to_jiffies
					      (NAND_CMD_TIMEOUT_MS));
	if (timeout == 0) {
		dev_err(info->dev, "cmd timeout : cmd = 0x%x,"
			" page_addr = 0x%x, col_addr = 0x%x\n",
			command, info->page_addr, info->col_addr);
	}

	if (info->nand_status) {
		dev_err(info->dev, "cmd fail : cmd = 0x%x,"
			" page_addr = 0x%x, col_addr = 0x%x, status = 0x%x\n",
			command, info->page_addr, info->col_addr,
			info->nand_status);
	}
#endif /* NAND_IRQ_POLL */

	nand_rda_do_cmd_post(mtd);

	if(info->nand_use_type > 0 && info->logic_operate_time <= info->nand_use_type && \
		(info->cmd == NAND_CMD_SEQIN || info->cmd == NAND_CMD_RNDIN))
		goto loop;

	return;
}

static int nand_rda_dev_ready(struct mtd_info *mtd)
{
	return 1;
}

static int rda_nand_read_page(struct mtd_info *mtd, struct nand_chip *chip,
				uint8_t *buf, int oob_required, int page)
{

	struct rda_nand_info *info = chip->priv;

	/* Upperlayer(nand_do_read_ops in nand_base.c) will send READ0
	 * before this function got called, so we have status before this
	 * */
	/* Nand controller will do ECC automatically, no sw control needed
	 * */
	rda_dbg_nand("%s for page %d\n", __func__, page);
	chip->read_buf(mtd, buf, mtd->writesize);
	if (oob_required)
		chip->read_buf(mtd, chip->oob_poi, mtd->oobsize);
	return info->ecc_bits;

}

static int rda_nand_write_page(struct mtd_info *mtd, struct nand_chip *chip,
			const uint8_t *buf, int oob_required)
{
	/* upper layer(nand_write_page  in nand_base.c)
	 * will send SEQIN before write page is called,
	 * and will send PAGEPROG after this write page function is called
	 * */
	/* Nand controller will do ECC automatically, no sw control needed
	 * */
	rda_dbg_nand("%s \n", __func__);
	chip->write_buf(mtd, buf, mtd->writesize);
	if (oob_required)
		chip->write_buf(mtd, chip->oob_poi, mtd->oobsize);
	/* how to return status of write_page? we won't be able to return
	 * any status here as no staus avaiable unitl PAGEPROG done,
	 * but wait_func will return the status of write to upperlayer
	 * */
	return 0;
}

static int nand_rda_map_type(struct rda_nand_info *info)
{
	rda_dbg_nand(
		"nand_rda_map_type, page_size = %d,"
		" chip_size = %lld, options = %x\n",
		info->mtd.writesize, info->nand_chip.chipsize,
		info->nand_chip.options);

	info->hard_ecc_hec = 0;
	switch (info->mtd.writesize) {
	case 2048:
		if (info->nand_chip.options & NAND_RDA_HEC) {
			if (info->mtd.erasesize == 2048 * 256) {
				info->type = NAND_TYPE_MLC4K;
				info->hard_ecc_hec = 1;
			}
			else {
				dev_err(info->dev, "invalid type for HEC\n");
				/* we only support HEC on 4k MLC */
				BUG_ON(1);
			}
			break;
		} else if (info->nand_chip.chipsize < SZ_256M)	// 256MB = 2Gb
			info->type = NAND_TYPE_2KS;
		else
			info->type = NAND_TYPE_2KL;

		info->nand_use_type = 0;
		break;
	case 3072:
		if (info->nand_chip.options & NAND_RDA_HEC) {
			if (info->mtd.erasesize == 3072 * 256) {
				info->type = NAND_TYPE_MLC4K;
				info->hard_ecc_hec = 1;
				info->nand_use_type = 0;
			}
			else {
				dev_err(info->dev, "invalid type for HEC\n");
				/* we only support HEC on 4k MLC */
				BUG_ON(1);
			}
			break;
		}
		break;
	case 4096:
		if (info->nand_chip.options & NAND_RDA_HEC){
			info->type = NAND_TYPE_MLC8K;
			info->hard_ecc_hec = 1;
			info->nand_use_type = 1;
		}else{
			info->type = NAND_TYPE_SLC4K;
			info->nand_use_type = 0;
		}
		break;
	case 6144:
		if (info->nand_chip.options & NAND_RDA_HEC) {
			if (info->mtd.erasesize == 6144 * 256) {
				info->type = NAND_TYPE_MLC8K;
				info->hard_ecc_hec = 1;
				info->nand_use_type = 1;
			}
			else {
				dev_err(info->dev, "invalid type for HEC\n");
				/* we only support HEC on 4k MLC */
				BUG_ON(1);
			}
			break;
		}
		break;
	case 8192:
		if (info->nand_chip.options & NAND_RDA_HEC){
			info->type = NAND_TYPE_MLC16K;
			info->hard_ecc_hec = 1;
			info->nand_use_type = 3;
		}else{
			info->type = NAND_TYPE_SLC8K;
			info->nand_use_type = 1;
		}
		break;
	case 12288:
		if (info->nand_chip.options & NAND_RDA_HEC) {
			if (info->mtd.erasesize == 12288 * 256) {
				info->type = NAND_TYPE_MLC16K;
				info->hard_ecc_hec = 1;
				info->nand_use_type = 3;
			}
			else {
				dev_err(info->dev, "invalid type for HEC\n");
				/* we only support HEC on 4k MLC */
				BUG_ON(1);
			}
			break;
		}
		break;
	case 16384:
		if (info->nand_chip.options & NAND_RDA_HEC){
			info->type = NAND_TYPE_INVALID;
			info->nand_use_type = 0xFF;
		}else{
			info->type = NAND_TYPE_SLC16K;
			info->nand_use_type = 3;
		}
		break;
	default:
		dev_err(info->dev, "invalid pagesize %d\n",
		       info->mtd.writesize);
		info->type = NAND_TYPE_INVALID;
		return -EINVAL;
	}

	if (info->nand_chip.options & NAND_BUSWIDTH_16)
		info->bus_width_16 = 1;
	else
		info->bus_width_16 = 0;

	info->logic_page_size = info->mtd.writesize / (info->nand_use_type + 1);
	info->logic_oob_size = info->mtd.oobsize / (info->nand_use_type + 1);
	info->logic_operate_time = 0;
	info->cmd_flag = NAND_CMD_NONE;

	rda_dbg_nand("nand_rda_map_type, map result, type = %d, logic_page_size = 0x%x,\
		logic_oob_size = 0x%x \n", info->type, info->logic_page_size, info->logic_oob_size);

	return 0;
}


#ifdef CONFIG_PROC_FS
static int nand_rda_reg_dump(struct seq_file *m, struct rda_nand_info *info)
{
	int len = 0;

	len += seq_printf(m ,"config A : 0x%x\n",
			(u32)__raw_readl(info->reg_base + NANDFC_REG_CONFIG_A));

	len += seq_printf(m ,"config B : 0x%x\n",
			(u32)__raw_readl(info->reg_base + NANDFC_REG_CONFIG_B));


	len += seq_printf(m ,"int status : 0x%x\n",
			(u32)__raw_readl(info->reg_base + NANDFC_REG_INT_STAT));

	len += seq_printf(m ,"def B : 0x%x\n",
			(u32)__raw_readl(info->reg_base + NANDFC_REG_CMD_DEF_B));
	return len;
}

static int nand_rda_proc_show(struct seq_file *m,void *v)
{
	int len;
	struct rda_nand_info *info = m->private;

	len = seq_printf(m, "Total Size: %dMiB\n", (int)(info->nand_chip.chipsize/SZ_1M));
	len += seq_printf(m, "Page Size : %dB\n",  info->mtd.writesize);

#ifdef NAND_STATISTIC
	len += seq_printf(m, "total read size: %llu KB\n", info->bytes_read/SZ_1K);
	len += seq_printf(m, "total write size: %llu KB\n", info->bytes_write/SZ_1K);
	len += seq_printf(m, "total flip bits: %llu \n", info->ecc_flips);
	len += seq_printf(m, "total read fails: %llu \n", info->read_fails);
	len += seq_printf(m, "total write fails: %llu \n", info->write_fails);
#endif
	if (info->ecc_bits_supported)
		len += seq_printf(m, "last page flip %u \n", hal_get_flipbits(info));
	else
		len += seq_printf(m, "last page flip not supported by hardware\n");
	len += nand_rda_reg_dump(m, info);
	return len;
}

static int nand_rda_proc_open(struct inode *inode, struct file *file)
{
	struct rda_nand_info *info = PDE_DATA(inode);
	return single_open(file, nand_rda_proc_show, info);
}

static const struct file_operations nand_rda_proc_fops = {
	.open		= nand_rda_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release
};

static void nand_rda_create_proc(struct rda_nand_info *info)
{
	struct proc_dir_entry *pde;
	pde = proc_create_data(PROCNAME, 0664, NULL, &nand_rda_proc_fops, info);
	if (pde == NULL) {
		pr_err("%s failed\n", __func__);
	}
}

static void  nand_rda_delete_proc(void)
{
	remove_proc_entry(PROCNAME, NULL);
}
#else

static void spi_nand_rda_create_proc(struct rda_nand_info *info) {}

static void spi_nand_rda_delete_proc(void) {}
#endif

static int nand_rda_init(struct rda_nand_info *info)
{
	int ret;

	ret = nand_rda_map_type(info);
	if (ret) {
		return ret;
	}

	return hal_init(info, info->type);
}

/**
 * nand_rda_wait - wait until the command is done
 * @mtd: MTD device structure
 * @chip: NAND chip structure
 *
 * Wait for command done. This applies to erase and program only. Erase can
 * take up to 400ms and program up to 20ms according to general NAND and
 * SmartMedia specs.
 */
static int nand_rda_wait(struct mtd_info *mtd, struct nand_chip *chip)
{
	struct rda_nand_info *info = chip->priv;
	int status;
	unsigned long timeo = jiffies;
	int state = chip->state;

	if (state == FL_ERASING) {
		timeo += (HZ * 400) / 1000;
	} else {
		timeo += (HZ * 20) / 1000;
	}

	/*
	 * Apply this short delay always to ensure that we do wait tWB in any
	 * case on any machine.
	 */
	ndelay(100);

	chip->cmdfunc(mtd, NAND_CMD_STATUS, -1, -1);

	while (time_before(jiffies, timeo)) {
		if (chip->dev_ready) {
			if (chip->dev_ready(mtd)) {
				break;
			}
		} else {
			if (chip->read_byte(mtd) & NAND_STATUS_READY) {
				break;
			}
		}
		cond_resched();
	}

	status = (int)chip->read_byte(mtd);
	/* Check if there are any errors be reported by interrupt. */
	if (info->nand_status) {
		status |= NAND_STATUS_FAIL;
	}

	return status;
}

#ifdef CONFIG_MTD_NAND_RDA_DMA
/*
 * nand_rda_dma_cb: callback on the completion of dma transfer
 * @ch: logical channel
 * @data: pointer to completion data structure
 */
static void nand_rda_dma_cb(u8 ch, void *data)
{
	complete((struct completion *)data);
}
#endif /* CONFIG_MTD_NAND_RDA_DMA */

/*
 * Probe for the NAND device.
 */
static int __init rda_nand_probe(struct platform_device *pdev)
{
	struct rda_nand_info *info;
	struct mtd_info *mtd;
	struct nand_chip *nand_chip;
	struct resource *mem;
	int res = 0;
	int irq;
	u16 metal_id;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "can not get resource mem\n");
		return -ENXIO;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		return irq;
	}

	/* Allocate memory for the device structure (and zero it) */
	info = kzalloc(sizeof(struct rda_nand_info), GFP_KERNEL);
	if (!info) {
		dev_err(&pdev->dev, "failed to allocate device structure.\n");
		return -ENOMEM;
	}
#ifdef CONFIG_MTD_NAND_RDA_DMA
	info->phy_addr = mem->start;
#endif /* CONFIG_MTD_NAND_RDA_DMA */
	info->databuf = kmalloc(SZ_32K, GFP_KERNEL);
	if (!info->databuf) {
		dev_err(&pdev->dev, "failed to allocate databuf\n");
		kfree(info);
		return -ENOMEM;
	}

	info->base = ioremap(mem->start, resource_size(mem));
	if (info->base == NULL) {
		dev_err(&pdev->dev, "ioremap failed\n");
		res = -EIO;
		goto err_nand_ioremap;
	}
	info->reg_base = info->base + NANDFC_REG_OFFSET;
	info->data_base = info->base + NANDFC_DATA_OFFSET;

#ifndef NAND_IRQ_POLL
	res = request_irq(irq, nand_rda_irq_handler,
			  IRQF_DISABLED | IRQF_ONESHOT, "nand_rda",
			  (void *)info);
	if (res < 0) {
		dev_err(&pdev->dev, "%s: request irq fail\n", __func__);
		res = -EPERM;
		goto err_nand_irq;
	}

	info->nand_status = 0;
#endif /* NAND_IRQ_POLL */

	mtd = &info->mtd;
	nand_chip = &info->nand_chip;
	info->plat_data = pdev->dev.platform_data;
	info->dev = &pdev->dev;

	info->master_clk = clk_get(NULL, RDA_CLK_APB2);
	if (!info->master_clk) {
		dev_err(&pdev->dev, "no handler of clock\n");
		res = -EINVAL;
		goto err_nand_get_clk;
	}
	info->clk = info->plat_data->max_clock;

	nand_chip->priv = info;	/* link the private data structures */
	mtd->priv = nand_chip;
	mtd->owner = THIS_MODULE;

	/* Set address of NAND IO lines */
//#ifndef CONFIG_MTD_NAND_RDA_DMA
	nand_chip->IO_ADDR_R = (void *)info->databuf;
	nand_chip->IO_ADDR_W = (void *)info->databuf;
//#else
//	nand_chip->IO_ADDR_R = info->data_base;
//	nand_chip->IO_ADDR_W = info->data_base;
//#endif /* CONFIG_MTD_NAND_RDA_DMA */
	nand_chip->cmd_ctrl = nand_rda_hwcontrol;
	nand_chip->cmdfunc = nand_rda_cmdfunc;
	nand_chip->dev_ready = nand_rda_dev_ready;

#ifdef CONFIG_MTD_NAND_RDA_DMA
	rda_request_dma(0, "rda-dma", nand_rda_dma_cb, &info->comp,
			&info->dma_ch);
#endif /* CONFIG_MTD_NAND_RDA_DMA */

	nand_chip->chip_delay = 20;	/* 20us command delay time */
	nand_chip->read_buf = nand_rda_read_buf;
	nand_chip->write_buf = nand_rda_write_buf;
	nand_chip->read_byte = nand_rda_read_byte;
	nand_chip->read_word = nand_rda_read_word;
	nand_chip->waitfunc = nand_rda_wait;
	/* we do use flash based bad block table, created by u-boot */
	nand_chip->bbt_options = NAND_BBT_USE_FLASH;

	info->write_ptr = 0;
	info->read_ptr = 0;
	info->index = 0;

	platform_set_drvdata(pdev, info);

#ifndef NAND_IRQ_POLL
	hal_irq_enable(info);
#endif /* NAND_IRQ_POLL */

	/* first scan to find the device and get the page size */
	if (nand_scan_ident(mtd, 1, NULL)) {
		res = -ENXIO;
		goto err_scan_ident;
	}

	nand_rda_init(info);

	nand_chip->ecc.mode = NAND_ECC_HW;
	nand_chip->ecc.size = mtd->writesize;
	if (info->hard_ecc_hec) {
		nand_chip->ecc.bytes = 180;
		nand_chip->ecc.strength = 96;
	} else {
		nand_chip->ecc.bytes = 45;
		nand_chip->ecc.strength = 20;
		/* Hardware can correct 24bit for every 2K data,
		 * but we don't have the real filpped bits for
		 * the first half 2K data. Be conservative here.
		 * */
	}

	/* hardware reported ecc flipbits only after U06 (metal id = 0XA)*/
	metal_id = rda_get_soc_metal_id();
	if (metal_id <= 0xA)
		info->ecc_bits_supported = false;
	else
		info->ecc_bits_supported = true;

	/* no way to control ECC steps, so the
	 * following setup can't be used.
	 * if (info->hard_ecc_hec) {
		nand_chip->ecc.bytes = 180;
		nand_chip->ecc.size = 1024;
		nand_chip->ecc.strength = 96;
	} else {
		nand_chip->ecc.bytes = 45;
		nand_chip->ecc.strength = 24;
		nand_chip->ecc.size = 2048;
	}
	TODO - should we set mtc->bitflip_threshold to
	 *    ecc.strength * page_size/ecc.bytes?
	 *    by default it is just set to ecc.strength
	 *    Default value is also OK.
	 * */
	nand_chip->ecc.read_page = rda_nand_read_page;
	nand_chip->ecc.write_page = rda_nand_write_page;

	if (!nand_chip->ecc.layout && (nand_chip->ecc.mode != NAND_ECC_SOFT_BCH)) {
		switch (mtd->oobsize) {
		case 8:
			nand_chip->ecc.layout = &rda_nand_oob_8;
			break;
		case 16:
			nand_chip->ecc.layout = &rda_nand_oob_16;
			break;
		case 64:
			nand_chip->ecc.layout = &rda_nand_oob_64;
			break;
		case 128:
			nand_chip->ecc.layout = &rda_nand_oob_128;
			break;
		case 256:
			nand_chip->ecc.layout = &rda_nand_oob_256;
			break;
		case 512:
			nand_chip->ecc.layout = &rda_nand_oob_512;
			break;
		default:
			pr_warn("No oob scheme defined for oobsize %d\n",
				   mtd->oobsize);
			BUG();
		}
	}

	/* second phase scan */
	if (nand_scan_tail(mtd)) {
		res = -ENXIO;
		goto err_scan_tail;
	}

	mtd->name = "rda_nand";
	res = mtd_device_parse_register(&info->mtd, NULL, NULL,
					partition_info,
					ARRAY_SIZE(partition_info));
	if (res) {
		goto err_parse_register;
	}

	dev_info(&pdev->dev, "rda_nand initialized, with HEC %s\n",
			info->hard_ecc_hec?"ON":"OFF");

	nand_rda_create_proc(info);

	return 0;

err_parse_register:
	nand_release(mtd);

err_scan_tail:
	hal_irq_disable(info);

err_scan_ident:

	//rda_nand_disable(host);
	platform_set_drvdata(pdev, NULL);

#ifdef CONFIG_MTD_NAND_RDA_DMA
	rda_free_dma(info->dma_ch);
#endif /* CONFIG_MTD_NAND_RDA_DMA */

	clk_put(info->master_clk);

err_nand_get_clk:
#ifndef NAND_IRQ_POLL
	free_irq(irq, (void *)info);

err_nand_irq:
#endif /* NAND_IRQ_POLL */
	iounmap(info->base);

err_nand_ioremap:
#ifndef CONFIG_MTD_NAND_RDA_DMA
	if (info->databuf) {
		kfree(info->databuf);
	}
#endif /* CONFIG_MTD_NAND_RDA_DMA */

	kfree(info);

	return res;
}

/*
 * Remove a NAND device.
 */
static int __exit rda_nand_remove(struct platform_device *pdev)
{
	struct rda_nand_info *info = platform_get_drvdata(pdev);
	struct mtd_info *mtd = &info->mtd;
	int irq;


	nand_rda_delete_proc();
	hal_irq_disable(info);

	nand_release(mtd);

	//rda_nand_disable(host);
#ifdef CONFIG_MTD_NAND_RDA_DMA
	rda_free_dma(info->dma_ch);
#endif /* CONFIG_MTD_NAND_RDA_DMA */

#ifndef NAND_IRQ_POLL
	irq = platform_get_irq(pdev, 0);
	free_irq(irq, (void *)info);
#endif /* NAND_IRQ_POLL */

	clk_put(info->master_clk);

	iounmap(info->base);

#ifndef CONFIG_MTD_NAND_RDA_DMA
	if (info->databuf) {
		kfree(info->databuf);
	}
#endif /* CONFIG_MTD_NAND_RDA_DMA */

	kfree(info);

	return 0;
}

static struct platform_driver rda_nand_driver = {
	.remove = __exit_p(rda_nand_remove),
	.driver = {
		   .name = RDA_NAND_HD_DRV_NAME,
		   .owner = THIS_MODULE,
		   },
};

static int __init rda_nand_init(void)
{
	return platform_driver_probe(&rda_nand_driver, rda_nand_probe);
}

static void __exit rda_nand_exit(void)
{
	platform_driver_unregister(&rda_nand_driver);
}

module_init(rda_nand_init);
module_exit(rda_nand_exit);

MODULE_DESCRIPTION("RDA NAND Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:rda_nand");
#endif
