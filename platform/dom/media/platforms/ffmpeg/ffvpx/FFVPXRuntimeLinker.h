/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __FFVPXRuntimeLinker_h__
#define __FFVPXRuntimeLinker_h__

#include "PlatformDecoderModule.h"
#include "ffvpx/tx.h"

struct FFmpegFFTFuncs {
  decltype(av_tx_init)* init;
  decltype(av_tx_uninit)* uninit;
};

namespace mozilla
{

class FFVPXRuntimeLinker
{
public:
  // Main thread only.
  static bool Init();
  // Main thread or after Init().
  static already_AddRefed<PlatformDecoderModule> CreateDecoderModule();

  // Call (on any thread) after Init().
  static void GetFFTFuncs(FFmpegFFTFuncs* aOutFuncs);

private:
  // Set once on the main thread and then read from other threads.
  static enum LinkStatus {
    LinkStatus_INIT = 0,
    LinkStatus_FAILED,
    LinkStatus_SUCCEEDED
  } sLinkStatus;
};

}

#endif /* __FFVPXRuntimeLinker_h__ */
