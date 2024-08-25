#ifndef __LINUX_NXP_PN5XX_H
#define __LINUX_NXP_PN5XX_H

struct pn544_i2c_platform_data {
	unsigned int irq_gpio;
	unsigned int ven_gpio;
	unsigned int firm_gpio;
	unsigned int clkreq_gpio;
	struct regulator *pvdd_reg;
	struct regulator *vbat_reg;
	struct regulator *pmuvcc_reg;
	struct regulator *sevdd_reg;
};

#endif /* __LINUX_NXP_PN5XX_H */