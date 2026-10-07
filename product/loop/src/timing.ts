import type { TurnRecord } from './evidence';

// One recording uses one performance.now clock. Gateway timestamps use another clock.
export function browserInterval(events: TurnRecord['events'], start: string, end: string): number | null {
  const starts = events.filter(({ event }) => event.source === 'browser' && event.name === start);
  const ends = events.filter(({ event }) => event.source === 'browser' && event.name === end);
  if (starts.length !== 1 || ends.length !== 1) return null;
  const a = starts[0]!.event, b = ends[0]!.event;
  if (![a.seq, b.seq].every((value) => Number.isSafeInteger(value) && value >= 0) || b.seq <= a.seq ||
      ![a.elapsed_ms, b.elapsed_ms].every((value) => Number.isFinite(value) && value >= 0) || b.elapsed_ms < a.elapsed_ms) return null;
  return Math.round((b.elapsed_ms - a.elapsed_ms) * 1000) / 1000;
}

export function browserTimings(events: TurnRecord['events']) {
  const interval = (end: string) => browserInterval(events, 'committing', end);
  return {
    text: interval('first_text_received'),
    audio: interval('first_audio_received'),
    scheduled: interval('first_playback_scheduled'),
    finished: interval('playback_finished'),
    interruption: browserInterval(events, 'interrupt_requested', 'playback_stopped'),
  };
}

export function responseTimeline(events: TurnRecord['events']) {
  const milestones = ['committing', 'first_text_received', 'first_audio_received', 'first_playback_scheduled', 'playback_finished'];
  const segments = [
    { label: 'text', className: 'w-think' }, { label: 'audio', className: 'w-speak' },
    { label: 'scheduling', className: 'w-plan' }, { label: 'playback', className: 'w-playback' },
  ].map((segment, i) => ({ ...segment, milliseconds: browserInterval(events, milestones[i]!, milestones[i + 1]!) }));
  if (segments.some((segment) => segment.milliseconds === null)) return null;
  return segments as Array<{ label: string; className: string; milliseconds: number }>;
}

export function formatMilliseconds(value: number | null): string {
  return value === null ? '—' : `${Math.round(value)}ms`;
}
