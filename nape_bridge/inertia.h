/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

void nape_inertia_reset(void);
void nape_inertia_track(int32_t dx, int32_t dy, uint32_t received_ms);
