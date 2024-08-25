/*
 * imx-alc5640.c
 *
 * Copyright (C) 2012-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#define DEBUG

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/fsl_devices.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/switch.h>
#include <linux/kthread.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/jack.h>
#include <mach/dma.h>
#include <mach/clock.h>
#include <mach/audmux.h>
#include <mach/gpio.h>
#include <asm/mach-types.h>

#include "imx-ssi.h"
#include "../codecs/rt5645.h"

#define IMX_INTR_DEBOUNCE               150
#define IMX_HS_INSERT_DET_DELAY         500
#define IMX_HS_REMOVE_DET_DELAY         500
#define IMX_BUTTON_DET_DELAY            100
#define IMX_HS_DET_POLL_INTRVL          100
#define IMX_BUTTON_EN_DELAY             1500
#define IMX_HS_DET_RETRY_COUNT          6

struct imx_priv {
	int sysclk;         /*mclk from the outside*/
	int codec_sysclk;
	int dai_aif1;
	int hp_irq;
	int hp_status;
	struct platform_device *pdev;
#ifdef CONFIG_ANDROID	
	struct switch_dev sdev;
#endif	
	struct snd_pcm_substream *first_stream;
	struct snd_pcm_substream *second_stream;
	int (*amp_enable) (int enable);

	// imx machine jack/btn private data
	struct snd_soc_jack jack;
	struct delayed_work hs_insert_work;
	struct delayed_work hs_remove_work;
	struct delayed_work hs_button_work;
	struct mutex jack_mlock;
	int jack_status;
	/* To enable button press interrupts after a delay after HS detection.
	   This is to avoid spurious button press events during slow HS insertion */
	struct delayed_work hs_button_en_work;
	int intr_debounce;
	int hs_insert_det_delay;
	int hs_remove_det_delay;
	int button_det_delay;
	int button_en_delay;
	int hs_det_poll_intrvl;
	int hs_det_retry;
	bool process_button_events;
};

static unsigned int sample_format = SNDRV_PCM_FMTBIT_S16_LE;
static struct imx_priv card_priv;
static struct snd_soc_card snd_soc_card_imx;
// static struct snd_soc_codec *gcodec;

static int imx_hs_detection(void);
static struct snd_soc_jack_gpio hs_gpio = {
	.name           = "imx-codec-int", 
	.report         = SND_JACK_HEADSET | SND_JACK_HEADPHONE |
		SND_JACK_BTN_0 | SND_JACK_BTN_1 |
		SND_JACK_BTN_2,
	.debounce_time      = IMX_INTR_DEBOUNCE,                                                                   
	.jack_status_check  = imx_hs_detection, 
};

/* Work function invoked by the Jack Infrastructure. Other delayed works                                           
   for jack detection/removal/button press are scheduled from this function */                                     
static int imx_hs_detection(void)
{                                                                                                                  
	struct snd_soc_jack_gpio *gpio = &hs_gpio;                                                                     
	struct snd_soc_jack *jack = gpio->jack;                                                                        
	struct snd_soc_codec *codec = jack->codec;                                                                     
	int status, jack_type = 0, ret = 0;
	struct imx_priv *ctx = container_of(jack, struct imx_priv, jack);                                  

	mutex_lock(&ctx->jack_mlock);                                                                                  
	pr_debug("Enter:%s, jack->status=0x%x, ctx->jack_status=0x%x\n", __func__, jack->status, ctx->jack_status);
	jack_type = ctx->jack_status;
	status = rt5645_check_jd_status(codec);                                                                        
	if (!status) {
		pr_debug("%s: Jack insert intr\n", __func__);
	} else { 
		pr_debug("%s: Jack out\n", __func__);
		jack_type = rt5645_headset_detect(codec, false);
		jack_type = 0;
	}
	
	if (!ctx->jack_status && !status) {
		ctx->hs_det_retry = IMX_HS_DET_RETRY_COUNT;
		ret = schedule_delayed_work(&ctx->hs_insert_work,
				msecs_to_jiffies(ctx->hs_insert_det_delay));
		if (!ret)     
			pr_debug("%s: imx_check_hs_insert_status already queued\n", __func__);
		else
			pr_debug("%s: Check hs insertion  after %d msec\n",
					__func__, ctx->hs_insert_det_delay); 
	} else {
		/* First check for accessory removal; If not removed,
		   check for button events*/
		status = rt5645_check_jd_status(codec);
		/* jd status high indicates accessory has been disconnected.
		   However, confirm the removal in the delayed work */ 
		if (status) {
			/* Do not process button events while we make sure
			   accessory is disconnected*/
			ctx->process_button_events = false;
			ret = schedule_delayed_work(&ctx->hs_remove_work,
					msecs_to_jiffies(ctx->hs_remove_det_delay));
			if (!ret)
				pr_debug("%s: imx_check_hs_remove_status already queued\n", __func__);
			else  
				pr_debug("%s: Check hs removal after %d msec\n",
						__func__, ctx->hs_remove_det_delay);
		} else { /* Must be button event. Confirm the event in delayed work*/
			if (((ctx->jack_status & SND_JACK_HEADSET) == SND_JACK_HEADSET) &&
					ctx->process_button_events) {
				ret = schedule_delayed_work(&ctx->hs_button_work,
						msecs_to_jiffies(ctx->button_det_delay));
				if (!ret)
					pr_debug("%s: imx_check_hs_button_status already queued\n", __func__);
				else
					pr_debug("%s: Check BP/BR after %d msec\n",
							__func__, ctx->button_det_delay);
			}
		}
	}

	pr_debug("Exit:%s, jack->status=0x%x, ctx->jack_status=0x%x\n", __func__, jack->status, ctx->jack_status);
	mutex_unlock(&ctx->jack_mlock);
	return jack_type;
}

/* Identify the jack type as Headset/Headphone/None */
static int imx_check_jack_type(void)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	int status, jack_type = 0;
	struct imx_priv *ctx = container_of(jack, struct imx_priv, jack);

	status = rt5645_check_jd_status(codec);
	/* jd status low indicates some accessory has been connected */
	if (!status) {
		pr_debug("%s: Jack insert intr\n", __func__);
		/* Do not process button events until accessory is detected as headset*/
		ctx->process_button_events = false;

		status = rt5645_headset_detect(codec, true);
		if (status == SND_JACK_HEADPHONE)
			jack_type = SND_JACK_HEADPHONE;
		else if (status == SND_JACK_HEADSET) {
			jack_type = SND_JACK_HEADSET;
			ctx->process_button_events = true;
			/* If headset is detected, enable button interrupts after a delay */
			schedule_delayed_work(&ctx->hs_button_en_work,
					msecs_to_jiffies(ctx->button_en_delay));
		} else /* RT5645_NO_JACK */
			jack_type = 0;
	} else
		jack_type = 0;

	pr_debug("%s: Jack type detected:0x%x\n", __func__, jack_type);

	return jack_type;
}

/*Checks jack insertion and identifies the jack type.
  Retries the detection if necessary */
static void imx_check_hs_insert_status(struct work_struct *work)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct imx_priv *ctx = container_of(work, struct imx_priv, hs_insert_work.work);
	int jack_type = 0, state = 0;

	mutex_lock(&ctx->jack_mlock);
	pr_debug("Enter:%s, jack->status=0x%x, ctx->jack_status=0x%x\n", __func__, jack->status, ctx->jack_status);

	jack_type = imx_check_jack_type();

	/* Report jack immediately only if jack is headset. If headphone or no jack was detected,
	   dont report it until the last HS det try. This is to avoid reporting any temporary
	   jack removal or accessory change(eg, HP to HS) during the detection tries.
	   This provides additional debounce that will help in the case of slow insertion.
	   This also avoids the pause in audio due to accessory change from HP to HS */
	if (ctx->hs_det_retry <= 0) { /* end of retries; report the status */
		snd_soc_jack_report(jack, jack_type, gpio->report);
		ctx->jack_status = jack_type;
#ifdef CONFIG_ANDROID
		// state: 0->NoJack, 1->HS, 2->HP
		switch (jack_type) {
		case SND_JACK_HEADSET:
			state = 1;
			break;
		case SND_JACK_HEADPHONE:
			state = 2;
			break;
		default:
			state = 0;
			pr_debug("%s: No detected", __func__);
			break;	
		}
		switch_set_state(&ctx->sdev, state);
#endif
	} else {
		/* Schedule another detection try if headphone or no jack is detected.
		   During slow insertion of headset, first a headphone may be detected.
		   Hence retry until headset is detected */
		if (jack_type == SND_JACK_HEADSET) {
			ctx->hs_det_retry = 0; /* HS detected, no more retries needed */
			snd_soc_jack_report(jack, jack_type, gpio->report);
			ctx->jack_status = jack_type;
#ifdef CONFIG_ANDROID
			switch_set_state(&ctx->sdev, 1);
#endif
		} else {
			ctx->hs_det_retry--;
			schedule_delayed_work(&ctx->hs_insert_work,
					msecs_to_jiffies(ctx->hs_det_poll_intrvl));
			pr_debug("%s: Re-try hs detection after %d msec\n",
					__func__, ctx->hs_det_poll_intrvl);
		}
	}

	pr_debug("Exit:%s, jack->status=0x%x, ctx->jack_status=0x%x\n", __func__, jack->status, ctx->jack_status);
	mutex_unlock(&ctx->jack_mlock);
}               

/* Checks jack removal. */
static void imx_check_hs_remove_status(struct work_struct *work)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	struct imx_priv *ctx = container_of(work, struct imx_priv, hs_remove_work.work);
	int status = 0, jack_type = 0;

	/* Cancel any pending insertion detection. There could be pending insertion detection in the
	   case of very slow insertion or insertion and immediate removal.*/
	cancel_delayed_work_sync(&ctx->hs_insert_work);

	mutex_lock(&ctx->jack_mlock);
	pr_debug("Enter:%s, jack->status=0x%x, ctx->jack_status=0x%x\n", __func__, jack->status, ctx->jack_status);
	/* Initialize jack_type with previous status.
	   If the event was an invalid one, we return the preious state*/
	jack_type = ctx->jack_status;

	if (ctx->jack_status) { /* jack is in connected state; look for removal event */     
		status = rt5645_check_jd_status(codec);
		if (status) { /* jd status high implies accessory disconnected */
			pr_debug("%s: Jack remove event\n", __func__);
			ctx->process_button_events = false;
			cancel_delayed_work_sync(&ctx->hs_button_en_work);
			status = rt5645_headset_detect(codec, false);
			jack_type = 0;
			ctx->jack_status = 0;
#ifdef CONFIG_ANDROID
			// state: 0->NoJack, 1->HS, 2->HP
			switch_set_state(&ctx->sdev, 0);
#endif
		} else if (((ctx->jack_status & SND_JACK_HEADSET) == SND_JACK_HEADSET) && !ctx->process_button_events) {
			/* Jack is still connected. We may come here if there was a spurious
			   jack removal event. No state change is done until removal is confirmed
			   by the check_jd_status above.i.e. jack status remains Headset or headphone.
			   But as soon as the interrupt thread(imx_hs_detection) detected a jack
			   removal, button processing gets disabled. Hence re-enable button processing
			   in the case of headset */
			pr_debug("%s: Spurious Jack remove event for headset; re-enable button events\n", __func__);
			ctx->process_button_events = true;
		}
	}

	snd_soc_jack_report(jack, jack_type, gpio->report);
	pr_debug("Exit:%s, jack->status=0x%x, ctx->jack_status=0x%x\n", __func__, jack->status, ctx->jack_status);
	mutex_unlock(&ctx->jack_mlock);
}

/* Check for button press/release */
static void imx_check_hs_button_status(struct work_struct *work)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	struct imx_priv *ctx = container_of(work, struct imx_priv, hs_button_work.work);
	int status = 0, jack_type = 0;
	int ret;

	mutex_lock(&ctx->jack_mlock);
	pr_debug("Enter:%s, jack->status=0x%x, ctx->jack_status=0x%x\n", __func__, jack->status, ctx->jack_status);
	/* Initialize jack_type with previous status.
	   If the event was an invalid one, we return the preious state*/
	jack_type = ctx->jack_status;

	if (((ctx->jack_status & SND_JACK_HEADSET) == SND_JACK_HEADSET)
			&& ctx->process_button_events) {

		status = rt5645_check_jd_status(codec);
		pr_debug("%s: rt5645_check_jd_status()=0x%x\n", __func__, status);
		if (!status) { /* confirm jack is connected */

			status = rt5645_button_detect(codec);
			pr_debug("%s: rt5645_button_detect()=0x%x\n", __func__, status);
			if (ctx->jack_status & (SND_JACK_BTN_0 | SND_JACK_BTN_1  
						| SND_JACK_BTN_2)) { /* if button was previosly in pressed state*/
				if (!status) {
					pr_debug("%s: BR event received\n", __func__);
					jack_type = SND_JACK_HEADSET;
				}
			} else { /* If button was previously in released state */
				// if (status) {
				// 	pr_debug("%s: BP event received\n", __func__);
				// }
				switch (status) {
					case 0x8000:
					case 0x4000:
					case 0x2000:
						pr_debug("%s: RT5645_BTN_UP BP event received\n", __func__);
						jack_type = SND_JACK_HEADSET | SND_JACK_BTN_0;
						break;
					case 0x1000:
					case 0x0800:
					case 0x0400:	
						pr_debug("%s: RT5645_BTN_CENTER BP event received\n", __func__);
						jack_type = SND_JACK_HEADSET | SND_JACK_BTN_1;
						break;
					case 0x0200:
					case 0x0100:
					case 0x0080:	
						pr_debug("%s: RT5645_BTN_DOWN BP event received\n", __func__);
						jack_type = SND_JACK_HEADSET | SND_JACK_BTN_2;
						break;
					default:
						pr_debug("%s: RT5645_BTN_UNKNOWN BP event received\n", __func__);
						jack_type = SND_JACK_HEADSET;
						break;
				}
			}
			ctx->jack_status = jack_type;
		}
		/* There could be button interrupts during jack removal. There can be
		   situations where a button interrupt is generated first but no jack
		   removal interrupt is generated. This can happen on platforrms where
		   jack detection is aligned to Headset Left pin instead of the ground
		   pin and codec multiplexes (ORs) the jack and button interrupts.
		   So schedule a jack removal detection work */
		ret = schedule_delayed_work(&ctx->hs_remove_work,
				msecs_to_jiffies(ctx->hs_remove_det_delay));
		if (!ret)
			pr_debug("%s: imx_check_hs_remove_status already queued\n", __func__);
		else
			pr_debug("%s: Check hs removal after %d msec\n",
					__func__, ctx->hs_remove_det_delay);

	}

	snd_soc_jack_report(jack, jack_type, gpio->report);
	pr_debug("Exit:%s, jack->status=0x%x, ctx->jack_status=0x%x\n", __func__, jack->status, ctx->jack_status);
	mutex_unlock(&ctx->jack_mlock);
}

/* Delayed work for enabling the overcurrent detection circuit and interrupt
   for generating button events */
static void imx_enable_hs_button_events(struct work_struct *work)
{
	//struct snd_soc_jack_gpio *gpio = &hs_gpio;
	//struct snd_soc_jack *jack = gpio->jack;
	//struct snd_soc_codec *codec = jack->codec;

	//rt5645_enable_ovcd_interrupt(codec, true);
}                       

static int imx_aif1_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct imx_priv *priv = &card_priv;
	struct mxc_audio_platform_data *plat = priv->pdev->dev.platform_data;

	if (!codec_dai->active && plat->clock_enable)
		plat->clock_enable(1);

	if (card_priv.amp_enable) 
		card_priv.amp_enable(1);

	return 0;
}

static void imx_aif1_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct imx_priv *priv = &card_priv;
	struct mxc_audio_platform_data *plat = priv->pdev->dev.platform_data;

	snd_soc_dai_digital_mute(codec_dai, 1);
	if (!codec_dai->active && plat->clock_enable)
		plat->clock_enable(0);

	return;
}

static int imx_aif1_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct imx_priv *priv = &card_priv;
	unsigned int channels = params_channels(params);
	unsigned int sample_rate = 44100;
	int ret = 0;
	u32 dai_format;
	unsigned int pll_out;
	unsigned int div_pm;

	if (!priv->first_stream)
		priv->first_stream = substream;
	else
		priv->second_stream = substream;

	dai_format = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS;

	/* set codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, dai_format);
	if (ret < 0)
		return ret;

	/* set i.MX active slot mask */
	snd_soc_dai_set_tdm_slot(cpu_dai,
			channels == 1 ? 0xfffffffe : 0xfffffffc,
			channels == 1 ? 0xfffffffe : 0xfffffffc,
			2, 32);

	dai_format = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_IF |
		SND_SOC_DAIFMT_CBS_CFS;

	/* set cpu DAI configuration */
	ret = snd_soc_dai_set_fmt(cpu_dai, dai_format);
	if (ret < 0)
		return ret;

	sample_rate = params_rate(params);
	sample_format = params_format(params);

	// if (sample_format == SNDRV_PCM_FORMAT_S24_LE)
	// 	pll_out = sample_rate * 192;
	// else
	// 	pll_out = sample_rate * 256 * 2;

	pll_out = 22579200;
	ret = snd_soc_dai_set_pll(codec_dai, 0,
			RT5645_PLL1_S_MCLK, 3528000, pll_out);
	if (ret < 0)
		pr_err("Failed to start PLL: %d\n", ret);
	ret = snd_soc_dai_set_sysclk(codec_dai, RT5645_SCLK_S_PLL1, pll_out, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		pr_err("Failed to set SYSCLK: %d\n", ret);
		return ret;
	}

	/* set the SSI system clock as input (unused) */
	snd_soc_dai_set_sysclk(cpu_dai, IMX_SSP_SYS_CLK, 0, SND_SOC_CLOCK_IN);

	// f_BIT_CLK  = f_SSI's sys clock  / [(DIV2 + 1) x (7 x PSR + 1) x (PM + 1) x2]
	// ssi's sys clock = pll4 / 8
	div_pm = ((priv->sysclk>>10)+(sample_rate>>1))/sample_rate;     // PM = rounded (ssi_sys_clk/128)

	snd_soc_dai_set_clkdiv(cpu_dai, IMX_SSI_TX_DIV_PM, (div_pm-1)); 
	snd_soc_dai_set_clkdiv(cpu_dai, IMX_SSI_TX_DIV_2, 0);
	snd_soc_dai_set_clkdiv(cpu_dai, IMX_SSI_TX_DIV_PSR, 0);

	return 0;
}

static const struct snd_soc_dapm_widget alc5645_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	//SND_SOC_DAPM_SPK("Speaker", NULL),
};

static const struct snd_soc_dapm_route alc5645_route[] = {
	{ "Headphone Jack",     NULL,   "HPOL" },
	{ "Headphone Jack",     NULL,   "HPOR" },
#if 0
	{"Speaker",             NULL,   "SPOLP"},
	{"Speaker",             NULL,   "SPOLN"},
	{"Speaker",             NULL,   "SPORP"},
	{"Speaker",             NULL,   "SPORN"},
#endif
};

static int imx_alc5645_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct imx_priv *ctx = &card_priv; 
	int ret;

	ctx->jack_status = 0;

	ret = snd_soc_jack_new(codec, "Imx Audio Jack",
			SND_JACK_HEADSET | SND_JACK_HEADPHONE | SND_JACK_BTN_0
			| SND_JACK_BTN_1 | SND_JACK_BTN_2,
			&ctx->jack);
	if (ret) {
		pr_err("jack creation failed\n");
		return ret;
	}

	ret = snd_soc_jack_add_gpios(&ctx->jack, 1, &hs_gpio);
	if (ret) {
		pr_err("adding jack GPIO failed\n");
		return ret;
	}

	snd_soc_dapm_new_controls(dapm, alc5645_dapm_widgets,
			ARRAY_SIZE(alc5645_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, alc5645_route, ARRAY_SIZE(alc5645_route));

	snd_soc_dapm_enable_pin(dapm, "Headphone Jack");
	//      snd_soc_dapm_enable_pin(dapm, "Speaker");

	snd_soc_dapm_sync(dapm);

	if (card_priv.amp_enable)
		card_priv.amp_enable(1);	

	ret = imx_hs_detection();
	pr_debug("%s: ret=%d", __func__, ret);

	return 0;
}

static struct snd_soc_ops imx_aif1_ops = {
	.hw_params = imx_aif1_hw_params,
	.startup = imx_aif1_startup,
	.shutdown = imx_aif1_shutdown,
};

static struct snd_soc_dai_link imx_dai[] = {
	{
		.name = "ALC5645",
		.stream_name = "ALC5645 AIF1",
		.codec_dai_name = "rt5645-aif1",
		.codec_name     = "rt5645.0-001a",
		.cpu_dai_name   = "imx-ssi.1",
		.platform_name  = "imx-pcm-audio.1",
		.init           = imx_alc5645_init,
		.ops            = &imx_aif1_ops,
	},
};

static struct snd_soc_card snd_soc_card_imx = {
	.name           = "alc5645-audio",
	.dai_link       = imx_dai,
	.num_links      = ARRAY_SIZE(imx_dai),
};

static int imx_audmux_config(int slave, int master)
{
	unsigned int ptcr, pdcr;
	slave = slave - 1;
	master = master - 1;

	ptcr = MXC_AUDMUX_V2_PTCR_SYN |
		MXC_AUDMUX_V2_PTCR_TFSDIR |
		MXC_AUDMUX_V2_PTCR_TFSEL(master) |
		MXC_AUDMUX_V2_PTCR_TCLKDIR |
		MXC_AUDMUX_V2_PTCR_TCSEL(master);
	pdcr = MXC_AUDMUX_V2_PDCR_RXDSEL(master);
	mxc_audmux_v2_configure_port(slave, ptcr, pdcr);

	ptcr = MXC_AUDMUX_V2_PTCR_SYN;
	pdcr = MXC_AUDMUX_V2_PDCR_RXDSEL(slave);
	mxc_audmux_v2_configure_port(master, ptcr, pdcr);

	return 0;
}

/*
 * This function will register the snd_soc_pcm_link drivers.
 */
static int __devinit imx_alc5645_probe(struct platform_device *pdev)
{

	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;
	struct imx_priv *priv = &card_priv;
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	int ret = 0;

	priv->pdev = pdev;

	imx_audmux_config(plat->src_port, plat->ext_port);

	if (plat->init && plat->init()) {
		ret = -EINVAL;
		return ret;
	}

	priv->sysclk = plat->sysclk;

	printk("JD gpio/irq: 0x%x\n", plat->hp_gpio);
	gpio->gpio = plat->hp_gpio;

	priv->intr_debounce = IMX_INTR_DEBOUNCE;
	priv->hs_insert_det_delay = IMX_HS_INSERT_DET_DELAY;
	priv->hs_remove_det_delay = IMX_HS_REMOVE_DET_DELAY;
	priv->button_det_delay = IMX_BUTTON_DET_DELAY;
	priv->hs_det_poll_intrvl = IMX_HS_DET_POLL_INTRVL;
	priv->hs_det_retry = IMX_HS_DET_RETRY_COUNT;
	priv->button_en_delay = IMX_BUTTON_EN_DELAY;
	priv->process_button_events = false;

	INIT_DELAYED_WORK(&priv->hs_insert_work, imx_check_hs_insert_status);
	INIT_DELAYED_WORK(&priv->hs_remove_work, imx_check_hs_remove_status);
	INIT_DELAYED_WORK(&priv->hs_button_work, imx_check_hs_button_status);
	INIT_DELAYED_WORK(&priv->hs_button_en_work, imx_enable_hs_button_events);
	mutex_init(&priv->jack_mlock);


#ifdef CONFIG_ANDROID
	priv->sdev.name = "h2w";
	ret = switch_dev_register(&priv->sdev);
	if (ret < 0) {
		ret = -EINVAL;
		return ret;
	}
	/*
	pr_debug("%s: plat->hp_active_low=%d", __func__, plat->hp_active_low);
	if (plat->hp_gpio != -1) {
		priv->hp_status = gpio_get_value(plat->hp_gpio);
		pr_debug("%s: priv->hp_status=%d", __func__, priv->hp_status);
		if (priv->hp_status != plat->hp_active_low)
			switch_set_state(&priv->sdev, 2);
		else
			switch_set_state(&priv->sdev, 0);
	}
	else {
		priv->hp_status = plat->hp_active_low;
		switch_set_state(&priv->sdev, 0);
	}
	*/
#endif  
	priv->amp_enable = plat->amp_enable;
	if (priv->amp_enable)
		priv->amp_enable(1);
	priv->first_stream = NULL;
	priv->second_stream = NULL;

	return ret;
}

static void snd_imx_unregister_jack(struct imx_priv *ctx)
{
	/* Set process button events to false so that the button
	   delayed work will not be scheduled.*/
	ctx->process_button_events = false;
	cancel_delayed_work_sync(&ctx->hs_insert_work);
	cancel_delayed_work_sync(&ctx->hs_button_en_work);
	cancel_delayed_work_sync(&ctx->hs_button_work);
	cancel_delayed_work_sync(&ctx->hs_remove_work);
	snd_soc_jack_free_gpios(&ctx->jack, 1, &hs_gpio);
}


static int __devexit imx_alc5645_remove(struct platform_device *pdev)
{
	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;
	struct imx_priv *priv = &card_priv;

	snd_imx_unregister_jack(priv);

	if (card_priv.amp_enable)
		card_priv.amp_enable(0);
	if (plat->clock_enable)
		plat->clock_enable(0);

	if (plat->finit)
		plat->finit();

#ifdef CONFIG_ANDROID
	switch_dev_unregister(&priv->sdev);
#endif
	return 0;
}

static struct platform_driver imx_alc5645_driver = {
	.probe = imx_alc5645_probe,
	.remove = imx_alc5645_remove,
	.driver = {
		.name = "imx-alc5645",
		.owner = THIS_MODULE,
	},
};

static struct platform_device *imx_snd_device;

static int __init imx_asoc_init(void)
{
	int ret;

	ret = platform_driver_register(&imx_alc5645_driver);
	if (ret < 0)
		goto exit;

	imx_snd_device = platform_device_alloc("soc-audio", -1);
	if (!imx_snd_device)
		goto err_device_alloc;

	platform_set_drvdata(imx_snd_device, &snd_soc_card_imx);

	ret = platform_device_add(imx_snd_device);

	if (0 == ret)
		goto exit;

	platform_device_put(imx_snd_device);

err_device_alloc:
	platform_driver_unregister(&imx_alc5645_driver);
exit:
	return ret;
}

static void __exit imx_asoc_exit(void)
{
	platform_driver_unregister(&imx_alc5645_driver);
	platform_device_unregister(imx_snd_device);
}

module_init(imx_asoc_init);
module_exit(imx_asoc_exit);

/* Module information */
MODULE_DESCRIPTION("ALSA SoC imx alc5645");
MODULE_LICENSE("GPL");
