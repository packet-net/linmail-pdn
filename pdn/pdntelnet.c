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
#define TN_LOGINTIME 120			// Seconds to log in
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
	char Name[64];
	char Pass[64];
	char Call[10];
	int Sysop;
	int StraightIn;					// appl field BBS: no need to type BBS
};

struct TelnetSess
{
	int Sock;
	int FBB;						// Logged in on the FBB port
	int State;
	int User;						// Index into Users once the name is known, else -1
	int Fails;
	int Echo;						// The client asked us to echo
	int EchoOffered;				// We said WILL ECHO and have not taken it back
	int SGAOffered;
	int Tel;						// Telnet command parser state
	int Verb;
	int PrevCR;						// Last input byte was a CR, so a LF or NUL after it goes
	int Stream;						// pdnhost stream (0 based) when in the BBS, else -1
	time_t Since;					// When the current state began
	time_t LastInput;
	char Peer[64];
	UCHAR Line[TN_LINE + 1];
	int LineLen;
	UCHAR * Out;
	int OutLen;
	int OutSize;
	UCHAR * Hold;					// Data for the BBS while waiting for a stream
	int HoldLen;
};

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

//	Settings

static void AddUser(char * Value)
{
	// LinBPQ's USER=user,password,call,appl,SYSOP. As there, each field ends
	// at its first space, and spaces before a field are skipped.

	char Field[5][64];
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

		strncpy(Field[n], ptr, 63);
		strlop(Field[n], ' ');
		strlop(Field[n], '\t');
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
		TelnetLog("user %s: %s is not a callsign the BBS can use - ignored", Field[0], Field[2]);
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
		TelnetLog("user %s: application %s ignored - the session starts at the prompt, where BBS enters the mailbox",
			Field[0], Field[3]);
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

static void LoginFailed(struct TelnetSess * S, char * Why)
{
	S->Fails++;

	TelnetLog("login failed from %s: %s", S->Peer, Why);

	if (S->Fails >= TN_MAXFAILS)
	{
		Say(S, AttemptsMsg);
		StartClosing(S);
	}
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
	int i;

	if (S->State == TN_USER)
	{
		if (Line[0] == 0)
		{
			if (!S->FBB)
				Say(S, "user:");
			return;
		}

		S->User = -1;

		for (i = 0; i < UserCount; i++)
		{
			if (strcmp(Line, Users[i].Name) == 0)
			{
				S->User = i;
				break;
			}
		}

		if (S->User < 0)
		{
			// Not the name: people type passwords here by mistake

			LoginFailed(S, "unknown user");

			if (S->State != TN_CLOSING)
				Say(S, "user:");
			return;
		}

		S->State = TN_PASS;

		if (!S->FBB)
			Say(S, "password:");
		return;
	}

	// The password. LinBPQ answers it with a new line at once, right or
	// wrong, as the password was not echoed.

	if (!S->FBB)
		Say(S, "\r\n");

	if (strcmp(Line, Users[S->User].Pass) != 0)
	{
		char Why[100];

		snprintf(Why, sizeof(Why), "wrong password for %s", Users[S->User].Name);
		LoginFailed(S, Why);

		if (S->State != TN_CLOSING)
			Say(S, "password:");
		return;
	}

	S->Fails = 0;
	S->State = TN_PROMPT;
	S->Since = time(NULL);

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

	if (bind(Sock, Result->ai_addr, Result->ai_addrlen) != 0 || listen(Sock, 8) != 0)
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

static void Accept(int Listener, int FBB)
{
	while (1)
	{
		struct sockaddr_storage Peer;
		socklen_t PLen = sizeof(Peer);
		struct TelnetSess * S = NULL;
		char Host[64] = "?";
		int Sock = accept(Listener, (struct sockaddr *)&Peer, &PLen);
		int i, Count = 0, One = 1;

		if (Sock < 0)
			return;

		getnameinfo((struct sockaddr *)&Peer, PLen, Host, sizeof(Host), NULL, 0, NI_NUMERICHOST);

		for (i = 0; i < TN_MAXSESS; i++)
		{
			if (Sess[i].Sock >= 0)
				Count++;
			else if (S == NULL)
				S = &Sess[i];
		}

		if (Count >= MaxSessions || S == NULL)
		{
			TelnetLog("connect from %s refused - all %d sessions in use", Host, MaxSessions);
			send(Sock, "No Free Sessions\r\n", 18, MSG_NOSIGNAL | MSG_DONTWAIT);
			close(Sock);
			continue;
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
		strcpy(S->Peer, Host);

		TelnetLog("connect from %s on the %s port", Host, FBB ? "FBB" : "telnet");

		if (!FBB)
		{
			// As LinBPQ: offer to echo and to suppress go-ahead, then ask

			TelnetCmd(S, WILL, OPT_SGA);
			TelnetCmd(S, WILL, OPT_ECHO);
			S->SGAOffered = S->EchoOffered = 1;
			Say(S, "user:");
		}
	}
}

void PdnTelnetFds(fd_set * Read, fd_set * Write, int * Max)
{
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

		FD_SET(S->Sock, Read);

		if (S->OutLen > 0)
			FD_SET(S->Sock, Write);

		if (S->Sock > *Max)
			*Max = S->Sock;
	}
}

void PdnTelnetPoll()
{
	time_t Now = time(NULL);
	UCHAR Buffer[4096];
	int i, n;

	if (TelnetListen >= 0)
		Accept(TelnetListen, FALSE);

	if (FBBListen >= 0)
		Accept(FBBListen, TRUE);

	for (i = 0; i < TN_MAXSESS; i++)
	{
		struct TelnetSess * S = &Sess[i];

		if (S->Sock < 0)
			continue;

		// Input

		while (S->Sock >= 0)
		{
			n = recv(S->Sock, Buffer, sizeof(Buffer), MSG_DONTWAIT);

			if (n > 0)
			{
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

		if (S->Sock < 0)
			continue;

		memset(Buffer, 0, sizeof(Buffer));		// It may have held a password

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
				TelnetLog("no login from %s in %d seconds - closed", S->Peer, TN_LOGINTIME);
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
