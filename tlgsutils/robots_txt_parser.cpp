#include "robots_txt_parser.hpp"
#include <regex>
#include <set>
#include <sstream>
#include <iostream>

std::vector<std::string> tlgs::parseRobotsTxt(const std::string& str, const std::set<std::string>& agents)
{
    std::set<std::string> disallowed_path; 
    std::stringstream ss;
    ss << str;
    static const std::regex line_re(R"((.*):[ \t](.*))");
    std::smatch match;
    std::string line;
    bool care = false;
    bool last_line_user_agent = false;
    while(std::getline(ss, line)) {
        if(!std::regex_match(line, match, line_re))
            continue;
        
        std::string key = match[1];
        if(key == "User-agent") {
            std::string agent = match[2];
            if(last_line_user_agent) {
                care |= agents.count(agent) > 0;
            }
            else {
                care = agents.count(agent) > 0;
            }
            last_line_user_agent = true;
        }
        else {
            last_line_user_agent = false;
        }
        
        if(key == "Disallow" && care == true) {
            std::string path = match[2];
            if(path.empty())
                disallowed_path.clear();
            else
                disallowed_path.insert(path);
        }
    }
    return std::vector<std::string>(disallowed_path.begin(), disallowed_path.end());
}

bool tlgs::isPathBlocked(const std::string& path, const std::vector<std::string>& disallowed_paths)
{
    for(const auto& disallowed : disallowed_paths) {
        if(path == disallowed || path == disallowed+"/"
            || (path.size() > disallowed.size()+1 && path.find(disallowed) == 0 && (path[disallowed.size()] == '/' || disallowed.back() == '/'))) {
            return true;
        }
    }
    return false;
}