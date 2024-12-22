#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

std::string trim(const std::string& str)
{
	if (str.empty()) return str;

	// Find the first non-space character
	auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char c) { return std::isspace(c); });

	// Find the last non-space character
	auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char c) { return std::isspace(c); }).base();

	// If the entire string is spaces, return an empty string
	return (start < end) ? std::string(start, end) : std::string();
}

std::vector<std::string> splitLines(const std::string& input)
{
	std::vector<std::string> lines;
	std::string currentLine;
	currentLine.reserve(input.size()); // Optional optimization

	for (std::size_t i = 0; i < input.size(); ++i)
	{
		char c = input[i];
		if (c == '\r')
		{
			// If next char is '\n', skip it as part of "\r\n"
			if (i + 1 < input.size() && input[i+1] == '\n')
			{
				++i;
			}
			// We reached a line boundary, push current line
			lines.push_back(currentLine);
			currentLine.clear();
		}
		else if (c == '\n')
		{
			// Unix-style line break
			lines.push_back(currentLine);
			currentLine.clear();
		}
		else
		{
			// Normal character
			currentLine.push_back(c);
		}
	}

	// If there's any leftover data in currentLine, push it as the last line
	lines.push_back(currentLine);

	return lines;
}

std::vector<std::string> splitBySpace(const std::string& input)
{
	std::vector<std::string> tokens;
	std::string currentToken;
	currentToken.reserve(input.size()); // Optional optimization

	for (char c : input)
	{
		if (c == ' ')
		{
			// Reached a space -> end of a token
			tokens.push_back(currentToken);
			currentToken.clear();
		}
		else
		{
			// Accumulate character into current token
			currentToken.push_back(c);
		}
	}

	// Push the last token (even if it’s empty)
	tokens.push_back(currentToken);

	return tokens;
}
