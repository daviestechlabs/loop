import { projectTurnProvenance } from "./turnProvenance.js";

export const FRAME_CONTROL_JSON = 0x01;
export const FRAME_TURN_EVENT_PROTO = 0x02;
export const FRAME_AUDIO_PACKET = 0x03;
export const CONTROL_PREFIX = new TextEncoder().encode("DTVA1:");
export const AUDIO_PREFIX = new TextEncoder().encode("DTVP1");
export const AUDIO_DATAGRAM_MAX_PAYLOAD = 1191;

const TURN_EVENT_NAMES: Record<number, TurnEventName> = {
  1: "started",
  2: "thinking_started",
  3: "thinking_ended",
  4: "text_delta",
  5: "text_completed",
  6: "tts_segment",
  7: "pcm_started",
  8: "pcm_chunk",
  9: "pcm_ended",
  10: "completed",
  11: "canceled",
  12: "failed",
};

export type TurnEventName =
  | "started"
  | "thinking_started"
  | "thinking_ended"
  | "text_delta"
  | "text_completed"
  | "tts_segment"
  | "pcm_started"
  | "pcm_chunk"
  | "pcm_ended"
  | "completed"
  | "canceled"
  | "failed"
  | "unknown";

export type DndEncounterParticipant = {
  id: string;
  name: string;
  initiative: number;
  maxHp: number;
  currentHp: number;
  conditions: string[];
};

export type DndEncounterState = {
  campaignId: string;
  encounterId: string;
  version: number;
  status: string;
  round: number;
  activeIndex: number;
  activeParticipantId: string;
  participants: DndEncounterParticipant[];
  toolCallId: string;
  outputSha256: string;
  operation: string;
};

export type DndCampaignCharacter = { id: string; name: string; kind: string; maxHp: number };
export type DndCampaignRoster = {
  campaignId: string;
  version: number;
  status: string;
  characters: DndCampaignCharacter[];
  toolCallId: string;
  outputSha256: string;
};
export type DndInitiativeRoll = {
  characterId: string;
  expression: string;
  rolls: number[];
  keptIndices: number[];
  total: number;
  entropySource: string;
};
export type DndInitiativeResult = {
  campaignVersion: number;
  campaignSha256: string;
  rolls: DndInitiativeRoll[];
};

export type TurnEvent = {
  requestId?: string;
  type: number;
  name: TurnEventName;
  state?: number;
  text: string;
  speechText: string;
  displayText: string;
  audio: Uint8Array;
  sampleRate: number;
  channels: number;
  bitDepth: number;
  sequence?: number;
  segmentIndex?: number;
  isFinal?: boolean;
  timestamp?: number;
  error: string;
  metadata: Record<string, string>;
  dndEncounterState?: DndEncounterState;
  dndCampaignRoster?: DndCampaignRoster;
  dndInitiativeResult?: DndInitiativeResult;
};

export type TurnStreamFrame = {
  frameType: number;
  payload: Uint8Array;
};

const readerBuffers = new WeakMap<ReadableStreamDefaultReader<Uint8Array>, Uint8Array>();

export function uuid(prefix = "turn"): string {
  if (globalThis.crypto?.randomUUID) {
    return globalThis.crypto.randomUUID();
  }
  return `${prefix}-${Math.random().toString(16).slice(2)}${Date.now().toString(16)}`;
}

export function writeFrame(frameType: number, payload: string | Uint8Array): Uint8Array {
  const body = typeof payload === "string" ? new TextEncoder().encode(payload) : payload;
  const out = new Uint8Array(5 + body.length);
  const view = new DataView(out.buffer);
  view.setUint8(0, frameType);
  view.setUint32(1, body.length, false);
  out.set(body, 5);
  return out;
}

export async function readFrame(reader: ReadableStreamDefaultReader<Uint8Array>): Promise<TurnStreamFrame | null> {
  const header = await readExact(reader, 5);
  if (!header) return null;
  const view = new DataView(header.buffer, header.byteOffset, header.byteLength);
  const frameType = view.getUint8(0);
  const length = view.getUint32(1, false);
  if (length === 0 || length > 1048576) throw new RangeError('Invalid turn frame length');
  const payload = await readExact(reader, length);
  if (!payload) throw new Error('Truncated turn frame');
  return { frameType, payload };
}

export function controlDatagram(payload: unknown): Uint8Array {
  const body = new TextEncoder().encode(JSON.stringify(payload));
  const out = new Uint8Array(CONTROL_PREFIX.length + body.length);
  out.set(CONTROL_PREFIX, 0);
  out.set(body, CONTROL_PREFIX.length);
  return out;
}

export function audioDatagram(sequence: number, pcm: Uint8Array): Uint8Array {
  if (!Number.isInteger(sequence) || sequence < 0 || sequence > 0xffffffff) {
    throw new RangeError("audio datagram sequence must be an unsigned 32-bit integer");
  }
  if (!(pcm instanceof Uint8Array) || pcm.byteLength === 0 ||
      pcm.byteLength > AUDIO_DATAGRAM_MAX_PAYLOAD || pcm.byteLength % 2 !== 0) {
    throw new RangeError("audio datagram PCM must be bounded, non-empty, and sample-aligned");
  }
  const out = new Uint8Array(AUDIO_PREFIX.length + 4 + pcm.byteLength);
  out.set(AUDIO_PREFIX, 0);
  new DataView(out.buffer).setUint32(AUDIO_PREFIX.length, sequence, false);
  out.set(pcm, AUDIO_PREFIX.length + 4);
  return out;
}

export function parseTurnEvent(bytes: Uint8Array, expectedRequestId?: string): TurnEvent {
  const event: TurnEvent = {
    type: 0,
    name: "unknown",
    text: "",
    speechText: "",
    displayText: "",
    audio: new Uint8Array(),
    sampleRate: 0,
    channels: 1,
    bitDepth: 16,
    error: "",
    metadata: {},
  };
  let offset = 0;
  let hasProvenance = false;
  let encounterBytes: Uint8Array | undefined;
  let rosterBytes: Uint8Array | undefined;
  let initiativeBytes: Uint8Array | undefined;
  while (offset < bytes.length) {
    const [tag, tagOffset] = readVarint(bytes, offset);
    offset = tagOffset;
    const fieldNumber = tag >> 3;
    const wireType = tag & 0x07;
    if (fieldNumber >= 19 && fieldNumber <= 23) hasProvenance = true;
    if (wireType === 0) {
      const [value, nextOffset] = readVarint(bytes, offset);
      offset = nextOffset;
      if (fieldNumber === 3) event.type = value;
      if (fieldNumber === 4) event.state = value;
      if (fieldNumber === 7) event.sampleRate = value;
      if (fieldNumber === 8) event.channels = value;
      if (fieldNumber === 9) event.bitDepth = value;
      if (fieldNumber === 10) event.sequence = value;
      if (fieldNumber === 11) event.segmentIndex = value;
      if (fieldNumber === 12) event.isFinal = value !== 0;
      if (fieldNumber === 13) event.timestamp = value;
      continue;
    }
    if (wireType === 2) {
      const [length, nextOffset] = readVarint(bytes, offset);
      offset = nextOffset;
      const value = bytes.slice(offset, offset + length);
      offset += length;
      if (fieldNumber === 1) event.requestId = new TextDecoder().decode(value);
      if (fieldNumber === 5) event.text = new TextDecoder().decode(value);
      if (fieldNumber === 6) event.audio = value;
      if (fieldNumber === 14) event.error = new TextDecoder().decode(value);
      if (fieldNumber === 16) {
        const entry = readStringMapEntry(value);
        if (entry) event.metadata[entry[0]] = entry[1];
      }
      if (fieldNumber === 17) event.speechText = new TextDecoder().decode(value);
      if (fieldNumber === 18) event.displayText = new TextDecoder().decode(value);
      if (fieldNumber === 19) encounterBytes = value;
      if (fieldNumber === 22) rosterBytes = value;
      if (fieldNumber === 23) initiativeBytes = value;
      continue;
    }
    break;
  }
  event.name = TURN_EVENT_NAMES[event.type] ?? "unknown";
  if (hasProvenance) {
    Object.assign(event.metadata, projectTurnProvenance(bytes, expectedRequestId ?? event.requestId ?? ""));
    // C validates the complete snapshot and receipt before this adapter copies it.
    if (encounterBytes) event.dndEncounterState = readDndEncounterState(encounterBytes);
    if (rosterBytes) event.dndCampaignRoster = readDndCampaignRoster(rosterBytes);
    if (initiativeBytes) event.dndInitiativeResult = readDndInitiativeResult(initiativeBytes);
  }
  return event;
}

function readDndEncounterState(bytes: Uint8Array): DndEncounterState {
  const state: DndEncounterState = {
    campaignId: "",
    encounterId: "",
    version: 0,
    status: "",
    round: 0,
    activeIndex: 0,
    activeParticipantId: "",
    participants: [],
    toolCallId: "",
    outputSha256: "",
    operation: "",
  };
  let offset = 0;
  while (offset < bytes.length) {
    const [tag, tagOffset] = readVarint(bytes, offset);
    offset = tagOffset;
    const fieldNumber = tag >> 3;
    const wireType = tag & 0x07;
    if (wireType === 0) {
      const [value, nextOffset] = readVarint(bytes, offset);
      offset = nextOffset;
      if (fieldNumber === 3) state.version = value;
      if (fieldNumber === 5) state.round = value;
      if (fieldNumber === 6) state.activeIndex = decodeZigZag32(value);
      continue;
    }
    if (wireType !== 2) break;
    const [length, nextOffset] = readVarint(bytes, offset);
    offset = nextOffset;
    const value = bytes.slice(offset, offset + length);
    offset += length;
    if (fieldNumber === 1) state.campaignId = decodeText(value);
    if (fieldNumber === 2) state.encounterId = decodeText(value);
    if (fieldNumber === 4) state.status = decodeText(value);
    if (fieldNumber === 7) state.activeParticipantId = decodeText(value);
    if (fieldNumber === 8) state.participants.push(readDndEncounterParticipant(value));
    if (fieldNumber === 9) state.toolCallId = decodeText(value);
    if (fieldNumber === 10) state.outputSha256 = decodeText(value);
    if (fieldNumber === 11) state.operation = decodeText(value);
  }
  return state;
}

function readDndEncounterParticipant(bytes: Uint8Array): DndEncounterParticipant {
  const participant: DndEncounterParticipant = {
    id: "",
    name: "",
    initiative: 0,
    maxHp: 0,
    currentHp: 0,
    conditions: [],
  };
  let offset = 0;
  while (offset < bytes.length) {
    const [tag, tagOffset] = readVarint(bytes, offset);
    offset = tagOffset;
    const fieldNumber = tag >> 3;
    const wireType = tag & 0x07;
    if (wireType === 0) {
      const [value, nextOffset] = readVarint(bytes, offset);
      offset = nextOffset;
      if (fieldNumber === 3) participant.initiative = decodeZigZag32(value);
      if (fieldNumber === 4) participant.maxHp = value;
      if (fieldNumber === 5) participant.currentHp = value;
      continue;
    }
    if (wireType !== 2) break;
    const [length, nextOffset] = readVarint(bytes, offset);
    offset = nextOffset;
    const value = bytes.slice(offset, offset + length);
    offset += length;
    if (fieldNumber === 1) participant.id = decodeText(value);
    if (fieldNumber === 2) participant.name = decodeText(value);
    if (fieldNumber === 6) participant.conditions.push(decodeText(value));
  }
  return participant;
}

// These adapters only copy fields after the shared C validator admits the event.
function readStateFields(bytes: Uint8Array, take: (field: number, value: number | Uint8Array) => void): void {
  let offset = 0;
  while (offset < bytes.length) {
    const [tag, tagEnd] = readVarint(bytes, offset);
    const [value, valueEnd] = readVarint(bytes, tagEnd);
    if ((tag & 7) === 0) {
      take(tag >> 3, value);
      offset = valueEnd;
    } else {
      take(tag >> 3, bytes.subarray(valueEnd, valueEnd + value));
      offset = valueEnd + value;
    }
  }
}

function readDndCampaignRoster(bytes: Uint8Array): DndCampaignRoster {
  const roster: DndCampaignRoster = {
    campaignId: "", version: 0, status: "", characters: [], toolCallId: "", outputSha256: "",
  };
  readStateFields(bytes, (field, value) => {
    if (typeof value === "number") { if (field === 2) roster.version = value; return; }
    if (field === 1) roster.campaignId = decodeText(value);
    if (field === 3) roster.status = decodeText(value);
    if (field === 5) roster.toolCallId = decodeText(value);
    if (field === 6) roster.outputSha256 = decodeText(value);
    if (field === 4) {
      const character: DndCampaignCharacter = { id: "", name: "", kind: "", maxHp: 0 };
      readStateFields(value, (key, item) => {
        if (typeof item === "number") { if (key === 4) character.maxHp = item; return; }
        if (key === 1) character.id = decodeText(item);
        if (key === 2) character.name = decodeText(item);
        if (key === 3) character.kind = decodeText(item);
      });
      roster.characters.push(character);
    }
  });
  return roster;
}

function readDndInitiativeResult(bytes: Uint8Array): DndInitiativeResult {
  const result: DndInitiativeResult = { campaignVersion: 0, campaignSha256: "", rolls: [] };
  readStateFields(bytes, (field, value) => {
    if (typeof value === "number") { if (field === 1) result.campaignVersion = value; return; }
    if (field === 2) result.campaignSha256 = decodeText(value);
    if (field === 3) {
      const roll: DndInitiativeRoll = {
        characterId: "", expression: "", rolls: [], keptIndices: [], total: 0, entropySource: "",
      };
      readStateFields(value, (key, item) => {
        if (key === 3 || key === 4) {
          const list = key === 3 ? roll.rolls : roll.keptIndices;
          if (typeof item === "number") list.push(item);
          else {
            let offset = 0;
            while (offset < item.length) {
              const [number, next] = readVarint(item, offset);
              list.push(number);
              offset = next;
            }
          }
          return;
        }
        if (typeof item === "number") { if (key === 5) roll.total = decodeZigZag32(item); return; }
        if (key === 1) roll.characterId = decodeText(item);
        if (key === 2) roll.expression = decodeText(item);
        if (key === 6) roll.entropySource = decodeText(item);
      });
      result.rolls.push(roll);
    }
  });
  return result;
}

function decodeZigZag32(value: number): number {
  return value % 2 === 0 ? value / 2 : -((value + 1) / 2);
}

function decodeText(value: Uint8Array): string {
  return new TextDecoder().decode(value);
}

async function readExact(reader: ReadableStreamDefaultReader<Uint8Array>, length: number): Promise<Uint8Array | null> {
  let buffered = readerBuffers.get(reader) ?? new Uint8Array(0);
  const chunks: Uint8Array[] = [];
  let total = 0;
  if (buffered.byteLength) {
    chunks.push(buffered);
    total += buffered.byteLength;
  }
  while (total < length) {
    const { value, done } = await reader.read();
    if (done) {
      readerBuffers.delete(reader);
      if (total) throw new Error('Truncated turn frame');
      return null;
    }
    if (!value?.byteLength) continue;
    chunks.push(value);
    total += value.byteLength;
  }
  const merged = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    merged.set(chunk, offset);
    offset += chunk.byteLength;
  }
  if (total === length) {
    readerBuffers.set(reader, new Uint8Array(0));
    return merged;
  }
  const wanted = merged.slice(0, length);
  buffered = merged.slice(length);
  readerBuffers.set(reader, buffered);
  return wanted;
}

function readVarint(bytes: Uint8Array, offset: number): [number, number] {
  let value = 0;
  let shift = 0;
  let cursor = offset;
  while (cursor < bytes.length) {
    const byte = bytes[cursor++] ?? 0;
    value += (byte & 0x7f) * 2 ** shift;
    if ((byte & 0x80) === 0) return [value, cursor];
    shift += 7;
  }
  return [value, cursor];
}

function readStringMapEntry(bytes: Uint8Array): [string, string] | null {
  let offset = 0;
  let key = "";
  let value = "";
  while (offset < bytes.length) {
    const [tag, tagOffset] = readVarint(bytes, offset);
    offset = tagOffset;
    const fieldNumber = tag >> 3;
    const wireType = tag & 0x07;
    if (wireType !== 2) break;
    const [length, nextOffset] = readVarint(bytes, offset);
    offset = nextOffset;
    const fieldValue = new TextDecoder().decode(bytes.slice(offset, offset + length));
    offset += length;
    if (fieldNumber === 1) key = fieldValue;
    if (fieldNumber === 2) value = fieldValue;
  }
  return key ? [key, value] : null;
}
