/*
 * netplay.c - see netplay.h.
 *
 * The game's object table: a byte of flags per slot at 14462(a6) (bit 0 gets
 * physics, bit 2 is looked at for collisions, bit 3 is drawn, bit 5 is
 * temporary), the 286-byte records after them (slot 0 is shorter; slot n >= 1
 * is at $d4 + 286 * (n - 1), the table L44b02). In a record: rotation matrix
 * words at 0, position at 44 (three low longs, three high), the slot in
 * which the parent body is at 68, 69 a frame flag, model at 90, own slot at
 * 93, speed vector at 98, shields at 180/182, hull at 184.
 */
#include <string.h>

#include <SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Socket;
#define INVALID_SOCKET (-1)
#define close_socket   close
#endif

#include "game_state.h"
#include "host.h"
#include "main.h"
#include "netplay.h"

bool netplay_enabled;

#define A6_PLAYER_SLOT 670  /* byte: the player's slot in the objects */
#define A6_BIG_SPACE   14466 /* long: where the object table really is */
#define OBJ_FLAGS_PTR 14462 /* A6: long, the object table */
#define OBJ_FIRST_FREE 20   /* slots below this belong to the game's fixed objects */
#define OBJ_LAST       114
#define OBJ_SIZE       286
#define OBJ_SLOT1      0xd4
#define OBJ_OWN_SLOT   93
#define OBJ_TIMER      97

#define FLAG_DRAWN   0x08
#define FLAG_COLLIDE 0x04
#define GHOST_FLAGS  (FLAG_DRAWN | FLAG_COLLIDE)

#define SEND_INTERVAL_MS 33
#define LOST_AFTER_MS    2000

/* The parts of a record that make up the pose we send (offset, length) */
static const struct
{
	int off, len;
} parts[] = {
	{0, 18},   /* rotation matrix */
	{44, 26},  /* position, parent slot, frame flag */
	{90, 2},   /* model */
	{98, 12},  /* speed */
	{180, 6},  /* shields, hull */
};
#define N_PARTS ((int)(sizeof parts / sizeof parts[0]))

/* Packet: "FEMP", version, active, sequence (2), session, the sender's
 * clock in ms (4), then the pose
 * (PAYLOAD_LEN bytes), then the number of hits (1) and that many (id (2),
 * damage (2)): the newest hits are in every packet until they are old, so
 * a lost packet loses none */
#define HEADER_LEN  13
#define PAYLOAD_LEN (18 + 26 + 2 + 12 + 6)
#define MAX_HITS    8
#define PACKET_MAX  (HEADER_LEN + PAYLOAD_LEN + 1 + 4 * MAX_HITS)

static Socket sock = INVALID_SOCKET;
static struct sockaddr_in peer;
static bool peer_known;

static unsigned int send_seq;
static Uint64 last_send;

/* what the other player last sent */
static unsigned char remote[PAYLOAD_LEN];
static bool remote_active;
static Uint64 remote_time;
static unsigned int remote_seq;
static bool remote_seen;

static int ghost_slot; /* 0: none in the game now */

/* The pose before the newest one, for the speed to carry the ghost on
 * between packets (the record's own speed vector is in units not worked out
 * yet): positions are 64 bit fixed point, high long first in the record */
typedef struct
{
	bool valid;
	long long p[3];
	unsigned char frame[2]; /* parent slot, frame flag: positions only compare in one */
	unsigned int stamp;     /* the sender's clock, ms */
	Uint64 arrived;         /* ours */
} PoseSample;
static PoseSample cur_pose, prev_pose;
#define EXTRAPOLATE_MAX_MS 150

/* hits we landed on the ghost (sent), and the other player's on us (to apply) */
static struct
{
	unsigned short id, damage;
} sent_hits[MAX_HITS];
static unsigned short hit_id; /* the newest id given; 0 = none yet */
static unsigned char session;
static unsigned char remote_session;
static unsigned short applied_id;
static bool applied_known;
static unsigned short pending[16];
static int n_pending;

static bool open_socket(int bind_port)
{
#ifdef _WIN32
	static bool wsa;
	if (!wsa)
	{
		WSADATA wd;
		if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
			return false;
		wsa = true;
	}
#endif
	Socket s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == INVALID_SOCKET)
		return false;
#ifdef _WIN32
	u_long nonblocking = 1;
	bool ok = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
	bool ok = fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) == 0;
#endif
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)bind_port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (!ok || bind(s, (struct sockaddr *)&sa, sizeof sa) < 0)
	{
		log_printf("Netplay: can't bind UDP port %d\n", bind_port);
		close_socket(s);
		return false;
	}
	sock = s;
	session = (unsigned char)(SDL_GetTicks64() ^ (SDL_GetPerformanceCounter() >> 3));
	netplay_enabled = true;
	return true;
}

bool netplay_listen(int port)
{
	if (port <= 0 || port > 65535 || !open_socket(port))
		return false;
	log_printf("Netplay: waiting for the other player on UDP port %d\n", port);
	return true;
}

bool netplay_connect(const char *host_port)
{
	char host[256];
	const char *colon = strrchr(host_port, ':');
	if (!colon || colon == host_port || colon - host_port >= (int)sizeof host)
		return false;
	memcpy(host, host_port, colon - host_port);
	host[colon - host_port] = 0;
	int port = atoi(colon + 1);
	if (port <= 0 || port > 65535)
		return false;

	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (!open_socket(0))
		return false;
	if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
	{
		log_printf("Netplay: can't find %s\n", host);
		return false;
	}
	memcpy(&peer, res->ai_addr, sizeof peer);
	peer.sin_port = htons((unsigned short)port);
	freeaddrinfo(res);
	peer_known = true;
	log_printf("Netplay: sending to %s\n", host_port);
	return true;
}

/* ---- the game's objects ----------------------------------------------- */

/* The object table, or 0 while the game hasn't one it has set up (a new
 * game or a load is rebuilding it: writing by a stale pointer breaks it) */
static u32 objects(void)
{
	u32 table = (u32)rdlong(GAME_A6 + OBJ_FLAGS_PTR);
	if (table == 0 || table != (u32)rdlong(GAME_A6 + A6_BIG_SPACE))
		return 0;
	if (table + OBJ_SLOT1 + OBJ_SIZE * OBJ_LAST + OBJ_SIZE > MEM_SIZE)
		return 0;
	return table;
}

static u32 slot_record(u32 table, int slot)
{
	return table + OBJ_SLOT1 + OBJ_SIZE * (slot - 1);
}

/* Our ship's record when we are flying and it's worth sending */
static u32 own_record(void)
{
	if (game_screen_owner() != OWNER_FLIGHT)
		return 0;
	if (game_flight_status() == FLIGHT_DOCKED)
		return 0;
	u32 table = objects();
	u32 ship = game_player_ship();
	/* a whole record of the table, on a slot's boundary */
	if (!table || ship < table + OBJ_SLOT1 || ship >= table + OBJ_SLOT1 + OBJ_SIZE * OBJ_LAST ||
		(ship - table - OBJ_SLOT1) % OBJ_SIZE != 0)
		return 0;
	return ship;
}

static void send_pose(u32 rec)
{
	if (sock == INVALID_SOCKET || !peer_known)
		return;
	Uint64 now = SDL_GetTicks64();
	if (now - last_send < SEND_INTERVAL_MS)
		return;
	last_send = now;

	unsigned char pkt[PACKET_MAX];
	memcpy(pkt, "FEMP", 4);
	pkt[4] = 2;
	pkt[5] = rec != 0;
	pkt[6] = send_seq >> 8;
	pkt[7] = send_seq;
	pkt[8] = session;
	unsigned int clock = (unsigned int)now;
	pkt[9] = clock >> 24;
	pkt[10] = clock >> 16;
	pkt[11] = clock >> 8;
	pkt[12] = clock;
	send_seq++;
	unsigned char *p = pkt + HEADER_LEN;
	for (int i = 0; i < N_PARTS; i++)
		for (int j = 0; j < parts[i].len; j++)
			*p++ = rec ? (unsigned char)rdbyte(rec + parts[i].off + j) : 0;
	int n = hit_id < MAX_HITS ? hit_id : MAX_HITS;
	*p++ = n;
	for (int i = 0; i < n; i++)
	{
		unsigned short id = hit_id - i;
		*p++ = id >> 8;
		*p++ = id;
		*p++ = sent_hits[id % MAX_HITS].damage >> 8;
		*p++ = sent_hits[id % MAX_HITS].damage;
	}
	sendto(sock, (const char *)pkt, p - pkt, 0, (struct sockaddr *)&peer, sizeof peer);
}

static unsigned int get32(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* A pose arrived (the payload's position part starts at +18) */
static void note_pose(bool active, const unsigned char *payload, unsigned int stamp)
{
	PoseSample s;
	memset(&s, 0, sizeof s);
	s.valid = active;
	for (int i = 0; i < 3; i++)
		s.p[i] = (long long)(((unsigned long long)get32(payload + 18 + 12 + 4 * i) << 32) | get32(payload + 18 + 4 * i));
	s.frame[0] = payload[18 + 24];
	s.frame[1] = payload[18 + 25];
	s.stamp = stamp;
	s.arrived = SDL_GetTicks64();
	prev_pose = cur_pose;
	cur_pose = s;
}

static void receive_poses(void)
{
	if (sock == INVALID_SOCKET)
		return;
	unsigned char pkt[512];
	struct sockaddr_in from;
	for (;;)
	{
#ifdef _WIN32
		int fl = sizeof from;
#else
		socklen_t fl = sizeof from;
#endif
		int n = (int)recvfrom(sock, (char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
		if (n < 0)
			break;
		if (n < HEADER_LEN + PAYLOAD_LEN + 1 || memcmp(pkt, "FEMP", 4) || pkt[4] != 2)
			continue;
		int hits = pkt[HEADER_LEN + PAYLOAD_LEN];
		if (hits > MAX_HITS || n != HEADER_LEN + PAYLOAD_LEN + 1 + 4 * hits)
			continue;
		if (!peer_known)
		{
			peer = from;
			peer_known = true;
			log_printf("Netplay: the other player is at %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
		}
		unsigned int seq = (pkt[6] << 8) | pkt[7];
		/* 16-bit sequence: drop what is older than the newest */
		if (remote_seen && (unsigned short)(seq - remote_seq) > 0x8000)
			continue;
		remote_seen = true;
		remote_seq = seq;
		remote_active = pkt[5] != 0;
		remote_time = SDL_GetTicks64();
		memcpy(remote, pkt + HEADER_LEN, PAYLOAD_LEN);
		note_pose(remote_active, pkt + HEADER_LEN, (pkt[9] << 24) | (pkt[10] << 16) | (pkt[11] << 8) | pkt[12]);

		/* The hits, newest first: the new ones, oldest first, are queued.
		 * What the other player had landed before we saw them is not. */
		const unsigned char *h = pkt + HEADER_LEN + PAYLOAD_LEN + 1;
		if (!applied_known || pkt[8] != remote_session)
		{
			remote_session = pkt[8];
			applied_known = true;
			applied_id = hits ? (h[0] << 8) | h[1] : 0;
			continue;
		}
		for (int i = hits - 1; i >= 0; i--)
		{
			unsigned short id = (h[4 * i] << 8) | h[4 * i + 1];
			unsigned short damage = (h[4 * i + 2] << 8) | h[4 * i + 3];
			if ((unsigned short)(id - applied_id) >= 0x8000 || id == applied_id)
				continue;
			applied_id = id;
			if (n_pending < (int)(sizeof pending / sizeof pending[0]))
				pending[n_pending++] = damage;
		}
	}
}

static void ghost_remove(void)
{
	if (ghost_slot)
	{
		u32 table = objects();
		if (table && (rdbyte(table + ghost_slot) & 0xff) == GHOST_FLAGS)
			wrbyte(table + ghost_slot, 0);
	}
	ghost_slot = 0;
}

/* The ghost's record, or 0 if the game has lost it (docking clears the
 * objects) */
static u32 ghost_record(u32 table)
{
	if (!ghost_slot)
		return 0;
	u32 rec = slot_record(table, ghost_slot);
	if ((rdbyte(table + ghost_slot) & 0xff) != GHOST_FLAGS || (rdbyte(rec + OBJ_OWN_SLOT) & 0xff) != ghost_slot)
	{
		ghost_slot = 0;
		return 0;
	}
	return rec;
}

/* A free slot, the game's way: the highest */
static u32 ghost_create(u32 table, u32 own)
{
	int slot = OBJ_LAST;
	while (slot >= OBJ_FIRST_FREE && rdbyte(table + slot) != 0)
		slot--;
	if (slot < OBJ_FIRST_FREE)
		return 0;
	u32 rec = slot_record(table, slot);
	/* A whole valid ship to start from; the pose is written over it */
	for (int i = 0; i < OBJ_SIZE; i++)
		wrbyte(rec + i, rdbyte(own + i));
	wrbyte(rec + OBJ_OWN_SLOT, slot);
	wrbyte(rec + OBJ_TIMER, 0);
	wrbyte(table + slot, GHOST_FLAGS);
	ghost_slot = slot;
	log_printf("Netplay: ghost in object slot %d\n", slot);
	return rec;
}

static void ghost_update(u32 own)
{
	Uint64 now = SDL_GetTicks64();
	bool show = own && remote_seen && remote_active && now - remote_time < LOST_AFTER_MS;
	if (!show)
	{
		ghost_remove();
		return;
	}
	u32 table = objects();
	if (!table)
		return;
	u32 rec = ghost_record(table);
	if (!rec)
		rec = ghost_create(table, own);
	if (!rec)
		return;
	const unsigned char *p = remote;
	for (int i = 0; i < N_PARTS; i++)
		for (int j = 0; j < parts[i].len; j++)
			wrbyte(rec + parts[i].off + j, *p++);

	/* On from the newest pose at the speed the last two give, for a while */
	const PoseSample *c = &cur_pose, *o = &prev_pose;
	unsigned int span = c->stamp - o->stamp; /* ms between the two, the sender's clock */
	if (!c->valid || !o->valid || c->frame[0] != o->frame[0] || c->frame[1] != o->frame[1] || span == 0 || span > 500)
		return;
	Uint64 elapsed = now - c->arrived;
	if (elapsed > EXTRAPOLATE_MAX_MS)
		elapsed = EXTRAPOLATE_MAX_MS;
	for (int i = 0; i < 3; i++)
	{
		long long moved = (long long)((double)(c->p[i] - o->p[i]) * (double)elapsed / (double)span);
		unsigned long long v = (unsigned long long)(c->p[i] + moved);
		wrlong(rec + 44 + 4 * i, (int)(unsigned int)v);
		wrlong(rec + 56 + 4 * i, (int)(unsigned int)(v >> 32));
	}
}

#if FE2_USE_MODDED
void Call_NetDamage(void)
{
}

void Call_NetPending(void)
{
	SetReg(REG_D0, 0);
}
#else
/* hcall at L44c96 (a1 = the ship hit, d0 = damage, d7 = who did it): a hit
 * by our ship's weapons on the other player's ghost is sent to them. Hits
 * with no one behind them (d7 = 0: crashing into a planet), by someone else
 * (the NPCs here are not there) and collisions are not. */
void Call_NetDamage(void)
{
	if (!netplay_enabled || !ghost_slot)
		return;
	u32 table = objects();
	if (!table || (u32)GetReg(REG_A1) != slot_record(table, ghost_slot))
		return;
	if ((GetReg(REG_D7) & 0xff) != (rdbyte(GAME_A6 + A6_PLAYER_SLOT) & 0xff) || (GetReg(REG_D7) & 0xff) == 0)
		return;
	u32 ret = (u32)rdlong(GetReg(REG_A7));
	if (ret >= FE2_Lnet_collide_begin && ret < FE2_Lnet_collide_end)
		return;
	unsigned short damage = (unsigned short)GetReg(REG_D0);
	if (!damage)
		return;
	hit_id++;
	sent_hits[hit_id % MAX_HITS].id = hit_id;
	sent_hits[hit_id % MAX_HITS].damage = damage;
	log_printf("Netplay: hit on the other player for %d\n", damage);
	last_send = 0; /* now */
}

/* hcall at the start of DoPhysics: the next hit the other player landed on
 * us (d0 = damage, d7 = their ghost's slot, a1 = our ship), d0 = 0 for none */
void Call_NetPending(void)
{
	u32 own = own_record();
	if (!own || !n_pending)
	{
		n_pending = 0;
		SetReg(REG_D0, 0);
		return;
	}
	unsigned short damage = pending[0];
	memmove(pending, pending + 1, --n_pending * sizeof pending[0]);
	SetReg(REG_D0, damage);
	SetReg(REG_D7, ghost_slot);
	SetReg(REG_A1, own);
	log_printf("Netplay: hit by the other player for %d\n", damage);
}
#endif

void netplay_tick(void)
{
	if (!netplay_enabled)
		return;
	u32 own = own_record();
	receive_poses();
	send_pose(own);
	ghost_update(own);
}
