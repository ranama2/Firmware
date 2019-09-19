/****************************************************************************
 *
 *   Copyright (c) 2019 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "UWB.h"
#include <px4_log.h>
#include <px4_getopt.h>
#include <px4_cli.h>
#include <errno.h>
#include <fcntl.h>
#include <systemlib/err.h>
#include <drivers/drv_hrt.h>

// Timeout between bytes. If there is more time than this between bytes, then this driver assumes
// that it is the boundary between messages.
// See UWB::run() for more detailed explanation.
#define BYTE_TIMEOUT_US 5000

// Amount of time to wait for a new message. If more time than this passes between messages, then this
// driver assumes that the UWB module is disconnected.
// (Right now it does not do anything about this)
#define MESSAGE_TIMEOUT_US 1000000

// The current version of the UWB software is locked to 115200 baud.
#define DEFAULT_BAUD 115200

extern "C" __EXPORT int uwb_main(int argc, char *argv[]);

UWB::UWB(const char *device_name, int baudrate):
	_read_count_perf(perf_alloc(PC_COUNT, "uwb_count")),
	_read_err_perf(perf_alloc(PC_COUNT, "uwb_err"))
{

	speed_t baud = B115200;

	switch (baudrate) {
	case 9600:
		baud = B9600;
		break;

	case 19200:
		baud = B19200;
		break;

	case 38400:
		baud = B38400;
		break;

	case 57600:
		baud = B57600;
		break;

	case 115200:
		baud = B115200;
		break;

	case 460800:
		baud = B460800;
		break;

	case 500000:
		baud = B500000;
		break;

	case 921600:
		baud = B921600;
		break;

	default:
		err(1, "%d is not a valid baud rate.", baudrate);
	}

	// start serial port
	_uart = open(device_name, O_RDWR | O_NOCTTY);

	if (_uart < 0) { err(1, "could not open %s", device_name); }

	int ret = 0;
	struct termios uart_config {};
	ret = tcgetattr(_uart, &uart_config);

	if (ret < 0) { err(1, "failed to get attr"); }

	uart_config.c_oflag &= ~ONLCR; // no CR for every LF
	ret = cfsetispeed(&uart_config, baud);

	if (ret < 0) { err(1, "failed to set input speed"); }

	ret = cfsetospeed(&uart_config, baud);

	if (ret < 0) { err(1, "failed to set output speed"); }

	ret = tcsetattr(_uart, TCSANOW, &uart_config);

	if (ret < 0) { err(1, "failed to set attr"); }

}

UWB::~UWB()
{
	perf_free(_read_err_perf);
	perf_free(_read_count_perf);

	close(_uart);
}

void UWB::run()
{
	int written = write(_uart, CMD_PURE_RANGING, sizeof(CMD_PURE_RANGING));

	if (written < (int) sizeof(CMD_PURE_RANGING)) {
		PX4_ERR("Only wrote %d bytes out of %d.", written, (int) sizeof(CMD_PURE_RANGING));
	}

	position_msg_t msg;
	uint8_t *buffer = (uint8_t *) &msg;


	while (!should_exit()) {

		FD_ZERO(&_uart_set);
		FD_SET(_uart, &_uart_set);
		_uart_timeout.tv_sec = 0;
		_uart_timeout.tv_usec = MESSAGE_TIMEOUT_US;

		size_t buffer_location = 0;

		// Messages are only delimited by time. There is a chance that this driver starts up in the middle
		// of a message, with no way to know this other than time. There is also always the possibility of
		// transmission errors causing a dropped byte.
		// Here is the process for dealing with that:
		//  - Wait up to 1 second to start receiving a message
		//  - Once receiving a message, keep going until EITHER:
		//    - There is too large of a gap between bytes (Currently set to 5ms).
		//      This means the message is incomplete. Throw it out and start over.
		//    - 51 bytes are received (the size of the whole message).
		while (buffer_location < sizeof(msg)
		       && select(_uart + 1, &_uart_set, nullptr, nullptr, &_uart_timeout) > 0) {
			int bytes_read = read(_uart, &buffer[buffer_location], sizeof(msg) - buffer_location);

			if (bytes_read > 0) {
				buffer_location += bytes_read;

			} else {
				break;
			}

			FD_ZERO(&_uart_set);
			FD_SET(_uart, &_uart_set);
			_uart_timeout.tv_sec = 0;
			// Setting this timeout too high (> 37ms) will cause problems because the next message will start
			//  coming in, and overlap with the current message.
			// Setting this timeout too low (< 1ms) will cause problems because there is some delay between
			//  the individual bytes of a message, and a too-short timeout will cause the message to be truncated.
			// The current value of 5ms was found experimentally to never cut off a message prematurely.
			// Strictly speaking, there are no downsides to setting this timeout as high as possible (Just under 37ms),
			// because if this process is waiting, it means that the last message was incomplete, so there is no current
			// data waiting to be published. But we would rather set this timeout lower in case the UWB board is
			// updated to publish data faster.
			_uart_timeout.tv_usec = BYTE_TIMEOUT_US;
		}

		perf_count(_read_count_perf);

		// All of the following criteria must be met for the message to be acceptable:
		//  - Size of message == sizeof(position_msg_t) (51 bytes)
		//  - status == 0x00
		//  - Values of all 3 position measurements are reasonable
		//      (If one or more anchors is missed, then position might be an unreasonably large number.)
		bool ok = buffer_location == sizeof(position_msg_t) && msg.status == 0x00;

		ok &= abs(msg.pos_x) < 100000.0f;
		ok &= abs(msg.pos_y) < 100000.0f;
		ok &= abs(msg.pos_z) < 100000.0f;

		if (ok) {
			_pozyx_report.pos_x = msg.pos_x / 100.0f;
			_pozyx_report.pos_y = msg.pos_y / 100.0f;
			_pozyx_report.pos_z = msg.pos_z / 100.0f;
			_pozyx_report.timestamp = hrt_absolute_time();
			_pozyx_pub.publish(_pozyx_report);

		} else {
			//PX4_ERR("Read %d bytes instead of %d.", (int) buffer_location, (int) sizeof(position_msg_t));
			perf_count(_read_err_perf);

			if (buffer_location == 0) {
				PX4_WARN("UWB module is not responding.");
			}
		}
	}

	written = write(_uart, &CMD_STOP_RANGING, sizeof(CMD_STOP_RANGING));

	if (written < (int) sizeof(CMD_STOP_RANGING)) {
		PX4_ERR("Only wrote %d bytes out of %d.", written, (int) sizeof(CMD_STOP_RANGING));
	}

}

int UWB::custom_command(int argc, char *argv[])
{
	return print_usage("Unrecognized command.");
}

int UWB::print_usage(const char *reason)
{
	if (reason) {
		printf("%s\n\n", reason);
	}

	PRINT_MODULE_USAGE_NAME("uwb", "driver");
	PRINT_MODULE_DESCRIPTION(R"DESC_STR(
### Description

Driver for NXP RDDrone UWB positioning system. This driver publishes a `pozyx_report` message
whenever the RDDrone has a position measurement available.

### Example

Start the driver with a given baud rate:

$ uwb start -b 115200 -d /dev/ttyS2

Start the driver with the value of the `TELEM2_BAUD` parameter:

$ uwb start -b p:TELEM2_BAUD -d /dev/ttyS2
	)DESC_STR");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_PARAM_INT('b', DEFAULT_BAUD, 9600, 921600, "Baud rate for serial communication with UWB", true);
	PRINT_MODULE_USAGE_PARAM_STRING('d', nullptr, "<file:dev>", "Name of device for serial communication with UWB", false);
	PRINT_MODULE_USAGE_COMMAND("stop");
	PRINT_MODULE_USAGE_COMMAND("status");
	return 0;
}

int UWB::task_spawn(int argc, char *argv[])
{
	int task_id = px4_task_spawn_cmd(
			      "uwb_driver",
			      SCHED_DEFAULT,
			      SCHED_PRIORITY_DEFAULT,
			      2048,
			      &run_trampoline,
			      argv
		      );

	if (task_id < 0) {
		return -errno;

	} else {
		_task_id = task_id;
		return 0;
	}
}

UWB *UWB::instantiate(int argc, char *argv[])
{
	int ch;
	int option_index = 1;
	const char *option_arg;
	const char *device_name = nullptr;
	bool error_flag = false;
	int baudrate = DEFAULT_BAUD;

	while ((ch = px4_getopt(argc, argv, "b:d:", &option_index, &option_arg)) != EOF) {
		switch (ch) {
		case 'b':
			if (px4_get_parameter_value(option_arg, baudrate) != 0) {
				PX4_ERR("Error parsing \"%s\"", option_arg);
				error_flag = true;
			}

			break;

		case 'd':
			device_name = option_arg;
			break;

		default:
			PX4_WARN("Unrecognized flag: %c", ch);
			error_flag = true;
			break;
		}
	}

	if (!error_flag && device_name == nullptr) {
		print_usage("Device name not provided.");
		error_flag = true;
	}

	if (!error_flag && baudrate == 0) {
		print_usage("Baudrate not provided.");
		error_flag = true;
	}

	// Right now, the UWB board runs at 115200 baud, with no option to change.
	// However, the way other serial drivers are configured, they take a baudrate argument.
	// To keep this consistent, I accept that parameter, but give an error if it is set wrong.
	// TODO: To whomever reviews this: Is it better to just not accept this command line parameter at all?
	if (baudrate != DEFAULT_BAUD) {
		PX4_WARN("Starting UWB driver with baudrate other than default %d", DEFAULT_BAUD);
	}

	if (error_flag) {
		PX4_WARN("Failed to start UWB driver.");
		return nullptr;

	} else {
		PX4_INFO("Constructing UWB. Device: %s, Baud: %d", device_name, baudrate);
		return new UWB(device_name, baudrate);
	}
}

int uwb_main(int argc, char *argv[])
{
	return UWB::main(argc, argv);
}