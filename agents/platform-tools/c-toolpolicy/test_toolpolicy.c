#define _POSIX_C_SOURCE 200809L
#include "toolpolicy.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

static void test_risk(void) {
    expect(strcmp(tp_parse_risk(" Medium "), "medium") == 0, "medium");
    expect(strcmp(tp_parse_risk("nope"), "unspecified") == 0, "unspecified");
}

static void test_builtin(void) {
    expect(tp_is_builtin_tool("workspace-edit") == 1, "edit builtin");
    expect(tp_is_builtin_tool("shell-exec") == 0, "not builtin");
}

static void test_validate_good_tool(void) {
    char err[512];
    const char *js =
        "{\"id\":\"workspace-read\",\"name\":\"Read\",\"version\":\"v1\","
        "\"inputSchema\":\"a\",\"outputSchema\":\"b\",\"authScope\":\"s\","
        "\"riskLevel\":\"low\",\"timeoutMs\":1000,"
        "\"metadata\":{\"execution_class\":\"builtin_no_shell\",\"network_egress\":\"http_only\"}}";
    expect(tp_validate_tool_json(js, err, sizeof(err)) == TP_OK, "good tool");
}

static void test_validate_bad_edit_sandbox(void) {
    char err[512];
    const char *js =
        "{\"id\":\"workspace-edit\",\"name\":\"Edit\",\"version\":\"v1\","
        "\"inputSchema\":\"a\",\"outputSchema\":\"b\",\"authScope\":\"s\","
        "\"riskLevel\":\"medium\",\"timeoutMs\":1000,"
        "\"sandbox\":{\"required\":false},"
        "\"metadata\":{\"execution_class\":\"builtin_no_shell\",\"network_egress\":\"http_only\"}}";
    expect(tp_validate_tool_json(js, err, sizeof(err)) == TP_ERR, "edit needs sandbox");
}

static void test_policy(void) {
    char err[512];
    const char *js =
        "{\"version\":\"v1\",\"tools\":[{"
        "\"id\":\"workspace-read\",\"name\":\"Read\",\"version\":\"v1\","
        "\"inputSchema\":\"a\",\"outputSchema\":\"b\",\"authScope\":\"s\","
        "\"riskLevel\":\"low\",\"timeoutMs\":1000,"
        "\"metadata\":{\"execution_class\":\"builtin_no_shell\",\"network_egress\":\"http_only\"}"
        "}],\"agents\":[{\"id\":\"coder\",\"tools\":[{\"id\":\"workspace-read\"}]}]}";
    expect(tp_validate_policy_json(js, err, sizeof(err)) == TP_OK, "policy ok");
}

static void test_agent_tool_authority(void) {
    expect(tp_agent_allows_tool("dnd-agent", "dnd-dice-roll") == 1,
           "dnd agent may roll dice");
    expect(tp_is_builtin_tool("dnd-scene-presence") && tp_agent_allows_tool("dnd-agent", "dnd-scene-presence"),
           "DND agent may read governed scene presence");
    expect(!tp_agent_allows_tool("waterdeep-coder", "dnd-scene-presence"),
           "generic workspace agent cannot read campaign presence");
    expect(tp_agent_allows_tool("dnd-agent", "workspace-read") == 0,
           "dnd agent may not read workspace");
    expect(tp_agent_allows_tool("waterdeep-coder", "workspace-read") == 1,
           "workspace agent may read");
    expect(tp_agent_allows_tool("waterdeep-coder", "dnd-campaign-state") == 0,
           "workspace agent may not mutate campaign");
    expect(tp_agent_allows_tool("", "workspace-read") == 0,
           "empty agent is denied");
    expect(tp_agent_allows_tool("unknown", "workspace-read") == 0,
           "unknown agent is denied");
}

static void test_scene_has_no_egress(void) {
    char input[1024], err[512];
    const char *modes[] = {"none", "http_only", "unrestricted"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        (void)snprintf(input, sizeof(input),
            "{\"id\":\"dnd-scene-presence\",\"name\":\"Scene\",\"version\":\"v1\","
            "\"inputSchema\":\"a\",\"outputSchema\":\"b\",\"authScope\":\"dnd:campaign\","
            "\"riskLevel\":\"low\",\"timeoutMs\":5000,"
            "\"metadata\":{\"execution_class\":\"builtin_no_shell\",\"network_egress\":\"%s\"}}", modes[i]);
        expect(tp_validate_tool_json(input, err, sizeof(err)) == (i == 0 ? TP_OK : TP_ERR),
               "scene definition requires zero tool egress");
    }
}

int main(void) {
    test_risk();
    test_builtin();
    test_validate_good_tool();
    test_validate_bad_edit_sandbox();
    test_policy();
    test_agent_tool_authority();
    test_scene_has_no_egress();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("ALL PASS c-toolpolicy unit\n");
    return 0;
}
