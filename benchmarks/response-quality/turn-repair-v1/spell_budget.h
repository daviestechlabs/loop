#ifndef RESEARCH_SPELL_BUDGET_H
#define RESEARCH_SPELL_BUDGET_H

#include <stdint.h>

/* Offline research only. Capacities are manually annotated remaining resources,
 * after resolving feature applicability. Haste is not an unrestricted action.
 * This is neither a natural-language parser nor permission to execute a spell. */
enum { BUDGET_2014 = 1, BUDGET_RULES_COMPLETE = 3, BUDGET_MAX_SPELLS = 2 };
enum { COST_UNKNOWN = 0, COST_ACTION = 1, COST_BONUS = 2, COST_REACTION = 3 };
enum { LEVELLED = 0, CANTRIP = 1, CANTRIP_UNKNOWN = 2 };
enum { HISTORY_NONE = 0, HISTORY_ACTION_CANTRIPS = 1, HISTORY_BONUS = 2,
       HISTORY_INCOMPATIBLE = 3, HISTORY_UNKNOWN = 4 };
enum { BUDGET_INVALID = 0, BUDGET_UNSUPPORTED = 1, BUDGET_UNKNOWN = 2,
       BUDGET_FITS = 3, BUDGET_EXCEEDS = 4 };
enum { REJECT_ACTION = 1, REJECT_BONUS = 2, REJECT_REACTION = 4,
       REJECT_BONUS_SPELL = 8, MISSING_RULES = 16, MISSING_FACTS = 32 };

typedef struct { uint8_t minimum, maximum; } budget_capacity;
typedef struct { uint8_t cost, cantrip; } budget_spell;
typedef struct {
    uint8_t ruleset, spell_count, admitted_rules;
    budget_capacity action, bonus, reaction;
    uint8_t history;
    budget_spell spells[BUDGET_MAX_SPELLS];
} spell_budget_input;
typedef struct {
    /* rejecting_reasons is a union of diagnostics, not a universal explanation. */
    uint32_t status, possible_worlds, fitting_worlds, rejecting_reasons, universal_reasons;
} spell_budget_result;

/* All worlds must agree for a definite result. No allocation, IO, or mutation. */
spell_budget_result spell_budget_evaluate(const spell_budget_input *input);

#endif
