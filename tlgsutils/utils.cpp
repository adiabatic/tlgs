#include "utils.hpp"

static std::string charToHex(char c)
{
    std::string result;
    char first, second;

    first = (c & 0xF0) / 16;
    first += first > 9 ? 'A' - 10 : '0';
    second = c & 0x0F;
    second += second > 9 ? 'A' - 10 : '0';

    result.append(1, first);
    result.append(1, second);

    return result;
}

bool tlgs::isAsciiArt(const std::string& str)
{
    // detection algorithm 2.A from https://www.w3.org/WAI/ER/IG/ert/AsciiArt.htm
    size_t count = 0;
    char last_ch = 0;
    for(auto ch : str) {
        if(ch == last_ch)
            count++;
        else {
            count = 1;
            last_ch = ch;
        }

        if(count >= 4 && ch != ' ' && ch != '\t')
            return true;
    }

    // Characters I'm sure is not used in code
    if(str.find("☆") != std::string::npos
    || str.find("★") != std::string::npos
    || str.find("░") != std::string::npos
    || str.find("█") != std::string::npos
    || str.find("⣿") != std::string::npos
    || str.find("⡇") != std::string::npos
    || str.find("⢀") != std::string::npos
    || str.find("┼") != std::string::npos
    || str.find("╭") != std::string::npos)
        return true;

    // patterns that's definatelly not normal text
    if(str.find("(_-<") != std::string::npos)
        return true;

    return false;
}

std::string tlgs::urlEncode(const std::string_view src)
{
    std::string result;
    result.reserve(src.size() + 8);  // Some sane amount to reduce allocation
    // Unserved symbols. See RFC 3986
    constexpr std::string_view symbols = "-_.~";

    for (char ch : src)
    {
        if (ch == ' ')
            result.append(1, '+');
        else if (isalnum(ch) || symbols.find(ch) != std::string_view::npos)
            result.append(1, ch);
        else
        {
            result.append(1, '%');
            result.append(charToHex(ch));
        }
    }

    return result;
}