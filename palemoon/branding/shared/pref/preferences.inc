// ===| General |==============================================================

pref("startup.homepage_welcome_url","https://www.palemoon.org/firstrun.shtml");

pref("app.vendorURL", "https://www.palemoon.org/");


// User Interface
pref("browser.identity.ssl_domain_display", 1); //show domain verified SSL (blue)

// ============================================================================

// ===| Application Update Service |===========================================

// The time interval between the downloading of mar file chunks in the
// background (in seconds)
pref("app.update.download.backgroundInterval", 600);

// Give the user x seconds to react before showing the big UI. default=48 hours
pref("app.update.promptWaitTime", 172800);

// ============================================================================

// ===| Add-ons Manager |======================================================

// Add-on window fixes
pref("extensions.getMoreThemesURL", "https://addons.palemoon.org/themes/");

pref("extensions.update.autoUpdateDefault", true); // Automatically update extensions by default
pref("extensions.getAddons.maxResults", 10);
pref("extensions.getAddons.cache.enabled", false);

// ============================================================================

// ===| DOM |==================================================================

// ============================================================================

// ===| Plugins |==============================================================

pref("plugin.expose_full_path", true); //Security: expose the full path to the plugin
pref("dom.ipc.plugins.timeoutSecs", 20);

// ============================================================================

// ===| Graphics |=============================================================

pref("nglayout.initialpaint.delay", 300);

// ============================================================================

// ===| Image |================================================================

pref("image.mem.max_ms_before_yield", 50);
pref("image.mem.decode_bytes_at_a_time", 65536); //larger chunks

// ============================================================================

// ===| Sync |=================================================================

// Pale Moon Sync server URLs
pref("services.sync.serverURL","https://pmsync.palemoon.org/sync/index.php/");
pref("services.sync.jpake.serverURL","https://keyserver.palemoon.org/");
pref("services.sync.termsURL", "https://www.palemoon.org/sync/terms.shtml");
pref("services.sync.privacyURL", "https://www.palemoon.org/sync/privacy.shtml");
pref("services.sync.statusURL", "https://pmsync.palemoon.org/status/");
pref("services.sync.syncKeyHelpURL", "https://www.palemoon.org/sync/help/recoverykey.shtml");

pref("services.sync.APILevel", 1); // FSyncMS doesn't support 'info/configuration' requests

// ============================================================================

// ===| Misc. |================================================================

// Make sure we shortcut out of a11y to save walking unnecessary code
pref("accessibility.force_disabled", 1);

// Enable Firefox compatmode by default in official channels.
pref("general.useragent.compatMode", 2);
pref("general.useragent.compatMode.gecko", true);
pref("general.useragent.compatMode.firefox", true);
pref("general.useragent.compatMode.version", "128.0");

// Limit protocol fallback from TLS 1.3
pref("security.tls.version.fallback-limit", 4);

// ============================================================================

// ===| DevTools |=============================================================

// Number of usages of the web console or scratchpad.
// If this is less than 5, then pasting code into the web console or scratchpad is disabled
pref("devtools.selfxss.count", 100);

// ============================================================================
