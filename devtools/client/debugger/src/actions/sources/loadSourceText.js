/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import { PROMISE } from "../utils/middleware/promise";
import {
  getSource,
  getSourceTextContent,
  getSourceContent,
  getGeneratedSource,
  getSourcesEpoch,
  getBreakpointsForSource,
  getSourceActorsForSource,
} from "../../selectors";
import { addBreakpoint } from "../breakpoints";

import { prettyPrintSource } from "./prettyPrint";
import { isFulfilled, fulfilled } from "../../utils/async-value";

import { isPretty } from "../../utils/source";
import { memoizeableAction } from "../../utils/memoizableAction";

const Telemetry = require("devtools/client/shared/telemetry");

// Measures the time it takes for a source to load
const loadSourceHistogram = "DEVTOOLS_DEBUGGER_LOAD_SOURCE_MS";
const telemetry = new Telemetry();

async function loadSource(
  state,
  source,
  sourceActor,
  { dispatch, sourceMaps, client, getState }
) {
  if (isPretty(source) && source.isOriginal) {
    const generatedSource = getGeneratedSource(state, source);
    if (!generatedSource) {
      throw new Error("Unable to find minified original.");
    }
    const content = getSourceContent(state, generatedSource.id);
    if (!content || !isFulfilled(content)) {
      throw new Error("Cannot pretty-print a file that has not loaded");
    }

    return prettyPrintSource(
      sourceMaps,
      generatedSource,
      content.value,
      getSourceActorsForSource(state, generatedSource.id)
    );
  }

  if (source.isOriginal) {
    const result = await sourceMaps.getOriginalSourceText(source.id);
    if (!result) {
      // The way we currently try to load and select a pending
      // selected location, it is possible that we will try to fetch the
      // original source text right after the source map has been cleared
      // after a navigation event.
      throw new Error("Original source text unavailable");
    }
    return result;
  }

  // If no source actor can be found then the text for the
  // source cannot be loaded.
  if (!sourceActor) {
    throw new Error("Unknown source");
  }

  let response;
  try {
    telemetry.start(loadSourceHistogram, source);
    response = await client.sourceContents(sourceActor);
    telemetry.finish(loadSourceHistogram, source);
  } catch (e) {
    throw new Error(`sourceContents failed: ${e}`);
  }

  return {
    actorId: sourceActor.actor,
    text: response.source,
    contentType: response.contentType || "text/javascript",
  };
}

async function loadSourceTextPromise(
  cx,
  source,
  sourceActor,
  { dispatch, getState, client, sourceMaps, parser }
) {
  const epoch = getSourcesEpoch(getState());

  await dispatch({
    type: "LOAD_SOURCE_TEXT",
    sourceId: source.id,
    epoch,
    [PROMISE]: loadSource(getState(), source, sourceActor, {
      sourceMaps,
      client,
      getState,
    }),
  });

  const newSource = getSource(getState(), source.id);

  if (!newSource) {
    return;
  }
  const content = getSourceContent(getState(), newSource.id);

  if (!newSource.isWasm && content) {
    parser.setSource(
      newSource.id,
      isFulfilled(content)
        ? content.value
        : { type: "text", value: "", contentType: undefined }
    );

    // Update the text in any breakpoints for this source by re-adding them.
    const breakpoints = getBreakpointsForSource(getState(), source.id);
    for (const { location, options, disabled } of breakpoints) {
      await dispatch(addBreakpoint(cx, location, options, disabled));
    }
  }
}

/**
 * Loads the source text for a source and source actor
 * @param {Object} source
 *                 The source to load the source text
 * @param {Object|null} sourceActor
 *                 There can be more than one source actor per source
 *                 so the source actor needs to be specified. This is
 *                 required for generated sources but will be null for
 *                 original/pretty printed sources.
 */
export const loadSourceText = memoizeableAction("loadSourceText", {
  getValue: ({ source, sourceActor }, { getState }) => {
    if (!source) {
      return null;
    }

    const sourceTextContent = getSourceTextContent(getState(), source.id);
    if (!sourceTextContent || sourceTextContent.state === "pending") {
      return sourceTextContent;
    }

    // if a source actor is specified,lets also check that the source actor for the
    // currently loaded text.
    if (sourceActor && sourceTextContent && isFulfilled(sourceTextContent)) {
      if (
        sourceTextContent.value.actorId &&
        sourceTextContent.value.actorId !== sourceActor?.actor
      ) {
        return null;
      }
    }

    // This currently swallows source-load-failure since we return fulfilled
    // here when content.state === "rejected". In an ideal world we should
    // propagate that error upward.
    return fulfilled(source);
  },
  createKey: ({ source }, { getState }) => {
    const epoch = getSourcesEpoch(getState());
    return `${epoch}:${source.id}`;
  },
  action: ({ cx, source, sourceActor }, thunkArgs) =>
    loadSourceTextPromise(cx, source, sourceActor, thunkArgs),
});
