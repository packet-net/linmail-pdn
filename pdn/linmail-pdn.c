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

//	linmail-pdn: the LinBPQ mail BBS as a standalone program for packet.net
//
//	This is the mail half of LinBPQ.c: it loads linmail.cfg and the mail
//	databases from a directory, then runs the same timer calls LinBPQ makes
//	from its node loop, at the same rates. The node itself is replaced by
//	pdnhost.c, which reaches a pdn node over RHPv2.

#define _GNU_SOURCE

#include "bpqmail.h"
#include "pdnhost.h"

#define CKernel
#include "Versions.h"

//	This build's own version, <LinBPQ version>-pdn<n> (for example
//	6.0.25.41-pdn1), written to build/pdnversion.h by the Makefile from
//	PDN_VERSION. The LinBPQ part is the one in Versions.h.

#include "pdnversion.h"

#ifndef PDN_VERSION
#define PDN_VERSION "dev"
#endif

#include <getopt.h>
#include <signal.h>

//	Globals the node side of LinBPQ.c normally provides to the mail code

char VersionString[50] = Verstring;
char VersionStringWithBuild[50] = Verstring;
int Ver[4] = {Vers};
char TextVerstring[50] = Verstring;

int _MYTIMEZONE = 0;
time_t LastTrafficTime;

BOOL LogBBS = TRUE;
BOOL LogCHAT = TRUE;
BOOL LogTCP = TRUE;

int ProgramErrors = 0;
BOOL Restarting = FALSE;
BOOL CLOSING = FALSE;
int KEEPGOING = 1;

char pgm[256] = "LINMAIL-PDN";
BOOL EventsEnabled = 0;

extern UCHAR BPQDirectory[260];
extern UCHAR LogDirectory[260];
extern UCHAR ConfigDirectory[260];

extern ConnectionInfo Connections[];
extern struct UserInfo * BBSChain;
extern int NumberofStreams;
extern int MaxStreams;
extern int BBSApplNum;
extern int BBSApplMask;
extern char BBSName[];
extern char SYSOPCall[];
extern BOOL SendAMPRDirect;
extern BOOL EnableUI;
extern int MailForInterval;
extern time_t MaintClock;
extern int MaintTime;
extern int MaintInterval;
extern time_t LastHouseKeepingTime;
extern BOOL GenerateTrafficReport;
extern struct SEM ConSemaphore;

extern char ConfigName[250];
extern char UserDatabaseName[MAX_PATH];
extern char UserDatabasePath[MAX_PATH];
extern char MsgDatabasePath[MAX_PATH];
extern char MsgDatabaseName[MAX_PATH];
extern char BIDDatabasePath[MAX_PATH];
extern char BIDDatabaseName[MAX_PATH];
extern char WPDatabasePath[MAX_PATH];
extern char WPDatabaseName[MAX_PATH];
extern char BadWordsPath[MAX_PATH];
extern char BadWordsName[MAX_PATH];
extern char NTSAliasesPath[MAX_PATH];
extern char NTSAliasesName[MAX_PATH];
extern char BaseDir[MAX_PATH];
extern char MailDir[MAX_PATH];

extern FILE * LogHandle[4];

BOOL GetConfig(char * ConfigName);
VOID SaveConfig(char * ConfigName);
VOID SetupNTSAliases(char * FN);
int GetHTMLForms();
char * AddUser(char * Call, char * password, BOOL BBSFlag);
int Connected(int Stream);
int Disconnected(int Stream);
int DoReceivedData(int Stream);
int DoBBSMonitorData(int Stream);
VOID FWDTimerProc();
VOID BBSSlowTimer();
VOID TCPTimer();
VOID TCPFastTimer();
void TrytoSend();
BOOL InitialiseTCP();
VOID InitialiseNNTP();
VOID SetupListenSet();
VOID SetupUIInterface();
VOID SendMailForThread(VOID * Param);
VOID DoHouseKeeping(BOOL Manual);
VOID CreateBBSTrafficReport();
void GetRestartData();
void SaveRestartData();
void GetPGConfig();
VOID DeleteRedundantMessages();

extern char RlineVer[50];
extern int SMTPInPort;
extern int POP3InPort;
extern int NNTPInPort;
void initUTF8();

static int Slowtimer = 0;

//	Small helpers LinBPQ.c and CommonCode.c provide

VOID CheckProgramErrors()
{
	if (Restarting)
		exit(0);

	ProgramErrors++;

	if (ProgramErrors > 25)
	{
		Restarting = TRUE;
		Logprintf(LOG_DEBUG_X, NULL, '!', "Too Many Program Errors - Closing");
		KEEPGOING = 0;
	}
}

BOOL CopyFile(char * In, char * Out, BOOL Failifexists)
{
	FILE * Handle;
	DWORD FileSize;
	char * Buffer;
	struct stat STAT;

	if (stat(In, &STAT) == -1)
		return FALSE;

	FileSize = STAT.st_size;

	Handle = fopen(In, "rb");

	if (Handle == NULL)
		return FALSE;

	Buffer = malloc(FileSize + 1);
	FileSize = fread(Buffer, 1, STAT.st_size, Handle);
	fclose(Handle);

	if (FileSize != STAT.st_size)
	{
		free(Buffer);
		return FALSE;
	}

	Handle = fopen(Out, "wb");

	if (Handle == NULL)
	{
		free(Buffer);
		return FALSE;
	}

	FileSize = fwrite(Buffer, 1, STAT.st_size, Handle);
	fclose(Handle);
	free(Buffer);

	return TRUE;
}

int RefreshMainWindow()
{
	return 0;
}

UCHAR * GetBPQDirectory()
{
	return BPQDirectory;
}

UCHAR * GetLogDirectory()
{
	return LogDirectory;
}

DllExport char * APIENTRY GetVersionString()
{
	return VersionString;
}

uint64_t GetTickCount()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint64_t MonotonicMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

char * strlop(char * buf, char delim)
{
	// Terminate buf at delim, and return rest of string

	char * ptr;

	if (buf == NULL) return NULL;

	ptr = strchr(buf, delim);

	if (ptr == NULL) return NULL;

	*(ptr)++ = 0;

	return ptr;
}

void _GetSemaphore(struct SEM * Semaphore, int ID, char * File, int Line)
{
	if (Semaphore->Flag != 0)
		Semaphore->Clashes++;

loop1:

	while (Semaphore->Flag != 0)
		Sleep(10);

	if (__sync_lock_test_and_set(&Semaphore->Flag, 1) != 0)
		goto loop1;

	Semaphore->Gets++;
	Semaphore->SemProcessID = GetCurrentProcessId();
	Semaphore->SemThreadID = GetCurrentThreadId();
	Semaphore->Line = Line;
	strcpy(Semaphore->File, File);
}

void FreeSemaphore(struct SEM * Semaphore)
{
	if (Semaphore->Flag == 0)
		Debugprintf("Free Semaphore Called when Sem not held");

	Semaphore->Rels++;
	Semaphore->Flag = 0;
}

//	Debug output. LinBPQ 6.0.25.41 moved Debugprintf out of BBSUtilities.c
//	into the node's CommonCode.c, where it writes logs/NodeDebugLog_*.log, and
//	sends the mail code's own LOG_DEBUG_X lines (Logprintf) to it. There is
//	no node here, so it writes the mail server's debug log
//	(logs/log_YYMMDD_DEBUG.txt), as BPQMail does on Windows. That keeps
//	LOG_DEBUG_X lines in the file they went to before the move.

VOID __cdecl Debugprintf(const char * format, ...)
{
	static int Busy = 0;
	char Mess[8192];
	va_list(arglist);
	int Len;

	if (Busy)
		return;					// Called again from inside WriteLogLine

	va_start(arglist, format);
	Len = vsnprintf(Mess, sizeof(Mess), format, arglist);
	va_end(arglist);

	if (Len < 0)
		return;

	if (Len >= (int)sizeof(Mess))
		Len = sizeof(Mess) - 1;

	while (Len > 0 && (Mess[Len - 1] == '\r' || Mess[Len - 1] == '\n'))
		Mess[--Len] = 0;

	Busy = 1;
	WriteLogLine(NULL, '!', Mess, Len, LOG_DEBUG_X);
	Busy = 0;
}

//	Since 6.0.25.41 WriteLogLine keeps each log file open for up to 30
//	seconds between writes instead of closing it every time, so a line can
//	sit in the stdio buffer until the next write. Flush once a tick so the
//	files (and the logLatest links) stay current for a sysop reading them.

extern struct SEM LogSEM;

static void FlushLogs()
{
	int i;

	GetSemaphore(&LogSEM, 0);

	for (i = 0; i < 4; i++)
	{
		if (LogHandle[i])
			fflush(LogHandle[i]);
	}

	FreeSemaphore(&LogSEM);
}

//	Connect script ELSE lines. After a failed connect, ProcessBBSConnectScript
//	(BBSUtilities.c) checks for "ELSE DELAY n" by comparing the five bytes at
//	Cmd[5], which for a bare "ELSE" line lie past the end of the string. Give
//	every short ELSE line enough zeroed room that the check reads only its own
//	memory. Lines are only ever replaced once; the table remembers which. See
//	pdn/UPSTREAM-BUGS.md.

#define MAXPADDED 1024

static char * Padded[MAXPADDED];
static int PaddedCount = 0;

static void PadElseList(char ** Script)
{
	int i, j;

	if (Script == NULL)
		return;

	for (i = 0; Script[i]; i++)
	{
		char * Line = Script[i];
		size_t Len = strlen(Line);

		if (Len >= 10 || _memicmp(Line, "ELSE", 4) != 0)
			continue;

		for (j = 0; j < PaddedCount; j++)
			if (Padded[j] == Line)
				break;

		if (j < PaddedCount)
			continue;				// Already done

		{
			char * New = zalloc(16);

			memcpy(New, Line, Len);
			Script[i] = New;
			free(Line);

			if (PaddedCount < MAXPADDED)
				Padded[PaddedCount++] = New;
		}
	}
}

static void PadElseLines()
{
	struct UserInfo * user;

	for (user = BBSChain; user; user = user->BBSNext)
	{
		if (user->ForwardingInfo)
		{
			PadElseList(user->ForwardingInfo->ConnectScript);
			PadElseList(user->ForwardingInfo->TempConnectScript);
		}
	}
}

//	The mail stream poll from LinBPQ.c

int PollStreams()
{
	int state, change;
	ConnectionInfo * conn;
	int n;

	for (n = 0; n < NumberofStreams; n++)
	{
		conn = &Connections[n];

		DoReceivedData(conn->BPQStream);
		DoBBSMonitorData(conn->BPQStream);

		SessionState(conn->BPQStream, &state, &change);

		if (change == 1)
		{
			if (state == 1) // Connected
			{
				GetSemaphore(&ConSemaphore, 0);
				Connected(conn->BPQStream);
				FreeSemaphore(&ConSemaphore);
			}
			else
			{
				GetSemaphore(&ConSemaphore, 0);
				Disconnected(conn->BPQStream);
				FreeSemaphore(&ConSemaphore);
			}
		}
	}

	return 0;
}

static void SigHandler(int sig)
{
	KEEPGOING = 0;
}

static char HelpScreen[] =
	"Usage: linmail-pdn [options]\n"
	"  -d, --datadir DIR    Directory holding linmail.cfg and the mail files\n"
	"                       (default $PDN_APP_STATE, else the current directory)\n"
	"  -f, --config FILE    linmail-pdn settings file (default DIR/linmail-pdn.conf)\n"
	"  -l, --logdir DIR     Directory for logs (default the data directory)\n"
	"  -r, --rhp HOST:PORT  pdn RHPv2 server (default 127.0.0.1:9000)\n"
	"  -c, --call CALL      Callsign the BBS answers to (default the BBS name)\n"
	"  -a, --alias CALLS    More callsigns to answer to, e.g. BBS\n"
	"  -n, --node CALL      pdn node callsign, used in connect replies\n"
	"  -m, --portmap MAP    Connect script port numbers to pdn port ids,\n"
	"                       e.g. 1=vhf,2=hf\n"
	"  -p, --defport ID     pdn port for a connect line with no port\n"
	"  -L, --linger SECS    Hold a close this long after our last send (default 10)\n"
	"  -u, --user USER      RHP auth user\n"
	"  -w, --pass PASS      RHP auth password\n"
	"  -t, --trace          Print all RHP traffic\n"
	"  -W, --web-port PORT  Loopback port for webmail through pdn's app gateway\n"
	"                       (default 18095, 0 = off)\n"
	"  -V, --version        Show the linmail-pdn and LinBPQ versions\n"
	"  -h, --help           Show this help\n"
	"Settings are taken from the settings file, then the environment the pdn app\n"
	"supervisor sets (PDN_RHP_HOST, PDN_RHP_PORT, PDN_APP_CALLSIGN, PDN_NODE_CALLSIGN,\n"
	"PDN_NODE_ALIAS), then these options. See pdn/linmail-pdn.conf.example.\n";

static struct option long_options[] =
{
	{"datadir", required_argument, 0, 'd'},
	{"config", required_argument, 0, 'f'},
	{"logdir", required_argument, 0, 'l'},
	{"rhp", required_argument, 0, 'r'},
	{"call", required_argument, 0, 'c'},
	{"alias", required_argument, 0, 'a'},
	{"node", required_argument, 0, 'n'},
	{"portmap", required_argument, 0, 'm'},
	{"defport", required_argument, 0, 'p'},
	{"linger", required_argument, 0, 'L'},
	{"user", required_argument, 0, 'u'},
	{"pass", required_argument, 0, 'w'},
	{"trace", no_argument, 0, 't'},
	{"web-port", required_argument, 0, 'W'},
	{"help", no_argument, 0, 'h'},
	{"version", no_argument, 0, 'V'},
	{NULL, no_argument, NULL, 0}
};

static char OptString[] = "d:f:l:r:c:a:n:m:p:L:u:w:tW:hV";

static char LogDirOption[260] = "";
static int WebPort = 18095;
static char HtmlDir[260] = "";

int PdnWebInit(int Port);
void PdnWebPoll();
void PdnWebSetTemplateDirs(char * Configured, char * ExeDir);

static void Copy(char * To, int Max, const char * Value)
{
	strncpy(To, Value, Max - 1);
	To[Max - 1] = 0;
}

static void SetRHP(const char * Arg)
{
	char Host[128];
	char * Port;

	Copy(Host, sizeof(Host), Arg);
	Port = strrchr(Host, ':');

	if (Port)
	{
		*(Port++) = 0;
		PdnCfg.RHPPort = atoi(Port);
	}

	if (Host[0])
		Copy(PdnCfg.RHPHost, sizeof(PdnCfg.RHPHost), Host);
}

//	One place that knows every setting, whether it comes from the settings
//	file, the environment or the command line. Returns FALSE for an unknown key.

static int SetOption(const char * Key, const char * Value)
{
	if (_stricmp(Key, "rhp") == 0)
		SetRHP(Value);
	else if (_stricmp(Key, "rhp_host") == 0)
		Copy(PdnCfg.RHPHost, sizeof(PdnCfg.RHPHost), Value);
	else if (_stricmp(Key, "rhp_port") == 0)
		PdnCfg.RHPPort = atoi(Value);
	else if (_stricmp(Key, "rhp_user") == 0)
		Copy(PdnCfg.RHPUser, sizeof(PdnCfg.RHPUser), Value);
	else if (_stricmp(Key, "rhp_pass") == 0)
		Copy(PdnCfg.RHPPass, sizeof(PdnCfg.RHPPass), Value);
	else if (_stricmp(Key, "call") == 0)
		Copy(PdnCfg.AppCall, sizeof(PdnCfg.AppCall), Value);
	else if (_stricmp(Key, "alias") == 0)
		Copy(PdnCfg.Aliases, sizeof(PdnCfg.Aliases), Value);
	else if (_stricmp(Key, "node") == 0)
		Copy(PdnCfg.NodeCall, sizeof(PdnCfg.NodeCall), Value);
	else if (_stricmp(Key, "node_alias") == 0)
		Copy(PdnCfg.NodeAlias, sizeof(PdnCfg.NodeAlias), Value);
	else if (_stricmp(Key, "portmap") == 0)
	{
		PdnCfg.PortCount = 0;
		PdnParsePortMap((char *)Value);
	}
	else if (_stricmp(Key, "default_port") == 0)
		Copy(PdnCfg.DefaultPort, sizeof(PdnCfg.DefaultPort), Value);
	else if (_stricmp(Key, "linger") == 0)
		PdnCfg.DiscLinger = atoi(Value);
	else if (_stricmp(Key, "trace") == 0)
		PdnCfg.Trace = atoi(Value);
	else if (_stricmp(Key, "logdir") == 0)
		Copy(LogDirOption, sizeof(LogDirOption), Value);
	else if (_stricmp(Key, "web_port") == 0)
		WebPort = atoi(Value);
	else if (_stricmp(Key, "html_dir") == 0)
		Copy(HtmlDir, sizeof(HtmlDir), Value);
	else
		return FALSE;

	return TRUE;
}

static int LoadSettingsFile(char * FileName, int Required)
{
	// key = value lines. # starts a comment.

	FILE * Handle = fopen(FileName, "r");
	char Line[512];
	int LineNo = 0;

	if (Handle == NULL)
	{
		if (Required)
		{
			printf("linmail-pdn: cannot open settings file %s\n", FileName);
			return FALSE;
		}
		return TRUE;
	}

	printf("linmail-pdn: settings from %s\n", FileName);

	while (fgets(Line, sizeof(Line), Handle))
	{
		char * Key = Line, * Value, * End;

		LineNo++;
		strlop(Line, '#');

		while (*Key == ' ' || *Key == '\t')
			Key++;

		Value = strchr(Key, '=');

		if (Value == NULL)
		{
			if (strspn(Key, " \t\r\n") != strlen(Key))
				printf("linmail-pdn: %s line %d not understood\n", FileName, LineNo);
			continue;
		}

		*(Value++) = 0;

		// Trim both sides

		End = Key + strlen(Key);
		while (End > Key && (End[-1] == ' ' || End[-1] == '\t'))
			*(--End) = 0;

		while (*Value == ' ' || *Value == '\t')
			Value++;

		End = Value + strlen(Value);
		while (End > Value && (End[-1] == ' ' || End[-1] == '\t' || End[-1] == '\r' || End[-1] == '\n'))
			*(--End) = 0;

		if (!SetOption(Key, Value))
			printf("linmail-pdn: %s line %d: unknown setting %s\n", FileName, LineNo, Key);
	}

	fclose(Handle);
	return TRUE;
}

static void EnvOption(char * Name, char * Key)
{
	char * Value = getenv(Name);

	if (Value && Value[0])
		SetOption(Key, Value);
}

static int ArgOption(int c, char * Arg)
{
	switch (c)
	{
	case 'l': return SetOption("logdir", Arg);
	case 'r': return SetOption("rhp", Arg);
	case 'c': return SetOption("call", Arg);
	case 'a': return SetOption("alias", Arg);
	case 'n': return SetOption("node", Arg);
	case 'm': return SetOption("portmap", Arg);
	case 'p': return SetOption("default_port", Arg);
	case 'L': return SetOption("linger", Arg);
	case 'u': return SetOption("rhp_user", Arg);
	case 'w': return SetOption("rhp_pass", Arg);
	case 't': return SetOption("trace", "1");
	case 'W': return SetOption("web_port", Arg);
	}
	return TRUE;
}

int main(int argc, char * argv[])
{
	struct UserInfo * user = NULL;
	ConnectionInfo * conn;
	struct stat STAT;
	char LogDir[260] = "";
	char DataDir[260] = "";
	char SettingsFile[300] = "";
	char * AddUserArgs[3] = {NULL, NULL, NULL};
	uint64_t NextTick;
	int i, c;

	setlinebuf(stdout);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, SigHandler);
	signal(SIGTERM, SigHandler);

	signal(SIGHUP, SIG_IGN);

	// As LinBPQ.c main()

	sprintf(RlineVer, "LinBPQ%d.%d.%d", Ver[0], Ver[1], Ver[2]);

	tzset();
	_MYTIMEZONE = _timezone;

	if (_MYTIMEZONE < -86400 || _MYTIMEZONE > 86400)
		_MYTIMEZONE = 0;

	initUTF8();

	// Defaults, then the settings file, then the environment (as set by the
	// pdn app supervisor), then the command line

	strcpy(PdnCfg.RHPHost, "127.0.0.1");
	PdnCfg.RHPPort = 9000;
	PdnCfg.DiscLinger = 10;

	// As LinBPQ: --adduser CALL PASSWORD ISBBS adds a user and exits. Take it
	// out of argv before the options are parsed.

	for (i = 1; i < argc; i++)
	{
		if (_stricmp(argv[i], "--adduser") == 0 && i + 3 < argc)
		{
			AddUserArgs[0] = argv[i + 1];
			AddUserArgs[1] = argv[i + 2];
			AddUserArgs[2] = argv[i + 3];
			memmove(&argv[i], &argv[i + 4], (argc - i - 4 + 1) * sizeof(char *));
			argc -= 4;
			break;
		}
	}

	// First pass: only the data directory, the settings file and help

	while ((c = getopt_long(argc, argv, OptString, long_options, NULL)) != -1)
	{
		switch (c)
		{
		case 'd': Copy(DataDir, sizeof(DataDir), optarg); break;
		case 'f': Copy(SettingsFile, sizeof(SettingsFile), optarg); break;
		case 'h':
			printf("%s", HelpScreen);
			return 0;
		case 'V':
			printf("linmail-pdn %s\nLinBPQ %s (G8BPQ's mail server)\n", PDN_VERSION, TextVerstring);
			return 0;
		case '?':
			printf("%s", HelpScreen);
			return 1;
		}
	}

	printf("linmail-pdn: linmail-pdn %s, G8BPQ Mail Server %s for packet.net\n", PDN_VERSION, TextVerstring);

	if (DataDir[0] == 0 && getenv("PDN_APP_STATE"))
		Copy(DataDir, sizeof(DataDir), getenv("PDN_APP_STATE"));

	if (DataDir[0])
		strcpy(BPQDirectory, DataDir);
	else if (getcwd(BPQDirectory, 256) == NULL)
		strcpy(BPQDirectory, ".");

	if (SettingsFile[0])
	{
		if (!LoadSettingsFile(SettingsFile, TRUE))
			return 1;
	}
	else
	{
		snprintf(SettingsFile, sizeof(SettingsFile), "%s/linmail-pdn.conf", BPQDirectory);
		LoadSettingsFile(SettingsFile, FALSE);
	}

	EnvOption("PDN_RHP_HOST", "rhp_host");
	EnvOption("PDN_RHP_PORT", "rhp_port");
	EnvOption("PDN_RHP_USER", "rhp_user");
	EnvOption("PDN_RHP_PASS", "rhp_pass");
	EnvOption("PDN_APP_CALLSIGN", "call");
	EnvOption("PDN_NODE_CALLSIGN", "node");
	EnvOption("PDN_NODE_ALIAS", "node_alias");
	EnvOption("PDN_LINMAIL_PORTMAP", "portmap");
	EnvOption("PDN_LINMAIL_DEFAULTPORT", "default_port");

	// Second pass: everything else

	optind = 0;

	while ((c = getopt_long(argc, argv, OptString, long_options, NULL)) != -1)
		ArgOption(c, optarg);

	_strupr(PdnCfg.AppCall);
	_strupr(PdnCfg.Aliases);
	_strupr(PdnCfg.NodeCall);
	_strupr(PdnCfg.NodeAlias);

	if (LogDirOption[0])
		strcpy(LogDir, LogDirOption);

	strcpy(ConfigDirectory, BPQDirectory);
	strcpy(LogDirectory, LogDir[0] ? LogDir : (char *)BPQDirectory);

	// The mail code keeps log file names in 100 byte buffers (FilesNames in
	// BBSUtilities.c), and a longer name aborts the program. Refuse up front.

	if (strlen(LogDirectory) > 99 - strlen("/logs/log_YYMMDD_CHAT.txt"))
	{
		printf("linmail-pdn: the log directory path %s is too long for the mail code (at most %d characters)\n",
			LogDirectory, (int)(99 - strlen("/logs/log_YYMMDD_CHAT.txt")));
		return 1;
	}

	sprintf(LogDir, "%s/logs", LogDirectory);
	mkdir(LogDir, S_IRWXU | S_IRWXG | S_IRWXO);

	printf("linmail-pdn: data directory %s, logs in %s\n", BPQDirectory, LogDir);

	// From here on this follows LinBPQ.c's "Start Mail" block

	sprintf(ConfigName, "%s/linmail.cfg", BPQDirectory);
	printf("Config File is %s\n", ConfigName);

	if (stat(ConfigName, &STAT) == -1)
	{
		printf("Config File not found - creating a default config\n");

		if (PdnCfg.AppCall[0])
			strcpy(BBSName, PdnCfg.AppCall);
		else
			strcpy(BBSName, "N0CALL");

		strlop(BBSName, '-');
		BBSApplNum = 1;
		MaxStreams = 10;
		SaveConfig(ConfigName);
	}

	if (GetConfig(ConfigName) == EXIT_FAILURE)
	{
		printf("BBS Config File seems corrupt - check before continuing\n");
		return -1;
	}

	printf("Config Processed\n");

	if (PdnCfg.AppCall[0] == 0)
		strcpy(PdnCfg.AppCall, BBSName);

	BBSApplMask = 1 << (BBSApplNum - 1);

	sprintf(BaseDir, "%s", BPQDirectory);

	sprintf(UserDatabasePath, "%s/%s", BaseDir, UserDatabaseName);
	sprintf(MsgDatabasePath, "%s/%s", BaseDir, MsgDatabaseName);
	sprintf(BIDDatabasePath, "%s/%s", BaseDir, BIDDatabaseName);
	sprintf(WPDatabasePath, "%s/%s", BaseDir, WPDatabaseName);
	sprintf(BadWordsPath, "%s/%s", BaseDir, BadWordsName);
	sprintf(NTSAliasesPath, "%s/%s", BaseDir, NTSAliasesName);
	sprintf(MailDir, "%s/Mail", BaseDir);

	mkdir(MailDir, S_IRWXU | S_IRWXG | S_IRWXO);
	chmod(MailDir, S_IRWXU | S_IRWXG | S_IRWXO);

	CopyBIDDatabase();
	CopyMessageDatabase();
	CopyUserDatabase();
	CopyWPDatabase();

	SetupMyHA();
	SetupFwdAliases();
	SetupNTSAliases(NTSAliasesPath);

	GetWPDatabase();

	GetMessageDatabase();
	GetUserDatabase();
	GetBIDDatabase();
	GetBadWordFile();
	GetHTMLForms();
	GetPGConfig();
	GetRestartData();

	// Make sure there is a user record for the BBS, with BBS bit set.

	user = LookupCall(BBSName);

	if (user == NULL)
	{
		user = AllocateUserRecord(BBSName);
		user->Temp = zalloc(sizeof (struct TempUserInfo));
	}

	if ((user->flags & F_BBS) == 0)
	{
		if (SetupNewBBS(user))
			user->flags |= F_BBS;
	}

	if (SendAMPRDirect)
	{
		BOOL NeedSave = FALSE;

		user = LookupCall("AMPR");

		if (user == NULL)
		{
			user = AllocateUserRecord("AMPR");
			user->Temp = zalloc(sizeof (struct TempUserInfo));
			NeedSave = TRUE;
		}

		if ((user->flags & F_BBS) == 0)
		{
			if (SetupNewBBS(user))
				user->flags |= F_BBS;
			NeedSave = TRUE;
		}

		if (NeedSave)
			SaveUserDatabase();
	}

	if (SYSOPCall[0] == 0)
		strcpy(SYSOPCall, BBSName);

	// See if just want to add user (mainly for setup scripts)

	if (AddUserArgs[0])
	{
		BOOL isBBS = FALSE;
		char * response;

		if (_stricmp(AddUserArgs[2], "TRUE") == 0)
			isBBS = TRUE;

		printf("Adding User %s\r\n", AddUserArgs[0]);
		response = AddUser(AddUserArgs[0], AddUserArgs[1], isBBS);
		printf("%s", response);
		SaveUserDatabase();
		exit(0);
	}

	// Connect to pdn, then allocate the BBS streams

	PdnHostInit();

	for (i = 0; i < MaxStreams; i++)
	{
		conn = &Connections[i];
		conn->BPQStream = FindFreeStream();

		if (conn->BPQStream == 255)
			break;

		NumberofStreams++;

		SetAppl(conn->BPQStream, (i == 0 && EnableUI) ? 0x82 : 2, BBSApplMask);
		Disconnect(conn->BPQStream);
	}

	// The SMTP, POP3 and NNTP servers are not part of linmail-pdn. Start the
	// TCP code with their ports zeroed so no listener opens, then put the
	// values back so linmail.cfg is saved unchanged.
	{
		int SavedSMTP = SMTPInPort, SavedPOP3 = POP3InPort, SavedNNTP = NNTPInPort;

		if (SMTPInPort || POP3InPort || NNTPInPort)
			printf("linmail-pdn: SMTP, POP3 and NNTP servers are not supported - ignoring their ports in linmail.cfg\n");

		SMTPInPort = POP3InPort = NNTPInPort = 0;

		InitialiseTCP();
		InitialiseNNTP();

		SMTPInPort = SavedSMTP;
		POP3InPort = SavedPOP3;
		NNTPInPort = SavedNNTP;
	}

	SetupListenSet();		// Master set of listening sockets

	if (EnableUI || MailForInterval)
		SetupUIInterface();

	if (MailForInterval)
		_beginthread(SendMailForThread, 0, 0);

	// Calculate time to run Housekeeping
	{
		struct tm * tm;
		time_t now;

		now = time(NULL);

		tm = gmtime(&now);

		tm->tm_hour = MaintTime / 100;
		tm->tm_min = MaintTime % 100;
		tm->tm_sec = 0;

		MaintClock = mktime(tm) - (time_t)_MYTIMEZONE;

		while (MaintClock < now)
			MaintClock += MaintInterval * 3600;

		Debugprintf("Maint Clock %lld NOW %lld Time to HouseKeeping %lld", (long long)MaintClock, (long long)now, (long long)(MaintClock - now));

		if (LastHouseKeepingTime)
		{
			if ((now - LastHouseKeepingTime) > MaintInterval * 3600)
				DoHouseKeeping(FALSE);
		}

		for (i = optind; i < argc; i++)
		{
			if (_stricmp(argv[i], "tidymail") == 0)
				DeleteRedundantMessages();

			if (_stricmp(argv[i], "nohomebbs") == 0)
				DontNeedHomeBBS = TRUE;
		}
	}

	printf("Mail Started\n");
	Logprintf(LOG_BBS, NULL, '!', "Mail Starting");

	printf("linmail-pdn: BBS %s answering as %s via pdn RHP %s:%d, %d streams\n",
		BBSName, PdnCfg.AppCall, PdnCfg.RHPHost, PdnCfg.RHPPort, NumberofStreams);

	// Webmail and the management pages, for pdn's app gateway
	{
		char ExeDir[300] = "";
		ssize_t n = readlink("/proc/self/exe", ExeDir, sizeof(ExeDir) - 1);

		if (n > 0)
		{
			char * Slash;

			ExeDir[n] = 0;
			Slash = strrchr(ExeDir, '/');

			if (Slash)
				*Slash = 0;
		}

		if (HtmlDir[0] == 0 && getenv("PDN_APP_DIR"))
			snprintf(HtmlDir, sizeof(HtmlDir), "%s/HTML", getenv("PDN_APP_DIR"));

		PdnWebSetTemplateDirs(HtmlDir, ExeDir);
		PdnWebInit(WebPort);
	}

	// The LinBPQ node loop, mail part only. One tick every 100 ms; the RHP
	// socket is serviced between ticks.

	NextTick = MonotonicMs() + 100;

	while (KEEPGOING)
	{
		uint64_t Now = MonotonicMs();

		if (Now < NextTick)
		{
			PdnHostPoll((int)(NextTick - Now));
			continue;
		}

		NextTick += 100;

		if (NextTick < Now)
			NextTick = Now + 100;			// Don't try to catch up after a stall

		PdnHostPoll(0);

		Slowtimer++;

		PadElseLines();
		PollStreams();

		if ((Slowtimer % 20) == 0)
			FWDTimerProc();

		if (Slowtimer > 100)		// 10 secs
		{
			time_t NOW = time(NULL);
			struct tm * tm;

			TCPTimer();
			BBSSlowTimer();

			if (MaintClock < NOW)
			{
				while (MaintClock < NOW)		// in case large time step
					MaintClock += MaintInterval * 3600;

				Debugprintf("|Enter HouseKeeping");
				DoHouseKeeping(FALSE);
			}

			tm = gmtime(&NOW);

			if (tm->tm_wday == 0)		// Sunday
			{
				if (GenerateTrafficReport && (LastTrafficTime + 86400) < NOW)
				{
					CreateBBSTrafficReport();
					LastTrafficTime = NOW;
				}
			}
		}

		TCPFastTimer();
		TrytoSend();
		PdnWebPoll();
		FlushLogs();

		if (Slowtimer > 100)
			Slowtimer = 0;
	}

	// Shut down as LinBPQ does

	printf("linmail-pdn: closing\n");

	for (i = 0; i < NumberofStreams; i++)
	{
		int BPQStream = Connections[i].BPQStream;

		if (BPQStream)
		{
			SetAppl(BPQStream, 0, 0);
			Disconnect(BPQStream);
			DeallocateStream(BPQStream);
		}
	}

	PdnHostClose();

	SaveMessageDatabase();
	SaveBIDDatabase();
	SaveConfig(ConfigName);
	SaveRestartData();

	for (i = 0; i < 4; i++)
	{
		if (LogHandle[i])
			fclose(LogHandle[i]);
	}

	return 0;
}
