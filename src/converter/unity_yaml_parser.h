#pragma once
#include <string>
#include <map>
#include <vector>
#include <cstdint>

namespace u2g {

struct YamlNode {
    enum Type { Null, Scalar, Map, Sequence };
    Type type = Null;
    std::string scalar;
    std::map<std::string, YamlNode> map;
    std::vector<YamlNode> sequence;

    const YamlNode& operator[](const std::string& key) const;
    std::string str(const std::string& def = "") const;
    float toFloat(float def = 0.0f) const;
    int64_t toInt(int64_t def = 0) const;
    bool isNull() const   { return type == Null; }
    bool isScalar() const { return type == Scalar; }
    bool isMap() const    { return type == Map; }
    bool isSeq() const    { return type == Sequence; }

    static YamlNode makeScalar(const std::string& s);
    static YamlNode makeMap();
    static YamlNode makeSeq();
    static const YamlNode NULL_NODE;
};

struct YamlDocument {
    int classID = 0;
    int64_t fileID = 0;
    std::string typeName; // e.g. "GameObject", "Transform"
    YamlNode root;        // the mapping under typeName
};

struct YamlFile {
    std::vector<YamlDocument> documents;
};

YamlFile parseUnityYaml(const std::string& content);

} // namespace u2g
