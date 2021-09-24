# NMS Prime async modempoller

## Description
This **asynchronous snmp poller** solves an issue with Cacti, when monitoring several thousand devices. Cacti produces a massive CPU load and takes a long time to collect its monitoring data by using hundreds of concurrent php workers, which handle each a small batch of devices sequentially. This blocks the CPU and scales pretty poorly.

By switching to the native C implementation, this program is highly efficient and depending on the devices in your network you can query easily over 2 Million OIDs per minute. It is especially efficient if you have to query all devices for the same OIDs (a lot of similar devices). The required data is retrieved from a **MySQL database** with the **MySQL C API**.

A huge chunk of the performance is achieved by using **SNMP BULK Requests** and handling them **asynchronously**. This translates to lower processor load as the processor is only used when sending and retrieving and can do "other stuff" while waiting for the SNMP information to come back.

We tested the performance of the poller and we got roughly 450k OIDs from over 5000 devices with 5s CPU usage and 20s overall execution time.

## Functionality
This program retrieves a set of devices from the **NMS Prime MYSQL database** and queries all devices for the given OIDs. Each vendor implements the SNMP protocol differently, so the program needs to check if all SNMP tables are fully received and if not, requesting another "batch".

For our usecase the requested OIDs are divided into three segments:
 * non-repeaters for system information (non-table)
 * downstream
 * upstream

For each host a separate session will be created. All requests are handled asynchronously and on response the next batch of the current segment is requested.

The modem poller uses the NETSNMP C-library and is based on the NET-SNMP async demo. (hat tip to Niels Baggesen (Niels.Baggesen@uni-c.dk))

## How to use

The build requires: `gcc net-snmp-devel mysql-devel`. To install those on CentOS, execute

```bash
yum install gcc net-snmp-devel mysql-devel
```

Compile the program with

```bash
gcc -l netsnmp `mysql_config --cflags --libs` -o src/modempoller-nmsprime src/modempoller-nmsprime.c
```

If you are not using the default nmsprime credentials you can supply them via parameters:

```bash
./modempoller-nmsprime [-a (to be used for single modem analysis view)] [-d nmsprime_db_name] [-h hostname] [-m modem-id] [-p nmsprime_db_password] [-u nmsprime_db_username]
```
