/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://drafts.fxtf.org/geometry/
 *
 * Copyright © 2012 W3C® (MIT, ERCIM, Keio), All Rights Reserved. W3C
 * liability, trademark and document use rules apply.
 */

[Constructor(optional unrestricted double x = 0, optional unrestricted double y = 0,
             optional unrestricted double width = 0, optional unrestricted double height = 0),
 Exposed=(Window,Worker)]
interface DOMRect : DOMRectReadOnly {
    [NewObject] static DOMRect fromRect(optional DOMRectInit other);

    inherit attribute unrestricted double x;
    inherit attribute unrestricted double y;
    inherit attribute unrestricted double width;
    inherit attribute unrestricted double height;
};

[Constructor(optional unrestricted double x = 0, optional unrestricted double y = 0,
             optional unrestricted double width = 0, optional unrestricted double height = 0),
 Exposed=(Window,Worker)]
interface DOMRectReadOnly {
    [NewObject] static DOMRectReadOnly fromRect(optional DOMRectInit other);

    readonly attribute unrestricted double x;
    readonly attribute unrestricted double y;
    readonly attribute unrestricted double width;
    readonly attribute unrestricted double height;
    readonly attribute unrestricted double top;
    readonly attribute unrestricted double right;
    readonly attribute unrestricted double bottom;
    readonly attribute unrestricted double left;
    
    jsonifier;
};

dictionary DOMRectInit {
    unrestricted double x = 0;
    unrestricted double y = 0;
    unrestricted double width = 0;
    unrestricted double height = 0;
};
