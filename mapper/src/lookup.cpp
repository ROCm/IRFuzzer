#include "lookup.h"

#include <fstream>

using namespace llvm;

bool Matcher::operator<(const Matcher &M) const {
  if (Idx < M.Idx)
    return true;
  return Idx == M.Idx && Size > M.Size;
}

// NOTE: exits program if error encountered
Expected<json::Value> readJSON(const std::string &Filename) {
  std::ifstream LookupIfs(Filename);
  if (!LookupIfs) {
    errs() << "Failed to open lookup file!\n";
    exit(1);
  }
  std::string LookupTableStr;
  std::getline(LookupIfs, LookupTableStr);
  if (LookupTableStr.empty()) {
    errs() << "Empty lookup table!\n";
    exit(1);
  }
  Expected<json::Value> ParseResult = json::parse(LookupTableStr);
  if (!ParseResult) {
    errs() << "Error parsing lookup table.\n";
    exit(1);
  }
  return ParseResult;
}

std::vector<std::string> getStringArray(const json::Object &LookupTable,
                                        const std::string &Key) {
  std::vector<std::string> V;
  for (const auto &Predicate : *LookupTable.getArray(Key)) {
    V.push_back(Predicate.getAsString().value().str());
  }
  return V;
}

std::vector<Pattern> getPatterns(const json::Object &TableJSON) {
  std::vector<Pattern> Patterns;
  for (const json::Value &PatternObject : *TableJSON.getArray("patterns")) {
    Pattern ThePattern;
    ThePattern.IncludePath =
        (*PatternObject.getAsObject()).getString("path").value();
    ThePattern.PatternSrc =
        (*PatternObject.getAsObject()).getString("pattern").value();
    for (const json::Value &PredIdx :
         *(*PatternObject.getAsObject()).getArray("predicates")) {
      ThePattern.NamedPredicates.push_back(PredIdx.getAsInteger().value());
    }
    Patterns.push_back(ThePattern);
  }
  return Patterns;
}

std::vector<Matcher> getMatchers(const json::Object &TableJSON) {
  std::vector<Matcher> Matchers;
  for (const json::Value &MatcherObject : *TableJSON.getArray("matchers")) {
    Matcher TheMatcher;
    TheMatcher.Idx = MatcherObject.getAsObject()->getInteger("index").value();
    TheMatcher.Size = MatcherObject.getAsObject()->getInteger("size").value();
    int KindInt = MatcherObject.getAsObject()->getInteger("kind").value();
    TheMatcher.Kind = static_cast<Matcher::KindTy>(KindInt);

    if (TheMatcher.hasPattern()) {
      TheMatcher.PatternIdx =
          MatcherObject.getAsObject()->getInteger("pattern").value();
    } else if (TheMatcher.Kind == Matcher::CheckPatternPredicate) {
      TheMatcher.PatPredIdx =
          MatcherObject.getAsObject()->getInteger("predicate").value();
    }

    Matchers.push_back(TheMatcher);
  }
  return Matchers;
}

LookupTable LookupTable::fromFile(const std::string &Filename,
                                  bool NameCaseSensitive) {
  Expected<json::Value> ExpectedTable = readJSON(Filename);
  LookupTable Table;
  json::Object &TableJSON = *ExpectedTable.get().getAsObject();

  Table.Matchers = getMatchers(TableJSON);
  Table.Patterns = getPatterns(TableJSON);
  std::sort(Table.Matchers.begin(), Table.Matchers.end());

  Table.PK.addNamedPredicates(getStringArray(TableJSON, "predicates"));
  Table.PK.addPatternPredicates(getStringArray(TableJSON, "pat_predicates"));
  Table.MatcherTableSize = TableJSON.getInteger("table_size").value();

  return Table;
}