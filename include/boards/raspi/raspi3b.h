// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#pragma once

#include "common/mm.h"
#include "common/types.h"

struct page_pool *get_rasp3b_page_pool(void);
