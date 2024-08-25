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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define PN7462_MAGIC	0xE9

/*
 * PN7462 power control via ioctl
 * PN7462_SET_PWR(0): power off
 * PN7462_SET_PWR(1): power on
 * PN7462_SET_PWR(2): reset and power on with firmware download enabled
 */

#define PWR_OFF 0
#define PWR_ON  1
#define PWR_FW  2

#define CLK_OFF 0
#define CLK_ON  1

#define GPIO_UNUSED -1

#define PN7462_SET_PWR	_IOW(PN7462_MAGIC, 0x01, unsigned int)
#define PN7462_CLK_REQ	_IOW(PN7462_MAGIC, 0x02, unsigned int)
#define PN7462_LED1	_IOW(PN7462_MAGIC, 0x03, unsigned int)
#define PN7462_LED2	_IOW(PN7462_MAGIC, 0x04, unsigned int)
#define PN7462_LED3	_IOW(PN7462_MAGIC, 0x05, unsigned int)
#define PN7462_LED4	_IOW(PN7462_MAGIC, 0x06, unsigned int)
#define PN7462_SLED1	_IOW(PN7462_MAGIC, 0x07, unsigned int)
#define PN7462_SLED2	_IOW(PN7462_MAGIC, 0x08, unsigned int)

struct pn7462_i2c_platform_data {
	unsigned int irq_gpio;
	unsigned int ven_gpio;
	unsigned int firm_gpio;
	unsigned int clkreq_gpio;
    unsigned int led1_gpio;
    unsigned int led2_gpio;
    unsigned int led3_gpio;
    unsigned int led4_gpio;
	unsigned int sled1_gpio;
	unsigned int sled2_gpio;	
	struct regulator *pvdd_reg;
	struct regulator *vbat_reg;
	struct regulator *pmuvcc_reg;
	struct regulator *sevdd_reg;
};
