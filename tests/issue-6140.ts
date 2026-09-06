// issue #6140: a markdown file whose name has a space ("Test Test.md") opened
// as a WebView2 404: Navigate() was given https://sumatrapdf.markdown/Test Test.html
// (invalid URI). WebView2 then asked for Test%20Test.html and showed
// "A webpage cannot be found on that web address".
//
// The page is served if we can scroll to a heading far down the document — a
// 404 page has nothing to scroll. Folder names with a space already worked
// (they never appear in the virtual URL); the file name is what is in the path.

import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand, withControlledSumatra } from "./control.ts";
import { EXE, runStandalone, SLOW_BUILD_FACTOR, tmpPath } from "./util.ts";

const MD_NAME = "Test Test.md";
const TARGET_DEST_NO = 3; // the file, "Start", then "Target Heading"
const MIN_TARGET_SCROLL_Y = 500;

async function tocNavigate(client: ControlClient, destNo: number, expected: string): Promise<string> {
  const deadline = Date.now() + 20_000 * SLOW_BUILD_FACTOR;
  let last = "";
  for (;;) {
    const res = await client.request(ControlCommand.TestMarkdownTocNavigate, [destNo, MIN_TARGET_SCROLL_Y]);
    const exitCode = res[0] as number;
    const output = String(res[1] ?? "").trim();
    last = output;
    if (exitCode === 0 && output.startsWith(expected)) {
      return output;
    }
    if (exitCode !== 2 || !output.startsWith("NOTREADY")) {
      throw new Error(`issue-6140: navigating a document named '${MD_NAME}' failed: ${output}`);
    }
    if (Date.now() > deadline) {
      throw new Error(`issue-6140: '${MD_NAME}' never rendered (404?): ${last}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
}

export async function testit(): Promise<void> {
  const dir = tmpPath("issue-6140-data");
  rmSync(dir, { recursive: true, force: true });
  // folder-with-space is the reporter's path; it is not the bug, but keep it
  const folder = join(dir, "test test");
  mkdirSync(folder, { recursive: true });
  const paragraphs = Array.from(
    { length: 120 },
    (_, i) => `Paragraph ${i + 1}: enough content to put the target heading below the viewport.`,
  );
  writeFileSync(join(folder, MD_NAME), ["# Start", ...paragraphs, "## Target Heading", "Target content."].join("\n\n"));

  const appdata = tmpPath("issue-6140-appdata");
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(
    join(appdata, "SumatraPDF-settings.txt"),
    ["MarkdownUI [", "\tUseFixedPageUI = false", "]", "RestoreSession = false", "ShowStartPage = false", ""].join("\n"),
  );

  await withControlledSumatra(
    EXE,
    async (client) => {
      const started = await tocNavigate(client, TARGET_DEST_NO, "NAVIGATING");
      if (!started.includes("target-heading")) {
        throw new Error(`issue-6140: dest ${TARGET_DEST_NO} is not the target heading: ${started}`);
      }
      const landed = await tocNavigate(client, 0, "OK");
      console.log(`issue-6140: '${MD_NAME}' ${landed}`);
    },
    ["-appdata", appdata, join(folder, MD_NAME)],
  );

  console.log("issue-6140: OK");
}

if (import.meta.main) {
  await runStandalone(testit);
}
