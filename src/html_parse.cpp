#include "html_parse.h"

#include "dom_helpers.h"

#include <QUrl>
#include <cstring>

namespace scrapeff {

using dom::elAttr;
using dom::hasClassToken;

static const GumboNode *nodeByElementId(const GumboNode *node, QLatin1String id)
{
    if (!node)
        return nullptr;
    if (node->type == GUMBO_NODE_DOCUMENT) {
        const GumboVector *ch = &node->v.document.children;
        for (unsigned i = 0; i < ch->length; ++i) {
            if (const GumboNode *f =
                    nodeByElementId(static_cast<const GumboNode *>(ch->data[i]), id))
                return f;
        }
        return nullptr;
    }
    if (node->type != GUMBO_NODE_ELEMENT)
        return nullptr;
    GumboAttribute *idAttr = gumbo_get_attribute(&node->v.element.attributes, "id");
    if (idAttr && idAttr->value && QString::fromUtf8(idAttr->value) == id)
        return node;
    const GumboVector *ch = &node->v.element.children;
    for (unsigned i = 0; i < ch->length; ++i) {
        if (const GumboNode *f =
                nodeByElementId(static_cast<const GumboNode *>(ch->data[i]), id))
            return f;
    }
    return nullptr;
}

static const GumboNode *nodeByClassName(const GumboNode *node, QLatin1String className)
{
    if (!node)
        return nullptr;
    if (node->type == GUMBO_NODE_DOCUMENT) {
        const GumboVector *ch = &node->v.document.children;
        for (unsigned i = 0; i < ch->length; ++i) {
            if (const GumboNode *f =
                    nodeByClassName(static_cast<const GumboNode *>(ch->data[i]), className))
                return f;
        }
        return nullptr;
    }
    if (node->type != GUMBO_NODE_ELEMENT)
        return nullptr;
    if (hasClassToken(node, className))
        return node;
    const GumboVector *ch = &node->v.element.children;
    for (unsigned i = 0; i < ch->length; ++i) {
        if (const GumboNode *f =
                nodeByClassName(static_cast<const GumboNode *>(ch->data[i]), className))
            return f;
    }
    return nullptr;
}

/** `output->root` is the `<html>` element (gumbo documented API). */
static const GumboNode *gumboBodyOrDocument(const GumboOutput *output)
{
    if (!output)
        return nullptr;
    const GumboNode *htmlRoot = output->root;
    if (htmlRoot && htmlRoot->type == GUMBO_NODE_ELEMENT && htmlRoot->v.element.tag == GUMBO_TAG_HTML) {
        const GumboVector *ch = &htmlRoot->v.element.children;
        for (unsigned i = 0; i < ch->length; ++i) {
            const GumboNode *n = static_cast<const GumboNode *>(ch->data[i]);
            if (n->type == GUMBO_NODE_ELEMENT && n->v.element.tag == GUMBO_TAG_BODY)
                return n;
        }
    }
    return output->document;
}

static QString resolveUrl(const QString &base, const QString &ref)
{
    if (ref.isEmpty() || ref.startsWith(QLatin1String("data:")))
        return {};
    const QUrl b(base);
    const QUrl resolved = b.resolved(QUrl(ref));
    return resolved.toString(QUrl::FullyEncoded);
}

QStringList urlsFromSrcset(const QString &srcset)
{
    QStringList out;
    const QStringList parts = srcset.split(QLatin1Char(','));
    for (QString part : parts) {
        part = part.trimmed();
        if (part.isEmpty())
            continue;
        const int sp = part.indexOf(QLatin1Char(' '));
        const QString urlPart = sp < 0 ? part : part.left(sp);
        if (!urlPart.isEmpty())
            out.append(urlPart);
    }
    return out;
}

static void addUrl(const QString &baseUrl, const QString &rel, QStringList &images, QHash<QString, bool> &seen)
{
    const QString abs = resolveUrl(baseUrl, rel);
    if (abs.isEmpty() || seen.contains(abs))
        return;
    seen.insert(abs, true);
    images.append(abs);
}

static bool isTagNamed(const GumboElement *el, const char *name)
{
    if (el->tag != GUMBO_TAG_UNKNOWN)
        return strcmp(gumbo_normalized_tagname(el->tag), name) == 0;
    // For unknown tags, check original_tag
    GumboStringPiece piece = el->original_tag;
    gumbo_tag_from_original_text(&piece);
    return piece.length == strlen(name) && strncasecmp(piece.data, name, piece.length) == 0;
}

static void collectImgsFromNode(const GumboNode *node, const QString &baseUrl, QStringList &images,
                                QHash<QString, bool> &seen)
{
    if (!node)
        return;

    if (node->type == GUMBO_NODE_ELEMENT) {
        const GumboElement *el = &node->v.element;
        const GumboTag tag = el->tag;

        // Handle <picture> element with <source> children
        if (isTagNamed(el, "picture")) {
            const GumboVector *ch = &el->children;
            for (unsigned i = 0; i < ch->length; ++i) {
                const GumboNode *c = static_cast<const GumboNode *>(ch->data[i]);
                if (c->type != GUMBO_NODE_ELEMENT || !isTagNamed(&c->v.element, "source"))
                    continue;
                const QString ss = elAttr(c, "srcset");
                if (ss.isEmpty())
                    continue;
                for (const QString &u : urlsFromSrcset(ss))
                    addUrl(baseUrl, u, images, seen);
            }
        }

        if (tag == GUMBO_TAG_IMG) {
            static const char *const attrs[] = {"src", "data-src", "data-lazy-src", "data-original"};
            for (const char *an : attrs) {
                const QString v = elAttr(node, an);
                if (!v.isEmpty())
                    addUrl(baseUrl, v, images, seen);
            }
            static const char *const srcsetAttrs[] = {"srcset", "data-srcset"};
            for (const char *an : srcsetAttrs) {
                const QString v = elAttr(node, an);
                if (v.isEmpty())
                    continue;
                for (const QString &u : urlsFromSrcset(v))
                    addUrl(baseUrl, u, images, seen);
            }
        }
    }

    if (node->type == GUMBO_NODE_ELEMENT) {
        const GumboVector *ch = &node->v.element.children;
        for (unsigned i = 0; i < ch->length; ++i)
            collectImgsFromNode(static_cast<const GumboNode *>(ch->data[i]), baseUrl, images, seen);
    }
}

QStringList extractImageLinks(const QString &htmlUtf8, const QString &baseUrl)
{
    QStringList images;
    QHash<QString, bool> seen;
    QByteArray utf = htmlUtf8.toUtf8();

    GumboOutput *output = gumbo_parse_with_options(&kGumboDefaultOptions, utf.constData(), size_t(utf.size()));
    if (!output)
        return images;

    const GumboNode *bodyRoot = gumboBodyOrDocument(output);
    if (!bodyRoot) {
        gumbo_destroy_output(&kGumboDefaultOptions, output);
        return images;
    }

    // Try finding by class first (div.main > div.foto)
    const GumboNode *mainNd = nodeByClassName(bodyRoot, QLatin1String("main"));
    if (mainNd && mainNd->type == GUMBO_NODE_ELEMENT) {
        const GumboNode *fotoNd = nodeByClassName(mainNd, QLatin1String("foto"));
        if (fotoNd) {
            collectImgsFromNode(fotoNd, baseUrl, images, seen);
        } else {
            // No .foto inside .main, collect from .main directly
            collectImgsFromNode(mainNd, baseUrl, images, seen);
        }
    }
    // Fallback: try by ID (id="main" > id="foto")
    if (images.isEmpty()) {
        const GumboNode *mainById = nodeByElementId(bodyRoot, QLatin1String("main"));
        if (mainById && mainById->type == GUMBO_NODE_ELEMENT) {
            const GumboNode *fotoById = nodeByElementId(mainById, QLatin1String("foto"));
            if (fotoById)
                collectImgsFromNode(fotoById, baseUrl, images, seen);
        }
    }

    gumbo_destroy_output(&kGumboDefaultOptions, output);
    return images;
}

static void walkDownloadAnchors(const GumboNode *node, const QString &baseUrl, ProfileDownloadLinks &links)
{
    if (!node)
        return;

    if (node->type == GUMBO_NODE_ELEMENT && node->v.element.tag == GUMBO_TAG_A) {
        QString href = elAttr(node, "href").trimmed();
        if (!href.isEmpty() && hasClassToken(node, QLatin1String("sys__download-link"))) {
            QString label;
            const GumboNode *parent = node->parent;
            if (parent && parent->type == GUMBO_NODE_ELEMENT && parent->v.element.tag == GUMBO_TAG_LI) {
                const GumboVector *pch = &parent->v.element.children;
                for (unsigned i = 0; i < pch->length; ++i) {
                    const GumboNode *ch = static_cast<const GumboNode *>(pch->data[i]);
                    if (ch->type != GUMBO_NODE_ELEMENT || ch->v.element.tag != GUMBO_TAG_SPAN)
                        continue;
                    if (!hasClassToken(ch, QLatin1String("span-02")))
                        continue;
                    label = dom::nodeTextContent(ch).toLower();
                    break;
                }
            }
            const QString full = resolveUrl(baseUrl, href);
            if (label.contains(QLatin1String("summary")) && label.contains(QLatin1String("profile")))
                links.summaryProfile = full;
            else if (label.contains(QLatin1String("medical")) && label.contains(QLatin1String("profile")))
                links.medicalProfile = full;
            else if (label.contains(QLatin1String("personal")) && label.contains(QLatin1String("profile")))
                links.personalProfile = full;
        }
    }

    if (node->type == GUMBO_NODE_ELEMENT) {
        const GumboVector *ch = &node->v.element.children;
        for (unsigned i = 0; i < ch->length; ++i)
            walkDownloadAnchors(static_cast<const GumboNode *>(ch->data[i]), baseUrl, links);
    }
}

ProfileDownloadLinks findDonorDownloadLinks(const QString &htmlUtf8, const QString &baseUrl)
{
    ProfileDownloadLinks links;
    QByteArray utf = htmlUtf8.toUtf8();

    GumboOutput *output = gumbo_parse_with_options(&kGumboDefaultOptions, utf.constData(), size_t(utf.size()));
    if (!output)
        return links;

    const GumboNode *start = gumboBodyOrDocument(output);
    if (start)
        walkDownloadAnchors(start, baseUrl, links);
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    return links;
}

QStringList profileDocumentUrlsSummaryMedical(const QString &htmlUtf8, const QString &baseUrl)
{
    const ProfileDownloadLinks L = findDonorDownloadLinks(htmlUtf8, baseUrl);
    QStringList out;
    auto add = [&](const QString &u) {
        if (!u.isEmpty() && !out.contains(u))
            out.append(u);
    };
    add(L.summaryProfile);
    add(L.medicalProfile);
    return out;
}

QStringList profileDocumentUrlsAllProfileDocs(const QString &htmlUtf8, const QString &baseUrl)
{
    const ProfileDownloadLinks L = findDonorDownloadLinks(htmlUtf8, baseUrl);
    QStringList out;
    auto add = [&](const QString &u) {
        if (!u.isEmpty() && !out.contains(u))
            out.append(u);
    };
    add(L.summaryProfile);
    add(L.medicalProfile);
    add(L.personalProfile);
    return out;
}

} // namespace scrapeff
