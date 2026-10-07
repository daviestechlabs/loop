#include "spell_budget.h"

static int capacity_valid(budget_capacity c, uint8_t maximum) {
    return c.minimum <= c.maximum && c.maximum <= maximum;
}

static uint32_t reject_world(const spell_budget_input *in, unsigned action,
    unsigned bonus, unsigned reaction, unsigned history, const budget_spell spells[2]) {
    unsigned needed[4] = {0, 0, 0, 0};
    uint32_t reasons = 0;
    for (unsigned i = 0; i < in->spell_count; i++) needed[spells[i].cost]++;
    if (needed[COST_ACTION] > action) reasons |= REJECT_ACTION;
    if (needed[COST_BONUS] > bonus) reasons |= REJECT_BONUS;
    if (needed[COST_REACTION] > reaction) reasons |= REJECT_REACTION;
    if (needed[COST_BONUS] || history == HISTORY_BONUS) {
        if (needed[COST_BONUS] && history == HISTORY_INCOMPATIBLE)
            reasons |= REJECT_BONUS_SPELL;
        for (unsigned i = 0; i < in->spell_count; i++) {
            if (spells[i].cost != COST_BONUS &&
                (spells[i].cost != COST_ACTION || spells[i].cantrip != CANTRIP))
                reasons |= REJECT_BONUS_SPELL;
        }
    }
    return reasons;
}

static void accumulate(const spell_budget_input *in, const budget_spell spells[2],
    spell_budget_result *out) {
    unsigned first = in->history == HISTORY_UNKNOWN ? HISTORY_NONE : in->history;
    unsigned last = in->history == HISTORY_UNKNOWN ? HISTORY_INCOMPATIBLE : in->history;
    for (unsigned history = first; history <= last; history++) {
        for (unsigned a = in->action.minimum; a <= in->action.maximum; a++) {
            for (unsigned b = in->bonus.minimum; b <= in->bonus.maximum; b++) {
                /* A prior bonus spell leaves no bonus action on this turn. */
                if (history == HISTORY_BONUS && b != 0) continue;
                for (unsigned r = in->reaction.minimum; r <= in->reaction.maximum; r++) {
                    uint32_t reasons = reject_world(in, a, b, r, history, spells);
                    out->universal_reasons = out->possible_worlds ? out->universal_reasons & reasons : reasons;
                    out->possible_worlds++;
                    if (!reasons) out->fitting_worlds++;
                    out->rejecting_reasons |= reasons;
                }
            }
        }
    }
}

static void expand(const spell_budget_input *in, unsigned index,
    budget_spell spells[2], spell_budget_result *out) {
    if (index == in->spell_count) {
        accumulate(in, spells, out);
        return;
    }
    budget_spell source = in->spells[index];
    unsigned first_cost = source.cost ? source.cost : COST_ACTION;
    unsigned last_cost = source.cost ? source.cost : COST_REACTION;
    unsigned first_level = source.cantrip == CANTRIP_UNKNOWN ? LEVELLED : source.cantrip;
    unsigned last_level = source.cantrip == CANTRIP_UNKNOWN ? CANTRIP : source.cantrip;
    for (unsigned cost = first_cost; cost <= last_cost; cost++) {
        for (unsigned level = first_level; level <= last_level; level++) {
            spells[index] = (budget_spell){(uint8_t)cost, (uint8_t)level};
            expand(in, index + 1, spells, out);
        }
    }
}

spell_budget_result spell_budget_evaluate(const spell_budget_input *in) {
    spell_budget_result out = {0, 0, 0, 0, 0};
    budget_spell spells[2] = {{0, 0}, {0, 0}};
    if (!in || !in->spell_count || in->spell_count > BUDGET_MAX_SPELLS ||
        !capacity_valid(in->action, 2) || !capacity_valid(in->bonus, 1) ||
        !capacity_valid(in->reaction, 1) || in->history > HISTORY_UNKNOWN ||
        (in->admitted_rules & ~BUDGET_RULES_COMPLETE)) return out;
    for (unsigned i = 0; i < BUDGET_MAX_SPELLS; i++) {
        if (in->spells[i].cost > COST_REACTION || in->spells[i].cantrip > CANTRIP_UNKNOWN ||
            (i >= in->spell_count && (in->spells[i].cost || in->spells[i].cantrip))) return out;
    }
    if (in->history == HISTORY_BONUS && in->bonus.minimum != 0) return out;
    if (in->ruleset != BUDGET_2014) {
        out.status = BUDGET_UNSUPPORTED;
        return out;
    }
    if (in->admitted_rules != BUDGET_RULES_COMPLETE) {
        out.status = BUDGET_UNKNOWN;
        out.rejecting_reasons = MISSING_RULES;
        return out;
    }
    expand(in, 0, spells, &out);
    if (!out.possible_worlds) return (spell_budget_result){0, 0, 0, 0, 0};
    out.status = out.fitting_worlds == 0 ? BUDGET_EXCEEDS :
        out.fitting_worlds == out.possible_worlds ? BUDGET_FITS : BUDGET_UNKNOWN;
    if (out.status == BUDGET_UNKNOWN) out.rejecting_reasons |= MISSING_FACTS;
    return out;
}
