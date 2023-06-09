/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * For more information on this interface, please see
 * http://www.whatwg.org/specs/web-apps/current-work/#channel-messaging
 */

[Exposed=(Window,Worker,System)]
interface MessagePort : EventTarget {
  [Throws]
  void postMessage(any message, sequence<object> transferable);
  [Throws]
  void postMessage(any message, optional StructuredSerializeOptions options);

  void start();
  void close();

  // event handlers
  attribute EventHandler onmessage;
};
// MessagePort implements Transferable;

// Used to declare which objects should be transferred.
dictionary StructuredSerializeOptions {
  sequence<object> transfer = [];
};