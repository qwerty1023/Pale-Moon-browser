/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsStyledElement.h"
#include "mozAutoDocUpdate.h"
#include "nsGkAtoms.h"
#include "nsAttrValue.h"
#include "nsAttrValueInlines.h"
#include "mozilla/dom/ElementInlines.h"
#include "mozilla/InternalMutationEvent.h"
#include "nsDOMCSSDeclaration.h"
#include "nsDOMCSSAttrDeclaration.h"
#include "nsServiceManagerUtils.h"
#include "nsIDocument.h"
#include "nsCSSParser.h"
#include "mozilla/css/Loader.h"
#include "mozilla/css/Declaration.h"
#include "nsIDOMMutationEvent.h"
#include "nsXULElement.h"
#include "nsContentUtils.h"
#include "nsStyleUtil.h"

using namespace mozilla;
using namespace mozilla::dom;

// Using the CC variant of this, even though this class does not define
// a new CC participant, to make QIing to the CC interfaces faster.
NS_IMPL_QUERY_INTERFACE_CYCLE_COLLECTION_INHERITED(nsStyledElement,
                                                   nsStyledElementBase,
                                                   nsStyledElement)

//----------------------------------------------------------------------
// nsIContent methods

bool
nsStyledElement::ParseAttribute(int32_t aNamespaceID,
                                nsIAtom* aAttribute,
                                const nsAString& aValue,
                                nsAttrValue& aResult)
{
  if (aAttribute == nsGkAtoms::style && aNamespaceID == kNameSpaceID_None) {
    ParseStyleAttribute(aValue, aResult, false);
    return true;
  }

  return nsStyledElementBase::ParseAttribute(aNamespaceID, aAttribute, aValue,
                                             aResult);
}

nsresult
nsStyledElement::BeforeSetAttr(int32_t aNamespaceID, nsIAtom* aName,
                               const nsAttrValueOrString* aValue, bool aNotify)
{
  if (aNamespaceID == kNameSpaceID_None) {
    if (aName == nsGkAtoms::style) {
      if (aValue) {
        SetMayHaveStyle();
      }
    }
  }

  return nsStyledElementBase::BeforeSetAttr(aNamespaceID, aName, aValue,
                                            aNotify);
}

nsresult
nsStyledElement::SetInlineStyleDeclaration(css::Declaration* aDeclaration,
                                           const nsAString* aSerialized,
                                           bool aNotify)
{
  SetMayHaveStyle();
  bool modification = false;
  nsAttrValue oldValue;
  bool oldValueSet = false;

  bool hasListeners = aNotify &&
    nsContentUtils::HasMutationListeners(this,
                                         NS_EVENT_BITS_MUTATION_ATTRMODIFIED,
                                         this);

  // There's no point in comparing the stylerule pointers since we're always
  // getting a new stylerule here. And we can't compare the stringvalues of
  // the old and the new rules since both will point to the same declaration
  // and thus will be the same.
  if (hasListeners) {
    // save the old attribute so we can set up the mutation event properly
    // XXXbz if the old rule points to the same declaration as the new one,
    // this is getting the new attr value, not the old one....
    nsAutoString oldValueStr;
    modification = GetAttr(kNameSpaceID_None, nsGkAtoms::style,
                           oldValueStr);
    if (modification) {
      oldValue.SetTo(oldValueStr);
      oldValueSet = true;
    }
  }
  else if (aNotify && IsInUncomposedDoc()) {
    modification = !!mAttrsAndChildren.GetAttr(nsGkAtoms::style);
  }

  nsAttrValue attrValue(do_AddRef(aDeclaration), aSerialized);

  // XXXbz do we ever end up with ADDITION here?  I doubt it.
  uint8_t modType = modification ?
    static_cast<uint8_t>(nsIDOMMutationEvent::MODIFICATION) :
    static_cast<uint8_t>(nsIDOMMutationEvent::ADDITION);

  nsIDocument* document = GetComposedDoc();
  mozAutoDocUpdate updateBatch(document, UPDATE_CONTENT_MODEL, aNotify);
  return SetAttrAndNotify(kNameSpaceID_None, nsGkAtoms::style, nullptr,
                          oldValueSet ? &oldValue : nullptr, attrValue,
                          nullptr, modType,
                          hasListeners, aNotify, kDontCallAfterSetAttr,
                          document, updateBatch);
}

css::Declaration*
nsStyledElement::GetInlineStyleDeclaration()
{
  if (!MayHaveStyle()) {
    return nullptr;
  }
  const nsAttrValue* attrVal = mAttrsAndChildren.GetAttr(nsGkAtoms::style);

  if (attrVal && attrVal->Type() == nsAttrValue::eCSSDeclaration) {
    return attrVal->GetCSSDeclarationValue();
  }

  return nullptr;
}

// ---------------------------------------------------------------
// Others and helpers

nsICSSDeclaration*
nsStyledElement::Style()
{
  Element::nsDOMSlots *slots = DOMSlots();

  if (!slots->mStyle) {
    // Just in case...
    ReparseStyleAttribute(true);

    slots->mStyle = new nsDOMCSSAttributeDeclaration(this, false);
    SetMayHaveStyle();
  }

  return slots->mStyle;
}

nsresult
nsStyledElement::ReparseStyleAttribute(bool aForceInDataDoc)
{
  if (!MayHaveStyle()) {
    return NS_OK;
  }
  const nsAttrValue* oldVal = mAttrsAndChildren.GetAttr(nsGkAtoms::style);
  if (oldVal && oldVal->Type() != nsAttrValue::eCSSDeclaration) {
    nsAttrValue attrValue;
    nsAutoString stringValue;
    oldVal->ToString(stringValue);
    ParseStyleAttribute(stringValue, attrValue, aForceInDataDoc);
    // Don't bother going through SetInlineStyleDeclaration; we don't
    // want to fire off mutation events or document notifications anyway
    bool oldValueSet;
    nsresult rv = mAttrsAndChildren.SetAndSwapAttr(nsGkAtoms::style, attrValue,
                                                   &oldValueSet);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  
  return NS_OK;
}

nsICSSDeclaration*
nsStyledElement::GetExistingStyle()
{
  Element::nsDOMSlots* slots = GetExistingDOMSlots();
  if (!slots) {
    return nullptr;
  }

  return slots->mStyle;
}

void
nsStyledElement::ParseStyleAttribute(const nsAString& aValue,
                                     nsAttrValue& aResult,
                                     bool aForceInDataDoc)
{
  nsIDocument* doc = OwnerDoc();
  bool isNativeAnon = IsInNativeAnonymousSubtree();

  if (!isNativeAnon &&
      !nsStyleUtil::CSPAllowsInlineStyle(nullptr, NodePrincipal(),
                                         doc->GetDocumentURI(), 0, 0, aValue,
                                         nullptr))
    return;

  if (aForceInDataDoc ||
      !doc->IsLoadedAsData() ||
      GetExistingStyle() ||
      doc->IsStaticDocument()) {
    bool isCSS = true; // assume CSS until proven otherwise

    if (!isNativeAnon) {  // native anonymous content always assumes CSS
      nsAutoString styleType;
      doc->GetHeaderData(nsGkAtoms::headerContentStyleType, styleType);
      if (!styleType.IsEmpty()) {
        static const char textCssStr[] = "text/css";
        isCSS = (styleType.EqualsIgnoreCase(textCssStr, sizeof(textCssStr) - 1));
      }
    }

    if (isCSS && aResult.ParseStyleAttribute(aValue, this)) {
      return;
    }
  }

  aResult.SetTo(aValue);
}
