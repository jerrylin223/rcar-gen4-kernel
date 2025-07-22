/*
 * A2B24XX driver
 *
 * Copyright 2019 Analog Devices Inc.
 *  Author: ADI Automotive Software Team, Bangalore
 *
 * Licensed under the GPL-2.
 */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>

#include <linux/mod_devicetable.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define A2B_SETUP_ALSA

#ifdef A2B_SETUP_ALSA
//#include "adi_a2b_commandlist.h"
//#include "adi_a2b_commandlist_slave_0123.h"
//#include "adi_a2b_commandlist_0522_bus_power.h"
#include "adi_a2b_commandlist_local_local_bus_0526.h"

#endif
#include "a2b24xx.h"

struct a2b24xx
{
	struct regmap *regmap;
	unsigned int sysclk;
	enum a2b24xx_sysclk_src sysclk_src;
	enum a2b24xx_type type;

	struct snd_pcm_hw_constraint_list constraints;

	struct device *dev;
	void (*switch_mode)(struct device *dev);

	unsigned int max_master_fs;
	bool master;
};
/*retronix add */
static int enble ,custom_mode;
/*retronix add */
static const struct reg_default a2b24xx_reg_defaults[] =
		{{0x00, 0x50} };

/* An example control -  no specific functionality */
static const DECLARE_TLV_DB_MINMAX_MUTE(a2b24xx_control_vol, -4200, 0);

#define A2B24XX_CONTROL(x) \
	SOC_SINGLE_TLV("A2B" #x "Template", \
		2, \
		0, 255, 1, a2b24xx_control_vol)

/* example control */
static const struct snd_kcontrol_new a2b24xx_snd_controls[] = {A2B24XX_CONTROL(1), };

static int __maybe_unused a2b24xx_reset(struct a2b24xx *a2b24xx)
{
	int ret = 0;

	regcache_cache_bypass(a2b24xx->regmap, true);
    /* A2B reset */
	return ret;
}
#ifdef A2B_SETUP_ALSA
/****************************************************************************/
/*!
 @brief          This function calculates reg value based on width and adds
 it to the data array

 @param [in]     pDstBuf               Pointer to destination array
 @param [in]     nAddrwidth            Data unpacking boundary(1 byte / 2 byte /4 byte )
 @param [in]     nAddr            	  Number of words to be copied

 @return          Return code
 - 0: Success
 - 1: Failure
 */
/********************************************************************************/
static void adi_a2b_Concat_Addr_Data(unsigned char pDstBuf[], unsigned int nAddrwidth, unsigned int nAddr)
{
	/* Store the read values in the place holder */
	switch (nAddrwidth)
	{ /* Byte */
		case 1u:
			pDstBuf[0u] = (unsigned char)nAddr;
			break;
			/* 16 bit word*/
		case 2u:

			pDstBuf[0u] = (unsigned char)(nAddr >> 8u);
			pDstBuf[1u] = (unsigned char)(nAddr & 0xFFu);

			break;
			/* 24 bit word */
		case 3u:
			pDstBuf[0u] = (unsigned char)((nAddr & 0xFF0000u) >> 16u);
			pDstBuf[1u] = (unsigned char)((nAddr & 0xFF00u) >> 8u);
			pDstBuf[2u] = (unsigned char)(nAddr & 0xFFu);
			break;

			/* 32 bit word */
		case 4u:
			pDstBuf[0u] = (unsigned char)(nAddr >> 24u);
			pDstBuf[1u] = (unsigned char)((nAddr & 0xFF0000u) >> 16u);
			pDstBuf[2u] = (unsigned char)((nAddr & 0xFF00u) >> 8u);
			pDstBuf[3u] = (unsigned char)(nAddr & 0xFFu);
			break;

		default:
			break;

	}
}

static int adi_a2b_I2CWrite(struct device *dev, unsigned short devAddr, unsigned short count, char *bytes)
{
	struct i2c_client *client = to_i2c_client(dev);
	client->addr = devAddr;
	return (i2c_master_send(client, bytes, count));
}

/****************************************************************************/
/*!
 @brief          This function does A2B network discovery
 and the peripheral configuration
 @return          None

 */
/********************************************************************************/
static void adi_a2b_NetworkSetup(struct device *dev)
{
	ADI_A2B_DISCOVERY_CONFIG* pOPUnit;
	unsigned int nIndex, nIndex1;
	//unsigned int status;
	/* Maximum number of writes */
	static unsigned char aDataBuffer[6000];
	static unsigned char aDataWriteReadBuf[4u];
	unsigned int nDelayVal ,config_line;
	config_line = CONFIG_LEN;
	/* Loop over all the configuration */
	if(enble <2)
	{
		for (nIndex = 0; nIndex < config_line; nIndex++)
		{
			//pOPUnit = (custom_mode == 1) ? &gaA2BConfig_test[nIndex] : &gaA2BConfig[nIndex];
			pOPUnit = &gaA2BConfig[nIndex];
			/*Operation code*/
			switch (pOPUnit->eOpCode)
			{
				/* Write */
				case WRITE:
					adi_a2b_Concat_Addr_Data(&aDataBuffer[0u], pOPUnit->nAddrWidth, pOPUnit->nAddr);
					(void)memcpy(&aDataBuffer[pOPUnit->nAddrWidth], pOPUnit->paConfigData, pOPUnit->nDataCount);
					//printk("local local bus Operation number \n %d", nIndex);
					/* PAL Call, replace with custom implementation  */
					adi_a2b_I2CWrite(dev, pOPUnit->nDeviceAddr, (pOPUnit->nAddrWidth + pOPUnit->nDataCount), &aDataBuffer[0u]);
					break;
					/* Read */
				case READ:
					(void)memset(&aDataBuffer[0u], 0u, pOPUnit->nDataCount);
					adi_a2b_Concat_Addr_Data(&aDataWriteReadBuf[0u], pOPUnit->nAddrWidth, pOPUnit->nAddr);
					/* Couple of milli seconds should be OK */
					mdelay(2);
					break;
					/* Delay */
				case DELAY:
					nDelayVal = 0u;
					for (nIndex1 = 0u; nIndex1 < pOPUnit->nDataCount; nIndex1++)
					{
						nDelayVal = pOPUnit->paConfigData[nIndex1] | nDelayVal << 8u;
					}
					//printk("local local bus \n %d", nIndex);
					mdelay(nDelayVal);
					break;
				default:
					break;
				}
		}
	enble++;
	}
}
#endif
/* Template functions */

static int a2b24xx_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	int ret = 0;

	
	return ret;
}


static int a2b24xx_set_tdm_slot(struct snd_soc_dai *dai, unsigned int tx_mask, unsigned int rx_mask, int slots, int width)
{
	// Add custom functionality

	return 0;
}

static int a2b24xx_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct snd_soc_component *codec = dai->component;
	struct a2b24xx *a2b24xx = snd_soc_component_get_drvdata(codec);
	//struct a2b24xx *a2b24xx = snd_soc_component_get_drvdata(dai->component);
	adi_a2b_NetworkSetup(codec->dev);
	// Add custom functionality 

	return 0;
}

static int a2b24xx_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct a2b24xx *a2b24xx = snd_soc_component_get_drvdata(dai->component);
	int ret = 0;

	return ret;

}

static int a2b24xx_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct a2b24xx *a2b24xx = snd_soc_component_get_drvdata(dai->component);
	// Add custom functionality 
	return 0;
}
static const struct snd_soc_dai_ops a2b24xx_dai_ops =
		{
		 .startup = a2b24xx_startup,
		 .hw_params = a2b24xx_hw_params,
		 .mute_stream = a2b24xx_mute,
		 .set_fmt = a2b24xx_set_dai_fmt,
		 .set_tdm_slot = a2b24xx_set_tdm_slot,
		};

static struct snd_soc_dai_driver a2b24xx_dai =
	{
		.name = "a2b24xx-hifi",
		
		.capture =
		{
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 32,
		.rates = SNDRV_PCM_RATE_KNOT,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,
		.sig_bits = 24, },
		.playback =
			{
			 .stream_name = "Playback",
			 .channels_min = 1,
			 .channels_max = 32,
			 .rates = SNDRV_PCM_RATE_KNOT,
			 .formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE, .sig_bits = 24,
			},
			.ops = &a2b24xx_dai_ops,
	};

// Supported rates 
static const unsigned int a2b24xx_rates[] =
 {1500, 2000,2400, 3000, 8000, 12000, 24000, 48000,  };

// Check system clock 
static bool a2b24xx_check_sysclk(unsigned int mclk, unsigned int base_freq)
{
	unsigned int mcs;

	return true;
}
// set system clock
static int a2b24xx_set_sysclk(struct snd_soc_component *codec, int clk_id, int source, unsigned int freq, int dir)
{
	struct a2b24xx *a2b24xx = snd_soc_component_get_drvdata(codec);
	unsigned int mask = 0;
	unsigned int clk_src;
	unsigned int ret = 0;

	// No functionality 

	return 0;
}
// Codec probe 
static int a2b24xx_codec_probe(struct snd_soc_component *codec)
{
	struct a2b24xx *a2b24xx = snd_soc_component_get_drvdata(codec);
	int ret = 0;

#ifdef A2B_SETUP_ALSA
	// Setting up A2B network
	//adi_a2b_NetworkSetup(codec->dev);
#endif

	return ret;
}

static struct snd_soc_component_driver a2b24xx_codec_driver =
{
  .probe = a2b24xx_codec_probe,
  .set_sysclk = a2b24xx_set_sysclk,
  .controls = a2b24xx_snd_controls,
  .num_controls = ARRAY_SIZE(a2b24xx_snd_controls),

};

static int device_status = 0; // 0: 停止, 1: 運行
static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "Device is %s\n", device_status ? "active" : "inactive");
}

static ssize_t status_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	switch (buf[0]) {
        case '1':
			custom_mode = 1;
			device_status = 1;
			enble = 0;
            break;
        case '0':
            device_status = 0;
			custom_mode = 0;
            adi_a2b_NetworkSetup(dev);
            break;
        default:
            break;
    }
    return count;
}
DEVICE_ATTR(status, 0644, status_show, status_store);

/* driver probe */

int a2b24xx_probe(struct device *dev, struct regmap *regmap, enum a2b24xx_type type, void (*switch_mode)(struct device *dev))
{
	struct a2b24xx *a2b24xx;
	int ret;

	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	a2b24xx = devm_kzalloc(dev, sizeof(*a2b24xx), GFP_KERNEL);
	if (a2b24xx == NULL)
		return -ENOMEM;

	a2b24xx->dev = dev;
	a2b24xx->type = type;
	a2b24xx->regmap = regmap;
	a2b24xx->switch_mode = switch_mode;
	a2b24xx->max_master_fs = 48000;

	a2b24xx->constraints.list = a2b24xx_rates;
	a2b24xx->constraints.count = ARRAY_SIZE(a2b24xx_rates);

	dev_set_drvdata(dev, a2b24xx);
	custom_mode = 0;
	//adi_a2b_NetworkSetup(a2b24xx->dev);
	
	return snd_soc_register_component(dev, &a2b24xx_codec_driver, &a2b24xx_dai, 1);
	//return 0;

}
//EXPORT_SYMBOL_GPL(a2b24xx_probe);

static bool a2b24xx_register_volatile(struct device *dev, unsigned int reg)
{
	return true;
}

const struct regmap_config a2b24xx_regmap_config =
{
 .max_register = 255,
 .volatile_reg = a2b24xx_register_volatile,
 .cache_type = REGCACHE_NONE,
 .reg_defaults = a2b24xx_reg_defaults,
 .num_reg_defaults = ARRAY_SIZE(a2b24xx_reg_defaults),
};
EXPORT_SYMBOL_GPL(a2b24xx_regmap_config);
/*i2c contorl*/
static int a2b24xx_i2c_probe(struct i2c_client *client,
    const struct i2c_device_id *id)
{
    struct regmap_config config;
    config = a2b24xx_regmap_config;
    config.val_bits = 8;
    config.reg_bits = 8;
	device_create_file(&client->dev, &dev_attr_status);
    return a2b24xx_probe(&client->dev,
        devm_regmap_init_i2c(client, &config),
        id->driver_data, NULL);
}

static int a2b24xx_i2c_remove(struct i2c_client *client)
{
	device_remove_file(&client->dev, &dev_attr_status);
    snd_soc_unregister_component(&client->dev);
    return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id a2b24xx_dt_ids[] = { 
    { .compatible = "adi,a2b24xx", },
    { } 
};
MODULE_DEVICE_TABLE(of, a2b24xx_dt_ids);
#endif

static const struct i2c_device_id a2b24xx_i2c_ids[] = { 
    { "a2b24xx", A2B24XX },
    { } 
};
MODULE_DEVICE_TABLE(i2c, a2b24xx_i2c_ids);

static struct i2c_driver a2b24xx_i2c_driver = { 
    .driver = { 
        .name = "a2b24xx",
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(a2b24xx_dt_ids),
    },  
    .probe = a2b24xx_i2c_probe,
    .remove = a2b24xx_i2c_remove,
    .id_table = a2b24xx_i2c_ids,
};
module_i2c_driver(a2b24xx_i2c_driver);

MODULE_DESCRIPTION("ASoC A2B24XX driver");
MODULE_AUTHOR("ADI Automotive Software Team, Bangalore");
MODULE_LICENSE("GPL");
