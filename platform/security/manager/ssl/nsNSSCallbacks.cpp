/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsNSSCallbacks.h"

#include "PSMRunnable.h"
#include "ScopedNSSTypes.h"
#include "SharedCertVerifier.h"
#include "SharedSSLState.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/RefPtr.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Unused.h"
#include "nsContentUtils.h"
#include "nsICertOverrideService.h"
#include "nsIHttpChannelInternal.h"
#include "nsIPrompt.h"
#include "nsISupportsPriority.h"
#include "nsITokenDialogs.h"
#include "nsIUploadChannel.h"
#include "nsIWebProgressListener.h"
#include "nsNSSCertificate.h"
#include "nsNSSComponent.h"
#include "nsNSSIOLayer.h"
#include "nsNetUtil.h"
#include "nsProtectedAuthThread.h"
#include "nsProxyRelease.h"
#include "mozpkix/pkixtypes.h"
#include "ssl.h"
#include "sslproto.h"

using namespace mozilla;
using namespace mozilla::psm;

extern LazyLogModule gPIPNSSLog;

namespace {

// These bits are numbered so that the least subtle issues have higher values.
// This should make it easier for us to interpret the results.
const uint32_t POSSIBLE_VERSION_DOWNGRADE = 4;
const uint32_t POSSIBLE_CIPHER_SUITE_DOWNGRADE = 2;
const uint32_t KEA_NOT_SUPPORTED = 1;

} // namespace

class nsHTTPDownloadEvent : public Runnable {
public:
  nsHTTPDownloadEvent();
  ~nsHTTPDownloadEvent();

  NS_IMETHOD Run();

  nsNSSHttpRequestSession *mRequestSession;
  
  RefPtr<nsHTTPListener> mListener;
  bool mResponsibleForDoneSignal;
  TimeStamp mStartTime;
};

nsHTTPDownloadEvent::nsHTTPDownloadEvent()
:mResponsibleForDoneSignal(true)
{
}

nsHTTPDownloadEvent::~nsHTTPDownloadEvent()
{
  if (mResponsibleForDoneSignal && mListener)
    mListener->send_done_signal();

  mRequestSession->Release();
}

NS_IMETHODIMP
nsHTTPDownloadEvent::Run()
{
  if (!mListener)
    return NS_OK;

  nsresult rv;

  nsCOMPtr<nsIIOService> ios = do_GetIOService();
  NS_ENSURE_STATE(ios);

  nsCOMPtr<nsIChannel> chan;
  ios->NewChannel2(mRequestSession->mURL,
                   nullptr,
                   nullptr,
                   nullptr, // aLoadingNode
                   nsContentUtils::GetSystemPrincipal(),
                   nullptr, // aTriggeringPrincipal
                   nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
                   nsIContentPolicy::TYPE_OTHER,
                   getter_AddRefs(chan));
  NS_ENSURE_STATE(chan);

  // Security operations scheduled through normal HTTP channels are given
  // high priority to accommodate real time OCSP transactions.
  nsCOMPtr<nsISupportsPriority> priorityChannel = do_QueryInterface(chan);
  if (priorityChannel)
    priorityChannel->AdjustPriority(nsISupportsPriority::PRIORITY_HIGHEST);

  chan->SetLoadFlags(nsIRequest::LOAD_ANONYMOUS |
                     nsIChannel::LOAD_BYPASS_SERVICE_WORKER);

  // For OCSP requests, only the first party domain aspect of origin attributes
  // is used. This means that OCSP requests are shared across different
  // containers.
  if (mRequestSession->mOriginAttributes != NeckoOriginAttributes()) {
    NeckoOriginAttributes attrs;
    attrs.mFirstPartyDomain =
      mRequestSession->mOriginAttributes.mFirstPartyDomain;

    nsCOMPtr<nsILoadInfo> loadInfo = chan->GetLoadInfo();
    if (loadInfo) {
      rv = loadInfo->SetOriginAttributes(attrs);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  // Create a loadgroup for this new channel.  This way if the channel
  // is redirected, we'll have a way to cancel the resulting channel.
  nsCOMPtr<nsILoadGroup> lg = do_CreateInstance(NS_LOADGROUP_CONTRACTID);
  chan->SetLoadGroup(lg);

  if (mRequestSession->mHasPostData)
  {
    nsCOMPtr<nsIInputStream> uploadStream;
    rv = NS_NewPostDataStream(getter_AddRefs(uploadStream),
                              false,
                              mRequestSession->mPostData);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIUploadChannel> uploadChannel(do_QueryInterface(chan));
    NS_ENSURE_STATE(uploadChannel);

    rv = uploadChannel->SetUploadStream(uploadStream, 
                                        mRequestSession->mPostContentType,
                                        -1);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Do not use SPDY for internal security operations. It could result
  // in the silent upgrade to ssl, which in turn could require an SSL
  // operation to fulfill something like an OCSP fetch, which is an
  // endless loop.
  nsCOMPtr<nsIHttpChannelInternal> internalChannel = do_QueryInterface(chan);
  if (internalChannel) {
    rv = internalChannel->SetAllowSpdy(false);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsCOMPtr<nsIHttpChannel> hchan = do_QueryInterface(chan);
  NS_ENSURE_STATE(hchan);

  rv = hchan->SetAllowSTS(false);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = hchan->SetRequestMethod(mRequestSession->mRequestMethod);
  NS_ENSURE_SUCCESS(rv, rv);

  mResponsibleForDoneSignal = false;
  mListener->mResponsibleForDoneSignal = true;

  mListener->mLoadGroup = lg.get();
  NS_ADDREF(mListener->mLoadGroup);
  mListener->mLoadGroupOwnerThread = PR_GetCurrentThread();

  rv = NS_NewStreamLoader(getter_AddRefs(mListener->mLoader), 
                          mListener);

  if (NS_SUCCEEDED(rv)) {
    mStartTime = TimeStamp::Now();
    rv = hchan->AsyncOpen2(mListener->mLoader);
  }

  if (NS_FAILED(rv)) {
    mListener->mResponsibleForDoneSignal = false;
    mResponsibleForDoneSignal = true;

    NS_RELEASE(mListener->mLoadGroup);
    mListener->mLoadGroup = nullptr;
    mListener->mLoadGroupOwnerThread = nullptr;
  }

  return NS_OK;
}

struct nsCancelHTTPDownloadEvent : Runnable {
  RefPtr<nsHTTPListener> mListener;

  NS_IMETHOD Run() override {
    mListener->FreeLoadGroup(true);
    mListener = nullptr;
    return NS_OK;
  }
};

mozilla::pkix::Result
nsNSSHttpServerSession::createSessionFcn(const char* host,
                                         uint16_t portnum,
                                 /*out*/ nsNSSHttpServerSession** pSession)
{
  if (!host || !pSession) {
    return Result::FATAL_ERROR_INVALID_ARGS;
  }

  nsNSSHttpServerSession* hss = new nsNSSHttpServerSession;
  if (!hss) {
    return Result::FATAL_ERROR_NO_MEMORY;
  }

  hss->mHost = host;
  hss->mPort = portnum;

  *pSession = hss;
  return Success;
}

mozilla::pkix::Result
nsNSSHttpRequestSession::createFcn(const nsNSSHttpServerSession* session,
                                   const char* http_protocol_variant,
                                   const char* path_and_query_string,
                                   const char* http_request_method,
                                   const NeckoOriginAttributes& origin_attributes,
                                   const PRIntervalTime timeout,
                           /*out*/ nsNSSHttpRequestSession** pRequest)
{
  if (!session || !http_protocol_variant || !path_and_query_string ||
      !http_request_method || !pRequest) {
    return Result::FATAL_ERROR_INVALID_ARGS;
  }

  nsNSSHttpRequestSession* rs = new nsNSSHttpRequestSession;
  if (!rs) {
    return Result::FATAL_ERROR_NO_MEMORY;
  }

  rs->mTimeoutInterval = timeout;

  // Use a maximum timeout value of 10 seconds because of bug 404059.
  // FIXME: Use a better approach once 406120 is ready.
  uint32_t maxBug404059Timeout = PR_TicksPerSecond() * 10;
  if (timeout > maxBug404059Timeout) {
    rs->mTimeoutInterval = maxBug404059Timeout;
  }

  rs->mURL.Assign(http_protocol_variant);
  rs->mURL.AppendLiteral("://");
  rs->mURL.Append(session->mHost);
  rs->mURL.Append(':');
  rs->mURL.AppendInt(session->mPort);
  rs->mURL.Append(path_and_query_string);

  rs->mOriginAttributes = origin_attributes;

  rs->mRequestMethod = http_request_method;

  *pRequest = rs;
  return Success;
}

mozilla::pkix::Result
nsNSSHttpRequestSession::setPostDataFcn(const char* http_data,
                                        const uint32_t http_data_len,
                                        const char* http_content_type)
{
  mHasPostData = true;
  mPostData.Assign(http_data, http_data_len);
  mPostContentType.Assign(http_content_type);

  return Success;
}

mozilla::pkix::Result
nsNSSHttpRequestSession::trySendAndReceiveFcn(PRPollDesc** pPollDesc,
                                              uint16_t* http_response_code,
                                              const char** http_response_content_type,
                                              const char** http_response_headers,
                                              const char** http_response_data,
                                              uint32_t* http_response_data_len)
{
  MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
         ("nsNSSHttpRequestSession::trySendAndReceiveFcn to %s\n", mURL.get()));

  bool onSTSThread;
  nsresult nrv;
  nsCOMPtr<nsIEventTarget> sts
    = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &nrv);
  if (NS_FAILED(nrv)) {
    NS_ERROR("Could not get STS service");
    return Result::FATAL_ERROR_INVALID_STATE;
  }

  nrv = sts->IsOnCurrentThread(&onSTSThread);
  if (NS_FAILED(nrv)) {
    NS_ERROR("IsOnCurrentThread failed");
    return Result::FATAL_ERROR_INVALID_STATE;
  }

  if (onSTSThread) {
    NS_ERROR("nsNSSHttpRequestSession::trySendAndReceiveFcn called on socket "
             "thread; this will not work.");
    return Result::FATAL_ERROR_INVALID_STATE;
  }

  const int max_retries = 2;
  int retry_count = 0;
  bool retryable_error = false;
  Result rv = Result::ERROR_UNKNOWN_ERROR;

  do
  {
    if (retry_count > 0)
    {
      if (retryable_error)
      {
        MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
               ("nsNSSHttpRequestSession::trySendAndReceiveFcn - sleeping and retrying: %d of %d\n",
                retry_count, max_retries));
      }

      PR_Sleep( PR_MillisecondsToInterval(300) * retry_count );
    }

    ++retry_count;
    retryable_error = false;

    rv =
      internal_send_receive_attempt(retryable_error, pPollDesc, http_response_code,
                                    http_response_content_type, http_response_headers,
                                    http_response_data, http_response_data_len);
  }
  while (retryable_error &&
         retry_count < max_retries);

  if (retry_count > 1)
  {
    if (retryable_error)
      MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
             ("nsNSSHttpRequestSession::trySendAndReceiveFcn - still failing, giving up...\n"));
    else
      MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
             ("nsNSSHttpRequestSession::trySendAndReceiveFcn - success at attempt %d\n",
              retry_count));
  }

  return rv;
}

void
nsNSSHttpRequestSession::AddRef()
{
  ++mRefCount;
}

void
nsNSSHttpRequestSession::Release()
{
  int32_t newRefCount = --mRefCount;
  if (!newRefCount) {
    delete this;
  }
}

mozilla::pkix::Result
nsNSSHttpRequestSession::internal_send_receive_attempt(bool &retryable_error,
                                                       PRPollDesc **pPollDesc,
                                                       uint16_t *http_response_code,
                                                       const char **http_response_content_type,
                                                       const char **http_response_headers,
                                                       const char **http_response_data,
                                                       uint32_t *http_response_data_len)
{
  if (pPollDesc) *pPollDesc = nullptr;
  if (http_response_code) *http_response_code = 0;
  if (http_response_content_type) *http_response_content_type = 0;
  if (http_response_headers) *http_response_headers = 0;
  if (http_response_data) *http_response_data = 0;

  uint32_t acceptableResultSize = 0;

  if (http_response_data_len)
  {
    acceptableResultSize = *http_response_data_len;
    *http_response_data_len = 0;
  }

  if (!mListener) {
    return Result::FATAL_ERROR_INVALID_STATE;
  }

  Mutex& waitLock = mListener->mLock;
  CondVar& waitCondition = mListener->mCondition;
  volatile bool &waitFlag = mListener->mWaitFlag;
  waitFlag = true;

  RefPtr<nsHTTPDownloadEvent> event(new nsHTTPDownloadEvent);
  if (!event) {
    return Result::FATAL_ERROR_NO_MEMORY;
  }

  event->mListener = mListener;
  this->AddRef();
  event->mRequestSession = this;

  nsresult rv = NS_DispatchToMainThread(event);
  if (NS_FAILED(rv)) {
    event->mResponsibleForDoneSignal = false;
    return Result::FATAL_ERROR_LIBRARY_FAILURE;
  }

  bool request_canceled = false;

  {
    MutexAutoLock locker(waitLock);

    const PRIntervalTime start_time = PR_IntervalNow();
    PRIntervalTime wait_interval;

    bool running_on_main_thread = NS_IsMainThread();
    if (running_on_main_thread)
    {
      // The result of running this on the main thread
      // is a series of small timeouts mixed with spinning the
      // event loop - this is always dangerous as there is so much main
      // thread code that does not expect to be called re-entrantly. Your
      // app really shouldn't do that.
      NS_WARNING("Security network blocking I/O on Main Thread");

      // let's process events quickly
      wait_interval = PR_MicrosecondsToInterval(50);
    }
    else
    { 
      // On a secondary thread, it's fine to wait some more for
      // for the condition variable.
      wait_interval = PR_MillisecondsToInterval(250);
    }

    while (waitFlag)
    {
      if (running_on_main_thread)
      {
        // Networking runs on the main thread, which we happen to block here.
        // Processing events will allow the OCSP networking to run while we 
        // are waiting. Thanks a lot to Darin Fisher for rewriting the 
        // thread manager. Thanks a lot to Christian Biesinger who
        // made me aware of this possibility. (kaie)

        MutexAutoUnlock unlock(waitLock);
        NS_ProcessNextEvent(nullptr);
      }

      waitCondition.Wait(wait_interval);
      
      if (!waitFlag)
        break;

      if (!request_canceled)
      {
        bool timeout = 
          (PRIntervalTime)(PR_IntervalNow() - start_time) > mTimeoutInterval;
 
        if (timeout)
        {
          request_canceled = true;

          RefPtr<nsCancelHTTPDownloadEvent> cancelevent(
            new nsCancelHTTPDownloadEvent);
          cancelevent->mListener = mListener;
          rv = NS_DispatchToMainThread(cancelevent);
          if (NS_FAILED(rv)) {
            NS_WARNING("cannot post cancel event");
          }
          break;
        }
      }
    }
  }

  if (request_canceled) {
    return Result::ERROR_OCSP_SERVER_ERROR;
  }

  if (NS_FAILED(mListener->mResultCode)) {
    if (mListener->mResultCode == NS_ERROR_CONNECTION_REFUSED ||
        mListener->mResultCode == NS_ERROR_NET_RESET) {
      retryable_error = true;
    }
    return Result::ERROR_OCSP_SERVER_ERROR;
  }

  if (http_response_code)
    *http_response_code = mListener->mHttpResponseCode;

  if (mListener->mHttpRequestSucceeded && http_response_data &&
      http_response_data_len) {
    *http_response_data_len = mListener->mResultLen;

    // acceptableResultSize == 0 means: any size is acceptable
    if (acceptableResultSize != 0 &&
        acceptableResultSize < mListener->mResultLen) {
      return Result::ERROR_OCSP_SERVER_ERROR;
    }

    // Return data by reference, result data will be valid until "this" gets
    // destroyed.
    *http_response_data = (const char*)mListener->mResultData;
  }

  if (mListener->mHttpRequestSucceeded && http_response_content_type) {
    if (mListener->mHttpResponseContentType.Length()) {
      *http_response_content_type = mListener->mHttpResponseContentType.get();
    }
  }

  return Success;
}

nsNSSHttpRequestSession::nsNSSHttpRequestSession()
: mRefCount(1),
  mHasPostData(false),
  mTimeoutInterval(0),
  mListener(new nsHTTPListener)
{
}

nsNSSHttpRequestSession::~nsNSSHttpRequestSession()
{
}

nsHTTPListener::nsHTTPListener()
: mResultData(nullptr),
  mResultLen(0),
  mLock("nsHTTPListener.mLock"),
  mCondition(mLock, "nsHTTPListener.mCondition"),
  mWaitFlag(true),
  mResponsibleForDoneSignal(false),
  mLoadGroup(nullptr),
  mLoadGroupOwnerThread(nullptr)
{
}

nsHTTPListener::~nsHTTPListener()
{
  if (mResponsibleForDoneSignal)
    send_done_signal();

  if (mResultData) {
    free(const_cast<uint8_t *>(mResultData));
  }

  if (mLoader) {
    NS_ReleaseOnMainThread(mLoader.forget());
  }
}

NS_IMPL_ISUPPORTS(nsHTTPListener, nsIStreamLoaderObserver)

void
nsHTTPListener::FreeLoadGroup(bool aCancelLoad)
{
  nsILoadGroup *lg = nullptr;

  MutexAutoLock locker(mLock);

  if (mLoadGroup) {
    if (mLoadGroupOwnerThread != PR_GetCurrentThread()) {
      NS_ASSERTION(false,
                   "attempt to access nsHTTPDownloadEvent::mLoadGroup on multiple threads, leaking it!");
    }
    else {
      lg = mLoadGroup;
      mLoadGroup = nullptr;
    }
  }

  if (lg) {
    if (aCancelLoad) {
      lg->Cancel(NS_ERROR_ABORT);
    }
    NS_RELEASE(lg);
  }
}

NS_IMETHODIMP
nsHTTPListener::OnStreamComplete(nsIStreamLoader* aLoader,
                                 nsISupports* aContext,
                                 nsresult aStatus,
                                 uint32_t stringLen,
                                 const uint8_t* string)
{
  mResultCode = aStatus;

  FreeLoadGroup(false);

  nsCOMPtr<nsIRequest> req;
  nsCOMPtr<nsIHttpChannel> hchan;

  nsresult rv = aLoader->GetRequest(getter_AddRefs(req));
  
  if (NS_FAILED(aStatus))
  {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
           ("nsHTTPListener::OnStreamComplete status failed %d", aStatus));
  }

  if (NS_SUCCEEDED(rv))
    hchan = do_QueryInterface(req, &rv);

  if (NS_SUCCEEDED(rv))
  {
    rv = hchan->GetRequestSucceeded(&mHttpRequestSucceeded);
    if (NS_FAILED(rv))
      mHttpRequestSucceeded = false;

    mResultLen = stringLen;
    mResultData = string; // take ownership of allocation
    aStatus = NS_SUCCESS_ADOPTED_DATA;

    unsigned int rcode;
    rv = hchan->GetResponseStatus(&rcode);
    if (NS_FAILED(rv))
      mHttpResponseCode = 500;
    else
      mHttpResponseCode = rcode;

    hchan->GetResponseHeader(NS_LITERAL_CSTRING("Content-Type"), 
                                    mHttpResponseContentType);
  }

  if (mResponsibleForDoneSignal)
    send_done_signal();
  
  return aStatus;
}

void nsHTTPListener::send_done_signal()
{
  mResponsibleForDoneSignal = false;

  {
    MutexAutoLock locker(mLock);
    mWaitFlag = false;
    mCondition.NotifyAll();
  }
}

static char*
ShowProtectedAuthPrompt(PK11SlotInfo* slot, nsIInterfaceRequestor *ir)
{
  if (!NS_IsMainThread()) {
    NS_ERROR("ShowProtectedAuthPrompt called off the main thread");
    return nullptr;
  }

  char* protAuthRetVal = nullptr;

  // Get protected auth dialogs
  nsCOMPtr<nsITokenDialogs> dialogs;
  nsresult nsrv = getNSSDialogs(getter_AddRefs(dialogs),
                                NS_GET_IID(nsITokenDialogs),
                                NS_TOKENDIALOGS_CONTRACTID);
  if (NS_SUCCEEDED(nsrv))
  {
    nsProtectedAuthThread* protectedAuthRunnable = new nsProtectedAuthThread();
    if (protectedAuthRunnable)
    {
      NS_ADDREF(protectedAuthRunnable);

      protectedAuthRunnable->SetParams(slot);
      
      nsCOMPtr<nsIProtectedAuthThread> runnable = do_QueryInterface(protectedAuthRunnable);
      if (runnable)
      {
        nsrv = dialogs->DisplayProtectedAuth(ir, runnable);
              
        // We call join on the thread,
        // so we can be sure that no simultaneous access will happen.
        protectedAuthRunnable->Join();
              
        if (NS_SUCCEEDED(nsrv))
        {
          SECStatus rv = protectedAuthRunnable->GetResult();
          switch (rv)
          {
              case SECSuccess:
                  protAuthRetVal = ToNewCString(nsDependentCString(PK11_PW_AUTHENTICATED));
                  break;
              case SECWouldBlock:
                  protAuthRetVal = ToNewCString(nsDependentCString(PK11_PW_RETRY));
                  break;
              default:
                  protAuthRetVal = nullptr;
                  break;
          }
        }
      }

      NS_RELEASE(protectedAuthRunnable);
    }
  }

  return protAuthRetVal;
}

class PK11PasswordPromptRunnable : public SyncRunnableBase
                                 , public nsNSSShutDownObject
{
public:
  PK11PasswordPromptRunnable(PK11SlotInfo* slot, 
                             nsIInterfaceRequestor* ir)
    : mResult(nullptr),
      mSlot(slot),
      mIR(ir)
  {
  }
  virtual ~PK11PasswordPromptRunnable();

  // This doesn't own the PK11SlotInfo or any other NSS objects, so there's
  // nothing to release.
  virtual void virtualDestroyNSSReference() override {}
  char * mResult; // out
  virtual void RunOnTargetThread() override;
private:
  PK11SlotInfo* const mSlot; // in
  nsIInterfaceRequestor* const mIR; // in
};

PK11PasswordPromptRunnable::~PK11PasswordPromptRunnable()
{
  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return;
  }

  shutdown(ShutdownCalledFrom::Object);
}

void
PK11PasswordPromptRunnable::RunOnTargetThread()
{
  static NS_DEFINE_CID(kNSSComponentCID, NS_NSSCOMPONENT_CID);

  nsNSSShutDownPreventionLock locker;
  if (isAlreadyShutDown()) {
    return;
  }

  nsresult rv;
  nsCOMPtr<nsIPrompt> prompt;
  if (!mIR) {
    rv = nsNSSComponent::GetNewPrompter(getter_AddRefs(prompt));
    if (NS_FAILED(rv)) {
      return;
    }
  } else {
    prompt = do_GetInterface(mIR);
    MOZ_ASSERT(prompt, "Interface requestor should implement nsIPrompt");
  }

  if (!prompt) {
    return;
  }

  if (PK11_ProtectedAuthenticationPath(mSlot)) {
    mResult = ShowProtectedAuthPrompt(mSlot, mIR);
    return;
  }

  nsCOMPtr<nsINSSComponent> nssComponent(do_GetService(kNSSComponentCID));
  if (!nssComponent) {
    return;
  }

  NS_ConvertUTF8toUTF16 tokenName(PK11_GetTokenName(mSlot));
  const char16_t* formatStrings[] = {
    tokenName.get(),
  };
  nsAutoString promptString;
  rv = nssComponent->PIPBundleFormatStringFromName("CertPassPrompt",
                                                   formatStrings,
                                                   ArrayLength(formatStrings),
                                                   promptString);
  if (NS_FAILED(rv)) {
    return;
  }

  nsXPIDLString password;
  // |checkState| is unused because |checkMsg| (the argument just before it) is
  // null, but XPConnect requires it to point to a valid bool nonetheless.
  bool checkState = false;
  bool userClickedOK = false;
  rv = prompt->PromptPassword(nullptr, promptString.get(),
                              getter_Copies(password), nullptr, &checkState,
                              &userClickedOK);
  if (NS_FAILED(rv) || !userClickedOK) {
    return;
  }

  mResult = ToNewUTF8String(password);
}

char*
PK11PasswordPrompt(PK11SlotInfo* slot, PRBool /*retry*/, void* arg)
{
  RefPtr<PK11PasswordPromptRunnable> runnable(
    new PK11PasswordPromptRunnable(slot,
                                   static_cast<nsIInterfaceRequestor*>(arg)));
  runnable->DispatchToMainThreadAndWait();
  return runnable->mResult;
}

static nsCString
getKeaGroupName(uint32_t aKeaGroup)
{
  nsCString groupName;
  switch (aKeaGroup) {
    case ssl_grp_ec_secp256r1:
      groupName = NS_LITERAL_CSTRING("P256");
      break;
    case ssl_grp_ec_secp384r1:
      groupName = NS_LITERAL_CSTRING("P384");
      break;
    case ssl_grp_ec_secp521r1:
      groupName = NS_LITERAL_CSTRING("P521");
      break;
    case ssl_grp_ec_curve25519:
      groupName = NS_LITERAL_CSTRING("x25519");
      break;
    case ssl_grp_ffdhe_2048:
      groupName = NS_LITERAL_CSTRING("FF 2048");
      break;
    case ssl_grp_ffdhe_3072:
      groupName = NS_LITERAL_CSTRING("FF 3072");
      break;
    case ssl_grp_none:
      groupName = NS_LITERAL_CSTRING("none");
      break;
    case ssl_grp_ffdhe_custom:
      groupName = NS_LITERAL_CSTRING("custom");
      break;
    // All other groups are not enabled in Firefox. See namedGroups in
    // nsNSSIOLayer.cpp.
    default:
      // This really shouldn't happen!
      MOZ_ASSERT_UNREACHABLE("Invalid key exchange group.");
      groupName = NS_LITERAL_CSTRING("unknown group");
  }
  return groupName;
}

static nsCString
getSignatureName(uint32_t aSignatureScheme)
{
  nsCString signatureName;
  switch (aSignatureScheme) {
    case ssl_sig_none:
      signatureName = NS_LITERAL_CSTRING("none");
      break;
    case ssl_sig_rsa_pkcs1_sha1:
      signatureName = NS_LITERAL_CSTRING("RSA-PKCS1-SHA1");
      break;
    case ssl_sig_rsa_pkcs1_sha256:
      signatureName = NS_LITERAL_CSTRING("RSA-PKCS1-SHA256");
      break;
    case ssl_sig_rsa_pkcs1_sha384:
      signatureName = NS_LITERAL_CSTRING("RSA-PKCS1-SHA384");
      break;
    case ssl_sig_rsa_pkcs1_sha512:
      signatureName = NS_LITERAL_CSTRING("RSA-PKCS1-SHA512");
      break;
    case ssl_sig_ecdsa_secp256r1_sha256:
      signatureName = NS_LITERAL_CSTRING("ECDSA-P256-SHA256");
      break;
    case ssl_sig_ecdsa_secp384r1_sha384:
      signatureName = NS_LITERAL_CSTRING("ECDSA-P384-SHA384");
      break;
    case ssl_sig_ecdsa_secp521r1_sha512:
      signatureName = NS_LITERAL_CSTRING("ECDSA-P521-SHA512");
      break;
    case ssl_sig_rsa_pss_sha256:
      signatureName = NS_LITERAL_CSTRING("RSA-PSS-SHA256");
      break;
    case ssl_sig_rsa_pss_sha384:
      signatureName = NS_LITERAL_CSTRING("RSA-PSS-SHA384");
      break;
    case ssl_sig_rsa_pss_sha512:
      signatureName = NS_LITERAL_CSTRING("RSA-PSS-SHA512");
      break;
    case ssl_sig_ecdsa_sha1:
      signatureName = NS_LITERAL_CSTRING("ECDSA-SHA1");
      break;
    case ssl_sig_rsa_pkcs1_sha1md5:
      signatureName = NS_LITERAL_CSTRING("RSA-PKCS1-SHA1MD5");
      break;
    // All other groups are not enabled in Firefox. See sEnabledSignatureSchemes
    // in nsNSSIOLayer.cpp.
    default:
      // This really shouldn't happen!
      MOZ_ASSERT_UNREACHABLE("Invalid signature scheme.");
      signatureName = NS_LITERAL_CSTRING("unknown signature");
  }
  return signatureName;
}

// call with shutdown prevention lock held
static void
PreliminaryHandshakeDone(PRFileDesc* fd)
{
  nsNSSSocketInfo* infoObject = (nsNSSSocketInfo*) fd->higher->secret;
  if (!infoObject)
    return;

  SSLChannelInfo channelInfo;
  if (SSL_GetChannelInfo(fd, &channelInfo, sizeof(channelInfo)) == SECSuccess) {
    infoObject->SetSSLVersionUsed(channelInfo.protocolVersion);
    infoObject->SetEarlyDataAccepted(channelInfo.earlyDataAccepted);

    SSLCipherSuiteInfo cipherInfo;
    if (SSL_GetCipherSuiteInfo(channelInfo.cipherSuite, &cipherInfo,
                               sizeof cipherInfo) == SECSuccess) {
      /* Set the SSL Status information */
      RefPtr<nsSSLStatus> status(infoObject->SSLStatus());
      if (!status) {
        status = new nsSSLStatus();
        infoObject->SetSSLStatus(status);
      }

      status->mHaveCipherSuiteAndProtocol = true;
      status->mCipherSuite = channelInfo.cipherSuite;
      status->mProtocolVersion = channelInfo.protocolVersion & 0xFF;
      status->mKeaGroup.Assign(getKeaGroupName(channelInfo.keaGroup));
      status->mSignatureSchemeName.Assign(
        getSignatureName(channelInfo.signatureScheme));
      infoObject->SetKEAUsed(channelInfo.keaType);
      infoObject->SetKEAKeyBits(channelInfo.keaKeyBits);
      infoObject->SetMACAlgorithmUsed(cipherInfo.macAlgorithm);
    }
  }

  // Don't update NPN details on renegotiation.
  if (infoObject->IsPreliminaryHandshakeDone()) {
    return;
  }

  // Get the NPN value.
  SSLNextProtoState state;
  unsigned char npnbuf[256];
  unsigned int npnlen;

  if (SSL_GetNextProto(fd, &state, npnbuf, &npnlen,
                       AssertedCast<unsigned int>(ArrayLength(npnbuf)))
        == SECSuccess) {
    if (state == SSL_NEXT_PROTO_NEGOTIATED ||
        state == SSL_NEXT_PROTO_SELECTED) {
      infoObject->SetNegotiatedNPN(BitwiseCast<char*, unsigned char*>(npnbuf),
                                   npnlen);
    } else {
      infoObject->SetNegotiatedNPN(nullptr, 0);
    }
  } else {
    infoObject->SetNegotiatedNPN(nullptr, 0);
  }

  infoObject->SetPreliminaryHandshakeDone();
}

SECStatus
CanFalseStartCallback(PRFileDesc* fd, void* client_data, PRBool *canFalseStart)
{
  *canFalseStart = false;

  nsNSSShutDownPreventionLock locker;

  nsNSSSocketInfo* infoObject = (nsNSSSocketInfo*) fd->higher->secret;
  if (!infoObject) {
    PR_SetError(PR_INVALID_STATE_ERROR, 0);
    return SECFailure;
  }

  infoObject->SetFalseStartCallbackCalled();

  if (infoObject->isAlreadyShutDown()) {
    MOZ_CRASH("SSL socket used after NSS shut down");
    PR_SetError(PR_INVALID_STATE_ERROR, 0);
    return SECFailure;
  }

  PreliminaryHandshakeDone(fd);

  uint32_t reasonsForNotFalseStarting = 0;

  SSLChannelInfo channelInfo;
  if (SSL_GetChannelInfo(fd, &channelInfo, sizeof(channelInfo)) != SECSuccess) {
    return SECSuccess;
  }

  SSLCipherSuiteInfo cipherInfo;
  if (SSL_GetCipherSuiteInfo(channelInfo.cipherSuite, &cipherInfo,
                             sizeof (cipherInfo)) != SECSuccess) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("CanFalseStartCallback [%p] failed - "
                                      " KEA %d\n", fd,
                                      static_cast<int32_t>(channelInfo.keaType)));
    return SECSuccess;
  }

  // Prevent version downgrade attacks from TLS 1.2, and avoid False Start for
  // TLS 1.3 and later. See Bug 861310 for all the details as to why.
  if (channelInfo.protocolVersion != SSL_LIBRARY_VERSION_TLS_1_2) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("CanFalseStartCallback [%p] failed - "
                                      "SSL Version must be TLS 1.2, was %x\n", fd,
                                      static_cast<int32_t>(channelInfo.protocolVersion)));
    reasonsForNotFalseStarting |= POSSIBLE_VERSION_DOWNGRADE;
  }

  // See bug 952863 for why ECDHE is allowed, but DHE (and RSA) are not.
  if (channelInfo.keaType != ssl_kea_ecdh) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("CanFalseStartCallback [%p] failed - "
                                      "unsupported KEA %d\n", fd,
                                      static_cast<int32_t>(channelInfo.keaType)));
    reasonsForNotFalseStarting |= KEA_NOT_SUPPORTED;
  }

  // Prevent downgrade attacks on the symmetric cipher. We do not allow CBC
  // mode due to BEAST, POODLE, and other attacks on the MAC-then-Encrypt
  // design. See bug 1109766 for more details.
  if (cipherInfo.macAlgorithm != ssl_mac_aead) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
           ("CanFalseStartCallback [%p] failed - non-AEAD cipher used, %d, "
            "is not supported with False Start.\n", fd,
            static_cast<int32_t>(cipherInfo.symCipher)));
    reasonsForNotFalseStarting |= POSSIBLE_CIPHER_SUITE_DOWNGRADE;
  }

  // XXX: An attacker can choose which protocols are advertised in the
  // ALPN extension. TODO(Bug 861311): We should restrict the ability
  // of an attacker leverage this capability by restricting false start
  // to the same protocol we previously saw for the server, after the
  // first successful connection to the server.

  if (reasonsForNotFalseStarting == 0) {
    *canFalseStart = PR_TRUE;
    infoObject->SetFalseStarted();
    infoObject->NoteTimeUntilReady();
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug, ("CanFalseStartCallback [%p] ok\n", fd));
  }

  return SECSuccess;
}

// In the case of session resumption, the AuthCertificate hook has been bypassed
// (because we've previously successfully connected to our peer). That being the
// case, we unfortunately don't know if the peer's server certificate verified
// as extended validation or not. To address this, we attempt to build a
// verified EV certificate chain here using as much of the original context as
// possible (e.g. stapled OCSP responses, SCTs, the hostname, the first party
// domain, etc.). Note that because we are on the socket thread, this must not
// cause any network requests, hence the use of FLAG_LOCAL_ONLY.
static void
DetermineEVStatusAndSetNewCert(RefPtr<nsSSLStatus> sslStatus, PRFileDesc* fd,
                               nsNSSSocketInfo* infoObject)
{
  MOZ_ASSERT(sslStatus);
  MOZ_ASSERT(fd);
  MOZ_ASSERT(infoObject);

  if (!sslStatus || !fd || !infoObject) {
    return;
  }

  UniqueCERTCertificate cert(SSL_PeerCertificate(fd));
  MOZ_ASSERT(cert, "SSL_PeerCertificate failed in TLS handshake callback?");
  if (!cert) {
    return;
  }

  RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
  MOZ_ASSERT(certVerifier,
             "Certificate verifier uninitialized in TLS handshake callback?");
  if (!certVerifier) {
    return;
  }

  // We don't own these pointers.
  const SECItemArray* stapledOCSPResponses = SSL_PeerStapledOCSPResponses(fd);
  const SECItem* stapledOCSPResponse = nullptr;
  // we currently only support single stapled responses
  if (stapledOCSPResponses && stapledOCSPResponses->len == 1) {
    stapledOCSPResponse = &stapledOCSPResponses->items[0];
  }
  const SECItem* sctsFromTLSExtension = SSL_PeerSignedCertTimestamps(fd);
  if (sctsFromTLSExtension && sctsFromTLSExtension->len == 0) {
    // SSL_PeerSignedCertTimestamps returns null on error and empty item
    // when no extension was returned by the server. We always use null when
    // no extension was received (for whatever reason), ignoring errors.
    sctsFromTLSExtension = nullptr;
  }

  int flags = mozilla::psm::CertVerifier::FLAG_LOCAL_ONLY |
              mozilla::psm::CertVerifier::FLAG_MUST_BE_EV;
  if (!infoObject->SharedState().IsOCSPStaplingEnabled() ||
      !infoObject->SharedState().IsOCSPMustStapleEnabled()) {
    flags |= CertVerifier::FLAG_TLS_IGNORE_STATUS_REQUEST;
  }

  SECOidTag evOidPolicy;
  UniqueCERTCertList unusedBuiltChain;
  const bool saveIntermediates = false;
  mozilla::pkix::Result rv = certVerifier->VerifySSLServerCert(
    cert,
    stapledOCSPResponse,
    sctsFromTLSExtension,
    mozilla::pkix::Now(),
    infoObject,
    infoObject->GetHostNameRaw(),
    unusedBuiltChain,
    saveIntermediates,
    flags,
    infoObject->GetOriginAttributes(),
    &evOidPolicy);

  RefPtr<nsNSSCertificate> nssc(nsNSSCertificate::Create(cert.get()));
  if (rv == Success && evOidPolicy != SEC_OID_UNKNOWN) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("HandshakeCallback using NEW cert %p (is EV)", nssc.get()));
    sslStatus->SetServerCert(nssc, EVStatus::EV);
  } else {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("HandshakeCallback using NEW cert %p (is not EV)", nssc.get()));
    sslStatus->SetServerCert(nssc, EVStatus::NotEV);
  }
}

void HandshakeCallback(PRFileDesc* fd, void* client_data) {
  nsNSSShutDownPreventionLock locker;
  SECStatus rv;

  nsNSSSocketInfo* infoObject = (nsNSSSocketInfo*) fd->higher->secret;

  // Do the bookkeeping that needs to be done after the
  // server's ServerHello...ServerHelloDone have been processed, but that doesn't
  // need the handshake to be completed.
  PreliminaryHandshakeDone(fd);

  nsSSLIOLayerHelpers& ioLayerHelpers
    = infoObject->SharedState().IOLayerHelpers();

  SSLVersionRange versions(infoObject->GetTLSVersionRange());

  MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
         ("[%p] HandshakeCallback: succeeded using TLS version range (0x%04x,0x%04x)\n",
          fd, static_cast<unsigned int>(versions.min),
              static_cast<unsigned int>(versions.max)));

  // If the handshake completed, then we know the site is TLS tolerant
  ioLayerHelpers.rememberTolerantAtVersion(infoObject->GetHostName(),
                                           infoObject->GetPort(),
                                           versions.max);

  SSLChannelInfo channelInfo;
  rv = SSL_GetChannelInfo(fd, &channelInfo, sizeof(channelInfo));
  MOZ_ASSERT(rv == SECSuccess);
  if (rv == SECSuccess) {
    SSLCipherSuiteInfo cipherInfo;
    rv = SSL_GetCipherSuiteInfo(channelInfo.cipherSuite, &cipherInfo,
                                sizeof cipherInfo);
    MOZ_ASSERT(rv == SECSuccess);
    if (rv == SECSuccess) {
      MOZ_ASSERT(infoObject->GetKEAUsed() == channelInfo.keaType);

      if (infoObject->IsFullHandshake()) {
        switch (channelInfo.keaType) {
          case ssl_kea_rsa:
            break;
          case ssl_kea_dh:
            break;
          case ssl_kea_ecdh:
            break;
          default:
            MOZ_CRASH("impossible KEA");
            break;
        }

        // RSA key exchange doesn't use a signature for auth.
        if (channelInfo.keaType != ssl_kea_rsa) {
          switch (channelInfo.authType) {
            case ssl_auth_rsa:
            case ssl_auth_rsa_sign:
              break;
            case ssl_auth_ecdsa:
              break;
            default:
              MOZ_CRASH("impossible auth algorithm");
              break;
          }
        }
      }
    }
  }

  PRBool siteSupportsSafeRenego;
  if (channelInfo.protocolVersion != SSL_LIBRARY_VERSION_TLS_1_3) {
    rv = SSL_HandshakeNegotiatedExtension(fd, ssl_renegotiation_info_xtn,
                                          &siteSupportsSafeRenego);
    MOZ_ASSERT(rv == SECSuccess);
    if (rv != SECSuccess) {
      siteSupportsSafeRenego = false;
    }
  } else {
    // TLS 1.3 dropped support for renegotiation.
    siteSupportsSafeRenego = true;
  }
  bool renegotiationUnsafe = !siteSupportsSafeRenego &&
                             ioLayerHelpers.treatUnsafeNegotiationAsBroken();


  /* Set the SSL Status information */
  RefPtr<nsSSLStatus> status(infoObject->SSLStatus());
  if (!status) {
    status = new nsSSLStatus();
    infoObject->SetSSLStatus(status);
  }

  RememberCertErrorsTable::GetInstance().LookupCertErrorBits(infoObject,
                                                             status);

  uint32_t state;
  if (renegotiationUnsafe) {
    state = nsIWebProgressListener::STATE_IS_BROKEN;
  } else {
    state = nsIWebProgressListener::STATE_IS_SECURE |
            nsIWebProgressListener::STATE_SECURE_HIGH;
  }

  if (status->HasServerCert()) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
           ("HandshakeCallback KEEPING existing cert\n"));
  } else {
    DetermineEVStatusAndSetNewCert(status, fd, infoObject);
  }

  bool domainMismatch;
  bool untrusted;
  bool notValidAtThisTime;
  // These all return NS_OK, so don't even bother checking the return values.
  Unused << status->GetIsDomainMismatch(&domainMismatch);
  Unused << status->GetIsUntrusted(&untrusted);
  Unused << status->GetIsNotValidAtThisTime(&notValidAtThisTime);
  // If we're here, the TLS handshake has succeeded. Thus if any of these
  // booleans are true, the user has added an override for a certificate error.
  if (domainMismatch || untrusted || notValidAtThisTime) {
    state |= nsIWebProgressListener::STATE_CERT_USER_OVERRIDDEN;
  }

  infoObject->SetSecurityState(state);

  // XXX Bug 883674: We shouldn't be formatting messages here in PSM; instead,
  // we should set a flag on the channel that higher (UI) level code can check
  // to log the warning. In particular, these warnings should go to the web
  // console instead of to the error console. Also, the warning is not
  // localized.
  if (!siteSupportsSafeRenego) {
    nsXPIDLCString hostName;
    infoObject->GetHostName(getter_Copies(hostName));

    nsAutoString msg;
    msg.Append(NS_ConvertASCIItoUTF16(hostName));
    msg.AppendLiteral(" : server does not support RFC 5746, see CVE-2009-3555");

    nsContentUtils::LogSimpleConsoleError(msg, "SSL");
  }

  infoObject->NoteTimeUntilReady();
  infoObject->SetHandshakeCompleted();
}
