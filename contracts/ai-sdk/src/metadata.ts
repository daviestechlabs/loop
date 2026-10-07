export const INTERACTION_PROFILES = [
  "realtime_voice",
  "detail_voice",
  "text_chat",
  "coder_agent",
  "dnd_app",
  "agent_task",
  "khelben_image",
  "khelben_session_image",
  "khelben_video",
] as const;

export type InteractionProfile = (typeof INTERACTION_PROFILES)[number];

export type InteractionModeConfig = {
  interactionProfile: InteractionProfile;
  voiceMode: string;
  turnProfile: string;
  enableTts: boolean;
  retrievalSkip: boolean;
  agentId?: string;
  taskIntent?: string;
};

export type MetadataValue = string | number | boolean | null | undefined;

export type TurnMetadataInput = {
  clientSurface: string;
  transport: string;
  interactionProfile: InteractionProfile;
  voiceMode: string;
  turnProfile: string;
  turnKind: "voice" | "chat" | "task";
  traceId?: string;
  retrievalSkip?: boolean;
  agentId?: string;
  taskIntent?: string;
  budgetMs?: number;
  deadlineUnixMs?: number;
  maxTokens?: string | number;
  inputMode?: string;
  capabilityId?: string;
  parentBundle?: string;
  promptHash?: string;
  extra?: Record<string, MetadataValue>;
};

export type TurnRequestInput = {
  text: string;
  sessionId: string;
  userId: string;
  username: string;
  identityToken?: string;
  clientSurface: string;
  transport: string;
  interactionProfile: InteractionProfile;
  premium?: boolean;
  enableRag?: boolean;
  enableTts?: boolean;
  voiceId?: string;
  requestId?: string;
  systemPrompt?: string;
  traceId?: string;
  inputMode?: string;
  metadata?: Record<string, MetadataValue>;
};

export type TurnRequestPayload = {
  request_id?: string;
  identity_token?: string;
  user_id: string;
  session_id: string;
  username: string;
  text: string;
  premium: boolean;
  enable_rag: boolean;
  enable_tts: boolean;
  voice_id: string;
  system_prompt?: string;
  metadata: Record<string, string>;
};

const MODE_CONFIGS: Record<InteractionProfile, InteractionModeConfig> = {
  realtime_voice: {
    interactionProfile: "realtime_voice",
    voiceMode: "realtime",
    turnProfile: "realtime",
    enableTts: true,
    retrievalSkip: true,
  },
  detail_voice: {
    interactionProfile: "detail_voice",
    voiceMode: "detail",
    turnProfile: "detail",
    enableTts: true,
    retrievalSkip: false,
  },
  text_chat: {
    interactionProfile: "text_chat",
    voiceMode: "text",
    turnProfile: "text",
    enableTts: false,
    retrievalSkip: false,
  },
  coder_agent: {
    interactionProfile: "coder_agent",
    voiceMode: "agent",
    turnProfile: "coder_agent",
    enableTts: false,
    retrievalSkip: true,
    agentId: "waterdeep-coder",
    taskIntent: "coder_assist",
  },
  dnd_app: {
    interactionProfile: "dnd_app",
    voiceMode: "app",
    turnProfile: "dnd_app",
    enableTts: true,
    retrievalSkip: false,
    agentId: "dnd-agent",
    taskIntent: "dnd_action",
  },
  agent_task: {
    interactionProfile: "agent_task",
    voiceMode: "agent",
    turnProfile: "agent_task",
    enableTts: false,
    retrievalSkip: true,
    agentId: "waterdeep-coder",
    taskIntent: "background_task",
  },
  khelben_image: {
    interactionProfile: "khelben_image",
    voiceMode: "app",
    turnProfile: "khelben_image",
    enableTts: true,
    retrievalSkip: true,
    agentId: "khelben-image",
    taskIntent: "generate_dnd_map",
  },
  khelben_session_image: {
    interactionProfile: "khelben_session_image",
    voiceMode: "app",
    turnProfile: "khelben_session_image",
    enableTts: true,
    retrievalSkip: true,
    agentId: "khelben-image",
    taskIntent: "generate_session_illustration",
  },
  khelben_video: {
    interactionProfile: "khelben_video",
    voiceMode: "app",
    turnProfile: "khelben_video",
    enableTts: true,
    retrievalSkip: true,
    agentId: "khelben-video",
    taskIntent: "generate_dnd_cinematic",
  },
};

export function interactionModeConfig(profile: InteractionProfile): InteractionModeConfig {
  return { ...MODE_CONFIGS[profile] };
}

export function makeClientTraceId(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;
  return `${prefix}-${random}`;
}

export function buildTurnMetadata(input: TurnMetadataInput): Record<string, string> {
  const metadata: Record<string, string> = {};
  put(metadata, "turn_source", `${input.clientSurface}-${input.transport}`);
  put(metadata, "turn_kind", input.turnKind);
  put(metadata, "client_trace_id", input.traceId ?? makeClientTraceId(input.clientSurface));
  put(metadata, "client_transport", input.transport);
  put(metadata, "client_surface", input.clientSurface);
  put(metadata, "interaction_profile", input.interactionProfile);
  put(metadata, "voice_mode", input.voiceMode);
  put(metadata, "turn_profile", input.turnProfile);
  if (typeof input.retrievalSkip === "boolean") put(metadata, "retrieval_skip", input.retrievalSkip);
  put(metadata, "agent_id", input.agentId);
  put(metadata, "task_intent", input.taskIntent);
  put(metadata, "turn_budget_ms", input.budgetMs);
  put(metadata, "turn_deadline_unix_ms", input.deadlineUnixMs);
  put(metadata, "turn_max_tokens", input.maxTokens);
  put(metadata, "input_mode", input.inputMode);
  put(metadata, "capability_id", input.capabilityId);
  put(metadata, "parent_bundle", input.parentBundle);
  put(metadata, "prompt_hash", input.promptHash);
  for (const [key, value] of Object.entries(input.extra ?? {})) {
    put(metadata, key, value);
  }
  return metadata;
}

export function buildTurnRequestPayload(input: TurnRequestInput): TurnRequestPayload {
  const mode = interactionModeConfig(input.interactionProfile);
  const enableTts = input.enableTts ?? mode.enableTts;
  const metadataInput: TurnMetadataInput = {
    clientSurface: input.clientSurface,
    transport: input.transport,
    interactionProfile: mode.interactionProfile,
    voiceMode: mode.voiceMode,
    turnProfile: mode.turnProfile,
    turnKind: enableTts ? "voice" : "chat",
    retrievalSkip: mode.retrievalSkip,
  };
  if (input.traceId !== undefined) metadataInput.traceId = input.traceId;
  if (mode.agentId !== undefined) metadataInput.agentId = mode.agentId;
  if (mode.taskIntent !== undefined) metadataInput.taskIntent = mode.taskIntent;
  if (input.inputMode !== undefined) metadataInput.inputMode = input.inputMode;
  if (input.metadata !== undefined) metadataInput.extra = input.metadata;
  const metadata = buildTurnMetadata(metadataInput);
  const payload: TurnRequestPayload = {
    user_id: input.userId,
    session_id: input.sessionId,
    username: input.username,
    text: input.text,
    premium: Boolean(input.premium),
    enable_rag: Boolean(input.enableRag),
    enable_tts: enableTts,
    voice_id: enableTts ? input.voiceId ?? "" : "",
    metadata,
  };
  if (input.requestId) payload.request_id = input.requestId;
  if (input.identityToken) payload.identity_token = input.identityToken;
  if (input.systemPrompt !== undefined) payload.system_prompt = input.systemPrompt;
  return payload;
}

function put(target: Record<string, string>, key: string, value: MetadataValue): void {
  if (value === null || value === undefined || value === "") return;
  target[key] = String(value);
}
