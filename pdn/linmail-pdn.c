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
	"  -l, --logdir DIR     Directory for logs (default the data directory)\n"
	"  -r, --rhp HOST:PORT  pdn RHPv2 server (default $PDN_RHP_HOST:$PDN_RHP_PORT,\n"
	"                       else 127.0.0.1:9000)\n"
	"  -c, --call CALL      Callsign the BBS answers to (default $PDN_APP_CALLSIGN,\n"
	"                       else the BBS name from linmail.cfg)\n"
	"  -n, --node CALL      pdn node callsign, used in connect replies\n"
	"                       (default $PDN_NODE_CALLSIGN)\n"
	"  -m, --portmap MAP    Connect script port numbers to pdn port ids,\n"
	"                       e.g. 1=vhf,2=hf (default $PDN_LINMAIL_PORTMAP)\n"
	"  -p, --defport ID     pdn port for a connect line with no port\n"
	"  -L, --linger SECS    Hold a close this long after our last send (default 10)\n"
	"  -u, --user USER      RHP auth user (default $PDN_RHP_USER)\n"
	"  -w, --pass PASS      RHP auth password (default $PDN_RHP_PASS)\n"
	"  -t, --trace          Print all RHP traffic\n"
	"  -h, --help           Show this help\n";

static struct option long_options[] =
{
	{"datadir", required_argument, 0, 'd'},
	{"logdir", required_argument, 0, 'l'},
	{"rhp", required_argument, 0, 'r'},
	{"call", required_argument, 0, 'c'},
	{"node", required_argument, 0, 'n'},
	{"portmap", required_argument, 0, 'm'},
	{"defport", required_argument, 0, 'p'},
	{"linger", required_argument, 0, 'L'},
	{"user", required_argument, 0, 'u'},
	{"pass", required_argument, 0, 'w'},
	{"trace", no_argument, 0, 't'},
	{"help", no_argument, 0, 'h'},
	{NULL, no_argument, NULL, 0}
};

static void EnvCopy(char * To, int Max, char * Name)
{
	char * Value = getenv(Name);

	if (Value && Value[0])
	{
		strncpy(To, Value, Max - 1);
		To[Max - 1] = 0;
	}
}

static void SetRHP(char * Arg)
{
	char * Port = strrchr(Arg, ':');

	if (Port)
	{
		*(Port++) = 0;
		PdnCfg.RHPPort = atoi(Port);
	}

	if (Arg[0])
		strncpy(PdnCfg.RHPHost, Arg, sizeof(PdnCfg.RHPHost) - 1);
}

int main(int argc, char * argv[])
{
	struct UserInfo * user = NULL;
	ConnectionInfo * conn;
	struct stat STAT;
	char LogDir[260] = "";
	char DataDir[260] = "";
	char * PortMap = getenv("PDN_LINMAIL_PORTMAP");
	char * Env;
	uint64_t NextTick;
	int i, c;

	setlinebuf(stdout);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, SigHandler);
	signal(SIGTERM, SigHandler);

	signal(SIGHUP, SIG_IGN);

	printf("linmail-pdn: G8BPQ Mail Server %s for packet.net\n", TextVerstring);

	// As LinBPQ.c main()

	sprintf(RlineVer, "LinBPQ%d.%d.%d", Ver[0], Ver[1], Ver[2]);

	tzset();
	_MYTIMEZONE = _timezone;

	if (_MYTIMEZONE < -86400 || _MYTIMEZONE > 86400)
		_MYTIMEZONE = 0;

	initUTF8();

	// Defaults, then environment (as set by the pdn app supervisor), then arguments

	strcpy(PdnCfg.RHPHost, "127.0.0.1");
	PdnCfg.RHPPort = 9000;
	PdnCfg.DiscLinger = 10;

	EnvCopy(PdnCfg.RHPHost, sizeof(PdnCfg.RHPHost), "PDN_RHP_HOST");

	if ((Env = getenv("PDN_RHP_PORT")) && atoi(Env))
		PdnCfg.RHPPort = atoi(Env);

	EnvCopy(PdnCfg.AppCall, sizeof(PdnCfg.AppCall), "PDN_APP_CALLSIGN");
	EnvCopy(PdnCfg.NodeCall, sizeof(PdnCfg.NodeCall), "PDN_NODE_CALLSIGN");
	EnvCopy(PdnCfg.NodeAlias, sizeof(PdnCfg.NodeAlias), "PDN_NODE_ALIAS");
	EnvCopy(PdnCfg.RHPUser, sizeof(PdnCfg.RHPUser), "PDN_RHP_USER");
	EnvCopy(PdnCfg.RHPPass, sizeof(PdnCfg.RHPPass), "PDN_RHP_PASS");
	EnvCopy(PdnCfg.DefaultPort, sizeof(PdnCfg.DefaultPort), "PDN_LINMAIL_DEFAULTPORT");
	EnvCopy(DataDir, sizeof(DataDir), "PDN_APP_STATE");

	while ((c = getopt_long(argc, argv, "d:l:r:c:n:m:p:L:u:w:th", long_options, NULL)) != -1)
	{
		switch (c)
		{
		case 'd': strncpy(DataDir, optarg, 259); break;
		case 'l': strncpy(LogDir, optarg, 259); break;
		case 'r': SetRHP(optarg); break;
		case 'c': strncpy(PdnCfg.AppCall, optarg, 9); break;
		case 'n': strncpy(PdnCfg.NodeCall, optarg, 9); break;
		case 'm': PortMap = optarg; break;
		case 'p': strncpy(PdnCfg.DefaultPort, optarg, 31); break;
		case 'L': PdnCfg.DiscLinger = atoi(optarg); break;
		case 'u': strncpy(PdnCfg.RHPUser, optarg, 63); break;
		case 'w': strncpy(PdnCfg.RHPPass, optarg, 63); break;
		case 't': PdnCfg.Trace = 1; break;
		case 'h':
		default:
			printf("%s", HelpScreen);
			return c == 'h' ? 0 : 1;
		}
	}

	if (PortMap)
		PdnParsePortMap(PortMap);

	_strupr(PdnCfg.AppCall);
	_strupr(PdnCfg.NodeCall);
	_strupr(PdnCfg.NodeAlias);

	if (DataDir[0])
		strcpy(BPQDirectory, DataDir);
	else
		getcwd(BPQDirectory, 256);

	strcpy(ConfigDirectory, BPQDirectory);
	strcpy(LogDirectory, LogDir[0] ? LogDir : (char *)BPQDirectory);

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

	if (argc - optind == 4 && _stricmp(argv[optind], "--adduser") == 0)
	{
		BOOL isBBS = FALSE;
		char * response;

		if (_stricmp(argv[optind + 3], "TRUE") == 0)
			isBBS = TRUE;

		printf("Adding User %s\r\n", argv[optind + 1]);
		response = AddUser(argv[optind + 1], argv[optind + 2], isBBS);
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

	InitialiseTCP();
	InitialiseNNTP();

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
