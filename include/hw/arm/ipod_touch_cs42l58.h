#ifndef HW_CS42L58_H
#define HW_CS42L58_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"

#define TYPE_CS42L58                 "cs42l58"
#define TYPE_CS42L59                 "cs42l59"   /* + power-down status */
#define TYPE_CS42L63                 "cs42l63"   /* + its own power-down status */
OBJECT_DECLARE_SIMPLE_TYPE(CS42L58State, CS42L58)

typedef struct CS42L58State {
	I2CSlave i2c;
	uint32_t cmd;
	bool have_cmd;
	bool autoinc;
	uint8_t regs[128];
	bool pdn_status;     /* CS42L59: 0x38 bit 3 reports power-down done */
	bool pdn_status63;   /* CS42L63: 0x62 bit 7, once 0x06 bit 0 (PDN) is set */
	Clock *lrclk;
} CS42L58State;

#endif
