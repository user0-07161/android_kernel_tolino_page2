#ifndef __LINUX_NXP_PN7462_H
#define __LINUX_NXP_PN7462_H

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

#endif /* __LINUX_NXP_PN7462_H */
