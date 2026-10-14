/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* EncounterCounter: Gen 3 wild encounter reporting, see encounter-counter.h */
#include "encounter-counter.h"
#include "encounter-counter-species.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET EcSock;
#define EC_NO_SOCK INVALID_SOCKET
#define ecClose closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int EcSock;
#define EC_NO_SOCK (-1)
#define ecClose close
#endif

#ifdef __ANDROID__
#include <android/log.h>
#define EC_LOG(...) __android_log_print(ANDROID_LOG_INFO, "EncounterCounter", __VA_ARGS__)
#else
#define EC_LOG(...) do { fprintf(stderr, "[EncounterCounter] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

#define EC_PORT 4210
#define EC_MON_SIZE 100
#define EC_RETRY_MS 400
#define EC_MAX_TRIES 8
#define EC_FOE_TIMEOUT_FRAMES 600
#define EC_QUEUE 16

#define BATTLE_TYPE_DOUBLE 0x01
#define BATTLE_TYPE_LINK 0x02
#define BATTLE_TYPE_TRAINER 0x08

void (*ecReportHook)(const char* json) = NULL;

/* ---- game table -------------------------------------------------------- */
/* Languages in the order E, D, F, I, S, J; 0 = same as English */
struct EcGame {
	const char* prefix;
	const char* name;
	uint32_t enemyParty[6];
	uint32_t battleTypeFlags[6];
	uint32_t gMain[6];
	uint16_t inBattleOffset; /* byte in gMain, bit 1 = inBattle */
};

static const struct EcGame kGames[] = {
	{ "BPE", "Emerald",
	  { 0x02024744, 0, 0, 0, 0, 0x020243E8 },
	  { 0x02022FEC, 0, 0, 0, 0, 0x02022C90 },
	  { 0x030022C0, 0, 0, 0, 0, 0x03002360 }, 0x439 },
	{ "BPR", "FireRed",
	  { 0x0202402C, 0, 0, 0, 0, 0x02023F8C },
	  { 0x02022B4C, 0, 0, 0, 0, 0x02022AAC },
	  { 0x030030F0, 0x03003040, 0x03003040, 0x03003040, 0x03003040, 0x03003130 }, 0x439 },
	{ "BPG", "LeafGreen",
	  { 0x0202402C, 0, 0, 0, 0, 0x02023F8C },
	  { 0x02022B4C, 0, 0, 0, 0, 0x02022AAC },
	  { 0x030030F0, 0x03003040, 0x03003040, 0x03003040, 0x03003040, 0x03003130 }, 0x439 },
	{ "AXV", "Ruby",
	  { 0x030045C0, 0x030045D0, 0x030045D0, 0x030045D0, 0x030045D0, 0x030044F0 },
	  { 0x020239F8, 0, 0, 0, 0, 0x02023758 },
	  { 0x03001770, 0, 0, 0, 0, 0x030016E0 }, 0x43D },
	{ "AXP", "Sapphire",
	  { 0x030045C0, 0x030045D0, 0x030045D0, 0x030045D0, 0x030045D0, 0x030044F0 },
	  { 0x020239F8, 0, 0, 0, 0, 0x02023758 },
	  { 0x03001770, 0, 0, 0, 0, 0x030016E0 }, 0x43D },
};

/* ---- state ------------------------------------------------------------- */
struct EcPending {
	char json[256];
	int tries;
	uint64_t nextMs;
};

struct EcTracker {
	bool supported;
	const struct EcGame* game;
	char code[5];
	uint32_t enemyParty, battleTypeFlags, inBattleAddr;

	bool inBattle;
	bool handled;
	uint64_t frame;
	uint64_t battleStart;
	uint32_t lastKey; /* PID ^ checksum of the last reported battle */

	EcSock sock;
	bool netReady;
	uint32_t learnedIp;  /* from a reply to a report (network byte order) */
	uint32_t mdnsIp;     /* from the mDNS answer */
	uint64_t mdnsAt;
	struct EcPending queue[EC_QUEUE];
	int qHead, qCount;
	uint32_t rng;
	uint32_t battleKey; /* enemy key when the current battle was handled */
};

static uint64_t nowMs(void) {
#ifdef _WIN32
	return (uint64_t) GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
#endif
}

static uint32_t randomU32(struct EcTracker* t) {
	if (!t->rng) {
		t->rng = (uint32_t) time(NULL) ^ (uint32_t) nowMs() * 2654435761u ^ (uint32_t) (uintptr_t) t;
		if (!t->rng) {
			t->rng = 0x9E3779B9u;
		}
	}
	/* xorshift32 */
	t->rng ^= t->rng << 13;
	t->rng ^= t->rng >> 17;
	t->rng ^= t->rng << 5;
	return t->rng;
}

static uint16_t rd16(EcRead8 read, void* ctx, uint32_t a) {
	return (uint16_t) (read(ctx, a) | (read(ctx, a + 1) << 8));
}

static uint32_t rd32(EcRead8 read, void* ctx, uint32_t a) {
	return (uint32_t) rd16(read, ctx, a) | ((uint32_t) rd16(read, ctx, a + 2) << 16);
}

/* ---- PK3 decoding ------------------------------------------------------ */
/* Position of the Growth, Attacks, EVs and Misc substructures for PID % 24 */
static const uint8_t kOrder[24][4] = {
	{ 0, 1, 2, 3 }, { 0, 1, 3, 2 }, { 0, 2, 1, 3 }, { 0, 3, 1, 2 }, { 0, 2, 3, 1 }, { 0, 3, 2, 1 },
	{ 1, 0, 2, 3 }, { 1, 0, 3, 2 }, { 2, 0, 1, 3 }, { 3, 0, 1, 2 }, { 2, 0, 3, 1 }, { 3, 0, 2, 1 },
	{ 1, 2, 0, 3 }, { 1, 3, 0, 2 }, { 2, 1, 0, 3 }, { 3, 1, 0, 2 }, { 2, 3, 0, 1 }, { 3, 2, 0, 1 },
	{ 1, 2, 3, 0 }, { 1, 3, 2, 0 }, { 2, 1, 3, 0 }, { 3, 1, 2, 0 }, { 2, 3, 1, 0 }, { 3, 2, 1, 0 },
};

bool ecDecodeMon(EcRead8 read, void* ctx, uint32_t a, struct EcMon* out) {
	uint32_t pid = rd32(read, ctx, a);
	uint32_t otId = rd32(read, ctx, a + 4);
	uint16_t checksum = rd16(read, ctx, a + 0x1C);
	if (!pid && !otId) {
		return false;
	}
	uint32_t key = pid ^ otId;
	uint32_t data[12];
	uint16_t sum = 0;
	int i;
	for (i = 0; i < 12; ++i) {
		data[i] = rd32(read, ctx, a + 0x20 + i * 4) ^ key;
		sum = (uint16_t) (sum + (data[i] & 0xFFFF) + (data[i] >> 16));
	}
	if (sum != checksum) {
		return false; /* not written completely yet or garbage */
	}
	const uint8_t* pos = kOrder[pid % 24];
	const uint32_t* growth = &data[pos[0] * 3];
	const uint32_t* misc = &data[pos[3] * 3];
	uint16_t internal = (uint16_t) (growth[0] & 0xFFFF);
	if (internal == 0 || internal >= EC_SPECIES_COUNT || !kEcSpecies[internal].national) {
		return false;
	}
	const struct EcSpecies* s = &kEcSpecies[internal];
	uint32_t ivEggAbility = misc[1];
	if (ivEggAbility & 0x40000000u) {
		return false; /* egg */
	}
	out->pid = pid;
	out->tid = (uint16_t) (otId & 0xFFFF);
	out->sid = (uint16_t) (otId >> 16);
	out->species = s->national;
	out->item = (uint16_t) (growth[0] >> 16);
	out->level = read(ctx, a + 0x54);
	out->ivs = ivEggAbility & 0x3FFFFFFFu;
	out->ability = s->abilities[(ivEggAbility >> 31) && s->abilities[1] ? 1 : 0];
	if (s->genderRatio == 255) {
		out->gender = 2;
	} else if (s->genderRatio == 254) {
		out->gender = 1;
	} else if (s->genderRatio == 0) {
		out->gender = 0;
	} else {
		out->gender = (pid & 0xFF) < s->genderRatio ? 1 : 0;
	}
	out->shiny = (uint16_t) (out->tid ^ out->sid ^ (pid >> 16) ^ (pid & 0xFFFF)) < 8;
	return out->level >= 1 && out->level <= 100;
}

/* ---- network ----------------------------------------------------------- */
static bool netInit(struct EcTracker* t) {
	if (t->netReady) {
		return t->sock != EC_NO_SOCK;
	}
	t->netReady = true;
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return false;
	}
#endif
	t->sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (t->sock == EC_NO_SOCK) {
		EC_LOG("socket() failed");
		return false;
	}
	int one = 1;
	setsockopt(t->sock, SOL_SOCKET, SO_BROADCAST, (const char*) &one, sizeof(one));
#ifdef _WIN32
	u_long nb = 1;
	ioctlsocket(t->sock, FIONBIO, &nb);
#else
	fcntl(t->sock, F_SETFL, fcntl(t->sock, F_GETFL, 0) | O_NONBLOCK);
#endif
	return true;
}

/* mDNS query for pokecounter.local from our own (non-5353) port: the
 * responder answers by unicast to us (RFC 6762, legacy unicast). */
static void sendMdnsQuery(struct EcTracker* t) {
	static const uint8_t q[] = {
		0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
		11, 'p', 'o', 'k', 'e', 'c', 'o', 'u', 'n', 't', 'e', 'r',
		5, 'l', 'o', 'c', 'a', 'l', 0,
		0, 1, 0x80, 1
	};
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(5353);
	to.sin_addr.s_addr = htonl(0xE00000FBu); /* 224.0.0.251 */
	sendto(t->sock, (const char*) q, sizeof(q), 0, (struct sockaddr*) &to, sizeof(to));
	t->mdnsAt = nowMs();
}

static void sendDatagram(struct EcTracker* t, const char* json) {
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(EC_PORT);
	if (t->learnedIp) {
		to.sin_addr.s_addr = t->learnedIp;
	} else if (t->mdnsIp) {
		to.sin_addr.s_addr = t->mdnsIp;
	} else {
		to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	}
	sendto(t->sock, json, (int) strlen(json), 0, (struct sockaddr*) &to, sizeof(to));
}

static void pumpNetwork(struct EcTracker* t) {
	if (t->sock == EC_NO_SOCK) {
		return;
	}
	char buf[600];
	struct sockaddr_in from;
	socklen_t fromLen = sizeof(from);
	int n;
	while ((n = (int) recvfrom(t->sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*) &from, &fromLen)) > 0) {
		buf[n] = 0;
		if (ntohs(from.sin_port) == 5353) {
			/* mDNS answer: QR bit set and at least one answer record */
			if (n > 12 && (buf[2] & 0x80) && (buf[6] | buf[7]) && !t->mdnsIp) {
				t->mdnsIp = from.sin_addr.s_addr;
				EC_LOG("pokecounter.local -> %s", inet_ntoa(from.sin_addr));
			}
		} else if (ntohs(from.sin_port) == EC_PORT) {
			if (!t->learnedIp) {
				EC_LOG("display found at %s", inet_ntoa(from.sin_addr));
			}
			t->learnedIp = from.sin_addr.s_addr;
			if (t->qCount && strstr(buf, "\"ok\"")) {
				t->qHead = (t->qHead + 1) % EC_QUEUE;
				t->qCount--;
			}
		}
		fromLen = sizeof(from);
	}

	uint64_t now = nowMs();
	if (!t->learnedIp && !t->mdnsIp && now - t->mdnsAt > 5000) {
		sendMdnsQuery(t);
	}
	if (!t->qCount) {
		return;
	}
	struct EcPending* p = &t->queue[t->qHead];
	if (now < p->nextMs) {
		return;
	}
	if (p->tries >= EC_MAX_TRIES) {
		EC_LOG("no reply from display, dropping: %s", p->json);
		t->learnedIp = 0; /* fall back to mDNS / broadcast */
		t->qHead = (t->qHead + 1) % EC_QUEUE;
		t->qCount--;
		return;
	}
	sendDatagram(t, p->json);
	p->tries++;
	p->nextMs = now + EC_RETRY_MS;
}

static void report(struct EcTracker* t, const struct EcMon* m) {
	char json[256];
	char gender = m->gender == 0 ? 'M' : (m->gender == 1 ? 'F' : 'N');
	snprintf(json, sizeof(json),
	         "{\"t\":\"enc\",\"sp\":%u,\"lv\":%u,\"g\":\"%c\",\"shiny\":%s,\"pid\":%lu,\"eid\":%lu,"
	         "\"ivs\":%lu,\"ab\":%u,\"it\":%u,\"tsv\":%u,\"src\":\"%s\"}",
	         m->species, m->level, gender, m->shiny ? "true" : "false", (unsigned long) m->pid,
	         (unsigned long) randomU32(t), (unsigned long) m->ivs, m->ability, m->item,
	         (unsigned) (m->tid ^ m->sid), t->code);
	EC_LOG("wild encounter: %s", json);
	if (ecReportHook) {
		ecReportHook(json);
	}
	if (!netInit(t)) {
		return;
	}
	if (t->qCount == EC_QUEUE) { /* drop the oldest */
		t->qHead = (t->qHead + 1) % EC_QUEUE;
		t->qCount--;
	}
	struct EcPending* p = &t->queue[(t->qHead + t->qCount) % EC_QUEUE];
	snprintf(p->json, sizeof(p->json), "%s", json);
	p->tries = 0;
	p->nextMs = 0;
	t->qCount++;
}

/* ---- game detection / battle tracking ---------------------------------- */
void ecSetGame(struct EcTracker* t, const char* code) {
	memset(t->code, 0, sizeof(t->code));
	if (code) {
		memcpy(t->code, code, 4);
	}
	t->supported = false;
	t->game = NULL;
	t->inBattle = false;
	t->handled = false;
	t->lastKey = 0;
	static const char kLangs[] = "EDFISJ";
	const char* lp = code && code[3] ? strchr(kLangs, code[3]) : NULL;
	if (!lp) {
		return;
	}
	int lang = (int) (lp - kLangs);
	size_t i;
	for (i = 0; i < sizeof(kGames) / sizeof(kGames[0]); ++i) {
		const struct EcGame* G = &kGames[i];
		if (strncmp(code, G->prefix, 3) != 0) {
			continue;
		}
		t->game = G;
		t->enemyParty = G->enemyParty[lang] ? G->enemyParty[lang] : G->enemyParty[0];
		t->battleTypeFlags = G->battleTypeFlags[lang] ? G->battleTypeFlags[lang] : G->battleTypeFlags[0];
		t->inBattleAddr = (G->gMain[lang] ? G->gMain[lang] : G->gMain[0]) + G->inBattleOffset;
		t->supported = true;
		EC_LOG("%s detected (%s), encounter reporting active", G->name, t->code);
		if (netInit(t)) {
			sendMdnsQuery(t);
		}
		return;
	}
}

static bool inBattle(struct EcTracker* t, EcRead8 read, void* ctx) {
	return (read(ctx, t->inBattleAddr) & 0x02) != 0;
}

static uint32_t enemyKey(struct EcTracker* t, EcRead8 read, void* ctx) {
	return rd32(read, ctx, t->enemyParty) ^ ((uint32_t) rd16(read, ctx, t->enemyParty + 0x1C) << 16);
}

void ecFrame(struct EcTracker* t, EcRead8 read, void* ctx) {
	t->frame++;
	pumpNetwork(t);
	if (!t->supported || (t->frame & 1)) {
		return;
	}
	if (!inBattle(t, read, ctx)) {
		t->inBattle = false;
		t->handled = false;
		return;
	}
	if (!t->inBattle) {
		t->inBattle = true;
		t->handled = false;
		t->battleStart = t->frame;
	}
	if (t->handled) {
		/* Opponent changed in the middle of a battle: a savestate of another
		 * battle was loaded (standalone mGBA has no load notification) */
		uint32_t k = enemyKey(t, read, ctx);
		struct EcMon probe;
		if (k == t->battleKey || !ecDecodeMon(read, ctx, t->enemyParty, &probe)) {
			return;
		}
		t->handled = false;
		t->battleStart = t->frame;
	}
	if (t->frame - t->battleStart > EC_FOE_TIMEOUT_FRAMES) {
		t->handled = true; /* no valid opponent data, ignore this battle */
		return;
	}
	struct EcMon foes[2];
	if (!ecDecodeMon(read, ctx, t->enemyParty, &foes[0])) {
		return; /* not written yet, retry next time */
	}
	/* Give the game a moment: gBattleTypeFlags is set while the battle is set up */
	if (t->frame - t->battleStart < 20) {
		return;
	}
	t->handled = true;
	t->battleKey = enemyKey(t, read, ctx);
	uint32_t flags = rd32(read, ctx, t->battleTypeFlags);
	if (flags & (BATTLE_TYPE_TRAINER | BATTLE_TYPE_LINK)) {
		return;
	}
	t->lastKey = t->battleKey;
	report(t, &foes[0]);
	if ((flags & BATTLE_TYPE_DOUBLE) && ecDecodeMon(read, ctx, t->enemyParty + EC_MON_SIZE, &foes[1])) {
		report(t, &foes[1]);
	}
}

void ecStateLoaded(struct EcTracker* t, EcRead8 read, void* ctx) {
	/* Resynchronise with the loaded RAM. A state saved before a battle starts
	 * outside of it, so the battle is reported again when it starts. A state
	 * saved during a battle that was already reported (also: RetroArch run-ahead
	 * and rewind, which load states every frame) is not reported again. */
	if (!t->supported) {
		return;
	}
	t->inBattle = inBattle(t, read, ctx);
	t->handled = t->inBattle && enemyKey(t, read, ctx) == t->lastKey;
	t->battleKey = t->handled ? t->lastKey : 0;
	t->battleStart = t->frame;
}

static void ecShutdown(struct EcTracker* t) {
	if (t->sock != EC_NO_SOCK) {
		ecClose(t->sock);
		t->sock = EC_NO_SOCK;
	}
	t->netReady = false;
	t->qCount = 0;
	t->learnedIp = t->mdnsIp = 0;
}

struct EcTracker* ecCreate(void) {
	struct EcTracker* t = calloc(1, sizeof(*t));
	if (t) {
		t->sock = EC_NO_SOCK;
	}
	return t;
}

void ecDestroy(struct EcTracker* t) {
	if (!t) {
		return;
	}
	ecShutdown(t);
	free(t);
}
