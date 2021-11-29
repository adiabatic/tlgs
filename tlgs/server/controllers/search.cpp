#include <algorithm>
#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>
#include <drogon/HttpAppFramework.h>
#include <tlgsutils/utils.hpp>
#include <tlgsutils/counter.hpp>
#include <tlgsutils/url_parser.hpp>
#include <nlohmann/json.hpp>
#include <ranges>
#include <atomic>
#include <regex>

#include "search_result.hpp"

using namespace drogon;

struct RankedResult
{
    std::string url;
    std::string content_type;
    size_t size;
    float score;
};

struct SearchController : public HttpController<SearchController>
{
public:
    Task<HttpResponsePtr> tlgs_search(HttpRequestPtr req);
    Task<HttpResponsePtr> add_seed(HttpRequestPtr req);
    Task<HttpResponsePtr> jump_search(HttpRequestPtr req, std::string search_term);
    Task<HttpResponsePtr> backlinks(HttpRequestPtr req);

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SearchController::tlgs_search, "/search", {Get});
    ADD_METHOD_TO(SearchController::tlgs_search, "/search/{page}", {Get});
    ADD_METHOD_TO(SearchController::jump_search, "/search_jump/{search_term}", {Get});
    ADD_METHOD_TO(SearchController::tlgs_search, "/v/search", {Get});
    ADD_METHOD_TO(SearchController::tlgs_search, "/v/search/{page}", {Get});
    ADD_METHOD_TO(SearchController::jump_search, "/v/search_jump/{search_term}", {Get});
    ADD_METHOD_TO(SearchController::backlinks, "/backlinks", {Get});
    METHOD_LIST_END


    Task<std::vector<RankedResult>> hitsSearch(const std::string query_str, bool find_auths = true);
    std::atomic<size_t> search_in_flight{0};
};

auto sanitizeGemini(std::string preview) -> std::string {
    utils::replaceAll(preview, "\n", " ");
    utils::replaceAll(preview, "\t", " ");
    utils::replaceAll(preview, "```", " ");
    auto idx = preview.find_first_not_of("`*=>#");
    if(idx == std::string::npos)
        return preview;
    return preview.substr(idx);
}

struct HitsNode
{
    std::vector<HitsNode*> out_neighbous;
    std::vector<HitsNode*> in_neighbous;
    std::string url;
    std::string content_type;
    size_t size = 0;
    float auth_score = 1;
    float new_auth_score = 1;
    float hub_score = 1;
    float new_hub_score = 1;
    float text_rank = 0;
    float score = 0;
    bool is_root = false;
};

enum class TokenType
{
    Text = 0,
    Filter,
    Logical,
};

struct FilterConstrant
{
    std::string value;
    bool negate;
};

struct SizeConstrant
{
    size_t size;
    bool greater;
};

struct SearchFilter
{
    std::vector<FilterConstrant> content_type;
    std::vector<FilterConstrant> domain;
    std::vector<SizeConstrant> size;
};

std::optional<size_t> parseSizeUnits(std::string unit)
{
    std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
    if(unit.size() > 1 && unit.back() == 'b')
        unit.pop_back();

    if(unit == "" || unit == "b" || unit == "byte") 
        return 1;
    else if(unit == "k")
        return 1000;
    else if(unit == "ki")
        return 1024;
    else if(unit == "m")
        return 1000*1000;
    else if(unit == "mi")
        return 1024*1024;
    else if(unit == "g")
        return 1000*1000*1000;
    else if(unit == "gi")
        return 1024*1024*1024;
    else
        return {};
}

std::pair<std::string, SearchFilter> parseSearchQuery(const std::string& query)
{
    std::pair<std::string, std::string> result;
    auto words = utils::splitString(query, " ");
    std::string search_query = "";
    SearchFilter filter;
    std::vector<TokenType> token_type;

    for(const auto& token : words) {
        auto seperator = token.find(":");
        if(seperator != std::string::npos &&
            seperator+1 != token.size() &&
            seperator != 0) {
            auto key = token.substr(0, seperator);
            if(key == "content_type" || key == "domain" || key == "size")
                token_type.push_back(TokenType::Filter);
            else
                token_type.push_back(TokenType::Text);
        }
        else if(token == "NOT" || token == "not")
            token_type.push_back(TokenType::Logical);
        else
            token_type.push_back(TokenType::Text);
    }

    bool negate = false;
    for(size_t i=0;i<words.size();i++) {
        auto type = token_type[i];
        const auto& token = words[i];
        if(type == TokenType::Text)
            search_query += token + " ";
        else if(type == TokenType::Filter) {
            auto idx = token.find(':');
            auto key = token.substr(0, idx);
            auto value = token.substr(idx+1);
            if(key == "content_type")
                filter.content_type.push_back({std::string(value), negate});
            else if(key == "domain")
                filter.domain.push_back({std::string(value), negate});
            else if(key == "size") {
                static const std::regex re(R"(([><])([\.0-9]+)([GBKMibyte]+)?)", std::regex_constants::icase);
                std::smatch match;
                if(std::regex_match(value, match, re) == false) {
                    LOG_DEBUG << "Bad size filter: " << token;
                    negate = false;
                    continue;
                }
                bool greater = match[1] == ">";
                auto unit = parseSizeUnits(match[3].str());
                if(!unit.has_value()) {
                    LOG_DEBUG << "Bad size unit: " << match[3].str();
                    negate = false;
                    continue;
                }
                size_t size = std::stod(match[2])*unit.value();
                filter.size.push_back({size, bool(negate^greater)});
            }
            negate = false;
        }
        else {
            if(i != words.size() - 1 && token_type[i+1] == TokenType::Filter) {
                negate = true;
            }
            else
                search_query += token + " ";
        }
    }

    if(!search_query.empty())
        search_query.resize(search_query.size()-1);
    return {search_query, filter};
}
// Search with scoring using the HITS algorithm
Task<std::vector<RankedResult>> SearchController::hitsSearch(const std::string query_str, bool find_auths)
{
    auto db = app().getDbClient();
    auto nodes_of_intrest = co_await db->execSqlCoro("SELECT url as source_url, cross_site_links, content_type, size, "
        "ts_rank_cd(pages.title_vector, plainto_tsquery($1))*50+ts_rank_cd(pages.search_vector, plainto_tsquery($1)) AS rank "
        "FROM pages WHERE pages.search_vector @@ plainto_tsquery($1) "
        "ORDER BY rank DESC LIMIT 50000;", query_str);
    auto links_to_node = co_await db->execSqlCoro("SELECT links.to_url AS dest_url, links.url AS source_url, content_type, size, "
        "0 AS rank FROM pages JOIN links ON pages.url=links.to_url WHERE links.is_cross_site = TRUE AND pages.search_vector @@ plainto_tsquery($1)"
        , query_str);

    std::unordered_map<std::string, size_t> node_table;
    std::vector<HitsNode> nodes;
    nodes.reserve(nodes_of_intrest.size());
    node_table.reserve(nodes_of_intrest.size());
    // Add all nodes to our graph
    // TODO: Graph construction seems to be the slow part then a common term is being search. "Gemini", "capsule" are good examples.
    // Optimize it
    for(const auto& links : {nodes_of_intrest, links_to_node}) {
        for(const auto& link : links) {
            auto source_url = link["source_url"].as<std::string>();
            if(node_table.count(source_url) == 0) {
                HitsNode node;
                node.url = source_url;
                node.text_rank = link["rank"].as<double>();
                node.size = link["size"].as<int64_t>();
                node.content_type = link["content_type"].as<std::string>();
                node.is_root = node.text_rank != 0; // Since the only reason for rank == 0 is it's in the base but not root
                nodes.emplace_back(std::move(node));
                node_table[source_url] = nodes.size()-1;
            }
        }
    }

    LOG_DEBUG << "DB returned " << nodes.size() << " pages";
    LOG_DEBUG << "Root set: " << nodes_of_intrest.size() << " pages";
    LOG_DEBUG << "Base set: " << nodes.size() - nodes_of_intrest.size() << " pages";
    if(nodes.size() == 0)
        co_return {};

    // populate links between nodes
    auto getIfExists= [&](const std::string& name) -> HitsNode* {
        auto it = node_table.find(name);
        if(it == node_table.end())
            return nullptr;
        return &nodes[it->second];
    };
    for(const auto& page : nodes_of_intrest) {
        auto source_url = page["source_url"].as<std::string>();
        if(page["cross_site_links"].isNull())
            continue;
        auto links_str = page["cross_site_links"].as<std::string>();
        auto links = nlohmann::json::parse(std::move(links_str)).get<std::vector<std::string>>();
        auto source_node = getIfExists(source_url);
        if(source_node == nullptr) // Should not ever happen
            continue;
        source_node->out_neighbous.reserve(links.size());
        for(const auto& link : links) {
            const auto& dest_url = link;
            auto dest_node = getIfExists(dest_url);

            if(dest_node == nullptr || source_url == dest_url)
                continue;
            source_node->out_neighbous.push_back(dest_node);
            dest_node->in_neighbous.push_back(source_node);
        }
    }
    for(const auto& link : links_to_node) {
        const auto& source_url = link["source_url"].as<std::string>();
        const auto& dest_url = link["dest_url"].as<std::string>();
        auto source_node = getIfExists(source_url);
        auto dest_node = getIfExists(dest_url);

        if(source_url == dest_url)
            continue;
        if(dest_node == nullptr || source_node == nullptr)
            continue;
        source_node->out_neighbous.push_back(dest_node);
        dest_node->in_neighbous.push_back(source_node);
    }

    // The HITS algorithm
    float score_delta = std::numeric_limits<float>::max_digits10;
    constexpr float epsilon = 0.005;
    constexpr size_t max_iter = 300;
    size_t hits_iter = 0;
    for(hits_iter=0;hits_iter<max_iter && score_delta > epsilon;hits_iter++) {
        for(auto& node : nodes) {
            node.new_auth_score = node.auth_score;
            node.new_hub_score = node.hub_score;
            float new_auth_score = 0; 
            float new_hub_score = 0;
            for(auto neighbour : node.in_neighbous)
                new_auth_score += neighbour->hub_score;
            for(auto neighbour : node.out_neighbous)
                new_hub_score += neighbour->auth_score;

            if(new_auth_score != 0)
                node.new_auth_score = new_auth_score;
            if(new_hub_score != 0)
                node.new_hub_score = new_hub_score;
        }

        float auth_sum = std::max(std::accumulate(nodes.begin(), nodes.end(), 0.f
            ,[](float v, const auto& node) { return node.new_auth_score + v;}), 1.f);
        float hub_sum = std::max(std::accumulate(nodes.begin(), nodes.end(), 0.f
            , [](float v, const auto& node) { return node.new_hub_score + v;}), 1.f);

        score_delta = 0;
        for(auto& node : nodes) {
            score_delta += std::abs(node.auth_score - node.new_auth_score / auth_sum);
            score_delta += std::abs(node.hub_score - node.new_hub_score / hub_sum);
            node.auth_score = node.new_auth_score / auth_sum;
            node.hub_score = node.new_hub_score / hub_sum;

            // avoid denormals
            if(node.auth_score < std::numeric_limits<float>::epsilon())
                node.auth_score = 0;
            if(node.hub_score < std::numeric_limits<float>::epsilon())
                node.hub_score = 0;
        }
    }
    LOG_DEBUG << "HITS finished in " << hits_iter << " iterations";

    float max_auth_score = std::max_element(nodes.begin(), nodes.end()
        , [](const auto& a, const auto& b) { return a.auth_score < b.auth_score; })->auth_score;
    if(max_auth_score == 0)
        max_auth_score = 1;
    // Combine the text score and the HITS score. Really want to use BM25 as text score
    // XXX: This scoring function works. But it kinda sucks
    for(auto& node : nodes) {
        if(find_auths) {
            float boost = exp((node.auth_score / max_auth_score)*6.5);
            node.score = 2*(boost * node.text_rank) / (boost + node.text_rank);
        }
        else
            node.score = node.hub_score;
    }

    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
        if(a.is_root != b.is_root)
            return a.is_root;
        return a.score > b.score;
    });
    if(find_auths) {
        nodes = std::vector<HitsNode>(nodes.begin(), std::lower_bound(nodes.begin(), nodes.end(), true
            , [](const auto& node, bool){ return node.is_root; }));
    }

    std::vector<RankedResult> search_result;
    search_result.reserve(nodes.size());
    for(const auto& item : nodes) {
        RankedResult result;
        result.url = item.url;
        result.score = item.score;
        result.size = item.size;
        result.content_type = item.content_type;
        search_result.emplace_back(std::move(result));
    }
    co_return search_result;
}

bool evalFilter(const std::string_view host, const std::string_view content_type, size_t size, const SearchFilter& filter)
{
    if(size == 0 && filter.size.size() != 0)
        return false;
    
    auto size_it = std::find_if(filter.size.begin(), filter.size.end(), [size](const auto& size_constrant){
        if(size_constrant.greater)
            return size > size_constrant.size;
        else
            return size < size_constrant.size;
    });
    if(!filter.size.empty() && size_it == filter.size.end())
        return false;
    
    auto domain_it = std::find_if(filter.domain.begin(), filter.domain.end(), [host](const auto& domain_constrant){
        return domain_constrant.negate ^ (host == domain_constrant.value);
    });
    if(!filter.domain.empty() && domain_it == filter.domain.end())
        return false;
    
    auto content_it = std::find_if(filter.content_type.begin(), filter.content_type.end(), [content_type](const auto& content_constrant){
        return content_constrant.negate ^ (content_type != "" && content_type.starts_with(content_constrant.value));
    });
    if(!filter.content_type.empty() && content_it == filter.content_type.end())
        return false;
    
    return true;
}

Task<HttpResponsePtr> SearchController::tlgs_search(HttpRequestPtr req)
{
    using namespace std::chrono;
    auto t1 = high_resolution_clock::now();
    // Prevent too many search requests piling up
    tlgs::Counter counter(search_in_flight);
    if(counter.count() > 120) {
        auto resp = HttpResponse::newHttpResponse();
        resp->addHeader("meta", "SlowDown");
        resp->setStatusCode((HttpStatusCode)44);
        co_return resp;
    }

    auto input = utils::urlDecode(req->getParameter("query"));
    auto [query_str, filter] = parseSearchQuery(input);
    std::transform(query_str.begin(), query_str.end(), query_str.begin(), ::tolower);

    if(query_str.empty())
    {
        auto resp = HttpResponse::newHttpResponse();
        resp->addHeader("meta", "Search for something");
        resp->setStatusCode((HttpStatusCode)10);
        co_return resp;
    }

    using HitsResult = std::vector<RankedResult>;

    static CacheMap<std::string, std::shared_ptr<HitsResult>> result_cache(app().getLoop(), 60);
    const static std::regex re(R"((?:\/v)?\/search\/([0-9]+))");
    std::smatch match;
    size_t current_page_idx = 0;
    if(std::regex_match(req->path(), match, re)) {
        std::string page_string = match[1];
        if(!page_string.empty()) {
            current_page_idx = std::stoull(page_string)-1;
        }
    }

    std::shared_ptr<HitsResult> ranked_result;
    bool cached = true;
    auto db = app().getDbClient();
    if(result_cache.findAndFetch(query_str, ranked_result) == false) {
        std::vector<RankedResult> result;
        result = co_await hitsSearch(query_str, true);
        ranked_result = std::make_shared<HitsResult>(std::move(result));
        result_cache.insert(query_str, ranked_result, 600);
        cached = false;
    }
    if(ranked_result == nullptr)
        throw std::runtime_error("search result is nullptr");
    // TODO: Maybe cache filtered results?
    std::shared_ptr<HitsResult> filtered_result = ranked_result;
    if(filter.content_type.size() != 0
        || filter.size.size() != 0
        || filter.domain.size() != 0) {
        filtered_result = std::make_shared<HitsResult>();
        for(const auto& item : *ranked_result) {
            if(evalFilter(tlgs::Url(item.url).host(), item.content_type, item.size, filter))
                filtered_result->push_back(item);
        }
    }
    size_t total_results = filtered_result->size();

    const size_t item_per_page = 10;
    auto being = filtered_result->begin()+item_per_page*current_page_idx;
    auto end = filtered_result->begin()+std::min(item_per_page*(current_page_idx+1), filtered_result->size());
    // XXX: Drogon's raw SQL querys does not support arrays/sets 
    // Preperbally a bad idea to use string concat for SQL. But we do ignore bad strings
    std::string url_array;
    for(const auto& item : std::ranges::subrange(being, end)) {
        if(item.url.find('\'') == std::string::npos)
            url_array += "'"+item.url+"', ";
    }
    if(url_array.size() != 0)
        url_array.resize(url_array.size()-2);

    std::vector<SearchResult> search_result;
    if(!url_array.empty()) {
        // HACK: Use the first 5K characters for highligh search. This is MUCH faster
        // without loosing too much accuracy
        auto page_data = co_await db->execSqlCoro("SELECT url, size, title, content_type, "
            "ts_headline(SUBSTRING(content_body, 0, 5000), plainto_tsquery($1), 'StartSel=\"\", "
                "StopSel=\"\", MinWords=23, MaxWords=37, MaxFragments=1, FragmentDelimiter=\" ... \"') AS preview, "
            "last_crawled_at FROM pages WHERE url IN ("+url_array+");", query_str);

        std::unordered_map<std::string, size_t> result_idx;
        for(size_t i=0;i<page_data.size();i++) {
            const auto& page = page_data[i];
            result_idx[page["url"].as<std::string>()] = i;
        }

        for(const auto& item : std::ranges::subrange(being, end)) {
            auto it = result_idx.find(item.url);
            if(it == result_idx.end()) {
                LOG_WARN << "Somehow found " << item.url << " in search. But that URL does not exist in DB";
                continue;
            }

            const auto& page = page_data[it->second];
            SearchResult res {
                .url = item.url,
                .title = page["title"].as<std::string>(),
                .content_type = page["content_type"].as<std::string>(),
                .preview = page["preview"].as<std::string>(),
                .last_crawled_at = trantor::Date::fromDbStringLocal(page["last_crawled_at"].as<std::string>())
                    .toCustomedFormattedString("%Y-%m-%d %H:%M:%S", false),
                .size = page["size"].as<uint64_t>(),
                .score = item.score
            };
            if(res.preview.empty())
                res.preview = "No preview provided";
            search_result.emplace_back(std::move(res));
        }
    }

    HttpViewData data;
    std::string encoded_search_term = tlgs::urlEncode(input);
    data["search_result"] = std::move(search_result);
    data["title"] = sanitizeGemini(input) + " - TLGS Search";
    data["verbose"] = req->path().starts_with("/v/search");
    data["encoded_search_term"] = encoded_search_term;
    data["total_results"] = total_results;
    data["current_page_idx"] = current_page_idx;
    data["item_per_page"] = item_per_page;
    data["search_query"] = input; 

    auto resp = HttpResponse::newHttpViewResponse("search_result", data);
    resp->setContentTypeCodeAndCustomString(CT_CUSTOM, "text/gemini");

    auto t2 = high_resolution_clock::now();
    double processing_time = duration_cast<duration<double>>(t2 - t1).count();
    LOG_DEBUG << "Searching took " << (cached?"(cached) ":"") << processing_time << " seconds.";
    co_return resp;
}

Task<HttpResponsePtr> SearchController::jump_search(HttpRequestPtr req, std::string search_term)
{
    auto input = utils::urlDecode(req->getParameter("query"));
    bool conversion_fail = false;
    size_t page = 0;
    try {
        if(!input.empty())
            page = std::stoull(input);
    }
    catch(...) {
        conversion_fail = true;
    }

    if(input.empty() || conversion_fail) {
        auto resp = HttpResponse::newHttpResponse();
        resp->addHeader("meta", "Go to page");
        resp->setStatusCode((HttpStatusCode)10);
        co_return resp;
    }

    bool verbose = req->path().starts_with("/v");
    std::string search_path = verbose ? "/v/search" : "/search";

    auto resp = HttpResponse::newHttpResponse();
    if(page != 1)
        resp->addHeader("meta", search_path+"/"+std::to_string(page)+"?"+search_term);
    else
        resp->addHeader("meta", search_path+"?"+search_term);
    resp->setStatusCode((HttpStatusCode)30);
    co_return resp;
}

Task<HttpResponsePtr> SearchController::backlinks(HttpRequestPtr req)
{
    auto input = utils::urlDecode(req->getParameter("query"));
    bool input_is_good = false;
    tlgs::Url url(input);
    if(!input.empty()) {
        input_is_good = url.good();
        if(!input_is_good) {
            url = tlgs::Url("gemini://"+input);
            input_is_good = url.good();
        }
    }

    if(!input_is_good) {
        auto resp = HttpResponse::newHttpResponse();
        resp->addHeader("meta", "Enter URL to a page");
        resp->setStatusCode((HttpStatusCode)10);
        co_return resp;
    }

    auto db = app().getDbClient();
    auto backlinks = co_await db->execSqlCoro("SELECT url, is_cross_site FROM links WHERE links.to_url = $1 "
        , url.str());
    std::vector<std::string> internal_backlinks; 
    std::vector<std::string> external_backlinks;
    for(const auto& link : backlinks) {
        std::string url = link["url"].as<std::string>();
        if(link["is_cross_site"].as<bool>())
            external_backlinks.push_back(url);
        else
            internal_backlinks.push_back(url);
    }
    HttpViewData data;
    data["title"] = "Backlinks to " + url.str() + " - TLGS Search";
    data["internal_backlinks"] = internal_backlinks;
    data["external_backlinks"] = external_backlinks;
    auto resp = HttpResponse::newHttpViewResponse("backlinks", data);
    resp->setContentTypeCodeAndCustomString(CT_CUSTOM, "text/gemini");
    co_return resp;
}
