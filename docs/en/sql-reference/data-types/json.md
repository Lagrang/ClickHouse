---
description: 'Documentation for deprecated Object data type in ClickHouse'
keywords: ['object', 'data type']
sidebar_label: 'Object Data Type'
sidebar_position: 26
slug: /sql-reference/data-types/object-data-type
title: 'Object Data Type '
---

import DeprecatedBadge from '@theme/badges/DeprecatedBadge';

# Object data type 

<DeprecatedBadge/>

**This feature is not production-ready and deprecated.** If you need to work with JSON documents, consider using [this guide](/integrations/data-formats/json/overview) instead. A new implementation to support JSON object is in Beta. Further details [here](/sql-reference/data-types/newjson).

<hr />

Stores JavaScript Object Notation (JSON) documents in a single column.

`JSON` can be used as an alias to `Object('json')` when setting [use_json_alias_for_old_object_type](/operations/settings/settings#use_json_alias_for_old_object_type) is enabled.

## Example {#example}

**Example 1**

Creating a table with a `JSON` column and inserting data into it:

```sql
CREATE TABLE json
(
    o JSON
)
ENGINE = Memory
```

```sql
INSERT INTO json VALUES ('{"a": 1, "b": { "c": 2, "d": [1, 2, 3] }}')
```

```sql
SELECT o.a, o.b.c, o.b.d[3] FROM json
```

```text
┌─o.a─┬─o.b.c─┬─arrayElement(o.b.d, 3)─┐
│   1 │     2 │                      3 │
└─────┴───────┴────────────────────────┘
```

**Example 2**

To be able to create an ordered `MergeTree` family table, the sorting key has to be extracted into its column. For example, to insert a file of compressed HTTP access logs in JSON format:

```sql
CREATE TABLE logs
(
    timestamp DateTime,
    message JSON
)
ENGINE = MergeTree
ORDER BY timestamp
```

```sql
INSERT INTO logs
SELECT parseDateTimeBestEffort(JSONExtractString(json, 'timestamp')), json
FROM file('access.json.gz', JSONAsString)
```

## Displaying JSON columns {#displaying-json-columns}

When displaying a `JSON` column, ClickHouse only shows the field values by default (because internally, it is represented as a tuple). You can also display the field names by setting `output_format_json_named_tuples_as_objects = 1`:

```sql
SET output_format_json_named_tuples_as_objects = 1

SELECT * FROM json FORMAT JSONEachRow
```

```text
{"o":{"a":1,"b":{"c":2,"d":[1,2,3]}}}
```

## Related Content {#related-content}

- [Using JSON in ClickHouse](/integrations/data-formats/json/overview)
- [Getting Data Into ClickHouse - Part 2 - A JSON detour](https://clickhouse.com/blog/getting-data-into-clickhouse-part-2-json)
