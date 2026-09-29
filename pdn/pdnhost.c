/*
Copyright 2026 the linbpq fork contributors

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

//	linmail-pdn host API shim
//
//	Implements the BPQ host API calls the mail code uses (SessionControl,
//	SessionState, GetMsg, SendMsg, GetConnectionInfo, ...) on top of an RHPv2
//	client connection to a packet.net (pdn) node.
//
//	Inbound:  socket/bind/listen on the BBS callsign. Each accepted child
//			  handle is attached to a free BBS stream, so the mail code sees a
//			  normal connect, data and disconnect.
//
//	Outbound: ConnectUsingAppl gives the mail code a stream "connected to the
//			  node", as in BPQ. The connect script's first line (C port CALL)
//			  is turned into an RHP open, and the result comes back as the
//			  BPQ-style node text the script engine looks for (Connected to,
//			  Busy from, Failure with). Once the open succeeds the stream is
//			  transparent, so later script lines go to the far node as data.
//
//	All RHP socket I/O happens on the main thread in PdnHostPoll. API calls
//	from other threads (the mail code's connect delay threads) only change
//	stream state under PdnLock, and queue RHP writes on the socket directly.

#include "bpqmail.h"
#include "pdnhost.h"

#include <jansson.h>
#include <pthread.h>
#include <netdb.h>
#include <netinet/tcp.h>

struct PdnConfig PdnCfg;

BPQVECSTRUC BPQHOSTVECTOR[BPQHOSTSTREAMS + 5];

//	Stream modes

#define PDN_IDLE	0		// Not connected
#define PDN_NODECMD	1		// Outbound, talking to the (emulated) node command handler
#define PDN_OPENING	2		// Outbound, RHP open sent, waiting for openReply
#define PDN_LINKED	3		// Attached to a connected RHP stream handle

struct PdnBuf
{
	struct PdnBuf * Next;
	int Len;
	UCHAR Data[256];
};

struct PdnStream
{
	int Mode;
	int Handle;					// RHP handle when LINKED
	int OpenId;					// id of the outstanding open when OPENING
	int Incoming;
	int TxOutstanding;			// sends not yet answered by sendReply
	time_t LastTx;
	time_t LastActivity;		// Last data either way, for the idle timeout
	int IdleTime;				// Seconds, as set by ChangeSessionIdletime. 0 = none
	char Remote[16];
	char PortLabel[32];
	struct PdnBuf * RxHead;
	struct PdnBuf * RxTail;
	int RxCount;
	UCHAR * Hold;				// Data sent while an open is in progress
	int HoldLen;
	char CmdBuf[512];			// Node command line being assembled
	int CmdLen;
	int RemoteClosed;			// pdn closed the handle; end once the data is read
};

static struct PdnStream Streams[PDN_MAXSTREAMS];
static TRANSPORTENTRY Sessions[PDN_MAXSTREAMS];

// Handles we have finished with but not yet closed (disconnect linger), and
// opens abandoned before their reply arrived

#define PDN_MAXDEFER 64

struct PdnDeferred
{
	int Handle;
	int OpenId;
	time_t CloseAt;
};

static struct PdnDeferred Deferred[PDN_MAXDEFER];

static pthread_mutex_t PdnLock;

static int RHPSock = -1;
static time_t RHPRetryAt = 0;
static int RHPWasUp = 0;
static int NextId = 1;

// Listeners: the BBS callsign, then any aliases (such as BBS)

#define PDN_MAXLISTEN 4

struct PdnListener
{
	char Call[10];
	int Handle;
	int State;					// 0 none, 1 socket sent, 2 bind sent, 3 listen sent, 4 listening
	int ReqId;					// id of the outstanding socket/bind/listen
	time_t RetryAt;
};

static struct PdnListener Listeners[PDN_MAXLISTEN];
static int ListenerCount = 0;

// One datagram socket for UI frames (mail-for beacons, FBB header broadcasts)

#define PDN_MAXUIQ 16

struct PdnUI
{
	char Port[32];
	char Local[10];
	char Remote[10];
	int Len;
	UCHAR Data[256];
};

static int UiHandle = 0;
static int UiOpenId = 0;
static struct PdnUI UiQueue[PDN_MAXUIQ];
static int UiQueueLen = 0;

// Inbound connects waiting for a free stream

#define PDN_MAXPENDING 8

struct PdnPending
{
	int Child;
	char Remote[16];
	char Local[10];
	char Port[32];
	time_t Since;
	UCHAR * Buf;				// Data that arrived before the stream was free
	int BufLen;
};

static struct PdnPending Pending[PDN_MAXPENDING];

static void AttachPending();

static UCHAR RxFrame[65536 + 2];
static int RxFrameLen = 0;

VOID __cdecl Debugprintf(const char * format, ...);
int ConvFromAX25(unsigned char * incall, unsigned char * outcall);
BOOL ConvToAX25(unsigned char * callsign, unsigned char * ax25call);

static void RHPDown(char * Why);
static void QueueRx(int n, char * Msg, int Len);

static void PdnLog(const char * format, ...)
{
	char Mess[1024];
	va_list(arglist);

	va_start(arglist, format);
	vsnprintf(Mess, sizeof(Mess), format, arglist);
	va_end(arglist);

	printf("linmail-pdn: %s\n", Mess);
	Debugprintf("linmail-pdn: %s", Mess);
}

//	Port map. Connect scripts say "C 2 GB7XYZ"; pdn ports are named by id.

int PdnParsePortMap(char * Map)
{
	// "1=axudp,2=vhf"

	char * Copy = _strdup(Map);
	char * Context;
	char * Item = strtok_s(Copy, ", ", &Context);

	while (Item && PdnCfg.PortCount < PDN_MAXPORTMAP)
	{
		char * Label = strchr(Item, '=');

		if (Label)
		{
			*(Label++) = 0;
			PdnCfg.PortNum[PdnCfg.PortCount] = atoi(Item);
			strncpy(PdnCfg.PortLabel[PdnCfg.PortCount], Label, 31);
			PdnCfg.PortCount++;
		}
		Item = strtok_s(NULL, ", ", &Context);
	}
	free(Copy);
	return PdnCfg.PortCount;
}

static char * PortLabelFromText(char * Text)
{
	// A number is looked up in the port map. Anything else (or an unmapped
	// number) is passed to pdn as a port id.

	int i, Num;
	char * ptr = Text;

	while (*ptr && isdigit((unsigned char)*ptr))
		ptr++;

	if (*ptr == 0)
	{
		Num = atoi(Text);

		for (i = 0; i < PdnCfg.PortCount; i++)
		{
			if (PdnCfg.PortNum[i] == Num)
				return PdnCfg.PortLabel[i];
		}
	}
	return Text;
}

int PdnPortNumber(const char * Label)
{
	int i;

	if (Label == NULL || Label[0] == 0)
		return 0;

	for (i = 0; i < PdnCfg.PortCount; i++)
	{
		if (_stricmp(PdnCfg.PortLabel[i], Label) == 0)
			return PdnCfg.PortNum[i];
	}
	return atoi(Label);
}

//	RHP data is bytes carried as a Latin-1 JSON string (one byte per code unit)

static json_t * BytesToJson(UCHAR * Data, int Len)
{
	char * Utf8 = malloc(Len * 2 + 1);
	int i, n = 0;
	json_t * Result;

	for (i = 0; i < Len; i++)
	{
		UCHAR c = Data[i];

		if (c < 0x80)
			Utf8[n++] = c;
		else
		{
			Utf8[n++] = 0xC0 | (c >> 6);
			Utf8[n++] = 0x80 | (c & 0x3F);
		}
	}
	Result = json_stringn(Utf8, n);
	free(Utf8);
	return Result;
}

static int JsonToBytes(json_t * Value, UCHAR * Out, int Max)
{
	const UCHAR * In = (const UCHAR *)json_string_value(Value);
	size_t InLen = json_string_length(Value);
	size_t i = 0;
	int n = 0;

	if (In == NULL)
		return 0;

	while (i < InLen && n < Max)
	{
		UCHAR c = In[i];

		if (c < 0x80)
		{
			Out[n++] = c;
			i++;
		}
		else if ((c & 0xE0) == 0xC0 && i + 1 < InLen)
		{
			int cp = ((c & 0x1F) << 6) | (In[i + 1] & 0x3F);
			Out[n++] = cp > 255 ? '?' : cp;
			i += 2;
		}
		else
		{
			// Not Latin-1 - should never happen on this wire. Skip the sequence.

			Out[n++] = '?';
			i++;
			while (i < InLen && (In[i] & 0xC0) == 0x80)
				i++;
		}
	}
	return n;
}

//	RHP framing: two byte big endian length, then the JSON text

static int RHPSend(json_t * Msg)
{
	char * Text;
	size_t Len;
	UCHAR * Frame;
	int Sent = 0, Ret;

	if (RHPSock == -1)
	{
		json_decref(Msg);
		return 0;
	}

	Text = json_dumps(Msg, JSON_COMPACT | JSON_PRESERVE_ORDER);
	json_decref(Msg);

	if (Text == NULL)
		return 0;

	Len = strlen(Text);

	if (Len > 0xFFFF)
	{
		PdnLog("RHP message too long (%d) - dropped", (int)Len);
		free(Text);
		return 0;
	}

	Frame = malloc(Len + 2);
	Frame[0] = (UCHAR)(Len >> 8);
	Frame[1] = (UCHAR)(Len & 0xFF);
	memcpy(&Frame[2], Text, Len);

	if (PdnCfg.Trace)
	{
		if (strncmp(Text, "{\"type\":\"auth\",", 15) == 0)
			printf("linmail-pdn: RHP > {\"type\":\"auth\", password not shown}\n");
		else
			printf("linmail-pdn: RHP > %s\n", Text);
	}
	free(Text);

	Len += 2;

	while (Sent < (int)Len)
	{
		Ret = send(RHPSock, &Frame[Sent], Len - Sent, MSG_NOSIGNAL);

		if (Ret < 0)
		{
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				fd_set wfds;
				struct timeval tv = {1, 0};

				FD_ZERO(&wfds);
				FD_SET(RHPSock, &wfds);
				select(RHPSock + 1, NULL, &wfds, NULL, &tv);
				continue;
			}
			free(Frame);
			RHPDown("send failed");
			return 0;
		}
		Sent += Ret;
	}
	free(Frame);
	return 1;
}

static json_t * NewRequest(char * Type, int * Id)
{
	json_t * Msg = json_object();

	json_object_set_new(Msg, "type", json_string(Type));

	if (Id)
	{
		*Id = NextId++;
		json_object_set_new(Msg, "id", json_integer(*Id));
	}
	return Msg;
}

static void RHPClose(int Handle)
{
	json_t * Msg;
	int Id;

	if (Handle <= 0)
		return;

	Msg = NewRequest("close", &Id);
	json_object_set_new(Msg, "handle", json_integer(Handle));
	RHPSend(Msg);
}

static void AddDeferred(int Handle, int OpenId, time_t CloseAt)
{
	int i;

	for (i = 0; i < PDN_MAXDEFER; i++)
	{
		if (Deferred[i].Handle == 0 && Deferred[i].OpenId == 0)
		{
			Deferred[i].Handle = Handle;
			Deferred[i].OpenId = OpenId;
			Deferred[i].CloseAt = CloseAt;
			return;
		}
	}

	// Table full - close now

	if (Handle)
		RHPClose(Handle);
}

static char * NodeHeader(char * Buffer)
{
	if (PdnCfg.NodeAlias[0] && PdnCfg.NodeCall[0])
		sprintf(Buffer, "%s:%s} ", PdnCfg.NodeAlias, PdnCfg.NodeCall);
	else if (PdnCfg.NodeCall[0])
		sprintf(Buffer, "%s} ", PdnCfg.NodeCall);
	else
		Buffer[0] = 0;

	return Buffer;
}

static void NodeReply(int n, const char * format, ...)
{
	char Mess[512];
	int Len;
	va_list(arglist);

	NodeHeader(Mess);
	Len = (int)strlen(Mess);

	va_start(arglist, format);
	Len += vsnprintf(&Mess[Len], sizeof(Mess) - Len, format, arglist);
	va_end(arglist);

	if (Len > (int)sizeof(Mess) - 1)
		Len = sizeof(Mess) - 1;

	QueueRx(n, Mess, Len);
}

//	Stream housekeeping

static void FreeRx(struct PdnStream * STREAM)
{
	struct PdnBuf * Buf = STREAM->RxHead;

	while (Buf)
	{
		struct PdnBuf * Next = Buf->Next;
		free(Buf);
		Buf = Next;
	}
	STREAM->RxHead = STREAM->RxTail = NULL;
	STREAM->RxCount = 0;

	if (STREAM->Hold)
		free(STREAM->Hold);

	STREAM->Hold = NULL;
	STREAM->HoldLen = 0;
}

static void QueueRx(int n, char * Msg, int Len)
{
	// Split into BPQ sized (256 byte) messages

	struct PdnStream * STREAM = &Streams[n];

	STREAM->LastActivity = time(NULL);

	while (Len > 0)
	{
		struct PdnBuf * Buf = malloc(sizeof(struct PdnBuf));
		int Chunk = Len > 256 ? 256 : Len;

		Buf->Next = NULL;
		Buf->Len = Chunk;
		memcpy(Buf->Data, Msg, Chunk);

		if (STREAM->RxTail)
			STREAM->RxTail->Next = Buf;
		else
			STREAM->RxHead = Buf;

		STREAM->RxTail = Buf;
		STREAM->RxCount++;

		Msg += Chunk;
		Len -= Chunk;
	}
}

static void StartSession(int n, char * Local)
{
	// Mark the stream connected. L4USER is our end's callsign, which the mail
	// code reads (and ChangeSessionCallsign writes) through BPQHOSTVECTOR

	TRANSPORTENTRY * L4 = &Sessions[n];

	memset(L4, 0, sizeof(TRANSPORTENTRY));
	ConvToAX25(Local, L4->L4USER);
	L4->L4CIRCUITTYPE = BPQHOST | UPLINK;

	BPQHOSTVECTOR[n].HOSTSESSION = L4;
	BPQHOSTVECTOR[n].HOSTFLAGS |= 1;		// State change

	Streams[n].LastActivity = time(NULL);
}

static void EndSession(int n)
{
	// The stream is no longer connected. Tell the mail code.

	struct PdnStream * STREAM = &Streams[n];

	FreeRx(STREAM);
	STREAM->Mode = PDN_IDLE;
	STREAM->Handle = 0;
	STREAM->OpenId = 0;
	STREAM->Incoming = 0;
	STREAM->TxOutstanding = 0;
	STREAM->CmdLen = 0;
	STREAM->RemoteClosed = 0;
	STREAM->Remote[0] = 0;
	STREAM->PortLabel[0] = 0;

	if (BPQHOSTVECTOR[n].HOSTSESSION)
	{
		BPQHOSTVECTOR[n].HOSTSESSION = NULL;
		BPQHOSTVECTOR[n].HOSTFLAGS |= 1;	// State change
	}
}

static int FindStreamByHandle(int Handle)
{
	int n;

	for (n = 0; n < PDN_MAXSTREAMS; n++)
	{
		if (Handle && Streams[n].Mode == PDN_LINKED && Streams[n].Handle == Handle)
			return n;
	}
	return -1;
}

static int FindStreamByOpenId(int Id)
{
	int n;

	for (n = 0; n < PDN_MAXSTREAMS; n++)
	{
		if (Streams[n].Mode == PDN_OPENING && Streams[n].OpenId == Id)
			return n;
	}
	return -1;
}

//	Emulated node command handler (outbound sessions before the RHP open)

static int IsCallsign(char * Call)
{
	// Callsign with optional numeric SSID 0-15

	char * ssid = strchr(Call, '-');
	int len = ssid ? (int)(ssid - Call) : (int)strlen(Call);
	int i, digits = 0;

	if (len < 3 || len > 6)
		return 0;

	for (i = 0; i < len; i++)
	{
		if (!isalnum((unsigned char)Call[i]))
			return 0;
		if (isdigit((unsigned char)Call[i]))
			digits++;
	}

	if (digits == 0)
		return 0;

	if (ssid)
	{
		int s;

		ssid++;
		if (*ssid == 0 || strlen(ssid) > 2)
			return 0;

		for (i = 0; ssid[i]; i++)
			if (!isdigit((unsigned char)ssid[i]))
				return 0;

		s = atoi(ssid);

		if (s > 15)
			return 0;
	}
	return 1;
}

static void Unmapped(int n, char * Line, char * Why)
{
	PdnLog("Stream %d: node command not mapped to pdn: \"%s\" (%s)", n + 1, Line, Why);
	NodeReply(n, "Sorry, %s\r", Why);
}

static void NodeConnect(int n, char ** Tok, int Count, char * Line)
{
	struct PdnStream * STREAM = &Streams[n];
	char * Port = NULL;
	char * Call;
	char Local[16];
	json_t * Msg;
	int i;

	// Drop a trailing S (stay) - pdn returns us to nothing, so stay means nothing here

	if (Count > 1 && (_stricmp(Tok[Count - 1], "S") == 0 || _stricmp(Tok[Count - 1], "STAY") == 0))
		Count--;

	for (i = 1; i < Count; i++)
	{
		if (_stricmp(Tok[i], "V") == 0 || _stricmp(Tok[i], "VIA") == 0)
		{
			Unmapped(n, Line, "digipeated connects are not supported by the pdn shim");
			return;
		}
	}

	if (Count == 1)
		Call = Tok[0];
	else if (Count == 2)
	{
		Port = PortLabelFromText(Tok[0]);
		Call = Tok[1];
	}
	else
	{
		Unmapped(n, Line, "connect command not understood");
		return;
	}

	_strupr(Call);

	if (!IsCallsign(Call))
	{
		// BPQ would try NET/ROM aliases and applications here. pdn's RHP open
		// is AX.25 only.

		Unmapped(n, Line, "only direct AX.25 connects to a callsign are supported");
		return;
	}

	if (Port == NULL && PdnCfg.DefaultPort[0])
		Port = PdnCfg.DefaultPort;

	if (RHPSock == -1)
	{
		NodeReply(n, "Failure with %s\r", Call);
		PdnLog("Stream %d: connect to %s failed - no RHP connection to pdn", n + 1, Call);
		return;
	}

	ConvFromAX25(Sessions[n].L4USER, Local);
	strlop(Local, ' ');

	Msg = NewRequest("open", &STREAM->OpenId);
	json_object_set_new(Msg, "pfam", json_string("ax25"));
	json_object_set_new(Msg, "mode", json_string("stream"));
	json_object_set_new(Msg, "port", Port ? json_string(Port) : json_null());
	json_object_set_new(Msg, "local", json_string(Local));
	json_object_set_new(Msg, "remote", json_string(Call));
	json_object_set_new(Msg, "flags", json_integer(128));		// Active

	STREAM->Mode = PDN_OPENING;
	strcpy(STREAM->Remote, Call);
	strncpy(STREAM->PortLabel, Port ? Port : "", 31);

	PdnLog("Stream %d: connecting %s to %s on port %s", n + 1, Local, Call, Port ? Port : "(local app)");

	RHPSend(Msg);
}

static void NodeCommand(int n, char * Line)
{
	char * Tok[16];
	int Count = 0;
	char * Copy = _strdup(Line);
	char * Context;
	char * ptr = strtok_s(Copy, " ", &Context);
	char * Verb;

	while (ptr && Count < 16)
	{
		Tok[Count++] = ptr;
		ptr = strtok_s(NULL, " ", &Context);
	}

	if (Count == 0)
	{
		free(Copy);
		return;
	}

	Verb = Tok[0];

	if (_stricmp(Verb, "C") == 0 || _stricmp(Verb, "CONNECT") == 0)
	{
		if (Count < 2)
			NodeReply(n, "Invalid command - connect needs a callsign\r");
		else
			NodeConnect(n, &Tok[1], Count - 1, Line);
	}
	else if (_stricmp(Verb, "B") == 0 || _stricmp(Verb, "BYE") == 0 ||
		_stricmp(Verb, "Q") == 0 || _stricmp(Verb, "QUIT") == 0)
	{
		EndSession(n);
	}
	else if (_stricmp(Verb, "PACLEN") == 0 || _stricmp(Verb, "IDLETIME") == 0)
	{
		// Session parameters. pdn chooses its own, so just agree.

		NodeReply(n, "Ok\r");
	}
	else
	{
		// NC, ATTACH, MCAST, RADIO, and the rest of the node command set

		Unmapped(n, Line, "command not supported by the pdn shim");
	}

	free(Copy);
}

static void NodeInput(int n, char * Msg, int Len)
{
	struct PdnStream * STREAM = &Streams[n];
	int i;

	for (i = 0; i < Len; i++)
	{
		char c = Msg[i];

		if (c == 13 || c == 10)
		{
			STREAM->CmdBuf[STREAM->CmdLen] = 0;

			if (STREAM->CmdLen)
				NodeCommand(n, STREAM->CmdBuf);

			STREAM->CmdLen = 0;

			// A connect may have changed the mode. Anything after the command
			// line is data for the far end (or held until the open completes).

			if (Streams[n].Mode != PDN_NODECMD)
			{
				if (i + 1 < Len)
					SendMsg(n + 1, &Msg[i + 1], Len - i - 1);
				return;
			}
		}
		else if (STREAM->CmdLen < (int)sizeof(STREAM->CmdBuf) - 1)
			STREAM->CmdBuf[STREAM->CmdLen++] = c;
	}
}

//	RHP message handlers

static int GetInt(json_t * Msg, char * Key)
{
	json_t * Value = json_object_get(Msg, Key);

	if (json_is_integer(Value))
		return (int)json_integer_value(Value);

	if (json_is_string(Value))
		return atoi(json_string_value(Value));

	return 0;
}

static const char * GetStr(json_t * Msg, char * Key)
{
	json_t * Value = json_object_get(Msg, Key);

	if (json_is_string(Value))
		return json_string_value(Value);

	if (json_is_integer(Value))
	{
		static char Num[32];
		sprintf(Num, "%d", (int)json_integer_value(Value));
		return Num;
	}
	return "";
}

static int ErrCode(json_t * Msg)
{
	int Err = GetInt(Msg, "errCode");

	if (Err == 0)
		Err = GetInt(Msg, "errcode");

	return Err;
}

static const char * ErrText(json_t * Msg)
{
	const char * Text = GetStr(Msg, "errText");

	if (Text[0] == 0)
		Text = GetStr(Msg, "errtext");

	return Text;
}

static void StartListen(struct PdnListener * LISTEN)
{
	json_t * Msg;

	Msg = NewRequest("socket", &LISTEN->ReqId);
	json_object_set_new(Msg, "pfam", json_string("ax25"));
	json_object_set_new(Msg, "mode", json_string("stream"));
	LISTEN->State = 1;
	LISTEN->RetryAt = 0;
	RHPSend(Msg);
}

static void ListenFailed(struct PdnListener * LISTEN, json_t * Msg, char * Step)
{
	PdnLog("RHP %s for %s failed: %d %s - retrying in 30 seconds", Step, LISTEN->Call, ErrCode(Msg), ErrText(Msg));

	if (LISTEN->Handle)
		RHPClose(LISTEN->Handle);

	LISTEN->Handle = 0;
	LISTEN->State = 0;
	LISTEN->RetryAt = time(NULL) + 30;
}

static int ProcessListenReply(char * Type, json_t * Msg)
{
	// Returns TRUE if the reply belonged to a listener

	struct PdnListener * LISTEN = NULL;
	int Id = GetInt(Msg, "id");
	json_t * Req;
	int i;

	for (i = 0; i < ListenerCount; i++)
	{
		if (Listeners[i].State >= 1 && Listeners[i].State <= 3 && Listeners[i].ReqId == Id)
			LISTEN = &Listeners[i];
	}

	if (LISTEN == NULL)
		return FALSE;

	if (strcmp(Type, "socketReply") == 0)
	{
		if (ErrCode(Msg))
		{
			ListenFailed(LISTEN, Msg, "socket");
			return TRUE;
		}
		LISTEN->Handle = GetInt(Msg, "handle");

		Req = NewRequest("bind", &LISTEN->ReqId);
		json_object_set_new(Req, "handle", json_integer(LISTEN->Handle));
		json_object_set_new(Req, "local", json_string(LISTEN->Call));
		json_object_set_new(Req, "port", json_null());		// All ports
		LISTEN->State = 2;
		RHPSend(Req);
		return TRUE;
	}

	if (strcmp(Type, "bindReply") == 0)
	{
		if (ErrCode(Msg))
		{
			ListenFailed(LISTEN, Msg, "bind");
			return TRUE;
		}

		Req = NewRequest("listen", &LISTEN->ReqId);
		json_object_set_new(Req, "handle", json_integer(LISTEN->Handle));
		json_object_set_new(Req, "flags", json_integer(0));
		LISTEN->State = 3;
		RHPSend(Req);
		return TRUE;
	}

	if (strcmp(Type, "listenReply") == 0)
	{
		if (ErrCode(Msg))
		{
			ListenFailed(LISTEN, Msg, "listen");
			return TRUE;
		}
		LISTEN->State = 4;
		PdnLog("Listening for connects to %s (RHP handle %d)", LISTEN->Call, LISTEN->Handle);
	}
	return TRUE;
}

//	UI frames go out through one RHP datagram socket, opened when first needed

static void SendUIFrame(struct PdnUI * DG)
{
	json_t * Msg;
	int Id;

	Msg = NewRequest("sendto", &Id);
	json_object_set_new(Msg, "handle", json_integer(UiHandle));
	json_object_set_new(Msg, "port", json_string(DG->Port));
	json_object_set_new(Msg, "local", json_string(DG->Local));
	json_object_set_new(Msg, "remote", json_string(DG->Remote));
	json_object_set_new(Msg, "data", BytesToJson(DG->Data, DG->Len));
	RHPSend(Msg);
}

static void QueueUIFrame(char * Port, char * Local, char * Remote, UCHAR * Data, int Len)
{
	struct PdnUI DG;

	if (RHPSock == -1)
		return;

	if (Len > 256)
		Len = 256;

	strncpy(DG.Port, Port, 31);
	DG.Port[31] = 0;
	strcpy(DG.Local, Local);
	strcpy(DG.Remote, Remote);
	memcpy(DG.Data, Data, Len);
	DG.Len = Len;

	if (UiHandle)
	{
		SendUIFrame(&DG);
		return;
	}

	if (UiQueueLen < PDN_MAXUIQ)
		UiQueue[UiQueueLen++] = DG;

	if (UiOpenId == 0)
	{
		json_t * Msg = NewRequest("open", &UiOpenId);

		json_object_set_new(Msg, "pfam", json_string("ax25"));
		json_object_set_new(Msg, "mode", json_string("dgram"));
		json_object_set_new(Msg, "local", json_string(PdnCfg.AppCall));
		json_object_set_new(Msg, "flags", json_integer(0));
		RHPSend(Msg);
	}
}

static void ProcessUIOpenReply(json_t * Msg)
{
	int i;

	UiOpenId = 0;

	if (ErrCode(Msg))
	{
		PdnLog("RHP datagram socket for UI frames failed: %d %s", ErrCode(Msg), ErrText(Msg));
		UiQueueLen = 0;
		return;
	}

	UiHandle = GetInt(Msg, "handle");

	for (i = 0; i < UiQueueLen; i++)
		SendUIFrame(&UiQueue[i]);

	UiQueueLen = 0;
}

static void ProcessOpenReply(json_t * Msg)
{
	int Id = GetInt(Msg, "id");
	int Handle = GetInt(Msg, "handle");
	int Err = ErrCode(Msg);
	int n, i;
	struct PdnStream * STREAM;

	if (Id && Id == UiOpenId)
	{
		ProcessUIOpenReply(Msg);
		return;
	}

	n = FindStreamByOpenId(Id);

	if (n < 0)
	{
		// Abandoned - the mail code disconnected while the open was in progress

		for (i = 0; i < PDN_MAXDEFER; i++)
		{
			if (Deferred[i].OpenId == Id)
			{
				Deferred[i].OpenId = 0;
				Deferred[i].Handle = 0;
			}
		}
		if (Err == 0 && Handle)
			RHPClose(Handle);
		return;
	}

	STREAM = &Streams[n];
	STREAM->OpenId = 0;

	if (Err)
	{
		const char * Text = ErrText(Msg);
		char Upper[256];

		strncpy(Upper, Text, 255);
		Upper[255] = 0;
		_strupr(Upper);

		PdnLog("Stream %d: connect to %s failed: %d %s", n + 1, STREAM->Remote, Err, Text);

		STREAM->Mode = PDN_NODECMD;

		// pdn's openReply does not say whether the far end sent DM (busy) or
		// never answered (both are errCode 15; packet.net issue #849), so
		// only an explicit "busy" becomes Busy from.

		if (strstr(Upper, "BUSY"))
			NodeReply(n, "Busy from %s\r", STREAM->Remote);
		else
			NodeReply(n, "Failure with %s\r", STREAM->Remote);

		if (STREAM->Hold)
			free(STREAM->Hold);

		STREAM->Hold = NULL;
		STREAM->HoldLen = 0;
		return;
	}

	STREAM->Mode = PDN_LINKED;
	STREAM->Handle = Handle;

	PdnLog("Stream %d: connected to %s (RHP handle %d)", n + 1, STREAM->Remote, Handle);

	NodeReply(n, "Connected to %s\r", STREAM->Remote);

	if (STREAM->HoldLen)
	{
		UCHAR * Hold = STREAM->Hold;
		int HoldLen = STREAM->HoldLen;

		STREAM->Hold = NULL;
		STREAM->HoldLen = 0;
		SendMsg(n + 1, Hold, HoldLen);
		free(Hold);
	}
}

static void ProcessAccept(json_t * Msg)
{
	int Child = GetInt(Msg, "child");
	const char * Remote = GetStr(Msg, "remote");
	const char * Local = GetStr(Msg, "local");
	const char * Port = GetStr(Msg, "port");
	char Call[16];
	int n;

	// The mail code keeps callsigns in 10 byte fields and looks users up by
	// callsign. pdn can hand us a peer that is not a callsign (a telnet
	// console user crossconnected as IP:port), so refuse those.

	strncpy(Call, Remote, 15);
	Call[15] = 0;
	_strupr(Call);

	if (!IsCallsign(Call))
	{
		json_t * Req;
		int Id;
		char Sorry[] = "Sorry, this BBS needs a callsign to log you in\r";

		PdnLog("Incoming connect from \"%s\" refused - not a callsign", Remote);

		Req = NewRequest("send", &Id);
		json_object_set_new(Req, "handle", json_integer(Child));
		json_object_set_new(Req, "data", BytesToJson(Sorry, (int)strlen(Sorry)));
		RHPSend(Req);
		AddDeferred(Child, 0, time(NULL) + 2);
		return;
	}

	// Park it until a stream is free. A stream whose last disconnect the
	// mail code has not yet seen is not free, so a connect arriving just
	// after one ends (or just after start-up) waits a tick or two.

	for (n = 0; n < PDN_MAXPENDING; n++)
	{
		struct PdnPending * PEND = &Pending[n];

		if (PEND->Child == 0)
		{
			memset(PEND, 0, sizeof(struct PdnPending));
			PEND->Child = Child;
			strcpy(PEND->Remote, Call);
			strncpy(PEND->Local, Local[0] ? Local : PdnCfg.AppCall, 9);
			strncpy(PEND->Port, Port, 31);
			PEND->Since = time(NULL);
			AttachPending();
			return;
		}
	}

	PdnLog("Incoming connect from %s refused - too many connects waiting", Call);
	RHPClose(Child);
}

static void AttachPending()
{
	int i, n;
	time_t Now = time(NULL);

	for (i = 0; i < PDN_MAXPENDING; i++)
	{
		struct PdnPending * PEND = &Pending[i];

		if (PEND->Child == 0)
			continue;

		for (n = 0; n < PDN_MAXSTREAMS; n++)
		{
			BPQVECSTRUC * SESS = &BPQHOSTVECTOR[n];

			if ((SESS->HOSTFLAGS & 0x80) && SESS->HOSTAPPLMASK && SESS->HOSTSESSION == NULL
				&& (SESS->HOSTFLAGS & 3) == 0 && Streams[n].Mode == PDN_IDLE)
			{
				struct PdnStream * STREAM = &Streams[n];

				FreeRx(STREAM);
				STREAM->Mode = PDN_LINKED;
				STREAM->Handle = PEND->Child;
				STREAM->Incoming = 1;
				STREAM->TxOutstanding = 0;
				STREAM->LastTx = 0;
				strcpy(STREAM->Remote, PEND->Remote);
				strcpy(STREAM->PortLabel, PEND->Port);

				StartSession(n, PEND->Local);

				PdnLog("Stream %d: incoming connect from %s to %s on port %s (RHP handle %d)",
					n + 1, PEND->Remote, PEND->Local, PEND->Port, PEND->Child);

				if (PEND->BufLen)
					QueueRx(n, PEND->Buf, PEND->BufLen);

				free(PEND->Buf);
				memset(PEND, 0, sizeof(struct PdnPending));
				break;
			}
		}

		if (PEND->Child && Now - PEND->Since > 5)
		{
			PdnLog("Incoming connect from %s refused - no free BBS streams", PEND->Remote);
			RHPClose(PEND->Child);
			free(PEND->Buf);
			memset(PEND, 0, sizeof(struct PdnPending));
		}
	}
}

static struct PdnPending * FindPending(int Handle)
{
	int i;

	for (i = 0; i < PDN_MAXPENDING; i++)
	{
		if (Handle && Pending[i].Child == Handle)
			return &Pending[i];
	}
	return NULL;
}

static void ProcessRecv(json_t * Msg)
{
	int Handle = GetInt(Msg, "handle");
	json_t * Data = json_object_get(Msg, "data");
	UCHAR * Bytes;
	int Len, n;

	n = FindStreamByHandle(Handle);

	if (n < 0 && json_is_string(Data))
	{
		struct PdnPending * PEND = FindPending(Handle);

		if (PEND)
		{
			int Max = (int)json_string_length(Data) + 1;

			PEND->Buf = realloc(PEND->Buf, PEND->BufLen + Max);
			PEND->BufLen += JsonToBytes(Data, &PEND->Buf[PEND->BufLen], Max);
		}
		return;
	}

	if (n < 0 || !json_is_string(Data))
		return;				// Lingering or unknown handle

	Bytes = malloc(json_string_length(Data) + 1);
	Len = JsonToBytes(Data, Bytes, (int)json_string_length(Data) + 1);
	QueueRx(n, Bytes, Len);
	free(Bytes);
}

static void ProcessClosePush(json_t * Msg)
{
	int Handle = GetInt(Msg, "handle");
	int n, i;

	for (i = 0; i < ListenerCount; i++)
	{
		if (Listeners[i].Handle && Handle == Listeners[i].Handle)
		{
			PdnLog("pdn closed the listener for %s - re-listening in 30 seconds", Listeners[i].Call);
			Listeners[i].Handle = 0;
			Listeners[i].State = 0;
			Listeners[i].RetryAt = time(NULL) + 30;
			return;
		}
	}

	if (Handle && Handle == UiHandle)
	{
		UiHandle = 0;
		return;
	}

	{
		struct PdnPending * PEND = FindPending(Handle);

		if (PEND)
		{
			free(PEND->Buf);
			memset(PEND, 0, sizeof(struct PdnPending));
			return;
		}
	}

	for (i = 0; i < PDN_MAXDEFER; i++)
	{
		if (Deferred[i].Handle == Handle)
		{
			Deferred[i].Handle = 0;
			return;
		}
	}

	n = FindStreamByHandle(Handle);

	if (n < 0)
		return;

	PdnLog("Stream %d: %s disconnected", n + 1, Streams[n].Remote);

	// Data that arrived just before the close (a message ending /EX, then an
	// immediate hangup) must reach the mail code before it hears about the
	// disconnect. The handle is gone on pdn's side; hold the session open
	// here until the mail code has read the lot (see FinishRemoteClose).

	Streams[n].Handle = 0;
	Streams[n].RemoteClosed = 1;

	if (Streams[n].RxHead == NULL)
		EndSession(n);
}

static void FinishRemoteClose()
{
	int n;

	for (n = 0; n < PDN_MAXSTREAMS; n++)
	{
		if (Streams[n].RemoteClosed && Streams[n].Mode == PDN_LINKED && Streams[n].RxHead == NULL)
			EndSession(n);
	}
}

static void ProcessSendReply(json_t * Msg)
{
	int Handle = GetInt(Msg, "handle");
	int n = FindStreamByHandle(Handle);

	if (n < 0)
		return;

	if (Streams[n].TxOutstanding > 0)
		Streams[n].TxOutstanding--;

	if (ErrCode(Msg))
	{
		PdnLog("Stream %d: send failed: %d %s", n + 1, ErrCode(Msg), ErrText(Msg));
		EndSession(n);
	}
}

static void ProcessRHPMessage(char * Text, int Len)
{
	json_error_t Error;
	json_t * Msg = json_loadb(Text, Len, JSON_ALLOW_NUL, &Error);
	const char * Type;

	if (Msg == NULL)
	{
		PdnLog("Bad JSON from RHP: %s", Error.text);
		return;
	}

	Type = GetStr(Msg, "type");

	if (PdnCfg.Trace)
		printf("linmail-pdn: RHP < %s\n", Text);

	if (strcmp(Type, "recv") == 0)
		ProcessRecv(Msg);
	else if (strcmp(Type, "accept") == 0)
		ProcessAccept(Msg);
	else if (strcmp(Type, "openReply") == 0)
		ProcessOpenReply(Msg);
	else if (strcmp(Type, "close") == 0)
		ProcessClosePush(Msg);
	else if (strcmp(Type, "sendReply") == 0)
		ProcessSendReply(Msg);
	else if (strcmp(Type, "socketReply") == 0 || strcmp(Type, "bindReply") == 0 || strcmp(Type, "listenReply") == 0)
	{
		if (!ProcessListenReply((char *)Type, Msg))
			PdnLog("Unexpected RHP message: %s", Text);
	}
	else if (strcmp(Type, "authReply") == 0)
	{
		if (ErrCode(Msg))
			PdnLog("RHP auth failed: %d %s", ErrCode(Msg), ErrText(Msg));
	}
	else if (strcmp(Type, "sendtoReply") == 0)
	{
		if (ErrCode(Msg))
			PdnLog("RHP UI frame refused: %d %s", ErrCode(Msg), ErrText(Msg));
	}
	else if (strcmp(Type, "status") == 0 || strcmp(Type, "closeReply") == 0)
	{
		// Nothing to do
	}
	else
		PdnLog("Unexpected RHP message: %s", Text);

	json_decref(Msg);
}

//	RHP connection management

static void RHPDown(char * Why)
{
	int n, i;

	if (RHPSock != -1)
		close(RHPSock);

	RHPSock = -1;
	RxFrameLen = 0;
	RHPRetryAt = time(NULL) + 5;

	for (i = 0; i < ListenerCount; i++)
	{
		Listeners[i].Handle = 0;
		Listeners[i].State = 0;
		Listeners[i].RetryAt = 0;
	}

	UiHandle = 0;
	UiOpenId = 0;
	UiQueueLen = 0;

	for (i = 0; i < PDN_MAXPENDING; i++)
	{
		free(Pending[i].Buf);
		memset(&Pending[i], 0, sizeof(struct PdnPending));
	}

	for (i = 0; i < PDN_MAXDEFER; i++)
	{
		Deferred[i].Handle = 0;
		Deferred[i].OpenId = 0;
	}

	// All handles die with the RHP connection

	for (n = 0; n < PDN_MAXSTREAMS; n++)
	{
		if (Streams[n].Mode == PDN_LINKED || Streams[n].Mode == PDN_OPENING)
			EndSession(n);
	}

	if (RHPWasUp)
		PdnLog("Lost RHP connection to pdn at %s:%d (%s) - retrying", PdnCfg.RHPHost, PdnCfg.RHPPort, Why);

	RHPWasUp = 0;
}

static void RHPConnect()
{
	struct addrinfo Hints = {0}, * Result = NULL;
	char Port[16];
	int Sock, One = 1, i;
	static int Reported = 0;

	Hints.ai_family = AF_UNSPEC;
	Hints.ai_socktype = SOCK_STREAM;
	sprintf(Port, "%d", PdnCfg.RHPPort);

	RHPRetryAt = time(NULL) + 5;

	if (getaddrinfo(PdnCfg.RHPHost, Port, &Hints, &Result) != 0 || Result == NULL)
	{
		if (!Reported++)
			PdnLog("Cannot resolve RHP host %s", PdnCfg.RHPHost);
		return;
	}

	Sock = socket(Result->ai_family, SOCK_STREAM, 0);

	if (Sock == -1 || connect(Sock, Result->ai_addr, Result->ai_addrlen) != 0)
	{
		if (!Reported++)
			PdnLog("Cannot connect to pdn RHP at %s:%d (%s) - will keep trying", PdnCfg.RHPHost, PdnCfg.RHPPort, strerror(errno));
		if (Sock != -1)
			close(Sock);
		freeaddrinfo(Result);
		return;
	}

	freeaddrinfo(Result);
	setsockopt(Sock, IPPROTO_TCP, TCP_NODELAY, &One, sizeof(One));

	RHPSock = Sock;
	RHPWasUp = 1;
	Reported = 0;
	RxFrameLen = 0;

	PdnLog("Connected to pdn RHP at %s:%d", PdnCfg.RHPHost, PdnCfg.RHPPort);

	if (PdnCfg.RHPUser[0])
	{
		int Id;
		json_t * Msg = NewRequest("auth", &Id);

		json_object_set_new(Msg, "user", json_string(PdnCfg.RHPUser));
		json_object_set_new(Msg, "pass", json_string(PdnCfg.RHPPass));
		RHPSend(Msg);
	}

	for (i = 0; i < ListenerCount; i++)
		StartListen(&Listeners[i]);
}

static void RHPRead()
{
	int Len = recv(RHPSock, &RxFrame[RxFrameLen], sizeof(RxFrame) - RxFrameLen - 1, 0);

	if (Len == 0)
	{
		RHPDown("closed by pdn");
		return;
	}

	if (Len < 0)
	{
		if (errno != EINTR && errno != EAGAIN)
			RHPDown(strerror(errno));
		return;
	}

	RxFrameLen += Len;

	while (RxFrameLen >= 2)
	{
		int FrameLen = (RxFrame[0] << 8) | RxFrame[1];
		char Save;

		if (RxFrameLen < FrameLen + 2)
			break;

		Save = RxFrame[FrameLen + 2];
		RxFrame[FrameLen + 2] = 0;
		ProcessRHPMessage((char *)&RxFrame[2], FrameLen);
		RxFrame[FrameLen + 2] = Save;

		if (RHPSock == -1)
			return;

		RxFrameLen -= FrameLen + 2;
		memmove(RxFrame, &RxFrame[FrameLen + 2], RxFrameLen);
	}
}

static void CheckIdle(time_t Now)
{
	// BPQ drops a session that has been idle for longer than its idle time
	// (L4LIMIT). Do the same, through the normal disconnect path.

	int n;

	for (n = 0; n < PDN_MAXSTREAMS; n++)
	{
		struct PdnStream * STREAM = &Streams[n];

		if (BPQHOSTVECTOR[n].HOSTSESSION && STREAM->IdleTime > 0
			&& (STREAM->Mode == PDN_NODECMD || STREAM->Mode == PDN_LINKED)
			&& Now - STREAM->LastActivity > STREAM->IdleTime)
		{
			PdnLog("Stream %d: %s idle for %d seconds - disconnecting", n + 1, STREAM->Remote, STREAM->IdleTime);
			SessionControl(n + 1, 2, 0);
		}
	}
}

static void AddListener(char * Call)
{
	char Copy[16];

	strncpy(Copy, Call, 15);
	Copy[15] = 0;
	strlop(Copy, ' ');
	_strupr(Copy);

	if (Copy[0] == 0 || strlen(Copy) > 9 || ListenerCount >= PDN_MAXLISTEN)
		return;

	memset(&Listeners[ListenerCount], 0, sizeof(struct PdnListener));
	strcpy(Listeners[ListenerCount].Call, Copy);
	ListenerCount++;
}

void PdnHostInit()
{
	char * Aliases, * Context, * Alias;

	pthread_mutexattr_t Attr;

	pthread_mutexattr_init(&Attr);
	pthread_mutexattr_settype(&Attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&PdnLock, &Attr);

	memset(Streams, 0, sizeof(Streams));
	memset(BPQHOSTVECTOR, 0, sizeof(BPQHOSTVECTOR));

	AddListener(PdnCfg.AppCall);

	Aliases = _strdup(PdnCfg.Aliases);
	Alias = strtok_s(Aliases, ", ", &Context);

	while (Alias)
	{
		AddListener(Alias);
		Alias = strtok_s(NULL, ", ", &Context);
	}
	free(Aliases);

	pthread_mutex_lock(&PdnLock);
	RHPConnect();
	pthread_mutex_unlock(&PdnLock);
}

void PdnHostPoll(int WaitMs)
{
	fd_set rfds;
	struct timeval tv;
	time_t Now;
	int i, Sock;

	pthread_mutex_lock(&PdnLock);

	Now = time(NULL);

	if (RHPSock == -1 && Now >= RHPRetryAt)
		RHPConnect();

	for (i = 0; RHPSock != -1 && i < ListenerCount; i++)
	{
		if (Listeners[i].State == 0 && Listeners[i].RetryAt && Now >= Listeners[i].RetryAt)
			StartListen(&Listeners[i]);
	}

	CheckIdle(Now);
	AttachPending();
	FinishRemoteClose();

	// Close handles whose linger has expired

	for (i = 0; i < PDN_MAXDEFER; i++)
	{
		if (Deferred[i].Handle && Deferred[i].OpenId == 0 && Now >= Deferred[i].CloseAt)
		{
			RHPClose(Deferred[i].Handle);
			Deferred[i].Handle = 0;
		}
	}

	Sock = RHPSock;
	pthread_mutex_unlock(&PdnLock);

	if (Sock == -1)
	{
		Sleep(WaitMs);
		return;
	}

	FD_ZERO(&rfds);
	FD_SET(Sock, &rfds);
	tv.tv_sec = WaitMs / 1000;
	tv.tv_usec = (WaitMs % 1000) * 1000;

	if (select(Sock + 1, &rfds, NULL, NULL, &tv) > 0)
	{
		pthread_mutex_lock(&PdnLock);

		if (RHPSock == Sock)
			RHPRead();

		pthread_mutex_unlock(&PdnLock);
	}
}

void PdnHostClose()
{
	int n;

	pthread_mutex_lock(&PdnLock);

	for (n = 0; n < PDN_MAXSTREAMS; n++)
	{
		if (Streams[n].Mode == PDN_LINKED)
			RHPClose(Streams[n].Handle);
	}

	for (n = 0; n < ListenerCount; n++)
	{
		if (Listeners[n].Handle)
			RHPClose(Listeners[n].Handle);
	}

	if (UiHandle)
		RHPClose(UiHandle);

	if (RHPSock != -1)
		close(RHPSock);

	RHPSock = -1;
	pthread_mutex_unlock(&PdnLock);
}

//	The BPQ host API, as used by the mail code. Streams are numbered 1 - 64.

DllExport int APIENTRY FindFreeStream()
{
	int n;

	pthread_mutex_lock(&PdnLock);

	for (n = 0; n < PDN_MAXSTREAMS; n++)
	{
		if ((BPQHOSTVECTOR[n].HOSTFLAGS & 0x80) == 0)
		{
			BPQHOSTVECTOR[n].HOSTFLAGS = 0x80;		// Allocated
			BPQHOSTVECTOR[n].HOSTSTREAM = n + 1;
			pthread_mutex_unlock(&PdnLock);
			return n + 1;
		}
	}

	pthread_mutex_unlock(&PdnLock);
	return 255;
}

DllExport int APIENTRY DeallocateStream(int stream)
{
	stream--;

	if (stream < 0 || stream >= PDN_MAXSTREAMS)
		return 0;

	if (BPQHOSTVECTOR[stream].HOSTSESSION)
		SessionControl(stream + 1, 2, 0);

	pthread_mutex_lock(&PdnLock);
	BPQHOSTVECTOR[stream].HOSTAPPLFLAGS = 0;
	BPQHOSTVECTOR[stream].HOSTAPPLMASK = 0;
	BPQHOSTVECTOR[stream].HOSTFLAGS &= 0x60;
	pthread_mutex_unlock(&PdnLock);
	return 0;
}

DllExport int APIENTRY SetAppl(int stream, int flags, int mask)
{
	int Appl = 0;

	stream--;

	if (stream < 0 || stream >= PDN_MAXSTREAMS)
		return 0;

	BPQHOSTVECTOR[stream].HOSTAPPLFLAGS = flags;
	BPQHOSTVECTOR[stream].HOSTAPPLMASK = mask;

	while (mask && (mask & 1) == 0)
	{
		mask >>= 1;
		Appl++;
	}

	BPQHOSTVECTOR[stream].HOSTAPPLNUM = mask ? Appl + 1 : 0;

	if (flags || BPQHOSTVECTOR[stream].HOSTAPPLMASK)
		BPQHOSTVECTOR[stream].HOSTFLAGS |= 0x80;

	return 0;
}

DllExport int APIENTRY SessionControl(int stream, int command, int Mask)
{
	//	CL=0 CONNECT USING APPL MASK IN DL
	//	CL=1, CONNECT. CL=2 - DISCONNECT. CL=3 RETURN TO NODE

	struct PdnStream * STREAM;
	BPQVECSTRUC * SESS;
	int n = stream - 1;

	if (n < 0 || n >= PDN_MAXSTREAMS)
		return 0;

	pthread_mutex_lock(&PdnLock);

	SESS = &BPQHOSTVECTOR[n];
	STREAM = &Streams[n];

	if (command > 1)
	{
		// Disconnect

		if (SESS->HOSTSESSION == NULL)
		{
			SESS->HOSTFLAGS |= 1;
			pthread_mutex_unlock(&PdnLock);
			return 0;
		}

		if (STREAM->Mode == PDN_LINKED && STREAM->Handle)
		{
			// Give pdn time to get the last of our data on air before the
			// close. pdn discards unsent data when a link is closed.

			time_t CloseAt = STREAM->LastTx + PdnCfg.DiscLinger;

			if (CloseAt <= time(NULL))
				RHPClose(STREAM->Handle);
			else
				AddDeferred(STREAM->Handle, 0, CloseAt);
		}
		else if (STREAM->Mode == PDN_OPENING)
			AddDeferred(0, STREAM->OpenId, 0);

		EndSession(n);
		pthread_mutex_unlock(&PdnLock);
		return 0;
	}

	// Connect - to the (emulated) node command handler

	if (SESS->HOSTSESSION)
	{
		SESS->HOSTFLAGS |= 1;
		pthread_mutex_unlock(&PdnLock);
		return 0;
	}

	SESS->HOSTFLAGS |= 0x80;

	FreeRx(STREAM);
	STREAM->Mode = PDN_NODECMD;
	STREAM->Incoming = 0;
	STREAM->CmdLen = 0;
	STREAM->Handle = 0;
	STREAM->TxOutstanding = 0;
	STREAM->LastTx = 0;
	strcpy(STREAM->Remote, "SWITCH");

	StartSession(n, PdnCfg.AppCall);

	pthread_mutex_unlock(&PdnLock);
	return 0;
}

DllExport int APIENTRY SessionState(int stream, int * state, int * change)
{
	BPQVECSTRUC * HOST;

	*state = *change = 0;

	if (stream < 1 || stream > PDN_MAXSTREAMS)
		return 0;

	pthread_mutex_lock(&PdnLock);

	HOST = &BPQHOSTVECTOR[stream - 1];

	*change = (HOST->HOSTFLAGS & 3) ? 1 : 0;
	*state = HOST->HOSTSESSION ? 1 : 0;

	HOST->HOSTFLAGS &= 0xFC;

	pthread_mutex_unlock(&PdnLock);
	return 0;
}

DllExport int APIENTRY SessionStateNoAck(int stream, int * state)
{
	*state = 0;

	if (stream < 1 || stream > PDN_MAXSTREAMS)
		return 0;

	*state = BPQHOSTVECTOR[stream - 1].HOSTSESSION ? 1 : 0;
	return 0;
}

DllExport int APIENTRY GetMsg(int stream, char * msg, int * len, int * count)
{
	struct PdnStream * STREAM;
	struct PdnBuf * Buf;
	int n = stream - 1;

	*len = 0;
	*count = 0;

	if (n < 0 || n >= PDN_MAXSTREAMS)
		return 0;

	pthread_mutex_lock(&PdnLock);

	STREAM = &Streams[n];

	// Hold data back until the mail code has seen the connect

	if (BPQHOSTVECTOR[n].HOSTSESSION == NULL || (BPQHOSTVECTOR[n].HOSTFLAGS & 3) || STREAM->RxHead == NULL)
	{
		pthread_mutex_unlock(&PdnLock);
		return 0;
	}

	Buf = STREAM->RxHead;
	STREAM->RxHead = Buf->Next;

	if (STREAM->RxHead == NULL)
		STREAM->RxTail = NULL;

	STREAM->RxCount--;

	memcpy(msg, Buf->Data, Buf->Len);
	*len = Buf->Len;
	*count = STREAM->RxCount;

	free(Buf);
	pthread_mutex_unlock(&PdnLock);
	return 0;
}

DllExport int APIENTRY RXCount(int stream)
{
	if (stream < 1 || stream > PDN_MAXSTREAMS)
		return 0;

	return Streams[stream - 1].RxCount;
}

DllExport int APIENTRY SendMsg(int stream, char * msg, int len)
{
	struct PdnStream * STREAM;
	json_t * Msg;
	int n = stream - 1;
	int Id;

	if (n < 0 || n >= PDN_MAXSTREAMS || len <= 0)
		return 0;

	pthread_mutex_lock(&PdnLock);

	STREAM = &Streams[n];
	STREAM->LastActivity = time(NULL);

	switch (STREAM->Mode)
	{
	case PDN_NODECMD:

		NodeInput(n, msg, len);
		break;

	case PDN_OPENING:

		STREAM->Hold = realloc(STREAM->Hold, STREAM->HoldLen + len);
		memcpy(&STREAM->Hold[STREAM->HoldLen], msg, len);
		STREAM->HoldLen += len;
		break;

	case PDN_LINKED:

		if (STREAM->RemoteClosed)
			break;					// Far end has gone; nothing to send it to

		Msg = NewRequest("send", &Id);
		json_object_set_new(Msg, "handle", json_integer(STREAM->Handle));
		json_object_set_new(Msg, "data", BytesToJson(msg, len));

		STREAM->TxOutstanding++;
		STREAM->LastTx = time(NULL);
		RHPSend(Msg);
		break;
	}

	pthread_mutex_unlock(&PdnLock);
	return 0;
}

DllExport int APIENTRY TXCount(int stream)
{
	if (stream < 1 || stream > PDN_MAXSTREAMS)
		return 0;

	return Streams[stream - 1].TxOutstanding;
}

DllExport int APIENTRY GetConnectionInfo(int stream, char * callsign,
	int * port, int * sesstype, int * paclen, int * maxframe, int * l4window)
{
	struct PdnStream * STREAM;
	char Call[32];
	int n = stream - 1;

	*port = 0;
	*sesstype = 0;
	*paclen = 0;
	*maxframe = 0;
	*l4window = 0;

	memcpy(callsign, "SWITCH    ", 10);

	if (n < 0 || n >= PDN_MAXSTREAMS || BPQHOSTVECTOR[n].HOSTSESSION == NULL)
		return 0;

	STREAM = &Streams[n];

	sprintf(Call, "%-10s", STREAM->Remote);
	memcpy(callsign, Call, 10);

	*port = PdnPortNumber(STREAM->PortLabel);
	*sesstype = STREAM->Incoming ? (Sess_L2LINK | Sess_UPLINK) : Sess_L2LINK;
	*paclen = 0;				// Let the mail code choose (236)

	return 0;					// Never a secure (console) session
}

DllExport int APIENTRY ChangeSessionIdletime(int Stream, int idletime)
{
	if (Stream >= 1 && Stream <= PDN_MAXSTREAMS)
		Streams[Stream - 1].IdleTime = idletime;

	return 0;
}

DllExport int APIENTRY ChangeSessionCallsign(int Stream, unsigned char * AXCall)
{
	if (Stream >= 1 && Stream <= PDN_MAXSTREAMS)
		memcpy(Sessions[Stream - 1].L4USER, AXCall, 7);

	return 0;
}

DllExport int APIENTRY GetApplNum(int Stream)
{
	if (Stream < 1 || Stream > PDN_MAXSTREAMS)
		return 0;

	return BPQHOSTVECTOR[Stream - 1].HOSTAPPLNUM;
}

DllExport char * APIENTRY GetApplCall(int Appl)
{
	static char Call[11];

	sprintf(Call, "%-10s", PdnCfg.AppCall);
	return Call;
}

//	Ports. The mail code only uses these for UI config and for display.

DllExport int APIENTRY GetNumberofPorts()
{
	return PdnCfg.PortCount;
}

DllExport int APIENTRY GetPortNumber(int portslot)
{
	if (portslot < 1 || portslot > PdnCfg.PortCount)
		return 0;

	return PdnCfg.PortNum[portslot - 1];
}

DllExport UCHAR * APIENTRY GetPortDescription(int portslot, char * Desc)
{
	if (portslot < 1 || portslot > PdnCfg.PortCount)
		strcpy(Desc, "");
	else
		sprintf(Desc, "pdn port %s", PdnCfg.PortLabel[portslot - 1]);

	return Desc;
}

struct PORTCONTROL * APIENTRY GetPortTableEntryFromSlot(int portslot)
{
	return NULL;
}

int GetPortHardwareType(struct PORTCONTROL * PORT)
{
	return 0;
}

DllExport uint64_t APIENTRY GetPortFrequency(int PortNo, char * FreqString)
{
	FreqString[0] = 0;
	return 0;
}

DllExport int APIENTRY SendRaw(int port, char * msg, int len)
{
	// A raw AX.25 frame from UIRoutines.c (mail-for beacons, FBB message
	// header broadcasts): DEST, ORIGIN, any digis, CTL, PID, info. Sent as an
	// RHP datagram. RHP has no digipeater path, so any digis are dropped.

	UCHAR * Frame = (UCHAR *)msg;
	char Dest[16], Origin[16];
	char * Label = NULL;
	int Pos = 14, i;
	static int DigiWarned = 0;

	if (len < 16)
		return 0;

	if ((Frame[13] & 1) == 0)
	{
		// Skip digis

		while (Pos + 7 <= len && (Frame[Pos + 6] & 1) == 0)
			Pos += 7;

		Pos += 7;

		if (!DigiWarned++)
			PdnLog("UI frames are sent without their digipeater path (not supported over RHP)");
	}

	if (Pos + 2 > len || Frame[Pos] != 3 || Frame[Pos + 1] != 0xF0)
		return 0;						// Only plain UI frames

	for (i = 0; i < PdnCfg.PortCount; i++)
	{
		if (PdnCfg.PortNum[i] == port)
			Label = PdnCfg.PortLabel[i];
	}

	if (Label == NULL && port >= 1 && port <= PdnCfg.PortCount)
		Label = PdnCfg.PortLabel[port - 1];		// Port slot

	if (Label == NULL)
		return 0;

	ConvFromAX25(Frame, Dest);
	strlop(Dest, ' ');
	ConvFromAX25(&Frame[7], Origin);
	strlop(Origin, ' ');

	pthread_mutex_lock(&PdnLock);
	QueueUIFrame(Label, Origin, Dest, &Frame[Pos + 2], len - Pos - 2);
	pthread_mutex_unlock(&PdnLock);

	return 0;
}

int APIENTRY GetRaw(int stream, char * msg, int * len, int * count)
{
	// No monitor feed

	*len = 0;
	*count = 0;
	return 0;
}

//	AX.25 address helpers (CommonCode.c in the node)

DllExport int ConvFromAX25(unsigned char * incall, unsigned char * outcall)
{
	int in, out = 0;
	unsigned char chr;

	memset(outcall, 0x20, 10);

	for (in = 0; in < 6; in++)
	{
		chr = incall[in];
		if (chr == 0x40)
			break;
		chr >>= 1;
		outcall[out++] = chr;
	}

	chr = incall[6];
	chr >>= 1;
	chr &= 15;

	if (chr > 0)
	{
		outcall[out++] = '-';
		if (chr > 9)
		{
			chr -= 10;
			outcall[out++] = '1';
		}
		chr += 48;
		outcall[out++] = chr;
	}
	return out;
}

DllExport BOOL ConvToAX25(unsigned char * callsign, unsigned char * ax25call)
{
	int i;

	memset(ax25call, 0x40, 6);
	ax25call[6] = 0x60;

	for (i = 0; i < 7; i++)
	{
		if (callsign[i] == '-')
		{
			ax25call[6] |= atoi(&callsign[i + 1]) << 1;
			return TRUE;
		}

		if (callsign[i] == 0 || callsign[i] == 13 || callsign[i] == ' ' || callsign[i] == ',')
			return TRUE;

		if (i == 6)
			return FALSE;

		ax25call[i] = toupper(callsign[i]) << 1;
	}
	return TRUE;
}
