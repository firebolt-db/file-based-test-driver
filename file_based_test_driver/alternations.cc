//
// Copyright 2020 Google LLC
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
//
#include "file_based_test_driver/alternations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include <string_view>

#include "absl/strings/str_join.h"
#include "re2_st/re2.h"
#include "file_based_test_driver/base/ret_check.h"
#include "file_based_test_driver/file_based_test_driver.h"

namespace file_based_test_driver {

// One spelling for both paths: the name goes into an `ALTERNATION GROUP: ...` part either way, so a
// file looks the same whichever path wrote it. The per-mode path read "EMPTY" while it encoded names
// into a `<result type>`, where angle brackets could not appear.
constexpr char kEmptyAlternationName[] = "<empty>";

absl::Status AlternationSet::Record(const std::string& alternation_name,
                                    const RunTestCaseResult& test_case_result) {
  FILE_BASED_TEST_DRIVER_RET_CHECK(!finished_);
  // Firebolt Start
  std::vector<std::string> test_outputs = test_case_result.test_outputs();
  if (test_case_result.compare_unsorted_result()) {
    // test_outputs.size() > 1 if there are multiple outputs per alternation.
    for (std::string& output : test_outputs) {
      output = SortLines(output, test_case_result.output_has_header());
    }
  }
  alternation_map_[test_outputs].push_back(alternation_names_.size());
  // Firebolt End
  alternation_names_.push_back(alternation_name.empty() ? kEmptyAlternationName
                                                        : alternation_name);
  return absl::OkStatus();
}

absl::Status AlternationSet::Finish(RunTestCaseResult* test_case_result) {
  FILE_BASED_TEST_DRIVER_RET_CHECK(!finished_);
  finished_ = true;
  std::vector<std::string>* test_outputs =
      test_case_result->mutable_test_outputs();
  FILE_BASED_TEST_DRIVER_RET_CHECK(test_outputs->empty());

  // If all of the results are the same, leave output as is. Otherwise,
  // process output into the form:
  //   {{{<semicolon separated list of alternation groups}}}
  //   <results for these groups>
  //   ----
  //   {{{<next groups>}}}
  //   <results for these groups>
  //   ...
  //
  // We'll group all alternations with equal outputs together,
  // ordering alternations by their original generation order.
  if (alternation_map_.size() <= 1) {
    if (!alternation_map_.empty()) {
      *test_outputs = alternation_map_.begin()->first;
    }
  } else {
    // Map the sorted vector of input indices with the same output
    // to that output value.
    // This will also sort the map by the first element of each vector.
    std::map<std::vector<int>, std::vector<std::string>> transposed_results_map;

    // Transpose alternation_map_ so it maps from a vector of indices (sorted)
    // to their common test output.
    // The vector of indices is already sorted, by construction.
    for (const auto& it : alternation_map_) {
      transposed_results_map[it.second] = it.first;
    }

    for (const auto& it : transposed_results_map) {
      const std::vector<int>& alternation_index_list = it.first;
      const std::vector<std::string>& group_output = it.second;

      std::vector<std::string> match_groups;
      match_groups.reserve(alternation_index_list.size());
      for (int idx : alternation_index_list) {
        match_groups.push_back(alternation_names_[idx]);
      }

      if (match_groups.size() > 1) {
        match_groups.insert(match_groups.begin(), "ALTERNATION GROUPS:");
        const std::string groups_list = absl::StrJoin(match_groups, "\n    ");
        test_outputs->push_back(groups_list);
      } else {
        test_outputs->push_back(
            absl::StrCat("ALTERNATION GROUP: ", match_groups[0]));
      }
      test_outputs->insert(test_outputs->end(), group_output.begin(),
                           group_output.end());
    }
  }

  test_outputs->insert(test_outputs->begin(), test_case_result->parts()[0]);
  alternation_map_.clear();

  return absl::OkStatus();
}

absl::Status AlternationSetWithModes::Record(
    const std::string& alternation_name,
    const RunTestCaseWithModesResult& test_case_result) {
  FILE_BASED_TEST_DRIVER_RET_CHECK(!finished_);
  // No restriction on what an alternation may be called: the name goes into an
  // `ALTERNATION GROUP: ...` part, as on the single-expected-output path, rather than into a result
  // type -- which is why characters like <, >, { and } used to be rejected here. Alternations are
  // frequently fragments of the test's own input, so they do contain them.
  TestCaseOutputs outputs = test_case_result.test_case_outputs();
  // [unsorted_output] means row order does not count, so normalize before grouping: two alternations
  // whose rows differ only in order belong in one group, as they do on the single-expected-output
  // path (AlternationSet::Record does the same).
  if (test_case_result.compare_unsorted_result()) {
    outputs.NormalizeOutputs([&test_case_result](absl::string_view part) {
      return SortLines(part, test_case_result.output_has_header());
    });
  }
  alternations_.emplace_back(
      alternation_name.empty() ? kEmptyAlternationName : alternation_name, outputs);
  return absl::OkStatus();
}

absl::Status AlternationSetWithModes::Finish(
    RunTestCaseWithModesResult* test_case_result) {
  FILE_BASED_TEST_DRIVER_RET_CHECK(!finished_);
  finished_ = true;

  TestCaseOutputs* test_case_outputs =
      test_case_result->mutable_test_case_outputs();

  TestCaseMode::UnorderedSet all_modes;
  FILE_BASED_TEST_DRIVER_RETURN_IF_ERROR(CollectModes(test_case_outputs, &all_modes));

  for (const TestCaseMode& mode : all_modes) {
    FILE_BASED_TEST_DRIVER_RETURN_IF_ERROR(BuildSingleMode(mode, test_case_outputs));
  }

  return absl::OkStatus();
}

absl::Status AlternationSetWithModes::CollectModes(
    TestCaseOutputs* test_case_outputs, TestCaseMode::UnorderedSet* all_modes) {
  bool set_possible_modes = true;
  for (const NameAndAlternationOutput& alternation : alternations_) {
    if (set_possible_modes) {
      FILE_BASED_TEST_DRIVER_RETURN_IF_ERROR(test_case_outputs->SetPossibleModes(
          alternation.outputs.possible_modes()));
      set_possible_modes = false;
    } else {
      FILE_BASED_TEST_DRIVER_RET_CHECK(test_case_outputs->possible_modes() ==
                alternation.outputs.possible_modes())
          << "Different possible modes for differerent alternations are not "
          << "allowed: {"
          << absl::StrJoin(test_case_outputs->possible_modes(), ",",
                           TestCaseMode::JoinFormatter())
          << "} vs {"
          << absl::StrJoin(alternation.outputs.possible_modes(), ",",
                           TestCaseMode::JoinFormatter())
          << "}";
    }
    for (const auto& mode_to_results : alternation.outputs.outputs_) {
      const TestCaseMode& mode = mode_to_results.first;
      all_modes->insert(mode);
    }
  }

  return absl::OkStatus();
}

absl::Status AlternationSetWithModes::BuildSingleMode(
    const TestCaseMode& mode, TestCaseOutputs* test_case_outputs) {
  ResultTypeToOutputMap result_type_to_output_map;

  FILE_BASED_TEST_DRIVER_RETURN_IF_ERROR(CollectAlternations(mode, &result_type_to_output_map));

  for (const auto& pair : result_type_to_output_map) {
    const std::string& result_type = pair.first;
    const OutputToAlternationNameMap& output_map = pair.second;
    bool added;
    FILE_BASED_TEST_DRIVER_RETURN_IF_ERROR(MaybeAddSingleOutput(mode, result_type, output_map,
                                         test_case_outputs, &added));
    if (!added) {
      FILE_BASED_TEST_DRIVER_RETURN_IF_ERROR(CombineAlternations(mode, result_type, output_map,
                                          test_case_outputs));
    }
  }

  return absl::OkStatus();
}

absl::Status AlternationSetWithModes::CollectAlternations(
    const TestCaseMode& mode,
    ResultTypeToOutputMap* result_type_to_output_map) {
  for (const NameAndAlternationOutput& alternation : alternations_) {
    auto it = alternation.outputs.outputs_.find(mode);
    FILE_BASED_TEST_DRIVER_RET_CHECK(it != alternation.outputs.outputs_.end());
    const TestCaseOutputs::ModeResults& mode_results = it->second;

    for (const auto& pair : mode_results) {
      const std::string& result_type = pair.first;
      const std::vector<std::string>& output = pair.second;
      (*result_type_to_output_map)[result_type][output].push_back(
          alternation.name);
    }
  }
  return absl::OkStatus();
}

absl::Status AlternationSetWithModes::MaybeAddSingleOutput(
    const TestCaseMode& mode, const std::string& result_type,
    const OutputToAlternationNameMap& output_map,
    TestCaseOutputs* test_case_outputs, bool* added) {
  *added = false;
  if (output_map.size() != 1) {
    return absl::OkStatus();
  }

  const std::pair<const std::vector<std::string>, std::vector<std::string>>&
      output_and_idx_list = *output_map.begin();
  if (output_and_idx_list.second.size() != alternations_.size()) {
    return absl::OkStatus();
  }

  FILE_BASED_TEST_DRIVER_RETURN_IF_ERROR(test_case_outputs->RecordOutputParts(
      mode, result_type, output_and_idx_list.first));
  *added = true;

  return absl::OkStatus();
}

absl::Status AlternationSetWithModes::CombineAlternations(
    const TestCaseMode& mode, const std::string& result_type,
    const OutputToAlternationNameMap& output_map,
    TestCaseOutputs* test_case_outputs) {
  // Emitted in the order the alternations ran, as the single-expected-output path emits them: walk
  // the alternations and take each group the first time one of its names comes up. One pass over the
  // names either way -- a test can have dozens of groups, so this does not sort them.
  using Group = OutputToAlternationNameMap::value_type;
  absl::flat_hash_map<std::string_view, const Group*> group_of_name;
  for (const Group& output_and_names : output_map) {
    for (const std::string& name : output_and_names.second) {
      group_of_name.try_emplace(name, &output_and_names);
    }
  }
  std::vector<const Group*> groups;
  groups.reserve(output_map.size());
  absl::flat_hash_set<const Group*> seen;
  seen.reserve(output_map.size());
  for (const NameAndAlternationOutput& alternation : alternations_) {
    const auto it = group_of_name.find(alternation.name);
    if (it == group_of_name.end()) continue;  // this alternation produced no output of this type
    if (seen.insert(it->second).second) groups.push_back(it->second);
  }

  std::vector<std::string> parts;
  for (const auto* output_and_names : groups) {
    const std::vector<std::string>& output = output_and_names->first;
    const std::vector<std::string>& alternation_names = output_and_names->second;

    if (alternation_names.size() > 1) {
      parts.push_back(absl::StrCat("ALTERNATION GROUPS:\n    ",
                                   absl::StrJoin(alternation_names, "\n    ")));
    } else {
      parts.push_back(absl::StrCat("ALTERNATION GROUP: ", alternation_names.front()));
    }
    parts.insert(parts.end(), output.begin(), output.end());
  }

  return test_case_outputs->RecordOutputParts(mode, result_type, std::move(parts));
}

}  // namespace file_based_test_driver
