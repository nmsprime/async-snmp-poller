/**************************************************************************/ /*
 * NET-SNMP modempoller
 *
 * Originated from the NET-SNMP async demo
 * Hat tip to Niels Baggesen (Niels.Baggesen@uni-c.dk)
 *
 * This program retrieves a set of modems from the cacti database and queries
 * all modems for the given OIDs. Each vendor implements the SNMP protocol
 * differently, so the program needs to check if all tables are correct and if
 * not, request another "batch".
 *
 * The requested OIDs are divided into three segments: non-repeaters for system
 * information, downstream and upstream. For each host a separate session will
 * be created. All requests are handled asynchronously and on response the next
 * batch of the current segment is requested.
 *
 * Christian Schramm (@cschra) and Ole Ernst (@olebowle), 2021
 *
 *****************************************************************************/

/********************************** DEFINES **********************************/
#define _GNU_SOURCE
#define RETRIES 3
#define TIMEOUT 5

/********************************* INCLUDES **********************************/
#include <ctype.h>
#include <libpq-fe.h>
#include <sys/resource.h>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/library/large_fd_set.h>

/****************************** GLOBAL STRUCTURES ****************************/
/* to keep track which segment is sent */
typedef enum pass {
    NON_REP,
    DOWNSTREAM30,
    DOWNSTREAM30A,
    UPSTREAM30,
    UPSTREAM30A,
    DOWNSTREAM31,
    UPSTREAM31,
    DOWNSUB31,
    PROFILE_STATS31,
    FINISH
} pass_t;

long repetitions[FINISH] = {0, 9, 9, 5, 5, 3, 3, 9, 5};

/* a list of variables to query for */
typedef struct oid_s {
    pass_t segment;
    const char *Name;
    oid Oid[MAX_OID_LEN];
    size_t OidLen;
} oid_t;

oid_t oids_single[] = {
    { NON_REP, "1.3.6.1.2.1.1.1" },                       /* SysDescr */
    { NON_REP, "1.3.6.1.2.1.1.3" },                       /* Uptime */
    { NON_REP, "1.3.6.1.2.1.10.127.1.1.5" },              /* DOCSIS */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.2" },          /* Status Code */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.3" },          /* US Power/dBmV */
    { NON_REP, "1.3.6.1.2.1.69.1.3.5" },                  /* Firmware */
    { NON_REP, "1.3.6.1.4.1.4491.2.1.28.1.1" },           /* D3.1 capable */
    { DOWNSTREAM30, "1.3.6.1.2.1.10.127.1.1.1.1.2" },     /* f/Mhz */
    { DOWNSTREAM30, "1.3.6.1.2.1.10.127.1.1.1.1.4" },     /* Modulation */
    { DOWNSTREAM30, "1.3.6.1.2.1.10.127.1.1.1.1.6" },     /* Power/dBmV */
    { DOWNSTREAM30A, "1.3.6.1.2.1.10.127.1.1.4.1.5" },    /* MER/dB */
    { DOWNSTREAM30A, "1.3.6.1.2.1.10.127.1.1.4.1.6" },    /* µR/-dBc */
    { DOWNSTREAM30A, "1.3.6.1.4.1.4491.2.1.20.1.24.1.1" },/* MER/dB */
    { UPSTREAM30, "1.3.6.1.2.1.10.127.1.1.2.1.2" },       /* f/MHz */
    { UPSTREAM30, "1.3.6.1.2.1.10.127.1.1.2.1.3" },       /* Width/MHz */
    { UPSTREAM30A, "1.3.6.1.4.1.4491.2.1.20.1.2.1.1" },   /* Power/dBmV */
    { UPSTREAM30A, "1.3.6.1.4.1.4491.2.1.20.1.2.1.9" },   /* RangingStatus */
    { DOWNSTREAM31, "1.3.6.1.4.1.4491.2.1.27.1.2.5.1.3" },/* avg RxMER */
    { DOWNSTREAM31, "1.3.6.1.4.1.4491.2.1.27.1.2.5.1.4" },/* RxMER std dev */
    { DOWNSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.9.1.3" },
    { DOWNSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.9.1.4" },
    { DOWNSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.9.1.5" },
    { DOWNSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.9.1.7" },
    { UPSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.13.1.2" },
    { UPSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.13.1.3" },
    { UPSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.13.1.4" },
    { UPSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.13.1.6" },
    { UPSTREAM31, "1.3.6.1.4.1.4491.2.1.28.1.13.1.10" },/* RxPow */
    { DOWNSUB31, "1.3.6.1.4.1.4491.2.1.28.1.11.1.2" },  /* OFDM center frequency */
    { DOWNSUB31, "1.3.6.1.4.1.4491.2.1.28.1.11.1.3" },
    { PROFILE_STATS31, "1.3.6.1.4.1.4491.2.1.28.1.10.1.3" },/* total CW */
    { PROFILE_STATS31, "1.3.6.1.4.1.4491.2.1.28.1.10.1.4" },/* cCWE */
    { PROFILE_STATS31, "1.3.6.1.4.1.4491.2.1.28.1.10.1.5" },/* uCWE */
    { PROFILE_STATS31, "1.3.6.1.4.1.4491.2.1.28.1.10.1.6" },/* received bytes */
    { PROFILE_STATS31, "1.3.6.1.4.1.4491.2.1.28.1.10.1.7" },/* received unicast bytes */
    { FINISH }
};

oid_t oids_multiple[] = {
    { NON_REP, "1.3.6.1.2.1.1.1" },                     /* SysDescr */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.3" },        /* US Power (2.0) */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.12" },       /* T3 Timeout */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.13" },       /* T4 Timeout */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.17" },       /* PreEq */
    { NON_REP, "1.3.6.1.2.1.31.1.1.1.6.1" },            /* ifHCInOctets (docsCableMaclayer) */
    { NON_REP, "1.3.6.1.2.1.31.1.1.1.10.1" },           /* ifHCOutOctets (docsCableMaclayer) */
    { DOWNSTREAM30, "1.3.6.1.2.1.10.127.1.1.1.1.6" },     /* Power */
    { DOWNSTREAM30, "1.3.6.1.2.1.10.127.1.1.4.1.3" },     /* Corrected */
    { DOWNSTREAM30, "1.3.6.1.2.1.10.127.1.1.4.1.4" },     /* Uncorrectable */
    { DOWNSTREAM30, "1.3.6.1.2.1.10.127.1.1.4.1.5" },     /* SNR (2.0) */
    { DOWNSTREAM30, "1.3.6.1.2.1.10.127.1.1.4.1.6" },     /* Microreflections */
    { DOWNSTREAM30, "1.3.6.1.4.1.4491.2.1.20.1.24.1.1" }, /* SNR (3.0) */
    { UPSTREAM30, "1.3.6.1.2.1.10.127.1.1.2.1.2" },       /* Frequency */
    { UPSTREAM30, "1.3.6.1.2.1.10.127.1.1.2.1.3" },       /* Bandwidth */
    { UPSTREAM30, "1.3.6.1.4.1.4491.2.1.20.1.2.1.1" },    /* Power (3.0) */
    { UPSTREAM30, "1.3.6.1.4.1.4491.2.1.20.1.2.1.9" },    /* Ranging Status */
    { FINISH }
};

oid_t *oids = NULL;

typedef struct hostContext {                            /* context structure to keep track of the current request */
    struct snmp_session *session;                       /* which host is currently processed */
    long requestIds[FINISH];                            /* the currently valid request id per segment */
    FILE *outputFile;                                   /* to which file should the response be written to */
} hostContext_t;

/****************************** GLOBAL VARIABLES *****************************/
int activeHosts;
int itemCount[FINISH] = { 0 };

/********************************* FUNCTIONS *********************************/
/*
 * Identify the current segment (passed by reference) using the request id and
 * return the last oid of this segment
 *
 * long reqid - the request id found in the modem response
 * long *requestIds - pointer to the array of request ids send to the modem
 * pass_t segment - segment to be identified
 *
 * returns oid_s *
 */
struct oid_s *getSegmentLastOid(long reqid, long *requestIds, pass_t *segment)
{
    int last = -1;

    for ((*segment) = NON_REP; (*segment) < FINISH; (*segment)++) {
        last += itemCount[*segment];

        if (reqid == requestIds[*segment]) {
            return &oids[last];
        }
    }

    return NULL;
}

/*****************************************************************************/
/*
 * Due to the list character of netsnmp_variable_list it is not possible to
 * access the last element directly. This loops through all variables and
 * returns the pointer to the last element
 *
 * netsnmp_variable_list varlist
 *
 * returns netsnmp_variable_list *
 */
netsnmp_variable_list *getLastVarBinding(netsnmp_variable_list *varlist)
{
    while (varlist) {
        if (! varlist->next_variable) {
            return varlist;
        }
        varlist = varlist->next_variable;
    }

    return NULL;
}

/*****************************************************************************/
/*
 * Only called if a table is not fully retrieved. To get the rest of the SNMP
 * Table a new BULK Request is generated with the last number of the last OID.
 * This works only, because the interfaces (segments) have the same indices.
 *
 * hostContext_t *hostContext - pointer to the current hostcontext structure
 * netsnmp_variable_list *varlist - pointer to last oid of previous answer
 * struct oid_s *oid - pointer to first oid to send for the new request
 *
 * returns int
 */
int sendNextBulkRequest(hostContext_t *hostContext, netsnmp_variable_list *varlist, struct oid_s *oid, int prefix)
{
    int i;
    size_t len;
    struct snmp_pdu *request;
    pass_t segment = oid->segment;

    request = snmp_pdu_create(SNMP_MSG_GETBULK);
    request->non_repeaters = 0;
    request->max_repetitions = repetitions[segment];

    while (oid->segment == segment) {
        len = oid->OidLen;
        for (i = prefix; i < varlist->name_length; i++) {
            oid->Oid[len++] = varlist->name[i];
        }
        snmp_add_null_var(request, oid->Oid, len);

        oid++;
    }

    if (snmp_send(hostContext->session, request)) {
        hostContext->requestIds[segment] = request->reqid;
        return 1;
    } else {
        snmp_perror("snmp_send");
        snmp_free_pdu(request);
    }

    return 0;
}

/*****************************************************************************/
/*
 * Called once a segment of a host is complete. Sets the host request element
 * of the current segment to zero, denoting that the segment is finished.
 * Finally checks if all segments of the current host are finished. If so,
 * decrement the activeHosts, denoting that all requests of the host are
 * complete.
 *
 * long reqid - the request id found in the modem response
 * long *requestIds - pointer to the array of request ids send to the modem
 * pass_t segment - completed segment
 *
 * returns int
 */
void updateActiveHosts(long reqid, long *requestIds, pass_t segment)
{
    static const long zero[FINISH] = { 0 };
    requestIds[segment] = 0;

    if (! memcmp(zero, requestIds, sizeof(zero))) {
        activeHosts--;
    }
}

/*****************************************************************************/
/*
 * Connect to the nmsprime SQL database
 *
 * const char *hostname - SQL hostname
 * const char *username - database username
 * const char *password - database password
 * const char *database - SQL database
 *
 * returns PGconn*
 */
PGconn* connectToSql(const char *hostname, const char *username, const char *password, const char *database)
{
    const char *keywords[] = {
        "host",
        "dbname",
        "user",
        "password",
        NULL
    };
    const char *values[] = {
        hostname ? hostname : "localhost",
        database ? database : "nmsprime",
        username ? username : "nmsprime",
        password ? password : "nmsprime",
        NULL
    };

    PGconn* conn = PQconnectdbParams(keywords, values, 0);
    if (PQstatus(conn) == CONNECTION_BAD) {
        fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        exit(1);
    }

    return conn;
}

/*****************************************************************************/
/*
 * Print the response into a File inside the current working directory.
 *
 * int status - state of the Response
 * hostContext_t *hostContext - pointer to the current hostcontext structure
 * struct snmp_pdu *responseData
 *
 * returns int
 */
int processResult(int status, hostContext_t *hostContext, struct snmp_pdu *responseData)
{
    struct variable_list *currentVariable;
    int ix;

    switch (status) {
    case STAT_SUCCESS:
        currentVariable = responseData->variables;
        if (responseData->errstat == SNMP_ERR_NOERROR) {
            while (currentVariable) {
                fprint_variable(hostContext->outputFile, currentVariable->name, currentVariable->name_length, currentVariable);
                currentVariable = currentVariable->next_variable;
            }
        } else {
            for (ix = 1; currentVariable && ix != responseData->errindex;
                 currentVariable = currentVariable->next_variable, ix++);

            fprintf(hostContext->outputFile, "ERROR: %s: ", hostContext->session->peername);
            if (currentVariable) {
                fprint_objid(hostContext->outputFile, currentVariable->name, currentVariable->name_length);
            }
            fprintf(hostContext->outputFile, ": %s\n", snmp_errstring(responseData->errstat));

        }
        return 1;
    case STAT_TIMEOUT:
        fprintf(stdout, "%s: Timeout\n", hostContext->session->peername);
        return 0;
    case STAT_ERROR:
        snmp_perror(hostContext->session->peername);
        return 0;
    }

    return 0;
}

/*****************************************************************************/
/*
 * This function sets the prerequisorities for the polling algorithm.
 * It does several things:
 * - Initializes the NET-SNMP library
 * - Set (increase) the limit for opened files (rlimit)
 * - Sets Configuration for NET-SNMP
 * - Decodes OIDs and fills OID structure
 * - Counts the number of OIDs for each segment
 *
 * returns void
 */
void initialize()
{
    struct oid_s *currentOid = oids;
    struct rlimit lim = { 1024 * 1024, 1024 * 1024 };
    activeHosts = 0;

    if (setrlimit(RLIMIT_NOFILE, &lim)) {
        perror("\nsetrlimit");
        fprintf(stderr, "You need to have superuser privileges to set a new file limit!\n");
        fprintf(stderr, "This program will most likely fail with more than 1000 Hosts!\n");
        fprintf(stderr, "Continuing anyway...\n\n");
    }

    /* initialize library */
    init_snmp("asynchapp");
    netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_OID_OUTPUT_FORMAT, NETSNMP_OID_OUTPUT_NUMERIC);
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_QUICK_PRINT, 1);
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_NUMERIC_TIMETICKS, 1);
    netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_HEX_OUTPUT_LENGTH, 0);

    /* parse the oids */
    while (currentOid->segment < FINISH) {
        currentOid->OidLen = MAX_OID_LEN;
        if (! read_objid(currentOid->Name, currentOid->Oid, &currentOid->OidLen)) {
            snmp_perror("read_objid");
            printf("Could not Parse OID: %s\n", currentOid->Name);
            exit(1);
        }

        itemCount[currentOid->segment]++;
        currentOid++;
    }
}

/*****************************************************************************/
/*
 * Function that gets called asynchronously each time a new SNMP packet
 * arrives. It checks whether the full table was retrieved and emits a new
 * SNMP request of the next batch of the current segment.
 *
 * int operation - state of the received mesasa
 * struct snmp_session *sp - not used as we get session from context data
 * int reqid - request id
 * struct snmp_pdu *responseData - response packet with data from modem
 * void *magic - magic pointer for context data
 *
 * returns int
 */
int asyncResponse(int operation, struct snmp_session *sp, int reqid, struct snmp_pdu *responseData, void *magic)
{
    int prefix;
    pass_t segment;
    struct oid_s *oid;
    netsnmp_variable_list *varlist;
    hostContext_t *hostContext = (hostContext_t *)magic;

    if (operation != NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE) {
        processResult(STAT_TIMEOUT, hostContext, responseData);
        activeHosts--;
        return 1;
    }
    if (! processResult(STAT_SUCCESS, hostContext, responseData)) {
        activeHosts--;
        return 1;
    }

    oid = getSegmentLastOid(reqid, hostContext->requestIds, &segment);
    if (segment == NON_REP) {
        updateActiveHosts(reqid, hostContext->requestIds, segment);
        return 1;
    }

    varlist = getLastVarBinding(responseData->variables);
    prefix = netsnmp_oid_find_prefix(varlist->name, varlist->name_length, oid->Oid, oid->OidLen);

    if (! memcmp(oid->Oid, varlist->name, oid->OidLen * sizeof(oid))) {
        oid -= itemCount[segment] - 1;
        sendNextBulkRequest(hostContext, varlist, oid, prefix);
    } else {
        updateActiveHosts(reqid, hostContext->requestIds, segment);
    }

    return 1;
}

/*****************************************************************************/
/*
 * Initiates the asynchronous SNMP transfer, starting with the non-repeaters.
 * The asyncResponse function gets called each time a packet is received.
 * while loop handles async behavior.
 *
 * PGconn *conn - SQL connection
 * char *query - SQL query
 *
 * returns void
 */
void asynchronous(PGconn *conn, char *query)
{
    int i, j, hostCount;
    hostContext_t *hostContext;

    struct snmp_pdu *request[FINISH];
    struct oid_s *currentOid = oids;

    for (i = NON_REP; i < FINISH; i++) {
        if (! itemCount[i]) {
            request[i] = 0;
            continue;
        }

        if (i == NON_REP) {
            request[i] = snmp_pdu_create(SNMP_MSG_GETNEXT);
        } else {
            request[i] = snmp_pdu_create(SNMP_MSG_GETBULK);
            request[i]->non_repeaters = 0;
            request[i]->max_repetitions = repetitions[i];
        }
    }

    while (currentOid->segment != FINISH) {
        snmp_add_null_var(request[currentOid->segment], currentOid->Oid, currentOid->OidLen);
        currentOid++;
    }

    /* startup all hosts */
    PGresult *result = PQexec(conn, query);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "No data retrieved\n");
        PQclear(result);
        exit(1);
    }

    hostCount = PQntuples(result);
    hostContext_t allHosts[hostCount]; // one hostContext structure per Host in DB

    for (i = 0; i < hostCount; i++) {
        struct snmp_session session;
        struct snmp_pdu *newRequest;
        hostContext = &allHosts[i];

        snmp_sess_init(&session);
        session.version = SNMP_VERSION_2c;
        session.retries = RETRIES;
        session.timeout = TIMEOUT * 1000000;
        session.peername = PQgetvalue(result, i, 0);
        session.community = (u_char *)PQgetvalue(result, i, 1);
        session.community_len = strlen((const char *)session.community);
        session.callback = asyncResponse;
        session.callback_magic = hostContext;

        if (! (hostContext->session = snmp_open(&session))) {
            snmp_perror("snmp_open");
            continue;
        }
        hostContext->outputFile = (oids == oids_single) ? stdout : fopen(PQgetvalue(result, i, 2), "w");
        fprintf(hostContext->outputFile, "ipv4:%s\n", PQgetvalue(result, i, 0));

        for (j = NON_REP; j < FINISH; j++) {
            if (! request[j]) {
                hostContext->requestIds[j] = 0;
                continue;
            }

            if (snmp_send(hostContext->session, newRequest = snmp_clone_pdu(request[j]))) {
                hostContext->requestIds[j] = newRequest->reqid;
                if (j == NON_REP) {
                    activeHosts++;
                }
            } else {
                snmp_perror("snmp_send");
                snmp_free_pdu(newRequest);
            }
        }
    }
    PQclear(result);

    int numfds, block;
    struct timeval timeout;
    netsnmp_large_fd_set fdset;
    netsnmp_large_fd_set_init(&fdset, FD_SETSIZE);

    time_t endwait = time(NULL) + (RETRIES + 2) * TIMEOUT;

    /* async event loop - loops while any active hosts or until timeout */
    while (activeHosts > 0 && time(NULL) < endwait) {
        numfds = 0;
        NETSNMP_LARGE_FD_ZERO(&fdset);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        block = 0;

        snmp_sess_select_info2(NULL, &numfds, &fdset, &timeout, &block);
        numfds = netsnmp_large_fd_set_select(numfds, &fdset, NULL, NULL, &timeout);

        if (numfds < 0) {
            perror("select failed");
            exit(1);
        }

        if (numfds) {
            snmp_read2(&fdset);
        } else {
            snmp_timeout();
        }
    }

    /* cleanup */
    for (i = NON_REP; i < FINISH; i++) {
        snmp_free_pdu(request[i]);
    }

    snmp_shutdown("asynchapp");
}

/*****************************************************************************/
/*
 * main function
 *
 * returns int
 */
int main(int argc, char **argv)
{
    int c, analysis = 0;
    const char *database = NULL, *hostname = NULL, *modem = NULL, *password = NULL, *username = NULL;
    static char usage[] = "usage: %s [-a (to be used for single modem analysis view)] [-d nmsprime_db_name] [-h hostname] [-m modem-id] [-p nmsprime_db_password] [-u nmsprime_db_username]\n";
    char query[512];
    PGconn *conn;

    while ((c = getopt(argc, argv, "ad:h:m:p:u:")) != -1) {
        switch (c) {
        case 'a':
            analysis = 1;
            break;
        case 'd':
            database = optarg;
            break;
        case 'h':
            hostname = optarg;
            break;
        case 'm':
            modem = optarg;
            break;
        case 'p':
            password = optarg;
            break;
        case 'u':
            username = optarg;
            break;
        case '?':
            if (optopt == 'd' || optopt == 'h' || optopt == 'm' || optopt == 'p' || optopt == 'u') {
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            } else if (isprint(optopt)) {
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            } else {
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
            }

            fprintf(stderr, usage, argv[0]);
            return 1;
        default:
            abort();
        }
    }

    oids = analysis ? oids_single : oids_multiple;

    if (modem) {
        uint32_t modemId = strtoul(modem, NULL, 10);
        snprintf(query, sizeof(query), "SET search_path TO nmsprime; SELECT CONCAT(modem.hostname, '.', provbase.domain_name), provbase.ro_community, CONCAT(modem.hostname, '.', provbase.domain_name) FROM modem, provbase WHERE modem.deleted_at IS NULL AND provbase.deleted_at IS NULL AND modem.hostname = 'cm-%u';", modemId);
    } else {
        snprintf(query, sizeof(query), "SET search_path TO nmsprime; SELECT COALESCE(host(modem.ipv4), CONCAT(modem.hostname, '.', provbase.domain_name)), provbase.ro_community, CONCAT(modem.hostname, '.', provbase.domain_name) FROM modem, provbase WHERE modem.deleted_at IS NULL AND provbase.deleted_at IS NULL AND modem.hostname LIKE 'cm-%%';");
    }

    initialize();
    conn = connectToSql(hostname, username, password, database);
    asynchronous(conn, query);
    PQfinish(conn);
    fcloseall();

    return 0;
}
