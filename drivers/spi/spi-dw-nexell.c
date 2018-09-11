// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018  Nexell Co., Ltd.
 * Author: ChoongHyun, Jeon <suker@nexell.co.kr>
 *
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/scatterlist.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/property.h>

#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/types.h>

#include <linux/platform_data/dma-dw.h>

#include "spi-dw.h"

#define RX_BUSY		0
#define TX_BUSY		1

struct dw_spi_nx {
	struct dw_spi	dws;
	struct clk	*clk;
	struct clk	*pclk;
};

static struct dw_dma_slave nx_dma_tx = { .dst_id = 1 };
static struct dw_dma_slave nx_dma_rx = { .src_id = 0 };

static bool nx_spi_dma_chan_filter(struct dma_chan *chan, void *param)
{
	struct dw_dma_slave *s = param;

	if (s->dma_dev != chan->device->dev)
		return false;

	chan->private = s;
	return true;
}

static int nx_spi_dma_init(struct dw_spi *dws)
{
	struct dw_dma_slave *tx = dws->dma_tx;
	struct dw_dma_slave *rx = dws->dma_rx;
	dma_cap_mask_t mask;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	/* 1. Init rx channel */
	rx->dma_dev = &dws->master->dev;
	dws->rxchan = dma_request_channel(mask, nx_spi_dma_chan_filter, rx);
	if (!dws->rxchan)
		goto err_exit;
	dws->master->dma_rx = dws->rxchan;

	/* 2. Init tx channel */
	tx->dma_dev = &dws->master->dev;
	dws->txchan = dma_request_channel(mask, nx_spi_dma_chan_filter, tx);
	if (!dws->txchan)
		goto free_rxchan;
	dws->master->dma_tx = dws->txchan;

	dws->dma_inited = 1;
	return 0;

free_rxchan:
	dma_release_channel(dws->rxchan);
err_exit:
	return -EBUSY;
}

static void nx_spi_dma_exit(struct dw_spi *dws)
{
	if (!dws->dma_inited)
		return;

	dmaengine_terminate_sync(dws->txchan);
	dma_release_channel(dws->txchan);

	dmaengine_terminate_sync(dws->rxchan);
	dma_release_channel(dws->rxchan);
}

static irqreturn_t nx_dma_transfer(struct dw_spi *dws)
{
	u16 irq_status = dw_readl(dws, DW_SPI_ISR);

	if (!irq_status)
		return IRQ_NONE;

	dw_readl(dws, DW_SPI_ICR);
	spi_reset_chip(dws);

	dev_err(&dws->master->dev, "%s: FIFO overrun/underrun\n", __func__);
	dws->master->cur_msg->status = -EIO;
	spi_finalize_current_transfer(dws->master);
	return IRQ_HANDLED;
}

static bool nx_spi_can_dma(struct spi_master *master, struct spi_device *spi,
		struct spi_transfer *xfer)
{
	struct dw_spi *dws = spi_master_get_devdata(master);

	if (!dws->dma_inited)
		return false;

	return xfer->len > dws->fifo_len;
}

static enum dma_slave_buswidth nx_convert_dma_width(u32 dma_width)
{
	if (dma_width == 1)
		return DMA_SLAVE_BUSWIDTH_1_BYTE;
	else if (dma_width == 2)
		return DMA_SLAVE_BUSWIDTH_2_BYTES;

	return DMA_SLAVE_BUSWIDTH_UNDEFINED;
}

/*
 * dws->dma_chan_busy is set before the dma transfer starts, callback for tx
 * channel will clear a corresponding bit.
 */
static void nx_dw_spi_dma_tx_done(void *arg)
{
	struct dw_spi *dws = arg;

	clear_bit(TX_BUSY, &dws->dma_chan_busy);
	if (test_bit(RX_BUSY, &dws->dma_chan_busy))
		return;
	spi_finalize_current_transfer(dws->master);
}

static struct dma_async_tx_descriptor *nx_dw_spi_dma_prepare_tx(
		struct dw_spi *dws, struct spi_transfer *xfer)
{
	struct dma_slave_config txconf;
	struct dma_async_tx_descriptor *txdesc;

	if (!xfer->tx_buf)
		return NULL;

	txconf.direction = DMA_MEM_TO_DEV;
	txconf.dst_addr = dws->dma_addr;
	txconf.dst_maxburst = 16;
	txconf.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	txconf.dst_addr_width = nx_convert_dma_width(dws->dma_width);
	txconf.device_fc = false;

	dmaengine_slave_config(dws->txchan, &txconf);

	txdesc = dmaengine_prep_slave_sg(dws->txchan,
				xfer->tx_sg.sgl,
				xfer->tx_sg.nents,
				DMA_MEM_TO_DEV,
				DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!txdesc)
		return NULL;

	txdesc->callback = nx_dw_spi_dma_tx_done;
	txdesc->callback_param = dws;

	return txdesc;
}

/*
 * dws->dma_chan_busy is set before the dma transfer starts, callback for rx
 * channel will clear a corresponding bit.
 */
static void nx_dw_spi_dma_rx_done(void *arg)
{
	struct dw_spi *dws = arg;

	clear_bit(RX_BUSY, &dws->dma_chan_busy);
	if (test_bit(TX_BUSY, &dws->dma_chan_busy))
		return;
	spi_finalize_current_transfer(dws->master);
}

static struct dma_async_tx_descriptor *nx_dw_spi_dma_prepare_rx(
		struct dw_spi *dws, struct spi_transfer *xfer)
{
	struct dma_slave_config rxconf;
	struct dma_async_tx_descriptor *rxdesc;

	if (!xfer->rx_buf)
		return NULL;

	rxconf.direction = DMA_DEV_TO_MEM;
	rxconf.src_addr = dws->dma_addr;
	rxconf.src_maxburst = 16;
	rxconf.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	rxconf.src_addr_width = nx_convert_dma_width(dws->dma_width);
	rxconf.device_fc = false;

	dmaengine_slave_config(dws->rxchan, &rxconf);

	rxdesc = dmaengine_prep_slave_sg(dws->rxchan,
				xfer->rx_sg.sgl,
				xfer->rx_sg.nents,
				DMA_DEV_TO_MEM,
				DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!rxdesc)
		return NULL;

	rxdesc->callback = nx_dw_spi_dma_rx_done;
	rxdesc->callback_param = dws;

	return rxdesc;
}

static int nx_spi_dma_setup(struct dw_spi *dws, struct spi_transfer *xfer)
{
	u16 dma_ctrl = 0;

	dw_writel(dws, DW_SPI_DMARDLR, 0xf);
	dw_writel(dws, DW_SPI_DMATDLR, 0x10);

	if (xfer->tx_buf)
		dma_ctrl |= SPI_DMA_TDMAE;
	if (xfer->rx_buf)
		dma_ctrl |= SPI_DMA_RDMAE;
	dw_writel(dws, DW_SPI_DMACR, dma_ctrl);

	/* Set the interrupt mask */
	spi_umask_intr(dws, SPI_INT_TXOI | SPI_INT_RXUI | SPI_INT_RXOI);

	dws->transfer_handler = nx_dma_transfer;

	return 0;
}

static int nx_spi_dma_transfer(struct dw_spi *dws, struct spi_transfer *xfer)
{
	struct dma_async_tx_descriptor *txdesc, *rxdesc;

	/* Prepare the TX dma transfer */
	txdesc = nx_dw_spi_dma_prepare_tx(dws, xfer);

	/* Prepare the RX dma transfer */
	rxdesc = nx_dw_spi_dma_prepare_rx(dws, xfer);

	/* rx must be started before tx due to spi instinct */
	if (rxdesc) {
		set_bit(RX_BUSY, &dws->dma_chan_busy);
		dmaengine_submit(rxdesc);
		dma_async_issue_pending(dws->rxchan);
	}

	if (txdesc) {
		set_bit(TX_BUSY, &dws->dma_chan_busy);
		dmaengine_submit(txdesc);
		dma_async_issue_pending(dws->txchan);
	}

	return 0;
}

static void nx_spi_dma_stop(struct dw_spi *dws)
{
	if (test_bit(TX_BUSY, &dws->dma_chan_busy)) {
		dmaengine_terminate_sync(dws->txchan);
		clear_bit(TX_BUSY, &dws->dma_chan_busy);
	}
	if (test_bit(RX_BUSY, &dws->dma_chan_busy)) {
		dmaengine_terminate_sync(dws->rxchan);
		clear_bit(RX_BUSY, &dws->dma_chan_busy);
	}
}

static const struct dw_spi_dma_ops nx_dma_ops = {
	.dma_init	= nx_spi_dma_init,
	.dma_exit	= nx_spi_dma_exit,
	.dma_setup	= nx_spi_dma_setup,
	.can_dma	= nx_spi_can_dma,
	.dma_transfer	= nx_spi_dma_transfer,
	.dma_stop	= nx_spi_dma_stop,
};

/* HW info for MRST Clk Control Unit, 32b reg per controller */
#define MRST_SPI_CLK_BASE	100000000	/* 100m */
#define MRST_CLK_SPI_REG	0xff11d86c
#define CLK_SPI_BDIV_OFFSET	0
#define CLK_SPI_BDIV_MASK	0x00000007
#define CLK_SPI_CDIV_OFFSET	9
#define CLK_SPI_CDIV_MASK	0x00000e00
#define CLK_SPI_DISABLE_OFFSET	8

static int nx_dw_spi_dma_init(struct dw_spi *dws)
{
	void __iomem *clk_reg;
	u32 clk_cdiv;

	clk_reg = ioremap_nocache(MRST_CLK_SPI_REG, 16);
	if (!clk_reg)
		return -ENOMEM;

	/* Get SPI controller operating freq info */
	clk_cdiv = readl(clk_reg + dws->bus_num * sizeof(u32));
	clk_cdiv &= CLK_SPI_CDIV_MASK;
	clk_cdiv >>= CLK_SPI_CDIV_OFFSET;
	dws->max_freq = MRST_SPI_CLK_BASE / (clk_cdiv + 1);

	iounmap(clk_reg);

	dws->dma_tx = &nx_dma_tx;
	dws->dma_rx = &nx_dma_rx;
	dws->dma_ops = &nx_dma_ops;

	return 0;
}

static int nx_dw_spi_probe(struct platform_device *pdev)
{
	struct dw_spi_nx *dws_nx;
	struct dw_spi *dws;
	struct resource *mem;
	int ret;

	dws_nx = devm_kzalloc(&pdev->dev, sizeof(struct dw_spi_nx),
			GFP_KERNEL);
	if (!dws_nx)
		return -ENOMEM;

	dws = &dws_nx->dws;

	/* Get basic io resource and map it */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dws->regs = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(dws->regs)) {
		dev_err(&pdev->dev, "SPI region map failed\n");
		return PTR_ERR(dws->regs);
	}

	dws->irq = platform_get_irq(pdev, 0);
	if (dws->irq < 0) {
		dev_err(&pdev->dev, "no irq resource?\n");
		return dws->irq; /* -ENXIO */
	}

	dws_nx->pclk = devm_clk_get(&pdev->dev, "apb");
	if (IS_ERR(dws_nx->pclk)) {
		return PTR_ERR(dws_nx->pclk);
	}
	ret = clk_prepare_enable(dws_nx->pclk);
	if (ret)
		return ret;

	dws_nx->clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(dws_nx->clk)) {
		ret = PTR_ERR(dws_nx->clk);
		goto err_clk;
	}
	ret = clk_prepare_enable(dws_nx->clk);
	if (ret)
		goto err_clk;

	dws->bus_num = pdev->id;
	dws->max_freq = clk_get_rate(dws_nx->pclk);

	if (device_property_read_u32(&pdev->dev, "spi-mode", &dws->spi_mode))
		dws->spi_mode = 0;
	if (device_property_read_u32(&pdev->dev, "num-cs", &dws->num_cs))
		dws->num_cs = 1;
	if (device_property_read_u32(&pdev->dev, "reg-io-width",
				&dws->reg_io_width))
		dws->reg_io_width = 4;

	if (pdev->dev.of_node) {
		int i;

		for (i = 0; i < dws->num_cs; i++) {
			int cs_gpio = of_get_named_gpio(pdev->dev.of_node,
					"cs-gpios", i);

			if (cs_gpio == -EPROBE_DEFER) {
				ret = cs_gpio;
				goto err_pclk;
			}

			if (gpio_is_valid(cs_gpio)) {
				ret = devm_gpio_request(&pdev->dev, cs_gpio,
						dev_name(&pdev->dev));
				if (ret)
					goto err_pclk;
			}
		}
	}

	if (of_find_property(pdev->dev.of_node, "support-dma", NULL)) {
		dws->master->dev = pdev->dev;
		nx_dw_spi_dma_init(dws);
	}

	ret = dw_spi_add_host(&pdev->dev, dws);
	if (ret)
		goto err_pclk;

	platform_set_drvdata(pdev, dws_nx);

	return 0;

err_pclk:
	clk_disable_unprepare(dws_nx->pclk);

err_clk:
	clk_disable_unprepare(dws_nx->clk);

	return ret;
}

static int nx_dw_spi_remove(struct platform_device *pdev)
{
	struct dw_spi_nx *dws_nx = platform_get_drvdata(pdev);

	dw_spi_remove_host(&dws_nx->dws);
	clk_disable_unprepare(dws_nx->pclk);
	clk_disable_unprepare(dws_nx->clk);

	return 0;
}

static const struct of_device_id nx_dw_spi_of_match[] = {
	{ .compatible = "nexell,swallow-dw-spi", },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, nx_dw_spi_of_match);

static struct platform_driver nx_dw_spi_driver = {
	.probe		= nx_dw_spi_probe,
	.remove		= nx_dw_spi_remove,
	.driver		= {
		.name	= "dw_spi_nx",
		.of_match_table = nx_dw_spi_of_match,
	},
};
module_platform_driver(nx_dw_spi_driver);

MODULE_AUTHOR("ChoongHyun Jeon <suker@nexell.co.kr>");
MODULE_DESCRIPTION("Nexell interface driver for DW SPI Core");
MODULE_LICENSE("GPL v2");
