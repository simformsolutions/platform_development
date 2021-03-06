// Copyright (C) 2016 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#include "proto/abi_dump.pb.h"
#pragma clang diagnostic pop

#include <header_abi_util.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <memory>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <stdlib.h>

static llvm::cl::OptionCategory header_linker_category(
    "header-abi-linker options");

static llvm::cl::list<std::string> dump_files(
    llvm::cl::Positional, llvm::cl::desc("<dump-files>"), llvm::cl::Required,
    llvm::cl::cat(header_linker_category), llvm::cl::OneOrMore);

static llvm::cl::opt<std::string> linked_dump(
    "o", llvm::cl::desc("<linked dump>"), llvm::cl::Required,
    llvm::cl::cat(header_linker_category));

static llvm::cl::list<std::string> exported_header_dirs(
    "I", llvm::cl::desc("<export_include_dirs>"), llvm::cl::Prefix,
    llvm::cl::ZeroOrMore, llvm::cl::cat(header_linker_category));

static llvm::cl::opt<std::string> version_script(
    "v", llvm::cl::desc("<version_script>"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<std::string> api(
    "api", llvm::cl::desc("<api>"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<std::string> arch(
    "arch", llvm::cl::desc("<arch>"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<bool> no_filter(
    "no-filter", llvm::cl::desc("Do not filter any abi"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<bool> use_version_script(
    "use-version-script", llvm::cl::desc("Use version script instead of .so"
                                         " file to filter out function"
                                         " and object symbols if available"),
    llvm::cl::Optional, llvm::cl::cat(header_linker_category));

static llvm::cl::opt<std::string> so_file(
    "so", llvm::cl::desc("<path to so file>"), llvm::cl::Required,
    llvm::cl::cat(header_linker_category));

class HeaderAbiLinker {
 public:
  HeaderAbiLinker(
      const std::vector<std::string> &dump_files,
      const std::vector<std::string> &exported_header_dirs,
      const std::string &version_script,
      const std::string &so_file,
      const std::string &linked_dump,
      const std::string &arch,
      const std::string &api)
    : dump_files_(dump_files), exported_header_dirs_(exported_header_dirs),
    version_script_(version_script), so_file_(so_file),
    out_dump_name_(linked_dump), arch_(arch), api_(api) {};

  bool LinkAndDump();

  template <typename T>
  static std::string GetLinkageName(T &element) {
    return element.type_info().linker_set_key();
  }
  template <typename T>
  static std::string GetSourceFile(T &element) {
    return element.type_info().source_file();
  }
 private:
  bool LinkTypes(const abi_dump::TranslationUnit &dump_tu,
                 abi_dump::TranslationUnit *linked_tu);
  bool LinkFunctions(const abi_dump::TranslationUnit &dump_tu,
                     abi_dump::TranslationUnit *linked_tu);

  bool LinkGlobalVars(const abi_dump::TranslationUnit &dump_tu,
                      abi_dump::TranslationUnit *linked_tu);

  template <typename T>
  inline bool LinkDecl(google::protobuf::RepeatedPtrField<T> *dst,
                       std::set<std::string> *link_set,
                       std::set<std::string> *regex_matched_link_set,
                       const std::regex *vs_regex,
                       const google::protobuf::RepeatedPtrField<T> &src,
                       bool use_version_script);

  bool ParseVersionScriptFiles();

  bool ParseSoFile();

  bool AddElfSymbols(abi_dump::TranslationUnit *linked_tu);

 private:
  const std::vector<std::string> &dump_files_;
  const std::vector<std::string> &exported_header_dirs_;
  const std::string &version_script_;
  const std::string &so_file_;
  const std::string &out_dump_name_;
  const std::string &arch_;
  const std::string &api_;
  // TODO: Add to a map of std::sets instead.
  std::set<std::string> exported_headers_;
  std::set<std::string> types_set_;
  std::set<std::string> function_decl_set_;
  std::set<std::string> globvar_decl_set_;
  // Version Script Regex Matching.
  std::set<std::string> functions_regex_matched_set;
  std::regex functions_vs_regex_;
  // Version Script Regex Matching.
  std::set<std::string> globvars_regex_matched_set;
  std::regex globvars_vs_regex_;
};

template <typename T, typename Iterable>
static bool AddElfSymbols(google::protobuf::RepeatedPtrField<T> *dst,
                          Iterable symbols) {
  for (auto &&symbol : symbols) {
    auto *added_symbol = dst->Add();
    if (added_symbol == nullptr) {
      return false;
    }
    added_symbol->set_name(symbol);
  }
  return true;
}

// To be called right after parsing the .so file / version script.
bool HeaderAbiLinker::AddElfSymbols(abi_dump::TranslationUnit *linked_tu) {

  return ::AddElfSymbols(linked_tu->mutable_elf_functions(), function_decl_set_)
      && ::AddElfSymbols(linked_tu->mutable_elf_objects(), globvar_decl_set_);
}

bool HeaderAbiLinker::LinkAndDump() {
  abi_dump::TranslationUnit linked_tu;
  std::ofstream text_output(out_dump_name_);
  google::protobuf::io::OstreamOutputStream text_os(&text_output);
  // If the user specifies that a version script should be used, use that.
  if (!use_version_script) {
    exported_headers_ =
        abi_util::CollectAllExportedHeaders(exported_header_dirs_);
    if (!ParseSoFile()) {
      llvm::errs() << "Couldn't parse so file\n";
      return false;
    }
  } else if (!ParseVersionScriptFiles()) {
    llvm::errs() << "Failed to parse stub files for exported symbols\n";
    return false;
  }

  AddElfSymbols(&linked_tu);

  for (auto &&i : dump_files_) {
    abi_dump::TranslationUnit dump_tu;
    std::ifstream input(i);
    google::protobuf::io::IstreamInputStream text_is(&input);
    if (!google::protobuf::TextFormat::Parse(&text_is, &dump_tu) ||
        !LinkTypes(dump_tu, &linked_tu) ||
        !LinkFunctions(dump_tu, &linked_tu) ||
        !LinkGlobalVars(dump_tu, &linked_tu)) {
      llvm::errs() << "Failed to link elements\n";
      return false;
    }
  }
  if (!google::protobuf::TextFormat::Print(linked_tu, &text_os)) {
    llvm::errs() << "Serialization to ostream failed\n";
    return false;
  }
  return true;
}

static bool QueryRegexMatches(std::set<std::string> *regex_matched_link_set,
                              const std::regex *vs_regex,
                              const std::string &symbol) {
  assert(regex_matched_link_set != nullptr);
  assert(vs_regex != nullptr);
  if (regex_matched_link_set->find(symbol) != regex_matched_link_set->end()) {
    return false;
  }
  if (std::regex_search(symbol, *vs_regex)) {
    regex_matched_link_set->insert(symbol);
    return true;
  }
  return false;
}

static std::regex CreateRegexMatchExprFromSet(
    const std::set<std::string> &link_set) {
  std::string all_regex_match_str = "";
  std::set<std::string>::iterator it = link_set.begin();
  while (it != link_set.end()) {
    std::string regex_match_str_find_glob =
      abi_util::FindAndReplace(*it, "\\*", ".*");
    all_regex_match_str += "(\\b" + regex_match_str_find_glob + "\\b)";
    if (++it != link_set.end()) {
      all_regex_match_str += "|";
    }
  }
  if (all_regex_match_str == "") {
    return std::regex();
  }
  return std::regex(all_regex_match_str);
}

//TODO: make linking decls multi-threaded b/63590537.
template <typename T>
inline bool HeaderAbiLinker::LinkDecl(
    google::protobuf::RepeatedPtrField<T> *dst, std::set<std::string> *link_set,
    std::set<std::string> *regex_matched_link_set, const std::regex *vs_regex,
    const google::protobuf::RepeatedPtrField<T> &src, bool use_version_script) {
  assert(dst != nullptr);
  assert(link_set != nullptr);
  for (auto &&element : src) {
    // If we are not using a version script and exported headers are available,
    // filter out unexported abi.
    std::string source_file = GetSourceFile(element);
    // Builtin types will not have source file information.
    if (!exported_headers_.empty() && !source_file.empty() &&
        exported_headers_.find(source_file) ==
        exported_headers_.end()) {
      continue;
    }
    std::string element_str = GetLinkageName(element);
    // Check for the existence of the element in linked dump / symbol file.
    if (!use_version_script) {
      if (!link_set->insert(element_str).second) {
        continue;
      }
    } else {
      std::set<std::string>::iterator it =
          link_set->find(element_str);
      if (it == link_set->end()) {
        if (!QueryRegexMatches(regex_matched_link_set, vs_regex, element_str)) {
          continue;
        }
      } else {
        // We get a pre-filled link name set while using version script.
        link_set->erase(*it); // Avoid multiple instances of the same symbol.
      }
    }
    T *added_element = dst->Add();
    if (!added_element) {
      llvm::errs() << "Failed to add element to linked dump\n";
      return false;
    }
    *added_element = element;
  }
  return true;
}


template<>
std::string HeaderAbiLinker::GetLinkageName<const abi_dump::FunctionDecl> (
    const abi_dump::FunctionDecl &element) {
  return element.linker_set_key();
}

template<>
std::string HeaderAbiLinker::GetSourceFile<const abi_dump::FunctionDecl> (
    const abi_dump::FunctionDecl &element) {
  return element.source_file();
}

template<>
std::string HeaderAbiLinker::GetLinkageName<const abi_dump::GlobalVarDecl> (
    const abi_dump::GlobalVarDecl &element) {
  return element.linker_set_key();
}

template<>
std::string HeaderAbiLinker::GetSourceFile<const abi_dump::GlobalVarDecl> (
    const abi_dump::GlobalVarDecl &element) {
  return element.source_file();
}

bool HeaderAbiLinker::LinkTypes(const abi_dump::TranslationUnit &dump_tu,
                                abi_dump::TranslationUnit *linked_tu) {
  assert(linked_tu != nullptr);
  // Even if version scripts are available we take in types, since the symbols
  // in the version script might reference a type exposed by the library.
  return LinkDecl(linked_tu->mutable_record_types(), &types_set_, nullptr,
                  nullptr, dump_tu.record_types(), false) &&
      LinkDecl(linked_tu->mutable_enum_types(), &types_set_, nullptr,
               nullptr, dump_tu.enum_types(), false) &&
      LinkDecl(linked_tu->mutable_builtin_types(), &types_set_, nullptr,
               nullptr, dump_tu.builtin_types(), false) &&
      LinkDecl(linked_tu->mutable_pointer_types(), &types_set_, nullptr,
               nullptr, dump_tu.pointer_types(), false) &&
      LinkDecl(linked_tu->mutable_rvalue_reference_types(), &types_set_, nullptr,
               nullptr, dump_tu.rvalue_reference_types(), false) &&
      LinkDecl(linked_tu->mutable_lvalue_reference_types(), &types_set_, nullptr,
               nullptr, dump_tu.lvalue_reference_types(), false) &&
      LinkDecl(linked_tu->mutable_array_types(), &types_set_, nullptr,
               nullptr, dump_tu.array_types(), false) &&
      LinkDecl(linked_tu->mutable_qualified_types(), &types_set_, nullptr,
               nullptr, dump_tu.qualified_types(), false);
}

bool HeaderAbiLinker::LinkFunctions(const abi_dump::TranslationUnit &dump_tu,
                                    abi_dump::TranslationUnit *linked_tu) {
  assert(linked_tu != nullptr);
  return LinkDecl(linked_tu->mutable_functions(), &function_decl_set_,
                  &functions_regex_matched_set, &functions_vs_regex_,
                  dump_tu.functions(),
                  (!version_script_.empty() || !so_file_.empty()));
}

bool HeaderAbiLinker::LinkGlobalVars(const abi_dump::TranslationUnit &dump_tu,
                                     abi_dump::TranslationUnit *linked_tu) {
  assert(linked_tu != nullptr);
  return LinkDecl(linked_tu->mutable_global_vars(), &globvar_decl_set_,
                  &globvars_regex_matched_set, &globvars_vs_regex_,
                  dump_tu.global_vars(),
                  (!version_script.empty() || !so_file_.empty()));
}

bool HeaderAbiLinker::ParseVersionScriptFiles() {
  abi_util::VersionScriptParser version_script_parser(version_script_, arch_,
                                                      api_);
  if (!version_script_parser.Parse()) {
    return false;
  }
  function_decl_set_ = version_script_parser.GetFunctions();
  globvar_decl_set_ = version_script_parser.GetGlobVars();
  std::set<std::string> function_regexs =
      version_script_parser.GetFunctionRegexs();
  std::set<std::string> globvar_regexs =
      version_script_parser.GetGlobVarRegexs();
  functions_vs_regex_ = CreateRegexMatchExprFromSet(function_regexs);
  globvars_vs_regex_ = CreateRegexMatchExprFromSet(globvar_regexs);
  return true;
}

bool HeaderAbiLinker::ParseSoFile() {
 auto Binary = llvm::object::createBinary(so_file_);

  if (!Binary) {
    llvm::errs() << "Couldn't really create object File \n";
    return false;
  }
  llvm::object::ObjectFile *objfile =
      llvm::dyn_cast<llvm::object::ObjectFile>(&(*Binary.get().getBinary()));
  if (!objfile) {
    llvm::errs() << "Not an object file\n";
    return false;
  }

  std::unique_ptr<abi_util::SoFileParser> so_parser =
      abi_util::SoFileParser::Create(objfile);
  if (so_parser == nullptr) {
    llvm::errs() << "Couldn't create soFile Parser\n";
    return false;
  }
  so_parser->GetSymbols();
  function_decl_set_ = so_parser->GetFunctions();
  globvar_decl_set_ = so_parser->GetGlobVars();
  return true;
}

int main(int argc, const char **argv) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  llvm::cl::ParseCommandLineOptions(argc, argv, "header-linker");
  if (no_filter) {
    static_cast<std::vector<std::string> &>(exported_header_dirs).clear();
  }
  HeaderAbiLinker Linker(dump_files, exported_header_dirs, version_script,
                         so_file, linked_dump, arch, api);

  if (!Linker.LinkAndDump()) {
    return -1;
  }
  return 0;
}
