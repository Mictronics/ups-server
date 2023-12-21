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
#include <libconfig.h>
#include <json-c/json.h>
#include <pthread.h>
#include <string.h>
#include <getopt.h>
#include <syslog.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <math.h>
#include "help.h"
#include "bicker.h"

#define NOTUSED(V) ((void)V)
#define WSBUFFERSIZE 1024 // Byte
#define UPDATE_TIME_SEC 1 // seconds, Websocket update

static int num_clients = 0;
static char config_file[PATH_MAX] = "/etc/default/ups-server.cfg";
static int syslog_options = LOG_PID | LOG_PERROR;
static config_t cfg;

#ifndef LWS_NO_DAEMONIZE
static int daemonize = 0;
#endif
static struct lws_context *context;
static struct lws_context_creation_info info;
static unsigned char wsbuffer[WSBUFFERSIZE];
static unsigned char *pwsbuffer = wsbuffer;
static int wsbuffer_len = 0;
pthread_mutex_t lock_established_conns = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_apc_status = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_ups_status = PTHREAD_MUTEX_INITIALIZER;
pthread_t ups_thread;
pthread_t shutdown_thread;
static unsigned int shutdown_delay = 1; // Default 1 second if not set in config
static int shutdown_soc_percent = 25;   // Default 25% state of charge shutdown
static bool shutdown_by_time = true;
static bool shutdown_by_soc = false;
static bool shutdown_override = false;
static bool ups_thread_exit = false;
static bool cmd_cap_esr_measurement = false;
static bool log_file_enable = false;

#define APC_RECORD_COUNT 29
static size_t apcstr_size = 0;
static char *apcstr = NULL;
static char *apcline = NULL;
static double nominal_input_voltage = 0.0;
static double nominal_battery_voltage = 0.0;
static int nominal_ouput_power = 0;
static int power_return_percent = 0;
static int max_backup_time = 0;
static int wakeup_delay = 0;
static int max_amps = 0;
static char hostname[256];

/**
 * HTTP protocol and server mount.
 */
static const struct lws_http_mount mount = {
    .mount_next = NULL,
    .mountpoint = "/",
    .origin = "/opt/ups-server/client",
    .def = "index.html",
    .protocol = NULL,
    .cgienv = NULL,
    .extra_mimetypes = NULL,
    .interpret = NULL,
    .cgi_timeout = 0,
    .cache_max_age = 0,
    .auth_mask = 0,
    .cache_reusable = 0,
    .cache_revalidate = 0,
    .cache_intermediaries = 0,
    .origin_protocol = LWSMPRO_FILE,
    .mountpoint_len = 1,
    .basic_auth_login_file = NULL,
};

/**
 * One of these is created for each client connecting.
 */
struct ws_pss
{
    struct ws_pss *pss_list;
    struct lws *wsi;
    char publishing; // nonzero: peer is publishing to us
};

/**
 *  One of these is created for each vhost our protocol is used with.
 */
struct ws_vhd
{
    struct lws_context *context;
    struct lws_vhost *vhost;
    const struct lws_protocols *protocol;
    struct ws_pss *pss_list; // linked-list of live pss
    int len;
    char buf[LWS_SEND_BUFFER_PRE_PADDING + 100];
    char apcstr[1024];
};

static int callback_raw(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len);
static int callback_broadcast(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len);
static error_t parse_opt(int key, char *arg, struct argp_state *state);
const char *argp_program_version = "UPS Server v1.0.3";
const char args_doc[] = "";
const char doc[] = "Websocket Server for Bicker PSZ-1063 uExtension module\nLicense GPL-3+\n(C) 2023 Michael Wolf\n"
                   "A websocket server reading data from a Bicker PSZ-1063 uExtension module in combination with their UPS";
static struct argp argp = {options, parse_opt, args_doc, doc, NULL, NULL, NULL};

/**
 * Function parsing the arguments provided on run
 */
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key)
    {
    case 'c':
        strncpy(config_file, arg, sizeof config_file);
        config_file[(sizeof config_file) - 1] = '\0';
        break;
    case ARGP_KEY_END:
        if (state->arg_num > 0)
            /* We use only options but no arguments */
            argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

/**
 * Handle requests from client.
 */
static void handle_client_request(void *in, size_t len)
{
    NOTUSED(len);
    json_object *jroot = json_tokener_parse(in);
    json_object *jval;
    json_object_object_get_ex(jroot, "cmd", &jval);
    const char *p = json_object_get_string(jval);
    // Start cap/esr measurement
    if (strcmp(p, "capesr") == 0)
    {
        pthread_mutex_lock(&lock_ups_status);
        cmd_cap_esr_measurement = true;
        pthread_mutex_unlock(&lock_ups_status);
    }
    json_object_put(jroot);
}

/**
 * Websocket protocol definition.
 */
static struct lws_protocols protocols[] = {
    {"http", callback_raw, 0, 0, 0, NULL, 0},
    {"broadcast", callback_broadcast, sizeof(struct ws_pss), WSBUFFERSIZE, 0, NULL, 0},
    {NULL, NULL, 0, 0, 0, NULL, 0} /* terminator */
};

/**
 * Callback that is serving the APC status report with the raw fallback protocol.
 */
static int callback_raw(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    struct ws_vhd *vhd = (struct ws_vhd *)lws_protocol_vh_priv_get(lws_get_vhost(wsi), lws_get_protocol(wsi));
    switch (reason)
    {
    case LWS_CALLBACK_PROTOCOL_INIT:
        vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
                                          lws_get_protocol(wsi),
                                          sizeof(struct ws_vhd));
        vhd->context = lws_get_context(wsi);
        vhd->protocol = lws_get_protocol(wsi);
        vhd->vhost = lws_get_vhost(wsi);
        break;
    /*
     * RAW protocol handler for APC status report
     */
    case LWS_CALLBACK_RAW_ADOPT:
        lwsl_notice("Connecting raw socket.");
        break;

    case LWS_CALLBACK_RAW_CLOSE:
        pthread_mutex_unlock(&lock_apc_status);
        lwsl_notice("Closing raw socket.");
        break;

    case LWS_CALLBACK_RAW_RX:
        // React only on apcaccess status request
        if (len != 8 || memcmp(in, "\x00\x06status", 8) != 0)
        {
            return lws_raw_transaction_completed(wsi);
        }
        // Prevent APC status report update as long as transmission is in progress.
        pthread_mutex_lock(&lock_apc_status);
        // Create temporary copy of status string since strtok is modifing the source one.
        // Otherwise consecutive apcaccess request will read only first line of report between updates.
        memcpy(vhd->apcstr, apcstr, MIN(apcstr_size, sizeof(vhd->apcstr)));
        // NIS protocol requires line by line transfer.
        apcline = strtok(vhd->apcstr, ";");
        if (apcline != NULL)
        {
            // NIS protocol first two bytes (uint16_t) are the length of the following line.
            vhd->len = strlen(apcline);
            vhd->buf[LWS_SEND_BUFFER_PRE_PADDING] = 0;
            vhd->buf[LWS_SEND_BUFFER_PRE_PADDING + 1] = vhd->len; // Set line length
            // Copy line content
            memcpy(&vhd->buf[LWS_SEND_BUFFER_PRE_PADDING + 2], apcline, vhd->len);
            vhd->len += 2; // uint16_t + line length
        }
        // Request first transmission callback
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_RAW_WRITEABLE:
        // Send APC status report line
        if (lws_write(wsi, (unsigned char *)&vhd->buf[LWS_SEND_BUFFER_PRE_PADDING], (unsigned int)vhd->len, LWS_WRITE_RAW | LWS_WRITE_NO_FIN) == -1)
        {
            size_t remaining = lws_remaining_packet_payload(wsi);
            lwsl_err("Error writing raw socket: %lu", remaining);
            return lws_raw_transaction_completed(wsi);
        }
        // Get next APC report line
        apcline = strtok(NULL, ";");
        if (apcline != NULL)
        {
            // See above.
            vhd->len = strlen(apcline);
            vhd->buf[LWS_SEND_BUFFER_PRE_PADDING] = 0;
            vhd->buf[LWS_SEND_BUFFER_PRE_PADDING + 1] = vhd->len;
            memcpy(&vhd->buf[LWS_SEND_BUFFER_PRE_PADDING + 2], apcline, vhd->len);
            vhd->len += 2;
            // Request next transmission callback
            lws_callback_on_writable(wsi);
        }
        else
        {
            // No more lines in APC status report. Send zero length to close connection.
            vhd->buf[LWS_SEND_BUFFER_PRE_PADDING] = 0;
            vhd->buf[LWS_SEND_BUFFER_PRE_PADDING + 1] = 0;
            vhd->len = 2;
            // Request next transmission callback
            lws_callback_on_writable(wsi);
            // Close connection server side
            return lws_raw_transaction_completed(wsi);
        }
        break;

    default:
        break;
    }

    return lws_callback_http_dummy(wsi, reason, user, in, len);
}

/**
 * Callback that is serving the web socket protocol.
 */
static int callback_broadcast(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len)
{
    struct ws_pss *pss = (struct ws_pss *)user;
    struct ws_vhd *vhd = (struct ws_vhd *)lws_protocol_vh_priv_get(lws_get_vhost(wsi), lws_get_protocol(wsi));
    switch (reason)
    {
    case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
        if (num_clients > 10)
        {
            lwsl_warn("10 clients already connected. New connection rejected...");
            return -1;
        }
        break;
    case LWS_CALLBACK_PROTOCOL_INIT:
        vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
                                          lws_get_protocol(wsi),
                                          sizeof(struct ws_vhd));
        vhd->context = lws_get_context(wsi);
        vhd->protocol = lws_get_protocol(wsi);
        vhd->vhost = lws_get_vhost(wsi);
        break;

    case LWS_CALLBACK_PROTOCOL_DESTROY:
        break;

    case LWS_CALLBACK_ESTABLISHED:
        ++num_clients;
        lwsl_notice("Client connected.");
        pss->wsi = wsi;
        if (lws_hdr_copy(wsi, vhd->buf, sizeof(vhd->buf), WSI_TOKEN_GET_URI) > 0)
            pss->publishing = !strcmp(vhd->buf, "/publisher");
        if (!pss->publishing)
            /* add subscribers to the list of live pss held in the vhd */
            lws_ll_fwd_insert(pss, pss_list, vhd->pss_list);
        break;

    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_WSI_DESTROY:
        --num_clients;
        lwsl_notice("Client disconnected.");
        /* remove our closing pss from the list of live pss */
        lws_ll_fwd_remove(struct ws_pss, pss_list,
                          pss, vhd->pss_list);
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (pss->publishing)
            break;
        /* notice we allowed for LWS_PRE in the payload already */
        vhd->len = lws_write(wsi, &pwsbuffer[LWS_SEND_BUFFER_PRE_PADDING], wsbuffer_len, LWS_WRITE_TEXT);
        if (vhd->len < (int)wsbuffer_len)
        {
            lwsl_err("Error writing to websocket");
            return -1;
        }
        break;

    case LWS_CALLBACK_RECEIVE:
        /*
         * For test, our policy is ignore publishing when there are
         * no subscribers connected.
         */
        if (!vhd->pss_list)
            break;

        if (len <= 0)
            break;
        pthread_mutex_lock(&lock_established_conns);
        /* Reply empty json object on unknown request */
        memcpy(&pwsbuffer[LWS_SEND_BUFFER_PRE_PADDING], "\"{}\"", 4);
        wsbuffer_len = 4;
        handle_client_request(in, len);
        pthread_mutex_unlock(&lock_established_conns);

        /*
         * let every subscriber know we want to write something
         * on them as soon as they are ready
         */
        lws_start_foreach_llp(struct ws_pss **, ppss, vhd->pss_list)
        {
            if (!(*ppss)->publishing)
                lws_callback_on_writable((*ppss)->wsi);
        }
        lws_end_foreach_llp(ppss, pss_list);
        break;

    default:
        break;
    }

    return 0;
}

/**
 * Incoming signal handler. Close server nice and clean.
 */
static void sighandler(int sig)
{
    NOTUSED(sig);
    // Stop UPS read thread
    ups_thread_exit = true;
    pthread_join(ups_thread, NULL);
    pthread_join(shutdown_thread, NULL);
    // Cleanup
    pthread_mutex_unlock(&lock_established_conns);
    pthread_mutex_destroy(&lock_established_conns);
    pthread_mutex_unlock(&lock_apc_status);
    pthread_mutex_destroy(&lock_apc_status);
    pthread_mutex_unlock(&lock_ups_status);
    pthread_mutex_destroy(&lock_ups_status);
    lws_cancel_service(context);
    lws_context_destroy(context);
    config_destroy(&cfg);
    exit(EXIT_SUCCESS);
}

/**
 * Thread to initiate system shutdown in case of input power fail.
 * Will be cancelled when input power returns while delay is pending.
 */
static void *shutdown_handler(void *arg)
{
    NOTUSED(arg);
    lwsl_warn("Initiating shutdown.");
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    // Wait shutdown time but not when we are low on charge
    if (shutdown_by_time == true)
    {
        if (shutdown_override == false)
        {
            sleep(shutdown_delay); // sleep is a point of cancellation
        }
        else
        {
            lwsl_warn("Immediate low on charge shutdown.");
        }
    }
    // Should not get here when cancelled
    lwsl_warn("System shutdown...");
    system("shutdown --poweroff now");
    // Stop this service
    ups_thread_exit = true;
    pthread_exit(NULL);
}

/**
 * Create apcupsd compatible status report in memory.
 */
static void apc_update_status(bicker_ups_status_t *ups)
{
    // Free previous APC report memory if any
    if (apcstr != NULL)
    {
        free(apcstr);
    }
    // Open new report file in memory
    FILE *apcout = open_memstream(&apcstr, &apcstr_size);
    if (apcout == NULL)
    {
        lwsl_warn("Creating APC status stream failed.");
        return;
    }

    char tstr[50];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(tstr, sizeof tstr, "%F %T %z", t);

    /* ';' is a delimiter used in raw socket callback to separate lines.
     * We send the line including the line break.
     */
    // Create dummy header, file size yet unknown.
    fprintf(apcout, "APC      : 001,%03u,0000\n;", APC_RECORD_COUNT);
    fprintf(apcout, "DATE     : %.50s\n;", tstr);
    fprintf(apcout, "HOSTNAME : %s\n;", hostname);
    fprintf(apcout, "UPSNAME  : %.20s\n;", ups->series);
    fprintf(apcout, "MODEL    : %.20s\n;", ups->battery_type);
    fprintf(apcout, "FIRMWARE : %.20s\n;", ups->firmware);
    fprintf(apcout, "CABLE    : Ethernet Link\n;");
    fprintf(apcout, "DRIVER   : NETWORKS UPS Driver\n;");
    fprintf(apcout, "STATUS   : ");
    if (ups->device_status.reg.is_charging)
    {
        fprintf(apcout, "ONLINE\n;");
    }
    else if (ups->device_status.reg.is_discharging)
    {
        fprintf(apcout, "ONBATT\n;");
    }
    else
    {
        fprintf(apcout, "OFFLINE\n;");
    }
    fprintf(apcout, "LINEFAIL : ");
    if (ups->device_status.reg.is_power_present)
    {
        fprintf(apcout, "No\n;");
    }
    else
    {
        fprintf(apcout, "Yes\n;");
    }
    fprintf(apcout, "LINEV    : %.1f Volts\n;", ups->input_voltage / 1000.0);
    fprintf(apcout, "LINEA    : %.3f Amps\n;", ups->input_current / 1000.0);
    fprintf(apcout, "OUTPUTV  : %.1f Volts\n;", ups->output_voltage / 1000.0);
    fprintf(apcout, "OUTPUTA  : %.3f Amps\n;", ups->output_current / 1000.0);
    fprintf(apcout, "LOADPCT  : %u Percent\n;", (int)(((double)ups->output_current / (double)max_amps) * 100.0));
    fprintf(apcout, "BATTV    : %.1f Volts\n;", ups->battery_voltage / 1000.0);
    fprintf(apcout, "BATTA    : %.3f Amps\n;", ups->battery_current / 1000.0);
    fprintf(apcout, "BCHARGE  : %u Percent\n;", ups->soc);
    fprintf(apcout, "ITEMP    : %d C\n;", ups->uc_temperature);
    fprintf(apcout, "DSHUTD   : %u Seconds\n;", shutdown_delay);
    fprintf(apcout, "DWAKE    : %u Seconds\n;", wakeup_delay);
    fprintf(apcout, "MAXTIME  : %u Seconds\n;", max_backup_time);
    fprintf(apcout, "RETPCT   : %u Percent\n;", power_return_percent);
    fprintf(apcout, "STATFLAG : 0x%02X\n;", ups->device_status.value);
    fprintf(apcout, "REG2     : 0x%04X\n;", ups->charge_status.value);
    fprintf(apcout, "REG3     : 0x%04X\n;", ups->monitor_status.value);
    fprintf(apcout, "NOMINV   : %.1f Volts\n;", nominal_input_voltage);
    fprintf(apcout, "NOMBATTV : %.1f Volts\n;", nominal_battery_voltage);
    fprintf(apcout, "NOMPOWER : %u Watts\n;", nominal_ouput_power);
    fprintf(apcout, "ENDAPC   : %.50s\n;", tstr);
    fflush(apcout);           // Update apcstr and apcstr_size
    long end = ftell(apcout); // Backup end of file
    rewind(apcout);           // Return to file start
    // Overwrite file header with current file size
    fprintf(apcout, "APC      : 001,%03u,%04lu\n;", APC_RECORD_COUNT, apcstr_size);
    fseek(apcout, end, SEEK_SET); // Restore end of file
    fclose(apcout);               // File content remains until apcstr is freed
}

static void log_to_file(bicker_ups_status_t *ups)
{
    char path[FILENAME_MAX];
    struct stat st = {0};
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    FILE *fp;
    bool exists = true;

    // Create file by flight and top number
    snprintf(path, FILENAME_MAX, "/var/tmp/ups-server_%02u%02u%04u.csv",
             t->tm_mday,
             t->tm_mon + 1,
             1900 + t->tm_year);
    if (stat(path, &st) == -1)
    {
        exists = false;
    }
    fp = fopen(path, "a");
    if (fp != NULL)
    {
        if (exists == false)
        { // Add header
            fputs("TIME;IN_V;IN_A;IN_W;OUT_V;OUT_A;OUT_W;LOAD;BATT_V;BATT_A;SOC;VCAP1;VCAP2;VCAP3;VCAP4;TEMP\n", fp);
        }
        fprintf(fp, "%lu;%0.1f;%0.3f;%0.1f;%0.1f;%0.3f;%0.1f;%u;%0.1f;%0.3f;%u;%0.1f;%0.1f;%0.1f;%0.1f;%u\n",
                now,
                ups->input_voltage / 1000.0,
                ups->input_current / 1000.0,
                (ups->input_voltage / 1000.0) * (ups->input_current / 1000.0),
                ups->output_voltage / 1000.0,
                ups->output_current / 1000.0,
                (ups->output_voltage / 1000.0) * (ups->output_current / 1000.0),
                (int)(((double)ups->output_current / (double)max_amps) * 100.0),
                ups->battery_voltage / 1000.0,
                ups->battery_current / 1000.0,
                ups->soc,
                ups->vcap_voltage.cap1 / 1000.0,
                ups->vcap_voltage.cap2 / 1000.0,
                ups->vcap_voltage.cap3 / 1000.0,
                ups->vcap_voltage.cap4 / 1000.0,
                ups->uc_temperature);
    }
    else
    {
        lwsl_err("Error creating log file: %s\n", strerror(errno));
    }
    fclose(fp);
}

/**
 * Bicker UPS read thread.
 */
static void *ups_read_handler(void *arg)
{
    NOTUSED(arg);
    lwsl_notice("UPS read thread started.");
    if (open_serial() == EXIT_FAILURE)
    {
        lwsl_err("Serial device init failed\n");
        ups_thread_exit = true;
    };

    gethostname(hostname, sizeof hostname);

    bool was_power_present = false;
    bool shutdown_pending = false;
    time_t t = time(NULL);
    int start_soc = 100, old_soc = 100;
    double remain = 0.0;

    while (!ups_thread_exit)
    {
        // Get UPS status for websocket service
        pthread_mutex_lock(&lock_ups_status);
        bicker_ups_status_t *bs = get_ups_status();
        // Check if serial interface connection is still present and there is no R/W error
        if (is_serial_error())
        {
            lwsl_err("Serial interface not accessible.\n");
            ups_thread_exit = true;
            break;
        }
        // Create JSON object for websocket transfer
        size_t len = 0;
        json_object *jroot = json_object_new_object();
        json_object_object_add(jroot, "inputVoltage", json_object_new_double((double)(bs->input_voltage / 1000.0)));
        json_object_object_add(jroot, "outputVoltage", json_object_new_double((double)(bs->output_voltage / 1000.0)));
        json_object_object_add(jroot, "batteryVoltage", json_object_new_double((double)(bs->battery_voltage / 1000.0)));
        json_object_object_add(jroot, "vcap1Voltage", json_object_new_double((double)(bs->vcap_voltage.cap1 / 1000.0)));
        json_object_object_add(jroot, "vcap2Voltage", json_object_new_double((double)(bs->vcap_voltage.cap2 / 1000.0)));
        json_object_object_add(jroot, "vcap3Voltage", json_object_new_double((double)(bs->vcap_voltage.cap3 / 1000.0)));
        json_object_object_add(jroot, "vcap4Voltage", json_object_new_double((double)(bs->vcap_voltage.cap4 / 1000.0)));
        json_object_object_add(jroot, "inputCurrent", json_object_new_int((int)bs->input_current));
        json_object_object_add(jroot, "outputCurrent", json_object_new_int((int)bs->output_current));
        json_object_object_add(jroot, "batteryCurrent", json_object_new_int((int)bs->battery_current));
        json_object_object_add(jroot, "outputLoad", json_object_new_int((int)(((double)bs->output_current / (double)max_amps) * 100.0)));
        json_object_object_add(jroot, "ucTemperature", json_object_new_int((int)bs->uc_temperature));
        json_object_object_add(jroot, "capacity", json_object_new_int((int)bs->capacity));
        json_object_object_add(jroot, "esr", json_object_new_int((int)bs->esr));
        json_object_object_add(jroot, "soc", json_object_new_int((int)bs->soc));
        json_object_object_add(jroot, "remainTime", json_object_new_double(remain));
        json_object_object_add(jroot, "chargeStatus", json_object_new_int((int)bs->charge_status.value));
        json_object_object_add(jroot, "monitorStatus", json_object_new_int((int)bs->monitor_status.value));
        json_object_object_add(jroot, "deviceStatus", json_object_new_int((int)bs->device_status.value));
        json_object_object_add(jroot, "batteryType", json_object_new_string(bs->battery_type));
        json_object_object_add(jroot, "series", json_object_new_string(bs->series));
        json_object_object_add(jroot, "firmware", json_object_new_string(bs->firmware));
        json_object_object_add(jroot, "hwRevision", json_object_new_string(bs->hw_revision));
        // Create JSON string and copy to websocket buffer
        const char *p = json_object_to_json_string_length(jroot, JSON_C_TO_STRING_PLAIN, &len);
        memcpy(&pwsbuffer[LWS_SEND_BUFFER_PRE_PADDING], (unsigned char *)p, len);
        wsbuffer_len = len;
        lws_callback_on_writable_all_protocol(context, &protocols[1]);
        // Send immediately to web client
        lws_service(context, 0);

        // Cleanup JSON allocated memory
        json_object_put(jroot);

        // Start capacity/ers measurement on request if not running
        if (cmd_cap_esr_measurement && !bs->monitor_status.reg.is_esr_measuring)
        {
            start_cap_esr_measurement();
            cmd_cap_esr_measurement = false;
        }
        pthread_mutex_unlock(&lock_ups_status);

        // Check for UPS power fail and shutdown request
        if (bs->device_status.reg.is_power_present == false || bs->device_status.reg.is_shutdown_set == true)
        {
            // Raise warning independent of shutdown mode.
            if (was_power_present == true)
            {
                lwsl_warn("Power fail detected!");
                was_power_present = false;
                t = time(NULL);
                start_soc = bs->soc;
            }

            // Proceed if we either shutdown by time or low state of charge
            if (shutdown_by_time == true || (bs->soc < shutdown_soc_percent && shutdown_by_soc == true))
            {
                // Override timed shutdown in case we are already low on charge
                if (bs->soc < shutdown_soc_percent)
                {
                    shutdown_override = true;
                }
                if (shutdown_pending == false)
                {
                    // Initiate shutdown if not pending
                    pthread_create(&shutdown_thread, NULL, shutdown_handler, NULL);
                    shutdown_pending = true;
                }
            }

            if (bs->soc < 100 && bs->soc < old_soc)
            {
                double dt = difftime(time(NULL), t);
                remain = ceilf((dt / (start_soc - (double)bs->soc)) * (double)bs->soc);
                old_soc = bs->soc;
            }
        }
        // Check if power returned and there is no shutdown request from UPS
        if (bs->device_status.reg.is_power_present == true && bs->device_status.reg.is_shutdown_set == false)
        {
            // Raise warning independent of shutdown mode.
            if (was_power_present == false)
            {
                lwsl_warn("Power good detected.");
                was_power_present = true;
            }

            if (shutdown_pending == true)
            {
                // Cancel a pending shutdown
                pthread_cancel(shutdown_thread);
                pthread_join(shutdown_thread, NULL);
                shutdown_pending = false;
                lwsl_warn("Shutdown cancelled.");
            }
            shutdown_override = false;
            old_soc = bs->soc;
            remain = 0.0;
        }

        // Update APC status report
        if (pthread_mutex_trylock(&lock_apc_status) == 0)
        {
            apc_update_status(bs);
            pthread_mutex_unlock(&lock_apc_status);
        }

        if (log_file_enable)
        {
            log_to_file(bs);
        }

        // Websocket update delay
        sleep(UPDATE_TIME_SEC);
    }
    // Cleanup
    close_serial();
    if (apcstr != NULL)
    {
        free(apcstr);
        apcstr = NULL;
    }
    pthread_exit(NULL);
}

/**
 * Well, it's main.
 */
int main(int argc, char **argv)
{
    /* Take care to zero down the info struct, contains random garbage
     * from the stack otherwise.
     */
    memset(&info, 0, sizeof(info));
    info.port = 10024;
    info.iface = "127.0.0.1";
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.max_http_header_pool = 16;
    info.options = LWS_SERVER_OPTION_FALLBACK_TO_RAW;
    info.mounts = &mount;
    info.extensions = NULL;
    info.timeout_secs = 4;
    info.max_http_header_data = 2048;
    info.vhost_name = "abakan.fitz.box";
    info.ka_time = 3;
    info.ka_interval = 1;
    info.ka_probes = 1;
    /* Parse the command line options */
    if (argp_parse(&argp, argc, argv, 0, 0, 0))
    {
        exit(EXIT_SUCCESS);
    }
    /* Parse configuration file */
    config_init(&cfg);
    if (!config_read_file(&cfg, config_file))
    {
        lwsl_err("Error reading configuration: %s:%d - %s", config_error_file(&cfg),
                 config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return (EXIT_FAILURE);
    }

    config_setting_t *setting = NULL;
    setting = config_lookup(&cfg, "server");
    if (setting != NULL)
    {
        config_lookup_string(&cfg, "server.ip", &info.iface);
        config_lookup_int(&cfg, "server.port", &info.port);
        config_lookup_int(&cfg, "server.user", &info.uid);
        config_lookup_int(&cfg, "server.group", &info.gid);
        config_lookup_bool(&cfg, "server.daemonize", &daemonize);
        config_lookup_bool(&cfg, "server.logToFile", (int *)&log_file_enable);
        config_lookup_int(&cfg, "server.shutdownDelay", (int *)&shutdown_delay);
        config_lookup_int(&cfg, "server.shutdownSocPercent", (int *)&shutdown_soc_percent);
        config_lookup_bool(&cfg, "server.shutdownByTime", (int *)&shutdown_by_time);
        config_lookup_bool(&cfg, "server.shutdownBySoc", (int *)&shutdown_by_soc);
        const char *buf = NULL;
        config_lookup_string(&cfg, "server.serial", &buf);
        set_serial_interface(buf);
        if (shutdown_soc_percent < 0)
            shutdown_soc_percent = 25;
        if (shutdown_soc_percent > 100)
            shutdown_soc_percent = 100;
        if (shutdown_by_time == false && shutdown_by_soc == false)
        {
            shutdown_by_time = true;
            lwsl_err("Configuration mismatch. Shutdown by time enabled.");
        }
    }
    else
    {
        lwsl_err("Server settings not found in configuration file.");
    }

    setting = config_lookup(&cfg, "ups");
    if (setting != NULL)
    {
        config_lookup_float(&cfg, "ups.inputVoltage", &nominal_input_voltage);
        config_lookup_float(&cfg, "ups.batteryVoltage", &nominal_battery_voltage);
        config_lookup_int(&cfg, "ups.powerReturnPercent", &power_return_percent);
        config_lookup_int(&cfg, "ups.maxBackupTime", &max_backup_time);
        config_lookup_int(&cfg, "ups.wakeupDelay", &wakeup_delay);
        config_lookup_int(&cfg, "ups.maxAmps", &max_amps);
        nominal_ouput_power = nominal_input_voltage * max_amps;
    }
    else
    {
        lwsl_err("UPS settings not found in configuration file.");
    }

    if (max_amps < 1)
    {
        max_amps = 5000;
        lwsl_warn("UPS maximum current rating not found. Set to 5 amps.");
    }
    else
    {
        max_amps *= 1000;
    }

#if !defined(LWS_NO_DAEMONIZE)
    if (daemonize)
    {
        syslog_options &= ~LOG_PERROR;
    }
    /* Normally lock path would be /var/lock/lwsts or similar, to
     * simplify getting started without having to take care about
     * permissions or running as root, set to /tmp/.lwsts-lock
     */
    if (daemonize && lws_daemonize("/tmp/.lwsts-lock"))
    {
        lwsl_err("Failed to daemonize.");
        config_destroy(&cfg);
        return EXIT_FAILURE;
    }
#endif

    signal(SIGABRT, sighandler);
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGQUIT, sighandler);

    /* Only try to log things according to our debug_level */
    setlogmask(LOG_UPTO(LOG_DEBUG));
    openlog("UPS Server", syslog_options, LOG_DAEMON);

    /* Tell the library what debug level to emit and to send it to syslog */
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_USER, lwsl_emit_syslog);

    /* Create libwebsocket context representing this server */
    context = lws_create_context(&info);
    if (context == NULL)
    {
        lwsl_err("libwebsocket init failed.");
        config_destroy(&cfg);
        return EXIT_FAILURE;
    }

    /* Start reading serial data from weather station */
    pthread_create(&ups_thread, NULL, ups_read_handler, NULL);

    //  Infinite loop, to end this server send SIGTERM. (CTRL+C) */
    for (;;)
    {
        lws_service(context, 100);
        // Check if UPS thread is running.
        // Exit if not, e.g. serial interface connection loss.
        if (ups_thread_exit)
        {
            break;
        }
    }

    sighandler(0);
    return EXIT_SUCCESS;
}