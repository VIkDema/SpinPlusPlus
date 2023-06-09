#include "scope.hpp"

#include "../spin.hpp"
#include <fmt/core.h>


namespace lexer {
int ScopeProcessor::scope_level_ = 0;
std::string ScopeProcessor::curr_scope_name_;
std::array<int, 256> ScopeProcessor::scope_seq_;


void ScopeProcessor::SetCurrScope() {
  curr_scope_name_ = "_";

  if (models::Symbol::GetContext() == nullptr) {
    return;
  }
  for (int i = 0; i < scope_level_; i++) {
    curr_scope_name_ += fmt::format("{}_", scope_seq_[i]);
  }
}
void ScopeProcessor::AddScope() { scope_seq_[scope_level_++]++; }
void ScopeProcessor::RemoveScope() { scope_level_--; }
void ScopeProcessor::InitScopeName() { curr_scope_name_ = "_"; }
std::string ScopeProcessor::GetCurrScope() { return curr_scope_name_; }
int ScopeProcessor::GetCurrSegment() { return scope_seq_[scope_level_]; }
int ScopeProcessor::GetCurrScopeLevel() { return scope_level_; }
void ScopeProcessor::SetCurrScopeLevel(int scope_level) {
  scope_level_ = scope_level;
}

} // namespace lexer