/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_Navigator_h
#define mozilla_dom_Navigator_h

#include "mozilla/MemoryReporting.h"
#include "mozilla/dom/Fetch.h"
#include "mozilla/dom/Nullable.h"
#include "mozilla/ErrorResult.h"
#include "nsIDOMNavigator.h"
#include "nsIMozNavigatorNetwork.h"
#include "nsWrapperCache.h"
#include "nsHashKeys.h"
#include "nsInterfaceHashtable.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsWeakPtr.h"

#ifdef MOZ_ENABLE_NPAPI
class nsPluginArray;
#endif
class nsMimeTypeArray;
class nsPIDOMWindowInner;
class nsIDOMNavigatorSystemMessages;
class nsIPrincipal;
class nsIURI;

namespace mozilla {
namespace dom {
class BodyExtractorBase;
class Geolocation;
class systemMessageCallback;
class MediaDevices;
struct MediaStreamConstraints;
class WakeLock;
class ArrayBufferOrArrayBufferViewOrBlobOrFormDataOrUSVStringOrURLSearchParams;
class ServiceWorkerContainer;
class DOMRequest;
class Clipboard;
} // namespace dom
} // namespace mozilla

//*****************************************************************************
// Navigator: Script "navigator" object
//*****************************************************************************

namespace mozilla {
namespace dom {

class Permissions;

class Promise;

class DesktopNotificationCenter;
class MozIdleObserver;
#ifdef MOZ_GAMEPAD
class Gamepad;
class GamepadServiceTest;
#endif // MOZ_GAMEPAD
class NavigatorUserMediaSuccessCallback;
class NavigatorUserMediaErrorCallback;
class MozGetUserMediaDevicesSuccessCallback;

namespace network {
class Connection;
} // namespace network

class PowerManager;
class LegacyMozTCPSocket;
class StorageManager;

namespace time {
class TimeManager;
} // namespace time

namespace system {
#ifdef MOZ_AUDIO_CHANNEL_MANAGER
class AudioChannelManager;
#endif
} // namespace system

class Navigator final : public nsIDOMNavigator
                      , public nsIMozNavigatorNetwork
                      , public nsWrapperCache
{
public:
  explicit Navigator(nsPIDOMWindowInner* aInnerWindow);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS_AMBIGUOUS(Navigator,
                                                         nsIDOMNavigator)
  NS_DECL_NSIDOMNAVIGATOR
  NS_DECL_NSIMOZNAVIGATORNETWORK

  static void Init();

  void Invalidate();
  nsPIDOMWindowInner *GetWindow() const
  {
    return mWindow;
  }

  void RefreshMIMEArray();

  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

  /**
   * For use during document.write where our inner window changes.
   */
  void SetWindow(nsPIDOMWindowInner *aInnerWindow);

  /**
   * Called when the inner window navigates to a new page.
   */
  void OnNavigation();

  // The XPCOM GetProduct is OK
  // The XPCOM GetLanguage is OK
  void GetUserAgent(nsString& aUserAgent, ErrorResult& /* unused */)
  {
    GetUserAgent(aUserAgent);
  }
  bool OnLine();
  void RegisterProtocolHandler(const nsAString& aScheme, const nsAString& aURL,
                               const nsAString& aTitle, ErrorResult& aRv);
  void RegisterContentHandler(const nsAString& aMIMEType, const nsAString& aURL,
                              const nsAString& aTitle, ErrorResult& aRv);
  nsMimeTypeArray* GetMimeTypes(ErrorResult& aRv);
#ifdef MOZ_ENABLE_NPAPI
  nsPluginArray* GetPlugins(ErrorResult& aRv);
#endif
  Permissions* GetPermissions(ErrorResult& aRv);
  bool GlobalPrivacyControl();
  Geolocation* GetGeolocation(ErrorResult& aRv);

  static void AppName(nsAString& aAppName, bool aUsePrefOverriddenValue);

  static nsresult GetPlatform(nsAString& aPlatform,
                              bool aUsePrefOverriddenValue);

  static nsresult GetAppVersion(nsAString& aAppVersion,
                                bool aUsePrefOverriddenValue);

  static nsresult GetUserAgent(nsPIDOMWindowInner* aWindow,
                               nsIURI* aURI,
                               bool aIsCallerChrome,
                               nsAString& aUserAgent);

  // Clears the user agent cache by calling:
  // NavigatorBinding::ClearCachedUserAgentValue(this);
  void ClearUserAgentCache();

  uint32_t MaxTouchPoints();
  void GetAppCodeName(nsString& aAppCodeName, ErrorResult& aRv)
  {
    aRv = GetAppCodeName(aAppCodeName);
  }
  void GetOscpu(nsString& aOscpu, ErrorResult& aRv)
  {
    aRv = GetOscpu(aOscpu);
  }
  // The XPCOM GetVendor is OK
  // The XPCOM GetVendorSub is OK
  // The XPCOM GetProductSub is OK
  bool CookieEnabled();
  void GetBuildID(nsString& aBuildID, ErrorResult& aRv)
  {
    aRv = GetBuildID(aBuildID);
  }
  PowerManager* GetMozPower(ErrorResult& aRv);
  bool JavaEnabled(ErrorResult& aRv);
  uint64_t HardwareConcurrency();
  bool CpuHasSSE2();
  bool CpuHasAVX();
  bool CpuHasAVX2();
  bool TaintEnabled()
  {
    return false;
  }
  void AddIdleObserver(MozIdleObserver& aObserver, ErrorResult& aRv);
  void RemoveIdleObserver(MozIdleObserver& aObserver, ErrorResult& aRv);
  already_AddRefed<WakeLock> RequestWakeLock(const nsAString &aTopic,
                                             ErrorResult& aRv);

  DesktopNotificationCenter* GetMozNotification(ErrorResult& aRv);
  already_AddRefed<LegacyMozTCPSocket> MozTCPSocket();
  network::Connection* GetConnection(ErrorResult& aRv);
  MediaDevices* GetMediaDevices(ErrorResult& aRv);

#ifdef MOZ_GAMEPAD
  void GetGamepads(nsTArray<RefPtr<Gamepad> >& aGamepads, ErrorResult& aRv);
  GamepadServiceTest* RequestGamepadServiceTest();
#endif // MOZ_GAMEPAD
#ifdef MOZ_TIME_MANAGER
  time::TimeManager* GetMozTime(ErrorResult& aRv);
#endif // MOZ_TIME_MANAGER
#ifdef MOZ_AUDIO_CHANNEL_MANAGER
  system::AudioChannelManager* GetMozAudioChannelManager(ErrorResult& aRv);
#endif // MOZ_AUDIO_CHANNEL_MANAGER

  bool SendBeacon(const nsAString& aUrl,
                  const Nullable<fetch::BodyInit>& aData,
                  ErrorResult& aRv);

  void MozGetUserMedia(const MediaStreamConstraints& aConstraints,
                       NavigatorUserMediaSuccessCallback& aOnSuccess,
                       NavigatorUserMediaErrorCallback& aOnError,
                       ErrorResult& aRv);
  void MozGetUserMediaDevices(const MediaStreamConstraints& aConstraints,
                              MozGetUserMediaDevicesSuccessCallback& aOnSuccess,
                              NavigatorUserMediaErrorCallback& aOnError,
                              uint64_t aInnerWindowID,
                              const nsAString& aCallID,
                              ErrorResult& aRv);

  already_AddRefed<ServiceWorkerContainer> ServiceWorker();
  
  dom::Clipboard* Clipboard();

  static bool Webdriver();
  
  void GetLanguages(nsTArray<nsString>& aLanguages);

  StorageManager* Storage();

  static void GetAcceptLanguages(nsTArray<nsString>& aLanguages);

  // WebIDL helper methods
  static bool HasWakeLockSupport(JSContext* /* unused*/, JSObject* /*unused */);
  static bool HasWifiManagerSupport(JSContext* /* unused */,
                                  JSObject* aGlobal);
  static bool HasUserMediaSupport(JSContext* /* unused */,
                                  JSObject* /* unused */);

  static bool IsE10sEnabled(JSContext* aCx, JSObject* aGlobal);

  nsPIDOMWindowInner* GetParentObject() const
  {
    return GetWindow();
  }

  virtual JSObject* WrapObject(JSContext* cx, JS::Handle<JSObject*> aGivenProto) override;

  // GetWindowFromGlobal returns the inner window for this global, if
  // any, else null.
  static already_AddRefed<nsPIDOMWindowInner> GetWindowFromGlobal(JSObject* aGlobal);

private:
  virtual ~Navigator();

  bool CheckPermission(const char* type);
  static bool CheckPermission(nsPIDOMWindowInner* aWindow, const char* aType);

  // This enum helps SendBeaconInternal to apply different behaviors to body
  // types.
  enum BeaconType {
    eBeaconTypeBlob,
    eBeaconTypeArrayBuffer,
    eBeaconTypeOther
  };

  bool SendBeaconInternal(const nsAString& aUrl,
                          BodyExtractorBase* aBody,
                          BeaconType aType,
                          ErrorResult& aRv);

  RefPtr<nsMimeTypeArray> mMimeTypes;
#ifdef MOZ_ENABLE_NPAPI
  RefPtr<nsPluginArray> mPlugins;
#endif
  RefPtr<Permissions> mPermissions;
  RefPtr<Geolocation> mGeolocation;
  RefPtr<DesktopNotificationCenter> mNotification;
  RefPtr<PowerManager> mPowerManager;
  RefPtr<network::Connection> mConnection;
  RefPtr<dom::Clipboard> mClipboard;
#ifdef MOZ_AUDIO_CHANNEL_MANAGER
  RefPtr<system::AudioChannelManager> mAudioChannelManager;
#endif
  RefPtr<MediaDevices> mMediaDevices;
  RefPtr<time::TimeManager> mTimeManager;
  RefPtr<ServiceWorkerContainer> mServiceWorkerContainer;
  nsCOMPtr<nsPIDOMWindowInner> mWindow;
#ifdef MOZ_GAMEPAD
  RefPtr<GamepadServiceTest> mGamepadServiceTest;
#endif
  nsTArray<RefPtr<Promise> > mVRGetDisplaysPromises;
  nsTArray<uint32_t> mRequestedVibrationPattern;
  RefPtr<StorageManager> mStorageManager;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_Navigator_h
