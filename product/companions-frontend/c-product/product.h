/* product.h — pure-C companions product policy (oauth/avatar/mission/rules). */
#ifndef COMPANIONS_C_PRODUCT_H
#define COMPANIONS_C_PRODUCT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PROD_OK = 0, PROD_ERR = 1 };

#define PROD_DEFAULT_AVATAR "models/free/Dungeon-Master.vrm"
#define PROD_MAX_OAUTH_CALLBACK_CODE 4096
#define PROD_OAUTH_STATE_DECODED_LEN 32

/* OAuth username: ^[A-Za-z0-9][A-Za-z0-9._@+-]{2,49}$ (len 3..50) */
int prod_oauth_username_ok(const char *username);

/* PKCE verifier: ^[A-Za-z0-9._~-]{43,128}$ */
int prod_oauth_pkce_ok(const char *verifier);

/* Provider name: ^[a-z0-9][a-z0-9._-]{0,49}$ (len 1..50) */
int prod_oauth_provider_ok(const char *name);

/* Callback code: non-empty, <=4096, no NUL/CR/LF */
int prod_oauth_callback_code_ok(const char *code);

/*
 * Identity field: UTF-8 bytes len in [min_bytes,max_bytes], equals trim,
 * no C0 control (except none) or DEL.
 */
int prod_oauth_identity_field_ok(const char *value, int min_bytes, int max_bytes);

/* Decoded OAuth state length must be exactly 32. */
int prod_oauth_state_decoded_len_ok(size_t n);

/* Avatar model allowlist (exact match). */
int prod_avatar_ok(const char *model);

/* Write default avatar path into out; returns PROD_OK. */
int prod_avatar_default(char *out, size_t cap);

/*
 * Write allowlisted models newline-separated into out.
 * *n_written = bytes without trailing NUL if needed; always NUL-terminated if cap>0.
 */
int prod_avatar_list(char *out, size_t cap);

/* Mission activity key allowlist (exact after caller trim). */
int prod_mission_activity_ok(const char *activity_key);

/* Rules-question heuristic (case-insensitive contains tables). */
int prod_looks_like_rules_question(const char *message);

/*
 * OAuth provider error normalize: trim + lowercase; keep
 * access_denied|temporarily_unavailable|server_error; else provider_error.
 * Writes into out (NUL-terminated). Returns PROD_OK.
 */
int prod_oauth_provider_error_norm(const char *raw, char *out, size_t cap);

/*
 * Local OAuth failure reason allowlist (exact match, no case fold).
 * Unknown → provider_error.
 */
int prod_oauth_local_failure_norm(const char *raw, char *out, size_t cap);

/*
 * Analytics dimension: trim; empty or len>limit → "";
 * allow ASCII alnum + -_:.@ and non-ASCII UTF-8 (letter-like); else "".
 * Writes accepted value into out. Returns PROD_OK if kept, PROD_ERR if rejected.
 */
int prod_bounded_dimension(const char *value, int limit, char *out, size_t cap);

/* default_dimension: bounded or fallback (always copies a non-null string into out). */
int prod_default_dimension(const char *value, const char *fallback, int limit, char *out, size_t cap);

/*
 * Analytics path: trim; must start with /; len<=256; strip ?query.
 * Returns PROD_OK if kept.
 */
int prod_bounded_path(const char *value, char *out, size_t cap);

/*
 * Health default transports for service type llm|stt|tts.
 * Writes comma-separated list (no spaces). Unknown → empty, PROD_ERR.
 */
int prod_default_transports(const char *service_type, char *out, size_t cap);

/*
 * Infer provider from raw URL (lowercased contains):
 * vllm→vLLM, orpheus→Orpheus, whisper→Whisper, ray→Ray Serve,
 * empty URL→"", else custom.
 */
int prod_infer_provider(const char *raw_url, char *out, size_t cap);

/*
 * Health mode: llm/stt/tts as 0|1.
 * all healthy → healthy; llm healthy → degraded; else down.
 */
int prod_health_mode(int llm_ok, int stt_ok, int tts_ok, char *out, size_t cap);

/*
 * Gateway turns path policy (after URL parse):
 * trim trailing '/'; empty or "/v1/voice/turns" → "/v1/voice/turns"; else ERR.
 */
int prod_gateway_turns_path(const char *path, char *out, size_t cap);

/*
 * Sanitize endpoint for health display: strip userinfo; keep scheme://host/path
 * (drops query/fragment). Unparseable → copy raw. Empty → empty.
 */
int prod_sanitize_endpoint(const char *raw_url, char *out, size_t cap);

/*
 * Export format metadata: mime_type and extension for markdown|txt|html|default(json).
 * Writes mime and ext (without leading dot).
 */
int prod_export_format(const char *format, char *mime_out, size_t mime_cap, char *ext_out,
                       size_t ext_cap);

/* user → "You"; anything else → "Companion". */
int prod_export_role_name(const char *sender, char *out, size_t cap);

#define PROD_EXPORT_MSG_TEXT 10000
#define PROD_EXPORT_MAX_MSGS 256

typedef struct prod_export_msg {
    char sender[32];
    char text[PROD_EXPORT_MSG_TEXT];
} prod_export_msg;

/*
 * Render session export body for format markdown|txt|html|json (default).
 * export_date is YYYY-MM-DD (or any freeform date string used in headers).
 * msgs may be empty (still renders headers/empty conversation).
 */
int prod_export_render(const char *format, const char *export_date, const prod_export_msg *msgs,
                       size_t nmsgs, char *out, size_t cap);

/*
 * Health summary text from mode + service flags (llm/stt/tts as 0|1).
 * healthy → fixed ready string; degraded/down list unavailable among LLM,STT,TTS.
 */
int prod_summarize_health(const char *mode, int llm_ok, int stt_ok, int tts_ok, char *out,
                          size_t cap);

/*
 * WebSocket control message type:
 *   login → "ignore"
 *   cancel_turn|stop_tts → "cancel"
 *   else → "unknown"
 */
int prod_ws_control_kind(const char *type, char *out, size_t cap);

/* Vision consent path action: start|revoke|stop → PROD_OK; else PROD_ERR. */
int prod_vision_consent_action_ok(const char *action);

/* Turn-identity key derivation (SHA-256(root || info) → 64 lowercase hex). */
#define PROD_TURN_IDENTITY_INFO "daviestechlabs/turn-identity/companions-frontend/v1"
#define PROD_TURN_IDENTITY_MAX_FIELD 256

/*
 * Derive signing key material for turn-identity JWT (HS256 key bytes as hex).
 * empty root → PROD_ERR. out_hex needs 65 bytes.
 */
int prod_turn_identity_derive_key(const unsigned char *root, size_t root_len, char *out_hex,
                                  size_t cap);

/*
 * Field gate for IssueTurnIdentityToken: each non-empty and len <= 256.
 * Returns PROD_OK if all three valid.
 */
int prod_turn_identity_fields_ok(const char *request_id, const char *user_id, const char *username);

/* ── Default product seed catalogs (monster tokens + daily missions) ── */

#define PROD_SEED_JSON_CAP 65536

/* Canonical default catalog sizes (closed product tables). */
size_t prod_monster_token_seed_count(void);
size_t prod_daily_mission_seed_count(void);

/*
 * Write JSON array of default monster token seeds into out.
 * Fields: id,name,type,cr,subtitle,max_hp,armor_class,image_key,tier.
 * Returns PROD_OK, or PROD_ERR if cap too small / null.
 */
int prod_monster_token_seeds_json(char *out, size_t cap);

/*
 * Write JSON array of default daily mission seeds into out.
 * Fields: id,title,description,activity_key,target_count,reward_xp,type,
 * reset_period,tier,icon,sort_order,active.
 */
int prod_daily_mission_seeds_json(char *out, size_t cap);

/* ── Progression pure-CPU (level / XP / rank tables) ── */

#define PROD_RANK_CAP 64

/* XP required to *reach* level (level<=1 → 0; else (level-1)^2 * 100). */
int prod_xp_required_for_level(int level);

/* Level for total_xp: floor(sqrt(total_xp/100))+1, clamped >=1. Negative XP → 0 treated as 0. */
int prod_level_for_xp(int total_xp);

/* Rank title for level (closed table). Writes into out. */
int prod_rank_for_level(int level, char *out, size_t cap);

/*
 * Full progression summary for total_xp.
 * Writes rank into rank_out; other fields via out pointers (nullable ok for unused).
 */
int prod_progression_for_xp(int total_xp, int *level_out, int *xp_into_out, int *xp_for_next_out,
                            int *next_level_xp_out, int *total_xp_out, char *rank_out,
                            size_t rank_cap);

/*
 * JSON: {"total_xp","level","rank","xp_into_level","xp_for_next_level","next_level_xp"}.
 */
int prod_progression_json(int total_xp, char *out, size_t cap);

/* ── OAuth provider config + endpoint URL policy ── */

#define PROD_OAUTH_URL_CAP 512
#define PROD_OAUTH_SCOPES_CAP 256

/*
 * Static provider config (github/google have full URLs; authentik/oidc scopes only).
 * auth_url/token_url/userinfo may be empty for authentik|oidc (env-driven residual host).
 * scopes written comma-separated. Unknown provider → PROD_ERR.
 */
int prod_oauth_provider_config(const char *provider, char *auth_url, size_t auth_cap,
                               char *token_url, size_t token_cap, char *userinfo_url,
                               size_t userinfo_cap, char *scopes_csv, size_t scopes_cap);

/*
 * Canonicalize/validate OAuth endpoint URL (pure-C mirror of residual Go rules):
 * absolute http(s), no userinfo/query/fragment, https or loopback http,
 * non-empty path without //, \, or . / .. segments.
 * callback=1 requires path exactly /api/oauth/callback.
 * On success writes canonical raw URL into out.
 */
int prod_oauth_endpoint_url(const char *raw, int callback, char *out, size_t cap);

#define PROD_OAUTH_USERINFO_FIELD_CAP 256
#define PROD_OAUTH_USERINFO_ERR_CAP 64

/*
 * Map a provider userinfo JSON body onto residual host fields.
 * github: id (decimal) + login; google: sub + email prefix;
 * authentik|oidc: sub + preferred_username, or verified-email prefix.
 * Validates identity/username/email/name with the same gates as residual Go.
 * On PROD_ERR writes a short token into err when err_cap > 0.
 */
int prod_oauth_parse_userinfo(const char *provider, const char *body, char *id, size_t id_cap,
                              char *username, size_t username_cap, char *email, size_t email_cap,
                              char *name, size_t name_cap, char *err, size_t err_cap);

/* ── JWT product-token policy constants ── */

#define PROD_JWT_MIN_KEY_BYTES 32
#define PROD_JWT_ISSUER "companions-frontend"
#define PROD_JWT_AUDIENCE "companions-product"

int prod_jwt_min_key_bytes(void);
int prod_jwt_key_len_ok(size_t n);
int prod_jwt_issuer(char *out, size_t cap);
int prod_jwt_audience(char *out, size_t cap);

/* ── Storage asset path policy (S3 keys) ── */

#define PROD_ASSET_NAME_CAP 256
#define PROD_TOKEN_IMAGE_FALLBACK "/assets/avatars/default-token.svg"

/* 1 if key ends with .webp|.svg|.png (case-sensitive, matches residual Go). */
int prod_asset_is_token_image(const char *key);

/* 1 if key ends with .vrm / .vrma. */
int prod_asset_is_vrm(const char *key);
int prod_asset_is_vrma(const char *key);

/* 1 if key is free of ".." path traversal. Empty key is unsafe. */
int prod_asset_key_safe(const char *key);

/*
 * Filename stem from S3 key (last path segment, strip final extension).
 * Empty key → empty out, PROD_OK. out always NUL-terminated on PROD_OK.
 */
int prod_asset_extract_name(const char *key, char *out, size_t cap);

/* Default local placeholder for token images without S3. */
int prod_token_image_fallback(char *out, size_t cap);

/*
 * Profile avatar upload extension allowlist (case-insensitive):
 * .jpg .jpeg .png .gif .webp
 * Accepts full filename or extension-only (with or without leading '.').
 */
int prod_avatar_upload_ext_ok(const char *filename_or_ext);

/*
 * Profile avatar upload Content-Type allowlist (exact, lower case after trim):
 * image/jpeg | image/png | image/gif | image/webp
 */
int prod_avatar_upload_content_type_ok(const char *content_type);

/* Default extension when upload filename has no allowlisted ext. */
int prod_avatar_upload_default_ext(char *out, size_t cap);

/*
 * Infer subscription tier from S3 model path segment models/{tier}/…
 * Writes free|adventurer|hero|legend (default free). Always NUL-terminates on PROD_OK.
 */
int prod_asset_tier_from_key(const char *key, char *out, size_t cap);


/* Mission type vocabulary: streak | activity (closed). */
int prod_mission_type_ok(const char *type);
/* 1 if type is exactly "streak". */
int prod_mission_type_is_streak(const char *type);

#ifdef __cplusplus
}
#endif

#endif

