/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

// The purpose of that test is to reproduce edge case behaviors that one can
// have while running whole ipc/glue/test/browser/ suite but that could this
// way be intermittent and hard to diagnose. By having such a test we make sure
// it is cleanly reproduced and wont regress somewhat silently.

"use strict";

async function runTest(src, expectation) {
  info(`Add media tabs: ${src}`);
  let tab = await addMediaTab(src);

  info("Play tab");
  await play(tab, expectation);

  info("Stop tab");
  await stop(tab);

  info("Remove tab");
  await BrowserTestUtils.removeTab(tab);
}

async function findGenericAudioDecoder() {
  const audioDecoders = (await ChromeUtils.requestProcInfo()).children.filter(
    p => {
      return (
        p.type === "utility" &&
        p.utilityActors.find(a => a.actorName === "audioDecoder_Generic")
      );
    }
  );
  ok(audioDecoders.length === 1, "Only one audio decoder present");
  return audioDecoders[0].pid;
}

add_setup(async function setup() {
  await SpecialPowers.pushPrefEnv({
    set: [["media.utility-process.enabled", true]],
  });
});

add_task(async function testKill() {
  await runTest("small-shot.ogg", "Utility Generic");

  const audioDecoderPid = await findGenericAudioDecoder();
  ok(audioDecoderPid > 0, `Valid PID found: ${audioDecoderPid}`);

  await cleanUtilityProcessShutdown(audioDecoderPid, /* preferKill */ true);

  info("Waiting 15s to trigger mShutdownBlockers assertions");
  await new Promise((resolve, reject) => {
    /* eslint-disable mozilla/no-arbitrary-setTimeout */
    setTimeout(resolve, 15 * 1000);
  });

  ok(true, "Waited 15s to trigger mShutdownBlockers assertions: over");
});

add_task(async function testShutdown() {
  await runTest("small-shot.ogg", "Utility Generic");

  const audioDecoderPid = await findGenericAudioDecoder();
  ok(audioDecoderPid > 0, `Valid PID found: ${audioDecoderPid}`);

  await cleanUtilityProcessShutdown(audioDecoderPid);

  info("Waiting 15s to trigger mShutdownBlockers assertions");
  await new Promise((resolve, reject) => {
    /* eslint-disable mozilla/no-arbitrary-setTimeout */
    setTimeout(resolve, 15 * 1000);
  });

  ok(true, "Waited 15s to trigger mShutdownBlockers assertions: over");
});
