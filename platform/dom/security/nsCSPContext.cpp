/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <string>
#include <unordered_set>

#include "nsCOMPtr.h"
#include "nsContentPolicyUtils.h"
#include "nsContentUtils.h"
#include "nsCSPContext.h"
#include "nsCSPParser.h"
#include "nsCSPService.h"
#include "nsError.h"
#include "nsIAsyncVerifyRedirectCallback.h"
#include "nsIClassInfoImpl.h"
#include "nsIDocShell.h"
#include "nsIDocShellTreeItem.h"
#include "nsIDOMHTMLDocument.h"
#include "nsIDOMHTMLElement.h"
#include "nsIDOMNode.h"
#include "nsIHttpChannel.h"
#include "nsIInterfaceRequestor.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsIObjectInputStream.h"
#include "nsIObjectOutputStream.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsIStringStream.h"
#include "nsIUploadChannel.h"
#include "nsIScriptError.h"
#include "nsIWebNavigation.h"
#include "nsMimeTypes.h"
#include "nsNetUtil.h"
#include "nsIContentPolicy.h"
#include "nsSupportsPrimitives.h"
#include "nsThreadUtils.h"
#include "nsString.h"
#include "nsScriptSecurityManager.h"
#include "nsStringStream.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/CSPReportBinding.h"
#include "mozilla/dom/CSPDictionariesBinding.h"
#include "mozilla/net/ReferrerPolicy.h"
#include "nsINetworkInterceptController.h"
#include "nsSandboxFlags.h"
#include "nsIScriptElement.h"

using namespace mozilla;

static LogModule*
GetCspContextLog()
{
  static LazyLogModule gCspContextPRLog("CSPContext");
  return gCspContextPRLog;
}

#define CSPCONTEXTLOG(args) MOZ_LOG(GetCspContextLog(), mozilla::LogLevel::Debug, args)
#define CSPCONTEXTLOGENABLED() MOZ_LOG_TEST(GetCspContextLog(), mozilla::LogLevel::Debug)

static const uint32_t CSP_CACHE_URI_CUTOFF_SIZE = 512;

#ifdef DEBUG
/**
 * This function is only used for verification purposes within
 * GatherSecurityPolicyViolationEventData.
 */
static bool
ValidateDirectiveName(const nsAString& aDirective)
{
  static const auto directives = [] () {
    std::unordered_set<std::string> directives;
    constexpr size_t dirLen = sizeof(CSPStrDirectives) / sizeof(CSPStrDirectives[0]);
    for (size_t i = 0; i < dirLen; ++i) {
      directives.insert(CSPStrDirectives[i]);
    }
    return directives;
  } ();

  nsAutoString directive(aDirective);
  auto itr = directives.find(NS_ConvertUTF16toUTF8(directive).get());
  return itr != directives.end();
}
#endif // DEBUG

/**
 * Creates a key for use in the ShouldLoad cache.
 * Looks like: <uri>!<nsIContentPolicy::LOAD_TYPE>
 */
nsresult
CreateCacheKey_Internal(nsIURI* aContentLocation,
                        nsContentPolicyType aContentType,
                        nsACString& outCacheKey)
{
  if (!aContentLocation) {
    return NS_ERROR_FAILURE;
  }

  bool isDataScheme = false;
  nsresult rv = aContentLocation->SchemeIs("data", &isDataScheme);
  NS_ENSURE_SUCCESS(rv, rv);

  outCacheKey.Truncate();
  if (aContentType != nsIContentPolicy::TYPE_SCRIPT && isDataScheme) {
    // For non-script data: URI, use ("data:", aContentType) as the cache key.
    outCacheKey.Append(NS_LITERAL_CSTRING("data:"));
    outCacheKey.AppendInt(aContentType);
    return NS_OK;
  }

  nsAutoCString spec;
  rv = aContentLocation->GetSpec(spec);
  NS_ENSURE_SUCCESS(rv, rv);

  // Don't cache for a URI longer than the cutoff size.
  if (spec.Length() <= CSP_CACHE_URI_CUTOFF_SIZE) {
    outCacheKey.Append(spec);
    outCacheKey.Append(NS_LITERAL_CSTRING("!"));
    outCacheKey.AppendInt(aContentType);
  }

  return NS_OK;
}

/* =====  nsIContentSecurityPolicy impl ====== */

NS_IMETHODIMP
nsCSPContext::ShouldLoad(nsContentPolicyType aContentType,
                         nsIURI*             aContentLocation,
                         nsIURI*             aRequestOrigin,
                         nsISupports*        aRequestContext,
                         const nsACString&   aMimeTypeGuess,
                         nsISupports*        aExtra,
                         int16_t*            outDecision)
{
  if (CSPCONTEXTLOGENABLED()) {
    CSPCONTEXTLOG(("nsCSPContext::ShouldLoad, aContentLocation: %s",
                   aContentLocation->GetSpecOrDefault().get()));
    CSPCONTEXTLOG((">>>>                      aContentType: %d", aContentType));
  }

  bool isPreload = nsContentUtils::IsPreloadType(aContentType);

  // Since we know whether we are dealing with a preload, we have to convert
  // the internal policytype ot the external policy type before moving on.
  // We still need to know if this is a worker so child-src can handle that
  // case correctly.
  aContentType = nsContentUtils::InternalContentPolicyTypeToExternalOrWorker(aContentType);

  nsresult rv = NS_OK;

  // This ShouldLoad function is called from nsCSPService::ShouldLoad,
  // which already checked a number of things, including:
  // * aContentLocation is not null; we can consume this without further checks
  // * scheme is not a whitelisted scheme (about: chrome:, etc).
  // * CSP is enabled
  // * Content Type is not whitelisted (CSP Reports, TYPE_DOCUMENT, etc).
  // * Fast Path for Apps

  nsAutoCString cacheKey;
  rv = CreateCacheKey_Internal(aContentLocation, aContentType, cacheKey);
  NS_ENSURE_SUCCESS(rv, rv);

  bool isCached = mShouldLoadCache.Get(cacheKey, outDecision);
  if (isCached && cacheKey.Length() > 0) {
    // this is cached, use the cached value.
    return NS_OK;
  }

  // Default decision, CSP can revise it if there's a policy to enforce
  *outDecision = nsIContentPolicy::ACCEPT;

  // If the content type doesn't map to a CSP directive, there's nothing for
  // CSP to do.
  CSPDirective dir = CSP_ContentTypeToDirective(aContentType);
  if (dir == nsIContentSecurityPolicy::NO_DIRECTIVE) {
    return NS_OK;
  }

  nsAutoString nonce;
  bool parserCreated = false;
  if (!isPreload) {
    if (aContentType == nsIContentPolicy::TYPE_SCRIPT ||
        aContentType == nsIContentPolicy::TYPE_STYLESHEET) {
      nsCOMPtr<nsIDOMHTMLElement> htmlElement = do_QueryInterface(aRequestContext);
      if (htmlElement) {
        rv = htmlElement->GetAttribute(NS_LITERAL_STRING("nonce"), nonce);
        NS_ENSURE_SUCCESS(rv, rv);
      }
    }

    nsCOMPtr<nsIScriptElement> script = do_QueryInterface(aRequestContext);
    if (script && script->GetParserCreated() != mozilla::dom::NOT_FROM_PARSER) {
      parserCreated = true;
    }
  }

  // aExtra holds the original URI of the channel if the
  // channel got redirected (until we fix Bug 1332422).
  nsCOMPtr<nsIURI> originalURI = do_QueryInterface(aExtra);
  bool wasRedirected = originalURI;

  bool permitted = permitsInternal(dir,
                                   aContentLocation,
                                   originalURI,
                                   nonce,
                                   wasRedirected,
                                   isPreload,
                                   false,     // allow fallback to default-src
                                   true,      // send violation reports
                                   true,     // send blocked URI in violation reports
                                   parserCreated);

  *outDecision = permitted ? nsIContentPolicy::ACCEPT
                           : nsIContentPolicy::REJECT_SERVER;

  // Done looping, cache any relevant result
  if (cacheKey.Length() > 0 && !isPreload) {
    mShouldLoadCache.Put(cacheKey, *outDecision);
  }

  if (CSPCONTEXTLOGENABLED()) {
    CSPCONTEXTLOG(("nsCSPContext::ShouldLoad, decision: %s, "
                   "aContentLocation: %s",
                   *outDecision > 0 ? "load" : "deny",
                   aContentLocation->GetSpecOrDefault().get()));
  }
  return NS_OK;
}

bool
nsCSPContext::permitsInternal(CSPDirective aDir,
                              nsIURI* aContentLocation,
                              nsIURI* aOriginalURI,
                              const nsAString& aNonce,
                              bool aWasRedirected,
                              bool aIsPreload,
                              bool aSpecific,
                              bool aSendViolationReports,
                              bool aSendContentLocationInViolationReports,
                              bool aParserCreated)
{
  bool permits = true;

  nsAutoString violatedDirective;
  for (uint32_t p = 0; p < mPolicies.Length(); p++) {
    if (!mPolicies[p]->permits(aDir,
                               aContentLocation,
                               aNonce,
                               aWasRedirected,
                               aSpecific,
                               aParserCreated,
                               violatedDirective)) {
      // If the policy is violated and not report-only, reject the load and
      // report to the console
      if (!mPolicies[p]->getReportOnlyFlag()) {
        CSPCONTEXTLOG(("nsCSPContext::permitsInternal, false"));
        permits = false;
      }

      // Do not send a report or notify observers if this is a preload - the
      // decision may be wrong due to the inability to get the nonce, and will
      // incorrectly fail the unit tests.
      if (CSPService::sCSPReportingEnabled && !aIsPreload && aSendViolationReports) {
        uint32_t lineNumber = 0;
        uint32_t columnNumber = 0;
        nsAutoCString spec;
        JSContext* cx = nsContentUtils::GetCurrentJSContext();
        if (cx) {
          nsJSUtils::GetCallingLocation(cx, spec, &lineNumber, &columnNumber);
          // If GetCallingLocation fails linenumber & columnNumber are set to 0
          // anyway so we can skip checking if that is the case.
        }
        this->AsyncReportViolation((aSendContentLocationInViolationReports ?
                                    aContentLocation : nullptr),
                                    aOriginalURI,  /* in case of redirect originalURI is not null */
                                    violatedDirective,
                                    p,                           /* policy index        */
                                    EmptyString(),               /* no observer subject */
                                    NS_ConvertUTF8toUTF16(spec), /* source file.        */
                                    EmptyString(),               /* no script sample    */
                                    lineNumber,                  /* line number         */
                                    columnNumber);               /* column number       */
      }
    }
  }

  return permits;
}



/* ===== nsISupports implementation ========== */

NS_IMPL_CLASSINFO(nsCSPContext,
                  nullptr,
                  nsIClassInfo::MAIN_THREAD_ONLY,
                  NS_CSPCONTEXT_CID)

NS_IMPL_ISUPPORTS_CI(nsCSPContext,
                     nsIContentSecurityPolicy,
                     nsISerializable)

int32_t nsCSPContext::sScriptSampleMaxLength;

nsCSPContext::nsCSPContext()
  : mInnerWindowID(0)
  , mLoadingContext(nullptr)
  , mLoadingPrincipal(nullptr)
  , mQueueUpMessages(true)
{
  static bool sInitialized = false;
  if (!sInitialized) {
    Preferences::AddIntVarCache(&sScriptSampleMaxLength,
                                "security.csp.reporting.script-sample.max-length",
                                40);
    sInitialized = true;
  }

  CSPCONTEXTLOG(("nsCSPContext::nsCSPContext"));
}

nsCSPContext::~nsCSPContext()
{
  CSPCONTEXTLOG(("nsCSPContext::~nsCSPContext"));
  for (uint32_t i = 0; i < mPolicies.Length(); i++) {
    delete mPolicies[i];
  }
  mShouldLoadCache.Clear();
}

NS_IMETHODIMP
nsCSPContext::GetPolicyString(uint32_t aIndex, nsAString& outStr)
{
  if (aIndex >= mPolicies.Length()) {
    return NS_ERROR_ILLEGAL_VALUE;
  }
  mPolicies[aIndex]->toString(outStr);
  return NS_OK;
}

const nsCSPPolicy*
nsCSPContext::GetPolicy(uint32_t aIndex)
{
  if (aIndex >= mPolicies.Length()) {
    return nullptr;
  }
  return mPolicies[aIndex];
}

NS_IMETHODIMP
nsCSPContext::GetPolicyCount(uint32_t *outPolicyCount)
{
  *outPolicyCount = mPolicies.Length();
  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::GetUpgradeInsecureRequests(bool *outUpgradeRequest)
{
  *outUpgradeRequest = false;
  for (uint32_t i = 0; i < mPolicies.Length(); i++) {
    if (mPolicies[i]->hasDirective(nsIContentSecurityPolicy::UPGRADE_IF_INSECURE_DIRECTIVE)) {
      *outUpgradeRequest = true;
      return NS_OK;
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::GetBlockAllMixedContent(bool *outBlockAllMixedContent)
{
  *outBlockAllMixedContent = false;
  for (uint32_t i = 0; i < mPolicies.Length(); i++) {
     if (!mPolicies[i]->getReportOnlyFlag() &&
        mPolicies[i]->hasDirective(nsIContentSecurityPolicy::BLOCK_ALL_MIXED_CONTENT)) {
      *outBlockAllMixedContent = true;
      return NS_OK;
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::GetEnforcesFrameAncestors(bool *outEnforcesFrameAncestors)
{
  *outEnforcesFrameAncestors = false;
  for (uint32_t i = 0; i < mPolicies.Length(); i++) {
    if (!mPolicies[i]->getReportOnlyFlag() &&
        mPolicies[i]->hasDirective(nsIContentSecurityPolicy::FRAME_ANCESTORS_DIRECTIVE)) {
      *outEnforcesFrameAncestors = true;
      return NS_OK;
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::AppendPolicy(const nsAString& aPolicyString,
                           bool aReportOnly,
                           bool aDeliveredViaMetaTag)
{
  CSPCONTEXTLOG(("nsCSPContext::AppendPolicy: %s",
                 NS_ConvertUTF16toUTF8(aPolicyString).get()));

  // Use the mSelfURI from setRequestContext, see bug 991474
  NS_ASSERTION(mSelfURI, "mSelfURI required for AppendPolicy, but not set");
  nsCSPPolicy* policy = nsCSPParser::parseContentSecurityPolicy(aPolicyString, mSelfURI,
                                                                aReportOnly, this,
                                                                aDeliveredViaMetaTag);
  if (policy) {
    mPolicies.AppendElement(policy);
    // reset cache since effective policy changes
    mShouldLoadCache.Clear();
  }
  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::GetAllowsEval(bool* outShouldReportViolation,
                            bool* outAllowsEval)
{
  *outShouldReportViolation = false;
  *outAllowsEval = true;

  for (uint32_t i = 0; i < mPolicies.Length(); i++) {
    if (!mPolicies[i]->allows(SCRIPT_SRC_DIRECTIVE,
                              CSP_UNSAFE_EVAL,
                              EmptyString(),
                              false)) {
      // policy is violated: must report the violation and allow the inline
      // script if the policy is report-only.
      *outShouldReportViolation = true;
      if (!mPolicies[i]->getReportOnlyFlag()) {
        *outAllowsEval = false;
      }
    }
  }
  return NS_OK;
}

// Helper function to report inline violations
void
nsCSPContext::reportInlineViolation(CSPDirective aDirective,
                                    const nsAString& aNonce,
                                    const nsAString& aContent,
                                    const nsAString& aViolatedDirective,
                                    uint32_t aViolatedPolicyIndex, // TODO, use report only flag for that
                                    uint32_t aLineNumber,
                                    uint32_t aColumnNumber)
{
  nsString observerSubject;
  // if the nonce is non empty, then we report the nonce error, otherwise
  // let's report the hash error; no need to report the unsafe-inline error
  // anymore.
  if (!aNonce.IsEmpty()) {
    observerSubject = (aDirective == SCRIPT_SRC_ELEM_DIRECTIVE ||
                       aDirective == SCRIPT_SRC_ATTR_DIRECTIVE)
                      ? NS_LITERAL_STRING(SCRIPT_NONCE_VIOLATION_OBSERVER_TOPIC)
                      : NS_LITERAL_STRING(STYLE_NONCE_VIOLATION_OBSERVER_TOPIC);
  }
  else {
    observerSubject = (aDirective == SCRIPT_SRC_ELEM_DIRECTIVE ||
                       aDirective == SCRIPT_SRC_ATTR_DIRECTIVE)
                      ? NS_LITERAL_STRING(SCRIPT_HASH_VIOLATION_OBSERVER_TOPIC)
                      : NS_LITERAL_STRING(STYLE_HASH_VIOLATION_OBSERVER_TOPIC);
  }

  nsCOMPtr<nsISupportsCString> selfICString(do_CreateInstance(NS_SUPPORTS_CSTRING_CONTRACTID));
  if (selfICString) {
    selfICString->SetData(nsDependentCString("inline"));
  }
  nsCOMPtr<nsISupports> selfISupports(do_QueryInterface(selfICString));

  // use selfURI as the sourceFile
  nsAutoCString sourceFile;
  if (mSelfURI) {
    mSelfURI->GetSpec(sourceFile);
  }

  nsAutoString codeSample(aContent);
  // cap the length of the script sample
  if (codeSample.Length() > ScriptSampleMaxLength()) {
    codeSample.Truncate(ScriptSampleMaxLength());
    codeSample.AppendLiteral("...");
  }

  uint32_t lineNumber = aLineNumber;
  uint32_t columnNumber = aColumnNumber;

  JSContext* cx = nsContentUtils::GetCurrentJSContext();
  if (cx) {
    if (!nsJSUtils::GetCallingLocation(cx, sourceFile, &lineNumber,
                                       &columnNumber)) {
      // Get Calling Location resets line/col to 0
      // so we reset those to the intial arguments
      // in case it failed
      lineNumber = aLineNumber;
      columnNumber = aColumnNumber;
    }
  }

  AsyncReportViolation(selfISupports,                      // aBlockedContentSource
                       mSelfURI,                           // aOriginalURI
                       aViolatedDirective,                 // aViolatedDirective
                       aViolatedPolicyIndex,               // aViolatedPolicyIndex
                       observerSubject,                    // aObserverSubject
                       NS_ConvertUTF8toUTF16(sourceFile),  // aSourceFile
                       codeSample,                         // aScriptSample
                       lineNumber,                         // aLineNum
                       columnNumber);                      // aColumnNum
}

NS_IMETHODIMP
nsCSPContext::GetAllowsInline(CSPDirective aDirective,
                              const nsAString& aNonce,
                              bool aParserCreated,
                              const nsAString& aContent,
                              uint32_t aLineNumber,
                              uint32_t aColumnNumber,
                              bool* outAllowsInline)
{
  *outAllowsInline = true;

  if (aDirective != SCRIPT_SRC_ELEM_DIRECTIVE &&
      aDirective != SCRIPT_SRC_ATTR_DIRECTIVE &&
      aDirective != STYLE_SRC_ELEM_DIRECTIVE &&
      aDirective != STYLE_SRC_ATTR_DIRECTIVE) {
    MOZ_ASSERT(false, "can only allow inline for (script/style)-src-(attr/elem) or style");
    return NS_OK;
  }

  // always iterate all policies, otherwise we might not send out all reports
  for (uint32_t i = 0; i < mPolicies.Length(); i++) {
    bool allowed =
      mPolicies[i]->allows(aDirective, CSP_UNSAFE_INLINE, EmptyString(), aParserCreated) ||
      mPolicies[i]->allows(aDirective, CSP_NONCE, aNonce, aParserCreated);
      
    // If the inlined script or style is allowed by either unsafe-inline or the
    // nonce, go ahead and shortcut this loop.
    if (allowed) {
      continue;
    }
    
    // Check if the csp-hash matches against the hash of the script.
    // If we don't have any content to check, block the script.
    if (!aContent.IsEmpty()) {
      allowed = mPolicies[i]->allows(aDirective, CSP_HASH, aContent, aParserCreated);
    }

    if (!allowed) {
      // policy is violoated: deny the load unless policy is report only and
      // report the violation.
      if (!mPolicies[i]->getReportOnlyFlag()) {
        *outAllowsInline = false;
      }
      nsAutoString violatedDirective;
      mPolicies[i]->getDirectiveStringForContentType(aDirective, violatedDirective);
      if(CSPService::sCSPReportingEnabled) {
        reportInlineViolation(aDirective,
                              aNonce,
                              aContent,
                              violatedDirective,
                              i,
                              aLineNumber,
                              aColumnNumber);
      }
    }
  }
  return NS_OK;
}


/**
 * Reduces some code repetition for the various logging situations in
 * LogViolationDetails.
 *
 * Call-sites for the eval/inline checks recieve two return values: allows
 * and violates.  Based on those, they must choose whether to call
 * LogViolationDetails or not.  Policies that are report-only allow the
 * loads/compilations but violations should still be reported.  Not all
 * policies in this nsIContentSecurityPolicy instance will be violated,
 * which is why we must check allows() again here.
 *
 * Note: This macro uses some parameters from its caller's context:
 * p, mPolicies, this, aSourceFile, aScriptSample, aLineNum, aColumnNum,
 * selfISupports
 *
 * @param violationType: the VIOLATION_TYPE_* constant (partial symbol)
 *                 such as INLINE_SCRIPT
 * @param contentPolicyType: a constant from nsIContentPolicy such as TYPE_STYLESHEET
 * @param nonceOrHash: for NONCE and HASH violations, it's the nonce or content
 *               string. For other violations, it is an empty string.
 * @param keyword: the keyword corresponding to violation (UNSAFE_INLINE for most)
 * @param observerTopic: the observer topic string to send with the CSP
 *                 observer notifications.
 *
 * Please note that inline violations for scripts are reported within
 * GetAllowsInline() and do not call this macro, hence we can pass 'false'
 * as the argument _aParserCreated_ to allows().
 */
#define CASE_CHECK_AND_REPORT(violationType, directive, nonceOrHash,           \
                              keyword, observerTopic)                          \
  case nsIContentSecurityPolicy::VIOLATION_TYPE_ ## violationType :            \
    PR_BEGIN_MACRO                                                             \
    static_assert(directive##_SRC_DIRECTIVE == SCRIPT_SRC_DIRECTIVE ||         \
                  directive##_SRC_DIRECTIVE == STYLE_SRC_DIRECTIVE);           \
    if(CSPService::sCSPReportingEnabled &&                                     \
       !mPolicies[p]->allows(directive##_SRC_DIRECTIVE, keyword, nonceOrHash,  \
                              false)) {                                        \
      nsAutoString violatedDirective;                                          \
      mPolicies[p]->getDirectiveStringForContentType(                          \
          directive##_SRC_DIRECTIVE, violatedDirective);                       \
      this->AsyncReportViolation(selfISupports, nullptr, violatedDirective, p, \
                                 NS_LITERAL_STRING(observerTopic), aSourceFile,\
                                 aScriptSample, aLineNum, aColumnNum);         \
    }                                                                          \
    PR_END_MACRO;                                                              \
    break

/**
 * For each policy, log any violation on the Error Console and send a report
 * if a report-uri is present in the policy
 *
 * @param aViolationType
 *     one of the VIOLATION_TYPE_* constants, e.g. inline-script or eval
 * @param aSourceFile
 *     name of the source file containing the violation (if available)
 * @param aContentSample
 *     sample of the violating content (to aid debugging)
 * @param aLineNum
 *     source line number of the violation (if available)
 * @param aNonce
 *     (optional) If this is a nonce violation, include the nonce so we can
 *     recheck to determine which policies were violated and send the
 *     appropriate reports.
 * @param aContent
 *     (optional) If this is a hash violation, include contents of the inline
 *     resource in the question so we can recheck the hash in order to
 *     determine which policies were violated and send the appropriate
 *     reports.
 */
NS_IMETHODIMP
nsCSPContext::LogViolationDetails(uint16_t aViolationType,
                                  const nsAString& aSourceFile,
                                  const nsAString& aScriptSample,
                                  int32_t aLineNum,
                                  int32_t aColumnNum,
                                  const nsAString& aNonce,
                                  const nsAString& aContent)
{
  for (uint32_t p = 0; p < mPolicies.Length(); p++) {
    NS_ASSERTION(mPolicies[p], "null pointer in nsTArray<nsCSPPolicy>");

    nsCOMPtr<nsISupportsCString> selfICString(do_CreateInstance(NS_SUPPORTS_CSTRING_CONTRACTID));
    if (selfICString) {
      if (aViolationType == nsIContentSecurityPolicy::VIOLATION_TYPE_EVAL) {
        selfICString->SetData(nsDependentCString("eval"));
      } else if (aViolationType == nsIContentSecurityPolicy::VIOLATION_TYPE_INLINE_SCRIPT ||
                 aViolationType == nsIContentSecurityPolicy::VIOLATION_TYPE_INLINE_STYLE) {
        selfICString->SetData(nsDependentCString("inline"));
      } else {
        // All the other types should have a URL, but just in case, let's use
        // 'self' here.
        selfICString->SetData(nsDependentCString("self"));
      }
    }
    nsCOMPtr<nsISupports> selfISupports(do_QueryInterface(selfICString));

    switch (aViolationType) {
      CASE_CHECK_AND_REPORT(EVAL,              SCRIPT,     NS_LITERAL_STRING(""),
                            CSP_UNSAFE_EVAL,   EVAL_VIOLATION_OBSERVER_TOPIC);
      CASE_CHECK_AND_REPORT(INLINE_STYLE,      STYLE,      NS_LITERAL_STRING(""),
                            CSP_UNSAFE_INLINE, INLINE_STYLE_VIOLATION_OBSERVER_TOPIC);
      CASE_CHECK_AND_REPORT(INLINE_SCRIPT,     SCRIPT,     NS_LITERAL_STRING(""),
                            CSP_UNSAFE_INLINE, INLINE_SCRIPT_VIOLATION_OBSERVER_TOPIC);
      CASE_CHECK_AND_REPORT(NONCE_SCRIPT,      SCRIPT,     aNonce,
                            CSP_UNSAFE_INLINE, SCRIPT_NONCE_VIOLATION_OBSERVER_TOPIC);
      CASE_CHECK_AND_REPORT(NONCE_STYLE,       STYLE,      aNonce,
                            CSP_UNSAFE_INLINE, STYLE_NONCE_VIOLATION_OBSERVER_TOPIC);
      CASE_CHECK_AND_REPORT(HASH_SCRIPT,       SCRIPT,     aContent,
                            CSP_UNSAFE_INLINE, SCRIPT_HASH_VIOLATION_OBSERVER_TOPIC);
      CASE_CHECK_AND_REPORT(HASH_STYLE,        STYLE,      aContent,
                            CSP_UNSAFE_INLINE, STYLE_HASH_VIOLATION_OBSERVER_TOPIC);
      CASE_CHECK_AND_REPORT(REQUIRE_SRI_FOR_STYLE,   STYLE, NS_LITERAL_STRING(""),
                            CSP_REQUIRE_SRI_FOR, REQUIRE_SRI_STYLE_VIOLATION_OBSERVER_TOPIC);
      CASE_CHECK_AND_REPORT(REQUIRE_SRI_FOR_SCRIPT,   SCRIPT, NS_LITERAL_STRING(""),
                            CSP_REQUIRE_SRI_FOR, REQUIRE_SRI_SCRIPT_VIOLATION_OBSERVER_TOPIC);


      default:
        NS_ASSERTION(false, "LogViolationDetails with invalid type");
        break;
    }
  }
  return NS_OK;
}

#undef CASE_CHECK_AND_REPORT

NS_IMETHODIMP
nsCSPContext::SetRequestContext(nsIDOMDocument* aDOMDocument,
                                nsIPrincipal* aPrincipal)
{
  NS_PRECONDITION(aDOMDocument || aPrincipal,
                  "Can't set context without doc or principal");
  NS_ENSURE_ARG(aDOMDocument || aPrincipal);

  if (aDOMDocument) {
    nsCOMPtr<nsIDocument> doc = do_QueryInterface(aDOMDocument);
    mLoadingContext = do_GetWeakReference(doc);
    mSelfURI = doc->GetDocumentURI();
    mLoadingPrincipal = doc->NodePrincipal();
    doc->GetReferrer(mReferrer);
    mInnerWindowID = doc->InnerWindowID();
    // the innerWindowID is not available for CSPs delivered through the
    // header at the time setReqeustContext is called - let's queue up
    // console messages until it becomes available, see flushConsoleMessages
    mQueueUpMessages = !mInnerWindowID;
    mCallingChannelLoadGroup = doc->GetDocumentLoadGroup();

    // set the flag on the document for CSP telemetry
    doc->SetHasCSP(true);
  }
  else {
    CSPCONTEXTLOG(("No Document in SetRequestContext; can not query loadgroup; sending reports may fail."));
    mLoadingPrincipal = aPrincipal;
    mLoadingPrincipal->GetURI(getter_AddRefs(mSelfURI));
    // if no document is available, then it also does not make sense to queue console messages
    // sending messages to the browser conolse instead of the web console in that case.
    mQueueUpMessages = false;
  }

  NS_ASSERTION(mSelfURI, "mSelfURI not available, can not translate 'self' into actual URI");
  return NS_OK;
}

struct ConsoleMsgQueueElem {
  nsXPIDLString mMsg;
  nsString      mSourceName;
  nsString      mSourceLine;
  uint32_t      mLineNumber;
  uint32_t      mColumnNumber;
  uint32_t      mSeverityFlag;
};

void
nsCSPContext::flushConsoleMessages()
{
  // should flush messages even if doc is not available
  nsCOMPtr<nsIDocument> doc = do_QueryReferent(mLoadingContext);
  if (doc) {
    mInnerWindowID = doc->InnerWindowID();
  }
  mQueueUpMessages = false;

  for (uint32_t i = 0; i < mConsoleMsgQueue.Length(); i++) {
    ConsoleMsgQueueElem &elem = mConsoleMsgQueue[i];
    CSP_LogMessage(elem.mMsg, elem.mSourceName, elem.mSourceLine,
                   elem.mLineNumber, elem.mColumnNumber,
                   elem.mSeverityFlag, "CSP", mInnerWindowID);
  }
  mConsoleMsgQueue.Clear();
}

void
nsCSPContext::logToConsole(const char16_t* aName,
                           const char16_t** aParams,
                           uint32_t aParamsLength,
                           const nsAString& aSourceName,
                           const nsAString& aSourceLine,
                           uint32_t aLineNumber,
                           uint32_t aColumnNumber,
                           uint32_t aSeverityFlag)
{
  // let's check if we have to queue up console messages
  if (mQueueUpMessages) {
    nsXPIDLString msg;
    CSP_GetLocalizedStr(aName, aParams, aParamsLength, getter_Copies(msg));
    ConsoleMsgQueueElem &elem = *mConsoleMsgQueue.AppendElement();
    elem.mMsg = msg;
    elem.mSourceName = PromiseFlatString(aSourceName);
    elem.mSourceLine = PromiseFlatString(aSourceLine);
    elem.mLineNumber = aLineNumber;
    elem.mColumnNumber = aColumnNumber;
    elem.mSeverityFlag = aSeverityFlag;
    return;
  }
  CSP_LogLocalizedStr(aName, aParams, aParamsLength, aSourceName,
                      aSourceLine, aLineNumber, aColumnNumber,
                      aSeverityFlag, "CSP", mInnerWindowID);
}

/**
 * Strip URI for reporting according to:
 * http://www.w3.org/TR/CSP/#violation-reports
 *
 * @param aURI
 *        The uri to be stripped for reporting
 * @param aSelfURI
 *        The uri of the protected resource
 *        which is needed to enforce the SOP.
 * @return ASCII serialization of the uri to be reported.
 */
void
StripURIForReporting(nsIURI* aURI,
                     nsIURI* aSelfURI,
                     nsACString& outStrippedURI)
{
  bool isAllowedScheme =
    (NS_SUCCEEDED(aURI->SchemeIs("http", &isAllowedScheme)) && isAllowedScheme) ||
    (NS_SUCCEEDED(aURI->SchemeIs("https", &isAllowedScheme)) && isAllowedScheme) ||
    (NS_SUCCEEDED(aURI->SchemeIs("ftp", &isAllowedScheme)) && isAllowedScheme) ||
    (NS_SUCCEEDED(aURI->SchemeIs("ws", &isAllowedScheme)) && isAllowedScheme) ||
    (NS_SUCCEEDED(aURI->SchemeIs("wss", &isAllowedScheme)) && isAllowedScheme);

  if (!isAllowedScheme) {
    // Step 1. If url's scheme is not an allowed scheme, then just return url's scheme,
    // i.e. treat aURI as a globally unique identifier.
    // What we really care about reporting is http/https/ftp.
    // https://github.com/w3c/webappsec-csp/issues/735: We also allow WS(S) schemes.
    aURI->GetScheme(outStrippedURI);
    return;
  }

  // Step 2. Set url's fragment to the empty string.
  // Implicit in GetSpecIgnoringRef() below.
  
  // Step 3. Set url's username/password to the empty string.
  nsCOMPtr<nsIURI> stripped;
  nsresult rv = aURI->Clone(getter_AddRefs(stripped));
  if (NS_FAILED(rv)) {
    // Cloning the URI failed for some reason, just return the scheme.
    aURI->GetScheme(outStrippedURI);
    return;
  }
  rv = stripped->SetUserPass(EmptyCString());
  if (NS_FAILED(rv)) {
    // Mutating the URI failed for some reason, just return the scheme.
    aURI->GetScheme(outStrippedURI);
    return;
  }

  // Non-standard: https://github.com/w3c/webappsec-csp/issues/735
  // We match other browsers here: To avoid leaking the whole URL when blocking
  // (or reporting!) cross-origin navigations inside a frame, we restrict the URLs
  // to just the (ASCII serialization of) uri's origin.
  if (!NS_SecurityCompareURIs(aSelfURI, stripped, false)) {
    stripped->GetPrePath(outStrippedURI);
    return;
  }

  // Step 4. Return uri, with any unwanted component removed.
  stripped->GetSpecIgnoringRef(outStrippedURI);
}

nsresult
nsCSPContext::GatherSecurityPolicyViolationEventData(
  nsIURI* aBlockedURI,
  const nsACString& aBlockedString,
  nsIURI* aOriginalURI,
  nsAString& aViolatedDirective,
  uint32_t aViolatedPolicyIndex,
  nsAString& aSourceFile,
  nsAString& aScriptSample,
  uint32_t aLineNum,
  uint32_t aColumnNum,
  mozilla::dom::SecurityPolicyViolationEventInit& aViolationEventInit)
{
  NS_ENSURE_ARG_MAX(aViolatedPolicyIndex, mPolicies.Length() - 1);

  MOZ_ASSERT(ValidateDirectiveName(aViolatedDirective), "Invalid directive name");

  if (!CSPService::sCSPReportingEnabled) {
    // Reporting is pref-disabled. Don't do any actual work and return success.
    nsContentUtils::ReportToConsoleNonLocalized(
        NS_LITERAL_STRING("CSP violation report not sent: reports have been disabled contrary to spec."),
        nsIScriptError::warningFlag,
        NS_LITERAL_CSTRING("Content Security Policy"),
        nullptr);
    return NS_OK;
  }
  
  nsresult rv;

  // document-uri
  nsAutoCString reportDocumentURI;
  StripURIForReporting(mSelfURI, mSelfURI, reportDocumentURI);
  aViolationEventInit.mDocumentURI = NS_ConvertUTF8toUTF16(reportDocumentURI);

  // referrer
  aViolationEventInit.mReferrer = mReferrer;

  // blocked-uri
  if (aBlockedURI) {
    nsAutoCString reportBlockedURI;
    StripURIForReporting(aBlockedURI, mSelfURI, reportBlockedURI);
    aViolationEventInit.mBlockedURI = NS_ConvertUTF8toUTF16(reportBlockedURI);
  } else {
    aViolationEventInit.mBlockedURI = NS_ConvertUTF8toUTF16(aBlockedString);
  }

  // effective-directive
  // The name of the policy directive that was violated.
  aViolationEventInit.mEffectiveDirective = aViolatedDirective;

  // violated-directive
  // In CSP2, the policy directive that was violated, as it appears in the policy.
  // In CSP3, the same as effective-directive.
  aViolationEventInit.mViolatedDirective = aViolatedDirective;

  // original-policy
  nsAutoString originalPolicy;
  rv = this->GetPolicyString(aViolatedPolicyIndex, originalPolicy);
  NS_ENSURE_SUCCESS(rv, rv);
  aViolationEventInit.mOriginalPolicy = originalPolicy;

  // source-file
  if (!aSourceFile.IsEmpty()) {
    // if aSourceFile is a URI, we have to make sure to strip fragments
    nsCOMPtr<nsIURI> sourceURI;
    NS_NewURI(getter_AddRefs(sourceURI), aSourceFile);
    if (sourceURI) {
      nsAutoCString spec;
      sourceURI->GetSpecIgnoringRef(spec);
      aSourceFile = NS_ConvertUTF8toUTF16(spec);
    }
    aViolationEventInit.mSourceFile = aSourceFile;
  }

  // sample, max 40 chars.
  aViolationEventInit.mSample = aScriptSample;
  uint32_t length = aViolationEventInit.mSample.Length();
  if (length > ScriptSampleMaxLength()) {
    uint32_t desiredLength = ScriptSampleMaxLength();
    // Don't cut off right before a low surrogate. Just include it.
    if (NS_IS_LOW_SURROGATE(aViolationEventInit.mSample[desiredLength])) {
      desiredLength++;
    }
    aViolationEventInit.mSample.Replace(ScriptSampleMaxLength(),
                                        length - desiredLength,
                                        nsContentUtils::GetLocalizedEllipsis());
  }

  // disposition
  aViolationEventInit.mDisposition = mPolicies[aViolatedPolicyIndex]->getReportOnlyFlag()
    ? mozilla::dom::SecurityPolicyViolationEventDisposition::Report
    : mozilla::dom::SecurityPolicyViolationEventDisposition::Enforce;

  // status-code
  uint16_t statusCode = 0;
  {
    nsCOMPtr<nsIDocument> doc = do_QueryReferent(mLoadingContext);
    if (doc) {
      nsCOMPtr<nsIHttpChannel> channel = do_QueryInterface(doc->GetChannel());
      if (channel) {
        uint32_t responseStatus = 0;
        nsresult rv = channel->GetResponseStatus(&responseStatus);
        if (NS_SUCCEEDED(rv) && (responseStatus <= UINT16_MAX)) {
          statusCode = static_cast<uint16_t>(responseStatus);
        }
      }
    }
  }
  aViolationEventInit.mStatusCode = statusCode;

  // line-number
  aViolationEventInit.mLineNumber = aLineNum;

  // column-number
  aViolationEventInit.mColumnNumber = aColumnNum;

  aViolationEventInit.mBubbles = true;
  aViolationEventInit.mComposed = true;

  return NS_OK;
}

nsresult
nsCSPContext::SendReports(
  const mozilla::dom::SecurityPolicyViolationEventInit& aViolationEventInit,
  uint32_t aViolatedPolicyIndex)
{
  NS_ENSURE_ARG_MAX(aViolatedPolicyIndex, mPolicies.Length() - 1);

  dom::CSPReport report;

  // blocked-uri
  report.mCsp_report.mBlocked_uri = aViolationEventInit.mBlockedURI;

  // document-uri
  report.mCsp_report.mDocument_uri = aViolationEventInit.mDocumentURI;

  // original-policy
  report.mCsp_report.mOriginal_policy = aViolationEventInit.mOriginalPolicy;

  // referrer
  report.mCsp_report.mReferrer = aViolationEventInit.mReferrer;

  // violated-directive
  report.mCsp_report.mViolated_directive = aViolationEventInit.mViolatedDirective;

  // source-file
  if (!aViolationEventInit.mSourceFile.IsEmpty()) {
    report.mCsp_report.mSource_file.Construct();
    report.mCsp_report.mSource_file.Value() = aViolationEventInit.mSourceFile;
  }

  // script-sample
  if (!aViolationEventInit.mSample.IsEmpty()) {
    report.mCsp_report.mScript_sample.Construct();
    report.mCsp_report.mScript_sample.Value() = aViolationEventInit.mSample;
  }

  // line-number
  if (aViolationEventInit.mLineNumber != 0) {
    report.mCsp_report.mLine_number.Construct();
    report.mCsp_report.mLine_number.Value() = aViolationEventInit.mLineNumber;
  }

  if (aViolationEventInit.mColumnNumber != 0) {
    report.mCsp_report.mColumn_number.Construct();
    report.mCsp_report.mColumn_number.Value() = aViolationEventInit.mColumnNumber;
  }

  nsString csp_report;
  if (!report.ToJSON(csp_report)) {
    return NS_ERROR_FAILURE;
  }

  // ---------- Assembled, now send it to all the report URIs ----------- //

  nsTArray<nsString> reportURIs;
  mPolicies[aViolatedPolicyIndex]->getReportURIs(reportURIs);

  nsCOMPtr<nsIDocument> doc = do_QueryReferent(mLoadingContext);
  nsCOMPtr<nsIURI> reportURI;
  nsCOMPtr<nsIChannel> reportChannel;

  nsresult rv;
  for (uint32_t r = 0; r < reportURIs.Length(); r++) {
    nsAutoCString reportURICstring = NS_ConvertUTF16toUTF8(reportURIs[r]);
    // try to create a new uri from every report-uri string
    rv = NS_NewURI(getter_AddRefs(reportURI), reportURIs[r]);
    if (NS_FAILED(rv)) {
      const char16_t* params[] = { reportURIs[r].get() };
      CSPCONTEXTLOG(("Could not create nsIURI for report URI %s",
                     reportURICstring.get()));
      logToConsole(u"triedToSendReport", params, ArrayLength(params),
                   aViolationEventInit.mSourceFile, aViolationEventInit.mSample,
                   aViolationEventInit.mLineNumber, aViolationEventInit.mColumnNumber, 
                   nsIScriptError::errorFlag);
      continue; // don't return yet, there may be more URIs
    }

    // try to create a new channel for every report-uri
    nsLoadFlags loadFlags = nsIRequest::LOAD_NORMAL;
    if (doc) {
      rv = NS_NewChannel(getter_AddRefs(reportChannel),
                         reportURI,
                         doc,
                         nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
                         nsIContentPolicy::TYPE_CSP_REPORT,
                         nullptr, // aLoadGroup
                         nullptr, // aCallbacks
                         loadFlags);
    }
    else {
      rv = NS_NewChannel(getter_AddRefs(reportChannel),
                         reportURI,
                         mLoadingPrincipal,
                         nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
                         nsIContentPolicy::TYPE_CSP_REPORT,
                         nullptr, // aLoadGroup
                         nullptr, // aCallbacks
                         loadFlags);
    }

    if (NS_FAILED(rv)) {
      CSPCONTEXTLOG(("Could not create new channel for report URI %s",
                     reportURICstring.get()));
      continue; // don't return yet, there may be more URIs
    }

    // log a warning to console if scheme is not http or https
    bool isHttpScheme =
      (NS_SUCCEEDED(reportURI->SchemeIs("http", &isHttpScheme)) && isHttpScheme) ||
      (NS_SUCCEEDED(reportURI->SchemeIs("https", &isHttpScheme)) && isHttpScheme);

    if (!isHttpScheme) {
      const char16_t* params[] = { reportURIs[r].get() };
      logToConsole(u"reportURInotHttpsOrHttp2", params, ArrayLength(params),
                   aViolationEventInit.mSourceFile, aViolationEventInit.mSample,
                   aViolationEventInit.mLineNumber, aViolationEventInit.mColumnNumber, 
                   nsIScriptError::errorFlag);
      continue;
    }

    // make sure this is an anonymous request (no cookies) so in case the
    // policy URI is injected, it can't be abused for CSRF.
    nsLoadFlags flags;
    rv = reportChannel->GetLoadFlags(&flags);
    NS_ENSURE_SUCCESS(rv, rv);
    flags |= nsIRequest::LOAD_ANONYMOUS;
    rv = reportChannel->SetLoadFlags(flags);
    NS_ENSURE_SUCCESS(rv, rv);

    // we need to set an nsIChannelEventSink on the channel object
    // so we can tell it to not follow redirects when posting the reports
    RefPtr<CSPReportRedirectSink> reportSink = new CSPReportRedirectSink();
    if (doc && doc->GetDocShell()) {
      nsCOMPtr<nsINetworkInterceptController> interceptController =
        do_QueryInterface(doc->GetDocShell());
      reportSink->SetInterceptController(interceptController);
    }
    reportChannel->SetNotificationCallbacks(reportSink);

    // apply the loadgroup from the channel taken by setRequestContext.  If
    // there's no loadgroup, AsyncOpen will fail on process-split necko (since
    // the channel cannot query the iTabChild).
    rv = reportChannel->SetLoadGroup(mCallingChannelLoadGroup);
    NS_ENSURE_SUCCESS(rv, rv);

    // wire in the string input stream to send the report
    nsCOMPtr<nsIStringInputStream> sis(do_CreateInstance(NS_STRINGINPUTSTREAM_CONTRACTID));
    NS_ASSERTION(sis, "nsIStringInputStream is needed but not available to send CSP violation reports");
    nsAutoCString utf8CSPReport = NS_ConvertUTF16toUTF8(csp_report);
    rv = sis->SetData(utf8CSPReport.get(), utf8CSPReport.Length());
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIUploadChannel> uploadChannel(do_QueryInterface(reportChannel));
    if (!uploadChannel) {
      // It's possible the URI provided can't be uploaded to, in which case
      // we skip this one. We'll already have warned about a non-HTTP URI earlier.
      continue;
    }

    rv = uploadChannel->SetUploadStream(sis, NS_LITERAL_CSTRING("application/csp-report"), -1);
    NS_ENSURE_SUCCESS(rv, rv);

    // if this is an HTTP channel, set the request method to post
    nsCOMPtr<nsIHttpChannel> httpChannel(do_QueryInterface(reportChannel));
    if (httpChannel) {
      httpChannel->SetRequestMethod(NS_LITERAL_CSTRING("POST"));
    }

    RefPtr<CSPViolationReportListener> listener = new CSPViolationReportListener();
    rv = reportChannel->AsyncOpen2(listener);

    // AsyncOpen should not fail, but could if there's no load group (like if
    // SetRequestContext is not given a channel).  This should fail quietly and
    // not return an error since it's really ok if reports don't go out, but
    // it's good to log the error locally.

    if (NS_FAILED(rv)) {
      const char16_t* params[] = { reportURIs[r].get() };
      CSPCONTEXTLOG(("AsyncOpen failed for report URI %s", params[0]));
      logToConsole(u"triedToSendReport", params, ArrayLength(params),
                   aViolationEventInit.mSourceFile, aViolationEventInit.mSample,
                   aViolationEventInit.mLineNumber, aViolationEventInit.mColumnNumber, 
                   nsIScriptError::errorFlag);
    } else {
      CSPCONTEXTLOG(("Sent violation report to URI %s", reportURICstring.get()));
    }
  }
  return NS_OK;
}

nsresult
nsCSPContext::FireViolationEvent(
  const mozilla::dom::SecurityPolicyViolationEventInit& aViolationEventInit)
{
  nsCOMPtr<nsIDocument> doc = do_QueryReferent(mLoadingContext);
  if (!doc) {
    return NS_OK;
  }

  RefPtr<mozilla::dom::Event> event =
    mozilla::dom::SecurityPolicyViolationEvent::Constructor(
      doc,
      NS_LITERAL_STRING("securitypolicyviolation"),
      aViolationEventInit);
  event->SetTrusted(true);

  bool rv;
  return doc->DispatchEvent(event, &rv);
}

/**
 * Dispatched from the main thread to send reports for one CSP violation.
 */
class CSPReportSenderRunnable final : public Runnable
{
  public:
    CSPReportSenderRunnable(nsISupports* aBlockedContentSource,
                            nsIURI* aOriginalURI,
                            uint32_t aViolatedPolicyIndex,
                            bool aReportOnlyFlag,
                            const nsAString& aViolatedDirective,
                            const nsAString& aObserverSubject,
                            const nsAString& aSourceFile,
                            const nsAString& aScriptSample,
                            uint32_t aLineNum,
                            uint32_t aColumnNum,
                            nsCSPContext* aCSPContext)
      : mBlockedContentSource(aBlockedContentSource)
      , mOriginalURI(aOriginalURI)
      , mViolatedPolicyIndex(aViolatedPolicyIndex)
      , mReportOnlyFlag(aReportOnlyFlag)
      , mViolatedDirective(aViolatedDirective)
      , mSourceFile(aSourceFile)
      , mScriptSample(aScriptSample)
      , mLineNum(aLineNum)
      , mColumnNum(aColumnNum)
      , mCSPContext(aCSPContext)
    {
      NS_ASSERTION(!aViolatedDirective.IsEmpty(), "Can not send reports without a violated directive");
      // the observer subject is an nsISupports: either an nsISupportsCString
      // from the arg passed in directly, or if that's empty, it's the blocked
      // source.
      if (aObserverSubject.IsEmpty()) {
        mObserverSubject = aBlockedContentSource;
      } else {
        nsCOMPtr<nsISupportsCString> supportscstr =
          do_CreateInstance(NS_SUPPORTS_CSTRING_CONTRACTID);
        NS_ASSERTION(supportscstr, "Couldn't allocate nsISupportsCString");
        supportscstr->SetData(NS_ConvertUTF16toUTF8(aObserverSubject));
        mObserverSubject = do_QueryInterface(supportscstr);
      }
    }

    NS_IMETHOD Run() override
    {
      MOZ_ASSERT(NS_IsMainThread());

      nsresult rv;

      // 0) prepare violation data
      mozilla::dom::SecurityPolicyViolationEventInit init;
      // mBlockedContentSource could be a URI or a string.
      nsCOMPtr<nsIURI> blockedURI = do_QueryInterface(mBlockedContentSource);
      // if mBlockedContentSource is not a URI, it could be a string
      nsCOMPtr<nsISupportsCString> blockedICString = do_QueryInterface(mBlockedContentSource);
      nsAutoCString blockedDataStr;
      if (blockedICString) {
        blockedICString->GetData(blockedDataStr);
      }
      rv = mCSPContext->GatherSecurityPolicyViolationEventData(
        blockedURI, blockedDataStr, mOriginalURI,
        mViolatedDirective, mViolatedPolicyIndex,
        mSourceFile, mScriptSample, mLineNum,
        mColumnNum, init);
      NS_ENSURE_SUCCESS(rv, rv);
     
      // 1) notify observers
      nsCOMPtr<nsIObserverService> observerService = mozilla::services::GetObserverService();
      NS_ASSERTION(observerService, "needs observer service");
      rv = observerService->NotifyObservers(mObserverSubject,
                                            CSP_VIOLATION_TOPIC,
                                            mViolatedDirective.get());
      NS_ENSURE_SUCCESS(rv, rv);

      // 2) send reports for the policy that was violated
      mCSPContext->SendReports(init, mViolatedPolicyIndex);

      // 3) log to console (one per policy violation)
      // if mBlockedContentSource is not a URI, it could be a string
      nsCOMPtr<nsISupportsCString> blockedString = do_QueryInterface(mBlockedContentSource);

      if (blockedURI) {
        blockedURI->GetSpec(blockedDataStr);
        if (blockedDataStr.Length() > nsCSPContext::ScriptSampleMaxLength()) {
          bool isData = false;
          rv = blockedURI->SchemeIs("data", &isData);
          if (NS_SUCCEEDED(rv) && isData &&
              blockedDataStr.Length() > nsCSPContext::ScriptSampleMaxLength()) {
            blockedDataStr.Truncate(nsCSPContext::ScriptSampleMaxLength());
            blockedDataStr.Append(NS_ConvertUTF16toUTF8(nsContentUtils::GetLocalizedEllipsis()));
          }
        }
      } else if (blockedString) {
        blockedString->GetData(blockedDataStr);
      }

      if (blockedDataStr.Length() > 0) {
        nsString blockedDataChar16 = NS_ConvertUTF8toUTF16(blockedDataStr);
        const char16_t* params[] = { mViolatedDirective.get(),
                                     blockedDataChar16.get() };
        mCSPContext->logToConsole(mReportOnlyFlag ? u"CSPROViolationWithURI" :
                                                    u"CSPViolationWithURI",
                                  params, ArrayLength(params), mSourceFile, mScriptSample,
                                  mLineNum, mColumnNum, nsIScriptError::errorFlag);
      }

      // 4) fire violation event
      mCSPContext->FireViolationEvent(init);

      return NS_OK;
    }

  private:
    nsCOMPtr<nsISupports>   mBlockedContentSource;
    nsCOMPtr<nsIURI>        mOriginalURI;
    uint32_t                mViolatedPolicyIndex;
    bool                    mReportOnlyFlag;
    nsString                mViolatedDirective;
    nsCOMPtr<nsISupports>   mObserverSubject;
    nsString                mSourceFile;
    nsString                mScriptSample;
    uint32_t                mLineNum;
    uint32_t                mColumnNum;
    RefPtr<nsCSPContext>    mCSPContext;
};

/**
 * Asynchronously notifies any nsIObservers listening to the CSP violation
 * topic that a violation occurred.  Also triggers report sending and console
 * logging.  All asynchronous on the main thread.
 *
 * @param aBlockedContentSource
 *        Either a CSP Source (like 'self', as string) or nsIURI: the source
 *        of the violation.
 * @param aOriginalUri
 *        The original URI if the blocked content is a redirect, else null
 * @param aViolatedDirective
 *        the directive that was violated (string).
 * @param aViolatedPolicyIndex
 *        the index of the policy that was violated (so we know where to send
 *        the reports).
 * @param aObserverSubject
 *        optional, subject sent to the nsIObservers listening to the CSP
 *        violation topic.
 * @param aSourceFile
 *        name of the file containing the inline script violation
 * @param aScriptSample
 *        a sample of the violating inline script
 * @param aLineNum
 *        source line number of the violation (if available)
 * @param aColumnNum
 *        source column number of the violation (if available)
 */
nsresult
nsCSPContext::AsyncReportViolation(nsISupports* aBlockedContentSource,
                                   nsIURI* aOriginalURI,
                                   const nsAString& aViolatedDirective,
                                   uint32_t aViolatedPolicyIndex,
                                   const nsAString& aObserverSubject,
                                   const nsAString& aSourceFile,
                                   const nsAString& aScriptSample,
                                   uint32_t aLineNum,
                                   uint32_t aColumnNum)
{
  NS_ENSURE_ARG_MAX(aViolatedPolicyIndex, mPolicies.Length() - 1);

  NS_DispatchToMainThread(new CSPReportSenderRunnable(aBlockedContentSource,
                                                      aOriginalURI,
                                                      aViolatedPolicyIndex,
                                                      mPolicies[aViolatedPolicyIndex]->getReportOnlyFlag(),
                                                      aViolatedDirective,
                                                      aObserverSubject,
                                                      aSourceFile,
                                                      aScriptSample,
                                                      aLineNum,
                                                      aColumnNum,
                                                      this));
   return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::RequireSRIForType(nsContentPolicyType aContentType, bool* outRequiresSRIForType)
{
  *outRequiresSRIForType = false;
  for (uint32_t i = 0; i < mPolicies.Length(); i++) {
    if (mPolicies[i]->hasDirective(REQUIRE_SRI_FOR)) {
      if (mPolicies[i]->requireSRIForType(aContentType)) {
        *outRequiresSRIForType = true;
        return NS_OK;
      }
    }
  }
  return NS_OK;
}

/**
 * Based on the given docshell, determines if this CSP context allows the
 * ancestry.
 *
 * In order to determine the URI of the parent document (one causing the load
 * of this protected document), this function obtains the docShellTreeItem,
 * then walks up the hierarchy until it finds a privileged (chrome) tree item.
 * Getting the a tree item's URI looks like this in pseudocode:
 *
 * nsIDocShellTreeItem->GetDocument()->GetDocumentURI();
 *
 * aDocShell is the docShell for the protected document.
 */
NS_IMETHODIMP
nsCSPContext::PermitsAncestry(nsIDocShell* aDocShell, bool* outPermitsAncestry)
{
  nsresult rv;

  // Can't check ancestry without a docShell.
  if (aDocShell == nullptr) {
    return NS_ERROR_FAILURE;
  }

  *outPermitsAncestry = true;

  // extract the ancestry as an array
  nsCOMArray<nsIURI> ancestorsArray;

  nsCOMPtr<nsIInterfaceRequestor> ir(do_QueryInterface(aDocShell));
  nsCOMPtr<nsIDocShellTreeItem> treeItem(do_GetInterface(ir));
  nsCOMPtr<nsIDocShellTreeItem> parentTreeItem;
  nsCOMPtr<nsIURI> currentURI;
  nsCOMPtr<nsIURI> uriClone;

  // iterate through each docShell parent item
  while (NS_SUCCEEDED(treeItem->GetParent(getter_AddRefs(parentTreeItem))) &&
         parentTreeItem != nullptr) {

    nsIDocument* doc = parentTreeItem->GetDocument();
    NS_ASSERTION(doc, "Could not get nsIDocument from nsIDocShellTreeItem in nsCSPContext::PermitsAncestry");
    NS_ENSURE_TRUE(doc, NS_ERROR_FAILURE);

    currentURI = doc->GetDocumentURI();

    if (currentURI) {
      // stop when reaching chrome
      bool isChrome = false;
      rv = currentURI->SchemeIs("chrome", &isChrome);
      NS_ENSURE_SUCCESS(rv, rv);
      if (isChrome) { break; }

      // delete the userpass from the URI.
      rv = currentURI->CloneIgnoringRef(getter_AddRefs(uriClone));
      NS_ENSURE_SUCCESS(rv, rv);

      // We don't care if this succeeds, just want to delete a userpass if
      // there was one.
      uriClone->SetUserPass(EmptyCString());

      if (CSPCONTEXTLOGENABLED()) {
        CSPCONTEXTLOG(("nsCSPContext::PermitsAncestry, found ancestor: %s",
                       uriClone->GetSpecOrDefault().get()));
      }
      ancestorsArray.AppendElement(uriClone);
    }

    // next ancestor
    treeItem = parentTreeItem;
  }

  nsAutoString violatedDirective;

  // Now that we've got the ancestry chain in ancestorsArray, time to check
  // them against any CSP.
  // NOTE:  the ancestors are not allowed to be sent cross origin; this is a
  // restriction not placed on subresource loads.

  for (uint32_t a = 0; a < ancestorsArray.Length(); a++) {
    if (CSPCONTEXTLOGENABLED()) {
      CSPCONTEXTLOG(("nsCSPContext::PermitsAncestry, checking ancestor: %s",
                     ancestorsArray[a]->GetSpecOrDefault().get()));
    }
    // omit the ancestor URI in violation reports if cross-origin as per spec
    // (it is a violation of the same-origin policy).
    bool okToSendAncestor = NS_SecurityCompareURIs(ancestorsArray[a], mSelfURI, true);


    bool permits = permitsInternal(nsIContentSecurityPolicy::FRAME_ANCESTORS_DIRECTIVE,
                                   ancestorsArray[a],
                                   nullptr, // no redirect here.
                                   EmptyString(), // no nonce
                                   false,   // no redirect here.
                                   false,   // not a preload.
                                   true,    // specific, do not use default-src
                                   true,    // send violation reports
                                   okToSendAncestor,
                                   false);  // not parser created
    if (!permits) {
      *outPermitsAncestry = false;
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::Permits(nsIURI* aURI,
                      CSPDirective aDir,
                      bool aSpecific,
                      bool aSendViolationReports,
                      bool* outPermits)
{
  // Can't perform check without aURI
  if (aURI == nullptr) {
    return NS_ERROR_FAILURE;
  }

  *outPermits = permitsInternal(aDir,
                                aURI,
                                nullptr,  // no original (pre-redirect) URI
                                EmptyString(),  // no nonce
                                false,    // not redirected.
                                false,    // not a preload.
                                aSpecific,
                                aSendViolationReports,
                                true,     // send blocked URI in violation reports
                                false);   // not parser created

  if (CSPCONTEXTLOGENABLED()) {
      CSPCONTEXTLOG(("nsCSPContext::Permits, aUri: %s, aDir: %s, isAllowed: %s",
                     aURI->GetSpecOrDefault().get(), CSP_CSPDirectiveToString(aDir),
                     *outPermits ? "allow" : "deny"));
  }

  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::ToJSON(nsAString& outCSPinJSON)
{
  outCSPinJSON.Truncate();
  dom::CSPPolicies jsonPolicies;
  jsonPolicies.mCsp_policies.Construct();

  for (uint32_t p = 0; p < mPolicies.Length(); p++) {
    dom::CSP jsonCSP;
    mPolicies[p]->toDomCSPStruct(jsonCSP);
    jsonPolicies.mCsp_policies.Value().AppendElement(jsonCSP, fallible);
  }

  // convert the gathered information to JSON
  if (!jsonPolicies.ToJSON(outCSPinJSON)) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::GetCSPSandboxFlags(uint32_t* aOutSandboxFlags)
{
  if (!aOutSandboxFlags) {
    return NS_ERROR_FAILURE;
  }
  *aOutSandboxFlags = SANDBOXED_NONE;

  for (uint32_t i = 0; i < mPolicies.Length(); i++) {
    uint32_t flags = mPolicies[i]->getSandboxFlags();

    // current policy doesn't have sandbox flag, check next policy
    if (!flags) {
      continue;
    }

    // current policy has sandbox flags, if the policy is in enforcement-mode
    // (i.e. not report-only) set these flags and check for policies with more
    // restrictions
    if (!mPolicies[i]->getReportOnlyFlag()) {
      *aOutSandboxFlags |= flags;
    } else {
      // sandbox directive is ignored in report-only mode, warn about it and
      // continue the loop checking for an enforcement policy.
      nsAutoString policy;
      mPolicies[i]->toString(policy);

      CSPCONTEXTLOG(("nsCSPContext::GetCSPSandboxFlags, report only policy, ignoring sandbox in: %s",
                    policy.get()));

      const char16_t* params[] = { policy.get() };
      logToConsole(u"ignoringReportOnlyDirective", params, ArrayLength(params),
                   EmptyString(), EmptyString(), 0, 0, nsIScriptError::warningFlag);
    }
  }

  return NS_OK;
}

/* ========== CSPViolationReportListener implementation ========== */

NS_IMPL_ISUPPORTS(CSPViolationReportListener, nsIStreamListener, nsIRequestObserver, nsISupports);

CSPViolationReportListener::CSPViolationReportListener()
{
}

CSPViolationReportListener::~CSPViolationReportListener()
{
}

nsresult
AppendSegmentToString(nsIInputStream* aInputStream,
                      void* aClosure,
                      const char* aRawSegment,
                      uint32_t aToOffset,
                      uint32_t aCount,
                      uint32_t* outWrittenCount)
{
  nsCString* decodedData = static_cast<nsCString*>(aClosure);
  decodedData->Append(aRawSegment, aCount);
  *outWrittenCount = aCount;
  return NS_OK;
}

NS_IMETHODIMP
CSPViolationReportListener::OnDataAvailable(nsIRequest* aRequest,
                                            nsISupports* aContext,
                                            nsIInputStream* aInputStream,
                                            uint64_t aOffset,
                                            uint32_t aCount)
{
  uint32_t read;
  nsCString decodedData;
  return aInputStream->ReadSegments(AppendSegmentToString,
                                    &decodedData,
                                    aCount,
                                    &read);
}

NS_IMETHODIMP
CSPViolationReportListener::OnStopRequest(nsIRequest* aRequest,
                                          nsISupports* aContext,
                                          nsresult aStatus)
{
  return NS_OK;
}

NS_IMETHODIMP
CSPViolationReportListener::OnStartRequest(nsIRequest* aRequest,
                                           nsISupports* aContext)
{
  return NS_OK;
}

/* ========== CSPReportRedirectSink implementation ========== */

NS_IMPL_ISUPPORTS(CSPReportRedirectSink, nsIChannelEventSink, nsIInterfaceRequestor);

CSPReportRedirectSink::CSPReportRedirectSink()
{
}

CSPReportRedirectSink::~CSPReportRedirectSink()
{
}

NS_IMETHODIMP
CSPReportRedirectSink::AsyncOnChannelRedirect(nsIChannel* aOldChannel,
                                              nsIChannel* aNewChannel,
                                              uint32_t aRedirFlags,
                                              nsIAsyncVerifyRedirectCallback* aCallback)
{
  // cancel the old channel so XHR failure callback happens
  nsresult rv = aOldChannel->Cancel(NS_ERROR_ABORT);
  NS_ENSURE_SUCCESS(rv, rv);

  // notify an observer that we have blocked the report POST due to a redirect,
  // used in testing, do this async since we're in an async call now to begin with
  nsCOMPtr<nsIURI> uri;
  rv = aOldChannel->GetURI(getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIObserverService> observerService = mozilla::services::GetObserverService();
  NS_ASSERTION(observerService, "Observer service required to log CSP violations");
  observerService->NotifyObservers(uri,
                                   CSP_VIOLATION_TOPIC,
                                   u"denied redirect while sending violation report");

  return NS_BINDING_REDIRECTED;
}

NS_IMETHODIMP
CSPReportRedirectSink::GetInterface(const nsIID& aIID, void** aResult)
{
  if (aIID.Equals(NS_GET_IID(nsINetworkInterceptController)) &&
      mInterceptController) {
    nsCOMPtr<nsINetworkInterceptController> copy(mInterceptController);
    *aResult = copy.forget().take();

    return NS_OK;
  }

  return QueryInterface(aIID, aResult);
}

void
CSPReportRedirectSink::SetInterceptController(nsINetworkInterceptController* aInterceptController)
{
  mInterceptController = aInterceptController;
}

/* ===== nsISerializable implementation ====== */

NS_IMETHODIMP
nsCSPContext::Read(nsIObjectInputStream* aStream)
{
  nsresult rv;
  nsCOMPtr<nsISupports> supports;

  rv = NS_ReadOptionalObject(aStream, true, getter_AddRefs(supports));
  NS_ENSURE_SUCCESS(rv, rv);

  mSelfURI = do_QueryInterface(supports);
  NS_ASSERTION(mSelfURI, "need a self URI to de-serialize");

  uint32_t numPolicies;
  rv = aStream->Read32(&numPolicies);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoString policyString;

  while (numPolicies > 0) {
    numPolicies--;

    rv = aStream->ReadString(policyString);
    NS_ENSURE_SUCCESS(rv, rv);

    bool reportOnly = false;
    rv = aStream->ReadBoolean(&reportOnly);
    NS_ENSURE_SUCCESS(rv, rv);

    // @param deliveredViaMetaTag:
    // when parsing the CSP policy string initially we already remove directives
    // that should not be processed when delivered via the meta tag. Such directives
    // will not be present at this point anymore.
    nsCSPPolicy* policy = nsCSPParser::parseContentSecurityPolicy(policyString,
                                                                  mSelfURI,
                                                                  reportOnly,
                                                                  this,
                                                                  false);
    if (policy) {
      mPolicies.AppendElement(policy);
    }
  }

  return NS_OK;
}

NS_IMETHODIMP
nsCSPContext::Write(nsIObjectOutputStream* aStream)
{
  nsresult rv = NS_WriteOptionalCompoundObject(aStream,
                                               mSelfURI,
                                               NS_GET_IID(nsIURI),
                                               true);
  NS_ENSURE_SUCCESS(rv, rv);

  // Serialize all the policies.
  aStream->Write32(mPolicies.Length());

  nsAutoString polStr;
  for (uint32_t p = 0; p < mPolicies.Length(); p++) {
    polStr.Truncate();
    mPolicies[p]->toString(polStr);
    aStream->WriteWStringZ(polStr.get());
    aStream->WriteBoolean(mPolicies[p]->getReportOnlyFlag());
  }
  return NS_OK;
}
