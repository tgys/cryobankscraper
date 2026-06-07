#include "cli_scrape.h"

int main(int argc, char *argv[])
{
    // Default: Chromium-style navigation (what gallery_qt spawns). Many origin/CDN stacks return bogus 404 /
    // empty bodies for identifiable script clients unless --urllib-compat is used explicitly.
    return runCliScraper(argc, argv, BuiltinScraperEntry::ScrapeffDefault);
}
