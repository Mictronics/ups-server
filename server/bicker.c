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
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include "bicker.h"

static char serial_name[255] = "";
static const char *serial_interface = "/dev/ttyUSB0";
static int baudrate = B38400;
static char serial_buffer[512];
static bool has_serial_interface = false; // Indicates serial interface is open and accessible
static bool has_rw_error = false;         // Indicates an read/write error
bicker_ups_status_t bicker_ups_status;
static struct pollfd poll_serial;
static char bicker_req[5] = {
    BICKER_SOH, BICKER_REQ_LEN, BICKER_CMD_INDEX3, 0, BICKER_EOT};

/**
 * Set serial interface device name.
 */
void set_serial_interface(const char *dname)
{
    if (dname == NULL)
    {
        return;
    }
    strncpy(serial_name, dname, sizeof serial_name);
    serial_name[(sizeof serial_name) - 1] = '\0';
    serial_interface = serial_name;
}

/**
 * Open serial interface and setup its parameters.
 */
int open_serial(void)
{
    struct termios tios;

    poll_serial.fd = open(serial_interface, O_RDWR | O_NOCTTY);
    if (poll_serial.fd < 0)
    {
        lwsl_err("Failed to open serial device %s: %s\n",
                 serial_interface, strerror(errno));
        return (EXIT_FAILURE);
    }

    if (tcgetattr(poll_serial.fd, &tios) < 0)
    {
        lwsl_err("tcgetattr(%s): %s\n", serial_interface, strerror(errno));
        return (EXIT_FAILURE);
    }

    memset(&tios, 0, sizeof(tios));
    tios.c_iflag = 0;
    tios.c_oflag = 0;
    tios.c_cflag |= CS8 | CREAD | CLOCAL; // 8N1 no handshake
    tios.c_lflag = 0;
    tios.c_cc[VMIN] = 4; // Read minimum 4 bytes
    tios.c_cc[VTIME] = 0;

    if (cfsetispeed(&tios, baudrate) < 0)
    {
        lwsl_err("Serial cfsetispeed(%s): %s\n",
                 serial_interface, strerror(errno));
        return (EXIT_FAILURE);
    }

    if (cfsetospeed(&tios, baudrate) < 0)
    {
        lwsl_err("Serial cfsetospeed(%s): %s\n",
                 serial_interface, strerror(errno));
        return (EXIT_FAILURE);
    }

    tcflush(poll_serial.fd, TCIFLUSH);

    if (tcsetattr(poll_serial.fd, TCSANOW, &tios) < 0)
    {
        lwsl_err("Serial tcsetattr(%s): %s\n",
                 serial_interface, strerror(errno));
        return (EXIT_FAILURE);
    }

    // Kick on handshake and start reception
    int RTSDTR_flag = TIOCM_RTS | TIOCM_DTR;
    ioctl(poll_serial.fd, TIOCMBIS, &RTSDTR_flag); // Set RTS&DTR pin

    memset(&bicker_ups_status, 0, sizeof(bicker_ups_status));
    has_serial_interface = true;
    has_rw_error = false;
    return EXIT_SUCCESS;
}

/**
 * Close serial interface.
 */
void close_serial(void)
{
    has_serial_interface = false;
    close(poll_serial.fd);
}

/**
 * Check if serial interface is still open and accessible or has an R/W error.
 */
bool is_serial_error()
{
    struct stat buffer;
    has_serial_interface = (stat(serial_interface, &buffer) == 0);
    return !has_serial_interface | has_rw_error;
}

/**
 * Read from serial interface in non-blocking mode with timeout.
 */
static char *read_serial(ssize_t *len)
{
    poll_serial.events = POLLIN;
    if (poll(&poll_serial, 1, SERIAL_TIMEOUT) > 0 && fcntl(poll_serial.fd, F_GETFD) != -1 && errno != EBADF)
    {
        *len = read(poll_serial.fd, serial_buffer, sizeof(serial_buffer) - 1);
        return serial_buffer;
    }
    else
    {
        lwsl_err("Error reading from serial interface.\n");
        has_rw_error = true;
        *len = -1;
        return NULL;
    }
}

/**
 * Write to serial interface in non-blocking mode with timeout.
 */
static ssize_t write_serial(const char *buf, size_t len)
{
    poll_serial.events = POLLOUT;
    if (poll(&poll_serial, 1, SERIAL_TIMEOUT) > 0 && fcntl(poll_serial.fd, F_GETFD) != -1 && errno != EBADF)
    {
        return write(poll_serial.fd, buf, len);
    }
    else
    {
        lwsl_err("Error writing to serial interface.\n");
        has_rw_error = true;
        return -1;
    }
}

/**
 * Get integer of 8 or 16 bit size.
 */
static signed short get_sshort(const cmd_list_t cmd, const char cmd_index)
{
    if (!has_serial_interface || has_rw_error)
        return 0;

    ssize_t len = 0;
    signed short i = 0;
    bicker_req[2] = cmd_index;
    bicker_req[3] = (char)cmd;
    write_serial(bicker_req, sizeof bicker_req);
    bicker_data_t *p = (bicker_data_t *)read_serial(&len);
    if (p != NULL && len >= 4 && p->soh == BICKER_SOH && p->cmd_list == cmd)
    {
        if (p->size == 4)
        { // int8_t data
            i = (signed short)p->data[0];
        }
        else if (p->size == 5)
        { // int16_t data
            i = (signed short)(p->data[1] * 256 + p->data[0]);
        }
    }
    return i;
}

/**
 * Get integer of 32 bit size.
 */
static signed int get_sint(const cmd_list_t cmd, const char cmd_index)
{
    if (!has_serial_interface || has_rw_error)
        return 0;

    ssize_t len = 0;
    signed int i = 0;
    bicker_req[2] = cmd_index;
    bicker_req[3] = (char)cmd;
    write_serial(bicker_req, sizeof bicker_req);
    bicker_data_t *p = (bicker_data_t *)read_serial(&len);
    if (p != NULL && len >= 4 && p->soh == BICKER_SOH && p->cmd_list == cmd)
    {
        if (p->size == 7)
        {
            i = p->data[3];
            i <<= 8;
            i += p->data[2];
            i <<= 8;
            i += p->data[1];
            i <<= 8;
            i += p->data[0];
        }
    }
    return i;
}

/**
 * Get string from UPS.
 */
static void get_string(cmd_list_t cmd, char *dest, ssize_t buflen)
{
    if (!has_serial_interface || has_rw_error)
        return;

    ssize_t len = 0;
    bicker_req[2] = BICKER_CMD_INDEX1;
    bicker_req[3] = (char)cmd;
    write_serial(bicker_req, sizeof bicker_req);
    bicker_data_t *p = (bicker_data_t *)read_serial(&len);
    if (p != NULL && len >= 7 && p->soh == BICKER_SOH && p->cmd_list == cmd)
    {
        if (p->size < buflen)
        {
            buflen = p->size;
        }
        memcpy(dest, p->data, buflen);
        return;
    }
}

bicker_ups_status_t *get_ups_status()
{
    bicker_ups_status.input_voltage = (signed int)get_sshort(GET_INPUT_VOLTAGE1, BICKER_CMD_INDEX1);
    bicker_ups_status.input_current = (signed int)get_sshort(GET_INPUT_CURRENT1, BICKER_CMD_INDEX1);
    bicker_ups_status.output_voltage = (signed int)get_sshort(GET_OUTPUT_VOLTAGE1, BICKER_CMD_INDEX1);
    bicker_ups_status.output_current = (signed int)get_sshort(GET_OUTPUT_CURRENT1, BICKER_CMD_INDEX1);
    bicker_ups_status.battery_current = (signed int)get_sshort(GET_BATTERY_CURRENT, BICKER_CMD_INDEX1);
    bicker_ups_status.battery_voltage = (signed int)get_sshort(GET_BATTERY_VOLTAGE, BICKER_CMD_INDEX1);
    bicker_ups_status.vcap_voltage.cap1 = (signed int)get_sshort(GET_VCAP1_VOLTAGE, BICKER_CMD_INDEX3);
    bicker_ups_status.vcap_voltage.cap2 = (signed int)get_sshort(GET_VCAP2_VOLTAGE, BICKER_CMD_INDEX3);
    bicker_ups_status.vcap_voltage.cap3 = (signed int)get_sshort(GET_VCAP3_VOLTAGE, BICKER_CMD_INDEX3);
    bicker_ups_status.vcap_voltage.cap4 = (signed int)get_sshort(GET_VCAP4_VOLTAGE, BICKER_CMD_INDEX3);
    bicker_ups_status.capacity = get_sint(GET_CAPACITY, BICKER_CMD_INDEX3);
    bicker_ups_status.esr = (signed int)get_sshort(GET_ESR, BICKER_CMD_INDEX3);
    bicker_ups_status.charge_status.value = (signed int)get_sshort(GET_CHARGE_STATUS_REGISTER, BICKER_CMD_INDEX3);
    bicker_ups_status.monitor_status.value = (signed int)get_sshort(GET_MONITOR_STATUS_REGISTER, BICKER_CMD_INDEX3);
    bicker_ups_status.device_status.value = (unsigned char)get_sshort(GET_DEVICE_STATUS, BICKER_CMD_INDEX1);
    bicker_ups_status.soc = (signed int)get_sshort(GET_SOC, BICKER_CMD_INDEX1);
    bicker_ups_status.uc_temperature = (signed int)get_sshort(GET_LTC3350_TEMPERATURE, BICKER_CMD_INDEX1);
    get_string(GET_BATTERY_TYPE, bicker_ups_status.battery_type, sizeof bicker_ups_status.battery_type);
    get_string(GET_FIRMWARE, bicker_ups_status.firmware, sizeof bicker_ups_status.firmware);
    get_string(GET_SERIES, bicker_ups_status.series, sizeof bicker_ups_status.series);
    get_string(GET_HARDWARE_REVISION, bicker_ups_status.hw_revision, sizeof bicker_ups_status.hw_revision);
    return &bicker_ups_status;
}

void start_cap_esr_measurement()
{
    get_sshort(START_CAP_ESR_MEASUREMENT, BICKER_CMD_INDEX3);
}
