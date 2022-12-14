"use strict";

const GOOD_COOKIE = "GoodCookie=OMNOMNOM";
const SPACEY_COOKIE = "Spacey Cookie=Major Tom";

add_task(async () => {
  Services.prefs.setBoolPref(
    "network.cookieJarSettings.unblocked_for_testing",
    true
  );
  Services.prefs.setBoolPref("dom.security.https_first", false);

  var cookieURI = Services.io.newURI(
    "http://mozilla.org/test_cookie_blacklist.js"
  );
  const channel = NetUtil.newChannel({
    uri: cookieURI,
    loadUsingSystemPrincipal: true,
    contentPolicyType: Ci.nsIContentPolicy.TYPE_DOCUMENT,
  });

  Services.cookies.setCookieStringFromHttp(
    cookieURI,
    "BadCookie1=\x01",
    channel
  );
  Services.cookies.setCookieStringFromHttp(cookieURI, "BadCookie2=\v", channel);
  Services.cookies.setCookieStringFromHttp(
    cookieURI,
    "Bad\x07Name=illegal",
    channel
  );
  Services.cookies.setCookieStringFromHttp(cookieURI, GOOD_COOKIE, channel);
  Services.cookies.setCookieStringFromHttp(cookieURI, SPACEY_COOKIE, channel);

  CookieXPCShellUtils.createServer({ hosts: ["mozilla.org"] });

  const storedCookie = await CookieXPCShellUtils.getCookieStringFromDocument(
    cookieURI.spec
  );
  Assert.equal(storedCookie, GOOD_COOKIE + "; " + SPACEY_COOKIE);
  Services.prefs.clearUserPref("dom.security.https_first");
});
