/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var EXPORTED_SYMBOLS = ["CookieBannerParent"];

const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "serviceMode",
  "cookiebanners.service.mode",
  Ci.nsICookieBannerService.MODE_DISABLED
);
XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "serviceModePBM",
  "cookiebanners.service.mode.privateBrowsing",
  Ci.nsICookieBannerService.MODE_DISABLED
);

class CookieBannerParent extends JSWindowActorParent {
  #isPrivateBrowsing() {
    let topBC = this.browsingContext.top;

    // Not all embedders are browsers. Skip other elements we can't do an
    // isBrowserPrivate check on.
    if (topBC.embedderElementType != "browser" || !topBC.embedderElement) {
      return false;
    }
    return lazy.PrivateBrowsingUtils.isBrowserPrivate(topBC.embedderElement);
  }

  async receiveMessage(message) {
    if (message.name == "CookieBanner::Test-FinishClicking") {
      Services.obs.notifyObservers(
        null,
        "cookie-banner-test-clicking-finish",
        this.manager.documentPrincipal?.baseDomain
      );
      return undefined;
    }

    if (message.name != "CookieBanner::GetClickRules") {
      return undefined;
    }

    // TODO: Bug 1790688: consider moving this logic to the cookie banner service.
    let mode;
    let isPrivateBrowsing = this.#isPrivateBrowsing();
    if (isPrivateBrowsing) {
      mode = lazy.serviceModePBM;
    } else {
      mode = lazy.serviceMode;
    }

    // Check if we have a site preference of the top-level URI. If so, it
    // takes precedence over the pref setting.
    let topBrowsingContext = this.manager.browsingContext.top;
    let topURI = topBrowsingContext.currentWindowGlobal?.documentURI;

    // We don't need to check the domain preference if the cookie banner
    // handling was disabled by pref.
    if (mode != Ci.nsICookieBannerService.MODE_DISABLED && topURI) {
      try {
        let perDomainMode = Services.cookieBanners.getDomainPref(
          topURI,
          isPrivateBrowsing
        );

        if (perDomainMode != Ci.nsICookieBannerService.MODE_UNSET) {
          mode = perDomainMode;
        }
      } catch (e) {
        // getPerSitePref could throw with NS_ERROR_NOT_AVAILABLE if the service
        // is disabled. We will fallback to global pref setting if any errors
        // occur.
        if (e.result == Cr.NS_ERROR_NOT_AVAILABLE) {
          Cu.reportError("The cookie banner handling service is not available");
        } else {
          Cu.reportError("Fail on getting domain pref:" + e);
        }
      }
    }

    // Service is disabled for current context (normal or private browsing),
    // return empty array.
    if (mode == Ci.nsICookieBannerService.MODE_DISABLED) {
      return [];
    }

    let domain = this.manager.documentPrincipal?.baseDomain;

    if (!domain) {
      return [];
    }

    let rules = Services.cookieBanners.getClickRulesForDomain(domain);

    if (!rules.length) {
      return [];
    }

    let modeIsRejectOrAccept =
      mode == Ci.nsICookieBannerService.MODE_REJECT_OR_ACCEPT;
    return rules.map(rule => {
      let target = rule.optOut;

      if (modeIsRejectOrAccept && !target) {
        target = rule.optIn;
      }
      return {
        hide: rule.hide ?? rule.presence,
        presence: rule.presence,
        target,
      };
    });
  }
}
