#include "dom_helpers.h"

#include <QStringList>

namespace scrapeff::dom {

QString elAttr(const GumboNode *node, const char *qname)
{
    if (!node || node->type != GUMBO_NODE_ELEMENT)
        return {};
    GumboAttribute *attr = gumbo_get_attribute(&node->v.element.attributes, qname);
    if (!attr || !attr->value)
        return {};
    return QString::fromUtf8(attr->value);
}

bool hasClassToken(const GumboNode *node, QLatin1String token)
{
    const QString cls = elAttr(node, "class");
    if (cls.isEmpty())
        return false;
    const auto parts = cls.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        if (p == token)
            return true;
    }
    return false;
}

QString tagNameLower(const GumboNode *node)
{
    if (!node || node->type != GUMBO_NODE_ELEMENT)
        return {};
    return QString::fromUtf8(gumbo_normalized_tagname(node->v.element.tag)).toLower();
}

static void appendTextRecursive(const GumboNode *node, QString *out)
{
    if (!node || !out)
        return;
    switch (node->type) {
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_WHITESPACE:
    case GUMBO_NODE_CDATA:
        *out += QString::fromUtf8(node->v.text.text);
        break;
    case GUMBO_NODE_ELEMENT: {
        const GumboVector *ch = &node->v.element.children;
        for (unsigned i = 0; i < ch->length; ++i)
            appendTextRecursive(static_cast<const GumboNode *>(ch->data[i]), out);
        break;
    }
    default:
        break;
    }
}

QString nodeTextContent(const GumboNode *node)
{
    QString out;
    if (!node)
        return {};
    if (node->type == GUMBO_NODE_DOCUMENT) {
        const GumboVector *ch = &node->v.document.children;
        for (unsigned i = 0; i < ch->length; ++i)
            appendTextRecursive(static_cast<const GumboNode *>(ch->data[i]), &out);
    } else {
        appendTextRecursive(node, &out);
    }
    return out.simplified();
}

} // namespace scrapeff::dom
