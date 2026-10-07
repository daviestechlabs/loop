export const INTERACTION_PHASES = [
  "idle",
  "connecting",
  "listening",
  "committing",
  "thinking",
  "speaking",
  "interrupting",
  "recovering",
  "completed",
  "failed",
] as const;

export type InteractionPhase = (typeof INTERACTION_PHASES)[number];
export type InteractionInputMode = "audio" | "text";

export const INTERACTION_EVENT_TYPES = [
  "start",
  "connected",
  "listening",
  "voice_activity",
  "commit",
  "thinking",
  "response_text",
  "speaking",
  "interrupt",
  "canceled",
  "recover",
  "complete",
  "fail",
  "transport_degraded",
  "reset",
] as const;

export type InteractionEventType = (typeof INTERACTION_EVENT_TYPES)[number];

export type InteractionRuntimeEvent = {
  type: InteractionEventType;
  at?: number;
  turnId?: string | undefined;
  inputMode?: InteractionInputMode;
  transport?: string;
  audioKernel?: string;
  reason?: string;
  error?: string;
  metadata?: Record<string, string | number | boolean | null | undefined>;
};

export type InteractionMilestones = {
  startedAt?: number;
  connectedAt?: number;
  firstVoiceAt?: number;
  committedAt?: number;
  thinkingAt?: number;
  firstTextAt?: number;
  firstAudioAt?: number;
  interruptedAt?: number;
  canceledAt?: number;
  recoveredAt?: number;
  completedAt?: number;
  failedAt?: number;
};

export type InteractionLatency = {
  connectMs?: number;
  utteranceMs?: number;
  firstTextMs?: number;
  firstAudioMs?: number;
  interruptMs?: number;
  totalMs?: number;
};

export type InteractionTransition = {
  sequence: number;
  at: number;
  event: InteractionEventType;
  from: InteractionPhase;
  to: InteractionPhase;
  turnId: string;
  reason?: string;
};

export type InteractionSnapshot = {
  phase: InteractionPhase;
  sequence: number;
  turnId: string;
  inputMode: InteractionInputMode;
  transport: string;
  audioKernel: string;
  degraded: boolean;
  lastError: string;
  context: Record<string, string>;
  milestones: InteractionMilestones;
  latency: InteractionLatency;
  history: InteractionTransition[];
};

export type InteractionRuntime = {
  dispatch(event: InteractionRuntimeEvent): InteractionSnapshot;
  getSnapshot(): InteractionSnapshot;
  subscribe(listener: (snapshot: InteractionSnapshot) => void): () => void;
};

export type InteractionRuntimeOptions = {
  now?: () => number;
  historyLimit?: number;
};

const DEFAULT_HISTORY_LIMIT = 32;

function elapsed(start: number | undefined, end: number | undefined): number | undefined {
  if (start === undefined || end === undefined) return undefined;
  return Math.max(0, Math.round((end - start) * 1000) / 1000);
}

function latencyFor(milestones: InteractionMilestones): InteractionLatency {
  const finishedAt = milestones.completedAt ?? milestones.failedAt ?? milestones.canceledAt;
  const latency: InteractionLatency = {};
  const values: Array<[keyof InteractionLatency, number | undefined]> = [
    ["connectMs", elapsed(milestones.startedAt, milestones.connectedAt)],
    ["utteranceMs", elapsed(milestones.firstVoiceAt, milestones.committedAt)],
    ["firstTextMs", elapsed(milestones.committedAt ?? milestones.startedAt, milestones.firstTextAt)],
    ["firstAudioMs", elapsed(milestones.committedAt ?? milestones.startedAt, milestones.firstAudioAt)],
    ["interruptMs", elapsed(milestones.interruptedAt, milestones.canceledAt)],
    ["totalMs", elapsed(milestones.startedAt, finishedAt)],
  ];
  for (const [key, value] of values) {
    if (value !== undefined) latency[key] = value;
  }
  return latency;
}

function phaseFor(
  current: InteractionPhase,
  event: InteractionRuntimeEvent,
  inputMode: InteractionInputMode,
): InteractionPhase {
  switch (event.type) {
    case "start":
      return "connecting";
    case "connected":
      return inputMode === "text" ? "thinking" : "listening";
    case "listening":
    case "voice_activity":
      return "listening";
    case "commit":
      return "committing";
    case "thinking":
    case "response_text":
      return current === "speaking" ? "speaking" : "thinking";
    case "speaking":
      return "speaking";
    case "interrupt":
      return "interrupting";
    case "canceled":
      return "recovering";
    case "recover":
      return event.inputMode === "audio" ? "listening" : "idle";
    case "complete":
      return "completed";
    case "fail":
      return "failed";
    case "reset":
      return "idle";
    case "transport_degraded":
      return current;
  }
}

function milestoneFor(
  milestones: InteractionMilestones,
  event: InteractionRuntimeEvent,
  at: number,
): InteractionMilestones {
  const next = { ...milestones };
  const setOnce = (key: keyof InteractionMilestones) => {
    if (next[key] === undefined) next[key] = at;
  };
  switch (event.type) {
    case "start":
      return { startedAt: at };
    case "connected":
      setOnce("connectedAt");
      break;
    case "voice_activity":
      setOnce("firstVoiceAt");
      break;
    case "commit":
      setOnce("committedAt");
      break;
    case "thinking":
      setOnce("thinkingAt");
      break;
    case "response_text":
      setOnce("firstTextAt");
      break;
    case "speaking":
      setOnce("firstAudioAt");
      break;
    case "interrupt":
      setOnce("interruptedAt");
      break;
    case "canceled":
      setOnce("canceledAt");
      break;
    case "recover":
      setOnce("recoveredAt");
      break;
    case "complete":
      setOnce("completedAt");
      break;
    case "fail":
      setOnce("failedAt");
      break;
    case "listening":
    case "transport_degraded":
    case "reset":
      break;
  }
  return next;
}

function normalizeMetadata(
  metadata: InteractionRuntimeEvent["metadata"],
): Record<string, string> {
  if (!metadata) return {};
  return Object.fromEntries(
    Object.entries(metadata).flatMap(([key, value]) =>
      value === null || value === undefined ? [] : [[key, String(value)]],
    ),
  );
}

function cloneSnapshot(snapshot: InteractionSnapshot): InteractionSnapshot {
  return {
    ...snapshot,
    context: { ...snapshot.context },
    milestones: { ...snapshot.milestones },
    latency: { ...snapshot.latency },
    history: snapshot.history.map((entry) => ({ ...entry })),
  };
}

export function createInteractionRuntime(
  options: InteractionRuntimeOptions = {},
): InteractionRuntime {
  const now = options.now ?? (() => performance.now());
  const historyLimit = Math.max(1, options.historyLimit ?? DEFAULT_HISTORY_LIMIT);
  const listeners = new Set<(snapshot: InteractionSnapshot) => void>();
  let snapshot: InteractionSnapshot = {
    phase: "idle",
    sequence: 0,
    turnId: "",
    inputMode: "audio",
    transport: "unknown",
    audioKernel: "unknown",
    degraded: false,
    lastError: "",
    context: {},
    milestones: {},
    latency: {},
    history: [],
  };

  const emit = () => {
    const value = cloneSnapshot(snapshot);
    for (const listener of listeners) listener(value);
    return value;
  };

  return {
    dispatch(event) {
      const at = Number.isFinite(event.at) ? Number(event.at) : now();
      if (
        event.type !== "start" &&
        event.turnId &&
        snapshot.turnId &&
        event.turnId !== snapshot.turnId &&
        !(event.type === "connected" && snapshot.phase === "connecting")
      ) {
        return cloneSnapshot(snapshot);
      }
      const from = snapshot.phase;
      const isNewTurn = event.type === "start";
      const sequence = snapshot.sequence + 1;
      const milestones = milestoneFor(isNewTurn ? {} : snapshot.milestones, event, at);
      const inputMode = event.inputMode ?? snapshot.inputMode;
      const to = phaseFor(from, event, inputMode);
      const turnId = event.turnId ?? (isNewTurn ? "" : snapshot.turnId);
      const context = {
        ...(isNewTurn ? {} : snapshot.context),
        ...normalizeMetadata(event.metadata),
      };
      const transition: InteractionTransition = {
        sequence,
        at,
        event: event.type,
        from,
        to,
        turnId,
        ...(event.reason ? { reason: event.reason } : {}),
      };
      const duplicateMilestone =
        (event.type === "voice_activity" && snapshot.milestones.firstVoiceAt !== undefined) ||
        (event.type === "response_text" && snapshot.milestones.firstTextAt !== undefined) ||
        (event.type === "speaking" && snapshot.milestones.firstAudioAt !== undefined);
      const history = duplicateMilestone
        ? snapshot.history
        : [...(isNewTurn ? [] : snapshot.history), transition].slice(-historyLimit);

      snapshot = {
        phase: to,
        sequence,
        turnId,
        inputMode,
        transport: event.transport ?? snapshot.transport,
        audioKernel: event.audioKernel ?? snapshot.audioKernel,
        degraded: isNewTurn ? false : snapshot.degraded || event.type === "transport_degraded",
        lastError: event.type === "fail" ? event.error ?? event.reason ?? "interaction failed" : isNewTurn ? "" : snapshot.lastError,
        context,
        milestones,
        latency: latencyFor(milestones),
        history,
      };
      if (event.type === "reset") {
        snapshot = {
          ...snapshot,
          turnId: "",
          degraded: false,
          lastError: "",
          context: {},
          milestones: {},
          latency: {},
        };
      }
      return emit();
    },

    getSnapshot() {
      return cloneSnapshot(snapshot);
    },

    subscribe(listener) {
      listeners.add(listener);
      listener(cloneSnapshot(snapshot));
      return () => listeners.delete(listener);
    },
  };
}

export function replayInteractionTrace(
  events: readonly InteractionRuntimeEvent[],
): InteractionSnapshot {
  const runtime = createInteractionRuntime({ now: () => 0 });
  for (const event of events) runtime.dispatch(event);
  return runtime.getSnapshot();
}

export function interactionPhasePresentation(phase: InteractionPhase): {
  label: string;
  detail: string;
} {
  switch (phase) {
    case "connecting":
      return { label: "Opening the channel", detail: "Negotiating the direct local voice path" };
    case "listening":
      return { label: "Listening", detail: "The companion is present and interruption-ready" };
    case "committing":
      return { label: "Holding the moment", detail: "Speech is committed to the local intelligence stack" };
    case "thinking":
      return { label: "Shaping the next beat", detail: "Planning, memory, and inference are active" };
    case "speaking":
      return { label: "In the scene", detail: "Voice is streaming and remains interruptible" };
    case "interrupting":
      return { label: "Yielding", detail: "The current response is stopping now" };
    case "recovering":
      return { label: "Rejoining the scene", detail: "Speech onset is preserved while the next turn opens" };
    case "completed":
      return { label: "Beat complete", detail: "The companion is ready for the next action" };
    case "failed":
      return { label: "Channel faltered", detail: "The interaction can recover without losing the session" };
    default:
      return { label: "Present", detail: "The local realm engine is standing by" };
  }
}
