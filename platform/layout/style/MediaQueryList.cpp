/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* implements DOM interface for querying and observing media queries */

#include "mozilla/dom/MediaQueryList.h"
#include "mozilla/dom/MediaQueryListEvent.h"
#include "mozilla/dom/EventTarget.h"
#include "mozilla/dom/EventTargetBinding.h"
#include "nsPresContext.h"
#include "nsIMediaList.h"
#include "nsCSSParser.h"
#include "nsIDocument.h"

// Fixed event target type
#define ONCHANGE_STRING NS_LITERAL_STRING("change")

namespace mozilla {
namespace dom {

MediaQueryList::MediaQueryList(nsIDocument *aDocument,
                               const nsAString &aMediaQueryList)
  : mDocument(aDocument)
  , mMediaList(new nsMediaList)
  , mMatchesValid(false)
  , mIsKeptAlive(false)
{
  PR_INIT_CLIST(this);

  nsCSSParser parser;
  parser.ParseMediaList(aMediaQueryList, nullptr, 0, mMediaList, false);
}

MediaQueryList::~MediaQueryList()
{
  if (mDocument) {
    PR_REMOVE_LINK(this);
  }
}

NS_IMPL_CYCLE_COLLECTION_CLASS(MediaQueryList)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(MediaQueryList, DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDocument)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(MediaQueryList, DOMEventTargetHelper)
  if (tmp->mDocument) {
    PR_REMOVE_LINK(tmp);
    NS_IMPL_CYCLE_COLLECTION_UNLINK(mDocument)
  }
  tmp->Disconnect();
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MediaQueryList)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(MediaQueryList, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(MediaQueryList, DOMEventTargetHelper)

void
MediaQueryList::GetMedia(nsAString &aMedia)
{
  mMediaList->GetText(aMedia);
}

bool
MediaQueryList::Matches()
{
  if (!mMatchesValid) {
    MOZ_ASSERT(!HasListeners(),
               "when listeners present, must keep mMatches current");
    RecomputeMatches();
  }

  return mMatches;
}

void
MediaQueryList::AddListener(EventListener* aListener, ErrorResult& aRv)
{
  if (!aListener) {
    return;
  }

  AddEventListenerOptionsOrBoolean options;
  options.SetAsBoolean() = false;

  AddEventListener(ONCHANGE_STRING, aListener, options, Nullable<bool>(), aRv);
}

void
MediaQueryList::AddEventListener(const nsAString& aType,
                                 EventListener* aCallback,
                                 const AddEventListenerOptionsOrBoolean& aOptions,
                                 const dom::Nullable<bool>& aWantsUntrusted,
                                 ErrorResult& aRv)
{
  if (!mMatchesValid) {
    MOZ_ASSERT(!HasListeners(),
               "when listeners present, must keep mMatches current");
    RecomputeMatches();
  }

  DOMEventTargetHelper::AddEventListener(aType, aCallback, aOptions,
                                         aWantsUntrusted, aRv);

  if (aRv.Failed()) {
    return;
  }

  UpdateMustKeepAlive();
}

void
MediaQueryList::RemoveListener(EventListener* aListener, ErrorResult& aRv)
{
  if (!aListener) {
    return;
  }

  EventListenerOptionsOrBoolean options;
  options.SetAsBoolean() = false;

  RemoveEventListener(ONCHANGE_STRING, aListener, options, aRv);
}

void
MediaQueryList::RemoveEventListener(const nsAString& aType,
                                    EventListener* aCallback,
                                    const EventListenerOptionsOrBoolean& aOptions,
                                    ErrorResult& aRv)
{
  DOMEventTargetHelper::RemoveEventListener(aType, aCallback, aOptions, aRv);

  if (aRv.Failed()) {
    return;
  }

  UpdateMustKeepAlive();
}

EventHandlerNonNull*
MediaQueryList::GetOnchange()
{
  if (NS_IsMainThread()) {
    return GetEventHandler(nsGkAtoms::onchange, EmptyString());
  }
  return GetEventHandler(nullptr, ONCHANGE_STRING);
}

void
MediaQueryList::SetOnchange(EventHandlerNonNull* aCallback)
{
  if (NS_IsMainThread()) {
    SetEventHandler(nsGkAtoms::onchange, EmptyString(), aCallback);
  } else {
    SetEventHandler(nullptr, ONCHANGE_STRING, aCallback);
  }

  UpdateMustKeepAlive();
}

void
MediaQueryList::UpdateMustKeepAlive()
{
  bool toKeepAlive = HasListeners();
  if (toKeepAlive == mIsKeptAlive) {
    return;
  }

  // When we have listeners, the pres context owns a reference to
  // this.  This is a cyclic reference that can only be broken by
  // cycle collection.

  mIsKeptAlive = toKeepAlive;

  if (toKeepAlive) {
    NS_ADDREF_THIS();
  } else {
    NS_RELEASE_THIS();
  }
}

bool
MediaQueryList::HasListeners()
{
  return HasListenersFor(ONCHANGE_STRING);
}

void
MediaQueryList::Disconnect()
{
  DisconnectFromOwner();

  if (mIsKeptAlive) {
    mIsKeptAlive = false;
    // See NS_ADDREF_THIS() in AddListener.
    NS_RELEASE_THIS();
  }
}

void
MediaQueryList::RecomputeMatches()
{
  if (!mDocument) {
    return;
  }

  if (mDocument->GetParentDocument()) {
    // Flush frames on the parent so our prescontext will get
    // recreated as needed.
    mDocument->GetParentDocument()->FlushPendingNotifications(Flush_Frames);
    // That might have killed our document, so recheck that.
    if (!mDocument) {
      return;
    }
  }

  nsIPresShell* shell = mDocument->GetShell();
  if (!shell) {
    // XXXbz What's the right behavior here?  Spec doesn't say.
    return;
  }

  nsPresContext* presContext = shell->GetPresContext();
  if (!presContext) {
    // XXXbz What's the right behavior here?  Spec doesn't say.
    return;
  }

  mMatches = mMediaList->Matches(presContext, nullptr);
  mMatchesValid = true;
}

nsISupports*
MediaQueryList::GetParentObject() const
{
  return mDocument;
}

JSObject*
MediaQueryList::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return MediaQueryListBinding::Wrap(aCx, this, aGivenProto);
}

void
MediaQueryList::MaybeNotify()
{
  mMatchesValid = false;

  if (!HasListeners()) {
    return;
  }

  bool oldMatches = mMatches;
  RecomputeMatches();

  // No need to notify the change.
  if (mMatches == oldMatches) {
    return;
  }

  MediaQueryListEventInit init;
  init.mBubbles = false;
  init.mCancelable = false;
  init.mMatches = mMatches;
  mMediaList->GetText(init.mMedia);

  RefPtr<MediaQueryListEvent> event =
    MediaQueryListEvent::Constructor(this, ONCHANGE_STRING, init);
  event->SetTrusted(true);

  bool dummy;
  DispatchEvent(event, &dummy);
}

} // namespace dom
} // namespace mozilla
