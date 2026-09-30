/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void nape_inertia_reset(void);
void nape_inertia_cancel(void);
void nape_inertia_prepare_motion(int32_t raw_x, int32_t raw_y, uint32_t received_ms,
                                 bool allow_inertia, int32_t *motion_x, int32_t *motion_y);
