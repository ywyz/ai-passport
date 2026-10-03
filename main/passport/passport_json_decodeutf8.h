/** UTF-8 encoding helper for \\u escapes. */
#ifndef PASSPORT_JSON_DECODEUTF8_H
#define PASSPORT_JSON_DECODEUTF8_H

#include <stddef.h>

int passport_json_utf8_encode(unsigned int cp, char *out, size_t cap);

#endif /* PASSPORT_JSON_DECODEUTF8_H */
