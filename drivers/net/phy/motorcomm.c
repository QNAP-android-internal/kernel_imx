// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Motorcomm PHYs
 *
 * Author: Peter Geis <pgwipeout@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/phy.h>

#define PHY_ID_YT8511		0x0000010a
#define PHY_ID_YT8521		0x0000011a

#define YT8511_PAGE_SELECT	0x1e
#define YT8511_PAGE		0x1f
#define YT8511_EXT_CLK_GATE	0x0c
#define YT8511_EXT_DELAY_DRIVE	0x0d
#define YT8511_EXT_SLEEP_CTRL	0x27

/* Extended Register's Address Offset Register (0x1E)
 * Extended Register's Data Register (0x1F)
*/
#define EXT_REG_ADDR_OFFSET	0x1e
#define EXT_REG_DATA		0x1f

/* 2b00 25m from pll
 * 2b01 25m from xtl *default*
 * 2b10 62.m from pll
 * 2b11 125m from pll
 */
#define YT8511_CLK_125M		(BIT(2) | BIT(1))
#define YT8511_PLLON_SLP	BIT(14)

/* RX Delay enabled = 1.8ns 1000T, 8ns 10/100T */
#define YT8511_DELAY_RX		BIT(0)

/* TX Gig-E Delay is bits 7:4, default 0x5
 * TX Fast-E Delay is bits 15:12, default 0xf
 * Delay = 150ps * N - 250ps
 * On = 2000ps, off = 50ps
 */
#define YT8511_DELAY_GE_TX_EN	(0xf << 4)
#define YT8511_DELAY_GE_TX_DIS	(0x2 << 4)
#define YT8511_DELAY_FE_TX_EN	(0xf << 12)
#define YT8511_DELAY_FE_TX_DIS	(0x2 << 12)

/* LED blink when link up, rx is bit 9 ,tx is bit 10 .
 * if BLINK status is not activated, when PHY link up:
 * speed mode is 10M ,LED on is bit 4
 * speed mode is 100M ,LED on is bit 5
 * speed mode is 1000M ,LED on is bit 6
 *
 * default value: LED0=0x610 LED1=0x620 LED2=0x640
 */
#define YT8521S_EXTREG_LED0	0xA00C
#define YT8521S_EXTREG_LED1	0xA00D
#define YT8521S_EXTREG_LED2	0xA00E

#define YT8521S_EXT_REG_TX_BLINK	(1 << 10)
#define YT8521S_EXT_REG_RX_BLINK	(1 << 9)
#define YT8521S_EXT_REG_100M		(1 << 5)
#define YT8521S_EXT_REG_1000M		(1 << 6)

static u32 ytphy_read_ext(struct phy_device *phydev, u32 regnum)
{
	int ret;

	phy_lock_mdio_bus(phydev);
	ret = __phy_write(phydev, EXT_REG_ADDR_OFFSET, regnum);
	if (ret < 0)
		goto err_handle;
	ret = __phy_read(phydev, EXT_REG_DATA);
	if (ret < 0)
		goto err_handle;

err_handle:
	phy_unlock_mdio_bus(phydev);
	return ret;
}

static int ytphy_write_ext(struct phy_device *phydev, u32 regnum, u16 val)
{
	int ret;

	phy_lock_mdio_bus(phydev);
	ret = __phy_write(phydev, EXT_REG_ADDR_OFFSET, regnum);
	if (ret < 0)
		goto err_handle;
	ret = __phy_write(phydev, EXT_REG_DATA, val);
	if (ret < 0)
		goto err_handle;

err_handle:
	phy_unlock_mdio_bus(phydev);
	return ret;
}

static int init_LAN_LED_standard_customization(struct phy_device *phydev)
{
	int ret = 0,val;

	val = ytphy_read_ext(phydev,YT8521S_EXTREG_LED0);
	val = (val | YT8521S_EXT_REG_100M | YT8521S_EXT_REG_1000M);
	ret = ytphy_write_ext(phydev, YT8521S_EXTREG_LED0, val);
	if (ret < 0)
		return ret;

	val = ytphy_read_ext(phydev,YT8521S_EXTREG_LED1);
	val = (val & ~YT8521S_EXT_REG_RX_BLINK & ~YT8521S_EXT_REG_TX_BLINK);
	ret = ytphy_write_ext(phydev, YT8521S_EXTREG_LED1, val);
	if (ret < 0)
		return ret;

	val = ytphy_read_ext(phydev,YT8521S_EXTREG_LED2);
	val = (val & ~YT8521S_EXT_REG_RX_BLINK & ~YT8521S_EXT_REG_TX_BLINK);
	ret = ytphy_write_ext(phydev, YT8521S_EXTREG_LED2, val);
	if (ret < 0)
		return ret;

	return ret;
}

static int yt8511_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, YT8511_PAGE_SELECT);
};

static int yt8511_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, YT8511_PAGE_SELECT, page);
};

static int yt8511_config_init(struct phy_device *phydev)
{
	int oldpage, ret = 0;
	unsigned int ge, fe;

	oldpage = phy_select_page(phydev, YT8511_EXT_CLK_GATE);
	if (oldpage < 0)
		goto err_restore_page;

	/* set rgmii delay mode */
	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		ge = YT8511_DELAY_GE_TX_DIS;
		fe = YT8511_DELAY_FE_TX_DIS;
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		ge = YT8511_DELAY_RX | YT8511_DELAY_GE_TX_DIS;
		fe = YT8511_DELAY_FE_TX_DIS;
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		ge = YT8511_DELAY_GE_TX_EN;
		fe = YT8511_DELAY_FE_TX_EN;
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
		ge = YT8511_DELAY_RX | YT8511_DELAY_GE_TX_EN;
		fe = YT8511_DELAY_FE_TX_EN;
		break;
	default: /* do not support other modes */
		ret = -EOPNOTSUPP;
		goto err_restore_page;
	}

	ret = __phy_modify(phydev, YT8511_PAGE, (YT8511_DELAY_RX | YT8511_DELAY_GE_TX_EN), ge);
	if (ret < 0)
		goto err_restore_page;

	/* set clock mode to 125mhz */
	ret = __phy_modify(phydev, YT8511_PAGE, 0, YT8511_CLK_125M);
	if (ret < 0)
		goto err_restore_page;

	/* fast ethernet delay is in a separate page */
	ret = __phy_write(phydev, YT8511_PAGE_SELECT, YT8511_EXT_DELAY_DRIVE);
	if (ret < 0)
		goto err_restore_page;

	ret = __phy_modify(phydev, YT8511_PAGE, YT8511_DELAY_FE_TX_EN, fe);
	if (ret < 0)
		goto err_restore_page;

	/* leave pll enabled in sleep */
	ret = __phy_write(phydev, YT8511_PAGE_SELECT, YT8511_EXT_SLEEP_CTRL);
	if (ret < 0)
		goto err_restore_page;

	ret = __phy_modify(phydev, YT8511_PAGE, 0, YT8511_PLLON_SLP);
	if (ret < 0)
		goto err_restore_page;

err_restore_page:
	return phy_restore_page(phydev, oldpage, ret);
}

static int yt8521_config_init(struct phy_device *phydev)
{
	int ret =0;
	ret = init_LAN_LED_standard_customization(phydev);
	if (ret < 0)
		return ret;

	return yt8511_config_init(phydev);
}

static struct phy_driver motorcomm_phy_drvs[] = {
	{
		PHY_ID_MATCH_EXACT(PHY_ID_YT8511),
		.name		= "YT8511 Gigabit Ethernet",
		.config_init	= yt8511_config_init,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= yt8511_read_page,
		.write_page	= yt8511_write_page,
	},
	{
		PHY_ID_MATCH_EXACT(PHY_ID_YT8521),
		.name		= "YT8521 Gigabit Ethernet",
		.config_init	= yt8521_config_init,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= yt8511_read_page,
		.write_page	= yt8511_write_page,
	},
};

module_phy_driver(motorcomm_phy_drvs);

MODULE_DESCRIPTION("Motorcomm PHY driver");
MODULE_AUTHOR("Peter Geis");
MODULE_LICENSE("GPL");

static const struct mdio_device_id __maybe_unused motorcomm_tbl[] = {
	{ PHY_ID_MATCH_EXACT(PHY_ID_YT8511) },
	{ /* sentinal */ }
};

MODULE_DEVICE_TABLE(mdio, motorcomm_tbl);
