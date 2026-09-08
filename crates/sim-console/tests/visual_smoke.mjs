#!/usr/bin/env node

import { spawn } from "node:child_process";
import { mkdtemp, readFile, rm, stat, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

const repoRoot = path.resolve(import.meta.dirname, "../../..");
const webRoot = path.join(repoRoot, "crates/sim-console/web");
const chrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const fixturePage = "/tmp/sim-console-visual-fixture.html";
const desktop = "/tmp/sim-console-desktop.png";
const mobile = "/tmp/sim-console-mobile.png";

const demo = {
  id: "urma-rpc-2",
  title: "URMA RPC / 2 Nodes",
  summary: "Dual-node RPC validation with retained interactive guest shells.",
  category: "URMA and RPC",
  topology: "pair",
  lifecycle: "interactive_shell",
  node_count: 2,
  model: null,
  estimated_duration_secs: 180,
  tags: ["rpc", "interactive-shell"],
  data_plane: ["URMA", "RPC"],
  requirements: ["QEMU", "guest kernel", "initramfs"],
  controls: ["stop", "node_input"],
  parameters: [
    {
      id: "run_secs",
      label: "Validation timeout",
      kind: "integer",
      default: "180",
      min: 1,
      max: 600,
    },
  ],
};

const nodes = Array.from({ length: 2 }, (_, index) => ({
  id: `node${String.fromCharCode(65 + index)}`,
  label: `Node ${String.fromCharCode(65 + index)}`,
  status: "passed",
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
        "[ub_rpc] nodeA request sent to nodeB",
        "[ub_rpc] nodeB response returned to nodeA",
        "iteration 1: dual-node apps pass",
        "interactive shells ready; use node input or Stop to terminate",
        "decode_output: token_ids=[9707, 11, 358]",
        "decode_output: token_pieces=\"Hello, I\"",
        "decode_token: step=0 node=nodeA token=9707 piece=\"Hello\" runner_up=123 margin_milli=456 text_checksum=0xabc",
        "decode_token: step=1 node=nodeB token=11 piece=\",\" runner_up=100 margin_milli=200 text_checksum=0xdef",
        "decode_token: step=2 node=nodeA token=358 piece=\" I\" runner_up=90 margin_milli=150 text_checksum=0x123",
        "timing_step: step=0 round_ms=120 critical_node=nodeA workers=2/2 max_worker_ms=100 avg_worker_ms=90 max_input_wait_ms=10 max_compute_window_ms=50 max_submit_ms=5 max_publish_ms=5 max_barrier_ms=0",
        "timing_step: step=1 round_ms=180 critical_node=nodeB workers=2/2 max_worker_ms=150 avg_worker_ms=140 max_input_wait_ms=20 max_compute_window_ms=60 max_submit_ms=6 max_publish_ms=4 max_barrier_ms=0",
        "timing_step: step=2 round_ms=140 critical_node=nodeA workers=2/2 max_worker_ms=120 avg_worker_ms=110 max_input_wait_ms=15 max_compute_window_ms=55 max_submit_ms=5 max_publish_ms=5 max_barrier_ms=0",
        "timing_bottleneck: slowest_step=1 round_ms=180 critical_node=nodeB",
        "summary: decode_steps_expected=3 decode_steps_observed=3 worker_timing_records=6 passed_nodes=2/2 handoff_timing_records=0 idle_timing_records=0",
        "engram_timing_step: records=3 steps=3/3 modes=copy max_latency_ms=9 max_latency_step=2 max_latency_node=nodeB",
        "w5_device_summary: records=2 tensor_consumers=2 devices=ub:0 backends=ub ops=copy nodes=nodeA,nodeB",
        "[1;34mbin[0m [32mready[0m <node-output>",
        "[38;5;208mindexed[0m [38;2;10;20;30mrgb[0m",
      ],
    })};
    window.__nodeInputRequests = [];
    window.__targetPreparationRequests = [];
    window.__blockTargetPreparation = () => {
      responses["/api/v1/runs"][0].status = "passed";
      responses["/api/v1/runs"][0].finished_at_ms = Date.now();
      responses["/api/v1/readiness"] = [{
        demo_id: "urma-rpc-2",
        target_id: "n4-910c1",
        ready: false,
        issues: [{
          code: "remote_source_repo_missing",
          message: "Git source repository is missing on n4-910c1 at /home/ll/ub_sim",
          remedy: "Prepare the registered target farm.",
        }],
      }];
    };
    window.fetch = async (input, options = {}) => {
      const url = String(input);
      const path = url.split("?")[0];
      if (path.includes("/nodes/") && path.endsWith("/input")) {
        const request = JSON.parse(options.body);
        window.__nodeInputRequests.push({ path, request });
        const bytes = new TextEncoder().encode(request.data).length +
          (request.append_newline ? 1 : 0);
        return new Response(JSON.stringify({
          run_id: "visual-run",
          node_id: "nodeA",
          bytes_written: bytes,
        }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      }
      if (path.endsWith("/api/v1/targets/n4-910c1/prepare")) {
        window.__targetPreparationRequests.push({ path, method: options.method });
        responses["/api/v1/readiness"] = [{
          demo_id: "urma-rpc-2",
          target_id: "n4-910c1",
          ready: true,
          issues: [],
        }];
        return new Response(JSON.stringify({
          target_id: "n4-910c1",
          source_repo: "/home/ll/ub_sim",
          source_revision: "15951308ea8fa1fbce600d434a5b8cf72e132f14",
          source_repo_created: true,
          installed_tools: ["cargo", "ninja"],
          prepared_submodules: ["mem_service"],
          prepared_files: ["guest-linux/aarch64/third_party/busybox-1.36.1.tar.bz2"],
          ready_demos: 1,
          blocked_demos: 0,
        }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      }
      let payload = responses[path];
      if (path.includes("/logs")) {
        const cursor = new URLSearchParams(url.split("?")[1] || "").get("cursor") || "0";
        payload = cursor === "0" ? logChunk : { next_cursor: Number(cursor), lines: [] };
      }
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
for (const output of [desktop, mobile]) {
  await assertNonEmpty(output);
  await assertNonEmpty(output.replace(/\.png$/, "-result.png"));
}
console.log(`desktop=${desktop}`);
console.log(`desktop-result=${desktop.replace(/\.png$/, "-result.png")}`);
console.log(`mobile=${mobile}`);
console.log(`mobile-result=${mobile.replace(/\.png$/, "-result.png")}`);

async function screenshot(width, height, output) {
  const profileDir = await mkdtemp(path.join(os.tmpdir(), "sim-console-chrome-"));
  const browser = spawn(
    chrome,
    [
      "--headless=new",
      "--disable-gpu",
      "--no-sandbox",
      "--disable-crashpad",
      "--disable-breakpad",
      "--disable-crash-reporter",
      "--hide-scrollbars",
      "--allow-file-access-from-files",
      "--remote-debugging-pipe",
      `--user-data-dir=${profileDir}`,
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
    await assertRunViewEntry(protocol, sessionId);
    await assertProcessLogRoundTrip(protocol, sessionId);
    await assertAnsiLogRendering(protocol, sessionId);
    await assertLogRefreshFeedbackClears(protocol, sessionId);

    const metrics = await protocol.send(
      "Runtime.evaluate",
      {
        expression:
          "({width: innerWidth, height: innerHeight, scrollWidth: document.documentElement.scrollWidth, logBandBottom: document.querySelector('.log-band').getBoundingClientRect().bottom, logOutputHeight: document.querySelector('.log-output').getBoundingClientRect().height, logActionsHeight: document.querySelector('.log-actions').getBoundingClientRect().height, logActionsRight: document.querySelector('.log-actions').getBoundingClientRect().right, inputHidden: document.querySelector('#node-input-form').hidden, inputBottom: document.querySelector('#node-input-form').getBoundingClientRect().bottom})",
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
        (viewport.inputHidden
          ? viewport.logOutputHeight < 120
          : viewport.logOutputHeight < 50 || viewport.inputBottom > viewport.height) ||
        viewport.logActionsHeight < 20 ||
        viewport.logActionsRight > viewport.width)
    ) {
      throw new Error(
        `log area geometry is invalid bottom=${viewport.logBandBottom} height=${viewport.height} outputHeight=${viewport.logOutputHeight} actionsHeight=${viewport.logActionsHeight} actionsRight=${viewport.logActionsRight} inputHidden=${viewport.inputHidden} inputBottom=${viewport.inputBottom}`,
      );
    }

    const captured = await protocol.send(
      "Page.captureScreenshot",
      { format: "png", fromSurface: true, captureBeyondViewport: false },
      sessionId,
    );
    await writeFile(output, Buffer.from(captured.data, "base64"));
    await assertParameterDraftSurvivesRefresh(protocol, sessionId);
    await assertTargetPreparation(protocol, sessionId);
    await assertResultPanel(protocol, sessionId);
    const resultOutput = output.replace(/\.png$/, "-result.png");
    const resultCaptured = await protocol.send(
      "Page.captureScreenshot",
      { format: "png", fromSurface: true, captureBeyondViewport: false },
      sessionId,
    );
    await writeFile(resultOutput, Buffer.from(resultCaptured.data, "base64"));
  } finally {
    await protocol.send("Browser.close").catch(() => {});
    await Promise.race([waitForExit(browser), delay(2000)]);
    if (browser.exitCode === null) browser.kill("SIGKILL");
    await rm(profileDir, { recursive: true, force: true }).catch(() => {});
  }
}

async function assertRunViewEntry(protocol, sessionId) {
  const view = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({runViewHidden: document.querySelector('#run-view').hidden, workspaceHidden: document.querySelector('#workspace').hidden, phases: [...document.querySelectorAll('.phase')].map((item) => item.className), activePhase: document.querySelector('.phase.active .phase-label')?.textContent || null, tiles: document.querySelectorAll('.node-tile').length, activity: document.querySelector('#activity-line').textContent, resultHidden: document.querySelector('#result-panel').hidden, livePillHidden: document.querySelector('#live-run-pill').hidden})",
      returnByValue: true,
    },
    sessionId,
  );
  const state = view.result.value;
  if (
    state.runViewHidden ||
    !state.workspaceHidden ||
    state.phases.length !== 5 ||
    state.activePhase !== "Workload" ||
    state.phases[0] !== "phase done" ||
    state.phases[3] !== "phase active" ||
    state.phases[4] !== "phase pending" ||
    state.tiles !== 2 ||
    !state.activity ||
    !state.resultHidden ||
    !state.livePillHidden
  ) {
    throw new Error(`run workspace entry state is invalid: ${JSON.stringify(state)}`);
  }
}

async function assertResultPanel(protocol, sessionId) {
  await protocol.send(
    "Runtime.evaluate",
    { expression: "document.querySelector('.run-item').click()" },
    sessionId,
  );
  await delay(1500);
  const panel = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "(() => { const panel = document.querySelector('#result-panel'); const chips = [...panel.querySelectorAll('.token-chip-piece')].map((chip) => chip.textContent); const meta = [...panel.querySelectorAll('.generated-meta .token')].map((chip) => chip.textContent); const rows = [...panel.querySelectorAll('.timing-row')].map((row) => ({label: row.querySelector('.timing-step-label').textContent, value: row.querySelector('.timing-value').textContent, width: row.querySelector('.timing-fill').style.width})); return {runViewHidden: document.querySelector('#run-view').hidden, hidden: panel.hidden, verdict: panel.querySelector('.verdict-banner strong')?.textContent || null, verdictClass: panel.querySelector('.verdict-banner')?.className || '', generated: panel.querySelector('.generated-text')?.textContent || null, meta, chips, rows, bottleneck: panel.querySelector('.timing-bottleneck')?.textContent || null, phases: [...document.querySelectorAll('.phase')].map((item) => item.className)}; })()",
      returnByValue: true,
    },
    sessionId,
  );
  const result = panel.result.value;
  if (
    result.runViewHidden ||
    result.hidden ||
    result.verdict !== "Run passed" ||
    !result.verdictClass.includes("passed") ||
    result.generated !== "Hello, I" ||
    result.chips.join("|") !== "Hello|,| I" ||
    !result.meta.includes("3 tokens") ||
    !result.meta.includes("decode steps 3/3") ||
    !result.meta.includes("nodes 2/2") ||
    result.rows.length !== 3 ||
    result.rows[0].label !== "step 0" ||
    result.rows[1].width !== "100%" ||
    result.bottleneck !== "Bottleneck: step 1 · 180 ms · nodeB" ||
    !result.phases.every((name) => name.includes("done"))
  ) {
    throw new Error(`result panel rendering failed: ${JSON.stringify(result)}`);
  }
}

async function assertLogRefreshFeedbackClears(protocol, sessionId) {
  const recovered = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "(async () => { showFeedback('Log refresh failed: fixture', true, 'log-refresh'); await refreshLogs(); return {hidden: document.querySelector('#feedback').hidden, text: document.querySelector('#feedback').textContent, source: document.querySelector('#feedback').dataset.source}; })()",
      awaitPromise: true,
      returnByValue: true,
    },
    sessionId,
  );
  if (
    !recovered.result.value.hidden ||
    recovered.result.value.text !== "" ||
    recovered.result.value.source !== ""
  ) {
    throw new Error(`log refresh feedback did not recover: ${JSON.stringify(recovered.result.value)}`);
  }
}

async function assertAnsiLogRendering(protocol, sessionId) {
  const rendered = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "(() => { const output = document.querySelector('#log-output'); const spans = [...output.querySelectorAll('span')]; const bin = spans.find((span) => span.textContent === 'bin'); const indexed = spans.find((span) => span.textContent === 'indexed'); const rgb = spans.find((span) => span.textContent === 'rgb'); return {text: output.textContent, rawEscape: output.textContent.includes('\\u001b') || output.textContent.includes('[1;34m'), injectedElement: Boolean(output.querySelector('node-output')), binWeight: bin ? getComputedStyle(bin).fontWeight : null, binColor: bin ? getComputedStyle(bin).color : null, indexedColor: indexed ? getComputedStyle(indexed).color : null, rgbColor: rgb ? getComputedStyle(rgb).color : null, baseColor: getComputedStyle(output).color}; })()",
      returnByValue: true,
    },
    sessionId,
  );
  const view = rendered.result.value;
  if (
    !view.text.includes("bin ready <node-output>") ||
    view.rawEscape ||
    view.injectedElement ||
    Number.parseInt(view.binWeight, 10) < 700 ||
    !view.binColor ||
    view.binColor === view.baseColor ||
    view.indexedColor !== "rgb(255, 135, 0)" ||
    view.rgbColor !== "rgb(10, 20, 30)"
  ) {
    throw new Error(`ANSI node log rendering failed: ${JSON.stringify(view)}`);
  }
}

async function assertTargetPreparation(protocol, sessionId) {
  await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "window.__blockTargetPreparation(); document.querySelector('#refresh-button').click()",
    },
    sessionId,
  );
  await delay(2500);
  const blocked = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({button: document.querySelector('.prepare-target-command')?.textContent, disabled: document.querySelector('.prepare-target-command')?.disabled, banner: document.querySelector('#demo-readiness').textContent})",
      returnByValue: true,
    },
    sessionId,
  );
  if (
    blocked.result.value.button !== "Prepare target farm" ||
    blocked.result.value.disabled ||
    !blocked.result.value.banner.includes("Git source repository is missing")
  ) {
    throw new Error(`target preparation action is unavailable: ${JSON.stringify(blocked.result.value)}`);
  }
  await protocol.send(
    "Runtime.evaluate",
    { expression: "document.querySelector('.prepare-target-command').click()" },
    sessionId,
  );
  await delay(2500);
  const prepared = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({requests: window.__targetPreparationRequests, feedback: document.querySelector('#feedback').textContent, readiness: document.querySelector('#demo-readiness').textContent, buttonCount: document.querySelectorAll('.prepare-target-command').length})",
      returnByValue: true,
    },
    sessionId,
  );
  if (
    prepared.result.value.requests.length !== 1 ||
    prepared.result.value.requests[0].method !== "POST" ||
    prepared.result.value.feedback !== "Prepared n4-910c1: 1 demos ready, 0 blocked." ||
    !prepared.result.value.readiness.includes("Ready to build and run") ||
    prepared.result.value.buttonCount !== 0
  ) {
    throw new Error(`target preparation did not complete: ${JSON.stringify(prepared.result.value)}`);
  }
}

async function assertParameterDraftSurvivesRefresh(protocol, sessionId) {
  await protocol.send(
    "Runtime.evaluate",
    { expression: "document.querySelector('#back-to-catalog').click()" },
    sessionId,
  );
  await delay(150);
  const launch = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({workspaceHidden: document.querySelector('#workspace').hidden, runViewHidden: document.querySelector('#run-view').hidden, livePillHidden: document.querySelector('#live-run-pill').hidden, livePillLabel: document.querySelector('#live-run-label').textContent})",
      returnByValue: true,
    },
    sessionId,
  );
  const launchState = launch.result.value;
  if (
    launchState.workspaceHidden ||
    !launchState.runViewHidden ||
    launchState.livePillHidden ||
    !launchState.livePillLabel.includes("URMA RPC")
  ) {
    throw new Error(`launch view return failed: ${JSON.stringify(launchState)}`);
  }
  await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "const input = document.querySelector('[name=run_secs]'); input.focus(); input.value = '240'; input.dispatchEvent(new Event('input', { bubbles: true }))",
    },
    sessionId,
  );
  await delay(1200);
  const draft = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({value: document.querySelector('[name=run_secs]').value, focused: document.activeElement === document.querySelector('[name=run_secs]')})",
      returnByValue: true,
    },
    sessionId,
  );
  if (draft.result.value.value !== "240" || !draft.result.value.focused) {
    throw new Error(`parameter draft did not survive refresh: ${JSON.stringify(draft.result.value)}`);
  }
}

async function assertProcessLogRoundTrip(protocol, sessionId) {
  const idleConsole = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({hidden: document.querySelector('#node-input-form').hidden, disabled: document.querySelector('#node-input').disabled, label: document.querySelector('#node-input-label').textContent, status: document.querySelector('#node-input-status').textContent})",
      returnByValue: true,
    },
    sessionId,
  );
  if (
    idleConsole.result.value.hidden ||
    !idleConsole.result.value.disabled ||
    idleConsole.result.value.label !== "Node console" ||
    idleConsole.result.value.status !== "Select a running node above."
  ) {
    throw new Error(`node console entry is not discoverable: ${JSON.stringify(idleConsole.result.value)}`);
  }

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
        "({title: document.querySelector('#log-title').textContent, processSelected: document.querySelector('#process-log').getAttribute('aria-pressed'), selectedNodes: document.querySelectorAll('.node-tile.selected').length, consoleEnabled: !document.querySelector('#node-input').disabled, consoleLabel: document.querySelector('#node-input-label').textContent})",
      returnByValue: true,
    },
    sessionId,
  );
  if (
    nodeView.result.value.title !== "Node A log" ||
    nodeView.result.value.processSelected !== "false" ||
    nodeView.result.value.selectedNodes !== 1 ||
    !nodeView.result.value.consoleEnabled ||
    nodeView.result.value.consoleLabel !== "Node A console"
  ) {
    throw new Error(`node log selection failed: ${JSON.stringify(nodeView.result.value)}`);
  }

  await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "document.querySelector('#node-input').value = 'echo ready'; document.querySelector('#node-input-form').requestSubmit()",
    },
    sessionId,
  );
  await delay(100);
  const inputView = await protocol.send(
    "Runtime.evaluate",
    {
      expression:
        "({requests: window.__nodeInputRequests, value: document.querySelector('#node-input').value, status: document.querySelector('#node-input-status').textContent, hidden: document.querySelector('#node-input-form').hidden})",
      returnByValue: true,
    },
    sessionId,
  );
  if (
    inputView.result.value.requests.length !== 1 ||
    inputView.result.value.requests[0].request.data !== "echo ready" ||
    inputView.result.value.requests[0].request.append_newline !== true ||
    inputView.result.value.value !== "" ||
    inputView.result.value.status !== "Sent 11 bytes to nodeA" ||
    inputView.result.value.hidden
  ) {
    throw new Error(`node input failed: ${JSON.stringify(inputView.result.value)}`);
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
        "({title: document.querySelector('#log-title').textContent, processSelected: document.querySelector('#process-log').getAttribute('aria-pressed'), selectedNodes: document.querySelectorAll('.node-tile.selected').length, inputHidden: document.querySelector('#node-input-form').hidden, inputDisabled: document.querySelector('#node-input').disabled, inputStatus: document.querySelector('#node-input-status').textContent})",
      returnByValue: true,
    },
    sessionId,
  );
  if (
    processView.result.value.title !== "Process log" ||
    processView.result.value.processSelected !== "true" ||
    processView.result.value.selectedNodes !== 0 ||
    processView.result.value.inputHidden ||
    !processView.result.value.inputDisabled ||
    processView.result.value.inputStatus !== "Select a running node above."
  ) {
    throw new Error(`process log return failed: ${JSON.stringify(processView.result.value)}`);
  }
  await protocol.send(
    "Runtime.evaluate",
    { expression: "document.querySelector('.node-tile').click()" },
    sessionId,
  );
  await delay(100);
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
