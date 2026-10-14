/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* EncounterCounter: reports wild Pokémon encounters of the Gen 3 games
 * (Ruby, Sapphire, Emerald, FireRed, LeafGreen) to a PokéCounter desk display.
 *
 *  * Watches gMain.inBattle; when a battle starts and it is not a trainer
 *    battle, the opposing Pokémon (gEnemyParty, two in a double battle) are
 *    decoded and sent as one small JSON datagram each (UDP port 4210):
 *    {"t":"enc","sp":249,"lv":45,"g":"N","shiny":false,"pid":...,"eid":...,
 *     "ivs":...,"ab":46,"it":0,"tsv":...,"src":"BPED"}
 *    "src" is the game code, so the display keeps separate statistics per
 *    edition; "it" is the Gen 3 item index; "tsv" is TID ^ SID.
 *  * The display is found via a direct mDNS query for pokecounter.local or
 *    UDP broadcast; the first reply pins its address. Nothing blocks the
 *    emulation (non-blocking socket, no name lookups).
 *  * Every report carries a random event id ("eid"); the sender repeats a
 *    report until the display answers, the display drops repeats by eid.
 *
 * RAM addresses from the pret decompilations and pokebot-gen3 symbol tables
 * (German, French, Italian, Spanish and Japanese versions included). */
#ifndef ENCOUNTER_COUNTER_H
#define ENCOUNTER_COUNTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t (*EcRead8)(void* ctx, uint32_t address);

/* One tracker per emulator instance */
struct EcTracker;
struct EcTracker* ecCreate(void);
void ecDestroy(struct EcTracker* t);

/* Game code from the cartridge header (e.g. "BPED"); unsupported games are ignored. */
void ecSetGame(struct EcTracker* t, const char* code);
/* One emulated frame; reads memory through read(ctx, address). */
void ecFrame(struct EcTracker* t, EcRead8 read, void* ctx);
/* A savestate was loaded (or the core was reset): resynchronise with RAM.
 * Optional: a different battle appearing mid-battle is detected anyway. */
void ecStateLoaded(struct EcTracker* t, EcRead8 read, void* ctx);

/* ---- exposed for tests ---- */
struct EcMon {
	uint32_t pid;
	uint16_t tid, sid;
	uint16_t species;   /* national dex number */
	uint16_t item;      /* Gen 3 item index */
	uint8_t level;
	uint8_t gender;     /* 0 = male, 1 = female, 2 = genderless */
	uint8_t ability;
	bool shiny;
	uint32_t ivs;       /* HP | Atk<<5 | Def<<10 | Spe<<15 | SpA<<20 | SpD<<25 */
};
bool ecDecodeMon(EcRead8 read, void* ctx, uint32_t address, struct EcMon* out);
/* Called for every reported encounter (tests); NULL = none */
extern void (*ecReportHook)(const char* json);

#ifdef __cplusplus
}
#endif

#endif
