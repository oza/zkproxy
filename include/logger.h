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

#ifndef LOGGER_H
#define LOGGER_H
#include <syslog.h>

#undef dprintf
extern int is_debug;

int log_init(const char *program_name, int debug_mode, const char *log_name);
void log_write(int prio, const char *fmt, ...);

#define eprintf(fmt, args...)						\
do {									\
	log_write(LOG_ERR, "%s(%d) " fmt, __func__, __LINE__, ##args);	\
} while (0)

//	log_write(LOG_ERR, "%s(%d) " fmt, __func__, __LINE__, ##args);	

#define dprintf(fmt, args...)						\
do {									\
	if (unlikely(is_debug))						\
		printf("%s(%d) " fmt, __func__, __LINE__, ##args);\
} while (0)

//	if (unlikely(is_debug))						
//		log_write(LOG_ERR, "%s(%d) " fmt, __func__, __LINE__, ##args);

#endif	/* LOG_H */
