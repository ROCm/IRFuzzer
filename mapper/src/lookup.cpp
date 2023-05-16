#include "lookup.h"

#include "simdjson.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>

using namespace llvm;
using namespace simdjson;

bool Matcher::operator<(const Matcher &M) const {
  return Begin == M.Begin ? End > M.End : Begin < M.Begin;
}

bool Matcher::contains(size_t i) const { return Begin <= i && i <= End; }

bool Matcher::contains(const Matcher &N) const {
  return Begin <= N.Begin && N.End <= End;
}

bool Matcher::operator==(const Matcher &N) const {
  return Begin == N.Begin && End == N.End;
}

bool Matcher::operator!=(const Matcher &N) const { return !(*this == N); }

bool Matcher::hasPattern() const {
  return Kind == CompleteMatch || Kind == MorphNodeTo;
}

bool Matcher::isLeaf() const {
  switch (Kind) {
  default:
    return true;
  case Scope:
  case SwitchOpcode:
  case SwitchType:
  case Group:
    return false;
  }
}

bool Matcher::hasPatPred() const { return Kind == CheckPatternPredicate; }

size_t Matcher::size() const { return End - Begin + 1; }

#define ENUM_TO_STR(name)                                                      \
  case name:                                                                   \
    return #name;

std::string Matcher::getKindAsString(KindTy Kind) {
  switch (Kind) {
    ENUM_TO_STR(Scope)
    ENUM_TO_STR(RecordNode)
    ENUM_TO_STR(RecordChild)
    ENUM_TO_STR(RecordMemRef)
    ENUM_TO_STR(CaptureGlueInput)
    ENUM_TO_STR(MoveChild)
    ENUM_TO_STR(MoveParent)
    ENUM_TO_STR(CheckSame)
    ENUM_TO_STR(CheckChildSame)
    ENUM_TO_STR(CheckPatternPredicate)
    ENUM_TO_STR(CheckPredicate)
    ENUM_TO_STR(CheckOpcode)
    ENUM_TO_STR(SwitchOpcode)
    ENUM_TO_STR(CheckType)
    ENUM_TO_STR(SwitchType)
    ENUM_TO_STR(CheckChildType)
    ENUM_TO_STR(CheckInteger)
    ENUM_TO_STR(CheckChildInteger)
    ENUM_TO_STR(CheckCondCode)
    ENUM_TO_STR(CheckChild2CondCode)
    ENUM_TO_STR(CheckValueType)
    ENUM_TO_STR(CheckComplexPat)
    ENUM_TO_STR(CheckAndImm)
    ENUM_TO_STR(CheckOrImm)
    ENUM_TO_STR(CheckImmAllOnesV)
    ENUM_TO_STR(CheckImmAllZerosV)
    ENUM_TO_STR(CheckFoldableChainNode)
    ENUM_TO_STR(EmitInteger)
    ENUM_TO_STR(EmitStringInteger)
    ENUM_TO_STR(EmitRegister)
    ENUM_TO_STR(EmitConvertToTarget)
    ENUM_TO_STR(EmitMergeInputChains)
    ENUM_TO_STR(EmitCopyToReg)
    ENUM_TO_STR(EmitNode)
    ENUM_TO_STR(EmitNodeXForm)
    ENUM_TO_STR(CompleteMatch)
    ENUM_TO_STR(MorphNodeTo)
    ENUM_TO_STR(Group)
  default:
    return "Unknown";
  }
}
#undef ENUM_TO_STR

std::string Matcher::getKindAsString() const { return getKindAsString(Kind); }

// NOTE: exits program if error encountered
std::string readFile(const std::string &Filename) {
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
  return LookupTableStr;
}

std::vector<std::string> getStringArray(ondemand::document &TableJSON,
                                        const std::string &Key) {
  std::vector<std::string> V;
  ondemand::array Arr = TableJSON[Key].get_array();
  for (auto Predicate : Arr) {
    V.push_back(std::string(Predicate.get_string().value()));
  }
  return V;
}

std::vector<Pattern> getPatterns(ondemand::document &TableJSON) {
  std::vector<Pattern> Patterns;
  for (ondemand::object PatternObject : TableJSON["patterns"]) {
    Pattern ThePattern;
    ThePattern.Index = Patterns.size();
    for (uint64_t PredIdx : PatternObject["predicates"].get_array()) {
      ThePattern.NamedPredicates.push_back(PredIdx);
    }
    Patterns.push_back(ThePattern);
  }
  return Patterns;
}

std::vector<Matcher> getMatchers(ondemand::document &TableJSON) {
  std::vector<Matcher> Matchers;
  for (ondemand::object MatcherObject : TableJSON["matchers"]) {
    Matcher TheMatcher;
    TheMatcher.Begin = MatcherObject["index"];
    TheMatcher.Kind = (Matcher::KindTy)(int)MatcherObject["kind"].get_int64();
    size_t Size = MatcherObject["size"];
    TheMatcher.End = TheMatcher.Begin + Size - 1;
    if (TheMatcher.hasPattern()) {
      TheMatcher.PIdx = MatcherObject["pattern"];
    } else if (TheMatcher.hasPatPred()) {
      TheMatcher.PIdx = MatcherObject["predicate"];
    }
    Matchers.push_back(TheMatcher);
  }
  return Matchers;
}

LookupTable LookupTable::fromFile(const std::string &Filename,
                                  bool NameCaseSensitive, size_t Verbosity) {
  padded_string TablePaddedStr = padded_string::load(Filename);
  ondemand::parser Parser;
  ondemand::document TableJSON = Parser.iterate(TablePaddedStr);
  LookupTable Table;

  Table.Matchers = getMatchers(TableJSON);
  Table.Patterns = getPatterns(TableJSON);
  std::sort(Table.Matchers.begin(), Table.Matchers.end());
  Table.Matchers[0].End++;

  Table.PK.Verbosity = Verbosity;
  Table.PK.IsCaseSensitive = NameCaseSensitive;
  if (Table.PK.Verbosity > 1)
    errs() << "NOTE: Adding named predicates.\n";
  Table.PK.addNamedPredicates(getStringArray(TableJSON, "predicates"));
  if (Table.PK.Verbosity > 1)
    errs() << "NOTE: Adding pattern predicates.\n";
  Table.PK.addPatternPredicates(getStringArray(TableJSON, "pat_predicates"));
  Table.MatcherTableSize = TableJSON["table_size"];

  return Table;
}