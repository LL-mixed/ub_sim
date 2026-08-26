#!/usr/bin/env node

import { spawn } from "node:child_process";
import { readFile, stat, writeFile } from "node:fs/promises";
import path from "node:path";

const repoRoot = path.resolve(import.meta.dirname, "../../..");
const webRoot = path.join(repoRoot, "crates/sim-console/web");
const chrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const fixturePage = "/tmp/sim-console-visual-fixture.html";
const desktop = "/tmp/sim-console-desktop.png";
const mobile = "/tmp/sim-console-mobile.png";

const demo = {
  id: "w5-deepseek-v4-flash-8node",
  title: "W5 DeepSeek V4 Flash PP",
  summary: "Eight-node W5 pipeline inference through Memory Service and GSVA.",
  category: "W5 inference",
  topology: "pipeline",
  node_count: 8,
  model: "DeepSeek V4 Flash",
  estimated_duration_secs: 480,
  tags: ["w5", "deepseek", "pipeline"],
  data_plane: ["Memory Service", "GSVA", "OBMM"],
  requirements: ["QEMU", "openEuler image", "2-bit GGUF"],
  controls: ["stop"],
  parameters: [
    {
      id: "steps",
      label: "Decode steps",
      kind: "integer",
      default: "8",
      min: 1,
      max: 64,
    },
  ],
};

const nodes = Array.from({ length: 8 }, (_, index) => ({
  id: `node${String.fromCharCode(65 + index)}`,
  label: `Node ${String.fromCharCode(65 + index)}`,
  status: index < 5 ? "passed" : index === 5 ? "running" : "ready",
  log_path: `/work/logs/visual-run/node${String.fromCharCode(65 + index)}_guest.log`,
}));

const run = {
  id: "visual-run",
  demo_id: demo.id,
  demo_title: demo.title,
  target_id: "n4-910c1",
  source_revision: "15951308ea8fa1fbce600d434a5b8cf72e132f14",
  status: "running",
  created_at_ms: Date.now() - 94_000,
  started_at_ms: Date.now() - 92_000,
  finished_at_ms: null,
  nodes,
};

const responses = {
  "/api/v1/health": { status: "ready" },
  "/api/v1/catalog": { version: 1, demos: [demo] },
  "/api/v1/targets": {
    version: 1,
    default_target: "n4-910c1",
    targets: [
      {
        id: "n4-910c1",
        title: "n4-910c1",
        kind: "ssh",
        description: "Native openEuler simulator testbed.",
        ssh_host: "n4-910c1",
        repo_root: "/home/ll/ub_sim",
      },
    ],
  },
  "/api/v1/readiness": [
    { demo_id: demo.id, target_id: "n4-910c1", ready: true, issues: [] },
  ],
  "/api/v1/runs": [run],
};

const mockApi = `
  <script>
    const responses = ${JSON.stringify(responses)};
    const logChunk = ${JSON.stringify({
      next_cursor: 512,
      lines: [
        "[sim-console] run=visual-run status=running",
        "[mem_service] ready data_plane_ready=1 provider=obmm",
        "[w5] nodeA..nodeH pipeline bootstrap complete",
        "[w5] prompt accepted model=deepseek-v4-flash steps=8",
        "[w5] decode step=4 handoff=nodeE gsva=attached",
      ],
    })};
    window.fetch = async (input) => {
      const url = String(input);
      const path = url.split("?")[0];
      const payload = path.includes("/logs") ? logChunk : responses[path];
      if (payload === undefined) {
        return new Response(JSON.stringify({ error: "fixture endpoint missing" }), {
          status: 404,
          headers: { "Content-Type": "application/json" },
        });
      }
      return new Response(JSON.stringify(payload), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    };
  </script>`;

const index = await readFile(path.join(webRoot, "index.html"), "utf8");
const fixture = index
  .replace('href="/styles.css"', `href="file://${path.join(webRoot, "styles.css")}"`)
  .replace(
    '<script src="/app.js" defer></script>',
    `${mockApi}<script src="file://${path.join(webRoot, "app.js")}" defer></script>`,
  );
await writeFile(fixturePage, fixture);

await screenshot(1440, 1000, desktop);
await screenshot(390, 844, mobile);
await assertNonEmpty(desktop);
await assertNonEmpty(mobile);
console.log(`desktop=${desktop}`);
console.log(`mobile=${mobile}`);

async function screenshot(width, height, output) {
  const browser = spawn(
    chrome,
    [
      "--headless=new",
      "--disable-gpu",
      "--hide-scrollbars",
      "--allow-file-access-from-files",
      "--remote-debugging-pipe",
      "about:blank",
    ],
    { stdio: ["ignore", "ignore", "inherit", "pipe", "pipe"] },
  );
  const protocol = connectDevTools(browser);
  try {
    const targets = await protocol.send("Target.getTargets");
    const page = targets.targetInfos.find((target) => target.type === "page");
    if (!page) throw new Error("Chrome did not create a page target");
    const attached = await protocol.send("Target.attachToTarget", {
      targetId: page.targetId,
      flatten: true,
    });
    const sessionId = attached.sessionId;
    await protocol.send(
      "Emulation.setDeviceMetricsOverride",
      {
        width,
        height,
        deviceScaleFactor: 1,
        mobile: width <= 480,
        screenWidth: width,
        screenHeight: height,
      },
      sessionId,
    );
    await protocol.send("Page.enable", {}, sessionId);
    await protocol.send("Page.navigate", { url: `file://${fixturePage}` }, sessionId);
    await delay(1800);
    await assertProcessLogRoundTrip(protocol, sessionId);

    const metrics = await protocol.send(
      "Runtime.evaluate",
      {
        expression:
          "({width: innerWidth, height: innerHeight, scrollWidth: document.documentElement.scrollWidth, logBandBottom: document.querySelector('.log-band').getBoundingClientRect().bottom, logOutputHeight: document.querySelector('.log-output').getBoundingClientRect().height, logActionsHeight: document.querySelector('.log-actions').getBoundingClientRect().height, logActionsRight: document.querySelector('.log-actions').getBoundingClientRect().right})",
        returnByValue: true,
      },
      sessionId,
    );
    const viewport = metrics.result.value;
    if (viewport.width !== width || viewport.scrollWidth > width) {
      throw new Error(
        `invalid viewport width=${viewport.width} scrollWidth=${viewport.scrollWidth} expected=${width}`,
      );
    }
    if (
      width > 820 &&
      (Math.abs(viewport.logBandBottom - viewport.height) > 1 ||
        viewport.logOutputHeight < 120 ||
        viewport.logActionsHeight < 20 ||
        viewport.logActionsRight > viewport.width)
    ) {
      throw new Error(
        `log area geometry is invalid bottom=${viewport.logBandBottom} height=${viewport.height} outputHeight=${viewport.logOutputHeight} actionsHeight=${viewport.logActionsHeight} actionsRight=${viewport.logActionsRight}`,
      );
    }

    const captured = await protocol.send(
      "Page.captureScreenshot",
      { format: "png", fromSurface: true, captureBeyondViewport: false },
      sessionId,
    );
    await writeFile(output, Buffer.from(captured.data, "base64"));
  } finally {
    await protocol.send("Browser.close").catch(() => {});
    await Promise.race([waitForExit(browser), delay(2000)]);
    if (browser.exitCode === null) browser.kill("SIGKILL");
  }
}

async function assertProcessLogRoundTrip(protocol, sessionId) {
  await protocol.send(
    "Runtime.evaluate",
    { expression: "document.querySelector('.node-tile').click()" },
    sessionId,
  );
  await delay(100);
  const nodeView = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({title: document.querySelector('#log-title').textContent, processSelected: document.querySelector('#process-log').getAttribute('aria-pressed'), selectedNodes: document.querySelectorAll('.node-tile.selected').length})",
      returnByValue: true,
    },
    sessionId,
  );
  if (
    nodeView.result.value.title !== "Node A log" ||
    nodeView.result.value.processSelected !== "false" ||
    nodeView.result.value.selectedNodes !== 1
  ) {
    throw new Error(`node log selection failed: ${JSON.stringify(nodeView.result.value)}`);
  }

  await protocol.send(
    "Runtime.evaluate",
    { expression: "document.querySelector('#process-log').click()" },
    sessionId,
  );
  await delay(100);
  const processView = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({title: document.querySelector('#log-title').textContent, processSelected: document.querySelector('#process-log').getAttribute('aria-pressed'), selectedNodes: document.querySelectorAll('.node-tile.selected').length})",
      returnByValue: true,
    },
    sessionId,
  );
  if (
    processView.result.value.title !== "Process log" ||
    processView.result.value.processSelected !== "true" ||
    processView.result.value.selectedNodes !== 0
  ) {
    throw new Error(`process log return failed: ${JSON.stringify(processView.result.value)}`);
  }
}

function connectDevTools(browser) {
  const input = browser.stdio[3];
  const output = browser.stdio[4];
  let buffer = "";
  let nextId = 0;
  const pending = new Map();

  output.setEncoding("utf8");
  output.on("data", (chunk) => {
    buffer += chunk;
    let separator;
    while ((separator = buffer.indexOf("\0")) >= 0) {
      const frame = buffer.slice(0, separator);
      buffer = buffer.slice(separator + 1);
      if (!frame) continue;
      const message = JSON.parse(frame);
      const request = pending.get(message.id);
      if (!request) continue;
      pending.delete(message.id);
      if (message.error) request.reject(new Error(message.error.message));
      else request.resolve(message.result || {});
    }
  });

  return {
    send(method, params = {}, sessionId = null) {
      const id = ++nextId;
      const message = { id, method, params };
      if (sessionId) message.sessionId = sessionId;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        input.write(`${JSON.stringify(message)}\0`);
      });
    },
  };
}

function waitForExit(child) {
  if (child.exitCode !== null) return Promise.resolve();
  return new Promise((resolve) => child.once("exit", resolve));
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function assertNonEmpty(file) {
  const metadata = await stat(file);
  if (metadata.size < 1024) {
    throw new Error(`screenshot is unexpectedly small: ${file}`);
  }
}
