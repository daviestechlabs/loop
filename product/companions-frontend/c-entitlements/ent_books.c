/* Shared product book policy. Slugs follow the reviewed D&D catalog. */
#include "ent_books.h"
#include <string.h>

static const ent_book catalog[] = {
    {"astral-adventurers-guide", "Astral Adventurer's Guide", "setting", 0},
    {"candlekeep-mysteries", "Candlekeep Mysteries", "adventure", 0},
    {"dragonlance-shadow-of-the-dragon-queen", "Dragonlance: Shadow of the Dragon Queen",
     "adventure", 0},
    {"players-handbook", "Player's Handbook", "core", 1},
    {"starter-set-rulebook", "Starter Set Rulebook", "core", 1},
    {"monster-manual", "Monster Manual", "core", 1},
    {"core-rulebooks-compendium", "Fifth Edition Core Rulebooks", "core", 1},
    {"art-and-arcana", "Art and Arcana", "reference", 0},
    {"baldurs-gate-descent-into-avernus", "Baldur's Gate: Descent into Avernus", "adventure",
     0},
    {"boos-astral-menagerie", "Boo's Astral Menagerie", "supplement", 0},
    {"eberron-rising-from-the-last-war", "Eberron: Rising from the Last War", "setting", 0},
    {"explorers-guide-to-wildemount", "Explorer's Guide to Wildemount", "setting", 0},
    {"ghosts-of-saltmarsh", "Ghosts of Saltmarsh", "adventure", 0},
    {"guildmasters-guide-to-ravnica", "Guildmasters' Guide to Ravnica", "setting", 0},
    {"hoard-of-the-dragon-queen", "Hoard of the Dragon Queen", "adventure", 0},
    {"icewind-dale-rime-of-the-frostmaiden", "Icewind Dale: Rime of the Frostmaiden",
     "adventure", 0},
    {"lazy-dungeon-master", "The Lazy Dungeon Master", "third-party", 0},
    {"lord-of-the-rings-roleplaying-core-compendium",
     "The Lord of the Rings Roleplaying Core Compendium", "other-system", 0},
    {"lord-of-the-rings-keepers-of-the-elven-rings",
     "The Lord of the Rings Roleplaying: Keepers of the Elven Rings", "other-system", 0},
    {"mordenkainens-tome-of-foes", "Mordenkainen's Tome of Foes", "supplement", 0},
    {"mythic-odysseys-of-theros", "Mythic Odysseys of Theros", "setting", 0},
    {"out-of-the-abyss", "Out of the Abyss", "adventure", 0},
    {"princes-of-the-apocalypse", "Princes of the Apocalypse", "adventure", 0},
    {"basic-rules-box-set", "Dungeons & Dragons Basic Rules Box Set", "other-edition", 0},
    {"light-of-xaryxis", "Light of Xaryxis", "adventure", 0},
    {"storm-kings-thunder", "Storm King's Thunder", "adventure", 0},
    {"strixhaven-curriculum-of-chaos", "Strixhaven: A Curriculum of Chaos", "setting", 0},
    {"sword-coast-adventurers-guide", "Sword Coast Adventurer's Guide", "supplement", 0},
    {"taldorei-campaign-setting", "Tal'Dorei Campaign Setting", "third-party", 0},
    {"tales-from-the-yawning-portal", "Tales from the Yawning Portal", "adventure", 0},
    {"tashas-cauldron-of-everything", "Tasha's Cauldron of Everything", "supplement", 0},
    {"tomb-of-annihilation", "Tomb of Annihilation", "adventure", 0},
    {"van-richtens-guide-to-ravenloft", "Van Richten's Guide to Ravenloft", "setting", 0},
    {"volos-guide-to-monsters", "Volo's Guide to Monsters", "supplement", 0},
    {"waterdeep-dragon-heist", "Waterdeep: Dragon Heist", "adventure", 0},
    {"waterdeep-dungeon-of-the-mad-mage", "Waterdeep: Dungeon of the Mad Mage", "adventure",
     0},
    {"rise-of-tiamat", "The Rise of Tiamat", "adventure", 0},
    {"return-of-the-lazy-dungeon-master", "Return of the Lazy Dungeon Master", "third-party", 0},
    {"kobolds-guide-to-monsters", "Kobold's Guide to Monsters", "third-party", 0},
    {"kobolds-guide-to-worldbuilding", "Kobold's Guide to Worldbuilding", "third-party", 0},
    {"the-monsters-know-what-theyre-doing", "The Monsters Know What They're Doing", "third-party",
     0},
    {"worldbuilding-for-fantasy-fans-and-authors", "Worldbuilding for Fantasy Fans and Authors",
     "third-party", 0},
};

_Static_assert(sizeof(catalog) / sizeof(catalog[0]) <= 64u, "book access mask capacity");

const ent_book *ent_books(size_t *count) {
    if (count) *count = sizeof(catalog) / sizeof(catalog[0]);
    return catalog;
}

uint64_t ent_book_access_mask(int premium) {
    size_t i;
    uint64_t mask = 0;
    if (premium != 0 && premium != 1) return 0;
    for (i = 0; i < sizeof(catalog) / sizeof(catalog[0]); ++i)
        if (premium || catalog[i].core) mask |= UINT64_C(1) << i;
    return mask;
}

int ent_book_mask_valid(uint64_t mask) {
    return mask != 0 && (mask & ~ent_book_access_mask(1)) == 0;
}

int ent_book_allowed(uint64_t mask, const char *slug) {
    size_t i;
    if (!slug || !ent_book_mask_valid(mask)) return 0;
    for (i = 0; i < sizeof(catalog) / sizeof(catalog[0]); ++i)
        if (strcmp(catalog[i].slug, slug) == 0) return (mask & (UINT64_C(1) << i)) != 0;
    return 0;
}
