/*
*
* Copyright 2015 Telefonica Investigacion y Desarrollo, S.A.U
*
* This file is part of Orion Context Broker.
*
* Orion Context Broker is free software: you can redistribute it and/or
* modify it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* Orion Context Broker is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero
* General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with Orion Context Broker. If not, see http://www.gnu.org/licenses/.
*
* For those usages not covered by this license please contact with
* iot_support at tid dot es
*
* Author: Ken Zangelin
*/
#include <time.h>
#include <semaphore.h>
#include <string>
#include <vector>

#include "logMsg/logMsg.h"
#include "logMsg/traceLevels.h"

#include "common/clockFunctions.h"
#include "common/string.h"
#include "mongoBackend/mongoConnectionPool.h"
#include "mongoBackend/MongoGlobal.h"



/* ****************************************************************************
*
* RECONNECT_RETRIES - number of retries after connect
* RECONNECT_DELAY   - number of millisecs to sleep between retries
*/
#define RECONNECT_RETRIES 100
#define RECONNECT_DELAY   1000  // One second



/* ****************************************************************************
*
* MongoConnection - 
*/
typedef struct MongoConnection
{
  DBClientBase*  connection;
  bool           free;
} MongoConnection;



/* ****************************************************************************
*
* globals - 
*/
static MongoConnection* connectionPool     = NULL;
static int              connectionPoolSize = 0;
static sem_t            connectionPoolSem;
static sem_t            connectionSem;
static struct timespec  semWaitingTime     = { 0, 0 };
static bool             semStatistics      = false;
static int              mongoVersionMayor = -1;
static int              mongoVersionMinor = -1;



/* ****************************************************************************
*
* mongoVersionGet - 
*/
void mongoVersionGet(int* mayor, int* minor)
{
  *mayor = mongoVersionMayor;
  *minor = mongoVersionMinor;
}



/* ****************************************************************************
*
* mongoConnect - 
*
* Default value for writeConcern == 1 (0: unacknowledged, 1: acknowledged)
*/
static DBClientBase* mongoConnect
(
  const char*  host,
  const char*  db,
  const char*  rplSet,
  const char*  username,
  const char*  passwd,
  bool         multitenant,
  int          writeConcern,
  double       timeout
)
{
  std::string   err;
  DBClientBase* connection = NULL;

  LM_T(LmtMongo, ("Connection info: dbName='%s', rplSet='%s', timeout=%f", db, rplSet, timeout));

  bool connected     = false;
  int  retries       = RECONNECT_RETRIES;

  if (strlen(rplSet) == 0)
  {
    // Setting the first argument to true is to use autoreconnect
    connection = new DBClientConnection(true);

    //
    // Not sure of how to generalize the following code,
    // given that DBClientBase class doesn't have a common connect() method (surprisingly)
    //
    for (int tryNo = 0; tryNo < retries; ++tryNo)
    {
      if ( ((DBClientConnection*)connection)->connect(host, err))
      {
        connected = true;
        break;
      }

      if (tryNo == 0)
      {
        LM_E(("Database Startup Error (cannot connect to mongo - doing %d retries with a %d microsecond interval)",
              retries,
              RECONNECT_DELAY));
      }
      else
      {
        LM_T(LmtMongo, ("Try %d connecting to mongo failed", tryNo));
      }

      usleep(RECONNECT_DELAY * 1000);  // usleep accepts microseconds
    }
  }
  else
  {
    LM_T(LmtMongo, ("Using replica set %s", rplSet));

    // autoReconnect is always on for DBClientReplicaSet connections.
    std::vector<std::string>  hostTokens;
    int components = stringSplit(host, ',', hostTokens);

    std::vector<HostAndPort> rplSetHosts;
    for (int ix = 0; ix < components; ix++)
    {
      LM_T(LmtMongo, ("rplSet host <%s>", hostTokens[ix].c_str()));
      rplSetHosts.push_back(HostAndPort(hostTokens[ix]));
    }

    connection = new DBClientReplicaSet(rplSet, rplSetHosts, timeout);

    //
    // Not sure of to generalize the following code,
    // given that DBClientBase class hasn't a common connect() method (surprisingly)
    //
    for (int tryNo = 0; tryNo < retries; ++tryNo)
    {
      if ( ((DBClientReplicaSet*)connection)->connect())
      {
        connected = true;
        break;
      }

      if (tryNo == 0)
      {
        LM_E(("Database Startup Error (cannot connect to mongo - doing %d retries with a %d microsecond interval)",
              retries,
              RECONNECT_DELAY));
      }
      else
      {
        LM_T(LmtMongo, ("Try %d connecting to mongo failed", tryNo));
      }

      usleep(RECONNECT_DELAY * 1000);  // usleep accepts microseconds
    }
  }

  if (connected == false)
  {
    LM_E(("Database Error (connection failed, after %d retries: '%s')", retries, err.c_str()));
    return NULL;
  }

  LM_I(("Successful connection to database"));

  //
  // WriteConcern
  //
  mongo::WriteConcern writeConcernCheck;

  //
  // In the legacy driver, writeConcern is no longer an int, but a class.
  // We need a small conversion step here
  //
  mongo::WriteConcern wc = writeConcern == 1 ? mongo::WriteConcern::acknowledged : mongo::WriteConcern::unacknowledged;

  connection->setWriteConcern((mongo::WriteConcern) wc);
  writeConcernCheck = (mongo::WriteConcern) connection->getWriteConcern();

  if (writeConcernCheck.nodes() != wc.nodes())
  {
    LM_E(("Database Error (Write Concern not set as desired)"));
    return NULL;
  }
  LM_T(LmtMongo, ("Active DB Write Concern mode: %d", writeConcern));

  /* Authentication is different depending if multiservice is used or not. In the case of not
   * using multiservice, we authenticate in the single-service database. In the case of using
   * multiservice, it isn't a default database that we know at contextBroker start time (when
   * this connection function is invoked) so we authenticate on the admin database, which provides
   * access to any database */
  if (multitenant)
  {
    if (strlen(username) != 0 && strlen(passwd) != 0)
    {
      if (!connection->auth("admin", std::string(username), std::string(passwd), err))
      {
        LM_E(("Database Startup Error (authentication: db='admin', username='%s', password='*****': %s)",
              username,
              err.c_str()));

        return NULL;
      }
    }
  }
  else
  {
    if (strlen(db) != 0 && strlen(username) != 0 && strlen(passwd) != 0)
    {
      if (!connection->auth(std::string(db), std::string(username), std::string(passwd), err))
      {
        LM_E(("Database Startup Error (authentication: db='%s', username='%s', password='*****': %s)",
              db,
              username,
              err.c_str()));

        return NULL;
      }
    }
  }

  /* Get mongo version with the 'buildinfo' command */
  BSONObj result;
  std::string extra;
  connection->runCommand("admin", BSON("buildinfo" << 1), result);
  std::string versionString = std::string(result.getStringField("version"));
  if (!versionParse(versionString, mongoVersionMayor, mongoVersionMinor, extra))
  {
    LM_E(("Database Startup Error (invalid version format: %s)", versionString.c_str()));
    return NULL;
  }
  LM_T(LmtMongo, ("mongo version server: %s (mayor: %d, minor: %d, extra: %s)",
                  versionString.c_str(),
                  mongoVersionMayor,
                  mongoVersionMinor,
                  extra.c_str()));

  return connection;
}



/* ****************************************************************************
*
* mongoConnectionPoolInit - 
*/
int mongoConnectionPoolInit
(
  const char*  host,
  const char*  db,
  const char*  rplSet,
  const char*  username,
  const char*  passwd,
  bool         multitenant,
  double       timeout,
  int          writeConcern,
  int          poolSize,
  bool         semTimeStat
)
{
  //
  // Create the pool
  //
  connectionPool  = (MongoConnection*) calloc(sizeof(MongoConnection), poolSize);
  if (connectionPool == NULL)
  {
    LM_E(("Runtime Error (insufficient memory to create connection pool of %d connections)", poolSize));
    return -1;
  }
  connectionPoolSize = poolSize;

  //
  // Initialize (connect) the pool
  //
  for (int ix = 0; ix < connectionPoolSize; ++ix)
  {
    connectionPool[ix].free       = true;
    connectionPool[ix].connection =
        mongoConnect(host, db, rplSet, username, passwd, multitenant, writeConcern, timeout);
  }

  //
  // Set up the semaphore protecting the pool itself (connectionPoolSem)
  //
  int r = sem_init(&connectionPoolSem, 0, 1);

  if (r != 0)
  {
    LM_E(("Runtime Error (cannot create connection pool semaphore)"));
    return -1;
  }

  //
  // Set up the semaphore protecting the set of connections of the pool (connectionSem)
  // Note that this is a counting semaphore, initialized to connectionPoolSize.
  //
  r = sem_init(&connectionSem, 0, connectionPoolSize);
  if (r != 0)
  {
    LM_E(("Runtime Error (cannot create connection semaphore-set)"));
    return -1;
  }

  // Measure accumulated semaphore waiting time?
  semStatistics = semTimeStat;

  return 0;
}



/* ****************************************************************************
*
* mongoPoolConnectionGet - 
*
* There are two semaphores to get a connection.
* - One binary semaphore that protects the connection-vector itself (connectionPoolSem)
* - One counting semaphore that makes the caller wait until there is at least one free connection (connectionSem)
*
* There is a limited number of connections and the first thing to do is to wait for a connection
* to become avilable (any of the N connections in the pool) - this is done waiting on the counting semaphore that is 
* initialized with "POOL SIZE" - meaning the semaphore can be taken N times if the pool size is N.
* 
* Once a connection is free, 'sem_wait(&connectionSem)' returns and we now have to take the semaphore 
* that protects for pool itself (we *are* going to modify the vector of the pool - can only do it in
* one thread at a time ...)
*
* After taking a connection, the semaphore 'connectionPoolSem' is freed, as all modifications to the connection pool
* have finished.
* The other semaphore however, 'connectionSem', is kept and it is not freed until we finish using the connection.
*
* The function mongoPoolConnectionRelease releases the counting semaphore 'connectionSem'.
* Very important to call the function 'mongoPoolConnectionRelease' after finishing using the connection !
*
*/
DBClientBase* mongoPoolConnectionGet(void)
{
  DBClientBase*    connection = NULL;
  struct timespec  startTime;
  struct timespec  endTime;
  struct timespec  diffTime;

  if (semStatistics)
  {
    clock_gettime(CLOCK_REALTIME, &startTime);
  }

  sem_wait(&connectionSem);
  sem_wait(&connectionPoolSem);

  if (semStatistics)
  {
    clock_gettime(CLOCK_REALTIME, &endTime);

    clock_difftime(&endTime, &startTime, &diffTime);
    clock_addtime(&semWaitingTime, &diffTime);
  }

  for (int ix = 0; ix < connectionPoolSize; ++ix)
  {
    if (connectionPool[ix].free == true)
    {
      connectionPool[ix].free = false;
      connection = connectionPool[ix].connection;
      break;
    }
  }

  sem_post(&connectionPoolSem);

  return connection;
}



/* ****************************************************************************
*
* mongoPoolConnectionRelease - 
*/
void mongoPoolConnectionRelease(DBClientBase* connection)
{
  sem_wait(&connectionPoolSem);

  for (int ix = 0; ix < connectionPoolSize; ++ix)
  {
    if (connectionPool[ix].connection == connection)
    {
      connectionPool[ix].free = true;
      sem_post(&connectionSem);
      break;
    }
  }

  sem_post(&connectionPoolSem);
}



/* ****************************************************************************
*
* mongoPoolConnectionSemWaitingTimeGet - 
*/
char* mongoPoolConnectionSemWaitingTimeGet(char* buf, int bufLen)
{
  if (semStatistics)
  {
    snprintf(buf, bufLen, "%lu.%09d", semWaitingTime.tv_sec, (int) semWaitingTime.tv_nsec);
  }
  else
  {
    snprintf(buf, bufLen, "Disabled");
  }

  return buf;
}



/* ****************************************************************************
*
* mongoPoolConnectionSemWaitingTimeReset - 
*/
void mongoPoolConnectionSemWaitingTimeReset(void)
{
  semWaitingTime.tv_sec  = 0;
  semWaitingTime.tv_nsec = 0;
}



