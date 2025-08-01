---
description: 'Allows ClickHouse to connect to external databases via ODBC.'
sidebar_label: 'ODBC'
sidebar_position: 150
slug: /engines/table-engines/integrations/odbc
title: 'ODBC'
---

import CloudNotSupportedBadge from '@theme/badges/CloudNotSupportedBadge';

# ODBC

<CloudNotSupportedBadge/>

Allows ClickHouse to connect to external databases via [ODBC](https://en.wikipedia.org/wiki/Open_Database_Connectivity).

To safely implement ODBC connections, ClickHouse uses a separate program `clickhouse-odbc-bridge`. If the ODBC driver is loaded directly from `clickhouse-server`, driver problems can crash the ClickHouse server. ClickHouse automatically starts `clickhouse-odbc-bridge` when it is required. The ODBC bridge program is installed from the same package as the `clickhouse-server`.

This engine supports the [Nullable](../../../sql-reference/data-types/nullable.md) data type.

## Creating a table {#creating-a-table}

```sql
CREATE TABLE [IF NOT EXISTS] [db.]table_name [ON CLUSTER cluster]
(
    name1 [type1],
    name2 [type2],
    ...
)
ENGINE = ODBC(datasource, external_database, external_table)
```

See a detailed description of the [CREATE TABLE](/sql-reference/statements/create/table) query.

The table structure can differ from the source table structure:

- Column names should be the same as in the source table, but you can use just some of these columns and in any order.
- Column types may differ from those in the source table. ClickHouse tries to [cast](/sql-reference/functions/type-conversion-functions#cast) values to the ClickHouse data types.
- The [external_table_functions_use_nulls](/operations/settings/settings#external_table_functions_use_nulls) setting defines how to handle Nullable columns. Default value: 1. If 0, the table function does not make Nullable columns and inserts default values instead of nulls. This is also applicable for NULL values inside arrays.

**Engine Parameters**

- `datasource` — Name of the section with connection settings in the `odbc.ini` file.
- `external_database` — Name of a database in an external DBMS.
- `external_table` — Name of a table in the `external_database`.

These parameters can also be passed using [named collections](operations/named-collections.md).

## Usage example {#usage-example}

**Retrieving data from the local MySQL installation via ODBC**

This example is checked for Ubuntu Linux 18.04 and MySQL server 5.7.

Ensure that unixODBC and MySQL Connector are installed.

By default (if installed from packages), ClickHouse starts as user `clickhouse`. Thus, you need to create and configure this user in the MySQL server.

```bash
$ sudo mysql
```

```sql
mysql> CREATE USER 'clickhouse'@'localhost' IDENTIFIED BY 'clickhouse';
mysql> GRANT ALL PRIVILEGES ON *.* TO 'clickhouse'@'localhost' WITH GRANT OPTION;
```

Then configure the connection in `/etc/odbc.ini`.

```bash
$ cat /etc/odbc.ini
[mysqlconn]
DRIVER = /usr/local/lib/libmyodbc5w.so
SERVER = 127.0.0.1
PORT = 3306
DATABASE = test
USER = clickhouse
PASSWORD = clickhouse
```

You can check the connection using the `isql` utility from the unixODBC installation.

```bash
$ isql -v mysqlconn
+-------------------------+
| Connected!                            |
|                                       |
...
```

Table in MySQL:

```text
mysql> CREATE DATABASE test;
Query OK, 1 row affected (0,01 sec)

mysql> CREATE TABLE `test`.`test` (
    ->   `int_id` INT NOT NULL AUTO_INCREMENT,
    ->   `int_nullable` INT NULL DEFAULT NULL,
    ->   `float` FLOAT NOT NULL,
    ->   `float_nullable` FLOAT NULL DEFAULT NULL,
    ->   PRIMARY KEY (`int_id`));
Query OK, 0 rows affected (0,09 sec)

mysql> insert into test.test (`int_id`, `float`) VALUES (1,2);
Query OK, 1 row affected (0,00 sec)

mysql> select * from test.test;
+------+----------+-----+----------+
| int_id | int_nullable | float | float_nullable |
+------+----------+-----+----------+
|      1 |         NULL |     2 |           NULL |
+------+----------+-----+----------+
1 row in set (0,00 sec)
```

Table in ClickHouse, retrieving data from the MySQL table:

```sql
CREATE TABLE odbc_t
(
    `int_id` Int32,
    `float_nullable` Nullable(Float32)
)
ENGINE = ODBC('DSN=mysqlconn', 'test', 'test')
```

```sql
SELECT * FROM odbc_t
```

```text
┌─int_id─┬─float_nullable─┐
│      1 │           ᴺᵁᴸᴸ │
└────────┴────────────────┘
```

## See also {#see-also}

- [ODBC dictionaries](/sql-reference/dictionaries#mysql)
- [ODBC table function](../../../sql-reference/table-functions/odbc.md)
