// SPDX-License-Identifier: GPL-2.0-only
/* Apple S8000 Samsung SPI v1 controller, bounded byte-wide PIO. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>

#define S8000_CTRL		0x00
#define S8000_CTRL_ENABLE	0x0d
#define S8000_CFG		0x04
#define S8000_CFG_BASE		0x4018
#define S8000_CFG_CPHA		BIT(1)
#define S8000_CFG_CPOL		BIT(2)
#define S8000_CFG_PIO		BIT(5)
#define S8000_STATUS		0x08
#define S8000_STATUS_CLEAR	0x40000f
#define S8000_TX_LEVEL		GENMASK(10, 6)
#define S8000_RX_LEVEL		GENMASK(15, 11)
#define S8000_PIN		0x0c
#define S8000_PIN_CS		BIT(1)
#define S8000_TXDATA		0x10
#define S8000_RXDATA		0x20
#define S8000_CLKDIV		0x30
#define S8000_RXCNT		0x34
#define S8000_WORD_DELAY		0x38
#define S8000_TXCNT		0x4c
#define S8000_FIFO_DEPTH		16
#define S8000_DIV_MAX		2047
#define S8000_XFER_TIMEOUT_US	1000000

static bool autosuspend;
module_param(autosuspend, bool, 0444);
MODULE_PARM_DESC(autosuspend, "Experimental PIO domain autosuspend (default: off)");

struct s8000_spi {
	void __iomem *base;
	struct clk *clk;
	bool held;
};

static void s8000_spi_pm_put(void *data)
{
	struct device *dev = data;
	struct spi_controller *ctlr = dev_get_drvdata(dev);
	struct s8000_spi *spi = spi_controller_get_devdata(ctlr);

	pm_runtime_dont_use_autosuspend(dev);
	if (spi->held)
		pm_runtime_put_sync(dev);
	else
		pm_runtime_barrier(dev);
}

static void s8000_spi_set_cs(struct spi_device *device, bool high)
{
	struct s8000_spi *spi = spi_controller_get_devdata(device->controller);

	/* Descriptor-backed chip selects belong to the SPI core. */
	if (!spi_get_csgpiod(device, 0))
		writel(high ? S8000_PIN_CS : 0, spi->base + S8000_PIN);
}

static size_t s8000_spi_max_transfer_size(struct spi_device *device)
{
	/* Bound both FIFO work and worst-case wall time for malformed clients. */
	return SZ_64K;
}

static int s8000_spi_transfer_one(struct spi_controller *ctlr,
				 struct spi_device *device,
				 struct spi_transfer *transfer)
{
	struct s8000_spi *spi = spi_controller_get_devdata(ctlr);
	const u8 *tx = transfer->tx_buf;
	u8 *rx = transfer->rx_buf;
	unsigned long rate = clk_get_rate(spi->clk);
	unsigned int sent = 0, received = 0, iterations = 0;
	u32 speed = transfer->speed_hz ?: device->max_speed_hz;
	u32 divisor, cfg, status, tx_level, rx_level;
	u64 word_delay_cycles;
	ktime_t deadline;
	int word_delay_ns, ret = 0;

	if (!transfer->len)
		return 0;
	if (transfer->bits_per_word != 8 || !speed || !rate ||
	    transfer->len > SZ_64K)
		return -EINVAL;
	divisor = DIV_ROUND_UP(rate, speed);
	if (!divisor || divisor > S8000_DIV_MAX)
		return -EINVAL;
	transfer->effective_speed_hz = rate / divisor;
	/* The SPI core merges the device minimum into transfer->word_delay. */
	word_delay_ns = spi_delay_to_ns(&transfer->word_delay, transfer);
	if (word_delay_ns < 0)
		return word_delay_ns;
	/* WORD_DELAY counts input-clock cycles, not divided SCK cycles. */
	word_delay_cycles = DIV_ROUND_UP_ULL((u64)rate * word_delay_ns,
					    NSEC_PER_SEC);
	if (word_delay_cycles > U32_MAX)
		return -EINVAL;
	cfg = S8000_CFG_BASE;
	if (device->mode & SPI_CPHA)
		cfg |= S8000_CFG_CPHA;
	if (device->mode & SPI_CPOL)
		cfg |= S8000_CFG_CPOL;

	/* spi-version 1 has FIFO counts in STATUS, unlike the M1 controller. */
	writel(0, spi->base + S8000_CTRL);
	writel(divisor, spi->base + S8000_CLKDIV);
	writel(word_delay_cycles, spi->base + S8000_WORD_DELAY);
	writel(S8000_STATUS_CLEAR, spi->base + S8000_STATUS);
	writel(cfg, spi->base + S8000_CFG);
	writel(S8000_CTRL_ENABLE, spi->base + S8000_CTRL);
	writel(transfer->len, spi->base + S8000_RXCNT);
	writel(transfer->len, spi->base + S8000_TXCNT);
	writel(cfg | S8000_CFG_PIO, spi->base + S8000_CFG);
	deadline = ktime_add_us(ktime_get(), S8000_XFER_TIMEOUT_US);

	while (received < transfer->len) {
		status = readl(spi->base + S8000_STATUS);
		tx_level = FIELD_GET(S8000_TX_LEVEL, status);
		rx_level = FIELD_GET(S8000_RX_LEVEL, status);
		if (tx_level > S8000_FIFO_DEPTH || rx_level > S8000_FIFO_DEPTH ||
		    rx_level > transfer->len - received) {
			ret = -EIO;
			break;
		}
		writel(status, spi->base + S8000_STATUS);
		while (rx_level--) {
			u8 value = readl(spi->base + S8000_RXDATA);

			if (rx)
				rx[received] = value;
			received++;
		}
		/* Never queue more than one RX FIFO's worth of unconsumed data. */
		while (sent < transfer->len && tx_level < S8000_FIFO_DEPTH &&
		       sent - received < S8000_FIFO_DEPTH) {
			writel(tx ? tx[sent] : 0xff, spi->base + S8000_TXDATA);
			sent++;
			tx_level++;
		}
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			dev_err(&device->dev,
				"PIO timeout: tx %u/%u rx %u/%u status=%#x\n",
				sent, transfer->len, received, transfer->len, status);
			ret = -ETIMEDOUT;
			break;
		}
		if (!(++iterations & 0xff))
			cond_resched();
		cpu_relax();
	}
	writel(cfg, spi->base + S8000_CFG);
	writel(0, spi->base + S8000_CTRL);
	writel(S8000_STATUS_CLEAR, spi->base + S8000_STATUS);
	return ret;
}

static int s8000_spi_runtime_suspend(struct device *dev)
{
	struct s8000_spi *spi = spi_controller_get_devdata(dev_get_drvdata(dev));

	/* SPI core holds a PM reference across the entire message, including CS. */
	writel(0, spi->base + S8000_CTRL);
	readl(spi->base + S8000_CTRL);
	return 0;
}

static int s8000_spi_runtime_resume(struct device *dev)
{
	struct s8000_spi *spi = spi_controller_get_devdata(dev_get_drvdata(dev));

	if (!autosuspend)
		return 0;
	/* genpd has powered the block before this callback. No IRQs or DMA. */
	writel(0, spi->base + S8000_CTRL);
	writel(S8000_PIN_CS, spi->base + S8000_PIN);
	writel(S8000_CFG_BASE, spi->base + S8000_CFG);
	writel(S8000_STATUS_CLEAR, spi->base + S8000_STATUS);
	/* CLKDIV, WORD_DELAY, CFG and counts are programmed per transfer. */
	return 0;
}

static const struct dev_pm_ops s8000_spi_pm_ops = {
	SET_RUNTIME_PM_OPS(s8000_spi_runtime_suspend, s8000_spi_runtime_resume, NULL)
};

static int s8000_spi_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct s8000_spi *spi;
	unsigned long rate;
	int ret;

	ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*spi));
	if (!ctlr)
		return -ENOMEM;
	spi = spi_controller_get_devdata(ctlr);
	spi->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spi->base))
		return PTR_ERR(spi->base);
	spi->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(spi->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(spi->clk), "missing SPI input clock\n");
	rate = clk_get_rate(spi->clk);
	if (!rate)
		return -EINVAL;
	platform_set_drvdata(pdev, ctlr);
	if (autosuspend) {
		pm_runtime_set_autosuspend_delay(&pdev->dev, 1000);
		pm_runtime_use_autosuspend(&pdev->dev);
	}
	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0)
		return ret;
	spi->held = true;
	/* Keep the default bring-up reference unless explicitly opted in. */
	ret = devm_add_action_or_reset(&pdev->dev, s8000_spi_pm_put, &pdev->dev);
	if (ret)
		return ret;

	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = SPI_CPHA | SPI_CPOL;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->min_speed_hz = DIV_ROUND_UP(rate, S8000_DIV_MAX);
	ctlr->max_speed_hz = rate;
	ctlr->set_cs = s8000_spi_set_cs;
	ctlr->transfer_one = s8000_spi_transfer_one;
	ctlr->max_transfer_size = s8000_spi_max_transfer_size;
	ctlr->use_gpio_descriptors = true;
	ctlr->auto_runtime_pm = autosuspend;
	/* No controller IRQ enable bits are set: every PIO transfer is bounded. */
	writel(0, spi->base + S8000_CTRL);
	dev_info(&pdev->dev, "Samsung SPI v1 PIO, input clock %lu Hz\n", rate);
	ret = devm_spi_register_controller(&pdev->dev, ctlr);
	if (ret)
		return ret;
	if (autosuspend) {
		spi->held = false;
		pm_runtime_mark_last_busy(&pdev->dev);
		pm_runtime_put_autosuspend(&pdev->dev);
		dev_info(&pdev->dev, "experimental 1000ms domain autosuspend enabled\n");
	}
	return 0;
}

static const struct of_device_id s8000_spi_match[] = {
	{ .compatible = "apple,s8000-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, s8000_spi_match);

static struct platform_driver s8000_spi_driver = {
	.probe = s8000_spi_probe,
	.driver = {
		.name = "apple-s8000-spi",
		.of_match_table = s8000_spi_match,
		.pm = &s8000_spi_pm_ops,
	},
};
module_platform_driver(s8000_spi_driver);
MODULE_DESCRIPTION("Apple S8000 Samsung SPI v1 PIO controller");
MODULE_LICENSE("GPL");
