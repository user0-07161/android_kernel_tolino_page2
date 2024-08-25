#ifndef IT7236_TOUCHKEY_H
#define IT7236_TOUCHKEY_H

#define MAX_BUFFER_SIZE		144
#define DEVICE_NAME		"IT7236"
#define DEVICE_VENDOR		0
#define DEVICE_PRODUCT		0
#define DEVICE_VERSION		0
#define VERSION_ABOVE_ANDROID_20
#define IOC_MAGIC		'd'
#define IOCTL_SET 		_IOW(IOC_MAGIC, 1, struct ioctl_cmd168)
#define IOCTL_GET 		_IOR(IOC_MAGIC, 2, struct ioctl_cmd168)

struct ioctl_cmd168 {
	unsigned short bufferIndex;
	unsigned short length;
	unsigned short buffer[MAX_BUFFER_SIZE];
};
struct IT7236_tk_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	int use_irq;
#ifdef CONFIG_HAS_EARLYSUSPEND	
	struct early_suspend early_suspend;
#endif	
	struct workqueue_struct *tk_workqueue;
	struct delayed_work	work;
	struct mutex device_mode_mutex;
	u32 irq_gpio;
};


#endif
