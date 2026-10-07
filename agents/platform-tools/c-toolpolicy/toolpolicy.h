/* toolpolicy.h — pure-C platform-tools registry policy validation (no Go). */
#ifndef PLATFORM_TOOLS_POLICY_H
#define PLATFORM_TOOLS_POLICY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TP_OK = 0,
    TP_ERR = 1
};

#define TP_STR 256
#define TP_PATH 512
#define TP_MSG 512
#define TP_MAX_TOOLS 64
#define TP_MAX_AGENTS 64
#define TP_MAX_AGENT_TOOLS 32

/* Risk names matching messages.ToolRiskLevel strings (lowercase). */
const char *tp_parse_risk(const char *raw); /* returns "low"|...|"unspecified" */

/* Validate one tool JSON object. err filled on failure. */
int tp_validate_tool_json(const char *tool_json, char *err, size_t err_cap);

/*
 * Validate full policy JSON:
 * {"version":"...","tools":[...],"agents":[{"id":"...","tools":[{"id":"..."}]}]}
 */
int tp_validate_policy_json(const char *json, char *err, size_t err_cap);

/* Builtin compiled tool ids. */
int tp_is_builtin_tool(const char *id);

/*
 * Compile-time agent/tool authority for the pure-C runtime.  The security
 * baseline keeps this table in lockstep with the mounted registry; callers
 * cannot widen it with request metadata or a ToolDefinitionSnapshot.
 */
int tp_agent_allows_tool(const char *agent_id, const char *tool_id);

#ifdef __cplusplus
}
#endif

#endif
