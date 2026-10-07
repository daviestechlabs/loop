import { expect, test } from 'bun:test';
import type { TurnRecord } from '../src/evidence';
import { browserInterval, browserTimings, responseTimeline } from '../src/timing';

function event(name: string, seq: number, elapsed_ms: number, source: 'browser' | 'gateway_observed' = 'browser'): TurnRecord['events'][number] {
  return { event: { name, seq, elapsed_ms, source, payload: {} }, sha256: '', received: 0 };
}
const events = [event('committing', 0, 100), event('first_text_received', 1, 300),
  event('first_audio_received', 2, 400), event('first_playback_scheduled', 3, 400), event('playback_finished', 4, 900)];

test('browser timeline separates response waiting, scheduling, and playback without gateway clock mixing', () => {
  expect(browserTimings(events)).toEqual({ text: 200, audio: 300, scheduled: 300, finished: 800, interruption: null });
  expect(responseTimeline(events)?.map((segment) => segment.milliseconds)).toEqual([200, 100, 0, 500]);
  expect(browserInterval([...events, event('first_text_received', 5, 12345, 'gateway_observed')], 'committing', 'first_text_received')).toBe(200);
});
test('missing, repeated, malformed, and reversed observations remain unknown', () => {
  for (const altered of [events.slice(1), [...events, events[0]!], [event('committing', 2, 500), events[1]!],
    [event('committing', -1, 100), events[1]!], [event('committing', 0, NaN), events[1]!],
    [event('committing', 0, -1), events[1]!], [event('committing', 0, Infinity), events[1]!]]) {
    expect(browserInterval(altered, 'committing', 'first_text_received')).toBeNull();
    expect(responseTimeline(altered)).toBeNull();
  }
  expect(responseTimeline(events.slice(0, -1))).toBeNull();
  expect(browserTimings(events.slice(0, -1)).finished).toBeNull();
  expect(browserInterval([event('interrupt_requested', 0, 50), event('playback_stopped', 1, 50)], 'interrupt_requested', 'playback_stopped')).toBe(0);
});
