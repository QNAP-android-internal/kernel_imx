// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025-2026 NXP
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uio_driver.h>

/*
 * This structure holds the private data associated with a UIO device,
 * typically used in a platform that exposes hardware to userspace via the UIO framework
 */
struct uio_prime {
	struct uio_info *uio;	// Ptr to  UIO info structure registered with the kernel
	void __iomem *regs;	// Virtual address of the memory-mapped I/O region
	u32 irq;		// IRQ number assigned to the device
};

static int prime_probe(struct platform_device *pdev)
{
	struct uio_prime *prime_priv;
	struct uio_info *uio;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct device_node *rmem_np;
	int ret;

	prime_priv = devm_kzalloc(dev, sizeof(struct uio_prime), GFP_KERNEL);
	if (!prime_priv)
		return -ENOMEM;

	uio = devm_kzalloc(dev, sizeof(struct uio_info), GFP_KERNEL);
	if (!uio)
		return -ENOMEM;

	/* MMIO resource 0: device registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || !res->start) {
		dev_err(dev, "no MMIO resource\n");
		return -ENODEV;
	}

	prime_priv->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(prime_priv->regs))
		return PTR_ERR(prime_priv->regs);

	/* Fill UIO info */
	uio->name = "PRIME UIO";
	uio->version = "PRIME UIO Driver 1.0";
	uio->priv = prime_priv;

	/* Map the register window to userspace */
	uio->mem[0].name = "PRIME MMIO";
	uio->mem[0].addr = res->start;
	uio->mem[0].size = resource_size(res);
	uio->mem[0].memtype = UIO_MEM_PHYS;
	uio->mem[0].internal_addr = prime_priv->regs;

	rmem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (rmem_np) {
		struct reserved_mem *rmem = of_reserved_mem_lookup(rmem_np);

		of_node_put(rmem_np);
		if (rmem) {
			uio->mem[1].name = "PRIME Reserved Memory";
			uio->mem[1].addr = rmem->base;
			uio->mem[1].size = rmem->size;
			uio->mem[1].memtype = UIO_MEM_PHYS;
			uio->mem[1].internal_addr = NULL;
		} else
			dev_warn(dev, "Reserved memory not found; map1 not created\n");
	}

	ret = devm_uio_register_device(dev, uio);
	if (ret) {
		dev_err(dev, "UIO register failed: %d\n", ret);
		return ret;
	}

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev, "reserved mem init failed: %d\n", ret);
		return ret;
	}

	prime_priv->uio = uio;
	platform_set_drvdata(pdev, prime_priv);
	dev_info(dev, "%s initialized (mmio=%pa..%pa)\n",
		 uio->name, &res->start, &res->end);

	return 0;
}

static const struct of_device_id prime_of_match[] = {
	{ .compatible = "fsl,imx94-mu-prime", },
	{ .compatible = "fsl,imx95-mu-prime", },
	{},
};

MODULE_DEVICE_TABLE(of, prime_of_match);

static struct platform_driver prime_drv = {
	.driver = {
		.name = "prime-uio",
		.of_match_table = prime_of_match,
	},
	.probe = prime_probe,
};

module_platform_driver(prime_drv);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("PRIME UIO Driver");
