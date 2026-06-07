#include "cli_scrape.h"

int main(int argc, char *argv[])
{
    return runCliScraper(argc, argv, BuiltinScraperEntry::PythonRequestsCompatible);
}
