# NET-SNMP modempoller

## Description
The snmp modem poller solves the issue with Cacti producing massive load and taking a long time by using hundreds of concurrent php workers which handle each a small batch of modem sequentially.
By switching to the native C implementation, the program is highly efficient and depending on the devices in your network you can query easily over 2 Million OIDs per minute.

This is achieved by using BULK requests and handling them asynchronously. So the processor is only used for sending the query and can do "other stuff" while waiting for the SNMP information to come back.

We tested the performance of the poller and we got roughly 450k OIDs from over 5000 devices with 5s CPU usage and 20s overall execution time.

## Functionality
This program retrieves a set of modems from the cacti database and queries all modems for the given OIDs. The Each vendor implements the SNMP protocol differently, so the program needs to check if all tables are correct and if not, request another "batch".


The requested OIDs are divided into three segments:
 * non-repeaters for system information
 * downstream
 * upstream

For each host a separate session will be created. All requests are handled asynchronously and on response the next segment or the next batch of the current segment is requested.

The modem poller uses the NETSNMP C-library and is based on the NET-SNMP async demo. (hat tip to Niels Baggesen (Niels.Baggesen@uni-c.dk))

## How to use

Replace the MySQL Credentials line 84 to 87 with your own

```C
    char host[] = "localhost";
    char user[] = "cactiuser";
    char pass[] = "secret";
    char db[] = "cacti";
```

and compile the program with

```bash
gcc -l netsnmp `mysql_config --cflags --libs` -o src/modempoller src/modempoller.c
```

make sure you have maketools(gcc) and the mysql-libraries installed.

To install the MySQL libraries on CentOS 7 you can execute:
```bash
yum install mysql-devel
```
