/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CS47L63_COMM_H_
#define _CS47L63_COMM_H_

#include <stdint.h>
#include "cirrus/cs47l63/cs47l63.h"

#define CS47L63_THREAD_PRIO 5
#define CS47L63_STACK_SIZE 700

/**@brief Initialize the CS47L63
 *
 * @param driver   Pointer to CS47L63 driver
 *
 * @return 0 on success.
 */
int cs47l63_comm_init(cs47l63_t *driver);

#endif /* _CS47L63_COMM_H_ */
