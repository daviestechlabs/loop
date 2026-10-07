import { browser } from 'k6/browser';
import { check } from 'k6';

export const options = {
	scenarios: {
		webtransport_oracle: {
			executor: 'shared-iterations',
			vus: 1,
			iterations: 1,
			maxDuration: '30s',
			options: { browser: { type: 'chromium' } },
		},
	},
	thresholds: { checks: ['rate==1'] },
};

function required(name) {
	const value = __ENV[name] || '';
	if (value === '') throw new Error(`${name} is required`);
	return value;
}

export default async function () {
	const context = await browser.newContext({ ignoreHTTPSErrors: true });
	const page = await context.newPage();
	let result = null;
	let failure = '';
	try {
		await page.goto(required('WT_ORACLE_BASE_URL'), {
			waitUntil: 'domcontentloaded',
			timeout: 10000,
		});
		result = await page.evaluate(async (config) => {
			const FRAME_CONTROL_JSON = 1;
			const FRAME_TURN_EVENT_PROTO = 2;
			const NEAR_CAP_CONTROL_BYTES = 16000;
			let transportOpenCount = 0;
			let keepaliveEvents = 0;

			function timeout(promise, milliseconds, label) {
				return Promise.race([
					promise,
					new Promise((_, reject) => setTimeout(
						() => reject(new Error(`${label} timed out`)), milliseconds,
					)),
				]);
			}

			function frame(type, text) {
				const payload = new TextEncoder().encode(text);
				const output = new Uint8Array(5 + payload.byteLength);
				const view = new DataView(output.buffer);
				output[0] = type;
				view.setUint32(1, payload.byteLength, false);
				output.set(payload, 5);
				return output;
			}

			function pcmDatagram(sequence, fill) {
				const output = new Uint8Array(9 + 640);
				output.set(new TextEncoder().encode('DTVP1'));
				new DataView(output.buffer).setUint32(5, sequence, false);
				output.fill(fill, 9);
				return output;
			}

			function streamReader(reader) {
				let buffered = new Uint8Array();
				async function exact(length) {
					while (buffered.byteLength < length) {
						const next = await reader.read();
						if (next.done) return null;
						const joined = new Uint8Array(buffered.byteLength + next.value.byteLength);
						joined.set(buffered);
						joined.set(next.value, buffered.byteLength);
						buffered = joined;
					}
					const output = buffered.slice(0, length);
					buffered = buffered.slice(length);
					return output;
				}
				return async function readFrame() {
					const header = await exact(5);
					if (!header) return null;
					const length = new DataView(header.buffer).getUint32(1, false);
					if (length > 1024 * 1024) throw new Error('frame exceeds one MiB');
					const payload = await exact(length);
					if (!payload) throw new Error('frame is truncated');
					return { type: header[0], payload };
				};
			}

			function readVarint(bytes, start) {
				let value = 0;
				let multiplier = 1;
				for (let offset = start; offset < bytes.byteLength && offset < start + 10; offset += 1) {
					const byte = bytes[offset];
					value += (byte & 0x7f) * multiplier;
					if ((byte & 0x80) === 0) return [value, offset + 1];
					multiplier *= 128;
				}
				throw new Error('invalid protobuf varint');
			}

			function turnEvent(bytes) {
				const event = { requestId: '', type: 0, displayText: '' };
				let offset = 0;
				while (offset < bytes.byteLength) {
					let tag;
					[tag, offset] = readVarint(bytes, offset);
					const field = Math.floor(tag / 8);
					const wire = tag % 8;
					if (wire === 0) {
						let value;
						[value, offset] = readVarint(bytes, offset);
						if (field === 3) event.type = value;
					} else if (wire === 2) {
						let length;
						[length, offset] = readVarint(bytes, offset);
						if (offset + length > bytes.byteLength) throw new Error('truncated protobuf field');
						const text = new TextDecoder().decode(bytes.slice(offset, offset + length));
						if (field === 1) event.requestId = text;
						if (field === 18) event.displayText = text;
						offset += length;
					} else if (wire === 1) {
						offset += 8;
					} else if (wire === 5) {
						offset += 4;
					} else {
						throw new Error('unsupported protobuf wire type');
					}
					if (offset > bytes.byteLength) throw new Error('truncated protobuf value');
				}
				return event;
			}

			function certificateHash(base64) {
				const raw = atob(base64);
				const bytes = new Uint8Array(raw.length);
				for (let index = 0; index < raw.length; index += 1) bytes[index] = raw.charCodeAt(index);
				return bytes;
			}

			async function openTransport() {
				const transport = new WebTransport(config.webTransportUrl, {
					congestionControl: 'low-latency',
					requireUnreliable: true,
					serverCertificateHashes: [{
						algorithm: 'sha-256',
						value: certificateHash(config.certificateHash),
					}],
				});
				await timeout(transport.ready, 5000, 'WebTransport ready');
				transportOpenCount += 1;
				return transport;
			}

			async function openControl(transport, bytes, closeWriter = true) {
				const control = await transport.createBidirectionalStream();
				const writer = control.writable.getWriter();
				await writer.write(bytes);
				if (closeWriter) await writer.close();
				const reader = control.readable.getReader();
				return { transport, writer, reader, readFrame: streamReader(reader) };
			}

			function dndMetadata(requestId) {
				if (requestId === config.reuseRequestId) return {};
				return {
					interaction_profile: 'dnd_app', voice_mode: 'app', turn_profile: 'dnd_app',
					agent_id: 'dnd-agent', task_intent: 'dnd_action',
					campaign_id: 'campaign-wt', character_id: 'character-wt',
					encounter_id: 'encounter-wt', requested_npc_id: 'npc-wt',
					knowledge_scope: 'shared_rulebook', retrieval_force: 'true',
					turn_budget_ms: '45000', turn_deadline_unix_ms: '4102444800000',
					audio_group_session: 'true', audio_participant_id: 'player-wt',
					audio_participant_label: 'Mira 🐉',
				};
			}

			async function openRequest(transport, requestId, identityToken, closeWriter = true, overrides = {}) {
				return openControl(transport, frame(FRAME_CONTROL_JSON, JSON.stringify({
					request_id: requestId,
					session_id: 'wt-oracle-session',
					text: 'local WebTransport oracle',
					identity_token: identityToken,
					enable_rag: requestId === config.reconnectRequestId,
					enable_tts: false,
					user_id: 'forged-owner', premium: true,
					client_transport: 'webtransport-turn-stream',
					metadata: { input_mode: 'text', client_surface: 'k6-local-oracle', ...dndMetadata(requestId) },
					...overrides,
				})), closeWriter);
			}

			async function openAudioRequest(transport, requestId, identityToken) {
				return openControl(transport, frame(FRAME_CONTROL_JSON, JSON.stringify({
					request_id: requestId,
					session_id: 'wt-oracle-session',
					text: '',
					identity_token: identityToken,
					enable_rag: false,
					enable_tts: false,
					user_id: 'forged-owner', premium: true,
					client_transport: 'webtransport-turn-stream',
					metadata: {
						input_mode: 'webtransport_audio',
						client_audio_datagram_protocol: 'dtvp1',
						client_surface: 'k6-local-oracle',
						client_audio_kernel: 'wasm-simd', client_audio_packet_ms: '20',
						...dndMetadata(requestId),
					},
				})), true);
			}

			async function readAcceptance(attempt, requestId) {
				const acceptedFrame = await timeout(
					attempt.readFrame(), 3000, `turn acceptance ${requestId}`,
				);
				if (!acceptedFrame || acceptedFrame.type !== FRAME_CONTROL_JSON) {
					throw new Error('accepted frame is missing');
				}
				const accepted = JSON.parse(new TextDecoder().decode(acceptedFrame.payload));
				if (accepted.type !== 'accepted' || accepted.request_id !== requestId ||
					accepted.metadata?.runtime !== 'pure-c' ||
					accepted.metadata?.response_event_contract !== 'canonical-v1' ||
					accepted.metadata?.response_keepalive_ms !== config.keepaliveMs) {
					throw new Error('accepted metadata does not prove the pure-C contract');
				}
				return accepted;
			}

			async function readTurnEventFrame(attempt, requestId, label) {
				while (true) {
					const response = await timeout(attempt.readFrame(), 3000, label);
					if (!response || response.type !== FRAME_CONTROL_JSON) return response;
					const envelope = JSON.parse(new TextDecoder().decode(response.payload));
					if (envelope.type !== 'keepalive' || envelope.request_id !== requestId ||
						envelope.protocol_version !== 'turnstream.v1alpha1') {
						throw new Error('unexpected response control frame');
					}
					keepaliveEvents += 1;
				}
			}

			async function readEnd(attempt, label) {
				const trailing = await timeout(attempt.readFrame(), 3000, `${label} end`);
				if (trailing !== null) throw new Error(`${label} emitted a frame after its terminal`);
			}

			async function rejectAttempt(attempt, expectedError) {
				try {
					const response = await timeout(attempt.readFrame(), 3000, expectedError);
					if (!response || response.type !== FRAME_CONTROL_JSON) throw new Error('error frame is missing');
					const body = JSON.parse(new TextDecoder().decode(response.payload));
					if (body.type !== 'error' || body.error !== expectedError) {
						throw new Error(`expected ${expectedError}, got ${body.error || body.type}`);
					}
					await readEnd(attempt, expectedError);
				} finally {
					attempt.reader.releaseLock();
				}
			}

			async function rejectRequest(transport, requestId, identityToken, expectedError) {
				await rejectAttempt(
					await openRequest(transport, requestId, identityToken), expectedError,
				);
			}

			async function acceptRequest(transport, requestId, identityToken) {
				const attempt = await openRequest(transport, requestId, identityToken);
				try {
					await readAcceptance(attempt, requestId);
					const events = [];
					for (let index = 0; index < 5; index += 1) {
						const response = await readTurnEventFrame(
							attempt, requestId, `event ${index}`,
						);
						if (!response || response.type !== FRAME_TURN_EVENT_PROTO) {
							throw new Error('event frame is missing');
						}
						events.push(turnEvent(response.payload));
					}
					const expectedTypes = [1, 2, 4, 5, 10];
					if (events.some((event, index) =>
						event.requestId !== requestId || event.type !== expectedTypes[index])) {
						throw new Error('canonical event order or request binding is wrong');
					}
					if (events[2].displayText !== 'oracle delta' ||
						events[3].displayText !== 'oracle final') {
						throw new Error('public display text is wrong');
					}
					await readEnd(attempt, `turn ${requestId}`);
					return events.map((event) => event.type).join(',');
				} finally {
					await attempt.writer.close().catch(() => {});
					attempt.reader.releaseLock();
				}
			}

			async function rejectConflictingAudio(transport, requestId, identityToken) {
				const attempt = await openAudioRequest(transport, requestId, identityToken);
				let datagramWriter = null;
				try {
					await readAcceptance(attempt, requestId);
					datagramWriter = attempt.transport.datagrams.writable.getWriter();
					await datagramWriter.write(pcmDatagram(1, 0x11));
					await datagramWriter.write(pcmDatagram(1, 0x22));
					const response = await timeout(
						attempt.readFrame(), 3000, 'conflicting PCM rejection',
					);
					if (!response || response.type !== FRAME_CONTROL_JSON) {
						throw new Error('conflicting PCM error frame is missing');
					}
					const body = JSON.parse(new TextDecoder().decode(response.payload));
					if (body.type !== 'error' || body.request_id !== requestId ||
						body.error !== 'audio_datagram_conflict') {
						throw new Error(`unexpected conflicting PCM result: ${body.error || body.type}`);
					}
					await readEnd(attempt, 'conflicting PCM');
					return body.error;
				} finally {
					if (datagramWriter) datagramWriter.releaseLock();
					await attempt.writer.close().catch(() => {});
					attempt.reader.releaseLock();
				}
			}

			async function acceptEndpointAudio(transport, requestId, identityToken) {
				const attempt = await openAudioRequest(transport, requestId, identityToken);
				let datagramWriter = null;
				let endpoint = null;
				let endpointFrames = 0;
				let inputCommit = null;
				let transcript = null;
				let terminal = null;
				const responseEventTypes = [];
				try {
					const accepted = await readAcceptance(attempt, requestId);
					if (accepted.metadata?.audio_endpointing !== 'server' ||
						accepted.metadata?.audio_endpoint_feedback !== 'control-v1') {
						throw new Error('server endpoint feedback was not negotiated');
					}
					const conflictTransport = await openTransport();
					try {
						await rejectAttempt(
							await openAudioRequest(
								conflictTransport,
								config.endpointConflictRequestId,
								config.endpointConflictIdentityToken,
							),
							'audio_session_conflict',
						);
					} finally {
						conflictTransport.close();
					}
					datagramWriter = attempt.transport.datagrams.writable.getWriter();
					await datagramWriter.write(pcmDatagram(0, 0x11));
					await datagramWriter.write(pcmDatagram(1, 0x22));
					while (endpoint === null) {
						const response = await timeout(
							attempt.readFrame(), 3000, 'server endpoint feedback',
						);
						if (!response || response.type !== FRAME_CONTROL_JSON) {
							throw new Error('server endpoint control frame is missing');
						}
						const body = JSON.parse(new TextDecoder().decode(response.payload));
						if (body.type === 'keepalive') {
							keepaliveEvents += 1;
							continue;
						}
						if (body.type !== 'input_endpoint' || body.request_id !== requestId) {
							throw new Error(`unexpected endpoint response: ${body.type || 'unknown'}`);
						}
						endpoint = body;
						endpointFrames += 1;
					}
					if (endpoint.received_datagrams !== 2 || endpoint.received_audio_bytes !== 1280 ||
						endpoint.forwarded_datagrams !== 2 || endpoint.forwarded_audio_bytes !== 1280 ||
						!Number.isInteger(endpoint.edge_input_endpoint_us) ||
						endpoint.edge_input_endpoint_us < 0) {
						throw new Error('server endpoint counters do not cover the forwarded prefix');
					}
					for (let sequence = 2; sequence < 6; sequence += 1) {
						await datagramWriter.write(pcmDatagram(sequence, 0x30 + sequence));
					}
					await datagramWriter.write(new TextEncoder().encode(
						'DTVA1:{"type":"end","packet_count":6,"audio_bytes":3840}',
					));
					while (terminal === null) {
						const response = await timeout(
							attempt.readFrame(), 3000, 'endpoint audio completion',
						);
						if (!response) throw new Error('endpoint audio response ended early');
						if (response.type === FRAME_CONTROL_JSON) {
							const body = JSON.parse(new TextDecoder().decode(response.payload));
							if (body.type === 'keepalive') {
								keepaliveEvents += 1;
								continue;
							}
							if (body.type === 'input_endpoint') {
								endpointFrames += 1;
								continue;
							}
							if (body.type === 'transcript') {
								if (transcript || body.request_id !== requestId || body.is_final !== true ||
									body.text !== 'Lynn says "bonjour". Before you cast the spell…' || responseEventTypes.length) {
									throw new Error('final transcript is not bound or ordered before response events');
								}
								transcript = body;
								continue;
							}
							if (body.type !== 'input_committed' || body.request_id !== requestId || inputCommit) {
								throw new Error(
									`unexpected endpoint audio control: ${body.error || body.type || 'unknown'}`,
								);
							}
							inputCommit = body;
							continue;
						}
						if (response.type !== FRAME_TURN_EVENT_PROTO) continue;
						const event = turnEvent(response.payload);
						const expectedType = [1, 5, 10][responseEventTypes.length];
						if (!transcript || event.requestId !== requestId || event.type !== expectedType) {
							throw new Error('endpoint audio terminal binding is wrong');
						}
						responseEventTypes.push(event.type);
						if (event.type === 10) terminal = event;
					}
					if (!transcript || !inputCommit || endpointFrames !== 1 ||
						inputCommit.audio_datagrams !== 6 || inputCommit.audio_bytes !== 3840 ||
						inputCommit.forwarded_datagrams !== 2 ||
						inputCommit.forwarded_audio_bytes !== 1280 ||
						inputCommit.drained_datagrams !== 4 ||
						inputCommit.drained_audio_bytes !== 2560 ||
						inputCommit.server_endpoint !== true) {
						throw new Error('endpoint drain receipt does not cover all admitted PCM');
					}
					await readEnd(attempt, 'endpoint audio');
					return {
						transcript: transcript.text,
						drainedBytes: inputCommit.drained_audio_bytes,
						drainedDatagrams: inputCommit.drained_datagrams,
						endpointFrames,
						forwardedDatagrams: inputCommit.forwarded_datagrams,
						sessionConflict: true,
					};
				} finally {
					if (datagramWriter) datagramWriter.releaseLock();
					await attempt.writer.close().catch(() => {});
					attempt.reader.releaseLock();
				}
			}

			const truncated = new Uint8Array(5 + NEAR_CAP_CONTROL_BYTES);
			truncated[0] = FRAME_CONTROL_JSON;
			new DataView(truncated.buffer).setUint32(1, NEAR_CAP_CONTROL_BYTES + 1, false);
			truncated[5] = 0x7b;
			const transport = await openTransport();
			let audioConflict;
			let endpointAudio;
			let eventTypes;
			let reuseEventTypes;
			try {
				await rejectAttempt(await openControl(transport, truncated), 'truncated_control');
				await rejectRequest(
					transport, `${config.requestId}-wrong`, config.identityToken, 'unauthorized',
				);
				await rejectAttempt(await openRequest(transport, config.requestId, config.identityToken, true, {
					metadata: { ...dndMetadata(config.requestId), governed_user_id: 'forged-owner' },
				}), 'invalid_turn');
				await rejectAttempt(await openRequest(transport, config.requestId, config.identityToken, true, {
					metadata: { ...dndMetadata(config.requestId), campaign_id: '' },
				}), 'invalid_turn');
				eventTypes = await acceptRequest(transport, config.requestId, config.identityToken);
				await new Promise((resolve) => setTimeout(resolve, 100));
				endpointAudio = await acceptEndpointAudio(
					transport, config.endpointRequestId, config.endpointIdentityToken,
				);
				await new Promise((resolve) => setTimeout(resolve, 100));
				audioConflict = await rejectConflictingAudio(
					transport, config.audioRequestId, config.audioIdentityToken,
				);
				await new Promise((resolve) => setTimeout(resolve, 100));
				reuseEventTypes = await acceptRequest(
					transport, config.reuseRequestId, config.reuseIdentityToken,
				);
				await new Promise((resolve) => setTimeout(resolve, 100));
				await rejectRequest(
					transport, config.requestId, config.identityToken, 'unauthorized',
				);
			} finally {
				transport.close();
			}
			await new Promise((resolve) => setTimeout(resolve, 100));
			const reconnectTransport = await openTransport();
			let reconnectEventTypes;
			try {
				reconnectEventTypes = await acceptRequest(
					reconnectTransport, config.reconnectRequestId, config.reconnectIdentityToken,
				);
			} finally {
				reconnectTransport.close();
			}
			return {
				accepted: true,
				audioConflict,
				endpointAudio,
				eventTypes,
				forgerySuppressed: true,
				replayRejected: true,
				bindingRejected: true,
				metadataRejected: true,
				reconnectEventTypes,
				reuseEventTypes,
				transportOpenCount,
				keepaliveEvents,
				truncatedRejected: true,
			};
		}, {
			audioIdentityToken: required('WT_ORACLE_AUDIO_IDENTITY_TOKEN'),
			audioRequestId: required('WT_ORACLE_AUDIO_REQUEST_ID'),
			endpointIdentityToken: required('WT_ORACLE_ENDPOINT_IDENTITY_TOKEN'),
			endpointRequestId: required('WT_ORACLE_ENDPOINT_REQUEST_ID'),
			endpointConflictIdentityToken: required('WT_ORACLE_ENDPOINT_CONFLICT_IDENTITY_TOKEN'),
			endpointConflictRequestId: required('WT_ORACLE_ENDPOINT_CONFLICT_REQUEST_ID'),
			certificateHash: required('WT_ORACLE_CERT_SHA256'),
			identityToken: required('WT_ORACLE_IDENTITY_TOKEN'),
			keepaliveMs: Number(__ENV.WT_ORACLE_KEEPALIVE_MS || '100'),
			requestId: required('WT_ORACLE_REQUEST_ID'),
			reconnectIdentityToken: required('WT_ORACLE_RECONNECT_IDENTITY_TOKEN'),
			reconnectRequestId: required('WT_ORACLE_RECONNECT_REQUEST_ID'),
			reuseIdentityToken: required('WT_ORACLE_REUSE_IDENTITY_TOKEN'),
			reuseRequestId: required('WT_ORACLE_REUSE_REQUEST_ID'),
			webTransportUrl: required('WT_ORACLE_WEBTRANSPORT_URL'),
		});
	} catch (error) {
		failure = String(error?.message || error);
	} finally {
		await page.close();
		await context.close();
	}
	check(result, {
		'authenticated browser turn is accepted': (value) => value?.accepted === true && failure === '',
		'canonical event lifecycle arrives in order': (value) => value?.eventTypes === '1,2,4,5,10',
		'quiet model work emits a request-bound keepalive': (value) => value?.keepaliveEvents >= 1,
		'conflicting PCM datagrams fail closed': (value) =>
			value?.audioConflict === 'audio_datagram_conflict',
		'request-bound endpoint feedback drains trailing PCM exactly once': (value) =>
			value?.endpointAudio?.endpointFrames === 1 &&
			value?.endpointAudio?.forwardedDatagrams === 2 &&
			value?.endpointAudio?.drainedDatagrams === 4 &&
			value?.endpointAudio?.drainedBytes === 2560,
		'one active audio session owns each endpoint subject': (value) =>
			value?.endpointAudio?.sessionConflict === true,
		'capability forgery and late events stay private': (value) => value?.forgerySuppressed === true,
		'one transport accepts sequential signed turns': (value) =>
			value?.reuseEventTypes === '1,2,4,5,10' && value?.transportOpenCount === 3,
		'released transport slot accepts a new session': (value) =>
			value?.reconnectEventTypes === '1,2,4,5,10',
		'near-cap truncated control frame fails closed': (value) => value?.truncatedRejected === true,
		'request binding and replay fail closed': (value) =>
			value?.bindingRejected === true && value?.replayRejected === true,
		'forged owner metadata and invalid campaign context fail before admission': (value) =>
			value?.metadataRejected === true,
	});
	if (failure !== '') throw new Error(failure);
}
