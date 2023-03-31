/*
 * This work is licensed under TURNSTONE OS Public License.
 * Please read and understand latest version of Licence.
 */

#include "setup.h"
#include <utils.h>
#include <buffer.h>
#include <data.h>
#include <sha2.h>
#include <bplustree.h>
#include <map.h>
#include <xxhash.h>

uint32_t main(uint32_t argc, char_t** argv);
data_t*  record_create(uint64_t db_id, uint64_t table_id, uint64_t nr_args, ...);

data_t* record_create(uint64_t db_id, uint64_t table_id, uint64_t nr_args, ...) {
    va_list args;
    va_start(args, nr_args);

    data_t* items = memory_malloc(sizeof(data_t) * (nr_args + 2));

    if(items == NULL) {
        return NULL;
    }

    items[0].type = DATA_TYPE_INT64;
    items[0].value = (void*)db_id;

    items[1].type = DATA_TYPE_INT64;
    items[1].value = (void*)table_id;

    uint64_t tmp_int_value;
    float64_t tmp_float_value;

    for(uint64_t i = 0; i < nr_args; i++) {
        items[i + 2].type = va_arg(args, data_type_t);

        tmp_int_value = 0;
        tmp_float_value = 0;

        switch(items[i + 2].type) {
        case DATA_TYPE_BOOLEAN:
        case DATA_TYPE_CHAR:
        case DATA_TYPE_INT8:
        case DATA_TYPE_INT16:
        case DATA_TYPE_INT32:
            tmp_int_value = va_arg(args, int32_t);
            items[i + 2].value = (void*)tmp_int_value;
            break;
        case DATA_TYPE_INT64:
            items[i + 2].value = (void*)va_arg(args, int64_t);
            break;
        case DATA_TYPE_FLOAT32:
        case DATA_TYPE_FLOAT64:
            tmp_float_value = va_arg(args, float64_t);
            memory_memcopy(&tmp_float_value, &tmp_int_value, sizeof(float64_t));
            items[i + 2].value = (void*)tmp_int_value;
            break;
        case DATA_TYPE_STRING:
            items[i + 2].value = va_arg(args, void*);
            break;
        case DATA_TYPE_INT8_ARRAY:

            items[i + 2].value = va_arg(args, void*);
            break;

        default:
            items[i + 2].value = va_arg(args, void*);
            break;
        }

    }

    va_end(args);

    data_t* tmp =  memory_malloc(sizeof(data_t));

    if(tmp == NULL) {
        memory_free(items);

        return NULL;
    }

    tmp->type = DATA_TYPE_DATA;
    tmp->length = nr_args + 2;
    tmp->value = items;

    data_t* res = data_bson_serialize(tmp, DATA_SERIALIZE_WITH_FLAGS | DATA_SERIALIZE_WITH_TYPE | DATA_SERIALIZE_WITH_LENGTH);

    memory_free(items);

    memory_free(tmp);

    return res;
}

uint32_t main(uint32_t argc, char_t** argv) {
    UNUSED(argc);
    UNUSED(argv);

    data_t* test_record1 = record_create(0, 0, 3, DATA_TYPE_STRING, "hello world", DATA_TYPE_INT32, 0x1234, DATA_TYPE_BOOLEAN, true);

    if(test_record1 == NULL) {
        print_error("cannot create test record 1");

        return -1;
    }

    FILE* out = fopen("tmp/test.bin", "w");
    fwrite(test_record1->value, test_record1->length, 1, out);
    fclose(out);

    memory_free(test_record1->value);
    memory_free(test_record1);


    print_success("TESTS PASSED");

    return 0;
}
