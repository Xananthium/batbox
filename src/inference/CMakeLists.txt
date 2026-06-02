# =============================================================================
# Component C4 — Inference Client (batbox_inference)
# OpenAI Chat Completions HTTP + SSE streaming.
# Tool-call delta accumulator, usage tracking, retries.
# Depends on: batbox_core, batbox_config, cpr::cpr
# =============================================================================

add_library(batbox_inference OBJECT)

set_target_properties(batbox_inference PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_inference
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_inference
    PUBLIC
        batbox_core
        batbox_config
        cpr::cpr
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 4.x task lands:
#   ChatRequest.cpp          (CPP 4.1) — wire model JSON serialisation
#   Client.cpp               (CPP 4.5) — non-streaming HTTP POST via cpr
#   SseParser.cpp            (CPP 4.2) — chunked SSE data: / event: parsing
#   ToolCallAccumulator.cpp  (CPP 4.3) — delta-stream tool_call delta reassembly
#   UsageTracker.cpp         (CPP 4.4) — per-request + session usage accumulation
#   ModelPricing.cpp         (CPP 4.5) — token cost table, cost_usd computation
#   Provider.cpp             (S8/S9)   — Provider interface, OpenAiCompatibleProvider,
#                                        ProviderRegistry, map_to_canonical_model,
#                                        should_use_responses_api
# ---------------------------------------------------------------------------
target_sources(batbox_inference
    PRIVATE
        .placeholder.cpp
        ChatRequest.cpp
        Client.cpp
        Provider.cpp
        CanonicalModel.cpp
        ReasoningTagProfile.cpp
        ThinkSplitter.cpp
        ReasoningAccumulator.cpp
        SseParser.cpp
        ToolCallAccumulator.cpp
        UsageTracker.cpp
        ModelPricing.cpp
)
