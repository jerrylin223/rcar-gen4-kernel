#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "max96712.h"

#define MAX96712_NUM_GMSL		4
#define MAX96712_N_SINKS		4
#define MAX96712_N_PADS			5
#define MAX96712_SRC_PAD		4

#define DEBUG_REG_DUMP			0
#define DEBUG_COLOR_PATTERN		1
#define DEBUG_MBPS			200000000
#define DEFAULT_MBPS			520000000
#define FAST_STARTUP			0

/* 0x01: internal type, 0x02: external type, 0x00: OFF */
static unsigned int FSYNC_INT_EXT = 0x00;
module_param_named(fsync_int_ext, FSYNC_INT_EXT, int, 0644);

struct max96712_source {
	struct v4l2_async_subdev asd;
	struct v4l2_subdev *sd;
	struct fwnode_handle *fwnode;
	bool linkup;
	
	unsigned int width;
	unsigned int height;
	unsigned int pixel_rate;
	u32 code;
};

#define asd_to_max96712_source(_asd) \
	container_of(_asd, struct max96712_source, asd)

struct max96712_link {
	struct v4l2_subdev sd;
	struct fwnode_handle *sd_fwnode;
	struct i2c_client *client;
	int ser_id;
	int ser_addr;
	int pipes_mask;			/* mask of pipes used by this link */
	int out_mipi;			/* MIPI# */
	int out_vc;			/* VC# */
	struct regulator *poc_reg;	/* PoC power supply */
};

static const struct regmap_config max96712_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xFFFF,
};

struct max96712_priv {
	struct regmap *regmap;
	struct i2c_client *client;

	struct gpio_desc *gpiod_pwdn;
	struct max96712_link *link[MAX96712_NUM_GMSL];
	struct max96712_source sources[MAX96712_NUM_GMSL];

	struct v4l2_subdev sd;
	struct v4l2_async_notifier notifier;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_mbus_framefmt fmt[MAX96712_N_SINKS];
	struct media_pad pads[MAX96712_N_PADS];

	unsigned int nsources;
	unsigned int source_mask;
	unsigned int bound_sources;
	unsigned int csi2_data_lanes;
	bool phy_pol_inv;
	int links_mask;
	int dt;
	int stream_count;
	int fsync_period;
	bool cphy_connection;
	int dev_id;
	unsigned int campw_gpio;
	struct v4l2_ctrl *pixel_rate_ctrl;
	unsigned int total_pixel_rate;
	unsigned int bpp;
	
	int test_pattern_mode;
};

static struct max96712_source *next_source(struct max96712_priv *priv,
					   struct max96712_source *source)
{
	if (!source)
		source = &priv->sources[0];
	else
		source++;

	for (; source < &priv->sources[MAX96712_NUM_GMSL]; source++) {
		if (source->fwnode)
			return source;
	}

	return NULL;
}

#define for_each_source(priv, source) \
	for ((source) = NULL; ((source) = next_source((priv), (source))); )

#define to_index(priv, source) ((source) - &(priv)->sources[0])

static inline struct max96712_priv *sd_to_max96712(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96712_priv, sd);
}

struct max96712_reg {
	u16	reg;
	u8	val;
};

const struct max96712_reg max96712_color_pattern_dinit[] = {
	{0x1050, 0xE3},        // Set VTG mode VRX_Patgen 0, Generate VS, HS, DE", Not Invert the VS
	{0x1051, 0xA0},        // Set 1 (Checkboard mode), 2 (Grad Mode)
	{0x1052, 0x00},        // Set VS_DLY = 0
	{0x1053, 0x00},
	{0x1054, 0x00},
	{0x1055, 0x25},        // Set VS High
	{0x1056, 0x99},
	{0x1057, 0x00},
	{0x1058, 0x00},        // Set VS Low
	{0x1059, 0x2A},
	{0x105A, 0xF8},
	{0x105B, 0x00},        // Set HS Delay V2H
	{0x105C, 0x00},
	{0x105D, 0x00},
	{0x105E, 0x08},        // Set HS_HIGH
	{0x105F, 0x6C},
	{0x1060, 0x00},        // Set HS_LOW
	{0x1061, 0x2C},
	{0x1062, 0x04},        // Set HS_CNT
	{0x1063, 0x65},
	{0x1064, 0x01},        // Set DE Delay
	{0x1065, 0x61},
	{0x1066, 0x18},
	{0x1067, 0x07},        // Set DE_HIGH
	{0x1068, 0x80},
	{0x1069, 0x01},        // Set DE_LOW
	{0x106A, 0x18},
	{0x106B, 0x04},        // Set DE_CNT
	{0x106C, 0x38},
	{0x106D, 0x03},        // Set Grad_INCR_0_0

// C{HECKERBOARD }SETUP - PATGEN MODE = 1
	{0x106E, 0x88},        // Set CHKR_COLOR_A_L_0
	{0x106F, 0xAA},        // Set CHKR_COLOR_A_M_0
	{0x1070, 0x55},        // Set CHKR_COLOR_A_H_0
	{0x1071, 0x00},        // Set CHKR_COLOR_B_L_0
	{0x1072, 0x08},        // Set CHKR_COLOR_B_M_0
	{0x1073, 0x80},        // Set CHKR_COLOR_B_H_0
	{0x1074, 0x50},        // Set CHKR_RPT_A_0
	{0x1075, 0xA0},        // Set CHKR_RPT_B_0
	{0x1076, 0x50},        // Set CHKR_ALT_0

// S{et Patgen Cl}k frequency 75MHz firstly
	{0x01DC, 0x00},
	{0x01FC, 0x00},
	{0x021C, 0x00},
	{0x023C, 0x00},

// R{GB888 Softwa}re override VC/DT/BPP for Pipe 1,2,3 and 4; 0x24 = 6'b100100, 24 = 5'b11000; 0x2C = 6'b101100, 12 = 5'b01100
	{0x040B, 0xC2},        // BPP override for 1
	{0x040C, 0x10},        // VC override for 1/2/3/4
	{0x040D, 0x32},
	{0x040E, 0xA4},        // DT override for 1/2/3/4
	{0x040F, 0x94},
	{0x0410, 0x90},
	{0x0411, 0xD8},        // BPP override for 2/3/4
	{0x0412, 0x60},
	{0xFFFF, 0xFF}	/* End Table */
};

const struct max96712_reg max96712_color_pattern_cinit[] = {
	{0x1050, 0xE3},	/* VRX_Patgen 0, Generate VS, HS, DE", Invert the VS */
	{0x1051, 0x20},	/* Set Patgen mode=2 (Color Gradient), Grad Mode=0 */
	{0x1052, 0x00},
	{0x1053, 0x00},
	{0x1054, 0x00},
	{0x1055, 0x25},	/* Set VS High  */
	{0x1056, 0x99},
	{0x1057, 0x00},
	{0x1058, 0x00},	/* Set VS Low  */
	{0x1059, 0x2A},
	{0x105A, 0xF8},
	{0x105B, 0x00},	/* Set HS Delay V2H */
	{0x105C, 0x00},
	{0x105D, 0x00},
	{0x105E, 0x08},	/* Set HS_HIGH */
	{0x105F, 0x6C},
	{0x1060, 0x00},	/* Set HS_LOW */
	{0x1061, 0x2C},
	{0x1062, 0x04},	/* Set HS_CNT */
	{0x1063, 0x65},
	{0x1064, 0x01},	/* Set DE Delay */
	{0x1065, 0x35},
	{0x1066, 0x60},
	{0x1067, 0x07},	/* Set DE_HIGH */
	{0x1068, 0x80},
	{0x1069, 0x01},	/* Set DE_LOW */
	{0x106A, 0x18},
	{0x106B, 0x04},	/* Set DE_CNT */
	{0x106C, 0x38},
	{0x106D, 0x03},	/* Set Grad_INCR_0_0 */

/* CHECKERBOARD SETUP - PATGEN MODE = 1 */
	{0x106E, 0x88},	/* Set CHKR_COLOR_A_L_0 */
	{0x106F, 0xAA},	/* Set CHKR_COLOR_A_M_0 */
	{0x1070, 0x55},	/* Set CHKR_COLOR_A_H_0 */
	{0x1071, 0x00},	/* Set CHKR_COLOR_B_L_0 */
	{0x1072, 0x08},	/* Set CHKR_COLOR_B_M_0 */
	{0x1073, 0x80},	/* Set CHKR_COLOR_B_H_0 */
	{0x1074, 0x50},	/* Set CHKR_RPT_A_0 */
	{0x1075, 0xA0},	/* Set CHKR_RPT_B_0 */
	{0x1076, 0x50},	/* Set CHKR_ALT_0 */

/* Set Patgen Clk frequency 75MHz firstly */
	{0x0009, 0x01},	/*75MHz -> 150MHz */
	{0x01DC, 0x00},
	{0x01FC, 0x00},
	{0x021C, 0x00},
	{0x023C, 0x00},
/*
 * RGB888 Software override VC/DT/BPP for Pipe 1,2,3
 * and 4; 0x24 = 6'b100100, 24 = 5'b11000; 0x2C = 6'b101100, 12 = 5'b01100
 */
	{0x040B, 0xC2},	/* BPP override for 1 */
	{0x040C, 0x10},	/* VC override for 1/2/3/4 */
	{0x040D, 0x32},
	{0x040E, 0xA4},	/* DT override for 1/2/3/4 */
	{0x040F, 0x94},
	{0x0410, 0x90},
	{0x0411, 0xD8},	/* BPP override for 2/3/4 */
	{0x0412, 0x60},

	{0x0006, 0x00},	/* Disable GMSL link */
	{0x0415, 0xEd},
	{0x0418, 0xE9},
	{0x094A, 0xa0},
	{0x098A, 0xa0},
	{0x08ad, 0x3f},
	{0x08ae, 0x7d},

	{0x08A3, 0xE4},	/* Set Phy lane Map for all MIPI PHYs */
	{0x090B, 0x07},	/* Set MAP_EN_L_0 for map 0 */
	{0x094B, 0x07},	/* Set MAP_EN_L_0 for map 0 */
	{0x098B, 0x07},	/* Set MAP_EN_L_0 for map 0 */
	{0x09CB, 0x07},	/* Set MAP_EN_L_0 for map 0 */
	{0x092D, 0x15},	/* Set MAP_DPHY_DEST TO CTRL 0 */
	{0x096D, 0x00},	/* Set MAP_DPHY_DEST TO CTRL 1 */
	{0x09AD, 0x2a},	/* Set MAP_DPHY_DEST TO CTRL 2 */
	{0x09ED, 0x3f},	/* Set MAP_DPHY_DEST TO CTRL 3 */
	{0x090D, 0x24},	/* Set MAP_SRC0 [7:6]=VC=0, [5:0]=DT=RGB888 */
	{0x094D, 0x64},	/* Set MAP_SRC1 [7:6]=VC=1, [5:0]=DT=RGB888 */
	{0x098D, 0xa4},	/* Set MAP_SRC2 [7:6]=VC=2, [5:0]=DT=RGB888 */
	{0x09CD, 0xe4},	/* Set MAP_SRC3 [7:6]=VC=3, [5:0]=DT=RGB888 */
	{0x090E, 0x24},
	{0x094E, 0x64},
	{0x098E, 0xa4},
	{0x09CE, 0xe4},
	{0x090F, 0x00},
	{0x094F, 0x00},
	{0x098F, 0x00},
	{0x09CF, 0x00},
	{0x0910, 0x00},
	{0x0950, 0x40},
	{0x0990, 0x80},
	{0x09d0, 0xc0},
	{0x0911, 0x01},
	{0x0951, 0x01},
	{0x0991, 0x01},
	{0x09d1, 0x01},
	{0x0912, 0x01},
	{0x0952, 0x41},
	{0x0992, 0x81},
	{0x09d2, 0xc1},
	{0xFFFF, 0xFF}	/* End Table */
};

const struct max96712_reg max96712_out_enable[] = {
// ----------Deserializer Setup--------------------------------------------------------------------
	{0x0006, 0xF0},        // Disable GMSL link
	{0x0903, 0x07},
	{0x0904, 0x01},
	{0x1c00, 0xf4},
	{0x0415, 0xEC},        // Set 1200M DPLL frequency, Enable the software override for 1/2/3/4
	{0x1c00, 0xf5},
	{0x0943, 0x07},
	{0x0944, 0x01},
	{0x1d00, 0xf4},
	{0x0418, 0xEC},
	{0x1d00, 0xf5},
	{0x08A3, 0xE4},        // Set Phy lane Map for all MIPI PHYs
	{0x08A4, 0xE4},
	{0x0939, 0xd0},
	{0x0936, 0x00},
	{0x0979, 0xd0},
	{0x0976, 0x00},
	{0x09b9, 0xd0},
	{0x09b6, 0x00},
	{0x09f9, 0xd0},
	{0x09f6, 0x00},
	{0x090B, 0x07},        // Set MAP_EN_L_0 for map 0
	{0x094B, 0x07},        // Set MAP_EN_L_0 for map 0
	{0x098B, 0x07},        // Set MAP_EN_L_0 for map 0
	{0x09CB, 0x07},        // Set MAP_EN_L_0 for map 0
	{0x092D, 0x15},        // Set MAP_DPHY_DEST TO CTRL 1
	{0x090D, 0x24},        // Set MAP_SRC AND DEST for data and FE/FS; [7:6]=VC, [5:0]=DT
	{0x094D, 0x64},        // Set MAP_SRC AND DEST for data and FE/FS; [7:6]=VC, [5:0]=DT
	{0x098D, 0xA4},        // Set MAP_SRC AND DEST for data and FE/FS; [7:6]=VC, [5:0]=DT
	{0x09CD, 0xE4},        // Set MAP_SRC AND DEST for data and FE/FS; [7:6]=VC, [5:0]=DT
	{0x090E, 0x24},
	{0x090F, 0x00},
	{0x0910, 0x00},
	{0x0911, 0x01},
	{0x0912, 0x01},
	{0x08A0, 0x84},        // Enable CSI clock w/ MIPI PHY 2x4 configuration
	{0x08A2, 0x30},        // Turn off unused PHYs

// End of Setup Script - MUST ONE-SHOT RESET!!!! <=================================================
	{0x0018, 0x0F},        // One shot reset for PHY A - HS86
	{0xFFFF, 0xFF}	/* End Table */
};

const struct max96712_reg max96712_out_disable[] = {
	{0x08A0, 0x04},         // Disable CSI clock w/ MIPI PHY 2x4 configuration,,
	{0x08A2, 0x00},         // Stanby mode PHYs
	{0xFFFF, 0xFF}    // End Table
};

static int max96712_power(struct max96712_priv *priv, int on)
{
	unsigned int reg, val;

	reg = MAX96712_GPIO(priv->campw_gpio);

	regmap_read(priv->regmap, 0x000D, &val);
	priv->dev_id = val;

	if (priv->campw_gpio && on) {
		regmap_read(priv->regmap, reg, &val);
		val &= ~(BIT(7) | BIT(0));
		val |= BIT(4);		// GPIO1=High=Camemra POC Protector ON
		regmap_write(priv->regmap, reg, val);
	}

	if(priv->campw_gpio && !on) {
		regmap_read(priv->regmap, reg, &val);
		val &= ~BIT(4);
		regmap_write(priv->regmap, reg, val);
	}
	mdelay(100);

	return 0;
}

#if DEBUG_COLOR_PATTERN

static int max96712_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max96712_priv *priv = container_of(ctrl->handler,
	    	    				  struct max96712_priv, ctrls);
	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		priv->test_pattern_mode = ctrl->val;
		break;
	default:
		return 0;
	}

	return 0;
}

static const struct v4l2_ctrl_ops max96712_ctrl_ops = {
	.s_ctrl = max96712_s_ctrl,
};

static const char * const max96712_test_pattern_menu[] = {
	"Camera Mode",
	"Gradient Pattern Mode",
	"Checkerboard Pattern Mode",
};

static const struct v4l2_ctrl_config max96712_test_pattern_cfg = {
	.ops = &max96712_ctrl_ops,
	.id = V4L2_CID_TEST_PATTERN,
	.name = "Test Pattern",
	.type = V4L2_CTRL_TYPE_MENU,
	.max = 2,
	.def = 0,
	.qmenu = max96712_test_pattern_menu,
};

static int max96712_set_regs(struct max96712_priv *priv,
			     const struct max96712_reg *regs,
			     unsigned int nr_regs)
{
	unsigned int i;
	int ret;

	for (i = 0; i < nr_regs; i++) {
		ret = regmap_write(priv->regmap, regs[i].reg, regs[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

static int max96712_color_pattern(struct max96712_priv *priv)
{
	int ret;

	max96712_power(priv, 1);

	if(priv->cphy_connection)
		ret = max96712_set_regs(priv, max96712_color_pattern_cinit,
					ARRAY_SIZE(max96712_color_pattern_cinit));
	else
		ret = max96712_set_regs(priv, max96712_color_pattern_dinit,
					ARRAY_SIZE(max96712_color_pattern_dinit));

	if(priv->test_pattern_mode == 1)
		ret = regmap_write(priv->regmap, 0x1051, 0x20);	//Gradient pattern
	else
		ret = regmap_write(priv->regmap, 0x1051, 0x10);	//Checkerboard pattern

	regmap_write(priv->regmap, 0x0006, 0xFF);
	regmap_write(priv->regmap, 0x0010, 0x22);
	regmap_write(priv->regmap, 0x0011, 0x22);
	regmap_write(priv->regmap, 0x0018, 0x0F);

	msleep(300); /* Delay ~100ms */

	if (priv->dev_id == MAX96712_ID)
		regmap_write(priv->regmap, 0x0009, 0x02);

	regmap_write(priv->regmap, 0x08A0, 0x84);
	regmap_write(priv->regmap, 0x08A2, 0x30);
	regmap_write(priv->regmap, 0x0018, 0x0F);

	return ret;
}
#endif

static void max96712_internal_fsync(struct max96712_priv *priv)
{
	/* 
	 * Method: manual, Internal GMSL2 generator mode: 00,
	 * source of FSYNC: Des.
	 */
	regmap_write(priv->regmap, MAX96712_FSYNC_0,  0x04);  
	
	/* Turn off auto master link selection */
	regmap_write(priv->regmap, MAX96712_FSYNC_2,  0x00); 
	
	/* Disable overlap window */
	regmap_write(priv->regmap, MAX96712_FSYNC_10, 0x00); 
	regmap_write(priv->regmap, MAX96712_FSYNC_11, 0x00); 

	/* disable error threshold */
	regmap_write(priv->regmap, MAX96712_FSYNC_8,  0x00); 
	regmap_write(priv->regmap, MAX96712_FSYNC_9,  0x00); 
	
	/* AUTO_FS_LINKS = 0, FS_USE_XTAL = 1, FS_LINK_[3:0] = 1*/
	regmap_write(priv->regmap, MAX96712_FSYNC_15, 0xCF);   
	
	/* fsync_period = 25 MHz / 30fps */		   
	priv->fsync_period = 25000000 / 30;	      
	regmap_write(priv->regmap, MAX96712_FSYNC_5,
		     (priv->fsync_period >>  0) & 0xFF); /* Fsync Period L */
	regmap_write(priv->regmap, MAX96712_FSYNC_6,
		     (priv->fsync_period >>  8) & 0xFF); /* Fsync Period M */
	regmap_write(priv->regmap, MAX96712_FSYNC_7,
		     (priv->fsync_period >> 16) & 0xFF); /* Fsync Period H */

	/* FSYNC_TX GPIO ID = 8 */
	regmap_write(priv->regmap, MAX96712_FSYNC_17, 0x40);
}

static void max96712_external_fsync(struct max96712_priv *priv)
{
	/* FSYNC_MODE: 10, source of FSYNC: SoC */
	regmap_write(priv->regmap, MAX96712_FSYNC_0,  0x08);  
	
	/* AUTO_FS_LINKS = 1, FS_LINK_[3:0] = 1*/
	regmap_write(priv->regmap, MAX96712_FSYNC_15, 0x9F);  
	
	/* Config MFP2 to receive FSYNC signal */
	regmap_write(priv->regmap, 0x0306, 0x83);
	
	/* Config MAX96712 MFP2 receive external 
	 * FSYNC signal for each link, TX ID = 12 */
	regmap_update_bits(priv->regmap, 0x0307, 0x1F, 0x0C);
	regmap_update_bits(priv->regmap, 0x033D, 0x3F, 0x2C);
	regmap_update_bits(priv->regmap, 0x0374, 0x3F, 0x2C);
	regmap_update_bits(priv->regmap, 0x03AA, 0x3F, 0x2C);
}

//static void ap0202_change_config(struct max96712_priv *priv)
//{
	//reg16_write16_addr(priv->client, AP0202_ADDR, 0x098E, 0x7C00);
	//msleep(200);

	//reg16_write16_addr(priv->client, AP0202_ADDR, 0xFC00, 0x2800);
	//reg16_write16_addr(priv->client, AP0202_ADDR, 0x0040, 0x8100);
	//msleep(200);
//}

static void max96712_gmsl2_fsync_setup(struct max96712_priv *priv)
{
	if (FSYNC_INT_EXT == 0x01)
		max96712_internal_fsync(priv); /* From another MAX96712 */
	else if (FSYNC_INT_EXT == 0x02)
		max96712_external_fsync(priv); /* From SoC */
	
	//// AR0231's ISP
	//if (FSYNC_INT_EXT == 0x02)
	//{	
		//reg16_write16_addr(priv->client, AP0202_ADDR, 0x098e, 0xc890);
		//reg16_write_addr(priv->client, AP0202_ADDR, 0xc890, 0x03);
		//reg16_write_addr(priv->client, AP0202_ADDR, 0xc891, 0x03);
		//reg16_write_addr(priv->client, AP0202_ADDR, 0xc892, 0x00); // trigger types,continuous trigger mode
		//ap0202_change_config(priv);
	//} 
	//else if (FSYNC_INT_EXT == 0x00)
	//{
		//reg16_write16_addr(priv->client, AP0202_ADDR, 0x098e, 0xc890);
		//reg16_write_addr(priv->client, AP0202_ADDR, 0xc890, 0x00);
		//ap0202_change_config(priv);
	//}
}

static int max96712_enable(struct v4l2_subdev *sd, int enable)
{
	int ret = 0, i, pipes_mask;
	struct max96712_priv *priv = sd_to_max96712(sd);

	if(enable) {
#if DEBUG_COLOR_PATTERN
		if (priv->test_pattern_mode > 0) {
			ret = max96712_set_regs(priv, max96712_out_enable,
						ARRAY_SIZE(max96712_out_enable));
			return ret;
		}
#endif
		for (i = 0; i < MAX96712_NUM_GMSL; i++) {
			pipes_mask = priv->link[i]->pipes_mask;
			/* Enable link pipes */
			regmap_update_bits(priv->regmap, MAX96712_VIDEO_PIPE_EN,
					   pipes_mask, pipes_mask);
		}

		/* CSI output enable */
		regmap_update_bits(priv->regmap, MAX_BACKTOP12(0), 0x02, 0x02);
		/* Enable Force all MIPI clocks running */
		regmap_update_bits(priv->regmap, MAX_MIPI_PHY0, 0x80, 0x80);

	} else {
#if DEBUG_COLOR_PATTERN
		if(priv->test_pattern_mode > 0) {
			ret = max96712_set_regs(priv, max96712_out_disable,
						ARRAY_SIZE(max96712_out_disable));
			return ret;
		}
#endif
		/* Disable Force all MIPI clocks running */
		regmap_update_bits(priv->regmap, MAX_MIPI_PHY0, 0x80, 0x00);
		/* CSI output disable */
		regmap_update_bits(priv->regmap, MAX_BACKTOP12(0), 0x02, 0);

		for (i = 0; i < MAX96712_NUM_GMSL; i++) {
			pipes_mask = priv->link[i]->pipes_mask;
			/* Disable link pipes */
			regmap_update_bits(priv->regmap, MAX96712_VIDEO_PIPE_EN,
					   pipes_mask, 0);
		}
	}

	return ret;
}

#if DEBUG_REG_DUMP
static void max96712_debug_dump(struct max96712_priv *priv)
{
	unsigned int i, val, valu[0x1000];

	for (i = 0; i < 0x1000; i++) {
		regmap_read(priv->regmap, i, &val);
		valu[i] = val;
	}

	for (i = 0; i < 0x1000; i += 16)
		printk("0x%04x\t%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x \n",
			i, valu[i], valu[i+1], valu[i+2], valu[i+3], valu[i+4], valu[i+5], valu[i+6], valu[i+7],
			valu[i+8], valu[i+9], valu[i+10], valu[i+11], valu[i+12], valu[i+13], valu[i+14], valu[i+15]);
}
#endif

static void max96712_reset_oneshot(struct max96712_priv *priv, int mask)
{
	int timeout;
	unsigned int val;

	mask &= 0x0f;
	regmap_update_bits(priv->regmap, MAX96712_CTRL1, mask, mask);

	/* wait for one-shot bit self-cleared */
	for (timeout = 0; timeout < 100; timeout++) {
		regmap_read(priv->regmap, MAX96712_CTRL1, &val);
		if (!(val & mask))
			break;

		mdelay(1);
	}

	if (val & mask)
		dev_err(&priv->client->dev, "Failed reset oneshot 0x%x\n", mask);
}

static void max96712_postinit(struct max96712_priv *priv)
{
	regmap_update_bits(priv->regmap, MAX96712_REG6, 0x0f, priv->links_mask);
	max96712_reset_oneshot(priv, priv->links_mask);
	msleep(100);
}

static int max96712_preinit(struct max96712_priv *priv)
{
	int i;

	regmap_update_bits(priv->regmap, MAX96712_PWR1, BIT(6), BIT(6));
	usleep_range(10000, 20000);

	regmap_write(priv->regmap, MAX96712_REG1, 0xCC); // DIS_LOC_CC_P2
	usleep_range(10000, 20000);

	if (priv->dev_id == MAX96724_ID || priv->dev_id == MAX96724F_ID ||
	    priv->dev_id == MAX96724R_ID) {
		regmap_write(priv->regmap, 0x0309, 0x84);
		regmap_write(priv->regmap, 0x030B, 0x0B);
		regmap_write(priv->regmap, 0x030C, 0x63);
		regmap_write(priv->regmap, 0x030D, 0x0C);
	} else {
		regmap_write(priv->regmap, 0x0323, 0x84);
		regmap_write(priv->regmap, 0x0325, 0x0B);
		regmap_write(priv->regmap, 0x0326, 0x63);
		regmap_write(priv->regmap, 0x0327, 0x2C);
	}

	regmap_write(priv->regmap, MAX96712_REG5, 0x40);
	usleep_range(10000, 20000);

	/* enable internal regulator for 1.2V VDD supply */
	regmap_update_bits(priv->regmap, MAX96712_CTRL0, BIT(2), BIT(2));
	regmap_update_bits(priv->regmap, MAX96712_CTRL2, BIT(4), BIT(4));

	/* I2C-I2C timings */
	for (i = 0; i < 8; i++) {
		regmap_write(priv->regmap, MAX96712_I2C_0(i), 0x01);
		regmap_write(priv->regmap, MAX96712_I2C_1(i), 0x51);
	}

	regmap_update_bits(priv->regmap, MAX96712_CTRL11, 0x55, 0x55);
	regmap_update_bits(priv->regmap, MAX96712_REG6, 0x0f, 0);

	return 0;
}

static void max96712_gmsl2_initial_setup(struct max96712_priv *priv)
{
	regmap_update_bits(priv->regmap, MAX96712_REG6, 0xF0, 0xF0);
	regmap_write(priv->regmap, MAX96712_REG26, 0x22);
	regmap_write(priv->regmap, MAX96712_REG27, 0x22);
}

static int max96712_mipi_setup(struct max96712_priv *priv)
{
	struct device dev = priv->client->dev;
	unsigned int lane_cnt = (priv->csi2_data_lanes - 1) << 6;
	u32 per_link_raw_bps = 1800;
	u32 per_link_bps = 1800;
	
	if(priv->total_pixel_rate > 0) {
		u64 mbps = (u64)priv->total_pixel_rate * priv->bpp;
		do_div(mbps, priv->csi2_data_lanes * 1000000);
		do_div(mbps, 2.28);
		per_link_raw_bps = mbps;
		per_link_bps = ((per_link_raw_bps + 99) / 100) * 100;
	}
 
	dev_dbg(&dev, "total_pixel_rate = %u ; lanes = %d ; bpp = %d\n",
		priv->total_pixel_rate, priv->csi2_data_lanes, priv->bpp);
	dev_dbg(&dev,"per_link_raw_bps = %d\n", per_link_raw_bps);
	dev_dbg(&dev,"per_link_bps = %d\n", per_link_bps);
	
	max96712_power(priv, 1);

	regmap_write(priv->regmap, MAX96712_VIDEO_PIPE_EN, 0);

	if (priv->csi2_data_lanes > 2) {
		regmap_write(priv->regmap, MAX_MIPI_PHY0, 0x04);
		regmap_write(priv->regmap, MAX_MIPI_PHY3, 0xe4);
		regmap_write(priv->regmap, MAX_MIPI_PHY4, 0xe4);
	} else {
		regmap_write(priv->regmap, MAX_MIPI_PHY0, 0x00);
		regmap_write(priv->regmap, MAX_MIPI_PHY3, 0x44);
		regmap_write(priv->regmap, MAX_MIPI_PHY4, 0x44);
	}

	if (priv->cphy_connection) {
		regmap_write(priv->regmap, MAX_MIPI_TX10(1), lane_cnt | BIT(5));
		regmap_write(priv->regmap, MAX_MIPI_TX10(2), lane_cnt | BIT(5));
		regmap_write(priv->regmap, MAX_MIPI_PHY13, 0x3F);
		regmap_write(priv->regmap, MAX_MIPI_PHY14, 0x7D);
	} else {
		regmap_write(priv->regmap, MAX_MIPI_TX10(1), lane_cnt);
		regmap_write(priv->regmap, MAX_MIPI_TX10(2), lane_cnt);
		regmap_write(priv->regmap, MAX_MIPI_TX3(1), 0x07);
		regmap_write(priv->regmap, MAX_MIPI_TX3(2), 0x07);
	}

	regmap_update_bits(priv->regmap, MAX_BACKTOP22(0), 0x3f,
			   ((per_link_bps / 100) & 0x1f) | BIT(5));
	regmap_update_bits(priv->regmap, MAX_BACKTOP25(0), 0x3f,
			   ((per_link_bps / 100) & 0x1f) | BIT(5));
	regmap_update_bits(priv->regmap, MAX_BACKTOP28(0), 0x3f,
			   ((per_link_bps / 100) & 0x1f) | BIT(5));
	regmap_update_bits(priv->regmap, MAX_BACKTOP31(0), 0x3f,
			   ((per_link_bps / 100) & 0x1f) | BIT(5));

	regmap_update_bits(priv->regmap, MAX_MIPI_PHY2, 0xf0, 0xf0);
	if (priv->phy_pol_inv)
		regmap_write(priv->regmap, MAX_MIPI_PHY5, 0x10);

	usleep_range(10000, 20000);

	return 0;
}

static int max96712_gmsl2_get_link_lock(struct max96712_priv *priv, int link_n)
{
	unsigned int lock_reg[] = {MAX96712_CTRL3, MAX96712_CTRL12,
				   MAX96712_CTRL13, MAX96712_CTRL14};
	unsigned int val;
	regmap_read(priv->regmap, lock_reg[link_n], &val);

	return !!(val & BIT(3));
}

static void max96712_pipe_override(struct max96712_priv *priv,
				   unsigned int pipe, unsigned int dt,
				   unsigned int vc)
{
	int bpp, bank;

	bpp = mipi_dt_to_bpp(dt);
	bank = pipe / 4;
	pipe %= 4;

	switch (pipe) {
	case 0:
		/* Pipe X: 0 or 4 */
		regmap_update_bits(priv->regmap, MAX_BACKTOP12(bank), 0x1f << 3,
				   bpp << 3);
		regmap_update_bits(priv->regmap, MAX_BACKTOP13(bank), 0x0f, vc);
		regmap_update_bits(priv->regmap, MAX_BACKTOP15(bank), 0x3f, dt);
		regmap_update_bits(priv->regmap, bank ? MAX_BACKTOP28(0) :
				   MAX_BACKTOP22(0), BIT(6), BIT(6));
		break;
	case 1:
		/* Pipe Y: 1 or 5 */
		regmap_update_bits(priv->regmap, MAX_BACKTOP18(bank), 0x1f, bpp);
		regmap_update_bits(priv->regmap, MAX_BACKTOP13(bank), 0x0f << 4,
				   vc << 4);
		regmap_update_bits(priv->regmap, MAX_BACKTOP16(bank), 0x0f,
				   dt & 0x0f);
		regmap_update_bits(priv->regmap, MAX_BACKTOP15(bank), 0x03 << 6,
				   (dt & 0x30) << 2);
		regmap_update_bits(priv->regmap, bank ? MAX_BACKTOP28(0) :
				   MAX_BACKTOP22(0), BIT(7), BIT(7));
		break;
	case 2:
		/* Pipe Z: 2 or 6 */
		regmap_update_bits(priv->regmap, MAX_BACKTOP19(bank), 0x03,
				   bpp & 0x03);
		regmap_update_bits(priv->regmap, MAX_BACKTOP18(bank), 0xe0,
				   (bpp & 0x1c) << 3);
		regmap_update_bits(priv->regmap, MAX_BACKTOP14(bank), 0x0f, vc);
		regmap_update_bits(priv->regmap, MAX_BACKTOP17(bank), 0x03,
				   dt & 0x03);
		regmap_update_bits(priv->regmap, MAX_BACKTOP16(bank), 0x0f << 4,
				   (dt & 0x3c) << 2);
		regmap_update_bits(priv->regmap, bank ? MAX_BACKTOP30(0) :
				   MAX_BACKTOP25(0), BIT(6), BIT(6));
		break;
	case 3:
		/* Pipe U: 3 or 7 */
		regmap_update_bits(priv->regmap, MAX_BACKTOP19(bank), 0xfc,
				   bpp << 2);
		regmap_update_bits(priv->regmap, MAX_BACKTOP14(bank), 0x0f << 4,
				   vc << 4);
		regmap_update_bits(priv->regmap, MAX_BACKTOP17(bank), 0x3f << 2,
				   dt << 2);
		regmap_update_bits(priv->regmap, bank ? MAX_BACKTOP30(0) :
				   MAX_BACKTOP25(0), BIT(7), BIT(7));
		break;
	}
}

static void max96712_mapping_pipe_to_mipi(struct max96712_priv *priv,
					  unsigned int pipe,
					  unsigned int map_n,
					  unsigned int in_dt,
					  unsigned int in_vc,
					  unsigned int out_dt,
					  unsigned int out_vc,
					  unsigned int out_mipi)
{
	int offset = 2 * (map_n % 4);
	regmap_write(priv->regmap, MAX_MIPI_MAP_SRC(pipe, map_n),
		     (in_vc << 6) | in_dt);
	regmap_write(priv->regmap, MAX_MIPI_MAP_DST(pipe, map_n),
		     (out_vc << 6) | out_dt);
	regmap_update_bits(priv->regmap, MAX_MIPI_MAP_DST_PHY(pipe, map_n / 4),
			   0x03 << offset, out_mipi << offset);
	/* enable SRC_n to DST_n mapping */
	regmap_update_bits(priv->regmap, MAX_MIPI_TX11(pipe), BIT(map_n),
			   BIT(map_n));
	regmap_update_bits(priv->regmap, MAX_MIPI_TX11(pipe), BIT(map_n),
			   BIT(map_n));

	usleep_range(10000, 20000);
}

static void max96712_gmsl2_pipe_set_source(struct max96712_priv *priv,
					   int pipe, int phy, int in_pipe)
{
	int offset = (pipe % 2) * 4;
	regmap_update_bits(priv->regmap,
			   MAX96712_VIDEO_PIPE_SEL(pipe / 2), 0x0f << offset,
			   (phy << (offset + 2)) | (in_pipe << offset));
}

static struct {
	int in_dt;
	int out_dt;
} gmsl2_pipe_maps[] = {
	{MIPI_DT_RAW10,	MIPI_DT_RAW10},	/* payload data */
	{0x00,		0x00},		/* FS */
	{0x01,		0x01},		/* FE */	

};

static void max96712_gmsl2_link_pipe_setup(struct max96712_priv *priv,
					   int link_n)
{
	struct max96712_link *link = priv->link[link_n];
	int pipe = link_n; /* straight mapping */
	int dt = priv->dt; /* must come from imager */
	int in_vc = 0;
	int i;

	switch(priv->sources[link_n].code) {
		case MEDIA_BUS_FMT_YUYV10_2X10:
		case MEDIA_BUS_FMT_UYVY10_2X10:
		case MEDIA_BUS_FMT_VYUY10_2X10:
		case MEDIA_BUS_FMT_YVYU10_2X10:
		case MEDIA_BUS_FMT_Y10_1X10:
			dt = MIPI_DT_RAW10;
			gmsl2_pipe_maps[0].in_dt = dt;
			gmsl2_pipe_maps[0].out_dt = dt;		
			break;
		default:
			break;
	}

	max96712_gmsl2_pipe_set_source(priv, pipe, link_n, 0);

	regmap_write(priv->regmap, MAX96712_RX0(pipe), 0);
	regmap_write(priv->regmap, MAX_VIDEO_RX0(pipe), 0x00);
	max96712_pipe_override(priv, pipe, dt, in_vc);
	usleep_range(10000, 20000);

	regmap_write(priv->regmap, MAX_MIPI_TX11(pipe), 0x00);
	regmap_write(priv->regmap, MAX_MIPI_TX12(pipe), 0x00);

	for (i = 0; i < ARRAY_SIZE(gmsl2_pipe_maps); i++) {
		max96712_mapping_pipe_to_mipi(priv, pipe, i,
					      gmsl2_pipe_maps[i].in_dt,
					      in_vc, gmsl2_pipe_maps[i].out_dt,
					      link->out_vc, link->out_mipi);
	}

	link->pipes_mask |= BIT(pipe);
}

static int max96712_gmsl2_reverse_channel_setup(struct max96712_priv *priv,
						int link_n)
{
	struct max96712_link *link = priv->link[link_n];
	int ser_addrs[] = {0x40, 0x42, 0x60, 0x62};
	int timeout = 100;
	int ret = 0;
	int val = 0, i, j = 0;

	regmap_update_bits(priv->regmap, MAX96712_REG6, 0xff,
			   0xf0 | BIT(link_n));
	max96712_reset_oneshot(priv, BIT(link_n));

	/*
	 * wait the link to be established,
	 * indicated when status bit LOCKED goes high
	 */
	for (; timeout > 0; timeout--) {
		if (max96712_gmsl2_get_link_lock(priv, link_n))
			break;
		mdelay(1);
	}

	if (!timeout) {
		ret = -ETIMEDOUT;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(ser_addrs); i++) {
		/* read serializer ID */
		__reg16_read(ser_addrs[i], 0x000d, &val);
		if (val == MAX9295A_ID || val == MAX9295B_ID) {
			dev_dbg(&priv->client->dev, "ID val:0x%x>\n", val);
			link->ser_id = val;
			 /* relocate serizlizer on I2C bus */
			__reg16_write(ser_addrs[i], 0x0000, link->ser_addr << 1);
			usleep_range(2000, 2500);
			j = i;
		}
	}

	priv->links_mask |= BIT(link_n);

out:
	dev_info(&priv->client->dev, "link%d %s %sat 0x%x (0x%x) %s\n",
		 link_n, chip_name(link->ser_id),
		 ret == -EADDRINUSE ? "already " : "",
		 link->ser_addr, ser_addrs[j], ret == -ETIMEDOUT ?
		 "not found: timeout GMSL2 link establish" : "");
	return ret;
}

static void max96712_setup(struct max96712_priv *priv)
{
	struct max96712_source *source;
	int ret;
	int link = 0;

#if DEBUG_COLOR_PATTERN
	if(priv->test_pattern_mode > 0) {
		max96712_color_pattern(priv);
		for_each_source(priv, source)
			source->linkup = false;

		return;
	}
#endif
	max96712_preinit(priv);
	max96712_gmsl2_initial_setup(priv);
	max96712_mipi_setup(priv);
	
	if (FSYNC_INT_EXT != 0x00)
		max96712_gmsl2_fsync_setup(priv);
		
	/* Start all cameras. */
	for_each_source(priv, source) {
		max96712_gmsl2_link_pipe_setup(priv, link);
		ret = max96712_gmsl2_reverse_channel_setup(priv, link);
		if (ret < 0)
			source->linkup = false;
		else
			source->linkup = true;

		link++;
	}

	return;
}

/* -----------------------------------------------------------------------------
 * Start stream
 */
static int max96712_s_stream(struct v4l2_subdev *sd, int enable)
{
	int ret;
	struct max96712_priv *priv = sd_to_max96712(sd);
	struct max96712_source *source;

	if (enable && priv->stream_count == 0) {
		if (!FAST_STARTUP) {
			max96712_setup(priv);
			max96712_postinit(priv);
		}

		for_each_source(priv, source) {
			if (!source->linkup)
				continue;

			ret = v4l2_subdev_call(source->sd, video, s_stream, 1);
			if (ret)
				return ret;
		}
	} else if (!enable && priv->stream_count == 1 && !FAST_STARTUP) {
		max96712_power(priv, 0);

		/* Stop all cameras. */
		for_each_source(priv, source) {
			if (!source->linkup)
				continue;

			v4l2_subdev_call(source->sd, video, s_stream, 0);
		}
		gpiod_direction_output_raw(priv->gpiod_pwdn, 0);
		gpiod_direction_output_raw(priv->gpiod_pwdn, 1);
	}

	priv->stream_count += enable ? 1 : -1;

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdev ops
 */
static int max96712_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_Y10_1X10;

	return 0;
}

static struct v4l2_mbus_framefmt *
max96712_get_pad_format(struct max96712_priv *priv,
			struct v4l2_subdev_pad_config *cfg,
			unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&priv->sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &priv->fmt[pad];
	default:
		return NULL;
	}
}

static int max96712_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct max96712_priv *priv = sd_to_max96712(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (format->pad >= MAX96712_SRC_PAD)
		return -EINVAL;

	/* Refuse non YUV422 formats as we hardcode DT to 8 bit YUV422 */
	switch (format->format.code) {
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
		break;
	default:
		format->format.code = MEDIA_BUS_FMT_Y10_1X10;
		break;
	}

	fmt = max96712_get_pad_format(priv, cfg, format->pad, format->which);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	return 0;
}

static int max96712_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct max96712_priv *priv = sd_to_max96712(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (format->pad >= MAX96712_SRC_PAD)
		return -EINVAL;

	fmt = max96712_get_pad_format(priv, cfg, format->pad, format->which);
	if (!fmt)
		return -EINVAL;

	format->format = *fmt;

	return 0;
}

static const struct v4l2_subdev_video_ops max96712_video_ops = {
	.s_stream    = max96712_s_stream,
	.enable_link = max96712_enable,
};

static const struct v4l2_subdev_pad_ops max96712_pad_ops = {
	.enum_mbus_code = max96712_enum_mbus_code,
	.get_fmt        = max96712_get_fmt,
	.set_fmt        = max96712_set_fmt,
};

static const struct v4l2_subdev_ops max96712_subdev_ops = {
	.video      = &max96712_video_ops,
	.pad        = &max96712_pad_ops,
};

static void max96712_init_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width          = 1920;
	fmt->height         = 1020;
	fmt->code           = MEDIA_BUS_FMT_Y10_1X10;
	fmt->colorspace     = V4L2_COLORSPACE_SRGB;
	fmt->field          = V4L2_FIELD_NONE;
	fmt->ycbcr_enc      = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization   = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func      = V4L2_XFER_FUNC_DEFAULT;
}

static int max96712_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *fmt;
	unsigned int i;

	for (i = 0; i < MAX96712_N_SINKS; i++) {
		fmt = v4l2_subdev_get_try_format(subdev, fh->pad, i);
		max96712_init_format(fmt);
	}

	return 0;
}

static const struct v4l2_subdev_internal_ops max96712_subdev_internal_ops = {
	.open = max96712_open,
};

/* -----------------------------------------------------------------------------
 * V4L2 Subdev bound
 */
static int max96712_notify_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_subdev *asd)
{
	struct max96712_priv *priv = sd_to_max96712(notifier->sd);
	struct device dev = priv->client->dev;
	struct max96712_source *source = asd_to_max96712_source(asd);
	struct v4l2_ctrl *ser_mbps;
	unsigned int index = to_index(priv, source);
	unsigned int src_pad;
	struct v4l2_subdev_format fmt = { 0 };
	int ret;

	ret = media_entity_get_fwnode_pad(&subdev->entity, source->fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (ret < 0) {
		dev_err(&dev, "Failed to find pad for %s\n", subdev->name);
		return ret;
	}

	source->sd = subdev;
	src_pad = ret;
	priv->bound_sources |= BIT(index);
	ret = media_create_pad_link(&source->sd->entity, src_pad,
				    &priv->sd.entity, index,
				    MEDIA_LNK_FL_ENABLED);

	if (ret) {
		dev_err(&dev, "Unable to link %s:%u -> %s:%u\n",
			source->sd->name, src_pad, priv->sd.name, index);
		return ret;
	}

	dev_dbg(&dev, "Bound %s pad: %u on index %u\n",
		subdev->name, src_pad, index);

	fmt.pad = 0; 
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;

	ret = v4l2_subdev_call(subdev, pad, get_fmt, NULL, &fmt);
	if (ret) {
		dev_err(&dev, "Failed to get format from %s[%u]\n",
			subdev->name, index);
	} else {
		source->width = fmt.format.width;
		source->height = fmt.format.height;
		source->code = fmt.format.code;
	}	

	dev_dbg(&dev, "source->sd (subdev) = %s, priv->sd = %s\n",
		source->sd->name, priv->sd.name);

    	ser_mbps = v4l2_ctrl_find(subdev->ctrl_handler, V4L2_CID_PIXEL_RATE);
	dev_dbg(&dev, "[%d] ser_mbps->val = %d\n", index, ser_mbps->val);

	source->pixel_rate = ser_mbps->val;
	priv->total_pixel_rate += source->pixel_rate;
	
	if(max96712_get_bpp(source->code) > priv->bpp)
		priv->bpp = max96712_get_bpp(source->code);

	if (priv->bound_sources != priv->source_mask)
		return 0;

	if (!FAST_STARTUP && priv->pixel_rate_ctrl) {
		v4l2_ctrl_s_ctrl_int64(priv->pixel_rate_ctrl,
				       priv->total_pixel_rate);
		dev_dbg(&dev, "Set V4L2_CID_PIXEL_RATE total pixel rate = %u\n",
			priv->total_pixel_rate);
	}

	return 0;
}

static void max96712_notify_unbind(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct max96712_source *source = asd_to_max96712_source(asd);

	source->sd = NULL;
}

static const struct v4l2_async_notifier_operations max96712_notify_ops = {
	.bound = max96712_notify_bound,
	.unbind = max96712_notify_unbind,
};

/* -----------------------------------------------------------------------------
 * Parse endpoint and subdev setup
 */
static int max96712_v4l2_subdev_init(struct max96712_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct i2c_client *client = priv->client;
	struct fwnode_handle *ep;
	unsigned int i;
	int ret;

	for (i = 0; i < MAX96712_N_SINKS; i++)
		max96712_init_format(&priv->fmt[i]);

	v4l2_i2c_subdev_init(&priv->sd, client, &max96712_subdev_ops);
	priv->sd.internal_ops = &max96712_subdev_internal_ops;
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	v4l2_ctrl_handler_init(&priv->ctrls, 1);
#if DEBUG_COLOR_PATTERN
	priv->pixel_rate_ctrl = v4l2_ctrl_new_std(&priv->ctrls, NULL,
						  V4L2_CID_PIXEL_RATE, 1,
						  INT_MAX, 1,
						  (unsigned int)DEBUG_MBPS);

	v4l2_ctrl_new_custom(&priv->ctrls, &max96712_test_pattern_cfg, NULL);
#else
	priv->pixel_rate_ctrl = v4l2_ctrl_new_std(&priv->ctrls, NULL,
						  V4L2_CID_PIXEL_RATE, 1,
						  INT_MAX, 1,
						  (unsigned int)DEFAULT_MBPS);
#endif
	priv->sd.ctrl_handler = &priv->ctrls;

	ret = priv->ctrls.error;
	if (ret) {
		v4l2_ctrl_handler_free(&priv->ctrls);
		return ret;
	}

	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	priv->pads[MAX96712_SRC_PAD].flags = MEDIA_PAD_FL_SOURCE;
	for (i = 0; i < MAX96712_N_SINKS; i++)
		priv->pads[i].flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&priv->sd.entity, MAX96712_N_PADS,
				     priv->pads);
	if (ret)
		return ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
					     MAX96712_SRC_PAD, 0, 0);
	if (!ep) {
		dev_err(dev, "Unable to retrieve endpoint on \"port@4\"\n");
		return -ENOENT;
	}
	priv->sd.fwnode = ep;

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret < 0) {
		dev_err(dev, "Unable to register subdevice\n");
		fwnode_handle_put(ep);
		return ret;
	}

	return 0;
}

static void max96712_cleanup_dt(struct max96712_priv *priv)
{
	struct max96712_source *source;

	/*
	 * Not strictly part of the DT, but the notifier is registered during
	 * max96712_parse_dt(), and the notifier holds references to the fwnodes
	 * thus the cleanup is here to mirror the registration.
	 */
	v4l2_async_notifier_unregister(&priv->notifier);

	for_each_source(priv, source) {
		fwnode_handle_put(source->fwnode);
		source->fwnode = NULL;
	}
}

static int max96712_parse_dt(struct max96712_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct device_node *gmsl_link;
	struct device_node *node = NULL;
	struct device_node *np = dev->of_node;
	unsigned int gmsl_link_mask = 0, val = 0;
	int ret, pwdnb;

	if (of_property_read_bool(np, "maxim,invert_phy-pol"))
		priv->phy_pol_inv = true;
	else
		priv->phy_pol_inv = false;

	if (!of_property_read_u32(dev->of_node, "campw_gpio", &val))
		priv->campw_gpio = val;

	pwdnb = of_get_gpio(np, 0);
	if (!gpio_is_valid(pwdnb))
		return -EINVAL;

	priv->gpiod_pwdn = gpio_to_desc(pwdnb);

	gmsl_link = of_find_node_by_name(np, "gmsl-serializer");
	if (!gmsl_link) {
		dev_err(dev, "Failed to find gmsl-serializer node\n");
		return -EINVAL;
	}

	/* Identify which channels are enabled */
	for_each_child_of_node(gmsl_link, node) {
		u32 id = 0;

		of_property_read_u32(node, "link-id", &id);
		if (id >= MAX96712_NUM_GMSL)
			continue;

		if (!of_device_is_available(node)) {
			dev_dbg(dev, "Skipping disabled I2C bus port %u\n", id);
			continue;
		}

		gmsl_link_mask |= BIT(id);
	}
	of_node_put(node);
	of_node_put(gmsl_link);

	v4l2_async_notifier_init(&priv->notifier);

	/* Parse the endpoints */
	for_each_endpoint_of_node(np, node) {
		struct max96712_source *source;
		struct of_endpoint ep;
		struct i2c_client *client;
		struct i2c_board_info info = { };

		of_graph_parse_endpoint(node, &ep);
		dev_dbg(dev, "Endpoint %pOF on port %d",
			ep.local_node, ep.port);

		if (ep.port > MAX96712_NUM_GMSL) {
			dev_err(dev, "Invalid endpoint %s on port %d",
				of_node_full_name(ep.local_node), ep.port);
			continue;
		}

		/* For the source endpoint just parse the bus configuration. */
		if (ep.port == MAX96712_SRC_PAD) {
			struct v4l2_fwnode_endpoint vep = { .bus_type = 0 };
			int ret;

			ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(node),
							 &vep);
			if (ret) {
				of_node_put(node);
				return ret;
			}

			if (vep.bus_type != V4L2_MBUS_CSI2_DPHY &&
				vep.bus_type != V4L2_MBUS_CSI2_CPHY) {
				dev_err(dev, "Media bus %u type not supported\n",
					vep.bus_type);
				v4l2_fwnode_endpoint_free(&vep);
				of_node_put(node);
				return -EINVAL;
			}

			if (vep.bus_type == V4L2_MBUS_CSI2_CPHY)
				priv->cphy_connection = true;
			else
				priv->cphy_connection = false;

			priv->csi2_data_lanes = vep.bus.mipi_csi2.num_data_lanes;
			v4l2_fwnode_endpoint_free(&vep);
			continue;
		}

		/* Skip if the corresponding GMSL link is unavailable. */
		if (!(gmsl_link_mask & BIT(ep.port)))
			continue;

		if (priv->sources[ep.port].fwnode) {
			dev_err(dev,
				"Multiple port endpoints are not supported: %d",
				ep.port);
			continue;
		}

		source = &priv->sources[ep.port];
		source->fwnode = fwnode_graph_get_remote_endpoint(
						of_fwnode_handle(node));
		if (!source->fwnode) {
			dev_err(dev, "Endpoint %pOF has no remote endpoint\n",
				ep.local_node);
			continue;
		}

		info.addr = priv->link[ep.port]->ser_addr;
		info.of_node = of_graph_get_remote_port_parent(node);
		client = i2c_new_client_device(priv->client->adapter, &info);
		if (IS_ERR(client)) {
			dev_err(dev,
				"Invalid remote device: port %d\n", ep.port);
			continue;
		}

		priv->link[ep.port]->client = client;

		source->asd.match_type = V4L2_ASYNC_MATCH_FWNODE;
		source->asd.match.fwnode = source->fwnode;

		ret = v4l2_async_notifier_add_subdev(&priv->notifier,
						     &source->asd);
		if (ret) {
			v4l2_async_notifier_cleanup(&priv->notifier);
			of_node_put(node);
			return ret;
		}

		priv->source_mask |= BIT(ep.port);
		priv->nsources++;
	}
	of_node_put(node);

	/* Do not register the subdev notifier if there are no devices. */
	if (!priv->nsources)
		return 0;

	priv->notifier.ops = &max96712_notify_ops;

	ret = v4l2_async_subdev_notifier_register(&priv->sd, &priv->notifier);
	if (ret)
		v4l2_async_notifier_cleanup(&priv->notifier);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Probe/Remove
 */
static int max96712_probe(struct i2c_client *client)
{
	struct max96712_priv *priv;
	struct device_node *np = client->dev.of_node;
	unsigned int i;
	int ret;
	int addrs[5];

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	for (i = 0; i < MAX96712_NUM_GMSL; i++) {
		priv->link[i] = devm_kzalloc(&client->dev,
					     sizeof(*priv->link[i]),
					     GFP_KERNEL);
		if (!priv->link[i])
			return -ENOMEM;
	}

	priv->regmap = devm_regmap_init_i2c(client, &max96712_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(priv->regmap),
			       	     "Failed to init i2c regmap\n");

	priv->client = client;
	priv->dt = MIPI_DT_RAW10;
	priv->stream_count = 0;

	of_property_read_u32_array(np, "reg", addrs, ARRAY_SIZE(addrs));

	for (i = 0; i < MAX96712_NUM_GMSL; i++) {
		priv->link[i]->ser_addr = addrs[i+1];
		priv->link[i]->out_mipi = 1;
		priv->link[i]->out_vc = i;
	}
	i2c_set_clientdata(client, priv);

	ret = max96712_parse_dt(priv);
	if (ret < 0)
		goto err_free_link;

	gpiod_direction_output_raw(priv->gpiod_pwdn, 1);

	dev_dbg(&client->dev,
		"All max96712 probed: start initialization sequence\n");

	ret = max96712_v4l2_subdev_init(priv);
	if (ret < 0)
		goto err_free_dt;

	if (FAST_STARTUP) {
		max96712_setup(priv);
		max96712_postinit(priv);
	}

	return 0;

err_free_dt:
	max96712_cleanup_dt(priv);
err_free_link:
	 for (i = 0; i < MAX96712_NUM_GMSL; i++)
	 	i2c_unregister_device(priv->link[i]->client);

	return ret;
}

static int max96712_remove(struct i2c_client *client)
{
	int i;
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max96712_priv *priv = sd_to_max96712(sd);

	 for (i = 0; i < MAX96712_NUM_GMSL; i++)
	 	i2c_unregister_device(priv->link[i]->client);

	fwnode_handle_put(priv->sd.fwnode);
	v4l2_async_unregister_subdev(&priv->sd);

	max96712_cleanup_dt(priv);
	regmap_exit(priv->regmap);

	return 0;
}

static const struct of_device_id max96712_dt_ids[] = {
	{.compatible = "maxim,max96712"},
	{.compatible = "maxim,max96724"},
	{},
};
MODULE_DEVICE_TABLE(of, max96712_dt_ids);

static const struct i2c_device_id max96712_id[] = {
	{"max96712", 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, max96712_id);

static struct i2c_driver max96712_i2c_driver = {
	.driver	= {
		.name	= "max96712",
		.of_match_table	= of_match_ptr(max96712_dt_ids),
	},
	.probe_new	= max96712_probe,
	.remove		= max96712_remove,
	.id_table	= max96712_id,
};
module_i2c_driver(max96712_i2c_driver);

MODULE_ALIAS("MAX96712");
MODULE_DESCRIPTION("Maxim MAX96712 GMSL2 Deserializer Driver");
MODULE_LICENSE("GPL");
