/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The equality comparisons of js/Equality.h, but with extra efficiency for
 * SpiderMonkey-internal callers.
 *
 * These functions, assuming they're passed C++-valid arguments, are identical
 * to the same-named JS::-namespaced functions -- just with hidden linkage (so
 * they're more efficient to call), and without various external-caller-focused
 * JSAPI-usage assertions performed that SpiderMonkey users never come close to
 * failing.
 */

#ifndef vm_EqualityOperations_h
#define vm_EqualityOperations_h

#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value

struct JSContext;

namespace js {

/** Computes |lval === rval|. */
extern bool
StrictlyEqual(JSContext* cx, JS::HandleValue lval, JS::HandleValue rval, bool* equal);

/** Computes |lval == rval|. */
extern bool
LooselyEqual(JSContext* cx, JS::HandleValue lval, JS::HandleValue rval, bool* result);

/**
 * Computes |SameValue(v1, v2)| -- strict equality except that NaNs are
 * considered equal and opposite-signed zeroes are considered unequal.
 */
extern bool
SameValue(JSContext* cx, JS::HandleValue v1, JS::HandleValue v2, bool* same);

}  // namespace js

#endif  // vm_EqualityOperations_h
