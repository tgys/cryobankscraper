#pragma once

#include <QString>

extern "C" {
#include <gumbo.h>
}

namespace scrapeff::dom {

QString elAttr(const GumboNode *elNode, const char *qname);
bool hasClassToken(const GumboNode *elNode, QLatin1String token);
QString tagNameLower(const GumboNode *elNode);
/** Descendant text (TEXT/WHITESPACE/CDATA); simplified. */
QString nodeTextContent(const GumboNode *node);

} // namespace scrapeff::dom
