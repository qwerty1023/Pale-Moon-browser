/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MenuBoxObject_h
#define mozilla_dom_MenuBoxObject_h

#include "mozilla/dom/BoxObject.h"

namespace mozilla {
namespace dom {

class KeyboardEvent;

class MenuBoxObject final : public BoxObject
{
public:

  MenuBoxObject();

  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

  void OpenMenu(bool aOpenFlag);
  already_AddRefed<Element> GetActiveChild();
  void SetActiveChild(Element* arg);
  bool HandleKeyPress(KeyboardEvent& keyEvent);
  bool OpenedWithKey();

private:
  ~MenuBoxObject();
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_MenuBoxObject_h
