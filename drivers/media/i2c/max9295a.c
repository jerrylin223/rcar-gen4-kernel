/*
 * The camera is connected to a Maxim MAX9295A GMSL2 serializer.
 * Take MAX9295A as virtual sensor.
 */
#include <linux/delay.h>
#include <linux/fwnode.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/regmap.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include "max9295a.h"

#define CAMERA_WIDTH		1920
#define CAMERA_HEIGHT		1020
#define CAMERA_FORMAT		MEDIA_BUS_FMT_YUYV10_2X10
#define CAMERA_FPS		30
#define MAX9295A_N_PADS		1
#define MAX9295A_SRC_PAD	0

#define TOTAL_WIDTH(w)		((w) * 160 / 100)
#define TOTAL_HEIGHT(h)		((h) * 170 / 100)

static const struct regmap_config max9295_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xFFFF,
};

struct max9295a_priv {
	struct i2c_client *client;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pads[MAX9295A_N_PADS];
	struct v4l2_mbus_framefmt fmt[MAX9295A_N_PADS];
	struct v4l2_ctrl_handler ctrls;
	struct fwnode_handle *fwnode;
	int stream_count;
};

static inline struct max9295a_priv *sd_to_max9295a(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max9295a_priv, sd);
}

static int max9295a_set_regs(struct max9295a_priv *priv,
			     const struct max9295a_reg *regs,
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

static int max9295a_initialize(struct max9295a_priv *priv)
{
	/* Implement as needed */
	return 0;
}

static int max9295a_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max9295a_priv *priv = sd_to_max9295a(sd);
	struct device *dev = &priv->client->dev;
	int ret;
	
	if (enable && priv->stream_count == 0) {
		regmap_write(priv->regmap, 0x0010, 0x21);	/* SW reset */
		msleep(100);
		ret = max9295a_set_regs(priv, configuretable_ar0231,
					ARRAY_SIZE(configuretable_ar0231));
		if (ret) {
			dev_err(dev, "Failed to max9295a set register\n");
			return -EINVAL;
		}
	}

	priv->stream_count += enable ? 1 : -1;
	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdev ops
 */
static int max9295a_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index > 0)
		return -EINVAL;

	code->code = CAMERA_FORMAT;

	return 0;
}

static struct v4l2_mbus_framefmt *
max9295a_get_pad_format(struct max9295a_priv *priv,
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

static int max9295a_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;

	if (format->pad >= MAX9295A_N_PADS)
		return -EINVAL;

	fmt->width        = CAMERA_WIDTH;
	fmt->height       = CAMERA_HEIGHT;
	fmt->code         = CAMERA_FORMAT; 
	fmt->colorspace   = V4L2_COLORSPACE_SRGB;
	fmt->field        = V4L2_FIELD_NONE;
	fmt->ycbcr_enc    = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func    = V4L2_XFER_FUNC_DEFAULT;

	return 0;
}

static int max9295a_set_fmt(struct v4l2_subdev *sd,
                            struct v4l2_subdev_pad_config *sd_state,
                            struct v4l2_subdev_format *format)
{
	struct max9295a_priv *priv = sd_to_max9295a(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (format->pad >= MAX9295A_N_PADS)
		return -EINVAL;
	
	/* Refuse non YUV422 formats as we hardcode DT to 8 bit YUV422 */
	switch (format->format.code) {
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
		break;
	default:
		format->format.code = MEDIA_BUS_FMT_YUYV10_2X10;
		break;
	}
	format->format.width  = CAMERA_WIDTH;
	format->format.height = CAMERA_HEIGHT;
	
	fmt = max9295a_get_pad_format(priv, sd_state, format->pad, format->which);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	return 0;
}

static struct v4l2_subdev_video_ops max9295a_video_ops = {
	.s_stream	= max9295a_s_stream,
};

static const struct v4l2_subdev_pad_ops max9295a_subdev_pad_ops = {
	.enum_mbus_code = max9295a_enum_mbus_code,
	.get_fmt    = max9295a_get_fmt,
	.set_fmt    = max9295a_set_fmt,
};

static struct v4l2_subdev_ops max9295a_subdev_ops = {
	.video      = &max9295a_video_ops,
	.pad        = &max9295a_subdev_pad_ops,
};

static void max9295a_init_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width        = CAMERA_WIDTH;
	fmt->height       = CAMERA_HEIGHT;
	fmt->code         = CAMERA_FORMAT; 
	fmt->colorspace   = V4L2_COLORSPACE_SRGB;
	fmt->field        = V4L2_FIELD_NONE;
	fmt->ycbcr_enc    = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func    = V4L2_XFER_FUNC_DEFAULT;
}

static int max9295a_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *fmt;
	
	fmt = v4l2_subdev_get_try_format(sd, fh->pad, MAX9295A_SRC_PAD);
	max9295a_init_format(fmt);
	
	return 0;
}

static const struct v4l2_subdev_internal_ops max9295a_subdev_internal_ops = {
	.open = max9295a_open,
};

static int max9295a_v4l2_subdev_init(struct max9295a_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct i2c_client *client = priv->client;
	unsigned int i, pixel_rate;
	int ret;

	for (i = 0; i < MAX9295A_N_PADS; i++)
		max9295a_init_format(&priv->fmt[i]);

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	v4l2_i2c_subdev_init(&priv->sd, client, &max9295a_subdev_ops);
	priv->sd.internal_ops = &max9295a_subdev_internal_ops;
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	/* Optional Ctrl Handler */
	v4l2_ctrl_handler_init(&priv->ctrls, 1);

	/* 1920x1020@30Hz, RAW10 bpp = 10 */
	pixel_rate = (TOTAL_WIDTH(CAMERA_WIDTH) * TOTAL_HEIGHT(CAMERA_HEIGHT)
		      * CAMERA_FPS);
	dev_dbg(dev, "MAX9295A calculated pixel rate: %u Hz\n", pixel_rate);
	v4l2_ctrl_new_std(&priv->ctrls, NULL, V4L2_CID_PIXEL_RATE,
			  1, INT_MAX, 1, pixel_rate);
	priv->sd.ctrl_handler = &priv->ctrls;

	ret = priv->ctrls.error;
	if (ret)
		return ret;
	
	priv->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	priv->pads[MAX9295A_SRC_PAD].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&priv->sd.entity, MAX9295A_N_PADS,
				     priv->pads);
	if (ret)
		return ret;

	priv->sd.fwnode = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);

	/* Register as a V4L2 subdevice for async notifier */
	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret) {
		dev_err(dev, "Failed to register subdev\n");
		return ret;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Probe/Remove
 */
static int max9295a_probe(struct i2c_client *client)
{
	struct max9295a_priv *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->stream_count = 0;

	priv->regmap = devm_regmap_init_i2c(client, &max9295_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(priv->regmap),
			       	     "Failed to init i2c regmap\n");

	i2c_set_clientdata(client, priv);

	ret = max9295a_v4l2_subdev_init(priv);
	if (ret < 0)
		return ret;

	max9295a_initialize(priv);
	return 0;
}

static int max9295a_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max9295a_priv *priv = sd_to_max9295a(sd);

	fwnode_handle_put(priv->sd.fwnode);
	v4l2_async_unregister_subdev(&priv->sd);
	regmap_exit(priv->regmap);
	return 0;
}

static const struct of_device_id max9295a_of_ids[] = {
	{.compatible = "maxim,max9295a"},
	{}
};
MODULE_DEVICE_TABLE(of, max9295a_ids);

static const struct i2c_device_id max9295a_ids[] = {
	{"max9295a", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, max96712_id);

static struct i2c_driver max9295a_i2c_driver = {
	.driver	= {
		.name	= "max9295a",
		.of_match_table = max9295a_of_ids,
	},
	.probe_new	= max9295a_probe,
	.remove		= max9295a_remove,
	.id_table	= max9295a_ids,
};
module_i2c_driver(max9295a_i2c_driver);

MODULE_ALIAS("MAX9295A");
MODULE_DESCRIPTION("GMSL2 MAX9295A driver");
MODULE_LICENSE("GPL");
