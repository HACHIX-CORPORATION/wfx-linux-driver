/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Scan related functions.
 *
 * Copyright (c) 2017-2019, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 */
#ifndef WFX_SCAN_H
#define WFX_SCAN_H

#include <net/mac80211.h>

#include "hif_api_cmd.h"

struct wfx_dev;
struct wfx_vif;

int wfx_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		struct ieee80211_scan_request *req);
void wfx_scan_complete(struct wfx_vif *wvif,
		       const struct hif_ind_scan_cmpl *ind);

#endif /* WFX_SCAN_H */
