"use strict";

const state = {
  catalog: [],
  targets: [],
  defaultTargetId: null,
  readiness: [],
  runs: [],
  selectedCategory: "All",
  selectedDemoId: null,
  selectedTargetId: null,
  selectedRunId: null,
  selectedNodeId: null,
  logCursor: 0,
  logLines: [],
  processLogCache: [],
  parameterDrafts: {},
  refreshing: false,
  sendingNodeInput: false,
  nodeInputFeedback: "",
  preparingTargetId: null,
  view: null,
  userChoseView: false,
  resultDrainedFor: null,
};

const elements = {
  apiIndicator: document.querySelector("#api-indicator"),
  apiStatus: document.querySelector("#api-status"),
  feedback: document.querySelector("#feedback"),
  refreshButton: document.querySelector("#refresh-button"),
  liveRunPill: document.querySelector("#live-run-pill"),
  liveRunLabel: document.querySelector("#live-run-label"),
  workspace: document.querySelector("#workspace"),
  runView: document.querySelector("#run-view"),
  backToCatalog: document.querySelector("#back-to-catalog"),
  catalogSearch: document.querySelector("#catalog-search"),
  categoryTabs: document.querySelector("#category-tabs"),
  catalogList: document.querySelector("#catalog-list"),
  catalogCount: document.querySelector("#catalog-count"),
  selectionCategory: document.querySelector("#selection-category"),
  selectionTitle: document.querySelector("#selection-title"),
  selectionSummary: document.querySelector("#selection-summary"),
  startButton: document.querySelector("#start-button"),
  stopButton: document.querySelector("#stop-button"),
  factTopology: document.querySelector("#fact-topology"),
  factNodes: document.querySelector("#fact-nodes"),
  factModel: document.querySelector("#fact-model"),
  factDuration: document.querySelector("#fact-duration"),
  executionTarget: document.querySelector("#execution-target"),
  targetDescription: document.querySelector("#target-description"),
  parameterForm: document.querySelector("#parameter-form"),
  requirements: document.querySelector("#requirements"),
  demoReadiness: document.querySelector("#demo-readiness"),
  runTitle: document.querySelector("#run-title"),
  runMeta: document.querySelector("#run-meta"),
  runStatus: document.querySelector("#run-status"),
  runElapsed: document.querySelector("#run-elapsed"),
  phaseStepper: document.querySelector("#phase-stepper"),
  topology: document.querySelector("#topology"),
  nodeDetail: document.querySelector("#node-detail"),
  resultPanel: document.querySelector("#result-panel"),
  runFacts: document.querySelector("#run-facts"),
  activityPanel: document.querySelector("#activity-panel"),
  activityLine: document.querySelector("#activity-line"),
  logBand: document.querySelector("#log-band"),
  logResizeHandle: document.querySelector("#log-resize-handle"),
  logTabs: document.querySelector("#log-tabs"),
  logTitle: document.querySelector("#log-title"),
  logOutput: document.querySelector("#log-output"),
  processLog: document.querySelector("#process-log"),
  followLog: document.querySelector("#follow-log"),
  clearLog: document.querySelector("#clear-log"),
  nodeInputForm: document.querySelector("#node-input-form"),
  nodeInputLabel: document.querySelector("#node-input-label"),
  nodeInput: document.querySelector("#node-input"),
  sendNodeInput: document.querySelector("#send-node-input"),
  nodeInputStatus: document.querySelector("#node-input-status"),
  runList: document.querySelector("#run-list"),
  runCount: document.querySelector("#run-count"),
};

function selectedDemo() {
  return state.catalog.find((demo) => demo.id === state.selectedDemoId) || null;
}

function selectedRun() {
  return state.runs.find((run) => run.id === state.selectedRunId) || null;
}

function selectedDemoReadiness() {
  return state.readiness.find(
    (item) => item.demo_id === state.selectedDemoId && item.target_id === state.selectedTargetId,
  ) || null;
}

function selectedTarget() {
  return state.targets.find((target) => target.id === state.selectedTargetId) || null;
}

function isLive(status) {
  return ["queued", "starting", "running"].includes(status);
}

const ANSI_COLORS = [
  "var(--ansi-black)",
  "var(--ansi-red)",
  "var(--ansi-green)",
  "var(--ansi-yellow)",
  "var(--ansi-blue)",
  "var(--ansi-magenta)",
  "var(--ansi-cyan)",
  "var(--ansi-white)",
  "var(--ansi-bright-black)",
  "var(--ansi-bright-red)",
  "var(--ansi-bright-green)",
  "var(--ansi-bright-yellow)",
  "var(--ansi-bright-blue)",
  "var(--ansi-bright-magenta)",
  "var(--ansi-bright-cyan)",
  "var(--ansi-bright-white)",
];

const ANSI_SEQUENCE = /\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\)|[PX^_][\s\S]*?\x1b\\|.)/g;

function stripAnsi(text) {
  ANSI_SEQUENCE.lastIndex = 0;
  return text.replace(ANSI_SEQUENCE, "");
}

function defaultAnsiStyle() {
  return {
    foreground: null,
    background: null,
    bold: false,
    faint: false,
    italic: false,
    underline: false,
    inverse: false,
    concealed: false,
    strike: false,
  };
}

function renderLogText(text) {
  const fragment = document.createDocumentFragment();
  const style = defaultAnsiStyle();
  let cursor = 0;

  ANSI_SEQUENCE.lastIndex = 0;
  for (let match = ANSI_SEQUENCE.exec(text); match; match = ANSI_SEQUENCE.exec(text)) {
    appendAnsiText(fragment, text.slice(cursor, match.index), style);
    if (match[0].startsWith("\x1b[") && match[0].endsWith("m")) {
      applyAnsiSgr(style, match[0].slice(2, -1));
    }
    cursor = match.index + match[0].length;
  }
  appendAnsiText(fragment, text.slice(cursor), style);
  elements.logOutput.replaceChildren(fragment);
}

function appendAnsiText(fragment, text, style) {
  const visibleText = text
    .replace(/\r(?!\n)/g, "\n")
    .replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/g, "");
  if (!visibleText) return;

  const decorations = [];
  if (style.underline) decorations.push("underline");
  if (style.strike) decorations.push("line-through");
  const foreground = style.inverse
    ? style.background || "var(--terminal)"
    : style.foreground;
  const background = style.inverse
    ? style.foreground || "var(--terminal-text)"
    : style.background;
  if (
    !foreground &&
    !background &&
    !style.bold &&
    !style.faint &&
    !style.italic &&
    !style.concealed &&
    !decorations.length
  ) {
    fragment.append(document.createTextNode(visibleText));
    return;
  }

  const span = document.createElement("span");
  span.textContent = visibleText;
  if (foreground) span.style.color = foreground;
  if (background) span.style.backgroundColor = background;
  if (style.bold) span.style.fontWeight = "700";
  if (style.faint) span.style.opacity = "0.65";
  if (style.italic) span.style.fontStyle = "italic";
  if (style.concealed) span.style.visibility = "hidden";
  if (decorations.length) span.style.textDecoration = decorations.join(" ");
  fragment.append(span);
}

function applyAnsiSgr(style, parameters) {
  const codes = parameters === ""
    ? [0]
    : parameters.split(";").map((value) => Number.parseInt(value || "0", 10));
  for (let index = 0; index < codes.length; index += 1) {
    const code = Number.isFinite(codes[index]) ? codes[index] : 0;
    if (code === 0) Object.assign(style, defaultAnsiStyle());
    else if (code === 1) style.bold = true;
    else if (code === 2) style.faint = true;
    else if (code === 3) style.italic = true;
    else if (code === 4 || code === 21) style.underline = true;
    else if (code === 7) style.inverse = true;
    else if (code === 8) style.concealed = true;
    else if (code === 9) style.strike = true;
    else if (code === 22) {
      style.bold = false;
      style.faint = false;
    } else if (code === 23) style.italic = false;
    else if (code === 24) style.underline = false;
    else if (code === 27) style.inverse = false;
    else if (code === 28) style.concealed = false;
    else if (code === 29) style.strike = false;
    else if (code >= 30 && code <= 37) style.foreground = ANSI_COLORS[code - 30];
    else if (code >= 40 && code <= 47) style.background = ANSI_COLORS[code - 40];
    else if (code >= 90 && code <= 97) style.foreground = ANSI_COLORS[code - 90 + 8];
    else if (code >= 100 && code <= 107) style.background = ANSI_COLORS[code - 100 + 8];
    else if (code === 39) style.foreground = null;
    else if (code === 49) style.background = null;
    else if (code === 38 || code === 48) {
      const extended = ansiExtendedColor(codes, index + 1);
      if (extended.color) {
        if (code === 38) style.foreground = extended.color;
        else style.background = extended.color;
      }
      index += extended.consumed;
    }
  }
}

function ansiExtendedColor(codes, start) {
  if (codes[start] === 5 && Number.isInteger(codes[start + 1])) {
    return { color: ansi256Color(codes[start + 1]), consumed: 2 };
  }
  const rgbComponents = codes.slice(start + 1, start + 4);
  if (
    codes[start] === 2 &&
    rgbComponents.length === 3 &&
    rgbComponents.every((value) => Number.isInteger(value))
  ) {
    const rgb = rgbComponents.map((value) => Math.max(0, Math.min(255, value)));
    return { color: `rgb(${rgb.join(", ")})`, consumed: 4 };
  }
  return { color: null, consumed: 0 };
}

function ansi256Color(index) {
  const color = Math.max(0, Math.min(255, index));
  if (color < ANSI_COLORS.length) return ANSI_COLORS[color];
  if (color >= 232) {
    const level = 8 + (color - 232) * 10;
    return `rgb(${level}, ${level}, ${level})`;
  }
  const cube = color - 16;
  const levels = [0, 95, 135, 175, 215, 255];
  const red = levels[Math.floor(cube / 36)];
  const green = levels[Math.floor((cube % 36) / 6)];
  const blue = levels[cube % 6];
  return `rgb(${red}, ${green}, ${blue})`;
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
    ...options,
  });
  if (!response.ok) {
    let message = `${response.status} ${response.statusText}`;
    try {
      const body = await response.json();
      message = body.error || message;
    } catch (_) {
      // Keep the HTTP status when the backend did not return JSON.
    }
    throw new Error(message);
  }
  return response.json();
}

function setHealth(ok, message) {
  elements.apiIndicator.className = `status-dot ${ok ? "ok" : "error"}`;
  elements.apiStatus.textContent = message;
}

function showFeedback(message, error = true, source = "") {
  elements.feedback.hidden = !message;
  elements.feedback.textContent = message || "";
  elements.feedback.dataset.source = message ? source : "";
  elements.feedback.className = message ? `feedback ${error ? "error" : "ok"}` : "feedback";
}

/* ------------------------------------------------------------------ views */

function setView(view, userInitiated = false) {
  state.view = view;
  if (userInitiated) state.userChoseView = true;
  elements.workspace.hidden = view !== "launch";
  elements.runView.hidden = view !== "run";
  document.body.dataset.view = view;
  renderLivePill();
}

function renderLivePill() {
  const liveRun = state.runs.find((run) => isLive(run.status));
  const show = Boolean(liveRun) && state.view === "launch";
  elements.liveRunPill.hidden = !show;
  if (show) {
    elements.liveRunLabel.textContent = `${liveRun.demo_title} · ${runTiming(liveRun)}`;
  }
}

/* ------------------------------------------------------------------ catalog */

function renderCategories() {
  const categories = ["All", ...new Set(state.catalog.map((demo) => demo.category))];
  elements.categoryTabs.replaceChildren();
  for (const category of categories) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `category-tab${state.selectedCategory === category ? " active" : ""}`;
    button.textContent = category;
    button.setAttribute("role", "tab");
    button.setAttribute("aria-selected", String(state.selectedCategory === category));
    button.addEventListener("click", () => {
      state.selectedCategory = category;
      renderCategories();
      renderCatalog();
    });
    elements.categoryTabs.append(button);
  }
}

function renderCatalog() {
  const search = elements.catalogSearch.value.trim().toLowerCase();
  const demos = state.catalog.filter((demo) => {
    const categoryMatch = state.selectedCategory === "All" || demo.category === state.selectedCategory;
    const text = [demo.title, demo.summary, demo.category, demo.model || "", ...(demo.tags || []), ...(demo.data_plane || [])]
      .join(" ")
      .toLowerCase();
    return categoryMatch && (!search || text.includes(search));
  });
  elements.catalogCount.textContent = `${demos.length} ${demos.length === 1 ? "entry" : "entries"}`;
  elements.catalogList.replaceChildren();
  if (!demos.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "No registered demos match this filter.";
    elements.catalogList.append(empty);
    return;
  }
  for (const demo of demos) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `catalog-item${demo.id === state.selectedDemoId ? " selected" : ""}`;
    const top = document.createElement("div");
    top.className = "item-topline";
    const title = document.createElement("strong");
    title.textContent = demo.title;
    const count = document.createElement("span");
    count.className = "node-count";
    count.textContent = `${demo.node_count}N`;
    const summary = document.createElement("p");
    summary.textContent = demo.summary;
    top.append(title, count);
    button.append(top, summary);
    button.addEventListener("click", () => selectDemo(demo.id));
    elements.catalogList.append(button);
  }
}

function selectDemo(demoId) {
  state.selectedDemoId = demoId;
  renderCatalog();
  renderSelection();
}

function renderTargets() {
  elements.executionTarget.replaceChildren();
  for (const target of state.targets) {
    const option = document.createElement("option");
    option.value = target.id;
    option.textContent = target.kind === "local" ? `${target.title} / local` : `${target.title} / SSH`;
    option.selected = target.id === state.selectedTargetId;
    elements.executionTarget.append(option);
  }
  const target = selectedTarget();
  elements.targetDescription.textContent = target
    ? `${target.description}${target.repo_root ? ` Repository: ${target.repo_root}` : ""}`
    : "No execution target is selected.";
}

function parameterDraftFor(demo) {
  const draft = state.parameterDrafts[demo.id] || {};
  for (const parameter of demo.parameters || []) {
    if (!Object.hasOwn(draft, parameter.id)) draft[parameter.id] = parameter.default;
  }
  state.parameterDrafts[demo.id] = draft;
  return draft;
}

function renderParameterForm(demo) {
  const parameters = demo.parameters || [];
  const signature = JSON.stringify(parameters);
  if (
    elements.parameterForm.dataset.demoId === demo.id &&
    elements.parameterForm.dataset.signature === signature
  ) {
    return;
  }

  const draft = parameterDraftFor(demo);
  elements.parameterForm.replaceChildren();
  for (const parameter of parameters) {
    const label = document.createElement("label");
    label.className = "parameter-field";
    label.textContent = parameter.label;
    let control;
    if (parameter.kind === "select") {
      control = document.createElement("select");
      for (const choice of parameter.choices) {
        const option = document.createElement("option");
        option.value = choice;
        option.textContent = choice;
        control.append(option);
      }
    } else {
      control = document.createElement("input");
      control.type = "number";
      if (parameter.min !== null && parameter.min !== undefined) control.min = String(parameter.min);
      if (parameter.max !== null && parameter.max !== undefined) control.max = String(parameter.max);
      control.step = "1";
    }
    control.name = parameter.id;
    control.value = draft[parameter.id];
    control.addEventListener("input", () => {
      draft[parameter.id] = control.value;
    });
    label.append(control);
    elements.parameterForm.append(label);
  }
  elements.parameterForm.dataset.demoId = demo.id;
  elements.parameterForm.dataset.signature = signature;
}

function renderSelection() {
  const demo = selectedDemo();
  const readiness = selectedDemoReadiness();
  const target = selectedTarget();
  const liveRun = state.runs.find((run) => isLive(run.status));
  if (!demo) {
    return;
  }
  renderTargets();
  elements.selectionCategory.textContent = demo.category;
  elements.selectionTitle.textContent = demo.title;
  elements.selectionSummary.textContent = demo.summary;
  elements.factTopology.textContent = titleCase(demo.topology);
  elements.factNodes.textContent = String(demo.node_count);
  elements.factModel.textContent = demo.model || "Not model-specific";
  elements.factDuration.textContent = formatDuration(demo.estimated_duration_secs * 1000);

  renderParameterForm(demo);

  elements.requirements.replaceChildren();
  for (const requirement of demo.requirements || []) {
    const token = document.createElement("span");
    token.className = "token";
    token.textContent = requirement;
    elements.requirements.append(token);
  }

  if (!readiness) {
    elements.demoReadiness.className = "readiness-banner checking";
    elements.demoReadiness.textContent = "Checking launch readiness...";
  } else if (readiness.ready) {
    elements.demoReadiness.className = "readiness-banner ready";
    elements.demoReadiness.textContent = `Ready to build and run on ${target?.title || state.selectedTargetId}.`;
  } else {
    elements.demoReadiness.className = "readiness-banner blocked";
    elements.demoReadiness.replaceChildren();
    const title = document.createElement("strong");
    title.textContent = "Launch blocked";
    elements.demoReadiness.append(title);
    for (const issue of readiness.issues) {
      const message = document.createElement("p");
      message.textContent = issue.message;
      const remedy = document.createElement("span");
      remedy.textContent = issue.remedy;
      elements.demoReadiness.append(message, remedy);
    }
    const preparableIssueCodes = new Set([
      "remote_source_repo_missing",
      "remote_submodule_object_missing",
      "remote_submodule_head_mismatch",
      "remote_submodule_checkout_dirty",
      "remote_tool_missing",
      "remote_bootstrap_file_missing",
    ]);
    const canPrepare =
      target?.kind === "ssh" &&
      readiness.issues.some((issue) => preparableIssueCodes.has(issue.code));
    if (canPrepare) {
      const prepareButton = document.createElement("button");
      const isPreparing = state.preparingTargetId === target.id;
      prepareButton.type = "button";
      prepareButton.className = "secondary-command prepare-target-command";
      prepareButton.textContent = isPreparing ? "Preparing target farm..." : "Prepare target farm";
      prepareButton.disabled = isPreparing || Boolean(liveRun);
      prepareButton.title = liveRun
        ? `Run ${liveRun.id} must finish before target preparation`
        : `Prepare ${target.title}`;
      prepareButton.addEventListener("click", prepareTarget);
      elements.demoReadiness.append(prepareButton);
    }
  }

  elements.startButton.disabled = Boolean(liveRun) || !readiness?.ready;
  elements.startButton.title = liveRun
    ? `Run ${liveRun.id} is active`
    : readiness?.ready
      ? `Start ${demo.title} on ${target?.title || state.selectedTargetId}`
      : "Resolve launch readiness before starting";
}

/* ------------------------------------------------------------------ run view */

const RUN_PHASES = [
  { id: "launch", label: "Launch" },
  { id: "boot", label: "Boot nodes" },
  { id: "ready", label: "Cluster ready" },
  { id: "workload", label: "Workload" },
  { id: "result", label: "Result" },
];

const WORKLOAD_MARKER = /decode_token:|decode_output:|\[mem_service\] stage|run_app|verdict=|status=pass|] pass/i;

function derivePhaseIndex(run, logText) {
  if (!run) return -1;
  if (!isLive(run.status)) return RUN_PHASES.length - 1;
  if (run.status === "queued" || run.status === "starting") return 0;
  const nodes = run.nodes || [];
  if (WORKLOAD_MARKER.test(logText)) return 3;
  const anyPending = nodes.some((node) => ["unknown", "booting"].includes(node.status));
  if (anyPending || !nodes.length) return 1;
  return 2;
}

function renderPhaseStepper(run) {
  const logText = state.processLogCache.length
    ? state.processLogCache.join("\n")
    : state.logLines.join("\n");
  const current = derivePhaseIndex(run, logText);
  const terminal = run && !isLive(run.status);
  elements.phaseStepper.replaceChildren();
  RUN_PHASES.forEach((phase, index) => {
    const item = document.createElement("li");
    let phaseState = "pending";
    if (index < current) phaseState = "done";
    else if (index === current) phaseState = terminal ? `done ${run.status}` : "active";
    item.className = `phase ${phaseState}`;
    const dot = document.createElement("span");
    dot.className = "phase-dot";
    const copy = document.createElement("span");
    copy.className = "phase-label";
    copy.textContent = phase.label;
    item.append(dot, copy);
    if (index < RUN_PHASES.length - 1) {
      const link = document.createElement("span");
      link.className = "phase-link";
      item.append(link);
    }
    elements.phaseStepper.append(item);
  });
}

function renderRunView() {
  const run = selectedRun();
  const demo = run ? state.catalog.find((item) => item.id === run.demo_id) || selectedDemo() : null;
  renderLivePill();
  if (!run) {
    return;
  }

  elements.runTitle.textContent = run.demo_title;
  elements.runMeta.textContent = `${run.target_id} / ${shortRevision(run.source_revision)} / ${run.id}`;
  setStatusBadge(elements.runStatus, run.status);
  elements.runElapsed.textContent = runTiming(run);
  elements.stopButton.hidden =
    !isLive(run.status) || !((demo?.controls) || []).includes("stop");

  renderPhaseStepper(run);
  renderTopology(run, demo);
  renderRunFacts(run, demo);
  renderActivity(run);
  renderResultPanel(run);
  renderLogTabs(run);
  renderNodeInput(run, demo);
}

function renderTopology(run, demo) {
  const nodes = run.nodes || [];
  elements.topology.className = `topology ${demo?.topology || "mesh"}`;
  elements.topology.replaceChildren();

  nodes.forEach((node, index) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `node-tile${state.selectedNodeId === node.id ? " selected" : ""}`;
    const top = document.createElement("div");
    top.className = "node-name";
    const name = document.createElement("span");
    name.textContent = node.label;
    const status = document.createElement("span");
    status.className = `status-badge ${node.status}`;
    status.textContent = node.status;
    const role = document.createElement("span");
    role.className = "node-role";
    role.textContent = demo ? nodeRole(demo, index) : "";
    top.append(name, status);
    button.append(top, role);
    button.addEventListener("click", () => selectNode(node.id));
    elements.topology.append(button);
  });

  const node = nodes.find((item) => item.id === state.selectedNodeId);
  if (node) {
    elements.nodeDetail.textContent = `${node.label} / ${node.status} / ${node.log_path || "process log until node log is discovered"}`;
  } else {
    elements.nodeDetail.textContent = "Select a node to inspect node-specific output.";
  }
}

function renderRunFacts(run, demo) {
  const facts = [
    ["Demo", run.demo_id],
    ["Target", run.target_id],
    ["Revision", shortRevision(run.source_revision)],
    ["Created", formatTimestamp(run.created_at_ms)],
    ["Started", run.started_at_ms ? formatTimestamp(run.started_at_ms) : "pending"],
    ["Finished", run.finished_at_ms ? formatTimestamp(run.finished_at_ms) : (isLive(run.status) ? "running" : "pending")],
    ["Duration", runTiming(run)],
  ];
  if (run.exit_code !== null && run.exit_code !== undefined) {
    facts.push(["Exit code", String(run.exit_code)]);
  }
  const parameters = Object.entries(run.parameters || {});
  if (parameters.length) {
    facts.push(["Parameters", parameters.map(([key, value]) => `${key}=${value}`).join(" ")]);
  }
  if (demo?.model) facts.push(["Model", demo.model]);

  elements.runFacts.replaceChildren();
  for (const [label, value] of facts) {
    const term = document.createElement("dt");
    term.textContent = label;
    const detail = document.createElement("dd");
    detail.textContent = value;
    elements.runFacts.append(term, detail);
  }
}

function renderActivity(run) {
  const lines = state.logLines.length ? state.logLines : state.processLogCache;
  let latest = "";
  for (let index = lines.length - 1; index >= 0; index -= 1) {
    const clean = stripAnsi(lines[index]).trim();
    if (clean) {
      latest = clean;
      break;
    }
  }
  elements.activityLine.textContent = latest || "Waiting for output...";
  elements.activityLine.title = latest;
}

function renderLogTabs(run) {
  const nodeButtons = elements.logTabs.querySelectorAll(".node-tab-command");
  nodeButtons.forEach((button) => button.remove());
  for (const node of run.nodes || []) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `secondary-command log-source-command node-tab-command${state.selectedNodeId === node.id ? " active" : ""}`;
    button.textContent = node.label;
    button.setAttribute("role", "tab");
    button.setAttribute("aria-pressed", String(state.selectedNodeId === node.id));
    button.addEventListener("click", () => selectLogSource(node.id));
    elements.logTabs.append(button);
  }
  const node = (run.nodes || []).find((item) => item.id === state.selectedNodeId);
  elements.logTitle.textContent = node ? `${node.label} log` : "Process log";
  elements.processLog.classList.toggle("active", !node);
  elements.processLog.setAttribute("aria-pressed", String(!node));
}

function renderNodeInput(run, demo) {
  const node = (run.nodes || []).find((item) => item.id === state.selectedNodeId);
  const nodeInputSupported = ((demo?.controls) || []).includes("node_input");
  const nodeInputAvailable = Boolean(node && isLive(run.status) && nodeInputSupported);
  let nodeInputAvailability = "";
  if (!isLive(run.status)) {
    nodeInputAvailability = "This run has ended. Start a new run to use its node consoles.";
  } else if (!node) {
    nodeInputAvailability = "Select a running node above.";
  } else {
    nodeInputAvailability = `${node.label} serial console is ready.`;
  }
  elements.nodeInputForm.hidden = !nodeInputSupported;
  elements.nodeInputForm.classList.toggle("available", nodeInputAvailable);
  elements.logBand.classList.toggle("node-input-active", nodeInputAvailable);
  elements.nodeInputLabel.textContent = node ? `${node.label} console` : "Node console";
  elements.nodeInput.placeholder = nodeInputAvailable
    ? `Type a command for ${node.label}`
    : nodeInputAvailability;
  elements.nodeInputStatus.textContent = nodeInputAvailable
    ? state.nodeInputFeedback || nodeInputAvailability
    : nodeInputAvailability;
  elements.nodeInput.disabled = !nodeInputAvailable || state.sendingNodeInput;
  elements.sendNodeInput.disabled = !nodeInputAvailable || state.sendingNodeInput;
}

function selectNode(nodeId) {
  selectLogSource(state.selectedNodeId === nodeId ? null : nodeId);
}

function selectLogSource(nodeId) {
  if (state.selectedNodeId === nodeId) return;
  state.selectedNodeId = nodeId;
  state.nodeInputFeedback = "";
  resetLog();
  renderRunView();
  void refreshLogs();
}

/* ------------------------------------------------------------------ run results */

const RESULT_MARKERS = [
  "decode_output:",
  "decode_token:",
  "timing_step:",
  "timing_node:",
  "timing_bottleneck:",
  "summary:",
];

function unescapeQuoted(value) {
  return value.replace(/\\(.)/g, (_, character) => {
    if (character === "n") return "\n";
    if (character === "t") return "\t";
    return character;
  });
}

function parseKeyValues(body) {
  const pairs = {};
  for (const chunk of body.split(/\s+/)) {
    const at = chunk.indexOf("=");
    if (at <= 0) continue;
    pairs[chunk.slice(0, at)] = chunk.slice(at + 1);
  }
  return pairs;
}

function extractRunResult(run, logLines) {
  const result = {
    status: run.status,
    message: run.message || "",
    exitCode: run.exit_code ?? null,
    generatedText: null,
    tokenIds: null,
    unavailableReason: null,
    tokens: [],
    steps: [],
    bottleneck: null,
    summary: {},
    hasStructuredOutput: false,
  };
  for (const rawLine of logLines) {
    const line = stripAnsi(rawLine);
    let body = null;
    for (const marker of RESULT_MARKERS) {
      const at = line.indexOf(marker);
      // Anchored markers only: a marker must start the line or follow a
      // non-word character (timestamp/bracket prefixes). This keeps prefixed
      // lines such as "engram_timing_step:" or "w5_device_summary:" from
      // being misparsed as "timing_step:" / "summary:" records.
      if (at >= 0 && (at === 0 || /\W/.test(line[at - 1])) && (body === null || at < body.at)) {
        body = { at, text: line.slice(at) };
      }
    }
    if (!body) continue;
    const text = body.text;
    if (text.startsWith("decode_output:")) {
      result.hasStructuredOutput = true;
      const pieces = /token_pieces="((?:[^"\\]|\\.)*)"/.exec(text);
      if (pieces) result.generatedText = unescapeQuoted(pieces[1]);
      const ids = /token_ids=\[([^\]]*)\]/.exec(text);
      if (ids) {
        result.tokenIds = ids[1]
          .split(",")
          .map((value) => Number.parseInt(value.trim(), 10))
          .filter((value) => Number.isFinite(value));
      }
      const unavailable = /unavailable\s+reason=(\S+)/.exec(text);
      if (unavailable) result.unavailableReason = unavailable[1];
    } else if (text.startsWith("decode_token:")) {
      result.hasStructuredOutput = true;
      const step = /\bstep=(\d+)/.exec(text);
      const node = /\bnode=(\S+)/.exec(text);
      const token = /\btoken=(\d+)/.exec(text);
      const piece = /piece="((?:[^"\\]|\\.)*)"/.exec(text);
      result.tokens.push({
        step: step ? Number.parseInt(step[1], 10) : null,
        node: node ? node[1] : "",
        token: token ? Number.parseInt(token[1], 10) : null,
        piece: piece ? unescapeQuoted(piece[1]) : "",
      });
    } else if (text.startsWith("timing_step:")) {
      result.hasStructuredOutput = true;
      const pairs = parseKeyValues(text.slice("timing_step:".length));
      result.steps.push({
        step: Number.parseInt(pairs.step, 10),
        roundMs: Number.parseInt(pairs.round_ms, 10),
        criticalNode: pairs.critical_node || "",
      });
    } else if (text.startsWith("timing_bottleneck:")) {
      result.hasStructuredOutput = true;
      const pairs = parseKeyValues(text.slice("timing_bottleneck:".length));
      result.bottleneck = {
        step: Number.parseInt(pairs.slowest_step, 10),
        roundMs: Number.parseInt(pairs.round_ms, 10),
        criticalNode: pairs.critical_node || "",
      };
    } else if (text.startsWith("summary:")) {
      result.hasStructuredOutput = true;
      Object.assign(result.summary, parseKeyValues(text.slice("summary:".length)));
    }
  }
  result.steps.sort((a, b) => a.step - b.step);
  return result;
}

function renderResultPanel(run) {
  const terminal = !isLive(run.status);
  elements.resultPanel.hidden = !terminal;
  if (!terminal) return;

  const drained = state.resultDrainedFor === run.id || state.processLogCache.length > 0;
  const result = extractRunResult(run, state.processLogCache);
  elements.resultPanel.replaceChildren();

  const verdict = document.createElement("div");
  verdict.className = `verdict-banner ${run.status}`;
  const verdictIcon = document.createElement("span");
  verdictIcon.className = "verdict-icon";
  verdictIcon.textContent = run.status === "passed" ? "✓" : run.status === "failed" ? "✕" : "■";
  const verdictCopy = document.createElement("div");
  const verdictTitle = document.createElement("strong");
  verdictTitle.textContent =
    run.status === "passed" ? "Run passed" : run.status === "failed" ? "Run failed" : "Run stopped";
  const verdictDetail = document.createElement("span");
  const detailParts = [run.message || ""];
  if (result.exitCode !== null) detailParts.push(`exit code ${result.exitCode}`);
  detailParts.push(`duration ${runTiming(run)}`);
  verdictDetail.textContent = detailParts.filter(Boolean).join(" · ");
  verdictCopy.append(verdictTitle, verdictDetail);
  verdict.append(verdictIcon, verdictCopy);
  elements.resultPanel.append(verdict);

  if (!drained) {
    const pending = document.createElement("p");
    pending.className = "result-pending";
    pending.textContent = "Collecting final output...";
    elements.resultPanel.append(pending);
    return;
  }

  if (result.unavailableReason) {
    const unavailable = document.createElement("p");
    unavailable.className = "result-pending";
    unavailable.textContent = `Inference output unavailable: ${result.unavailableReason}`;
    elements.resultPanel.append(unavailable);
  }

  if (result.generatedText !== null) {
    const card = document.createElement("div");
    card.className = "generated-text-card";
    const label = document.createElement("span");
    label.className = "eyebrow";
    label.textContent = "Generated text";
    const text = document.createElement("p");
    text.className = "generated-text";
    text.textContent = result.generatedText;
    card.append(label, text);
    const meta = document.createElement("div");
    meta.className = "generated-meta";
    const observed = result.summary.decode_steps_observed;
    const expected = result.summary.decode_steps_expected;
    const passedNodes = result.summary.passed_nodes;
    const chips = [];
    if (result.tokenIds) chips.push(`${result.tokenIds.length} tokens`);
    if (observed !== undefined && expected !== undefined) chips.push(`decode steps ${observed}/${expected}`);
    if (passedNodes) chips.push(`nodes ${passedNodes}`);
    for (const chip of chips) {
      const item = document.createElement("span");
      item.className = "token";
      item.textContent = chip;
      meta.append(item);
    }
    card.append(meta);
    elements.resultPanel.append(card);
  }

  if (result.tokens.length) {
    const section = document.createElement("div");
    section.className = "token-stream";
    const label = document.createElement("span");
    label.className = "eyebrow";
    label.textContent = "Decode stream";
    const stream = document.createElement("div");
    stream.className = "token-chip-row";
    for (const token of result.tokens) {
      const chip = document.createElement("span");
      chip.className = "token-chip";
      chip.title = `step ${token.step} · ${token.node} · token ${token.token}`;
      const piece = document.createElement("span");
      piece.className = "token-chip-piece";
      piece.textContent = token.piece || `�${token.token}`;
      const step = document.createElement("span");
      step.className = "token-chip-step";
      step.textContent = `#${token.step}`;
      chip.append(piece, step);
      stream.append(chip);
    }
    section.append(label, stream);
    elements.resultPanel.append(section);
  }

  if (result.steps.length) {
    const section = document.createElement("div");
    section.className = "timing-section";
    const label = document.createElement("span");
    label.className = "eyebrow";
    label.textContent = "Step latency";
    const bars = document.createElement("div");
    bars.className = "timing-bars";
    const maxRound = Math.max(...result.steps.map((step) => step.roundMs || 0), 1);
    for (const step of result.steps) {
      const row = document.createElement("div");
      row.className = "timing-row";
      const stepLabel = document.createElement("span");
      stepLabel.className = "timing-step-label";
      stepLabel.textContent = `step ${step.step}`;
      const track = document.createElement("div");
      track.className = "timing-track";
      const fill = document.createElement("div");
      fill.className = "timing-fill";
      fill.style.width = `${Math.max(2, Math.round(((step.roundMs || 0) / maxRound) * 100))}%`;
      track.append(fill);
      const value = document.createElement("span");
      value.className = "timing-value";
      value.textContent = `${step.roundMs} ms${step.criticalNode ? ` · ${step.criticalNode}` : ""}`;
      row.append(stepLabel, track, value);
      bars.append(row);
    }
    section.append(label, bars);
    if (result.bottleneck && Number.isFinite(result.bottleneck.roundMs)) {
      const bottleneck = document.createElement("p");
      bottleneck.className = "timing-bottleneck";
      bottleneck.textContent =
        `Bottleneck: step ${result.bottleneck.step} · ${result.bottleneck.roundMs} ms · ${result.bottleneck.criticalNode}`;
      section.append(bottleneck);
    }
    elements.resultPanel.append(section);
  }

  if (!result.hasStructuredOutput) {
    const nodes = (run.nodes || []).map((node) => `${node.label} ${node.status}`).join(" · ");
    const generic = document.createElement("p");
    generic.className = "result-pending";
    generic.textContent = nodes
      ? `Final node state: ${nodes}. Full output is available in the log below.`
      : "No structured result markers were emitted. Full output is available in the log below.";
    elements.resultPanel.append(generic);
  }
}

/* ------------------------------------------------------------------ run history */

function renderRuns() {
  elements.runCount.textContent = `${state.runs.length} ${state.runs.length === 1 ? "run" : "runs"}`;
  elements.runList.replaceChildren();
  if (!state.runs.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "Runs started from the CLI or Web will appear here.";
    elements.runList.append(empty);
    return;
  }
  for (const run of state.runs) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `run-item${run.id === state.selectedRunId ? " selected" : ""}`;
    const top = document.createElement("div");
    top.className = "run-item-topline";
    const title = document.createElement("strong");
    title.textContent = run.demo_title;
    const status = document.createElement("span");
    setStatusBadge(status, run.status);
    const detail = document.createElement("p");
    detail.textContent = `${run.target_id} / ${shortRevision(run.source_revision)} / ${run.id} / ${runTiming(run)}`;
    top.append(title, status);
    button.append(top, detail);
    button.addEventListener("click", () => selectRun(run.id));
    elements.runList.append(button);
  }
}

function selectRun(runId) {
  const run = state.runs.find((item) => item.id === runId);
  if (!run) return;
  state.selectedRunId = runId;
  state.selectedDemoId = run.demo_id;
  state.selectedTargetId = run.target_id;
  state.readiness = [];
  state.selectedNodeId = null;
  resetLog();
  setView("run", true);
  renderCatalog();
  renderSelection();
  renderRunView();
  renderRuns();
  void refreshLogs();
}

function resetLog() {
  state.logCursor = 0;
  state.logLines = [];
  state.processLogCache = [];
  state.resultDrainedFor = null;
  renderLogText(state.selectedRunId ? "Waiting for output..." : "No active run.");
}

/* ------------------------------------------------------------------ actions */

async function startRun() {
  const demo = selectedDemo();
  if (!demo) return;
  const parameters = {};
  for (const control of elements.parameterForm.elements) {
    if (control.name) parameters[control.name] = control.value;
  }
  elements.startButton.disabled = true;
  showFeedback("");
  try {
    const run = await api("/api/v1/runs", {
      method: "POST",
      body: JSON.stringify({
        demo_id: demo.id,
        target_id: state.selectedTargetId,
        parameters,
      }),
    });
    state.runs.unshift(run);
    state.selectedRunId = run.id;
    state.selectedNodeId = null;
    resetLog();
    setView("run", true);
    renderAll();
    showFeedback(`Started ${run.id}`, false);
    setTimeout(() => showFeedback(""), 2500);
  } catch (error) {
    showFeedback(`Start failed: ${error.message}`);
    state.readiness = [];
  } finally {
    renderSelection();
  }
}

async function prepareTarget() {
  const target = selectedTarget();
  if (!target || target.kind !== "ssh" || state.preparingTargetId) return;
  state.preparingTargetId = target.id;
  showFeedback("");
  renderSelection();
  try {
    const result = await api(`/api/v1/targets/${encodeURIComponent(target.id)}/prepare`, {
      method: "POST",
    });
    state.readiness = [];
    showFeedback(
      `Prepared ${target.title}: ${result.ready_demos} demos ready, ${result.blocked_demos} blocked.`,
      false,
    );
    await refreshAll();
  } catch (error) {
    showFeedback(`Target preparation failed: ${error.message}`);
  } finally {
    state.preparingTargetId = null;
    renderSelection();
  }
}

async function stopRun() {
  const run = selectedRun();
  if (!run || !isLive(run.status)) return;
  elements.stopButton.disabled = true;
  try {
    await api(`/api/v1/runs/${encodeURIComponent(run.id)}/stop`, { method: "POST", body: "{}" });
    showFeedback(`Stop requested for ${run.id}`, false);
    await refreshAll();
  } catch (error) {
    showFeedback(`Stop failed: ${error.message}`);
  } finally {
    elements.stopButton.disabled = false;
  }
}

async function sendNodeInput(event) {
  event.preventDefault();
  const run = selectedRun();
  const nodeId = state.selectedNodeId;
  if (!run || !nodeId || !isLive(run.status) || state.sendingNodeInput) return;
  state.sendingNodeInput = true;
  state.nodeInputFeedback = `Sending to ${nodeId}...`;
  elements.nodeInput.disabled = true;
  elements.sendNodeInput.disabled = true;
  elements.nodeInputStatus.textContent = state.nodeInputFeedback;
  try {
    const result = await api(
      `/api/v1/runs/${encodeURIComponent(run.id)}/nodes/${encodeURIComponent(nodeId)}/input`,
      {
        method: "POST",
        body: JSON.stringify({ data: elements.nodeInput.value, append_newline: true }),
      },
    );
    elements.nodeInput.value = "";
    state.nodeInputFeedback = `Sent ${result.bytes_written} bytes to ${nodeId}`;
    elements.nodeInputStatus.textContent = state.nodeInputFeedback;
    elements.nodeInput.focus();
  } catch (error) {
    state.nodeInputFeedback = `Send failed: ${error.message}`;
    elements.nodeInputStatus.textContent = state.nodeInputFeedback;
  } finally {
    state.sendingNodeInput = false;
    renderRunView();
  }
}

/* ------------------------------------------------------------------ polling */

async function fetchLogChunk(run, cursor, nodeId) {
  const query = new URLSearchParams({ cursor: String(cursor) });
  if (nodeId) query.set("node", nodeId);
  return api(`/api/v1/runs/${encodeURIComponent(run.id)}/logs?${query}`);
}

async function refreshLogs() {
  const run = selectedRun();
  if (!run) return;
  const terminal = !isLive(run.status);
  const drain = terminal && state.resultDrainedFor !== run.id;
  try {
    if (!state.selectedNodeId) {
      let iterations = 0;
      let received = 0;
      do {
        const chunk = await fetchLogChunk(run, state.logCursor, null);
        received = chunk.lines.length;
        if (received) {
          state.logLines.push(...chunk.lines);
          if (state.logLines.length > 2000) state.logLines.splice(0, state.logLines.length - 2000);
          state.processLogCache = state.logLines.slice();
          renderLogText(state.logLines.join("\n"));
          if (elements.followLog.checked) elements.logOutput.scrollTop = elements.logOutput.scrollHeight;
        }
        state.logCursor = chunk.next_cursor;
        iterations += 1;
      } while (drain && received > 0 && iterations < 48);
    } else {
      const chunk = await fetchLogChunk(run, state.logCursor, state.selectedNodeId);
      if (chunk.lines.length) {
        state.logLines.push(...chunk.lines);
        if (state.logLines.length > 2000) state.logLines.splice(0, state.logLines.length - 2000);
        renderLogText(state.logLines.join("\n"));
        if (elements.followLog.checked) elements.logOutput.scrollTop = elements.logOutput.scrollHeight;
      }
      state.logCursor = chunk.next_cursor;
      if (drain && !state.processLogCache.length) {
        let cursor = 0;
        let received = 0;
        let iterations = 0;
        do {
          const processChunk = await fetchLogChunk(run, cursor, null);
          received = processChunk.lines.length;
          state.processLogCache.push(...processChunk.lines);
          if (state.processLogCache.length > 2000) {
            state.processLogCache.splice(0, state.processLogCache.length - 2000);
          }
          cursor = processChunk.next_cursor;
          iterations += 1;
        } while (received > 0 && iterations < 48);
      }
    }
    if (drain) state.resultDrainedFor = run.id;
    if (elements.feedback.dataset.source === "log-refresh") showFeedback("");
  } catch (error) {
    showFeedback(`Log refresh failed: ${error.message}`, true, "log-refresh");
  }
  renderRunView();
}

async function refreshAll() {
  if (state.refreshing) return;
  state.refreshing = true;
  try {
    await api("/api/v1/health");
    setHealth(true, "Backend ready");
    if (!state.catalog.length) {
      const catalog = await api("/api/v1/catalog");
      state.catalog = catalog.demos;
      if (!state.selectedDemoId && state.catalog.length) state.selectedDemoId = state.catalog[0].id;
      renderCategories();
    }
    if (!state.targets.length) {
      const registry = await api("/api/v1/targets");
      state.targets = registry.targets;
      state.defaultTargetId = registry.default_target;
      if (!state.selectedTargetId) state.selectedTargetId = registry.default_target;
    }
    if (!state.readiness.length) {
      const query = new URLSearchParams({ target: state.selectedTargetId });
      state.readiness = await api(`/api/v1/readiness?${query}`);
    }
    state.runs = await api("/api/v1/runs");
    if (!state.selectedRunId) {
      const activeRun = state.runs.find((run) => isLive(run.status));
      if (activeRun) {
        state.selectedRunId = activeRun.id;
        state.selectedDemoId = activeRun.demo_id;
        state.selectedTargetId = activeRun.target_id;
        state.readiness = [];
      }
    }
    if (state.selectedRunId && !state.runs.some((run) => run.id === state.selectedRunId)) {
      state.selectedRunId = null;
      state.selectedNodeId = null;
      resetLog();
      if (state.view === "run") setView("launch");
    }
    if (!state.userChoseView) {
      const activeRun = state.runs.find((run) => isLive(run.status));
      setView(activeRun ? "run" : "launch");
    }
    renderAll();
    await refreshLogs();
  } catch (error) {
    setHealth(false, "Backend unavailable");
    showFeedback(`Refresh failed: ${error.message}`);
  } finally {
    state.refreshing = false;
  }
}

function renderAll() {
  renderCatalog();
  renderSelection();
  renderRunView();
  renderRuns();
}

function setStatusBadge(element, status) {
  element.className = `status-badge ${status}`;
  element.textContent = status;
}

function nodeRole(demo, index) {
  if (demo.topology === "pipeline") return `pipeline stage ${index}`;
  if (demo.topology === "service") return index === 0 ? "service" : "client";
  if (demo.topology === "pair") return index === 0 ? "initiator" : "peer";
  return `mesh peer ${index}`;
}

function runTiming(run) {
  const start = run.started_at_ms || run.created_at_ms;
  const end = run.finished_at_ms || Date.now();
  return formatDuration(Math.max(0, end - start));
}

function shortRevision(revision) {
  return revision ? revision.slice(0, 12) : "unversioned";
}

function formatDuration(milliseconds) {
  const seconds = Math.max(0, Math.round(milliseconds / 1000));
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  const remainder = seconds % 60;
  if (minutes < 60) return `${minutes}m ${remainder}s`;
  const hours = Math.floor(minutes / 60);
  return `${hours}h ${minutes % 60}m`;
}

function formatTimestamp(milliseconds) {
  if (!milliseconds) return "-";
  const date = new Date(milliseconds);
  const pad = (value) => String(value).padStart(2, "0");
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

function titleCase(value) {
  return value.replaceAll("_", " ").replace(/\b\w/g, (letter) => letter.toUpperCase());
}

/* ------------------------------------------------------------------ events */

elements.catalogSearch.addEventListener("input", renderCatalog);
elements.executionTarget.addEventListener("change", () => {
  state.selectedTargetId = elements.executionTarget.value;
  state.readiness = [];
  renderAll();
  void refreshAll();
});
elements.refreshButton.addEventListener("click", () => {
  state.readiness = [];
  void refreshAll();
});
elements.backToCatalog.addEventListener("click", () => {
  setView("launch", true);
});
elements.liveRunPill.addEventListener("click", () => {
  const liveRun = state.runs.find((run) => isLive(run.status));
  if (liveRun) selectRun(liveRun.id);
});
elements.startButton.addEventListener("click", startRun);
elements.stopButton.addEventListener("click", stopRun);
elements.processLog.addEventListener("click", () => selectLogSource(null));
elements.nodeInputForm.addEventListener("submit", sendNodeInput);
elements.clearLog.addEventListener("click", () => {
  state.logLines = [];
  renderLogText("View cleared. New output will continue from the current cursor.");
});

elements.logResizeHandle.addEventListener("pointerdown", (event) => {
  event.preventDefault();
  const startY = event.clientY;
  const startHeight = elements.logBand.getBoundingClientRect().height;
  const maxHeight = Math.max(220, window.innerHeight - 260);
  document.body.classList.add("log-resizing");
  elements.logResizeHandle.setPointerCapture(event.pointerId);
  const onMove = (moveEvent) => {
    const next = Math.min(maxHeight, Math.max(170, startHeight + (startY - moveEvent.clientY)));
    elements.logBand.style.height = `${next}px`;
  };
  const onUp = () => {
    document.body.classList.remove("log-resizing");
    elements.logResizeHandle.removeEventListener("pointermove", onMove);
    elements.logResizeHandle.removeEventListener("pointerup", onUp);
    elements.logResizeHandle.removeEventListener("pointercancel", onUp);
  };
  elements.logResizeHandle.addEventListener("pointermove", onMove);
  elements.logResizeHandle.addEventListener("pointerup", onUp);
  elements.logResizeHandle.addEventListener("pointercancel", onUp);
});

setView("launch");
void refreshAll();
setInterval(refreshAll, 1000);
