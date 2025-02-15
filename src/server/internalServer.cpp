/*
 * Copyright 2019 Matthieu Gautier <mgautier@kymeria.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU  General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include "internalServer.h"

#ifdef __FreeBSD__
#include <netinet/in.h>
#endif

#ifdef _WIN32
# if !defined(__MINGW32__) && (_MSC_VER < 1600)
#   include "stdint4win.h"
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# ifdef __GNUC__
  // inet_pton is not declared in mingw, even if the function exists.
  extern "C" {
    WINSOCK_API_LINKAGE  INT WSAAPI inet_pton( INT Family, PCSTR pszAddrString, PVOID pAddrBuf);
  }
# endif
  typedef UINT64 uint64_t;
  typedef UINT16 uint16_t;
#endif

extern "C" {
#include "microhttpd_wrapper.h"
}

#include "tools.h"
#include "tools/pathTools.h"
#include "tools/regexTools.h"
#include "tools/stringTools.h"
#include "tools/archiveTools.h"
#include "tools/networkTools.h"
#include "library.h"
#include "name_mapper.h"
#include "entry.h"
#include "searcher.h"
#include "search_renderer.h"
#include "opds_dumper.h"
#include "i18n.h"

#include <zim/uuid.h>
#include <zim/error.h>
#include <zim/entry.h>
#include <zim/item.h>

#include <mustache.hpp>

#include <atomic>
#include <string>
#include <vector>
#include <chrono>
#include "kiwixlib-resources.h"

#ifndef _WIN32
# include <arpa/inet.h>
#endif

#include "request_context.h"
#include "response.h"

#define MAX_SEARCH_LEN 140
#define DEFAULT_CACHE_SIZE 2

namespace kiwix {

namespace
{

inline std::string normalizeRootUrl(std::string rootUrl)
{
  while ( !rootUrl.empty() && rootUrl.back() == '/' )
    rootUrl.pop_back();

  while ( !rootUrl.empty() && rootUrl.front() == '/' )
    rootUrl = rootUrl.substr(1);
  return rootUrl.empty() ? rootUrl : "/" + rootUrl;
}

// Returns the value of env var `name` if found, otherwise returns defaultVal
unsigned int getCacheLength(const char* name, unsigned int defaultVal) {
  try {
    const char* envString = std::getenv(name);
    if (envString == nullptr) {
      throw std::runtime_error("Environment variable not set");
    }
    return extractFromString<unsigned int>(envString);
  } catch (...) {}

  return defaultVal;
}
} // unnamed namespace

SearchInfo::SearchInfo(const std::string& pattern)
  : pattern(pattern),
    geoQuery()
{}

SearchInfo::SearchInfo(const std::string& pattern, GeoQuery geoQuery)
  : pattern(pattern),
    geoQuery(geoQuery)
{}

SearchInfo::SearchInfo(const RequestContext& request)
  : pattern(request.get_optional_param<std::string>("pattern", "")),
    geoQuery(),
    bookName(request.get_optional_param<std::string>("content", ""))
{
  /* Retrive geo search */
  try {
    auto latitude = request.get_argument<float>("latitude");
    auto longitude = request.get_argument<float>("longitude");
    auto distance = request.get_argument<float>("distance");
    geoQuery = GeoQuery(latitude, longitude, distance);
  } catch(const std::out_of_range&) {}
    catch(const std::invalid_argument&) {}

  if (!geoQuery && pattern.empty()) {
    throw std::invalid_argument("No query provided.");
  }
}

zim::Query SearchInfo::getZimQuery(bool verbose) const {
  zim::Query query;
  if (verbose) {
    std::cout << "Performing query '" << pattern<< "'";
  }
  query.setQuery(pattern);
  if (geoQuery) {
    if (verbose) {
      std::cout << " with geo query '" << geoQuery.distance << "&(" << geoQuery.latitude << ";" << geoQuery.longitude << ")'";
    }
    query.setGeorange(geoQuery.latitude, geoQuery.longitude, geoQuery.distance);
  }
  if (verbose) {
    std::cout << std::endl;
  }
  return query;
}


static IdNameMapper defaultNameMapper;

static MHD_Result staticHandlerCallback(void* cls,
                                        struct MHD_Connection* connection,
                                        const char* url,
                                        const char* method,
                                        const char* version,
                                        const char* upload_data,
                                        size_t* upload_data_size,
                                        void** cont_cls);


InternalServer::InternalServer(Library* library,
                               NameMapper* nameMapper,
                               std::string addr,
                               int port,
                               std::string root,
                               int nbThreads,
                               bool verbose,
                               bool withTaskbar,
                               bool withLibraryButton,
                               bool blockExternalLinks,
                               std::string indexTemplateString,
                               int ipConnectionLimit) :
  m_addr(addr),
  m_port(port),
  m_root(normalizeRootUrl(root)),
  m_nbThreads(nbThreads),
  m_verbose(verbose),
  m_withTaskbar(withTaskbar),
  m_withLibraryButton(withLibraryButton),
  m_blockExternalLinks(blockExternalLinks),
  m_indexTemplateString(indexTemplateString.empty() ? RESOURCE::templates::index_html : indexTemplateString),
  m_ipConnectionLimit(ipConnectionLimit),
  mp_daemon(nullptr),
  mp_library(library),
  mp_nameMapper(nameMapper ? nameMapper : &defaultNameMapper),
  searcherCache(getCacheLength("SEARCHER_CACHE_SIZE", std::max((unsigned int) (mp_library->getBookCount(true, true)*0.1), 1U))),
  searchCache(getCacheLength("SEARCH_CACHE_SIZE", DEFAULT_CACHE_SIZE)),
  suggestionSearcherCache(getCacheLength("SUGGESTION_SEARCHER_CACHE_SIZE", std::max((unsigned int) (mp_library->getBookCount(true, true)*0.1), 1U)))
{}

bool InternalServer::start() {
#ifdef _WIN32
  int flags = MHD_USE_SELECT_INTERNALLY;
#else
  int flags = MHD_USE_POLL_INTERNALLY;
#endif
  if (m_verbose.load())
    flags |= MHD_USE_DEBUG;

  struct sockaddr_in sockAddr;
  memset(&sockAddr, 0, sizeof(sockAddr));
  sockAddr.sin_family = AF_INET;
  sockAddr.sin_port = htons(m_port);
  if (m_addr.empty()) {
    if (0 != INADDR_ANY) {
      sockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    m_addr = kiwix::getBestPublicIp();
  } else {
    if (inet_pton(AF_INET, m_addr.c_str(), &(sockAddr.sin_addr.s_addr)) == 0) {
      std::cerr << "Ip address " << m_addr << "  is not a valid ip address" << std::endl;
      return false;
    }
  }
  mp_daemon = MHD_start_daemon(flags,
                            m_port,
                            NULL,
                            NULL,
                            &staticHandlerCallback,
                            this,
                            MHD_OPTION_SOCK_ADDR, &sockAddr,
                            MHD_OPTION_THREAD_POOL_SIZE, m_nbThreads,
                            MHD_OPTION_PER_IP_CONNECTION_LIMIT, m_ipConnectionLimit,
                            MHD_OPTION_END);
  if (mp_daemon == nullptr) {
    std::cerr << "Unable to instantiate the HTTP daemon. The port " << m_port
              << " is maybe already occupied or need more permissions to be open. "
                 "Please try as root or with a port number higher or equal to 1024."
              << std::endl;
    return false;
  }
  auto server_start_time = std::chrono::system_clock::now().time_since_epoch();
  m_server_id = kiwix::to_string(server_start_time.count());
  m_library_id = m_server_id;
  return true;
}

void InternalServer::stop()
{
  MHD_stop_daemon(mp_daemon);
}

static MHD_Result staticHandlerCallback(void* cls,
                                        struct MHD_Connection* connection,
                                        const char* url,
                                        const char* method,
                                        const char* version,
                                        const char* upload_data,
                                        size_t* upload_data_size,
                                        void** cont_cls)
{
  InternalServer* _this = static_cast<InternalServer*>(cls);

  return _this->handlerCallback(connection,
                                url,
                                method,
                                version,
                                upload_data,
                                upload_data_size,
                                cont_cls);
}

MHD_Result InternalServer::handlerCallback(struct MHD_Connection* connection,
                                           const char* url,
                                           const char* method,
                                           const char* version,
                                           const char* upload_data,
                                           size_t* upload_data_size,
                                           void** cont_cls)
{
  auto start_time = std::chrono::steady_clock::now();
  if (m_verbose.load() ) {
    printf("======================\n");
    printf("Requesting : \n");
    printf("full_url  : %s\n", url);
  }
  RequestContext request(connection, m_root, url, method, version);

  if (m_verbose.load() ) {
    request.print_debug_info();
  }
  /* Unexpected method */
  if (request.get_method() != RequestMethod::GET
   && request.get_method() != RequestMethod::POST
   && request.get_method() != RequestMethod::HEAD) {
    printf("Reject request because of unhandled request method.\n");
    printf("----------------------\n");
    return MHD_NO;
  }

  auto response = handle_request(request);

  if (response->getReturnCode() == MHD_HTTP_INTERNAL_SERVER_ERROR) {
    printf("========== INTERNAL ERROR !! ============\n");
    if (!m_verbose.load()) {
      printf("Requesting : \n");
      printf("full_url : %s\n", url);
      request.print_debug_info();
    }
  }

  if (response->getReturnCode() == MHD_HTTP_OK && !etag_not_needed(request))
    response->set_server_id(m_server_id);

  auto ret = response->send(request, connection);
  auto end_time = std::chrono::steady_clock::now();
  auto time_span = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time);
  if (m_verbose.load()) {
    printf("Request time : %fs\n", time_span.count());
    printf("----------------------\n");
  }
  return ret;
}

std::unique_ptr<Response> InternalServer::handle_request(const RequestContext& request)
{
  try {
    if (! request.is_valid_url()) {
      return HTTP404HtmlResponse(*this, request)
             + urlNotFoundMsg;
    }

    const ETag etag = get_matching_if_none_match_etag(request);
    if ( etag )
      return Response::build_304(*this, etag);

    if (startsWith(request.get_url(), "/skin/"))
      return handle_skin(request);

    if (startsWith(request.get_url(), "/catalog/"))
      return handle_catalog(request);

    if (startsWith(request.get_url(), "/raw/"))
      return handle_raw(request);

    if (request.get_url() == "/search")
      return handle_search(request);

    if (request.get_url() == "/suggest")
     return handle_suggest(request);

    if (request.get_url() == "/random")
      return handle_random(request);

    if (request.get_url() == "/catch/external")
      return handle_captured_external(request);

    return handle_content(request);
  } catch (std::exception& e) {
    fprintf(stderr, "===== Unhandled error : %s\n", e.what());
    return HTTP500HtmlResponse(*this, request)
         + e.what();
  } catch (...) {
    fprintf(stderr, "===== Unhandled unknown error\n");
    return HTTP500HtmlResponse(*this, request)
         + "Unknown error";
  }
}

MustacheData InternalServer::get_default_data() const
{
  MustacheData data;
  data.set("root", m_root);
  return data;
}

bool InternalServer::etag_not_needed(const RequestContext& request) const
{
  const std::string url = request.get_url();
  return kiwix::startsWith(url, "/catalog")
      || url == "/search"
      || url == "/suggest"
      || url == "/random"
      || url == "/catch/external";
}

ETag
InternalServer::get_matching_if_none_match_etag(const RequestContext& r) const
{
  try {
    const std::string etag_list = r.get_header(MHD_HTTP_HEADER_IF_NONE_MATCH);
    return ETag::match(etag_list, m_server_id);
  } catch (const std::out_of_range&) {
    return ETag();
  }
}

std::unique_ptr<Response> InternalServer::build_homepage(const RequestContext& request)
{
  return ContentResponse::build(*this, m_indexTemplateString, get_default_data(), "text/html; charset=utf-8", true);
}

/**
 * Archive and Zim handlers begin
 **/

SuggestionsList_t getSuggestions(SuggestionSearcherCache& cache, const zim::Archive* const archive,
                  const std::string& bookId, const std::string& queryString, int start, int suggestionCount)
{
  SuggestionsList_t suggestions;
  std::shared_ptr<zim::SuggestionSearcher> searcher;
  searcher = cache.getOrPut(bookId, [=](){ return make_shared<zim::SuggestionSearcher>(*archive); });

  if (archive->hasTitleIndex()) {
    auto search = searcher->suggest(queryString);
    auto srs = search.getResults(start, suggestionCount);

    for (auto it : srs) {
      SuggestionItem suggestion(it.getTitle(), kiwix::normalize(it.getTitle()),
                                it.getPath(), it.getSnippet());
      suggestions.push_back(suggestion);
    }
  } else {
    // TODO: This case should be handled by libzim
    std::vector<std::string> variants = getTitleVariants(queryString);
    int currCount = 0;
    for (auto it = variants.begin(); it != variants.end() && currCount < suggestionCount; it++) {
      auto search = searcher->suggest(queryString);
      auto srs = search.getResults(0, suggestionCount);
      for (auto it : srs) {
        SuggestionItem suggestion(it.getTitle(), kiwix::normalize(it.getTitle()),
                                  it.getPath());
        suggestions.push_back(suggestion);
        currCount++;
      }
    }
  }
  return suggestions;
}

namespace
{

std::string makeFulltextSearchSuggestion(const std::string& lang, const std::string& queryString)
{
  return i18n::expandParameterizedString(lang, "suggest-full-text-search",
               {
                  {"SEARCH_TERMS", queryString}
               }
         );
}

ParameterizedMessage noSuchBookErrorMsg(const std::string& bookName)
{
  return ParameterizedMessage("no-such-book", { {"BOOK_NAME", bookName} });
}

ParameterizedMessage invalidRawAccessMsg(const std::string& dt)
{
  return ParameterizedMessage("invalid-raw-data-type", { {"DATATYPE", dt} });
}

ParameterizedMessage rawEntryNotFoundMsg(const std::string& dt, const std::string& entry)
{
  return ParameterizedMessage("raw-entry-not-found",
                              {
                                {"DATATYPE", dt},
                                {"ENTRY", entry},
                              }
  );
}

ParameterizedMessage nonParameterizedMessage(const std::string& msgId)
{
  const ParameterizedMessage::Parameters noParams;
  return ParameterizedMessage(msgId, noParams);
}

} // unnamed namespace

std::unique_ptr<Response> InternalServer::handle_suggest(const RequestContext& request)
{
  if (m_verbose.load()) {
    printf("** running handle_suggest\n");
  }

  std::string bookName, bookId;
  std::shared_ptr<zim::Archive> archive;
  try {
    bookName = request.get_argument("content");
    bookId = mp_nameMapper->getIdForName(bookName);
    archive = mp_library->getArchiveById(bookId);
  } catch (const std::out_of_range&) {
    // error handled by the archive == nullptr check below
  }

  if (archive == nullptr) {
    return HTTP404HtmlResponse(*this, request)
           + noSuchBookErrorMsg(bookName)
           + TaskbarInfo(bookName);
  }

  const auto queryString = request.get_optional_param("term", std::string());
  const auto start = request.get_optional_param<unsigned int>("start", 0);
  unsigned int count = request.get_optional_param<unsigned int>("count", 10);
  if (count == 0) {
    count = 10;
  }

  if (m_verbose.load()) {
    printf("Searching suggestions for: \"%s\"\n", queryString.c_str());
  }

  MustacheData results{MustacheData::type::list};

  bool first = true;

  /* Get the suggestions */
  SuggestionsList_t suggestions = getSuggestions(suggestionSearcherCache, archive.get(),
                                                  bookId, queryString, start, count);
  for(auto& suggestion:suggestions) {
    MustacheData result;
    result.set("label", suggestion.getTitle());

    if (suggestion.hasSnippet()) {
      result.set("label", suggestion.getSnippet());
    }

    result.set("value", suggestion.getTitle());
    result.set("kind", "path");
    result.set("path", suggestion.getPath());
    result.set("first", first);
    first = false;
    results.push_back(result);
  }


  /* Propose the fulltext search if possible */
  if (archive->hasFulltextIndex()) {
    MustacheData result;
    const auto lang = request.get_user_language();
    result.set("label", makeFulltextSearchSuggestion(lang, queryString));
    result.set("value", queryString + " ");
    result.set("kind", "pattern");
    result.set("first", first);
    results.push_back(result);
  }

  auto data = get_default_data();
  data.set("suggestions", results);

  auto response = ContentResponse::build(*this, RESOURCE::templates::suggestion_json, data, "application/json; charset=utf-8");
  return std::move(response);
}

std::unique_ptr<Response> InternalServer::handle_skin(const RequestContext& request)
{
  if (m_verbose.load()) {
    printf("** running handle_skin\n");
  }

  auto resourceName = request.get_url().substr(1);
  try {
    auto response = ContentResponse::build(
        *this,
        getResource(resourceName),
        getMimeTypeForFile(resourceName));
    response->set_cacheable();
    return std::move(response);
  } catch (const ResourceNotFound& e) {
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg;
  }
}

std::unique_ptr<Response> InternalServer::handle_search(const RequestContext& request)
{
  if (m_verbose.load()) {
    printf("** running handle_search\n");
  }

  try {
    auto searchInfo = SearchInfo(request);

    std::string bookId;
    std::shared_ptr<zim::Archive> archive;
    if (!searchInfo.bookName.empty()) {
      try {
        bookId = mp_nameMapper->getIdForName(searchInfo.bookName);
        archive = mp_library->getArchiveById(bookId);
      } catch (const std::out_of_range&) {
        throw std::invalid_argument("The requested book doesn't exist.");
      }
    }


    /* Make the search */
    // Try to get a search from the searchInfo, else build it
    std::shared_ptr<zim::Search> search;
    try {
      search = searchCache.getOrPut(searchInfo,
        [=](){
          std::shared_ptr<zim::Searcher> searcher;
          if (archive) {
            searcher = searcherCache.getOrPut(bookId, [=](){ return std::make_shared<zim::Searcher>(*archive);});
          } else {
            for (auto& bookId: mp_library->filter(kiwix::Filter().local(true).valid(true))) {
              auto currentArchive = mp_library->getArchiveById(bookId);
              if (currentArchive) {
                if (! searcher) {
                  searcher = std::make_shared<zim::Searcher>(*currentArchive);
                } else {
                  searcher->addArchive(*currentArchive);
                }
              }
           }
          }
          return make_shared<zim::Search>(searcher->search(searchInfo.getZimQuery(m_verbose.load())));
        }
      );
    } catch(std::runtime_error& e) {
      // Searcher->search will throw a runtime error if there is no valid xapian database to do the search.
      // (in case of zim file not containing a index)
      return HTTPErrorHtmlResponse(*this, request, MHD_HTTP_NOT_FOUND,
                                   "fulltext-search-unavailable",
                                   "404-page-heading",
                                   m_root + "/skin/search_results.css")
           + nonParameterizedMessage("no-search-results")
           + TaskbarInfo(searchInfo.bookName, archive.get());
    }


    auto start = 0;
    try {
      start = request.get_argument<unsigned int>("start");
    } catch (const std::exception&) {}

    auto pageLength = 25;
    try {
      pageLength = request.get_argument<unsigned int>("pageLength");
    } catch (const std::exception&) {}
    if (pageLength > MAX_SEARCH_LEN) {
      pageLength = MAX_SEARCH_LEN;
    }
    if (pageLength == 0) {
      pageLength = 25;
    }

    /* Get the results */
    SearchRenderer renderer(search->getResults(start, pageLength), mp_nameMapper, mp_library, start,
                            search->getEstimatedMatches());
    renderer.setSearchPattern(searchInfo.pattern);
    renderer.setSearchContent(searchInfo.bookName);
    renderer.setProtocolPrefix(m_root + "/");
    renderer.setSearchProtocolPrefix(m_root + "/search?");
    renderer.setPageLength(pageLength);
    auto response = ContentResponse::build(*this, renderer.getHtml(), "text/html; charset=utf-8");
    response->set_taskbar(searchInfo.bookName, archive.get());
    return std::move(response);
  } catch (const std::invalid_argument& e) {
    return HTTP400HtmlResponse(*this, request)
      + invalidUrlMsg
      + std::string(e.what());
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return HTTP500HtmlResponse(*this, request)
         + e.what();
  }
}

std::unique_ptr<Response> InternalServer::handle_random(const RequestContext& request)
{
  if (m_verbose.load()) {
    printf("** running handle_random\n");
  }

  std::string bookName;
  std::shared_ptr<zim::Archive> archive;
  try {
    bookName = request.get_argument("content");
    const std::string bookId = mp_nameMapper->getIdForName(bookName);
    archive = mp_library->getArchiveById(bookId);
  } catch (const std::out_of_range&) {
    // error handled by the archive == nullptr check below
  }

  if (archive == nullptr) {
    return HTTP404HtmlResponse(*this, request)
           + noSuchBookErrorMsg(bookName)
           + TaskbarInfo(bookName);
  }

  try {
    auto entry = archive->getRandomEntry();
    return build_redirect(bookName, getFinalItem(*archive, entry));
  } catch(zim::EntryNotFound& e) {
    return HTTP404HtmlResponse(*this, request)
           + nonParameterizedMessage("random-article-failure")
           + TaskbarInfo(bookName, archive.get());
  }
}

std::unique_ptr<Response> InternalServer::handle_captured_external(const RequestContext& request)
{
  std::string source = "";
  try {
    source = kiwix::urlDecode(request.get_argument("source"));
  } catch (const std::out_of_range& e) {}

  if (source.empty()) {
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg;
  }

  auto data = get_default_data();
  data.set("source", source);
  return ContentResponse::build(*this, RESOURCE::templates::captured_external_html, data, "text/html; charset=utf-8");
}

std::unique_ptr<Response> InternalServer::handle_catalog(const RequestContext& request)
{
  if (m_verbose.load()) {
    printf("** running handle_catalog");
  }

  std::string host;
  std::string url;
  try {
    host = request.get_header("Host");
    url  = request.get_url_part(1);
  } catch (const std::out_of_range&) {
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg;
  }

  if (url == "v2") {
    return handle_catalog_v2(request);
  }

  if (url != "searchdescription.xml" && url != "root.xml" && url != "search") {
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg;
  }

  if (url == "searchdescription.xml") {
    auto response = ContentResponse::build(*this, RESOURCE::opensearchdescription_xml, get_default_data(), "application/opensearchdescription+xml");
    return std::move(response);
  }

  zim::Uuid uuid;
  kiwix::OPDSDumper opdsDumper(mp_library);
  opdsDumper.setRootLocation(m_root);
  opdsDumper.setLibraryId(m_library_id);
  std::vector<std::string> bookIdsToDump;
  if (url == "root.xml") {
    uuid = zim::Uuid::generate(host);
    bookIdsToDump = mp_library->filter(kiwix::Filter().valid(true).local(true).remote(true));
  } else if (url == "search") {
    bookIdsToDump = search_catalog(request, opdsDumper);
    uuid = zim::Uuid::generate();
  }

  auto response = ContentResponse::build(
      *this,
      opdsDumper.dumpOPDSFeed(bookIdsToDump, request.get_query()),
      "application/atom+xml; profile=opds-catalog; kind=acquisition; charset=utf-8");
  return std::move(response);
}

namespace
{

Filter get_search_filter(const RequestContext& request)
{
    auto filter = kiwix::Filter().valid(true).local(true);
    try {
      filter.query(request.get_argument("q"));
    } catch (const std::out_of_range&) {}
    try {
      filter.maxSize(request.get_argument<unsigned long>("maxsize"));
    } catch (...) {}
    try {
      filter.name(request.get_argument("name"));
    } catch (const std::out_of_range&) {}
    try {
      filter.category(request.get_argument("category"));
    } catch (const std::out_of_range&) {}
    try {
      filter.lang(request.get_argument("lang"));
    } catch (const std::out_of_range&) {}
    try {
      filter.acceptTags(kiwix::split(request.get_argument("tag"), ";"));
    } catch (...) {}
    try {
      filter.rejectTags(kiwix::split(request.get_argument("notag"), ";"));
    } catch (...) {}
    return filter;
}

template<class T>
std::vector<T> subrange(const std::vector<T>& v, size_t s, size_t n)
{
  const size_t e = std::min(v.size(), s+n);
  return std::vector<T>(v.begin()+std::min(v.size(), s), v.begin()+e);
}

} // unnamed namespace

std::vector<std::string>
InternalServer::search_catalog(const RequestContext& request,
                               kiwix::OPDSDumper& opdsDumper)
{
    const auto filter = get_search_filter(request);
    const std::string q = filter.hasQuery()
                        ? filter.getQuery()
                        : "<Empty query>";
    std::vector<std::string> bookIdsToDump = mp_library->filter(filter);
    const auto totalResults = bookIdsToDump.size();
    const size_t count = request.get_optional_param("count", 10UL);
    const size_t startIndex = request.get_optional_param("start", 0UL);
    const size_t intendedCount = count > 0 ? count : bookIdsToDump.size();
    bookIdsToDump = subrange(bookIdsToDump, startIndex, intendedCount);
    opdsDumper.setOpenSearchInfo(totalResults, startIndex, bookIdsToDump.size());
    return bookIdsToDump;
}

namespace
{

std::string get_book_name(const RequestContext& request)
{
  try {
    return request.get_url_part(0);
  } catch (const std::out_of_range& e) {
    return std::string();
  }
}

ParameterizedMessage suggestSearchMsg(const std::string& searchURL, const std::string& pattern)
{
  return ParameterizedMessage("suggest-search",
                              {
                                { "PATTERN",    pattern   },
                                { "SEARCH_URL", searchURL }
                              });
}

} // unnamed namespace

std::unique_ptr<Response>
InternalServer::build_redirect(const std::string& bookName, const zim::Item& item) const
{
  auto redirectUrl = m_root + "/" + bookName + "/" + kiwix::urlEncode(item.getPath());
  return Response::build_redirect(*this, redirectUrl);
}

std::unique_ptr<Response> InternalServer::handle_content(const RequestContext& request)
{
  const std::string url = request.get_url();
  const std::string pattern = url.substr((url.find_last_of('/'))+1);
  if (m_verbose.load()) {
    printf("** running handle_content\n");
  }

  const std::string bookName = get_book_name(request);
  if (bookName.empty())
    return build_homepage(request);

  std::shared_ptr<zim::Archive> archive;
  try {
    const std::string bookId = mp_nameMapper->getIdForName(bookName);
    archive = mp_library->getArchiveById(bookId);
  } catch (const std::out_of_range& e) {}

  if (archive == nullptr) {
    const std::string searchURL = m_root + "/search?pattern=" + kiwix::urlEncode(pattern, true);
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg
           + suggestSearchMsg(searchURL, kiwix::urlDecode(pattern))
           + TaskbarInfo(bookName);
  }

  auto urlStr = request.get_url().substr(bookName.size()+1);
  if (urlStr[0] == '/') {
    urlStr = urlStr.substr(1);
  }

  try {
    auto entry = getEntryFromPath(*archive, urlStr);
    if (entry.isRedirect() || urlStr.empty()) {
      // If urlStr is empty, we want to mainPage.
      // We must do a redirection to the real page.
      return build_redirect(bookName, getFinalItem(*archive, entry));
    }
    auto response = ItemResponse::build(*this, request, entry.getItem());
    try {
      dynamic_cast<ContentResponse&>(*response).set_taskbar(bookName, archive.get());
    } catch (std::bad_cast& e) {}

    if (m_verbose.load()) {
      printf("Found %s\n", entry.getPath().c_str());
      printf("mimeType: %s\n", entry.getItem(true).getMimetype().c_str());
    }

    return response;
  } catch(zim::EntryNotFound& e) {
    if (m_verbose.load())
      printf("Failed to find %s\n", urlStr.c_str());

    std::string searchURL = m_root + "/search?content=" + bookName + "&pattern=" + kiwix::urlEncode(pattern, true);
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg
           + suggestSearchMsg(searchURL, kiwix::urlDecode(pattern))
           + TaskbarInfo(bookName, archive.get());
  }
}


std::unique_ptr<Response> InternalServer::handle_raw(const RequestContext& request)
{
  if (m_verbose.load()) {
    printf("** running handle_raw\n");
  }

  std::string bookName;
  std::string kind;
  try {
     bookName = request.get_url_part(1);
     kind = request.get_url_part(2);
  } catch (const std::out_of_range& e) {
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg;
  }

  if (kind != "meta" && kind!= "content") {
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg
           + invalidRawAccessMsg(kind);
  }

  std::shared_ptr<zim::Archive> archive;
  try {
    const std::string bookId = mp_nameMapper->getIdForName(bookName);
    archive = mp_library->getArchiveById(bookId);
  } catch (const std::out_of_range& e) {}

  if (archive == nullptr) {
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg
           + noSuchBookErrorMsg(bookName);
  }

  // Remove the beggining of the path:
  // /raw/<bookName>/<kind>/foo
  // ^^^^^          ^      ^
  //   5      +     1  +   1   = 7
  auto itemPath = request.get_url().substr(bookName.size()+kind.size()+7);

  try {
    if (kind == "meta") {
      auto item = archive->getMetadataItem(itemPath);
      return ItemResponse::build(*this, request, item, /*raw=*/true);
    } else {
      auto entry = archive->getEntryByPath(itemPath);
      if (entry.isRedirect()) {
        return build_redirect(bookName, entry.getItem(true));
      }
      return ItemResponse::build(*this, request, entry.getItem(), /*raw=*/true);
    }
  } catch (zim::EntryNotFound& e ) {
    if (m_verbose.load()) {
      printf("Failed to find %s\n", itemPath.c_str());
    }
    return HTTP404HtmlResponse(*this, request)
           + urlNotFoundMsg
           + rawEntryNotFoundMsg(kind, itemPath);
  }
}

}
