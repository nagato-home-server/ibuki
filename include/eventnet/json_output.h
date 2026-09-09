#ifndef EVENTNET_JSON_OUTPUT_H
#define EVENTNET_JSON_OUTPUT_H

#include "eventnet/types.h"
#include "yyjson.h"

#include <stdio.h>

en_error_code_t en_json_mut_doc_to_buffer(
    yyjson_mut_doc *document,
    char *output,
    size_t output_len
);

en_error_code_t en_json_mut_doc_write_line(FILE *output, yyjson_mut_doc *document);

#endif
