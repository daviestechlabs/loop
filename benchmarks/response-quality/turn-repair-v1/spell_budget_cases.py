"""Authored typed controls. These labels are not parsed from user speech."""


def encoded(
    *,
    actions=(1, 1),
    bonus=(1, 1),
    reaction=(1, 1),
    history=0,
    spells=((1, 0), (1, 0)),
    rules=3,
    edition=1,
):
    return bytes(
        (
            edition,
            len(spells),
            rules,
            *actions,
            *bonus,
            *reaction,
            history,
            *(v for s in spells for v in s),
            *([0, 0] if len(spells) == 1 else []),
        )
    )


def controls():
    # Status: 0 invalid, 1 unsupported, 2 unknown, 3 fits, 4 exceeds.
    return [
        (
            "unknown_capabilities",
            encoded(actions=(0, 2), bonus=(0, 1), reaction=(0, 1), history=4),
            2,
        ),
        ("one_action", encoded(), 4),
        ("unused_action_surge", encoded(actions=(2, 2)), 3),
        ("haste_action_cannot_cast", encoded(), 4),
        ("spent_action_surge", encoded(), 4),
        (
            "two_action_cantrips_with_surge",
            encoded(actions=(2, 2), spells=((1, 1), (1, 1))),
            3,
        ),
        ("quickened_and_levelled", encoded(spells=((2, 0), (1, 0))), 4),
        ("quickened_and_action_cantrip", encoded(spells=((2, 0), (1, 1))), 3),
        ("bonus_cantrip_and_action_levelled", encoded(spells=((2, 1), (1, 0))), 4),
        ("bonus_and_reaction_cantrip", encoded(spells=((2, 0), (3, 1))), 4),
        ("earlier_levelled_then_bonus", encoded(spells=((2, 0),), history=3), 4),
        ("earlier_action_cantrip_then_bonus", encoded(spells=((2, 0),), history=1), 3),
        (
            "earlier_bonus_then_action_cantrip",
            encoded(bonus=(0, 0), spells=((1, 1),), history=2),
            3,
        ),
        (
            "earlier_bonus_then_action_levelled",
            encoded(bonus=(0, 0), spells=((1, 0),), history=2),
            4,
        ),
        ("unknown_earlier_spell", encoded(spells=((2, 0),), history=4), 2),
        (
            "no_bonus_even_with_unknown_history",
            encoded(bonus=(0, 0), spells=((2, 0),), history=4),
            4,
        ),
        ("unknown_cantrip_exception", encoded(spells=((2, 0), (1, 2))), 2),
        ("unknown_supported_cost_all_fit", encoded(spells=((0, 0),)), 3),
        ("missing_rules", encoded(actions=(2, 2), rules=1), 2),
        ("edition_mismatch", encoded(actions=(2, 2), edition=2), 1),
        ("contradictory_bonus_history", encoded(history=2), 0),
    ]
