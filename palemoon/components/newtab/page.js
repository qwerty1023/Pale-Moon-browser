#ifdef 0
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#endif

// The amount of time we wait while coalescing updates for hidden pages.
const SCHEDULE_UPDATE_TIMEOUT_MS = 1000;

/**
 * This singleton represents the whole 'New Tab Page' and takes care of
 * initializing all its components.
 */
var gPage = {
  /**
   * Initializes the page.
   */
  init: function() {
    // Add ourselves to the list of pages to receive notifications.
    gAllPages.register(this);

    // Listen for 'unload' to unregister this page.
    addEventListener("unload", this, false);
    
    // Listen for toggle button clicks.
    let button = document.getElementById("newtab-toggle");
    button.addEventListener("click", e => this.toggleEnabled(e));

    // XXX bug 991111 - Not all click events are correctly triggered when
    // listening from xhtml nodes -- in particular middle clicks on sites, so
    // listen from the xul window and filter then delegate
    addEventListener("click", this, false);

    // Check if the new tab feature is enabled.
    let enabled = gAllPages.enabled;
    if (enabled)
      this._init();

    this._updateAttributes(enabled);
  },

  /**
   * Listens for notifications specific to this page.
   */
  observe: function(aSubject, aTopic, aData) {
    if (aTopic == "nsPref:changed") {
      let enabled = gAllPages.enabled;
      this._updateAttributes(enabled);

      // Initialize the whole page if we haven't done that, yet.
      if (enabled) {
        this._init();
      } else {
        gUndoDialog.hide();
      }
    } else if (aTopic == "page-thumbnail:create" && gGrid.ready) {
      for (let site of gGrid.sites) {
        if (site && site.url === aData) {
          site.refreshThumbnail();
        }
      }
    }
  },

  /**
   * Updates the page's grid right away for visible pages. If the page is
   * currently hidden, i.e. in a background tab or in the preloader, then we
   * batch multiple update requests and refresh the grid once after a short
   * delay. Accepts a single parameter the specifies the reason for requesting
   * a page update. The page may decide to delay or prevent a requested updated
   * based on the given reason.
   */
  update(reason = "") {
    // Update immediately if we're visible.
    if (!document.hidden) {
      // Ignore updates where reason=links-changed as those signal that the
      // provider's set of links changed. We don't want to update visible pages
      // in that case, it is ok to wait until the user opens the next tab.
      if (reason != "links-changed" && gGrid.ready) {
        gGrid.refresh();
      }

      return;
    }

    // Bail out if we scheduled before.
    if (this._scheduleUpdateTimeout) {
      return;
    }

    this._scheduleUpdateTimeout = setTimeout(() => {
      // Refresh if the grid is ready.
      if (gGrid.ready) {
        gGrid.refresh();
      }

      this._scheduleUpdateTimeout = null;
    }, SCHEDULE_UPDATE_TIMEOUT_MS);
  },

  /**
   * Internally initializes the page. This runs only when/if the feature
   * is/gets enabled.
   */
  _init: function() {
    if (this._initialized)
      return;

    this._initialized = true;

    // XXX: This comment makes no sense for what it does. There is no HC check,
    // and it changes the button unconditionally to a "play" button, which is
    // wrong for a search action. Commented out for now.
    
    // Set submit button label for when CSS background are disabled (e.g.
    // high contrast mode).
    //document.getElementById("searchSubmit").value =
    //  document.body.getAttribute("dir") == "ltr" ? "\u25B6" : "\u25C0";

    if (document.hidden) {
      addEventListener("visibilitychange", this);
    } else {
      setTimeout(() => this.onPageFirstVisible());
    }

    // Initialize and render the grid.
    gGrid.init();

    // Initialize the drop target shim.
    gDropTargetShim.init();

#ifdef XP_MACOSX
    // Workaround to prevent a delay on MacOSX due to a slow drop animation.
    document.addEventListener("dragover", this, false);
    document.addEventListener("drop", this, false);
#endif
  },

  /**
   * Updates the 'page-disabled' attributes of the respective DOM nodes.
   * @param aValue Whether the New Tab Page is enabled or not.
   */
  _updateAttributes: function(aValue) {
    // Set the nodes' states.
    let nodeSelector = "#newtab-grid, #searchContainer";
    for (let node of document.querySelectorAll(nodeSelector)) {
      if (aValue)
        node.removeAttribute("page-disabled");
      else
        node.setAttribute("page-disabled", "true");
    }

    // Enables/disables the control and link elements.
    let inputSelector = ".newtab-control, .newtab-link";
    for (let input of document.querySelectorAll(inputSelector)) {
      if (aValue)
        input.removeAttribute("tabindex");
      else
        input.setAttribute("tabindex", "-1");
    }
  },

  /**
   * Handles unload event
   */
  _handleUnloadEvent: function() {
    gAllPages.unregister(this);
  },

  /**
   * Handles all page events.
   */
  handleEvent: function(aEvent) {
    switch (aEvent.type) {
      case "load":
        this.onPageVisibleAndLoaded();
        break;
      case "unload":
        this._handleUnloadEvent();
        break;
      case "click":
        let {button, target} = aEvent;
        // Go up ancestors until we find a Site or not
        while (target) {
          if (target.hasOwnProperty("_newtabSite")) {
            target._newtabSite.onClick(aEvent);
            break;
          }
          target = target.parentNode;
        }
        break;
      case "dragover":
        if (gDrag.isValid(aEvent) && gDrag.draggedSite)
          aEvent.preventDefault();
        break;
      case "drop":
        if (gDrag.isValid(aEvent) && gDrag.draggedSite) {
          aEvent.preventDefault();
          aEvent.stopPropagation();
        }
        break;
      case "visibilitychange":
        // Cancel any delayed updates for hidden pages now that we're visible.
        if (this._scheduleUpdateTimeout) {
          clearTimeout(this._scheduleUpdateTimeout);
          this._scheduleUpdateTimeout = null;

          // An update was pending so force an update now.
          this.update();
        }

        setTimeout(() => this.onPageFirstVisible());
        removeEventListener("visibilitychange", this);
        break;
    }
  },

  onPageFirstVisible: function() {
    for (let site of gGrid.sites) {
      if (site) {
        // The site may need to modify and/or re-render itself if
        // something changed after newtab was created by preloader.
        // For example, the suggested tile endTime may have passed.
        site.onFirstVisible();
      }
    }

    // save timestamp to compute page life-span delta
    this._firstVisibleTime = Date.now();

    if (document.readyState == "complete") {
      this.onPageVisibleAndLoaded();
    } else {
      addEventListener("load", this);
    }
  },

  onPageVisibleAndLoaded() {
  },

  toggleEnabled: function(aEvent) {
    gAllPages.enabled = !gAllPages.enabled;
    aEvent.stopPropagation();
  }
};
