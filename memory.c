/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Global variable definitions and memory reallocation handling
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "shmem.h"

FTLFileNamesStruct FTLfiles = {
	// Default path for config file (regular installations)
	"/etc/pihole/pihole-FTL.conf",
	// Alternative path for config file (snap installations)
	"/var/snap/pihole/common/etc/pihole/pihole-FTL.conf",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

logFileNamesStruct files = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

// Fixed size structs
countersStruct *counters = NULL;
ConfigStruct config;

// Variable size array structs
queriesDataStruct *queries = NULL;
forwardedDataStruct *forwarded = NULL;
clientsDataStruct *clients = NULL;
domainsDataStruct *domains = NULL;
overTimeDataStruct *overTime = NULL;
int **overTimeClientData = NULL;

void memory_check(int which)
{
	switch(which)
	{
		case QUERIES:
			if(counters->queries >= counters->queries_MAX-1)
			{
				// Have to reallocate shared memory
				queries = enlarge_shmem_struct(QUERIES);
				if(queries == NULL)
				{
					logg("FATAL: Memory allocation failed! Exiting");
					exit(EXIT_FAILURE);
				}
			}
		break;
		case FORWARDED:
			if(counters->forwarded >= counters->forwarded_MAX-1)
			{
				// Have to reallocate shared memory
				forwarded = enlarge_shmem_struct(FORWARDED);
				if(forwarded == NULL)
				{
					logg("FATAL: Memory allocation failed! Exiting");
					exit(EXIT_FAILURE);
				}
			}
		break;
		case CLIENTS:
			if(counters->clients >= counters->clients_MAX-1)
			{
				// Have to reallocate shared memory
				clients = enlarge_shmem_struct(CLIENTS);
				if(clients == NULL)
				{
					logg("FATAL: Memory allocation failed! Exiting");
					exit(EXIT_FAILURE);
				}
			}
		break;
		case DOMAINS:
			if(counters->domains >= counters->domains_MAX-1)
			{
				// Have to reallocate shared memory
				domains = enlarge_shmem_struct(DOMAINS);
				if(domains == NULL)
				{
					logg("FATAL: Memory allocation failed! Exiting");
					exit(EXIT_FAILURE);
				}
			}
		break;
		case OVERTIME:
			if(counters->overTime >= counters->overTime_MAX-1)
			{
				// Have to reallocate shared memory
				overTime = enlarge_shmem_struct(OVERTIME);
				if(overTime == NULL)
				{
					logg("FATAL: Memory allocation failed! Exiting");
					exit(EXIT_FAILURE);
				}
			}
		break;
		default:
			/* That cannot happen */
			logg("Fatal error in memory_check(%i)", which);
			exit(EXIT_FAILURE);
		break;
	}
}

void validate_access(const char * name, int pos, bool testmagic, int line, const char * function, const char * file)
{
	int limit = 0;
	if(name[0] == 'c') limit = counters->clients_MAX;
	else if(name[0] == 'd') limit = counters->domains_MAX;
	else if(name[0] == 'q') limit = counters->queries_MAX;
	else if(name[0] == 'o') limit = counters->overTime_MAX;
	else if(name[0] == 'f') limit = counters->forwarded_MAX;
	else { logg("Validator error (range)"); killed = 1; }

	if(pos >= limit || pos < 0)
	{
		logg("FATAL ERROR: Trying to access %s[%i], but maximum is %i", name, pos, limit);
		logg("             found in %s() (%s:%i)", function, file, line);
	}
	// Don't test magic byte if detected potential out-of-bounds error
	else if(testmagic)
	{
		unsigned char magic = 0x00;
		if(name[0] == 'c') magic = clients[pos].magic;
		else if(name[0] == 'd') magic = domains[pos].magic;
		else if(name[0] == 'q') magic = queries[pos].magic;
		else if(name[0] == 'o') magic = overTime[pos].magic;
		else if(name[0] == 'f') magic = forwarded[pos].magic;
		else { logg("Validator error (magic byte)"); killed = 1; }
		if(magic != MAGICBYTE)
		{
			logg("FATAL ERROR: Trying to access %s[%i], but magic byte is %x", name, pos, magic);
			logg("             found in %s() (%s:%i)", function, file, line);
		}
	}
}

// The special memory handling routines have to be the last ones in this source file
// as we restore the original definition of the strdup, free, calloc, and realloc
// functions in here, i.e. if anything extra would come below these lines, it would
// not be protected by our (error logging) functions!

#undef strdup
char *FTLstrdup(const char *src, const char * file, const char * function, int line)
{
	// The FTLstrdup() function returns a pointer to a new string which is a
	// duplicate of the string s. Memory for the new string is obtained with
	// calloc(3), and can be freed with free(3).
	if(src == NULL)
	{
		logg("WARN: Trying to copy a NULL string in %s() (%s:%i)", function, file, line);
		return NULL;
	}
	size_t len = strlen(src);
	char *dest = calloc(len+1, sizeof(char));
	if(dest == NULL)
	{
		logg("FATAL: Memory allocation failed in %s() (%s:%i)", function, file, line);
		return NULL;
	}
	// Use memcpy as memory areas cannot overlap
	memcpy(dest, src, len);
	dest[len] = '\0';

	return dest;
}

#undef calloc
void *FTLcalloc(size_t nmemb, size_t size, const char * file, const char * function, int line)
{
	// The FTLcalloc() function allocates memory for an array of nmemb elements
	// of size bytes each and returns a pointer to the allocated memory. The
	// memory is set to zero. If nmemb or size is 0, then calloc() returns
	// either NULL, or a unique pointer value that can later be successfully
	// passed to free().
	void *ptr = calloc(nmemb, size);
	if(ptr == NULL)
		logg("FATAL: Memory allocation (%u x %u) failed in %s() (%s:%i)",
		     nmemb, size, function, file, line);

	return ptr;
}

#undef realloc
void *FTLrealloc(void *ptr_in, size_t size, const char * file, const char * function, int line)
{
	// The FTLrealloc() function changes the size of the memory block pointed to
	// by ptr to size bytes. The contents will be unchanged in the range from
	// the start of the region up to the minimum of the old and new sizes. If
	// the new size is larger than the old size, the added memory will not be
	// initialized. If ptr is NULL, then the call is equivalent to malloc(size),
	// for all values of size; if size is equal to zero, and ptr is
	// not NULL, then the call is equivalent to free(ptr). Unless ptr is
	// NULL, it must have been returned by an earlier call to malloc(), cal‐
	// loc() or realloc(). If the area pointed to was moved, a free(ptr) is
	// done.
	void *ptr_out = realloc(ptr_in, size);
	if(ptr_out == NULL)
		logg("FATAL: Memory reallocation (%p -> %u) failed in %s() (%s:%i)",
		     ptr_in, size, function, file, line);

	return ptr_out;
}

#undef free
void FTLfree(void *ptr, const char * file, const char * function, int line)
{
	// The free() function frees the memory space pointed  to  by  ptr,  which
	// must  have  been  returned by a previous call to malloc(), calloc(), or
	// realloc().  Otherwise, or if free(ptr) has already been called  before,
	// undefined behavior occurs.  If ptr is NULL, no operation is performed.
	if(ptr == NULL)
		logg("FATAL: Trying to free NULL pointer in %s() (%s:%i)", function, file, line);

	// We intentionally run free() nevertheless to see the crash in the debugger
	free(ptr);
}
