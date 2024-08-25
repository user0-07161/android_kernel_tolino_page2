
#ifndef __LINUX_S1V3S3XX_H
#define __LINUX_S1V3S3XX_H

#define S1V3S3XX_NAME "s1v3s3xx"

struct s1v3s3xx_platform_data {
	unsigned gpio_TTS_INT;
	unsigned gpio_TTS_RST;
	unsigned gpio_TTS_STB;
	unsigned gpio_TTS_CS;
	unsigned gpio_AMP;
	unsigned gpio_BOOST;
	unsigned char bReserved[4];
};

#endif /* __LINUX_S1V3S3XX_H */

