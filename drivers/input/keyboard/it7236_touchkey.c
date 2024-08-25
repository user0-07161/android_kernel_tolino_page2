//#define DEBUG
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <asm/div64.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/i2c.h>
#include <asm/uaccess.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>

//#include <mach/regs-gpio.h>
//#include <mach/gpio-bank.h>

#include <linux/of_gpio.h>
#include <linux/time.h>
#include <linux/clk.h>
#include <linux/input/mt.h>
#include <linux/regulator/consumer.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define IT7236_FW_AUTO_UPGRADE     0 // Upgrade Firmware form driver rawDATA[] array 1:Enable ; 0:Disable
#define IT7236_TOUCH_DATA_REGISTER_ADDRESS	0xFF  // Change the address to read others RAM data
#define DRIVER_VERSION	"1.0.0"

#if IT7236_FW_AUTO_UPGRADE
#include "IT7236_FW.h"
#endif

#include "it7236_touchkey.h"

#define IT7236_I2C_NAME "it7236_touchkey"
#define USE_MY_WORKQUEUE		1
static struct IT7236_tk_data *gl_ts;

static int ite7236_major = 0;	// dynamic major by default
static int ite7236_minor = 0;
static struct cdev ite7236_cdev;
static struct class *ite7236_class = NULL;
static dev_t ite7236_dev;
static struct input_dev *input_dev;
static int fw_upgrade_success = 0;
static u8 wTemp[128] = {0x00};
static char config_id[10];
static unsigned short i2c_addr_bak;  

static DEFINE_SPINLOCK(it7236_lock);

static char it7236_fw_name[256] = "IT7236_FW";	
// Put the IT7236 firmware file at /etc/firmware/ and the file name is IT7236_FW

#ifdef CONFIG_HAS_EARLYSUSPEND
static void IT7236_tk_early_suspend(struct early_suspend *h);
static void IT7236_tk_late_resume(struct early_suspend *h);
#endif

#define EVENT_PENDING_NONE		0
#define EVENT_PENDING_DOWN		1
#define EVENT_PENDING_UP			2
static int giIT7236_event_pending = EVENT_PENDING_NONE;
static int giIT7236_int_1st;

static int get_config_ver(void);

// TODO: Depend on your platform.
// Change your GPIO interrupt setting here.
// Below Setting is for mini210 and use S5PV210_GPH2 bit 0
static void GPIO_Init(void)
{
	/*
	if (gpio_is_valid (S5PV210_GPH2(0))){
		if(gpio_request (S5PV210_GPH2(0), "GPH2")){
			s3c_gpio_cfgpin(S5PV210_GPH2(0), S3C_GPIO_SFN(0xf));  
		}else{
			gpio_free (S5PV210_GPH2(0));
			s3c_gpio_cfgpin(S5PV210_GPH2(0), S3C_GPIO_SFN(0xf));  
		}
	}else{
	}
	*/

}


static int i2cReadFromIt7236(struct i2c_client *client, unsigned char bufferIndex, unsigned char dataBuffer[], unsigned short dataLength)
{
	int ret;
	//unsigned long lock_flags;
	struct i2c_msg msgs[2] = { {
		.addr = client->addr,
		.flags = I2C_M_RECOVER,
		.len = 1,
		.buf = &bufferIndex
		}, {
		.addr = client->addr,
//		.flags = I2C_M_RD,
		.flags = I2C_M_RD_RESTART|I2C_M_RECOVER,
		.len = dataLength,
		.buf = dataBuffer
		}
	};

	memset(dataBuffer, 0xFF, dataLength);

	//spin_lock_irqsave(&it7236_lock, lock_flags);
	spin_lock(&it7236_lock);
	ret = i2c_transfer(client->adapter, msgs, 2);
	spin_unlock(&it7236_lock);
	//spin_unlock_irqrestore(&it7236_lock, lock_flags);
	
	return ret;
}

static int i2cReadFromIt7280(struct i2c_client *client, unsigned char bufferIndex, unsigned char dataBuffer[], unsigned short dataLength)
{
	return i2cReadFromIt7236(client, bufferIndex, dataBuffer, dataLength);
}

static int i2cWriteToIt7236_Ex(struct i2c_client *client, unsigned char bufferIndex, unsigned char const dataBuffer[], unsigned short dataLength,int retry)
{
	unsigned char buffer4Write[256];
	int ret;


	struct i2c_msg msgs[1] = { {
		.addr = client->addr,
		.flags = I2C_M_RECOVER,
		.len = dataLength + 1,
		.buf = buffer4Write
		}
	};

	if(dataLength < 256) {
		buffer4Write[0] = bufferIndex;
		memcpy(&(buffer4Write[1]), dataBuffer, dataLength);

		do {
			spin_lock(&it7236_lock);
			ret = i2c_transfer(client->adapter, msgs, 1);
			spin_unlock(&it7236_lock);
			retry--;
		} while((ret != 1) && (retry > 0));

		if(ret != 1)
			pr_err("%s : i2c_transfer offset=0x%x error(%d)\n", 
					__func__,bufferIndex,ret);

		return ret;
	}
	else {
		pr_err("%s : i2c_transfer error , out of size\n", __func__);
		return -1;
	}
}

static int i2cWriteToIt7236(struct i2c_client *client, unsigned char bufferIndex, unsigned char const dataBuffer[], unsigned short dataLength)
{
	return  i2cWriteToIt7236_Ex(client, bufferIndex, dataBuffer,dataLength,3);
}

static int i2cWriteToIt7280(struct i2c_client *client, unsigned char bufferIndex, unsigned char const dataBuffer[], unsigned short dataLength)
{
	return i2cWriteToIt7236(client, bufferIndex, dataBuffer, dataLength);
}

static bool waitCommandDone(void)
{
	unsigned char ucQuery = 0x00;
	unsigned int count = 0;

	do {
		if(!i2cReadFromIt7236(gl_ts->client, 0xFA, &ucQuery, 1))
			ucQuery = 0x00;
		count++;
	} while((ucQuery != 0x80) && (count < 10));

	if( ucQuery == 0x80)
		return  true;
	else
		return  false;
}

static bool fnFirmwareReinitialize(void)
{
	int count = 0, addr= 0;
	u8 data[1];
	char pucBuffer[1];
	
	pucBuffer[0] = 0x01;
	i2cWriteToIt7280(gl_ts->client, 0xF1, pucBuffer,1);
	mdelay(4);	
	pucBuffer[0] = 0xFF;
	i2cWriteToIt7280(gl_ts->client, 0xF6, pucBuffer, 1);
	mdelay(4);
	pucBuffer[0] = 0x64;
	i2cWriteToIt7280(gl_ts->client, 0xF0, pucBuffer, 1);
	mdelay(4);
	pucBuffer[0] = 0x44;
	i2cWriteToIt7280(gl_ts->client, 0x01, pucBuffer, 1);
	mdelay(4);
	pucBuffer[0] = 0x80;
	i2cWriteToIt7280(gl_ts->client, 0x00, pucBuffer, 1);
	mdelay(4);
	pucBuffer[0] = 0x00;
	i2cWriteToIt7280(gl_ts->client, 0x00, pucBuffer, 1);
	mdelay(4);
	
	for(addr= 0xFF ;addr> 0xF0 ;addr--)
	{
		pucBuffer[0] = 0x00;
		i2cWriteToIt7280(gl_ts->client, addr, pucBuffer, 1);
		mdelay(4);
	}
	
	pucBuffer[0] = 0x01;
	i2cWriteToIt7280(gl_ts->client, 0xF1, pucBuffer,1);
	mdelay(4);
	
	pucBuffer[0] = 0x00;
	i2cWriteToIt7280(gl_ts->client, 0x01, pucBuffer,1);
	mdelay(4);
	
	pucBuffer[0] = 0x00;
	i2cWriteToIt7280(gl_ts->client, 0xF1, pucBuffer,1);
	mdelay(4);
	
	pucBuffer[0] = 0x00;
	i2cWriteToIt7280(gl_ts->client, 0xF0, pucBuffer,1);
	mdelay(4);
	do{
		i2cReadFromIt7280(gl_ts->client, 0xFA, data, 1);
		count++;
	} while( (data[0] != 0x80) && (count < 20));

	pucBuffer[0] = 0x00;
	i2cWriteToIt7280(gl_ts->client, 0xF1, pucBuffer, 1);

	return true;
}

static ssize_t IT7236_upgrade_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s not ready\n", __func__);
}

static int it7236_upgrade(u8* InputBuffer, int fileSize)
{
	int i, j, k;
	int StartPage = 0;
	int registerI, registerJ;
	int page, pageres;
	int Addr;
	int nErasePage;
	int err_temp;
	u8 result = 1;
	u8 DATABuffer1[128] = {0x00};
	u8 DATABuffer2[128] = {0x00};
	u8 OutputDataBuffer[8192] = {0x00};
	u8 WriteDATABuffer[128] = {0x00};
	char pucBuffer[1];
	int retry = 0;
	int ret1, ret2;

	disable_irq_nosync(gl_ts->client->irq);

	pr_info("[IT7236] Start Upgrade Firmware \n");
	pucBuffer[0] = 0x55;
	i2cWriteToIt7236(gl_ts->client, 0xFB,pucBuffer,1);	
	mdelay(10);

	//Request Full Authority of All Registers
	pucBuffer[0] = 0x01;
	i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);

	// 1. Assert Reset of MCU
	pucBuffer[0] = 0x64;
	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);
	pucBuffer[0] = 0x04;
	i2cWriteToIt7236(gl_ts->client, 0x01, pucBuffer, 1);

	// close watchdog	
	
	pucBuffer[0] = 0x00;
	i2cWriteToIt7236(gl_ts->client, 0x22, pucBuffer, 1);
	
	
	// 2. Assert EF enable & reset
	pucBuffer[0] = 0x10;
	i2cWriteToIt7236(gl_ts->client, 0x2B, pucBuffer, 1);
	pucBuffer[0] = 0x00;
	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);

	// 3. Test Mode Enable
	pucBuffer[0] = 0x07;
	gl_ts->client->addr = 0x7F;
	err_temp = i2cWriteToIt7236(gl_ts->client, 0xF4, pucBuffer, 1);
	if(err_temp != 1){
		pr_err("[IT7236]%s : [%d]  xxx  error err_temp=%d \n",__func__,__LINE__,err_temp);
		return err_temp;
	}

	gl_ts->client->addr = i2c_addr_bak;

	nErasePage = fileSize/256;
	if(fileSize % 256 == 0)
		nErasePage -= 1;

	// 4. EF HVC Flow (Erase Flash)
	for( i = 0 ; i < nErasePage + 1 ; i++ ){
		// EF HVC Flow
		pucBuffer[0] = i+StartPage;
		i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);	//	Select axa of EF E/P Mode

		pucBuffer[0] = 0x00;
		i2cWriteToIt7236(gl_ts->client, 0xF9, pucBuffer, 1);	// Select axa of EF E/P Mode Fail
		pucBuffer[0] = 0xB2;
		i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);	// Select efmode of EF E/P Mode(all erase)
		pucBuffer[0] = 0x80;
		i2cWriteToIt7236(gl_ts->client, 0xF9, pucBuffer, 1);	// Pump Enable
		mdelay(10);
		pucBuffer[0] = 0xB6;
		i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);	// Write EF CHVPL Mode Cmd
		pucBuffer[0] = 0x00;
		i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);	// Write EF Standby Mode Cmd
	}

	// 5. EFPL Flow - Write EF
	for (i = 0; i < fileSize; i += 256)
	{
		pucBuffer[0] = 0x05;
		err_temp = i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);	// Write EF PL Mode Cmd
		if(err_temp != 1){
			pr_err("[IT7236]%s : [%d]  xxx  error err_temp=%d \n",__func__,__LINE__,err_temp);
			return err_temp;
		}

		//	Write EF Data - half page(128 bytes)
		for(registerI = 0 ; registerI < 128; registerI++)
		{
			if(( i + registerI ) < fileSize ) {
				DATABuffer1[registerI] = InputBuffer[i+registerI];
			}
			else {
				DATABuffer1[registerI] = 0x00;
			}
		}
		for(registerI = 128 ; registerI < 256; registerI++)
		{
			if(( i + registerI ) < fileSize ) {
				DATABuffer2[registerI - 128] = InputBuffer[i+registerI];
			}
			else {
				DATABuffer2[registerI - 128] = 0x00;
			}
		}
		registerJ = i & 0x00FF;
		page = ((i & 0x3F00)>>8) + StartPage;
		pageres = i % 256;
		retry = 0;
		do {
			pucBuffer[0] = page;
			i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);
			i2cReadFromIt7236(gl_ts->client, 0xF0, pucBuffer, 1);
			retry++;
		} while((pucBuffer[0] != page) && (retry < 10));

		/* write 256 bytes once */
		retry = 0;

		gl_ts->client->addr = 0x7F;
		do {
			ret1 = i2cWriteToIt7236(gl_ts->client, 0x00, DATABuffer1, 128);
			ret2 = i2cWriteToIt7236(gl_ts->client, 0x00 + 128, DATABuffer2, 128);
			retry++;
		} while(((ret1 * ret2) != 1) && (retry < 5));
		if(retry>4){
			pr_err("[IT7236] retry  %s : [%d]   \n",__func__,__LINE__);
		}
		gl_ts->client->addr = i2c_addr_bak;

		pucBuffer[0] = 0x00;
		i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);	// Write EF Standby Mode Cmd
		pucBuffer[0] = 0x00;
		i2cWriteToIt7236(gl_ts->client, 0xF9, pucBuffer, 1);	// Select axa of EF E/P Mode Fail
		pucBuffer[0] = 0xE2;
		i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);	// Select efmode of EF E/P Mode(all erase)
		pucBuffer[0] = 0x80;
		i2cWriteToIt7236(gl_ts->client, 0xF9, pucBuffer, 1);	// Pump Enable
		mdelay(10);
		pucBuffer[0] = 0xE6;
		i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);	// Write EF CHVPL Mode Cmd
		pucBuffer[0] = 0x00;
		i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);	// Write EF Standby Mode Cmd
	}

	// 6. Page switch to 0
	pucBuffer[0] = 0x00;
	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);

	// 7. Write EF Read Mode Cmd
	pucBuffer[0] = 0x01;
	i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);		// Write EF Standby Mode Cmd

	// 8. Read EF Data, Compare the firmware and input data. for j loop
	for ( j = 0; j < fileSize; j+=128)
	{
		page = ((j & 0x3F00)>>8) + StartPage;					// 3F = 0011 1111, at most 32 pages
		pageres = j % 256;
		pucBuffer[0] = page;
		i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);

		gl_ts->client->addr = 0x7F;
		i2cReadFromIt7236(gl_ts->client, 0x00 + pageres, wTemp, 128); // use 0x7f to read data
		gl_ts->client->addr = i2c_addr_bak;
		// Compare Flash Data
		for( k = 0 ; k < 128 ; k++ )
		{
			if( j+k >= fileSize )
				break;

			pageres = (j + k) % 256;
			OutputDataBuffer[j+k] = wTemp[k];
			WriteDATABuffer[k] = InputBuffer[j+k];

			if(OutputDataBuffer[j+k] != WriteDATABuffer[k])
			{
				Addr = page << 8 | pageres;
				pr_err("Addr: %04x, Expected: %02x, Read: %02x\r\n", Addr, WriteDATABuffer[k], wTemp[k]);
				result = 0;
			}
		}
	}

	if(!result)
	{
		fnFirmwareReinitialize();
		pr_err("[IT7236] Failed to Upgrade Firmware\n\n");
		fw_upgrade_success = 1;
		return -1;
	}

	// 9. Write EF Standby Mode Cmd
	pucBuffer[0] = 0x00;
	i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);
	// 10. Power On Reset
	fnFirmwareReinitialize();

	pr_info("[IT7236] Success to Upgrade Firmware\n\n");

	get_config_ver();
	fw_upgrade_success = 0;
	//enable_irq(gl_ts->client->irq);
	return 0;
}

static ssize_t IT7236_upgrade_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, retry = 3;
	const struct firmware *fw = NULL;
	int fileSize;
	u8* InputBuffer = kzalloc(0x8000, GFP_KERNEL);

	ret = request_firmware( &fw, it7236_fw_name, dev);
	if (ret < 0) {
		pr_err("[IT7236] Open %s failed(%d)\n", it7236_fw_name,ret);
		return -EINVAL;
	}
	else
		memcpy(InputBuffer ,fw->data, fw->size);

	fileSize = fw->size;

	pr_info("[IT7236] Firmware File Version : %#x, %#x, %#x, %#x\n",*(fw->data + 0x0406), *(fw->data + 0x0407), *(fw->data + 0x0408), *(fw->data + 0x0409));
	while(((it7236_upgrade(InputBuffer, fileSize)) != 0) && (retry > 0))
		retry--;

	return count;
}
#if 0
static u32 get_firmware_ver_cmd(void)
{
	char pucBuffer[1];
        u8  wTemp[4];
        u32  fw_version;
		int ret;

	//Wakeup
	pucBuffer[0] = 0x55; 
	i2cWriteToIt7236(gl_ts->client, 0xFB, pucBuffer, 1); 
	mdelay(1);
	waitCommandDone();
	pucBuffer[0] = 0x00;
	ret = i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);
	pucBuffer[0] = 0x80;
	ret = i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);
	pucBuffer[0] = 0x01;
	ret = i2cWriteToIt7236(gl_ts->client, 0x40, pucBuffer, 1);
	pucBuffer[0] = 0x01;
	ret = i2cWriteToIt7236(gl_ts->client, 0x41, pucBuffer, 1);
	pucBuffer[0] = 0x40;
	ret = i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);
	pucBuffer[0] = 0x00;
	ret = i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);
	pucBuffer[0] = 0x80;
	ret = i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);

	waitCommandDone();	

	ret = i2cReadFromIt7236(gl_ts->client, 0x48, wTemp, 4);
	if(ret < 0)
	{
		return ret;
	}
	fw_version = (wTemp[0] << 24) | (wTemp[1] << 16) | (wTemp[2] << 8) | (wTemp[3]);
	//memcpy(fw_version, wTemp, 4);
	pr_info("[IT7236] EF Flash Firmware Version :0x%02x%02x%02x%02x\n", wTemp[0], wTemp[1], wTemp[2], wTemp[3]);

	return fw_version;
}
#endif
#if IT7236_FW_AUTO_UPGRADE
static ssize_t IT7236_upgrade_auto(void)
{
	    int retry = 2;
	   	 	while(((it7236_upgrade((u8 *)rawDATA,sizeof(rawDATA))) != 0) && (retry > 0))
				retry--;

	   return retry;
}
#endif

static ssize_t IT7236_appfwupgrade_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s not ready\n", __func__);
}

static ssize_t IT7236_appfwupgrade_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	pr_err("[IT7236]: %s not ready\n", __func__);
	return count;
}

static ssize_t IT7236_touchcontrol_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	char pucBuffer[1];

	i2cReadFromIt7236(gl_ts->client, IT7236_TOUCH_DATA_REGISTER_ADDRESS, pucBuffer, 1);

	return sprintf(buf, "%d\n", pucBuffer[0]);
}

static ssize_t firmware_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", it7236_fw_name);
}

static ssize_t firmware_name_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if(count > 256) {
		pr_err("%s : input string is too long, count = %d\n", __func__, count);
		return -EINVAL;
	}

	sprintf(it7236_fw_name, "%s", buf);
	it7236_fw_name[count-1] = '\0';

	return count;
}

static DEVICE_ATTR(upgrade, 0644, IT7236_upgrade_show, IT7236_upgrade_store);
static DEVICE_ATTR(appfwupgrade, 0644, IT7236_appfwupgrade_show, IT7236_appfwupgrade_store);
static DEVICE_ATTR(touchcontrol, 0644, IT7236_touchcontrol_show, NULL);
static DEVICE_ATTR(firmware_name, 0644, firmware_name_show, firmware_name_store);

static struct attribute *it7236_attributes[] = {
	&dev_attr_upgrade.attr,
	&dev_attr_appfwupgrade.attr,
	&dev_attr_touchcontrol.attr,
	&dev_attr_firmware_name.attr,
	NULL
};

static const struct attribute_group it7236_attr_group = {
	.attrs = it7236_attributes,
};


//====================================================================================
static int schedule_my_delayed_work(struct delayed_work *dwork,
					unsigned long delay)
{
#ifdef USE_MY_WORKQUEUE //[
	if(!gl_ts) {
		pr_err("[IT7236] it7236 data not ready !\n");
		return -1;
	}
	else {
		return queue_delayed_work(gl_ts->tk_workqueue, dwork, delay);
	}
#else //][!USE_MY_WORKQUEUE
	return schedule_delayed_work(dwork,delay);
#endif //] USE_MY_WORKQUEUE
}

static long ite7236_ioctl(struct file *filp, unsigned int cmd,unsigned long arg)
{
	int retval = 0;
	int i;
	unsigned char buffer[MAX_BUFFER_SIZE];
	struct ioctl_cmd168 data;
	struct i2c_client * i2c_client;
	memset(&data, 0, sizeof(struct ioctl_cmd168));
	
	pr_info("[IT7236] ite7236_ioctl cmd =%d\n",cmd);
	i2c_client=gl_ts->client;

	switch (cmd) {
	case IOCTL_SET:
		pr_info("[IT7236] : =IOCTL_SET=\n");
		disable_irq_nosync(gl_ts->client->irq);
		if (!access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd))) {
			retval = -EFAULT;
			goto done;
		}

		if ( copy_from_user(&data, (int __user *)arg, sizeof(struct ioctl_cmd168)) ) {
			retval = -EFAULT;
			goto done;
		}
		
		for (i = 0; i < data.length; i++) {
			buffer[i] = (unsigned char) data.buffer[i];
		}
        
		retval = i2cWriteToIt7236( i2c_client,
				(unsigned char) data.bufferIndex,
				buffer,
				(unsigned char)data.length );
		break;

	case IOCTL_GET:
		pr_info("[IT7236] : =IOCTL_GET=\n");
		disable_irq_nosync(gl_ts->client->irq);
		if (!access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd))) {
			retval = -EFAULT;
			goto done;
		}

		if (!access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd))) {
			retval = -EFAULT;
			goto done;
		}

		if ( copy_from_user(&data, (int __user *)arg, sizeof(struct ioctl_cmd168)) ) {
			retval = -EFAULT;
			goto done;
		}

		retval = i2cReadFromIt7236(i2c_client,
				(unsigned char) data.bufferIndex,
				(unsigned char*) buffer,
				(unsigned char) data.length);

		for (i = 0; i < data.length; i++) {
			data.buffer[i] = (unsigned short) buffer[i];
		}

		if ( copy_to_user((int __user *)arg, &data, sizeof(struct ioctl_cmd168)) ) {
			retval = -EFAULT;
			goto done;
		}
		break;

	default:
		retval = -ENOTTY;
		break;
	}

done:
	//enable_irq(gl_ts->client->irq);

	return (retval);
}

static int ite7236_open(struct inode *inode, struct file *filp)
{
	int i;
	struct ioctl_cmd168 *dev;
	pr_info("[IT7236] Open Device\n");
	dev = kzalloc(sizeof(struct ioctl_cmd168), GFP_KERNEL);
	if (dev == NULL) {
		return -ENOMEM;
	}

	/* initialize members */
	//rwlock_init(&dev->lock);
	for (i = 0; i < MAX_BUFFER_SIZE; i++) {
		dev->buffer[i] = 0xFF;
	}

	filp->private_data = dev;

	return 0; /* success */
}

static int ite7236_close(struct inode *inode, struct file *filp)
{
	struct ioctl_cmd168 *dev = filp->private_data;
	pr_info("[IT7236] Close Device\n");
	if (dev) {
		kfree(dev);
	}

	return 0; /* success */
}


struct file_operations ite7236_fops = {
	.owner = THIS_MODULE,
	.open = ite7236_open,
	.release = ite7236_close,
	.unlocked_ioctl = ite7236_ioctl,
};

static int get_config_ver(void)
{
	char pucBuffer[1];
	int ret;

	//Wakeup
	pucBuffer[0] = 0x55; 
	i2cWriteToIt7236(gl_ts->client, 0xFB, pucBuffer, 1); 
	mdelay(1);
	waitCommandDone();

	// 1. Request Full Authority of All Registers
	pucBuffer[0] = 0x01;
	i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);

	// 2. Assert Reset of MCU
	pucBuffer[0] = 0x64;
	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);
	pucBuffer[0] = 0x04;
	i2cWriteToIt7236(gl_ts->client, 0x01, pucBuffer, 1);

	// 3. Assert EF enable & reset
	pucBuffer[0] = 0x10;
	i2cWriteToIt7236(gl_ts->client, 0x2B, pucBuffer, 1);
	pucBuffer[0] = 0x00;
	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);

	// 4. Test Mode Enable
	pucBuffer[0] = 0x07;
	gl_ts->client->addr = 0x7F;
	i2cWriteToIt7236(gl_ts->client, 0xF4, pucBuffer, 1);
	gl_ts->client->addr = i2c_addr_bak;

	pucBuffer[0] = 0x04;
	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);

	// 6. Write EF Read Mode Cmd
	pucBuffer[0] = 0x01;
	i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);

	gl_ts->client->addr = 0x7F;
	ret = i2cReadFromIt7236(gl_ts->client, 0x00, wTemp, 10);
	gl_ts->client->addr = i2c_addr_bak;

	// 7. Write EF Standby Mode Cmd
	pucBuffer[0] = 0x00;
	i2cWriteToIt7236(gl_ts->client, 0xF7, pucBuffer, 1);

	// 8. Power On Reset
	fnFirmwareReinitialize();

	memcpy(config_id, wTemp+6, 4);
	pr_info("[IT7236] EF Flash Firmware Version : %#x, %#x, %#x, %#x\n", config_id[0], config_id[1], config_id[2], config_id[3]);

	return ret;
}

static int _ReportTouchKey(struct IT7236_tk_data *ts)
{
	unsigned char pucBuffer[2];
	unsigned short wVal = 0;
	int iRet = 0;
	int iChk;

	if(!ts) {
		pr_err("%s() it7236 tk data not exist !!\n",__func__);
		//dump_stack();
		return 0;
	}

	
	if( EVENT_PENDING_DOWN == giIT7236_event_pending ) {

	
	//TODO:You can change the IT7236_TOUCH_DATA_REGISTER_ADDRESS to others register address.
		//i2cReadFromIt7236(ts->client, IT7236_TOUCH_DATA_REGISTER_ADDRESS, pucBuffer, 1);

#if 1 // [  ITE 建議 : 加入確認page的code .
		iChk = i2cReadFromIt7236(gl_ts->client, 0xf0, pucBuffer, 1);
		if(iChk<0) {
			pr_err("%s() IT7236 read reset reg(0xf0) error(%d) !!\n",__func__,iChk);
		}
		else {
			if(0x00 != pucBuffer[0]) {
				pr_warn("page != 0x00 , force clean it !\n");
				pucBuffer[0] = 0x00;
				i2cWriteToIt7236(ts->client,0xf0,pucBuffer,1);
			}
		}
#endif // ] 
		
		iChk = i2cReadFromIt7236(ts->client, 0x00, pucBuffer, 2);
		if(iChk<0) {
			pr_err("%s() IT7236 read data reg(0x0) error (%d)!!\n",__func__,iChk);
			iRet = -1;
		}
		else {
			wVal = (unsigned short)(pucBuffer[1]<<8|pucBuffer[0]);
			if(0xffff==wVal) {
				pr_err("[IT7236] %s : invalid key down val=0x%04x \n",__func__,wVal);
				iRet = -1;
			}
			else {
				pr_devel("[IT7236] %s : touch key down, val=0x%04x \n",__func__,wVal);
				input_event(ts->input_dev,EV_MSC,MSC_TOUCHPAD,(int)wVal);
				input_sync(ts->input_dev);
				iRet = 1;
			}
		}
	}
	else {
		i2cReadFromIt7236(ts->client, 0x00, pucBuffer, 2);
		wVal = (unsigned short)(pucBuffer[1]<<8|pucBuffer[0]);
		pr_devel("[IT7236] %s : touch key up, val=0x%04x \n",__func__,wVal);
		input_event(ts->input_dev,EV_MSC,MSC_TOUCHPAD,0);
		input_sync(ts->input_dev);
		iRet = 0;
	}

	return iRet;

}
#define IT7632_REPEAT_CHK_MS	100
static void it7632_tk_work_func(struct work_struct *work)
{
	int iChk;
	int iIT7236_current_state = gpio_get_value(gl_ts->irq_gpio)?
			EVENT_PENDING_UP:EVENT_PENDING_DOWN;

	//cancel_delayed_work_sync(&gl_ts->work);

	if(giIT7236_int_1st) {
		giIT7236_int_1st = 0;
	}
	else {
  	giIT7236_event_pending = iIT7236_current_state;
	}
	
	iChk = _ReportTouchKey(gl_ts);
	if(iChk<0) {
		schedule_my_delayed_work(&gl_ts->work,0);
	}
	else if(iChk) {
		schedule_my_delayed_work(&gl_ts->work, msecs_to_jiffies(IT7632_REPEAT_CHK_MS));
	}


}
static irqreturn_t IT7236_isr(int irq, void *dev_id)
{

  giIT7236_event_pending = gpio_get_value(gl_ts->irq_gpio)?
		EVENT_PENDING_UP:EVENT_PENDING_DOWN;
	giIT7236_int_1st = 1;

	pr_devel("[IT7236] %s %d\n",__func__,giIT7236_event_pending);

	//disable_irq_nosync(gl_ts->client->irq);
	cancel_delayed_work_sync(&gl_ts->work);
	//cancel_delayed_work(&gl_ts->work);
	schedule_my_delayed_work(&gl_ts->work, 2);
	//it7632_tk_work_func(&gl_ts->work);
	//enable_irq(gl_ts->client->irq);
	return IRQ_HANDLED;
}

static int IT7236_tk_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct IT7236_tk_data *ts;
	int ret = 0;
	int err;
	dev_t dev = MKDEV(ite7236_major, 0);
	struct device *class_dev = NULL;


	pr_info("[IT7236] enter probe\n");

	if( i2cWriteToIt7236_Ex(client, 0x00, 0, 0,0) < 1 )
	{
		return -ENXIO;
	}

	pr_info("IT7236 Driver Version : %s",DRIVER_VERSION);

	//ts = client->dev.platform_data;
	ts = kmalloc(sizeof(struct IT7236_tk_data),GFP_KERNEL);
	if(!ts) {
		pr_info("%s : malloc driver data (%d) failed \n",__func__,sizeof(struct IT7236_tk_data));
		return -ENOMEM;
	}
	memset(ts,0,sizeof(struct IT7236_tk_data));

	INIT_DELAYED_WORK(&ts->work, it7632_tk_work_func);

	err = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if (err) {
		pr_err("[IT7236] IT7236 cdev can't get major number\n");
		goto err_alloc_dev;
	}
	ite7236_major = MAJOR(dev);

	/*allocate the character device*/
	cdev_init(&ite7236_cdev, &ite7236_fops);
	ite7236_cdev.owner = THIS_MODULE;
	ite7236_cdev.ops = &ite7236_fops;
	err = cdev_add(&ite7236_cdev, MKDEV(ite7236_major, ite7236_minor), 1);
	if(err) {
		goto err_add_dev;
	}

	/*register class*/
	ite7236_class = class_create(THIS_MODULE, DEVICE_NAME);
	if(IS_ERR(ite7236_class)) {
		pr_err("[IT7236]: failed in creating class.\n");
		err = -ENOMEM;
		goto err_create_class;
	}

	ite7236_dev = MKDEV(ite7236_major, ite7236_minor);
	class_dev = device_create(ite7236_class, NULL, ite7236_dev, NULL, DEVICE_NAME);
	if(class_dev == NULL) {
		pr_err("[IT7236]: failed IT7236 in creating device.\n");
		err = -ENOMEM;
		goto err_create_dev;
	}

	input_dev = input_allocate_device();
	if (input_dev == NULL) {
		err = -ENOMEM;
		pr_err("[IT7236]: failed to allocate input device\n");
		goto err_alloc_input_dev;
	}

	input_dev->name = IT7236_I2C_NAME;
	input_dev->id.bustype = BUS_I2C;

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_MSC, input_dev->evbit);
	input_set_capability(input_dev, EV_MSC,MSC_TOUCHPAD );

	err = input_register_device(input_dev);
	if(err) {
		pr_err("[IT7236]: device register error\n");
		goto dev_reg_err;
	}

	mutex_init(&ts->device_mode_mutex);
	ts->client = client;

	i2c_set_clientdata(client, ts);


	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_info("[IT7236] : IT7236_tk_probe: need I2C_FUNC_I2C\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}

	//ts->tk_workqueue = create_singlethread_workqueue(IT7236_I2C_NAME);
	ts->tk_workqueue = alloc_workqueue(IT7236_I2C_NAME,WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_HIGHPRI,1);
	if (!ts->tk_workqueue) {
		pr_err("[IT7236] : IT7236_tk_probe: workqueue creating failed !\n");
		err = -ENOMEM;
		goto err_create_queue;
	}

	spin_lock_init(&it7236_lock);
	ts->input_dev = input_dev;


	// TODO: Change your GPIO interrupt setting here.
	GPIO_Init();	
	//ts->irq_gpio = S5PV210_GPH2(0);
	ts->irq_gpio = (int)client->dev.platform_data;
	ts->client->irq = gpio_to_irq(ts->irq_gpio);

	gl_ts = ts;

	if (ts->irq_gpio) {
		pr_info("[IT7236] : irq = %d , gpio = %d\n",gpio_to_irq(ts->irq_gpio), ts->irq_gpio);
		if(IMX_GPIO_NR(4, 14)==ts->irq_gpio) {
			pr_info("IT7236_INT is GP4_14\n");
		}

		ret = request_threaded_irq(gpio_to_irq(ts->irq_gpio), NULL,
				   IT7236_isr,IRQF_TRIGGER_FALLING |IRQF_TRIGGER_RISING |IRQF_ONESHOT,
				   IT7236_I2C_NAME, ts);
		if (ret == 0){
			ts->use_irq = 1;
		}
		else {
			dev_err(&client->dev, "[IT7236] : Request IRQ Failed\n");
			err = -EIO;
			goto err_request_irq;  
		}
	}else{
		pr_info("[IT7236] : FAIL irq = %d , gpio = %d\n",gpio_to_irq(ts->irq_gpio), ts->irq_gpio);
	}
	//disable_irq_nosync(gl_ts->client->irq);// disable irq
	

#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING - 1;
	ts->early_suspend.suspend = IT7236_tk_early_suspend;
	ts->early_suspend.resume = IT7236_tk_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif

	ret = sysfs_create_group(&input_dev->dev.kobj, &it7236_attr_group);
	if (ret) {
		pr_err("[IT7236] : sysfs_create_group: Error to create calibration attribute\n");
		goto err_sysfs_create_group;
	}

	ret = sysfs_create_link(input_dev->dev.kobj.parent, &input_dev->dev.kobj, "touchkey");
	if(ret)
	{
		pr_err("[IT7236] : sysfs_create_link failed create [touchkey] link\n");
		goto err_sysfs_create_link;
	}
	i2c_addr_bak = ts->client->addr;
	#if IT7236_FW_AUTO_UPGRADE
	IT7236_upgrade_auto();
	#endif
	get_config_ver();

	return 0;

err_sysfs_create_link:
	sysfs_remove_group(&input_dev->dev.kobj, &it7236_attr_group);
err_sysfs_create_group:
	free_irq(client->irq, ts);
err_request_irq:
	destroy_workqueue(ts->tk_workqueue);
err_create_queue:
err_check_functionality_failed:
dev_reg_err:
	input_free_device(input_dev);
err_alloc_input_dev:
	device_destroy(ite7236_class, ite7236_dev);
err_create_dev:
	class_destroy(ite7236_class);
err_create_class:
	cdev_del(&ite7236_cdev);
err_add_dev:
err_alloc_dev:
	if(ts) {
		kfree(ts);
	}
	return ret;

}



static int IT7236_tk_remove(struct i2c_client *client)
{
	sysfs_remove_group(&input_dev->dev.kobj, &it7236_attr_group);
	destroy_workqueue(gl_ts->tk_workqueue);
	input_unregister_device(input_dev);
	input_free_device(input_dev);
	device_destroy(ite7236_class, ite7236_dev);
	class_destroy(ite7236_class);
	cdev_del(&ite7236_cdev);
	devm_kfree(&client->dev, gl_ts);

	return 0;
}
//#define IT7236_SUSPEND_TEST		1
int IT7236_check_can_suspend(void)
{
	if(!gl_ts) {
		return 1;
	}

	if(0==gpio_get_value(gl_ts->irq_gpio)) {
		pr_err("%s() : touch pad pressing !\n",__func__);
		return 0;
	}

	if(EVENT_PENDING_DOWN==giIT7236_event_pending) {
		pr_err("%s() : isr is pending !\n",__func__);
		return 0;
	}

	return 1;
}
static int IT7236_tk_suspend(struct i2c_client *client, pm_message_t mesg)
{
	char pucBuffer[3],data[2];
	int count=0;

	printk(KERN_DEBUG"%s enter,int=%d \n",__func__,gpio_get_value(gl_ts->irq_gpio));

	if(delayed_work_pending(&gl_ts->work)) {
		pr_err("%s() : event work pending !\n",__func__);
		return -1;
	}
	
	if(0==gpio_get_value(gl_ts->irq_gpio)) {
		pr_err("%s() : touch pad pressing !\n",__func__);
		return -1;
	}

	if(EVENT_PENDING_DOWN==giIT7236_event_pending) {
		pr_err("%s() : isr is pending !\n",__func__);
		return -1;
	}

#ifdef IT7236_SUSPEND_TEST //[
	//Wakeup
	pucBuffer[0] = 0x55; 
	i2cWriteToIt7236(gl_ts->client, 0xFB, pucBuffer, 1); 
	mdelay(1);	
	// disable_IRQ
	//disable_irq_nosync(gl_ts->client->irq);
	
	do{
	  	i2cReadFromIt7236(gl_ts->client, 0xFA, data, 2);
	  	pucBuffer[0] = 0x00;
	  	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);//set Pate to Page0
	  	
		pucBuffer[0] = 0x80;
		i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);//set 0XF1 bit7 t0 1
	      	      
		i2cReadFromIt7236(gl_ts->client, 0xFA, data, 2);
		count++;
	} while( ((data[0]&0X01) != 0x01) && (count < 5));//0xFA  bit0  =1?
	
        pucBuffer[0] = 0x01;//C/R
     	pucBuffer[1] = 0x30;//CMD=0x30,Set power mode
    	pucBuffer[2] = 0x01;//sub cmd =0x02,idle mode
	i2cWriteToIt7236(gl_ts->client, 0x40, pucBuffer, 3);//write commond buffer at 0X40
             
	pucBuffer[0] = 0x40;
	i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);//set Pate to Page0
	  	
	i2cReadFromIt7236(gl_ts->client, 0xF3, data, 2);
	  	
	pucBuffer[0] = 0x00;
	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);//set Pate to Page0
	  	  	
	pucBuffer[0] = 0x80;
	i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);//set 0XF1 bit7 t0 1
	   	  	
	i2cReadFromIt7236(gl_ts->client, 0xFA, data, 2);
#endif//] IT7236_SUSPEND_TEST
	//enable_irq_wake(client->irq);
	return 0;	
}

static int IT7236_tk_resume(struct i2c_client *client)
{
	char pucBuffer[2],data[2];
	int count=0;

	printk(KERN_DEBUG"%s enter \n",__func__);


	if(!gpio_get_value(gl_ts->irq_gpio)) {
  	giIT7236_event_pending = EVENT_PENDING_DOWN;
		giIT7236_int_1st = 1;
	}

	GPIO_Init();

#ifdef IT7236_SUSPEND_TEST //[
	pucBuffer[0] = 0x55; 
	i2cWriteToIt7236(gl_ts->client, 0xFB, pucBuffer, 1); 
	mdelay(1);
	do{
	  	i2cReadFromIt7236(gl_ts->client, 0xFA, data, 2);
	  	
	  	pucBuffer[0] = 0x00;
	  	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);//set Pate to Page0
	  	
		pucBuffer[0] = 0x80;
		i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);//set 0XF1 bit7 t0 1
	      	      
		i2cReadFromIt7236(gl_ts->client, 0xFA, data, 2);
		count++;
	} while( ((data[0]&0X01) != 0x01) && (count < 5));//0xFA  bit0  =1?

	pucBuffer[0] = 0x01;//C/R
 	pucBuffer[1] = 0xF0;//CMD=0x30,Set power mode
	i2cWriteToIt7236(gl_ts->client, 0x40, pucBuffer,2);//write commond buffer at 0X40

	pucBuffer[0] = 0x40;
  	i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);//set Pate to Page0
  		
	pucBuffer[0] = 0x00;
  	i2cWriteToIt7236(gl_ts->client, 0xF0, pucBuffer, 1);//set Pate to Page0
  	
	pucBuffer[0] = 0x80;
	i2cWriteToIt7236(gl_ts->client, 0xF1, pucBuffer, 1);//set 0XF1 bit7 t0 1
  	
	i2cReadFromIt7236(gl_ts->client, 0xFA, data, 2);
#endif //] IT7236_SUSPEND_TEST

	if( EVENT_PENDING_DOWN == giIT7236_event_pending ) {
		//schedule_my_delayed_work(&gl_ts->work, 2);
		it7632_tk_work_func(&gl_ts->work);
	}
	
	//enable_irq(gl_ts->client->irq);
	//disable_irq_wake(client->irq);

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void IT7236_tk_early_suspend(struct early_suspend *h)
{
	struct IT7236_tk_data *ts;

	//pr_info("[IT7236] : %s \n", __func__);

	ts = container_of(h, struct IT7236_tk_data, early_suspend);
	IT7236_tk_suspend(ts->client, PMSG_SUSPEND);
}

static void IT7236_tk_late_resume(struct early_suspend *h)
{
	struct IT7236_tk_data *ts;
	//pr_info("[IT7236] : %s \n", __func__);

	ts = container_of(h, struct IT7236_tk_data, early_suspend);

	input_sync(ts->input_dev);

	IT7236_tk_resume(ts->client);
}
#endif

static const struct i2c_device_id IT7236_tk_id[] = {
	{ IT7236_I2C_NAME, 0 },
	//{ "ite,7236", 0 },
	{ },
};

static struct of_device_id it7236_match_table[] = {
	//{ .compatible = "ite,7236",},
	{ .compatible = IT7236_I2C_NAME,},
	{ },
};

static int it7236_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	return IT7236_tk_suspend(client, PMSG_SUSPEND);
}
static int it7236_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	return IT7236_tk_resume(client);
}

#ifdef CONFIG_PM
static const struct dev_pm_ops it7236_pm_ops = {
//#ifndef CONFIG_HAS_EARLYSUSPEND
		.suspend = it7236_i2c_suspend,
		.resume = it7236_i2c_resume,
//#endif
};
#endif

static struct i2c_driver IT7236_tk_driver = {
//	.class = I2C_CLASS_HWMON,
	.probe = IT7236_tk_probe,
	.remove = IT7236_tk_remove,
	.id_table = IT7236_tk_id,
	.driver = {
		.owner = THIS_MODULE,
		.name = IT7236_I2C_NAME,
		//.of_match_table = it7236_match_table,
#ifdef CONFIG_PM
		.pm = &it7236_pm_ops,
#endif
	},
};

static int __init IT7236_tk_init(void)
{
	//pr_info("info: %s()\n",__func__);
	return i2c_add_driver(&IT7236_tk_driver);
}

static void __exit IT7236_tk_exit(void)
{
	i2c_del_driver(&IT7236_tk_driver);
}

module_init( IT7236_tk_init);
module_exit( IT7236_tk_exit);

MODULE_DESCRIPTION("ITE IT723x TouchKey Driver");
MODULE_LICENSE("GPL");
