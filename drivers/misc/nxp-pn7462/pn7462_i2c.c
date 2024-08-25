/*
 * Copyright (C) 2010 Trusted Logic S.A.
 * modifications copyright (C) 2015 NXP B.V.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include "pn7462_i2c.h"
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/of.h>

#define MAX_BUFFER_SIZE	512

//#define NFC_EE_TEST

#define MODE_OFF    0
#define MODE_RUN    1
#define MODE_FW     2

/* Only pn548, pn547 and pn544 are supported */
#define CHIP "pn7462"
#define DRIVER_CARD "PN7462 NFC"
#define DRIVER_DESC "NFC driver for PN7462 Family"

/*#ifndef CONFIG_OF
#define CONFIG_OF
#endif*/

struct pn7462_dev	{
	wait_queue_head_t read_wq;
	struct mutex read_mutex;
	struct i2c_client *client;
	struct miscdevice pn7462_device;
	int ven_gpio;
	int firm_gpio;
	int irq_gpio;
	int clkreq_gpio;
	int led1_gpio;
	int led2_gpio;
	int led3_gpio;
	int led4_gpio;
	int sled1_gpio;
	int sled2_gpio;
	struct regulator *pvdd_reg;
	struct regulator *vbat_reg;
	struct regulator *pmuvcc_reg;
	struct regulator *sevdd_reg;
	bool irq_enabled;
	spinlock_t irq_enabled_lock;
	int pon;
};

/**********************************************************
 * Interrupt control and handler
 **********************************************************/
static void pn7462_disable_irq(struct pn7462_dev *pn7462_dev)
{
	unsigned long flags;

	spin_lock_irqsave(&pn7462_dev->irq_enabled_lock, flags);
	if (pn7462_dev->irq_enabled) {
		disable_irq_nosync(pn7462_dev->client->irq);
		pn7462_dev->irq_enabled = false;
	}
	spin_unlock_irqrestore(&pn7462_dev->irq_enabled_lock, flags);
}

static irqreturn_t pn7462_dev_irq_handler(int irq, void *dev_id)
{
	struct pn7462_dev *pn7462_dev = dev_id;
	//printk("NFC get interrupt!\n");
#ifdef NFC_EE_TEST
	printk("NFC get interrupt!\n");
	pn7462_disable_irq(pn7462_dev);
#else
	pn7462_disable_irq(pn7462_dev);

	/* Wake up waiting readers */
	wake_up(&pn7462_dev->read_wq);
#endif

	return IRQ_HANDLED;
}

/**********************************************************
 * private functions
 **********************************************************/
static int pn7462_i2c_enable(struct pn7462_dev * pn7462_dev) // i2c power on
{
	char tmp[16];
	int ret;
	mutex_lock(&pn7462_dev->read_mutex);
	pn7462_disable_irq(pn7462_dev);
	ret = i2c_master_recv(pn7462_dev->client, tmp, 1);
	pn7462_dev->irq_enabled = true;
	enable_irq(pn7462_dev->client->irq);
	ret = wait_event_interruptible(pn7462_dev->read_wq,!pn7462_dev->irq_enabled);
	pn7462_disable_irq(pn7462_dev);
	if(16==i2c_master_recv(pn7462_dev->client, tmp, 16))
	{
		printk("pn7462 i2c enable ver:%s\n",tmp);
		pn7462_dev->pon=true;
	}
	mutex_unlock(&pn7462_dev->read_mutex);
	return ret;
}
static int pn7462_enable(struct pn7462_dev *dev, int mode)
{
	if (MODE_RUN == mode) {
		pr_info("%s power on\n", __func__);
		/*if (gpio_is_valid(dev->firm_gpio))
			gpio_set_value_cansleep(dev->firm_gpio, 0);*/
		gpio_direction_output(dev->ven_gpio, 1);
		gpio_set_value_cansleep(dev->ven_gpio, 1);
		msleep(100);
	}
	else if (MODE_FW == mode) {
		/* power on with firmware download (requires hw reset)
		 */
		pr_info("%s power on with firmware\n", __func__);
		gpio_set_value(dev->ven_gpio, 1);
		msleep(20);
		/*if (gpio_is_valid(dev->firm_gpio)) {
			gpio_set_value(dev->firm_gpio, 1);
		}
		else {
			pr_err("%s Unused Firm GPIO %d\n", __func__, mode);
			return GPIO_UNUSED;
		}*/
		msleep(20);
		gpio_set_value(dev->ven_gpio, 0);
		msleep(100);
		gpio_set_value(dev->ven_gpio, 1);
		msleep(20);
	}
	else {
		pr_err("%s bad arg %d\n", __func__, mode);
		return -EINVAL;
	}

	return 0;

}

static void pn7462_disable(struct pn7462_dev *dev)
{
	/* power off */
	pr_info("%s power off\n", __func__);

	gpio_set_value_cansleep(dev->ven_gpio, 0);
	msleep(100);

}

/**********************************************************
 * driver functions
 **********************************************************/
static ssize_t pn7462_dev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn7462_dev *pn7462_dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE];
	int ret,i;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	pr_debug("%s : reading %zu bytes.\n", __func__, count);

	mutex_lock(&pn7462_dev->read_mutex);

	if (gpio_get_value(pn7462_dev->irq_gpio)) {
		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto fail;
		}

		while (1) {
			pn7462_dev->irq_enabled = true;
			enable_irq(pn7462_dev->client->irq);
			ret = wait_event_interruptible(
					pn7462_dev->read_wq,
					!pn7462_dev->irq_enabled);

			pn7462_disable_irq(pn7462_dev);

			if (ret)
				goto fail;

			if (!gpio_get_value(pn7462_dev->irq_gpio))
				break;

			pr_warning("%s: spurious interrupt detected\n", __func__);
		}
	}

	/* Read data */
	ret = i2c_master_recv(pn7462_dev->client, tmp, count);

	mutex_unlock(&pn7462_dev->read_mutex);

	/* pn7462 seems to be slow in handling I2C read requests
	 * so add 1ms delay after recv operation */
	udelay(1000);

	if (ret < 0) {
		pr_err("%s: i2c_master_recv returned %d\n", __func__, ret);
		return ret;
	}
	if (ret > count) {
		pr_err("%s: received too many bytes from i2c (%d)\n",
			__func__, ret);
		return -EIO;
	}

	if (copy_to_user(buf, tmp, ret)) {
		pr_warning("%s : failed to copy to user space\n", __func__);
		return -EFAULT;
	}
	return ret;

fail:
	mutex_unlock(&pn7462_dev->read_mutex);
	return ret;
}

static ssize_t pn7462_dev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn7462_dev  *pn7462_dev;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	pn7462_dev = filp->private_data;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	if (copy_from_user(tmp, buf, count)) {
		pr_err("%s : failed to copy from user space\n", __func__);
		return -EFAULT;
	}

	pr_debug("%s : writing %zu bytes.\n", __func__, count);
	/* Write data */
	ret = i2c_master_send(pn7462_dev->client, tmp, count);
	if (ret != count) {
		pr_err("%s : i2c_master_send returned %d\n", __func__, ret);
		ret = -EIO;
	}

	/* pn7462 seems to be slow in handling I2C write requests
	 * so add 1ms delay after I2C send oparation */
	udelay(1000);

	return ret;
}

static int pn7462_dev_open(struct inode *inode, struct file *filp)
{
	struct pn7462_dev *pn7462_dev = container_of(filp->private_data,
											   struct pn7462_dev,
											   pn7462_device);

	filp->private_data = pn7462_dev;

	pr_info("%s : %d,%d\n", __func__, imajor(inode), iminor(inode));

	if(!pn7462_dev->pon)
	{
		pn7462_i2c_enable(pn7462_dev);

	}

	//pn7462_enable(pn7462_dev, MODE_RUN);

	return 0;
}

static int pn7462_dev_release(struct inode *inode, struct file *filp)
{
	// struct pn7462_dev *pn7462_dev = container_of(filp->private_data,
	//										   struct pn7462_dev,
	//										   pn7462_device);

	pr_info("%s : closing %d,%d\n", __func__, imajor(inode), iminor(inode));

	// pn7462_disable(pn7462_dev);

	return 0;
}

static long  pn7462_dev_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	struct pn7462_dev *pn7462_dev = filp->private_data;

	pr_info("%s, cmd=%d, arg=%lu\n", __func__, cmd, arg);
	switch (cmd) {
	case PN7462_SET_PWR:
		if (arg == 2) {
			/* power on w/FW */
			if (GPIO_UNUSED == pn7462_enable(pn7462_dev, arg)) {
				return GPIO_UNUSED;
			}
		} else if (arg == 1) {
			/* power on */
			pn7462_enable(pn7462_dev, arg);
		} else  if (arg == 0) {
			/* power off */
			pn7462_disable(pn7462_dev);
		} else {
			pr_err("%s bad SET_PWR arg %lu\n", __func__, arg);
			return -EINVAL;
		}
		break;
#if 0
	case PN7462_LED1:
		(arg == 0? gpio_direction_output(pn7462_dev->led1_gpio, 0):gpio_direction_output(pn7462_dev->led1_gpio, 1));
		break;
	case PN7462_LED2:
		(arg == 0? gpio_direction_output(pn7462_dev->led2_gpio, 0):gpio_direction_output(pn7462_dev->led2_gpio, 1));
		break;
	case PN7462_LED3:
		(arg == 0? gpio_direction_output(pn7462_dev->led3_gpio, 0):gpio_direction_output(pn7462_dev->led3_gpio, 1));
		break;
	case PN7462_LED4:
		(arg == 0? gpio_direction_output(pn7462_dev->led4_gpio, 0):gpio_direction_output(pn7462_dev->led4_gpio, 1));
		break;
	case PN7462_SLED1:
		(arg == 0? gpio_direction_output(pn7462_dev->sled1_gpio, 0):gpio_direction_output(pn7462_dev->sled1_gpio, 1));
		break;
	case PN7462_SLED2:
		(arg == 0? gpio_direction_output(pn7462_dev->sled2_gpio, 0):gpio_direction_output(pn7462_dev->sled2_gpio, 1));
		break;				
#endif		
	default:
		pr_err("%s bad ioctl %u\n", __func__, cmd);
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations pn7462_dev_fops = {
	.owner	= THIS_MODULE,
	.llseek	= no_llseek,
	.read	= pn7462_dev_read,
	.write	= pn7462_dev_write,
	.open	= pn7462_dev_open,
	.release  = pn7462_dev_release,
	.unlocked_ioctl  = pn7462_dev_ioctl,
};


/*
 * Handlers for alternative sources of platform_data
 */
#ifdef CONFIG_OF
/*
 * Translate OpenFirmware node properties into platform_data
 */
static int pn7462_get_pdata(struct device *dev,
							struct pn7462_i2c_platform_data *pdata)
{
	struct device_node *node;
	u32 flags;
	int val;

	/* make sure there is actually a device tree node */
	node = dev->of_node;
	if (!node)
		return -ENODEV;

	memset(pdata, 0, sizeof(*pdata));

	/* read the dev tree data */

	/* ven pin - enable's power to the chip - REQUIRED */
	val = of_get_named_gpio_flags(node, "enable-gpios", 0, &flags);
	if (val >= 0) {
		pdata->ven_gpio = val;
	}
	else {
		dev_err(dev, "VEN GPIO error getting from OF node\n");
		return val;
	}

	/* firm pin - controls firmware download - OPTIONAL */
	val = of_get_named_gpio_flags(node, "firmware-gpios", 0, &flags);
	if (val >= 0) {
		pdata->firm_gpio = val;
	}
	else {
		pdata->firm_gpio = GPIO_UNUSED;
		dev_warn(dev, "FIRM GPIO <OPTIONAL> error getting from OF node\n");
	}

	/* irq pin - data available irq - REQUIRED */
	val = of_get_named_gpio_flags(node, "interrupt-gpios", 0, &flags);
	if (val >= 0) {
		pdata->irq_gpio = val;
	}
	else {
		dev_err(dev, "IRQ GPIO error getting from OF node\n");
		return val;
	}

	/* clkreq pin - controls the clock to the PN7462 - OPTIONAL */
	val = of_get_named_gpio_flags(node, "nxp,pn7462-clkreq", 0, &flags);
	if (val >= 0) {
		pdata->clkreq_gpio = val;
	}
	else {
		pdata->clkreq_gpio = GPIO_UNUSED;
		dev_warn(dev, "CLKREQ GPIO <OPTIONAL> error getting from OF node\n");
	}

	/* handle the regulator lines - these are optional
	 * PVdd - pad Vdd (544, 547)
	 * Vbat - Battery (544, 547)
	 * PMUVcc - UICC Power (544, 547)
	 * SEVdd - SE Power (544)
	 *
	 * Will attempt to load a matching Regulator Resource for each
	 * If no resource is provided, then the input will not be controlled
	 * Example: if only PVdd is provided, it is the only one that will be
	 *  turned on/off.
	 */
	pdata->pvdd_reg = regulator_get(dev, "nxp,pn7462-pvdd");
	if(IS_ERR(pdata->pvdd_reg)) {
		pr_err("%s: could not get nxp,pn7462-pvdd, rc=%ld\n", __func__, PTR_ERR(pdata->pvdd_reg));
		pdata->pvdd_reg = NULL;
	}

	pdata->vbat_reg = regulator_get(dev, "nxp,pn7462-vbat");
	if (IS_ERR(pdata->vbat_reg)) {
		pr_err("%s: could not get nxp,pn7462-vbat, rc=%ld\n", __func__, PTR_ERR(pdata->vbat_reg));
		pdata->vbat_reg = NULL;
	}

	pdata->pmuvcc_reg = regulator_get(dev, "nxp,pn7462-pmuvcc");
	if (IS_ERR(pdata->pmuvcc_reg)) {
		pr_err("%s: could not get nxp,pn7462-pmuvcc, rc=%ld\n", __func__, PTR_ERR(pdata->pmuvcc_reg));
		pdata->pmuvcc_reg = NULL;
	}

	pdata->sevdd_reg = regulator_get(dev, "nxp,pn7462-sevdd");
	if (IS_ERR(pdata->sevdd_reg)) {
		pr_err("%s: could not get nxp,pn7462-sevdd, rc=%ld\n", __func__, PTR_ERR(pdata->sevdd_reg));
		pdata->sevdd_reg = NULL;
	}

	return 0;
}
#else
static int pn7462_get_pdata(struct device *dev,
							struct pn7462_i2c_platform_data *pdata)
{
	pdata = dev->platform_data;
	return 0;
}
#endif

/**********************************************************
 * NFC EE test device i2c command
 **********************************************************/
#ifdef NFC_EE_TEST
static int recivce_nfc_ack_data(struct i2c_client *client, char *buf, int len)
{
	struct i2c_msg msg;
	
	msg.addr = client->addr;
	msg.flags = client->flags | I2C_M_RD;
	msg.buf = buf;
	msg.len = len;
	
	return i2c_transfer(client->adapter, &msg, 1);
}

static int nfc_test_code(struct pn7462_dev *pn7462_dev)
{
	int ret = 0;
	char NCICoreReset[4] = {0x20, 0x00, 0x01, 0x01};
	char NCIResetAck[6];
	char NCICoreInit[3] = {0x20, 0x01, 0x00};
	char NCIAck[MAX_BUFFER_SIZE];
	
	/* reset ven gpio */
	pn7462_enable(pn746_dev, 1);

	/* enable interrupt */
	enable_irq(pn7462_dev->client->irq);
      
	/* send reset code */
	ret = i2c_master_send(pn7462_dev->client, NCICoreReset, sizeof(NCICoreReset));
	if (ret != sizeof(NCICoreReset)) {
		pr_err("%s : i2c_master_send NCICoreReset returned %d\n", __func__, ret);
		return -1;
	}
	
	msleep(5);

	/* recivce reset code ack and check recive data */
	ret = recivce_nfc_ack_data(pn7462_dev->client, NCIResetAck, sizeof(NCIResetAck));
	if ((!ret) || (NCIResetAck[0] != 0x40) || (NCIResetAck[1] != 0x00)) {
		printk("nfc send NCICoreReset {0x20,0x00,0x01,0x01} failed!\n");
		printk("ret %d get error data: 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n", ret,
		       NCIResetAck[0], NCIResetAck[1], NCIResetAck[2], NCIResetAck[3], NCIResetAck[4], NCIResetAck[5]);
		return -1;
	}
	
	/* send NCI Core Init */
	ret = i2c_master_send(pn7462_dev->client, NCICoreInit, sizeof(NCICoreInit));
	if (ret != sizeof(NCICoreInit)) {
		pr_err("%s : i2c_master_send NCICoreInit returned %d\n", __func__, ret);
		return -1;
	}
	
	msleep(5);
  
	/* get version */
	ret = recivce_nfc_ack_data(pn7462_dev->client, NCIAck, sizeof(NCIAck));
	if ((NCIAck[0] != 0x40) || (NCIAck[1] != 0x01) || (NCIAck[3] != 0x00)) {
		printk("nfc send NCICoreInit {0x20,0x01,0x00} failed!\n");
		printk("ret %d get error data: 0x%x, 0x%x, 0x%x, 0x%x\n", ret, NCIAck[0], NCIAck[1], NCIAck[2], NCIAck[3]);
		return -1;
	}
	
	printk("NFC chip firmware version: 0x%x 0x%x 0x%x\n", NCIAck[17+NCIAck[8]], NCIAck[18+NCIAck[8]], NCIAck[19+NCIAck[8]]);
	
	return 0;
	
}
#endif


/*
 * pn7462_probe
 */
static int pn7462_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int ret;
	struct pn7462_i2c_platform_data *pdata; // gpio values, from board file or DT
	struct pn7462_i2c_platform_data tmp_pdata;
	struct pn7462_dev *pn7462_dev; // internal device specific data

	pr_info("%s\n", __func__);

	/* ---- retrieve the platform data ---- */
	/* If the dev.platform_data is NULL, then */
	/* attempt to read from the device tree */
	if(!client->dev.platform_data)
	{
		ret = pn7462_get_pdata(&(client->dev), &tmp_pdata);
		if(ret){
			return ret;
		}

		pdata = &tmp_pdata;
	}
	else
	{
		pdata = client->dev.platform_data;
	}

	if (pdata == NULL) {
		pr_err("%s : nfc probe fail\n", __func__);
		return  -ENODEV;
	}

	/* validate the the adapter has basic I2C functionality */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "i2c_check_functionality error\n");
		return -EIO;
	}

	/* reserve the GPIO pins */
	pr_info("%s: request irq_gpio %d\n", __func__, pdata->irq_gpio);
	ret = gpio_request(pdata->irq_gpio, "nfc_int");
	if (ret){
		pr_err("%s :not able to get GPIO irq_gpio\n", __func__);
		return  -ENODEV;
	}
	ret = gpio_to_irq(pdata->irq_gpio);
	if (ret < 0){
		pr_err("%s :not able to map GPIO irq_gpio to an IRQ\n", __func__);
		goto err_ven;
	}
	else{
		client->irq = ret;
	}

	ret = gpio_request(pdata->ven_gpio, "nfc_ven");
	if (ret){
		pr_err("%s :not able to get GPIO ven_gpio\n", __func__);
		goto err_ven;
	}
#if 0
	ret = gpio_request(pdata->led1_gpio, "led1");
	if (ret){
		pr_err("%s :not able to get GPIO led1_gpio\n", __func__);
		goto err_ven;
	}	

	ret = gpio_request(pdata->led2_gpio, "led2");
	if (ret){
		pr_err("%s :not able to get GPIO led2_gpio\n", __func__);
		goto err_ven;
	}

	ret = gpio_request(pdata->led3_gpio, "led3");
	if (ret){
		pr_err("%s :not able to get GPIO led3_gpio\n", __func__);
		goto err_ven;
	}

	ret = gpio_request(pdata->led4_gpio, "led4");
	if (ret){
		pr_err("%s :not able to get GPIO led4_gpio\n", __func__);
		goto err_ven;
	}	

	ret = gpio_request(pdata->sled1_gpio, "sled1");
	if (ret){
		pr_err("%s :not able to get GPIO sled1_gpio\n", __func__);
		goto err_ven;
	}

	ret = gpio_request(pdata->sled2_gpio, "sled2");
	if (ret){
		pr_err("%s :not able to get GPIO sled1_gpio\n", __func__);
		goto err_ven;
	}		
#endif
	/* allocate the pn7462 driver information structure */
	pn7462_dev = kzalloc(sizeof(*pn7462_dev), GFP_KERNEL);
	if (pn7462_dev == NULL) {
		dev_err(&client->dev, "failed to allocate memory for module data\n");
		ret = -ENOMEM;
		goto err_exit;
	}

	/* store the platform data in the driver info struct */
	pn7462_dev->irq_gpio = pdata->irq_gpio;
	pn7462_dev->ven_gpio = pdata->ven_gpio;
	pn7462_dev->led1_gpio = pdata->led1_gpio;
	pn7462_dev->led2_gpio = pdata->led2_gpio;
	pn7462_dev->led3_gpio = pdata->led3_gpio;
	pn7462_dev->led4_gpio = pdata->led4_gpio;
	pn7462_dev->sled1_gpio = pdata->sled1_gpio;
	pn7462_dev->sled2_gpio = pdata->sled2_gpio;


	pn7462_dev->client = client;

	/* finish configuring the I/O */
	ret = gpio_direction_input(pn7462_dev->irq_gpio);
	if (ret < 0) {
		pr_err("%s :not able to set irq_gpio as input\n", __func__);
		goto err_exit;
	}

	ret = gpio_direction_output(pn7462_dev->ven_gpio, 0);
	if (ret < 0) {
		pr_err("%s : not able to set ven_gpio as output\n", __func__);
		goto err_exit;
	}
#if 0
	ret = gpio_direction_output(pn7462_dev->led1_gpio, 0);
	if (ret < 0) {
		pr_err("%s : not able to set led1_gpio as output\n", __func__);
		goto err_exit;
	}

	ret = gpio_direction_output(pn7462_dev->led2_gpio, 0);
	if (ret < 0) {
		pr_err("%s : not able to set led2_gpio as output\n", __func__);
		goto err_exit;
	}

	ret = gpio_direction_output(pn7462_dev->led3_gpio, 0);
	if (ret < 0) {
		pr_err("%s : not able to set led3_gpio as output\n", __func__);
		goto err_exit;
	}

	ret = gpio_direction_output(pn7462_dev->led4_gpio, 0);
	if (ret < 0) {
		pr_err("%s : not able to set led4_gpio as output\n", __func__);
		goto err_exit;
	}			

	ret = gpio_direction_output(pn7462_dev->sled1_gpio, 0);
	if (ret < 0) {
		pr_err("%s : not able to set sled1_gpio as output\n", __func__);
		goto err_exit;
	}

	ret = gpio_direction_output(pn7462_dev->sled2_gpio, 0);
	if (ret < 0) {
		pr_err("%s : not able to set sled2_gpio as output\n", __func__);
		goto err_exit;
	}				
#endif
	/* init mutex and queues */
	init_waitqueue_head(&pn7462_dev->read_wq);
	mutex_init(&pn7462_dev->read_mutex);
	spin_lock_init(&pn7462_dev->irq_enabled_lock);

	/* register as a misc device - character based with one entry point */
	pn7462_dev->pn7462_device.minor = MISC_DYNAMIC_MINOR;
	pn7462_dev->pn7462_device.name = CHIP;
	pn7462_dev->pn7462_device.fops = &pn7462_dev_fops;
	ret = misc_register(&pn7462_dev->pn7462_device);
	if (ret) {
		pr_err("%s : misc_register failed\n", __FILE__);
		goto err_misc_register;
	}

	/* request irq.  the irq is set whenever the chip has data available
	 * for reading.  it is cleared when all data has been read.
	 */
	pr_info("%s : requesting IRQ %d\n", __func__, client->irq);
	pn7462_dev->irq_enabled = true;
	pn7462_dev->pon = false;

	ret = request_irq(client->irq, pn7462_dev_irq_handler,
				IRQF_TRIGGER_RISING, client->name, pn7462_dev);
	if (ret) {
		dev_err(&client->dev, "request_irq failed\n");
		goto err_request_irq_failed;
	}
	pn7462_disable_irq(pn7462_dev);

	i2c_set_clientdata(client, pn7462_dev);
	
#ifdef NFC_EE_TEST
	ret = nfc_test_code(pn7462_dev);
	if (!ret) {
		printk("NFC I2c test success!!\n");
	}
	else {
		printk("NFC I2c test failed!!\n");
	}
#endif
	
	return 0;

err_request_irq_failed:
	misc_deregister(&pn7462_dev->pn7462_device);
err_misc_register:
err_exit:
err_firm:
	gpio_free(pdata->ven_gpio);
err_ven:
	gpio_free(pdata->irq_gpio);

	return ret;
}

static int pn7462_remove(struct i2c_client *client)
{
	struct pn7462_dev *pn7462_dev;

	pr_info("%s\n", __func__);

	pn7462_dev = i2c_get_clientdata(client);
	free_irq(client->irq, pn7462_dev);
	misc_deregister(&pn7462_dev->pn7462_device);
	mutex_destroy(&pn7462_dev->read_mutex);
	gpio_free(pn7462_dev->irq_gpio);
	gpio_free(pn7462_dev->ven_gpio);

	kfree(pn7462_dev);

	return 0;
}

static const struct i2c_device_id pn7462_id[] = {
	{ "nxp_nfc_pn7462", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, pn7462_id);

#ifdef CONFIG_PM
static int nfc_pn7462_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	
	return 0;
}

static int nfc_pn7462_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	
	return 0;
}

static SIMPLE_DEV_PM_OPS(nfc_pn7462_pm, nfc_pn7462_suspend, nfc_pn7462_resume);
#endif

static struct i2c_driver pn7462_driver = {
	.driver = {
		.name	= "nxp_nfc_p7462",
		.owner	= THIS_MODULE,
		.pm	= &nfc_pn7462_pm,
	},
	
	.probe		= pn7462_probe,
	.remove		= pn7462_remove,
	.id_table	= pn7462_id,
};

/*
 * module load/unload record keeping
 */

static int __init pn7462_dev_init(void)
{
	pr_info("%s\n", __func__);
	return i2c_add_driver(&pn7462_driver);
}

static void __exit pn7462_dev_exit(void)
{
	pr_info("%s\n", __func__);
	i2c_del_driver(&pn7462_driver);
}

module_init(pn7462_dev_init);
module_exit(pn7462_dev_exit);

MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
