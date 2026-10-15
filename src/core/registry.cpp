#include "registry.h"
#include "../providers/claude/claude_provider.h"
#include "../providers/claude_credits/claude_credits_provider.h"
#include "../providers/openai/openai_provider.h"

namespace {
const std::vector<Registry::TypeInfo> kTypes = {
    { "claude",         "Claude"         },
    { "claude_credits", "Claude Credits" },
    { "openai",         "Codex"          },
};
}  // namespace

const std::vector<Registry::TypeInfo>& Registry::types() {
    return kTypes;
}

bool Registry::isKnownType(const String& typeId) {
    for (const auto& t : kTypes) {
        if (typeId == t.id) return true;
    }
    return false;
}

std::unique_ptr<Provider> Registry::create(const String& typeId) {
    if (typeId == "claude")         return std::unique_ptr<Provider>(new ClaudeProvider());
    if (typeId == "claude_credits") return std::unique_ptr<Provider>(new ClaudeCreditsProvider());
    if (typeId == "openai")         return std::unique_ptr<Provider>(new OpenAiProvider());
    return nullptr;
}

std::unique_ptr<Provider> Registry::fromRecord(const AccountRecord& rec) {
    auto p = create(rec.type);
    if (!p) return nullptr;
    p->loadSettings(rec.settings);
    p->setLabel(rec.label);
    return p;
}
