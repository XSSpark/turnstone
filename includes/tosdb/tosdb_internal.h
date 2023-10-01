/**
 * @file tosdb_internal.h
 * @brief tosdb internal interface
 *
 * This work is licensed under TURNSTONE OS Public License.
 * Please read and understand latest version of Licence.
 */

#ifndef ___TOSDB_TOSDB_INTERNAL_H
#define ___TOSDB_TOSDB_INTERNAL_H 0

#include <tosdb/tosdb.h>
#include <future.h>
#include <utils.h>
#include <hashmap.h>
#include <cpu/sync.h>
#include <linkedlist.h>
#include <indexer.h>
#include <bloomfilter.h>
#include <set.h>
#include <compression.h>


#define TOSDB_PAGE_SIZE 4096
#define TOSDB_SUPERBLOCK_SIGNATURE "TURNSTONE OS DB\0"
#define TOSDB_VERSION_MAJOR 0
#define TOSDB_VERSION_MINOR 1

#define TOSDB_NAME_MAX_LEN 256

typedef enum tosdb_block_type_t {
    TOSDB_BLOCK_TYPE_NONE,
    TOSDB_BLOCK_TYPE_SUPERBLOCK,
    TOSDB_BLOCK_TYPE_DATABASE_LIST,
    TOSDB_BLOCK_TYPE_DATABASE,
    TOSDB_BLOCK_TYPE_TABLE_LIST,
    TOSDB_BLOCK_TYPE_TABLE,
    TOSDB_BLOCK_TYPE_COLUMN_LIST,
    TOSDB_BLOCK_TYPE_INDEX_LIST,
    TOSDB_BLOCK_TYPE_SSTABLE_LIST,
    TOSDB_BLOCK_TYPE_SSTABLE,
    TOSDB_BLOCK_TYPE_SSTABLE_INDEX,
    TOSDB_BLOCK_TYPE_SSTABLE_INDEX_DATA,
    TOSDB_BLOCK_TYPE_VALUELOG,
} tosdb_block_type_t;

typedef struct tosdb_block_header_t {
    char_t             signature[16];
    uint64_t           checksum;
    tosdb_block_type_t block_type : 16;
    uint64_t           block_size;
    uint32_t           version_major;
    uint32_t           version_minor;
    uint64_t           previous_block_location;
    uint64_t           previous_block_size;
    boolean_t          previous_block_invalid;
}__attribute__((packed, aligned(8))) tosdb_block_header_t;

_Static_assert(((sizeof(tosdb_block_header_t) % 8) == 0), "block header size is not aligned");

typedef struct tosdb_superblock_t {
    tosdb_block_header_t header;
    uint64_t             capacity;
    uint32_t             page_size;
    uint64_t             free_next_location;
    uint64_t             database_list_location;
    uint64_t             database_list_size;
    uint64_t             database_next_id;
    compression_type_t   compression_type;
    uint8_t              reservedN[2048] __attribute__((aligned(2048)));
}__attribute__((packed, aligned(8))) tosdb_superblock_t;

_Static_assert(sizeof(tosdb_superblock_t) == TOSDB_PAGE_SIZE, "super block size mismatch");

typedef struct tosdb_block_database_list_item_t {
    uint64_t  id;
    char_t    name[TOSDB_NAME_MAX_LEN];
    uint64_t  metadata_location;
    uint64_t  metadata_size;
    boolean_t deleted;
} __attribute__((packed, aligned(8))) tosdb_block_database_list_item_t;

typedef struct tosdb_block_database_list_t {
    tosdb_block_header_t             header;
    uint64_t                         database_count;
    tosdb_block_database_list_item_t databases[];
}__attribute__((packed, aligned(8))) tosdb_block_database_list_t;

typedef struct tosdb_block_database_t {
    tosdb_block_header_t header;
    uint64_t             id;
    char_t               name[TOSDB_NAME_MAX_LEN];
    uint64_t             table_next_id;
    uint64_t             table_list_location;
    uint64_t             table_list_size;
}__attribute__((packed, aligned(8))) tosdb_block_database_t;

typedef struct tosdb_block_table_list_item_t {
    uint64_t  id;
    char_t    name[TOSDB_NAME_MAX_LEN];
    uint64_t  metadata_location;
    uint64_t  metadata_size;
    boolean_t deleted;
}__attribute__((packed, aligned(8))) tosdb_block_table_list_item_t;

typedef struct tosdb_block_table_list_t {
    tosdb_block_header_t          header;
    uint64_t                      database_id;
    uint64_t                      table_count;
    tosdb_block_table_list_item_t tables[];
}__attribute__((packed, aligned(8))) tosdb_block_table_list_t;

typedef struct tosdb_block_table_t {
    tosdb_block_header_t header;
    uint64_t             id;
    uint64_t             database_id;
    char_t               name[TOSDB_NAME_MAX_LEN];
    uint64_t             column_next_id;
    uint64_t             column_list_location;
    uint64_t             column_list_size;
    uint64_t             index_next_id;
    uint64_t             index_list_location;
    uint64_t             index_list_size;
    uint64_t             memtable_next_id;
    uint64_t             sstable_list_location;
    uint64_t             sstable_list_size;
    uint64_t             primary_index_id;
    uint64_t             primary_column_id;
    data_type_t          primary_column_type;
}__attribute__((packed, aligned(8))) tosdb_block_table_t;

typedef struct tosdb_block_column_list_item_t {
    uint64_t    id;
    char_t      name[TOSDB_NAME_MAX_LEN];
    data_type_t type : 16;
    boolean_t   deleted;
}__attribute__((packed, aligned(8))) tosdb_block_column_list_item_t;

typedef struct tosdb_block_column_list_t {
    tosdb_block_header_t           header;
    uint64_t                       database_id;
    uint64_t                       table_id;
    uint64_t                       column_count;
    tosdb_block_column_list_item_t columns[];
}__attribute__((packed, aligned(8))) tosdb_block_column_list_t;

typedef struct tosdb_block_index_list_item_t {
    uint64_t           id;
    tosdb_index_type_t type : 16;
    boolean_t          deleted;
    uint64_t           column_id;
}__attribute__((packed, aligned(8))) tosdb_block_index_list_item_t;

typedef struct tosdb_block_index_list_t {
    tosdb_block_header_t          header;
    uint64_t                      database_id;
    uint64_t                      table_id;
    uint64_t                      index_count;
    tosdb_block_index_list_item_t indexes[];
}__attribute__((packed, aligned(8))) tosdb_block_index_list_t;

typedef struct tosdb_block_valuelog_t {
    tosdb_block_header_t header;
    uint64_t             database_id;
    uint64_t             table_id;
    uint64_t             sstable_id;
    uint64_t             data_size;
    uint64_t             valuelog_unpacked_size;
    uint8_t              data[];
}__attribute__((packed, aligned(8))) tosdb_block_valuelog_t;

typedef struct tosdb_block_sstable_list_item_index_pair_t {
    uint64_t index_id;
    uint64_t index_location;
    uint64_t index_size;
}__attribute__((packed, aligned(8))) tosdb_block_sstable_list_item_index_pair_t;

typedef struct tosdb_block_sstable_list_item_t {
    uint64_t                                   sstable_id;
    uint64_t                                   level;
    uint64_t                                   record_count;
    uint64_t                                   valuelog_location;
    uint64_t                                   valuelog_size;
    uint64_t                                   index_count;
    tosdb_block_sstable_list_item_index_pair_t indexes[];
}__attribute__((packed, aligned(8))) tosdb_block_sstable_list_item_t;

typedef struct tosdb_block_sstable_list_t {
    tosdb_block_header_t            header;
    uint64_t                        database_id;
    uint64_t                        table_id;
    uint64_t                        sstable_count;
    tosdb_block_sstable_list_item_t sstables[];
}__attribute__((packed, aligned(8))) tosdb_block_sstable_list_t;

typedef struct tosdb_block_sstable_index_t {
    tosdb_block_header_t header;
    uint64_t             database_id;
    uint64_t             table_id;
    uint64_t             sstable_id;
    uint64_t             index_id;
    uint64_t             record_count;
    uint64_t             minmax_key_size;
    uint64_t             bloomfilter_size;
    uint64_t             bloomfilter_unpacked_size;
    uint64_t             index_data_location;
    uint64_t             index_data_size;
    uint8_t              data[];
}__attribute__((packed, aligned(8))) tosdb_block_sstable_index_t;

typedef struct tosdb_block_sstable_index_data_t {
    tosdb_block_header_t header;
    uint64_t             database_id;
    uint64_t             table_id;
    uint64_t             sstable_id;
    uint64_t             index_id;
    uint64_t             record_count;
    uint64_t             index_data_size;
    uint64_t             index_data_unpacked_size;
    uint8_t              data[];
}__attribute__((packed, aligned(8))) tosdb_block_sstable_index_data_t;

typedef struct tosdb_cache_t tosdb_cache_t;

struct tosdb_t {
    tosdb_backend_t*    backend;
    tosdb_superblock_t* superblock;
    boolean_t           is_dirty;
    hashmap_t*          databases;
    hashmap_t*          database_new;
    lock_t              lock;
    tosdb_cache_t*      cache;
    compression_t*      compression;
};

boolean_t             tosdb_write_and_flush_superblock(tosdb_backend_t* backend, tosdb_superblock_t* sb);
uint64_t              tosdb_block_write(tosdb_t* tdb, tosdb_block_header_t* block);
tosdb_block_header_t* tosdb_block_read(tosdb_t* tdb, uint64_t location, uint64_t size);
boolean_t             tosdb_persist(tosdb_t* tdb);
boolean_t             tosdb_load_databases(tosdb_t* tdb);

struct tosdb_database_t {
    tosdb_t*   tdb;
    boolean_t  is_open;
    boolean_t  is_dirty;
    boolean_t  is_deleted;
    uint64_t   id;
    uint64_t   table_next_id;
    char_t*    name;
    lock_t     lock;
    hashmap_t* tables;
    hashmap_t* table_new;
    uint64_t   metadata_location;
    uint64_t   metadata_size;
    uint64_t   table_list_location;
    uint64_t   table_list_size;
    hashmap_t* sequences;
};

boolean_t         tosdb_database_persist(tosdb_database_t* db);
tosdb_database_t* tosdb_database_load_database(tosdb_database_t* db);
boolean_t         tosdb_database_load_tables(tosdb_database_t* db);

typedef struct tosdb_memtable_t tosdb_memtable_t;

struct tosdb_table_t {
    tosdb_database_t* db;
    boolean_t         is_open;
    boolean_t         is_dirty;
    uint64_t          id;
    char_t*           name;
    lock_t            lock;
    hashmap_t*        columns;
    hashmap_t*        indexes;
    uint64_t          primary_index_id;
    uint64_t          primary_column_id;
    data_type_t       primary_column_type;
    uint64_t          metadata_location;
    uint64_t          metadata_size;
    boolean_t         is_deleted;
    uint64_t          column_next_id;
    uint64_t          column_new_count;
    linkedlist_t*     column_new;
    uint64_t          column_list_location;
    uint64_t          column_list_size;
    uint64_t          index_next_id;
    uint64_t          index_new_count;
    linkedlist_t*     index_new;
    uint64_t          index_list_location;
    uint64_t          index_list_size;
    uint64_t          max_record_count;
    uint64_t          max_valuelog_size;
    uint64_t          max_memtable_count;
    tosdb_memtable_t* current_memtable;
    linkedlist_t*     memtables;
    uint64_t          memtable_next_id;
    uint64_t          sstable_list_location;
    uint64_t          sstable_list_size;
    linkedlist_t*     sstable_list_items;
    hashmap_t*        sstable_levels;
    uint64_t          sstable_max_level;
};

boolean_t      tosdb_table_persist(tosdb_table_t* tbl);
tosdb_table_t* tosdb_table_load_table(tosdb_table_t* tbl);
boolean_t      tosdb_table_load_columns(tosdb_table_t* tbl);
boolean_t      tosdb_table_load_indexes(tosdb_table_t* tbl);
boolean_t      tosdb_table_load_sstables(tosdb_table_t* tbl);

typedef struct tosdb_column_t {
    uint64_t    id;
    char_t*     name;
    data_type_t type;
    boolean_t   is_deleted;
} tosdb_column_t;

boolean_t tosdb_table_column_persist(tosdb_table_t* tbl);

typedef struct tosdb_index_t {
    uint64_t           id;
    tosdb_index_type_t type;
    boolean_t          is_deleted;
    uint64_t           column_id;
} tosdb_index_t;

boolean_t             tosdb_table_index_persist(tosdb_table_t* tbl);
boolean_t             tosdb_table_memtable_persist(tosdb_table_t* tbl);
const tosdb_column_t* tosdb_table_get_column_by_index_id(tosdb_table_t* tbl, uint64_t id);

typedef struct tosdb_memtable_index_item_t {
    uint128_t record_id;
    uint64_t  key_hash;
    boolean_t is_deleted;
    uint64_t  offset;
    uint64_t  length;
    uint64_t  key_length;
    uint8_t   key[];
}__attribute__((packed, aligned(8))) tosdb_memtable_index_item_t;

typedef struct tosdb_memtable_secondary_index_item_t {
    uint128_t record_id;
    uint64_t  secondary_key_hash;
    uint64_t  secondary_key_length;
    boolean_t is_primary_key_deleted;
    uint64_t  primary_key_hash;
    uint64_t  primary_key_length;
    uint8_t   data[];
}__attribute__((packed, aligned(8))) tosdb_memtable_secondary_index_item_t;

int8_t tosdb_memtable_index_comparator(const void* i1, const void* i2);
int8_t tosdb_memtable_record_id_comparator(const void* i1, const void* i2);
int8_t tosdb_memtable_secondary_index_comparator(const void* i1, const void* i2);
int8_t tosdb_memtable_secondary_index_record_id_comparator(const void* i1, const void* i2);

typedef struct tosdb_memtable_index_t {
    tosdb_index_t* ti;
    bloomfilter_t* bloomfilter;
    index_t*       index;
} tosdb_memtable_index_t;

struct tosdb_memtable_t {
    tosdb_table_t*                   tbl;
    uint64_t                         id;
    boolean_t                        is_readonly;
    boolean_t                        is_full;
    boolean_t                        is_dirty;
    hashmap_t*                       indexes;
    buffer_t*                        values;
    uint64_t                         record_count;
    tosdb_block_sstable_list_item_t* stli;
};

boolean_t tosdb_memtable_new(tosdb_table_t * tbl);
boolean_t tosdb_memtable_free(tosdb_memtable_t* mt);
boolean_t tosdb_memtable_upsert(tosdb_record_t * record, boolean_t del);
boolean_t tosdb_memtable_persist(tosdb_memtable_t* mt);
boolean_t tosdb_memtable_index_persist(tosdb_memtable_t* mt, tosdb_block_sstable_list_item_t* stli, uint64_t idx, tosdb_memtable_index_t* mt_idx);
boolean_t tosdb_memtable_is_deleted(tosdb_record_t* record);

typedef struct tosdb_record_context_t {
    tosdb_table_t* table;
    uint128_t      record_id;
    hashmap_t*     columns;
    hashmap_t*     keys;
    uint64_t       level;
    uint64_t       sstable_id;
    boolean_t      is_deleted;
} tosdb_record_context_t;

typedef struct tosdb_record_key_t {
    uint64_t index_id;
    uint64_t key_hash;
    uint64_t key_length;
    uint8_t* key;
}tosdb_record_key_t;

data_t*   tosdb_record_serialize(tosdb_record_t* record);
boolean_t tosdb_record_set_data_with_colid(tosdb_record_t * record, const uint64_t col_id, data_type_t type, uint64_t len, const void* value);
boolean_t tosdb_record_get_data_with_colid(tosdb_record_t * record, const uint64_t col_id, data_type_t type, uint64_t* len, void** value);

boolean_t tosdb_memtable_get(tosdb_record_t* record);
boolean_t tosdb_sstable_get(tosdb_record_t* record);

boolean_t tosdb_memtable_search(tosdb_record_t* record, set_t* results);
boolean_t tosdb_sstable_search(tosdb_record_t* record, set_t* results);

linkedlist_t* tosdb_record_search(tosdb_record_t* record);
boolean_t     tosdb_record_search_set_destroy_cb(void * item);

boolean_t tosdb_database_compact(const tosdb_database_t* db, tosdb_compaction_type_t type);
boolean_t tosdb_table_compact(const tosdb_table_t* tbl, tosdb_compaction_type_t type);
boolean_t tosdb_sstable_level_minor_compact(const tosdb_table_t* tbl, uint64_t level);
boolean_t tosdb_sstable_level_major_compact(const tosdb_table_t* tbl, uint64_t level);
int8_t    tosdb_record_primary_key_comparator(const void* item1, const void* item2);
boolean_t tosdb_table_get_primary_keys_internal(const tosdb_table_t* tbl, set_t* pks, linkedlist_t* old_pks);

#define TOSDB_SEQUENCE_TABLE_NAME ".sequences"

typedef struct tosdb_sequence_t {
    tosdb_record_t* this_record;
    int64_t         id;
    int64_t         next_value;
    int64_t         cache_size;
    int64_t         cache_current_size;
    lock_t          lock;
} tosdb_sequence_t;

#endif
