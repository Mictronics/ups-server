# UPS Server

A websocket server reading data from a Bicker PSZ-1063 uExtension module in combination with one of their UPS through a serial RS232 connection.

The server daemon is monitoring the UPS and provides its parameters to a web application for visualization. In addition a system shutdown is initiated as soon as an UPS input power fail is detected. A shutdown delay is configurable. When input power returns while the delay is pending then the shutdown is cancelled.

Tested with Bicker UPSIC-1205 + PSZ1063. See [www.bicker.de](https://www.bicker.de)

The above hardware combination is powering a Nipogi mini PC via 12V supply. This mini PC is running Ubuntu Server and the UPS monitoring ensures a clean shutdown in case of input power fail.

There are two protocols implemented on the port that is provided by the UPS server: a Websocket where the web application connects to and a RAW protocol that is serving a apcupsd compatible output for tools like apcaccess or Netdata's apcupsd plugin.

```bash
~$ /usr/sbin/apcaccess status localhost:10024
APC      : 001,025,0593
DATE     : 2023-11-05 12:45:38 +0100
HOSTNAME : workhorse
UPSNAME  : UPSIC Series
MODEL    : SUC-1011
FIRMWARE : 2.0.5R
STATUS   : ONLINE
LINEFAIL : No
LINEV    : 12.6 Volts
LINEA    : 0.004 Amps
OUTPUTV  : 12.6 Volts
OUTPUTA  : 0.000 Amps
BATTV    : 10.5 Volts
BATTA    : 0.010 Amps
BCHARGE  : 100 Percent
ITEMP    : 28 C
DSHUTD   : 15 Seconds
DWAKE    : 8 Seconds
MAXTIME  : 60 Seconds
RETPCT   : 90 Percent
STATFLAG : 0x0D
REG2     : 0x1425
REG3     : 0x0200
NOMINV   : 12.4 Volts
NOMBATTV : 10.4 Volts
ENDAPC   : 2023-11-05 12:45:38 +0100

```

## Debian/Ubuntu packages

It is designed to build as a Debian package.

### Build Dependencies

- libpthread-stubs0-dev
- libwebsockets-dev
- libconfig-dev
- libjson-c-dev

### Actually building it

Nothing special, just build it `dpkg-buildpackage -b -uc`.

### Installation

Install package `sudo dpkg -i ups-server_1.0.0~dev_amd64.deb`.

The package installation is not including a webserver and its configuration. The ups-server binary is installed in `/opt/ups-server` while the web client is in `/opt/www/ups-server`.

### Removal

Remove the package `sudo apt-get purge ups-server`.

### Configuration

To configure the service in `/etc/default/ups-server.cfg` as required.

## Building manually

You can probably just run "make" after installing the required dependencies.
Binaries are built in the source directory; you will need to arrange to
install them (and a method for starting them) yourself.

## Disclaimer

I am not affiliated, associated, authorized, endorsed by, or in any way officially connected with Bicker GmbH. This is a pure hobbyist project.
