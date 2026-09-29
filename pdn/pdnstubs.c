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

//	linmail-pdn: stand-ins for node features the mail code calls but which
//	are out of scope for the pdn proof of concept. Each is listed in
//	pdn/PLAN.md. Nothing here pretends to work: it either does nothing or
//	returns "not available".

#include "bpqmail.h"

UCHAR LogDirectory[260] = "";
UCHAR ConfigDirectory[260] = ".";
char LOC[7] = "";

char WL2KModes [55][18] = {
	"Packet 1200", "Packet 2400", "Packet 4800", "Packet 9600", "Packet 19200", "Packet 38400", "High Speed Packet", "", "", "", "",
	"Pactor 1", "Pactor", "Pactor", "Pactor 2", "Pactor", "Pactor 3", "Pactor", "Pactor", "Pactor", "Pactor 4", // 11 - 20
	"Winmor 500", "Winmor 1600", "", "", "", "", "", "", "",				// 21 - 29
	"Robust Packet", "", "", "", "", "", "", "", "", "",					// 30 - 39
	"ARDOP 200", "ARDOP 500", "ARDOP 1000", "ARDOP 2000", "ARDOP 2000 FM", "", "", "", "", "",	// 40 - 49
	"VARA", "VARA FM", "VARA FM WIDE", "VARA 500", "VARA 2750"};

void md5(char * arg, unsigned char * checksum);

//	One time passwords (CommonCode.c). Used by the sysop "password" command
//	and the RADIO AUTH script line. Pure functions, copied unchanged.

Dll VOID APIENTRY CreateOneTimePassword(char * Password, char * KeyPhrase, int TimeOffset)
{
	time_t NOW = time(NULL);
	UCHAR Hash[16];
	char Key[1000];
	int i, chr;

	NOW = NOW/30 + TimeOffset;				// Only Change every 30 secs

	sprintf(Key, "%s%x", KeyPhrase, (int)NOW);

	md5(Key, Hash);

	for (i=0; i<16; i++)
	{
		chr = (Hash[i] & 31);
		if (chr > 9) chr += 7;

		Password[i] = chr + 48;
	}

	Password[16] = 0;
	return;
}

Dll BOOL APIENTRY CheckOneTimePassword(char * Password, char * KeyPhrase)
{
	char CheckPassword[17];
	int Offsets[10] = {0, -1, 1, -2, 2, -3, 3, -4, 4};
	int i, Pass = 0;

	if (strlen(Password) < 16)
		Pass = atoi(Password);

	for (i = 0; i < 9; i++)
	{
		CreateOneTimePassword(CheckPassword, KeyPhrase, Offsets[i]);

		if (strlen(Password) < 16)
		{
			// Using a numeric extract

			long long Val;

			memcpy(&Val, CheckPassword, 8);
			Val %= 1000000;

			if (Pass == Val)
				return TRUE;
		}
		else
			if (memcmp(Password, CheckPassword, 16) == 0)
				return TRUE;
	}

	return FALSE;
}

//	Node monitor (the BBS log copied to the node's trace). No node to copy to.

int BPQTRACE(MESSAGE * Msg, BOOL APRS)
{
	return 0;
}

//	APRS (WebMail's "send APRS message", position for the BBS map)

Dll BOOL APIENTRY APISendAPRSMessage(char * Text, char * ToCall)
{
	return FALSE;
}

BOOL APIENTRY GetAPRSLatLon(double * PLat, double * PLon)
{
	return FALSE;
}

BOOL APIENTRY GetAPRSLatLonString(char * PLat, char * PLon)
{
	return FALSE;
}

//	Multicast mail (Multicast.c, needs the node's MCAST port)

VOID ProcessMCASTLine(ConnectionInfo * conn, struct UserInfo * user, char * Buffer, int MsgLen)
{
}

VOID MCastTimer()
{
}

VOID MCastConTimer(ConnectionInfo * conn)
{
}

int MulticastStatusHTML(char * Reply)
{
	return 0;
}

//	Event programs (Events.c, part of the node's MQTT and event plumbing)

int RunEventProgram(char * Program, char * Param)
{
	return 0;
}

//	Web page helpers that live in the node's HTTPcode.c and TelnetV6.c. The
//	pages themselves are served by pdnweb.c.

static int HexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';

	c = tolower(c);

	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;

	return -1;
}

void UndoTransparency(char * input)
{
	// Decode %xx and + in a form or URL, as HTTPcode.c does, except that a
	// '%' not followed by two hex digits is kept as it is. (HTTPcode.c's
	// version reads past the end of the string on a trailing '%'.)

	char * ptr1, * ptr2;
	char c;

	if (input == NULL)
		return;

	ptr1 = ptr2 = input;

	while ((c = *(ptr1++)))
	{
		if (c == '%' && HexValue(ptr1[0]) >= 0 && HexValue(ptr1[1]) >= 0)
		{
			*(ptr2++) = (HexValue(ptr1[0]) << 4) | HexValue(ptr1[1]);
			ptr1 += 2;
		}
		else if (c == '+')
			*(ptr2++) = 32;
		else
			*(ptr2++) = c;
	}
	*ptr2 = 0;
}

int RefreshWebMailIndex()
{
	return 0;
}

struct HTTPConnectionInfo;

int MailAPIProcessHTTPMessage(struct HTTPConnectionInfo * Session, char * response, char * Method, char * URL, char * request, BOOL LOCAL, char * Param, char * Token)
{
	return 0;
}

//	Packet map / node map reporting (mailapi.c)

void SendBBSDataToPktMap()
{
}
