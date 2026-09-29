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

//	linmail-pdn web front end
//
//	A small loopback HTTP server that serves webmail and the BBS management
//	pages through pdn's app gateway (packet.net docs/app-gateway.md). It does
//	what HTTPcode.c does for these pages in LinBPQ, and calls the same upstream
//	handler, ProcessMailHTTPMessage, but replaces BPQ's logins with the
//	gateway's identity:
//
//	- Every request must carry X-Pdn-Gateway: 1. The listener binds loopback
//	  only, and pdn strips client copies of the X-Pdn-* headers, so these can
//	  be trusted. Anything else is refused.
//	- X-Pdn-User names the viewer. A callsign username maps to that BBS user.
//	  A node admin whose username is not a callsign maps to the BBS sysop.
//	- The management pages (/Mail/...) and any BBS account flagged sysop need
//	  X-Pdn-Scope: admin.
//	- pdn mounts the app under X-Forwarded-Prefix (/apps/linmail). The mail
//	  pages use root-relative URLs (/WebMail/..., /Mail/...), so those are
//	  rewritten in every text reply.
//
//	Requests are handled on the main thread from the mail timer loop, like
//	everything else that touches the mail data.

#define MAIL
#include "bpqmail.h"
#include "httpconnectioninfo.h"
#include "pdnhost.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define WEB_MAXCONN 16
#define WEB_MAXREQUEST (4 * 1024 * 1024)
#define WEB_REPLYSIZE 250000			// As HTTPcode.c

void ProcessMailHTTPMessage(struct HTTPConnectionInfo * Session, char * Method, char * URL, char * input, char * Reply, int * RLen, int InputLen, char * Token);
struct HTTPConnectionInfo * AllocateWebMailSession();
struct HTTPConnectionInfo * FindWMSession(char * Key);

extern char BBSName[];
extern char SYSOPCall[];

struct WebConn
{
	int Sock;
	time_t Started;
	char * Buf;
	int Len;
	int Size;
};

static struct WebConn Conns[WEB_MAXCONN];
static int ListenSock = -1;

// Sessions for the management pages. HTTPcode.c keeps these in the node; here
// each is tied to the pdn user it was made for.

#define WEB_MAXMAILSESS 16

struct MailWebSession
{
	struct HTTPConnectionInfo Info;
	char Owner[64];
	time_t LastUsed;
	int InUse;
};

static struct MailWebSession MailSessions[WEB_MAXMAILSESS];

static char TemplateDirs[4][300];
static int TemplateDirCount = 0;

static void WebLog(const char * format, ...)
{
	char Mess[512];
	va_list(arglist);

	va_start(arglist, format);
	vsnprintf(Mess, sizeof(Mess), format, arglist);
	va_end(arglist);

	printf("linmail-pdn: web: %s\n", Mess);
}

//	HTML templates. LinBPQ reads them from $BPQDirectory/HTML. Keep that (so a
//	sysop's edited copies still win), then fall back to the ones installed
//	with linmail-pdn.

void PdnWebSetTemplateDirs(char * Configured, char * ExeDir)
{
	TemplateDirCount = 0;

	snprintf(TemplateDirs[TemplateDirCount++], 300, "%s/HTML", BPQDirectory);

	if (Configured && Configured[0])
		snprintf(TemplateDirs[TemplateDirCount++], 300, "%s", Configured);

	if (ExeDir && ExeDir[0])
	{
		snprintf(TemplateDirs[TemplateDirCount++], 300, "%s/HTML", ExeDir);
		snprintf(TemplateDirs[TemplateDirCount++], 300, "%s/../HTML", ExeDir);	// Running from a source tree
	}
}

static char * FindTemplate(char * FN, char * Path, int Max)
{
	struct stat STAT;
	int i;

	if (strstr(FN, "..") || strchr(FN, '/'))
		return NULL;

	for (i = 0; i < TemplateDirCount; i++)
	{
		snprintf(Path, Max, "%s/%s", TemplateDirs[i], FN);

		if (stat(Path, &STAT) == 0)
			return Path;
	}

	return NULL;
}

static char * ReadWholeFile(char * Path, int * Len)
{
	struct stat STAT;
	FILE * Handle;
	char * Data;

	if (stat(Path, &STAT) == -1)
		return NULL;

	Handle = fopen(Path, "rb");

	if (Handle == NULL)
		return NULL;

	Data = malloc(STAT.st_size + 1);
	*Len = (int)fread(Data, 1, STAT.st_size, Handle);
	Data[*Len] = 0;
	fclose(Handle);
	return Data;
}

char * GetTemplateFromFile(int Version, char * FN)
{
	// Same contract as HTMLCommonCode.c

	char Path[400];
	char * MsgBytes;
	int Len;

	if (FindTemplate(FN, Path, sizeof(Path)) == NULL || (MsgBytes = ReadWholeFile(Path, &Len)) == NULL)
		return _strdup("File is missing");

	if (Version)
	{
		int PageVersion = 0;

		if (memcmp(MsgBytes, "<!-- Version", 12) == 0)
			PageVersion = atoi(&MsgBytes[13]);

		if (Version != PageVersion)
		{
			free(MsgBytes);
			MsgBytes = malloc(256);
			sprintf(MsgBytes, "Wrong Version of HTML Page %s - is %d should be %d. Please update", FN, PageVersion, Version);
		}
	}
	return MsgBytes;
}

//	URL rewriting for the gateway prefix

static int RootLink(const char * In, int i, int Len)
{
	// In[i] is a '/' that follows a quote, '=' or '('. Is it the start of a
	// root-relative URL into our pages?

	static const char * Roots[] = {"webmail", "mail/", "background.jpg", "favicon.ico", NULL};
	const char * p = &In[i + 1];
	int Left = Len - i - 1;
	int j;

	for (j = 0; Roots[j]; j++)
	{
		int n = (int)strlen(Roots[j]);

		if (Left >= n && _memicmp((char *)p, (char *)Roots[j], n) == 0)
			return TRUE;
	}

	// The site root itself ("/" alone), as in href=/> or location = '/'. Only
	// after an '=' or '(' (possibly quoted), so a self-closing "/> is left alone.

	if (Left <= 0 || p[0] == '"' || p[0] == '\'' || p[0] == '>' || p[0] == ' ' || p[0] == ')')
	{
		j = i - 1;

		if (In[j] == '"' || In[j] == '\'')
		{
			j--;
			while (j >= 0 && In[j] == ' ')
				j--;
		}
		return j >= 0 && (In[j] == '=' || In[j] == '(');
	}
	return FALSE;
}

static char * Rewrite(const char * In, int Len, const char * Prefix, int * OutLen)
{
	int PLen = (int)strlen(Prefix);
	char * Out = malloc(Len * 2 + 64 + (PLen ? Len / 4 * PLen : 0));
	int i, n = 0, Cap = Len * 2 + 64 + (PLen ? Len / 4 * PLen : 0);

	for (i = 0; i < Len; i++)
	{
		if (PLen && In[i] == '/' && i > 0 && strchr("\"'=(", In[i - 1]) && In[i - 1] != 0
			&& (i + 1 >= Len || In[i + 1] != '/') && RootLink(In, i, Len))
		{
			if (n + PLen + 1 >= Cap)
			{
				Cap = Cap * 2 + PLen;
				Out = realloc(Out, Cap);
			}
			memcpy(&Out[n], Prefix, PLen);
			n += PLen;
		}

		if (n + 2 >= Cap)
		{
			Cap *= 2;
			Out = realloc(Out, Cap);
		}
		Out[n++] = In[i];
	}
	*OutLen = n;
	return Out;
}

//	Sending

static void SendAll(int Sock, const char * Data, int Len)
{
	int Sent = 0, Ret;

	while (Sent < Len)
	{
		Ret = send(Sock, &Data[Sent], Len - Sent, MSG_NOSIGNAL);

		if (Ret < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
			{
				fd_set wfds;
				struct timeval tv = {2, 0};

				FD_ZERO(&wfds);
				FD_SET(Sock, &wfds);

				if (select(Sock + 1, NULL, &wfds, NULL, &tv) <= 0)
					return;
				continue;
			}
			return;
		}
		Sent += Ret;
	}
}

static void SendReply(int Sock, int Status, const char * Type, const char * Body, int Len, const char * Extra)
{
	char Header[512];
	int HLen;
	const char * Text = Status == 200 ? "OK" : Status == 302 ? "Found" : Status == 403 ? "Forbidden" : Status == 404 ? "Not Found" : "Error";

	HLen = snprintf(Header, sizeof(Header), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
		"Cache-Control: no-store\r\nConnection: close\r\n%s\r\n", Status, Text, Type, Len, Extra ? Extra : "");

	SendAll(Sock, Header, HLen);
	SendAll(Sock, Body, Len);
}

static void SendPage(int Sock, int Status, const char * Title, const char * Format, ...)
{
	char Body[4096], Inner[3000];
	int Len;
	va_list(arglist);

	va_start(arglist, Format);
	vsnprintf(Inner, sizeof(Inner), Format, arglist);
	va_end(arglist);

	Len = snprintf(Body, sizeof(Body), "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title>"
		"<style>body{font-family:sans-serif;margin:2em;max-width:40em}a{margin-right:1.5em}</style></head>"
		"<body><h2>%s</h2>%s</body></html>", Title, Title, Inner);

	SendReply(Sock, Status, "text/html; charset=utf-8", Body, Len, NULL);
}

static void SendMailReply(int Sock, char * Reply, int Len, const char * Prefix)
{
	// A reply from the mail code: either a complete HTTP response, or an HTML
	// body that HTTPcode.c would wrap (adding its closing tags).

	char * Body;
	int BodyLen;

	if (Len >= 4 && memcmp(Reply, "HTTP", 4) == 0)
	{
		char * End = strstr(Reply, "\r\n\r\n");
		char Headers[2048];
		int HLen, Text;
		char * Line, * Context;
		char NewHeaders[2400] = "";
		int NLen = 0;
		char Status[128] = "HTTP/1.1 200 OK";

		if (End == NULL || End - Reply > (int)sizeof(Headers) - 1)
		{
			SendAll(Sock, Reply, Len);
			return;
		}

		HLen = (int)(End - Reply);
		memcpy(Headers, Reply, HLen);
		Headers[HLen] = 0;

		Text = strstr(Headers, "text/") || strstr(Headers, "javascript") || !strstr(Headers, "Content-Type");

		Body = End + 4;
		BodyLen = Len - HLen - 4;

		if (Text)
			Body = Rewrite(Body, BodyLen, Prefix, &BodyLen);

		// Rebuild the headers: fix Content-Length, prefix Location

		Line = strtok_s(Headers, "\r\n", &Context);

		if (Line)
			strncpy(Status, Line, sizeof(Status) - 1);

		while ((Line = strtok_s(NULL, "\r\n", &Context)))
		{
			if (_memicmp(Line, "Content-Length:", 15) == 0 || _memicmp(Line, "Connection:", 11) == 0)
				continue;

			if (_memicmp(Line, "Location: /", 11) == 0)
				NLen += snprintf(&NewHeaders[NLen], sizeof(NewHeaders) - NLen, "Location: %s%s\r\n", Prefix, &Line[10]);
			else
				NLen += snprintf(&NewHeaders[NLen], sizeof(NewHeaders) - NLen, "%s\r\n", Line);

			if (NLen >= (int)sizeof(NewHeaders) - 1)
				break;
		}

		{
			char Head[3000];
			int n = snprintf(Head, sizeof(Head), "%s\r\n%sContent-Length: %d\r\nConnection: close\r\n\r\n", Status, NewHeaders, BodyLen);
			SendAll(Sock, Head, n);
			SendAll(Sock, Body, BodyLen);
		}

		if (Text)
			free(Body);
		return;
	}

	Reply[Len] = 0;
	strcpy(&Reply[Len], "</body></html>");
	Len += 14;

	Body = Rewrite(Reply, Len, Prefix, &BodyLen);
	SendReply(Sock, 200, "text/html", Body, BodyLen, NULL);
	free(Body);
}

//	Requests

static char * GetHeader(char * Request, char * Name, char * Value, int Max)
{
	// Case-insensitive header lookup in the raw request

	int NLen = (int)strlen(Name);
	char * Line = strstr(Request, "\r\n");
	char * End = strstr(Request, "\r\n\r\n");

	Value[0] = 0;

	while (Line && End && Line < End)
	{
		Line += 2;

		if (_memicmp(Line, Name, NLen) == 0 && Line[NLen] == ':')
		{
			char * v = &Line[NLen + 1];
			int n = 0;

			while (*v == ' ' || *v == '\t')
				v++;

			while (v[n] && v[n] != '\r' && v[n] != '\n' && n < Max - 1)
			{
				Value[n] = v[n];
				n++;
			}
			Value[n] = 0;

			while (n > 0 && (Value[n - 1] == ' ' || Value[n - 1] == '\t'))
				Value[--n] = 0;

			return Value;
		}
		Line = strstr(Line, "\r\n");
	}
	return NULL;
}

static int CallsignLike(char * Call)
{
	// 3 to 6 letters and digits with at least one digit, optional -SSID

	char * ssid = strchr(Call, '-');
	int len = ssid ? (int)(ssid - Call) : (int)strlen(Call);
	int i, digits = 0;

	if (len < 3 || len > 6)
		return FALSE;

	for (i = 0; i < len; i++)
	{
		if (!isalnum((unsigned char)Call[i]))
			return FALSE;
		if (isdigit((unsigned char)Call[i]))
			digits++;
	}

	if (ssid)
	{
		int s = atoi(ssid + 1);

		if (ssid[1] == 0 || strlen(ssid + 1) > 2 || s < 0 || s > 15)
			return FALSE;

		for (i = 1; ssid[i]; i++)
			if (!isdigit((unsigned char)ssid[i]))
				return FALSE;
	}
	return digits > 0;
}

static struct UserInfo * MapUser(char * PdnUser, int Admin, char * Why, int WhyLen)
{
	// The pdn username to the BBS user it acts as

	char Call[64];
	struct UserInfo * User;

	strncpy(Call, PdnUser, sizeof(Call) - 1);
	Call[sizeof(Call) - 1] = 0;
	_strupr(Call);

	if (CallsignLike(Call))
	{
		strlop(Call, '-');
		User = LookupCall(Call);

		if (User == NULL)
		{
			snprintf(Why, WhyLen, "There is no BBS account for %s yet. Connect to the BBS over the air once to create it.", Call);
			return NULL;
		}
	}
	else if (Admin)
	{
		// A node admin with a plain username: the BBS sysop

		User = LookupCall(SYSOPCall[0] ? SYSOPCall : BBSName);

		if (User == NULL)
		{
			snprintf(Why, WhyLen, "The BBS sysop account (%s) does not exist.", SYSOPCall[0] ? SYSOPCall : BBSName);
			return NULL;
		}
	}
	else
	{
		snprintf(Why, WhyLen, "Your pdn username (%s) is not a callsign, so it cannot be matched to a BBS account.", PdnUser);
		return NULL;
	}

	if ((User->flags & F_Excluded))
	{
		snprintf(Why, WhyLen, "The BBS account %s is excluded.", User->Call);
		return NULL;
	}

	if ((User->flags & F_SYSOP) && !Admin)
	{
		snprintf(Why, WhyLen, "%s is a BBS sysop account, which needs admin rights on the pdn node.", User->Call);
		return NULL;
	}

	return User;
}

static char * QueryKey(char * Target, char * Key, int Max)
{
	// The session key is the first query parameter, up to any '&'

	char * q = strchr(Target, '?');
	int n = 0;

	Key[0] = 0;

	if (q == NULL)
		return Key;

	q++;

	while (q[n] && q[n] != '&' && n < Max - 1)
	{
		Key[n] = q[n];
		n++;
	}
	Key[n] = 0;
	return Key;
}

static struct HTTPConnectionInfo * MailSessionFor(char * Key, char * Owner, struct UserInfo * User)
{
	// Find the management page session for Key if it belongs to Owner, else make one

	time_t Now = time(NULL);
	int i, Free = -1, Oldest = 0;

	for (i = 0; i < WEB_MAXMAILSESS; i++)
	{
		struct MailWebSession * S = &MailSessions[i];

		if (S->InUse && Now - S->LastUsed > 3600)
			S->InUse = 0;

		if (S->InUse && Key[0] && strcmp(S->Info.Key, Key) == 0 && strcmp(S->Owner, Owner) == 0)
		{
			S->LastUsed = Now;
			return &S->Info;
		}

		if (!S->InUse && Free < 0)
			Free = i;

		if (MailSessions[i].LastUsed < MailSessions[Oldest].LastUsed)
			Oldest = i;
	}

	if (Free < 0)
		Free = Oldest;

	{
		struct MailWebSession * S = &MailSessions[Free];
		unsigned char Rand[6];
		FILE * f = fopen("/dev/urandom", "rb");

		if (f == NULL || fread(Rand, 1, 6, f) != 6)
		{
			for (i = 0; i < 6; i++)
				Rand[i] = rand();
		}
		if (f)
			fclose(f);

		memset(S, 0, sizeof(struct MailWebSession));
		S->InUse = 1;
		S->LastUsed = Now;
		strncpy(S->Owner, Owner, sizeof(S->Owner) - 1);
		sprintf(S->Info.Key, "M%02X%02X%02X%02X%02X%02X", Rand[0], Rand[1], Rand[2], Rand[3], Rand[4], Rand[5]);
		strcpy(S->Info.Callsign, User->Call);
		return &S->Info;
	}
}

static void HandleRequest(int Sock, char * Request, int ReqLen)
{
	char Method[16], Target[2048], Version[16];
	char Gateway[16], PdnUser[128], Scope[32], Prefix[256], Why[300];
	char Key[64];
	int Admin;
	struct UserInfo * User;
	char * Reply;
	int RLen = 0;

	if (sscanf(Request, "%15s %2047s %15s", Method, Target, Version) != 3)
	{
		SendPage(Sock, 400, "Bad request", "");
		return;
	}

	if (strcmp(Target, "/health") == 0)
	{
		SendReply(Sock, 200, "text/plain", "ok\n", 3, NULL);
		return;
	}

	if (GetHeader(Request, "X-Pdn-Gateway", Gateway, sizeof(Gateway)) == NULL || strcmp(Gateway, "1") != 0)
	{
		SendPage(Sock, 403, "LinBPQ Mail", "<p>This page is only available through the pdn control panel.</p>");
		return;
	}

	GetHeader(Request, "X-Pdn-User", PdnUser, sizeof(PdnUser));
	GetHeader(Request, "X-Pdn-Scope", Scope, sizeof(Scope));
	GetHeader(Request, "X-Forwarded-Prefix", Prefix, sizeof(Prefix));

	while (Prefix[0] && Prefix[strlen(Prefix) - 1] == '/')
		Prefix[strlen(Prefix) - 1] = 0;

	if (strchr(Prefix, '"') || strchr(Prefix, '\'') || strchr(Prefix, '<'))
		Prefix[0] = 0;

	if (PdnUser[0] == 0)
	{
		SendPage(Sock, 403, "LinBPQ Mail", "<p>Sign in to the pdn control panel first. LinBPQ Mail needs to know who you are, "
			"so it does not work while the node's login is turned off.</p>");
		return;
	}

	Admin = _stricmp(Scope, "admin") == 0;

	User = MapUser(PdnUser, Admin, Why, sizeof(Why));

	if (User == NULL)
	{
		SendPage(Sock, 403, "LinBPQ Mail", "<p>%s</p>", Why);
		return;
	}

	// Pictures the pages use

	if (_stricmp(Target, "/background.jpg") == 0 || _stricmp(Target, "/favicon.ico") == 0)
	{
		char Path[400];
		char * Data;
		int Len;

		if (FindTemplate(&Target[1], Path, sizeof(Path)) && (Data = ReadWholeFile(Path, &Len)))
		{
			SendReply(Sock, 200, Target[1] == 'b' ? "image/jpeg" : "image/x-icon", Data, Len, NULL);
			free(Data);
		}
		else
			SendPage(Sock, 404, "Not found", "");
		return;
	}

	// The app's home: links to webmail and, for a node admin, management

	if (strcmp(Target, "/") == 0 || strncmp(Target, "/?", 2) == 0)
	{
		SendPage(Sock, 200, "LinBPQ Mail", "<p>%s mailbox. Signed in as %s (BBS user %s).</p>"
			"<p><a href=\"%s/WebMail\">WebMail</a>%s%s%s</p>",
			BBSName, PdnUser, User->Call, Prefix,
			Admin ? "<a href=\"" : "", Admin ? Prefix : "", Admin ? "/Mail/Header\">Mail management</a>" : "");
		return;
	}

	Reply = malloc(WEB_REPLYSIZE + 64);

	if (_memicmp(Target, "/WebMail", 8) == 0)
	{
		// Webmail keeps its own sessions, keyed in the URL. Use the one in the
		// request if it is this user's, else sign this user straight in.

		struct HTTPConnectionInfo Dummy = {0};
		struct HTTPConnectionInfo * WM;
		char URL[2100];

		QueryKey(Target, Key, sizeof(Key));

		WM = Key[0] ? FindWMSession(Key) : NULL;

		if (WM && WM->User != User)
			WM = NULL;

		if (WM == NULL && _memicmp(Target, "/WebMail/webscript.js", 21) != 0
			&& _memicmp(Target, "/WebMail/Local", 14) != 0 && _memicmp(Target, "/WebMail/Standard", 17) != 0
			&& _memicmp(Target, "/WebMail/WMFile/", 16) != 0)
		{
			char Msg[128];
			int n;

			WM = AllocateWebMailSession();
			WM->User = User;
			WM->WebMailSkip = 0;
			WM->WebMailLastUsed = time(NULL);

			snprintf(URL, sizeof(URL), "/WebMail/WebMail?%s", WM->Key);
			strcpy(Method, "GET");

			n = snprintf(Msg, sizeof(Msg), "Webmail Connect from %s through pdn (as %s)", User->Call, PdnUser);
			WriteLogLine(NULL, '|', Msg, n, LOG_BBS);
		}
		else
			strcpy(URL, Target);

		ProcessMailHTTPMessage(&Dummy, Method, URL, Request, Reply, &RLen, ReqLen, "");
	}
	else if (_memicmp(Target, "/Mail/", 6) == 0)
	{
		struct HTTPConnectionInfo * Session;
		char URL[2100];

		if (!Admin)
		{
			free(Reply);
			SendPage(Sock, 403, "LinBPQ Mail", "<p>The mail management pages need admin rights on the pdn node.</p>"
				"<p><a href=\"%s/WebMail\">WebMail</a></p>", Prefix);
			return;
		}

		if (_memicmp(Target, "/Mail/API/", 10) == 0)
		{
			free(Reply);
			SendPage(Sock, 404, "Not found", "<p>The mail API is not available in linmail-pdn.</p>");
			return;
		}

		QueryKey(Target, Key, sizeof(Key));
		Session = MailSessionFor(Key, PdnUser, User);

		if (strcmp(Session->Key, Key) != 0)
		{
			// New session: start at the header page, as HTTPcode.c does after signon

			snprintf(URL, sizeof(URL), "/Mail/Header?%s", Session->Key);
			strcpy(Method, "POST");
		}
		else
			strcpy(URL, Target);

		Session->TNC = 0;				// Never "local": no automatic sysop sign-in
		ProcessMailHTTPMessage(Session, Method, URL, Request, Reply, &RLen, ReqLen, "");
	}
	else
	{
		free(Reply);
		SendPage(Sock, 404, "Not found", "<p><a href=\"%s/\">LinBPQ Mail</a></p>", Prefix);
		return;
	}

	if (RLen <= 0)
		SendPage(Sock, 404, "Not found", "<p><a href=\"%s/\">LinBPQ Mail</a></p>", Prefix);
	else
		SendMailReply(Sock, Reply, RLen, Prefix);

	free(Reply);
}

//	The listener

int PdnWebInit(int Port)
{
	struct sockaddr_in Addr = {0};
	int One = 1;

	if (Port <= 0)
		return 0;

	ListenSock = socket(AF_INET, SOCK_STREAM, 0);

	if (ListenSock < 0)
		return 0;

	setsockopt(ListenSock, SOL_SOCKET, SO_REUSEADDR, &One, sizeof(One));

	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(Port);
	Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);			// Loopback only - see the gateway trust rules

	if (bind(ListenSock, (struct sockaddr *)&Addr, sizeof(Addr)) != 0 || listen(ListenSock, 16) != 0)
	{
		WebLog("cannot listen on 127.0.0.1:%d (%s) - web pages off", Port, strerror(errno));
		close(ListenSock);
		ListenSock = -1;
		return 0;
	}

	fcntl(ListenSock, F_SETFL, fcntl(ListenSock, F_GETFL) | O_NONBLOCK);
	WebLog("listening on 127.0.0.1:%d", Port);
	return 1;
}

static void CloseConn(struct WebConn * C)
{
	close(C->Sock);
	free(C->Buf);
	memset(C, 0, sizeof(struct WebConn));
	C->Sock = -1;
}

static int RequestComplete(struct WebConn * C)
{
	char * End;
	char Value[32];
	int HLen, BodyLen = 0;

	C->Buf[C->Len] = 0;
	End = strstr(C->Buf, "\r\n\r\n");

	if (End == NULL)
		return FALSE;

	HLen = (int)(End - C->Buf) + 4;

	if (GetHeader(C->Buf, "Content-Length", Value, sizeof(Value)))
		BodyLen = atoi(Value);

	return C->Len >= HLen + BodyLen;
}

void PdnWebPoll()
{
	int i;
	time_t Now = time(NULL);

	if (ListenSock < 0)
		return;

	// New connections

	while (1)
	{
		struct sockaddr_in Peer;
		socklen_t PLen = sizeof(Peer);
		int Sock = accept(ListenSock, (struct sockaddr *)&Peer, &PLen);

		if (Sock < 0)
			break;

		if (Peer.sin_family != AF_INET || (ntohl(Peer.sin_addr.s_addr) >> 24) != 127)
		{
			close(Sock);
			continue;
		}

		for (i = 0; i < WEB_MAXCONN; i++)
		{
			if (Conns[i].Buf == NULL)
			{
				Conns[i].Sock = Sock;
				Conns[i].Started = Now;
				Conns[i].Size = 8192;
				Conns[i].Buf = malloc(Conns[i].Size + 1);
				Conns[i].Len = 0;
				fcntl(Sock, F_SETFL, fcntl(Sock, F_GETFL) | O_NONBLOCK);
				break;
			}
		}

		if (i == WEB_MAXCONN)
			close(Sock);
	}

	// Read what has arrived, and answer complete requests

	for (i = 0; i < WEB_MAXCONN; i++)
	{
		struct WebConn * C = &Conns[i];
		int n;

		if (C->Buf == NULL)
			continue;

		while (1)
		{
			if (C->Len >= C->Size)
			{
				if (C->Size >= WEB_MAXREQUEST)
				{
					SendPage(C->Sock, 413, "Too large", "");
					CloseConn(C);
					break;
				}
				C->Size *= 2;
				C->Buf = realloc(C->Buf, C->Size + 1);
			}

			n = recv(C->Sock, &C->Buf[C->Len], C->Size - C->Len, 0);

			if (n > 0)
			{
				C->Len += n;
				continue;
			}

			if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			{
				CloseConn(C);
				break;
			}
			break;				// Nothing more for now
		}

		if (C->Buf == NULL)
			continue;

		if (RequestComplete(C))
		{
			int Flags = fcntl(C->Sock, F_GETFL);

			fcntl(C->Sock, F_SETFL, Flags & ~O_NONBLOCK);
			HandleRequest(C->Sock, C->Buf, C->Len);
			CloseConn(C);
		}
		else if (Now - C->Started > 30)
			CloseConn(C);
	}
}
