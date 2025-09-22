// SPDX-License-Identifier: GPL-2.0+
/* drivers/net/phy/realtek.c
 *
 * Driver for Realtek PHYs
 *
 * Author: Johnson Leung <r58129@freescale.com>
 *
 * Copyright (c) 2004 Freescale Semiconductor, Inc.
 */
#include <linux/bitops.h>
#include <linux/phy.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of.h>

#define RTL821x_PHYSR				0x11
#define RTL821x_PHYSR_DUPLEX			BIT(13)
#define RTL821x_PHYSR_SPEED			GENMASK(15, 14)

#define RTL821x_INER				0x12
#define RTL8211B_INER_INIT			0x6400
#define RTL8211E_INER_LINK_STATUS		BIT(10)
#define RTL8211F_INER_LINK_STATUS		BIT(4)

#define RTL821x_INSR				0x13

#define RTL821x_EXT_PAGE_SELECT			0x1e
#define RTL821x_PAGE_SELECT			0x1f

#define RTL8211F_PHYCR1				0x18
#define RTL8211F_INSR				0x1d

#define RTL8211F_TX_DELAY			BIT(8)
#define RTL8211F_RX_DELAY			BIT(3)

#define RTL8211F_ALDPS_PLL_OFF			BIT(1)
#define RTL8211F_ALDPS_ENABLE			BIT(2)
#define RTL8211F_ALDPS_XTAL_OFF			BIT(12)

#define RTL8211E_CTRL_DELAY			BIT(13)
#define RTL8211E_TX_DELAY			BIT(12)
#define RTL8211E_RX_DELAY			BIT(11)

#define RTL8201F_ISR				0x1e
#define RTL8201F_IER				0x13

#define RTL8366RB_POWER_SAVE			0x15
#define RTL8366RB_POWER_SAVE_ON			BIT(12)

#define RTL_SUPPORTS_5000FULL			BIT(14)
#define RTL_SUPPORTS_2500FULL			BIT(13)
#define RTL_SUPPORTS_10000FULL			BIT(0)
#define RTL_ADV_2500FULL			BIT(7)
#define RTL_LPADV_10000FULL			BIT(11)
#define RTL_LPADV_5000FULL			BIT(6)
#define RTL_LPADV_2500FULL			BIT(5)

#define RTLGEN_SPEED_MASK			0x0630

#define RTL_GENERIC_PHYID			0x001cc800

#define RTL9000A_GINMR				0x14
#define RTL9000A_GINMR_LINK_STATUS		BIT(4)

#define RTL9010Bx_PHYSRAD			0x1B
#define RTL9010Bx_PHYSRD			0x1C
#define RTL9010Bx_GINSR				0x1D
#define RTL9010Bx_PAGSR				0x1F

typedef enum {
	E_NOERR	= 0x00,
	E_INVALD	= 0xFC,	/* Invalid argument or value */
	E_NOTRDY	= 0xFD,	/* Not Ready to Access */
	E_TIMOUT	= 0xFE,	/* Time out */
	E_FAILED	= 0xFF	/* Unexpected Result */
} Error_Code;

u16 param_check[21] = {0x0D13, 21, 0x0047, 0, 0x859C, 0x0609, 0, 0x85A2, 0x0300, 0, 0x85A4, 0x0D07,
			0, 0xDC00, 0x1899, 0x0A4C, 20, 0x2A2E, 0x0A49, 20, 0xFB00};

static int rtl821x_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, RTL821x_PAGE_SELECT);
}

static int rtl821x_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, RTL821x_PAGE_SELECT, page);
}

static int rtl8201_ack_interrupt(struct phy_device *phydev)
{
	int err;

	err = phy_read(phydev, RTL8201F_ISR);

	return (err < 0) ? err : 0;
}

static int rtl821x_ack_interrupt(struct phy_device *phydev)
{
	int err;

	err = phy_read(phydev, RTL821x_INSR);

	return (err < 0) ? err : 0;
}

static int rtl8211f_ack_interrupt(struct phy_device *phydev)
{
	int err;

	err = phy_read_paged(phydev, 0xa43, RTL8211F_INSR);

	return (err < 0) ? err : 0;
}

static int rtl8201_config_intr(struct phy_device *phydev)
{
	u16 val;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		val = BIT(13) | BIT(12) | BIT(11);
	else
		val = 0;

	return phy_write_paged(phydev, 0x7, RTL8201F_IER, val);
}

static int rtl8211b_config_intr(struct phy_device *phydev)
{
	int err;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		err = phy_write(phydev, RTL821x_INER,
				RTL8211B_INER_INIT);
	else
		err = phy_write(phydev, RTL821x_INER, 0);

	return err;
}

static int rtl8211e_config_intr(struct phy_device *phydev)
{
	int err;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		err = phy_write(phydev, RTL821x_INER,
				RTL8211E_INER_LINK_STATUS);
	else
		err = phy_write(phydev, RTL821x_INER, 0);

	return err;
}

static int rtl8211f_config_intr(struct phy_device *phydev)
{
	u16 val;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		val = RTL8211F_INER_LINK_STATUS;
	else
		val = 0;

	return phy_write_paged(phydev, 0xa42, RTL821x_INER, val);
}

static int rtl8211_config_aneg(struct phy_device *phydev)
{
	int ret;

	ret = genphy_config_aneg(phydev);
	if (ret < 0)
		return ret;

	/* Quirk was copied from vendor driver. Unfortunately it includes no
	 * description of the magic numbers.
	 */
	if (phydev->speed == SPEED_100 && phydev->autoneg == AUTONEG_DISABLE) {
		phy_write(phydev, 0x17, 0x2138);
		phy_write(phydev, 0x0e, 0x0260);
	} else {
		phy_write(phydev, 0x17, 0x2108);
		phy_write(phydev, 0x0e, 0x0000);
	}

	return 0;
}

static int rtl8211c_config_init(struct phy_device *phydev)
{
	/* RTL8211C has an issue when operating in Gigabit slave mode */
	return phy_set_bits(phydev, MII_CTRL1000,
			    CTL1000_ENABLE_MASTER | CTL1000_AS_MASTER);
}

static int rtl8211f_config_init(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	u16 val_txdly, val_rxdly;
	u16 val;
	int ret;

	val = RTL8211F_ALDPS_ENABLE | RTL8211F_ALDPS_PLL_OFF | RTL8211F_ALDPS_XTAL_OFF;
	phy_modify_paged_changed(phydev, 0xa43, RTL8211F_PHYCR1, val, val);

	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		val_txdly = 0;
		val_rxdly = 0;
		break;

	case PHY_INTERFACE_MODE_RGMII_RXID:
		val_txdly = 0;
		val_rxdly = RTL8211F_RX_DELAY;
		break;

	case PHY_INTERFACE_MODE_RGMII_TXID:
		val_txdly = RTL8211F_TX_DELAY;
		val_rxdly = 0;
		break;

	case PHY_INTERFACE_MODE_RGMII_ID:
		val_txdly = RTL8211F_TX_DELAY;
		val_rxdly = RTL8211F_RX_DELAY;
		break;

	default: /* the rest of the modes imply leaving delay as is. */
		return 0;
	}

	ret = phy_modify_paged_changed(phydev, 0xd08, 0x11, RTL8211F_TX_DELAY,
				       val_txdly);
	if (ret < 0) {
		dev_err(dev, "Failed to update the TX delay register\n");
		return ret;
	} else if (ret) {
		dev_dbg(dev,
			"%s 2ns TX delay (and changing the value from pin-strapping RXD1 or the bootloader)\n",
			val_txdly ? "Enabling" : "Disabling");
	} else {
		dev_dbg(dev,
			"2ns TX delay was already %s (by pin-strapping RXD1 or bootloader configuration)\n",
			val_txdly ? "enabled" : "disabled");
	}

	ret = phy_modify_paged_changed(phydev, 0xd08, 0x15, RTL8211F_RX_DELAY,
				       val_rxdly);
	if (ret < 0) {
		dev_err(dev, "Failed to update the RX delay register\n");
		return ret;
	} else if (ret) {
		dev_dbg(dev,
			"%s 2ns RX delay (and changing the value from pin-strapping RXD0 or the bootloader)\n",
			val_rxdly ? "Enabling" : "Disabling");
	} else {
		dev_dbg(dev,
			"2ns RX delay was already %s (by pin-strapping RXD0 or bootloader configuration)\n",
			val_rxdly ? "enabled" : "disabled");
	}

	return 0;
}

static int rtl821x_resume(struct phy_device *phydev)
{
	int ret;

	ret = genphy_resume(phydev);
	if (ret < 0)
		return ret;

	msleep(20);

	return 0;
}

static int rtl8211e_config_init(struct phy_device *phydev)
{
	int ret = 0, oldpage;
	u16 val;

	/* enable TX/RX delay for rgmii-* modes, and disable them for rgmii. */
	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		val = RTL8211E_CTRL_DELAY | 0;
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
		val = RTL8211E_CTRL_DELAY | RTL8211E_TX_DELAY | RTL8211E_RX_DELAY;
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		val = RTL8211E_CTRL_DELAY | RTL8211E_RX_DELAY;
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		val = RTL8211E_CTRL_DELAY | RTL8211E_TX_DELAY;
		break;
	default: /* the rest of the modes imply leaving delays as is. */
		return 0;
	}

	/* According to a sample driver there is a 0x1c config register on the
	 * 0xa4 extension page (0x7) layout. It can be used to disable/enable
	 * the RX/TX delays otherwise controlled by RXDLY/TXDLY pins.
	 * The configuration register definition:
	 * 14 = reserved
	 * 13 = Force Tx RX Delay controlled by bit12 bit11,
	 * 12 = RX Delay, 11 = TX Delay
	 * 10:0 = Test && debug settings reserved by realtek
	 */
	oldpage = phy_select_page(phydev, 0x7);
	if (oldpage < 0)
		goto err_restore_page;

	ret = __phy_write(phydev, RTL821x_EXT_PAGE_SELECT, 0xa4);
	if (ret)
		goto err_restore_page;

	ret = __phy_modify(phydev, 0x1c, RTL8211E_CTRL_DELAY
			   | RTL8211E_TX_DELAY | RTL8211E_RX_DELAY,
			   val);

err_restore_page:
	return phy_restore_page(phydev, oldpage, ret);
}

static int rtl8211b_suspend(struct phy_device *phydev)
{
	phy_write(phydev, MII_MMD_DATA, BIT(9));

	return genphy_suspend(phydev);
}

static int rtl8211b_resume(struct phy_device *phydev)
{
	phy_write(phydev, MII_MMD_DATA, 0);

	return genphy_resume(phydev);
}

static int rtl8366rb_config_init(struct phy_device *phydev)
{
	int ret;

	ret = phy_set_bits(phydev, RTL8366RB_POWER_SAVE,
			   RTL8366RB_POWER_SAVE_ON);
	if (ret) {
		dev_err(&phydev->mdio.dev,
			"error enabling power management\n");
	}

	return ret;
}

static int rtl9010_get_features(struct phy_device *phydev)
{

	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT1_Full_BIT, phydev->supported);
	linkmode_copy(phydev->advertising, phydev->supported);
	return 0;
}

static int rtl9010_Initial_Configuration_Check(struct phy_device *phydev)
{
	u16 mdio_data = 0;
	u16 mdio_data_chk = 0;

	u16 page;
	u16 reg, i;
	u32 timer = 2000; // set a 2ms timer

	for (i = 0; i < 21 ; i = i + 3) {
		page = param_check[i];
		mdio_data_chk = param_check[i + 2];
		reg = param_check[i + 1];
		if (page == 0) {
			phy_write(phydev, 27, reg);
			mdio_data = phy_read(phydev, 28);
		} else {
			phy_write(phydev, 31, page);
			mdio_data = phy_read(phydev, reg);
		}

		if (mdio_data_chk != mdio_data) {
			printk("%dth param error page=0x%04X reg=0x%04X data=0x%04X\r\n", i / 3, page, reg, mdio_data);
//			return E_FAILED;
		}
	}

	return E_NOERR;
}

static int rtl9010_Initial_Configuration(struct phy_device *phydev)
{
	u32 mdio_data = 0;
	u32 timer = 2000; // set a 2ms timer

	//PHY parameter start//
	phy_write(phydev, RTL9010Bx_PAGSR, 0x0D13);
	phy_write(phydev, 21, 0x0047); 	//added
	phy_write(phydev, 27, 0x859C);
	phy_write(phydev, 28, 0x0609);
	phy_write(phydev, 27, 0x85A2);
	phy_write(phydev, 28, 0x0300);
	phy_write(phydev, 27, 0x85A4);
	phy_write(phydev, 28, 0x0D07); 	//modified
	phy_write(phydev, 27, 0xDC00);
	phy_write(phydev, 28, 0x1899);
	phy_write(phydev, 31, 0x0A4C);
	phy_write(phydev, 20, 0x2A2E);
	phy_write(phydev, 31, 0x0A49);
	phy_write(phydev, 20, 0xFB00);

	phy_write(phydev, MII_BMCR, BMCR_RESET);	// PHY soft-reset
	msleep(20);
	do {	// Check soft-reset complete

		mdio_data = phy_read(phydev, 0);
		if (mdio_data == 0xFFFF)
			return E_NOTRDY;

		timer--;
		if (timer == 0) {
			return E_TIMOUT;
		}
	} while(mdio_data != 0x0140);
	return rtl9010_Initial_Configuration_Check(phydev);
}

static int rtl9010_wait_reset(struct phy_device *phydev)
{
	int i, bmcr;

	/* Poll BMCR.RESET cleared */
	for (i = 0; i < 10; i++) {
		bmcr = phy_read(phydev, MII_BMCR);
		if (bmcr < 0)
			return bmcr;
		if (!(bmcr & BMCR_RESET))
			return 0;
		udelay(100);
	}
	return -ETIMEDOUT;
}

static int rtl9010_set_rgmii_delay_1000M(struct phy_device *phydev)
{
	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII_ID:   /* TXDLY + RXDLY */
		phy_write(phydev, 27, 0xD04A);	// RX
		phy_write(phydev, 28, 0x000F);
		phy_write(phydev, 27, 0xD082);	// TX
		phy_write(phydev, 28, 0x0003);
		phy_write(phydev, 27, 0xD084);	// TX
		phy_write(phydev, 28, 0x4007);
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID: /* TXDLY only */
		phy_write(phydev, 27, 0xD082);	// TX
		phy_write(phydev, 28, 0x0003);
		phy_write(phydev, 27, 0xD084);	// TX
		phy_write(phydev, 28, 0x4007);
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID: /* RXDLY only */
		phy_write(phydev, 27, 0xD04A);	// RX
		phy_write(phydev, 28, 0x000F);
		break;
	case PHY_INTERFACE_MODE_RGMII:
		break;
	default:
		break;
	}
	return 0;
}

static int rtl9010_set_rgmii_delay_100M(struct phy_device *phydev)
{
	switch (phydev->interface) {
    case PHY_INTERFACE_MODE_RGMII_ID:   /* TXDLY + RXDLY */
		printk("AVB2: Set PHY delay (TXDLY + RXDLY).\n", __FUNCTION__);
		phy_write(phydev, 27, 0xD04A);    // RX
		phy_write(phydev, 28, 0x000F);
		phy_write(phydev, 27, 0xD082);    // TX
		phy_write(phydev, 28, 0x4080);
		phy_write(phydev, 27, 0xD084);    // TX
		phy_write(phydev, 28, 0x0000);
		break;
    case PHY_INTERFACE_MODE_RGMII_TXID: /* TXDLY only */
		printk("AVB2: Set PHY delay (TXDLY).\n", __FUNCTION__);
		phy_write(phydev, 27, 0xD082);    // TX
		phy_write(phydev, 28, 0x4080);
		phy_write(phydev, 27, 0xD084);    // TX
		phy_write(phydev, 28, 0x0000);
        break;
    case PHY_INTERFACE_MODE_RGMII_RXID: /* RXDLY only */
		printk("AVB2: Set PHY delay (RXID).\n", __FUNCTION__);
		phy_write(phydev, 27, 0xD04A);    // RX
		phy_write(phydev, 28, 0x000F);
        break;
    case PHY_INTERFACE_MODE_RGMII:
        break;
    default:
        break;
    }
	return 0;
}

static int rtl9010_txrx_speed(struct phy_device *phydev)
{
	// Force Speed/Duplex
	int ctl;

	ctl = phy_read(phydev, MII_BMCR);
	if (ctl < 0)
		return ctl;

	ctl &= ~(BMCR_ANENABLE | BMCR_SPEED1000 | BMCR_SPEED100 | BMCR_FULLDPLX);

	switch (phydev->speed) {
	case SPEED_1000:
		ctl |= BMCR_SPEED1000;
		break;
	case SPEED_100:
		ctl |= BMCR_SPEED100;
		break;
	case SPEED_10:
		break;
	}

	if (phydev->duplex == DUPLEX_FULL)
		ctl |= BMCR_FULLDPLX;

	phy_write(phydev, MII_BMCR, ctl);
	msleep(20);
	return 0;
}

static int rtl9010_parse_dt(struct phy_device *phydev)
{
	u32 val;
	struct device_node *np = phydev->mdio.dev.of_node;

	/* set speed */
    if (rtl9010_txrx_speed(phydev) != 0) {
		printk("[ERROR] rtl9010_txrx_speed failed.\n");
		return -1;
	} 
		
    of_property_read_u32(np, "set-phy-delay", &val);
	if (val == 0) {
		printk("AVB2: Set MAC RX/TX delay.\n");
		return 0;
	}

	/* set RGMII TX/RX Delay（phy-mode）*/
    if (phydev->speed == SPEED_1000) {
		phydev->interface = PHY_INTERFACE_MODE_RGMII_ID;
		rtl9010_set_rgmii_delay_1000M(phydev);
	} else if (phydev->speed == SPEED_100) {
		phydev->interface = PHY_INTERFACE_MODE_RGMII_ID;
		rtl9010_set_rgmii_delay_100M(phydev);
	}
	return 0;
}

static int rtl9010_config_init(struct phy_device *phydev)
{
	int ret;

	if (phy_write(phydev, MII_BMCR, BMCR_RESET) != 0) {
		printk("[ERROR] BMCR_RESET failed.\n");
		return -1;
	}
	msleep(20);

	phydev->autoneg = AUTONEG_DISABLE;
	phydev->speed   = SPEED_1000;
	phydev->duplex  = DUPLEX_FULL;
	phydev->link	= 1;

	if (rtl9010_Initial_Configuration(phydev) != 0) {
		printk("[ERROR] rtl9010_Initial_Configuration failed.\n");
		return -1;
	}

	/* Include TX / RX delay and speed */
    if (rtl9010_parse_dt(phydev) != 0) {
	    printk("[ERROR] rtl9010_parse_dt failed.\n");
	    return -1;
	} 

	// phy_write(phydev, MII_BMCR, 0x8000);

	/* default page 0x0A42 */
	phy_write(phydev, RTL9010Bx_PAGSR, 0x0A42);

	return 0;
}

static int rtl9010_config_aneg(struct phy_device *phydev)
{
	int ret;
	u16 ctl = 0;

	phydev->autoneg = AUTONEG_DISABLE;

	/* 設定 Auto-Negotiation，直接用 kernel 提供的 */
	if (phydev->autoneg == AUTONEG_ENABLE) {
		return genphy_config_aneg(phydev);
	}

	if (rtl9010_txrx_speed(phydev) != 0) {
		printk("[ERROR] rtl9010_txrx_speed failed.\n");
		return -1;
	} 

    if (phydev->speed == SPEED_1000) {
		phydev->interface = PHY_INTERFACE_MODE_RGMII_ID;
		rtl9010_set_rgmii_delay_1000M(phydev);
	} else if (phydev->speed == SPEED_100) {
		phydev->interface = PHY_INTERFACE_MODE_RGMII_ID;
		rtl9010_set_rgmii_delay_100M(phydev);
	}

	switch (phydev->master_slave_set) {
	case MASTER_SLAVE_CFG_MASTER_FORCE:
		ctl |= CTL1000_AS_MASTER;
		break;
	case MASTER_SLAVE_CFG_SLAVE_FORCE:
		break;
	case MASTER_SLAVE_CFG_UNKNOWN:
	case MASTER_SLAVE_CFG_UNSUPPORTED:
		return 0;
	default:
		phydev_warn(phydev, "Unsupported Master/Slave mode\n");
		return -EOPNOTSUPP;
	}

	ret = phy_modify_changed(phydev, MII_CTRL1000, CTL1000_AS_MASTER, ctl);
	if (ret == 1)
		ret = genphy_restart_aneg(phydev);

	pr_info("RTL9010: Auto-negotiation configured\n");
	return 0;
}

static int rtl9010_read_status(struct phy_device *phydev)
{
	int ret;

	phydev->master_slave_get = MASTER_SLAVE_CFG_UNKNOWN;
	phydev->master_slave_state = MASTER_SLAVE_STATE_UNKNOWN;

	ret = genphy_update_link(phydev);
	if (ret)
		return ret;

	ret = phy_read(phydev, MII_CTRL1000);
	if (ret < 0)
		return ret;
	if (ret & CTL1000_AS_MASTER)
		phydev->master_slave_get = MASTER_SLAVE_CFG_MASTER_FORCE;
	else
		phydev->master_slave_get = MASTER_SLAVE_CFG_SLAVE_FORCE;

	ret = phy_read(phydev, MII_STAT1000);
	if (ret < 0)
		return ret;
	if (ret & LPA_1000MSRES)
		phydev->master_slave_state = MASTER_SLAVE_STATE_MASTER;
	else
		phydev->master_slave_state = MASTER_SLAVE_STATE_SLAVE;

	ret = genphy_read_status(phydev);
	if (ret)
		return ret;

	//pr_info("RTL9010: Link %s, %d Mbps, %s-duplex\n",
		//phydev->link ? "up" : "down",
		//phydev->speed,
		//phydev->duplex == DUPLEX_FULL ? "full" : "half");

	return 0;
}

static int rtl9010_ack_interrupt(struct phy_device *phydev)
{
	int err;

	err = phy_read(phydev, RTL8211F_INSR);

	return (err < 0) ? err : 0;
}

static int rtl9010_config_intr(struct phy_device *phydev)
{
	u16 val;
	int err;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		err = rtl9010_ack_interrupt(phydev);
		if (err)
			return err;

		val = (u16)~RTL9000A_GINMR_LINK_STATUS;
		err = phy_write_paged(phydev, 0xa42, RTL9000A_GINMR, val);
	} else {
		val = ~0;
		err = phy_write_paged(phydev, 0xa42, RTL9000A_GINMR, val);
		if (err)
			return err;

		err = rtl9010_ack_interrupt(phydev);
	}

	return phy_write_paged(phydev, 0xa42, RTL9000A_GINMR, val);
}

static irqreturn_t rtl9010_handle_interrupt(struct phy_device *phydev)
{
	int irq_status;

	irq_status = phy_read(phydev, RTL8211F_INSR);
	if (irq_status < 0) {
//		phy_error(phydev);
		return IRQ_NONE;
	}

	if (!(irq_status & RTL8211F_INER_LINK_STATUS))
		return IRQ_NONE;

//	phy_trigger_machine(phydev);

	return IRQ_HANDLED;
}

/* get actual speed to cover the downshift case */
static int rtlgen_get_speed(struct phy_device *phydev)
{
	int val;

	if (!phydev->link)
		return 0;

	val = phy_read_paged(phydev, 0xa43, 0x12);
	if (val < 0)
		return val;

	switch (val & RTLGEN_SPEED_MASK) {
	case 0x0000:
		phydev->speed = SPEED_10;
		break;
	case 0x0010:
		phydev->speed = SPEED_100;
		break;
	case 0x0020:
		phydev->speed = SPEED_1000;
		break;
	case 0x0200:
		phydev->speed = SPEED_10000;
		break;
	case 0x0210:
		phydev->speed = SPEED_2500;
		break;
	case 0x0220:
		phydev->speed = SPEED_5000;
		break;
	default:
		break;
	}

	return 0;
}

static int rtlgen_read_status(struct phy_device *phydev)
{
	int ret;

	ret = genphy_read_status(phydev);
	if (ret < 0)
		return ret;

	return rtlgen_get_speed(phydev);
}

static int rtlgen_read_mmd(struct phy_device *phydev, int devnum, u16 regnum)
{
	int ret;

	if (devnum == MDIO_MMD_PCS && regnum == MDIO_PCS_EEE_ABLE) {
		rtl821x_write_page(phydev, 0xa5c);
		ret = __phy_read(phydev, 0x12);
		rtl821x_write_page(phydev, 0);
	} else if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_ADV) {
		rtl821x_write_page(phydev, 0xa5d);
		ret = __phy_read(phydev, 0x10);
		rtl821x_write_page(phydev, 0);
	} else if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_LPABLE) {
		rtl821x_write_page(phydev, 0xa5d);
		ret = __phy_read(phydev, 0x11);
		rtl821x_write_page(phydev, 0);
	} else {
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static int rtlgen_write_mmd(struct phy_device *phydev, int devnum, u16 regnum,
			    u16 val)
{
	int ret;

	if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_ADV) {
		rtl821x_write_page(phydev, 0xa5d);
		ret = __phy_write(phydev, 0x10, val);
		rtl821x_write_page(phydev, 0);
	} else {
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static int rtl822x_read_mmd(struct phy_device *phydev, int devnum, u16 regnum)
{
	int ret = rtlgen_read_mmd(phydev, devnum, regnum);

	if (ret != -EOPNOTSUPP)
		return ret;

	if (devnum == MDIO_MMD_PCS && regnum == MDIO_PCS_EEE_ABLE2) {
		rtl821x_write_page(phydev, 0xa6e);
		ret = __phy_read(phydev, 0x16);
		rtl821x_write_page(phydev, 0);
	} else if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_ADV2) {
		rtl821x_write_page(phydev, 0xa6d);
		ret = __phy_read(phydev, 0x12);
		rtl821x_write_page(phydev, 0);
	} else if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_LPABLE2) {
		rtl821x_write_page(phydev, 0xa6d);
		ret = __phy_read(phydev, 0x10);
		rtl821x_write_page(phydev, 0);
	}

	return ret;
}

static int rtl822x_write_mmd(struct phy_device *phydev, int devnum, u16 regnum,
			     u16 val)
{
	int ret = rtlgen_write_mmd(phydev, devnum, regnum, val);

	if (ret != -EOPNOTSUPP)
		return ret;

	if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_ADV2) {
		rtl821x_write_page(phydev, 0xa6d);
		ret = __phy_write(phydev, 0x12, val);
		rtl821x_write_page(phydev, 0);
	}

	return ret;
}

static int rtl822x_get_features(struct phy_device *phydev)
{
	int val;

	val = phy_read_paged(phydev, 0xa61, 0x13);
	if (val < 0)
		return val;

	linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
			 phydev->supported, val & RTL_SUPPORTS_2500FULL);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_5000baseT_Full_BIT,
			 phydev->supported, val & RTL_SUPPORTS_5000FULL);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_10000baseT_Full_BIT,
			 phydev->supported, val & RTL_SUPPORTS_10000FULL);

	return genphy_read_abilities(phydev);
}

static int rtl822x_config_aneg(struct phy_device *phydev)
{
	int ret = 0;

	if (phydev->autoneg == AUTONEG_ENABLE) {
		u16 adv2500 = 0;

		if (linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
				      phydev->advertising))
			adv2500 = RTL_ADV_2500FULL;

		ret = phy_modify_paged_changed(phydev, 0xa5d, 0x12,
					       RTL_ADV_2500FULL, adv2500);
		if (ret < 0)
			return ret;
	}

	return __genphy_config_aneg(phydev, ret);
}

static int rtl822x_read_status(struct phy_device *phydev)
{
	int ret;

	if (phydev->autoneg == AUTONEG_ENABLE) {
		int lpadv = phy_read_paged(phydev, 0xa5d, 0x13);

		if (lpadv < 0)
			return lpadv;

		linkmode_mod_bit(ETHTOOL_LINK_MODE_10000baseT_Full_BIT,
			phydev->lp_advertising, lpadv & RTL_LPADV_10000FULL);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_5000baseT_Full_BIT,
			phydev->lp_advertising, lpadv & RTL_LPADV_5000FULL);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
			phydev->lp_advertising, lpadv & RTL_LPADV_2500FULL);
	}

	ret = genphy_read_status(phydev);
	if (ret < 0)
		return ret;

	return rtlgen_get_speed(phydev);
}

static bool rtlgen_supports_2_5gbps(struct phy_device *phydev)
{
	int val;

	phy_write(phydev, RTL821x_PAGE_SELECT, 0xa61);
	val = phy_read(phydev, 0x13);
	phy_write(phydev, RTL821x_PAGE_SELECT, 0);

	return val >= 0 && val & RTL_SUPPORTS_2500FULL;
}

static int rtlgen_match_phy_device(struct phy_device *phydev)
{
	return phydev->phy_id == RTL_GENERIC_PHYID &&
	       !rtlgen_supports_2_5gbps(phydev);
}

static int rtl8226_match_phy_device(struct phy_device *phydev)
{
	return phydev->phy_id == RTL_GENERIC_PHYID &&
	       rtlgen_supports_2_5gbps(phydev);
}

static int rtlgen_resume(struct phy_device *phydev)
{
	int ret = genphy_resume(phydev);

	/* Internal PHY's from RTL8168h up may not be instantly ready */
	msleep(20);

	return ret;
}

static struct phy_driver realtek_drvs[] = {
	{
		PHY_ID_MATCH_EXACT(0x00008201),
		.name           = "RTL8201CP Ethernet",
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc816),
		.name		= "RTL8201F Fast Ethernet",
		.ack_interrupt	= &rtl8201_ack_interrupt,
		.config_intr	= &rtl8201_config_intr,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		PHY_ID_MATCH_MODEL(0x001cc880),
		.name		= "RTL8208 Fast Ethernet",
		.read_mmd	= genphy_read_mmd_unsupported,
		.write_mmd	= genphy_write_mmd_unsupported,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc910),
		.name		= "RTL8211 Gigabit Ethernet",
		.config_aneg	= rtl8211_config_aneg,
		.read_mmd	= &genphy_read_mmd_unsupported,
		.write_mmd	= &genphy_write_mmd_unsupported,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc912),
		.name		= "RTL8211B Gigabit Ethernet",
		.ack_interrupt	= &rtl821x_ack_interrupt,
		.config_intr	= &rtl8211b_config_intr,
		.read_mmd	= &genphy_read_mmd_unsupported,
		.write_mmd	= &genphy_write_mmd_unsupported,
		.suspend	= rtl8211b_suspend,
		.resume		= rtl8211b_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc913),
		.name		= "RTL8211C Gigabit Ethernet",
		.config_init	= rtl8211c_config_init,
		.read_mmd	= &genphy_read_mmd_unsupported,
		.write_mmd	= &genphy_write_mmd_unsupported,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc914),
		.name		= "RTL8211DN Gigabit Ethernet",
		.ack_interrupt	= rtl821x_ack_interrupt,
		.config_intr	= rtl8211e_config_intr,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc915),
		.name		= "RTL8211E Gigabit Ethernet",
		.config_init	= &rtl8211e_config_init,
		.ack_interrupt	= &rtl821x_ack_interrupt,
		.config_intr	= &rtl8211e_config_intr,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc916),
		.name		= "RTL8211F Gigabit Ethernet",
		.config_init	= &rtl8211f_config_init,
		.ack_interrupt	= &rtl8211f_ack_interrupt,
		.config_intr	= &rtl8211f_config_intr,
		.suspend	= genphy_suspend,
		.resume		= rtl821x_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	}, {
		.name		= "Generic FE-GE Realtek PHY",
		.match_phy_device = rtlgen_match_phy_device,
		.read_status	= rtlgen_read_status,
		.suspend	= genphy_suspend,
		.resume		= rtlgen_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
		.read_mmd	= rtlgen_read_mmd,
		.write_mmd	= rtlgen_write_mmd,
	}, {
		.name		= "RTL8226 2.5Gbps PHY",
		.match_phy_device = rtl8226_match_phy_device,
		.get_features	= rtl822x_get_features,
		.config_aneg	= rtl822x_config_aneg,
		.read_status	= rtl822x_read_status,
		.suspend	= genphy_suspend,
		.resume		= rtlgen_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
		.read_mmd	= rtl822x_read_mmd,
		.write_mmd	= rtl822x_write_mmd,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc840),
		.name		= "RTL8226B_RTL8221B 2.5Gbps PHY",
		.get_features	= rtl822x_get_features,
		.config_aneg	= rtl822x_config_aneg,
		.read_status	= rtl822x_read_status,
		.suspend	= genphy_suspend,
		.resume		= rtlgen_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
		.read_mmd	= rtl822x_read_mmd,
		.write_mmd	= rtl822x_write_mmd,
	}, {
		PHY_ID_MATCH_EXACT(0x001cc961),
		.name		= "RTL8366RB Gigabit Ethernet",
		.config_init	= &rtl8366rb_config_init,
		/* These interrupts are handled by the irq controller
		 * embedded inside the RTL8366RB, they get unmasked when the
		 * irq is requested and ACKed by reading the status register,
		 * which is done by the irqchip code.
		 */
		.ack_interrupt	= genphy_no_ack_interrupt,
		.config_intr	= genphy_no_config_intr,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
	}, {
		PHY_ID_MATCH_EXACT(0x001ccb31),
		.phy_id		= 0x001ccb31,
		.name		= "RTL9010BRG Ethernet",
		.phy_id_mask	= 0x001fffff,
//		.get_features	= rtl9010_get_features,
		.features	= PHY_GBIT_FEATURES,
		.config_init	= rtl9010_config_init,
		.config_aneg	= rtl9010_config_aneg,
		.read_status	= rtl9010_read_status,
		.config_intr	= rtl9010_config_intr,
//		.handle_interrupt = rtl9010_handle_interrupt,
//		.suspend	= genphy_suspend,
//		.resume		= genphy_resume,
		.read_page	= rtl821x_read_page,
		.write_page	= rtl821x_write_page,
	},
};

module_phy_driver(realtek_drvs);

static const struct mdio_device_id __maybe_unused realtek_tbl[] = {
	{ PHY_ID_MATCH_VENDOR(0x001ccb31) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, realtek_tbl);
MODULE_DESCRIPTION("Realtek PHY driver");
MODULE_AUTHOR("Johnson Leung");
MODULE_LICENSE("GPL");
