/* include/linux/max17048_tweaks.h */

#ifndef _LINUX_MAX17048_TWEAKS_H
#define _LINUX_MAX17048_TWEAKS_H

#define VBT_MAX_MV  4400
#define VBT_MIN_MV  3504
#define VBT_STEP_MV  16

int get_max_voltage_mv(void);
void set_max_voltage_mv(int data);
int get_full_soc(void);
void set_full_soc(int full_soc_in);
int get_fcc_mah(void);
void set_fcc_mah(int data);

#endif