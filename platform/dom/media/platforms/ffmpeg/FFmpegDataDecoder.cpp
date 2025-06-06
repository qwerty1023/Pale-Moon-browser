/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/SyncRunnable.h"
#include "mozilla/TaskQueue.h"

#include <string.h>
#ifdef __GNUC__
#include <unistd.h>
#endif

#include "FFmpegLog.h"
#include "FFmpegDataDecoder.h"
#include "prsystem.h"

namespace mozilla
{

StaticMutex FFmpegDataDecoder<LIBAV_VER>::sMonitor;

  FFmpegDataDecoder<LIBAV_VER>::FFmpegDataDecoder(FFmpegLibWrapper* aLib,
                                                  TaskQueue* aTaskQueue,
                                                  MediaDataDecoderCallback* aCallback,
                                                  AVCodecID aCodecID)
  : mLib(aLib)
  , mCallback(aCallback)
  , mCodecContext(nullptr)
  , mFrame(NULL)
  , mExtraData(nullptr)
  , mCodecID(aCodecID)
  , mTaskQueue(aTaskQueue)
  , mIsFlushing(false)
{
  MOZ_ASSERT(aLib);
  MOZ_COUNT_CTOR(FFmpegDataDecoder);
}

FFmpegDataDecoder<LIBAV_VER>::~FFmpegDataDecoder()
{
  MOZ_COUNT_DTOR(FFmpegDataDecoder);
}

nsresult
FFmpegDataDecoder<LIBAV_VER>::InitDecoder()
{
  FFMPEG_LOG("Initializing FFmpeg decoder.");

  AVCodec* codec = FindAVCodec(mLib, mCodecID);
  if (!codec) {
    NS_WARNING("Couldn't find ffmpeg decoder");
    return NS_ERROR_FAILURE;
  }

  StaticMutexAutoLock mon(sMonitor);

  if (!(mCodecContext = mLib->avcodec_alloc_context3(codec))) {
    NS_WARNING("Couldn't init ffmpeg context");
    return NS_ERROR_FAILURE;
  }

  mCodecContext->opaque = this;

  InitCodecContext();

  if (mExtraData) {
    mCodecContext->extradata_size = mExtraData->Length();
    // FFmpeg may use SIMD instructions to access the data which reads the
    // data in 32 bytes block. Must ensure we have enough data to read.
    uint32_t padding_size =
      AV_INPUT_BUFFER_PADDING_SIZE;
    mCodecContext->extradata = static_cast<uint8_t*>(
      mLib->av_malloc(mExtraData->Length() + padding_size));
    if (!mCodecContext->extradata) {
      return MediaResult(NS_ERROR_OUT_OF_MEMORY,
                        RESULT_DETAIL("Couldn't init ffmpeg extradata"));
    }
    memcpy(mCodecContext->extradata,
           mExtraData->Elements(),
           mExtraData->Length());
  } else {
    mCodecContext->extradata_size = 0;
  }

  if (mLib->avcodec_open2(mCodecContext, codec, nullptr) < 0) {
    NS_WARNING("Couldn't initialize ffmpeg decoder");
    mLib->avcodec_close(mCodecContext);
    mLib->av_freep(&mCodecContext);
    return NS_ERROR_FAILURE;
  }

  FFMPEG_LOG("FFmpeg init successful.");
  return NS_OK;
}

void
FFmpegDataDecoder<LIBAV_VER>::Shutdown()
{
  if (mTaskQueue) {
    nsCOMPtr<nsIRunnable> runnable =
      NewRunnableMethod(this, &FFmpegDataDecoder<LIBAV_VER>::ProcessShutdown);
    mTaskQueue->Dispatch(runnable.forget());
  } else {
    ProcessShutdown();
  }
}

void
FFmpegDataDecoder<LIBAV_VER>::ProcessDecode(MediaRawData* aSample)
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());
  if (mIsFlushing) {
    return;
  }
  MediaResult rv = DoDecode(aSample);
  if (NS_FAILED(rv)) {
    mCallback->Error(rv);
  } else {
    mCallback->InputExhausted();
  }
}

void
FFmpegDataDecoder<LIBAV_VER>::Input(MediaRawData* aSample)
{
  mTaskQueue->Dispatch(NewRunnableMethod<RefPtr<MediaRawData>>(
    this, &FFmpegDataDecoder::ProcessDecode, aSample));
}

void
FFmpegDataDecoder<LIBAV_VER>::Flush()
{
  MOZ_ASSERT(mCallback->OnReaderTaskQueue());
  mIsFlushing = true;
  nsCOMPtr<nsIRunnable> runnable =
    NewRunnableMethod(this, &FFmpegDataDecoder<LIBAV_VER>::ProcessFlush);
  SyncRunnable::DispatchToThread(mTaskQueue, runnable);
  mIsFlushing = false;
}

void
FFmpegDataDecoder<LIBAV_VER>::Drain()
{
  MOZ_ASSERT(mCallback->OnReaderTaskQueue());
  nsCOMPtr<nsIRunnable> runnable =
    NewRunnableMethod(this, &FFmpegDataDecoder<LIBAV_VER>::ProcessDrain);
  mTaskQueue->Dispatch(runnable.forget());
}

void
FFmpegDataDecoder<LIBAV_VER>::ProcessFlush()
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());
  if (mCodecContext) {
    mLib->avcodec_flush_buffers(mCodecContext);
  }
}

void
FFmpegDataDecoder<LIBAV_VER>::ProcessShutdown()
{
  StaticMutexAutoLock mon(sMonitor);

  if (mCodecContext) {
    if (mCodecContext->extradata) {
      mLib->av_freep(&mCodecContext->extradata);
    }
    mLib->avcodec_close(mCodecContext);
    mLib->av_freep(&mCodecContext);
    mLib->av_frame_free(&mFrame);
  }
}

AVFrame*
FFmpegDataDecoder<LIBAV_VER>::PrepareFrame()
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());
  if (mFrame) {
    mLib->av_frame_unref(mFrame);
  } else {
    mFrame = mLib->av_frame_alloc();
  }
  return mFrame;
}

/* static */ AVCodec*
FFmpegDataDecoder<LIBAV_VER>::FindAVCodec(FFmpegLibWrapper* aLib,
                                          AVCodecID aCodec)
{
  return aLib->avcodec_find_decoder(aCodec);
}

} // namespace mozilla
