/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/DocGroup.h"
#include "mozilla/dom/HTMLSlotElement.h"
#include "mozilla/dom/HTMLSlotElementBinding.h"
#include "mozilla/dom/HTMLUnknownElement.h"
#include "mozilla/dom/ShadowRoot.h"
#include "nsGkAtoms.h"
#include "nsDocument.h"
#include "nsLayoutUtils.h"

nsGenericHTMLElement*
NS_NewHTMLSlotElement(already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo,
                      mozilla::dom::FromParser aFromParser)
{
  RefPtr<mozilla::dom::NodeInfo> nodeInfo(aNodeInfo);
 if (nsDocument::IsWebComponentsEnabled(nodeInfo->GetDocument())) {
    already_AddRefed<mozilla::dom::NodeInfo> nodeInfoArg(nodeInfo.forget());
    return new mozilla::dom::HTMLSlotElement(nodeInfoArg);
  }

  already_AddRefed<mozilla::dom::NodeInfo> nodeInfoArg(nodeInfo.forget());
  return new mozilla::dom::HTMLUnknownElement(nodeInfoArg);
}

namespace mozilla {
namespace dom {

HTMLSlotElement::HTMLSlotElement(already_AddRefed<mozilla::dom::NodeInfo>& aNodeInfo)
  : nsGenericHTMLElement(aNodeInfo)
{
}

HTMLSlotElement::~HTMLSlotElement()
{
}

NS_IMPL_ADDREF_INHERITED(HTMLSlotElement, nsGenericHTMLElement)
NS_IMPL_RELEASE_INHERITED(HTMLSlotElement, nsGenericHTMLElement)

NS_IMPL_CYCLE_COLLECTION_INHERITED(HTMLSlotElement,
                                   nsGenericHTMLElement,
                                   mAssignedNodes)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(HTMLSlotElement)
NS_INTERFACE_MAP_END_INHERITING(nsGenericHTMLElement)

NS_IMPL_ELEMENT_CLONE(HTMLSlotElement)

nsresult
HTMLSlotElement::BindToTree(nsIDocument* aDocument,
                            nsIContent* aParent,
                            nsIContent* aBindingParent,
                            bool aCompileEventHandlers)
{
  RefPtr<ShadowRoot> oldContainingShadow = GetContainingShadow();

  nsresult rv = nsGenericHTMLElement::BindToTree(aDocument, aParent,
                                                 aBindingParent,
                                                 aCompileEventHandlers);
  NS_ENSURE_SUCCESS(rv, rv);

  ShadowRoot* containingShadow = GetContainingShadow();
  if (containingShadow && !oldContainingShadow) {
    containingShadow->AddSlot(this);
  }

  return NS_OK;
}

void
HTMLSlotElement::UnbindFromTree(bool aDeep, bool aNullParent)
{
  RefPtr<ShadowRoot> oldContainingShadow = GetContainingShadow();

  nsGenericHTMLElement::UnbindFromTree(aDeep, aNullParent);

  if (oldContainingShadow && !GetContainingShadow()) {
    oldContainingShadow->RemoveSlot(this);
  }
}

nsresult
HTMLSlotElement::BeforeSetAttr(int32_t aNameSpaceID, nsIAtom* aName,
                               const nsAttrValueOrString* aValue,
                               bool aNotify)
{
  if (aNameSpaceID == kNameSpaceID_None && aName == nsGkAtoms::name) {
    if (ShadowRoot* containingShadow = GetContainingShadow()) {
      containingShadow->RemoveSlot(this);
    }
  }

  return nsGenericHTMLElement::BeforeSetAttr(aNameSpaceID, aName, aValue,
                                             aNotify);
}

nsresult
HTMLSlotElement::AfterSetAttr(int32_t aNameSpaceID, nsIAtom* aName,
                              const nsAttrValue* aValue,
                              const nsAttrValue* aOldValue,
                              nsIPrincipal* aSubjectPrincipal,
                              bool aNotify)
{

  if (aNameSpaceID == kNameSpaceID_None && aName == nsGkAtoms::name) {
    if (ShadowRoot* containingShadow = GetContainingShadow()) {
      containingShadow->AddSlot(this);
    }
  }

  if (nsIContent* parent = GetParent()) {
    if (parent->IsElement()) {
      nsLayoutUtils::PostRestyleEvent(
        parent->AsElement(), eRestyle_Subtree, nsChangeHint(0));
    }
  }

  return nsGenericHTMLElement::AfterSetAttr(aNameSpaceID, aName, aValue,
                                            aOldValue, aSubjectPrincipal,
                                            aNotify);
}

/**
 * Flatten assigned nodes given a slot, as in:
 * https://dom.spec.whatwg.org/#find-flattened-slotables
 */
static void
FlattenAssignedNodes(HTMLSlotElement* aSlot, nsTArray<RefPtr<nsINode>>& aNodes)
{
  if (!aSlot->GetContainingShadow()) {
    return;
  }

  const nsTArray<RefPtr<nsINode>>& assignedNodes = aSlot->AssignedNodes();

  // If assignedNodes is empty, use children of slot as fallback content.
  if (assignedNodes.IsEmpty()) {
    for (nsIContent* child = aSlot->AsContent()->GetFirstChild();
         child;
         child = child->GetNextSibling()) {
      if (!child->IsSlotable()) {
        continue;
      }

      if (child->IsHTMLElement(nsGkAtoms::slot)) {
        FlattenAssignedNodes(HTMLSlotElement::FromContent(child), aNodes);
      } else {
        aNodes.AppendElement(child);
      }
    }
    return;
  }

  for (uint32_t i = 0; i < assignedNodes.Length(); i++) {
    nsINode* assignedNode = assignedNodes[i];
    if (assignedNode->IsHTMLElement(nsGkAtoms::slot)) {
      FlattenAssignedNodes(
        HTMLSlotElement::FromContent(assignedNode->AsContent()), aNodes);
    } else {
      aNodes.AppendElement(assignedNode);
    }
  }
}

void
HTMLSlotElement::AssignedNodes(const AssignedNodesOptions& aOptions,
                               nsTArray<RefPtr<nsINode>>& aNodes)
{
  if (aOptions.mFlatten) {
    return FlattenAssignedNodes(this, aNodes);
  }

  aNodes = mAssignedNodes;
}

void HTMLSlotElement::AssignedElements(const AssignedNodesOptions& aOptions,
                                       nsTArray<RefPtr<Element>>& aElements) {
  AutoTArray<RefPtr<nsINode>, 128> assignedNodes;
  AssignedNodes(aOptions, assignedNodes);
  for (const RefPtr<nsINode>& assignedNode : assignedNodes) {
    if (assignedNode->IsElement()) {
      aElements.AppendElement(assignedNode->AsElement());
    }
  }
}

const nsTArray<RefPtr<nsINode>>&
HTMLSlotElement::AssignedNodes() const
{
  return mAssignedNodes;
}

void
HTMLSlotElement::InsertAssignedNode(uint32_t aIndex, nsINode* aNode)
{
  mAssignedNodes.InsertElementAt(aIndex, aNode);
  aNode->AsContent()->SetAssignedSlot(this);
}

void
HTMLSlotElement::AppendAssignedNode(nsINode* aNode)
{
  mAssignedNodes.AppendElement(aNode);
  aNode->AsContent()->SetAssignedSlot(this);
}

void
HTMLSlotElement::RemoveAssignedNode(nsINode* aNode)
{
  mAssignedNodes.RemoveElement(aNode);
  aNode->AsContent()->SetAssignedSlot(nullptr);
}

void
HTMLSlotElement::ClearAssignedNodes()
{
  for (uint32_t i = 0; i < mAssignedNodes.Length(); i++) {
    mAssignedNodes[i]->AsContent()->SetAssignedSlot(nullptr);
  }

  mAssignedNodes.Clear();
}

void
HTMLSlotElement::EnqueueSlotChangeEvent() const
{
  DocGroup* docGroup = OwnerDoc()->GetDocGroup();
  if (!docGroup) {
    return;
  }

  docGroup->SignalSlotChange(this);
}

void
HTMLSlotElement::FireSlotChangeEvent()
{
  nsContentUtils::DispatchTrustedEvent(OwnerDoc(),
                                       static_cast<nsIContent*>(this),
                                       NS_LITERAL_STRING("slotchange"), true,
                                       false);
}

JSObject*
HTMLSlotElement::WrapNode(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return HTMLSlotElementBinding::Wrap(aCx, this, aGivenProto);
}

} // namespace dom
} // namespace mozilla
