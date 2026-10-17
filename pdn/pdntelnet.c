/*
Copyright 2026 the linmail-pdn contributors

This file is part of LinBPQ/BPQ32.

LinBPQ/BPQ32 is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

LinBPQ/BPQ32 is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with LinBPQ/BPQ32.  If not, see http://www.gnu.org/licenses
*/

//	linmail-pdn telnet logins
//
//	A small TCP login listener that behaves like LinBPQ's Telnet driver
//	(TelnetV6.c), so the programs and people that log in to a LinBPQ BBS over
//	its telnet ports reach linmail-pdn with only a host and port change. It is
//	off unless a port is set in linmail-pdn.conf.
//
//	telnet_port is LinBPQ's TCPPORT: telnet negotiation, "user:" and
//	"password:" prompts, echo if the client asks for it, and CR turned into
//	CR LF on the way out. fbb_port is LinBPQ's FBBPORT, for programs: no
//	prompts, no negotiation, no echo, and once in the BBS everything passes
//	through untouched (FBB forwarding is binary).
//
//	Logins come from "user = name,password,callsign,appl,SYSOP" lines, the
//	shape of LinBPQ's USER= lines, so they can be pasted from bpq32.cfg. The
//	name and password are case-sensitive, as in LinBPQ.
//
//	After a login LinBPQ leaves the caller at its node prompt, where BBS
//	connects to the mailbox. There is no node here, so a logged in session
//	waits for a BBS line (or MAIL, pdn's name for this app) and then opens a
//	BBS session for the user's callsign through the same stream code as an
//	inbound pdn connect (PdnLocalConnect in pdnhost.c). A USER line whose
//	appl field is BBS goes straight in, as it does in LinBPQ. A login whose
//	USER line has SYSOP is a secure session, so a BBS user flagged sysop gets
//	sysop status, as in LinBPQ.
//
//	Passwords are never echoed, logged or stored anywhere but in memory.
//	Everything runs on the main thread, inside PdnHostPoll, under its lock.
//
//	It is built not to let one client hurt the rest: each session is read a
//	little at a time, and not at all while the BBS has not yet taken what it
//	sent (so TCP pushes back on a client that sends faster than the BBS
//	reads); a session not yet logged in may send only a little, has 30
//	seconds to log in, and may not hold more than a few slots from one
//	address, and the oldest of them gives way when the slots run out; a
//	failed login is answered only after a delay that grows with the failures
//	from that address; and the prompts don't say whether a name exists.

#include "bpqmail.h"
#include "pdnhost.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define TN_MAXSESS 64				// Most sessions ever, whatever telnet_max says
#define TN_LINE 256					// Longest line before it is passed on in pieces
#define TN_MAXOUT (1024 * 1024)		// Output waiting for a client that is not reading
#define TN_MAXFAILS 5				// Failed logins before the session is dropped, as LinBPQ
#define TN_LOGINTIME 30				// Seconds to log in
#define TN_READ 4096				// One read
#define TN_READS 4					// Reads per session each time round, at most
#define TN_BACKLOG (64 * 1024)		// Unread by the BBS before a session is read no more
#define TN_PRELOGIN 8192			// Most a client may send before it has logged in
#define TN_PERADDR 3				// Sessions not yet logged in from one address
#define TN_FIELD 256				// Longest USER line field, as LinBPQ, with its 0
#define TN_FAILADDRS 32				// Addresses whose failed logins are remembered
#define TN_NOISE 20					// Connect and login failure lines a minute, at most
#define TN_ACCEPTS 16				// Connects taken from each port each time round, at most
#define TN_SPOKE 2000				// ms after its user line that a login is not dropped for a newcomer
#define TN_PROMPTIDLE 900			// Seconds at the prompt, as LinBPQ's IDLETIME
#define TN_STREAMWAIT 5				// Seconds to wait for a free BBS stream
#define TN_CLOSEWAIT 30				// Seconds to send what is left after the BBS hangs up

// Session states

#define TN_FREE 0
#define TN_USER 1					// Waiting for the user name
#define TN_PASS 2					// Waiting for the password
#define TN_PROMPT 3					// Logged in, at the (stand-in) node prompt
#define TN_WAITSTREAM 4				// Asked for the BBS, waiting for a free stream
#define TN_BBS 5					// In the BBS
#define TN_CLOSING 6				// Sending what is left, then closing

// Telnet

#define IAC 255
#define DONT 254
#define DO 253
#define WONT 252
#define WILL 251
#define SB 250
#define SE 240
#define OPT_ECHO 1
#define OPT_SGA 3

struct TelnetUser
{
	char Name[TN_FIELD];
	char Pass[TN_FIELD];
	char Call[10];
	int Sysop;
	int StraightIn;					// appl field BBS: no need to type BBS
};

struct TelnetSess
{
	int Sock;
	int FBB;						// Logged in on the FBB port
	int State;
	int User;						// Index into Users once logged in, else -1
	int Fails;
	uint64_t NotBefore;				// After a failed login: nothing is read or answered until then (ms)
	int Pending;					// What to do then: 1 ask for the user again, 2 hang up
	int PreLogin;					// Bytes received before logging in
	char Name[TN_LINE + 1];			// The name typed, until the password comes
	int Echo;						// The client asked us to echo
	int EchoOffered;				// We said WILL ECHO and have not taken it back
	int SGAOffered;
	int Tel;						// Telnet command parser state
	int Verb;
	int PrevCR;						// Last input byte was a CR, so a LF or NUL after it goes
	int Stream;						// pdnhost stream (0 based) when in the BBS, else -1
	time_t Since;					// When the current state began
	uint64_t ConnectNo;				// Connects counted in order, so the oldest is clear even in one ms
	uint64_t PassSinceMs;			// When its user line came (monotonic ms): it is waiting for the password
	time_t LastInput;
	char Peer[64];
	UCHAR Line[TN_LINE + 1];
	int LineLen;
	UCHAR * Out;
	int OutLen;
	int OutSize;
	UCHAR * Hold;					// Data for the BBS while waiting for a stream
	int HoldLen;
	UCHAR * Defer;					// Input that came after a failed login, for after the delay
	int DeferLen;
};

// Failed logins by address, for the delay after each

struct FailAddr
{
	char Addr[64];
	int Count;
	time_t Last;
};

static struct FailAddr FailAddrs[TN_FAILADDRS];

static time_t NoiseMinute = 0;
static int NoiseCount = 0;
static int NoiseDropped = 0;
static uint64_t Connects = 0;

static struct TelnetUser * Users = NULL;
static int UserCount = 0;

static struct TelnetSess Sess[TN_MAXSESS];

static int TelnetPort = 0;
static int FBBPort = 0;
static char BindAddr[64] = "127.0.0.1";
static int MaxSessions = 10;		// LinBPQ's MAXSESSIONS default

static int TelnetListen = -1;
static int FBBListen = -1;

static char AttemptsMsg[] = "Too many attempts - Disconnected\r\n";

static void TelnetLog(const char * format, ...)
{
	char Mess[512];
	va_list(arglist);

	va_start(arglist, format);
	vsnprintf(Mess, sizeof(Mess), format, arglist);
	va_end(arglist);

	printf("linmail-pdn: telnet: %s\n", Mess);
	Debugprintf("linmail-pdn: telnet: %s", Mess);
}

static void TelnetNoise(const char * format, ...)
{
	// Connects, refusals and failed logins: anyone can cause these, so at
	// most TN_NOISE lines a minute, and a count of the rest

	char Mess[512];
	time_t Now = time(NULL);
	va_list(arglist);

	if (Now - NoiseMinute >= 60)
	{
		if (NoiseDropped)
			TelnetLog("%d more connect and login lines not shown in the last minute", NoiseDropped);

		NoiseMinute = Now;
		NoiseCount = NoiseDropped = 0;
	}

	if (NoiseCount >= TN_NOISE)
	{
		NoiseDropped++;
		return;
	}

	NoiseCount++;

	va_start(arglist, format);
	vsnprintf(Mess, sizeof(Mess), format, arglist);
	va_end(arglist);

	TelnetLog("%s", Mess);
}

static uint64_t NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

//	Settings

static void AddUser(char * Value)
{
	// LinBPQ's USER=user,password,call,appl,SYSOP. As there, each field ends
	// at its first space, and spaces before a field are skipped.

	char Field[5][TN_FIELD + 1];
	char * ptr = Value, * Comma;
	struct TelnetUser * USER;
	int n = 0;

	memset(Field, 0, sizeof(Field));

	while (ptr && *ptr && n < 5)
	{
		while (*ptr == ' ' || *ptr == '\t')
			ptr++;

		Comma = strchr(ptr, ',');

		if (Comma)
			*(Comma++) = 0;

		strncpy(Field[n], ptr, TN_FIELD);
		strlop(Field[n], ' ');
		strlop(Field[n], '\t');

		if (strlen(Field[n]) >= TN_FIELD)
		{
			// Never shown: it may be the password

			TelnetLog("a user line has a field longer than %d characters - user ignored", TN_FIELD - 1);
			return;
		}
		n++;
		ptr = Comma;
	}

	if (Field[0][0] == 0 || Field[1][0] == 0 || Field[2][0] == 0)
	{
		TelnetLog("a user line needs a name, a password and a callsign - ignored");
		return;
	}

	_strupr(Field[2]);

	if (_stricmp(Field[0], "ANON") == 0)
	{
		TelnetLog("ANON logins are not supported - user line ignored");
		return;
	}

	if (!PdnIsCallsign(Field[2]))
	{
		TelnetLog("user %s: the callsign field is not a callsign the BBS can use - user ignored", Field[0]);
		return;
	}

	Users = realloc(Users, (UserCount + 1) * sizeof(struct TelnetUser));
	USER = &Users[UserCount++];
	memset(USER, 0, sizeof(struct TelnetUser));

	strcpy(USER->Name, Field[0]);
	strcpy(USER->Pass, Field[1]);
	strcpy(USER->Call, Field[2]);
	USER->Sysop = _stricmp(Field[4], "SYSOP") == 0;

	if (_stricmp(Field[3], "BBS") == 0 || _stricmp(Field[3], "MAIL") == 0)
		USER->StraightIn = 1;
	else if (Field[3][0] && strcmp(Field[3], "\"\"") != 0)
		TelnetLog("user %s: the application field is not BBS, so it is ignored - the session starts at the prompt, where BBS enters the mailbox",
			Field[0]);
}

int PdnTelnetOption(const char * Key, const char * Value)
{
	if (_stricmp(Key, "telnet_port") == 0)
		TelnetPort = atoi(Value);
	else if (_stricmp(Key, "fbb_port") == 0)
		FBBPort = atoi(Value);
	else if (_stricmp(Key, "telnet_bind") == 0)
	{
		strncpy(BindAddr, Value, sizeof(BindAddr) - 1);
		BindAddr[sizeof(BindAddr) - 1] = 0;
	}
	else if (_stricmp(Key, "telnet_max") == 0)
	{
		MaxSessions = atoi(Value);

		if (MaxSessions < 1)
			MaxSessions = 1;

		if (MaxSessions > TN_MAXSESS)
			MaxSessions = TN_MAXSESS;
	}
	else if (_stricmp(Key, "user") == 0)
	{
		char * Copy = _strdup(Value);
		AddUser(Copy);
		free(Copy);
	}
	else
		return FALSE;

	return TRUE;
}

//	Output

static void FlushOut(struct TelnetSess * S)
{
	int Sent;

	while (S->OutLen > 0)
	{
		Sent = send(S->Sock, S->Out, S->OutLen, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (Sent < 0)
		{
			if (errno == EINTR)
				continue;

			if (errno != EAGAIN && errno != EWOULDBLOCK)
				S->OutLen = -1;		// Broken: the poll closes it

			return;
		}

		S->OutLen -= Sent;
		memmove(S->Out, &S->Out[Sent], S->OutLen);
	}
}

static void Queue(struct TelnetSess * S, const UCHAR * Data, int Len)
{
	if (S->OutLen < 0 || Len <= 0)
		return;

	if (S->OutLen + Len > S->OutSize)
	{
		if (S->OutLen + Len > TN_MAXOUT)
		{
			S->OutLen = -1;			// The client has stopped reading: drop it
			return;
		}

		S->OutSize = S->OutLen + Len + 4096;
		S->Out = realloc(S->Out, S->OutSize);
	}

	memcpy(&S->Out[S->OutLen], Data, Len);
	S->OutLen += Len;
}

static void Say(struct TelnetSess * S, const char * Text)
{
	// Text of our own, written with CR LF line ends. The FBB port gets CR
	// only, as the BBS itself sends there.

	if (S->FBB)
	{
		const char * ptr;

		for (ptr = Text; *ptr; ptr++)
			if (*ptr != 10)
				Queue(S, (const UCHAR *)ptr, 1);
	}
	else
		Queue(S, (const UCHAR *)Text, (int)strlen(Text));

	FlushOut(S);
}

static void TelnetCmd(struct TelnetSess * S, int Verb, int Option)
{
	UCHAR Cmd[3] = {IAC, Verb, Option};

	Queue(S, Cmd, 3);
}

int PdnTelnetSend(int Id, UCHAR * Data, int Len)
{
	// From the BBS. The telnet port gets CR as CR LF, as LinBPQ does; the
	// FBB port gets it exactly as sent.

	struct TelnetSess * S;
	int i, Start = 0;

	if (Id < 0 || Id >= TN_MAXSESS)
		return 0;

	S = &Sess[Id];

	if (S->State != TN_BBS)
		return 0;

	if (S->FBB)
		Queue(S, Data, Len);
	else
	{
		for (i = 0; i < Len; i++)
		{
			if (Data[i] == 13)
			{
				Queue(S, &Data[Start], i - Start + 1);
				Queue(S, (const UCHAR *)"\n", 1);
				Start = i + 1;
			}
		}
		Queue(S, &Data[Start], Len - Start);
	}

	FlushOut(S);
	return 0;
}

int PdnTelnetQueued(int Id)
{
	// Output still to go, in BPQ sized buffers, for the mail code's flow
	// control (TXCount)

	if (Id < 0 || Id >= TN_MAXSESS || Sess[Id].OutLen <= 0)
		return 0;

	return (Sess[Id].OutLen + 255) / 256;
}

//	Sessions

static void CloseSess(struct TelnetSess * S)
{
	if (S->Stream >= 0)
		PdnLocalClosed(S->Stream);		// The BBS hears the caller has gone

	close(S->Sock);
	free(S->Out);
	free(S->Hold);
	free(S->Defer);
	memset(S, 0, sizeof(struct TelnetSess));
	S->Sock = -1;
	S->Stream = -1;
	S->User = -1;
}

static void StartClosing(struct TelnetSess * S)
{
	if (S->Stream >= 0)
	{
		PdnLocalClosed(S->Stream);
		S->Stream = -1;
	}

	S->State = TN_CLOSING;
	S->Since = time(NULL);
}

void PdnTelnetHangup(int Id)
{
	// The BBS has disconnected. Send what is left, then close, as LinBPQ does.

	struct TelnetSess * S;

	if (Id < 0 || Id >= TN_MAXSESS)
		return;

	S = &Sess[Id];

	if (S->State != TN_BBS)
		return;

	S->Stream = -1;
	S->State = TN_CLOSING;
	S->Since = time(NULL);
	FlushOut(S);
}

static void ToBBS(struct TelnetSess * S, UCHAR * Data, int Len)
{
	if (Len <= 0)
		return;

	if (S->State == TN_BBS)
		PdnLocalData(S->Stream, Data, Len);
	else if (S->State == TN_WAITSTREAM)
	{
		S->Hold = realloc(S->Hold, S->HoldLen + Len);
		memcpy(&S->Hold[S->HoldLen], Data, Len);
		S->HoldLen += Len;
	}
}

static void TryBBS(struct TelnetSess * S)
{
	struct TelnetUser * USER = &Users[S->User];
	int n = PdnLocalConnect(USER->Call, USER->Sysop, (int)(S - Sess), S->Peer);

	if (n < 0)
		return;						// None free yet

	S->Stream = n;
	S->State = TN_BBS;

	if (S->HoldLen)
	{
		PdnLocalData(n, S->Hold, S->HoldLen);
		free(S->Hold);
		S->Hold = NULL;
		S->HoldLen = 0;
	}
}

static void EnterBBS(struct TelnetSess * S)
{
	S->State = TN_WAITSTREAM;
	S->Since = time(NULL);
	TryBBS(S);
}

static int FailDelay(char * Addr)
{
	// How long to wait before answering a failed login, in ms: a second for
	// each of the first five failures from an address, then doubling, up to
	// half a minute. An address is forgiven after ten quiet minutes.

	struct FailAddr * F = NULL, * Oldest = &FailAddrs[0];
	time_t Now = time(NULL);
	int i, Delay;

	for (i = 0; i < TN_FAILADDRS; i++)
	{
		if (strcmp(FailAddrs[i].Addr, Addr) == 0)
			F = &FailAddrs[i];

		if (FailAddrs[i].Last < Oldest->Last)
			Oldest = &FailAddrs[i];
	}

	if (F == NULL)
	{
		F = Oldest;
		memset(F, 0, sizeof(struct FailAddr));
		strcpy(F->Addr, Addr);
	}

	if (Now - F->Last > 600)
		F->Count = 0;

	F->Count++;
	F->Last = Now;

	if (F->Count <= 5)
		return 1000;

	Delay = F->Count - 5 > 5 ? 30000 : 1000 << (F->Count - 5);
	return Delay > 30000 ? 30000 : Delay;
}

static void LoginFailed(struct TelnetSess * S, char * Why)
{
	// Answered after a delay, and always by asking for the user again, so the
	// answer says nothing about which part was wrong

	S->Fails++;
	S->User = -1;
	S->State = TN_USER;
	memset(S->Name, 0, sizeof(S->Name));

	TelnetNoise("login failed from %s: %s", S->Peer, Why);

	S->NotBefore = NowMs() + FailDelay(S->Peer);
	S->Pending = S->Fails >= TN_MAXFAILS ? 2 : 1;
}

static void PromptLine(struct TelnetSess * S, char * Line)
{
	// The stand-in for LinBPQ's node prompt

	char * ptr = Line;
	char * End;

	while (*ptr == ' ')
		ptr++;

	End = ptr + strlen(ptr);

	while (End > ptr && End[-1] == ' ')
		*(--End) = 0;

	if (*ptr == 0)
		return;

	if (_stricmp(ptr, "BBS") == 0 || _stricmp(ptr, "MAIL") == 0)
		EnterBBS(S);
	else if (_stricmp(ptr, "B") == 0 || _stricmp(ptr, "BYE") == 0 ||
		_stricmp(ptr, "Q") == 0 || _stricmp(ptr, "QUIT") == 0)
		StartClosing(S);
	else
		Say(S, "There is no node here: type BBS for the mailbox, or B to leave\r\n");
}

static void LoginLine(struct TelnetSess * S, char * Line)
{
	int i, Found = -1;

	if (S->State == TN_USER)
	{
		if (Line[0] == 0)
		{
			if (!S->FBB)
				Say(S, "user:");
			return;
		}

		// Any name gets the password prompt; name and password are checked
		// together, so the prompts don't say whether a name exists

		strcpy(S->Name, Line);
		S->State = TN_PASS;
		S->PassSinceMs = NowMs();

		if (!S->FBB)
			Say(S, "password:");
		return;
	}

	// The password. LinBPQ answers it with a new line at once, right or
	// wrong, as the password was not echoed.

	if (!S->FBB)
		Say(S, "\r\n");

	for (i = 0; i < UserCount; i++)
	{
		if (strcmp(S->Name, Users[i].Name) == 0)
		{
			Found = i;
			break;
		}
	}

	if (Found < 0)
	{
		// The name typed is not shown: people type passwords there by mistake

		LoginFailed(S, "unknown user");
		return;
	}

	if (strcmp(Line, Users[Found].Pass) != 0)
	{
		char Why[300];

		snprintf(Why, sizeof(Why), "wrong password for %s", Users[Found].Name);
		LoginFailed(S, Why);
		return;
	}

	S->User = Found;
	S->Fails = 0;
	S->State = TN_PROMPT;
	S->Since = time(NULL);
	memset(S->Name, 0, sizeof(S->Name));

	TelnetLog("%s logged in from %s as %s%s", Users[S->User].Name, S->Peer, Users[S->User].Call,
		Users[S->User].Sysop ? " (sysop)" : "");

	if (!S->FBB)
	{
		char Text[200];

		snprintf(Text, sizeof(Text), "Connected to %s's mailbox. Type BBS to go in, or B to leave.\r\n\r\n", BBSName);
		Say(S, Text);
	}

	if (Users[S->User].StraightIn)
		EnterBBS(S);
}

static void EndLine(struct TelnetSess * S)
{
	S->Line[S->LineLen] = 0;

	switch (S->State)
	{
	case TN_USER:
	case TN_PASS:
		LoginLine(S, (char *)S->Line);
		break;

	case TN_PROMPT:
		PromptLine(S, (char *)S->Line);
		break;

	case TN_BBS:
	case TN_WAITSTREAM:
		S->Line[S->LineLen++] = 13;
		ToBBS(S, S->Line, S->LineLen);
		break;
	}

	// Scrub what was typed (it may have been a password)

	memset(S->Line, 0, sizeof(S->Line));
	S->LineLen = 0;
}

static void TelnetOption(struct TelnetSess * S, int Verb, int Option)
{
	// We offer WILL ECHO and WILL SUPPRESS-GO-AHEAD at the start, as LinBPQ
	// does. Echo only if the client agrees. Refuse everything else, and
	// never answer a request that changes nothing, so nothing loops.

	switch (Verb)
	{
	case DO:
		if (Option == OPT_ECHO)
		{
			if (!S->EchoOffered)
				TelnetCmd(S, WILL, OPT_ECHO);
			S->EchoOffered = 1;
			S->Echo = 1;
		}
		else if (Option == OPT_SGA)
		{
			if (!S->SGAOffered)
				TelnetCmd(S, WILL, OPT_SGA);
			S->SGAOffered = 1;
		}
		else
			TelnetCmd(S, WONT, Option);
		break;

	case DONT:
		if (Option == OPT_ECHO)
		{
			if (S->EchoOffered)
				TelnetCmd(S, WONT, OPT_ECHO);
			S->EchoOffered = 0;
			S->Echo = 0;
		}
		else if (Option == OPT_SGA && S->SGAOffered)
		{
			TelnetCmd(S, WONT, OPT_SGA);
			S->SGAOffered = 0;
		}
		break;

	case WILL:
		TelnetCmd(S, DONT, Option);
		break;
	}
}

static void Input(struct TelnetSess * S, UCHAR * Data, int Len)
{
	int i;

	S->LastInput = time(NULL);

	for (i = 0; i < Len; i++)
	{
		UCHAR c = Data[i];

		if (S->State == TN_CLOSING || S->State == TN_FREE)
			return;

		if (S->NotBefore)
		{
			// A login just failed: the rest waits for the delay

			S->Defer = realloc(S->Defer, S->DeferLen + Len - i);
			memcpy(&S->Defer[S->DeferLen], &Data[i], Len - i);
			S->DeferLen += Len - i;
			return;
		}

		if (S->FBB && (S->State == TN_BBS || S->State == TN_WAITSTREAM))
		{
			// Binary from here on (held until a stream is free). Drop a LF
			// straight after the line that asked for the BBS.

			if (S->PrevCR && c == 10)
				i++;

			S->PrevCR = 0;
			ToBBS(S, &Data[i], Len - i);
			return;
		}

		if (!S->FBB)
		{
			// Telnet commands

			switch (S->Tel)
			{
			case 0:
				if (c == IAC)
				{
					S->Tel = 1;
					continue;
				}
				break;

			case 1:
				S->Tel = 0;

				if (c == IAC)
					break;			// A literal 255

				if (c >= WILL && c <= DONT)
				{
					S->Verb = c;
					S->Tel = 2;
				}
				else if (c == SB)
					S->Tel = 3;
				continue;

			case 2:
				TelnetOption(S, S->Verb, c);
				S->Tel = 0;
				continue;

			case 3:
				if (c == IAC)
					S->Tel = 4;
				continue;

			case 4:
				S->Tel = (c == SE) ? 0 : 3;
				continue;
			}
		}

		// Line ends: CR, CR LF, CR NUL or LF

		if ((c == 10 || c == 0) && S->PrevCR)
		{
			S->PrevCR = 0;
			continue;
		}

		S->PrevCR = (c == 13);

		if (c == 13 || c == 10)
		{
			if (S->Echo && S->State != TN_PASS)
				Queue(S, (const UCHAR *)"\r\n", 2);

			EndLine(S);
			continue;
		}

		if (c == 0 && S->FBB)
			continue;

		if (!S->FBB && (c == 8 || c == 127))
		{
			if (S->LineLen > 0)
			{
				S->LineLen--;

				if (S->Echo && S->State != TN_PASS)
					Queue(S, (const UCHAR *)"\b \b", 3);
			}
			continue;
		}

		if (S->Echo && S->State != TN_PASS)
			Queue(S, &c, 1);

		if (S->LineLen >= TN_LINE - 1)
		{
			// A very long line. In the BBS pass it on in pieces, as LinBPQ
			// does; at a login or the prompt it is not a name or a command.

			if (S->State == TN_BBS || S->State == TN_WAITSTREAM)
				ToBBS(S, S->Line, S->LineLen);

			S->LineLen = 0;
		}

		S->Line[S->LineLen++] = c;
	}
}

//	Listening

static int Listen(int Port, char * What)
{
	struct addrinfo Hints = {0}, * Result = NULL;
	char PortText[16];
	int Sock, One = 1, Err;

	Hints.ai_family = AF_UNSPEC;
	Hints.ai_socktype = SOCK_STREAM;
	Hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
	sprintf(PortText, "%d", Port);

	Err = getaddrinfo(BindAddr[0] ? BindAddr : NULL, PortText, &Hints, &Result);

	if (Err != 0 || Result == NULL)
	{
		TelnetLog("telnet_bind %s is not an IP address - %s logins off", BindAddr, What);
		return -1;
	}

	Sock = socket(Result->ai_family, SOCK_STREAM, 0);

	if (Sock < 0)
	{
		freeaddrinfo(Result);
		return -1;
	}

	setsockopt(Sock, SOL_SOCKET, SO_REUSEADDR, &One, sizeof(One));

	if (bind(Sock, Result->ai_addr, Result->ai_addrlen) != 0 || listen(Sock, 64) != 0)
	{
		TelnetLog("cannot listen on %s port %d (%s) - %s logins off. pdn's own telnet console "
			"uses 127.0.0.1:8011 unless told otherwise; move one of them.", BindAddr, Port, strerror(errno), What);
		close(Sock);
		freeaddrinfo(Result);
		return -1;
	}

	freeaddrinfo(Result);
	fcntl(Sock, F_SETFL, fcntl(Sock, F_GETFL) | O_NONBLOCK);

	TelnetLog("%s logins on %s port %d, %d user%s", What, BindAddr, Port, UserCount, UserCount == 1 ? "" : "s");
	return Sock;
}

void PdnTelnetInit()
{
	int i;

	for (i = 0; i < TN_MAXSESS; i++)
	{
		memset(&Sess[i], 0, sizeof(struct TelnetSess));
		Sess[i].Sock = -1;
		Sess[i].Stream = -1;
		Sess[i].User = -1;
	}

	if (TelnetPort > 0)
		TelnetListen = Listen(TelnetPort, "telnet");

	if (FBBPort > 0)
		FBBListen = Listen(FBBPort, "FBB");

	if ((TelnetListen >= 0 || FBBListen >= 0) && UserCount == 0)
		TelnetLog("no user lines in linmail-pdn.conf, so nobody can log in");
}

static int Readable(struct TelnetSess * S, uint64_t Now);
static void Input(struct TelnetSess * S, UCHAR * Data, int Len);

static void ReadSome(struct TelnetSess * S, uint64_t NowMilli)
{
	// Input: a little at a time, so no one session holds up the rest

	UCHAR Buffer[TN_READ];
	int n, Reads;

	for (Reads = 0; Reads < TN_READS && S->Sock >= 0 && S->State != TN_FREE && Readable(S, NowMilli); Reads++)
	{
		int Before = S->State == TN_USER || S->State == TN_PASS;

		n = recv(S->Sock, Buffer, sizeof(Buffer), MSG_DONTWAIT);

		if (n > 0)
		{
			if (Before)
			{
				S->PreLogin += n;

				if (S->PreLogin > TN_PRELOGIN)
				{
					TelnetNoise("%s sent too much without logging in - closed", S->Peer);
					CloseSess(S);
					break;
				}
			}

			Input(S, Buffer, n);
			continue;
		}

		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			break;

		// Closed by the client, or broken

		if (S->State == TN_BBS)
			TelnetLog("%s (%s) disconnected", Users[S->User].Call, S->Peer);

		CloseSess(S);
	}

	memset(Buffer, 0, sizeof(Buffer));		// It may have held a password
}

static int LoggingIn(struct TelnetSess * S)
{
	return S->Sock >= 0 && (S->State == TN_USER || S->State == TN_PASS);
}

static int Protected(struct TelnetSess * S)
{
	// A login whose user line has just come is waiting for its password,
	// which may be a network round trip behind (the head end writes user,
	// password and BBS separately). It is safe for TN_SPOKE ms from its
	// user line, once: more bytes don't renew it, so a client trickling
	// bytes can't hold slots this way.

	return S->State == TN_PASS && NowMs() - S->PassSinceMs < TN_SPOKE;
}

static struct TelnetSess * ToDrop(char * Host)
{
	// The login in progress to drop for a newcomer (from Host, or from
	// anywhere): the oldest by connect order. Each candidate is read once
	// more first, so one whose login has just arrived (the head end sends
	// its login straight after connecting) logs in instead, and the next
	// oldest is tried. One just given its user line is not dropped (see
	// Protected). Returns
	// NULL if there is nothing to drop. A candidate that turns out to have
	// gone is returned already closed.

	while (1)
	{
		struct TelnetSess * Oldest = NULL;
		int i;

		for (i = 0; i < TN_MAXSESS; i++)
		{
			struct TelnetSess * T = &Sess[i];

			if (LoggingIn(T) && (Host == NULL || strcmp(T->Peer, Host) == 0)
				&& !Protected(T)
				&& (Oldest == NULL || T->ConnectNo < Oldest->ConnectNo))
				Oldest = T;
		}

		if (Oldest == NULL)
			return NULL;

		ReadSome(Oldest, NowMs());

		if (Oldest->Sock < 0)
			return Oldest;

		if (LoggingIn(Oldest) && !Protected(Oldest))
			return Oldest;
	}
}

static int CountSessions(char * Host, struct TelnetSess ** Free)
{
	// Sessions in use, or (with Host) logins in progress from there

	int i, Count = 0;

	for (i = 0; i < TN_MAXSESS; i++)
	{
		struct TelnetSess * T = &Sess[i];

		if (T->Sock < 0)
		{
			if (Free && *Free == NULL)
				*Free = T;
			continue;
		}

		if (Host == NULL || (LoggingIn(T) && strcmp(T->Peer, Host) == 0))
			Count++;
	}
	return Count;
}

static void Accept(int Listener, int FBB)
{
	// A few at a time: a flood of connects must not keep the main loop (and
	// so the BBS) from running. The rest wait in the kernel until next time.

	int Taken;

	for (Taken = 0; Taken < TN_ACCEPTS; Taken++)
	{
		struct sockaddr_storage Peer;
		socklen_t PLen = sizeof(Peer);
		struct TelnetSess * S = NULL, * T;
		char Host[64] = "?";
		int Sock = accept(Listener, (struct sockaddr *)&Peer, &PLen);
		int One = 1;

		if (Sock < 0)
			return;

		getnameinfo((struct sockaddr *)&Peer, PLen, Host, sizeof(Host), NULL, 0, NI_NUMERICHOST);

		if (CountSessions(Host, NULL) >= TN_PERADDR && (T = ToDrop(Host)) != NULL)
		{
			// Only a few logins in progress from one address: the oldest of
			// them makes way, so one address can't take every slot, and
			// can't lock out a client of its own that logs in promptly

			if (T->Sock >= 0)
			{
				TelnetNoise("%d logins in progress from %s - dropped the oldest", TN_PERADDR, Host);
				CloseSess(T);
			}
		}

		if (CountSessions(Host, NULL) >= TN_PERADDR)
		{
			// All of them are part way through logging in: this one waits

			TelnetNoise("connect from %s refused - %d logins from there already in progress", Host, TN_PERADDR);
			send(Sock, "No Free Sessions\r\n", 18, MSG_NOSIGNAL | MSG_DONTWAIT);
			close(Sock);
			continue;
		}

		if (CountSessions(NULL, &S) >= MaxSessions || S == NULL)
		{
			// Full. Make room by dropping the oldest login in progress, so
			// connections that never log in can't lock everyone out.

			T = ToDrop(NULL);

			if (T == NULL)
			{
				TelnetNoise("connect from %s refused - all %d sessions in use", Host, MaxSessions);
				send(Sock, "No Free Sessions\r\n", 18, MSG_NOSIGNAL | MSG_DONTWAIT);
				close(Sock);
				continue;
			}

			if (T->Sock >= 0)
			{
				TelnetNoise("all %d sessions in use - dropped the login in progress from %s", MaxSessions, T->Peer);
				CloseSess(T);
			}

			S = NULL;
			CountSessions(NULL, &S);
		}

		fcntl(Sock, F_SETFL, fcntl(Sock, F_GETFL) | O_NONBLOCK);
		setsockopt(Sock, IPPROTO_TCP, TCP_NODELAY, &One, sizeof(One));

		memset(S, 0, sizeof(struct TelnetSess));
		S->Sock = Sock;
		S->FBB = FBB;
		S->State = TN_USER;
		S->User = -1;
		S->Stream = -1;
		S->Since = S->LastInput = time(NULL);
		S->ConnectNo = ++Connects;
		strcpy(S->Peer, Host);

		TelnetNoise("connect from %s on the %s port", Host, FBB ? "FBB" : "telnet");

		if (!FBB)
		{
			// As LinBPQ: offer to echo and to suppress go-ahead, then ask

			TelnetCmd(S, WILL, OPT_SGA);
			TelnetCmd(S, WILL, OPT_ECHO);
			S->SGAOffered = S->EchoOffered = 1;
			Say(S, "user:");
		}

		// Read it at once: a client that sends its login blind (the head
		// end) is dealt with before any later connect in this pass, and
		// one that has already gone frees its slot

		ReadSome(S, NowMs());
	}
}

static int Readable(struct TelnetSess * S, uint64_t Now)
{
	// Whether to read this session now. Not while a failed login's delay
	// runs, nor while the BBS (or the wait for a stream) still holds a lot
	// of what it sent: leaving it in the socket makes TCP push back.

	if (S->NotBefore && Now < S->NotBefore)
		return FALSE;

	if (S->State == TN_BBS && PdnLocalBacklog(S->Stream) >= TN_BACKLOG)
		return FALSE;

	if (S->State == TN_WAITSTREAM && S->HoldLen >= TN_BACKLOG)
		return FALSE;

	return TRUE;
}

void PdnTelnetFds(fd_set * Read, fd_set * Write, int * Max)
{
	uint64_t Now = NowMs();
	int i;

	if (TelnetListen >= 0)
	{
		FD_SET(TelnetListen, Read);
		if (TelnetListen > *Max) *Max = TelnetListen;
	}

	if (FBBListen >= 0)
	{
		FD_SET(FBBListen, Read);
		if (FBBListen > *Max) *Max = FBBListen;
	}

	for (i = 0; i < TN_MAXSESS; i++)
	{
		struct TelnetSess * S = &Sess[i];

		if (S->Sock < 0)
			continue;

		if (Readable(S, Now))
			FD_SET(S->Sock, Read);

		if (S->OutLen > 0)
			FD_SET(S->Sock, Write);

		if (S->Sock > *Max)
			*Max = S->Sock;
	}
}

static void AfterDelay(struct TelnetSess * S)
{
	// A failed login's delay is over: answer it, then deal with what came
	// in meanwhile

	UCHAR * Rest = S->Defer;
	int RestLen = S->DeferLen;

	S->NotBefore = 0;
	S->Defer = NULL;
	S->DeferLen = 0;

	if (S->Pending == 2)
	{
		S->Pending = 0;
		free(Rest);
		Say(S, AttemptsMsg);
		StartClosing(S);
		return;
	}

	S->Pending = 0;
	Say(S, "user:");

	if (Rest)
	{
		Input(S, Rest, RestLen);
		memset(Rest, 0, RestLen);			// It may have held a password
		free(Rest);
	}
}

void PdnTelnetPoll()
{
	time_t Now = time(NULL);
	uint64_t NowMilli = NowMs();
	int i;

	if (TelnetListen >= 0)
		Accept(TelnetListen, FALSE);

	if (FBBListen >= 0)
		Accept(FBBListen, TRUE);

	for (i = 0; i < TN_MAXSESS; i++)
	{
		struct TelnetSess * S = &Sess[i];

		if (S->Sock < 0)
			continue;

		if (S->NotBefore && NowMilli < S->NotBefore)
		{
			// In a failed login's delay nothing is read, but a client that
			// has hung up must not hold its slot until the delay ends

			UCHAR Peek;
			int n = recv(S->Sock, &Peek, 1, MSG_PEEK | MSG_DONTWAIT);

			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			{
				CloseSess(S);
				continue;
			}
		}

		if (S->NotBefore && NowMilli >= S->NotBefore)
			AfterDelay(S);

		ReadSome(S, NowMilli);

		if (S->Sock < 0)
			continue;


		FlushOut(S);

		if (S->OutLen < 0)
		{
			TelnetLog("%s stopped taking data - closed", S->Peer);
			CloseSess(S);
			continue;
		}

		switch (S->State)
		{
		case TN_USER:
		case TN_PASS:
			if (Now - S->Since > TN_LOGINTIME)
			{
				TelnetNoise("no login from %s in %d seconds - closed", S->Peer, TN_LOGINTIME);
				CloseSess(S);
			}
			break;

		case TN_PROMPT:
			if (Now - S->LastInput > TN_PROMPTIDLE)
			{
				TelnetLog("%s idle at the prompt - closed", S->Peer);
				CloseSess(S);
			}
			break;

		case TN_WAITSTREAM:
			TryBBS(S);

			if (S->State == TN_WAITSTREAM && Now - S->Since > TN_STREAMWAIT)
			{
				TelnetLog("%s refused - no free BBS streams", Users[S->User].Call);
				Say(S, "Sorry, no free BBS streams - try again later\r\n");
				StartClosing(S);
			}
			break;

		case TN_CLOSING:
			if (S->OutLen == 0 || Now - S->Since > TN_CLOSEWAIT)
				CloseSess(S);
			break;
		}
	}
}

void PdnTelnetClose()
{
	int i;

	for (i = 0; i < TN_MAXSESS; i++)
	{
		if (Sess[i].Sock >= 0)
		{
			FlushOut(&Sess[i]);
			Sess[i].Stream = -1;
			CloseSess(&Sess[i]);
		}
	}

	if (TelnetListen >= 0)
		close(TelnetListen);

	if (FBBListen >= 0)
		close(FBBListen);

	TelnetListen = FBBListen = -1;
}
