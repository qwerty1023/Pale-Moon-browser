/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Types and functions related to the compilation of JavaScript off the
 * direct JSAPI-using thread.
 */

#ifndef js_OffThreadScriptCompilation_h
#define js_OffThreadScriptCompilation_h

#include "mozilla/Range.h" // mozilla::Range
#include "mozilla/Vector.h" // mozilla::Vector

#include <stddef.h> // size_t

#include "jstypes.h" // JS_PUBLIC_API

#include "js/CompileOptions.h" // JS::ReadOnlyCompileOptions
#include "js/GCVector.h" // JS::GCVector
#include "js/Transcoding.h" // JS::TranscodeSource

struct JSContext;
class JSScript;

namespace JS {

class SourceBufferHolder;

} // namespace JS

namespace JS {

using OffThreadCompileCallback = void (*)(void* token, void* callbackData);

extern JS_PUBLIC_API(bool)
CanCompileOffThread(JSContext* cx, const ReadOnlyCompileOptions& options, size_t length);

/*
 * Off thread compilation control flow.
 *
 * After successfully triggering an off thread compile of a script, the
 * callback will eventually be invoked with the specified data and a token
 * for the compilation. The callback will be invoked while off the main thread,
 * so must ensure that its operations are thread safe. Afterwards, one of the
 * following functions must be invoked on the main thread:
 *
 * - FinishOffThreadScript, to get the result script (or nullptr on failure).
 * - CancelOffThreadScript, to free the resources without creating a script.
 *
 * The characters passed in to CompileOffThread must remain live until the
 * callback is invoked, and the resulting script will be rooted until the call
 * to FinishOffThreadScript.
 */

extern JS_PUBLIC_API(bool)
CompileOffThread(JSContext* cx, const ReadOnlyCompileOptions& options,
                 const char16_t* chars, size_t length,
                 OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API(JSScript*)
FinishOffThreadScript(JSContext* cx, void* token);

extern JS_PUBLIC_API(void)
CancelOffThreadScript(JSContext* cx, void* token);

extern JS_PUBLIC_API(bool)
CompileOffThreadModule(JSContext* cx, const ReadOnlyCompileOptions& options,
                       const char16_t* chars, size_t length,
                       OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API(JSObject*)
FinishOffThreadModule(JSContext* cx, void* token);

extern JS_PUBLIC_API(void)
CancelOffThreadModule(JSContext* cx, void* token);

extern JS_PUBLIC_API(bool)
DecodeOffThreadScript(JSContext* cx, const ReadOnlyCompileOptions& options,
                      mozilla::Vector<uint8_t>& buffer /* TranscodeBuffer& */, size_t cursor,
                      OffThreadCompileCallback callback, void* callbackData);

extern JS_PUBLIC_API(JSScript*)
FinishOffThreadScriptDecoder(JSContext* cx, void* token);

extern JS_PUBLIC_API(void)
CancelOffThreadScriptDecoder(JSContext* cx, void* token);

} // namespace JS

#endif /* js_OffThreadScriptCompilation_h */