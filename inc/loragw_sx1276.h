#ifndef _LORAGW_SX1276_H
#define _LORAGW_SX1276_H

#include <stdint.h>
int lgw_sx127x_reg_w(uint8_t address, uint8_t reg_value);
void reset_sx1276();
int lgw_sx127x_reg_r(uint8_t address, uint8_t *reg_value);

#endif