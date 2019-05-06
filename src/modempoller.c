/**************************************************************************/ /*
 * NET-SNMP modempoller
 *
 * Originated from the NET-SNMP async demo
 * Hat tip to Niels Baggesen (Niels.Baggesen@uni-c.dk)
 *
 * This program retrieves a set of modems from the cacti database and queries
 * all modems for the given OIDs. The Each vendor implements the SNMP protocol
 * differently, so the program needs to check if all tables are correct and if
 * not, request another "batch".
 *
 * The requested OIDs are divided into three segments: non-repeaters for system
 * information, downstream and upstream. For each host a separate session will
 * be created. All requests are handled asynchronously and on response the next
 * segment or the next batch of the current segment is requested.
 *
 * Christian Schramm (@cschra) and Ole Ernst (@olebowle), 2019
 *
 *****************************************************************************/

/********************************** DEFINES **********************************/
#define _GNU_SOURCE
#define MAX_REPETITIONS 9
#define RETRIES 3
#define TIMEOUT 5000000

/********************************* INCLUDES **********************************/
#include <ctype.h>
#include <mysql.h>
#include <sys/resource.h>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/library/large_fd_set.h>

/****************************** GLOBAL STRUCTURES ****************************/
/* to keep track which segment is sent */
typedef enum pass { NON_REP, DOWNSTREAM, UPSTREAM, FINISH } pass_t;

/* a list of variables to query for */
struct oid_s {
    pass_t segment;
    const char *Name;
    oid Oid[MAX_OID_LEN];
    size_t OidLen;
} oids[] = {
    { NON_REP, "1.3.6.1.2.1.1.1" }, /* SysDescr */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.3" }, /* US Power (2.0) */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.12" }, /* T3 Timeout */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.13" }, /* T4 Timeout */
    { NON_REP, "1.3.6.1.2.1.10.127.1.2.2.1.17" }, /* PreEq */
    { DOWNSTREAM, "1.3.6.1.2.1.10.127.1.1.1.1.6" }, /* Power */
    { DOWNSTREAM, "1.3.6.1.2.1.10.127.1.1.4.1.3" }, /* Corrected */
    { DOWNSTREAM, "1.3.6.1.2.1.10.127.1.1.4.1.4" }, /* Uncorrectable */
    { DOWNSTREAM, "1.3.6.1.2.1.10.127.1.1.4.1.5" }, /* SNR (2.0) */
    { DOWNSTREAM, "1.3.6.1.2.1.10.127.1.1.4.1.6" }, /* Microreflections */
    { DOWNSTREAM, "1.3.6.1.4.1.4491.2.1.20.1.24.1.1" }, /* SNR (3.0) */
    { UPSTREAM, "1.3.6.1.2.1.10.127.1.1.2.1.3" }, /* Bandwidth */
    { UPSTREAM, "1.3.6.1.4.1.4491.2.1.20.1.2.1.1" }, /* Power (3.0) */
    { UPSTREAM, "1.3.6.1.4.1.4491.2.1.20.1.2.1.9" }, /* Ranging Status */
    { FINISH }
};

/* context structure to keep track of the current request */
typedef struct hostContext {
    struct snmp_session *session; /* which host is currently processed */
    long reqids[FINISH];
    FILE *outputFile; /* to which file should the response be written to */
} hostContext_t;

/****************************** GLOBAL VARIABLES *****************************/
int activeHosts, hostCount;
int itemCount[FINISH] = {0};
MYSQL_RES *result;

/********************************* FUNCTIONS *********************************/
/*
 * Connect to the cacti MySQL Database using the mysql-c high level API.
 *
 * The result of the query is stored in the global MYSQL_RES *result Variable
 * and the amount of hosts is stored in the global int hostCount Variable
 *
 * returns void
 */
void connectToMySql(const char *hostname, const char *username, const char *password, const char *database)
{
    MYSQL *con = mysql_init(NULL);

    if (!con) {
        fprintf(stderr, "%s\n", mysql_error(con));
        exit(1);
    }

    if (!mysql_real_connect(con,
                            hostname ? hostname : "localhost",
                            username ? username : "cactiuser",
                            password ? password : "cactiuser",
                            database ? database : "cacti",
                            0, NULL, 0)) {
        fprintf(stderr, "%s\n", mysql_error(con));
        mysql_close(con);
        exit(1);
    }

    if (mysql_query(con, "SELECT hostname, snmp_community FROM host WHERE hostname LIKE 'cm-%' ORDER BY hostname"))
        fprintf(stderr, "%s\n", mysql_error(con));

    result = mysql_store_result(con);

    hostCount = mysql_num_rows(result);

    mysql_close(con);
}

struct oid_s *getSegmentLastOid(long reqid, long *host_reqids, pass_t *segment) {
    int last = -1;

    for ((*segment) = 0; (*segment) < FINISH; (*segment)++) {
        last += itemCount[*segment];

        if (reqid == host_reqids[*segment]) {
            return &oids[last];
        }
    }

    return NULL;
}

/*****************************************************************************/
/*
 * This function sets the prerequisorities for the polling algorithm.
 * It does several things:
 * - Opens a Socket if on a windows machine
 * - Initializes the NET-SNMP library
 * - Sets Configuration for NET-SNMP
 * - Decodes OIDs and fills OID structure
 * - Counts the number of OIDs for each segment
 *
 * returns void
 */
void initialize()
{
    struct oid_s *currentOid = oids;
    struct rlimit lim = {1024*1024, 1024*1024};
    activeHosts = hostCount = 0;

    setrlimit(RLIMIT_NOFILE, &lim);

    /* initialize library */
    init_snmp("asynchapp");
    netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_OID_OUTPUT_FORMAT, NETSNMP_OID_OUTPUT_NUMERIC);
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_QUICK_PRINT, 1);
    netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_HEX_OUTPUT_LENGTH, 0);

    /* parse the oids */
    while (currentOid->segment < FINISH) {
        currentOid->OidLen = MAX_OID_LEN;
        if (!read_objid(currentOid->Name, currentOid->Oid, &currentOid->OidLen)) {
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
    char buf[1024];
    struct variable_list *currentVariable;
    int ix;

    switch (status) {
    case STAT_SUCCESS:
        currentVariable = responseData->variables;
        if (responseData->errstat == SNMP_ERR_NOERROR) {
            while (currentVariable) {
                snprint_variable(buf, sizeof(buf), currentVariable->name, currentVariable->name_length, currentVariable);
                fprintf(hostContext->outputFile, "%s\n", buf);
                currentVariable = currentVariable->next_variable;
            }
        } else {
            for (ix = 1; currentVariable && ix != responseData->errindex;
                 currentVariable = currentVariable->next_variable, ix++);

            if (currentVariable)
                snprint_objid(buf, sizeof(buf), currentVariable->name, currentVariable->name_length);
            else
                strcpy(buf, "(none)");

            fprintf(hostContext->outputFile, "ERROR: %s: %s: %s\n", hostContext->session->peername, buf,
                    snmp_errstring(responseData->errstat));
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
        if (!varlist->next_variable)
            return varlist;
        varlist = varlist->next_variable;
    }

    return NULL;
}

/*****************************************************************************/
/*
 * Utility function as this is called multiple times over the course of the
 * program. This compiles the Request Package Payload.
 *
 * struct snmp_pdu *request - request pdu
 * hostContext_t *hostContext - pointer to the current hostcontext structure
 * netsnmp_variable_list *varlist - empty variable list to fill
 * pass_t segment - determines which OIDs to add as request payload
 *
 * returns void
 */
int sendNextBulkRequest(hostContext_t *hostContext, netsnmp_variable_list *varlist, struct oid_s *oid)
{
    struct snmp_pdu *request;
    pass_t segment = oid->segment;

    request = snmp_pdu_create(SNMP_MSG_GETBULK);
    request->non_repeaters = 0;
    request->max_repetitions = MAX_REPETITIONS;

    while (oid->segment == segment) {
        oid->Oid[oid->OidLen] = varlist->name[varlist->name_length - 1];
        snmp_add_null_var(request, oid->Oid, oid->OidLen + 1);

        oid++;
    }

    if (snmp_send(hostContext->session, request)) {
        hostContext->reqids[segment] = request->reqid;
        return 1;
    } else {
        snmp_perror("snmp_send");
        snmp_free_pdu(request);
    }

    return 0;
}

void update_active_hosts(long reqid, long *host_reqids, pass_t segment)
{
    static const long zero[FINISH] = { 0 };
    host_reqids[segment] = 0;

    if (!memcmp(zero, host_reqids, sizeof(zero)))
        activeHosts--;
}

/*****************************************************************************/
/*
 * State machine that gets called asynchronously each time a new SNMP packet
 * arrives. It checks whether the full table was retrieved and emits a new
 * SNMP request of either the next batch of the current segment or the next
 * segment.
 *
 * int operation - state of the received mesasa
 * struct snmp_session *sp - not used as we get session from context data
 * int reqid - request id - also unused
 * struct snmp_pdu *responseData - response packet with data from modem
 * void *magic - magic pointer for context data
 *
 * returns int
 */
int async_response(int operation, struct snmp_session *sp, int reqid, struct snmp_pdu *responseData, void *magic)
{
    pass_t segment;
    struct oid_s *oid;
    netsnmp_variable_list *varlist;
    hostContext_t *hostContext = (hostContext_t *)magic;

    if (operation != NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE) {
        processResult(STAT_TIMEOUT, hostContext, responseData);
        activeHosts--;
        return 1;
    }
    if (!processResult(STAT_SUCCESS, hostContext, responseData))
        return 1;

    oid = getSegmentLastOid(reqid, hostContext->reqids, &segment);
    if (segment == NON_REP) {
        update_active_hosts(reqid, hostContext->reqids, segment);
        return 1;
    }

    varlist = getLastVarBinding(responseData->variables);
    if (!memcmp(oid->Oid, varlist->name, oid->OidLen * sizeof(oid))) {
        oid -= itemCount[segment] - 1;
        sendNextBulkRequest(hostContext, varlist, oid);
    } else {
        update_active_hosts(reqid, hostContext->reqids, segment);
    }

    return 1;
}

/*****************************************************************************/
/*
 * Initiates the asynchronous SNMP transfer, starting with the non-repeaters.
 * The async_response function gets called each time a packet is received.
 * while loop handles asynch behavior.
 *
 * returns void
 */
void asynchronous()
{
    pass_t i;
    MYSQL_ROW currentHost;
    hostContext_t *hostContext;
    hostContext_t allHosts[hostCount]; /* one hostContext structure per Host in DB */

    struct snmp_pdu *request[FINISH];
    struct oid_s *currentOid = oids;

    for (i = 0; i < FINISH; i++) {
        request[i] = snmp_pdu_create(SNMP_MSG_GETBULK);
        request[i]->non_repeaters = (i == NON_REP) ? itemCount[i] : 0;
        request[i]->max_repetitions = (i == NON_REP) ? 0 : MAX_REPETITIONS;
    }

    while (currentOid->segment != FINISH) {
        snmp_add_null_var(request[currentOid->segment], currentOid->Oid, currentOid->OidLen);
        currentOid++;
    }

    /* startup all hosts */
    for (hostContext = allHosts; (currentHost = mysql_fetch_row(result)); hostContext++) {
        struct snmp_session session;
        struct snmp_pdu *newRequest;

        snmp_sess_init(&session);
        session.version = SNMP_VERSION_2c;
        session.retries = RETRIES;
        session.timeout = TIMEOUT;
        session.peername = strdup(currentHost[0]);
        session.community = (u_char*)strdup(currentHost[1]);
        session.community_len = strlen((const char*)session.community);
        session.callback = async_response;
        session.callback_magic = hostContext;

        if (!(hostContext->session = snmp_open(&session))) {
            snmp_perror("snmp_open");
            continue;
        }
        hostContext->outputFile = fopen(session.peername, "w");

        for (i = 0; i < FINISH; i++) {
            if (snmp_send(hostContext->session, newRequest = snmp_clone_pdu(request[i]))) {
                hostContext->reqids[i] = newRequest->reqid;
                if (!i)
                    activeHosts++;
            } else {
                snmp_perror("snmp_send");
                snmp_free_pdu(newRequest);
            }
        }
    }

    /* async event loop - loops while any active hosts */
    while (activeHosts > 0) {
        int fds = 0, block = 1;
        struct timeval timeout;
        netsnmp_large_fd_set fdset;

        snmp_sess_select_info2(NULL, &fds, &fdset, &timeout, &block);

        fds = netsnmp_large_fd_set_select(fds, &fdset, NULL, NULL, block ? NULL : &timeout);

        if (fds < 0) {
            perror("select failed");
            exit(1);
        }

        if (fds)
            snmp_read2(&fdset);
        else
            snmp_timeout();
    }

    /* cleanup */
    for (i = 0; i < FINISH; i++)
        snmp_free_pdu(request[i]);

    for (hostContext = allHosts, i = 0; i < hostCount; hostContext++, i++)
        if (hostContext->session)
            snmp_close(hostContext->session);
}

/*****************************************************************************/
/*
 * close all file descriptors and free the MySQL result
 *
 * returns void
 */
void cleanup()
{
    mysql_free_result(result);
    fcloseall();
}

/*****************************************************************************/
/*
 * main function
 *
 * returns int
 */
int main(int argc, char **argv)
{
    int c;
    const char *database = NULL, *hostname = NULL, *password = NULL, *username = NULL;
    static char usage[] = "usage: %s [-d cacti_db_name] [-h hostname] [-p cacti_db_password] [-u cacti_db_username]\n";

    while ((c = getopt(argc, argv, "d:h:p:u:")) != -1)
        switch (c) {
        case 'd':
            database = optarg;
            break;
        case 'h':
            hostname = optarg;
            break;
        case 'p':
            password = optarg;
            break;
        case 'u':
            username = optarg;
            break;
        case '?':
            if (optopt == 'd' || optopt == 'h' || optopt == 'p' || optopt == 'u')
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            else if (isprint(optopt))
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            else
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);

            fprintf(stderr, usage, argv[0]);
            return 1;
        default:
            abort();
        }

    initialize();
    connectToMySql(hostname, username, password, database);
    asynchronous();
    cleanup();

    return 0;
}
