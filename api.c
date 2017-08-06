/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  General API commands
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "api.h"

void sendAPIResponse(int sock, char type) {
	if(type == APIH)
	{
		// Send header only for full HTTP requests
		ssend(sock,
		      "HTTP/1.0 200 OK\nServer: FTL\nCache-Control: no-cache\nAccess-Control-Allow-Origin: *\n"
				      "Content-Type: application/json\n\n{");
	}
}
