/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/* Copyright (c) 2019 Mellanox Technologies. All rights reserved */

#ifndef _MLXSW_SPECTRUM_PTP_H
#define _MLXSW_SPECTRUM_PTP_H

#include <linux/device.h>

#include "spectrum.h"

struct mlxsw_sp_ptp_clock;

#if IS_REACHABLE(CONFIG_PTP_1588_CLOCK)

struct mlxsw_sp_ptp_clock *
mlxsw_sp1_ptp_clock_init(struct mlxsw_sp *mlxsw_sp, struct device *dev);

void mlxsw_sp1_ptp_clock_fini(struct mlxsw_sp_ptp_clock *clock);

#else

static inline struct mlxsw_sp_ptp_clock *
mlxsw_sp1_ptp_clock_init(struct mlxsw_sp *mlxsw_sp, struct device *dev)
{
	return NULL;
}

static inline void mlxsw_sp1_ptp_clock_fini(struct mlxsw_sp_ptp_clock *clock)
{
}

#endif

static inline struct mlxsw_sp_ptp_clock *
mlxsw_sp2_ptp_clock_init(struct mlxsw_sp *mlxsw_sp, struct device *dev)
{
	return NULL;
}

static inline void mlxsw_sp2_ptp_clock_fini(struct mlxsw_sp_ptp_clock *clock)
{
}

#endif
