#include "nsContentSecurityManager.h"
#include "nsEscape.h"
#include "nsIChannel.h"
#include "nsIHttpChannelInternal.h"
#include "nsIStreamListener.h"
#include "nsILoadInfo.h"
#include "nsIOService.h"
#include "nsIProtocolHandler.h"
#include "nsContentUtils.h"
#include "nsCORSListenerProxy.h"
#include "nsIStreamListener.h"
#include "nsIScriptError.h"
#include "nsCDefaultURIFixup.h"
#include "nsIURIFixup.h"
#include "nsIImageLoadingContent.h"
#include "nsNetUtil.h"
#include "mozilla/ArrayUtils.h"
#include "nsString.h"
#include "nsMimeTypes.h"
#include "nsContentPolicyUtils.h"
#include "nsCharSeparatedTokenizer.h"

#include "mozilla/dom/Element.h"
#include "mozilla/dom/TabChild.h"

using namespace mozilla;

NS_IMPL_ISUPPORTS(nsContentSecurityManager,
                  nsIContentSecurityManager,
                  nsIChannelEventSink)

/* static */ bool
nsContentSecurityManager::AllowTopLevelNavigationToDataURI(nsIChannel* aChannel)
{
  // Let's block all toplevel document navigations to a data: URI.
  // In all cases where the toplevel document is navigated to a
  // data: URI the triggeringPrincipal is a codeBasePrincipal, or
  // a NullPrincipal. In other cases, e.g. typing a data: URL into
  // the URL-Bar, the triggeringPrincipal is a SystemPrincipal;
  // we don't want to block those loads. Only exception, loads coming
  // from an external applicaton (e.g. Thunderbird) don't load
  // using a codeBasePrincipal, but we want to block those loads.
  if (!mozilla::net::nsIOService::BlockToplevelDataUriNavigations()) {
    return true;
  }
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
  if (!loadInfo) {
    return true;
  }
  if (loadInfo->GetExternalContentPolicyType() != nsIContentPolicy::TYPE_DOCUMENT) {
    return true;
  }
  if (loadInfo->GetForceAllowDataURI()) {
    // if the loadinfo explicitly allows the data URI navigation, let's allow it now
    return true;
  }
  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_GetFinalChannelURI(aChannel, getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, true);
  bool isDataURI =
    (NS_SUCCEEDED(uri->SchemeIs("data", &isDataURI)) && isDataURI);
  if (!isDataURI) {
    return true;
  }
  // Whitelist data: images as long as they are not SVGs
  nsAutoCString filePath;
  uri->GetFilePath(filePath);
  if (StringBeginsWith(filePath, NS_LITERAL_CSTRING("image/")) &&
      !StringBeginsWith(filePath, NS_LITERAL_CSTRING("image/svg+xml"))) {
    return true;
  }
  // Whitelist data: PDFs and JSON
  if (StringBeginsWith(filePath, NS_LITERAL_CSTRING("application/pdf")) ||
      StringBeginsWith(filePath, NS_LITERAL_CSTRING("application/json"))) {
    return true;
  }
  // Redirecting to a toplevel data: URI is not allowed, hence we make
  // sure the RedirectChain is empty.
  if (!loadInfo->GetLoadTriggeredFromExternal() &&
      nsContentUtils::IsSystemPrincipal(loadInfo->TriggeringPrincipal()) &&
      loadInfo->RedirectChain().IsEmpty()) {
    return true;
  }
  nsAutoCString dataSpec;
  uri->GetSpec(dataSpec);
  if (dataSpec.Length() > 50) {
    dataSpec.Truncate(50);
    dataSpec.AppendLiteral("...");
  }
  nsCOMPtr<nsITabChild> tabChild = do_QueryInterface(loadInfo->ContextForTopLevelLoad());
  nsCOMPtr<nsIDocument> doc;
  if (tabChild) {
    doc = static_cast<mozilla::dom::TabChild*>(tabChild.get())->GetDocument();
  }
  NS_ConvertUTF8toUTF16 specUTF16(NS_UnescapeURL(dataSpec));
  const char16_t* params[] = { specUTF16.get() };
  nsContentUtils::ReportToConsole(nsIScriptError::warningFlag,
                                  NS_LITERAL_CSTRING("DATA_URI_BLOCKED"),
                                  doc,
                                  nsContentUtils::eSECURITY_PROPERTIES,
                                  "BlockTopLevelDataURINavigation",
                                  params, ArrayLength(params));
  return false;
}

/* static */ nsresult
nsContentSecurityManager::CheckFTPSubresourceLoad(nsIChannel* aChannel)
{
  // We dissallow using FTP resources as a subresource almost everywhere.
  // The only valid way to use FTP resources is loading it as
  // a top level document.
  
  // Override blocking if the pref is set to allow.
  if (!mozilla::net::nsIOService::BlockFTPSubresources()) {
    return NS_OK;
  }

  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
  if (!loadInfo) {
    return NS_OK;
  }

  nsContentPolicyType type = loadInfo->GetExternalContentPolicyType();

  // Allow save-as download of FTP files on HTTP pages.
  if (type == nsIContentPolicy::TYPE_SAVEAS_DOWNLOAD) {
    return NS_OK;
  }

  // Allow direct document requests
  if (type == nsIContentPolicy::TYPE_DOCUMENT) {
    return NS_OK;
  }

  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_GetFinalChannelURI(aChannel, getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, rv);
  if (!uri) {
    return NS_OK;
  }

  // Allow if it's not the FTP protocol
  bool isFtpURI = (NS_SUCCEEDED(uri->SchemeIs("ftp", &isFtpURI)) && isFtpURI);
  if (!isFtpURI) {
    return NS_OK;
  }

  // Allow loading FTP subresources in top-level FTP documents.
  nsIPrincipal* triggeringPrincipal = loadInfo->TriggeringPrincipal();
  nsCOMPtr<nsIURI> tURI;
  triggeringPrincipal->GetURI(getter_AddRefs(tURI));
  if (!tURI) {
    // We don't have a triggering principal URI, meaning this isn't actually
    // a subresource, but rather a top-level document, i.e. something we can
    // display in-browser and might be saving as-is. Allow the load.
    return NS_OK;
  }
  bool isTrigFtpURI = (NS_SUCCEEDED(tURI->SchemeIs("ftp", &isTrigFtpURI)) && isTrigFtpURI);
  if (isTrigFtpURI) {
    // The document loading this resource is also on FTP, satisfying the SOP.
    // Allow the load.
    return NS_OK;
  }

  // If we get here, the request is blocked and should be reported.
  nsCOMPtr<nsIDocument> doc;
  if (nsINode* node = loadInfo->LoadingNode()) {
    doc = node->OwnerDoc();
  }

  nsAutoCString spec;
  uri->GetSpec(spec);
  NS_ConvertUTF8toUTF16 specUTF16(NS_UnescapeURL(spec));
  const char16_t* params[] = { specUTF16.get() };

  nsContentUtils::ReportToConsole(nsIScriptError::warningFlag,
                                  NS_LITERAL_CSTRING("FTP_URI_BLOCKED"),
                                  doc,
                                  nsContentUtils::eSECURITY_PROPERTIES,
                                  "BlockSubresourceFTP",
                                  params, ArrayLength(params));

  return NS_ERROR_CONTENT_BLOCKED;
}

static nsresult
ValidateSecurityFlags(nsILoadInfo* aLoadInfo)
{
  nsSecurityFlags securityMode = aLoadInfo->GetSecurityMode();

  if (securityMode != nsILoadInfo::SEC_REQUIRE_SAME_ORIGIN_DATA_INHERITS &&
      securityMode != nsILoadInfo::SEC_REQUIRE_SAME_ORIGIN_DATA_IS_BLOCKED &&
      securityMode != nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_INHERITS &&
      securityMode != nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL &&
      securityMode != nsILoadInfo::SEC_REQUIRE_CORS_DATA_INHERITS) {
    MOZ_ASSERT(false, "need one securityflag from nsILoadInfo to perform security checks");
    return NS_ERROR_FAILURE;
  }

  // all good, found the right security flags
  return NS_OK;
}

static bool SchemeIs(nsIURI* aURI, const char* aScheme)
{
  nsCOMPtr<nsIURI> baseURI = NS_GetInnermostURI(aURI);
  NS_ENSURE_TRUE(baseURI, false);

  bool isScheme = false;
  return NS_SUCCEEDED(baseURI->SchemeIs(aScheme, &isScheme)) && isScheme;
}


static bool IsImageLoadInEditorAppType(nsILoadInfo* aLoadInfo)
{
  // Editor apps get special treatment here, editors can load images
  // from anywhere.  This allows editor to insert images from file://
  // into documents that are being edited.
  nsContentPolicyType type = aLoadInfo->InternalContentPolicyType();
  if (type != nsIContentPolicy::TYPE_INTERNAL_IMAGE  &&
      type != nsIContentPolicy::TYPE_INTERNAL_IMAGE_PRELOAD &&
      type != nsIContentPolicy::TYPE_INTERNAL_IMAGE_FAVICON &&
      type != nsIContentPolicy::TYPE_IMAGESET) {
    return false;
  }

  uint32_t appType = nsIDocShell::APP_TYPE_UNKNOWN;
  nsINode* node = aLoadInfo->LoadingNode();
  if (!node) {
    return false;
  }
  nsIDocument* doc = node->OwnerDoc();
  if (!doc) {
    return false;
  }

  nsCOMPtr<nsIDocShellTreeItem> docShellTreeItem = doc->GetDocShell();
  if (!docShellTreeItem) {
    return false;
  }

  nsCOMPtr<nsIDocShellTreeItem> root;
  docShellTreeItem->GetRootTreeItem(getter_AddRefs(root));
  nsCOMPtr<nsIDocShell> docShell(do_QueryInterface(root));
  if (!docShell || NS_FAILED(docShell->GetAppType(&appType))) {
    appType = nsIDocShell::APP_TYPE_UNKNOWN;
  }

  return appType == nsIDocShell::APP_TYPE_EDITOR;
}

static nsresult
DoCheckLoadURIChecks(nsIURI* aURI, nsILoadInfo* aLoadInfo)
{
  // Bug 1228117: determine the correct security policy for DTD loads
  if (aLoadInfo->GetExternalContentPolicyType() == nsIContentPolicy::TYPE_DTD) {
    return NS_OK;
  }

  if (IsImageLoadInEditorAppType(aLoadInfo)) {
    return NS_OK;
  }

  uint32_t flags = nsIScriptSecurityManager::STANDARD;
  if (aLoadInfo->GetAllowChrome()) {
    flags |= nsIScriptSecurityManager::ALLOW_CHROME;
  }
  if (aLoadInfo->GetDisallowScript()) {
    flags |= nsIScriptSecurityManager::DISALLOW_SCRIPT;
  }

  // Only call CheckLoadURIWithPrincipal() using the TriggeringPrincipal and not
  // the LoadingPrincipal when SEC_ALLOW_CROSS_ORIGIN_* security flags are set,
  // to allow, e.g. user stylesheets to load chrome:// URIs.
  return nsContentUtils::GetSecurityManager()->
           CheckLoadURIWithPrincipal(aLoadInfo->TriggeringPrincipal(),
                                     aURI,
                                     flags);
}

static bool
URIHasFlags(nsIURI* aURI, uint32_t aURIFlags)
{
  bool hasFlags;
  nsresult rv = NS_URIChainHasFlags(aURI, aURIFlags, &hasFlags);
  NS_ENSURE_SUCCESS(rv, false);

  return hasFlags;
}

static nsresult
DoSOPChecks(nsIURI* aURI, nsILoadInfo* aLoadInfo, nsIChannel* aChannel)
{
  if (aLoadInfo->GetAllowChrome() &&
      (URIHasFlags(aURI, nsIProtocolHandler::URI_IS_UI_RESOURCE) ||
       SchemeIs(aURI, "moz-safe-about"))) {
    // UI resources are allowed.
    return DoCheckLoadURIChecks(aURI, aLoadInfo);
  }

  NS_ENSURE_FALSE(NS_HasBeenCrossOrigin(aChannel, true),
                  NS_ERROR_DOM_BAD_URI);

  return NS_OK;
}

static nsresult
DoCORSChecks(nsIChannel* aChannel, nsILoadInfo* aLoadInfo,
             nsCOMPtr<nsIStreamListener>& aInAndOutListener)
{
  MOZ_RELEASE_ASSERT(aInAndOutListener, "can not perform CORS checks without a listener");

  // No need to set up CORS if TriggeringPrincipal is the SystemPrincipal.
  // For example, allow user stylesheets to load XBL from external files
  // without requiring CORS.
  if (nsContentUtils::IsSystemPrincipal(aLoadInfo->TriggeringPrincipal())) {
    return NS_OK;
  }

  nsIPrincipal* loadingPrincipal = aLoadInfo->LoadingPrincipal();
  RefPtr<nsCORSListenerProxy> corsListener =
    new nsCORSListenerProxy(aInAndOutListener,
                            loadingPrincipal,
                            aLoadInfo->GetCookiePolicy() ==
                              nsILoadInfo::SEC_COOKIES_INCLUDE);
  // XXX: @arg: DataURIHandling::Allow
  // lets use  DataURIHandling::Allow for now and then decide on callsite basis. see also:
  // http://mxr.mozilla.org/mozilla-central/source/dom/security/nsCORSListenerProxy.h#33
  nsresult rv = corsListener->Init(aChannel, DataURIHandling::Allow);
  NS_ENSURE_SUCCESS(rv, rv);
  aInAndOutListener = corsListener;
  return NS_OK;
}

static nsresult
DoContentSecurityChecks(nsIChannel* aChannel, nsILoadInfo* aLoadInfo)
{
  nsContentPolicyType contentPolicyType =
    aLoadInfo->GetExternalContentPolicyType();
  nsContentPolicyType internalContentPolicyType =
    aLoadInfo->InternalContentPolicyType();
  nsCString mimeTypeGuess;
  nsCOMPtr<nsISupports> requestingContext = nullptr;

  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_GetFinalChannelURI(aChannel, getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, rv);

  if (contentPolicyType == nsIContentPolicy::TYPE_DOCUMENT ||
      contentPolicyType == nsIContentPolicy::TYPE_SUBDOCUMENT) {
    // TYPE_DOCUMENT and TYPE_SUBDOCUMENT loads might potentially
    // be wyciwyg:// channels. Let's fix up the URI so we can
    // perform proper security checks.
    nsCOMPtr<nsIURIFixup> urifixup(do_GetService(NS_URIFIXUP_CONTRACTID, &rv));
    if (NS_SUCCEEDED(rv) && urifixup) {
      nsCOMPtr<nsIURI> fixedURI;
      rv = urifixup->CreateExposableURI(uri, getter_AddRefs(fixedURI));
      if (NS_SUCCEEDED(rv)) {
        uri = fixedURI;
      }
    }
  }

  switch(contentPolicyType) {
    case nsIContentPolicy::TYPE_OTHER: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_SCRIPT: {
      mimeTypeGuess = NS_LITERAL_CSTRING("application/javascript");
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_IMAGE: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_STYLESHEET: {
      mimeTypeGuess = NS_LITERAL_CSTRING("text/css");
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_OBJECT: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_DOCUMENT: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->ContextForTopLevelLoad();
      break;
    }

    case nsIContentPolicy::TYPE_SUBDOCUMENT: {
      mimeTypeGuess = NS_LITERAL_CSTRING("text/html");
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_REFRESH: {
      MOZ_ASSERT(false, "contentPolicyType not supported yet");
      break;
    }

    case nsIContentPolicy::TYPE_XBL: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_PING: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_XMLHTTPREQUEST: {
      // alias nsIContentPolicy::TYPE_DATAREQUEST:
      requestingContext = aLoadInfo->LoadingNode();
#ifdef DEBUG
      {
        nsCOMPtr<nsINode> node = do_QueryInterface(requestingContext);
        MOZ_ASSERT(!node || node->NodeType() == nsIDOMNode::DOCUMENT_NODE,
                   "type_xml requires requestingContext of type Document");
      }
#endif
      // We're checking for the external TYPE_XMLHTTPREQUEST here in case
      // an addon creates a request with that type.
      if (internalContentPolicyType ==
            nsIContentPolicy::TYPE_INTERNAL_XMLHTTPREQUEST ||
          internalContentPolicyType ==
            nsIContentPolicy::TYPE_XMLHTTPREQUEST) {
        mimeTypeGuess = EmptyCString();
      }
      else {
        MOZ_ASSERT(internalContentPolicyType ==
                   nsIContentPolicy::TYPE_INTERNAL_EVENTSOURCE,
                   "can not set mime type guess for unexpected internal type");
        mimeTypeGuess = NS_LITERAL_CSTRING(TEXT_EVENT_STREAM);
      }
      break;
    }

    case nsIContentPolicy::TYPE_OBJECT_SUBREQUEST: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
#ifdef DEBUG
      {
        nsCOMPtr<nsINode> node = do_QueryInterface(requestingContext);
        MOZ_ASSERT(!node || node->NodeType() == nsIDOMNode::ELEMENT_NODE,
                   "type_subrequest requires requestingContext of type Element");
      }
#endif
      break;
    }

    case nsIContentPolicy::TYPE_DTD: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
#ifdef DEBUG
      {
        nsCOMPtr<nsINode> node = do_QueryInterface(requestingContext);
        MOZ_ASSERT(!node || node->NodeType() == nsIDOMNode::DOCUMENT_NODE,
                   "type_dtd requires requestingContext of type Document");
      }
#endif
      break;
    }

    case nsIContentPolicy::TYPE_FONT: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_MEDIA: {
      if (internalContentPolicyType == nsIContentPolicy::TYPE_INTERNAL_TRACK) {
        mimeTypeGuess = NS_LITERAL_CSTRING("text/vtt");
      }
      else {
        mimeTypeGuess = EmptyCString();
      }
      requestingContext = aLoadInfo->LoadingNode();
#ifdef DEBUG
      {
        nsCOMPtr<nsINode> node = do_QueryInterface(requestingContext);
        MOZ_ASSERT(!node || node->NodeType() == nsIDOMNode::ELEMENT_NODE,
                   "type_media requires requestingContext of type Element");
      }
#endif
      break;
    }

    case nsIContentPolicy::TYPE_WEBSOCKET: {
      // Websockets have to use the proxied URI:
      // ws:// instead of http:// for CSP checks
      nsCOMPtr<nsIHttpChannelInternal> httpChannelInternal
        = do_QueryInterface(aChannel);
      MOZ_ASSERT(httpChannelInternal);
      if (httpChannelInternal) {
        httpChannelInternal->GetProxyURI(getter_AddRefs(uri));
      }
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_CSP_REPORT: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_XSLT: {
      mimeTypeGuess = NS_LITERAL_CSTRING("application/xml");
      requestingContext = aLoadInfo->LoadingNode();
#ifdef DEBUG
      {
        nsCOMPtr<nsINode> node = do_QueryInterface(requestingContext);
        MOZ_ASSERT(!node || node->NodeType() == nsIDOMNode::DOCUMENT_NODE,
                   "type_xslt requires requestingContext of type Document");
      }
#endif
      break;
    }

    case nsIContentPolicy::TYPE_BEACON: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
#ifdef DEBUG
      {
        nsCOMPtr<nsINode> node = do_QueryInterface(requestingContext);
        MOZ_ASSERT(!node || node->NodeType() == nsIDOMNode::DOCUMENT_NODE,
                   "type_beacon requires requestingContext of type Document");
      }
#endif
      break;
    }

    case nsIContentPolicy::TYPE_FETCH: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_IMAGESET: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_WEB_MANIFEST: {
      mimeTypeGuess = NS_LITERAL_CSTRING("application/manifest+json");
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    case nsIContentPolicy::TYPE_SAVEAS_DOWNLOAD: {
      mimeTypeGuess = EmptyCString();
      requestingContext = aLoadInfo->LoadingNode();
      break;
    }

    default:
      // nsIContentPolicy::TYPE_INVALID
      MOZ_ASSERT(false, "can not perform security check without a valid contentType");
  }

  int16_t shouldLoad = nsIContentPolicy::ACCEPT;
  rv = NS_CheckContentLoadPolicy(internalContentPolicyType,
                                 uri,
                                 aLoadInfo->LoadingPrincipal(),
                                 aLoadInfo->TriggeringPrincipal(),
                                 requestingContext,
                                 mimeTypeGuess,
                                 nullptr,        //extra,
                                 &shouldLoad,
                                 nsContentUtils::GetContentPolicy(),
                                 nsContentUtils::GetSecurityManager());

  if (NS_FAILED(rv) || NS_CP_REJECTED(shouldLoad)) {
    if ((NS_SUCCEEDED(rv) && shouldLoad == nsIContentPolicy::REJECT_TYPE) &&
        (contentPolicyType == nsIContentPolicy::TYPE_DOCUMENT ||
         contentPolicyType == nsIContentPolicy::TYPE_SUBDOCUMENT)) {
      // for docshell loads we might have to return SHOW_ALT.
      return NS_ERROR_CONTENT_BLOCKED_SHOW_ALT;
    }
    return NS_ERROR_CONTENT_BLOCKED;
  }

  return NS_OK;
}

/*
 * Based on the security flags provided in the loadInfo of the channel,
 * doContentSecurityCheck() performs the following content security checks
 * before opening the channel:
 *
 * (1) Same Origin Policy Check (if applicable)
 * (2) Allow Cross Origin but perform sanity checks whether a principal
 *     is allowed to access the following URL.
 * (3) Perform CORS check (if applicable)
 * (4) ContentPolicy checks (Content-Security-Policy, Mixed Content, ...)
 *
 * @param aChannel
 *    The channel to perform the security checks on.
 * @param aInAndOutListener
 *    The streamListener that is passed to channel->AsyncOpen2() that is now potentially
 *    wrappend within nsCORSListenerProxy() and becomes the corsListener that now needs
 *    to be set as new streamListener on the channel.
 */
nsresult
nsContentSecurityManager::doContentSecurityCheck(nsIChannel* aChannel,
                                                 nsCOMPtr<nsIStreamListener>& aInAndOutListener)
{
  NS_ENSURE_ARG(aChannel);
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();

  if (!loadInfo) {
    MOZ_ASSERT(false, "channel needs to have loadInfo to perform security checks");
    return NS_ERROR_UNEXPECTED;
  }

  // if dealing with a redirected channel then we have already installed
  // streamlistener and redirect proxies and so we are done.
  if (loadInfo->GetInitialSecurityCheckDone()) {
    return NS_OK;
  }

  // make sure that only one of the five security flags is set in the loadinfo
  // e.g. do not require same origin and allow cross origin at the same time
  nsresult rv = ValidateSecurityFlags(loadInfo);
  NS_ENSURE_SUCCESS(rv, rv);

  // since aChannel was openend using asyncOpen2() we have to make sure
  // that redirects of that channel also get openend using asyncOpen2()
  // please note that some implementations of ::AsyncOpen2 might already
  // have set that flag to true (e.g. nsViewSourceChannel) in which case
  // we just set the flag again.
  loadInfo->SetEnforceSecurity(true);

  if (loadInfo->GetSecurityMode() == nsILoadInfo::SEC_REQUIRE_CORS_DATA_INHERITS) {
    rv = DoCORSChecks(aChannel, loadInfo, aInAndOutListener);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  rv = CheckChannel(aChannel);
  NS_ENSURE_SUCCESS(rv, rv);

  // Perform all ContentPolicy checks (MixedContent, CSP, ...)
  rv = DoContentSecurityChecks(aChannel, loadInfo);
  NS_ENSURE_SUCCESS(rv, rv);

  // Apply this after CSP checks to allow CSP reporting.
  rv = CheckFTPSubresourceLoad(aChannel);
  NS_ENSURE_SUCCESS(rv, rv);

  // now lets set the initalSecurityFlag for subsequent calls
  loadInfo->SetInitialSecurityCheckDone(true);

  // all security checks passed - lets allow the load
  return NS_OK;
}

NS_IMETHODIMP
nsContentSecurityManager::AsyncOnChannelRedirect(nsIChannel* aOldChannel,
                                                 nsIChannel* aNewChannel,
                                                 uint32_t aRedirFlags,
                                                 nsIAsyncVerifyRedirectCallback *aCb)
{
  nsCOMPtr<nsILoadInfo> loadInfo = aOldChannel->GetLoadInfo();
  // Are we enforcing security using LoadInfo?
  if (loadInfo && loadInfo->GetEnforceSecurity()) {
    nsresult rv = CheckChannel(aNewChannel);
    if (NS_SUCCEEDED(rv)) {
      rv = CheckFTPSubresourceLoad(aNewChannel);
    }
    if (NS_FAILED(rv)) {
      aOldChannel->Cancel(rv);
      return rv;
    }
  }

  // Also verify that the redirecting server is allowed to redirect to the
  // given URI
  nsCOMPtr<nsIPrincipal> oldPrincipal;
  nsContentUtils::GetSecurityManager()->
    GetChannelResultPrincipal(aOldChannel, getter_AddRefs(oldPrincipal));

  nsCOMPtr<nsIURI> newURI;
  aNewChannel->GetURI(getter_AddRefs(newURI));
  nsCOMPtr<nsIURI> newOriginalURI;
  aNewChannel->GetOriginalURI(getter_AddRefs(newOriginalURI));

  NS_ENSURE_STATE(oldPrincipal && newURI && newOriginalURI);

  const uint32_t flags =
      nsIScriptSecurityManager::LOAD_IS_AUTOMATIC_DOCUMENT_REPLACEMENT |
      nsIScriptSecurityManager::DISALLOW_SCRIPT;
  nsresult rv = nsContentUtils::GetSecurityManager()->
    CheckLoadURIWithPrincipal(oldPrincipal, newURI, flags);
  if (NS_SUCCEEDED(rv) && newOriginalURI != newURI) {
      rv = nsContentUtils::GetSecurityManager()->
        CheckLoadURIWithPrincipal(oldPrincipal, newOriginalURI, flags);
  }
  NS_ENSURE_SUCCESS(rv, rv);

  aCb->OnRedirectVerifyCallback(NS_OK);
  return NS_OK;
}

static void
AddLoadFlags(nsIRequest *aRequest, nsLoadFlags aNewFlags)
{
  nsLoadFlags flags;
  aRequest->GetLoadFlags(&flags);
  flags |= aNewFlags;
  aRequest->SetLoadFlags(flags);
}

/*
 * Check that this channel passes all security checks. Returns an error code
 * if this requesst should not be permitted.
 */
nsresult
nsContentSecurityManager::CheckChannel(nsIChannel* aChannel)
{
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
  MOZ_ASSERT(loadInfo);

  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_GetFinalChannelURI(aChannel, getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, rv);

  nsContentPolicyType contentPolicyType =
    loadInfo->GetExternalContentPolicyType();

  if (contentPolicyType == nsIContentPolicy::TYPE_DOCUMENT ||
      contentPolicyType == nsIContentPolicy::TYPE_SUBDOCUMENT) {
    // TYPE_DOCUMENT and TYPE_SUBDOCUMENT loads might potentially
    // be wyciwyg:// channels. Let's fix up the URI so we can
    // perform proper security checks.
    nsCOMPtr<nsIURIFixup> urifixup(do_GetService(NS_URIFIXUP_CONTRACTID, &rv));
    if (NS_SUCCEEDED(rv) && urifixup) {
      nsCOMPtr<nsIURI> fixedURI;
      rv = urifixup->CreateExposableURI(uri, getter_AddRefs(fixedURI));
      if (NS_SUCCEEDED(rv)) {
        uri = fixedURI;
      }
    }
  }

  // Handle cookie policies
  uint32_t cookiePolicy = loadInfo->GetCookiePolicy();
  if (cookiePolicy == nsILoadInfo::SEC_COOKIES_SAME_ORIGIN) {

    // We shouldn't have the SEC_COOKIES_SAME_ORIGIN flag for top level loads
    MOZ_ASSERT(loadInfo->GetExternalContentPolicyType() !=
               nsIContentPolicy::TYPE_DOCUMENT);
    nsIPrincipal* loadingPrincipal = loadInfo->LoadingPrincipal();

    // It doesn't matter what we pass for the third, data-inherits, argument.
    // Any protocol which inherits won't pay attention to cookies anyway.
    rv = loadingPrincipal->CheckMayLoad(uri, false, false);
    if (NS_FAILED(rv)) {
      AddLoadFlags(aChannel, nsIRequest::LOAD_ANONYMOUS);
    }
  }
  else if (cookiePolicy == nsILoadInfo::SEC_COOKIES_OMIT) {
    AddLoadFlags(aChannel, nsIRequest::LOAD_ANONYMOUS);
  }

  nsSecurityFlags securityMode = loadInfo->GetSecurityMode();

  // CORS mode is handled by nsCORSListenerProxy
  if (securityMode == nsILoadInfo::SEC_REQUIRE_CORS_DATA_INHERITS) {
    if (NS_HasBeenCrossOrigin(aChannel)) {
      loadInfo->MaybeIncreaseTainting(LoadTainting::CORS);
    }
    return NS_OK;
  }

  // Allow subresource loads if TriggeringPrincipal is the SystemPrincipal.
  // For example, allow user stylesheets to load XBL from external files.
  if (nsContentUtils::IsSystemPrincipal(loadInfo->TriggeringPrincipal()) &&
      loadInfo->GetExternalContentPolicyType() != nsIContentPolicy::TYPE_DOCUMENT &&
      loadInfo->GetExternalContentPolicyType() != nsIContentPolicy::TYPE_SUBDOCUMENT) {
    return NS_OK;
  }

  // if none of the REQUIRE_SAME_ORIGIN flags are set, then SOP does not apply
  if ((securityMode == nsILoadInfo::SEC_REQUIRE_SAME_ORIGIN_DATA_INHERITS) ||
      (securityMode == nsILoadInfo::SEC_REQUIRE_SAME_ORIGIN_DATA_IS_BLOCKED)) {
    rv = DoSOPChecks(uri, loadInfo, aChannel);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if ((securityMode == nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_INHERITS) ||
      (securityMode == nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL)) {
    if (NS_HasBeenCrossOrigin(aChannel)) {
      loadInfo->MaybeIncreaseTainting(LoadTainting::Opaque);
    }
    // Please note that DoCheckLoadURIChecks should only be enforced for
    // cross origin requests. If the flag SEC_REQUIRE_CORS_DATA_INHERITS is set
    // within the loadInfo, then then CheckLoadURIWithPrincipal is performed
    // within nsCorsListenerProxy
    rv = DoCheckLoadURIChecks(uri, loadInfo);
    NS_ENSURE_SUCCESS(rv, rv);
    // TODO: Bug 1371237
    // consider calling SetBlockedRequest in nsContentSecurityManager::CheckChannel
  }

  return NS_OK;
}

// ==== nsIContentSecurityManager implementation =====

NS_IMETHODIMP
nsContentSecurityManager::PerformSecurityCheck(nsIChannel* aChannel,
                                               nsIStreamListener* aStreamListener,
                                               nsIStreamListener** outStreamListener)
{
  nsCOMPtr<nsIStreamListener> inAndOutListener = aStreamListener;
  nsresult rv = doContentSecurityCheck(aChannel, inAndOutListener);
  NS_ENSURE_SUCCESS(rv, rv);

  inAndOutListener.forget(outStreamListener);
  return NS_OK;
}

NS_IMETHODIMP
nsContentSecurityManager::IsOriginPotentiallyTrustworthy(nsIPrincipal* aPrincipal,
                                                         bool* aIsTrustWorthy)
{
  MOZ_ASSERT(NS_IsMainThread());
  NS_ENSURE_ARG_POINTER(aPrincipal);
  NS_ENSURE_ARG_POINTER(aIsTrustWorthy);

  if (aPrincipal->GetIsSystemPrincipal()) {
    *aIsTrustWorthy = true;
    return NS_OK;
  }

  // The following implements:
  // https://w3c.github.io/webappsec-secure-contexts/#is-origin-trustworthy

  *aIsTrustWorthy = false;

  if (aPrincipal->GetIsNullPrincipal()) {
    return NS_OK;
  }

  MOZ_ASSERT(aPrincipal->GetIsCodebasePrincipal(),
             "Nobody is expected to call us with an nsIExpandedPrincipal");

  nsCOMPtr<nsIURI> uri;
  aPrincipal->GetURI(getter_AddRefs(uri));

  nsAutoCString scheme;
  nsresult rv = uri->GetScheme(scheme);
  if (NS_FAILED(rv)) {
    return NS_OK;
  }

  // Blobs are expected to inherit their principal so we don't expect to have
  // a codebase principal with scheme 'blob' here.  We can't assert that though
  // since someone could mess with a non-blob URI to give it that scheme.
  NS_WARNING_ASSERTION(!scheme.EqualsLiteral("blob"),
                       "IsOriginPotentiallyTrustworthy ignoring blob scheme");

  // According to the specification, the user agent may choose to extend the
  // trust to other, vendor-specific URL schemes. We use this for "resource:",
  // which is technically a substituting protocol handler that is not limited to
  // local resource mapping, but in practice is never mapped remotely as this
  // would violate assumptions a lot of code makes.
  if (scheme.EqualsLiteral("https") ||
      scheme.EqualsLiteral("file") ||
      scheme.EqualsLiteral("resource") ||
      scheme.EqualsLiteral("app") ||
      scheme.EqualsLiteral("moz-extension") ||
      scheme.EqualsLiteral("wss")) {
    *aIsTrustWorthy = true;
    return NS_OK;
  }

  nsAutoCString host;
  rv = uri->GetHost(host);
  if (NS_FAILED(rv)) {
    return NS_OK;
  }

  if (host.Equals("127.0.0.1") ||
      host.Equals("localhost") ||
      host.Equals("::1")) {
    *aIsTrustWorthy = true;
    return NS_OK;
  }

  // If a host is not considered secure according to the default algorithm, then
  // check to see if it has been whitelisted by the user.  We only apply this
  // whitelist for network resources, i.e., those with scheme "http" or "ws".
  // The pref should contain a comma-separated list of hostnames.
  if (scheme.EqualsLiteral("http") || scheme.EqualsLiteral("ws")) {
    nsAdoptingCString whitelist = Preferences::GetCString("dom.securecontext.whitelist");
    if (whitelist) {
      nsCCharSeparatedTokenizer tokenizer(whitelist, ',');
      while (tokenizer.hasMoreTokens()) {
        const nsCSubstring& allowedHost = tokenizer.nextToken();
        if (host.Equals(allowedHost)) {
          *aIsTrustWorthy = true;
          return NS_OK;
        }
      }
    }
  }

  return NS_OK;
}
