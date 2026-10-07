# Loop C service

The service owns OAuth sessions, voice admission, and evaluation evidence.
It reuses the Companions C OAuth implementation and the voice identity signer.
It stores evidence in SQLite with write-ahead logging and full synchronization.
A recorded value retains its origin: browser observation, gateway event, or operator annotation.

The default production mode is shared gateway SSO.
Set `SSO_ISSUER`, `SSO_AUDIENCE`, `SSO_REQUIRED_GROUP`, and `SSO_JWKS_URL`.
The JWKS URL must use HTTPS and supplies public signing keys only.
The C verifier checks the gateway's `x-envoy-oidc-id-token` on each authenticated API request.
Duplicate token headers are rejected at the HTTP boundary.
No Loop OAuth client secret or operator-subject secret is required in this mode.

Storage owners bind the verified issuer and subject.
Legacy direct-OAuth cookies cannot authenticate gateway-mode requests.
State-changing requests still require the exact Origin.
`POST /api/logout` returns the shared logout URL, `/oauth2/logout`, for browser navigation.
This logs out the shared gateway session; it is not a local Loop cookie deletion.

Explicit `LOOP_AUTH_MODE=oauth` retains the earlier direct-OAuth development mode.
That mode requires `OAUTH_*` configuration and `LOOP_OPERATOR_SUBJECTS`.
Its opaque sessions expire after eight hours and recheck the configured subject allowlist.
Its stored owners differ from gateway mode and are not automatically reassigned.

The service never accepts arbitrary inference endpoints from browser requests.
Anvil owns candidate activation and resource admission.

Linked Anvil reads forward the already verified gateway ID token to fixed HTTPS evidence routes.
Loop sends no browser cookies. The gateway verifies the same issuer and audience.
Anvil rechecks the signature, expiry, token age, required group, and object owner.
Loop validates the returned evidence hashes and owner before it displays verified readback.
The delegated route admits only existing evidence reads. Browser Anvil routes retain OIDC login.

## Build

Run `make` in this directory.
Dependencies are SQLite, OpenSSL, POSIX threads, and the system C library.
The OAuth client uses the existing verified TLS implementation.

Run `make test` for storage and admission tests.
Run `make sanitize` for address and undefined-behavior checks.

The C SSO test restores a snapshot created while its source remains open.
Signed synthetic gateway identities verify identical owner receipts, reports, and input audio after restore.
A different admitted subject receives 404; missing gateway identity and local session cookies receive 401.
This test does not establish live gateway login or production recovery acceptance.

## Verified recording reports

`GET /api/turns/:id/report` checks a finalized recording owned by the signed-in operator.
It verifies manifest, final, event, and audio hashes against their stored bytes.
It also checks event sequence, event count, timestamps, and audio format.
Incomplete recordings return 409. Failed integrity checks return 409.

The report derives intervals from named browser milestones.
Missing milestones produce null measurements with a reason.
A measured zero remains zero.
Repeated milestones produce null measurements with a duplicate reason.
Event sequence determines order when timestamps are equal.
Gateway observations cannot supply browser milestones.
The report separates first-text-to-first-audio delay from audio-to-playback-scheduling delay.
Both intervals use browser observations from the same recording.
They do not isolate provider compute time or measure acoustic delay.
Scheduled playback does not prove acoustic output.
The report does not verify model revision or human acceptance.

The report's `inputDelivery` object reconciles recorded input with two observations.
It reads browser `input_end_sent` counters and captured gateway `input_committed` counters.
A match requires one observation of each kind, in order, with valid integer counters.
Both packet counts must match complete 640-byte frames at 16 kHz.
Both byte counts must match the recorded input, and reported packet loss must be zero.

Matching evidence produces `matched_browser_observations`.
Missing, duplicate, invalid, or conflicting evidence produces `not_established` with a reason.
Missing or ambiguous counters remain null. A reported zero remains zero.
`serverAttested` remains false in both cases.
A delivery match does not imply a successful response, model quality, or human acceptance.

The report's `responseAudio` object checks recorded output against browser response counters.
The browser emits these counters after final text, final PCM, gateway completion, and response stream closure.
A match requires one browser summary after one captured gateway completion event.
Recorded bytes and sample rate must match the summary.
Packet counts must describe nonempty PCM frames within the decoder's 16384-byte limit.
The packet count remains a browser observation; the stored PCM does not retain packet boundaries.

Missing, duplicate, invalid, mismatched, or incorrectly ordered observations produce `not_established` with a reason.
Older recordings without the summary remain readable and do not gain a completion claim.
`pcmDurationMs` derives from verified stored bytes and sample rate, even for a partial response.
It does not measure response latency or audible duration.
A complete captured response can coexist with interrupted or failed playback.
`serverAttested` and `acousticOutputVerified` remain false.

`GET /api/turns/:id/receipt` downloads the exact receipt bytes after the same checks.
The receipt binds the component hashes in a fixed order.
Hash the downloaded UTF-8 bytes without formatting changes.
The result matches `receiptSha256` in the report.
This checks stored consistency; it is not a signature against database administrator changes.

The report's `anvilEvidence` field matches an existing Anvil evidence reference: `name` and `sha256`.
Save the receipt with that name and include the reference in an existing Anvil report.
Anvil retains report import, run binding, comparisons, human review, and promotion.
Loop does not create an Anvil run or assign its source revision.
This export alone does not complete Anvil integration or prove model quality.

## Anvil learning report export

`POST /api/turns/:id/anvil-report` exports an existing Anvil learning report format.
The route requires a signed-in owner, the exact Origin, and a finalized recording.
Supply exactly these fields:

| Field | Value |
|---|---|
| `run_id` | Operator-selected lowercase version 4 UUID for the Anvil evidence run |
| `sequence` | Report sequence from 1 through 1,000,000 |
| `source_revision` | Operator-supplied lowercase 40-character source commit |
| `receipt_sha256` | Receipt hash from the checked Loop report |

The C service verifies the recording before export and rejects a mismatched receipt hash.
Identical requests produce identical output for the same stored report and service version.
The recording creation time supplies `recordedAt`; export does not invent a new observation time.
Anvil can reject this timestamp or sequence when an existing run already has newer evidence.
Use a new Anvil evidence run when the original run is terminal.

The exported report uses the existing `cohesion-speed` track and `development` scope.
Its status remains `held`, regardless of the recording's completion status.
Known browser intervals become metrics with the `loop_browser_` prefix.
Unknown intervals are omitted. Measured zero remains zero.
The summary identifies the recording status and the operator-supplied bindings.
The export does not independently verify a model revision, run owner, or human acceptance.

`POST /api/turns/:id/anvil-bundle` accepts the same binding and returns a single TAR archive.
It contains the Anvil report, Loop report, and receipt from one database snapshot.
The three regular files have deterministic names, private permissions, and unchanged JSON bytes.
The response includes the archive SHA-256 in `X-Content-SHA256`.
It excludes audio and raw event payloads; download PCM separately when an admitted benchmark requires it.
Extract the archive into private storage, then import its `-anvil.json` file through Anvil.

Alternatively, download the receipt and Loop report beside the Anvil report.
Keep their exact response bytes under the filenames in the report's `evidence` array.
That array binds both files by SHA-256.
Anvil's existing learning panel imports the exported report under its authenticated owner.
Loop does not perform that import, create an experiment, or assign a verdict.
Anvil retains import admission, review, comparisons, and promotion.

Local HTTP tests validate the C export against Anvil's actual report schema.
They verify both evidence hashes and reject malformed bindings.
An independent TAR reader checks the bundle before the importer receives its report.
An isolated PostgreSQL test applies Anvil's real migrations and calls its production importer.
It checks owner isolation, identical retries, changed duplicates, terminal runs, and the import audit event.
Zod and PGlite are test dependencies; the deployed Loop backend remains C.
These checks do not prove a live Anvil import.

## Recorded audio replay

`GET /api/turns/:id/replay/input.wav` exports recorded microphone audio.
`GET /api/turns/:id/replay/output.wav` exports recorded response audio.
Both routes require ownership and a finalized recording with verified stored evidence.
The service reads the report and audio in one database snapshot.
Missing audio returns 404. Failed integrity checks return 409.

The C service wraps unchanged mono PCM16 bytes in a WAV container.
It preserves the recorded sample rate.
Response headers contain WAV, PCM, and source receipt hashes.
These exports never call a model, execute a tool, or overwrite the source recording.
Recorded playback is not a controlled or full-system rerun.

Use the existing voice validation suite for benchmark execution and reviewed capture admission.
The suite requires 16 kHz input, complete 20 ms frames, and explicit capture approval.
Loop does not resample, pad, or approve recordings to satisfy those gates.

## Benchmark audio export

`GET /api/turns/:id/replay/input.s16le` exports the exact stored input PCM.
`GET /api/turns/:id/replay/output.s16le` exports the exact stored response PCM.
These routes use the same ownership, finalization, and integrity checks as WAV replay.
They return `application/octet-stream` without a WAV header.
The filename ends in `.s16le`.

Headers identify the encoding, sample rate, channel count, content hash, and source receipt hash.
`X-PCM-Encoding` is `pcm_s16le`. `X-PCM-Channels` is `1`.
`X-PCM-Sample-Rate` preserves the recorded rate.
`X-Content-SHA256` and `X-PCM-SHA256` match the downloaded bytes.
`X-Loop-Receipt-SHA256` binds the export to the checked recording receipt.

Keep the exported PCM, receipt, and report together in private storage.
The existing benchmark capture workflow owns manifest preparation and human approval.
It requires complete 20 ms input frames at 16 kHz, at most 30 seconds.
An exported recording does not automatically meet those requirements.
Loop does not supply missing capture timestamps, acoustic observations, transcripts, or approval.

Input recording occurs before the browser completes each network write.
Failed turns can therefore contain audio that the gateway did not receive.
Review `inputDelivery` and capture provenance before preparing a real-browser benchmark manifest.
Synthetic tests and saved-input reruns do not become new human microphone captures.
The [existing suite](../../../benchmarks/voice-validation-suite/README.md) retains its admission and execution contracts.
Anvil retains model activation, experiment comparisons, review, and promotion.

## Anvil references

`POST /api/turns/:id/anvil-links` saves an immutable operator reference beside a finalized recording.
Supply exactly four fields: `id`, `kind`, `reference`, and `receipt_sha256`.
Use the checked Loop receipt hash as `receipt_sha256`.
The `experiment` kind requires an Anvil experiment UUID.
The `learning_report` and `answer_review` kinds require an Anvil learning report hash.
An answer review reference names the report under review, not a copied verdict.

`GET /api/turns/:id/anvil-links` checks the reference hashes and their source receipt binding.
Both routes require recording ownership. Each recording allows at most 32 references.
Identical retries succeed. Changed references with the same ID return 409.
References survive restart and do not change the original recording receipt.

These records remain operator annotations with `remoteVerification` set to `not_verified`.
Loop does not infer remote ownership, existence, human approval, or model provenance from an identifier.
Anvil retains the authoritative objects and their authorization checks.

The store upgrades schema version 1 to version 2 in one transaction.
Unknown future versions fail startup without changing their version or evidence.

## Saved-input reruns

`POST /api/turns/:id/reruns` admits a new recording from owned, finalized input audio.
The body contains `request_id` and `mode`.
The supported mode is `full_system_active_model`.
The new request ID must differ from the source ID.
Input must contain complete 20 ms mono PCM16 frames at 16 kHz, at most 30 seconds.

The C service verifies the source and binds its receipt and PCM hashes into the new manifest.
Admission creates a separate empty recording. It does not claim execution success.
The browser verifies the saved bytes and sends them through the existing authenticated C gateway.
The browser does not open the microphone for this run.
Early gateway endpointing fails the rerun if input remains unsent.
Results retain the new request ID and cannot overwrite the source.

The C service reserves rerun metadata for this admission route.
Ordinary turn creation rejects the `rerun` field and the `saved_input_rerun` purpose.
Rerun input uploads must match the bound source bytes at 16 kHz.
Failed or interrupted captures can store an exact prefix of complete 20 ms frames.
Changed bytes, wrong rates, damaged source audio, and partial frames return 409.
A completed rerun requires the entire bound input recording.
These checks verify stored replay input; they do not prove gateway delivery or model execution.

The gateway uses its current active model and live tool policy.
This mode does not pin an unrecorded model revision or replay recorded tool side effects.
Controlled model reruns with frozen dependencies remain unsupported.
Anvil retains candidate activation, comparisons, review, and promotion.

## Runtime

Set `LOOP_ORIGIN` to the HTTPS origin.
Set `LOOP_DB` to a persistent SQLite file path.
`LOOP_DB_MAX_BYTES` limits database pages and defaults to 2147483648 bytes (2 GiB).
It accepts byte counts from 1 MiB through 4 GiB and rounds down to whole database pages.
Startup refuses a database whose allocated pages already exceed the configured limit.
It does not delete or shrink existing evidence to meet that limit.
Set `LOOP_STATIC` to the built frontend directory.
Set the shared SSO variables described above for the default gateway mode.
Set `LOOP_WEBTRANSPORT_URL` to the approved gateway URL.
Set `VOICE_GATEWAY_TOKEN` to the existing purpose-separated gateway secret.
The token must contain at least 32 bytes.
Only direct-OAuth mode uses `OAUTH_*` variables.
In that mode, the redirect URL must equal `LOOP_ORIGIN` plus `/api/oauth/callback`.
The WebTransport URL must use HTTPS and the exact `/v1/voice/turns` path.
The operator configures its authority; browser requests cannot supply another authority.
Startup and identity issuance reject credentials, invalid authorities, and other routes.
The C response policy permits external connections only to the configured voice authority.
It cannot contain credentials, a query, or a fragment.
This route matches the browser connection policy.
Set `LOOP_ANVIL_ORIGIN` to the existing Anvil HTTPS origin without a path, query, or fragment.
The existing lab origin remains its compatibility default.
Authenticated `GET /api/config` supplies that origin to the browser handoff.
Anvil readback uses that same origin and its existing fixed evidence routes.
Startup rejects missing voice credentials or mismatched routing before opening the database or HTTP listener.

`LOOP_BIND` defaults to `127.0.0.1`; `LOOP_PORT` defaults to `8080`.
Test builds provide a loopback fixture mode that production builds cannot enable.

The HTTP service uses four workers and a queue of sixteen connections.
Each connection has a fifteen-second socket deadline from acceptance.
Queue time, request reads, and response writes share that deadline.
Sending more bytes does not restart the deadline.
The deadline does not interrupt a running OAuth or database operation.
Provider exchanges run outside the evidence database lock.
Only one OAuth callback can be active per service process.
Concurrent callbacks return 503 with `oauth_callback_busy` and `Retry-After: 1` before consuming their state.
This admission limit preserves worker capacity for existing sessions during a slow provider exchange.
It does not guarantee availability during unrelated worker or database exhaustion.

A deterministic C test blocks the provider exchange while another thread stores evidence and requests a voice identity.
It verifies callback rejection, retained state, session cookies, provider failure, operator denial, storage failure, and recovery.
The test uses the production OAuth adapter with synthetic provider responses.
It does not verify live provider availability or deployed authentication.

The isolated HTTP fixture uses a one-second deadline.
Its fault test sends slow headers and bodies through all four workers, then checks recovery.
The package script runs this fixture with address and undefined-behavior sanitizers.
These checks do not prove fleet load behavior or model service recovery.

## Storage capacity failures

Evidence writes return HTTP 507 with `evidence_store_full` when SQLite reports exhausted capacity.
This covers the configured page limit and SQLite's filesystem-full condition.
Other storage failures retain HTTP 503.
A failed write does not acknowledge persistence.
Keep unsaved browser evidence available until storage capacity is restored and the retry succeeds.

Tests force page exhaustion during audio, Anvil-reference, and rerun writes.
They verify rollback, unchanged saved receipts, safe identical retries, and recovery across restart.
SQLite can roll back a savepoint automatically after exhaustion; the service preserves the capacity error.
These isolated tests do not fill the host disk or prove physical disk recovery.

The page limit is not a filesystem quota.
The write-ahead log, shared-memory file, backups, and filesystem overhead need additional space.
Long external readers can delay checkpoints and grow the write-ahead log.
The held 5 GiB volume uses a 2 GiB database limit.
Monitor the whole volume and keep backups off that volume.
No automatic retention or evidence deletion is enabled.

## Backup and recovery

The release binary provides a local operator command.
It needs filesystem access, not OAuth credentials or an HTTP listener.

```sh
loop-server --backup /data/loop.sqlite /private-backups/loop-20260920.sqlite
```

Both paths must use trusted directories.
The destination must not exist, including as a symbolic link.
The command opens the source read-only and uses the SQLite backup API.
It includes committed write-ahead log pages in a consistent snapshot.
The command requires schema version 2 and checks database integrity before publishing the file.
It creates a private temporary file beside the destination and publishes it without overwriting existing files.
The output has mode 0600 and does not require WAL sidecars.
The command syncs the destination directory after publishing the new filename.
A directory sync failure returns failure and can leave the destination file present.
Do not count that attempt as a completed backup or overwrite its file.
Use a new destination for the next attempt and investigate the storage error.
A crash can leave temporary files whose names contain `.partial-`.
Do not restore those files.

Keep completed backups on separate protected storage.
The snapshot contains private recordings and session hashes.
The backup command does not encrypt, upload, schedule, or rotate backups.
An integrity check verifies database structure, not model quality or every evidence hash.
Use owned receipt and replay endpoints to check restored evidence.

For recovery, stop Loop before changing its database path.
Restore a completed snapshot to a new private directory on local persistent storage.
Set ownership to the service UID and set `LOOP_DB` to the restored file.
Keep the original database and its sidecars together for rollback.
Do not overwrite a running database or combine old WAL files with a restored snapshot.
Start Loop with the current operator allowlist and verify saved receipts, audio, and failure cases.
Restored sessions still require current operator admission and have their original expiration times.

Native tests restore a snapshot while the source remains open with committed WAL data.
They compare exact receipt and PCM bytes, check cases and Anvil references, and enforce ownership.
They also verify that later source writes are absent from the snapshot.
An injected directory sync error verifies failure reporting after publication.
These tests do not simulate power loss or establish storage hardware flush guarantees.
These tests do not prove off-node backup retention or cluster disaster recovery.

The package HTTP recovery test starts isolated C fixtures with synthetic sessions.
It compares restored receipt, report, and audio bytes through the HTTP routes.
It checks snapshot boundaries, unfinished records, invalid sessions, logout, and fresh session access.
This test does not establish live gateway SSO or deployed recovery.

### Offline recording verification

Run the C verifier against a completed snapshot or an isolated restore.

```sh
loop-server --verify-recordings /private-backups/loop-20260920.sqlite
```

This operator command opens the database read-only and pins one transaction.
It needs no OAuth settings, signing keys, or HTTP listener.
It checks schema version, SQLite integrity, and every finalized recording through the existing receipt validator.
That validator checks manifest, final record, event order, event count, and audio hashes.
A failed check exits with code 1 and prints no recording content or owner identifiers.
Success prints the verified count and a separate count of unfinished recordings.
Unfinished recordings remain unverified, including their stored bytes.
An empty database reports zero verified recordings.

This check does not verify session admission, failure annotations, Anvil links, remote model identity, or acoustic playback.
It cannot detect a recording that was removed before the snapshot.
Compare retained receipt hashes separately when exact recovery equivalence is required.
The source directories must remain trusted, as with the backup command.

### Verified recording snapshots

Use the recording backup command for scheduled evidence snapshots.

```sh
loop-server --backup-recordings /data/loop.sqlite /private-backups/loop-new.sqlite
```

The C command copies one SQLite snapshot, including committed WAL pages.
It checks every finalized recording before publishing the destination name.
It uses the same receipt checks as the recovery verifier.
Corrupt evidence fails without publishing a destination.
Existing destination files remain unchanged.
A directory-sync failure can leave a published file, but the command reports failure.
Success reports finalized and unfinished counts separately.
Unfinished records remain in the snapshot without a verified receipt claim.
This command does not prove external receipt equivalence or authenticated recovery.

## Storage capacity check

`loop-server --check-storage PATH` reads filesystem statistics without opening the database or starting HTTP.
It reports bytes available to an unprivileged process and total filesystem capacity.
Exit zero requires at least 15 percent available and at least one GiB available.
Exit one means low capacity. Exit two means missing, invalid, or overflowing statistics.
The command does not inspect files, reserve space, enforce quotas, or delete evidence.
An NFS result describes the shared filesystem, not the requested PVC size.


## Backup retention command

`loop-server --prune-backups DIRECTORY DAYS` verifies a retention plan without deleting snapshots.
The operator must supply 1 to 3650 days. There is no default policy.
Add `--apply` only to execute the selected policy.
Use a dedicated backup directory with no concurrent backup writers.

The command recognizes only `loop-UUID.sqlite` filenames used by the scheduled snapshot template.
It verifies every recognized snapshot before any deletion.
Files must be private, regular, singly linked, and owned by the command user.
Symlinks, future timestamps, invalid recordings, and a directory containing `loop.sqlite` stop the operation.
Unknown filenames and partial snapshot files remain untouched.

Age uses each snapshot's modification time.
Only snapshots strictly older than the supplied retention interval are eligible.
The newest verified snapshot remains, even when every snapshot is expired.
Equal timestamps use the filename as a deterministic tie breaker.
The scan stops at 1024 snapshots or 4096 directory entries.

Dry runs can create `.loop-retention.lock` with private permissions.
This writable file supports advisory locking on NFS and prevents overlapping retention commands.
The command checks each file's identity again before deletion and syncs the directory afterward.
A failure can leave a partially applied policy; the output reports the deletion count.
It never reports that partial deletion was atomic.

Local C tests cover expiry boundaries, invalid evidence, links, permissions, clocks, locking, and source isolation.
Local sanitizer tests do not prove NFS locking or directory synchronization.
No scheduled cleanup is enabled by this command alone.
Retention policy and a live NFS dry run remain required before activation.

## Owned Anvil evidence reads

`GET /api/turns/:turn/anvil-links/:link/evidence` reads one existing Anvil reference.
Shared gateway SSO is required.
Loop checks the local receipt and immutable reference before contacting Anvil.
Learning reports, answer reviews, and experiment references support this endpoint.
Experiment reads return existing Anvil cases, outputs, verdicts, and gate evidence.

The C client forwards the current session cookie to the fixed Anvil HTTPS authority.
TLS checks remain enabled. Redirects and malformed response framing fail the request.
Cookies are neither stored nor returned. Network reads do not hold the evidence database lock.
Responses are bounded to one MiB and must match the reference and authenticated subject.
Experiment responses also bind the exact returned snapshot bytes with SHA-256.
That hash identifies a read snapshot. Anvil can change its experiment after the read.
Answer-review responses must report that promotion eligibility is false.
A failed transport or binding check returns `anvil_evidence_unverified` with HTTP 502.

The returned object remains Anvil evidence.
Loop does not infer model admission, provenance, or promotion from a stored report hash.
This endpoint requires the Anvil evidence API release before deployed acceptance.

The package test script runs the production C HTTPS client against a local TLS fixture.
Disposable certificates and a synthetic cookie test trust, hostname checks, forwarding, redirects, framing, and response limits.
The client runs with address and undefined-behavior sanitizers.
This test does not establish shared-session acceptance against deployed Anvil.


## Prepared requests in Anvil experiments

`GET /api/turns/:id/model-request` verifies the owned recording and its edge signature.
The response includes `requestBase64`, which preserves the exact prepared bytes after JSON parsing.
Its request hash covers the decoded bytes, including whitespace.
The signature proves capture at the voice edge, not inference completion or model weights.
Historical signature checks still depend on the current signing key.

The internal delegated route is `GET /api/anvil-model-request/:id`.
It calls the same C projection and requires shared gateway identity.
The gateway requires a signed ID token and the existing administrator group.
It admits no mutation, cookie identity, preview identity, or external listener.
Anvil independently verifies the token before this owned read.

In Trace, select `prepare captured request for Anvil`.
Open the Anvil comparison link and choose an admitted challenger.
Approve storing the complete prompt and context in the private Anvil experiment.
Anvil reads the recording again and verifies both expected hashes.
The link carries only the turn ID and hashes.

Anvil retains its four core cases and adds one through three captured trials.
Its existing controller owns model placement, execution, restoration, review, and promotion.
Captured trials retain the prepared settings and bytes except for an admitted model alias change.
The trial stores both request hashes and the exact transformation label.
Provider-reported identity and usage remain separate from controller configuration.
Neither record attests model weights in memory.
These source checks do not establish deployed controlled execution or human acceptance.
