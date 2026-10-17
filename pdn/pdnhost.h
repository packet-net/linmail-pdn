//	linmail-pdn: the BPQ host API backed by a packet.net (pdn) RHPv2 connection
//
//	The mail code (BBSUtilities.c and friends) talks to "the node" through a
//	small set of BPQ host API calls (SessionControl, GetMsg, SendMsg, ...).
//	In LinBPQ those are served by the node itself. In linmail-pdn they are
//	served by pdnhost.c, which turns them into RHPv2 requests to a pdn node.

#ifndef PDNHOST_H
#define PDNHOST_H

#define PDN_MAXSTREAMS 64
#define PDN_MAXPORTMAP 32

struct PdnConfig
{
	char RHPHost[128];			// PDN_RHP_HOST, default 127.0.0.1
	int RHPPort;				// PDN_RHP_PORT, default 9000
	char RHPUser[64];			// optional RHP auth (PDN_RHP_USER / PDN_RHP_PASS)
	char RHPPass[64];
	char AppCall[10];			// Callsign the BBS answers to (PDN_APP_CALLSIGN)
	char Aliases[64];			// More callsigns to answer to, such as BBS
	char NodeCall[10];			// pdn node callsign, used in synthesised node replies
	char NodeAlias[10];			// optional node alias for the same
	char DefaultPort[32];		// Port label for "C CALL" with no port (empty = none)
	int Trace;					// Print RHP traffic to stdout
	int PortCount;
	int PortNum[PDN_MAXPORTMAP];	// BPQ-style port number used in connect scripts
	char PortLabel[PDN_MAXPORTMAP][32];	// pdn port id it maps to
};

extern struct PdnConfig PdnCfg;

int PdnParsePortMap(char * Map);
void PdnHostInit();
void PdnHostPoll(int WaitMs);		// Service the RHP socket. Waits up to WaitMs for input
void PdnHostClose();
int PdnPortNumber(const char * Label);
int PdnIsCallsign(char * Call);

//	Logins that arrive some other way than pdn (pdntelnet.c). A local login
//	gets a BBS stream exactly as an inbound pdn connect does; the stream then
//	reads from and writes to the login instead of an RHP handle.

int PdnLocalConnect(char * Call, int Secure, int Id, char * Peer);	// Stream (0 based), or -1 if none is free
void PdnLocalData(int n, UCHAR * Data, int Len);	// From the caller to the BBS
void PdnLocalClosed(int n);							// The caller has gone
int PdnLocalBacklog(int n);							// Bytes the BBS has not yet read, roughly

//	pdntelnet.c: LinBPQ style telnet and FBB logins

int PdnTelnetOption(const char * Key, const char * Value);
void PdnTelnetInit();
void PdnTelnetFds(fd_set * Read, fd_set * Write, int * Max);
void PdnTelnetPoll();
void PdnTelnetClose();
int PdnTelnetSend(int Id, UCHAR * Data, int Len);	// From the BBS to the caller
int PdnTelnetQueued(int Id);						// Output not yet sent, in 256 byte buffers
void PdnTelnetHangup(int Id);						// The BBS has disconnected

#endif
