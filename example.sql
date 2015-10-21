drop EXTENSION cassandra2_fdw cascade;      
CREATE EXTENSION cassandra2_fdw;
CREATE SERVER cass_serv FOREIGN DATA WRAPPER cassandra2_fdw
    OPTIONS(url 'localhost');
CREATE USER MAPPING FOR public SERVER cass_serv OPTIONS(username 'test', password 'test');
CREATE FOREIGN TABLE test_cass_q (id int, data text) SERVER cass_serv OPTIONS (table 'test.test');
select * from test_cass_q where data ='dddd';
select * from test_cass_q,dupa where test_id = test_cass_q.id and test_cass_q.id = 1;

