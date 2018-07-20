// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018  Nexell Co., Ltd.
 * Author: Seoji Kim <seoji@nexell.co.kr>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/suspend.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/reset.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/driver.h>
#include <linux/iio/machine.h>

/*
 * ADC definitions
 */
#define	ADC_MAX_SAMPLE_RATE	(1*1000*1000)	/* with 6bit */
#define	ADC_MAX_SAMPLE_BITS	6
#define	ADC_MAX_PRESCALE	256		/* 8bit */
#define	ADC_MIN_PRESCALE	20

#define ADC_TIMEOUT		(msecs_to_jiffies(100))
#define MAX_ADC_V1_CHANNELS	4
#define MAX_ADC_V2_CHANNELS	4

/* Register definitions for ADC_V1 */
#define ADC_V1_CON(x)		((x) + 0x00)
#define ADC_V1_DAT(x)		((x) + 0x04)
#define ADC_V1_INTENB(x)	((x) + 0x08)
#define ADC_V1_INTCLR(x)	((x) + 0x0c)
#define ADC_V1_PRESCON(x)	((x) + 0x10)

/* Bit definitions for ADC_V1 */
#define ADC_V1_CON_APEN		(1u << 14)
#define ADC_V1_CON_APSV(x)	(((x) & 0xff) << 6)
#define ADC_V1_CON_ASEL(x)	(((x) & 0x7) << 3)
#define ADC_V1_CON_STBY		(1u << 2)
#define ADC_V1_CON_ADEN		(1u << 0)
#define ADC_V1_INTENB_ENB	(1u << 0)
#define ADC_V1_INTCLR_CLR	(1u << 0)

/* Register definitions for ADC_V2 */
#define ADC_V2_CON(x)		((x) + 0x00)
#define ADC_V2_DAT(x)		((x) + 0x04)
#define ADC_V2_INTENB(x)	((x) + 0x08)
#define ADC_V2_INTCLR(x)	((x) + 0x0c)
#define ADC_V2_PRESCON(x)	((x) + 0x10)

/* Bit definitions for ADC_V2 */
#define ADC_V2_CON_DATA_SEL(x)	(((x) & 0xf) << 10)
#define ADC_V2_CON_CLK_CNT(x)	(((x) & 0xf) << 6)
#define ADC_V2_CON_ASEL(x)	(((x) & 0x7) << 3)
#define ADC_V2_CON_STBY		(1u << 2)
#define ADC_V2_CON_ADEN		(1u << 0)
#define ADC_V2_INTENB_ENB	(1u << 0)
#define ADC_V2_INTCLR_CLR	(1u << 0)
#define ADC_V2_PRESCON_APEN	(1u << 15)
#define ADC_V2_PRESCON_PRES(x)	(((x) & 0x3ff) << 0)

#define ADC_V2_DATA_SEL_VAL	(0)	/* 0:5clk, 1:4clk, 2:3clk, 3:2clk */
					/* 4:1clk: 5:not delayed, else: 4clk */
#define ADC_V2_CLK_CNT_VAL	(6)	/* 28nm ADC */

/* Register definitions for ADC_V3 */
#define ADC_V3_EN(x)		((x) + 0x18)

#define ADC_CHANNEL(_index, _addr, _id) {		\
	.type = IIO_VOLTAGE,				\
	.indexed = 1,					\
	.channel = _index,				\
	.address = _addr,				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
	.datasheet_name = _id,				\
}

static struct iio_chan_spec nexell_adc_channels[] = {
	ADC_CHANNEL(0, 0, "adc0"),
	ADC_CHANNEL(1, 1, "adc1"),
	ADC_CHANNEL(2, 2, "adc2"),
	ADC_CHANNEL(3, 3, "adc3"),
};

static struct iio_chan_spec nexell_adc_v3_channels[] = {
	ADC_CHANNEL(0, 0, "adc0"),
	ADC_CHANNEL(1, 1, "adc1"),
	ADC_CHANNEL(2, 2, "adc2"),
	ADC_CHANNEL(3, 3, "adc3"),
	ADC_CHANNEL(6, 4, "adc6"),
	ADC_CHANNEL(7, 5, "adc7"),
	ADC_CHANNEL(8, 6, "adc8"),
	ADC_CHANNEL(9, 7, "adc9"),
};

/*
 * ADC data
 */
struct nexell_adc_info {
	struct nexell_adc_data *data;
	void __iomem *adc_base;
	ulong clk_rate;
	ulong sample_rate;
	ulong max_sample_rate;
	ulong min_sample_rate;
	int value;
	int prescale;
	spinlock_t lock;
	struct completion completion;
	int irq;
	struct clk *clk;
	struct iio_map *map;
	struct reset_control *rst;
};

struct nexell_adc_data {
	int version;
	int num_channels;
	struct iio_chan_spec *channels;
	int (*adc_con)(struct nexell_adc_info *adc);
	int (*read_polling)(struct nexell_adc_info *adc, int ch);
	int (*read_val)(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan,
			int *val,
			int *val2,
			long mask);
};

static int nexell_adc_remove_devices(struct device *dev, void *c)
{
	struct platform_device *pdev = to_platform_device(dev);

	platform_device_unregister(pdev);

	return 0;
}

static int setup_adc_con(struct nexell_adc_info *adc)
{
	void __iomem *reg = adc->adc_base;

	if (adc->data->version == 3)
		if ((readl(ADC_V3_EN(reg)) & 0x1) == 0)
			writel(0x1, ADC_V3_EN(reg));

	if (adc->data->adc_con)
		adc->data->adc_con(adc);

	return 0;
}

/* you must irq re-enable after sampling */
static void nexell_adc_v1_ch_start(void __iomem *reg, int ch)
{
	unsigned int adcon = 0;

	adcon = readl(ADC_V1_CON(reg)) & ~ADC_V1_CON_ASEL(7);
	adcon &= ~ADC_V1_CON_ADEN;
	adcon |= ADC_V1_CON_ASEL(ch);	/* channel */
	writel(adcon, ADC_V1_CON(reg));
	adcon = readl(ADC_V1_CON(reg));

	adcon |= ADC_V1_CON_ADEN;	/* start */
	local_irq_disable();
	writel(adcon, ADC_V1_CON(reg));
}

static int nexell_adc_v1_read_polling(struct nexell_adc_info *adc, int ch)
{
	void __iomem *reg = adc->adc_base;
	unsigned long wait = loops_per_jiffy * (HZ/10);

	nexell_adc_v1_ch_start(reg, ch);

	while (wait > 0) {
		if (!(readl(ADC_V1_CON(reg)) & ADC_V1_CON_ADEN)) {
			/* get value */
			adc->value = readl(ADC_V1_DAT(reg));
			/* pending clear */
			writel(ADC_V1_INTCLR_CLR, ADC_V1_INTCLR(reg));
			break;
		}
		wait--;
	}
	local_irq_enable();
	if (wait == 0)
		return -ETIMEDOUT;

	return 0;
}

static int nexell_adc_v1_adc_con(struct nexell_adc_info *adc)
{
	unsigned int adcon = 0;
	void __iomem *reg = adc->adc_base;

	adcon = ADC_V1_CON_APSV(adc->prescale);
	adcon &= ~ADC_V1_CON_STBY;
	writel(adcon, ADC_V1_CON(reg));
	adcon |= ADC_V1_CON_APEN;
	writel(adcon, ADC_V1_CON(reg));

	/* *****************************************************
	 * Turn-around invalid value after Power On
	 * *****************************************************/
	nexell_adc_v1_read_polling(adc, 0);
	adc->value = 0;

	writel(ADC_V1_INTCLR_CLR, ADC_V1_INTCLR(reg));
	writel(ADC_V1_INTENB_ENB, ADC_V1_INTENB(reg));
	init_completion(&adc->completion);

	return 0;
}

static int nexell_adc_v1_read_val(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int *val,
		int *val2,
		long mask)
{
	struct nexell_adc_info *adc = iio_priv(indio_dev);
	int ret = 0;

	reinit_completion(&adc->completion);

	if (adc->data->read_polling)
		ret = adc->data->read_polling(adc, chan->address);
	if (ret < 0) {
		dev_warn(&indio_dev->dev,
				"Conversion timed out! resetting...\n");
		if (adc->rst)
			reset_control_reset(adc->rst);
		setup_adc_con(adc);
		ret = -ETIMEDOUT;
	}

	return ret;
}


static void nexell_adc_v2_ch_start(void __iomem *reg, int ch)
{
	unsigned int adcon = 0;

	adcon = readl(ADC_V2_CON(reg)) & ~ADC_V2_CON_ASEL(7);
	adcon &= ~ADC_V2_CON_ADEN;
	adcon |= ADC_V2_CON_ASEL(ch);	/* channel */
	writel(adcon, ADC_V2_CON(reg));
	adcon = readl(ADC_V2_CON(reg));

	adcon |= ADC_V2_CON_ADEN;	/* conversion start */
	writel(adcon, ADC_V2_CON(reg));
}

static int nexell_adc_v2_read_polling(struct nexell_adc_info *adc, int ch)
{
	void __iomem *reg = adc->adc_base;
	unsigned long wait = loops_per_jiffy * (HZ/10);

	nexell_adc_v2_ch_start(reg, ch);

	while (wait > 0) {
		if (readl(ADC_V2_INTCLR(reg)) & ADC_V2_INTCLR_CLR) {
			/* pending clear */
			writel(ADC_V2_INTCLR_CLR, ADC_V2_INTCLR(reg));
			/* get value */
			adc->value = readl(ADC_V2_DAT(reg));
			break;
		}
		wait--;
	}
	if (wait == 0)
		return -ETIMEDOUT;

	return 0;
}

static int nexell_adc_v2_adc_con(struct nexell_adc_info *adc)
{
	unsigned int adcon = 0;
	unsigned int pres = 0;
	void __iomem *reg = adc->adc_base;

	adcon = ADC_V2_CON_DATA_SEL(ADC_V2_DATA_SEL_VAL) |
		ADC_V2_CON_CLK_CNT(ADC_V2_CLK_CNT_VAL);
	adcon &= ~ADC_V2_CON_STBY;
	writel(adcon, ADC_V2_CON(reg));

	pres = ADC_V2_PRESCON_PRES(adc->prescale);
	writel(pres, ADC_V2_PRESCON(reg));
	pres |= ADC_V2_PRESCON_APEN;
	writel(pres, ADC_V2_PRESCON(reg));

	/* *****************************************************
	 * Turn-around invalid value after Power On
	 * *****************************************************/
	nexell_adc_v2_read_polling(adc, 0);
	adc->value = 0;

	writel(ADC_V2_INTCLR_CLR, ADC_V2_INTCLR(reg));
	writel(ADC_V2_INTENB_ENB, ADC_V2_INTENB(reg));
	init_completion(&adc->completion);

	return 0;
}

static int nexell_adc_v2_read_val(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int *val,
		int *val2,
		long mask)
{
	struct nexell_adc_info *adc = iio_priv(indio_dev);
	void __iomem *reg = adc->adc_base;
	unsigned long timeout;
	int ret = 0;

	reinit_completion(&adc->completion);

	nexell_adc_v2_ch_start(reg, chan->address);

	timeout = wait_for_completion_timeout(&adc->completion, ADC_TIMEOUT);
	if (timeout == 0) {
		dev_warn(&indio_dev->dev,
				"Conversion timed out! resetting...\n");
		if (adc->rst)
			reset_control_reset(adc->rst);
		setup_adc_con(adc);
		ret = -ETIMEDOUT;
	}

	return ret;
}

static const struct nexell_adc_data nexell_adc_swallow_data = {
	.version	= 3,
	.num_channels	= MAX_ADC_V2_CHANNELS,
	.channels	= nexell_adc_channels,
	.adc_con	= nexell_adc_v2_adc_con,
	.read_polling	= nexell_adc_v2_read_polling,
	.read_val	= nexell_adc_v2_read_val,
};

#ifdef CONFIG_OF
static const struct of_device_id nexell_adc_match[] = {
	{
		.compatible = "nexell,swallow-adc",
		.data = &nexell_adc_swallow_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nexell_adc_match);

static struct nexell_adc_data *nexell_adc_get_data(struct platform_device *pdev)
{
	const struct of_device_id *match;

	match = of_match_node(nexell_adc_match, pdev->dev.of_node);
	return (struct nexell_adc_data *)match->data;
}
#endif

/*
 * ADC functions
 */
static irqreturn_t nexell_adc_v2_isr(int irq, void *dev_id)
{
	struct nexell_adc_info *adc = (struct nexell_adc_info *)dev_id;
	void __iomem *reg = adc->adc_base;

	writel(ADC_V2_INTCLR_CLR, ADC_V2_INTCLR(reg)); /* pending clear */
	adc->value = readl(ADC_V2_DAT(reg)); /* get value */

	complete(&adc->completion);

	return IRQ_HANDLED;
}

static int nexell_adc_setup(struct nexell_adc_info *adc,
		struct platform_device *pdev)
{
	ulong min_rate;
	uint32_t sample_rate;
	int prescale = 0;

	of_property_read_u32(pdev->dev.of_node, "sample_rate", &sample_rate);

	prescale = (adc->clk_rate) / (sample_rate * ADC_MAX_SAMPLE_BITS);
	min_rate = (adc->clk_rate) / (ADC_MAX_PRESCALE * ADC_MAX_SAMPLE_BITS);

	if (sample_rate > ADC_MAX_SAMPLE_RATE ||
			min_rate > sample_rate) {
		dev_err(&pdev->dev, "not support %u(%d ~ %lu) sample rate\n",
			sample_rate, ADC_MAX_SAMPLE_RATE, min_rate);
		return -EINVAL;
	}

	adc->sample_rate = sample_rate;
	adc->max_sample_rate = ADC_MAX_SAMPLE_RATE;
	adc->min_sample_rate = min_rate;
	adc->prescale = prescale;

	setup_adc_con(adc);

	dev_info(&pdev->dev, "CHs %d, %ld(%ld ~ %ld) sample rate, scale=%d(bit %d)\n",
		adc->data->num_channels,
		adc->sample_rate,
		adc->max_sample_rate, adc->min_sample_rate,
		prescale, ADC_MAX_SAMPLE_BITS);

	return 0;
}

static int nexell_read_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int *val,
		int *val2,
		long mask)
{
	struct nexell_adc_info *adc = iio_priv(indio_dev);
	int ret;

	mutex_lock(&indio_dev->mlock);

	if (adc->data->read_val) {
		ret = adc->data->read_val(indio_dev, chan, val, val2, mask);
		if (ret < 0)
			goto out;
	}

	*val = adc->value;
	*val2 = 0;
	ret = IIO_VAL_INT;

	dev_dbg(&indio_dev->dev, "ch=%d, val=0x%x\n", chan->channel, *val);

out:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static const struct iio_info nexell_adc_iio_info = {
	.read_raw = &nexell_read_raw,
};

static int nexell_adc_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int nexell_adc_resume(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct nexell_adc_info *adc = iio_priv(indio_dev);

	if (adc->rst)
		reset_control_reset(adc->rst);

	setup_adc_con(adc);

	return 0;
}

static int nexell_adc_probe(struct platform_device *pdev)
{
	struct iio_dev *iio = NULL;
	struct nexell_adc_info *adc = NULL;
	struct resource	*mem;
	struct device_node *np = pdev->dev.of_node;
	int irq;
	int ret = -ENODEV;

	if (!np)
		return ret;

	iio = devm_iio_device_alloc(&pdev->dev, sizeof(struct nexell_adc_info));
	if (!iio) {
		dev_err(&pdev->dev, "failed allocating iio ADC device\n");
		return -ENOMEM;
	}

	adc = iio_priv(iio);

	adc->data = nexell_adc_get_data(pdev);
	if (!adc->data) {
		dev_err(&pdev->dev, "failed getting nexell ADC data\n");
		return -EINVAL;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	adc->adc_base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(adc->adc_base))
		return PTR_ERR(adc->adc_base);

	adc->clk = devm_clk_get(&pdev->dev, "adc0_apb");
	if (IS_ERR(adc->clk)) {
		dev_err(&pdev->dev, "failed getting clock for ADC\n");
		return PTR_ERR(adc->clk);
	}
	adc->clk_rate = clk_get_rate(adc->clk);
	clk_prepare_enable(adc->clk);

	/* setup: reset */
	if (adc->data->version != 3) {
		adc->rst = devm_reset_control_get(&pdev->dev, "adc-reset");
		if (IS_ERR(adc->rst)) {
			dev_err(&pdev->dev, "failed to get reset\n");
			return PTR_ERR(adc->rst);
		}

		reset_control_reset(adc->rst);
	}

	/* setup: irq */
	if (adc->data->version >= 2) {
		irq = platform_get_irq(pdev, 0);
		if (irq < 0) {
			dev_err(&pdev->dev, "failed get irq resource\n");
			goto err_unprepare_clk;
		}

		ret = devm_request_irq(&pdev->dev, irq, nexell_adc_v2_isr,
				0, dev_name(&pdev->dev), adc);
		if (ret < 0) {
			dev_err(&pdev->dev, "failed get irq (%d)\n", irq);
			goto err_unprepare_clk;
		}

		adc->irq = irq;
	}

	/* setup: adc */
	ret = nexell_adc_setup(adc, pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed setup iio ADC device\n");
		goto err_unprepare_clk;
	}
	platform_set_drvdata(pdev, iio);

	iio->name = dev_name(&pdev->dev);
	iio->driver_module = THIS_MODULE;
	iio->dev.parent = &pdev->dev;
	iio->info = &nexell_adc_iio_info;
	iio->modes = INDIO_DIRECT_MODE;
	iio->channels = adc->data->channels;
	iio->num_channels = adc->data->num_channels;
	iio->dev.of_node = pdev->dev.of_node;

	/*
	 * sys interface : user interface
	 */
	ret = devm_iio_device_register(&pdev->dev, iio);
	if (ret)
		goto err_unprepare_clk;

	ret = of_platform_populate(np, nexell_adc_match, NULL, &pdev->dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed adding child nodes\n");
		goto err_of_populate;
	}

	dev_dbg(&pdev->dev, "ADC init success\n");

	return 0;

err_of_populate:
	device_for_each_child(&pdev->dev, NULL,
			nexell_adc_remove_devices);
err_unprepare_clk:
	clk_disable_unprepare(adc->clk);

	return ret;
}

static int nexell_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *iio = platform_get_drvdata(pdev);
	struct nexell_adc_info *adc = iio_priv(iio);

	device_for_each_child(&pdev->dev, NULL,
			nexell_adc_remove_devices);
	clk_disable_unprepare(adc->clk);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver nexell_adc_driver = {
	.probe		= nexell_adc_probe,
	.remove		= nexell_adc_remove,
	.suspend	= nexell_adc_suspend,
	.resume		= nexell_adc_resume,
	.driver		= {
		.name	= "nexell-adc",
		.owner	= THIS_MODULE,
		.of_match_table = nexell_adc_match,
	},
};

module_platform_driver(nexell_adc_driver);

MODULE_AUTHOR("Seoji Kim <seoji@nexell.co.kr>");
MODULE_DESCRIPTION("ADC driver for the Nexell SoC");
MODULE_LICENSE("GPL v2");
