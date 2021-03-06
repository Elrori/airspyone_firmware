/*
 * Copyright 2012 Jared Boone
 * Copyright 2013-2015 Benjamin Vernoux <bvernoux@airspy.com>
 * Copyright 2015 Ian Gilmour <ian@sdrsharp.com>
 *
 * This file is part of AirSpy (based on HackRF project).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>

#include <libopencm3/lpc43xx/cgu.h>
#include <libopencm3/lpc43xx/i2c.h>
#include <libopencm3/lpc43xx/gpio.h>
#include <libopencm3/lpc43xx/m0/nvic.h>
#include <libopencm3/lpc43xx/creg.h>
#include <libopencm3/lpc43xx/rgu.h>

#include <airspy_core.h>
#include <si5351c.h>
#include <r820t.h>
#include <w25q80bv.h>
#include <rom_iap.h>
#include <signal_mcu.h>

#include "usb.h"
#include "usb_standard_request.h"

#include "usb_device.h"
#include "usb_endpoint.h"
#include "usb_descriptor.h"

#include "airspy_conf.h"
#include "airspy_usb_req.h"
#include "airspy_commands.h"
#include "airspy_rx.h"
#include "r820t.h"
#include "airspy_m0.hdr"

extern uint32_t cm4_data_share; /* defined in linker script */
extern uint32_t cm0_data_share; /* defined in linker script */

volatile unsigned int phase = 0;

volatile uint32_t *usb_bulk_buffer_offset = (&cm4_data_share);
volatile uint32_t *usb_bulk_buffer_length = ((&cm4_data_share)+1);
volatile uint32_t *last_offset_m0 = ((&cm4_data_share)+2);

volatile airspy_mcore_t *start_adchs = (airspy_mcore_t *)(&cm0_data_share);
volatile airspy_mcore_t *set_samplerate = (airspy_mcore_t *)((&cm0_data_share)+1);
volatile airspy_mcore_t *set_packing = (airspy_mcore_t *)((&cm0_data_share)+2);

#define get_usb_buffer_offset() (usb_bulk_buffer_offset[0])
#define get_usb_buffer_length() (usb_bulk_buffer_length[0])

#define MASTER_TXEV_FLAG  ((uint32_t *) 0x40043130)
#define MASTER_TXEV_QUIT()  { *MASTER_TXEV_FLAG = 0x0; }

uint8_t* const usb_bulk_buffer = (uint8_t*)0x20004000;

const char version_string[] = " " AIRSPY_FW_GIT_TAG " " AIRSPY_FW_CHECKIN_DATE;

typedef struct {
  uint32_t freq_hz;
} set_freq_params_t;

set_freq_params_t set_freq_params;

typedef struct {
  uint32_t freq_hz;
  uint32_t divider;
} set_sample_r_params_t;

set_sample_r_params_t set_sample_r_params;

__attribute__ ((always_inline)) static inline void start_stop_adchs_m4(uint8_t conf_num, uint8_t command)
{
  start_adchs->conf = conf_num;
  start_adchs->cmd = command;

  signal_sev();

  /* Wait until M4 have finished executing the command (it set the data to 0) */
  while(1)
  {
    if(start_adchs->raw == 0)
      break;
  }
}

void set_samplerate_m4(uint8_t conf_num)
{
  set_samplerate->conf = conf_num;
  set_samplerate->cmd = SET_SAMPLERATE_CMD;

  signal_sev();

  /* Wait until M4 have finished executing the command (it set the data to 0) */
  while(1)
  {
    if(set_samplerate->raw == 0)
      break;
  }
}

void set_packing_m4(uint8_t state)
{
  set_packing->conf = state;
  set_packing->cmd = SET_PACKING_CMD;
  
  signal_sev();
  
  while(1)
  {
    if(set_packing->raw == 0)
      break;
  }
}

void usb_configuration_changed(usb_device_t* const device)
{
  if( device->configuration->number )
  {
    /* RECEIVER ON */
    set_receiver_mode(get_receiver_mode());
  } else
  {
    /* RECEIVER OFF */
    /* Configuration number equal 0 means usb bus reset. */
    set_receiver_mode(RECEIVER_MODE_OFF);
  }
}

void ADCHS_start(uint8_t conf_num)
{
  start_stop_adchs_m4(conf_num, START_ADCHS_CMD);

  //enable_r820t_power();

  /* Re-Init I2C0 & I2C1 after PLL1 frequency is modified (for I2C1 also because PowerOn on R820T) */
  i2c0_init(airspy_conf->i2c_conf.i2c0_pll1_ls_hs_conf_val); /* Si5351C I2C peripheral */
  i2c1_init(airspy_conf->i2c_conf.i2c1_pll1_hs_conf_val); /* R820T I2C peripheral */

  if((conf_num & AIRSPY_SAMPLERATE_CONF_ALT) == AIRSPY_SAMPLERATE_CONF_ALT)
  {
    conf_num = conf_num & (~AIRSPY_SAMPLERATE_CONF_ALT);
    r820t_init(&airspy_conf->r820t_conf_rw, airspy_conf->airspy_m0_m4_alt_conf[conf_num].airspy_m0_conf.r820t_if_freq);
    r820t_set_if_bandwidth(&airspy_conf->r820t_conf_rw, airspy_conf->airspy_m0_m4_alt_conf[conf_num].airspy_m0_conf.r820t_if_bw);
  }else
  {
    r820t_init(&airspy_conf->r820t_conf_rw, airspy_conf->airspy_m0_m4_conf[conf_num].airspy_m0_conf.r820t_if_freq);
    r820t_set_if_bandwidth(&airspy_conf->r820t_conf_rw, airspy_conf->airspy_m0_m4_conf[conf_num].airspy_m0_conf.r820t_if_bw);
  }
  phase = 1;
}

void ADCHS_stop(uint8_t conf_num)
{
  r820t_standby();
  start_stop_adchs_m4(conf_num, STOP_ADCHS_CMD);

  /* Re-Init I2C0 & I2C1 after PLL1 frequency is modified */
  i2c0_init(airspy_conf->i2c_conf.i2c0_pll1_ls_hs_conf_val); /* Si5351C I2C peripheral */
  i2c1_init(airspy_conf->i2c_conf.i2c1_pll1_ls_conf_val); /* R820T I2C peripheral */
}

/***************************/
/* adchs_isr managed by M4 */
/***************************/
void m4core_isr(void)
{
  MASTER_TXEV_QUIT();
}

/*
M0 Core Manage USB 
*/
int main(void)
{
  iap_cmd_res_t iap_cmd_res;
  usb_descriptor_serial_number_t serial_number;
  airspy_usb_req_init();

  /* R820T Startup */
  r820t_startup(&airspy_conf->r820t_conf_rw);

  usb_set_configuration_changed_cb(usb_configuration_changed);
  usb_peripheral_reset();

  usb_device_init(0, &usb_device);

  usb_queue_init(&usb_endpoint_control_out_queue);
  usb_queue_init(&usb_endpoint_control_in_queue);
  usb_queue_init(&usb_endpoint_bulk_out_queue);
  usb_queue_init(&usb_endpoint_bulk_in_queue);

  usb_endpoint_init(&usb_endpoint_control_out);
  usb_endpoint_init(&usb_endpoint_control_in);

  /* Read IAP Serial Number Identification */
  iap_cmd_res.cmd_param.command_code = IAP_CMD_READ_SERIAL_NO;
  iap_cmd_call(&iap_cmd_res);
  if(iap_cmd_res.status_res.status_ret == CMD_SUCCESS)
  {
    /* Only retrieve 2 last 32bits for Serial Number */
    serial_number.sn_32b[0] = iap_cmd_res.status_res.iap_result[2];
    serial_number.sn_32b[1] = iap_cmd_res.status_res.iap_result[3];
    usb_descriptor_fill_string_serial_number(serial_number);
  }

  nvic_set_priority(NVIC_USB0_IRQ, 255);
  
  nvic_set_priority(NVIC_M4CORE_IRQ, 1);
  nvic_enable_irq(NVIC_M4CORE_IRQ);

  usb_run(&usb_device);

  while(true)
  {
    signal_wfe();

    uint32_t offset = get_usb_buffer_offset();
    uint32_t length = get_usb_buffer_length();

    if(offset != *last_offset_m0)
    {
      usb_transfer_schedule_block(&usb_endpoint_bulk_in, &usb_bulk_buffer[offset], length);
      *last_offset_m0 = offset;
    }
  }
}
