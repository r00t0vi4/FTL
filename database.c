/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Database routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "shmem.h"

sqlite3 *db;
bool database = false;
bool DBdeleteoldqueries = false;
long int lastdbindex = 0;
long int lastDBimportedtimestamp = 0;

pthread_mutex_t dblock;

// TABLE ftl
enum { DB_VERSION, DB_LASTTIMESTAMP, DB_FIRSTCOUNTERTIMESTAMP };
// TABLE counters
enum { DB_TOTALQUERIES, DB_BLOCKEDQUERIES };

bool db_set_counter(unsigned int ID, int value);
bool db_set_FTL_property(unsigned int ID, int value);
int db_get_FTL_property(unsigned int ID);

void check_database(int rc)
{
	// We will retry if the database is busy at the moment
	// However, we won't retry if any other error happened
	// and - instead - disable the database functionality
	// altogether in FTL (setting database to false)
	if(rc != SQLITE_OK &&
	   rc != SQLITE_DONE &&
	   rc != SQLITE_ROW &&
	   rc != SQLITE_BUSY)
	{
		database = false;
	}
}

void dbclose(void)
{
	int rc = sqlite3_close(db);
	// Report any error
	if( rc )
		logg("dbclose() - SQL error (%i): %s", rc, sqlite3_errmsg(db));

	// Unlock mutex on the database
	pthread_mutex_unlock(&dblock);
}

double get_db_filesize(void)
{
	struct stat st;
	if(stat(FTLfiles.db, &st) != 0)
	{
		// stat() failed (maybe the DB file does not exist?)
		return 0;
	}
	return 1e-6*st.st_size;
}

bool dbopen(void)
{
	pthread_mutex_lock(&dblock);
	int rc = sqlite3_open_v2(FTLfiles.db, &db, SQLITE_OPEN_READWRITE, NULL);
	if( rc ){
		logg("dbopen() - SQL error (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return false;
	}

	return true;
}

bool dbquery(const char *format, ...)
{
	char *zErrMsg = NULL;
	va_list args;

	va_start(args, format);
	char *query = sqlite3_vmprintf(format, args);
	va_end(args);

	if(query == NULL)
	{
		logg("Memory allocation failed in dbquery()");
		return false;
	}

	int rc = sqlite3_exec(db, query, NULL, NULL, &zErrMsg);

	if( rc != SQLITE_OK ){
		logg("dbquery(%s) - SQL error (%i): %s", query, rc, zErrMsg);
		sqlite3_free(zErrMsg);
		check_database(rc);
		return false;
	}

	sqlite3_free(query);

	return true;

}

bool create_counter_table(void)
{
	bool ret;
	// Create FTL table in the database (holds properties like database version, etc.)
	ret = dbquery("CREATE TABLE counters ( id INTEGER PRIMARY KEY NOT NULL, value INTEGER NOT NULL );");
	if(!ret){ dbclose(); return false; }

	// ID 0 = total queries
	ret = db_set_counter(DB_TOTALQUERIES, 0);
	if(!ret){ dbclose(); return false; }

	// ID 1 = total blocked queries
	ret = db_set_counter(DB_BLOCKEDQUERIES, 0);
	if(!ret){ dbclose(); return false; }

	// Time stamp of creation of the counters database
	ret = db_set_FTL_property(DB_FIRSTCOUNTERTIMESTAMP, time(NULL));
	if(!ret){ dbclose(); return false; }

	// Update database version to 2
	ret = db_set_FTL_property(DB_VERSION, 2);
	if(!ret){ dbclose(); return false; }

	return true;
}

bool db_create(void)
{
	bool ret;
	int rc = sqlite3_open_v2(FTLfiles.db, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if( rc ){
		logg("db_create() - SQL error (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return false;
	}
	// Create Queries table in the database
	ret = dbquery("CREATE TABLE queries ( id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, type INTEGER NOT NULL, status INTEGER NOT NULL, domain TEXT NOT NULL, client TEXT NOT NULL, forward TEXT );");
	if(!ret){ dbclose(); return false; }
	// Add an index on the timestamps (not a unique index!)
	ret = dbquery("CREATE INDEX idx_queries_timestamps ON queries (timestamp);");
	if(!ret){ dbclose(); return false; }
	// Create FTL table in the database (holds properties like database version, etc.)
	ret = dbquery("CREATE TABLE ftl ( id INTEGER PRIMARY KEY NOT NULL, value BLOB NOT NULL );");
	if(!ret){ dbclose(); return false; }

	// DB version 2
	ret = dbquery("INSERT INTO ftl (ID,VALUE) VALUES(%i,2);", DB_VERSION);
	if(!ret){ dbclose(); return false; }

	// Most recent timestamp initialized to 00:00 1 Jan 1970
	ret = dbquery("INSERT INTO ftl (ID,VALUE) VALUES(%i,0);", DB_LASTTIMESTAMP);
	if(!ret){ dbclose(); return false; }

	// Create counter table
	if(!create_counter_table())
		return false;

	return true;
}

void db_init(void)
{
	// First check if the user doesn't want to use the database and set an
	// empty string as file name in FTL's config file
	if(FTLfiles.db == NULL || strlen(FTLfiles.db) == 0)
	{
		database = false;
		return;
	}

	int rc = sqlite3_open_v2(FTLfiles.db, &db, SQLITE_OPEN_READWRITE, NULL);
	if( rc ){
		logg("db_init() - Cannot open database (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);

		logg("Creating new (empty) database");
		if (!db_create())
		{
			logg("Database not available");
			database = false;
			return;
		}
	}

	// Test DB version and see if we need to upgrade the database file
	int dbversion = db_get_FTL_property(DB_VERSION);
	if(dbversion < 1)
	{
		logg("Database version incorrect, database not available");
		database = false;
		return;
	}
	else if(dbversion < 2)
	{
		// Database is still in version 1
		// Update to version 2 and create counters table
		if (!create_counter_table())
		{
			logg("Counter table not initialized, database not available");
			database = false;
			return;
		}
	}

	// Close database to prevent having it opened all time
	// we already closed the database when we returned earlier
	sqlite3_close(db);

	if (pthread_mutex_init(&dblock, NULL) != 0)
	{
		logg("FATAL: DB mutex init failed\n");
		// Return failure
		exit(EXIT_FAILURE);
	}

	logg("Database successfully initialized");
	database = true;
}

int db_get_FTL_property(unsigned int ID)
{
	int rc, ret = 0;
	sqlite3_stmt* dbstmt;
	char *querystring = NULL;

	// Prepare SQL statement
	ret = asprintf(&querystring, "SELECT VALUE FROM ftl WHERE id = %u;",ID);

	if(querystring == NULL || ret < 0)
	{
		logg("Memory allocation failed in db_get_FTL_property, not saving query with ID = %u (%i)", ID, ret);
		return false;
	}

	rc = sqlite3_prepare(db, querystring, -1, &dbstmt, NULL);
	if( rc ){
		logg("db_get_FTL_property() - SQL error prepare (%i): %s", rc, sqlite3_errmsg(db));
		logg("Query: \"%s\"", querystring);
		dbclose();
		check_database(rc);
		return -1;
	}
	free(querystring);

	// Evaluate SQL statement
	rc = sqlite3_step(dbstmt);
	if( rc != SQLITE_ROW ){
		logg("db_get_FTL_property() - SQL error step (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return -1;
	}

	int result = sqlite3_column_int(dbstmt, 0);

	sqlite3_finalize(dbstmt);

	return result;
}

bool db_set_FTL_property(unsigned int ID, int value)
{
	return dbquery("INSERT OR REPLACE INTO ftl (id, value) VALUES ( %u, %i );", ID, value);
}

bool db_set_counter(unsigned int ID, int value)
{
	return dbquery("INSERT OR REPLACE INTO counters (id, value) VALUES ( %u, %i );", ID, value);
}

bool db_update_counters(int total, int blocked)
{
	if(!dbquery("UPDATE counters SET value = value + %i WHERE id = %i;", total, DB_TOTALQUERIES))
		return false;
	if(!dbquery("UPDATE counters SET value = value + %i WHERE id = %i;", blocked, DB_BLOCKEDQUERIES))
		return false;
	return true;
}

int number_of_queries_in_DB(void)
{
	sqlite3_stmt* stmt;

	// Count number of rows using the index timestamp is faster than select(*)
	int rc = sqlite3_prepare_v2(db, "SELECT COUNT(timestamp) FROM queries", -1, &stmt, NULL);
	if( rc ){
		logg("number_of_queries_in_DB() - SQL error prepare (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return -1;
	}

	rc = sqlite3_step(stmt);
	if( rc != SQLITE_ROW ){
		logg("number_of_queries_in_DB() - SQL error step (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return -1;
	}

	int result = sqlite3_column_int(stmt, 0);

	sqlite3_finalize(stmt);

	return result;
}

static sqlite3_int64 last_ID_in_DB(void)
{
	sqlite3_stmt* stmt;

	int rc = sqlite3_prepare_v2(db, "SELECT MAX(ID) FROM queries", -1, &stmt, NULL);
	if( rc ){
		logg("last_ID_in_DB() - SQL error prepare (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return -1;
	}

	rc = sqlite3_step(stmt);
	if( rc != SQLITE_ROW ){
		logg("last_ID_in_DB() - SQL error step (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return -1;
	}

	sqlite3_int64 result = sqlite3_column_int64(stmt, 0);

	sqlite3_finalize(stmt);

	return result;
}

int get_number_of_queries_in_DB(void)
{
	int result = -1;

	if(!dbopen())
	{
		logg("Failed to open DB in get_number_of_queries_in_DB()");
		return -2;
	}

	result = number_of_queries_in_DB();

	// Close database
	dbclose();

	return result;
}

void save_to_DB(void)
{
	// Don't save anything to the database if in PRIVACY_NOSTATS mode
	if(config.privacylevel >= PRIVACY_NOSTATS)
		return;

	// Start database timer
	if(debug) timer_start(DATABASE_WRITE_TIMER);

	// Open database
	if(!dbopen())
	{
		logg("save_to_DB() - failed to open DB");
		return;
	}

	unsigned int saved = 0, saved_error = 0;
	long int i;
	sqlite3_stmt* stmt;

	// Get last ID stored in the database
	sqlite3_int64 lastID = last_ID_in_DB();

	bool ret = dbquery("BEGIN TRANSACTION");
	if(!ret)
	{
		logg("save_to_DB() - unable to begin transaction (%i): %s", ret, sqlite3_errmsg(db));
		dbclose();
		return;
	}

	int rc = sqlite3_prepare_v2(db, "INSERT INTO queries VALUES (NULL,?,?,?,?,?,?)", -1, &stmt, NULL);
	if( rc )
	{
		logg("save_to_DB() - error in preparing SQL statement (%i): %s", ret, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return;
	}

	int total = 0, blocked = 0;
	time_t currenttimestamp = time(NULL);
	time_t newlasttimestamp = 0;
	for(i = 0; i < counters->queries; i++)
	{
		validate_access("queries", i, true, __LINE__, __FUNCTION__, __FILE__);
		if(queries[i].db != 0)
		{
			// Skip, already saved in database
			continue;
		}

		if(!queries[i].complete && queries[i].timestamp > currenttimestamp-2)
		{
			// Break if a brand new query (age < 2 seconds) is not yet completed
			// giving it a chance to be stored next time
			break;
		}

		// Memory checks
		validate_access("queries", i, true, __LINE__, __FUNCTION__, __FILE__);

		if(queries[i].privacylevel >= PRIVACY_MAXIMUM)
		{
			// Skip, we never store nor count queries recorded
			// while have been in maximum privacy mode in the database
			continue;
		}

		// TIMESTAMP
		sqlite3_bind_int(stmt, 1, queries[i].timestamp);

		// TYPE
		sqlite3_bind_int(stmt, 2, queries[i].type);

		// STATUS
		sqlite3_bind_int(stmt, 3, queries[i].status);

		// DOMAIN
		char *domain = getDomainString(i);
		sqlite3_bind_text(stmt, 4, domain, -1, SQLITE_TRANSIENT);

		// CLIENT
		char *client = getClientIPString(i);
		sqlite3_bind_text(stmt, 5, client, -1, SQLITE_TRANSIENT);

		// FORWARD
		if(queries[i].status == QUERY_FORWARDED && queries[i].forwardID > -1)
		{
			validate_access("forwarded", queries[i].forwardID, true, __LINE__, __FUNCTION__, __FILE__);
			sqlite3_bind_text(stmt, 6, getstr(forwarded[queries[i].forwardID].ippos), -1, SQLITE_TRANSIENT);
		}
		else
		{
			sqlite3_bind_null(stmt, 6);
		}

		// Step and check if successful
		rc = sqlite3_step(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_reset(stmt);

		if( rc != SQLITE_DONE ){
			logg("save_to_DB() - SQL error (%i): %s", rc, sqlite3_errmsg(db));
			saved_error++;
			if(saved_error < 3)
			{
				continue;
			}
			else
			{
				logg("save_to_DB() - exiting due to too many errors");
				break;
			}
			// Check this error message
			check_database(rc);
		}

		saved++;
		// Mark this query as saved in the database by setting the corresponding ID
		queries[i].db = ++lastID;

		// Total counter information (delta computation)
		total++;
		if(queries[i].status == QUERY_GRAVITY ||
		   queries[i].status == QUERY_BLACKLIST ||
		   queries[i].status == QUERY_WILDCARD ||
		   queries[i].status == QUERY_EXTERNAL_BLOCKED)
			blocked++;

		// Update lasttimestamp variable with timestamp of the latest stored query
		if(queries[i].timestamp > newlasttimestamp)
			newlasttimestamp = queries[i].timestamp;
	}

	// Finish prepared statement
	ret = dbquery("END TRANSACTION");
	int ret2 = sqlite3_finalize(stmt);
	if(!ret || ret2 != SQLITE_OK){ dbclose(); return; }

	// Store index for next loop interation round and update last time stamp
	// in the database only if all queries have been saved successfully
	if(saved_error == 0)
	{
		lastdbindex = i;
		db_set_FTL_property(DB_LASTTIMESTAMP, newlasttimestamp);
	}

	// Update total counters in DB
	if(!db_update_counters(total, blocked))
	{
		dbclose();
		return;
	}

	// Close database
	dbclose();

	if(debug)
	{
		logg("Notice: Queries stored in DB: %u (took %.1f ms, last SQLite ID %llu)", saved, timer_elapsed_msec(DATABASE_WRITE_TIMER), lastID);
		if(saved_error > 0)
			logg("        There are queries that have not been saved");
	}
}

void delete_old_queries_in_DB(void)
{
	// Open database
	if(!dbopen())
	{
		logg("Failed to open DB in delete_old_queries_in_DB()");
		return;
	}

	int timestamp = time(NULL) - config.maxDBdays * 86400;

	if(!dbquery("DELETE FROM queries WHERE timestamp <= %i", timestamp))
	{
		dbclose();
		logg("delete_old_queries_in_DB(): Deleting queries due to age of entries failed!");
		database = true;
		return;
	}

	// Get how many rows have been affected (deleted)
	int affected = sqlite3_changes(db);

	// Print final message only if there is a difference
	if(debug || affected)
		logg("Notice: Database size is %.2f MB, deleted %i rows", get_db_filesize(), affected);

	// Close database
	dbclose();

	// Re-enable database actions
	database = true;
}

int lastDBsave = 0;
void *DB_thread(void *val)
{
	// Set thread name
	prctl(PR_SET_NAME,"database",0,0,0);

	// Save timestamp as we do not want to store immediately
	// to the database
	lastDBsave = time(NULL) - time(NULL)%config.DBinterval;

	while(!killed && database)
	{
		if(time(NULL) - lastDBsave >= config.DBinterval)
		{
			// Update lastDBsave timer
			lastDBsave = time(NULL) - time(NULL)%config.DBinterval;

			// Lock FTL's data structures, since it is
			// likely that they will be changed here
			lock_shm();

			// Save data to database
			save_to_DB();

			// Release data lock
			unlock_shm();

			// Check if GC should be done on the database
			if(DBdeleteoldqueries)
			{
				// No thread locks needed
				delete_old_queries_in_DB();
				DBdeleteoldqueries = false;
			}
		}
		sleepms(100);
	}

	return NULL;
}

// Get most recent 24 hours data from long-term database
void read_data_from_DB(void)
{
	// Don't try to load anything to the database if in PRIVACY_NOSTATS mode
	if(config.privacylevel >= PRIVACY_NOSTATS)
		return;

	// Open database file
	if(!dbopen())
	{
		logg("read_data_from_DB() - Failed to open DB");
		return;
	}

	// Prepare request
	char *rstr = NULL;
	// Get time stamp 24 hours in the past
	time_t now = time(NULL);
	time_t mintime = now - config.maxlogage;
	int rc = asprintf(&rstr, "SELECT * FROM queries WHERE timestamp >= %li", mintime);
	if(rc < 1)
	{
		logg("read_data_from_DB() - Allocation error (%i): %s", rc, sqlite3_errmsg(db));
		return;
	}
	// Log DB query string in debug mode
	if(debug) logg(rstr);

	// Prepare SQLite3 statement
	sqlite3_stmt* stmt;
	rc = sqlite3_prepare_v2(db, rstr, -1, &stmt, NULL);
	if( rc ){
		logg("read_data_from_DB() - SQL error prepare (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return;
	}

	// Loop through returned database rows
	while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
	{
		sqlite3_int64 dbid = sqlite3_column_int64(stmt, 0);
		int queryTimeStamp = sqlite3_column_int(stmt, 1);
		// 1483228800 = 01/01/2017 @ 12:00am (UTC)
		if(queryTimeStamp < 1483228800)
		{
			logg("DB warn: TIMESTAMP should be larger than 01/01/2017 but is %i", queryTimeStamp);
			continue;
		}
		if(queryTimeStamp > now)
		{
			if(debug) logg("DB warn: Skipping query logged in the future (%i)", queryTimeStamp);
			continue;
		}

		int type = sqlite3_column_int(stmt, 2);
		if(type < TYPE_A || type >= TYPE_MAX)
		{
			logg("DB warn: TYPE should not be %i", type);
			continue;
		}
		// Don't import AAAA queries from database if the user set
		// AAAA_QUERY_ANALYSIS=no in pihole-FTL.conf
		if(type == TYPE_AAAA && !config.analyze_AAAA)
		{
			continue;
		}

		int status = sqlite3_column_int(stmt, 3);
		if(status < QUERY_UNKNOWN || status > QUERY_EXTERNAL_BLOCKED)
		{
			logg("DB warn: STATUS should be within [%i,%i] but is %i", QUERY_UNKNOWN, QUERY_EXTERNAL_BLOCKED, status);
			continue;
		}

		const char * domain = (const char *)sqlite3_column_text(stmt, 4);
		if(domain == NULL)
		{
			logg("DB warn: DOMAIN should never be NULL, %i", queryTimeStamp);
			continue;
		}

		const char * client = (const char *)sqlite3_column_text(stmt, 5);
		if(client == NULL)
		{
			logg("DB warn: CLIENT should never be NULL, %i", queryTimeStamp);
			continue;
		}

		// Check if user wants to skip queries coming from localhost
		if(config.ignore_localhost &&
		   (strcmp(client, "127.0.0.1") == 0 || strcmp(client, "::1") == 0))
		{
			continue;
		}

		const char *forwarddest = (const char *)sqlite3_column_text(stmt, 6);
		int forwardID = 0;
		// Determine forwardID only when status == 2 (forwarded) as the
		// field need not to be filled for other query status types
		if(status == QUERY_FORWARDED)
		{
			if(forwarddest == NULL)
			{
				logg("DB warn: FORWARD should not be NULL with status QUERY_FORWARDED, %i", queryTimeStamp);
				continue;
			}
			forwardID = findForwardID(forwarddest, true);
		}

		// Obtain IDs only after filtering which queries we want to keep
		int overTimeTimeStamp = queryTimeStamp - (queryTimeStamp % 600) + 300;
		int timeidx = findOverTimeID(overTimeTimeStamp);
		int domainID = findDomainID(domain);
		int clientID = findClientID(client);

		// Ensure we have enough space in the queries struct
		memory_check(QUERIES);

		// Set index for this query
		int queryIndex = counters->queries;

		// Store this query in memory
		validate_access("overTime", timeidx, true, __LINE__, __FUNCTION__, __FILE__);
		validate_access("queries", queryIndex, false, __LINE__, __FUNCTION__, __FILE__);
		queries[queryIndex].magic = MAGICBYTE;
		queries[queryIndex].timestamp = queryTimeStamp;
		queries[queryIndex].type = type;
		queries[queryIndex].status = status;
		queries[queryIndex].domainID = domainID;
		queries[queryIndex].clientID = clientID;
		queries[queryIndex].forwardID = forwardID;
		queries[queryIndex].timeidx = timeidx;
		queries[queryIndex].db = dbid;
		queries[queryIndex].id = 0;
		queries[queryIndex].complete = true; // Mark as all information is avaiable
		queries[queryIndex].response = 0;
		queries[queryIndex].AD = false;
		lastDBimportedtimestamp = queryTimeStamp;

		// Handle type counters
		if(type >= TYPE_A && type < TYPE_MAX)
		{
			counters->querytype[type-1]++;
			overTime[timeidx].querytypedata[type-1]++;
		}

		// Update overTime data
		overTime[timeidx].total++;

		// Update overTime data structure with the new client
		overTimeClientData[clientID][timeidx]++;

		// Increase DNS queries counter
		counters->queries++;

		// Increment status counters
		switch(status)
		{
			case QUERY_UNKNOWN: // Unknown
				counters->unknown++;
				break;

			case QUERY_GRAVITY: // Blocked by gravity.list
			case QUERY_WILDCARD: // Blocked by regex filter
			case QUERY_BLACKLIST: // Blocked by black.list
			case QUERY_EXTERNAL_BLOCKED: // Blocked by external provider
				counters->blocked++;
				overTime[timeidx].blocked++;
				domains[domainID].blockedcount++;
				clients[clientID].blockedcount++;
				break;

			case QUERY_FORWARDED: // Forwarded
				counters->forwardedqueries++;
				// Update overTime data structure
				break;

			case QUERY_CACHE: // Cached or local config
				counters->cached++;
				// Update overTime data structure
				overTime[timeidx].cached++;
				break;

			default:
				logg("Error: Found unknown status %i in long term database!", status);
				logg("       Timestamp: %i", queryTimeStamp);
				logg("       Continuing anyway...");
				break;
		}
	}
	logg("Imported %i queries from the long-term database", counters->queries);

	if( rc != SQLITE_DONE ){
		logg("read_data_from_DB() - SQL error step (%i): %s", rc, sqlite3_errmsg(db));
		dbclose();
		check_database(rc);
		return;
	}

	// Finalize SQLite3 statement
	sqlite3_finalize(stmt);
	dbclose();
	free(rstr);
}
