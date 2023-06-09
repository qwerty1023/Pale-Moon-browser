/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsfriendapi.h"
#include "nsContentUtils.h"
#include "CPOWTimer.h"

#include "jsapi.h"

CPOWTimer::~CPOWTimer()
{
    if (!cx_) {
        // Monitoring was off when we started the timer.
        return;
    }

    if (!js::GetStopwatchIsMonitoringCPOW(cx_)) {
        // Monitoring has been deactivated while we were in the timer.
        return;
    }

    const int64_t endInterval = JS_Now();
    if (endInterval <= startInterval_) {
        // Do not assume monotonicity.
        return;
    }

    js::AddCPOWPerformanceDelta(cx_, endInterval - startInterval_);
}
