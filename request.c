/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Socket request handling routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "api.h"

void process_socket_request(char *client_message, int *sock)
{
	char EOT[2];
	EOT[0] = 0x04;
	EOT[1] = 0x00;
	bool processed = false;
	char type = SOCKET;

	if(command(client_message, ">stats"))
	{
		processed = true;
		getStats(sock, type);
	}
	else if(command(client_message, ">overTime"))
	{
		processed = true;
		getOverTime(sock, type);
	}
	else if(command(client_message, ">top-domains") || command(client_message, ">top-ads"))
	{
		processed = true;
		getTopDomains(client_message, sock, type);
	}
	else if(command(client_message, ">top-clients"))
	{
		processed = true;
		getTopClients(client_message, sock, type);
	}
	else if(command(client_message, ">forward-dest"))
	{
		processed = true;
		getForwardDestinations(client_message, sock, type);
	}
	else if(command(client_message, ">forward-names"))
	{
		processed = true;
		getForwardNames(sock, type);
	}
	else if(command(client_message, ">querytypes"))
	{
		processed = true;
		getQueryTypes(sock, type);
	}
	else if(command(client_message, ">getallqueries"))
	{
		processed = true;
		getAllQueries(client_message, sock, type);
	}
	else if(command(client_message, ">recentBlocked"))
	{
		processed = true;
		getRecentBlocked(client_message, sock, type);
	}
	else if(command(client_message, ">memory"))
	{
		processed = true;
		getMemoryUsage(sock, type);
	}
	else if(command(client_message, ">clientID"))
	{
		processed = true;
		getClientID(sock, type);
	}
	else if(command(client_message, ">ForwardedoverTime"))
	{
		processed = true;
		getForwardDestinationsOverTime(sock, type);
	}
	else if(command(client_message, ">QueryTypesoverTime"))
	{
		processed = true;
		getQueryTypesOverTime(sock, type);
	}
	else if(command(client_message, ">version"))
	{
		processed = true;
		getVersion(sock, type);
	}
	else if(command(client_message, ">dbstats"))
	{
		processed = true;
		getDBstats(sock, type);
	}

	// End of queryable commands
	if(processed)
	{
		// Send EOM
		seom(*sock);
	}

	// Test only at the end if we want to quit or kill
	// so things can be processed before
	if(command(client_message, ">quit") || command(client_message, EOT))
	{
		processed = true;
		if(debugclients)
			logg("Client wants to disconnect, ID: %i",*sock);

		close(*sock);
		*sock = 0;
	}
	else if(command(client_message, ">kill"))
	{
		processed = true;
		logg("FTL killed by client ID: %i",*sock);
		killed = 1;
	}

	if(!processed)
	{
		ssend(*sock,"unknown command: %s\n",client_message);
	}
}

void process_api_request(char *client_message, int *sock, bool header)
{
	char type;
	if(header)
		type = APIH;
	else
		type = API;

	if(command(client_message, "GET /stats/summary"))
	{
		getStats(sock, type);
	}
	else if(command(client_message, "GET /stats/overTime/graph"))
	{
		getOverTime(sock, type);
	}
	else if(command(client_message, "GET /stats/top_domains") || command(client_message, "GET /stats/top_ads"))
	{
		getTopDomains(client_message, sock, type);
	}
	else if(command(client_message, "GET /stats/top_clients"))
	{
		getTopClients(client_message, sock, type);
	}
	else if(command(client_message, "GET /stats/forward_dest") || command(client_message, "GET /stats/forward_destinations"))
	{
		getForwardDestinations(client_message, sock, type);
	}
	else if(command(client_message, "GET /stats/dashboard"))
	{
		getStats(sock, type);
		type = API;
		ssend(*sock, ",");
		getOverTime(sock, type);
		ssend(*sock, ",");
		getTopDomains(client_message, sock, type);
		ssend(*sock, ",");
		getTopClients(client_message, sock, type);
		ssend(*sock, ",");
		getForwardDestinations(client_message, sock, type);
	}
	else if(command(client_message, "GET /stats/query_types"))
	{
		getQueryTypes(sock, type);
	}
	else if(command(client_message, "GET /stats/history"))
	{
		getAllQueries(client_message, sock, type);
	}
	else if(command(client_message, "GET /stats/recent_blocked"))
	{
		getRecentBlocked(client_message, sock, type);
	}
	else if(command(client_message, "GET /stats/overTime/forward_dest"))
	{
		getForwardDestinationsOverTime(sock, type);
	}
	else if(command(client_message, "GET /stats/overTime/query_types"))
	{
		getQueryTypesOverTime(sock, type);
	}
	else if(command(client_message, "GET /dns/whitelist"))
	{
		getList(sock, type, WHITELIST);
	}
	else if(command(client_message, "GET /dns/blacklist"))
	{
		getList(sock, type, BLACKLIST);
	}
	else if(command(client_message, "GET /dns/status"))
	{
		getPiholeStatus(sock, type);
	}
	else if(header)
	{
		ssend(*sock,
		      "HTTP/1.0 404 Not Found\nServer: FTL\nCache-Control: no-cache\nAccess-Control-Allow-Origin: *\n"
		      "Content-Type: application/json\nContent-Length: 21\n\n{status: \"not_found\"");
	}

	ssend(*sock, "}");
}

bool command(char *client_message, const char* cmd)
{
	return strstr(client_message,cmd) != NULL;
}
