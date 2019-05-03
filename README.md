# NET-SNMP modempoller

Originated from the NET-SNMP async demo - hat tip to Niels Baggesen (Niels.Baggesen@uni-c.dk)

This program retrieves a set of modems from the cacti database and queries all modems for the given OIDs. The Each vendor implements the SNMP protocol differently, so the program needs to check if all tables are correct and if not, request another "batch".

The requested OIDs are devided into three segments: non-repeaters for system information, downstream and upstream. For each host a seperate session will be created. All requests are handled asynchronously and on response the next segment or the next batch of the current segment is requested.

# How to use
add your mysql credentials and then do

```bash
gcc -l netsnmp `mysql_config --cflags --libs` -o src/modempoller src/modempoller.c
```
