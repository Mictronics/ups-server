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

#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "apc.h"

#define APC_RECORD_COUNT 25

static FILE *apcout;
static size_t apcstr_size = 0;
static char *apcstr = NULL;
static char hostname[256];
static bool has_hostname = false;
static bool has_config = false;
static double nominal_input_voltage = 0.0;
static double nominal_battery_voltage = 0.0;
static int power_return_percent = 0;
static int max_backup_time = 0;
static int wakeup_delay = 0;

/**
 * Create apcupsd compatible status report in memory.
 */
void apc_update_status(bicker_ups_status_t *ups, config_t *cfg, unsigned int shutdown_delay)
{
    // Free previous APC report memory if any
    if (apcstr != NULL)
    {
        free(apcstr);
    }
    // Open new report file in memory
    apcout = open_memstream(&apcstr, &apcstr_size);
    if (apcout == NULL)
    {
        lwsl_warn("Creating APC status stream failed.");
        return;
    }

    if (!has_hostname)
    {
        memset(hostname, 0, sizeof hostname);
        gethostname(hostname, sizeof hostname);
        has_hostname = true;
    }

    if (!has_config)
    {
        config_setting_t *setting = NULL;
        setting = config_lookup(cfg, "ups");
        if (setting != NULL)
        {
            config_lookup_float(cfg, "ups.inputVoltage", &nominal_input_voltage);
            config_lookup_float(cfg, "ups.batteryVoltage", &nominal_battery_voltage);
            config_lookup_int(cfg, "ups.powerReturnPercent", &power_return_percent);
            config_lookup_int(cfg, "ups.maxBackupTime", &max_backup_time);
            config_lookup_int(cfg, "ups.wakeupDelay", &wakeup_delay);
            has_config = true;
        }
        else
        {
            lwsl_err("Server settings not found in configuration file.\n");
        }
    }

    char tstr[50];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(tstr, sizeof tstr, "%F %T %z", t);

    // Create dummy header, file size yet unknown
    fprintf(apcout, "APC      : 001,%03u,0000\n", APC_RECORD_COUNT);
    fprintf(apcout, "DATE     : %.50s\n", tstr);
    fprintf(apcout, "HOSTNAME : %s\n", hostname);
    fprintf(apcout, "UPSNAME  : %.20s\n", ups->series);
    fprintf(apcout, "MODEL    : %.20s\n", ups->battery_type);
    fprintf(apcout, "FIRMWARE : %.20s\n", ups->firmware);
    fprintf(apcout, "STATUS   : ");
    if (ups->device_status.reg.is_charging)
    {
        fprintf(apcout, "ONLINE\n");
    }
    else if (ups->device_status.reg.is_discharging)
    {
        fprintf(apcout, "ONBATT\n");
    }
    else
    {
        fprintf(apcout, "OFFLINE\n");
    }
    fprintf(apcout, "LINEFAIL : ");
    if (ups->device_status.reg.is_power_present)
    {
        fprintf(apcout, "No\n");
    }
    else
    {
        fprintf(apcout, "Yes\n");
    }
    fprintf(apcout, "LINEV    : %.1f Volts\n", ups->input_voltage / 1000.0);
    fprintf(apcout, "LINEA    : %.3f Amps\n", ups->input_current / 1000.0);
    fprintf(apcout, "OUTPUTV  : %.1f Volts\n", ups->output_voltage / 1000.0);
    fprintf(apcout, "OUTPUTA  : %.3f Amps\n", ups->output_current / 1000.0);
    fprintf(apcout, "BATTV    : %.1f Volts\n", ups->battery_voltage / 1000.0);
    fprintf(apcout, "BATTA    : %.3f Amps\n", ups->battery_current / 1000.0);
    fprintf(apcout, "BCHARGE  : %u Percent\n", ups->soc);
    fprintf(apcout, "ITEMP    : %d C\n", ups->uc_temperature);
    fprintf(apcout, "DSHUTD   : %u Seconds\n", shutdown_delay);
    fprintf(apcout, "DWAKE    : %u Seconds\n", wakeup_delay);
    fprintf(apcout, "MAXTIME  : %u Seconds\n", max_backup_time);
    fprintf(apcout, "RETPCT   : %u Percent\n", power_return_percent);
    fprintf(apcout, "STATFLAG : 0x%02X\n", ups->device_status.value);
    fprintf(apcout, "REG2     : 0x%04X\n", ups->charge_status.value);
    fprintf(apcout, "REG3     : 0x%04X\n", ups->monitor_status.value);
    fprintf(apcout, "NOMINV   : %.1f Volts\n", nominal_input_voltage);
    fprintf(apcout, "NOMBATTV : %.1f Volts\n", nominal_battery_voltage);
    fprintf(apcout, "ENDAPC   : %.50s\n", tstr);
    fflush(apcout);           // Update apcstr and apcstr_size
    long end = ftell(apcout); // Backup end of file
    rewind(apcout);           // Return to file start
    // Overwrite file header with current file size
    fprintf(apcout, "APC      : 001,%03u,%04lu\n", APC_RECORD_COUNT, apcstr_size);
    fseek(apcout, end, SEEK_SET); // Restore end of file
    fclose(apcout);               // File content remains until apcstr is freed
}

/**
 * Free dynamically allocated memory for APC status string.
 */
void apc_destroy()
{
    if (apcstr != NULL)
    {
        free(apcstr);
    }
}

/**
 * Return APC report string.
 */
char *get_apc_report()
{
    return apcstr;
}