/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Create a RegExp from a given wildcard string.
 */
export function wildcardToRegExp(s) {
  return new RegExp(
    s
      .split("*")
      .map(regExpEscape)
      .join(".*"),
    "i"
  );
}

/**
 * Escapes all special RegExp characters in the given string.
 */
const regExpEscape = s => {
  return s.replace(/[|\\{}()[\]^$+*?.]/g, "\\$&");
};
