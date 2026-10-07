import { LiveMicrophone, LiveTurns, type LiveFrames } from './live';
import { VoiceTurn, voiceCapability, type Phase } from './voice';
import { anvilBundle, gatewaySTTInterval, gatewayMetadata, gatewayTokenUsage, reportedMilliseconds, api, APIError, CaptureSubmission, Recorder, type FailureCapture, type FinalRecord, type TurnRecord, type TurnSummary } from './evidence';
import { showAnvilReferences } from './anvil-evidence';
import { browserTimings, responseTimeline, formatMilliseconds } from './timing';
function find<T extends HTMLElement>(selector: string): T {
  const value = document.querySelector(selector); if (!value) throw new Error(`Missing ${selector}`); return value as T;
}
const app = find('.app'), mic = find<HTMLButtonElement>('.mic'), partial = find('.partial');
const mode = find('.mode'), meter = find('#wt-meter'), feed = find('.feed');
const consent = find<HTMLInputElement>('#record-consent'), stop = find<HTMLButtonElement>('.interrupt');
const retry = find<HTMLButtonElement>('#retry-save');
const transport = find<HTMLSelectElement>('#input-transport');
type InputTransport = 'datagram' | 'stream';
function selectedTransport(): InputTransport {
  if (transport.value !== 'datagram' && transport.value !== 'stream') throw new Error('Select a supported audio transport.');
  return transport.value;
}
let signedIn = false;
let conversation: { sessionId: string; microphone: LiveMicrophone; turns: LiveTurns; inputTransport: InputTransport } | null = null;
let selected: TurnRecord | null = null;
let inspectionController: AbortController | null = null;
let active: { controller: AbortController; voice: VoiceTurn | null; recorder: Recorder } | null = null;
let unsaved: { recorder: Recorder; final: FinalRecord } | null = null;
const rows = new Map<string, HTMLElement>();
let audioUrl: string | null = null;
function clearPlayback(): void {
  const player = document.getElementById('recorded-playback') as HTMLAudioElement | null;
  player?.pause(); player?.remove();
  if (audioUrl) URL.revokeObjectURL(audioUrl); audioUrl = null;
}
function text(tag: string, value: string, className = ''): HTMLElement {
  const el = document.createElement(tag); el.textContent = value; el.className = className; return el;
}
function message(value: string): void { partial.textContent = value; partial.hidden = !value; }
function errorMessage(error: unknown): string { return error instanceof Error ? error.message : String(error); }
function setMode(value: string): void {
  app.dataset.mode = value; mode.textContent = value === 'idle' ? 'ready' : value;
  find('.prompts').hidden = value !== 'idle'; stop.hidden = !active && !conversation;
  find('.mic-label').textContent = conversation ? 'stop' : 'talk';
  mic.setAttribute('aria-label', conversation ? 'Stop live conversation' : 'Start live conversation');
  mic.setAttribute('aria-pressed', String(Boolean(conversation)));
}
function controls(): void {
  mic.disabled = !conversation && (!signedIn || Boolean(active || unsaved) || Boolean(voiceCapability()));
  transport.disabled = !signedIn || Boolean(active || conversation || unsaved);
  consent.disabled = Boolean(active || conversation); retry.hidden = !unsaved; stop.hidden = !active && !conversation;
  const input = selected?.audio.find((audio) => audio.kind === 'input');
  for (const button of document.querySelectorAll<HTMLButtonElement>('#rail-rerun, [data-act="rerun"]')) {
    button.textContent = 'rerun saved input';
    button.title = 'Send saved audio through the active gateway model and live tool policy; save a separate result.';
    button.disabled = Boolean(active || conversation || unsaved || !signedIn || voiceCapability(true) || selected?.status === 'recording' || !input || input.sample_rate !== 16000 || input.bytes > 960000 || input.bytes % 640);
  }
}
function putTurn(turn: TurnSummary): HTMLElement {
  feed.querySelector('.empty-feed')?.remove();
  let el = rows.get(turn.id);
  if (!el) {
    el = document.createElement('article'); el.className = 'turn'; el.tabIndex = 0; el.dataset.turnId = turn.id;
    rows.set(turn.id, el); feed.prepend(el);
    el.addEventListener('click', () => { void inspect(turn.id).catch((e: unknown) => message(errorMessage(e))); });
    el.addEventListener('keydown', (event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); el!.click(); } });
  }
  const header = text('header', '', 'turn-meta'); header.append(text('span', new Date(turn.created * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })), text('span', turn.status), text('span', turn.id, 'tid'));
  const pending = active?.recorder.id === turn.id ? 'Connecting…' : turn.status === 'recording' ? 'Recording is unfinished.' : 'No final response recorded.';
  const talk = text('div', '', 'talk'); talk.append(text('p', turn.final?.transcript || '—', 'q'), text('p', turn.final?.answer || turn.final?.error || pending, 'a'));
  el.replaceChildren(header, talk, text('p', `${turn.purpose === 'saved_input_rerun' ? 'Saved input rerun · ' : ''}Select turn to inspect model identity and token usage.`, 'foot'));
  el.classList.toggle('fail', turn.status === 'failed'); return el;
}
function paint(record: TurnRecord): void {
  inspectionController?.abort(); inspectionController = null;
  if (selected?.id !== record.id) clearPlayback();
  selected = record;
  for (const [id, row] of rows) row.classList.toggle('on', id === record.id);
  find('#path-label').textContent = `${record.status} · ${record.id}`;
  find('#rail-id').textContent = 'Model identity not reported'; find('#rail-burn').textContent = 'Usage not reported';
  const meta = gatewayMetadata(record.events);
  const stageKeys = [['listen','stt_ms'],['plan','planner_ms'],['tool','tool_ms'],['think','llm_ms'],['speak','tts_ms']] as const;
  const table = find('#rail-path'); table.replaceChildren();
  for (const [stage,key] of stageKeys) {
    const derivedSTT = key === 'stt_ms' && !Object.hasOwn(meta, key);
    const n = derivedSTT ? gatewaySTTInterval(record.events) : reportedMilliseconds(meta[key]);
    const value = n !== null ? `${n}ms` : '—';
    const row = document.createElement('tr'); row.append(text('th', stage), text('td', value), text('td', value === '—' ? 'not reported' : derivedSTT ? 'STT receipt to transcript · gateway observation' : 'gateway observation'));
    table.append(row);
  }
  const timings = browserTimings(record.events);
  find('#e2e-meter').textContent = formatMilliseconds(timings.finished);
  find('#ttft-meter').textContent = formatMilliseconds(timings.text);
  for (const [label, value] of [['first text', timings.text], ['first audio', timings.audio], ['playback start', timings.scheduled], ['playback finish', timings.finished], ['interruption', timings.interruption]] as const) {
    const row = document.createElement('tr');
    row.append(text('th', label), text('td', formatMilliseconds(value)), text('td', value === null ? 'not observed'
      : label === 'interruption' ? 'request to local playback stop · browser observation' : 'input completion requested to event · browser observation'));
    table.append(row);
  }
  const waterfall = find('#rail-waterfall'), timeline = responseTimeline(record.events);
  waterfall.replaceChildren(); waterfall.classList.toggle('empty', !timeline);
  if (timeline) for (const segment of timeline) {
    const bar = document.createElement('i'); bar.className = segment.className;
    bar.style.flexGrow = String(segment.milliseconds); bar.title = `${segment.label} ${formatMilliseconds(segment.milliseconds)}`;
    waterfall.append(bar);
  }
  find('#waterfall-legend').textContent = timeline
    ? `${timeline.map((segment) => `${segment.label} ${formatMilliseconds(segment.milliseconds)}`).join(' · ')} · browser observation`
    : 'Complete browser response timeline not observed.';
  if (typeof meta.model_id === 'string' && meta.model_id.trim()) find('#rail-id').textContent = `${meta.model_id} · ${meta.model_identity_source === 'provider_reported' ? 'provider reported' : 'gateway observation'}`;
  const usage = gatewayTokenUsage(record.events);
  const usageLabel = usage ? `${usage.input} input · ${usage.output} output · ${usage.total} total LLM tokens · provider reported` : 'Usage not reported';
  find('#rail-burn').textContent = usageLabel;
  find('#token-count').textContent = usage ? String(usage.total) : '—';
  const footer = rows.get(record.id)?.querySelector('.foot');
  if (footer) footer.textContent = `${record.purpose === 'saved_input_rerun' ? 'Saved input rerun · ' : ''}${find('#rail-id').textContent} · ${usageLabel}`;
  find<HTMLButtonElement>('#rail-trace').disabled = false;
  find<HTMLButtonElement>('#rail-trace').title = 'Inspect recorded evidence';
  find<HTMLButtonElement>('#rail-replay').disabled = !record.audio.some((audio) => audio.kind === 'output');
  find<HTMLButtonElement>('#rail-replay').title = 'Play verified saved response audio';
  controls();
}
const localRecords = new Map<string, TurnRecord>();
async function inspect(id: string): Promise<void> {
  inspectionController?.abort();
  const controller = new AbortController(); inspectionController = controller;
  selected = null; clearPlayback();
  for (const [rowId, row] of rows) row.classList.toggle('on', rowId === id);
  find('#path-label').textContent = `Loading · ${id}`;
  find('#rail-id').textContent = 'Loading turn…'; find('#rail-burn').textContent = 'Usage not reported';
  for (const selector of ['#e2e-meter', '#ttft-meter', '#token-count']) find(selector).textContent = '—';
  find('#rail-path').replaceChildren();
  const waterfall = find('#rail-waterfall'); waterfall.replaceChildren(); waterfall.classList.add('empty');
  find('#waterfall-legend').textContent = 'Loading turn…';
  for (const selector of ['#rail-trace', '#rail-replay']) find<HTMLButtonElement>(selector).disabled = true;
  controls();
  try {
    const record = localRecords.get(id) ?? await api<TurnRecord>(`/api/turns/${id}`, undefined, controller.signal);
    if (controller.signal.aborted || inspectionController !== controller) return;
    if (record.id !== id) throw new Error('Recording selection mismatch');
    inspectionController = null; paint(record);
  } catch (error) {
    if (controller.signal.aborted || inspectionController !== controller) return;
    find('#path-label').textContent = `Could not load · ${id}`;
    find('#rail-id').textContent = 'Recording unavailable';
    find('#waterfall-legend').textContent = 'Recording unavailable';
    throw error;
  } finally {
    if (inspectionController === controller) { inspectionController = null; controls(); }
  }
}
async function loadTurns(): Promise<void> {
  const { turns } = await api<{ turns: TurnSummary[] }>('/api/turns');
  for (const turn of [...turns].reverse()) putTurn(turn);
}
async function start(source?: TurnRecord, liveInput?: LiveFrames, liveContext?: AudioContext): Promise<boolean> {
  if (active || !signedIn || unsaved || (source ? voiceCapability(true) : mic.disabled)) return false;
  const inputTransport = conversation?.inputTransport ?? selectedTransport();
  // Saved-input reruns are isolated from live conversations.
  const sessionId = source ? crypto.randomUUID() : conversation?.sessionId ?? crypto.randomUUID();
  const id = crypto.randomUUID(), controller = new AbortController(), recorder = new Recorder(id);
  const save = Boolean(source) || consent.checked;
  const captureModelRequest = consent.checked;
  const createdAt = Date.now(); const manifest = { request_id: id, recording_consent: save, model_request_consent: captureModelRequest,
    started_at: new Date(createdAt).toISOString(), client: { surface: 'loop', version: 'evaluation-iteration-1' },
    browser: { user_agent: navigator.userAgent }, purpose: 'human_voice' };
  const session = { controller, recorder, voice: null as VoiceTurn | null }; active = session;
  let created = false, transcript = '', answer = '', recordingError: string | null = null, error: string | null = null;
  let outcome: FinalRecord['status'] = 'failed';
  const view: TurnRecord = { purpose: source ? 'saved_input_rerun' : 'human_voice', id, created: createdAt / 1000, status: 'recording', manifest, manifest_sha256: '', final: null, final_sha256: null, evidence_origin: 'browser_observed', events: [], audio: [] };
  localRecords.set(id, view); putTurn(view); paint(view); setMode('connecting'); meter.textContent = 'connecting';
  controls(); message('Connecting… Click stop to end the conversation.');
  try {
    recorder.record({ name: 'conversation_bound', source: 'browser', payload: { session_id: sessionId } });
    recorder.record({ name: 'input_transport_selected', source: 'browser', payload: { input_transport: inputTransport } });
    let recordedInput: Uint8Array | undefined;
    if (source) {
      const plan = await api<{ request_id: string; manifest: Record<string, unknown>; source_audio: { sha256: string; bytes: number; sample_rate: number } }>(`/api/turns/${source.id}/reruns`, { request_id: id, mode: 'full_system_active_model', model_request_consent: captureModelRequest });
      created = true; view.manifest = plan.manifest;
      if (plan.request_id !== id || plan.source_audio.sample_rate !== 16000) throw new Error('Invalid rerun admission');
      const response = await fetch(`/api/turns/${source.id}/replay/input.wav`, { credentials: 'same-origin', signal: AbortSignal.any([controller.signal, AbortSignal.timeout(15000)]) });
      if (!response.ok) throw new Error(`Saved input load failed (${response.status})`);
      const wav = await response.arrayBuffer();
      if (response.headers.get('content-type') !== 'audio/wav' || wav.byteLength !== plan.source_audio.bytes + 44) throw new Error('Saved input format mismatch');
      recordedInput = new Uint8Array(wav.slice(44));
      const hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', wav.slice(44))), (v) => v.toString(16).padStart(2, '0')).join('');
      if (hash !== plan.source_audio.sha256) throw new Error('Saved input digest mismatch');
      recorder.record({ name: 'rerun_admitted', source: 'browser', payload: { mode: 'full_system_active_model', source_turn: source.id, microphone_recaptured: false } });
    } else if (save) { await api('/api/turns', manifest); created = true; }
    if (controller.signal.aborted) throw new DOMException('Conversation stopped during setup', 'AbortError');
    const identity = await api<{ request_id: string; identity_token: string; web_transport_endpoint: string }>('/api/turn-identity', { request_id: id, capture_model_request: captureModelRequest }, controller.signal);
    if (identity.request_id !== id || !identity.identity_token || !identity.web_transport_endpoint?.startsWith('https://')) throw new Error('Invalid voice identity response');
    if (controller.signal.aborted) throw new DOMException('Conversation stopped during setup', 'AbortError');
    session.voice = new VoiceTurn({ sessionId, inputTransport, captureModelRequest, ...(recordedInput ? { recordedInput } : {}), ...(liveInput && liveContext ? { liveInput, audioContext: liveContext } : {}), requestId: id, signal: controller.signal, identity: identity.identity_token, endpoint: identity.web_transport_endpoint,
      onPhase: (phase: Phase) => { if (liveInput) conversation?.turns.setPhase(liveInput, phase); setMode(phase === 'closed' ? 'idle' : phase); meter.textContent = phase === 'connecting' ? 'connecting' : phase === 'closed' ? 'idle' : 'up'; if (phase === 'listening') message(source ? 'Replaying saved input through the active model…' : 'listening…'); },
      onObservation: (event) => {
        try { recorder.record(event); } catch (e) { recordingError = errorMessage(e); controller.abort(); }
        view.events = recorder.events.map((value) => ({ event: value, sha256: '', received: Date.now() / 1000 }));
        if (selected?.id === id) paint(view);
        if (save) void recorder.flush().catch(() => message('Evidence upload delayed. Keep this page open until saving succeeds.'));
      },
      onTranscript: (value) => { transcript = value; rows.get(id)!.querySelector('.q')!.textContent = value; message(value); },
      onText: (value) => { answer = value; rows.get(id)!.querySelector('.a')!.textContent = value; },
      onAudio: (kind, pcm, rate) => { if (save) recorder.addAudio(kind, pcm, rate); },
    });
    const result = await session.voice.run(); answer = result.displayText || result.text; outcome = 'completed';
  } catch (e) { outcome = controller.signal.aborted && !recordingError ? 'interrupted' : 'failed'; error = errorMessage(e); }
  finally {
    const final: FinalRecord = { status: outcome, event_count: recorder.events.length, transcript, answer, error, recording_error: recordingError };
    view.status = outcome; view.final = final; putTurn(view); paint(view); meter.textContent = 'idle';
    setMode(outcome === 'failed' ? 'error' : 'idle'); message(error || '');
    if (created) {
      unsaved = { recorder, final };
      try { await recorder.finish(final); unsaved = null; localRecords.delete(id); await inspect(id); }
      catch (e) { message(`Evidence is not fully saved. Keep this page open and retry. ${errorMessage(e)}`); }
    }
    active = null; controls();
  }
  return !unsaved && outcome !== 'failed';
}
function interrupt(): void {
  if (!active || active.controller.signal.aborted) return;
  try { active.recorder.record({ name: 'interrupt_requested', source: 'browser', payload: {} }); } catch { /* Existing capacity error remains recorded. */ }
  active.controller.abort();
}
function endConversation(): void {
  const current = conversation; conversation = null;
  current?.turns.stop(); current?.microphone.stop(); interrupt();
  setMode('idle'); controls();
}
async function beginConversation(): Promise<void> {
  if (conversation || active || mic.disabled) return;
  clearPlayback();
  const failed = (error: Error) => { endConversation(); setMode('error'); message(error.message); };
  const turns = new LiveTurns({
    run: async (input) => {
      if (!conversation) return false;
      const ok = await start(undefined, input, conversation.microphone.context);
      if (!ok) endConversation();
      return ok;
    },
    interrupt: (reason) => {
      if (active && !active.controller.signal.aborted) {
        try { active.recorder.record({ name: 'live_interruption_requested', source: 'browser', payload: { detection: reason === 'speech' ? 'local_audio_activity' : 'conversation_stop', server_acknowledged: false } }); } catch { /* Capacity errors remain failures. */ }
      }
      interrupt();
    },
    idle: () => { if (conversation) { setMode('listening'); message('Listening… Speak to start another turn.'); meter.textContent = 'idle'; } },
    fail: failed,
  });
  const microphone = new LiveMicrophone((pcm, rms) => turns.packet(pcm, rms), failed);
  const current = { sessionId: crypto.randomUUID(), microphone, turns, inputTransport: selectedTransport() }; conversation = current;
  setMode('connecting'); controls(); message('Opening microphone…');
  try {
    await microphone.start((event) => { if (event.name === 'microphone_settings_retry') message('Retrying default microphone settings…'); });
    if (conversation !== current) return;
    setMode('listening'); message('Listening… Speak naturally. Speak over a reply to interrupt it.');
  } catch (error) { if (conversation === current) failed(error instanceof Error ? error : new Error(String(error))); }
}
mic.addEventListener('click', () => { if (conversation) { endConversation(); message('Conversation stopped.'); } else void beginConversation().catch((error: unknown) => { endConversation(); setMode('error'); message(errorMessage(error)); }); });
mic.addEventListener('keydown', (event) => { if (event.repeat && (event.key === ' ' || event.key === 'Enter')) event.preventDefault(); });
stop.textContent = 'stop conversation';
stop.addEventListener('click', () => { endConversation(); message('Conversation stopped.'); });
window.addEventListener('pagehide', endConversation);
window.addEventListener('beforeunload', (e) => { if (active || conversation || unsaved) e.preventDefault(); });
retry.addEventListener('click', () => { void (async () => {
  if (!unsaved) return; const pending = unsaved;
  try { await pending.recorder.finish(pending.final); unsaved = null; localRecords.delete(pending.recorder.id); await inspect(pending.recorder.id); message('Evidence saved.'); }
  catch (e) { message(errorMessage(e)); } finally { controls(); }
})(); });
let dialogRecord: TurnRecord | null = null;
let closeAnvilReferences: (() => void) | null = null;
let bundleController: AbortController | null = null;
const captureDrafts = new Map<string, CaptureSubmission>();
async function loadCaptures(record: TurnRecord): Promise<void> {
  const list = find('#capture-list'); list.replaceChildren(text('li', 'Loading…'));
  try {
    const result = await api<{ captures: Array<{ definition: FailureCapture }> }>('/api/captures');
    if (dialogRecord !== record) return;
    const cases = result.captures.filter(({ definition }) => definition.source_turn === record.id);
    list.replaceChildren(...cases.map(({ definition }) => text('li', `${definition.title} · ${definition.category} · ${definition.id}\nExpected: ${definition.expected}`)));
    if (!cases.length) list.append(text('li', 'No matching case in the latest 50.'));
  } catch (e) { if (dialogRecord === record) list.replaceChildren(text('li', `Could not load cases: ${errorMessage(e)}`)); }
}
find('#rail-trace').addEventListener('click', () => {
  if (!selected) return;
  bundleController?.abort(); bundleController = null;
  dialogRecord = structuredClone(selected);
  const draft = captureDrafts.get(dialogRecord.id);
  const eligible = signedIn && dialogRecord.status !== 'recording' && Boolean(dialogRecord.final_sha256 && dialogRecord.manifest_sha256);
  closeAnvilReferences?.(); closeAnvilReferences = null;
  const references = find('#anvil-references');
  if (eligible) closeAnvilReferences = showAnvilReferences(references, dialogRecord);
  else references.textContent = 'Save the finalized recording first.';
  find<HTMLFormElement>('#anvil-export-form').reset();
  find<HTMLFieldSetElement>('#anvil-export-fields').disabled = !eligible;
  find('#anvil-export-status').textContent = eligible ? '' : 'Save the finalized recording first.';
  find<HTMLInputElement>('#capture-title').value = draft?.definition.title ?? '';
  find<HTMLInputElement>('#capture-category').value = draft?.definition.category ?? '';
  find<HTMLTextAreaElement>('#capture-expected').value = draft?.definition.expected ?? '';
  find<HTMLFieldSetElement>('#capture-fields').disabled = !eligible || Boolean(draft);
  find<HTMLButtonElement>('#capture-save').disabled = !eligible || Boolean(draft?.saved);
  find('#capture-status').textContent = draft?.saved ? `Saved case ${draft.definition.id}.` : !eligible ? 'Save the finalized recording first.' : draft ? 'Retry uses the same case ID and fields.' : '';
  find('#evidence-content').textContent = JSON.stringify(dialogRecord, null, 2);
  find<HTMLDialogElement>('#evidence-dialog').showModal();
  void loadCaptures(dialogRecord);
});
find<HTMLFormElement>('#capture-form').addEventListener('submit', (event) => { event.preventDefault(); void (async () => {
  const record = dialogRecord; if (!record) return;
  const button = find<HTMLButtonElement>('#capture-save'); button.disabled = true;
  try {
    let draft = captureDrafts.get(record.id);
    if (!draft) {
      draft = new CaptureSubmission(record, { title: find<HTMLInputElement>('#capture-title').value,
        category: find<HTMLInputElement>('#capture-category').value, expected: find<HTMLTextAreaElement>('#capture-expected').value });
      captureDrafts.set(record.id, draft);
    }
    find<HTMLFieldSetElement>('#capture-fields').disabled = true;
    find('#capture-status').textContent = 'Saving…'; await draft.save();
    if (dialogRecord !== record) return;
    find('#capture-status').textContent = `Saved case ${draft.definition.id}.`; await loadCaptures(record);
  } catch (e) {
    if (dialogRecord !== record) return;
    find('#capture-status').textContent = errorMessage(e) + (captureDrafts.has(record.id) ? ' Retry keeps the same case ID and fields.' : ''); button.disabled = false;
  }
})(); });
find<HTMLDialogElement>('#evidence-dialog').addEventListener('close', () => {
  closeAnvilReferences?.(); closeAnvilReferences = null;
  bundleController?.abort(); bundleController = null; dialogRecord = null;
});
find<HTMLFormElement>('#anvil-export-form').addEventListener('submit', (event) => { event.preventDefault(); void (async () => {
  const record = dialogRecord; if (!record || bundleController) return;
  const controller = new AbortController(); bundleController = controller;
  const fields = find<HTMLFieldSetElement>('#anvil-export-fields'); fields.disabled = true;
  find('#anvil-export-status').textContent = 'Verifying and packaging evidence…';
  try {
    const blob = await anvilBundle(record, { runId: find<HTMLInputElement>('#anvil-run-id').value.trim(),
      sourceRevision: find<HTMLInputElement>('#anvil-source').value.trim(), sequence: Number(find<HTMLInputElement>('#anvil-sequence').value) }, controller.signal);
    if (dialogRecord !== record || controller.signal.aborted) return;
    const url = URL.createObjectURL(blob), link = document.createElement('a');
    link.href = url; link.download = `loop-${record.id}-anvil.tar`; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    find('#anvil-export-status').textContent = 'Bundle verified. Extract it privately and import its -anvil.json report in Anvil.';
  } catch (error) {
    if (dialogRecord === record && !controller.signal.aborted) find('#anvil-export-status').textContent = errorMessage(error);
  } finally {
    if (bundleController === controller) { bundleController = null; if (dialogRecord === record) fields.disabled = false; }
  }
})(); });
find('#export-evidence').addEventListener('click', () => {
  if (!dialogRecord) return;
  const url = URL.createObjectURL(new Blob([JSON.stringify(dialogRecord, null, 2)], { type: 'application/json' }));
  const link = document.createElement('a'); link.href = url; link.download = `loop-${dialogRecord.id}.json`; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
});
find('#rail-replay').addEventListener('click', () => { void (async () => {
  const record = selected; const audio = record?.audio.find((value) => value.kind === 'output'); if (!record || !audio) return;
  try {
    const response = await fetch(`/api/turns/${record.id}/replay/output.wav`, { credentials: 'same-origin', signal: AbortSignal.timeout(15000) });
    if (!response.ok) throw new Error(`Audio load failed (${response.status})`);
    const recording = await response.arrayBuffer();
    if (response.headers.get('content-type') !== 'audio/wav' || recording.byteLength !== audio.bytes + 44) throw new Error('Invalid recorded WAV');
    const pcm = recording.slice(44);
    const hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', pcm)), (v) => v.toString(16).padStart(2, '0')).join('');
    if (hash !== audio.sha256 || pcm.byteLength !== audio.bytes) throw new Error('Audio digest mismatch');
    if (selected?.id !== record.id) return;
    clearPlayback();
    audioUrl = URL.createObjectURL(new Blob([recording], { type: 'audio/wav' }));
    const player = document.createElement('audio'); player.id = 'recorded-playback'; player.controls = true; player.src = audioUrl;
    find('#rail-burn').after(player);
  } catch (e) { message(errorMessage(e)); }
})(); });
for (const button of document.querySelectorAll<HTMLButtonElement>('#rail-rerun, [data-act="rerun"]')) button.addEventListener('click', () => {
  if (!button.disabled && selected) void start(selected);
});
const mobileLayout = matchMedia('(max-width: 960px)');
function setInspector(open: boolean, restoreFocus = false): void {
  app.dataset.sheet = open ? 'open' : 'closed';
  find('#sheet-toggle').setAttribute('aria-expanded', String(open));
  find('.sheet-label').textContent = open ? 'Close inspection' : 'Inspect turn';
  find('.rail').inert = mobileLayout.matches && !open;
  if (open && mobileLayout.matches) find<HTMLButtonElement>('#sheet-close').focus();
  else if (restoreFocus && mobileLayout.matches) find<HTMLButtonElement>('#sheet-toggle').focus();
}
find('#sheet-toggle').addEventListener('click', () => setInspector(app.dataset.sheet !== 'open'));
find('#sheet-close').addEventListener('click', () => setInspector(false, true));
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && app.dataset.sheet === 'open' && !find<HTMLDialogElement>('#evidence-dialog').open) setInspector(false, true);
});
mobileLayout.addEventListener('change', () => setInspector(false));
setInspector(false);
for (const tab of document.querySelectorAll<HTMLElement>('.sheet-tab')) tab.addEventListener('click', () => {
  find('.rail').dataset.sheet = tab.dataset.sheet!; setInspector(true);
  for (const other of document.querySelectorAll('.sheet-tab')) { other.classList.toggle('on', other === tab); other.setAttribute('aria-selected', String(other === tab)); }
});
void (async () => {
  try { await api('/api/session'); signedIn = true; find('#sign-in').hidden = true; await loadTurns(); }
  catch (e) { message(e instanceof APIError && e.status === 401 ? 'Sign in to use live voice.' : errorMessage(e)); }
  const missing = voiceCapability(); if (missing) { meter.textContent = 'unavailable'; message(missing); setMode('error'); }
  controls();
})();
