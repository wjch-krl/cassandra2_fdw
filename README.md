cassandra2_fdw
==============

Foreign Data Wrapper (FDW) that allows quering Cassandra > 2.0 from PostgreSQL > 9.3.

### 1. Installation:
1. Install (http://downloads.datastax.com/cpp-driver/) or build (https://datastax.github.io/cpp-driver/topics/building/) DataStax Cassandra driver for C/C++.

2. Build postgresql extension
```bash
make USE_PGXS=1 install
```


### 2. Usage examples:
```sql
--Load extension
CREATE EXTENSION cassandra2_fdw;

--Create conncetion to Cassandra server
CREATE SERVER cass_serv FOREIGN DATA WRAPPER cassandra2_fdw   
    OPTIONS(url 'localhost:9160');

--Create user mapping
CREATE USER MAPPING FOR public SERVER cass_serv 
    OPTIONS(username 'test', password 'test');

--Create foreign table
CREATE FOREIGN TABLE test_cass_q (id int, data text) 
    SERVER cass_serv OPTIONS (table 'test.test');

--Run query against created table
select * from test_cass_q where id = 1;
```
