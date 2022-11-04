#pragma once

#include <string_view>
#include <vector>
#include <string>

#include <dremini/GeminiParser.hpp>

namespace tlgs
{

struct GeminiDocument
{
    std::string text;
    std::string title;
    std::vector<std::string> links;
};

/**
 * @brief Parse and convert a Gemini document into title
 *  , plaintext(without styling and heading) and links
 * 
 * @param sv the Gemini document
 */
GeminiDocument extractGemini(const std::string_view sv);

/**
 * @brief Parse and convert a Gemini document into title
 * , plaintext(without styling and heading and ASCII art) and links
 * 
 * @param sv the Gemini document
 */
GeminiDocument extractGeminiConcise(const std::string_view sv);

/**
 * @brief Check if a Gemini page can be interperd as Gemsub using heuristics
 * 
 * @param doc The parsed Gemini document
 */
bool isGemsub(const std::vector<dremini::GeminiASTNode>& doc);
}
