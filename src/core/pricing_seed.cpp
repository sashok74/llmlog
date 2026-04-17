#include "llmlog/core/pricing_seed.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace llmlog::core {

namespace {

using json = nlohmann::json;

std::string requireString(const json& obj, std::string_view field, std::string_view ctx) {
    if (!obj.contains(field)) {
        throw std::runtime_error(std::string{"seed: missing field '"}
            + std::string{field} + "' at " + std::string{ctx});
    }
    const auto& v = obj.at(field);
    if (!v.is_string()) {
        throw std::runtime_error(std::string{"seed: field '"}
            + std::string{field} + "' at " + std::string{ctx} + " must be a string");
    }
    return v.get<std::string>();
}

std::string optionalString(const json& obj, std::string_view field) {
    auto it = obj.find(field);
    if (it == obj.end() || it->is_null()) return {};
    if (!it->is_string()) return {};  // silently ignore wrong-type optionals
    return it->get<std::string>();
}

ProviderMeta parseProvider(const json& obj, std::size_t index) {
    const auto ctx = std::string{"/providers["} + std::to_string(index) + "]";
    ProviderMeta p;
    p.name     = requireString(obj, "name",     ctx);
    p.base_url = requireString(obj, "base_url", ctx);
    p.kind     = requireString(obj, "kind",     ctx);
    return p;
}

Price parsePrice(const json& obj, std::size_t index) {
    const auto ctx = std::string{"/prices["} + std::to_string(index) + "]";
    Price p;
    p.provider_name   = requireString(obj, "provider",         ctx);
    p.model_id        = requireString(obj, "model",            ctx);
    p.input_per_mtok  = requireString(obj, "input_per_mtok",   ctx);
    p.output_per_mtok = requireString(obj, "output_per_mtok",  ctx);
    p.effective_from  = requireString(obj, "effective_from",   ctx);
    p.family               = optionalString(obj, "family");
    p.cache_read_per_mtok  = optionalString(obj, "cache_read_per_mtok");
    p.cache_write_per_mtok = optionalString(obj, "cache_write_per_mtok");
    p.image_per_mtok       = optionalString(obj, "image_per_mtok");
    p.note                 = optionalString(obj, "note");
    return p;
}

} // namespace

PricingSeed parsePricingSeed(const std::string& jsonText) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string{"seed: malformed JSON: "} + e.what());
    }
    if (!root.is_object()) {
        throw std::runtime_error("seed: top-level value must be an object");
    }

    PricingSeed seed;

    if (auto it = root.find("providers"); it != root.end()) {
        if (!it->is_array()) {
            throw std::runtime_error("seed: 'providers' must be an array");
        }
        seed.providers.reserve(it->size());
        for (std::size_t i = 0; i < it->size(); ++i) {
            seed.providers.push_back(parseProvider(it->at(i), i));
        }
    }

    if (auto it = root.find("prices"); it != root.end()) {
        if (!it->is_array()) {
            throw std::runtime_error("seed: 'prices' must be an array");
        }
        seed.prices.reserve(it->size());
        for (std::size_t i = 0; i < it->size(); ++i) {
            seed.prices.push_back(parsePrice(it->at(i), i));
        }
    }

    return seed;
}

PricingSeed loadPricingSeedFile(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        throw std::runtime_error("seed file not found: " + file.string());
    }
    std::ifstream f(file);
    if (!f) throw std::runtime_error("cannot open seed file: " + file.string());
    std::ostringstream buf;
    buf << f.rdbuf();
    return parsePricingSeed(buf.str());
}

std::size_t applyPricingSeed(PricingDao& dao, const PricingSeed& seed) {
    for (const auto& p : seed.providers) {
        dao.upsertProvider(p);
    }
    std::size_t rows = 0;
    for (const auto& price : seed.prices) {
        dao.upsertPrice(price);
        ++rows;
    }
    return rows;
}

} // namespace llmlog::core
