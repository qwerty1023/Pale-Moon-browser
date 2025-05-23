/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PerformanceResourceTiming.h"
#include "mozilla/dom/PerformanceResourceTimingBinding.h"
#include "mozilla/Unused.h"

using namespace mozilla::dom;

NS_IMPL_CYCLE_COLLECTION_INHERITED(PerformanceResourceTiming,
                                   PerformanceEntry,
                                   mPerformance)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(PerformanceResourceTiming,
                                               PerformanceEntry)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(PerformanceResourceTiming)
NS_INTERFACE_MAP_END_INHERITING(PerformanceEntry)

NS_IMPL_ADDREF_INHERITED(PerformanceResourceTiming, PerformanceEntry)
NS_IMPL_RELEASE_INHERITED(PerformanceResourceTiming, PerformanceEntry)

PerformanceResourceTiming::PerformanceResourceTiming(UniquePtr<PerformanceTimingData>&& aPerformanceTiming,
                                                     Performance* aPerformance,
                                                     const nsAString& aName)
  : PerformanceEntry(aPerformance->GetParentObject(), aName, NS_LITERAL_STRING("resource"))
  , mTimingData(Move(aPerformanceTiming))
  , mPerformance(aPerformance)
{
  MOZ_ASSERT(aPerformance, "Parent performance object should be provided");
}

PerformanceResourceTiming::~PerformanceResourceTiming()
{
}

DOMHighResTimeStamp
PerformanceResourceTiming::StartTime() const
{
  // Force the start time to be the earliest of:
  //  - RedirectStart
  //  - WorkerStart
  //  - AsyncOpen
  // Ignore zero values.  The RedirectStart and WorkerStart values
  // can come from earlier redirected channels prior to the AsyncOpen
  // time being recorded.
  DOMHighResTimeStamp redirect = mTimingData->RedirectStartHighRes(mPerformance);
  redirect = redirect ? redirect : DBL_MAX;

  DOMHighResTimeStamp worker = mTimingData->WorkerStartHighRes(mPerformance);
  worker = worker ? worker : DBL_MAX;

  DOMHighResTimeStamp asyncOpen = mTimingData->AsyncOpenHighRes(mPerformance);

  return std::min(asyncOpen, std::min(redirect, worker));
}

JSObject*
PerformanceResourceTiming::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return PerformanceResourceTimingBinding::Wrap(aCx, this, aGivenProto);
}
