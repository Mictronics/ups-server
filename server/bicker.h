// Part of UPS Server.
// A websocket server reading data from a Bicker PSZ-1063 uExtension module
// in combination with a Bicker UPS.
//
// Copyright (c) 2023 Michael Wolf <michael@mictronics.de>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef BICKER_H
#define BICKER_H

#include <stdbool.h>

#define SERIAL_TIMEOUT 3000 // ms

#define BICKER_SOH 0x01 // Start of header
#define BICKER_EOT 0x04 // End of transmission
#define BICKER_CMD_INDEX3 0x03
#define BICKER_CMD_INDEX1 0x01
#define BICKER_REQ_LEN 0x03

/**
 * Know commands for UPSIC-1205 + PSZ1063
 */
typedef enum
{
    GET_CHARGE_STATUS_REGISTER = 0X1B,  // cmd index 0x03
    GET_MONITOR_STATUS_REGISTER = 0X1C, // cmd index 0x03
    GET_CAPACITY = 0X1E,                // cmd index 0x03
    GET_ESR = 0X1F,                     // cmd index 0x03
    GET_INPUT_VOLTAGE = 0x25,           // cmd index 0x03
    GET_CAP_STACK_VOLTAGE = 0x26,       // cmd index 0x03
    GET_OUTPUT_VOLTAGE = 0x27,          // cmd index 0x03
    GET_INPUT_CURRENT = 0x28,           // cmd index 0x03
    GET_CHARGE_CURRENT = 0x29,          // cmd index 0x03
    GET_VCAP1_VOLTAGE = 0x20,           // cmd index 0x03
    GET_VCAP2_VOLTAGE = 0x21,           // cmd index 0x01 + 0x03
    GET_VCAP3_VOLTAGE = 0x22,           // cmd index 0x03
    GET_VCAP4_VOLTAGE = 0x23,           // cmd index 0x03
    START_CAP_ESR_MEASUREMENT = 0x31,   // cmd index 0x03
    GET_DEVICE_STATUS = 0x40,           // cmd index 0x01
    GET_INPUT_VOLTAGE1 = 0x41,          // cmd index 0x01
    GET_INPUT_CURRENT1 = 0x42,          // cmd index 0x01
    GET_OUTPUT_VOLTAGE1 = 0x43,         // cmd index 0x01
    GET_OUTPUT_CURRENT1 = 0x44,         // cmd index 0x01
    GET_BATTERY_VOLTAGE = 0x45,         // cmd index 0x01
    GET_BATTERY_CURRENT = 0x46,         // cmd index 0x01
    GET_SOC = 0x47,                     // cmd index 0x01
    GET_BATTERY_TEMPERATURE = 0x4A,     // cmd index 0x01
    GET_MANUFACTURER = 0x60,            // cmd index 0x01
    GET_SERIAL = 0x61,                  // cmd index 0x01
    GET_SERIES = 0x62,                  // cmd index 0x01
    GET_FIRMWARE = 0x63,                // cmd index 0x01
    GET_BATTERY_TYPE = 0x64,            // cmd index 0x01
    GET_UC_TEMPERATURE = 0x66,          // cmd index 0x01
    GET_HARDWARE_REVISION = 0x67,       // cmd index 0x01
} cmd_list_t;

/**
 * Data frame
 */
typedef struct __attribute__((__packed__))
{
    unsigned char soh;
    unsigned char size;
    unsigned char cmd_index;
    unsigned char cmd_list;
    unsigned char data[250 + 1];
} bicker_data_t;

/**
 * Battery cell voltage from vcap commands
 */
typedef struct
{
    signed int cap1;
    signed int cap2;
    signed int cap3;
    signed int cap4;
} bicker_vcap_t;

/**
 * Device status
 */
typedef union
{
    unsigned char value;
    struct
    {
        // LSB
        bool is_charging : 1;
        bool is_discharging : 1;
        bool is_power_present : 1;
        bool is_battery_present : 1;
        bool is_shutdown_set : 1;
        bool is_over_current : 1;
        bool reserve1 : 1;
        bool reserve2 : 1;
        // MSB
    } reg;
} bicker_device_status_t;

/**
 * Charge status register
 */
typedef union
{
    signed int value;
    struct
    {
        // LSB
        bool is_step_down : 1;
        bool is_step_up : 1;
        bool is_constant_voltage : 1;
        bool is_under_voltage : 1;
        bool is_current_limit : 1;
        bool is_power_good : 1;
        bool is_shunting : 1;
        bool is_balancing : 1;
        bool is_cap_measurement : 1;
        bool is_constant_current : 1;
        bool reserve10 : 1;
        bool is_power_fail : 1;
        bool reserve12 : 1;
        bool reserve13 : 1;
        bool reserve14 : 1;
        bool reserve15 : 1;
        // MSB
    } reg;
    struct
    {
        signed int reserve : 16;
    } reserved;
} bicker_charge_status_t;

/**
 * Monitor status register
 */
typedef union
{
    signed int value;
    struct
    {
        // LSB
        bool is_esr_measuring : 1;
        bool is_esr_waiting : 1;
        bool is_waiting_condition : 1;
        bool is_capacity_complete : 1;
        bool is_esr_complete : 1;
        bool is_last_cap_fail : 1;
        bool is_last_esr_fail : 1;
        bool reserve7 : 1;
        bool is_power_fail : 1;
        bool is_power_recovery : 1;
        bool reserve10 : 1;
        bool reserve11 : 1;
        bool reserve12 : 1;
        bool reserve13 : 1;
        bool reserve14 : 1;
        bool reserve15 : 1;
        // MSB
    } reg;
    struct
    {
        signed int reserve : 16;
    } reserved;
} bicker_monitor_status_t;

/**
 * UPS parameter collection
 */
typedef struct
{
    signed int input_voltage;
    signed int output_voltage;
    signed int input_current;
    signed int output_current;
    signed int charge_current;
    signed int battery_voltage;
    signed int battery_current;
    signed int capacity;
    signed int esr;
    signed int soc;
    signed int uc_temperature;
    bicker_vcap_t vcap_voltage;
    bicker_charge_status_t charge_status;
    bicker_monitor_status_t monitor_status;
    bicker_device_status_t device_status;
    char battery_type[20];
    char series[20];
    char firmware[20];
    char hw_revision[20];
} bicker_ups_status_t;

void close_serial(void);
int open_serial(void);
void set_serial_interface(const char *dname);
bool is_serial_error();
bicker_ups_status_t *get_ups_status();
void start_cap_esr_measurement();

#endif /* BICKER_H */