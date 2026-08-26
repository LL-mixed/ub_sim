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
  parameterDrafts: {},
  refreshing: false,
  sendingNodeInput: false,
};

const elements = {
  apiIndicator: document.querySelector("#api-indicator"),
  apiStatus: document.querySelector("#api-status"),
  feedback: document.querySelector("#feedback"),
  refreshButton: document.querySelector("#refresh-button"),
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
  runStatus: document.querySelector("#run-status"),
  runElapsed: document.querySelector("#run-elapsed"),
  topology: document.querySelector("#topology"),
  nodeDetail: document.querySelector("#node-detail"),
  logBand: document.querySelector(".log-band"),
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

function showFeedback(message, error = true) {
  elements.feedback.hidden = !message;
  elements.feedback.textContent = message || "";
  elements.feedback.style.color = error ? "#713029" : "#075f50";
  elements.feedback.style.background = error ? "#fbe5e2" : "#dff1ec";
}

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
  state.selectedRunId = null;
  state.selectedNodeId = null;
  resetLog();
  renderCatalog();
  renderSelection();
  renderRunWorkspace();
  renderRuns();
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
  }

  const liveRun = state.runs.find((run) => isLive(run.status));
  elements.startButton.disabled = Boolean(liveRun) || !readiness?.ready;
  elements.startButton.title = liveRun
    ? `Run ${liveRun.id} is active`
    : readiness?.ready
      ? `Start ${demo.title} on ${target?.title || state.selectedTargetId}`
      : "Resolve launch readiness before starting";
}

function previewNodes(demo) {
  return Array.from({ length: demo.node_count }, (_, index) => ({
    id: `node${String.fromCharCode(65 + index)}`,
    label: `Node ${String.fromCharCode(65 + index)}`,
    status: "unknown",
    log_path: null,
  }));
}

function renderRunWorkspace() {
  const demo = selectedDemo();
  const run = selectedRun();
  if (!demo) return;
  const nodes = run ? run.nodes : previewNodes(demo);
  elements.runTitle.textContent = run
    ? `${run.demo_title} / ${run.target_id} / ${shortRevision(run.source_revision)} / ${run.id}`
    : `Topology preview / ${state.selectedTargetId || "no target"}`;
  setStatusBadge(elements.runStatus, run ? run.status : "idle");
  elements.runElapsed.textContent = run ? runTiming(run) : "Not started";
  elements.stopButton.hidden = !run || !isLive(run.status) || !(demo.controls || []).includes("stop");
  elements.topology.className = `topology ${demo.topology}`;
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
    role.textContent = nodeRole(demo, index);
    top.append(name, status);
    button.append(top, role);
    button.addEventListener("click", () => selectNode(node.id));
    elements.topology.append(button);
  });

  const node = nodes.find((item) => item.id === state.selectedNodeId);
  if (node) {
    elements.nodeDetail.textContent = `${node.label} / ${node.status} / ${node.log_path || "process log until node log is discovered"}`;
  } else {
    elements.nodeDetail.textContent = run
      ? "Select a node to inspect node-specific output."
      : "Preview only. Start the run to observe node state.";
  }
  elements.logTitle.textContent = node ? `${node.label} log` : "Process log";
  elements.processLog.classList.toggle("active", !node);
  elements.processLog.setAttribute("aria-pressed", String(!node));
  const nodeInputAvailable = Boolean(
    run &&
      node &&
      isLive(run.status) &&
      (demo.controls || []).includes("node_input"),
  );
  elements.nodeInputForm.hidden = !nodeInputAvailable;
  elements.logBand.classList.toggle("node-input-active", nodeInputAvailable);
  elements.nodeInputLabel.textContent = node ? `${node.label} input` : "Node input";
  elements.nodeInput.placeholder = node ? `Send a line to ${node.label}` : "Send a line";
  elements.nodeInput.disabled = !nodeInputAvailable || state.sendingNodeInput;
  elements.sendNodeInput.disabled = !nodeInputAvailable || state.sendingNodeInput;
}

function selectNode(nodeId) {
  selectLogSource(state.selectedNodeId === nodeId ? null : nodeId);
}

function selectLogSource(nodeId) {
  if (state.selectedNodeId === nodeId) return;
  state.selectedNodeId = nodeId;
  elements.nodeInputStatus.textContent = "";
  resetLog();
  renderRunWorkspace();
  void refreshLogs();
}

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
  renderCatalog();
  renderSelection();
  renderRunWorkspace();
  renderRuns();
  void refreshLogs();
}

function resetLog() {
  state.logCursor = 0;
  state.logLines = [];
  elements.logOutput.textContent = state.selectedRunId ? "Waiting for output..." : "No active run.";
}

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
  elements.nodeInput.disabled = true;
  elements.sendNodeInput.disabled = true;
  elements.nodeInputStatus.textContent = `Sending to ${nodeId}...`;
  try {
    const result = await api(
      `/api/v1/runs/${encodeURIComponent(run.id)}/nodes/${encodeURIComponent(nodeId)}/input`,
      {
        method: "POST",
        body: JSON.stringify({ data: elements.nodeInput.value, append_newline: true }),
      },
    );
    elements.nodeInput.value = "";
    elements.nodeInputStatus.textContent = `Sent ${result.bytes_written} bytes to ${nodeId}`;
    elements.nodeInput.focus();
  } catch (error) {
    elements.nodeInputStatus.textContent = `Send failed: ${error.message}`;
  } finally {
    state.sendingNodeInput = false;
    renderRunWorkspace();
  }
}

async function refreshLogs() {
  const run = selectedRun();
  if (!run) return;
  const query = new URLSearchParams({ cursor: String(state.logCursor) });
  if (state.selectedNodeId) query.set("node", state.selectedNodeId);
  try {
    const chunk = await api(`/api/v1/runs/${encodeURIComponent(run.id)}/logs?${query}`);
    if (chunk.lines.length) {
      state.logLines.push(...chunk.lines);
      if (state.logLines.length > 2000) state.logLines.splice(0, state.logLines.length - 2000);
      elements.logOutput.textContent = state.logLines.join("\n");
      if (elements.followLog.checked) elements.logOutput.scrollTop = elements.logOutput.scrollHeight;
    }
    state.logCursor = chunk.next_cursor;
  } catch (error) {
    showFeedback(`Log refresh failed: ${error.message}`);
  }
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
  renderRunWorkspace();
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

function titleCase(value) {
  return value.replaceAll("_", " ").replace(/\b\w/g, (letter) => letter.toUpperCase());
}

elements.catalogSearch.addEventListener("input", renderCatalog);
elements.executionTarget.addEventListener("change", () => {
  state.selectedTargetId = elements.executionTarget.value;
  state.selectedRunId = null;
  state.selectedNodeId = null;
  state.readiness = [];
  resetLog();
  renderAll();
  void refreshAll();
});
elements.refreshButton.addEventListener("click", () => {
  state.readiness = [];
  void refreshAll();
});
elements.startButton.addEventListener("click", startRun);
elements.stopButton.addEventListener("click", stopRun);
elements.processLog.addEventListener("click", () => selectLogSource(null));
elements.nodeInputForm.addEventListener("submit", sendNodeInput);
elements.clearLog.addEventListener("click", () => {
  state.logLines = [];
  elements.logOutput.textContent = "View cleared. New output will continue from the current cursor.";
});

void refreshAll();
setInterval(refreshAll, 1000);
