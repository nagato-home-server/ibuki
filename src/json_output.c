#include "eventnet/json_output.h"

#include <stdlib.h>
#include <string.h>

en_error_code_t en_json_mut_doc_to_buffer(yyjson_mut_doc *document, char *output, size_t output_len)
{
    if (document == NULL || output == NULL || output_len == 0) return EN_ERR_INVALID_ARGUMENT;
    size_t written_len = 0;
    char *serialized = yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &written_len);
    if (serialized == NULL || written_len + 1 > output_len) {
        free(serialized);
        return EN_ERR_INVALID_ARGUMENT;
    }
    memcpy(output, serialized, written_len);
    output[written_len] = '\0';
    free(serialized);
    return EN_ERR_NONE;
}

en_error_code_t en_json_mut_doc_write_line(FILE *output, yyjson_mut_doc *document)
{
    if (output == NULL || document == NULL) return EN_ERR_INVALID_ARGUMENT;
    yyjson_write_err write_error = {0};
    if (!yyjson_mut_write_fp(output, document, YYJSON_WRITE_NOFLAG, NULL, &write_error) || fputc('\n', output) == EOF) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    return fflush(output) == 0 ? EN_ERR_NONE : EN_ERR_INVALID_ARGUMENT;
}
