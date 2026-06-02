// src/inference/ModelPricing.cpp
// =============================================================================
// ModelPricing implementation.
//
// Pricing table loading strategy:
//   1. The built-in table is embedded as a constexpr string literal (content
//      of data/models.json copied at compile time via a CMake configure_file
//      or by embedding in this TU).  It is parsed once at first call via a
//      function-local static (C++11 magic-static, thread-safe).
//   2. If BATBOX_MODEL_PRICING_OVERRIDE is set, the file at that path is read
//      and its entries merged on top, replacing matching model IDs.
//   3. The merged table is stored as a flat map<string, PriceEntry>.
//
// JSON shape expected (models.json and override files):
//   {
//     "models": [
//       {
//         "id": "gpt-4o",
//         "pricing": {
//           "prompt_per_million": 2.50,
//           "completion_per_million": 10.00
//         }
//       }, ...
//     ]
//   }
// =============================================================================

#include <batbox/inference/ModelPricing.hpp>
#include <batbox/core/Json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace batbox::inference {

// Defined in Provider.hpp (src/inference/Provider.cpp).  Forward-declared here
// rather than #include'd so the pricing TU does not pull in the full
// Provider/Client/Config/cpr include chain for a single free function.
std::string map_to_canonical_model(std::string_view raw);

// =============================================================================
// Embedded pricing table — content of data/models.json
// =============================================================================

// This string literal contains the full contents of data/models.json.
// It is initialised once; the linker embeds it in the binary.
static constexpr std::string_view kBuiltinPricingJson = R"JSON(
{
  "models": [
    {
      "id": "gpt-4o",
      "name": "GPT-4o",
      "provider": "openai",
      "pricing": {
        "prompt_per_million": 2.50,
        "completion_per_million": 10.00
      }
    },
    {
      "id": "gpt-4o-mini",
      "name": "GPT-4o mini",
      "provider": "openai",
      "pricing": {
        "prompt_per_million": 0.15,
        "completion_per_million": 0.60
      }
    },
    {
      "id": "o1-preview",
      "name": "o1-preview",
      "provider": "openai",
      "pricing": {
        "prompt_per_million": 15.00,
        "completion_per_million": 60.00
      }
    },
    {
      "id": "o1-mini",
      "name": "o1-mini",
      "provider": "openai",
      "pricing": {
        "prompt_per_million": 3.00,
        "completion_per_million": 12.00
      }
    },
    {
      "id": "o3-mini",
      "name": "o3-mini",
      "provider": "openai",
      "pricing": {
        "prompt_per_million": 1.10,
        "completion_per_million": 4.40
      }
    },
    {
      "id": "gpt-4-turbo",
      "name": "GPT-4 Turbo",
      "provider": "openai",
      "pricing": {
        "prompt_per_million": 10.00,
        "completion_per_million": 30.00
      }
    },
    {
      "id": "gpt-4",
      "name": "GPT-4",
      "provider": "openai",
      "pricing": {
        "prompt_per_million": 30.00,
        "completion_per_million": 60.00
      }
    },
    {
      "id": "gpt-3.5-turbo",
      "name": "GPT-3.5 Turbo",
      "provider": "openai",
      "pricing": {
        "prompt_per_million": 0.50,
        "completion_per_million": 1.50
      }
    },
    {
      "id": "claude-3-5-sonnet-20241022",
      "name": "Claude 3.5 Sonnet",
      "provider": "anthropic",
      "pricing": {
        "prompt_per_million": 3.00,
        "completion_per_million": 15.00
      }
    },
    {
      "id": "claude-3-5-haiku-20241022",
      "name": "Claude 3.5 Haiku",
      "provider": "anthropic",
      "pricing": {
        "prompt_per_million": 0.80,
        "completion_per_million": 4.00
      }
    },
    {
      "id": "claude-3-opus-20240229",
      "name": "Claude 3 Opus",
      "provider": "anthropic",
      "pricing": {
        "prompt_per_million": 15.00,
        "completion_per_million": 75.00
      }
    },
    {
      "id": "claude-3-5-sonnet",
      "name": "Claude 3.5 Sonnet (alias)",
      "provider": "anthropic",
      "pricing": {
        "prompt_per_million": 3.00,
        "completion_per_million": 15.00
      }
    },
    {
      "id": "claude-sonnet-4-5",
      "name": "Claude Sonnet 4.5 (placeholder - zero cost)",
      "provider": "anthropic",
      "pricing": {
        "prompt_per_million": 0.0,
        "completion_per_million": 0.0
      }
    },
    {
      "id": "claude-opus-4",
      "name": "Claude Opus 4 (placeholder - zero cost)",
      "provider": "anthropic",
      "pricing": {
        "prompt_per_million": 0.0,
        "completion_per_million": 0.0
      }
    }
  ]
}
)JSON";

// =============================================================================
// Internal price entry
// =============================================================================

struct PriceEntry {
    double prompt_per_million     = 0.0;
    double completion_per_million = 0.0;
};

using PriceTable = std::unordered_map<std::string, PriceEntry>;

// =============================================================================
// Table loading helpers
// =============================================================================

/// Parse models from a JSON value and insert/replace entries in the table.
static void merge_models_json(const Json& root, PriceTable& table) {
    if (!root.contains("models") || !root["models"].is_array()) {
        return;
    }
    for (const auto& entry : root["models"]) {
        if (!entry.contains("id") || !entry["id"].is_string()) {
            continue;
        }
        const std::string id = entry["id"].get<std::string>();

        if (!entry.contains("pricing") || !entry["pricing"].is_object()) {
            // Model is known but has no pricing — store as zero-cost.
            table[id] = PriceEntry{0.0, 0.0};
            continue;
        }
        const auto& pricing = entry["pricing"];
        PriceEntry pe;
        pe.prompt_per_million     = pricing.value("prompt_per_million",     0.0);
        pe.completion_per_million = pricing.value("completion_per_million", 0.0);
        table[id] = pe;
    }
}

/// Load and merge an override file from the given filesystem path.
static void merge_override_file(const std::string& path, PriceTable& table) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return;  // Override file missing or unreadable — silently skip.
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string text = buf.str();

    try {
        const Json root = Json::parse(text);
        merge_models_json(root, table);
    } catch (const std::exception&) {
        // Malformed override — ignore; built-in table remains intact.
    }
}

// =============================================================================
// Global table singleton
// =============================================================================

/// Returns the initialised pricing table.
/// Uses a function-local static for thread-safe one-time initialisation.
static PriceTable& get_table() {
    static PriceTable s_table = []() -> PriceTable {
        PriceTable table;

        // 1. Parse the built-in table.
        try {
            const Json root = Json::parse(kBuiltinPricingJson);
            merge_models_json(root, table);
        } catch (const std::exception&) {
            // Malformed built-in JSON — table remains empty.
        }

        // 2. Merge user override if provided.
        const char* override_path = std::getenv("BATBOX_MODEL_PRICING_OVERRIDE");
        if (override_path != nullptr && override_path[0] != '\0') {
            merge_override_file(std::string(override_path), table);
        }

        return table;
    }();
    return s_table;
}

// =============================================================================
// ModelPricing public API
// =============================================================================

double ModelPricing::cost(std::string_view model,
                           int prompt_tokens,
                           int completion_tokens) {
    const PriceTable& table = get_table();

    auto it = table.find(std::string(model));
    if (it == table.end()) {
        // Raw id missed — retry under the canonical normalisation so that
        // provider-prefixed / tag-suffixed / mixed-case ids
        // (e.g. "openai/gpt-4o", "gpt-4o:latest", "GPT-4O") resolve to the
        // same priced entry.  Fallback-only: a raw hit above is never touched,
        // so this can never regress an existing successful lookup.
        it = table.find(map_to_canonical_model(model));
        if (it == table.end()) {
            return 0.0;  // Unknown model — zero cost.
        }
    }

    const PriceEntry& pe = it->second;
    const double raw = (static_cast<double>(prompt_tokens)     * pe.prompt_per_million
                      + static_cast<double>(completion_tokens) * pe.completion_per_million)
                     / 1'000'000.0;

    // Round to 4 decimal places.
    return std::round(raw * 10'000.0) / 10'000.0;
}

void ModelPricing::reset_for_testing() {
    // Re-initialise by rebuilding the table in place.
    PriceTable& table = get_table();
    table.clear();

    try {
        const Json root = Json::parse(kBuiltinPricingJson);
        merge_models_json(root, table);
    } catch (const std::exception&) {
        // Ignore parse errors in test cleanup.
    }

    const char* override_path = std::getenv("BATBOX_MODEL_PRICING_OVERRIDE");
    if (override_path != nullptr && override_path[0] != '\0') {
        merge_override_file(std::string(override_path), table);
    }
}

} // namespace batbox::inference
