#include "converter/unity_yaml_parser.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace u2g {

// ---------------------------------------------------------------------------
// YamlNode statics and helpers
// ---------------------------------------------------------------------------

const YamlNode YamlNode::NULL_NODE = {};

const YamlNode& YamlNode::operator[](const std::string& key) const {
    if (type != Map) return NULL_NODE;
    auto it = map.find(key);
    if (it == map.end()) return NULL_NODE;
    return it->second;
}

std::string YamlNode::str(const std::string& def) const {
    if (type != Scalar) return def;
    return scalar;
}

float YamlNode::toFloat(float def) const {
    if (type != Scalar || scalar.empty()) return def;
    try {
        return std::stof(scalar);
    } catch (...) {
        return def;
    }
}

int64_t YamlNode::toInt(int64_t def) const {
    if (type != Scalar || scalar.empty()) return def;
    try {
        return std::stoll(scalar);
    } catch (...) {
        return def;
    }
}

YamlNode YamlNode::makeScalar(const std::string& s) {
    YamlNode n;
    n.type = Scalar;
    n.scalar = s;
    return n;
}

YamlNode YamlNode::makeMap() {
    YamlNode n;
    n.type = Map;
    return n;
}

YamlNode YamlNode::makeSeq() {
    YamlNode n;
    n.type = Sequence;
    return n;
}

// ---------------------------------------------------------------------------
// Internal parser helpers
// ---------------------------------------------------------------------------

namespace {

// Split content into lines (handles \r\n and \n).
std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string::size_type pos = 0;
    while (pos <= content.size()) {
        auto nl = content.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(content.substr(pos));
            break;
        }
        std::string line = content.substr(pos, nl - pos);
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        pos = nl + 1;
    }
    return lines;
}

// Count leading spaces of a line.
int indentOf(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else break;
    }
    return n;
}

// Trim leading and trailing whitespace.
std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Trim leading whitespace only.
std::string ltrim(const std::string& s) {
    auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    return s.substr(b);
}

// Check if a line is blank or comment or directive.
bool isSkipLine(const std::string& line) {
    std::string t = ltrim(line);
    if (t.empty()) return true;
    if (t[0] == '#') return true;
    if (t.size() >= 5 && t.substr(0, 5) == "%YAML") return true;
    if (t.size() >= 4 && t.substr(0, 4) == "%TAG") return true;
    return false;
}

// Check if line starts a new document.
bool isDocSeparator(const std::string& line) {
    return line.size() >= 3 && line[0] == '-' && line[1] == '-' && line[2] == '-';
}

// Unquote a scalar value if it's quoted.
std::string unquoteScalar(const std::string& s) {
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') ||
            (s.front() == '\'' && s.back() == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

// Forward declarations for mutual recursion.
YamlNode parseFlowMapping(const std::string& str, size_t& pos);
YamlNode parseFlowSequence(const std::string& str, size_t& pos);
YamlNode parseFlowValue(const std::string& str, size_t& pos);

void skipFlowSpaces(const std::string& str, size_t& pos) {
    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t'))
        ++pos;
}

// Parse a flow scalar (up to a delimiter: , } ] or end of string).
YamlNode parseFlowScalar(const std::string& str, size_t& pos) {
    skipFlowSpaces(str, pos);
    if (pos >= str.size())
        return YamlNode::makeScalar("");

    // Quoted string
    if (str[pos] == '"' || str[pos] == '\'') {
        char q = str[pos++];
        std::string val;
        while (pos < str.size() && str[pos] != q) {
            if (str[pos] == '\\' && pos + 1 < str.size()) {
                val += str[pos + 1];
                pos += 2;
            } else {
                val += str[pos++];
            }
        }
        if (pos < str.size()) ++pos; // skip closing quote
        return YamlNode::makeScalar(val);
    }

    // Unquoted: read until , } ] or end
    std::string val;
    while (pos < str.size() && str[pos] != ',' && str[pos] != '}' && str[pos] != ']') {
        val += str[pos++];
    }
    // Trim trailing spaces
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
        val.pop_back();
    return YamlNode::makeScalar(val);
}

// Parse a flow value: could be { ... }, [ ... ], or a scalar.
YamlNode parseFlowValue(const std::string& str, size_t& pos) {
    skipFlowSpaces(str, pos);
    if (pos >= str.size()) return YamlNode::makeScalar("");

    if (str[pos] == '{') return parseFlowMapping(str, pos);
    if (str[pos] == '[') return parseFlowSequence(str, pos);
    return parseFlowScalar(str, pos);
}

// Parse { key: val, key: val, ... }
YamlNode parseFlowMapping(const std::string& str, size_t& pos) {
    YamlNode node = YamlNode::makeMap();
    if (pos >= str.size() || str[pos] != '{') return node;
    ++pos; // skip '{'

    while (pos < str.size()) {
        skipFlowSpaces(str, pos);
        if (pos >= str.size() || str[pos] == '}') {
            if (pos < str.size()) ++pos; // skip '}'
            break;
        }

        // Parse key
        std::string key;
        if (pos < str.size() && (str[pos] == '"' || str[pos] == '\'')) {
            char q = str[pos++];
            while (pos < str.size() && str[pos] != q) {
                key += str[pos++];
            }
            if (pos < str.size()) ++pos; // closing quote
        } else {
            while (pos < str.size() && str[pos] != ':' && str[pos] != '}' && str[pos] != ',') {
                key += str[pos++];
            }
        }
        // Trim key
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
            key.erase(key.begin());

        skipFlowSpaces(str, pos);

        if (pos < str.size() && str[pos] == ':') {
            ++pos; // skip ':'
            skipFlowSpaces(str, pos);
            YamlNode val = parseFlowValue(str, pos);
            if (!key.empty())
                node.map[key] = std::move(val);
        } else {
            // Key with no value — treat as scalar in some edge case
            if (!key.empty())
                node.map[key] = YamlNode::makeScalar("");
        }

        skipFlowSpaces(str, pos);
        if (pos < str.size() && str[pos] == ',')
            ++pos; // skip ','
    }
    return node;
}

// Parse [ val, val, ... ]
YamlNode parseFlowSequence(const std::string& str, size_t& pos) {
    YamlNode node = YamlNode::makeSeq();
    if (pos >= str.size() || str[pos] != '[') return node;
    ++pos; // skip '['

    while (pos < str.size()) {
        skipFlowSpaces(str, pos);
        if (pos >= str.size() || str[pos] == ']') {
            if (pos < str.size()) ++pos; // skip ']'
            break;
        }

        YamlNode val = parseFlowValue(str, pos);
        node.sequence.push_back(std::move(val));

        skipFlowSpaces(str, pos);
        if (pos < str.size() && str[pos] == ',')
            ++pos;
    }
    return node;
}

// Convenience: parse a flow mapping from a full string like "{x: 1, y: 2}".
YamlNode parseFlowMappingStr(const std::string& str) {
    size_t pos = 0;
    std::string trimmed = trim(str);
    return parseFlowMapping(trimmed, pos);
}

// Convenience: parse a flow sequence from a full string like "[a, b, c]".
YamlNode parseFlowSequenceStr(const std::string& str) {
    size_t pos = 0;
    std::string trimmed = trim(str);
    return parseFlowSequence(trimmed, pos);
}

// ---------------------------------------------------------------------------
// Block (indentation-based) parser
// ---------------------------------------------------------------------------

// Forward declaration
YamlNode parseBlock(const std::vector<std::string>& lines, size_t& idx, int baseIndent);

// Find the colon separator in a line that separates key from value.
// Returns the position of ':', or std::string::npos.
// The colon must be followed by ' ', end of line, or be at end — but not inside
// braces or quotes.
size_t findKeyColon(const std::string& line, size_t startPos) {
    bool inSingle = false;
    bool inDouble = false;
    int braceDepth = 0;
    int bracketDepth = 0;

    for (size_t i = startPos; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"' && !inSingle) inDouble = !inDouble;
        else if (c == '\'' && !inDouble) inSingle = !inSingle;
        if (inSingle || inDouble) continue;

        if (c == '{') ++braceDepth;
        else if (c == '}') --braceDepth;
        else if (c == '[') ++bracketDepth;
        else if (c == ']') --bracketDepth;

        if (braceDepth > 0 || bracketDepth > 0) continue;

        if (c == ':') {
            // Must be followed by ' ' or end of content
            if (i + 1 >= line.size() || line[i + 1] == ' ' || line[i + 1] == '\t') {
                return i;
            }
        }
    }
    return std::string::npos;
}

// Check if a trimmed line content starts with "- ".
bool isSequenceItem(const std::string& line, int indent) {
    if ((int)line.size() <= indent) return false;
    if (line[(size_t)indent] == '-') {
        if ((size_t)indent + 1 >= line.size() || line[(size_t)indent + 1] == ' ')
            return true;
    }
    return false;
}

// Parse a mapping (set of key: value pairs) at a given indent level.
YamlNode parseMapping(const std::vector<std::string>& lines, size_t& idx, int baseIndent) {
    YamlNode node = YamlNode::makeMap();

    while (idx < lines.size()) {
        const std::string& line = lines[idx];

        // Skip blank/comment lines
        if (isSkipLine(line)) { ++idx; continue; }

        // Stop at document separator
        if (isDocSeparator(line)) break;

        int lineIndent = indentOf(line);

        // If indent is less than base, this line belongs to parent scope
        if (lineIndent < baseIndent) break;

        // If this is a sequence item at our indent, let the parent handle it
        // (this happens when a mapping is inside a sequence)
        if (lineIndent == baseIndent && isSequenceItem(line, baseIndent)) break;

        // If indent is deeper than base, that's unexpected at this level — skip
        if (lineIndent > baseIndent) {
            // Could be continuation; skip it
            ++idx;
            continue;
        }

        // We expect a key: value pair at baseIndent
        std::string content = line.substr((size_t)baseIndent);

        // Check for sequence item at this level "- key: val" — this shouldn't
        // happen in a mapping context, but if it does, break
        if (!content.empty() && content[0] == '-') break;

        // Find key: value separator
        size_t colonPos = findKeyColon(line, (size_t)baseIndent);
        if (colonPos == std::string::npos) {
            // Not a key-value pair — skip
            ++idx;
            continue;
        }

        std::string key = trim(line.substr((size_t)baseIndent, colonPos - (size_t)baseIndent));
        std::string rest = (colonPos + 1 < line.size()) ? line.substr(colonPos + 1) : "";
        std::string value = trim(rest);

        if (key.empty()) {
            ++idx;
            continue;
        }

        ++idx; // consume this line

        if (!value.empty()) {
            // Inline value on the same line
            if (value[0] == '{') {
                node.map[key] = parseFlowMappingStr(value);
            } else if (value[0] == '[') {
                node.map[key] = parseFlowSequenceStr(value);
            } else {
                node.map[key] = YamlNode::makeScalar(unquoteScalar(value));
            }
        } else {
            // Value is on subsequent lines — look at what follows
            // Skip blank/comment lines to find the next content line
            size_t peekIdx = idx;
            while (peekIdx < lines.size() && isSkipLine(lines[peekIdx]) && !isDocSeparator(lines[peekIdx]))
                ++peekIdx;

            if (peekIdx >= lines.size() || isDocSeparator(lines[peekIdx])) {
                // No more content — empty scalar
                node.map[key] = YamlNode::makeScalar("");
                continue;
            }

            int nextIndent = indentOf(lines[peekIdx]);

            if (nextIndent <= baseIndent) {
                // Next line is at same or lesser indent — empty scalar value
                node.map[key] = YamlNode::makeScalar("");
            } else {
                // Deeper indent — it's a nested structure
                // Skip blank lines to get to the actual content
                idx = peekIdx;

                if (isSequenceItem(lines[idx], nextIndent)) {
                    // It's a sequence
                    node.map[key] = parseBlock(lines, idx, nextIndent);
                } else {
                    // It's a nested mapping
                    node.map[key] = parseMapping(lines, idx, nextIndent);
                }
            }
        }
    }
    return node;
}

// Parse a sequence at a given indent level.
YamlNode parseSequence(const std::vector<std::string>& lines, size_t& idx, int baseIndent) {
    YamlNode node = YamlNode::makeSeq();

    while (idx < lines.size()) {
        const std::string& line = lines[idx];

        // Skip blank/comment lines
        if (isSkipLine(line)) { ++idx; continue; }

        // Stop at document separator
        if (isDocSeparator(line)) break;

        int lineIndent = indentOf(line);

        // If indent is less than base, done with this sequence
        if (lineIndent < baseIndent) break;

        // Must be a sequence item at baseIndent
        if (!isSequenceItem(line, baseIndent)) break;

        // It's a "- " item
        ++idx; // consume this line

        // Content after "- "
        std::string afterDash;
        size_t dashContentStart = (size_t)baseIndent + 1;
        if (dashContentStart < line.size() && line[dashContentStart] == ' ')
            ++dashContentStart;
        if (dashContentStart < line.size())
            afterDash = line.substr(dashContentStart);
        else
            afterDash = "";

        std::string trimmedAfter = trim(afterDash);

        if (trimmedAfter.empty()) {
            // The item's content is on subsequent lines (deeper indent)
            size_t peekIdx = idx;
            while (peekIdx < lines.size() && isSkipLine(lines[peekIdx]) && !isDocSeparator(lines[peekIdx]))
                ++peekIdx;

            if (peekIdx >= lines.size() || isDocSeparator(lines[peekIdx])) {
                node.sequence.push_back(YamlNode::makeScalar(""));
                continue;
            }

            int nextIndent = indentOf(lines[peekIdx]);
            if (nextIndent > baseIndent) {
                idx = peekIdx;
                node.sequence.push_back(parseBlock(lines, idx, nextIndent));
            } else {
                node.sequence.push_back(YamlNode::makeScalar(""));
            }
        } else if (trimmedAfter[0] == '{') {
            // Flow mapping: - {fileID: 123}
            node.sequence.push_back(parseFlowMappingStr(trimmedAfter));
        } else if (trimmedAfter[0] == '[') {
            // Flow sequence: - [a, b, c]
            node.sequence.push_back(parseFlowSequenceStr(trimmedAfter));
        } else {
            // Could be "- key: value" (a map item) or "- scalar"
            // The content after "- " is at a virtual indent of baseIndent + 2
            int itemContentIndent = baseIndent + 2;

            // Check if afterDash contains a key: value pair
            size_t colonPos = findKeyColon(afterDash, 0);
            if (colonPos != std::string::npos) {
                // It's a mapping item like "- component: {fileID: 123}"
                // or "- _MainTex:" followed by deeper content
                std::string itemKey = trim(afterDash.substr(0, colonPos));
                std::string itemRest = (colonPos + 1 < afterDash.size()) ? afterDash.substr(colonPos + 1) : "";
                std::string itemValue = trim(itemRest);

                YamlNode mapItem = YamlNode::makeMap();

                if (!itemValue.empty()) {
                    if (itemValue[0] == '{') {
                        mapItem.map[itemKey] = parseFlowMappingStr(itemValue);
                    } else if (itemValue[0] == '[') {
                        mapItem.map[itemKey] = parseFlowSequenceStr(itemValue);
                    } else {
                        mapItem.map[itemKey] = YamlNode::makeScalar(unquoteScalar(itemValue));
                    }
                } else {
                    // Value on subsequent lines
                    size_t peekIdx = idx;
                    while (peekIdx < lines.size() && isSkipLine(lines[peekIdx]) && !isDocSeparator(lines[peekIdx]))
                        ++peekIdx;

                    if (peekIdx < lines.size() && !isDocSeparator(lines[peekIdx])) {
                        int nextIndent = indentOf(lines[peekIdx]);
                        if (nextIndent > baseIndent) {
                            idx = peekIdx;
                            mapItem.map[itemKey] = parseBlock(lines, idx, nextIndent);
                        } else {
                            mapItem.map[itemKey] = YamlNode::makeScalar("");
                        }
                    } else {
                        mapItem.map[itemKey] = YamlNode::makeScalar("");
                    }
                }

                // Check if there are more key-value pairs at itemContentIndent
                // following this first key (i.e., the sequence item is a
                // multi-key map).
                while (idx < lines.size()) {
                    if (isSkipLine(lines[idx]) && !isDocSeparator(lines[idx])) {
                        ++idx;
                        continue;
                    }
                    if (isDocSeparator(lines[idx])) break;
                    int ni = indentOf(lines[idx]);
                    if (ni < itemContentIndent) break;
                    if (ni == itemContentIndent && !isSequenceItem(lines[idx], ni)) {
                        // Another key at the same level as the first key in this
                        // sequence item — it's part of the same map.
                        // Parse remaining keys via parseMapping.
                        YamlNode rest = parseMapping(lines, idx, itemContentIndent);
                        for (auto& kv : rest.map)
                            mapItem.map[kv.first] = std::move(kv.second);
                        break;
                    }
                    break;
                }

                node.sequence.push_back(std::move(mapItem));
            } else {
                // Plain scalar item: - someValue
                node.sequence.push_back(YamlNode::makeScalar(unquoteScalar(trimmedAfter)));
            }
        }
    }
    return node;
}

// Parse a block starting at a given indent — determine if it's a sequence or
// mapping and delegate.
YamlNode parseBlock(const std::vector<std::string>& lines, size_t& idx, int baseIndent) {
    // Peek to see what kind of block this is
    size_t peekIdx = idx;
    while (peekIdx < lines.size()) {
        if (isSkipLine(lines[peekIdx]) && !isDocSeparator(lines[peekIdx])) {
            ++peekIdx;
            continue;
        }
        break;
    }

    if (peekIdx >= lines.size() || isDocSeparator(lines[peekIdx]))
        return YamlNode::makeScalar("");

    if (isSequenceItem(lines[peekIdx], baseIndent))
        return parseSequence(lines, idx, baseIndent);
    else
        return parseMapping(lines, idx, baseIndent);
}

// Parse the document header "--- !u!<classID> &<fileID>".
// Returns true on success.
bool parseDocHeader(const std::string& line, int& classID, int64_t& fileID) {
    classID = 0;
    fileID = 0;

    // Format: --- !u!<classID> &<fileID>
    // There may also be "--- !u!<classID> &<fileID> stripped"
    auto bangPos = line.find("!u!");
    if (bangPos == std::string::npos) return false;

    size_t idStart = bangPos + 3;
    size_t idEnd = idStart;
    while (idEnd < line.size() && line[idEnd] >= '0' && line[idEnd] <= '9')
        ++idEnd;

    if (idEnd == idStart) return false;
    classID = std::atoi(line.substr(idStart, idEnd - idStart).c_str());

    auto ampPos = line.find('&', idEnd);
    if (ampPos == std::string::npos) return false;

    size_t fidStart = ampPos + 1;
    size_t fidEnd = fidStart;
    // fileID can be negative (rare but possible)
    if (fidEnd < line.size() && line[fidEnd] == '-') ++fidEnd;
    while (fidEnd < line.size() && line[fidEnd] >= '0' && line[fidEnd] <= '9')
        ++fidEnd;

    if (fidEnd == fidStart) return false;
    fileID = std::stoll(line.substr(fidStart, fidEnd - fidStart));

    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

YamlFile parseUnityYaml(const std::string& content) {
    YamlFile file;
    if (content.empty()) return file;

    std::vector<std::string> lines = splitLines(content);
    size_t idx = 0;

    // Skip leading directives and blanks until first document separator
    while (idx < lines.size() && !isDocSeparator(lines[idx]))
        ++idx;

    while (idx < lines.size()) {
        // We should be at a "---" line
        if (!isDocSeparator(lines[idx])) {
            ++idx;
            continue;
        }

        const std::string& headerLine = lines[idx];
        ++idx;

        int classID = 0;
        int64_t fileID = 0;
        if (!parseDocHeader(headerLine, classID, fileID)) {
            // Malformed document header — skip to next "---"
            while (idx < lines.size() && !isDocSeparator(lines[idx]))
                ++idx;
            continue;
        }

        // Skip blank/comment lines after header
        while (idx < lines.size() && isSkipLine(lines[idx]) && !isDocSeparator(lines[idx]))
            ++idx;

        if (idx >= lines.size() || isDocSeparator(lines[idx])) {
            // Empty document body
            YamlDocument doc;
            doc.classID = classID;
            doc.fileID = fileID;
            doc.root = YamlNode::makeMap();
            file.documents.push_back(std::move(doc));
            continue;
        }

        // The first line should be the typeName at indent 0: "TypeName:"
        const std::string& typeNameLine = lines[idx];
        int tnIndent = indentOf(typeNameLine);

        // Extract typeName (the key before the colon)
        size_t colonPos = findKeyColon(typeNameLine, (size_t)tnIndent);
        if (colonPos == std::string::npos) {
            // Unexpected format — skip document
            while (idx < lines.size() && !isDocSeparator(lines[idx]))
                ++idx;
            continue;
        }

        std::string typeName = trim(typeNameLine.substr((size_t)tnIndent, colonPos - (size_t)tnIndent));
        std::string typeValueStr = trim(
            (colonPos + 1 < typeNameLine.size()) ? typeNameLine.substr(colonPos + 1) : "");

        ++idx; // consume typeName line

        YamlDocument doc;
        doc.classID = classID;
        doc.fileID = fileID;
        doc.typeName = typeName;

        if (!typeValueStr.empty()) {
            // Rare: inline value after typeName (e.g., "TypeName: {key: val}")
            if (typeValueStr[0] == '{') {
                doc.root = parseFlowMappingStr(typeValueStr);
            } else if (typeValueStr[0] == '[') {
                doc.root = parseFlowSequenceStr(typeValueStr);
            } else {
                doc.root = YamlNode::makeScalar(unquoteScalar(typeValueStr));
            }
        } else {
            // Parse the body of this document.
            // Skip blanks to find the indent of the content under typeName.
            size_t peekIdx = idx;
            while (peekIdx < lines.size() && isSkipLine(lines[peekIdx]) && !isDocSeparator(lines[peekIdx]))
                ++peekIdx;

            if (peekIdx < lines.size() && !isDocSeparator(lines[peekIdx])) {
                int bodyIndent = indentOf(lines[peekIdx]);
                idx = peekIdx;
                doc.root = parseBlock(lines, idx, bodyIndent);
            } else {
                doc.root = YamlNode::makeMap();
                idx = peekIdx;
            }
        }

        file.documents.push_back(std::move(doc));
    }

    return file;
}

} // namespace u2g
