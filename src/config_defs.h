#ifndef HSDWL_CONFIG_DEFS_H
#define HSDWL_CONFIG_DEFS_H

#include <stddef.h>
#include "config.h"

enum field_type {
    FIELD_INT,
    FIELD_BOOL,
    FIELD_FLOAT,
    FIELD_STRING,
    FIELD_COLOR,
    FIELD_BEZIER,
};

struct config_field {
    const char *key;
    enum field_type type;
    size_t offset;       
    size_t data_size;    
    const void *default_ptr;  
};

struct action_entry {
    const char *name;
    enum hsdwl_action action;
};

extern const struct action_entry action_map[];
extern int action_map_count;

extern const struct config_field config_fields[];
extern int config_fields_count;

#endif
