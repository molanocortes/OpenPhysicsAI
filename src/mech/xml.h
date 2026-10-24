/* xml.h - small strict XML reader for robot description files (URDF)
 *
 * Supported: the XML declaration, comments, processing instructions (skipped), elements, attributes in single or double
 * quotes, character data (kept, trimmed), CDATA sections, the five predefined entities and numeric character references.
 * Rejected: DOCTYPE and custom entities (no entity expansion), mismatched or unclosed tags, duplicate attributes,
 * nesting deeper than 64. Every node records its line for error messages. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct XmlAttr {
    char *name, *value;
} XmlAttr;

typedef struct XmlNode XmlNode;
struct XmlNode {
    char *name;
    XmlAttr *attrs;
    int nattrs;
    XmlNode **children;
    int nchildren, cap;
    char *text; /* concatenated character data, trimmed; NULL if none */
    int line;
    XmlNode *parent;
};

XmlNode *xml_parse(const char *text, size_t len, char *err, size_t errlen);
XmlNode *xml_read_file(const char *path, size_t max_bytes, char *err, size_t errlen);
void xml_free(XmlNode *n);
const char *xml_attr(const XmlNode *n, const char *name); /* NULL if absent */
XmlNode *xml_child(const XmlNode *n, const char *name);   /* first child element with that name */
