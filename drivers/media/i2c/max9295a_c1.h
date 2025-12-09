#ifndef __MAX9295A_C1_H__
#define __MAX9295A_C1_H__

/* Register 0x04 */
#define MAX9295A_SEREN			BIT(7)
#define MAX9295A_CLINKEN		BIT(6)
#define MAX9295A_PRBSEN			BIT(5)
#define MAX9295A_SLEEP			BIT(4)
#define MAX9295A_INTTYPE_I2C		(0 << 2)
#define MAX9295A_INTTYPE_UART		(1 << 2)
#define MAX9295A_INTTYPE_NONE		(2 << 2)
#define MAX9295A_REVCCEN		BIT(1)
#define MAX9295A_FWDCCEN		BIT(0)
/* Register 0x07 */
#define MAX9295A_DBL			BIT(7)
#define MAX9295A_DRS			BIT(6)
#define MAX9295A_BWS			BIT(5)
#define MAX9295A_ES			BIT(4)
#define MAX9295A_HVEN			BIT(2)
#define MAX9295A_EDC_1BIT_PARITY	(0 << 0)
#define MAX9295A_EDC_6BIT_CRC		(1 << 0)
#define MAX9295A_EDC_6BIT_HAMMING	(2 << 0)
/* Register 0x08 */
#define MAX9295A_INVVS			BIT(7)
#define MAX9295A_INVHS			BIT(6)
#define MAX9295A_REV_LOGAIN		BIT(3)
#define MAX9295A_REV_HIVTH		BIT(0)
/* Register 0x09 */
#define MAX9295A_ID_REG			0x09
/* Register 0x0d */
#define MAX9295A_I2CLOCACK		BIT(7)
#define MAX9295A_I2CSLVSH_1046NS_469NS	(3 << 5)
#define MAX9295A_I2CSLVSH_938NS_352NS	(2 << 5)
#define MAX9295A_I2CSLVSH_469NS_234NS	(1 << 5)
#define MAX9295A_I2CSLVSH_352NS_117NS	(0 << 5)
#define MAX9295A_I2CMSTBT_837KBPS	(7 << 2)
#define MAX9295A_I2CMSTBT_533KBPS	(6 << 2)
#define MAX9295A_I2CMSTBT_339KBPS	(5 << 2)
#define MAX9295A_I2CMSTBT_173KBPS	(4 << 2)
#define MAX9295A_I2CMSTBT_105KBPS	(3 << 2)
#define MAX9295A_I2CMSTBT_84KBPS	(2 << 2)
#define MAX9295A_I2CMSTBT_28KBPS	(1 << 2)
#define MAX9295A_I2CMSTBT_8KBPS		(0 << 2)
#define MAX9295A_I2CSLVTO_NONE		(3 << 0)
#define MAX9295A_I2CSLVTO_1024US	(2 << 0)
#define MAX9295A_I2CSLVTO_256US		(1 << 0)
#define MAX9295A_I2CSLVTO_64US		(0 << 0)
/* Register 0x0f */
#define MAX9295A_GPIO5OUT		BIT(5)
#define MAX9295A_GPIO4OUT		BIT(4)
#define MAX9295A_GPIO3OUT		BIT(3)
#define MAX9295A_GPIO2OUT		BIT(2)
#define MAX9295A_GPIO1OUT		BIT(1)
#define MAX9295A_SETGPO			BIT(0)
/* Register 0x15 */
#define MAX9295A_PCLKDET		BIT(0)

#define MAX9295_REG2			0x02
#define MAX9295_REG7			0x07
#define MAX9295_CTRL0			0x10
#define MAX9295_I2C2			0x42
#define MAX9295_I2C3			0x43
#define MAX9295_I2C4			0x44
#define MAX9295_I2C5			0x45
#define MAX9295_I2C6			0x46

#define MAX9295_CROSS(n)		(0x1b0 + n)

#define MAX9295_GPIO_A(n)		(0x2be + (3 * n))
#define MAX9295_GPIO_B(n)		(0x2bf + (3 * n))
#define MAX9295_GPIO_C(n)		(0x2c0 + (3 * n))

#define MAX9295_VIDEO_TX_BASE(n)	(0x100 + (0x8 * n))
#define MAX9295_VIDEO_TX0(n)		(MAX9295_VIDEO_TX_BASE(n) + 0)
#define MAX9295_VIDEO_TX1(n)		(MAX9295_VIDEO_TX_BASE(n) + 1)

#define MAX9295_FRONTTOP_0		0x308
#define MAX9295_FRONTTOP_9		0x311
#define MAX9295_FRONTTOP_12		0x314
#define MAX9295_FRONTTOP_13		0x315

#define MAX9295_MIPI_RX0		0x330
#define MAX9295_MIPI_RX1		0x331
#define MAX9295_MIPI_RX2		0x332
#define MAX9295_MIPI_RX3		0x333

struct max9295a_reg {
	u16	reg;
	u8	val;
};

static const struct max9295a_reg configuretable_isx021[] = {
	{0x0002, 0x03}, /* VideoTX Disable and write 3 to reserved bits *///[RTX] ok
	{0x0100, 0x60}, /* VIDEO_TX) - Line CRC enabled.  Encoding ON. Read back 62. *///[RTX] ?

	{0x0332, 0xE0}, /* PHY lane mapping *///[RTX] 1x4
	{0x0333, 0x04}, /* PHY lane mapping *///[RTX] 1x4
	{0x0314, 0xDE}, /* Select designated datatype to route to Video Pipeline X *///[RTX] 1x4 (0x1E | 0xC)
	{0x0316, 0x22}, /* Select designated datatype to route to Video Pipeline Y *///[RTX] ok
	{0x0318, 0x22}, /* Select designated datatype to route to Video Pipeline Z *///[RTX] ok
	{0x031A, 0x22}, /* Select designated datatype to route to Video Pipeline U *///[RTX] ok

	{0x0308, 0x61},	//[RTX] Enable Line ; Enable PortB ; PortB for X
	{0x0002, 0x13}, /* Video transmit Pipe X enable *///[RTX] ok

	{0x0003, 0x03},//{0x0003, 0x03}, /* Set RCLKOUT soruce to the reference PLL clock*/ [RTX] But Enable RCLK output from alternative MFP pin disable
	{0x03F0, 0x5A}, /* PLL setting & Reset PLL */
	
	{0x02BE, 0x90},	//GIOP0:1M / GPIO High
	{0x02BF, 0x60}, //GIOP0:Pull up / Push-pull
	
	{0x03F1, 0x89}, //PCLKOUT pin enable ; PCLK enable ; GPIO4 pin PCLK output //[RTX] ok
	{0x03F0, 0x59}, /* PLL setting & Enable PLL */
	
	
	{0x0044, 0x1b << 1}, //[RTX] Src:1b
	{0x0045, 0x1a << 1}, //[RTX] Dst:1a
	
	{0x02D6, 0x00},	//GPIO 8 ; Low
	{0x02CD, 0x04},//{0x02CD, 0x04},	//GPIO 5 This GPIO source enabled for GMSL2 reception
	{0x02CA, 0x10},	//GPIO 4 ; HIGH
	{0x02D6, 0x10}, //GPIO 8 ; HIGH

};


#endif
