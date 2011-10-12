/*
 * zkproxy
 *
 * Copyright (C) 2011 OZAWA Tsuyoshi
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */
#include "logger.h"
#include <stdarg.h>

int is_debug = 0;

void log_write(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	syslog(prio, fmt, ap);
	va_end(ap);
}

int log_init(const char *program_name, int debug_mode, const char *outfile) {
	const char *log_name = program_name;

	openlog(log_name, 0, LOG_DAEMON);
	return 0;
}

