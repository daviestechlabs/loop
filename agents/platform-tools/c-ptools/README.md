# c-ptools — pure-C platform-tools host

This package runs the bounded tool manager and its local workers.
Authenticated HTTP admits dice, campaign, and encounter commands.

## Authenticated HTTP

Set `TOOL_HTTP_ADDR` to `:8081` or an IPv4 address such as `127.0.0.1:8081`.
Set `TOOL_HTTP_AUTH_SECRET` through the service secret environment.
The dedicated key must contain 32 to 255 bytes.
Do not reuse the product gateway key.
Do not set `NATS_URL`.

| Route | Behavior |
|---|---|
| `GET /healthz` | Public health; reports configured authentication |
| `POST /v1/tools/execute` | Authenticated D&D tool execution |
| `POST /v1/tools/calls` | Same bounded D&D admission |
| `GET /v1/tools/calls/{id}` | Stored owner reads a call |
| `POST /v1/tools/calls/{id}/cancel` | Stored owner cancels a pending call |

Each protected request needs four unique headers.

| Header | Value |
|---|---|
| `X-Tool-User` | Authenticated user identifier |
| `X-Tool-Timestamp` | Decimal Unix seconds within 30 seconds of the host clock |
| `X-Tool-Nonce` | Fresh 32-character lowercase hexadecimal nonce |
| `X-Tool-Signature` | 64-character lowercase HMAC-SHA256 signature |

The signature uses `voice_auth_sign` from the shared C runtime.
Its canonical fields bind the method, path, user, timestamp, nonce, and body hash.
The dedicated tool key separates this service from the voice gateway.
The bounded nonce cache rejects replay during the process lifetime.
Durable idempotency protects dice retries across restart.

POST requires one Content-Length and one `application/json` Content-Type.
Headers are bounded to 8192 bytes.
Bodies are bounded to 16384 bytes.
Transfer encoding, ambiguous headers, embedded NULs, and truncated requests fail.
The listener serializes requests with bounded socket deadlines.

A dice request requires these fields.

```json
{
  "tool_call_id": "dice-1",
  "idempotency_key": "roll-1",
  "parent_turn_id": "turn-1",
  "session_id": "table-1",
  "agent_id": "dnd-agent",
  "tool_id": "dnd-dice-roll",
  "input_json": "{\"expression\":\"2d6+3\"}"
}
```

The verified header supplies the stored user.
An optional body `user_id` must match it.
An optional `deadline_unix_ms` must fall within the next 60 seconds.
The default deadline is five seconds.
Unknown fields, duplicate keys, and other tool routes reject admission.
The response contains `accepted`, a complete `call` record, and `signature`.
The record stores the typed dice result in its `output_json` string.
The response signature uses method `RESULT` with the original path, user, timestamp, and nonce.
Its signed body is the exact JSON bytes of the nested `call` object.
This binds the result to one authenticated request.
Completed rolls stay complete when the owner sends cancellation.

The host saves dice output before it marks the call complete.
`output_sha256` identifies the exact output bytes.
`output_artifact` contains `sha256:<hash>`.
The artifact resides at `TOOL_ARTIFACT_DIR/<hash>.json` with mode 0600.
File and directory synchronization precede a successful response.
An existing artifact must match; the host never replaces corrupt bytes.
Missing or changed artifacts reject authenticated reads and retries.

Startup checks retained dice artifacts before the listener accepts requests.
It restores mode 0600 only for intact, singly linked mode-0660 files owned by the service user and primary group.
It compares every byte before changing permissions through the same file descriptor.
Other invalid artifacts remain unchanged and unavailable.
HTTP reads never repair permissions.
The Deployment uses `OnRootMismatch` to avoid repeated volume permission changes when its root already matches.

### Retained call history

The host caches at most 128 decoded call records.
A private SQLite index locates every retained call and its scoped retry key.
The index uses a bounded page cache and resides beside the call store.
It does not require a database service.
JSON records, immutable artifacts, and D&D journals remain authoritative.

Startup validates every retained record before it accepts requests.
It rebuilds the index and checks artifacts across the complete history.
Cold lookups validate the selected record before caching it.
An invalid cold record or uncertain write blocks new work until restart and validation.
The health endpoint returns 503 while that failure remains latched.
The existing Kubernetes probes then restart the host and require startup validation.
The host does not evict durable receipts or reuse their retry keys.

Normal shutdown removes the process's private `.lookup-*` file.
An abrupt exit can leave that disposable index file.
Operators can remove these index files while the host is stopped.
Do not remove the snapshot, hashed record directory, artifacts, or D&D journals.
One service process still owns the volume.
The native build requires SQLite headers and its runtime library.

### Owned campaign and encounter commands

State commands use the same signed envelope and result signature as dice.
Set `tool_id` to `dnd-campaign-state` or `dnd-encounter-state`.
The `input_json` string holds one complete command.
The strict C command parsers reject unknown fields, duplicates, invalid versions, and truncation before admission.
Retries require the original command bytes and parent identities.

A campaign create command uses this inner object.

```json
{"operation":"create","campaign_id":"table","expected_version":0,"campaign":{"name":"Lost Mines","ruleset":"5e"}}
```

The verified HTTP user becomes the stored campaign owner.
Every later campaign command requires that owner.
The `get_roster` command accepts only `operation` and `campaign_id`.
It returns the campaign version, status, and each character's ID, name, kind, and maximum HP.
It validates the complete retained campaign before projecting those fields.
It does not expose account references, personas, knowledge scopes, or recaps.
An encounter start command uses this inner object.

```json
{"operation":"start","campaign_id":"table","encounter_id":"battle","expected_version":0}
```

Every encounter command requires an existing campaign owned by the verified user.
The encounter also retains its own owner check.
Saved result reads, retries, cancellation, and journal recovery check current campaign authority again.
Missing, malformed, linked, or conflicting campaign files deny access.
Old orphan encounters remain unchanged and unavailable; the service never invents parent authority.

The owner policy does not grant access to collaborators or licensed books.
Campaign requests cannot assign an owner, membership, or entitlement.
Only new campaign creation can proceed without a retained campaign file.
Committed work must finish recovery before new state work starts.
Request deadlines bound admission; a committed transaction must still complete after interruption.

Run `python3 test_state_authority.py` after building the host and fixtures.
The tests verify signed state lifecycles, exact Unicode receipts, owner isolation, restart, and six campaign crash boundaries.
They also check authority changes during pending encounter recovery.

### Scene observations

The campaign owner can record up to 20 observations for one identified scene.
Each observation names an existing character, presence, visibility, and source turn.
Presence is `present`, `absent`, or `unknown`.
Visibility is `table` or `dm`.
The source turn is an owner-supplied reference.
The service does not verify that transcript or infer facts from it.

`set_scene_observations` replaces the complete observation set.
The write requires the current campaign version.
The service assigns the next campaign version to the observation set.
An empty set clears prior observations.
Identifiers use the campaign ID syntax, with at most 64 characters.

```json
{"operation":"set_scene_observations","campaign_id":"table","expected_version":2,"scene_observations":{"scene_id":"hall","entries":[{"character_id":"mira","presence":"present","visibility":"table","source_turn_id":"mira-enters-hall"}]}}
```

`get_scene_presence` requires the campaign version, scene ID, and character ID.
It returns one table-visible observation at that exact campaign version.
The command requires the existing campaign owner.
It does not grant player or collaborator access.

```json
{"operation":"get_scene_presence","campaign_id":"table","expected_version":3,"scene_id":"hall","character_id":"mira"}
```

Hidden, missing, stale, or archived observations return `unknown` with an empty source reference.
A different scene or character also returns `unknown`.
The result does not explain which of those conditions applied.
Any campaign mutation makes older observations stale until the owner records a new set.
This check compares campaign versions; it does not measure elapsed time.
Room presence does not establish spell geometry or authorize an action.

Existing campaign response shapes remain unchanged.
The retained state preserves observations through older commands.
Only the new write response echoes the complete observation set.
A saved read receipt retains its original version across retries.
Consumers must compare that version with current authority before using the result.
The dedicated `dnd-scene-presence` tool accepts only `resolve_scene_presence`.
It requires `campaign_id`, `scene_id`, and `character_name`.
It resolves the name and observation from one current owned campaign version.
ASCII names compare without case; other UTF-8 bytes compare exactly.
Duplicate names return `unknown`.
The query accepts no mutation, roster read, or private campaign read.

```json
{"operation":"resolve_scene_presence","campaign_id":"table","scene_id":"hall","character_name":"Mira"}
```

This tool uses the same authenticated HTTP envelope and immutable result artifacts.
Its recovery journal is `scene-query-transaction-v1.json` with schema `dnd-scene-query-transaction/v1`.
Read recovery never rewrites the campaign file.
Each C voice invocation uses a fresh query identity, even when the parent request ID repeats.
Direct tool retries still return their original historical receipt.
The shared store retains all receipts and caches at most 128 decoded calls.

Run the portable C contract tests on Waterdeep.

```bash
make -C agents/platform-tools/c-ptools test-scene-observations
```

These tests execute the production campaign parser, transition, projection, and canonical encoder.
They do not test the HTTP listener or filesystem crash recovery.
The Linux package gates retain those checks.
The [local evidence](evidence/2026-09-13-scene-contract.md) records coverage and the deliberate fault checks.

## Durable retries

Retry keys bind the user, session, agent, and tool.
The original input and parent identities must match.
The manager serializes call transactions.
Each write replaces one hashed record after file synchronization.
The directory is synchronized after rename.
Memory changes only after a successful write.

Restart restores complete records, including escaped strings and parent identities.
Malformed records, mismatched filenames, and conflicting scoped retry keys reject startup.
The legacy snapshot stays unchanged.
The store requires one service process on its volume.

## State tools

### Encounter command and state boundaries

The encounter host validates complete commands with the shared C JSON parser.
Each operation accepts only its required fields and documented optional fields.
Unknown fields, duplicate keys, nested field substitution, invalid numbers, and truncated values fail before state access.
Participant creation requires an ID, name, initiative, and maximum HP.
Missing current HP defaults to maximum HP; explicit zero remains zero.
Optional conditions use the kernel's sorted, unique condition list.
Damage accepts an optional damage type.
Advance, damage, healing, condition changes, and ending also accept `include_previous: true`.
This option adds the prior active state to the signed output for independent transition verification.
The prior snapshot omits the owner; the authenticated call retains that identity binding.
Output capacity failure stops the transition before persistence.
Requests without this option retain the existing output and journal format.
The product uses this option only with bounded typed encounter changes.
Mutations require the current version; start requires version zero.

Reads validate every retained state field and match campaign, encounter, and owner identities.
Malformed, oversized, linked, and non-regular state files remain unavailable and unchanged.
Valid legacy JSON files retain their format.
Output preserves escaped names and every participant, or the operation fails before persistence.
Writes reuse the C tool store's temporary file, rename, and synchronization sequence.
New state files use mode 0600.
The state directory requires one serialized writer and trusted parent directories.
An uncertain write leaves the call queued until the manager completes recovery.

The process fixture checks JSON with an independent Python oracle and temporary local files.
It covers restart, ownership, zero HP, integer bounds, counters, output capacity, and injected synchronization failures.
Healing clamps before addition; participant insertion preserves the active participant across initiative sorting.
Run `python3 test_encounter.py` after building `c-ptools-encounter-fixture`.
The HTTP authority tests establish the separate signed admission boundary.

### D&D state recovery

The manager owns encounter and campaign execution through one C transaction engine.
It serializes both tools on the same volume.
The generic worker cannot bypass that transaction.
The manager saves the queued call before preparing a state result.
It then saves the immutable result artifact and one bounded recovery journal.
The journal contains the before state, after state, and completed call.
The host saves domain state and the completed call before removing the journal.
Each durable write uses file and directory synchronization.

Startup completes a retained transaction before the listener accepts calls.
Every state request checks both journals before admission.
Recovery reconstructs the transition from the retained request and before state.
It verifies the artifact and every completed call field before writing.
The current state must match the before or after state.
A completed cleanup journal accepts later versions without rewriting them.
Missing artifacts, changed identities, corrupt journals, and conflicting state reject recovery.
The original files remain available for operator inspection.

An uncertain journal or state write leaves the call queued for recovery.
A queued retry without a committed journal may execute the original command.
Changed inputs and wrong owners still reject that retry.
Completed retries return the same result and verified artifact.
Cancellation can stop a queued call before journal commit.
After commit, cancellation and owned reads complete recovery or report pending recovery without invalidating the journal.
Old completed state records without artifacts remain unavailable; startup does not manufacture their provenance.
Read operations create a result artifact but do not rewrite domain state.

Each built-in tool has one fixed journal filename.
Existing encounter commands retain their version-one journal format.
Initiative rolls use version two under the same encounter journal filename.
The campaign journal uses `campaign-transaction-v1.json` and schema `dnd-campaign-transaction/v1`.
Each journal has a fixed byte limit derived from the existing state and call bounds.
Recovery rejects a journal under the other tool's filename.
The call structure and its 128-record cache retain their existing sizes.
Run `python3 test_encounter_recovery.py` after building the native fixture.
The fixture kills the process at six write boundaries and verifies restart with an independent JSON oracle.
Run `python3 test_state_recovery.py` to check campaign recovery and shared admission behavior.
Those tests cover preserved collections, immutable artifacts, pending reads, cancellation, and rejection of conflicting state.
These tests cover process exit and injected I/O errors, not storage-device power loss.
Collaborator grants and initiative voice intent routing remain separate gates.
The product already returns typed encounter snapshots for explicit refresh commands.

### Initiative rolls and saved encounters

`roll_initiative` selects campaign characters and saves their initiative in one encounter transaction.
The command requires the campaign owner, current campaign version, and expected encounter version.
Version zero creates a new encounter.
An existing encounter must remain active.
The command selects one to 64 unique character IDs from the complete retained campaign.

Each selection supplies an explicit final expression: `1d20`, `2d20kh1`, or `2d20kl1`, with an optional signed modifier.
Modifiers range from -101 to 80, so every possible total fits the existing initiative range.
Expressions use canonical spelling, without spaces, leading zeroes, or a zero modifier.
The host does not infer feats, equipment, house rules, or a final modifier from Dexterity.
The existing C dice kernel obtains unbiased rolls from `getrandom`.

The transaction journal captures the canonical campaign snapshot and raw dice outcomes.
Recovery recomputes selection, arithmetic, order, state, and the complete receipt without obtaining new entropy.
The receipt includes each expression, raw rolls, kept index, total, and the campaign snapshot hash.
After journal commit, retries and recovery retain those exact outcomes.
Before journal commit, an interrupted attempt may replace unpublished draws.
Such an attempt does not change encounter state.

Existing participants retain their names, HP, and conditions.
New participants use campaign names and start at their recorded maximum HP.
Unselected participants, the current round, and the active participant identity remain intact.
The C kernel sorts initiative in descending order, then character IDs in ascending order.
The encounter version advances once.
Campaign records remain unchanged.

The complete saved encounter must fit the existing 64-participant, 8192-byte product projection.
The host checks that limit with the runtime C protobuf encoder before persistence.
Names must fit 120 UTF-8 bytes and the public snapshot validation rules.
A pending journal requires the current campaign to match its pinned snapshot.
A completed cleanup journal permits later authorized versions and never rewinds them.
Missing artifacts and conflicting campaign or encounter state reject recovery.

Run `python3 test_initiative.py` for signed HTTP, arithmetic, crash, authority, and capacity checks.
The fixture disables entropy during committed recovery and checks all six process exit boundaries.
Its controlled entropy wrapper exists only in test binaries.
The browser does not yet supply these admitted selections and expressions through a product turn.
See the [local initiative proof](../evidence/initiative-transaction-local-2026-09-08.md) for validation and remaining gates.

### Campaign command and state boundaries

The C host validates complete campaign metadata and roster commands.
Metadata operations are `create`, `get`, `update`, and `set_scene`.
Roster operations are `upsert_character`, `add_character`, and `remove_character`.
Each operation accepts only the fields it uses.
Unknown fields, duplicates, nested substitutions, invalid numbers, and truncated values fail before state access.
Campaign identifiers use the existing C kernel's 64-byte identifier policy.
The host validates the stored campaign and owner against the request.

`add_character` uses the complete character contract and a pinned campaign version.
It rejects an existing character ID before any replacement can occur.
Other character records and private collections retain their validated bytes.

Create and update require a name and ruleset.
Create defaults omitted description, scene, and house rules to empty values.
Update preserves those fields when omitted; explicit empty values clear them.
Metadata uses the existing C byte limits, including the full 4000-byte description.
The host preserves escaped text and Unicode without silent truncation.

Retained characters, recaps, house rules, and scene leases receive complete validation.
Updates preserve all validated collections.
The output includes those collections and the complete scene director.
The old empty director shape remains readable and receives complete defaults in output.
House-rule maps accept 100 entries; values accept 1000 Unicode characters.
Safety rules accept 500 Unicode characters; recap summaries accept 1200.
A span iterator reads maps without enlarging the shared JSON field directory.

Upsert requires one complete `character` record and the current campaign version.
It replaces an existing identifier in place or appends a new identifier.
It does not merge partial character fields.
Remove requires `character_id` and the current version; a missing character rejects the command.
Both operations preserve other characters, recaps, house rules, and scene leases.
The host validates the resulting roster before it prepares state or output.
Removing an active scene character or making that character ineligible rejects the mutation.

Character player identifiers and knowledge scopes describe campaign data.
They do not grant campaign membership, tool authority, or book access.
The verified caller must remain the campaign owner.
Changed retries fail; identical retries return the saved roster snapshot after restart.
Existing encounters retain their own state until an explicit encounter operation changes it.

The complete inner command must fit the existing 8192-byte buffer, including its terminator.
The roster permits at most 200 characters, subject to the smaller total state and output byte limits.
Oversized commands and resulting snapshots reject without truncation or partial persistence.
Run `python3 test_roster.py` for signed admission, preservation, ownership, capacity, and crash recovery proof.
See the [local roster proof](../evidence/roster-admission-local-2026-09-08.md) for validation and remaining product gates.

State and output each have a 65536-byte buffer, including the terminator.
Both encodings must fit before a mutation writes state.
Reads reject malformed, oversized, linked, and non-regular files without changing them.
Writes use temporary files, rename, and file and directory synchronization.
The state directory requires one serialized writer and trusted parent directories.
Campaign calls use the shared recovery engine and immutable result artifacts described above.
An uncertain committed write stays queued until recovery completes.

Run `python3 test_campaign.py` after building `c-ptools-campaign-fixture`.
Its independent JSON oracle checks retained data, schema fields, ownership, limits, output capacity, and synchronization failures.
Collaborator grants and product voice routing remain open gates.

## Validation and deployment

Run `make test`, `make sanitize`, `make thread-sanitize`, and `make fuzz-smoke`.
HTTP integration tests use the real C listener and temporary local directories.
Both Dockerfiles build from the monorepo root and run the host tests.
The host shares the product JSON and HTTP parsers, UTF-8 validator, and runtime HMAC implementation.
It also uses the runtime C protobuf encoder to check saved encounter projection capacity.

The C cascade client and dice artifacts have local process proof.
Desired state selects authenticated HTTP and admits the C runtime through its NetworkPolicy.
External Secrets derives a dedicated tool key from Vault with a distinct domain string.
The template rejects a missing root or fewer than 32 bytes after trimming.
The C cascade reads the same secret key.

Authenticated calls use `/data/http-auth-v1/tool-calls.json` and `/data/http-auth-v1/artifacts`.
The prior snapshot and artifacts remain at their original paths.
The HTTP deployment does not import those unauthenticated records.
Source changes do not prove a live Flux cutover.
