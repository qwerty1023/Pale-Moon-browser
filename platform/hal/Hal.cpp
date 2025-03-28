/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Hal.h"

#include "HalImpl.h"
#include "HalLog.h"
#include "HalSandbox.h"
#include "nsIDOMDocument.h"
#include "nsIDOMWindow.h"
#include "nsIDocument.h"
#include "nsIDocShell.h"
#include "nsITabChild.h"
#include "nsIWebNavigation.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"
#include "nsPIDOMWindow.h"
#include "nsJSUtils.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Observer.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/ScreenOrientation.h"
#include "WindowIdentifier.h"

#ifdef XP_WIN
#include <process.h>
#define getpid _getpid
#endif

using namespace mozilla::services;
using namespace mozilla::dom;

#define PROXY_IF_SANDBOXED(_call)                 \
  do {                                            \
    if (InSandbox()) {                            \
      if (!hal_sandbox::HalChildDestroyed()) {    \
        hal_sandbox::_call;                       \
      }                                           \
    } else {                                      \
      hal_impl::_call;                            \
    }                                             \
  } while (0)

#define RETURN_PROXY_IF_SANDBOXED(_call, defValue)\
  do {                                            \
    if (InSandbox()) {                            \
      if (hal_sandbox::HalChildDestroyed()) {     \
        return defValue;                          \
      }                                           \
      return hal_sandbox::_call;                  \
    } else {                                      \
      return hal_impl::_call;                     \
    }                                             \
  } while (0)

namespace mozilla {
namespace hal {

mozilla::LogModule *
GetHalLog()
{
  static mozilla::LazyLogModule sHalLog("hal");
  return sHalLog;
}

namespace {

void
AssertMainThread()
{
  MOZ_ASSERT(NS_IsMainThread());
}

bool
InSandbox()
{
  return GeckoProcessType_Content == XRE_GetProcessType();
}

void
AssertMainProcess()
{
  MOZ_ASSERT(GeckoProcessType_Default == XRE_GetProcessType());
}

bool
WindowIsActive(nsPIDOMWindowInner* aWindow)
{
  nsIDocument* document = aWindow->GetDoc();
  NS_ENSURE_TRUE(document, false);

  return !document->Hidden();
}

} // namespace

template <class InfoType>
class ObserversManager
{
public:
  void AddObserver(Observer<InfoType>* aObserver) {
    if (!mObservers) {
      mObservers = new mozilla::ObserverList<InfoType>();
    }

    mObservers->AddObserver(aObserver);

    if (mObservers->Length() == 1) {
      EnableNotifications();
    }
  }

  void RemoveObserver(Observer<InfoType>* aObserver) {
    bool removed = mObservers && mObservers->RemoveObserver(aObserver);
    if (!removed) {
      return;
    }

    if (mObservers->Length() == 0) {
      DisableNotifications();

      OnNotificationsDisabled();

      delete mObservers;
      mObservers = nullptr;
    }
  }

  void BroadcastInformation(const InfoType& aInfo) {
    // It is possible for mObservers to be nullptr here on some platforms,
    // because a call to BroadcastInformation gets queued up asynchronously
    // while RemoveObserver is running (and before the notifications are
    // disabled). The queued call can then get run after mObservers has
    // been nulled out. See bug 757025.
    if (!mObservers) {
      return;
    }
    mObservers->Broadcast(aInfo);
  }

protected:
  virtual void EnableNotifications() = 0;
  virtual void DisableNotifications() = 0;
  virtual void OnNotificationsDisabled() {}

private:
  mozilla::ObserverList<InfoType>* mObservers;
};

template <class InfoType>
class CachingObserversManager : public ObserversManager<InfoType>
{
public:
  InfoType GetCurrentInformation() {
    if (mHasValidCache) {
      return mInfo;
    }

    GetCurrentInformationInternal(&mInfo);
    mHasValidCache = true;
    return mInfo;
  }

  void CacheInformation(const InfoType& aInfo) {
    mHasValidCache = true;
    mInfo = aInfo;
  }

  void BroadcastCachedInformation() {
    this->BroadcastInformation(mInfo);
  }

protected:
  virtual void GetCurrentInformationInternal(InfoType*) = 0;

  virtual void OnNotificationsDisabled() {
    mHasValidCache = false;
  }

private:
  InfoType                mInfo;
  bool                    mHasValidCache;
};

class NetworkObserversManager : public CachingObserversManager<NetworkInformation>
{
protected:
  void EnableNotifications() {
    PROXY_IF_SANDBOXED(EnableNetworkNotifications());
  }

  void DisableNotifications() {
    PROXY_IF_SANDBOXED(DisableNetworkNotifications());
  }

  void GetCurrentInformationInternal(NetworkInformation* aInfo) {
    PROXY_IF_SANDBOXED(GetCurrentNetworkInformation(aInfo));
  }
};

static NetworkObserversManager&
NetworkObservers()
{
  static NetworkObserversManager sNetworkObservers;
  AssertMainThread();
  return sNetworkObservers;
}

class WakeLockObserversManager : public ObserversManager<WakeLockInformation>
{
protected:
  void EnableNotifications() {
    PROXY_IF_SANDBOXED(EnableWakeLockNotifications());
  }

  void DisableNotifications() {
    PROXY_IF_SANDBOXED(DisableWakeLockNotifications());
  }
};

static WakeLockObserversManager&
WakeLockObservers()
{
  static WakeLockObserversManager sWakeLockObservers;
  AssertMainThread();
  return sWakeLockObservers;
}

class ScreenConfigurationObserversManager : public CachingObserversManager<ScreenConfiguration>
{
protected:
  void EnableNotifications() {
    PROXY_IF_SANDBOXED(EnableScreenConfigurationNotifications());
  }

  void DisableNotifications() {
    PROXY_IF_SANDBOXED(DisableScreenConfigurationNotifications());
  }

  void GetCurrentInformationInternal(ScreenConfiguration* aInfo) {
    PROXY_IF_SANDBOXED(GetCurrentScreenConfiguration(aInfo));
  }
};

static ScreenConfigurationObserversManager&
ScreenConfigurationObservers()
{
  AssertMainThread();
  static ScreenConfigurationObserversManager sScreenConfigurationObservers;
  return sScreenConfigurationObservers;
}

bool GetScreenEnabled()
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(GetScreenEnabled(), false);
}

void SetScreenEnabled(bool aEnabled)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(SetScreenEnabled(aEnabled));
}

bool GetKeyLightEnabled()
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(GetKeyLightEnabled(), false);
}

void SetKeyLightEnabled(bool aEnabled)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(SetKeyLightEnabled(aEnabled));
}

bool GetCpuSleepAllowed()
{
  // Generally for interfaces that are accessible by normal web content
  // we should cache the result and be notified on state changes, like
  // what the battery API does. But since this is only used by
  // privileged interface, the synchronous getter is OK here.
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(GetCpuSleepAllowed(), true);
}

void SetCpuSleepAllowed(bool aAllowed)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(SetCpuSleepAllowed(aAllowed));
}

double GetScreenBrightness()
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(GetScreenBrightness(), 0);
}

void SetScreenBrightness(double aBrightness)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(SetScreenBrightness(clamped(aBrightness, 0.0, 1.0)));
}

class SystemClockChangeObserversManager : public ObserversManager<int64_t>
{
protected:
  void EnableNotifications() {
    PROXY_IF_SANDBOXED(EnableSystemClockChangeNotifications());
  }

  void DisableNotifications() {
    PROXY_IF_SANDBOXED(DisableSystemClockChangeNotifications());
  }
};

static SystemClockChangeObserversManager&
SystemClockChangeObservers()
{
  static SystemClockChangeObserversManager sSystemClockChangeObservers;
  AssertMainThread();
  return sSystemClockChangeObservers;
}

void
RegisterSystemClockChangeObserver(SystemClockChangeObserver* aObserver)
{
  AssertMainThread();
  SystemClockChangeObservers().AddObserver(aObserver);
}

void
UnregisterSystemClockChangeObserver(SystemClockChangeObserver* aObserver)
{
  AssertMainThread();
  SystemClockChangeObservers().RemoveObserver(aObserver);
}

void
NotifySystemClockChange(const int64_t& aClockDeltaMS)
{
  SystemClockChangeObservers().BroadcastInformation(aClockDeltaMS);
}

class SystemTimezoneChangeObserversManager : public ObserversManager<SystemTimezoneChangeInformation>
{
protected:
  void EnableNotifications() {
    PROXY_IF_SANDBOXED(EnableSystemTimezoneChangeNotifications());
  }

  void DisableNotifications() {
    PROXY_IF_SANDBOXED(DisableSystemTimezoneChangeNotifications());
  }
};

static SystemTimezoneChangeObserversManager&
SystemTimezoneChangeObservers()
{
  static SystemTimezoneChangeObserversManager sSystemTimezoneChangeObservers;
  return sSystemTimezoneChangeObservers;
}

void
RegisterSystemTimezoneChangeObserver(SystemTimezoneChangeObserver* aObserver)
{
  AssertMainThread();
  SystemTimezoneChangeObservers().AddObserver(aObserver);
}

void
UnregisterSystemTimezoneChangeObserver(SystemTimezoneChangeObserver* aObserver)
{
  AssertMainThread();
  SystemTimezoneChangeObservers().RemoveObserver(aObserver);
}

void
NotifySystemTimezoneChange(const SystemTimezoneChangeInformation& aSystemTimezoneChangeInfo)
{
  nsJSUtils::ResetTimeZone();
  SystemTimezoneChangeObservers().BroadcastInformation(aSystemTimezoneChangeInfo);
}

void
AdjustSystemClock(int64_t aDeltaMilliseconds)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(AdjustSystemClock(aDeltaMilliseconds));
}

void
SetTimezone(const nsCString& aTimezoneSpec)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(SetTimezone(aTimezoneSpec));
}

int32_t
GetTimezoneOffset()
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(GetTimezoneOffset(), 0);
}

nsCString
GetTimezone()
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(GetTimezone(), nsCString(""));
}

void
EnableSensorNotifications(SensorType aSensor) {
  AssertMainThread();
  PROXY_IF_SANDBOXED(EnableSensorNotifications(aSensor));
}

void
DisableSensorNotifications(SensorType aSensor) {
  AssertMainThread();
  PROXY_IF_SANDBOXED(DisableSensorNotifications(aSensor));
}

typedef mozilla::ObserverList<SensorData> SensorObserverList;
static SensorObserverList* gSensorObservers = nullptr;

static SensorObserverList &
GetSensorObservers(SensorType sensor_type) {
  MOZ_ASSERT(sensor_type < NUM_SENSOR_TYPE);

  if(!gSensorObservers) {
    gSensorObservers = new SensorObserverList[NUM_SENSOR_TYPE];
  }
  return gSensorObservers[sensor_type];
}

void
RegisterSensorObserver(SensorType aSensor, ISensorObserver *aObserver) {
  SensorObserverList &observers = GetSensorObservers(aSensor);

  AssertMainThread();

  observers.AddObserver(aObserver);
  if(observers.Length() == 1) {
    EnableSensorNotifications(aSensor);
  }
}

void
UnregisterSensorObserver(SensorType aSensor, ISensorObserver *aObserver) {
  AssertMainThread();

  if (!gSensorObservers) {
    HAL_ERR("Un-registering a sensor when none have been registered");
    return;
  }

  SensorObserverList &observers = GetSensorObservers(aSensor);
  if (!observers.RemoveObserver(aObserver) || observers.Length() > 0) {
    return;
  }
  DisableSensorNotifications(aSensor);

  // Destroy sSensorObservers only if all observer lists are empty.
  for (int i = 0; i < NUM_SENSOR_TYPE; i++) {
    if (gSensorObservers[i].Length() > 0) {
      return;
    }
  }
  delete [] gSensorObservers;
  gSensorObservers = nullptr;
}

void
NotifySensorChange(const SensorData &aSensorData) {
  SensorObserverList &observers = GetSensorObservers(aSensorData.sensor());

  AssertMainThread();

  observers.Broadcast(aSensorData);
}

void
RegisterNetworkObserver(NetworkObserver* aObserver)
{
  AssertMainThread();
  NetworkObservers().AddObserver(aObserver);
}

void
UnregisterNetworkObserver(NetworkObserver* aObserver)
{
  AssertMainThread();
  NetworkObservers().RemoveObserver(aObserver);
}

void
GetCurrentNetworkInformation(NetworkInformation* aInfo)
{
  AssertMainThread();
  *aInfo = NetworkObservers().GetCurrentInformation();
}

void
NotifyNetworkChange(const NetworkInformation& aInfo)
{
  NetworkObservers().CacheInformation(aInfo);
  NetworkObservers().BroadcastCachedInformation();
}

void Reboot()
{
  AssertMainProcess();
  AssertMainThread();
  PROXY_IF_SANDBOXED(Reboot());
}

void PowerOff()
{
  AssertMainProcess();
  AssertMainThread();
  PROXY_IF_SANDBOXED(PowerOff());
}

void StartForceQuitWatchdog(ShutdownMode aMode, int32_t aTimeoutSecs)
{
  AssertMainProcess();
  AssertMainThread();
  PROXY_IF_SANDBOXED(StartForceQuitWatchdog(aMode, aTimeoutSecs));
}

void
RegisterWakeLockObserver(WakeLockObserver* aObserver)
{
  AssertMainThread();
  WakeLockObservers().AddObserver(aObserver);
}

void
UnregisterWakeLockObserver(WakeLockObserver* aObserver)
{
  AssertMainThread();
  WakeLockObservers().RemoveObserver(aObserver);
}

void
ModifyWakeLock(const nsAString& aTopic,
               WakeLockControl aLockAdjust,
               WakeLockControl aHiddenAdjust,
               uint64_t aProcessID /* = CONTENT_PROCESS_ID_UNKNOWN */)
{
  AssertMainThread();

  if (aProcessID == CONTENT_PROCESS_ID_UNKNOWN) {
    aProcessID = InSandbox() ? ContentChild::GetSingleton()->GetID() :
                               CONTENT_PROCESS_ID_MAIN;
  }

  PROXY_IF_SANDBOXED(ModifyWakeLock(aTopic, aLockAdjust,
                                    aHiddenAdjust, aProcessID));
}

void
GetWakeLockInfo(const nsAString& aTopic, WakeLockInformation* aWakeLockInfo)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(GetWakeLockInfo(aTopic, aWakeLockInfo));
}

void
NotifyWakeLockChange(const WakeLockInformation& aInfo)
{
  AssertMainThread();
  WakeLockObservers().BroadcastInformation(aInfo);
}

void
RegisterScreenConfigurationObserver(ScreenConfigurationObserver* aObserver)
{
  AssertMainThread();
  ScreenConfigurationObservers().AddObserver(aObserver);
}

void
UnregisterScreenConfigurationObserver(ScreenConfigurationObserver* aObserver)
{
  AssertMainThread();
  ScreenConfigurationObservers().RemoveObserver(aObserver);
}

void
GetCurrentScreenConfiguration(ScreenConfiguration* aScreenConfiguration)
{
  AssertMainThread();
  *aScreenConfiguration = ScreenConfigurationObservers().GetCurrentInformation();
}

void
NotifyScreenConfigurationChange(const ScreenConfiguration& aScreenConfiguration)
{
  ScreenConfigurationObservers().CacheInformation(aScreenConfiguration);
  ScreenConfigurationObservers().BroadcastCachedInformation();
}

bool
LockScreenOrientation(const dom::ScreenOrientationInternal& aOrientation)
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(LockScreenOrientation(aOrientation), false);
}

void
UnlockScreenOrientation()
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(UnlockScreenOrientation());
}

static AlarmObserver* sAlarmObserver;

bool
RegisterTheOneAlarmObserver(AlarmObserver* aObserver)
{
  MOZ_ASSERT(!InSandbox());
  MOZ_ASSERT(!sAlarmObserver);

  sAlarmObserver = aObserver;
  RETURN_PROXY_IF_SANDBOXED(EnableAlarm(), false);
}

void
UnregisterTheOneAlarmObserver()
{
  if (sAlarmObserver) {
    sAlarmObserver = nullptr;
    PROXY_IF_SANDBOXED(DisableAlarm());
  }
}

void
NotifyAlarmFired()
{
  if (sAlarmObserver) {
    sAlarmObserver->Notify(void_t());
  }
}

bool
SetAlarm(int32_t aSeconds, int32_t aNanoseconds)
{
  // It's pointless to program an alarm nothing is going to observe ...
  MOZ_ASSERT(sAlarmObserver);
  RETURN_PROXY_IF_SANDBOXED(SetAlarm(aSeconds, aNanoseconds), false);
}

void
SetProcessPriority(int aPid, ProcessPriority aPriority, uint32_t aLRU)
{
  // n.b. The sandboxed implementation crashes; SetProcessPriority works only
  // from the main process.
  PROXY_IF_SANDBOXED(SetProcessPriority(aPid, aPriority, aLRU));
}

void
SetCurrentThreadPriority(hal::ThreadPriority aThreadPriority)
{
  PROXY_IF_SANDBOXED(SetCurrentThreadPriority(aThreadPriority));
}

void
SetThreadPriority(PlatformThreadId aThreadId,
                  hal::ThreadPriority aThreadPriority)
{
  PROXY_IF_SANDBOXED(SetThreadPriority(aThreadId, aThreadPriority));
}

// From HalTypes.h.
const char*
ProcessPriorityToString(ProcessPriority aPriority)
{
  switch (aPriority) {
  case PROCESS_PRIORITY_MASTER:
    return "MASTER";
  case PROCESS_PRIORITY_FOREGROUND_HIGH:
    return "FOREGROUND_HIGH";
  case PROCESS_PRIORITY_FOREGROUND:
    return "FOREGROUND";
  case PROCESS_PRIORITY_FOREGROUND_KEYBOARD:
    return "FOREGROUND_KEYBOARD";
  case PROCESS_PRIORITY_BACKGROUND_PERCEIVABLE:
    return "BACKGROUND_PERCEIVABLE";
  case PROCESS_PRIORITY_BACKGROUND:
    return "BACKGROUND";
  case PROCESS_PRIORITY_UNKNOWN:
    return "UNKNOWN";
  default:
    MOZ_ASSERT(false);
    return "???";
  }
}

const char *
ThreadPriorityToString(ThreadPriority aPriority)
{
  switch (aPriority) {
    case THREAD_PRIORITY_COMPOSITOR:
      return "COMPOSITOR";
    default:
      MOZ_ASSERT(false);
      return "???";
  }
}

void FactoryReset(mozilla::dom::FactoryResetReason& aReason)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(FactoryReset(aReason));
}

uint32_t
GetTotalSystemMemory()
{
  return hal_impl::GetTotalSystemMemory();
}

bool IsHeadphoneEventFromInputDev()
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(IsHeadphoneEventFromInputDev(), false);
}

nsresult StartSystemService(const char* aSvcName, const char* aArgs)
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(StartSystemService(aSvcName, aArgs), NS_ERROR_FAILURE);
}

void StopSystemService(const char* aSvcName)
{
  AssertMainThread();
  PROXY_IF_SANDBOXED(StopSystemService(aSvcName));
}

bool SystemServiceIsRunning(const char* aSvcName)
{
  AssertMainThread();
  RETURN_PROXY_IF_SANDBOXED(SystemServiceIsRunning(aSvcName), false);
}

} // namespace hal
} // namespace mozilla
