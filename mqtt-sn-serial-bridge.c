/*
  MQTT-SN serial bridge
  Copyright (C) 2014 Nicholas Humfrey

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//#define _POSIX_SOURCE 1 /* POSIX compliant source */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <fcntl.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>

#include "mqtt-sn.h"


const char *mqtt_sn_host = "127.0.0.1";
const char *mqtt_sn_port = "1883";
const char *serial_device = NULL;
speed_t serial_baud = B9600;
uint8_t debug = FALSE;

uint8_t keep_running = TRUE;


static void usage()
{
    fprintf(stderr, "Usage: mqtt-sn-serial-bridge [opts] <device>\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -b <baud>      Set the baud rate. Defaults to %d.\n", (int)serial_baud);
    fprintf(stderr, "  -d             Enable debug messages.\n");
    fprintf(stderr, "  -h <host>      MQTT-SN host to connect to. Defaults to '%s'.\n", mqtt_sn_host);
    fprintf(stderr, "  -p <port>      Network port to connect to. Defaults to %s.\n", mqtt_sn_port);
    exit(-1);
}

static void parse_opts(int argc, char** argv)
{
    int ch;

    // Parse the options/switches
    while ((ch = getopt(argc, argv, "b:dh:p:?")) != -1)
        switch (ch) {
        case 'b':
            serial_baud = atoi(optarg);
        break;

        case 'd':
            debug = TRUE;
        break;

        case 'h':
            mqtt_sn_host = optarg;
        break;

        case 'p':
            mqtt_sn_port = optarg;
        break;

        case '?':
        default:
            usage();
        break;
    }

    // Final argument is the serial port device path
    if (argc-optind < 1) {
        fprintf(stderr, "Missing serial port.\n");
        usage();
    } else {
        serial_device = argv[optind];
    }
}


static int open_serial_port(const char* device_path)
{
    struct termios tios;
    int fd;

    fd = open(device_path, O_RDONLY | O_NOCTTY | O_NDELAY );
    if (fd < 0) {perror(device_path); exit(EXIT_FAILURE); }

    // Turn back on blocking reads
    fcntl(fd, F_SETFL, 0);

    // Read existing serial port settings
    tcgetattr(fd, &tios);

    // Set the input and output baud rates
    cfsetispeed(&tios, serial_baud);
    cfsetospeed(&tios, serial_baud);

    // Set to local mode
    tios.c_cflag |= CLOCAL | CREAD;

    // Set serial port mode to 8N1
    tios.c_cflag &= ~PARENB;
    tios.c_cflag &= ~CSTOPB;
    tios.c_cflag &= ~CSIZE;
    tios.c_cflag |= CS8;

    // Turn off flow control and ignore parity
    tios.c_iflag &= ~(IXON | IXOFF | IXANY);
    tios.c_iflag |= IGNPAR;

    // Turn off output post-processing
    tios.c_oflag &= ~OPOST;

    // set input mode (non-canonical, no echo,...)
    tios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    // Set VMIN high, so that we wait for a gap in the data
    // http://www.unixwiz.net/techtips/termios-vmin-vtime.html
    tios.c_cc[VMIN]     = 255;
    tios.c_cc[VTIME]    = 1;

    tcsetattr(fd, TCSAFLUSH, &tios);

    return fd;
}

static void termination_handler (int signum)
{
    switch(signum) {
        case SIGHUP:  fprintf(stderr, "Got hangup signal."); break;
        case SIGTERM: fprintf(stderr, "Got termination signal."); break;
        case SIGINT:  fprintf(stderr, "Got interupt signal."); break;
    }

    // Signal the main thead to stop
    keep_running = FALSE;
}


int main(int argc, char* argv[])
{
    int fd, ret;
    char buf[255];
    int sock;

    // Parse the command-line options
    parse_opts(argc, argv);

    mqtt_sn_set_debug(debug);

    // Setup signal handlers
    signal(SIGTERM, termination_handler);
    signal(SIGINT, termination_handler);
    signal(SIGHUP, termination_handler);

    // Create a UDP socket
    sock = mqtt_sn_create_socket(mqtt_sn_host, mqtt_sn_port);

    // Open the serial port
    fd = open_serial_port(serial_device);

    // Flush the input buffer
    sleep(1);
    tcflush(fd, TCIOFLUSH);


    while (keep_running) {
        int len = read(fd, buf, sizeof(buf));

        if (len > 0) {
            if (len != buf[0]) {
                fprintf(stderr, "Error: length of MQTT-SN packet doesn't match number of bytes read.\n");
                continue;
            }
        
            if (debug) {
                const char* type = mqtt_sn_type_string(buf[1]);
                fprintf(stderr, "Sending packet (len=%d, type=%s)\n", len, type);
            }
            mqtt_sn_send_packet(sock, buf, len);
        } else {
            fprintf(stderr, "Error reading packet from serial port: %d, %d\n", ret, errno);
        }
    }

    close(sock);
    close(fd);

    mqtt_sn_cleanup();

    return 0;
}
